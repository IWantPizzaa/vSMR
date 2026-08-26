#pragma once

#include <cstddef>

namespace VsmrRimcasLogic
{
	constexpr bool IsRunwayOccupancyMonitored(bool arrivals, bool departures) noexcept
	{
		return arrivals || departures;
	}

	constexpr bool HasApproachingConflict(std::size_t runwayOccupantCount) noexcept
	{
		return runwayOccupantCount > 0;
	}
}
