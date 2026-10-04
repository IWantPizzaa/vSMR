#pragma once

#include "weather/WeatherStore.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace VsmrWeatherInset
{
	inline constexpr int MinimumWidth = 240;
	inline constexpr int MinimumContentHeight = 24;
	enum class Detail { Full, Compact, Mini };
	inline bool ValidDetail(const std::string& mode) { return mode == "full" || mode == "compact" || mode == "mini"; }
	inline const char* DetailName(Detail mode) { return mode == Detail::Full ? "Full" : mode == Detail::Compact ? "Compact" : "Mini"; }
	inline std::vector<std::string> NormalizeStations(const std::vector<std::string>& candidates)
	{
		std::vector<std::string> result;
		for (const auto& candidate : candidates)
		{
			const auto station = VsmrWeather::NormalizeIcao(candidate);
			if (!station.empty()) result.push_back(station);
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}
	struct Layout
	{
		Detail detail = Detail::Mini;
		int columns = 1, rows = 1, capacity = 1, pageCount = 1;
	};
	inline Layout ResolveLayout(int width, int height, int count, const std::string& preferred)
	{
		count = (std::max)(1, count);
		Layout result;
		const auto fit = [&](Detail detail, int cellWidth, int cellHeight) {
			result.detail = detail;
			result.columns = (std::max)(1, width / cellWidth);
			result.rows = (std::max)(1, height / cellHeight);
			result.capacity = result.columns * result.rows;
			return width >= cellWidth && height >= cellHeight && result.capacity >= count;
		};
		if (preferred == "full" && fit(Detail::Full, 306, 175)) {}
		else if (preferred != "mini" && fit(Detail::Compact, 150, 175)) {}
		else fit(Detail::Mini, MinimumWidth, MinimumContentHeight);
		result.columns = (std::min)(count, result.columns);
		result.capacity = result.columns * result.rows;
		result.pageCount = (count + result.capacity - 1) / result.capacity;
		result.rows = (std::min)(result.rows, (count + result.columns - 1) / result.columns);
		return result;
	}
	inline std::string WindText(const VsmrWeather::Snapshot& weather)
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
		return wind;
	}
	inline std::string CompactText(const std::string& station, const VsmrWeather::Snapshot& weather)
	{
		return (station.empty() ? "----" : station) + " " + WindText(weather) + " Q" +
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
