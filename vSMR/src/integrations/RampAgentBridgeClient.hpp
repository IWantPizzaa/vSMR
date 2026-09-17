#pragma once

#include "integrations/RampAgentBridgeData.hpp"

#include <string>

namespace VsmrPluginBridge
{
	struct Tick;
}

namespace VsmrRampAgent
{
	// Ramp Agent publishes stand assignments only through the plug-in bridge; vSMR
	// no longer reads flight strip annotations 3 and 4. Timer callback only.
	bool Poll(const VsmrPluginBridge::Tick& tick);
	bool TryGetAircraftData(const std::string& callsign, AircraftData& outData);
	void Shutdown() noexcept;
}
