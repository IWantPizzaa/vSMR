#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/PluginBridgeApi.hpp"

#include "EuroScopePlugIn.h"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using namespace EuroScopePlugIn;
	using namespace VsmrPluginBridgeAbi;

	constexpr char ProviderId[] = "com.viffsys.cdm";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;
	constexpr std::size_t MaximumFlightPlans = 4096U;
	constexpr std::size_t MinimumApiSize =
		offsetof(ApiV1, providerRevision) + sizeof(decltype(ApiV1::providerRevision));

	enum class Field : std::size_t
	{
		Tobt,
		Tsat,
		Ttot,
		Ctot,
		Tsac,
		Asrt,
		Asat,
		Deice,
		TobtSetBy,
		FlowRestriction,
		EcfmpRestriction,
		ManualCtot,
		Count
	};

	struct FieldDefinition
	{
		const char* name;
		Type type;
	};

	constexpr std::array<FieldDefinition, static_cast<std::size_t>(Field::Count)> Fields = { {
		{ "tobt", Integer },
		{ "tsat", Integer },
		{ "ttot", Integer },
		{ "ctot", Integer },
		{ "tsac", Integer },
		{ "asrt", Integer },
		{ "asat", Integer },
		{ "deice", String },
		{ "tobt_set_by", String },
		{ "flow_restriction", String },
		{ "ecfmp_restriction", String },
		{ "manual_ctot", Boolean }
	} };

	std::mutex StateMutex;
	std::unordered_map<std::string, VsmrCdm::AircraftData> AircraftByCallsign;
	std::unordered_set<std::string> LastScannedCallsigns;
	HMODULE BridgeModule = nullptr;
	const ApiV1* BridgeApi = nullptr;
	std::array<FieldId, Fields.size()> FieldIds{};
	std::uint64_t LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
	bool ProviderReadyLogged = false;
	bool IncompatibleSchemaLogged = false;

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
		FieldIds.fill(0U);
		LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
		LastScannedCallsigns.clear();
		ProviderReadyLogged = false;
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

	bool ResolveFields()
	{
		std::uint32_t major = 0U;
		std::uint32_t minor = 0U;
		if (BridgeApi->providerVersion(ProviderId, &major, &minor) != Ok)
		{
			ResetProviderState();
			return false;
		}
		(void)minor;
		if (major != SupportedSchemaMajor)
		{
			ResetProviderState();
			if (!IncompatibleSchemaLogged)
			{
				Logger::info("CDM bridge provider uses an unsupported schema major version");
				IncompatibleSchemaLogged = true;
			}
			return false;
		}
		IncompatibleSchemaLogged = false;

		if (FieldIds.front() != 0U)
			return true;

		for (std::size_t index = 0U; index < Fields.size(); ++index)
		{
			const std::string qualifiedName =
				std::string(ProviderId) + "/" + Fields[index].name;
			if (BridgeApi->resolve(
				qualifiedName.c_str(), Fields[index].type, &FieldIds[index]) != Ok)
			{
				ResetProviderState();
				return false;
			}
		}
		LastProviderRevision = (std::numeric_limits<std::uint64_t>::max)();
		if (!ProviderReadyLogged)
		{
			Logger::info("CDM Next Gen interface connected through EuroScope Plugin Bridge");
			ProviderReadyLogged = true;
		}
		return true;
	}

	Status ReadInteger(Aircraft aircraft, Field field, std::optional<std::int64_t>& value)
	{
		Value bridgeValue{};
		std::uint32_t bytes = 0U;
		const Status status = BridgeApi->getAircraft(
			aircraft, FieldIds[static_cast<std::size_t>(field)],
			&bridgeValue, nullptr, &bytes);
		if (status == Unset)
		{
			value.reset();
			return Ok;
		}
		if (status != Ok)
			return status;
		if (bridgeValue.type != Integer || bridgeValue.bytes != sizeof(std::int64_t))
			return TypeMismatch;
		value = bridgeValue.data.integer;
		return Ok;
	}

	Status ReadBoolean(Aircraft aircraft, Field field, std::optional<bool>& value)
	{
		Value bridgeValue{};
		std::uint32_t bytes = 0U;
		const Status status = BridgeApi->getAircraft(
			aircraft, FieldIds[static_cast<std::size_t>(field)],
			&bridgeValue, nullptr, &bytes);
		if (status == Unset)
		{
			value.reset();
			return Ok;
		}
		if (status != Ok)
			return status;
		if (bridgeValue.type != Boolean || bridgeValue.bytes != sizeof(std::int32_t))
			return TypeMismatch;
		value = bridgeValue.data.boolean != 0;
		return Ok;
	}

	Status ReadString(Aircraft aircraft, Field field, std::string& value)
	{
		std::array<char, VsmrCdm::MaximumStringFieldBytes> buffer{};
		std::uint32_t bytes = static_cast<std::uint32_t>(buffer.size());
		Value bridgeValue{};
		const Status status = BridgeApi->getAircraft(
			aircraft, FieldIds[static_cast<std::size_t>(field)],
			&bridgeValue, buffer.data(), &bytes);
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
		value = VsmrCdm::NormalizeStringField(
			std::string_view(buffer.data(), bridgeValue.bytes));
		return Ok;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrCdm::AircraftData> next)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == next)
			return false;
		AircraftByCallsign = std::move(next);
		return true;
	}
}

bool VsmrCdm::Poll(EuroScopePlugIn::CPlugIn& plugin)
{
	try
	{
		if (!AttachBridge() || !ResolveFields())
			return ClearCachedAircraft();

		std::unordered_set<std::string> currentCallsigns;
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
			if (!callsign.empty())
				currentCallsigns.insert(callsign);
		}

		const std::uint64_t providerRevision = BridgeApi->providerRevision(ProviderId);
		if (providerRevision == LastProviderRevision &&
			currentCallsigns == LastScannedCallsigns)
		{
			return false;
		}

		std::unordered_map<std::string, AircraftData> next;
		bool snapshotComplete = true;
		for (const std::string& callsign : currentCallsigns)
		{
			Aircraft aircraft = 0U;
			if (BridgeApi->aircraft(callsign.c_str(), &aircraft) != Ok)
			{
				snapshotComplete = false;
				continue;
			}

			AircraftData data;
			const std::array<Status, static_cast<std::size_t>(Field::Count)> statuses = { {
				ReadInteger(aircraft, Field::Tobt, data.tobt),
				ReadInteger(aircraft, Field::Tsat, data.tsat),
				ReadInteger(aircraft, Field::Ttot, data.ttot),
				ReadInteger(aircraft, Field::Ctot, data.ctot),
				ReadInteger(aircraft, Field::Tsac, data.tsac),
				ReadInteger(aircraft, Field::Asrt, data.asrt),
				ReadInteger(aircraft, Field::Asat, data.asat),
				ReadString(aircraft, Field::Deice, data.deice),
				ReadString(aircraft, Field::TobtSetBy, data.tobtSetBy),
				ReadString(aircraft, Field::FlowRestriction, data.flowRestriction),
				ReadString(aircraft, Field::EcfmpRestriction, data.ecfmpRestriction),
				ReadBoolean(aircraft, Field::ManualCtot, data.manualCtot)
			} };
			if (std::find(statuses.begin(), statuses.end(), NoProvider) != statuses.end() ||
				std::find(statuses.begin(), statuses.end(), VsmrPluginBridgeAbi::Shutdown) != statuses.end())
			{
				ResetProviderState();
				return ClearCachedAircraft();
			}
			if (std::find_if(statuses.begin(), statuses.end(),
				[](Status status) { return status != Ok; }) != statuses.end())
			{
				snapshotComplete = false;
				continue;
			}
			if (HasPublishedAircraftData(data))
				next.emplace(callsign, std::move(data));
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete
			? providerRevision
			: (std::numeric_limits<std::uint64_t>::max)();
		LastScannedCallsigns = std::move(currentCallsigns);
		return ReplaceSnapshot(std::move(next));
	}
	catch (...)
	{
		Logger::info("CDM bridge poll failed");
		ResetProviderState();
		return ClearCachedAircraft();
	}
}

bool VsmrCdm::TryGetAircraftData(
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

VsmrCdm::InterfaceState VsmrCdm::GetInterfaceState()
{
	InterfaceState state;
	state.bridgeLoaded = BridgeModule != nullptr;
	state.bridgeCompatible = BridgeApi != nullptr;
	state.providerReady = BridgeApi != nullptr && FieldIds.front() != 0U;
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		state.aircraftCount = AircraftByCallsign.size();
	}
	return state;
}

void VsmrCdm::Shutdown() noexcept
{
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
	}
	BridgeModule = nullptr;
	BridgeApi = nullptr;
	ResetProviderState();
	IncompatibleSchemaLogged = false;
}
