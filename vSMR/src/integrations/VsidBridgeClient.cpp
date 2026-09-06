#include "platform/windows/PrecompiledHeader.hpp"

#include "integrations/VsidBridgeClient.hpp"
#include "integrations/PluginBridgeApi.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include "EuroScopePlugIn.h"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using namespace EuroScopePlugIn;
	using namespace VsmrPluginBridgeAbi;

	constexpr char ProviderId[] = "vsid";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;
	constexpr std::size_t MaximumFlightPlans = 4096U;
	constexpr std::size_t MinimumApiSize =
		offsetof(ApiV1, providerRevision) + sizeof(decltype(ApiV1::providerRevision));

	std::mutex StateMutex;
	std::unordered_map<std::string, VsmrVsid::AircraftData> AircraftByCallsign;
	std::unordered_set<std::string> DisconnectedCallsigns;
	std::unordered_set<std::string> LastScannedCallsigns;
	VsmrVsid::LfpgOperatingMode CurrentLfpgMode =
		VsmrVsid::LfpgOperatingMode::MinimumTaxiing;
	VsmrVsid::LfpgLinkMode CurrentLfpgLinkMode =
		VsmrVsid::LfpgLinkMode::Unlinked;
	std::optional<VsmrVsid::CommandAction> PendingCommandAction;
	HMODULE BridgeModule = nullptr;
	const ApiV1* BridgeApi = nullptr;
	FieldId SidField = 0U;
	FieldId RunwayField = 0U;
	FieldId ClearedFlightLevelField = 0U;
	std::uint64_t LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
	bool ProviderReadyLogged = false;
	bool IncompatibleSchemaLogged = false;
	bool InterfaceStateInitialized = false;
	bool LastBridgeLoaded = false;
	bool LastBridgeCompatible = false;
	bool LastProviderReady = false;

	std::string NormalizeCallsign(const std::string& callsign)
	{
		return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	}

	bool ClearCachedAircraft()
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty())
			return false;
		AircraftByCallsign.clear();
		return true;
	}

	void ResetProviderState()
	{
		SidField = 0U;
		RunwayField = 0U;
		ClearedFlightLevelField = 0U;
		LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
		LastScannedCallsigns.clear();
		ProviderReadyLogged = false;
	}

	bool UpdateInterfaceState()
	{
		const bool bridgeLoaded = BridgeModule != nullptr;
		const bool bridgeCompatible = BridgeApi != nullptr;
		const bool providerReady = bridgeCompatible &&
			SidField != 0U && RunwayField != 0U &&
			ClearedFlightLevelField != 0U;
		const bool changed = !InterfaceStateInitialized ||
			bridgeLoaded != LastBridgeLoaded ||
			bridgeCompatible != LastBridgeCompatible ||
			providerReady != LastProviderReady;
		InterfaceStateInitialized = true;
		LastBridgeLoaded = bridgeLoaded;
		LastBridgeCompatible = bridgeCompatible;
		LastProviderReady = providerReady;
		return changed;
	}

	bool AttachBridge()
	{
		HMODULE module = ::GetModuleHandleW(ModuleName);
		if (module == nullptr)
		{
			BridgeModule = nullptr;
			BridgeApi = nullptr;
			ResetProviderState();
			return false;
		}
		if (BridgeApi != nullptr && BridgeModule == module)
			return true;

		BridgeModule = module;
		BridgeApi = nullptr;
		ResetProviderState();
		const auto getApi = reinterpret_cast<GetApiFunction>(
			::GetProcAddress(module, EntrySymbol));
		if (getApi == nullptr)
			return false;

		const ApiV1* api = getApi(AbiVersion);
		if (api == nullptr || api->abiVersion != AbiVersion ||
			api->structureSize < MinimumApiSize || api->resolve == nullptr ||
			api->providerVersion == nullptr || api->aircraft == nullptr ||
			api->getAircraft == nullptr || api->providerRevision == nullptr)
		{
			return false;
		}
		BridgeApi = api;
		return true;
	}

	bool ResolveVsidFields()
	{
		std::uint32_t major = 0U;
		std::uint32_t minor = 0U;
		const Status versionStatus = BridgeApi->providerVersion(
			ProviderId,
			&major,
			&minor);
		(void)minor;
		if (versionStatus != Ok)
		{
			ResetProviderState();
			return false;
		}
		if (major != SupportedSchemaMajor)
		{
			ResetProviderState();
			if (!IncompatibleSchemaLogged)
			{
				Logger::info("vSID bridge provider uses an unsupported schema major version");
				IncompatibleSchemaLogged = true;
			}
			return false;
		}
		IncompatibleSchemaLogged = false;

		if (SidField != 0U && RunwayField != 0U && ClearedFlightLevelField != 0U)
			return true;

		FieldId sid = 0U;
		FieldId runway = 0U;
		FieldId clearedFlightLevel = 0U;
		if (BridgeApi->resolve("vsid/sid", String, &sid) != Ok ||
			BridgeApi->resolve("vsid/rwy", String, &runway) != Ok ||
			BridgeApi->resolve("vsid/cfl", String, &clearedFlightLevel) != Ok)
		{
			ResetProviderState();
			return false;
		}

		SidField = sid;
		RunwayField = runway;
		ClearedFlightLevelField = clearedFlightLevel;
		LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
		if (!ProviderReadyLogged)
		{
			Logger::info("vSID interface connected through EuroScope Plugin Bridge");
			ProviderReadyLogged = true;
		}
		return true;
	}

	Status ReadString(Aircraft aircraft, FieldId field, std::string& value)
	{
		std::array<char, VsmrVsid::MaximumFieldBytes> buffer{};
		std::uint32_t bytes = static_cast<std::uint32_t>(buffer.size());
		Value bridgeValue{};
		const Status status = BridgeApi->getAircraft(
			aircraft,
			field,
			&bridgeValue,
			buffer.data(),
			&bytes);
		if (status == Unset)
		{
			value.clear();
			return Ok;
		}
		if (status != Ok)
			return status;
		if (bridgeValue.type != String || bridgeValue.bytes > buffer.size() ||
			bridgeValue.bytes != bytes)
		{
			return TypeMismatch;
		}

		value = VsmrVsid::NormalizeFieldValue(
			std::string_view(buffer.data(), bridgeValue.bytes));
		return Ok;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrVsid::AircraftData> next)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == next)
			return false;
		AircraftByCallsign = std::move(next);
		return true;
	}
}

bool VsmrVsid::Poll(EuroScopePlugIn::CPlugIn& plugin)
{
	bool commandStateChanged = false;
	switch (VsmrEuroScopeCommandLine::Poll(
		VsmrEuroScopeCommandLine::Owner::Vsid))
	{
	case VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed:
		{
			std::lock_guard<std::mutex> guard(StateMutex);
			if (PendingCommandAction == CommandAction::LfpgMinimumTaxiing)
				CurrentLfpgMode = LfpgOperatingMode::MinimumTaxiing;
			else if (PendingCommandAction == CommandAction::LfpgGroundCrossing)
				CurrentLfpgMode = LfpgOperatingMode::GroundCrossing;
			else if (PendingCommandAction == CommandAction::LfpgLinked)
				CurrentLfpgLinkMode = LfpgLinkMode::Linked;
			else if (PendingCommandAction == CommandAction::LfpgUnlinked)
				CurrentLfpgLinkMode = LfpgLinkMode::Unlinked;
			else if (PendingCommandAction == CommandAction::ReloadConfiguration)
			{
				// LFPG custom rules use false for their default, non-opposing states.
				CurrentLfpgMode = LfpgOperatingMode::MinimumTaxiing;
				CurrentLfpgLinkMode = LfpgLinkMode::Unlinked;
			}
			PendingCommandAction.reset();
		}
		Logger::info("vSID command consumed by EuroScope");
		commandStateChanged = true;
		break;
	case VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous:
		{
			std::lock_guard<std::mutex> guard(StateMutex);
			PendingCommandAction.reset();
		}
		Logger::info("vSID command submission could not be confirmed");
		commandStateChanged = true;
		break;
	default:
		break;
	}
	auto finish = [&](bool dataChanged)
	{
		const bool interfaceStateChanged = UpdateInterfaceState();
		return dataChanged || commandStateChanged || interfaceStateChanged;
	};

	try
	{
		if (!AttachBridge() || !ResolveVsidFields())
			return finish(ClearCachedAircraft());

		std::unordered_set<std::string> currentCallsigns;
		std::unordered_set<std::string> disconnectedCallsigns;
		{
			std::lock_guard<std::mutex> guard(StateMutex);
			disconnectedCallsigns = DisconnectedCallsigns;
		}
		std::size_t flightPlanCount = 0U;
		for (CFlightPlan flightPlan = plugin.FlightPlanSelectFirst();
			flightPlan.IsValid() && flightPlanCount < MaximumFlightPlans;
			flightPlan = plugin.FlightPlanSelectNext(flightPlan), ++flightPlanCount)
		{
			if (flightPlan.GetFPState() == FLIGHT_PLAN_STATE_TERMINATED ||
				flightPlan.GetSimulated())
			{
				continue;
			}
			const char* rawCallsign = flightPlan.GetCallsign();
			const std::string callsign = NormalizeCallsign(
				rawCallsign != nullptr ? rawCallsign : "");
			if (!callsign.empty() && disconnectedCallsigns.count(callsign) == 0U)
				currentCallsigns.insert(callsign);
		}

		const std::uint64_t providerRevision = BridgeApi->providerRevision(ProviderId);
		if (providerRevision == LastProviderRevision &&
			currentCallsigns == LastScannedCallsigns)
		{
			return finish(false);
		}

		std::unordered_map<std::string, AircraftData> next;
		for (const std::string& callsign : currentCallsigns)
		{
			Aircraft aircraft = 0U;
			if (BridgeApi->aircraft(callsign.c_str(), &aircraft) != Ok)
				continue;

			AircraftData data;
			const Status sidStatus = ReadString(aircraft, SidField, data.sid);
			const Status runwayStatus = ReadString(aircraft, RunwayField, data.runway);
			const Status cflStatus = ReadString(
				aircraft,
				ClearedFlightLevelField,
				data.clearedFlightLevel);
			if (sidStatus == NoProvider || runwayStatus == NoProvider || cflStatus == NoProvider ||
				sidStatus == VsmrPluginBridgeAbi::Shutdown ||
				runwayStatus == VsmrPluginBridgeAbi::Shutdown ||
				cflStatus == VsmrPluginBridgeAbi::Shutdown)
			{
				ResetProviderState();
				return finish(ClearCachedAircraft());
			}
			if (sidStatus != Ok || runwayStatus != Ok || cflStatus != Ok)
				continue;
			// A bridge aircraft handle can exist without vSID publishing data for it.
			// Such handles are not connected vSID aircraft and must not inflate status.
			if (!HasPublishedAircraftData(data))
				continue;
			next.emplace(callsign, std::move(data));
		}

		LastProviderRevision = providerRevision;
		LastScannedCallsigns = std::move(currentCallsigns);
		return finish(ReplaceSnapshot(std::move(next)));
	}
	catch (const std::exception& exception)
	{
		Logger::info("vSID bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("vSID bridge poll failed: unknown exception");
	}
	ResetProviderState();
	return finish(ClearCachedAircraft());
}

VsmrVsid::InterfaceState VsmrVsid::GetInterfaceState()
{
	InterfaceState state;
	state.bridgeLoaded = BridgeModule != nullptr;
	state.bridgeCompatible = BridgeApi != nullptr;
	state.providerReady = BridgeApi != nullptr &&
		SidField != 0U && RunwayField != 0U && ClearedFlightLevelField != 0U;
	state.commandLineBusy = VsmrEuroScopeCommandLine::IsBusy();
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		state.aircraftCount = AircraftByCallsign.size();
		state.lfpgMode = CurrentLfpgMode;
		state.lfpgLinkMode = CurrentLfpgLinkMode;
	}
	return state;
}

bool VsmrVsid::SubmitCommand(
	CommandAction action,
	const std::string& activeAirport,
	std::string& error)
{
	error.clear();
	const InterfaceState state = GetInterfaceState();
	if (!state.bridgeLoaded)
	{
		error = "Load EuroScopeBridge.dll before using the vSID interface.";
		return false;
	}
	if (!state.bridgeCompatible)
	{
		error = "The loaded EuroScope Plugin Bridge is incompatible.";
		return false;
	}
	if (!state.providerReady)
	{
		error = "vSID 0.15.0.2 or later is not available through the bridge.";
		return false;
	}

	const std::string command = BuildCommand(action, activeAirport);
	if (command.empty())
	{
		error = "Select a valid four-character airport before using this vSID action.";
		return false;
	}
	if (!VsmrEuroScopeCommandLine::Begin(
		VsmrEuroScopeCommandLine::Owner::Vsid,
		command,
		&error))
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PendingCommandAction = action;
	}
	return true;
}

bool VsmrVsid::TryGetAircraftData(
	const std::string& callsign,
	AircraftData& outData)
{
	const std::string normalizedCallsign = NormalizeCallsign(callsign);
	if (normalizedCallsign.empty())
		return false;
	std::lock_guard<std::mutex> guard(StateMutex);
	const auto found = AircraftByCallsign.find(normalizedCallsign);
	if (found == AircraftByCallsign.end())
		return false;
	outData = found->second;
	return true;
}

void VsmrVsid::ForgetAircraft(const std::string& callsign)
{
	const std::string normalizedCallsign = NormalizeCallsign(callsign);
	if (normalizedCallsign.empty())
		return;
	LastScannedCallsigns.erase(normalizedCallsign);
	std::lock_guard<std::mutex> guard(StateMutex);
	DisconnectedCallsigns.insert(normalizedCallsign);
	AircraftByCallsign.erase(normalizedCallsign);
}

void VsmrVsid::ObserveAircraft(const std::string& callsign)
{
	const std::string normalizedCallsign = NormalizeCallsign(callsign);
	if (normalizedCallsign.empty())
		return;
	LastScannedCallsigns.erase(normalizedCallsign);
	std::lock_guard<std::mutex> guard(StateMutex);
	DisconnectedCallsigns.erase(normalizedCallsign);
}

void VsmrVsid::Shutdown() noexcept
{
	VsmrEuroScopeCommandLine::Cancel(
		VsmrEuroScopeCommandLine::Owner::Vsid);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
		DisconnectedCallsigns.clear();
		PendingCommandAction.reset();
		CurrentLfpgMode = LfpgOperatingMode::MinimumTaxiing;
		CurrentLfpgLinkMode = LfpgLinkMode::Unlinked;
	}
	BridgeModule = nullptr;
	BridgeApi = nullptr;
	ResetProviderState();
	IncompatibleSchemaLogged = false;
	InterfaceStateInitialized = false;
	LastBridgeLoaded = false;
	LastBridgeCompatible = false;
	LastProviderReady = false;
}
