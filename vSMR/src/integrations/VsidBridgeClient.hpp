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
		// Last completed command sequence, not an authoritative area-state snapshot.
		std::optional<LfpgTaxiMode> lastSubmittedLfpgTaxiMode;
	};

	// Polling happens only from EuroScope's timer callback. Rendering reads the
	// resulting snapshot and never calls across the plug-in bridge from a worker.
	bool Poll(const VsmrPluginBridge::Tick& tick);
	InterfaceState GetInterfaceState(const std::string& airport = {});
	bool SubmitCommand(
		CommandAction action,
		const std::string& activeAirport,
		std::string& error);
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
