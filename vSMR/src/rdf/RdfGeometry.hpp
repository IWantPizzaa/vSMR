#pragma once

#include <Windows.h>
#include <algorithm>
#include <vector>

namespace VsmrRdf
{
	inline bool Contains(const RECT& rect, POINT point)
	{
		return point.x >= rect.left && point.x < rect.right &&
			point.y >= rect.top && point.y < rect.bottom;
	}

	inline bool IsVisible(POINT point, const std::vector<RECT>& visibleAreas)
	{
		return std::any_of(visibleAreas.begin(), visibleAreas.end(),
			[point](const RECT& area) { return Contains(area, point); });
	}

	// Subtract the actual inset frames, not their bounding box: radar space
	// between floating or overlapping insets must remain usable.
	inline std::vector<RECT> VisibleAreas(const RECT& viewport, const std::vector<RECT>& occlusions)
	{
		if (viewport.right <= viewport.left || viewport.bottom <= viewport.top)
			return {};
		std::vector<RECT> areas{ viewport };
		for (const RECT& cover : occlusions)
		{
			std::vector<RECT> remaining;
			for (const RECT& area : areas)
			{
				RECT intersection{};
				if (!::IntersectRect(&intersection, &area, &cover))
				{
					remaining.push_back(area);
					continue;
				}
				const RECT pieces[] = {
					{ area.left, area.top, area.right, intersection.top },
					{ area.left, intersection.bottom, area.right, area.bottom },
					{ area.left, intersection.top, intersection.left, intersection.bottom },
					{ intersection.right, intersection.top, area.right, intersection.bottom }
				};
				for (const RECT& piece : pieces)
					if (piece.right > piece.left && piece.bottom > piece.top)
						remaining.push_back(piece);
			}
			areas.swap(remaining);
		}
		return areas;
	}

	inline POINT DirectionOrigin(const RECT& viewport, const std::vector<RECT>& visibleAreas)
	{
		const auto center = [](const RECT& rect) -> POINT {
			return { rect.left + (rect.right - rect.left) / 2,
				rect.top + (rect.bottom - rect.top) / 2 };
		};
		const POINT usualOrigin = center(viewport);
		if (visibleAreas.empty() || IsVisible(usualOrigin, visibleAreas))
			return usualOrigin;
		const auto area = [](const RECT& rect) {
			return static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
		};
		return center(*std::max_element(visibleAreas.begin(), visibleAreas.end(),
			[&](const RECT& left, const RECT& right) { return area(left) < area(right); }));
	}
}
