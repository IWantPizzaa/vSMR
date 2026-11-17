#pragma once

#include "integrations/CdmBridgeData.hpp"
#include "integrations/CdmInterfaceData.hpp"

#include <cstddef>
#include <string>

namespace VsmrPluginBridge
{
	struct Tick;
}

namespace VsmrCdm
{
	struct InterfaceState
	{
		bool bridgeLoaded = false;
		bool bridgeCompatible = false;
		bool providerReady = false;
		std::size_t aircraftCount = 0U;
		bool commandLineBusy = false;
		bool proxy = false;
		std::optional<AirportPanel> panel;
	};

	bool Poll(const VsmrPluginBridge::Tick& tick);
	InterfaceState GetInterfaceState();
	void SetUiRefreshCallback(void (*callback)()) noexcept;
	bool SubmitCommand(CommandAction action, const std::string& airport, std::string& error);
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
