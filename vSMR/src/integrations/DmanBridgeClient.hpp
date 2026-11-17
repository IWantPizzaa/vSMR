#pragma once

#include <optional>
#include <string>

namespace VsmrPluginBridge
{
	struct Tick;
}

namespace VsmrDman
{
	// DMAN's provider declaration (schema 1.x): "dman/pdc_station" is a global STR
	// field holding the EuroScope callsign of the station whose PDC DMAN drives
	// through PINEDE, unset while DMAN handles none. "dman/activated" is an
	// aircraft BOOL, set for the departures DMAN holds state for and true once
	// DMAN has handed the flight to ground. Timer callback only; returns true
	// when an aircraft's activated value changed.
	bool Poll(const VsmrPluginBridge::Tick& tick);
	// The station DMAN handles the PDC of, normalized; empty when DMAN is not
	// loaded, not attached to the bridge, or handles none.
	std::string GetPdcStation();
	// DMAN's activated value for the flight; empty when DMAN publishes none for
	// it (no DMAN, no bridge, field undeclared, or no DMAN state for the flight).
	std::optional<bool> GetActivated(const std::string& callsign);
	void Shutdown() noexcept;

	// The RDY indicator is green on DMAN's activation for a flight DMAN
	// publishes, and on CDM's ASRT otherwise.
	inline bool RdyGreen(std::optional<bool> dmanActivated, bool asrtSet) noexcept
	{
		return dmanActivated.has_value() ? *dmanActivated : asrtSet;
	}
}
