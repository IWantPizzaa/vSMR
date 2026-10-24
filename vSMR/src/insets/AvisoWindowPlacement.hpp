#pragma once

#include "insets/InsetWindow.hpp"

#include <mutex>
#include <optional>

namespace VsmrAvisoWindowPlacement
{
	struct Placement
	{
		RECT area{};
		CInsetWindow::AvisoLayoutMode layoutMode = CInsetWindow::AvisoLayoutMode::Floating;
	};

	inline std::mutex sharedMutex;
	inline std::optional<Placement> lastPlacement;

	inline bool Valid(const RECT& area)
	{
		return area.right > area.left && area.bottom > area.top;
	}

	inline void Remember(const CInsetWindow& window)
	{
		if (!window.IsAvisoViewport() || !Valid(window.m_Area))
			return;
		std::lock_guard<std::mutex> lock(sharedMutex);
		lastPlacement = Placement{ window.m_Area, window.m_AvisoLayoutMode };
	}

	inline bool Restore(CInsetWindow& window)
	{
		if (!window.IsAvisoViewport())
			return false;
		std::lock_guard<std::mutex> lock(sharedMutex);
		if (!lastPlacement)
			return false;
		window.m_Area = lastPlacement->area;
		window.m_AvisoLayoutMode = lastPlacement->layoutMode;
		window.ClearAvisoViewportCache();
		return true;
	}
}
