#pragma once

#include "integrations/VsidBridgeData.hpp"

#include <string>

namespace EuroScopePlugIn
{
	class CPlugIn;
}

namespace VsmrVsid
{
	struct InterfaceState
	{
		bool bridgeLoaded = false;
		bool bridgeCompatible = false;
		bool providerReady = false;
		bool commandLineBusy = false;
		std::size_t aircraftCount = 0U;
		LfpgOperatingMode lfpgMode = LfpgOperatingMode::MinimumTaxiing;
		LfpgLinkMode lfpgLinkMode = LfpgLinkMode::Linked;
	};

	// Polling happens only from EuroScope's timer callback. Rendering reads the
	// resulting snapshot and never calls across the plug-in bridge from a worker.
	bool Poll(EuroScopePlugIn::CPlugIn& plugin);
	InterfaceState GetInterfaceState();
	bool SubmitCommand(
		CommandAction action,
		const std::string& activeAirport,
		std::string& error);
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void ObserveAircraft(const std::string& callsign);
	void ForgetAircraft(const std::string& callsign);
	void Shutdown() noexcept;
}
