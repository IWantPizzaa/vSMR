#pragma once

#include "aircraft/GroundState.hpp"

#include <cstddef>

namespace VsmrRimcasLogic
{
	// LFLB's parallel grass runway and Sierra lie inside the generic 185 m
	// corridor. Use the published pavement widths for this close runway pair.
	// SIA AD 2 LFLB 2.12: 18/36 = 45 m, 18L/36R = 60 m.
	inline float RunwayHalfWidth(const std::string& airport, const std::string& first, const std::string& second)
	{
		if (airport == "LFLB")
		{
			if ((first == "18" && second == "36") || (first == "36" && second == "18")) return 22.5f;
			if ((first == "18L" && second == "36R") || (first == "36R" && second == "18L")) return 30.0f;
		}
		return 92.5f;
	}
	constexpr bool IsRunwayEntryAuthorized(GroundStateCategory state) noexcept
	{
		return state == GroundStateCategory::Depa || state == GroundStateCategory::Lnup || state == GroundStateCategory::Cross;
	}

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
			if (onRunway && (assignedSpeed == VsmrGroundStateSync::LineupAssignedSpeed || assignedSpeed == VsmrGroundStateSync::CrossingAssignedSpeed) &&
				classifyGroundState(state, reportedGroundSpeed, onRunway) == GroundStateCategory::Taxi)
				return VsmrGroundStateSync::CategoryForAssignedSpeed(assignedSpeed);
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
