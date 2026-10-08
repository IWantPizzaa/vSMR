#pragma once

#include <cmath>

namespace VsmrAvisoViewportCenter
{
	inline bool OverlapsAirport(
		double centerLatitude, double centerLongitude,
		double halfLatitudeSpan, double halfLongitudeSpan,
		double minLatitude, double maxLatitude,
		double minLongitude, double maxLongitude) noexcept
	{
		return std::isfinite(centerLatitude) && std::isfinite(centerLongitude) &&
			std::isfinite(halfLatitudeSpan) && std::isfinite(halfLongitudeSpan) &&
			halfLatitudeSpan >= 0.0 && halfLongitudeSpan >= 0.0 &&
			std::isfinite(minLatitude) && std::isfinite(maxLatitude) &&
			std::isfinite(minLongitude) && std::isfinite(maxLongitude) &&
			minLatitude <= maxLatitude && minLongitude <= maxLongitude &&
			centerLatitude + halfLatitudeSpan >= minLatitude &&
			centerLatitude - halfLatitudeSpan <= maxLatitude &&
			centerLongitude + halfLongitudeSpan >= minLongitude &&
			centerLongitude - halfLongitudeSpan <= maxLongitude;
	}

	inline bool HasRestorableCenter(bool latitudeLoaded, bool longitudeLoaded,
		bool initialized) noexcept
	{
		return latitudeLoaded && longitudeLoaded && initialized;
	}
}
