#include "platform/windows/PrecompiledHeader.hpp"

#include "integrations/VsidBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	using VsmrPluginBridge::AttachState;
	using VsmrPluginBridge::FieldSpec;
	using VsmrPluginBridge::ProviderState;
	using VsmrPluginBridge::ReadStatus;

	// vSID's provider declaration (vSIDPlugin.h): schema 1.0 publishes sid, rwy and
	// cfl as aircraft STR fields of at most 32 bytes. automode is the optional
	// schema 1.1 global described in docs/integrations/vsid-automode.md. The
	// companion schema 1.2/1.3 also publishes the manual Paris rules snapshot.
	constexpr char ProviderId[] = "vsid";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
	{
		SidField,
		RunwayField,
		ClearedFlightLevelField,
		AutomaticModeField,
		ParisStateField,
		FieldCount
	};

	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "sid", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "rwy", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "cfl", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "automode", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumAutomaticModeBytes) },
		{ "paris", ESB_T_STR, static_cast<std::uint32_t>(VsmrParis::Airports.size() * 9U) }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("vSID");

	// Snapshot shared with rendering, the Runtime Menu and command submission.
	std::mutex StateMutex;
	std::map<std::string, bool> AutomaticModes;
	std::map<std::string, VsmrParis::State> ParisStates;
	std::unordered_map<std::string, VsmrVsid::AircraftData> AircraftByCallsign;
	std::atomic<bool> ParisCommandsAvailable{ false };
	std::atomic<bool> RegionalCommandsAvailable{ false };
	std::atomic<bool> ProviderReady{ false };

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;
	bool InterfaceStateInitialized = false;
	AttachState LastAttachState = AttachState::NotLoaded;
	bool LastProviderReady = false;

	bool DisconnectProvider()
	{
		Provider.Reset();
		ProviderReady.store(false, std::memory_order_relaxed);
		ParisCommandsAvailable.store(false, std::memory_order_relaxed);
		RegionalCommandsAvailable.store(false, std::memory_order_relaxed);
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty() && AutomaticModes.empty() && ParisStates.empty())
			return false;
		AircraftByCallsign.clear();
		AutomaticModes.clear();
		ParisStates.clear();
		return true;
	}

	bool UpdateInterfaceState()
	{
		const AttachState attachState = VsmrPluginBridge::GetAttachState();
		const bool providerReady = ProviderReady.load(std::memory_order_relaxed);
		const bool changed = !InterfaceStateInitialized ||
			attachState != LastAttachState ||
			providerReady != LastProviderReady;
		InterfaceStateInitialized = true;
		LastAttachState = attachState;
		LastProviderReady = providerReady;
		return changed;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrVsid::AircraftData> aircraft,
		std::map<std::string, bool> automaticModes,
		std::map<std::string, VsmrParis::State> parisStates)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == aircraft && AutomaticModes == automaticModes && ParisStates == parisStates)
			return false;
		AircraftByCallsign = std::move(aircraft);
		AutomaticModes = std::move(automaticModes);
		ParisStates = std::move(parisStates);
		return true;
	}
}

bool VsmrVsid::Poll(const VsmrPluginBridge::Tick& tick)
{
	bool commandStateChanged = false;
	switch (VsmrEuroScopeCommandLine::Poll(
		VsmrEuroScopeCommandLine::Owner::Vsid))
	{
	case VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed:
		Logger::info("vSID command consumed by EuroScope");
		commandStateChanged = true;
		break;
	case VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous:
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
		if (tick.api == nullptr)
			return finish(DisconnectProvider());
		const ESB_Api_v1& api = *tick.api;

		// vSID is optional: an absent provider is a normal configuration (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return finish(DisconnectProvider());
		ProviderReady.store(true, std::memory_order_relaxed);
		const bool parisCommands = SupportsParisCommands(Provider.SchemaMajor(), Provider.SchemaMinor());
		const bool regionalCommands = SupportsRegionalCommands(Provider.SchemaMajor(), Provider.SchemaMinor());
		commandStateChanged = (ParisCommandsAvailable.exchange(parisCommands, std::memory_order_relaxed) != parisCommands) || commandStateChanged;
		commandStateChanged = (RegionalCommandsAvailable.exchange(regionalCommands, std::memory_order_relaxed) != regionalCommands) || commandStateChanged;

		// Coarse gate (B2.5): every vSID write or clear, global or per aircraft,
		// advances the provider revision.
		const std::uint64_t providerRevision = api.provider_revision(ProviderId);
		if (providerRevision == LastProviderRevision &&
			tick.callsigns == LastScannedCallsigns)
		{
			return finish(false);
		}

		bool snapshotComplete = true;
		std::map<std::string, bool> automaticModes;
		if (Provider.Field(AutomaticModeField) != ESB_FIELD_NONE)
		{
			std::string snapshot;
			const ReadStatus automaticStatus = VsmrPluginBridge::ReadGlobalString(
				api,
				Provider.Field(AutomaticModeField),
				Fields[AutomaticModeField].expectedBytes,
				snapshot);
			if (automaticStatus == ReadStatus::ProviderLost)
				return finish(DisconnectProvider());
			if (automaticStatus == ReadStatus::Failed)
				snapshotComplete = false;
			// Unset or malformed snapshots stay Unknown, never an inferred Off state.
			if (automaticStatus == ReadStatus::Value)
				automaticModes = ParseAutomaticModes(snapshot);
		}

		std::map<std::string, VsmrParis::State> parisStates;
		if (Provider.Field(ParisStateField) != ESB_FIELD_NONE)
		{
			std::string snapshot;
			const ReadStatus parisStatus = VsmrPluginBridge::ReadGlobalString(
				api, Provider.Field(ParisStateField), Fields[ParisStateField].expectedBytes, snapshot);
			if (parisStatus == ReadStatus::ProviderLost)
				return finish(DisconnectProvider());
			if (parisStatus == ReadStatus::Failed)
				snapshotComplete = false;
			// Only vSID's published manual rules establish the selected button state.
			if (parisStatus == ReadStatus::Value)
				parisStates = VsmrParis::Parse(snapshot);
		}

		std::unordered_map<std::string, AircraftData> next;
		for (const std::string& callsign : tick.callsigns)
		{
			ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
			// An aircraft the bridge has not seen cannot carry published values yet.
			if (VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft) != ReadStatus::Value)
				continue;

			AircraftData data;
			bool providerLost = false;
			const auto readField = [&](Field field, std::string& value)
			{
				// B2.3: a field that did not resolve with its expected type is never read.
				if (providerLost || Provider.Field(field) == ESB_FIELD_NONE)
					return;
				std::string raw;
				switch (VsmrPluginBridge::ReadAircraftString(
					api,
					callsign,
					aircraft,
					Provider.Field(field),
					Fields[field].expectedBytes,
					raw))
				{
				case ReadStatus::Value:
					value = NormalizeFieldValue(raw);
					break;
				case ReadStatus::ProviderLost:
					providerLost = true;
					break;
				case ReadStatus::Failed:
					snapshotComplete = false;
					break;
				default:
					// B2.8: unset means vSID holds no value for this aircraft.
					break;
				}
			};
			readField(SidField, data.sid);
			readField(RunwayField, data.runway);
			readField(ClearedFlightLevelField, data.clearedFlightLevel);
			if (providerLost)
				return finish(DisconnectProvider());
			// A bridge aircraft handle can exist without vSID publishing data for it.
			// Such handles are not connected vSID aircraft and must not inflate status.
			if (HasPublishedAircraftData(data))
				next.emplace(callsign, std::move(data));
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete ? providerRevision : NoRevision;
		LastScannedCallsigns = tick.callsigns;
		return finish(ReplaceSnapshot(std::move(next), std::move(automaticModes), std::move(parisStates)));
	}
	catch (const std::exception& exception)
	{
		Logger::info("vSID bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("vSID bridge poll failed: unknown exception");
	}
	return finish(DisconnectProvider());
}

VsmrVsid::InterfaceState VsmrVsid::GetInterfaceState(const std::string& airport)
{
	InterfaceState state;
	const AttachState attachState = VsmrPluginBridge::GetAttachState();
	state.bridgeLoaded = attachState != AttachState::NotLoaded;
	state.bridgeCompatible = attachState == AttachState::Attached;
	state.providerReady = state.bridgeCompatible &&
		ProviderReady.load(std::memory_order_relaxed);
	state.parisCommandsAvailable = state.providerReady && ParisCommandsAvailable.load(std::memory_order_relaxed);
	state.regionalCommandsAvailable = state.providerReady && RegionalCommandsAvailable.load(std::memory_order_relaxed);
	state.commandLineBusy = VsmrEuroScopeCommandLine::IsBusy();
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		state.aircraftCount = AircraftByCallsign.size();
		const auto automatic = AutomaticModes.find(NormalizeAirport(airport));
		if (state.providerReady && automatic != AutomaticModes.end())
			state.automaticMode = automatic->second;
		const auto paris = ParisStates.find(NormalizeAirport(airport));
		if (state.providerReady && paris != ParisStates.end()) state.paris = paris->second;
	}
	return state;
}

bool VsmrVsid::SubmitCommand(
	CommandAction action,
	const std::string& activeAirport,
	std::string& error)
{
	error.clear();
	const InterfaceState state = GetInterfaceState(activeAirport);
	if (!state.bridgeLoaded)
	{
		error = VsmrPluginBridge::MissingBridgeMessage();
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
	if (IsParisAction(action) && !state.parisCommandsAvailable)
	{
		error = "Paris runway controls require the companion vSID build and airport configuration.";
		return false;
	}
	if (IsRegionalAction(action) && !state.regionalCommandsAvailable)
	{
		error = "Regional configuration buttons require the companion vSID build with bridge schema 1.3.";
		return false;
	}
	if (!VsmrEuroScopeCommandLine::Begin(
		VsmrEuroScopeCommandLine::Owner::Vsid,
		command,
		&error))
	{
		return false;
	}
	return true;
}

bool VsmrVsid::TryGetAircraftData(
	const std::string& callsign,
	AircraftData& outData)
{
	const std::string normalizedCallsign = VsmrPluginBridge::NormalizeCallsign(callsign);
	if (normalizedCallsign.empty())
		return false;
	std::lock_guard<std::mutex> guard(StateMutex);
	const auto found = AircraftByCallsign.find(normalizedCallsign);
	if (found == AircraftByCallsign.end())
		return false;
	outData = found->second;
	return true;
}

void VsmrVsid::Shutdown() noexcept
{
	VsmrEuroScopeCommandLine::Cancel(
		VsmrEuroScopeCommandLine::Owner::Vsid);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
		AutomaticModes.clear();
		ParisStates.clear();
	}
	Provider.Reset();
	Diagnostics.Reset();
	ProviderReady.store(false, std::memory_order_relaxed);
	ParisCommandsAvailable.store(false, std::memory_order_relaxed);
	RegionalCommandsAvailable.store(false, std::memory_order_relaxed);
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
	InterfaceStateInitialized = false;
	LastAttachState = AttachState::NotLoaded;
	LastProviderReady = false;
}
