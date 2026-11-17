#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/DmanBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>

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
		FieldCount
	};

	// A EuroScope callsign; longer values are followed by the read helper anyway
	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "pdc_station", ESB_T_STR, 32U }
	} };

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("DMAN");

	std::mutex StateMutex;
	std::string PdcStation;

	void SetPdcStation(std::string station)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PdcStation = std::move(station);
	}

	void DisconnectProvider()
	{
		Provider.Reset();
		SetPdcStation("");
	}
}

void VsmrDman::Poll(const VsmrPluginBridge::Tick& tick)
{
	try
	{
		if (tick.api == nullptr)
			return DisconnectProvider();
		const ESB_Api_v1& api = *tick.api;

		// DMAN is optional: an absent provider means it handles no station (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready || Provider.Field(PdcStationField) == ESB_FIELD_NONE)
			return DisconnectProvider();

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
			DisconnectProvider();
			break;
		case ReadStatus::Failed:
			// keep the last value: a passing read error must not start a second poller
			break;
		default:
			// B2.8: unset while DMAN handles no station
			SetPdcStation("");
			break;
		}
		return;
	}
	catch (const std::exception& exception)
	{
		Logger::info("DMAN bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("DMAN bridge poll failed: unknown exception");
	}
	DisconnectProvider();
}

std::string VsmrDman::GetPdcStation()
{
	std::lock_guard<std::mutex> guard(StateMutex);
	return PdcStation;
}

void VsmrDman::Shutdown() noexcept
{
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PdcStation.clear();
	}
	Provider.Reset();
	Diagnostics.Reset();
}
