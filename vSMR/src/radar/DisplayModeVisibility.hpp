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
		GroundStateCategory targetStatus) noexcept
	{
		const bool airborne = VsmrTargetRoleLogic::IsAirborneForTagRole(isArrival, reportedGs);
		if (airborne && !statuses.airborne)
			return false;
		if (targetOnRunway && !statuses.onRunway)
			return false;

		// Actual arrivals retain their role, including after landing.
		if (isArrival)
			return statuses.arrivals;

		// Resolve ground states before the generic non-local destination fallback.
		if (!airborne &&
			(targetStatus == GroundStateCategory::Nsts ||
			 targetStatus == GroundStateCategory::Unknown))
		{
			return statuses.noStatus;
		}

		if (!airborne && targetStatus == GroundStateCategory::Gate)
			return statuses.parked;

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
		case GroundStateCategory::Unknown: return statuses.noStatus;
		case GroundStateCategory::Gate: return statuses.parked;
		case GroundStateCategory::Arr: return statuses.arrivals;
		default: return true;
		}
	}
}
