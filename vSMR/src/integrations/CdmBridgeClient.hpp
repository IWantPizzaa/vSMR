#pragma once

#include "integrations/CdmBridgeData.hpp"

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
	};

	bool Poll(const VsmrPluginBridge::Tick& tick);
	InterfaceState GetInterfaceState();
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
