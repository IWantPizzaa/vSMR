#pragma once

#include <Windows.h>

namespace VsmrRadarInteraction
{
	// EuroScope may render into a memory DC. Its owning frame is not necessarily
	// the radar view, so calibrate the coordinate space from an SDK mouse event.
	class HoverPointer
	{
	public:
		bool Observe(HWND window, POINT screenPoint, POINT radarPoint)
		{
			POINT clientPoint = screenPoint;
			if (window == nullptr || !::IsWindow(window) ||
				!::ScreenToClient(window, &clientPoint))
				return false;
			window_ = window;
			offset_ = { radarPoint.x - clientPoint.x, radarPoint.y - clientPoint.y };
			return true;
		}

		HWND Window() const { return window_; }

		bool Resolve(POINT screenPoint, HWND underCursor, HWND foreground, POINT& radarPoint) const
		{
			if (window_ == nullptr || !::IsWindow(window_) || foreground == nullptr ||
				(underCursor != window_ && !::IsChild(window_, underCursor)) ||
				::GetAncestor(foreground, GA_ROOT) != ::GetAncestor(window_, GA_ROOT) ||
				!::ScreenToClient(window_, &screenPoint))
				return false;
			radarPoint = { screenPoint.x + offset_.x, screenPoint.y + offset_.y };
			return true;
		}

	private:
		HWND window_ = nullptr;
		POINT offset_{};
	};

	inline bool NeedsHoverRefresh(UINT message, bool hasDetailedTags)
	{
		// Mouse movement must also be able to expand the *first* hovered tag.
		return message == WM_MOUSEMOVE || hasDetailedTags;
	}
}
