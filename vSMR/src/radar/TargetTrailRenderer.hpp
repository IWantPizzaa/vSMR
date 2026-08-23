#pragma once

#include "scene/RadarScene.hpp"

#include <Windows.h>
#include <GdiPlus.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace VsmrTargetTrail
{
	inline BYTE BlendChannel(std::uint8_t from, BYTE to, double amount)
	{
		return static_cast<BYTE>(std::clamp(
			static_cast<int>(std::lround(
				static_cast<double>(from) +
				(static_cast<double>(to) - static_cast<double>(from)) * amount)),
			0,
			255));
	}

	template <typename ProjectPoint, typename PointVisible>
	void Draw(
		Gdiplus::Graphics& graphics,
		const VsmrScene::Target& target,
		ProjectPoint&& projectPoint,
		PointVisible&& pointVisible,
		double symbolScale = 1.0)
	{
		symbolScale = std::clamp(symbolScale, 0.5, 1.5);
		if (target.style.icon == VsmrScene::IconStyle::Nova)
		{
			// Match the original vSMR/NOVA afterglow: retain the three previous
			// irregular primary-return silhouettes, oldest first, in cyan shades.
			constexpr BYTE afterglowChannels[] = { 255, 219, 183 };
			std::vector<Gdiplus::PointF> polygon;
			for (int historyIndex = 2; historyIndex >= 0; --historyIndex)
			{
				const std::vector<VsmrScene::GeoPoint>& history =
					target.primaryReturnAfterglow[static_cast<std::size_t>(historyIndex)];
				if (history.size() < 3)
					continue;
				polygon.clear();
				polygon.reserve(history.size());
				for (const VsmrScene::GeoPoint& sourcePoint : history)
				{
					if (!sourcePoint.valid)
						continue;
					const POINT point = projectPoint(sourcePoint);
					polygon.emplace_back(
						static_cast<Gdiplus::REAL>(point.x),
						static_cast<Gdiplus::REAL>(point.y));
				}
				if (polygon.size() >= 3 && std::abs(symbolScale - 1.0) > 0.0001)
				{
					Gdiplus::REAL centerX = 0.0f;
					Gdiplus::REAL centerY = 0.0f;
					for (const Gdiplus::PointF& point : polygon)
					{
						centerX += point.X;
						centerY += point.Y;
					}
					centerX /= static_cast<Gdiplus::REAL>(polygon.size());
					centerY /= static_cast<Gdiplus::REAL>(polygon.size());
					for (Gdiplus::PointF& point : polygon)
					{
						point.X = centerX + static_cast<Gdiplus::REAL>((point.X - centerX) * symbolScale);
						point.Y = centerY + static_cast<Gdiplus::REAL>((point.Y - centerY) * symbolScale);
					}
				}
				if (polygon.size() >= 3)
				{
					const BYTE channel = afterglowChannels[historyIndex];
					Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, channel, channel));
					graphics.FillPolygon(&brush, polygon.data(), static_cast<INT>(polygon.size()));
				}
			}
		}

		const std::size_t pointCount = target.trailPositions.size();
		if (pointCount == 0)
			return;

		const VsmrScene::Color& source = target.style.color;
		for (std::size_t index = 0; index < pointCount; ++index)
		{
			const VsmrScene::GeoPoint& history = target.trailPositions[index];
			if (!history.valid)
				continue;
			const POINT point = projectPoint(history);
			if (!pointVisible(point, 7))
				continue;

			const double age = pointCount > 1
				? static_cast<double>(index) / static_cast<double>(pointCount - 1)
				: 0.0;
			if (target.style.icon == VsmrScene::IconStyle::Nova)
			{
				// The original vSMR/NOVA trail is a compact white radar-history dot.
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
				graphics.FillRectangle(&brush, point.x - 1, point.y - 1, 2, 2);
				continue;
			}

			const BYTE red = BlendChannel(source.red, 112, age);
			const BYTE green = BlendChannel(source.green, 112, age);
			const BYTE blue = BlendChannel(source.blue, 112, age);
			const BYTE newestAlpha = static_cast<BYTE>(std::min<int>(source.alpha, 220));
			const BYTE alpha = BlendChannel(newestAlpha, 38, age);

			if (target.style.icon == VsmrScene::IconStyle::Realistic)
			{
				// Aircraft icons use small filled bubbles which grey and fade with age.
				const int diameter = std::clamp(
					static_cast<int>(std::lround(5.0 - age * 2.0)),
					2,
					5);
				const int radius = diameter / 2;
				Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, red, green, blue));
				graphics.FillEllipse(&brush, point.x - radius, point.y - radius, diameter, diameter);
				continue;
			}

			// Triangle (and the retained legacy Diamond style) follows the reference:
			// hollow circles become smaller and fainter farther behind the aircraft.
			const int diameter = std::clamp(
				static_cast<int>(std::lround(9.0 - age * 4.0)),
				4,
				9);
			const int radius = diameter / 2;
			Gdiplus::Pen pen(Gdiplus::Color(alpha, red, green, blue), 1.5f);
			graphics.DrawEllipse(&pen, point.x - radius, point.y - radius, diameter, diameter);
		}
	}
}
