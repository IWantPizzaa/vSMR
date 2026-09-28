#pragma once

#include "integrations/VsidBridgeData.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace VsmrPluginBridge
{
	struct Tick;
}

namespace VsmrVsid
{
	struct InterfaceState
	{
		bool bridgeLoaded = false;
		bool bridgeCompatible = false;
		bool providerReady = false;
		bool parisCommandsAvailable = false;
		bool regionalCommandsAvailable = false;
		bool commandLineBusy = false;
		std::size_t aircraftCount = 0U;
		std::optional<bool> automaticMode;
		std::optional<VsmrParis::State> paris;
		bool liveLfpgTaxiAvailable = false;
		std::optional<LfpgTaxiMode> lfpgTaxiMode;
		// Last completed command sequence, not an authoritative area-state snapshot.
		std::optional<LfpgTaxiMode> lastSubmittedLfpgTaxiMode;
	};

	// Full polling happens from EuroScope's timer callback. Pending commands also
	// use a short-lived UI-thread timer to advance delivery and refresh buttons,
	// without scanning aircraft. Rendering reads snapshots; no worker calls the bridge.
	bool Poll(const VsmrPluginBridge::Tick& tick);
	// UI thread only; cleared on shutdown. Uses the host's radar refresh API,
	// since invalidating a native window alone may repaint a cached radar image.
	void SetUiRefreshCallback(void (*callback)()) noexcept;
	InterfaceState GetInterfaceState(const std::string& airport = {});
	bool SubmitCommand(
		CommandAction action,
		const std::string& activeAirport,
		std::string& error);
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
