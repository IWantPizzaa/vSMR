#pragma once

#include "weather/WeatherStore.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdio>

namespace VsmrWeatherInset
{
	inline constexpr int MinimumWidth = 240;
	inline constexpr int MinimumContentHeight = 24;

	inline std::string CompactText(const std::string& station, const VsmrWeather::Snapshot& weather)
	{
		std::string wind = "--- --KT";
		if (weather.hasWind)
		{
			char direction[8] = {};
			std::snprintf(direction, sizeof(direction), "%03d", weather.windCalm ? 0 : weather.windDirectionDegrees);
			char speed[16] = {};
			std::snprintf(speed, sizeof(speed), "%02d", weather.windCalm ? 0 : weather.windSpeedKnots);
			wind = (weather.windVariable && !weather.windCalm ? "VRB" : std::string(direction)) + speed;
			if (weather.hasWindGust)
			{
				char gust[16] = {};
				std::snprintf(gust, sizeof(gust), "G%02d", weather.windGustKnots);
				wind += gust;
			}
			wind += "KT";
		}
		return (station.empty() ? "----" : station) + " " + wind + " Q" +
			(weather.hasQnh ? std::to_string(weather.qnhHpa) : "----");
	}

	// Weather stores content bounds in every layout mode. Snapping translates
	// that rectangle to an edge/corner; it never stretches it into a split view.
	template<class LayoutMode>
	RECT AnchorContent(LayoutMode mode, RECT content, const RECT& bounds, int titleHeight)
	{
		const LONG maxWidth = bounds.right - bounds.left;
		const LONG minTop = bounds.top + titleHeight;
		const LONG maxHeight = bounds.bottom - minTop;
		if (maxWidth <= 0 || maxHeight <= 0) return content;
		const LONG width = std::clamp<LONG>(content.right - content.left,
			(std::min<LONG>)(MinimumWidth, maxWidth), maxWidth);
		const LONG height = std::clamp<LONG>(content.bottom - content.top,
			(std::min<LONG>)(MinimumContentHeight, maxHeight), maxHeight);
		const LONG maxLeft = bounds.right - width;
		const LONG maxTop = bounds.bottom - height;
		LONG left = content.left, top = content.top;
		switch (mode)
		{
		case LayoutMode::SplitLeft: left = bounds.left; break;
		case LayoutMode::SplitRight: left = maxLeft; break;
		case LayoutMode::SplitTop: top = minTop; break;
		case LayoutMode::SplitBottom: top = maxTop; break;
		case LayoutMode::CornerTopLeft: left = bounds.left; top = minTop; break;
		case LayoutMode::CornerTopRight: left = maxLeft; top = minTop; break;
		case LayoutMode::CornerBottomLeft: left = bounds.left; top = maxTop; break;
		case LayoutMode::CornerBottomRight: left = maxLeft; top = maxTop; break;
		default: break;
		}
		left = std::clamp(left, bounds.left, maxLeft);
		top = std::clamp(top, minTop, maxTop);
		return {left, top, left + width, top + height};
	}
}
