#pragma once

#include "aircraft/GroundState.hpp"

namespace VsmrDisplayModeVisibility
{
	template <typename StatusVisibility>
	bool IsVisible(
		const StatusVisibility& statuses,
		bool isArrival,
		bool isDeparture,
		bool hasDestination,
		int reportedGs,
		bool targetOnRunway,
		GroundStateCategory targetStatus,
		bool explicitlyParked = false,
		bool flightPlanDataReceived = true) noexcept
	{
		const bool airborne = VsmrTargetRoleLogic::IsAirborneForTagRole(isArrival, reportedGs);
		if (airborne && !statuses.airborne)
			return false;
		if (targetOnRunway && !statuses.onRunway)
			return false;
		if (!airborne && !flightPlanDataReceived)
			return statuses.noFlightPlan;
		// Gate also represents stationary targets with an empty ground status.
		// Only the explicit PARK/PARKED selection belongs to the Parked filter.
		if (!airborne && explicitlyParked)
			return statuses.parked;

		// Actual arrivals retain their role, including after landing.
		if (isArrival)
			return statuses.arrivals;

		// Resolve ground states before the generic non-local destination fallback.
		if (!airborne &&
			(targetStatus == GroundStateCategory::Nsts ||
			 targetStatus == GroundStateCategory::Gate ||
			 targetStatus == GroundStateCategory::Unknown))
		{
			return statuses.noStatus;
		}

		if (!isDeparture && hasDestination)
			return statuses.arrivals;

		switch (targetStatus)
		{
		case GroundStateCategory::Push: return statuses.push;
		case GroundStateCategory::Stup: return statuses.startup;
		case GroundStateCategory::Taxi: return statuses.taxi;
		case GroundStateCategory::Lnup: return statuses.lineup;
		case GroundStateCategory::Depa: return statuses.departure;
		case GroundStateCategory::Nsts:
		case GroundStateCategory::Gate:
		case GroundStateCategory::Unknown: return statuses.noStatus;
		case GroundStateCategory::Arr: return statuses.arrivals;
		default: return true;
		}
	}
}
