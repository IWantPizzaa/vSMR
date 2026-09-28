#pragma once

#include <cmath>
#include <vector>

namespace VsmrAviso
{
	template <typename Point>
	double ProjectedAxisLength(const Point& from, const Point& to)
	{
		return std::hypot(static_cast<double>(to.X) - from.X,
			static_cast<double>(to.Y) - from.Y);
	}

	template <typename Point>
	void AppendProjectedRasterPoint(std::vector<Point>& points, const Point& point,
		double minDistanceSquared, bool preserveVertex)
	{
		if (!points.empty())
		{
			const double dx = static_cast<double>(point.X) - points.back().X;
			const double dy = static_cast<double>(point.Y) - points.back().Y;
			if (dx == 0.0 && dy == 0.0)
				return;
			// Polygon corners (and line endpoints) are not disposable merely
			// because their adjoining edge becomes subpixel at a distant zoom.
			if (!preserveVertex && dx * dx + dy * dy < minDistanceSquared)
				return;
		}
		points.push_back(point);
	}
}
