#pragma once

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
}
