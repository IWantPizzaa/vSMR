#pragma once

#include "integrations/CdmBridgeData.hpp"

#include <string>

namespace EuroScopePlugIn
{
	class CPlugIn;
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

	bool Poll(EuroScopePlugIn::CPlugIn& plugin);
	InterfaceState GetInterfaceState();
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
