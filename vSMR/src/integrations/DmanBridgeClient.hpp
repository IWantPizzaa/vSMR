#pragma once

#include <string>

namespace VsmrPluginBridge
{
	struct Tick;
}

namespace VsmrDman
{
	// DMAN's provider declaration (schema 1.x): "dman/pdc_station" is a global STR
	// field holding the EuroScope callsign of the station whose PDC DMAN drives
	// through PINEDE, unset while DMAN handles none. Timer callback only.
	void Poll(const VsmrPluginBridge::Tick& tick);
	// The station DMAN handles the PDC of, normalized; empty when DMAN is not
	// loaded, not attached to the bridge, or handles none.
	std::string GetPdcStation();
	void Shutdown() noexcept;
}
