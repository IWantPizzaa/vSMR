#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/DmanBridgeClient.hpp"
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

	constexpr char ProviderId[] = "dman";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
	{
		PdcStationField,
		ActivatedField,
		FieldCount
	};

	// A EuroScope callsign; longer values are followed by the read helper anyway
	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "pdc_station", ESB_T_STR, 32U },
		{ "activated", ESB_T_BOOL, 0U }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("DMAN");

	// Shared with rendering and the datalink workflows.
	std::mutex StateMutex;
	std::string PdcStation;
	std::unordered_map<std::string, bool> ActivatedByCallsign;

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;

	void SetPdcStation(std::string station)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PdcStation = std::move(station);
	}

	bool ReplaceActivated(std::unordered_map<std::string, bool> next)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (ActivatedByCallsign == next)
			return false;
		ActivatedByCallsign = std::move(next);
		return true;
	}

	bool ClearActivated()
	{
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		return ReplaceActivated({});
	}

	bool DisconnectProvider()
	{
		Provider.Reset();
		SetPdcStation("");
		return ClearActivated();
	}

	// B2.5: every DMAN write or clear advances the provider revision, so an
	// unchanged revision over the same flights needs no aircraft read.
	bool PollActivated(const ESB_Api_v1& api, const VsmrPluginBridge::Tick& tick, bool& providerLost)
	{
		const std::uint64_t providerRevision = api.provider_revision(ProviderId);
		if (!Provider.SourceChanged() && providerRevision == LastProviderRevision &&
			tick.callsigns == LastScannedCallsigns)
		{
			return false;
		}

		std::unordered_map<std::string, bool> previous;
		{
			std::lock_guard<std::mutex> guard(StateMutex);
			previous = ActivatedByCallsign;
		}
		const auto keepPrevious = [&](const std::string& callsign, std::unordered_map<std::string, bool>& next)
		{
			const auto found = previous.find(callsign);
			if (found != previous.end())
				next.emplace(callsign, found->second);
		};

		std::unordered_map<std::string, bool> next;
		bool snapshotComplete = true;
		for (const std::string& callsign : tick.callsigns)
		{
			ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
			const ReadStatus resolved = VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft);
			if (resolved == ReadStatus::Failed)
			{
				// a passing bridge error keeps what DMAN last said about the flight
				keepPrevious(callsign, next);
				snapshotComplete = false;
				continue;
			}
			// An aircraft the bridge has not seen cannot carry a DMAN value yet.
			if (resolved != ReadStatus::Value)
				continue;

			bool activated = false;
			switch (VsmrPluginBridge::ReadAircraftBoolean(
				api, callsign, aircraft, Provider.Field(ActivatedField), activated))
			{
			case ReadStatus::Value:
				next.emplace(callsign, activated);
				break;
			case ReadStatus::ProviderLost:
				providerLost = true;
				return false;
			case ReadStatus::Failed:
				keepPrevious(callsign, next);
				snapshotComplete = false;
				break;
			default:
				// B2.8: unset while DMAN holds no state for the flight
				break;
			}
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete ? providerRevision : NoRevision;
		LastScannedCallsigns = tick.callsigns;
		return ReplaceActivated(std::move(next));
	}
}

bool VsmrDman::Poll(const VsmrPluginBridge::Tick& tick)
{
	try
	{
		if (tick.api == nullptr)
			return DisconnectProvider();
		const ESB_Api_v1& api = *tick.api;

		// DMAN is optional: an absent provider means it handles no station and
		// publishes no flight (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return DisconnectProvider();

		// B2.3: a field that did not resolve with its expected type is never read.
		if (Provider.Field(PdcStationField) == ESB_FIELD_NONE)
			SetPdcStation("");
		else
		{
			std::string station;
			switch (VsmrPluginBridge::ReadGlobalString(
				api,
				Provider.Field(PdcStationField),
				Fields[PdcStationField].expectedBytes,
				station))
			{
			case ReadStatus::Value:
				SetPdcStation(VsmrPluginBridge::NormalizeCallsign(station));
				break;
			case ReadStatus::ProviderLost:
				return DisconnectProvider();
			case ReadStatus::Failed:
				// keep the last value: a passing read error must not start a second poller
				break;
			default:
				// B2.8: unset while DMAN handles no station
				SetPdcStation("");
				break;
			}
		}

		if (Provider.Field(ActivatedField) == ESB_FIELD_NONE)
			return ClearActivated();
		bool providerLost = false;
		const bool changed = PollActivated(api, tick, providerLost);
		if (providerLost)
		{
			// Resolve again next tick. The station was read this tick: keeping it
			// spares the PDC cycle a one-tick return to PINEDE.
			Provider.Reset();
			return ClearActivated();
		}
		return changed;
	}
	catch (const std::exception& exception)
	{
		Logger::info("DMAN bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("DMAN bridge poll failed: unknown exception");
	}
	return DisconnectProvider();
}

std::string VsmrDman::GetPdcStation()
{
	std::lock_guard<std::mutex> guard(StateMutex);
	return PdcStation;
}

std::optional<bool> VsmrDman::GetActivated(const std::string& callsign)
{
	const std::string normalized = VsmrPluginBridge::NormalizeCallsign(callsign);
	if (normalized.empty())
		return std::nullopt;
	std::lock_guard<std::mutex> guard(StateMutex);
	const auto found = ActivatedByCallsign.find(normalized);
	if (found == ActivatedByCallsign.end())
		return std::nullopt;
	return found->second;
}

void VsmrDman::Shutdown() noexcept
{
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PdcStation.clear();
		ActivatedByCallsign.clear();
	}
	Provider.Reset();
	Diagnostics.Reset();
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
}
