#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	using VsmrPluginBridge::FieldSpec;
	using VsmrPluginBridge::ProviderState;
	using VsmrPluginBridge::ReadStatus;

	constexpr char ProviderId[] = "rampagent";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
	{
		StandField,
		RemarkField,
		FieldCount
	};

	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "stand", ESB_T_STR, VsmrRampAgent::StandMaximumBytes },
		{ "remark", ESB_T_STR, VsmrRampAgent::RemarkMaximumBytes }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("Ramp Agent");

	// Snapshot shared with tag rendering.
	std::mutex StateMutex;
	std::unordered_map<std::string, VsmrRampAgent::AircraftData> AircraftByCallsign;

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;

	bool DisconnectProvider()
	{
		Provider.Reset();
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty())
			return false;
		AircraftByCallsign.clear();
		return true;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrRampAgent::AircraftData> next)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == next)
			return false;
		AircraftByCallsign = std::move(next);
		return true;
	}
}

bool VsmrRampAgent::Poll(const VsmrPluginBridge::Tick& tick)
{
	try
	{
		if (tick.api == nullptr)
			return DisconnectProvider();
		const ESB_Api_v1& api = *tick.api;

		// Ramp Agent is optional: an absent provider is a normal configuration (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return DisconnectProvider();

		// Coarse gate (B2.5): Ramp Agent's set_ac and clear_ac both advance the
		// provider revision, so a released stand is noticed too.
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
					// Kept as validated UTF-8, which the tag renderer decodes directly.
					value = NormalizeText(raw, Fields[field].expectedBytes);
					break;
				case ReadStatus::ProviderLost:
					providerLost = true;
					break;
				case ReadStatus::Failed:
					snapshotComplete = false;
					break;
				default:
					// B2.8: unset while Ramp Agent holds no stand for this aircraft.
					break;
				}
			};
			readField(StandField, data.stand);
			readField(RemarkField, data.remark);
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
		Logger::info("Ramp Agent bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("Ramp Agent bridge poll failed: unknown exception");
	}
	return DisconnectProvider();
}

bool VsmrRampAgent::TryGetAircraftData(
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

void VsmrRampAgent::Shutdown() noexcept
{
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
	}
	Provider.Reset();
	Diagnostics.Reset();
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
}
