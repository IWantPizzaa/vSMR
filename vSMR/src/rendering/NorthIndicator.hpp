#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace VsmrRendering
{
	// Sample the actual host projection, not a stored Rotate Screen preference.
	// The inset rotation and main compass must agree, including on CoFrance hosts.
	template<typename Project>
	double ProjectedNorthRotation(double latitude, double longitude, const Project& project)
	{
		if (!std::isfinite(latitude) || !std::isfinite(longitude)) return 0.0;
		const double centerLatitude = std::clamp(latitude, -85.0, 85.0);
		const POINT center = project(centerLatitude, longitude);
		double bestAngle = 0.0;
		double bestDistanceSquared = 0.0;
		for (double delta : { 0.02, 0.05, 0.1, 0.25, 0.5 })
		{
			const double northLatitude = std::clamp(latitude + delta, -85.0, 85.0);
			if (std::abs(northLatitude - centerLatitude) < 1e-9) continue;
			const POINT north = project(northLatitude, longitude);
			const double dx = static_cast<double>(north.x) - center.x;
			const double dy = static_cast<double>(north.y) - center.y;
			const double distanceSquared = dx * dx + dy * dy;
			if (distanceSquared < 4.0) continue;
			if (distanceSquared > bestDistanceSquared)
			{
				bestDistanceSquared = distanceSquared;
				bestAngle = std::atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
			}
			if (distanceSquared >= 2500.0) break;
		}
		return bestAngle;
	}

	inline bool ShowNorthIndicator(double degrees) noexcept
	{
		// Half a degree avoids flicker caused by integer-pixel projection rounding.
		return std::isfinite(degrees) && std::abs(std::remainder(degrees, 360.0)) > 0.5;
	}

	inline RECT NorthIndicatorBounds(const RECT& viewport, POINT offset = { -1, -1 }) noexcept
	{
		constexpr LONG diameter = 72;
		constexpr LONG margin = 8;
		const LONG maxX = viewport.right - viewport.left - diameter - margin;
		const LONG maxY = viewport.bottom - viewport.top - diameter - margin;
		if (maxX < margin || maxY < margin) return {};
		const LONG x = viewport.left + (offset.x < 0 ? maxX : std::clamp(offset.x, margin, maxX));
		const LONG y = viewport.top + (offset.y < 0 ? margin : std::clamp(offset.y, margin, maxY));
		return { x, y, x + diameter, y + diameter };
	}

	inline POINT NorthIndicatorDraggedOffset(const RECT& viewport, const RECT& moved) noexcept
	{
		const RECT bounds = NorthIndicatorBounds(viewport,
			{ (std::max)(0L, moved.left - viewport.left), (std::max)(0L, moved.top - viewport.top) });
		if (bounds.right <= bounds.left) return { -1, -1 };
		return { bounds.left - viewport.left, bounds.top - viewport.top };
	}

	// Small screen-space overlay: no raster rebuild or zoom scaling.
	inline bool DrawNorthIndicator(Gdiplus::Graphics& graphics, const RECT& viewport,
		double degrees, COLORREF background, POINT offset = { -1, -1 })
	{
		const RECT bounds = NorthIndicatorBounds(viewport, offset);
		if (!ShowNorthIndicator(degrees) || bounds.right <= bounds.left) return false;
		const Gdiplus::PointF center((bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f);
		const auto saved = graphics.Save();
		graphics.SetClip(Gdiplus::Rect(viewport.left, viewport.top,
			viewport.right - viewport.left, viewport.bottom - viewport.top), Gdiplus::CombineModeIntersect);
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		const bool light = (GetRValue(background) * 299 + GetGValue(background) * 587 +
			GetBValue(background) * 114) > 128000;
		Gdiplus::SolidBrush backing(Gdiplus::Color(215, GetRValue(background), GetGValue(background), GetBValue(background)));
		Gdiplus::SolidBrush bright(light ? Gdiplus::Color(255, 30, 42, 47) : Gdiplus::Color(255, 218, 227, 231));
		Gdiplus::SolidBrush shade(light ? Gdiplus::Color(255, 100, 115, 121) : Gdiplus::Color(255, 110, 131, 140));
		Gdiplus::Pen outline(light ? Gdiplus::Color(110, 30, 42, 47) : Gdiplus::Color(110, 218, 227, 231), 0.8f);
		graphics.FillEllipse(&backing, center.X - 35, center.Y - 35, 70.0f, 70.0f);
		const double radians = degrees * 3.14159265358979323846 / 180.0;
		const float cosine = static_cast<float>(std::cos(radians));
		const float sine = static_cast<float>(std::sin(radians));
		auto point = [&](float x, float y) {
			return Gdiplus::PointF(center.X + x * cosine - y * sine, center.Y + x * sine + y * cosine);
		};
		for (int spoke = 0; spoke < 8; ++spoke)
		{
			const double angle = spoke * 3.14159265358979323846 / 4;
			const float dx = static_cast<float>(std::sin(angle));
			const float dy = -static_cast<float>(std::cos(angle));
			const float length = spoke == 0 ? 21.0f : (spoke % 2 == 0 ? 16.0f : 10.0f);
			const float width = spoke % 2 == 0 ? 3.0f : 1.5f;
			const auto tip = point(dx * length, dy * length);
			Gdiplus::PointF left[]{ center, point(dy * width, -dx * width), tip };
			Gdiplus::PointF right[]{ center, tip, point(-dy * width, dx * width) };
			graphics.FillPolygon(&bright, left, 3);
			graphics.FillPolygon(&shade, right, 3);
			graphics.DrawLine(&outline, center, tip);
		}
		// Keep the letter upright while its position follows the north point.
		const auto northLabel = point(0, -28);
		Gdiplus::Font font(L"Arial", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		graphics.DrawString(L"N", 1, &font,
			Gdiplus::RectF(northLabel.X - 7, northLabel.Y - 7, 14, 14), &format, &bright);
		graphics.Restore(saved);
		return true;
	}
}
