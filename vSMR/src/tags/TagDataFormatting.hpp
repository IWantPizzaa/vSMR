#pragma once
#include "tags/TagTokenValues.hpp"

namespace VsmrTags
{
	struct TagDataInput
	{
		bool hasRadarTarget = false, hasFlightPlan = false, receivedFlightPlan = false;
		bool correlated = false, proMode = false, primary = true, clearance = false, lineup = false;
		int groundSpeed = 0, flightLevel = 0, pressureAltitude = 0, transitionAltitude = 0;
		int altitudeDelta = 0, flightPlanState = 0;
		char assignedCommunication = 0, filedCommunication = 0, wake = '?';
		std::string callsign, assignedSquawk, squawk, aircraftType, departureRunway, arrivalRunway;
		std::string scratchpad, sid, origin, destination, groundState, stand, remark, holdingPoint;
	};

	// Formats one captured snapshot without SDK or integration lookups.
	void FormatTagData(const TagDataInput& input, TokenValues& output);
}
