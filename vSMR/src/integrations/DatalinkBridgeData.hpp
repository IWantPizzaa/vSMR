#pragma once

// The PDC state vSMR publishes on the plug-in bridge as vsmr/dcl, so a departure
// list in another plug-in (CoFrance) can show it next to the flight. Pure helpers
// only, kept apart from the provider so the encoding can be tested without a bridge.
namespace VsmrDatalinkBridge
{
	// Bits of the per-callsign datalink flags vSMR already builds for its proxy view
	// (CSMRPlugin::SynchronizeProxyDatalink). One aircraft may carry several.
	constexpr unsigned FlagDemanding = 1U;  // PINEDE request waiting for a controller
	constexpr unsigned FlagStandby = 2U;    // ... and the controller answered STANDBY
	constexpr unsigned FlagMessage = 4U;    // the pilot sent a message nobody answered
	constexpr unsigned FlagWilco = 8U;      // the pilot answered WILCO / ROGER
	constexpr unsigned FlagSent = 16U;      // a clearance or message went out

	// The one-letter value of vsmr/dcl, or '\0' when the aircraft has no PDC state
	// and the field should be unset. Same priority as vSMR's own datalink tag item,
	// so both plug-ins show the same thing for the same aircraft:
	//   R  request waiting           S  request on standby
	//   T  pilot message waiting     W  pilot answered WILCO
	//   C  clearance sent, no answer yet
	// Distinct letters where the vSMR tag tells WILCO from "sent" by colour only.
	constexpr char StateCode(unsigned flags) noexcept
	{
		if ((flags & FlagDemanding) != 0U)
			return (flags & FlagStandby) != 0U ? 'S' : 'R';
		if ((flags & FlagMessage) != 0U)
			return 'T';
		if ((flags & FlagWilco) != 0U)
			return 'W';
		if ((flags & FlagSent) != 0U)
			return 'C';
		return '\0';
	}
}
