#pragma once

#include <algorithm>
#include <cmath>

namespace VsmrAviso
{
	// Spend the bitmap budget on visible pixels first, then on pan overscan.
	// The final raster still applies its hard allocation caps for larger desktops.
	inline double NativeResolutionOverscan(double width, double height, double maxPixels)
	{
		if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0)
			return 0.0;
		const double expansion = (std::min)({ 2.0, 6398.0 / (std::max)(width, height),
			std::sqrt((maxPixels * 0.999) / (width * height)) });
		return (std::max)(0.0, (expansion - 1.0) * 0.5);
	}

	template <typename Point>
	double NativeResolutionOverscan(const Point& tl, const Point& tr,
		const Point& bl, const Point& br, double maxPixels)
	{
		const double width = (std::max)({ tl.X, tr.X, bl.X, br.X }) -
			(std::min)({ tl.X, tr.X, bl.X, br.X });
		const double height = (std::max)({ tl.Y, tr.Y, bl.Y, br.Y }) -
			(std::min)({ tl.Y, tr.Y, bl.Y, br.Y });
		return NativeResolutionOverscan(width, height, maxPixels);
	}

	inline double RasterWorkingMargin(double viewSpan, double cachedViewSpan, double cachedRenderSpan)
	{
		// Refresh after consuming half the available overscan. A fixed 25% margin
		// would continuously rebuild a native-resolution 4K cache with less overscan.
		return (std::max)(0.0, (std::min)(viewSpan * 0.25,
			(cachedRenderSpan - cachedViewSpan) * 0.25));
	}
}
