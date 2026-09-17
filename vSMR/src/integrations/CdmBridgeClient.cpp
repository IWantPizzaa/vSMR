#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
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

	// CDM schema 1.0, declared by kCdmBridgeFields in CDMSingle.cpp
	// (IWantPizzaa/CDM commit 1df65062). All fields are aircraft-scoped.
	constexpr char ProviderId[] = "com.viffsys.cdm";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
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
		FieldCount
	};

	// Times are I64 minutes since midnight UTC, the unit
	// VsmrCdm::FormatTimeToken renders.
	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "tobt", ESB_T_I64, 0U },
		{ "tsat", ESB_T_I64, 0U },
		{ "ttot", ESB_T_I64, 0U },
		{ "ctot", ESB_T_I64, 0U },
		{ "tsac", ESB_T_I64, 0U },
		{ "asrt", ESB_T_I64, 0U },
		{ "asat", ESB_T_I64, 0U },
		{ "deice", ESB_T_STR, 32U },
		{ "tobt_set_by", ESB_T_STR, 16U },
		{ "flow_restriction", ESB_T_STR, 512U },
		{ "ecfmp_restriction", ESB_T_STR, 64U },
		{ "manual_ctot", ESB_T_BOOL, 0U }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("CDM");

	// Snapshot shared with rendering and the datalink workflows.
	std::mutex StateMutex;
	std::unordered_map<std::string, VsmrCdm::AircraftData> AircraftByCallsign;
	std::atomic<bool> ProviderReady{ false };

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;

	bool DisconnectProvider()
	{
		Provider.Reset();
		ProviderReady.store(false, std::memory_order_relaxed);
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty())
			return false;
		AircraftByCallsign.clear();
		return true;
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

bool VsmrCdm::Poll(const VsmrPluginBridge::Tick& tick)
{
	try
	{
		if (tick.api == nullptr)
			return DisconnectProvider();
		const ESB_Api_v1& api = *tick.api;

		// CDM is optional: an absent provider is a normal configuration (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return DisconnectProvider();
		ProviderReady.store(true, std::memory_order_relaxed);

		// Coarse gate (B2.5): every CDM write or clear advances the provider revision.
		const std::uint64_t providerRevision = api.provider_revision(ProviderId);
		if (providerRevision == LastProviderRevision &&
			tick.callsigns == LastScannedCallsigns)
		{
			return false;
		}

		std::unordered_map<std::string, AircraftData> next;
		bool snapshotComplete = true;
		for (const std::string& callsign : tick.callsigns)
		{
			ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
			// An aircraft the bridge has not seen cannot carry published values yet.
			if (VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft) != ReadStatus::Value)
				continue;

			AircraftData data;
			bool providerLost = false;
			// B2.3: a field that did not resolve with its expected type is never read.
			const auto readable = [&](Field field)
			{
				return !providerLost && Provider.Field(field) != ESB_FIELD_NONE;
			};
			// B2.8: unset leaves the value empty; only failures force a rescan.
			const auto accept = [&](ReadStatus status)
			{
				if (status == ReadStatus::ProviderLost)
					providerLost = true;
				else if (status == ReadStatus::Failed)
					snapshotComplete = false;
				return status == ReadStatus::Value;
			};
			const auto readTime = [&](Field field, std::optional<std::int64_t>& value)
			{
				std::int64_t minutes = 0;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftInteger(
					api, callsign, aircraft, Provider.Field(field), minutes)))
				{
					value = minutes;
				}
			};
			const auto readText = [&](Field field, std::string& value)
			{
				std::string raw;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftString(
					api, callsign, aircraft, Provider.Field(field),
					Fields[field].expectedBytes, raw)))
				{
					value = NormalizeStringField(raw);
				}
			};
			const auto readFlag = [&](Field field, std::optional<bool>& value)
			{
				bool flag = false;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftBoolean(
					api, callsign, aircraft, Provider.Field(field), flag)))
				{
					value = flag;
				}
			};
			readTime(Tobt, data.tobt);
			readTime(Tsat, data.tsat);
			readTime(Ttot, data.ttot);
			readTime(Ctot, data.ctot);
			readTime(Tsac, data.tsac);
			readTime(Asrt, data.asrt);
			readTime(Asat, data.asat);
			readText(Deice, data.deice);
			readText(TobtSetBy, data.tobtSetBy);
			readText(FlowRestriction, data.flowRestriction);
			readText(EcfmpRestriction, data.ecfmpRestriction);
			readFlag(ManualCtot, data.manualCtot);
			if (providerLost)
				return DisconnectProvider();
			if (HasPublishedAircraftData(data))
				next.emplace(callsign, std::move(data));
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete ? providerRevision : NoRevision;
		LastScannedCallsigns = tick.callsigns;
		return ReplaceSnapshot(std::move(next));
	}
	catch (const std::exception& exception)
	{
		Logger::info("CDM bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("CDM bridge poll failed: unknown exception");
	}
	return DisconnectProvider();
}

bool VsmrCdm::TryGetAircraftData(
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

VsmrCdm::InterfaceState VsmrCdm::GetInterfaceState()
{
	InterfaceState state;
	const AttachState attachState = VsmrPluginBridge::GetAttachState();
	state.bridgeLoaded = attachState != AttachState::NotLoaded;
	state.bridgeCompatible = attachState == AttachState::Attached;
	state.providerReady = state.bridgeCompatible &&
		ProviderReady.load(std::memory_order_relaxed);
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
	Provider.Reset();
	Diagnostics.Reset();
	ProviderReady.store(false, std::memory_order_relaxed);
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
}
