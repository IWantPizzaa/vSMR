#pragma once

#include <cmath>
#include <string_view>

namespace VsmrRendering
{
	// Presentation pixels only. Geographic projections and radar zoom stay unchanged.
	inline double ResolutionScale(std::string_view preset) noexcept
	{
		if (preset == "4k") return 2.0;
		if (preset == "2k") return 1440.0 / 1080.0;
		return 1.0;
	}

	inline int ScalePixels(int pixels, double scale) noexcept
	{
		return static_cast<int>(std::lround(pixels * scale));
	}

	// The tag font setting is already in screen pixels, including on 2K/4K displays.
	inline int TagFontPixels(int configuredPixels) noexcept
	{
		return configuredPixels < 6 ? 6 : configuredPixels;
	}
}
