#pragma once

#include <cmath>

namespace VsmrCdmReminderSafety
{
	constexpr int MaximumGroundSpeedKnots = 5;
	constexpr int MaximumAbsoluteVerticalSpeedFeetPerMinute = 300;
	constexpr int MaximumPositionAgeSeconds = 15;
	constexpr double MaximumAirportDistanceNauticalMiles = 5.0;

	struct EligibilitySnapshot
	{
		bool activeAirportResolved = false;
		bool originMatchesActiveAirport = false;
		bool flightPlanNotStarted = false;
		bool simulatedFlightPlan = true;
		bool radarTargetValid = false;
		bool radarPositionValid = false;
		bool noGroundStatus = false;
		int positionAgeSeconds = MaximumPositionAgeSeconds + 1;
		int groundSpeedKnots = MaximumGroundSpeedKnots + 1;
		int verticalSpeedFeetPerMinute = MaximumAbsoluteVerticalSpeedFeetPerMinute + 1;
		double airportDistanceNauticalMiles = MaximumAirportDistanceNauticalMiles + 1.0;
	};

	inline bool IsEligible(const EligibilitySnapshot& snapshot) noexcept
	{
		return snapshot.activeAirportResolved &&
			snapshot.originMatchesActiveAirport &&
			snapshot.flightPlanNotStarted &&
			!snapshot.simulatedFlightPlan &&
			snapshot.radarTargetValid &&
			snapshot.radarPositionValid &&
			snapshot.noGroundStatus &&
			snapshot.positionAgeSeconds >= 0 &&
			snapshot.positionAgeSeconds <= MaximumPositionAgeSeconds &&
			snapshot.groundSpeedKnots >= 0 &&
			snapshot.groundSpeedKnots <= MaximumGroundSpeedKnots &&
			std::abs(snapshot.verticalSpeedFeetPerMinute) <=
				MaximumAbsoluteVerticalSpeedFeetPerMinute &&
			std::isfinite(snapshot.airportDistanceNauticalMiles) &&
			snapshot.airportDistanceNauticalMiles >= 0.0 &&
			snapshot.airportDistanceNauticalMiles <=
				MaximumAirportDistanceNauticalMiles;
	}
}
