#pragma once

#include "aircraft/GroundState.hpp"

#include <cstddef>

namespace VsmrRimcasLogic
{
	struct RunwayMonitoring
	{
		bool arrivals = false;
		bool departures = false;
	};

	constexpr RunwayMonitoring ResolveSelectedRunwayMonitoring(
		bool endOneArrival,
		bool endOneDeparture,
		bool endTwoArrival,
		bool endTwoDeparture) noexcept
	{
		return {
			endOneArrival || endTwoArrival,
			endOneDeparture || endTwoDeparture
		};
	}

	constexpr bool IsRunwayOccupancyMonitored(bool arrivals, bool departures) noexcept
	{
		return arrivals || departures;
	}

	constexpr bool HasApproachingConflict(std::size_t runwayOccupantCount) noexcept
	{
		return runwayOccupantCount > 0;
	}

	inline GroundStateCategory ResolveRunwayMovementState(
		const char* correlatedState,
		int correlatedAssignedSpeed,
		const char* selectedState,
		int selectedAssignedSpeed,
		bool hasSelectedFlightPlan,
		int reportedGroundSpeed,
		bool onRunway)
	{
		const auto classify = [reportedGroundSpeed, onRunway](const char* state, int assignedSpeed)
		{
			// The airborne tag threshold must not revoke a runway clearance while
			// the aircraft is physically on the runway. An unchanged TAXI + lineup
			// marker remains LNUP even after the aircraft accelerates past 50 kt.
			if (onRunway && assignedSpeed == VsmrGroundStateSync::LineupAssignedSpeed &&
				classifyGroundState(state, reportedGroundSpeed, onRunway) == GroundStateCategory::Taxi)
				return GroundStateCategory::Lnup;
			return classifyGroundStateWithSharedState(
				state, reportedGroundSpeed, onRunway, assignedSpeed);
		};

		// EuroScope can momentarily expose a different ground state through the
		// selected and correlated flight-plan handles. The tag uses the selected
		// plan, so let its explicit status govern the movement alert too. If the
		// selected plan has no usable status, retain the correlated-plan fallback.
		if (hasSelectedFlightPlan && selectedState != nullptr && selectedState[0] != '\0')
		{
			const GroundStateCategory selected = classify(selectedState, selectedAssignedSpeed);
			if (selected != GroundStateCategory::Unknown)
				return selected;
		}
		return classify(correlatedState, correlatedAssignedSpeed);
	}
}
