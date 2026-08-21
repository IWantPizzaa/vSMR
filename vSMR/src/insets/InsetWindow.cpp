#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
#include "weather/WeatherStore.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/logging/Logger.hpp"
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
	constexpr double kAvisoMetersPerNm = 1852.0;
	constexpr double kAvisoLatMetersPerDegree = 110540.0;
	constexpr double kAvisoLonMetersPerDegree = 111320.0;
	constexpr int kAvisoViewportTopBarHeight = 15;
	constexpr int kAvisoSnapThresholdPx = 28;
	constexpr int kAvisoCornerSnapThresholdPx = 48;
	constexpr int kAvisoMinLayoutWidth = 300;
	constexpr int kAvisoMinLayoutHeight = 120;
	constexpr int kInsetResizeHitPx = 7;
	constexpr int kInsetResizeInsidePx = 5;
	constexpr int kInsetResizeCornerPx = 18;
	constexpr int kInsetDragThresholdPx = 4;
	constexpr int kTimerColumnCount = 2;
	constexpr int kTimerRowCount = 2;
	constexpr int kTimerContentWidth = 84;
	constexpr int kTimerContentHeight = 56;

	using AvisoLayoutMode = CInsetWindow::AvisoLayoutMode;
	using ResizeRegion = CInsetWindow::ResizeRegion;

	double ClampAvisoLatitude(double latitude)
	{
		return std::clamp(latitude, -85.0, 85.0);
	}

	Gdiplus::Color SceneColorToGdi(const VsmrScene::Color& color)
	{
		return Gdiplus::Color(color.alpha, color.red, color.green, color.blue);
	}

	double AvisoCosLatitude(double latitude)
	{
		return max(0.05, std::abs(std::cos(DegToRad(latitude))));
	}

	bool AvisoWithinTolerance(double left, double right, double tolerance)
	{
		const double delta = left - right;
		return delta >= -tolerance && delta <= tolerance;
	}

	bool AvisoPointWithinTolerance(const Gdiplus::PointF& left, const Gdiplus::PointF& right, double tolerance)
	{
		return AvisoWithinTolerance(static_cast<double>(left.X), static_cast<double>(right.X), tolerance) &&
			AvisoWithinTolerance(static_cast<double>(left.Y), static_cast<double>(right.Y), tolerance);
	}

	bool AvisoProjectionTransformWithinTolerance(
		const Gdiplus::PointF& cachedTopLeft,
		const Gdiplus::PointF& cachedTopRight,
		const Gdiplus::PointF& cachedBottomLeft,
		double cachedLongitudeSpan,
		double cachedLatitudeSpan,
		const Gdiplus::PointF& currentTopLeft,
		const Gdiplus::PointF& currentTopRight,
		const Gdiplus::PointF& currentBottomLeft,
		double currentLongitudeSpan,
		double currentLatitudeSpan,
		double tolerance)
	{
		if (cachedLongitudeSpan <= 0.0 || cachedLatitudeSpan <= 0.0 ||
			currentLongitudeSpan <= 0.0 || currentLatitudeSpan <= 0.0)
		{
			return false;
		}

		const double cachedHorizontalX = static_cast<double>(cachedTopRight.X - cachedTopLeft.X);
		const double cachedHorizontalY = static_cast<double>(cachedTopRight.Y - cachedTopLeft.Y);
		const double cachedVerticalX = static_cast<double>(cachedBottomLeft.X - cachedTopLeft.X);
		const double cachedVerticalY = static_cast<double>(cachedBottomLeft.Y - cachedTopLeft.Y);
		const double currentHorizontalX = static_cast<double>(currentTopRight.X - currentTopLeft.X);
		const double currentHorizontalY = static_cast<double>(currentTopRight.Y - currentTopLeft.Y);
		const double currentVerticalX = static_cast<double>(currentBottomLeft.X - currentTopLeft.X);
		const double currentVerticalY = static_cast<double>(currentBottomLeft.Y - currentTopLeft.Y);

		// A wheel zoom changes the geographic span while keeping the viewport's
		// projection basis. Keep using the geo-anchored cache until its replacement
		// arrives instead of falling back to a stretched viewport crop.
		const bool sameViewportBasis =
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX, tolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY, tolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX, tolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY, tolerance);
		if (sameViewportBasis)
			return true;

		// A window resize can instead change the viewport span while preserving
		// the number of pixels per geographic degree.
		const double horizontalSpanRatio = cachedLongitudeSpan / currentLongitudeSpan;
		const double verticalSpanRatio = cachedLatitudeSpan / currentLatitudeSpan;
		return
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX * horizontalSpanRatio, tolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY * horizontalSpanRatio, tolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX * verticalSpanRatio, tolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY * verticalSpanRatio, tolerance);
	}

	Gdiplus::PointF RotateAvisoVector(double x, double y, double degrees)
	{
		if (!std::isfinite(degrees) || std::abs(degrees) < 0.001)
			return Gdiplus::PointF(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y));

		const double radians = DegToRad(degrees);
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		return Gdiplus::PointF(
			static_cast<Gdiplus::REAL>((x * cosine) - (y * sine)),
			static_cast<Gdiplus::REAL>((x * sine) + (y * cosine)));
	}

	Gdiplus::PointF RotateAvisoPointAround(double x, double y, const Gdiplus::PointF& center, double degrees)
	{
		const Gdiplus::PointF rotated = RotateAvisoVector(
			x - static_cast<double>(center.X),
			y - static_cast<double>(center.Y),
			degrees);
		return Gdiplus::PointF(
			static_cast<Gdiplus::REAL>(static_cast<double>(center.X) + rotated.X),
			static_cast<Gdiplus::REAL>(static_cast<double>(center.Y) + rotated.Y));
	}

	double ResolveAvisoViewportScreenRotationDeg(CSMRRadar* radarScreen, double latitude, double longitude)
	{
		if (radarScreen == nullptr)
			return 0.0;

		CPosition centerPosition;
		centerPosition.m_Latitude = ClampAvisoLatitude(latitude);
		centerPosition.m_Longitude = longitude;
		const POINT centerPixel = radarScreen->ConvertCoordFromPositionToPixel(centerPosition);

		double bestRotationDeg = 0.0;
		double bestDistanceSquared = 0.0;
		for (double sampleDelta : { 0.02, 0.05, 0.1, 0.25, 0.5 })
		{
			CPosition northPosition = centerPosition;
			northPosition.m_Latitude = ClampAvisoLatitude(latitude + sampleDelta);
			if (std::abs(northPosition.m_Latitude - centerPosition.m_Latitude) < 1e-9)
				continue;

			const POINT northPixel = radarScreen->ConvertCoordFromPositionToPixel(northPosition);
			const double dx = static_cast<double>(northPixel.x - centerPixel.x);
			const double dy = static_cast<double>(northPixel.y - centerPixel.y);
			const double distanceSquared = (dx * dx) + (dy * dy);
			if (distanceSquared < 4.0)
				continue;

			const double rotationDeg = std::atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
			if (!std::isfinite(rotationDeg))
				continue;

			if (distanceSquared > bestDistanceSquared)
			{
				bestDistanceSquared = distanceSquared;
				bestRotationDeg = rotationDeg;
			}

			if (distanceSquared >= 2500.0)
				return bestRotationDeg;
		}

		return bestRotationDeg;
	}

	constexpr int kInsetToolbarButtonSize = 13;
	constexpr int kInsetToolbarButtonGap = 2;
	constexpr int kInsetToolbarRightMargin = 3;

	int InsetToolbarRightOffset(int buttonIndexFromRight)
	{
		return kInsetToolbarRightMargin +
			buttonIndexFromRight * (kInsetToolbarButtonSize + kInsetToolbarButtonGap);
	}

	CRect DrawInsetButton(CDC& dc, const char* label, CRect rect, POINT mouseLocation)
	{
		rect.NormalizeRect();
		CBrush buttonBrush(mouseWithin(mouseLocation, rect) ? RGB(53, 71, 75) : RGB(41, 57, 59));
		dc.FillRect(rect, &buttonBrush);

		const COLORREF oldTextColor = dc.SetTextColor(RGB(208, 217, 220));
		const int oldBkMode = dc.SetBkMode(TRANSPARENT);
		dc.DrawTextA(label, -1, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		dc.SetBkMode(oldBkMode);
		dc.SetTextColor(oldTextColor);

		dc.Draw3dRect(rect, RGB(82, 96, 101), RGB(5, 7, 8));

		return rect;
	}

	CRect DrawInsetToolbarButton(CDC& dc, const char* label, const CRect& topBar, int rightOffset, POINT mouseLocation)
	{
		CRect rect(
			topBar.right - rightOffset - kInsetToolbarButtonSize,
			topBar.top + 1,
			topBar.right - rightOffset,
			topBar.bottom - 1);
		return DrawInsetButton(dc, label, rect, mouseLocation);
	}

	void DrawStripedInsetTitleBar(CDC& dc, CRect rect)
	{
		rect.NormalizeRect();
		dc.FillSolidRect(rect, RGB(16, 20, 22));
		const int savedDc = ::SaveDC(dc.GetSafeHdc());
		if (savedDc != 0)
		{
			::IntersectClipRect(dc.GetSafeHdc(), rect.left, rect.top, rect.right, rect.bottom);
			::SelectObject(dc.GetSafeHdc(), ::GetStockObject(DC_PEN));
			::SetDCPenColor(dc.GetSafeHdc(), RGB(46, 57, 60));
			for (int x = rect.left - rect.Height(); x < rect.right; x += 5)
			{
				::MoveToEx(dc.GetSafeHdc(), x, rect.bottom, nullptr);
				::LineTo(dc.GetSafeHdc(), x + rect.Height(), rect.top);
			}
			::RestoreDC(dc.GetSafeHdc(), savedDc);
		}
		dc.Draw3dRect(rect, RGB(5, 7, 8), RGB(5, 7, 8));
	}

	void DrawInsetTitle(CDC& dc, const CRect& topBar, const std::string& title)
	{
		const CSize titleSize = dc.GetTextExtent(title.c_str());
		const int titleX = topBar.left + max(0, (topBar.Width() - titleSize.cx) / 2);
		const int titleY = topBar.top + max(0, (topBar.Height() - titleSize.cy) / 2);
		CRect titlePad(
			titleX - 3,
			topBar.top + 1,
			titleX + titleSize.cx + 3,
			topBar.bottom - 1);
		dc.FillSolidRect(titlePad, RGB(16, 20, 22));
		const COLORREF oldTextColor = dc.SetTextColor(RGB(208, 217, 220));
		const int oldBkMode = dc.SetBkMode(TRANSPARENT);
		dc.TextOutA(titleX, titleY, title.c_str());
		dc.SetBkMode(oldBkMode);
		dc.SetTextColor(oldTextColor);
	}

	CRect NormalizedAvisoLayoutBounds(const RECT* layoutBounds)
	{
		if (layoutBounds == nullptr)
			return CRect(0, 0, 0, 0);

		CRect bounds(*layoutBounds);
		bounds.NormalizeRect();
		if (bounds.Width() <= kAvisoMinLayoutWidth || bounds.Height() <= kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight)
			return CRect(0, 0, 0, 0);

		return bounds;
	}

	int ClampAvisoDividerX(int x, const CRect& bounds)
	{
		const int minX = static_cast<int>(bounds.left) + kAvisoMinLayoutWidth;
		const int maxX = static_cast<int>(bounds.right) - kAvisoMinLayoutWidth;
		if (maxX < minX)
			return (bounds.left + bounds.right) / 2;
		return std::clamp(x, minX, maxX);
	}

	int ClampAvisoDividerY(int y, const CRect& bounds)
	{
		const int minimumFrameHeight = kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight;
		const int minY = static_cast<int>(bounds.top) + minimumFrameHeight;
		const int maxY = static_cast<int>(bounds.bottom) - minimumFrameHeight;
		if (maxY < minY)
			return (bounds.top + bounds.bottom) / 2;
		return std::clamp(y, minY, maxY);
	}

	bool IsAvisoVerticalSplit(AvisoLayoutMode mode)
	{
		return mode == AvisoLayoutMode::SplitLeft ||
			mode == AvisoLayoutMode::SplitRight;
	}

	bool IsAvisoHorizontalSplit(AvisoLayoutMode mode)
	{
		return mode == AvisoLayoutMode::SplitTop ||
			mode == AvisoLayoutMode::SplitBottom;
	}

	bool IsAvisoSplitLayout(AvisoLayoutMode mode)
	{
		return IsAvisoVerticalSplit(mode) || IsAvisoHorizontalSplit(mode);
	}

	bool IsAvisoCornerLayout(AvisoLayoutMode mode)
	{
		return mode == AvisoLayoutMode::CornerTopLeft ||
			mode == AvisoLayoutMode::CornerTopRight ||
			mode == AvisoLayoutMode::CornerBottomLeft ||
			mode == AvisoLayoutMode::CornerBottomRight;
	}

	bool IsAvisoSnappedLayout(AvisoLayoutMode mode)
	{
		return IsAvisoSplitLayout(mode) || IsAvisoCornerLayout(mode);
	}

	bool IsAvisoCornerRightAnchored(AvisoLayoutMode mode)
	{
		return mode == AvisoLayoutMode::CornerTopRight ||
			mode == AvisoLayoutMode::CornerBottomRight;
	}

	bool IsAvisoCornerBottomAnchored(AvisoLayoutMode mode)
	{
		return mode == AvisoLayoutMode::CornerBottomLeft ||
			mode == AvisoLayoutMode::CornerBottomRight;
	}

	CRect DefaultAvisoSplitRect(AvisoLayoutMode mode, const CRect& bounds)
	{
		const int midX = (bounds.left + bounds.right) / 2;
		const int midY = (bounds.top + bounds.bottom) / 2;

		switch (mode)
		{
		case AvisoLayoutMode::SplitLeft:
			return CRect(bounds.left, bounds.top, midX, bounds.bottom);
		case AvisoLayoutMode::SplitRight:
			return CRect(midX, bounds.top, bounds.right, bounds.bottom);
		case AvisoLayoutMode::SplitTop:
			return CRect(bounds.left, bounds.top, bounds.right, midY);
		case AvisoLayoutMode::SplitBottom:
			return CRect(bounds.left, midY, bounds.right, bounds.bottom);
		default:
			return CRect(0, 0, 0, 0);
		}
	}

	CRect AvisoCornerRectForFrameSize(AvisoLayoutMode mode, const CRect& bounds, CSize requestedSize)
	{
		if (!IsAvisoCornerLayout(mode) || bounds.IsRectEmpty())
			return CRect(0, 0, 0, 0);

		const int minimumFrameHeight = kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight;
		const int boundsWidth = static_cast<int>(bounds.Width());
		const int boundsHeight = static_cast<int>(bounds.Height());
		const int minimumWidth = min(kAvisoMinLayoutWidth, boundsWidth);
		const int minimumHeight = min(minimumFrameHeight, boundsHeight);
		const int width = std::clamp(static_cast<int>(requestedSize.cx), minimumWidth, boundsWidth);
		const int height = std::clamp(static_cast<int>(requestedSize.cy), minimumHeight, boundsHeight);
		const int left = IsAvisoCornerRightAnchored(mode) ? bounds.right - width : bounds.left;
		const int top = IsAvisoCornerBottomAnchored(mode) ? bounds.bottom - height : bounds.top;
		return CRect(left, top, left + width, top + height);
	}

	bool ResolveAvisoSnapTarget(
		POINT point,
		const CRect& bounds,
		CSize cornerFrameSize,
		AvisoLayoutMode& mode,
		CRect& area)
	{
		if (bounds.IsRectEmpty())
			return false;

		const bool nearLeft = point.x <= bounds.left + kAvisoSnapThresholdPx;
		const bool nearRight = point.x >= bounds.right - kAvisoSnapThresholdPx;
		const bool nearTop = point.y <= bounds.top + kAvisoSnapThresholdPx;
		const bool nearBottom = point.y >= bounds.bottom - kAvisoSnapThresholdPx;
		const bool nearCornerLeft = point.x <= bounds.left + kAvisoCornerSnapThresholdPx;
		const bool nearCornerRight = point.x >= bounds.right - kAvisoCornerSnapThresholdPx;
		const bool nearCornerTop = point.y <= bounds.top + kAvisoCornerSnapThresholdPx;
		const bool nearCornerBottom = point.y >= bounds.bottom - kAvisoCornerSnapThresholdPx;

		mode = AvisoLayoutMode::Floating;
		if (nearCornerLeft && nearCornerTop)
			mode = AvisoLayoutMode::CornerTopLeft;
		else if (nearCornerRight && nearCornerTop)
			mode = AvisoLayoutMode::CornerTopRight;
		else if (nearCornerLeft && nearCornerBottom)
			mode = AvisoLayoutMode::CornerBottomLeft;
		else if (nearCornerRight && nearCornerBottom)
			mode = AvisoLayoutMode::CornerBottomRight;
		else if (nearLeft)
			mode = AvisoLayoutMode::SplitLeft;
		else if (nearRight)
			mode = AvisoLayoutMode::SplitRight;
		else if (nearTop)
			mode = AvisoLayoutMode::SplitTop;
		else if (nearBottom)
			mode = AvisoLayoutMode::SplitBottom;
		else
			return false;

		area = IsAvisoCornerLayout(mode)
			? AvisoCornerRectForFrameSize(mode, bounds, cornerFrameSize)
			: DefaultAvisoSplitRect(mode, bounds);
		return !area.IsRectEmpty();
	}

	CRect InsetFrameRect(AvisoLayoutMode mode, const RECT& areaValue)
	{
		CRect frame(areaValue);
		frame.NormalizeRect();
		if (mode == AvisoLayoutMode::Floating)
			frame.top -= kAvisoViewportTopBarHeight;
		return frame;
	}

	CRect InsetContentRect(AvisoLayoutMode mode, const RECT& areaValue)
	{
		CRect content(areaValue);
		content.NormalizeRect();
		if (mode != AvisoLayoutMode::Floating)
			content.top = min(content.bottom, content.top + kAvisoViewportTopBarHeight);
		return content;
	}

	CRect InsetTitleBarRect(AvisoLayoutMode mode, const RECT& areaValue)
	{
		CRect area(areaValue);
		area.NormalizeRect();
		if (mode == AvisoLayoutMode::Floating)
			return CRect(area.left, area.top - kAvisoViewportTopBarHeight, area.right, area.top);
		return CRect(area.left, area.top, area.right, min(area.bottom, area.top + kAvisoViewportTopBarHeight));
	}

	CRect InsetCloseButtonRect(AvisoLayoutMode mode, const RECT& areaValue)
	{
		const CRect titleBar = InsetTitleBarRect(mode, areaValue);
		return CRect(
			titleBar.right - InsetToolbarRightOffset(0) - kInsetToolbarButtonSize,
			titleBar.top + 1,
			titleBar.right - InsetToolbarRightOffset(0),
			titleBar.bottom - 1);
	}

	CRect InsetFilterButtonRect(AvisoLayoutMode mode, const RECT& areaValue)
	{
		const CRect titleBar = InsetTitleBarRect(mode, areaValue);
		return CRect(
			titleBar.right - InsetToolbarRightOffset(1) - kInsetToolbarButtonSize,
			titleBar.top + 1,
			titleBar.right - InsetToolbarRightOffset(1),
			titleBar.bottom - 1);
	}

	CRect InsetTitleBarMoveRect(
		AvisoLayoutMode mode,
		const RECT& areaValue,
		bool showFilter,
		bool allowResize = true)
	{
		CRect moveRect = InsetTitleBarRect(mode, areaValue);
		moveRect.NormalizeRect();
		if (allowResize)
		{
			moveRect.left = min(moveRect.right, moveRect.left + kInsetResizeCornerPx);
			moveRect.right = max(moveRect.left, moveRect.right - kInsetResizeCornerPx);
			moveRect.top = min(moveRect.bottom, moveRect.top + kInsetResizeInsidePx + 1);
		}

		CRect closeButton = InsetCloseButtonRect(mode, areaValue);
		closeButton.NormalizeRect();
		moveRect.right = min(moveRect.right, closeButton.left);
		if (showFilter && mode == AvisoLayoutMode::Floating)
		{
			CRect filterButton = InsetFilterButtonRect(mode, areaValue);
			filterButton.NormalizeRect();
			moveRect.right = min(moveRect.right, filterButton.left);
		}
		if (moveRect.right <= moveRect.left || moveRect.bottom <= moveRect.top)
			return CRect(0, 0, 0, 0);
		return moveRect;
	}

	const char* InsetResizeObjectId(ResizeRegion region)
	{
		switch (region)
		{
		case ResizeRegion::Left: return "resize_left";
		case ResizeRegion::Right: return "resize_right";
		case ResizeRegion::Top: return "resize_top";
		case ResizeRegion::Bottom: return "resize_bottom";
		case ResizeRegion::TopLeft: return "resize_tl";
		case ResizeRegion::TopRight: return "resize_tr";
		case ResizeRegion::BottomLeft: return "resize_bl";
		case ResizeRegion::BottomRight: return "resize_br";
		default: return "";
		}
	}

	bool TryParseInsetResizeObjectId(const char* objectId, ResizeRegion& region)
	{
		region = ResizeRegion::None;
		if (objectId == nullptr)
			return false;
		for (ResizeRegion candidate : {
			ResizeRegion::Left,
			ResizeRegion::Right,
			ResizeRegion::Top,
			ResizeRegion::Bottom,
			ResizeRegion::TopLeft,
			ResizeRegion::TopRight,
			ResizeRegion::BottomLeft,
			ResizeRegion::BottomRight })
		{
			if (strcmp(objectId, InsetResizeObjectId(candidate)) == 0)
			{
				region = candidate;
				return true;
			}
		}
		return false;
	}

	CRect InsetResizeObjectRect(AvisoLayoutMode mode, const RECT& areaValue, ResizeRegion region)
	{
		CRect frame = InsetFrameRect(mode, areaValue);
		frame.NormalizeRect();
		const int outside = kInsetResizeHitPx;
		const int inside = kInsetResizeInsidePx;
		const int corner = kInsetResizeCornerPx;
		switch (region)
		{
		case ResizeRegion::Left:
			return CRect(frame.left - outside, frame.top + corner, frame.left + inside + 1, frame.bottom - corner);
		case ResizeRegion::Right:
			return CRect(frame.right - inside, frame.top + corner, frame.right + outside + 1, frame.bottom - corner);
		case ResizeRegion::Top:
			return CRect(frame.left + corner, frame.top - outside, frame.right - corner, frame.top + inside + 1);
		case ResizeRegion::Bottom:
			return CRect(frame.left + corner, frame.bottom - inside, frame.right - corner, frame.bottom + outside + 1);
		case ResizeRegion::TopLeft:
			return CRect(frame.left - outside, frame.top - outside, frame.left + corner, frame.top + corner);
		case ResizeRegion::TopRight:
			// The close button owns the inside of the top-right title-bar corner.
			// Keep diagonal resize available immediately outside the top edge.
			return CRect(frame.right - corner, frame.top - outside, frame.right + outside + 1, frame.top + 1);
		case ResizeRegion::BottomLeft:
			return CRect(frame.left - outside, frame.bottom - corner, frame.left + corner, frame.bottom + outside + 1);
		case ResizeRegion::BottomRight:
			return CRect(frame.right - corner, frame.bottom - corner, frame.right + outside + 1, frame.bottom + outside + 1);
		default:
			return CRect(0, 0, 0, 0);
		}
	}

	void RegisterInsetResizeObjects(
		CSMRRadar* radarScreen,
		int objectType,
		AvisoLayoutMode mode,
		const RECT& areaValue)
	{
		if (radarScreen == nullptr)
			return;
		for (ResizeRegion region : {
			ResizeRegion::Left,
			ResizeRegion::Right,
			ResizeRegion::Top,
			ResizeRegion::Bottom,
			ResizeRegion::TopLeft,
			ResizeRegion::TopRight,
			ResizeRegion::BottomLeft,
			ResizeRegion::BottomRight })
		{
			CRect hitRect = InsetResizeObjectRect(mode, areaValue, region);
			hitRect.NormalizeRect();
			if (!hitRect.IsRectEmpty())
				radarScreen->AddScreenObject(objectType, InsetResizeObjectId(region), hitRect, true, "");
		}
	}

	bool ResizeRegionHasHorizontalEdge(CInsetWindow::ResizeRegion region)
	{
		return region == CInsetWindow::ResizeRegion::Left ||
			region == CInsetWindow::ResizeRegion::Right ||
			region == CInsetWindow::ResizeRegion::TopLeft ||
			region == CInsetWindow::ResizeRegion::TopRight ||
			region == CInsetWindow::ResizeRegion::BottomLeft ||
			region == CInsetWindow::ResizeRegion::BottomRight;
	}

	bool ResizeRegionHasVerticalEdge(CInsetWindow::ResizeRegion region)
	{
		return region == CInsetWindow::ResizeRegion::Top ||
			region == CInsetWindow::ResizeRegion::Bottom ||
			region == CInsetWindow::ResizeRegion::TopLeft ||
			region == CInsetWindow::ResizeRegion::TopRight ||
			region == CInsetWindow::ResizeRegion::BottomLeft ||
			region == CInsetWindow::ResizeRegion::BottomRight;
	}

	bool IsSnappedResizeRegionSupported(AvisoLayoutMode mode, CInsetWindow::ResizeRegion region)
	{
		switch (mode)
		{
		case AvisoLayoutMode::SplitLeft:
			return region == CInsetWindow::ResizeRegion::Right;
		case AvisoLayoutMode::SplitRight:
			return region == CInsetWindow::ResizeRegion::Left;
		case AvisoLayoutMode::SplitTop:
			return region == CInsetWindow::ResizeRegion::Bottom;
		case AvisoLayoutMode::SplitBottom:
			return region == CInsetWindow::ResizeRegion::Top;
		case AvisoLayoutMode::CornerTopLeft:
			return region == CInsetWindow::ResizeRegion::Right ||
				region == CInsetWindow::ResizeRegion::Bottom ||
				region == CInsetWindow::ResizeRegion::BottomRight;
		case AvisoLayoutMode::CornerTopRight:
			return region == CInsetWindow::ResizeRegion::Left ||
				region == CInsetWindow::ResizeRegion::Bottom ||
				region == CInsetWindow::ResizeRegion::BottomLeft;
		case AvisoLayoutMode::CornerBottomLeft:
			return region == CInsetWindow::ResizeRegion::Right ||
				region == CInsetWindow::ResizeRegion::Top ||
				region == CInsetWindow::ResizeRegion::TopRight;
		case AvisoLayoutMode::CornerBottomRight:
			return region == CInsetWindow::ResizeRegion::Left ||
				region == CInsetWindow::ResizeRegion::Top ||
				region == CInsetWindow::ResizeRegion::TopLeft;
		default:
			return false;
		}
	}

	bool ResizeAvisoSplitRectToPoint(AvisoLayoutMode mode, POINT Pt, const CRect& bounds, RECT& area)
	{
		switch (mode)
		{
		case AvisoLayoutMode::SplitLeft:
		{
			const int dividerX = ClampAvisoDividerX(Pt.x, bounds);
			area = { bounds.left, bounds.top, dividerX, bounds.bottom };
			return true;
		}
		case AvisoLayoutMode::SplitRight:
		{
			const int dividerX = ClampAvisoDividerX(Pt.x, bounds);
			area = { dividerX, bounds.top, bounds.right, bounds.bottom };
			return true;
		}
		case AvisoLayoutMode::SplitTop:
		{
			const int dividerY = ClampAvisoDividerY(Pt.y, bounds);
			area = { bounds.left, bounds.top, bounds.right, dividerY };
			return true;
		}
		case AvisoLayoutMode::SplitBottom:
		{
			const int dividerY = ClampAvisoDividerY(Pt.y, bounds);
			area = { bounds.left, dividerY, bounds.right, bounds.bottom };
			return true;
		}
		default:
			return false;
		}
	}

	bool ResizeAvisoCornerRectToPoint(AvisoLayoutMode mode, POINT Pt, const CRect& bounds, RECT& area, bool resizeX, bool resizeY)
	{
		if (!IsAvisoCornerLayout(mode))
			return false;

		CRect current(area);
		current.NormalizeRect();
		const int minimumFrameHeight = kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight;
		if (current.Width() < kAvisoMinLayoutWidth || current.Height() < minimumFrameHeight)
		{
			current = AvisoCornerRectForFrameSize(
				mode,
				bounds,
				CSize(
					max(kAvisoMinLayoutWidth, static_cast<int>(current.Width())),
					max(minimumFrameHeight, static_cast<int>(current.Height()))));
		}

		const bool rightAnchored = IsAvisoCornerRightAnchored(mode);
		const bool bottomAnchored = IsAvisoCornerBottomAnchored(mode);

		int left = rightAnchored ? current.left : bounds.left;
		int right = rightAnchored ? bounds.right : current.right;
		int top = bottomAnchored ? current.top : bounds.top;
		int bottom = bottomAnchored ? bounds.bottom : current.bottom;

		if (resizeX)
		{
			if (rightAnchored)
				left = std::clamp(Pt.x, bounds.left, bounds.right - kAvisoMinLayoutWidth);
			else
				right = std::clamp(Pt.x, bounds.left + kAvisoMinLayoutWidth, bounds.right);
		}

		if (resizeY)
		{
			if (bottomAnchored)
				top = std::clamp(Pt.y, bounds.top, bounds.bottom - minimumFrameHeight);
			else
				bottom = std::clamp(Pt.y, bounds.top + minimumFrameHeight, bounds.bottom);
		}

		area = { left, top, right, bottom };
		return true;
	}

	void DrawInsetWindowChrome(
		CDC& dc,
		CSMRRadar* radarScreen,
		int objectType,
		AvisoLayoutMode mode,
		const RECT& areaValue,
		const std::string& title,
		bool showFilter,
		POINT mouseLocation,
		bool allowResize = true,
		double* elapsedMilliseconds = nullptr)
	{
		const auto chromeStarted = std::chrono::steady_clock::now();
		struct AccumulateChromeTime
		{
			double* destination = nullptr;
			std::chrono::steady_clock::time_point started;
			~AccumulateChromeTime() noexcept
			{
				if (destination != nullptr)
				{
					*destination += std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - started).count();
				}
			}
		} accumulateChromeTime{ elapsedMilliseconds, chromeStarted };
		if (radarScreen == nullptr)
			return;

		CRect area(areaValue);
		area.NormalizeRect();
		if (area.IsRectEmpty())
			return;

		CRect titleBar = InsetTitleBarRect(mode, areaValue);
		titleBar.NormalizeRect();
		DrawStripedInsetTitleBar(dc, titleBar);
		CRect titleBarMoveRect = InsetTitleBarMoveRect(mode, areaValue, showFilter, allowResize);
		if (!titleBarMoveRect.IsRectEmpty())
			radarScreen->AddScreenObject(objectType, "topbar", titleBarMoveRect, true, "");
		DrawInsetTitle(dc, titleBar, title);
		if (allowResize)
			RegisterInsetResizeObjects(radarScreen, objectType, mode, areaValue);

		if (showFilter && mode == AvisoLayoutMode::Floating)
		{
			const CRect filterRect = DrawInsetToolbarButton(dc, "F", titleBar, InsetToolbarRightOffset(1), mouseLocation);
			radarScreen->AddScreenObject(objectType, "filter", filterRect, false, "");
		}
		const CRect closeRect = DrawInsetToolbarButton(
			dc,
			"X",
			titleBar,
			InsetToolbarRightOffset(0),
			mouseLocation);
		radarScreen->AddScreenObject(objectType, "close", closeRect, false, "");
	}

	double AvisoFinitePositive(double value, double fallback, double minValue, double maxValue)
	{
		if (!std::isfinite(value))
			return fallback;
		return std::clamp(value, minValue, maxValue);
	}

	bool AvisoRectIntersects(const CRect& one, const CRect& two)
	{
		CRect a(one);
		CRect b(two);
		a.NormalizeRect();
		b.NormalizeRect();
		return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
	}
}

struct AvisoViewportState
{
	~AvisoViewportState()
	{
		StopRenderThread();
		ClearCache();
	}

	void ClearCache()
	{
		if (cacheBitmap != nullptr)
		{
			::DeleteObject(cacheBitmap);
			cacheBitmap = nullptr;
		}

		cachePath.clear();
		cacheGroupGeneration = 0;
		cacheWidth = 0;
		cacheHeight = 0;
		displayMinLongitude = 0.0;
		displayMinLatitude = 0.0;
		displayMaxLongitude = 0.0;
		displayMaxLatitude = 0.0;
		renderMinLongitude = 0.0;
		renderMinLatitude = 0.0;
		renderMaxLongitude = 0.0;
		renderMaxLatitude = 0.0;
		projectedTopLeft = Gdiplus::PointF();
		projectedTopRight = Gdiplus::PointF();
		projectedBottomLeft = Gdiplus::PointF();
		projectedBottomRight = Gdiplus::PointF();
		anchorValid = false;
	}

	void InvalidateRenderRequests()
	{
		std::lock_guard<std::mutex> guard(renderMutex);
		pendingRenderRequest.reset();
		completedRenderResult.reset();
		pendingRenderRadar = nullptr;
		latestRequestId = ++nextRequestId;
		cancellationToken->store(latestRequestId, std::memory_order_release);
		lastRequestValid = false;
		renderPending.store(false, std::memory_order_relaxed);
	}

	std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> TakeCompletedRender()
	{
		std::lock_guard<std::mutex> guard(renderMutex);
		std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> result = std::move(completedRenderResult);
		renderPending.store(renderInFlight || pendingRenderRequest != nullptr || completedRenderResult != nullptr, std::memory_order_relaxed);
		return result;
	}

	void AllowRetryForDiscardedResult(std::uint64_t requestId)
	{
		std::lock_guard<std::mutex> guard(renderMutex);
		// Do not invalidate a newer request that was queued after this completed
		// raster. If this is still the latest request, however, coalescing it would
		// otherwise prevent the current viewport from ever trying the view again.
		if (latestRequestId == requestId &&
			pendingRenderRequest == nullptr &&
			!renderInFlight)
		{
			lastRequestValid = false;
		}
	}

	void QueueRender(CSMRRadar* radarScreen, CSMRRadar::AvisoRasterRenderRequest request)
	{
		if (radarScreen == nullptr ||
			request.path.empty() ||
			request.features == nullptr ||
			request.labels == nullptr ||
			request.rasterWidth <= 0 ||
			request.rasterHeight <= 0)
		{
			return;
		}

		if (radarScreen->IsShutdownRequested() || radarScreen->IsAvisoGeoJsonRenderStopRequested())
			return;

		bool shouldNotify = false;
		{
			std::lock_guard<std::mutex> guard(renderMutex);
			if (renderStopRequested)
				return;

			const double longitudeTolerance = max(
				std::abs(request.displayMaxLongitude - request.displayMinLongitude) /
					static_cast<double>((std::max)(request.rasterWidth, 1)) * 0.5,
				1e-10);
			const double latitudeTolerance = max(
				std::abs(request.displayMaxLatitude - request.displayMinLatitude) /
					static_cast<double>((std::max)(request.rasterHeight, 1)) * 0.5,
				1e-10);
			const bool sameRequest =
				lastRequestValid &&
				lastRequestPath == request.path &&
				lastRequestGroupGeneration == request.groupGeneration &&
				std::abs(lastRequestRasterWidth - request.rasterWidth) <= 2 &&
				std::abs(lastRequestRasterHeight - request.rasterHeight) <= 2 &&
				AvisoWithinTolerance(lastRequestMinLongitude, request.displayMinLongitude, longitudeTolerance) &&
				AvisoWithinTolerance(lastRequestMinLatitude, request.displayMinLatitude, latitudeTolerance) &&
				AvisoWithinTolerance(lastRequestMaxLongitude, request.displayMaxLongitude, longitudeTolerance) &&
				AvisoWithinTolerance(lastRequestMaxLatitude, request.displayMaxLatitude, latitudeTolerance) &&
				AvisoPointWithinTolerance(lastRequestProjectedTopLeft, request.projectedTopLeft, 0.75) &&
				AvisoPointWithinTolerance(lastRequestProjectedTopRight, request.projectedTopRight, 0.75) &&
				AvisoPointWithinTolerance(lastRequestProjectedBottomLeft, request.projectedBottomLeft, 0.75) &&
				AvisoPointWithinTolerance(lastRequestProjectedBottomRight, request.projectedBottomRight, 0.75);
			if (sameRequest)
			{
				radarScreen->PerformanceDiagnostics.RecordAvisoRequestCoalesced(
					VsmrPerformance::AvisoViewport::Inset);
				return;
			}

			request.requestId = ++nextRequestId;
			request.performanceQueuedAtMilliseconds =
				VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
			if (request.debounceMilliseconds == 0 && cacheBitmap != nullptr)
				request.debounceMilliseconds = 24;
			request.cancellationToken = cancellationToken;
			latestRequestId = request.requestId;
			cancellationToken->store(request.requestId, std::memory_order_release);
			lastRequestValid = true;
			lastRequestPath = request.path;
			lastRequestMinLongitude = request.displayMinLongitude;
			lastRequestMinLatitude = request.displayMinLatitude;
			lastRequestMaxLongitude = request.displayMaxLongitude;
			lastRequestMaxLatitude = request.displayMaxLatitude;
			lastRequestRasterWidth = request.rasterWidth;
			lastRequestRasterHeight = request.rasterHeight;
			lastRequestGroupGeneration = request.groupGeneration;
			lastRequestProjectedTopLeft = request.projectedTopLeft;
			lastRequestProjectedTopRight = request.projectedTopRight;
			lastRequestProjectedBottomLeft = request.projectedBottomLeft;
			lastRequestProjectedBottomRight = request.projectedBottomRight;
			const bool supersededPendingRequest =
				pendingRenderRequest != nullptr || renderInFlight;
			pendingRenderRadar = radarScreen;
			pendingRenderRequest = std::make_unique<CSMRRadar::AvisoRasterRenderRequest>(std::move(request));
			renderPending.store(true, std::memory_order_relaxed);
			radarScreen->PerformanceDiagnostics.RecordAvisoRequestQueued(
				VsmrPerformance::AvisoViewport::Inset,
				supersededPendingRequest);

			if (!renderThreadStarted)
			{
				renderStopRequested = false;
				try
				{
					renderThread = std::thread(&AvisoViewportState::RenderThreadMain, this);
					renderThreadStarted = true;
				}
				catch (const std::exception& ex)
				{
					pendingRenderRequest.reset();
					pendingRenderRadar = nullptr;
					renderPending.store(false, std::memory_order_relaxed);
					lastRequestValid = false;
					Logger::info("Inset AVISO render worker start failed: " + std::string(ex.what()));
					return;
				}
				catch (...)
				{
					pendingRenderRequest.reset();
					pendingRenderRadar = nullptr;
					renderPending.store(false, std::memory_order_relaxed);
					lastRequestValid = false;
					Logger::info("Inset AVISO render worker start failed: unknown exception");
					return;
				}
			}
			shouldNotify = true;
		}

		if (shouldNotify)
			renderCondition.notify_one();
	}

	void StopRenderThread()
	{
		bool shouldJoin = false;
		{
			std::lock_guard<std::mutex> guard(renderMutex);
			renderStopRequested = true;
			cancellationToken->fetch_add(1, std::memory_order_release);
			pendingRenderRequest.reset();
			completedRenderResult.reset();
			pendingRenderRadar = nullptr;
			renderInFlight = false;
			renderPending.store(false, std::memory_order_relaxed);
			shouldJoin = renderThreadStarted;
		}

		renderCondition.notify_all();
		if (shouldJoin && renderThread.joinable())
			renderThread.join();

		{
			std::lock_guard<std::mutex> guard(renderMutex);
			renderThreadStarted = false;
			renderStopRequested = false;
			lastRequestValid = false;
		}
	}

	void RenderThreadMain()
	{
		VsmrCrashRuntime::OwnedThreadRole crashThreadRole("inset AVISO render worker");
		for (;;)
		{
			std::unique_ptr<CSMRRadar::AvisoRasterRenderRequest> request;
			CSMRRadar* radarScreen = nullptr;
			{
				std::unique_lock<std::mutex> lock(renderMutex);
				renderCondition.wait(lock, [&]() {
					return renderStopRequested || pendingRenderRequest != nullptr;
				});

				if (renderStopRequested)
					return;
				while (pendingRenderRequest != nullptr &&
					pendingRenderRequest->debounceMilliseconds > 0)
				{
					const std::uint64_t observedRequestId = pendingRenderRequest->requestId;
					const std::uint64_t readyAt =
						pendingRenderRequest->performanceQueuedAtMilliseconds +
						pendingRenderRequest->debounceMilliseconds;
					const std::uint64_t now =
						VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
					if (now >= readyAt)
						break;
					renderCondition.wait_for(
						lock,
						std::chrono::milliseconds(
							static_cast<long long>(readyAt - now)),
						[&]() {
							return renderStopRequested ||
								pendingRenderRequest == nullptr ||
								pendingRenderRequest->requestId != observedRequestId;
						});
					if (renderStopRequested)
						return;
					if (pendingRenderRequest != nullptr &&
						pendingRenderRequest->requestId != observedRequestId)
					{
						if (pendingRenderRadar != nullptr)
						{
							pendingRenderRadar->PerformanceDiagnostics.RecordAvisoRequestDebounced(
								VsmrPerformance::AvisoViewport::Inset);
						}
						continue;
					}
					break;
				}

				request = std::move(pendingRenderRequest);
				radarScreen = pendingRenderRadar;
				pendingRenderRadar = nullptr;
				renderInFlight = request != nullptr;
				renderPending.store(renderInFlight || pendingRenderRequest != nullptr || completedRenderResult != nullptr, std::memory_order_relaxed);
			}

			if (request == nullptr || radarScreen == nullptr)
			{
				std::lock_guard<std::mutex> guard(renderMutex);
				renderInFlight = false;
				renderPending.store(pendingRenderRequest != nullptr || completedRenderResult != nullptr, std::memory_order_relaxed);
				continue;
			}
			VsmrCrashRuntime::RecordCurrentThreadCallback(
				"AvisoViewportState::RenderThreadMain",
				reinterpret_cast<std::uintptr_t>(radarScreen));

			std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> result;
			const std::uint64_t renderStartMilliseconds =
				VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
			const double queueWaitMilliseconds = request->performanceQueuedAtMilliseconds == 0
				? 0.0
				: static_cast<double>(renderStartMilliseconds - request->performanceQueuedAtMilliseconds);
			const auto renderStart = std::chrono::steady_clock::now();
			try
			{
				result = radarScreen->RenderAvisoGeoJsonRaster(*request);
			}
			catch (CException* ex)
			{
				if (ex != nullptr)
					ex->Delete();
				Logger::info("Inset AVISO render worker caught MFC exception");
			}
			catch (...)
			{
				result.reset();
			}
			const double rebuildMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - renderStart).count();
			const bool renderCancelled = result == nullptr &&
				radarScreen->IsAvisoRasterRenderRequestCancelled(*request);
			if (renderCancelled)
			{
				radarScreen->PerformanceDiagnostics.RecordAvisoRasterBuildCancelled(
					VsmrPerformance::AvisoViewport::Inset);
			}
			else
			{
				radarScreen->PerformanceDiagnostics.RecordAvisoRasterBuild(
					VsmrPerformance::AvisoViewport::Inset,
					rebuildMilliseconds,
					queueWaitMilliseconds,
					result != nullptr);
			}

			bool shouldRefresh = false;
			bool discardedResult = false;
			{
				std::lock_guard<std::mutex> guard(renderMutex);
				if (result == nullptr &&
					!renderCancelled &&
					request->requestId == latestRequestId)
				{
					// Let an identical frame retry after a transient render failure.
					lastRequestValid = false;
				}
				if (renderStopRequested)
					return;

				if (result != nullptr &&
					result->requestId == latestRequestId &&
					!radarScreen->IsShutdownRequested() &&
					!radarScreen->IsAvisoGeoJsonRenderStopRequested())
				{
					if (completedRenderResult != nullptr)
						discardedResult = true;
					completedRenderResult = std::move(result);
					shouldRefresh = true;
				}
				else if (result != nullptr)
				{
					discardedResult = true;
				}

				renderInFlight = false;
				renderPending.store(pendingRenderRequest != nullptr || completedRenderResult != nullptr, std::memory_order_relaxed);
			}
			if (discardedResult)
			{
				radarScreen->PerformanceDiagnostics.RecordAvisoResultDiscarded(
					VsmrPerformance::AvisoViewport::Inset);
			}

			if (shouldRefresh)
				radarScreen->RequestRefreshFromWorker();
		}
	}

	HBITMAP cacheBitmap = nullptr;
	string cachePath;
	unsigned long long cacheGroupGeneration = 0;
	int cacheWidth = 0;
	int cacheHeight = 0;
	double displayMinLongitude = 0.0;
	double displayMinLatitude = 0.0;
	double displayMaxLongitude = 0.0;
	double displayMaxLatitude = 0.0;
	double renderMinLongitude = 0.0;
	double renderMinLatitude = 0.0;
	double renderMaxLongitude = 0.0;
	double renderMaxLatitude = 0.0;
	Gdiplus::PointF projectedTopLeft;
	Gdiplus::PointF projectedTopRight;
	Gdiplus::PointF projectedBottomLeft;
	Gdiplus::PointF projectedBottomRight;
	bool anchorValid = false;
	double screenRotationDeg = 0.0;
	std::atomic<bool> renderPending{ false };
	unsigned long long nextRequestId = 0;
	unsigned long long latestRequestId = 0;
	std::shared_ptr<std::atomic<std::uint64_t>> cancellationToken =
		std::make_shared<std::atomic<std::uint64_t>>(0);
	std::mutex renderMutex;
	std::condition_variable renderCondition;
	std::thread renderThread;
	bool renderThreadStarted = false;
	bool renderStopRequested = false;
	bool renderInFlight = false;
	CSMRRadar* pendingRenderRadar = nullptr;
	std::unique_ptr<CSMRRadar::AvisoRasterRenderRequest> pendingRenderRequest;
	std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> completedRenderResult;
	bool lastRequestValid = false;
	string lastRequestPath;
	double lastRequestMinLongitude = 0.0;
	double lastRequestMinLatitude = 0.0;
	double lastRequestMaxLongitude = 0.0;
	double lastRequestMaxLatitude = 0.0;
	int lastRequestRasterWidth = 0;
	int lastRequestRasterHeight = 0;
	unsigned long long lastRequestGroupGeneration = 0;
	Gdiplus::PointF lastRequestProjectedTopLeft;
	Gdiplus::PointF lastRequestProjectedTopRight;
	Gdiplus::PointF lastRequestProjectedBottomLeft;
	Gdiplus::PointF lastRequestProjectedBottomRight;

	VsmrPerformance::AvisoQueueDepth PerformanceQueueDepth()
	{
		std::lock_guard<std::mutex> guard(renderMutex);
		VsmrPerformance::AvisoQueueDepth result;
		result.pending = pendingRenderRequest != nullptr ? 1U : 0U;
		result.inFlight = renderInFlight ? 1U : 0U;
		result.completed = completedRenderResult != nullptr ? 1U : 0U;
		result.workers = renderThreadStarted ? 1U : 0U;
		return result;
	}
};

CInsetWindow::CInsetWindow(int Id)
{
	m_Id = Id;
	m_AvisoState = std::make_unique<AvisoViewportState>();
}

CInsetWindow::~CInsetWindow()
{
	CancelAvisoViewportRender();
	ReleaseCachedFonts();
}

HFONT CInsetWindow::GetWeatherFont(
	size_t index,
	int height,
	int weight,
	DWORD pitchAndFamily,
	const char* faceName)
{
	if (index >= m_WeatherFonts.size())
		return nullptr;

	if (m_WeatherFonts[index] != nullptr && m_WeatherFontHeights[index] == height)
		return m_WeatherFonts[index];

	if (m_WeatherFonts[index] != nullptr)
	{
		::DeleteObject(m_WeatherFonts[index]);
		m_WeatherFonts[index] = nullptr;
	}

	HFONT font = ::CreateFontA(
		height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, pitchAndFamily, faceName);
	if (font != nullptr)
	{
		m_WeatherFonts[index] = font;
		m_WeatherFontHeights[index] = height;
	}
	return font;
}

HFONT CInsetWindow::GetTimerFont()
{
	if (m_TimerFont == nullptr)
	{
		m_TimerFont = ::CreateFontA(
			-10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	}
	return m_TimerFont;
}

void CInsetWindow::ReleaseCachedFonts()
{
	for (HFONT& font : m_WeatherFonts)
	{
		if (font != nullptr)
		{
			::DeleteObject(font);
			font = nullptr;
		}
	}
	m_WeatherFontHeights.fill(0);

	if (m_TimerFont != nullptr)
	{
		::DeleteObject(m_TimerFont);
		m_TimerFont = nullptr;
	}

	m_SrwFontSource = nullptr;
	m_SrwFontSize = 0.0f;
	m_SrwFontStyle = 0;
	m_SrwFontFamily.clear();
	m_SrwBoldFont.reset();
	m_SrwBlankWidth = 0;
	m_SrwLineHeight = 0;
}

bool CInsetWindow::IsAvisoViewport() const
{
	return m_Mode == Mode::AvisoViewport;
}

bool CInsetWindow::IsSecondaryRadar() const
{
	return m_Mode == Mode::SecondaryRadar;
}

bool CInsetWindow::IsWeather() const
{
	return m_Mode == Mode::Weather;
}

bool CInsetWindow::IsTimer() const
{
	return m_Mode == Mode::Timer;
}

bool CInsetWindow::SupportsPanAndZoom() const
{
	return IsAvisoViewport() || IsSecondaryRadar();
}

bool CInsetWindow::IsSnappedLayout() const
{
	return IsAvisoSnappedLayout(m_AvisoLayoutMode);
}

void CInsetWindow::ClearAvisoViewportCache()
{
	if (m_AvisoState != nullptr)
	{
		m_AvisoState->InvalidateRenderRequests();
		m_AvisoState->ClearCache();
	}
}

void CInsetWindow::InvalidateAvisoViewportRendering()
{
	if (m_AvisoState != nullptr)
		m_AvisoState->InvalidateRenderRequests();
}

VsmrPerformance::AvisoQueueDepth CInsetWindow::GetAvisoPerformanceQueueDepth()
{
	return m_AvisoState != nullptr
		? m_AvisoState->PerformanceQueueDepth()
		: VsmrPerformance::AvisoQueueDepth{};
}

std::size_t CInsetWindow::GetAvisoPerformanceBitmapCount(
	std::uint64_t* estimatedBytes) const
{
	if (estimatedBytes != nullptr)
		*estimatedBytes = 0;
	if (m_AvisoState == nullptr || m_AvisoState->cacheBitmap == nullptr)
		return 0;
	if (estimatedBytes != nullptr &&
		m_AvisoState->cacheWidth > 0 && m_AvisoState->cacheHeight > 0)
	{
		*estimatedBytes = static_cast<std::uint64_t>(m_AvisoState->cacheWidth) *
			static_cast<std::uint64_t>(m_AvisoState->cacheHeight) * 4ULL;
	}
	return 1;
}

double CInsetWindow::GetLastRdfRenderMilliseconds() const noexcept
{
	return m_LastRdfRenderMilliseconds;
}

double CInsetWindow::GetLastChromeRenderMilliseconds() const noexcept
{
	return m_LastChromeRenderMilliseconds;
}

void CInsetWindow::CancelAvisoViewportRender()
{
	if (m_AvisoState == nullptr)
		return;

	m_AvisoState->StopRenderThread();
}

void CInsetWindow::ResetAvisoInteractionState()
{
	m_AvisoRightPanning = false;
	m_AvisoScrollSelected = false;
	m_AvisoScreenArea = { 0, 0, 0, 0 };
	m_AvisoScreenAreaValid = false;
	m_AvisoRenderWindow = nullptr;
	CancelWindowInteraction();
}

bool CInsetWindow::IsPointInside(POINT Pt) const
{
	CRect areaRect = GetWindowContentRect();
	areaRect.NormalizeRect();
	return
		Pt.x >= areaRect.left &&
		Pt.x <= areaRect.right &&
		Pt.y >= areaRect.top &&
			Pt.y <= areaRect.bottom;
}

CRect CInsetWindow::GetWindowFrameRect() const
{
	if (IsTimer())
		return InsetFrameRect(AvisoLayoutMode::Floating, m_Area);
	return InsetFrameRect(m_AvisoLayoutMode, m_Area);
}

CRect CInsetWindow::GetWindowContentRect() const
{
	if (IsTimer())
		return CRect(m_Area);
	return InsetContentRect(m_AvisoLayoutMode, m_Area);
}

CInsetWindow::ResizeRegion CInsetWindow::HitTestResize(POINT Pt) const
{
	if (IsTimer())
		return ResizeRegion::None;
	if (GetWindowFrameRect().IsRectEmpty())
		return ResizeRegion::None;

	CRect closeButton = InsetCloseButtonRect(m_AvisoLayoutMode, m_Area);
	closeButton.NormalizeRect();
	if (closeButton.PtInRect(Pt))
		return ResizeRegion::None;
	if (IsSecondaryRadar() && m_AvisoLayoutMode == AvisoLayoutMode::Floating)
	{
		CRect filterButton = InsetFilterButtonRect(m_AvisoLayoutMode, m_Area);
		filterButton.NormalizeRect();
		if (filterButton.PtInRect(Pt))
			return ResizeRegion::None;
	}

	// These are the same rectangles registered as EuroScope moveable objects.
	// Keep corners first so the diagonal cursor wins at the four junctions.
	for (ResizeRegion region : {
		ResizeRegion::TopLeft,
		ResizeRegion::TopRight,
		ResizeRegion::BottomLeft,
		ResizeRegion::BottomRight,
		ResizeRegion::Left,
		ResizeRegion::Right,
		ResizeRegion::Top,
		ResizeRegion::Bottom })
	{
		CRect hitRect = InsetResizeObjectRect(m_AvisoLayoutMode, m_Area, region);
		hitRect.NormalizeRect();
		if (hitRect.PtInRect(Pt))
			return region;
	}
	return ResizeRegion::None;
}

bool CInsetWindow::HitTestTitleBar(POINT Pt) const
{
	const AvisoLayoutMode chromeMode = IsTimer() ? AvisoLayoutMode::Floating : m_AvisoLayoutMode;
	CRect moveRect = InsetTitleBarMoveRect(chromeMode, m_Area, IsSecondaryRadar(), !IsTimer());
	moveRect.NormalizeRect();
	return moveRect.PtInRect(Pt) != FALSE;
}

bool CInsetWindow::BeginWindowMove(POINT Pt, const RECT* layoutBounds, bool requireTitleBarHit)
{
	if ((requireTitleBarHit && !HitTestTitleBar(Pt)) || m_WindowResizeActive)
		return false;

	ApplyAvisoLayoutBounds(layoutBounds);
	m_WindowMoveActive = true;
	m_WindowMoveStartedSnapped = IsSnappedLayout() && !IsTimer();
	m_WindowMoveDetached = !m_WindowMoveStartedSnapped;
	m_WindowInteractionMoved = false;
	m_WindowInteractionStartPoint = Pt;
	m_WindowInteractionStartArea = m_Area;
	m_SnapPreviewValid = false;
	m_SnapPreviewMode = AvisoLayoutMode::Floating;
	m_SnapPreviewArea = { 0, 0, 0, 0 };
	m_Grip = true;
	return true;
}

bool CInsetWindow::UpdateWindowMove(POINT Pt, const RECT* layoutBounds)
{
	if (!m_WindowMoveActive)
		return false;

	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
		return false;
	const int initialDeltaX = Pt.x - m_WindowInteractionStartPoint.x;
	const int initialDeltaY = Pt.y - m_WindowInteractionStartPoint.y;
	if (!m_WindowInteractionMoved)
	{
		if (std::abs(initialDeltaX) < kInsetDragThresholdPx &&
			std::abs(initialDeltaY) < kInsetDragThresholdPx)
		{
			return true;
		}
		m_WindowInteractionMoved = true;
	}

	if (m_WindowMoveStartedSnapped && !m_WindowMoveDetached)
	{
		const CRect snappedFrame = GetWindowFrameRect();
		const CRect snappedTitleBar = InsetTitleBarRect(m_AvisoLayoutMode, m_Area);
		const double horizontalGrabRatio = std::clamp(
			static_cast<double>(m_WindowInteractionStartPoint.x - snappedFrame.left) /
				static_cast<double>(max(1, snappedFrame.Width())),
			0.08,
			0.92);
		const int titleGrabOffsetY = std::clamp(
			static_cast<int>(m_WindowInteractionStartPoint.y - snappedTitleBar.top),
			1,
			max(1, static_cast<int>(snappedTitleBar.Height()) - 1));
		FloatAvisoViewport(Pt, layoutBounds);
		CRect detachedContent(m_Area);
		detachedContent.NormalizeRect();
		const int detachedWidth = detachedContent.Width();
		const int detachedHeight = detachedContent.Height();
		const int detachedLeft = Pt.x - static_cast<int>(std::lround(
			static_cast<double>(detachedWidth) * horizontalGrabRatio));
		const int detachedTop = Pt.y - titleGrabOffsetY + kAvisoViewportTopBarHeight;
		m_Area = {
			detachedLeft,
			detachedTop,
			detachedLeft + detachedWidth,
			detachedTop + detachedHeight
		};
		ApplyAvisoLayoutBounds(layoutBounds);
		m_WindowMoveDetached = true;
		m_WindowInteractionStartPoint = Pt;
		m_WindowInteractionStartArea = m_Area;
		m_Grip = true;
	}

	CRect startArea(m_WindowInteractionStartArea);
	startArea.NormalizeRect();
	const int width = startArea.Width();
	const int height = startArea.Height();
	if (width <= 0 || height <= 0)
		return false;

	const int deltaX = Pt.x - m_WindowInteractionStartPoint.x;
	const int deltaY = Pt.y - m_WindowInteractionStartPoint.y;
	m_AvisoLayoutMode = AvisoLayoutMode::Floating;
	m_Area = {
		startArea.left + deltaX,
		startArea.top + deltaY,
		startArea.left + deltaX + width,
		startArea.top + deltaY + height
	};
	ApplyAvisoLayoutBounds(layoutBounds);
	if (IsTimer())
	{
		CRect frame = GetWindowFrameRect();
		frame.NormalizeRect();
		bool snapHorizontal = false;
		bool snapVertical = false;
		bool snapLeft = false;
		bool snapTop = false;
		int snappedLeft = frame.left;
		int snappedTop = frame.top;
		// The compact Timer may be grabbed far from the edge of its frame.
		// Snap from frame proximity so a visually flush window always receives
		// an anchor and the matching preview, regardless of the grab point.
		const bool nearLeft = frame.left <= bounds.left + kAvisoSnapThresholdPx;
		const bool nearRight = frame.right >= bounds.right - kAvisoSnapThresholdPx;
		const bool nearTop = frame.top <= bounds.top + kAvisoSnapThresholdPx;
		const bool nearBottom = frame.bottom >= bounds.bottom - kAvisoSnapThresholdPx;
		const bool nearCornerLeft = frame.left <= bounds.left + kAvisoCornerSnapThresholdPx;
		const bool nearCornerRight = frame.right >= bounds.right - kAvisoCornerSnapThresholdPx;
		const bool nearCornerTop = frame.top <= bounds.top + kAvisoCornerSnapThresholdPx;
		const bool nearCornerBottom = frame.bottom >= bounds.bottom - kAvisoCornerSnapThresholdPx;

		if ((nearCornerLeft || nearCornerRight) && (nearCornerTop || nearCornerBottom))
		{
			snapHorizontal = true;
			snapVertical = true;
			snapLeft = nearCornerLeft;
			snapTop = nearCornerTop;
			snappedLeft = snapLeft ? bounds.left : bounds.right - frame.Width();
			snappedTop = snapTop ? bounds.top : bounds.bottom - frame.Height();
		}
		else
		{
			if (nearLeft || nearRight)
			{
				snapHorizontal = true;
				snapLeft = nearLeft;
				snappedLeft = snapLeft ? bounds.left : bounds.right - frame.Width();
			}
			if (nearTop || nearBottom)
			{
				snapVertical = true;
				snapTop = nearTop;
				snappedTop = snapTop ? bounds.top : bounds.bottom - frame.Height();
			}
		}

		m_SnapPreviewValid = snapHorizontal || snapVertical;
		if (snapHorizontal && snapVertical)
		{
			m_SnapPreviewMode = snapLeft
				? (snapTop ? AvisoLayoutMode::CornerTopLeft : AvisoLayoutMode::CornerBottomLeft)
				: (snapTop ? AvisoLayoutMode::CornerTopRight : AvisoLayoutMode::CornerBottomRight);
		}
		else if (snapHorizontal)
		{
			m_SnapPreviewMode = snapLeft ? AvisoLayoutMode::SplitLeft : AvisoLayoutMode::SplitRight;
		}
		else if (snapVertical)
		{
			m_SnapPreviewMode = snapTop ? AvisoLayoutMode::SplitTop : AvisoLayoutMode::SplitBottom;
		}
		else
		{
			m_SnapPreviewMode = AvisoLayoutMode::Floating;
		}
		if (m_SnapPreviewValid)
		{
			const int minimumLeft = static_cast<int>(bounds.left);
			const int minimumTop = static_cast<int>(bounds.top);
			const int maximumLeft = max(minimumLeft, static_cast<int>(bounds.right) - frame.Width());
			const int maximumTop = max(minimumTop, static_cast<int>(bounds.bottom) - frame.Height());
			snappedLeft = std::clamp(snappedLeft, minimumLeft, maximumLeft);
			snappedTop = std::clamp(snappedTop, minimumTop, maximumTop);
			m_SnapPreviewArea = {
				snappedLeft,
				snappedTop + kAvisoViewportTopBarHeight,
				snappedLeft + frame.Width(),
				snappedTop + frame.Height()
			};
		}
		else
		{
			m_SnapPreviewArea = { 0, 0, 0, 0 };
		}
		return true;
	}

	CRect previewArea;
	AvisoLayoutMode previewMode = AvisoLayoutMode::Floating;
	m_SnapPreviewValid = ResolveAvisoSnapTarget(
		Pt,
		bounds,
		GetWindowFrameRect().Size(),
		previewMode,
		previewArea);
	if (m_WindowInteractionMoved && m_SnapPreviewValid)
	{
		m_SnapPreviewMode = previewMode;
		m_SnapPreviewArea = previewArea;
	}
	else
	{
		m_SnapPreviewMode = AvisoLayoutMode::Floating;
		m_SnapPreviewArea = { 0, 0, 0, 0 };
	}
	return true;
}

bool CInsetWindow::EndWindowMove(POINT Pt, const RECT* layoutBounds)
{
	if (!m_WindowMoveActive)
		return false;

	UpdateWindowMove(Pt, layoutBounds);
	if (m_WindowInteractionMoved && m_SnapPreviewValid)
	{
		m_AvisoLayoutMode = m_SnapPreviewMode;
		m_Area = m_SnapPreviewArea;
		ApplyAvisoLayoutBounds(layoutBounds);
	}

	m_WindowMoveActive = false;
	m_WindowMoveStartedSnapped = false;
	m_WindowMoveDetached = false;
	m_WindowInteractionMoved = false;
	m_SnapPreviewValid = false;
	m_SnapPreviewMode = AvisoLayoutMode::Floating;
	m_SnapPreviewArea = { 0, 0, 0, 0 };
	m_Grip = false;
	return true;
}

bool CInsetWindow::BeginWindowResize(ResizeRegion region, POINT Pt, const RECT* layoutBounds)
{
	if (IsTimer() || region == ResizeRegion::None || m_WindowMoveActive)
		return false;

	ApplyAvisoLayoutBounds(layoutBounds);
	m_WindowResizeActive = true;
	m_WindowResizeDetached = false;
	m_WindowResizeRegion = region;
	m_WindowInteractionMoved = false;
	m_WindowInteractionStartPoint = Pt;
	m_WindowInteractionStartArea = m_Area;
	m_WindowResizeStartFrame = GetWindowFrameRect();
	m_SnapPreviewValid = false;
	m_Grip = true;
	return true;
}

bool CInsetWindow::UpdateWindowResize(POINT Pt, const RECT* layoutBounds)
{
	if (!m_WindowResizeActive)
		return false;

	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
		return false;
	const int deltaX = Pt.x - m_WindowInteractionStartPoint.x;
	const int deltaY = Pt.y - m_WindowInteractionStartPoint.y;
	if (!m_WindowInteractionMoved)
	{
		if (std::abs(deltaX) < kInsetDragThresholdPx &&
			std::abs(deltaY) < kInsetDragThresholdPx)
		{
			return true;
		}
		m_WindowInteractionMoved = true;
	}

	if (IsSnappedLayout() &&
		!IsSnappedResizeRegionSupported(m_AvisoLayoutMode, m_WindowResizeRegion) &&
		!m_WindowResizeDetached)
	{
		CRect snappedFrame(m_WindowResizeStartFrame);
		snappedFrame.NormalizeRect();
		m_AvisoLayoutMode = AvisoLayoutMode::Floating;
		m_Area = {
			snappedFrame.left,
			snappedFrame.top + kAvisoViewportTopBarHeight,
			snappedFrame.right,
			snappedFrame.bottom
		};
		ApplyAvisoLayoutBounds(layoutBounds);
		m_WindowResizeDetached = true;
	}

	if (IsAvisoSplitLayout(m_AvisoLayoutMode))
	{
		POINT dividerPoint = Pt;
		switch (m_AvisoLayoutMode)
		{
		case AvisoLayoutMode::SplitLeft:
			dividerPoint.x = m_WindowResizeStartFrame.right + deltaX;
			break;
		case AvisoLayoutMode::SplitRight:
			dividerPoint.x = m_WindowResizeStartFrame.left + deltaX;
			break;
		case AvisoLayoutMode::SplitTop:
			dividerPoint.y = m_WindowResizeStartFrame.bottom + deltaY;
			break;
		case AvisoLayoutMode::SplitBottom:
			dividerPoint.y = m_WindowResizeStartFrame.top + deltaY;
			break;
		default:
			break;
		}
		ResizeAvisoSplitRectToPoint(m_AvisoLayoutMode, dividerPoint, bounds, m_Area);
		ApplyAvisoLayoutBounds(layoutBounds);
		return true;
	}
	if (IsAvisoCornerLayout(m_AvisoLayoutMode))
	{
		POINT dividerPoint = Pt;
		dividerPoint.x = IsAvisoCornerRightAnchored(m_AvisoLayoutMode)
			? m_WindowResizeStartFrame.left + deltaX
			: m_WindowResizeStartFrame.right + deltaX;
		dividerPoint.y = IsAvisoCornerBottomAnchored(m_AvisoLayoutMode)
			? m_WindowResizeStartFrame.top + deltaY
			: m_WindowResizeStartFrame.bottom + deltaY;
		ResizeAvisoCornerRectToPoint(
			m_AvisoLayoutMode,
			dividerPoint,
			bounds,
			m_Area,
			ResizeRegionHasHorizontalEdge(m_WindowResizeRegion),
			ResizeRegionHasVerticalEdge(m_WindowResizeRegion));
		ApplyAvisoLayoutBounds(layoutBounds);
		return true;
	}

	CRect frame(m_WindowResizeStartFrame);
	frame.NormalizeRect();
	const bool resizeLeft = m_WindowResizeRegion == ResizeRegion::Left ||
		m_WindowResizeRegion == ResizeRegion::TopLeft ||
		m_WindowResizeRegion == ResizeRegion::BottomLeft;
	const bool resizeRight = m_WindowResizeRegion == ResizeRegion::Right ||
		m_WindowResizeRegion == ResizeRegion::TopRight ||
		m_WindowResizeRegion == ResizeRegion::BottomRight;
	const bool resizeTop = m_WindowResizeRegion == ResizeRegion::Top ||
		m_WindowResizeRegion == ResizeRegion::TopLeft ||
		m_WindowResizeRegion == ResizeRegion::TopRight;
	const bool resizeBottom = m_WindowResizeRegion == ResizeRegion::Bottom ||
		m_WindowResizeRegion == ResizeRegion::BottomLeft ||
		m_WindowResizeRegion == ResizeRegion::BottomRight;

	if (resizeLeft)
		frame.left += deltaX;
	if (resizeRight)
		frame.right += deltaX;
	if (resizeTop)
		frame.top += deltaY;
	if (resizeBottom)
		frame.bottom += deltaY;

	const int minFrameWidth = min(kAvisoMinLayoutWidth, bounds.Width());
	const int minFrameHeight = min(kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight, bounds.Height());
	if (frame.Width() < minFrameWidth)
	{
		if (resizeLeft)
			frame.left = frame.right - minFrameWidth;
		else
			frame.right = frame.left + minFrameWidth;
	}
	if (frame.Height() < minFrameHeight)
	{
		if (resizeTop)
			frame.top = frame.bottom - minFrameHeight;
		else
			frame.bottom = frame.top + minFrameHeight;
	}

	if (resizeLeft)
		frame.left = max(frame.left, bounds.left);
	if (resizeRight)
		frame.right = min(frame.right, bounds.right);
	if (resizeTop)
		frame.top = max(frame.top, bounds.top);
	if (resizeBottom)
		frame.bottom = min(frame.bottom, bounds.bottom);

	m_Area = {
		frame.left,
		frame.top + kAvisoViewportTopBarHeight,
		frame.right,
		frame.bottom
	};
	ApplyAvisoLayoutBounds(layoutBounds);
	return true;
}

bool CInsetWindow::EndWindowResize(POINT Pt, const RECT* layoutBounds)
{
	if (!m_WindowResizeActive)
		return false;
	UpdateWindowResize(Pt, layoutBounds);
	m_WindowResizeActive = false;
	m_WindowResizeDetached = false;
	m_WindowResizeRegion = ResizeRegion::None;
	m_WindowInteractionMoved = false;
	m_Grip = false;
	return true;
}

bool CInsetWindow::IsWindowMoveActive() const
{
	return m_WindowMoveActive;
}

bool CInsetWindow::IsWindowResizeActive() const
{
	return m_WindowResizeActive;
}

CInsetWindow::ResizeRegion CInsetWindow::GetActiveResizeRegion() const
{
	return m_WindowResizeRegion;
}

void CInsetWindow::CancelWindowInteraction()
{
	m_WindowMoveActive = false;
	m_WindowMoveStartedSnapped = false;
	m_WindowMoveDetached = false;
	m_WindowInteractionMoved = false;
	m_WindowResizeActive = false;
	m_WindowResizeDetached = false;
	m_WindowResizeRegion = ResizeRegion::None;
	m_SnapPreviewValid = false;
	m_SnapPreviewMode = AvisoLayoutMode::Floating;
	m_SnapPreviewArea = { 0, 0, 0, 0 };
	m_Grip = false;
}

bool CInsetWindow::GetSnapPreviewRect(CRect& preview) const
{
	preview.SetRectEmpty();
	if (!m_SnapPreviewValid)
		return false;
	preview = CRect(m_SnapPreviewArea);
	if (IsTimer())
		preview = InsetFrameRect(AvisoLayoutMode::Floating, m_SnapPreviewArea);
	preview.NormalizeRect();
	return !preview.IsRectEmpty();
}

void CInsetWindow::RenderSnapPreview(Gdiplus::Graphics& graphics) const
{
	if (!m_SnapPreviewValid)
		return;

	CRect preview;
	if (!GetSnapPreviewRect(preview))
		return;

	const Gdiplus::GraphicsState state = graphics.Save();
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
	Gdiplus::SolidBrush fill(Gdiplus::Color(64, 72, 169, 205));
	Gdiplus::Pen outline(Gdiplus::Color(230, 135, 220, 238), 2.0f);
	graphics.FillRectangle(
		&fill,
		Gdiplus::Rect(preview.left, preview.top, preview.Width(), preview.Height()));
	graphics.DrawRectangle(
		&outline,
		Gdiplus::Rect(preview.left + 1, preview.top + 1, max(1, preview.Width() - 2), max(1, preview.Height() - 2)));
	graphics.Restore(state);
}

void CInsetWindow::ApplyAvisoLayoutBounds(const RECT* layoutBounds)
{
	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
		return;

	const int contentTop = bounds.top + kAvisoViewportTopBarHeight;
	if (contentTop >= bounds.bottom)
		return;
	if (IsTimer())
	{
		CRect area(m_Area);
		area.NormalizeRect();
		const int width = min(kTimerContentWidth, bounds.Width());
		const int height = min(kTimerContentHeight, static_cast<int>(bounds.bottom) - contentTop);
		const int minimumLeft = static_cast<int>(bounds.left);
		const int minimumTop = contentTop;
		const int maximumLeft = max(minimumLeft, static_cast<int>(bounds.right) - width);
		const int maximumTop = max(minimumTop, static_cast<int>(bounds.bottom) - height);
		int left = static_cast<int>(area.left);
		int top = static_cast<int>(area.top);
		switch (m_AvisoLayoutMode)
		{
		case AvisoLayoutMode::SplitLeft:
			left = minimumLeft;
			break;
		case AvisoLayoutMode::SplitRight:
			left = maximumLeft;
			break;
		case AvisoLayoutMode::SplitTop:
			top = minimumTop;
			break;
		case AvisoLayoutMode::SplitBottom:
			top = maximumTop;
			break;
		case AvisoLayoutMode::CornerTopLeft:
			left = minimumLeft;
			top = minimumTop;
			break;
		case AvisoLayoutMode::CornerTopRight:
			left = maximumLeft;
			top = minimumTop;
			break;
		case AvisoLayoutMode::CornerBottomLeft:
			left = minimumLeft;
			top = maximumTop;
			break;
		case AvisoLayoutMode::CornerBottomRight:
			left = maximumLeft;
			top = maximumTop;
			break;
		default:
			break;
		}
		left = std::clamp(left, minimumLeft, maximumLeft);
		top = std::clamp(top, minimumTop, maximumTop);
		m_Area = { left, top, left + width, top + height };
		return;
	}

	CRect area(m_Area);
	area.NormalizeRect();
	if (area.Width() <= 0 || area.Height() <= 0)
		area = CRect(bounds.left, contentTop, bounds.left + 500, contentTop + 300);

	switch (m_AvisoLayoutMode)
	{
	case AvisoLayoutMode::SplitLeft:
	{
		const int dividerX = ClampAvisoDividerX(area.right, bounds);
		m_Area = { bounds.left, bounds.top, dividerX, bounds.bottom };
		break;
	}
	case AvisoLayoutMode::SplitRight:
	{
		const int dividerX = ClampAvisoDividerX(area.left, bounds);
		m_Area = { dividerX, bounds.top, bounds.right, bounds.bottom };
		break;
	}
	case AvisoLayoutMode::SplitTop:
	{
		const int dividerY = ClampAvisoDividerY(area.bottom, bounds);
		m_Area = { bounds.left, bounds.top, bounds.right, dividerY };
		break;
	}
	case AvisoLayoutMode::SplitBottom:
	{
		const int dividerY = ClampAvisoDividerY(area.top, bounds);
		m_Area = { bounds.left, dividerY, bounds.right, bounds.bottom };
		break;
	}
	case AvisoLayoutMode::CornerTopLeft:
	case AvisoLayoutMode::CornerTopRight:
	case AvisoLayoutMode::CornerBottomLeft:
	case AvisoLayoutMode::CornerBottomRight:
	{
		const int minimumFrameHeight = kAvisoMinLayoutHeight + kAvisoViewportTopBarHeight;
		const int boundsWidth = static_cast<int>(bounds.Width());
		const int boundsHeight = static_cast<int>(bounds.Height());
		const int minimumWidth = min(kAvisoMinLayoutWidth, boundsWidth);
		const int minimumHeight = min(minimumFrameHeight, boundsHeight);
		const int width = std::clamp(static_cast<int>(area.Width()), minimumWidth, boundsWidth);
		const int height = std::clamp(static_cast<int>(area.Height()), minimumHeight, boundsHeight);
		m_Area = AvisoCornerRectForFrameSize(m_AvisoLayoutMode, bounds, CSize(width, height));
		break;
	}
	case AvisoLayoutMode::Floating:
	default:
	{
		const int maxWidth = max(kAvisoMinLayoutWidth, bounds.Width());
		const int maxHeight = max(kAvisoMinLayoutHeight, bounds.Height() - kAvisoViewportTopBarHeight);
		int width = std::clamp(area.Width(), kAvisoMinLayoutWidth, maxWidth);
		int height = std::clamp(area.Height(), kAvisoMinLayoutHeight, maxHeight);
		if (width > bounds.Width())
			width = bounds.Width();
		if (height > bounds.bottom - contentTop)
			height = bounds.bottom - contentTop;

		const int leftMax = max(bounds.left, bounds.right - width);
		const int topMax = max(contentTop, bounds.bottom - height);
		const int left = std::clamp(static_cast<int>(area.left), static_cast<int>(bounds.left), leftMax);
		const int top = std::clamp(static_cast<int>(area.top), contentTop, topMax);
		m_Area = { left, top, left + width, top + height };
		break;
	}
	}
}

void CInsetWindow::SnapAvisoLayoutToPoint(POINT Pt, const RECT* layoutBounds)
{
	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
	{
		m_AvisoLayoutMode = AvisoLayoutMode::Floating;
		return;
	}

	AvisoLayoutMode snappedMode = AvisoLayoutMode::Floating;
	CRect snappedArea;
	if (ResolveAvisoSnapTarget(Pt, bounds, GetWindowFrameRect().Size(), snappedMode, snappedArea))
	{
		m_AvisoLayoutMode = snappedMode;
		m_Area = snappedArea;
	}
	else
	{
		m_AvisoLayoutMode = AvisoLayoutMode::Floating;
	}

	ApplyAvisoLayoutBounds(layoutBounds);
}

void CInsetWindow::UpdateAvisoScreenArea(HWND hwnd)
{
	m_AvisoScreenArea = { 0, 0, 0, 0 };
	m_AvisoScreenAreaValid = false;
	m_AvisoRenderWindow = nullptr;
	if (hwnd == nullptr || !::IsWindow(hwnd))
		return;

	CRect areaRect = GetWindowContentRect();
	areaRect.NormalizeRect();
	if (areaRect.Width() <= 0 || areaRect.Height() <= 0)
		return;

	POINT topLeft = areaRect.TopLeft();
	POINT bottomRight = areaRect.BottomRight();
	if (!::ClientToScreen(hwnd, &topLeft) || !::ClientToScreen(hwnd, &bottomRight))
		return;

	CRect screenRect(topLeft, bottomRight);
	screenRect.NormalizeRect();
	if (screenRect.Width() <= 0 || screenRect.Height() <= 0)
		return;

	m_AvisoScreenArea = screenRect;
	m_AvisoScreenAreaValid = true;
	m_AvisoRenderWindow = hwnd;
}

bool CInsetWindow::TryMapAvisoScreenPoint(POINT screenPoint, POINT& avisoPoint) const
{
	if (!m_AvisoScreenAreaValid || m_AvisoRenderWindow == nullptr ||
		!::IsWindow(m_AvisoRenderWindow))
		return false;

	POINT clientPoint = screenPoint;
	if (!::ScreenToClient(m_AvisoRenderWindow, &clientPoint))
		return false;

	CRect frameRect = GetWindowFrameRect();
	frameRect.NormalizeRect();
	if (frameRect.Width() <= 0 || frameRect.Height() <= 0 ||
		clientPoint.x < frameRect.left || clientPoint.x > frameRect.right ||
		clientPoint.y < frameRect.top || clientPoint.y > frameRect.bottom)
	{
		return false;
	}

	avisoPoint = clientPoint;
	return true;
}

void CInsetWindow::BeginAvisoPan(POINT Pt)
{
	if (!SupportsPanAndZoom())
		return;

	m_OffsetDrag = Pt;
	if (IsAvisoViewport())
	{
		m_AvisoDragStartLatitude = m_AvisoCenterLatitude;
		m_AvisoDragStartLongitude = m_AvisoCenterLongitude;
	}
	else
	{
		m_OffsetInit = m_Offset;
	}
	m_AvisoRightPanning = true;
	m_AvisoScrollSelected = true;
	m_Grip = false;
}

bool CInsetWindow::UpdateAvisoPan(POINT Pt)
{
	if (!SupportsPanAndZoom() || !m_AvisoRightPanning)
		return false;

	if (!IsAvisoViewport())
	{
		CRect area = GetWindowContentRect();
		area.NormalizeRect();
		const POINT maximumOffset = { area.Width() / 2, area.Height() / 2 };
		m_Offset.x = std::clamp(m_OffsetInit.x + (Pt.x - m_OffsetDrag.x), -maximumOffset.x, maximumOffset.x);
		m_Offset.y = std::clamp(m_OffsetInit.y + (Pt.y - m_OffsetDrag.y), -maximumOffset.y, maximumOffset.y);
		return true;
	}

	const int scale = max(1, m_AvisoScale);
	const double metersPerPixel = kAvisoMetersPerNm / static_cast<double>(scale);
	const double lonDegreesPerPixel = metersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoDragStartLatitude));
	const double latDegreesPerPixel = metersPerPixel / kAvisoLatMetersPerDegree;
	const Gdiplus::PointF drag = RotateAvisoVector(
		static_cast<double>(Pt.x - m_OffsetDrag.x),
		static_cast<double>(Pt.y - m_OffsetDrag.y),
		-(m_AvisoState != nullptr ? m_AvisoState->screenRotationDeg : 0.0));
	m_AvisoCenterLongitude = m_AvisoDragStartLongitude - (static_cast<double>(drag.X) * lonDegreesPerPixel);
	m_AvisoCenterLatitude = ClampAvisoLatitude(m_AvisoDragStartLatitude + (static_cast<double>(drag.Y) * latDegreesPerPixel));
	return true;
}

void CInsetWindow::EndAvisoPan()
{
	m_AvisoRightPanning = false;
}

void CInsetWindow::FloatAvisoViewport(POINT Pt, const RECT* layoutBounds)
{
	ApplyAvisoLayoutBounds(layoutBounds);
	CRect currentArea = GetWindowContentRect();
	currentArea.NormalizeRect();
	if (currentArea.Width() <= 0 || currentArea.Height() <= 0)
		return;

	const bool preserveCornerSize = IsAvisoCornerLayout(m_AvisoLayoutMode);
	const int detachedWidth = preserveCornerSize
		? currentArea.Width()
		: min(
			max(kAvisoMinLayoutWidth, currentArea.Width() - 40),
			std::clamp(currentArea.Width() / 2, kAvisoMinLayoutWidth, 620));
	const int detachedHeight = preserveCornerSize
		? currentArea.Height()
		: min(
			max(kAvisoMinLayoutHeight, currentArea.Height() - 40),
			std::clamp(currentArea.Height() / 2, kAvisoMinLayoutHeight, 380));

	const double xRatio = std::clamp(
		static_cast<double>(Pt.x - currentArea.left) / static_cast<double>(max(1, currentArea.Width())),
		0.15,
		0.85);
	const double yRatio = std::clamp(
		static_cast<double>(Pt.y - currentArea.top) / static_cast<double>(max(1, currentArea.Height())),
		0.15,
		0.85);
	const int left = Pt.x - static_cast<int>(std::lround(static_cast<double>(detachedWidth) * xRatio));
	const int top = Pt.y - static_cast<int>(std::lround(static_cast<double>(detachedHeight) * yRatio));

	m_AvisoLayoutMode = AvisoLayoutMode::Floating;
	m_Area = { left, top, left + detachedWidth, top + detachedHeight };
	m_AvisoRightPanning = false;
	m_AvisoScrollSelected = true;
	m_Grip = false;
	ApplyAvisoLayoutBounds(layoutBounds);
}

bool CInsetWindow::ZoomAvisoAtPoint(POINT Pt, double scaleMultiplier)
{
	if (!SupportsPanAndZoom() || !IsPointInside(Pt) || !std::isfinite(scaleMultiplier) || scaleMultiplier <= 0.0)
		return false;

	CRect viewportRect = GetWindowContentRect();
	viewportRect.NormalizeRect();
	const int viewportWidth = viewportRect.Width();
	const int viewportHeight = viewportRect.Height();
	if (viewportWidth <= 0 || viewportHeight <= 0)
		return false;

	if (!IsAvisoViewport())
	{
		const int minimumScale = 1;
		const int maximumScale = 2400;
		const int oldScale = std::clamp(m_Scale, minimumScale, maximumScale);
		int newScale = std::clamp(
			static_cast<int>(std::lround(static_cast<double>(oldScale) * scaleMultiplier)),
			minimumScale,
			maximumScale);
		if (newScale == oldScale)
			newScale = std::clamp(oldScale + (scaleMultiplier > 1.0 ? 1 : -1), minimumScale, maximumScale);
		if (newScale == oldScale)
			return false;

		const POINT center = viewportRect.CenterPoint();
		const double ratio = static_cast<double>(newScale) / static_cast<double>(oldScale);
		const double oldReferenceX = static_cast<double>(center.x + m_Offset.x);
		const double oldReferenceY = static_cast<double>(center.y + m_Offset.y);
		const double newReferenceX = static_cast<double>(Pt.x) -
			(ratio * (static_cast<double>(Pt.x) - oldReferenceX));
		const double newReferenceY = static_cast<double>(Pt.y) -
			(ratio * (static_cast<double>(Pt.y) - oldReferenceY));
		m_Scale = newScale;
		const int maximumOffsetX = viewportWidth / 2;
		const int maximumOffsetY = viewportHeight / 2;
		m_Offset.x = std::clamp(
			static_cast<int>(std::lround(newReferenceX - static_cast<double>(center.x))),
			-maximumOffsetX,
			maximumOffsetX);
		m_Offset.y = std::clamp(
			static_cast<int>(std::lround(newReferenceY - static_cast<double>(center.y))),
			-maximumOffsetY,
			maximumOffsetY);
		m_AvisoScrollSelected = true;
		return true;
	}

	const POINT centerPoint = viewportRect.CenterPoint();
	const Gdiplus::PointF localOffset = RotateAvisoVector(
		static_cast<double>(Pt.x - centerPoint.x),
		static_cast<double>(Pt.y - centerPoint.y),
		-(m_AvisoState != nullptr ? m_AvisoState->screenRotationDeg : 0.0));
	const double dx = static_cast<double>(localOffset.X);
	const double dy = static_cast<double>(localOffset.Y);

	const int oldScale = max(1, m_AvisoScale);
	const double oldMetersPerPixel = kAvisoMetersPerNm / static_cast<double>(oldScale);
	const double oldLonDegreesPerPixel = oldMetersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoCenterLatitude));
	const double oldLatDegreesPerPixel = oldMetersPerPixel / kAvisoLatMetersPerDegree;
	const double anchorLongitude = m_AvisoCenterLongitude + (dx * oldLonDegreesPerPixel);
	const double anchorLatitude = m_AvisoCenterLatitude - (dy * oldLatDegreesPerPixel);

	const int newScale = std::clamp(static_cast<int>(std::lround(static_cast<double>(oldScale) * scaleMultiplier)), 20, 2400);
	if (newScale == oldScale)
		return false;

	m_AvisoScale = newScale;
	const double newMetersPerPixel = kAvisoMetersPerNm / static_cast<double>(newScale);
	const double newLonDegreesPerPixel = newMetersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(anchorLatitude));
	const double newLatDegreesPerPixel = newMetersPerPixel / kAvisoLatMetersPerDegree;
	m_AvisoCenterLongitude = anchorLongitude - (dx * newLonDegreesPerPixel);
	m_AvisoCenterLatitude = ClampAvisoLatitude(anchorLatitude + (dy * newLatDegreesPerPixel));
	m_AvisoScrollSelected = true;
	return true;
}

void CInsetWindow::setAirport(string airportIcao)
{
	icao = airportIcao;
}

void CInsetWindow::OnClickScreenObject(const char * sItemString, POINT Pt, int Button)
{
	UNREFERENCED_PARAMETER(Pt);
	if (!IsTimer() || sItemString == nullptr)
		return;

	int durationMinutes = 0;
	if (strcmp(sItemString, "timer.1m") == 0)
		durationMinutes = 1;
	else if (strcmp(sItemString, "timer.2m") == 0)
		durationMinutes = 2;
	else if (strcmp(sItemString, "timer.3m") == 0)
		durationMinutes = 3;
	else if (strcmp(sItemString, "timer.4m") == 0)
		durationMinutes = 4;
	if (durationMinutes == 0)
		return;

	if (Button == BUTTON_LEFT)
		StartTimer(durationMinutes);
	else if (Button == BUTTON_RIGHT)
		ResetTimer(durationMinutes);
}

void CInsetWindow::StartTimer(int durationMinutes)
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	if (m_TimerDeadlineTicks[index] != 0)
		return;
	m_TimerDeadlineTicks[index] = ::GetTickCount64() +
		(static_cast<unsigned long long>(durationMinutes) * 60ULL * 1000ULL);
	m_TimerExpired[index] = false;
}

void CInsetWindow::ResetTimer(int durationMinutes)
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	m_TimerDeadlineTicks[index] = 0;
	m_TimerExpired[index] = false;
}

bool CInsetWindow::UpdateTimerCountdowns()
{
	if (!IsTimer())
		return false;

	const unsigned long long now = ::GetTickCount64();
	bool alarmDue = false;
	for (size_t index = 0; index < m_TimerDeadlineTicks.size(); ++index)
	{
		const unsigned long long deadline = m_TimerDeadlineTicks[index];
		if (deadline == 0 || now < deadline)
			continue;

		m_TimerDeadlineTicks[index] = 0;
		m_TimerExpired[index] = true;
		alarmDue = true;
	}
	return alarmDue;
}

int CInsetWindow::GetTimerRemainingSeconds(int durationMinutes, unsigned long long now) const
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return 0;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	const unsigned long long deadline = m_TimerDeadlineTicks[index];
	if (deadline == 0)
		return 0;
	if (now >= deadline)
		return 0;
	return static_cast<int>((deadline - now + 999ULL) / 1000ULL);
}

bool CInsetWindow::OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool Released, const RECT* layoutBounds)
{
	if (sObjectId == nullptr)
		return true;

	const auto originalPointerFromMovedObject = [&](const CRect& originalObject) -> POINT
	{
		CRect movedObject(Area);
		movedObject.NormalizeRect();
		return {
			Pt.x - (movedObject.left - originalObject.left),
			Pt.y - (movedObject.top - originalObject.top)
		};
	};

	if (strcmp(sObjectId, "topbar") == 0)
	{
		if (!m_WindowMoveActive)
		{
			const AvisoLayoutMode chromeMode = IsTimer() ? AvisoLayoutMode::Floating : m_AvisoLayoutMode;
			CRect originalTitleBar = InsetTitleBarMoveRect(
				chromeMode,
				m_Area,
				IsSecondaryRadar(),
				!IsTimer());
			originalTitleBar.NormalizeRect();
			const POINT startPoint = originalPointerFromMovedObject(originalTitleBar);
			if (!BeginWindowMove(startPoint, layoutBounds, false))
				return true;
		}

		if (Released)
			EndWindowMove(Pt, layoutBounds);
		else
			UpdateWindowMove(Pt, layoutBounds);
		return Released;
	}

	ResizeRegion resizeRegion = ResizeRegion::None;
	if (TryParseInsetResizeObjectId(sObjectId, resizeRegion))
	{
		if (!m_WindowResizeActive)
		{
			CRect originalResizeObject = InsetResizeObjectRect(m_AvisoLayoutMode, m_Area, resizeRegion);
			originalResizeObject.NormalizeRect();
			const POINT startPoint = originalPointerFromMovedObject(originalResizeObject);
			if (!BeginWindowResize(resizeRegion, startPoint, layoutBounds))
				return true;
		}

		if (Released)
			EndWindowResize(Pt, layoutBounds);
		else
			UpdateWindowResize(Pt, layoutBounds);
		return Released;
	}

	if (strcmp(sObjectId, "divider") == 0 || strcmp(sObjectId, "divider_x") == 0 || strcmp(sObjectId, "divider_y") == 0)
	{
		CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
		if (bounds.IsRectEmpty())
			return true;

		if (strcmp(sObjectId, "divider") == 0)
		{
			ResizeAvisoSplitRectToPoint(m_AvisoLayoutMode, Pt, bounds, m_Area);
		}
		else if (strcmp(sObjectId, "divider_x") == 0)
		{
			ResizeAvisoCornerRectToPoint(m_AvisoLayoutMode, Pt, bounds, m_Area, true, false);
		}
		else
		{
			ResizeAvisoCornerRectToPoint(m_AvisoLayoutMode, Pt, bounds, m_Area, false, true);
		}
		ApplyAvisoLayoutBounds(layoutBounds);

		return Released;
	}

	if (strcmp(sObjectId, "window") == 0) {
		if (IsAvisoViewport())
		{
			if (m_AvisoRightPanning)
			{
				UpdateAvisoPan(Pt);
				if (Released)
					EndAvisoPan();

				return true;
			}

			if (Released && !m_Grip)
				return true;

			if (!m_Grip)
			{
				ApplyAvisoLayoutBounds(layoutBounds);
				CRect currentArea(m_Area);
				currentArea.NormalizeRect();
				if (currentArea.Width() <= 0 || currentArea.Height() <= 0)
					return true;

				if (IsAvisoSplitLayout(m_AvisoLayoutMode))
				{
					FloatAvisoViewport(Pt, layoutBounds);
					currentArea = CRect(m_Area);
					currentArea.NormalizeRect();
				}
				else if (m_AvisoLayoutMode != AvisoLayoutMode::Floating)
				{
					m_AvisoLayoutMode = AvisoLayoutMode::Floating;
					ApplyAvisoLayoutBounds(layoutBounds);
					currentArea = CRect(m_Area);
					currentArea.NormalizeRect();
				}

				m_OffsetDrag = { Pt.x - currentArea.left, Pt.y - currentArea.top };
				m_Grip = true;
			}

			CRect appWindowRect(m_Area);
			appWindowRect.NormalizeRect();
			const int width = appWindowRect.Width();
			const int height = appWindowRect.Height();
			if (width <= 0 || height <= 0)
				return true;

			const int left = Pt.x - m_OffsetDrag.x;
			const int top = Pt.y - m_OffsetDrag.y;
			m_Area = { left, top, left + width, top + height };
			ApplyAvisoLayoutBounds(layoutBounds);

			if (Released)
				m_Grip = false;

			return Released;
		}

		if (!IsSecondaryRadar())
			return true;

		if (!this->m_Grip)
		{
			m_OffsetInit = m_Offset;
			m_OffsetDrag = Pt;
			m_Grip = true;
		}

		POINT maxoffset = {
			(m_Area.right - m_Area.left) / 2,
			(m_Area.bottom - (m_Area.top + 15)) / 2
		};
		m_Offset.x = max(-maxoffset.x, min(maxoffset.x, m_OffsetInit.x + (Pt.x - m_OffsetDrag.x)));
		m_Offset.y = max(-maxoffset.y, min(maxoffset.y, m_OffsetInit.y + (Pt.y - m_OffsetDrag.y)));

		if (Released)
		{
			m_Grip = false;
		}
	}
	if (strcmp(sObjectId, "resize") == 0) {
		ApplyAvisoLayoutBounds(layoutBounds);
		CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
		if (!bounds.IsRectEmpty() && IsAvisoCornerLayout(m_AvisoLayoutMode))
		{
			ResizeAvisoCornerRectToPoint(m_AvisoLayoutMode, Pt, bounds, m_Area, true, true);
			ApplyAvisoLayoutBounds(layoutBounds);
			return Released;
		}

		m_AvisoLayoutMode = AvisoLayoutMode::Floating;

		POINT TopLeft = { m_Area.left, m_Area.top };
		POINT BottomRight = { Area.right, Area.bottom };

		CRect newSize(TopLeft, BottomRight);
		newSize.NormalizeRect();

		if (newSize.Height() < 100) {
			newSize.top = m_Area.top;
			newSize.bottom = m_Area.bottom;
		}

		if (newSize.Width() < 300) {
			newSize.left = m_Area.left;
			newSize.right = m_Area.right;
		}

		m_Area = newSize;
		ApplyAvisoLayoutBounds(layoutBounds);

		return Released;
	}
	if (strcmp(sObjectId, "topbar") == 0) {
		ApplyAvisoLayoutBounds(layoutBounds);
		m_AvisoLayoutMode = AvisoLayoutMode::Floating;

		CRect appWindowRect(m_Area);
		appWindowRect.NormalizeRect();

		POINT TopLeft = { Area.left, Area.bottom + 1 };
		POINT BottomRight = { TopLeft.x + appWindowRect.Width(), TopLeft.y + appWindowRect.Height() };
		CRect newPos(TopLeft, BottomRight);
		newPos.NormalizeRect();

		m_Area = newPos;
		if (Released)
			SnapAvisoLayoutToPoint(Pt, layoutBounds);
		else
			ApplyAvisoLayoutBounds(layoutBounds);

		return Released;
	}

	if (strcmp(sObjectId, "window") != 0 &&
		strcmp(sObjectId, "resize") != 0 &&
		strcmp(sObjectId, "topbar") != 0 &&
		strcmp(sObjectId, "divider") != 0 &&
		strcmp(sObjectId, "divider_x") != 0 &&
		strcmp(sObjectId, "divider_y") != 0)
	{
		string callsign = sObjectId;
		if (!callsign.empty())
		{
			POINT tagCenter{};
			const bool firstDragFrame = (!Released && m_TagBeingDragged != callsign);
			if (firstDragFrame)
			{
				POINT rectCenter{};
				auto fullRectIt = m_TagAreas.find(callsign);
				if (fullRectIt != m_TagAreas.end())
				{
					CRect fullRect = fullRectIt->second;
					fullRect.NormalizeRect();
					rectCenter = fullRect.CenterPoint();
				}
				else
				{
					CRect tagRect(Area);
					tagRect.NormalizeRect();
					rectCenter = tagRect.CenterPoint();
				}
				POINT offset = { rectCenter.x - Pt.x, rectCenter.y - Pt.y };
				m_TagDragOffsetFromCenter[callsign] = offset;
			}

			auto offsetIt = m_TagDragOffsetFromCenter.find(callsign);
			if (offsetIt != m_TagDragOffsetFromCenter.end())
			{
				tagCenter.x = Pt.x + offsetIt->second.x;
				tagCenter.y = Pt.y + offsetIt->second.y;
			}
			else
			{
				auto fullRectIt = m_TagAreas.find(callsign);
				if (fullRectIt != m_TagAreas.end())
				{
					CRect fullRect = fullRectIt->second;
					fullRect.NormalizeRect();
					tagCenter = fullRect.CenterPoint();
				}
				else
				{
					CRect tagRect(Area);
					tagRect.NormalizeRect();
					tagCenter = tagRect.CenterPoint();
				}
			}

			auto targetIt = m_TargetPoints.find(callsign);
			if (targetIt != m_TargetPoints.end())
			{
				POINT customTag = { tagCenter.x - targetIt->second.x, tagCenter.y - targetIt->second.y };
				m_TagOffsets[callsign] = customTag;

				double angle = fmod(atan2(double(customTag.y), double(customTag.x)) * 180.0 / 3.14159265358979323846, 360.0);
				if (angle < 0.0)
					angle += 360.0;
				m_TagAngles[callsign] = angle;
			}

			if (Released)
			{
				m_TagBeingDragged.clear();
				m_TagDragOffsetFromCenter.erase(callsign);
			}
			else
			{
				m_TagBeingDragged = callsign;
			}
		}
	}

	return true;
}

POINT CInsetWindow::projectPoint(CPosition pos)
{
	CRect areaRect = GetWindowContentRect();
	areaRect.NormalizeRect();

	POINT refPt = areaRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	POINT out = {0, 0};
	if (!m_AirportPositionValid)
		return refPt;

	double dist = m_AirportPosition.DistanceTo(pos);
	double dir = TrueBearing(m_AirportPosition, pos);


	out.x = refPt.x + int(m_Scale * dist * sin(dir) + 0.5);
	out.y = refPt.y - int(m_Scale * dist * cos(dir) + 0.5);

	if (m_Rotation != 0)
	{
		return rotate_point(out, m_Rotation, refPt);
	} else
	{
		return out;
	}
}

void CInsetWindow::renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || m_AvisoState == nullptr)
		return;
	if (radar_screen->IsShutdownRequested())
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect viewportRect = GetWindowContentRect();
	viewportRect.NormalizeRect();
	const int viewportWidth = viewportRect.Width();
	const int viewportHeight = viewportRect.Height();
	if (viewportWidth <= 0 || viewportHeight <= 0)
	{
		dc.Detach();
		return;
	}
	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	dc.FillSolidRect(viewportRect, RGB(10, 26, 38));
	radar_screen->AddScreenObject(m_Id, "window", viewportRect, false, "");
	radar_screen->AddScreenObject(m_Id, "viewport", viewportRect, false, "");

	auto drawCenteredMessage = [&](const char* message)
	{
		COLORREF oldTextColor = dc.SetTextColor(RGB(180, 190, 195));
		const CSize messageSize = dc.GetTextExtent(message);
		dc.TextOutA(
			viewportRect.left + (viewportRect.Width() - messageSize.cx) / 2,
			viewportRect.top + (viewportRect.Height() - messageSize.cy) / 2,
			message);
		dc.SetTextColor(oldTextColor);
	};

	auto drawChrome = [&]()
	{
		CBrush frameBrush(RGB(5, 7, 8));
		dc.FrameRect(viewportRect, &frameBrush);
		DrawInsetWindowChrome(
			dc,
			radar_screen,
			m_Id,
			m_AvisoLayoutMode,
			m_Area,
			"AVISO",
			false,
			mouseLocation,
			true,
			&m_LastChromeRenderMilliseconds);
	};

	const std::string airport = radar_screen->getActiveAirport();
	const std::string path = radar_screen->ResolveAvisoGeoJsonPathForAirport(airport);
	if (path.empty() ||
		!radar_screen->EnsureAvisoGeoJsonLoaded(path) ||
		(radar_screen->AvisoGeoJsonFeatures.empty() && radar_screen->AvisoGeoJsonLabels.empty()))
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	std::shared_ptr<const std::vector<CSMRRadar::AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<CSMRRadar::AvisoLabel>> labelSnapshot;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	std::shared_ptr<const CSMRRadar::AvisoFrequencyOwnershipSnapshot> frequencyOwnership;
	unsigned long long groupGeneration = 0;
	if (!radar_screen->GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		frequencyOwnership,
		groupGeneration))
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	if (const VsmrScene::RadarScene* scene = radar_screen->GetCurrentRadarScene();
		scene != nullptr && scene->avisoGeneration == groupGeneration)
	{
		frequencyOwnership = scene->frequencyOwnership;
	}

	if (!m_AvisoViewInitialized)
	{
		CPosition airportPosition;
		if (radar_screen->TryGetActiveAirportPosition(airportPosition))
		{
			m_AvisoCenterLatitude = airportPosition.m_Latitude;
			m_AvisoCenterLongitude = airportPosition.m_Longitude;
		}
		else if (radar_screen->AvisoGeoJsonHasBounds)
		{
			m_AvisoCenterLatitude = (radar_screen->AvisoGeoJsonMinLatitude + radar_screen->AvisoGeoJsonMaxLatitude) * 0.5;
			m_AvisoCenterLongitude = (radar_screen->AvisoGeoJsonMinLongitude + radar_screen->AvisoGeoJsonMaxLongitude) * 0.5;
		}
		m_AvisoViewInitialized = true;
		m_AvisoCenterLatitude = ClampAvisoLatitude(m_AvisoCenterLatitude);
	}

	std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> completedRenderResult = m_AvisoState->TakeCompletedRender();

	const int scale = max(1, m_AvisoScale);
	const double metersPerPixel = kAvisoMetersPerNm / static_cast<double>(scale);
	const double lonDegreesPerPixel = metersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoCenterLatitude));
	const double latDegreesPerPixel = metersPerPixel / kAvisoLatMetersPerDegree;
	const double halfLonSpan = static_cast<double>(viewportWidth) * lonDegreesPerPixel * 0.5;
	const double halfLatSpan = static_cast<double>(viewportHeight) * latDegreesPerPixel * 0.5;
	const double displayMinLon = m_AvisoCenterLongitude - halfLonSpan;
	const double displayMaxLon = m_AvisoCenterLongitude + halfLonSpan;
	const double displayMinLat = ClampAvisoLatitude(m_AvisoCenterLatitude - halfLatSpan);
	const double displayMaxLat = ClampAvisoLatitude(m_AvisoCenterLatitude + halfLatSpan);
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}

	const double scaleX = static_cast<double>(viewportWidth) / lonSpan;
	const double scaleY = static_cast<double>(viewportHeight) / latSpan;
	const double screenRotationDeg = ResolveAvisoViewportScreenRotationDeg(radar_screen, m_AvisoCenterLatitude, m_AvisoCenterLongitude);
	m_AvisoState->screenRotationDeg = screenRotationDeg;
	const CPoint viewportCenterPoint = viewportRect.CenterPoint();
	const Gdiplus::PointF viewportCenter(
		static_cast<Gdiplus::REAL>(viewportCenterPoint.x),
		static_cast<Gdiplus::REAL>(viewportCenterPoint.y));
	auto rotateViewportPoint = [&](double x, double y) -> Gdiplus::PointF
	{
		return RotateAvisoPointAround(x, y, viewportCenter, screenRotationDeg);
	};
	const Gdiplus::PointF projectedTopLeft = rotateViewportPoint(viewportRect.left, viewportRect.top);
	const Gdiplus::PointF projectedTopRight = rotateViewportPoint(viewportRect.right, viewportRect.top);
	const Gdiplus::PointF projectedBottomLeft = rotateViewportPoint(viewportRect.left, viewportRect.bottom);
	const Gdiplus::PointF projectedBottomRight = rotateViewportPoint(viewportRect.right, viewportRect.bottom);
	auto projectPoint = [&](double longitude, double latitude) -> Gdiplus::PointF
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(projectedTopLeft.X) + static_cast<double>(projectedTopRight.X - projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(projectedBottomLeft.X) + static_cast<double>(projectedBottomRight.X - projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(projectedTopLeft.Y) + static_cast<double>(projectedTopRight.Y - projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(projectedBottomLeft.Y) + static_cast<double>(projectedBottomRight.Y - projectedBottomLeft.Y) * u;
		return Gdiplus::PointF(
			static_cast<Gdiplus::REAL>(topX + (bottomX - topX) * v),
			static_cast<Gdiplus::REAL>(topY + (bottomY - topY) * v));
	};
	auto cacheTransformMatchesCurrentView = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr || !m_AvisoState->anchorValid)
			return false;
		if (m_AvisoState->cacheGroupGeneration != groupGeneration)
			return false;

		const double cachedLongitudeSpan =
			m_AvisoState->displayMaxLongitude - m_AvisoState->displayMinLongitude;
		const double cachedLatitudeSpan =
			m_AvisoState->displayMaxLatitude - m_AvisoState->displayMinLatitude;
		const double transformPixelTolerance = 12.0;
		return AvisoProjectionTransformWithinTolerance(
			m_AvisoState->projectedTopLeft,
			m_AvisoState->projectedTopRight,
			m_AvisoState->projectedBottomLeft,
			cachedLongitudeSpan,
			cachedLatitudeSpan,
			projectedTopLeft,
			projectedTopRight,
			projectedBottomLeft,
			lonSpan,
			latSpan,
			transformPixelTolerance);
	};
	auto completedResultMatchesCurrentView = [&](const CSMRRadar::AvisoRasterRenderResult& result) -> bool
	{
		if (result.bitmap == nullptr ||
			result.path != path ||
			result.groupGeneration != groupGeneration ||
			result.rasterWidth <= 0 ||
			result.rasterHeight <= 0)
		{
			return false;
		}

		const double resultLongitudeSpan = result.displayMaxLongitude - result.displayMinLongitude;
		const double resultLatitudeSpan = result.displayMaxLatitude - result.displayMinLatitude;
		const double transformPixelTolerance = 12.0;
		if (!AvisoProjectionTransformWithinTolerance(
			result.projectedTopLeft,
			result.projectedTopRight,
			result.projectedBottomLeft,
			resultLongitudeSpan,
			resultLatitudeSpan,
			projectedTopLeft,
			projectedTopRight,
			projectedBottomLeft,
			lonSpan,
			latSpan,
			transformPixelTolerance))
		{
			return false;
		}

		const double coverageToleranceLon = lonSpan * 0.02;
		const double coverageToleranceLat = latSpan * 0.02;
		return
			result.renderMinLongitude <= displayMinLon + coverageToleranceLon &&
			result.renderMaxLongitude >= displayMaxLon - coverageToleranceLon &&
			result.renderMinLatitude <= displayMinLat + coverageToleranceLat &&
			result.renderMaxLatitude >= displayMaxLat - coverageToleranceLat;
	};
	bool completedResultApplied = false;
	if (completedRenderResult != nullptr && completedResultMatchesCurrentView(*completedRenderResult))
	{
		std::lock_guard<std::mutex> groupGuard(radar_screen->AvisoGroupMutex);
		if (completedRenderResult->groupGeneration ==
			radar_screen->AvisoGroupGeneration.load(std::memory_order_relaxed))
		{
			m_AvisoState->ClearCache();
			m_AvisoState->cacheBitmap = completedRenderResult->bitmap;
			completedRenderResult->bitmap = nullptr;
			m_AvisoState->cachePath = completedRenderResult->path;
			m_AvisoState->cacheGroupGeneration = completedRenderResult->groupGeneration;
			m_AvisoState->cacheWidth = completedRenderResult->rasterWidth;
			m_AvisoState->cacheHeight = completedRenderResult->rasterHeight;
			m_AvisoState->displayMinLongitude = completedRenderResult->displayMinLongitude;
			m_AvisoState->displayMinLatitude = completedRenderResult->displayMinLatitude;
			m_AvisoState->displayMaxLongitude = completedRenderResult->displayMaxLongitude;
			m_AvisoState->displayMaxLatitude = completedRenderResult->displayMaxLatitude;
			m_AvisoState->renderMinLongitude = completedRenderResult->renderMinLongitude;
			m_AvisoState->renderMinLatitude = completedRenderResult->renderMinLatitude;
			m_AvisoState->renderMaxLongitude = completedRenderResult->renderMaxLongitude;
			m_AvisoState->renderMaxLatitude = completedRenderResult->renderMaxLatitude;
			m_AvisoState->projectedTopLeft = completedRenderResult->projectedTopLeft;
			m_AvisoState->projectedTopRight = completedRenderResult->projectedTopRight;
			m_AvisoState->projectedBottomLeft = completedRenderResult->projectedBottomLeft;
			m_AvisoState->projectedBottomRight = completedRenderResult->projectedBottomRight;
			m_AvisoState->anchorValid = true;
			completedResultApplied = true;
		}
	}
	if (completedResultApplied)
	{
		radar_screen->PerformanceDiagnostics.RecordAvisoResultApplied(
			VsmrPerformance::AvisoViewport::Inset);
	}
	else if (completedRenderResult != nullptr)
	{
		radar_screen->PerformanceDiagnostics.RecordAvisoResultDiscarded(
			VsmrPerformance::AvisoViewport::Inset);
		m_AvisoState->AllowRetryForDiscardedResult(completedRenderResult->requestId);
	}

	auto drawCache = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
			m_AvisoState->cacheGroupGeneration != groupGeneration ||
			m_AvisoState->cacheWidth <= 0 ||
			m_AvisoState->cacheHeight <= 0 ||
			!m_AvisoState->anchorValid)
		{
			return false;
		}
		if (!cacheTransformMatchesCurrentView())
			return false;

		const Gdiplus::PointF destTopLeft = projectPoint(m_AvisoState->renderMinLongitude, m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF destTopRight = projectPoint(m_AvisoState->renderMaxLongitude, m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF destBottomLeft = projectPoint(m_AvisoState->renderMinLongitude, m_AvisoState->renderMinLatitude);
		const Gdiplus::PointF destBottomRight = projectPoint(m_AvisoState->renderMaxLongitude, m_AvisoState->renderMinLatitude);
		const double destX = min(min(static_cast<double>(destTopLeft.X), static_cast<double>(destTopRight.X)), min(static_cast<double>(destBottomLeft.X), static_cast<double>(destBottomRight.X)));
		const double destY = min(min(static_cast<double>(destTopLeft.Y), static_cast<double>(destTopRight.Y)), min(static_cast<double>(destBottomLeft.Y), static_cast<double>(destBottomRight.Y)));
		const double destRight = max(max(static_cast<double>(destTopLeft.X), static_cast<double>(destTopRight.X)), max(static_cast<double>(destBottomLeft.X), static_cast<double>(destBottomRight.X)));
		const double destBottom = max(max(static_cast<double>(destTopLeft.Y), static_cast<double>(destTopRight.Y)), max(static_cast<double>(destBottomLeft.Y), static_cast<double>(destBottomRight.Y)));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		const double visibleLeft = max(destX, static_cast<double>(viewportRect.left));
		const double visibleTop = max(destY, static_cast<double>(viewportRect.top));
		const double visibleRight = min(destRight, static_cast<double>(viewportRect.right));
		const double visibleBottom = min(destBottom, static_cast<double>(viewportRect.bottom));
		const double visibleWidth = visibleRight - visibleLeft;
		const double visibleHeight = visibleBottom - visibleTop;
		if (visibleWidth < 1.0 || visibleHeight < 1.0)
			return false;

		const double sourceScaleX = static_cast<double>(m_AvisoState->cacheWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(m_AvisoState->cacheHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, m_AvisoState->cacheWidth);
		sourceYInt = std::clamp(sourceYInt, 0, m_AvisoState->cacheHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, m_AvisoState->cacheWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, m_AvisoState->cacheHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		// Derive the integer destination from the rounded source crop so both
		// rectangles remain on one geographic transform. Rounding them
		// independently creates a visible one-pixel snap when a new cache arrives.
		const double alignedDestLeft = destX + (static_cast<double>(sourceXInt) / sourceScaleX);
		const double alignedDestTop = destY + (static_cast<double>(sourceYInt) / sourceScaleY);
		const double alignedDestRight = destX + (static_cast<double>(sourceRightInt) / sourceScaleX);
		const double alignedDestBottom = destY + (static_cast<double>(sourceBottomInt) / sourceScaleY);
		const int destLeft = static_cast<int>(std::lround(alignedDestLeft));
		const int destTop = static_cast<int>(std::lround(alignedDestTop));
		const int destRightInt = static_cast<int>(std::lround(alignedDestRight));
		const int destBottomInt = static_cast<int>(std::lround(alignedDestBottom));
		const int destWidthInt = destRightInt - destLeft;
		const int destHeightInt = destBottomInt - destTop;
		if (destWidthInt <= 0 || destHeightInt <= 0)
			return false;

		HDC sourceDc = ::CreateCompatibleDC(hDC);
		if (sourceDc == nullptr)
			return false;
		HGDIOBJ oldBitmap = ::SelectObject(sourceDc, m_AvisoState->cacheBitmap);
		if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR)
		{
			::DeleteDC(sourceDc);
			return false;
		}

		gdi->Flush(Gdiplus::FlushIntentionFlush);
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
		{
			::SelectObject(sourceDc, oldBitmap);
			::DeleteDC(sourceDc);
			return false;
		}

		::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
		const bool nearNativeScale =
			std::abs(static_cast<double>(destWidthInt - sourceWidthInt)) <= 1.0 &&
			std::abs(static_cast<double>(destHeightInt - sourceHeightInt)) <= 1.0;
		const int oldStretchMode = ::SetStretchBltMode(hDC, nearNativeScale ? COLORONCOLOR : HALFTONE);
		if (!nearNativeScale)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			destWidthInt,
			destHeightInt,
			sourceDc,
			sourceXInt,
			sourceYInt,
			sourceWidthInt,
			sourceHeightInt,
			blend);

		if (oldStretchMode != 0)
			::SetStretchBltMode(hDC, oldStretchMode);
		::RestoreDC(hDC, savedDc);
		::SelectObject(sourceDc, oldBitmap);
		::DeleteDC(sourceDc);
		return blended != FALSE;
	};
	auto drawPreviousCacheViewportAligned = [&]() -> bool
	{
		// Retain a geographically covered previous raster while the definitive
		// view is debounced or rebuilt. A resize must not stretch a clamped crop
		// to a new aspect ratio; the normal geo-aligned path handles that case.
		if (m_WindowResizeActive)
			return false;
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
			m_AvisoState->cacheWidth <= 0 ||
			m_AvisoState->cacheHeight <= 0 ||
			!m_AvisoState->anchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = m_AvisoState->displayMaxLongitude - m_AvisoState->displayMinLongitude;
		const double cachedDisplayLatSpan = m_AvisoState->displayMaxLatitude - m_AvisoState->displayMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		auto projectCachedPoint = [&](double longitude, double latitude) -> Gdiplus::PointF
		{
			const double u = (longitude - m_AvisoState->displayMinLongitude) / cachedDisplayLonSpan;
			const double v = (m_AvisoState->displayMaxLatitude - latitude) / cachedDisplayLatSpan;
			const double topX = static_cast<double>(m_AvisoState->projectedTopLeft.X) + static_cast<double>(m_AvisoState->projectedTopRight.X - m_AvisoState->projectedTopLeft.X) * u;
			const double bottomX = static_cast<double>(m_AvisoState->projectedBottomLeft.X) + static_cast<double>(m_AvisoState->projectedBottomRight.X - m_AvisoState->projectedBottomLeft.X) * u;
			const double topY = static_cast<double>(m_AvisoState->projectedTopLeft.Y) + static_cast<double>(m_AvisoState->projectedTopRight.Y - m_AvisoState->projectedTopLeft.Y) * u;
			const double bottomY = static_cast<double>(m_AvisoState->projectedBottomLeft.Y) + static_cast<double>(m_AvisoState->projectedBottomRight.Y - m_AvisoState->projectedBottomLeft.Y) * u;
			return Gdiplus::PointF(
				static_cast<Gdiplus::REAL>(topX + (bottomX - topX) * v),
				static_cast<Gdiplus::REAL>(topY + (bottomY - topY) * v));
		};

		const Gdiplus::PointF renderTopLeft = projectCachedPoint(m_AvisoState->renderMinLongitude, m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF renderTopRight = projectCachedPoint(m_AvisoState->renderMaxLongitude, m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF renderBottomLeft = projectCachedPoint(m_AvisoState->renderMinLongitude, m_AvisoState->renderMinLatitude);
		const Gdiplus::PointF renderBottomRight = projectCachedPoint(m_AvisoState->renderMaxLongitude, m_AvisoState->renderMinLatitude);
		const double cachedRenderLeft = min(min(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), min(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double cachedRenderTop = min(min(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), min(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double cachedRenderRight = max(max(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), max(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double cachedRenderBottom = max(max(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), max(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double cachedRenderWidth = cachedRenderRight - cachedRenderLeft;
		const double cachedRenderHeight = cachedRenderBottom - cachedRenderTop;
		if (cachedRenderWidth < 1.0 || cachedRenderHeight < 1.0)
			return false;

		const Gdiplus::PointF sourceTopLeft = projectCachedPoint(displayMinLon, displayMaxLat);
		const Gdiplus::PointF sourceTopRight = projectCachedPoint(displayMaxLon, displayMaxLat);
		const Gdiplus::PointF sourceBottomLeft = projectCachedPoint(displayMinLon, displayMinLat);
		const Gdiplus::PointF sourceBottomRight = projectCachedPoint(displayMaxLon, displayMinLat);
		const double sourceLeft = min(min(static_cast<double>(sourceTopLeft.X), static_cast<double>(sourceTopRight.X)), min(static_cast<double>(sourceBottomLeft.X), static_cast<double>(sourceBottomRight.X)));
		const double sourceTop = min(min(static_cast<double>(sourceTopLeft.Y), static_cast<double>(sourceTopRight.Y)), min(static_cast<double>(sourceBottomLeft.Y), static_cast<double>(sourceBottomRight.Y)));
		const double sourceRight = max(max(static_cast<double>(sourceTopLeft.X), static_cast<double>(sourceTopRight.X)), max(static_cast<double>(sourceBottomLeft.X), static_cast<double>(sourceBottomRight.X)));
		const double sourceBottom = max(max(static_cast<double>(sourceTopLeft.Y), static_cast<double>(sourceTopRight.Y)), max(static_cast<double>(sourceBottomLeft.Y), static_cast<double>(sourceBottomRight.Y)));

		const double sourceScaleX = static_cast<double>(m_AvisoState->cacheWidth) / cachedRenderWidth;
		const double sourceScaleY = static_cast<double>(m_AvisoState->cacheHeight) / cachedRenderHeight;
		const double sourceX = (sourceLeft - cachedRenderLeft) * sourceScaleX;
		const double sourceY = (sourceTop - cachedRenderTop) * sourceScaleY;
		const double sourceRightRaster = (sourceRight - cachedRenderLeft) * sourceScaleX;
		const double sourceBottomRaster = (sourceBottom - cachedRenderTop) * sourceScaleY;
		const double coverageTolerance = 1e-6;
		if (sourceX < -coverageTolerance ||
			sourceY < -coverageTolerance ||
			sourceRightRaster > static_cast<double>(m_AvisoState->cacheWidth) + coverageTolerance ||
			sourceBottomRaster > static_cast<double>(m_AvisoState->cacheHeight) + coverageTolerance)
		{
			return false;
		}

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceRightRaster));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceBottomRaster));
		sourceXInt = std::clamp(sourceXInt, 0, m_AvisoState->cacheWidth);
		sourceYInt = std::clamp(sourceYInt, 0, m_AvisoState->cacheHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, m_AvisoState->cacheWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, m_AvisoState->cacheHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		HDC sourceDc = ::CreateCompatibleDC(hDC);
		if (sourceDc == nullptr)
			return false;
		HGDIOBJ oldBitmap = ::SelectObject(sourceDc, m_AvisoState->cacheBitmap);
		if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR)
		{
			::DeleteDC(sourceDc);
			return false;
		}

		gdi->Flush(Gdiplus::FlushIntentionFlush);
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
		{
			::SelectObject(sourceDc, oldBitmap);
			::DeleteDC(sourceDc);
			return false;
		}

		::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
		const bool nearNativeScale =
			std::abs(static_cast<double>(viewportRect.Width() - sourceWidthInt)) <= 1.0 &&
			std::abs(static_cast<double>(viewportRect.Height() - sourceHeightInt)) <= 1.0;
		const int oldStretchMode = ::SetStretchBltMode(hDC, nearNativeScale ? COLORONCOLOR : HALFTONE);
		if (!nearNativeScale)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			viewportRect.left,
			viewportRect.top,
			viewportRect.Width(),
			viewportRect.Height(),
			sourceDc,
			sourceXInt,
			sourceYInt,
			sourceWidthInt,
			sourceHeightInt,
			blend);

		if (oldStretchMode != 0)
			::SetStretchBltMode(hDC, oldStretchMode);
		::RestoreDC(hDC, savedDc);
		::SelectObject(sourceDc, oldBitmap);
		::DeleteDC(sourceDc);
		return blended != FALSE;
	};

	auto cacheHasWorkingMargin = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
			m_AvisoState->cacheGroupGeneration != groupGeneration ||
			!m_AvisoState->anchorValid)
		{
			return false;
		}
		if (!cacheTransformMatchesCurrentView())
			return false;

		const double cachedDisplayLonSpan = m_AvisoState->displayMaxLongitude - m_AvisoState->displayMinLongitude;
		const double cachedDisplayLatSpan = m_AvisoState->displayMaxLatitude - m_AvisoState->displayMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		if (lonScaleRatio < 0.985 || lonScaleRatio > 1.015 ||
			latScaleRatio < 0.985 || latScaleRatio > 1.015)
		{
			return false;
		}

		const double requiredLonMargin = lonSpan * 0.25;
		const double requiredLatMargin = latSpan * 0.25;
		return
			m_AvisoState->renderMinLongitude <= displayMinLon - requiredLonMargin &&
			m_AvisoState->renderMaxLongitude >= displayMaxLon + requiredLonMargin &&
			m_AvisoState->renderMinLatitude <= displayMinLat - requiredLatMargin &&
			m_AvisoState->renderMaxLatitude >= displayMaxLat + requiredLatMargin;
	};

	bool cacheDrawn = drawCache();
	if (!cacheDrawn)
		cacheDrawn = drawPreviousCacheViewportAligned();
	bool updateRequested = false;
	if (!cacheDrawn || !cacheHasWorkingMargin())
	{
		// Half a viewport of overscan still doubles each raster dimension and
		// comfortably exceeds the 25% refresh margin, while avoiding the 56%
		// extra bitmap area produced by the previous 75% margin.
		const double overscanRatio = 0.50;
		const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
		const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
		const double renderMinLat = displayMinLat - (latSpan * overscanRatio);
		const double renderMaxLat = displayMaxLat + (latSpan * overscanRatio);
		const Gdiplus::PointF renderTopLeft = projectPoint(renderMinLon, renderMaxLat);
		const Gdiplus::PointF renderTopRight = projectPoint(renderMaxLon, renderMaxLat);
		const Gdiplus::PointF renderBottomLeft = projectPoint(renderMinLon, renderMinLat);
		const Gdiplus::PointF renderBottomRight = projectPoint(renderMaxLon, renderMinLat);
		const double renderScreenLeft = min(min(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), min(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double renderScreenTop = min(min(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), min(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double renderScreenRight = max(max(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), max(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double renderScreenBottom = max(max(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), max(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double renderPixelWidth = renderScreenRight - renderScreenLeft;
		const double renderPixelHeight = renderScreenBottom - renderScreenTop;
		if (renderPixelWidth > 0.0 && renderPixelHeight > 0.0)
		{
			const double targetRasterScale = 1.0;
			const double maxRasterSide = 6400.0;
			const double maxRasterPixels = 18000000.0;
			double rasterScale = targetRasterScale;
			const double maxDimension = max(renderPixelWidth, renderPixelHeight);
			const double sideLimitedScale = maxRasterSide / maxDimension;
			if (sideLimitedScale > 0.0 && sideLimitedScale < rasterScale)
				rasterScale = sideLimitedScale;
			const double pixelLimitedScale = std::sqrt(maxRasterPixels / (renderPixelWidth * renderPixelHeight));
			if (pixelLimitedScale > 0.0 && pixelLimitedScale < rasterScale)
				rasterScale = pixelLimitedScale;
			// Keep the allocation caps hard even on unusually large desktops.
			rasterScale = min(rasterScale, targetRasterScale);

			CSMRRadar::AvisoRasterRenderRequest request;
			request.groupGeneration = groupGeneration;
			request.path = path;
			request.features = featureSnapshot;
			request.labels = labelSnapshot;
			request.groupVisibility = groupVisibility;
			request.frequencyOwnership = frequencyOwnership;
			request.rasterWidth = max(1, static_cast<int>(std::floor(renderPixelWidth * rasterScale)));
			request.rasterHeight = max(1, static_cast<int>(std::floor(renderPixelHeight * rasterScale)));
			request.rasterScale = rasterScale;
			request.displayMinLongitude = displayMinLon;
			request.displayMinLatitude = displayMinLat;
			request.displayMaxLongitude = displayMaxLon;
			request.displayMaxLatitude = displayMaxLat;
			request.renderMinLongitude = renderMinLon;
			request.renderMinLatitude = renderMinLat;
			request.renderMaxLongitude = renderMaxLon;
			request.renderMaxLatitude = renderMaxLat;
			request.renderScreenLeft = renderScreenLeft;
			request.renderScreenTop = renderScreenTop;
			request.scaleX = scaleX;
			request.scaleY = scaleY;
			request.projectedTopLeft = projectedTopLeft;
			request.projectedTopRight = projectedTopRight;
			request.projectedBottomLeft = projectedBottomLeft;
			request.projectedBottomRight = projectedBottomRight;

			updateRequested = true;
			m_AvisoState->QueueRender(radar_screen, std::move(request));
		}
	}
	const bool delayedByAvisoUpdate = updateRequested &&
		m_AvisoState->renderPending.load(std::memory_order_relaxed);
	radar_screen->PerformanceDiagnostics.RecordAvisoCacheOutcome(
		VsmrPerformance::AvisoViewport::Inset,
		cacheDrawn
			? (delayedByAvisoUpdate
				? VsmrPerformance::AvisoCacheOutcome::Preview
				: VsmrPerformance::AvisoCacheOutcome::Exact)
			: VsmrPerformance::AvisoCacheOutcome::Miss,
		delayedByAvisoUpdate,
		!cacheDrawn);

	if (!cacheDrawn)
		drawCenteredMessage(m_AvisoState->renderPending.load(std::memory_order_relaxed) ? "Rendering AVISO" : "AVISO unavailable");

	auto drawAircraft = [&]()
	{
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
			return;

		::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
		Gdiplus::GraphicsState graphicsState = gdi->Save();
		gdi->SetClip(CopyRect(viewportRect), Gdiplus::CombineModeIntersect);
		m_TargetPoints.clear();
		m_TagAreas.clear();

		auto pointInViewport = [&](const POINT& point, int margin = 0) -> bool
		{
			return
				point.x >= viewportRect.left - margin &&
				point.x <= viewportRect.right + margin &&
				point.y >= viewportRect.top - margin &&
				point.y <= viewportRect.bottom + margin;
		};
		auto clipToViewport = [&](CRect rect) -> CRect
		{
			rect.NormalizeRect();
			CRect clipped;
			clipped.IntersectRect(rect, viewportRect);
			return clipped;
		};
		auto projectTargetPosition = [&](const CPosition& position) -> POINT
		{
			const Gdiplus::PointF projected = projectPoint(position.m_Longitude, position.m_Latitude);
			return {
				static_cast<LONG>(std::lround(static_cast<double>(projected.X))),
				static_cast<LONG>(std::lround(static_cast<double>(projected.Y)))
			};
		};
		auto positionNearViewport = [&](const CPosition& position) -> bool
		{
			const double lonMargin = lonSpan * 0.25;
			const double latMargin = latSpan * 0.25;
			return
				position.m_Longitude >= displayMinLon - lonMargin &&
				position.m_Longitude <= displayMaxLon + lonMargin &&
				position.m_Latitude >= displayMinLat - latMargin &&
				position.m_Latitude <= displayMaxLat + latMargin;
		};
		auto rectIntersectsViewport = [&](const CRect& rect) -> bool
		{
			return AvisoRectIntersects(rect, viewportRect);
		};

		static const Value emptyObject(kObjectType);
		const Value& activeProfile = (radar_screen->CurrentConfig != nullptr)
			? radar_screen->CurrentConfig->getActiveProfile()
			: emptyObject;
		auto getProfileObjectSection = [&](const char* key) -> const Value*
		{
			if (!activeProfile.IsObject() || !activeProfile.HasMember(key) || !activeProfile[key].IsObject())
				return nullptr;
			return &activeProfile[key];
		};
		auto getSectionBool = [&](const Value* section, const char* key, bool fallback) -> bool
		{
			if (section != nullptr && section->HasMember(key) && (*section)[key].IsBool())
				return (*section)[key].GetBool();
			return fallback;
		};
		auto getSectionColor = [&](const Value* section, const char* key, const Color& fallback) -> Color
		{
			if (radar_screen->CurrentConfig != nullptr &&
				section != nullptr &&
				section->HasMember(key) &&
				(*section)[key].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
			}
			return fallback;
		};
		const Value* rimcasSection = getProfileObjectSection("rimcas");
		const bool rimcasLabelOnlySetting = getSectionBool(rimcasSection, "rimcas_label_only", true);
		const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
		const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
		const VsmrScene::RadarScene* targetScene = radar_screen->GetCurrentRadarScene();
		const VsmrScene::TargetPresentation defaultTargetPresentation;
		const VsmrScene::TargetPresentation& targetPresentation = targetScene != nullptr
			? targetScene->targetPresentation
			: defaultTargetPresentation;
		const bool useNovaIconStyle = targetPresentation.icon == VsmrScene::IconStyle::Nova;
		const bool useDiamondIconStyle = targetPresentation.icon == VsmrScene::IconStyle::Diamond;
		const bool useRealisticIconStyle = targetPresentation.icon == VsmrScene::IconStyle::Realistic;
		const bool smallIconBoostEnabled = targetPresentation.smallIconBoostEnabled;
		const bool fixedPixelIconSize = targetPresentation.fixedPixelSize;
		const double smallIconBoostFactor = targetPresentation.smallIconBoostFactor;
		const double smallIconBoostResolutionScale = targetPresentation.resolutionScale;
		const double fixedTriangleScale = targetPresentation.fixedTriangleScale;
		const double pixPerMeter = max(0.0, static_cast<double>(max(1, m_AvisoScale)) / kAvisoMetersPerNm);
		const Color symbolWhiteColor(255, 255, 255, 255);
		const unsigned long long realisticIconCacheFrame = useRealisticIconStyle
			? ++radar_screen->RealisticIconCacheFrame
			: radar_screen->RealisticIconCacheFrame;

		const Gdiplus::InterpolationMode savedInterpolationMode = gdi->GetInterpolationMode();
		const Gdiplus::PixelOffsetMode savedPixelOffsetMode = gdi->GetPixelOffsetMode();
		const Gdiplus::CompositingQuality savedCompositingQuality = gdi->GetCompositingQuality();
		if (useRealisticIconStyle)
		{
			gdi->SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
			gdi->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighSpeed);
			gdi->SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
		}

		CPen symbolPen(PS_SOLID, 1, symbolWhiteColor.ToCOLORREF());

		vector<POINT> appAreaVect = {
			viewportRect.TopLeft(),
			{ viewportRect.right, viewportRect.top },
			viewportRect.BottomRight(),
			{ viewportRect.left, viewportRect.bottom }
		};
		std::vector<PointF> patatoidePolygonPoints;
		auto drawPatatoidePolygon = [&](const std::vector<VsmrScene::GeoPoint>& sourcePoints, const Color& fillColor)
		{
			if (sourcePoints.size() < 3)
				return;

			patatoidePolygonPoints.clear();
			patatoidePolygonPoints.reserve(sourcePoints.size());
			for (const VsmrScene::GeoPoint& sourcePoint : sourcePoints)
			{
				if (!sourcePoint.valid)
					continue;
				const Gdiplus::PointF point = projectPoint(sourcePoint.longitude, sourcePoint.latitude);
				patatoidePolygonPoints.emplace_back(point);
			}

			if (patatoidePolygonPoints.size() < 3)
				return;

			SolidBrush polygonBrush(fillColor);
			gdi->FillPolygon(&polygonBrush, patatoidePolygonPoints.data(), static_cast<INT>(patatoidePolygonPoints.size()));
		};

		auto drawConfiguredIcon = [&](const VsmrScene::Target& sceneTarget, const POINT& targetPoint) -> int
		{
			Color targetColor(
				sceneTarget.style.color.alpha,
				sceneTarget.style.color.red,
				sceneTarget.style.color.green,
				sceneTarget.style.color.blue);
			if (useNovaIconStyle)
			{
				Pen novaSymbolPen(targetColor, 1.0f);
				if (sceneTarget.transponderModeC)
				{
					PointF novaPoints[] = {
						PointF(static_cast<REAL>(targetPoint.x), static_cast<REAL>(targetPoint.y - 6)),
						PointF(static_cast<REAL>(targetPoint.x - 6), static_cast<REAL>(targetPoint.y)),
						PointF(static_cast<REAL>(targetPoint.x), static_cast<REAL>(targetPoint.y + 6)),
						PointF(static_cast<REAL>(targetPoint.x + 6), static_cast<REAL>(targetPoint.y)),
						PointF(static_cast<REAL>(targetPoint.x), static_cast<REAL>(targetPoint.y - 6))
					};
					gdi->DrawLines(&novaSymbolPen, novaPoints, static_cast<INT>(_countof(novaPoints)));
				}
				else
				{
					gdi->DrawLine(&novaSymbolPen, targetPoint.x, targetPoint.y, targetPoint.x - 4, targetPoint.y - 4);
					gdi->DrawLine(&novaSymbolPen, targetPoint.x, targetPoint.y, targetPoint.x + 4, targetPoint.y - 4);
					gdi->DrawLine(&novaSymbolPen, targetPoint.x, targetPoint.y, targetPoint.x - 4, targetPoint.y + 4);
					gdi->DrawLine(&novaSymbolPen, targetPoint.x, targetPoint.y, targetPoint.x + 4, targetPoint.y + 4);
				}
				return 18;
			}
			const double headingDeg = sceneTarget.headingTrueDegrees;

			const std::string& iconType = sceneTarget.style.assetKey;
			Gdiplus::Bitmap* iconBmp = nullptr;
			if (useRealisticIconStyle)
				iconBmp = radar_screen->GetAircraftIcon(iconType);

			UINT iconBmpWidth = 0;
			UINT iconBmpHeight = 0;
			bool canUseRealisticIcon = useRealisticIconStyle && iconBmp != nullptr;
			if (canUseRealisticIcon)
			{
				iconBmpWidth = iconBmp->GetWidth();
				iconBmpHeight = iconBmp->GetHeight();
				canUseRealisticIcon = iconBmp->GetLastStatus() == Gdiplus::Ok && iconBmpWidth > 0 && iconBmpHeight > 0;
			}

			if (canUseRealisticIcon)
			{
				const double lengthMeters = sceneTarget.style.lengthMeters;
				const double spanMeters = sceneTarget.style.wingspanMeters;

				double drawW = spanMeters * pixPerMeter;
				double drawH = lengthMeters * pixPerMeter;
				if (fixedPixelIconSize)
				{
					const double configuredFactor = smallIconBoostEnabled ? smallIconBoostFactor : 1.0;
					const double pxPerMeterFixed = (18.0 * smallIconBoostResolutionScale) / 40.0;
					drawW = spanMeters * pxPerMeterFixed * configuredFactor;
					drawH = lengthMeters * pxPerMeterFixed * configuredFactor;
				}
				else if (smallIconBoostEnabled && pixPerMeter > 0.0)
				{
					const double referenceScreenSize = 40.0 * pixPerMeter;
					const double boostStartSize = 14.0 * smallIconBoostResolutionScale;
					if (referenceScreenSize < boostStartSize)
					{
						const double boostedReferenceSize = 18.0 * smallIconBoostFactor * smallIconBoostResolutionScale;
						const double zoomBoostScale = std::clamp(boostedReferenceSize / max(0.01, referenceScreenSize), 1.0, 6.0 * smallIconBoostFactor * smallIconBoostResolutionScale);
						drawW *= zoomBoostScale;
						drawH *= zoomBoostScale;
					}
				}
				drawW = AvisoFinitePositive(drawW, 24.0, 12.0, 1200.0);
				drawH = AvisoFinitePositive(drawH, 24.0, 12.0, 1200.0);

				int drawPixelW = 0;
				int drawPixelH = 0;
				std::string scaledCacheKey;
				Gdiplus::Bitmap* cachedIcon = radar_screen->GetCachedRealisticIconBitmap(
					iconType,
					iconBmp,
					iconBmpWidth,
					iconBmpHeight,
					true,
					targetColor,
					drawW,
					drawH,
					realisticIconCacheFrame,
					drawPixelW,
					drawPixelH,
					scaledCacheKey);
				if (cachedIcon == nullptr)
				{
					drawPixelW = std::clamp(static_cast<int>(std::lround(drawW)), 1, 2048);
					drawPixelH = std::clamp(static_cast<int>(std::lround(drawH)), 1, 2048);
				}

				CPosition nosePos;
				nosePos.m_Latitude = sceneTarget.headingProbe.latitude;
				nosePos.m_Longitude = sceneTarget.headingProbe.longitude;
				POINT nosePix = projectTargetPosition(nosePos);
				const double screenHeadingDeg = atan2(double(nosePix.y - targetPoint.y), double(nosePix.x - targetPoint.x)) * 180.0 / 3.14159265358979323846;
				double rotationDeg = screenHeadingDeg + 90.0;
				if (!std::isfinite(rotationDeg))
					rotationDeg = 0.0;

				CSMRRadar::RealisticIconCacheEntry* rotatedIcon = radar_screen->GetCachedRotatedRealisticIconBitmap(
					scaledCacheKey,
					cachedIcon,
					drawPixelW,
					drawPixelH,
					rotationDeg,
					realisticIconCacheFrame);
				if (rotatedIcon != nullptr && rotatedIcon->bitmap != nullptr)
				{
					gdi->DrawImage(rotatedIcon->bitmap.get(), targetPoint.x - rotatedIcon->centerX, targetPoint.y - rotatedIcon->centerY);
					return max(
						static_cast<int>(rotatedIcon->bitmap->GetWidth()),
						static_cast<int>(rotatedIcon->bitmap->GetHeight()));
				}
				else
				{
					GraphicsState state = gdi->Save();
					Gdiplus::Matrix matrix;
					matrix.Translate(Gdiplus::REAL(targetPoint.x), Gdiplus::REAL(targetPoint.y));
					matrix.Rotate(Gdiplus::REAL(rotationDeg));
					matrix.Translate(Gdiplus::REAL(-drawPixelW / 2.0), Gdiplus::REAL(-drawPixelH / 2.0));
					gdi->SetTransform(&matrix);
					if (cachedIcon != nullptr)
						gdi->DrawImage(cachedIcon, 0, 0);
					else
						gdi->DrawImage(iconBmp, Gdiplus::REAL(0), Gdiplus::REAL(0), Gdiplus::REAL(drawPixelW), Gdiplus::REAL(drawPixelH));
					gdi->Restore(state);

					const double rotationRadians = rotationDeg * 3.14159265358979323846 / 180.0;
					const double absCos = std::abs(std::cos(rotationRadians));
					const double absSin = std::abs(std::sin(rotationRadians));
					const int rotatedWidth = static_cast<int>(std::ceil(drawPixelW * absCos + drawPixelH * absSin));
					const int rotatedHeight = static_cast<int>(std::ceil(drawPixelW * absSin + drawPixelH * absCos));
					return max(rotatedWidth, rotatedHeight);
				}
			}

			double lenPx = 20.0;
			double halfWidthPx = 12.0;
			double lenMetersUsed = 20.0;
			double halfWidthMetersUsed = 12.0;
			if (fixedPixelIconSize)
			{
				const double fixedScale = (smallIconBoostEnabled ? smallIconBoostFactor : 1.0) * smallIconBoostResolutionScale;
				lenPx = std::clamp(lenPx * fixedScale, 6.0, 160.0);
				halfWidthPx = std::clamp(halfWidthPx * fixedScale, 3.0, 80.0);
			}
			else if (pixPerMeter > 0.0)
			{
				lenPx = std::clamp(pixPerMeter * 20.0, 6.0, 120.0);
				halfWidthPx = std::clamp(pixPerMeter * 12.0, 3.0, 60.0);
				if (smallIconBoostEnabled)
				{
					const double currentExtent = lenPx + halfWidthPx;
					const double targetMinExtent = 14.0 * smallIconBoostFactor * smallIconBoostResolutionScale;
					const double boostScale = std::clamp(targetMinExtent / max(0.01, currentExtent), 1.0, 2.0 * smallIconBoostFactor * smallIconBoostResolutionScale);
					lenPx *= boostScale;
					halfWidthPx *= boostScale;
				}
			}
			lenPx = AvisoFinitePositive(lenPx * fixedTriangleScale, 20.0, 1.0, 220.0);
			halfWidthPx = AvisoFinitePositive(halfWidthPx * fixedTriangleScale, 12.0, 1.0, 110.0);
			if (pixPerMeter > 0.0)
			{
				lenMetersUsed = lenPx / pixPerMeter;
				halfWidthMetersUsed = halfWidthPx / pixPerMeter;
			}

			if (useDiamondIconStyle)
			{
				const double diagonalPx = std::clamp(lenPx + halfWidthPx, 10.0, 220.0);
				const double sidePx = diagonalPx / std::sqrt(2.0);
				const double halfSide = sidePx / 2.0;
				const Gdiplus::REAL rectX = static_cast<Gdiplus::REAL>(targetPoint.x - halfSide);
				const Gdiplus::REAL rectY = static_cast<Gdiplus::REAL>(targetPoint.y - halfSide);
				const Gdiplus::REAL rectW = static_cast<Gdiplus::REAL>(sidePx);
				const Gdiplus::REAL rectH = static_cast<Gdiplus::REAL>(sidePx);
				Gdiplus::REAL radius = std::clamp(static_cast<Gdiplus::REAL>(sidePx * 0.22), 2.0f, static_cast<Gdiplus::REAL>(sidePx / 2.0));

				Gdiplus::GraphicsPath diamondPath;
				const Gdiplus::REAL diameter = radius * 2.0f;
				diamondPath.AddArc(rectX, rectY, diameter, diameter, 180, 90);
				diamondPath.AddArc(rectX + rectW - diameter, rectY, diameter, diameter, 270, 90);
				diamondPath.AddArc(rectX + rectW - diameter, rectY + rectH - diameter, diameter, diameter, 0, 90);
				diamondPath.AddArc(rectX, rectY + rectH - diameter, diameter, diameter, 90, 90);
				diamondPath.CloseFigure();

				CPosition nosePos;
				nosePos.m_Latitude = sceneTarget.headingProbe.latitude;
				nosePos.m_Longitude = sceneTarget.headingProbe.longitude;
				POINT nosePix = projectTargetPosition(nosePos);
				const double screenHeadingDeg = atan2(double(nosePix.y - targetPoint.y), double(nosePix.x - targetPoint.x)) * 180.0 / 3.14159265358979323846;
				Gdiplus::GraphicsState state = gdi->Save();
				Gdiplus::Matrix transform;
				transform.RotateAt(static_cast<Gdiplus::REAL>(screenHeadingDeg + 45.0), PointF(static_cast<Gdiplus::REAL>(targetPoint.x), static_cast<Gdiplus::REAL>(targetPoint.y)));
				gdi->MultiplyTransform(&transform);
				SolidBrush brush(targetColor);
				gdi->FillPath(&brush, &diamondPath);
				gdi->Restore(state);
				return int(max(12.0, diagonalPx));
			}

			auto wrap360 = [](double deg)
			{
				double wrapped = fmod(deg, 360.0);
				return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
			};
			CPosition acPos;
			acPos.m_Latitude = sceneTarget.position.latitude;
			acPos.m_Longitude = sceneTarget.position.longitude;
			const CPosition tipPos = BetterHarversine(acPos, wrap360(headingDeg), lenMetersUsed);
			const CPosition basePos = BetterHarversine(acPos, wrap360(headingDeg + 180.0), lenMetersUsed * 0.33);
			const CPosition notchPos = BetterHarversine(acPos, wrap360(headingDeg + 180.0), lenMetersUsed * 0.05);
			const CPosition rightPos = BetterHarversine(basePos, wrap360(headingDeg + 90.0), halfWidthMetersUsed);
			const CPosition leftPos = BetterHarversine(basePos, wrap360(headingDeg - 90.0), halfWidthMetersUsed);
			POINT tip = projectTargetPosition(tipPos);
			POINT right = projectTargetPosition(rightPos);
			POINT notch = projectTargetPosition(notchPos);
			POINT left = projectTargetPosition(leftPos);
			PointF arrow[4] = {
				PointF(Gdiplus::REAL(tip.x), Gdiplus::REAL(tip.y)),
				PointF(Gdiplus::REAL(right.x), Gdiplus::REAL(right.y)),
				PointF(Gdiplus::REAL(notch.x), Gdiplus::REAL(notch.y)),
				PointF(Gdiplus::REAL(left.x), Gdiplus::REAL(left.y))
			};
			SolidBrush arrowBrush(targetColor);
			gdi->FillPolygon(&arrowBrush, arrow, 4);
			return int(max(12.0, lenPx + halfWidthPx));
		};

		auto tagFontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
		Gdiplus::Font* tagRegularFont = (tagFontIt != radar_screen->customFonts.end()) ? tagFontIt->second.get() : nullptr;
		Gdiplus::Font* tagBoldFont = tagRegularFont;
		std::unique_ptr<Gdiplus::Font> tagBoldFontOwned;
		int tagBlankWidth = 2;
		int tagOneLineHeight = 10;
		Gdiplus::StringFormat defaultStringFormat;
		if (tagRegularFont != nullptr)
		{
			Gdiplus::FontFamily baseFamily;
			if (tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok)
			{
				const INT boldStyle = tagRegularFont->GetStyle() | Gdiplus::FontStyleBold;
				tagBoldFontOwned.reset(new Gdiplus::Font(&baseFamily, tagRegularFont->GetSize(), boldStyle, Gdiplus::UnitPixel));
				if (tagBoldFontOwned->GetLastStatus() == Gdiplus::Ok)
					tagBoldFont = tagBoldFontOwned.get();
			}

			RectF fontMeasureRect;
			gdi->MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &defaultStringFormat, &fontMeasureRect);
			tagBlankWidth = max(2, static_cast<int>(fontMeasureRect.GetRight()));

			fontMeasureRect = RectF(0, 0, 0, 0);
			gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
				tagRegularFont, PointF(0, 0), &defaultStringFormat, &fontMeasureRect);
			tagOneLineHeight = max(1, static_cast<int>(fontMeasureRect.GetBottom()));
			if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
			{
				RectF boldMeasureRect;
				gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
					tagBoldFont, PointF(0, 0), &defaultStringFormat, &boldMeasureRect);
				tagOneLineHeight = max(tagOneLineHeight, static_cast<int>(boldMeasureRect.GetBottom()));
			}
		}

		auto drawTag = [&](const VsmrScene::Target& sceneTarget, const POINT& targetPoint)
		{
			if (tagRegularFont == nullptr)
				return;
			const std::string& rtCallsign = sceneTarget.callsign;
			const std::string& bottomLine = sceneTarget.bottomLine;
			const int blankWidth = tagBlankWidth;
			const int oneLineHeight = tagOneLineHeight;

			RectF measureRect;

			POINT tagCenter{};
			m_TargetPoints[rtCallsign] = targetPoint;
			auto customTagOffsetIt = m_TagOffsets.find(rtCallsign);
			if (customTagOffsetIt != m_TagOffsets.end())
			{
				tagCenter.x = targetPoint.x + customTagOffsetIt->second.x;
				tagCenter.y = targetPoint.y + customTagOffsetIt->second.y;
			}
			else
			{
				if (m_TagAngles.find(rtCallsign) == m_TagAngles.end())
					m_TagAngles[rtCallsign] = 45.0;
				const int leaderLength = 50;
				tagCenter.x = long(targetPoint.x + float(leaderLength * cos(DegToRad(m_TagAngles[rtCallsign]))));
				tagCenter.y = long(targetPoint.y + float(leaderLength * sin(DegToRad(m_TagAngles[rtCallsign]))));
			}

			const map<string, string>& tagReplacingMap = sceneTarget.tag.tokens;

			struct RenderedTagElement
			{
				std::string text;
				int action = TAG_CITEM_NO;
				bool bold = false;
				VsmrScene::Color effectiveColor;
				int measuredWidth = 0;
				int measuredHeight = 0;
			};
			vector<vector<RenderedTagElement>> renderedLines;
			int tagWidth = 0;
			int tagHeight = 0;
			for (const VsmrScene::TagLine& sceneLine : sceneTarget.tag.normal.lines)
			{
				if (sceneLine.elements.empty())
					continue;

				vector<RenderedTagElement> renderedLine;
				renderedLine.reserve(sceneLine.elements.size());
				int tempTagWidth = 0;
				for (const VsmrScene::TagElement& sceneElement : sceneLine.elements)
				{
					RenderedTagElement renderedElement;
					renderedElement.text = sceneElement.text;
					renderedElement.action = sceneElement.action;
					renderedElement.bold = sceneElement.bold;
					renderedElement.effectiveColor = sceneElement.effectiveColor;
					if (!sceneElement.text.empty())
					{
						wstring wstr(sceneElement.text.begin(), sceneElement.text.end());
						Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
						gdi->MeasureString(wstr.c_str(), static_cast<INT>(wstr.size()), measureFont, PointF(0, 0), &defaultStringFormat, &measureRect);
						renderedElement.measuredWidth = static_cast<int>(measureRect.GetRight());
						renderedElement.measuredHeight = static_cast<int>(measureRect.GetBottom());
					}
					tempTagWidth += renderedElement.measuredWidth;
					renderedLine.push_back(std::move(renderedElement));
				}

				if (renderedLine.empty())
					continue;
				if (!renderedLine.empty())
					tempTagWidth += blankWidth * (static_cast<int>(renderedLine.size()) - 1);
				tagHeight += oneLineHeight;
				tagWidth = max(tagWidth, tempTagWidth);
				renderedLines.push_back(std::move(renderedLine));
			}

			if (renderedLines.empty())
			{
				auto callsignIt = tagReplacingMap.find("callsign");
				const std::string callsignText = (callsignIt != tagReplacingMap.end() && !callsignIt->second.empty())
					? callsignIt->second
					: rtCallsign;
				RenderedTagElement fallbackElement;
				fallbackElement.text = callsignText;
				fallbackElement.effectiveColor = sceneTarget.tag.normalPalette.text;
				wstring wstr(callsignText.begin(), callsignText.end());
				gdi->MeasureString(wstr.c_str(), wcslen(wstr.c_str()), tagRegularFont, PointF(0, 0), &defaultStringFormat, &measureRect);
				fallbackElement.measuredWidth = static_cast<int>(measureRect.GetRight());
				fallbackElement.measuredHeight = static_cast<int>(measureRect.GetBottom());
				tagWidth = fallbackElement.measuredWidth;
				tagHeight = oneLineHeight;
				renderedLines.push_back({ fallbackElement });
			}
			if (tagHeight > 0)
				tagHeight -= 2;

			const VsmrScene::TagPalette& tagPalette = sceneTarget.tag.normalPalette;
			const Color definedBackgroundColor = SceneColorToGdi(tagPalette.background);
			const Color definedBackgroundOnRunwayColor = SceneColorToGdi(tagPalette.backgroundOnRunway);

			const CRimcas::RimcasAlertTypes rimcasStage =
				static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
			auto resolveRimcasBackground = [&](bool includeAlertStage) -> Color
			{
				if (includeAlertStage && rimcasStage == CRimcas::StageOne)
					return rimcasStageOneColor;
				if (includeAlertStage && rimcasStage == CRimcas::StageTwo)
					return rimcasStageTwoColor;
				return sceneTarget.rimcas.onRunway ? definedBackgroundOnRunwayColor : definedBackgroundColor;
			};
			Color tagBackgroundColor = resolveRimcasBackground(true);
			if (rimcasLabelOnlySetting)
			{
				tagBackgroundColor = resolveRimcasBackground(false);
			}
			CRect tagBackgroundRect(
				tagCenter.x - (tagWidth / 2),
				tagCenter.y - (tagHeight / 2),
				tagCenter.x + (tagWidth / 2),
				tagCenter.y + (tagHeight / 2));
			const int tagPadding = 1;
			tagBackgroundRect.InflateRect(tagPadding, tagPadding);
			tagBackgroundRect.NormalizeRect();
			if (!rectIntersectsViewport(tagBackgroundRect) && !pointInViewport(targetPoint, 20))
				return;

			m_TagAreas[rtCallsign] = tagBackgroundRect;
			const CRect clippedTagRect = clipToViewport(tagBackgroundRect);
			if (!clippedTagRect.IsRectEmpty())
				radar_screen->AddScreenObject(m_Id, rtCallsign.c_str(), clippedTagRect, true, bottomLine.c_str());

			Gdiplus::GraphicsPath roundedPath;
			const int radius = 4;
			const int diameter = radius * 2;
			Rect roundedRect = CopyRect(tagBackgroundRect);
			roundedPath.AddArc(roundedRect.X, roundedRect.Y, diameter, diameter, 180, 90);
			roundedPath.AddArc(roundedRect.GetRight() - diameter, roundedRect.Y, diameter, diameter, 270, 90);
			roundedPath.AddArc(roundedRect.GetRight() - diameter, roundedRect.GetBottom() - diameter, diameter, diameter, 0, 90);
			roundedPath.AddArc(roundedRect.X, roundedRect.GetBottom() - diameter, diameter, diameter, 90, 90);
			roundedPath.CloseFigure();
			SolidBrush tagBackgroundBrush(tagBackgroundColor);
			gdi->FillPath(&tagBackgroundBrush, &roundedPath);

			const int textLeft = tagBackgroundRect.left + tagPadding;
			const int textTop = tagBackgroundRect.top + tagPadding;
			const int textWidth = max(0, tagBackgroundRect.Width() - (tagPadding * 2));
			int heightOffset = 0;
			for (auto&& line : renderedLines)
			{
				int lineWidth = 0;
				for (auto&& renderedElement : line)
					lineWidth += renderedElement.measuredWidth;
				if (!line.empty())
					lineWidth += blankWidth * (static_cast<int>(line.size()) - 1);

				int widthOffset = max(0, (textWidth - lineWidth) / 2);
				for (auto&& renderedElement : line)
				{
					if (renderedElement.text.empty())
					{
						widthOffset += blankWidth;
						continue;
					}

					Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					SolidBrush elementBrush(SceneColorToGdi(renderedElement.effectiveColor));

					wstring text(renderedElement.text.begin(), renderedElement.text.end());
					const int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
					gdi->DrawString(text.c_str(), wcslen(text.c_str()), drawFont,
						PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
						&defaultStringFormat, &elementBrush);

					const int clickItemType = renderedElement.action;

					const int itemWidth = renderedElement.measuredWidth;
					const int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);
					if (itemWidth > 0 && itemHeight > 0)
					{
						CRect itemRect(
							textLeft + widthOffset,
							textTop + heightOffset,
							textLeft + widthOffset + itemWidth,
							textTop + heightOffset + itemHeight);
						const CRect clippedItemRect = clipToViewport(itemRect);
						if (!clippedItemRect.IsRectEmpty())
							radar_screen->AddScreenObject(clickItemType, rtCallsign.c_str(), clippedItemRect, true, bottomLine.c_str());
					}

					widthOffset += renderedElement.measuredWidth + blankWidth;
				}
				heightOffset += oneLineHeight;
			}

			POINT toDraw1, toDraw2;
			RECT tagRectData = tagBackgroundRect;
			if (LiangBarsky(tagRectData, targetPoint, tagBackgroundRect.CenterPoint(), toDraw1, toDraw2))
			{
				Gdiplus::Pen leaderPen(symbolWhiteColor);
				gdi->DrawLine(&leaderPen,
					PointF(Gdiplus::REAL(targetPoint.x), Gdiplus::REAL(targetPoint.y)),
					PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));
			}

			if (rimcasLabelOnlySetting)
			{
				const Color aliceBlueColor(255, 240, 248, 255);
				const Color rimcasLabelColor = rimcasStage == CRimcas::StageOne
					? rimcasStageOneColor
					: (rimcasStage == CRimcas::StageTwo ? rimcasStageTwoColor : aliceBlueColor);
				if (rimcasLabelColor.ToCOLORREF() != aliceBlueColor.ToCOLORREF())
				{
					wstring alertText(L"ALERT");
					RectF alertMeasure;
					gdi->MeasureString(alertText.c_str(), wcslen(alertText.c_str()), tagRegularFont, PointF(0, 0), &defaultStringFormat, &alertMeasure);
					const int rimcasHeight = max(1, static_cast<int>(alertMeasure.GetBottom()));
					CRect rimcasLabelRect(tagBackgroundRect.left, tagBackgroundRect.top - rimcasHeight, tagBackgroundRect.right, tagBackgroundRect.top);
					SolidBrush rimcasBrush(rimcasLabelColor);
					gdi->FillRectangle(&rimcasBrush, CopyRect(rimcasLabelRect));
					StringFormat stringFormat;
					stringFormat.SetAlignment(StringAlignment::StringAlignmentCenter);
					SolidBrush alertTextBrushStageOne(Color(255, 30, 30, 30));
					SolidBrush alertTextBrushStageTwo(Color(255, 255, 255, 255));
					SolidBrush* rimcasTextBrush = (rimcasStage == CRimcas::StageTwo)
						? &alertTextBrushStageTwo
						: &alertTextBrushStageOne;
					gdi->DrawString(alertText.c_str(), wcslen(alertText.c_str()), tagRegularFont,
						PointF(Gdiplus::REAL((rimcasLabelRect.left + rimcasLabelRect.right) / 2), Gdiplus::REAL(rimcasLabelRect.top)),
						&stringFormat,
						rimcasTextBrush);
				}
			}
		};

		const VsmrScene::RadarScene* radarScene = radar_screen->GetCurrentRadarScene();
		if (radarScene != nullptr)
		for (const VsmrScene::Target& sceneTarget : radarScene->targets)
		{
			if (!sceneTarget.iconVisible || !sceneTarget.position.valid)
				continue;
			const std::string& rtCallsign = sceneTarget.callsign;
			CPosition targetPosition;
			targetPosition.m_Latitude = sceneTarget.position.latitude;
			targetPosition.m_Longitude = sceneTarget.position.longitude;
			if (!positionNearViewport(targetPosition))
				continue;

			const POINT targetPoint = projectTargetPosition(targetPosition);
			if (!pointInViewport(targetPoint, 180))
				continue;

			if (useNovaIconStyle && sceneTarget.style.showPrimaryReturn && !sceneTarget.primaryReturnPolygon.empty())
			{
				const VsmrScene::Color& primaryColor = sceneTarget.style.primaryReturnColor;
				drawPatatoidePolygon(
					sceneTarget.primaryReturnPolygon,
					Color(primaryColor.alpha, primaryColor.red, primaryColor.green, primaryColor.blue));
			}
			const int iconSize = drawConfiguredIcon(sceneTarget, targetPoint);

			if (mouseWithin(mouseLocation, { targetPoint.x - 5, targetPoint.y - 5, targetPoint.x + 5, targetPoint.y + 5 }))
			{
				CPen* oldPen = dc.SelectObject(&symbolPen);
				dc.MoveTo(targetPoint.x, targetPoint.y - 8);
				dc.LineTo(targetPoint.x - 6, targetPoint.y - 12);
				dc.MoveTo(targetPoint.x, targetPoint.y - 8);
				dc.LineTo(targetPoint.x + 6, targetPoint.y - 12);
				dc.MoveTo(targetPoint.x, targetPoint.y + 8);
				dc.LineTo(targetPoint.x - 6, targetPoint.y + 12);
				dc.MoveTo(targetPoint.x, targetPoint.y + 8);
				dc.LineTo(targetPoint.x + 6, targetPoint.y + 12);
				dc.MoveTo(targetPoint.x - 8, targetPoint.y);
				dc.LineTo(targetPoint.x - 12, targetPoint.y - 6);
				dc.MoveTo(targetPoint.x - 8, targetPoint.y);
				dc.LineTo(targetPoint.x - 12, targetPoint.y + 6);
				dc.MoveTo(targetPoint.x + 8, targetPoint.y);
				dc.LineTo(targetPoint.x + 12, targetPoint.y - 6);
				dc.MoveTo(targetPoint.x + 8, targetPoint.y);
				dc.LineTo(targetPoint.x + 12, targetPoint.y + 6);
				dc.SelectObject(oldPen);
			}

			const int hitSize = max(iconSize, 12);
			CRect targetArea(
				targetPoint.x - hitSize / 2,
				targetPoint.y - hitSize / 2,
				targetPoint.x + hitSize / 2,
				targetPoint.y + hitSize / 2);
			targetArea.NormalizeRect();
			const CRect clippedTargetArea = clipToViewport(targetArea);
			if (!clippedTargetArea.IsRectEmpty())
			{
				radar_screen->AddScreenObject(
					DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE),
					rtCallsign.c_str(),
					clippedTargetArea,
					false,
					sceneTarget.bottomLine.c_str());
			}

			if (sceneTarget.tagVisible)
				drawTag(sceneTarget, targetPoint);
		}

		if (useRealisticIconStyle)
		{
			gdi->SetInterpolationMode(savedInterpolationMode);
			gdi->SetPixelOffsetMode(savedPixelOffsetMode);
			gdi->SetCompositingQuality(savedCompositingQuality);
		}
		gdi->Restore(graphicsState);
		::RestoreDC(hDC, savedDc);
	};

	if (cacheDrawn)
		drawAircraft();

	// Use the AVISO viewport's own pan/zoom/rotation projection.  The external
	// RDF plugin only knows the parent radar transform, which is why its marker
	// cannot be reused for this inset.
	gdi->Flush(Gdiplus::FlushIntentionSync);
	const auto rdfStarted = std::chrono::steady_clock::now();
	VsmrRdf::Draw(
		hDC,
		radar_screen,
		viewportRect,
		[&](const CPosition& position) -> POINT
		{
			const Gdiplus::PointF projected = projectPoint(
				position.m_Longitude,
				position.m_Latitude);
			return {
				static_cast<LONG>(std::lround(static_cast<double>(projected.X))),
				static_cast<LONG>(std::lround(static_cast<double>(projected.Y)))
			};
		});
	m_LastRdfRenderMilliseconds += std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - rdfStarted).count();

	drawChrome();

	dc.Detach();
}

void CInsetWindow::renderWeather(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || radar_screen->IsShutdownRequested())
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect content = GetWindowContentRect();
	content.NormalizeRect();
	if (content.Width() <= 0 || content.Height() <= 0)
	{
		dc.Detach();
		return;
	}

	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	const COLORREF background = RGB(43, 52, 55);
	const COLORREF panel = RGB(41, 54, 57);
	const COLORREF panelHeader = RGB(33, 43, 46);
	const COLORREF panelSoft = RGB(47, 62, 66);
	const COLORREF control = RGB(36, 48, 51);
	const COLORREF outerBorder = RGB(5, 7, 8);
	const COLORREF innerBorder = RGB(17, 23, 25);
	const COLORREF text = RGB(201, 214, 219);
	const COLORREF mutedText = RGB(143, 160, 165);
	const COLORREF cyan = RGB(92, 180, 211);

	dc.FillSolidRect(content, background);
	radar_screen->AddScreenObject(m_Id, "window", content, false, "");

	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	const Gdiplus::GraphicsState graphicsState = gdi->Save();
	gdi->SetClip(CopyRect(content), Gdiplus::CombineModeIntersect);
	gdi->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	gdi->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	const double weatherScale = std::clamp(
		min(static_cast<double>(content.Width()) / 306.0,
			static_cast<double>(content.Height()) / 175.0),
		0.75,
		3.0);
	auto scaledFontHeight = [weatherScale](int pixels) -> int
	{
		return -max(1, static_cast<int>(std::lround(static_cast<double>(pixels) * weatherScale)));
	};
	HFONT labelFont = GetWeatherFont(0, scaledFontHeight(8), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT valueFont = GetWeatherFont(1, scaledFontHeight(11), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT directionFont = GetWeatherFont(2, scaledFontHeight(20), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT speedFont = GetWeatherFont(3, scaledFontHeight(15), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT gustFont = GetWeatherFont(4, scaledFontHeight(11), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT qnhFont = GetWeatherFont(5, scaledFontHeight(17), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT compassFont = GetWeatherFont(6, scaledFontHeight(8), FW_NORMAL, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT rawFont = GetWeatherFont(7, scaledFontHeight(8), FW_NORMAL, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT rawBoldFont = GetWeatherFont(8, scaledFontHeight(8), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HGDIOBJ originalFont = ::GetCurrentObject(hDC, OBJ_FONT);

	auto drawText = [&](const CRect& source, const std::string& value, HFONT font, COLORREF color, UINT flags)
	{
		CRect rect(source);
		if (font != nullptr)
			::SelectObject(hDC, font);
		::SetBkMode(hDC, TRANSPARENT);
		::SetTextColor(hDC, color);
		::DrawTextA(hDC, value.c_str(), -1, &rect, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	};
	auto fillAndBorder = [&](const CRect& rect, COLORREF fill)
	{
		dc.FillSolidRect(rect, fill);
		dc.Draw3dRect(rect, innerBorder, outerBorder);
	};
	auto formatDegrees = [](int degrees) -> std::string
	{
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%03d\xB0", std::clamp(degrees, 0, 360));
		return buffer;
	};
	auto formatVariation = [](int from, int to) -> std::string
	{
		char buffer[20] = {};
		std::snprintf(buffer, sizeof(buffer), "%03d-%03d", std::clamp(from, 0, 360), std::clamp(to, 0, 360));
		return buffer;
	};
	auto windFlowDirection = [](int meteorologicalDirection) -> int
	{
		return (meteorologicalDirection + 180) % 360;
	};
	auto formatObservationTime = [](std::time_t value) -> std::string
	{
		if (value <= 0)
			return "----Z";
		std::tm utc = {};
		if (::gmtime_s(&utc, &value) != 0)
			return "----Z";
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%02d%02dZ", utc.tm_hour, utc.tm_min);
		return buffer;
	};

	const std::string station = VsmrWeather::NormalizeIcao(radar_screen->getActiveAirport());
	VsmrWeather::Snapshot weather;
	const bool hasSnapshot = !station.empty() && VsmrWeather::TryGet(station, weather);
	const bool hasWeather = hasSnapshot && (weather.hasWind || weather.hasQnh);
	const std::time_t now = std::time(nullptr);
	const bool stale = hasWeather && weather.updatedUtc > 0 && now > weather.updatedUtc &&
		(now - weather.updatedUtc) > (90 * 60);
	const VsmrScene::RadarScene* weatherScene = radar_screen->GetCurrentRadarScene();

	struct RunwayReference
	{
		bool valid = false;
		std::string name;
		double trueHeading = 0.0;
		int priority = 3;
		double headwindScore = -1000000.0;
	};
	RunwayReference referenceRunway;
	const auto considerRunway = [&](const std::string& runwayName, double trueHeading, bool headingValid)
	{
		if (!headingValid || runwayName.empty() || weatherScene == nullptr)
			return;
		const auto statusIt = weatherScene->airport.runwayStatuses.find(runwayName);
		if (statusIt == weatherScene->airport.runwayStatuses.end())
			return;
		const CRimcas::RunwayStatus status = static_cast<CRimcas::RunwayStatus>(statusIt->second);
		const int priority = (status == CRimcas::DEP || status == CRimcas::BOTH)
			? 0
			: (status == CRimcas::ARR ? 1 : 3);
		if (priority >= 3)
			return;

		const double headwindScore = weather.hasWind && !weather.windVariable
			? static_cast<double>(weather.windSpeedKnots) *
				std::cos(DegToRad(static_cast<double>(weather.windDirectionDegrees) - trueHeading))
			: 0.0;
		if (!referenceRunway.valid || priority < referenceRunway.priority ||
			(priority == referenceRunway.priority && headwindScore > referenceRunway.headwindScore))
		{
			referenceRunway.valid = true;
			referenceRunway.name = runwayName;
			referenceRunway.trueHeading = trueHeading;
			referenceRunway.priority = priority;
			referenceRunway.headwindScore = headwindScore;
		}
	};
	for (const auto& runway : radar_screen->CachedRunwayGeometries)
	{
		considerRunway(runway.runwayNameA, runway.trueHeadingA, runway.trueHeadingAValid);
		considerRunway(runway.runwayNameB, runway.trueHeadingB, runway.trueHeadingBValid);
	}

	std::string qnhValue = weather.hasQnh ? std::to_string(weather.qnhHpa) : "----";
	std::string variationValue = "---";
	if (weather.hasWindVariation)
		variationValue = formatVariation(weather.windVariationFromDegrees, weather.windVariationToDegrees);
	else if (weather.hasWind && weather.windVariable)
		variationValue = "VRB";

	std::string headValue = "---";
	std::string crossValue = "---";
	if (referenceRunway.valid && weather.hasWind && !weather.windVariable)
	{
		const double delta = DegToRad(
			static_cast<double>(weather.windDirectionDegrees) - referenceRunway.trueHeading);
		const int head = static_cast<int>(std::lround(static_cast<double>(weather.windSpeedKnots) * std::cos(delta)));
		const int cross = static_cast<int>(std::lround(static_cast<double>(weather.windSpeedKnots) * std::sin(delta)));
		headValue = std::string(head >= 0 ? "H" : "T") + std::to_string(std::abs(head));
		crossValue = cross == 0
			? "0"
			: std::to_string(std::abs(cross)) + (cross > 0 ? "R" : "L");
	}

	const int peakWind = weather.hasWindGust
		? max(weather.windSpeedKnots, weather.windGustKnots)
		: weather.windSpeedKnots;
	const COLORREF windColor = peakWind < 10
		? RGB(94, 193, 137)
		: (peakWind < 20
			? RGB(224, 189, 71)
			: (peakWind < 30 ? RGB(230, 135, 55) : RGB(235, 90, 90)));
	std::string directionValue = hasSnapshot ? "NO WIND" : "WAIT";
	std::string speedValue;
	std::string gustValue;
	if (weather.hasWind)
	{
		directionValue = weather.windCalm
			? "CALM"
			: (weather.windVariable ? "VRB" : formatDegrees(weather.windDirectionDegrees));
		speedValue = std::to_string(weather.windSpeedKnots) + " KT";
		if (weather.hasWindGust)
			gustValue = "G" + std::to_string(weather.windGustKnots);
	}

	const int padding = 4;
	const int gap = 4;
	CRect inner(content);
	inner.DeflateRect(padding, padding);
	const bool hasRawReport = hasSnapshot && !weather.rawReport.empty();
	int rawHeight = 0;
	if (hasRawReport && inner.Height() >= 58)
		rawHeight = std::clamp(inner.Height() / 3, 28, 64);
	CRect rawPanel;
	CRect primary(inner);
	if (rawHeight > 0)
	{
		rawPanel = CRect(inner.left, inner.bottom - rawHeight, inner.right, inner.bottom);
		primary.bottom = max(primary.top, rawPanel.top - gap);
	}
	const int primaryWidth = max(1, primary.Width());
	const int primaryHeight = max(1, primary.Height());
	const bool compactLayout = primaryWidth < 165 || primaryHeight < 78;
	const bool stackedLayout = !compactLayout && primaryWidth < 230 && primaryHeight >= 142;
	CRect windPanel;
	CRect statsPanel;
	if (compactLayout)
	{
		windPanel = primary;
	}
	else if (stackedLayout)
	{
		const int availableHeight = max(1, primaryHeight - gap);
		const int maximumWindHeight = max(1, availableHeight - 52);
		const int minimumWindHeight = min(82, maximumWindHeight);
		const int windHeight = std::clamp(
			min(primaryWidth, maximumWindHeight),
			minimumWindHeight,
			maximumWindHeight);
		windPanel = CRect(primary.left, primary.top, primary.right, primary.top + windHeight);
		statsPanel = CRect(primary.left, windPanel.bottom + gap, primary.right, primary.bottom);
	}
	else
	{
		const int minimumStatsWidth = 104;
		const int availableWidth = max(1, primaryWidth - gap);
		const int maximumWindWidth = max(1, availableWidth - minimumStatsWidth);
		const int minimumWindWidth = min(116, maximumWindWidth);
		const int windWidth = std::clamp(
			static_cast<int>(std::lround(static_cast<double>(availableWidth) * 0.55)),
			minimumWindWidth,
			maximumWindWidth);
		windPanel = CRect(primary.left, primary.top, primary.left + windWidth, primary.bottom);
		statsPanel = CRect(windPanel.right + gap, primary.top, primary.right, primary.bottom);
	}
	fillAndBorder(windPanel, panel);
	if (!statsPanel.IsRectEmpty())
		fillAndBorder(statsPanel, panel);
	if (!rawPanel.IsRectEmpty())
		fillAndBorder(rawPanel, control);

	if (!compactLayout)
	{
	CRect roseBounds(
		windPanel.left + 4,
		windPanel.top + 4,
		windPanel.right - 4,
		windPanel.bottom - 4);
	const int roseDiameter = max(20, min(roseBounds.Width(), roseBounds.Height()) - 4);
	const float radius = static_cast<float>(roseDiameter) * 0.5f;
	const float centerX = static_cast<float>(roseBounds.left + roseBounds.Width() / 2);
	const float centerY = static_cast<float>(roseBounds.top + roseBounds.Height() / 2);
	const Gdiplus::RectF roseRect(centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);
	Gdiplus::SolidBrush roseBrush(Gdiplus::Color(255, 36, 48, 51));
	Gdiplus::Pen roseBorder(Gdiplus::Color(255, 17, 23, 25), 1.0f);
	gdi->FillEllipse(&roseBrush, roseRect);
	gdi->DrawEllipse(&roseBorder, roseRect);

	const double pi = 3.14159265358979323846;
	const Gdiplus::Color variationColor(230, 92, 180, 211);
	const float variationOuterRadius = max(2.0f, radius - 3.0f);
	const float variationBandWidth = std::clamp(static_cast<float>(5.0 * weatherScale), 4.0f, 9.0f);
	const float variationInnerRadius = max(1.0f, variationOuterRadius - variationBandWidth);
	const Gdiplus::RectF variationOuterRect(
		centerX - variationOuterRadius,
		centerY - variationOuterRadius,
		variationOuterRadius * 2.0f,
		variationOuterRadius * 2.0f);
	const Gdiplus::RectF variationInnerRect(
		centerX - variationInnerRadius,
		centerY - variationInnerRadius,
		variationInnerRadius * 2.0f,
		variationInnerRadius * 2.0f);
	if (weather.hasWindVariation)
	{
		int sweep = weather.windVariationToDegrees - weather.windVariationFromDegrees;
		if (sweep < 0)
			sweep += 360;
		if (sweep > 0)
		{
			const Gdiplus::REAL startAngle = static_cast<Gdiplus::REAL>(
				windFlowDirection(weather.windVariationFromDegrees) - 90);
			Gdiplus::GraphicsPath variationBand;
			variationBand.AddArc(variationOuterRect, startAngle, static_cast<Gdiplus::REAL>(sweep));
			variationBand.AddArc(
				variationInnerRect,
				startAngle + static_cast<Gdiplus::REAL>(sweep),
				-static_cast<Gdiplus::REAL>(sweep));
			variationBand.CloseFigure();

			Gdiplus::SolidBrush variationFill(Gdiplus::Color(72, 92, 180, 211));
			Gdiplus::Pen variationOutline(Gdiplus::Color(170, 92, 180, 211), 1.0f);
			variationOutline.SetLineJoin(Gdiplus::LineJoinRound);
			gdi->FillPath(&variationFill, &variationBand);
			gdi->DrawPath(&variationOutline, &variationBand);

			Gdiplus::Pen variationEdge(variationColor, 2.0f);
			variationEdge.SetStartCap(Gdiplus::LineCapRound);
			variationEdge.SetEndCap(Gdiplus::LineCapRound);
			gdi->DrawArc(
				&variationEdge,
				variationOuterRect,
				startAngle,
				static_cast<Gdiplus::REAL>(sweep));

			Gdiplus::SolidBrush endpointBrush(variationColor);
			const float endpointRadius = (variationOuterRadius + variationInnerRadius) * 0.5f;
			const float endpointDotRadius = std::clamp(static_cast<float>(1.7 * weatherScale), 1.5f, 3.0f);
			for (int endpoint : { weather.windVariationFromDegrees, weather.windVariationToDegrees })
			{
				const double endpointAngle = (static_cast<double>(windFlowDirection(endpoint)) - 90.0) * pi / 180.0;
				const Gdiplus::PointF endpointPoint(
					centerX + static_cast<float>(std::cos(endpointAngle) * endpointRadius),
					centerY + static_cast<float>(std::sin(endpointAngle) * endpointRadius));
				gdi->FillEllipse(
					&endpointBrush,
					endpointPoint.X - endpointDotRadius,
					endpointPoint.Y - endpointDotRadius,
					endpointDotRadius * 2.0f,
					endpointDotRadius * 2.0f);
			}
		}
	}
	else if (weather.hasWind && weather.windVariable && !weather.windCalm)
	{
		Gdiplus::GraphicsPath variableBand(Gdiplus::FillModeAlternate);
		variableBand.AddEllipse(variationOuterRect);
		variableBand.AddEllipse(variationInnerRect);
		Gdiplus::SolidBrush variableFill(Gdiplus::Color(45, 92, 180, 211));
		gdi->FillPath(&variableFill, &variableBand);

		Gdiplus::Pen variableEdge(variationColor, 1.8f);
		variableEdge.SetDashStyle(Gdiplus::DashStyleDash);
		variableEdge.SetDashCap(Gdiplus::DashCapRound);
		gdi->DrawEllipse(&variableEdge, variationOuterRect);
	}

	Gdiplus::Pen minorTick(Gdiplus::Color(255, 92, 108, 113), 1.0f);
	Gdiplus::Pen majorTick(Gdiplus::Color(255, 151, 169, 174), 1.0f);
	for (int degrees = 0; degrees < 360; degrees += 10)
	{
		const double angle = (static_cast<double>(degrees) - 90.0) * pi / 180.0;
		const float tickLength = degrees % 30 == 0 ? 5.0f : 2.5f;
		const Gdiplus::PointF outer(
			centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f)),
			centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f)));
		const Gdiplus::PointF innerTick(
			centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f - tickLength)),
			centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f - tickLength)));
		gdi->DrawLine(degrees % 30 == 0 ? &majorTick : &minorTick, outer, innerTick);

		if (degrees % 30 == 0 && radius >= 46.0f)
		{
			const float labelRadius = radius - static_cast<float>(11.0 * weatherScale);
			const int labelX = static_cast<int>(std::lround(centerX + std::cos(angle) * labelRadius));
			const int labelY = static_cast<int>(std::lround(centerY + std::sin(angle) * labelRadius));
			const int labelHalfWidth = max(10, static_cast<int>(std::lround(10.0 * weatherScale)));
			const int labelHalfHeight = max(5, static_cast<int>(std::lround(6.0 * weatherScale)));
			CRect labelArea(
				labelX - labelHalfWidth,
				labelY - labelHalfHeight,
				labelX + labelHalfWidth,
				labelY + labelHalfHeight);
			drawText(labelArea, degrees == 0 ? "36" : std::to_string(degrees / 10), compassFont, mutedText, DT_CENTER);
		}
	}

	if (weather.hasWind && !weather.windVariable && !weather.windCalm)
	{
		const double angle = (static_cast<double>(windFlowDirection(weather.windDirectionDegrees)) - 90.0) * pi / 180.0;
		const float lineRadius = max(8.0f, radius - 16.0f);
		const Gdiplus::PointF tip(
			centerX + static_cast<float>(std::cos(angle) * lineRadius),
			centerY + static_cast<float>(std::sin(angle) * lineRadius));
		const Gdiplus::Color needleColor(255, GetRValue(windColor), GetGValue(windColor), GetBValue(windColor));
		Gdiplus::Pen needlePen(needleColor, 2.2f);
		gdi->DrawLine(&needlePen, Gdiplus::PointF(centerX, centerY), tip);
		const double leftAngle = angle + 2.55;
		const double rightAngle = angle - 2.55;
		Gdiplus::PointF arrow[] = {
			tip,
			Gdiplus::PointF(
				tip.X + static_cast<float>(std::cos(leftAngle) * 7.0),
				tip.Y + static_cast<float>(std::sin(leftAngle) * 7.0)),
			Gdiplus::PointF(
				tip.X + static_cast<float>(std::cos(rightAngle) * 7.0),
				tip.Y + static_cast<float>(std::sin(rightAngle) * 7.0))
		};
		Gdiplus::SolidBrush arrowBrush(needleColor);
		gdi->FillPolygon(&arrowBrush, arrow, static_cast<INT>(_countof(arrow)));
	}

	const float centerTextHalfWidth = min(radius - 8.0f, static_cast<float>(42.0 * weatherScale));
	const float centerTextHalfHeight = min(radius - 8.0f, static_cast<float>(29.0 * weatherScale));
	Gdiplus::SolidBrush centerPlate(Gdiplus::Color(225, 36, 48, 51));
	gdi->FillRectangle(
		&centerPlate,
		centerX - centerTextHalfWidth,
		centerY - centerTextHalfHeight,
		centerTextHalfWidth * 2.0f,
		centerTextHalfHeight * 2.0f);

	CRect directionArea(
		static_cast<int>(centerX - centerTextHalfWidth),
		static_cast<int>(centerY - 25.0 * weatherScale),
		static_cast<int>(centerX + centerTextHalfWidth),
		static_cast<int>(centerY - 3.0 * weatherScale));
	CRect speedArea(
		directionArea.left,
		directionArea.bottom,
		directionArea.right,
		static_cast<int>(centerY + 16.0 * weatherScale));
	CRect gustArea(
		directionArea.left,
		speedArea.bottom - 1,
		directionArea.right,
		static_cast<int>(centerY + 29.0 * weatherScale));
	drawText(directionArea, directionValue, radius >= 35.0f ? directionFont : valueFont, weather.hasWind ? windColor : mutedText, DT_CENTER);
	if (radius >= 35.0f)
		drawText(speedArea, speedValue, speedFont, text, DT_CENTER);
	if (radius >= 45.0f)
		drawText(gustArea, gustValue, gustFont, RGB(245, 214, 122), DT_CENTER);

	std::string footer;
	if (!hasWeather)
	{
		footer = hasSnapshot ? "NO CURRENT WEATHER" : "WAITING FOR METAR";
	}
	else
	{
		if (stale)
			footer = "STALE";
		else if (footer.empty() && referenceRunway.valid)
			footer = formatObservationTime(weather.observationUtc) + "  RWY " + referenceRunway.name;
		else if (footer.empty())
			footer = formatObservationTime(weather.observationUtc) + "  NO ACTIVE RWY";
	}
	CRect footerArea(windPanel.left + 4, windPanel.bottom - max(12, static_cast<int>(14.0 * weatherScale)), windPanel.right - 4, windPanel.bottom - 2);
	drawText(footerArea, footer, labelFont, stale ? RGB(230, 135, 55) : mutedText, DT_CENTER);
	}
	else
	{
		CRect compactTop(windPanel);
		compactTop.DeflateRect(5, 3);
		const int split = compactTop.top + max(18, compactTop.Height() / 2);
		CRect windLine(compactTop.left, compactTop.top, compactTop.right, min(compactTop.bottom, split));
		CRect detailLine(compactTop.left, windLine.bottom, compactTop.right, compactTop.bottom);
		std::string windSummary = directionValue;
		if (!speedValue.empty())
			windSummary += "  " + speedValue;
		if (!gustValue.empty())
			windSummary += " " + gustValue;
		drawText(windLine, windSummary, valueFont, weather.hasWind ? windColor : mutedText, DT_LEFT);
		std::string detailSummary = "QNH " + qnhValue;
		if (weather.hasWindVariation)
			detailSummary += "  VAR " + variationValue;
		if (referenceRunway.valid)
			detailSummary += "  " + headValue + "/" + crossValue;
		drawText(detailLine, detailSummary, labelFont, text, DT_LEFT);
	}

	const struct StatCell
	{
		const char* label;
		std::string value;
	} cells[] = {
		{ "QNH", qnhValue },
		{ "VAR", variationValue },
		{ "HEAD", headValue },
		{ "XWIND", crossValue }
	};
	if (!statsPanel.IsRectEmpty())
	{
		const int statsWidth = max(1, statsPanel.Width());
		const int statsHeight = max(1, statsPanel.Height());
		const int firstColumnWidth = statsWidth / 2;
		for (int row = 0; row < 2; ++row)
		{
			const int rowTop = statsPanel.top + (statsHeight * row) / 2;
			const int rowBottom = statsPanel.top + (statsHeight * (row + 1)) / 2;
			for (int column = 0; column < 2; ++column)
			{
				const int index = row * 2 + column;
				const int left = column == 0 ? statsPanel.left : statsPanel.left + firstColumnWidth;
				const int right = column == 0 ? statsPanel.left + firstColumnWidth : statsPanel.right;
				CRect cell(left, rowTop, right, rowBottom);
				dc.FillSolidRect(cell, panelSoft);
				dc.Draw3dRect(cell, innerBorder, outerBorder);
				const int desiredHeaderHeight = max(
					10,
					static_cast<int>(std::lround(14.0 * weatherScale)));
				const int headerHeight = min(desiredHeaderHeight, max(9, cell.Height() / 3));
				CRect header(cell.left + 1, cell.top + 1, cell.right - 1, min(cell.bottom - 1, cell.top + headerHeight));
				dc.FillSolidRect(header, panelHeader);
				CRect labelArea(header.left + 3, header.top, header.right - 2, header.bottom);
				CRect valueArea(cell.left + 2, header.bottom, cell.right - 2, cell.bottom - 1);
				drawText(labelArea, cells[index].label, labelFont, mutedText, DT_LEFT);
				drawText(valueArea, cells[index].value, index == 0 ? qnhFont : valueFont, text, DT_CENTER);
			}
		}
	}

	if (!rawPanel.IsRectEmpty() && hasRawReport)
	{
		auto allDigits = [](const std::string& token, size_t first, size_t count) -> bool
		{
			if (first > token.size() || count > token.size() - first)
				return false;
			for (size_t index = first; index < first + count; ++index)
			{
				if (std::isdigit(static_cast<unsigned char>(token[index])) == 0)
					return false;
			}
			return true;
		};
		auto beginsWith = [](const std::string& token, const char* prefix) -> bool
		{
			const size_t length = std::strlen(prefix);
			return token.size() >= length && token.compare(0, length, prefix) == 0;
		};
		auto containsWeatherCode = [](const std::string& token) -> bool
		{
			static const char* codes[] = {
				"RA", "SN", "DZ", "TS", "FG", "BR", "HZ", "SH", "FZ", "GR", "GS", "SQ"
			};
			for (const char* code : codes)
			{
				if (token.find(code) != std::string::npos)
					return true;
			}
			return false;
		};

		CRect rawBounds(rawPanel);
		rawBounds.DeflateRect(4, 3);
		::SetBkMode(hDC, TRANSPARENT);
		int cursorX = rawBounds.left;
		int cursorY = rawBounds.top;
		const int lineHeight = max(11, static_cast<int>(std::lround(12.0 * weatherScale)));
		std::istringstream report(weather.rawReport);
		std::string token;
		size_t tokenIndex = 0;
		while (report >> token)
		{
			const bool qnhToken = token.size() == 5 &&
				(token[0] == 'Q' || token[0] == 'A') && allDigits(token, 1, 4);
			const bool windToken =
				(token.size() >= 7 && token.compare(token.size() - 2, 2, "KT") == 0) ||
				(token.size() >= 8 && token.compare(token.size() - 3, 3, "MPS") == 0) ||
				(token.size() == 7 && token[3] == 'V' && allDigits(token, 0, 3) && allDigits(token, 4, 3));
			const bool visibilityToken = token == "CAVOK" ||
				(token.size() == 4 && allDigits(token, 0, 4));
			const bool cloudToken = beginsWith(token, "FEW") || beginsWith(token, "SCT") ||
				beginsWith(token, "BKN") || beginsWith(token, "OVC") || beginsWith(token, "VV");
			const size_t temperatureSeparator = token.find('/');
			const bool temperatureToken = temperatureSeparator != std::string::npos &&
				temperatureSeparator > 0 && temperatureSeparator + 1 < token.size() &&
				token.size() <= 7;
			const bool weatherToken = tokenIndex > 2 && !windToken && !cloudToken &&
				!temperatureToken && containsWeatherCode(token);
			COLORREF tokenColor = text;
			COLORREF tokenFill = control;
			bool highlighted = false;
			if (qnhToken)
			{
				tokenColor = RGB(245, 214, 122);
				tokenFill = RGB(67, 58, 35);
				highlighted = true;
			}
			else if (windToken)
			{
				tokenColor = windColor;
				tokenFill = RGB(40, 58, 61);
				highlighted = true;
			}
			else if (visibilityToken)
			{
				tokenColor = token == "CAVOK" || std::atoi(token.c_str()) >= 5000
					? RGB(116, 207, 151)
					: (std::atoi(token.c_str()) >= 1500 ? RGB(245, 214, 122) : RGB(241, 116, 116));
				tokenFill = RGB(38, 59, 52);
				highlighted = true;
			}
			else if (weatherToken)
			{
				tokenColor = RGB(241, 164, 92);
				tokenFill = RGB(64, 47, 35);
				highlighted = true;
			}
			else if (cloudToken)
			{
				tokenColor = cyan;
			}
			else if (temperatureToken)
			{
				tokenColor = RGB(205, 190, 230);
			}

			HFONT tokenFont = highlighted ? rawBoldFont : rawFont;
			if (tokenFont != nullptr)
				::SelectObject(hDC, tokenFont);
			SIZE tokenSize = {};
			::GetTextExtentPoint32A(hDC, token.c_str(), static_cast<int>(token.size()), &tokenSize);
			const int tokenWidth = max(1, tokenSize.cx);
			if (cursorX > rawBounds.left && cursorX + tokenWidth > rawBounds.right)
			{
				cursorX = rawBounds.left;
				cursorY += lineHeight;
			}
			if (cursorY + lineHeight > rawBounds.bottom)
				break;
			if (highlighted)
				dc.FillSolidRect(CRect(cursorX - 1, cursorY, min(rawBounds.right, cursorX + tokenWidth + 2), cursorY + lineHeight), tokenFill);
			::SetTextColor(hDC, tokenColor);
			::TextOutA(hDC, cursorX, cursorY + max(0, (lineHeight - tokenSize.cy) / 2), token.c_str(), static_cast<int>(token.size()));
			SIZE spaceSize = {};
			::GetTextExtentPoint32A(hDC, " ", 1, &spaceSize);
			cursorX += tokenWidth + max(3, spaceSize.cx);
			++tokenIndex;
		}
	}

	if (originalFont != nullptr)
		::SelectObject(hDC, originalFont);

	gdi->Restore(graphicsState);
	if (savedDc != 0)
		::RestoreDC(hDC, savedDc);

	CBrush frameBrush(outerBorder);
	dc.FrameRect(content, &frameBrush);
	DrawInsetWindowChrome(
		dc,
		radar_screen,
		m_Id,
		m_AvisoLayoutMode,
		m_Area,
		"Metar",
		false,
		mouseLocation,
		true,
		&m_LastChromeRenderMilliseconds);

	dc.Detach();
}

void CInsetWindow::renderTimer(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || radar_screen->IsShutdownRequested())
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect content = GetWindowContentRect();
	content.NormalizeRect();
	if (content.Width() <= 0 || content.Height() <= 0)
	{
		dc.Detach();
		return;
	}

	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	const COLORREF outerBorder = RGB(5, 7, 8);
	const COLORREF innerBorder = RGB(82, 96, 101);
	const COLORREF idleFill = RGB(36, 48, 51);
	const COLORREF hoverFill = RGB(48, 64, 68);
	const COLORREF runningFill = RGB(38, 79, 91);
	const COLORREF expiredFill = RGB(92, 42, 42);
	const COLORREF idleText = RGB(208, 217, 220);
	const COLORREF runningText = RGB(115, 216, 229);
	const COLORREF expiredText = RGB(255, 167, 157);

	dc.FillSolidRect(content, idleFill);
	radar_screen->AddScreenObject(m_Id, "window", content, false, "Timer");
	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	HFONT timerFont = GetTimerFont();
	HGDIOBJ originalFont = timerFont != nullptr ? ::SelectObject(hDC, timerFont) : nullptr;
	const int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
	const unsigned long long now = ::GetTickCount64();

	const int timerCount = static_cast<int>(m_TimerDeadlineTicks.size());
	for (int durationMinutes = 1; durationMinutes <= timerCount; ++durationMinutes)
	{
		const int index = durationMinutes - 1;
		const int column = index % kTimerColumnCount;
		const int row = index / kTimerColumnCount;
		CRect cell(
			content.left + (content.Width() * column) / kTimerColumnCount,
			content.top + (content.Height() * row) / kTimerRowCount,
			content.left + (content.Width() * (column + 1)) / kTimerColumnCount,
			content.top + (content.Height() * (row + 1)) / kTimerRowCount);
		const int remainingSeconds = GetTimerRemainingSeconds(durationMinutes, now);
		const bool running = m_TimerDeadlineTicks[static_cast<size_t>(index)] != 0;
		const bool expired = m_TimerExpired[static_cast<size_t>(index)];
		COLORREF fill = running ? runningFill : (expired ? expiredFill : idleFill);
		if (!running && !expired && cell.PtInRect(mouseLocation))
			fill = hoverFill;
		dc.FillSolidRect(cell, fill);
		dc.Draw3dRect(cell, innerBorder, outerBorder);

		char label[16] = {};
		if (running)
		{
			std::snprintf(label, sizeof(label), "%d:%02d", remainingSeconds / 60, remainingSeconds % 60);
		}
		else if (expired)
		{
			std::snprintf(label, sizeof(label), "0:00");
		}
		else
		{
			std::snprintf(label, sizeof(label), "%dM", durationMinutes);
		}
		::SetTextColor(hDC, running ? runningText : (expired ? expiredText : idleText));
		CRect textRect(cell);
		::DrawTextA(hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		const std::string objectId = "timer." + std::to_string(durationMinutes) + "m";
		radar_screen->AddScreenObject(
			m_Id,
			objectId.c_str(),
			cell,
			false,
			"Left click to start; right click to reset");
	}

	::SetBkMode(hDC, oldBkMode);
	if (originalFont != nullptr)
		::SelectObject(hDC, originalFont);
	if (savedDc != 0)
		::RestoreDC(hDC, savedDc);

	CBrush frameBrush(outerBorder);
	dc.FrameRect(content, &frameBrush);
	DrawInsetWindowChrome(
		dc,
		radar_screen,
		m_Id,
		AvisoLayoutMode::Floating,
		m_Area,
		"Timer",
		false,
		mouseLocation,
		false,
		&m_LastChromeRenderMilliseconds);

	dc.Detach();
}

void CInsetWindow::render(HDC hDC, CSMRRadar * radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	m_LastRdfRenderMilliseconds = 0.0;
	m_LastChromeRenderMilliseconds = 0.0;
	if (this->m_Id == -1)
		return;

	const char* insetName = "SRW1";
	if (IsAvisoViewport())
		insetName = "AVISO";
	else if (IsWeather())
		insetName = "WEATHER";
	else if (IsTimer())
		insetName = "TIMER";
	if (radar_screen != nullptr)
		radar_screen->PublishCrashRadarState("inset", insetName);
	VsmrCrashReporter::RecordBreadcrumb("inset render", insetName);
	struct RestoreCrashRadarState
	{
		CSMRRadar* radar = nullptr;
		~RestoreCrashRadarState()
		{
			if (radar != nullptr)
				radar->PublishCrashRadarState("main");
		}
	} restoreCrashRadarState{ radar_screen };

	if (IsAvisoViewport())
	{
		renderAvisoViewport(hDC, radar_screen, gdi, mouseLocation);
		return;
	}
	if (IsWeather())
	{
		renderWeather(hDC, radar_screen, gdi, mouseLocation);
		return;
	}
	if (IsTimer())
	{
		renderTimer(hDC, radar_screen, gdi, mouseLocation);
		return;
	}
	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);
	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	icao = radar_screen->getActiveAirport();
	m_AirportPositionValid = radar_screen->TryGetActiveAirportPosition(m_AirportPosition);
	m_TargetPoints.clear();
	m_TagAreas.clear();

	const Value& activeProfile = radar_screen->CurrentConfig->getActiveProfile();
	const auto getProfileObjectSection = [&](const char* key) -> const Value*
	{
		if (!activeProfile.IsObject() || !activeProfile.HasMember(key) || !activeProfile[key].IsObject())
			return nullptr;
		return &activeProfile[key];
	};
	const auto getSectionInt = [&](const Value* section, const char* key, int fallback) -> int
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsInt())
			return (*section)[key].GetInt();
		return fallback;
	};
	const auto getSectionDouble = [&](const Value* section, const char* key, double fallback) -> double
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsNumber())
			return (*section)[key].GetDouble();
		return fallback;
	};
	const auto getSectionBool = [&](const Value* section, const char* key, bool fallback) -> bool
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsBool())
			return (*section)[key].GetBool();
		return fallback;
	};
	const auto getSectionColorRef = [&](const Value* section, const char* key, const COLORREF fallback) -> COLORREF
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
			return radar_screen->CurrentConfig->getConfigColorRef((*section)[key]);
		return fallback;
	};
	const auto getSectionColor = [&](const Value* section, const char* key, const Color& fallback) -> Color
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
			return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
		return fallback;
	};

	const Value* srwInsetSection = getProfileObjectSection("approach_insets");
	const Value* filterSection = getProfileObjectSection("filters");
	const Value* rimcasSection = getProfileObjectSection("rimcas");

	const COLORREF qBackgroundColor = getSectionColorRef(srwInsetSection, "background_color", RGB(30, 30, 30));
	const COLORREF srwRunwayColor = getSectionColorRef(srwInsetSection, "runway_color", RGB(255, 255, 255));
	const COLORREF srwExtendedLineColor = getSectionColorRef(srwInsetSection, "extended_lines_color", RGB(180, 180, 180));
	const double srwExtendedLineLengthNm = max(0.1, getSectionDouble(srwInsetSection, "extended_lines_length", 15.0));
	const int srwExtendedLineTickSpacingNm = max(1, getSectionInt(srwInsetSection, "extended_lines_ticks_spacing", 1));
	const int radarRangeNm = max(1, getSectionInt(filterSection, "radar_range_nm", 999));
	const bool rimcasLabelOnlySetting = getSectionBool(rimcasSection, "rimcas_label_only", true);
	const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
	const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));

	CRect windowAreaCRect = GetWindowContentRect();
	windowAreaCRect.NormalizeRect();

	// We create the radar
	dc.FillSolidRect(windowAreaCRect, qBackgroundColor);
	radar_screen->AddScreenObject(m_Id, "window", windowAreaCRect, false, "");

	// Keep every SRW drawing primitive and hit target inside the usable content
	// rectangle.  This matters when a docked divider is resized through a tag or
	// target: the title bar and neighbouring radar area must remain unobstructed.
	const int srwSavedDc = ::SaveDC(hDC);
	if (srwSavedDc != 0)
		::IntersectClipRect(hDC, windowAreaCRect.left, windowAreaCRect.top, windowAreaCRect.right, windowAreaCRect.bottom);
	const Gdiplus::GraphicsState srwGraphicsState = gdi->Save();
	gdi->SetClip(CopyRect(windowAreaCRect), Gdiplus::CombineModeIntersect);
	const auto clipToWindowContent = [&](CRect rect) -> CRect
	{
		rect.NormalizeRect();
		CRect clipped;
		clipped.IntersectRect(rect, windowAreaCRect);
		return clipped;
	};

	auto scale = m_Scale;

	POINT refPt = windowAreaCRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	// Here we draw all runways for the airport
	CPen runwayPen(PS_SOLID, 1, srwRunwayColor);
	CPen extendedCentreLinePen(PS_SOLID, 1, srwExtendedLineColor);
	CSectorElement rwy;
	for (rwy = radar_screen->GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = radar_screen->GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{

		if (startsWith(icao.c_str(), rwy.GetAirportName()))
		{

			CPen* oldPen = dc.SelectObject(&runwayPen);

			CPosition EndOne, EndTwo;
			rwy.GetPosition(&EndOne, 0);
			rwy.GetPosition(&EndTwo, 1);

			POINT Pt1, Pt2;
			Pt1 = projectPoint(EndOne);
			Pt2 = projectPoint(EndTwo);

			POINT toDraw1, toDraw2;
			if (LiangBarsky(windowAreaCRect, Pt1, Pt2, toDraw1, toDraw2)) {
				dc.MoveTo(toDraw1);
				dc.LineTo(toDraw2);

			}

			if (rwy.IsElementActive(false, 0) || rwy.IsElementActive(false, 1))
			{
				CPosition Threshold, OtherEnd;
				if (rwy.IsElementActive(false, 0))
				{
					Threshold = EndOne; 
					OtherEnd = EndTwo;
				} else
				{
					Threshold = EndTwo; 
					OtherEnd = EndOne;
				}
					

				double reverseHeading = RadToDeg(TrueBearing(OtherEnd, Threshold));
				double length = srwExtendedLineLengthNm * 1852.0;

				// Drawing the extended centreline
				CPosition endExtended = BetterHarversine(Threshold, reverseHeading, length);

				Pt1 = projectPoint(Threshold);
				Pt2 = projectPoint(endExtended);

				if (LiangBarsky(windowAreaCRect, Pt1, Pt2, toDraw1, toDraw2)) {
					dc.SelectObject(&extendedCentreLinePen);
					dc.MoveTo(toDraw1);
					dc.LineTo(toDraw2);
				}

				// Drawing the ticks
				int increment = srwExtendedLineTickSpacingNm * 1852;

				for (int j = increment; j <= int(srwExtendedLineLengthNm * 1852); j += increment) {

					CPosition tickPosition = BetterHarversine(Threshold, reverseHeading, j);
					CPosition tickBottom = BetterHarversine(tickPosition, fmod(reverseHeading - 90, 360), 500);
					CPosition tickTop = BetterHarversine(tickPosition, fmod(reverseHeading + 90, 360), 500);


					Pt1 = projectPoint(tickBottom);
					Pt2 = projectPoint(tickTop);

					if (LiangBarsky(windowAreaCRect, Pt1, Pt2, toDraw1, toDraw2)) {
						dc.SelectObject(&extendedCentreLinePen);
						dc.MoveTo(toDraw1);
						dc.LineTo(toDraw2);
					}

				}
			} 

			dc.SelectObject(oldPen);
		}
	}

	// Aircrafts

	CPen WhitePen(PS_SOLID, 1, RGB(255, 255, 255));

	auto fontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
	Gdiplus::Font* tagRegularFont = (fontIt != radar_screen->customFonts.end()) ? fontIt->second.get() : nullptr;
	Gdiplus::Font* tagBoldFont = tagRegularFont;
	int blankWidth = m_SrwBlankWidth;
	int oneLineHeight = m_SrwLineHeight;
	Gdiplus::StringFormat defaultStringFormat;
	const Color whiteColor(255, 255, 255, 255);
	const Color aliceBlueColor(255, 240, 248, 255);
	if (tagRegularFont != nullptr)
	{
		Gdiplus::FontFamily baseFamily;
		WCHAR familyName[LF_FACESIZE] = {};
		const bool familyAvailable =
			tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok &&
			baseFamily.GetFamilyName(familyName) == Gdiplus::Ok;
		const std::wstring currentFamily = familyAvailable ? familyName : L"";
		const Gdiplus::REAL currentSize = tagRegularFont->GetSize();
		const INT currentStyle = tagRegularFont->GetStyle();
		const bool fontChanged =
			m_SrwFontSource != tagRegularFont ||
			std::abs(static_cast<double>(m_SrwFontSize - currentSize)) > 0.001 ||
			m_SrwFontStyle != currentStyle ||
			m_SrwFontFamily != currentFamily;
		if (fontChanged)
		{
			m_SrwFontSource = tagRegularFont;
			m_SrwFontSize = currentSize;
			m_SrwFontStyle = currentStyle;
			m_SrwFontFamily = currentFamily;
			m_SrwBoldFont.reset();
			if (familyAvailable)
			{
				const INT boldStyle = currentStyle | Gdiplus::FontStyleBold;
				m_SrwBoldFont = std::make_unique<Gdiplus::Font>(
					&baseFamily,
					currentSize,
					boldStyle,
					Gdiplus::UnitPixel);
				if (m_SrwBoldFont->GetLastStatus() != Gdiplus::Ok)
					m_SrwBoldFont.reset();
			}

			Gdiplus::Font* metricBoldFont = m_SrwBoldFont != nullptr
				? m_SrwBoldFont.get()
				: tagRegularFont;
			RectF measureRect;
			gdi->MeasureString(L" ", 1, tagRegularFont, PointF(0, 0), &defaultStringFormat, &measureRect);
			m_SrwBlankWidth = static_cast<int>(measureRect.GetRight());
			measureRect = RectF(0, 0, 0, 0);
			static const wchar_t kMetricSample[] = L"AZERTYUIOPQSDFGHJKLMWXCVBN";
			gdi->MeasureString(kMetricSample, _countof(kMetricSample) - 1,
				tagRegularFont, PointF(0, 0), &defaultStringFormat, &measureRect);
			m_SrwLineHeight = static_cast<int>(measureRect.GetBottom());
			if (metricBoldFont != tagRegularFont)
			{
				RectF boldMeasureRect;
				gdi->MeasureString(kMetricSample, _countof(kMetricSample) - 1,
					metricBoldFont, PointF(0, 0), &defaultStringFormat, &boldMeasureRect);
				m_SrwLineHeight = max(
					m_SrwLineHeight,
					static_cast<int>(boldMeasureRect.GetBottom()));
			}
		}
		tagBoldFont = m_SrwBoldFont != nullptr ? m_SrwBoldFont.get() : tagRegularFont;
		blankWidth = m_SrwBlankWidth;
		oneLineHeight = m_SrwLineHeight;
	}

	const VsmrScene::RadarScene* radarScene = radar_screen->GetCurrentRadarScene();
	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& targetPresentation = radarScene != nullptr
		? radarScene->targetPresentation
		: defaultTargetPresentation;
	auto drawSceneTargetSymbol = [&](const VsmrScene::Target& target, const POINT& center) -> int
	{
		const VsmrScene::Color& sceneColor = target.style.color;
		const Color drawColor(sceneColor.alpha, sceneColor.red, sceneColor.green, sceneColor.blue);
		CPosition headingPosition;
		headingPosition.m_Latitude = target.headingProbe.latitude;
		headingPosition.m_Longitude = target.headingProbe.longitude;
		const POINT headingPoint = target.headingProbe.valid ? projectPoint(headingPosition) : POINT{ center.x, center.y - 10 };
		double forwardX = static_cast<double>(headingPoint.x - center.x);
		double forwardY = static_cast<double>(headingPoint.y - center.y);
		double forwardLength = std::hypot(forwardX, forwardY);
		if (!std::isfinite(forwardLength) || forwardLength < 0.01)
		{
			forwardX = 0.0;
			forwardY = -1.0;
			forwardLength = 1.0;
		}
		forwardX /= forwardLength;
		forwardY /= forwardLength;
		const double rightX = -forwardY;
		const double rightY = forwardX;

		if (target.style.icon == VsmrScene::IconStyle::Nova)
		{
			if (target.style.showPrimaryReturn && target.primaryReturnPolygon.size() >= 3)
			{
				std::vector<PointF> outline;
				outline.reserve(target.primaryReturnPolygon.size());
				for (const VsmrScene::GeoPoint& source : target.primaryReturnPolygon)
				{
					if (!source.valid)
						continue;
					CPosition sourcePosition;
					sourcePosition.m_Latitude = source.latitude;
					sourcePosition.m_Longitude = source.longitude;
					const POINT point = projectPoint(sourcePosition);
					outline.emplace_back(static_cast<REAL>(point.x), static_cast<REAL>(point.y));
				}
				if (outline.size() >= 3)
				{
					const VsmrScene::Color& primary = target.style.primaryReturnColor;
					SolidBrush primaryBrush(Color(primary.alpha, primary.red, primary.green, primary.blue));
					gdi->FillPolygon(&primaryBrush, outline.data(), static_cast<INT>(outline.size()));
				}
			}
			Pen symbolPen(drawColor, 1.0f);
			if (target.transponderModeC)
			{
				PointF points[] = {
					PointF(static_cast<REAL>(center.x), static_cast<REAL>(center.y - 5)),
					PointF(static_cast<REAL>(center.x - 5), static_cast<REAL>(center.y)),
					PointF(static_cast<REAL>(center.x), static_cast<REAL>(center.y + 5)),
					PointF(static_cast<REAL>(center.x + 5), static_cast<REAL>(center.y)),
					PointF(static_cast<REAL>(center.x), static_cast<REAL>(center.y - 5))
				};
				gdi->DrawLines(&symbolPen, points, static_cast<INT>(_countof(points)));
			}
			else
			{
				gdi->DrawLine(&symbolPen, center.x, center.y, center.x - 4, center.y - 4);
				gdi->DrawLine(&symbolPen, center.x, center.y, center.x + 4, center.y - 4);
				gdi->DrawLine(&symbolPen, center.x, center.y, center.x - 4, center.y + 4);
				gdi->DrawLine(&symbolPen, center.x, center.y, center.x + 4, center.y + 4);
			}
			return 12;
		}

		if (target.style.icon == VsmrScene::IconStyle::Realistic)
		{
			Gdiplus::Bitmap* sourceBitmap = radar_screen->GetAircraftIcon(target.style.assetKey);
			if (sourceBitmap != nullptr && sourceBitmap->GetLastStatus() == Gdiplus::Ok &&
				sourceBitmap->GetWidth() > 0 && sourceBitmap->GetHeight() > 0)
			{
				const double pixelsPerMeter = max(0.0, forwardLength / 50.0);
				double drawWidth = target.style.wingspanMeters * pixelsPerMeter;
				double drawHeight = target.style.lengthMeters * pixelsPerMeter;
				if (targetPresentation.fixedPixelSize)
				{
					const double configuredFactor = targetPresentation.smallIconBoostEnabled
						? targetPresentation.smallIconBoostFactor : 1.0;
					const double fixedPixelsPerMeter = (18.0 * targetPresentation.resolutionScale) / 40.0;
					drawWidth = target.style.wingspanMeters * fixedPixelsPerMeter * configuredFactor;
					drawHeight = target.style.lengthMeters * fixedPixelsPerMeter * configuredFactor;
				}
				else if (targetPresentation.smallIconBoostEnabled && pixelsPerMeter > 0.0)
				{
					const double referenceScreenSize = 40.0 * pixelsPerMeter;
					const double boostStartSize = 14.0 * targetPresentation.resolutionScale;
					if (referenceScreenSize < boostStartSize)
					{
						const double boostedReferenceSize = 18.0 * targetPresentation.smallIconBoostFactor * targetPresentation.resolutionScale;
						const double boostScale = std::clamp(
							boostedReferenceSize / max(0.01, referenceScreenSize),
							1.0,
							6.0 * targetPresentation.smallIconBoostFactor * targetPresentation.resolutionScale);
						drawWidth *= boostScale;
						drawHeight *= boostScale;
					}
				}
				drawWidth = std::clamp(drawWidth, 4.0, 1200.0);
				drawHeight = std::clamp(drawHeight, 4.0, 1200.0);
				int pixelWidth = 0;
				int pixelHeight = 0;
				std::string cacheKey;
				Gdiplus::Bitmap* scaled = radar_screen->GetCachedRealisticIconBitmap(
					target.style.assetKey,
					sourceBitmap,
					sourceBitmap->GetWidth(),
					sourceBitmap->GetHeight(),
					true,
					drawColor,
					drawWidth,
					drawHeight,
					radar_screen->RealisticIconCacheFrame,
					pixelWidth,
					pixelHeight,
					cacheKey);
				if (scaled == nullptr)
				{
					pixelWidth = std::clamp(static_cast<int>(std::lround(drawWidth)), 1, 2048);
					pixelHeight = std::clamp(static_cast<int>(std::lround(drawHeight)), 1, 2048);
				}
				const double rotationDegrees = std::atan2(forwardY, forwardX) * 180.0 / 3.14159265358979323846 + 90.0;
				CSMRRadar::RealisticIconCacheEntry* rotated = radar_screen->GetCachedRotatedRealisticIconBitmap(
					cacheKey,
					scaled,
					pixelWidth,
					pixelHeight,
					rotationDegrees,
					radar_screen->RealisticIconCacheFrame);
				if (rotated != nullptr && rotated->bitmap != nullptr)
				{
					gdi->DrawImage(rotated->bitmap.get(), center.x - rotated->centerX, center.y - rotated->centerY);
					return max(
						static_cast<int>(rotated->bitmap->GetWidth()),
						static_cast<int>(rotated->bitmap->GetHeight()));
				}

				GraphicsState state = gdi->Save();
				Gdiplus::Matrix matrix;
				matrix.Translate(Gdiplus::REAL(center.x), Gdiplus::REAL(center.y));
				matrix.Rotate(Gdiplus::REAL(rotationDegrees));
				matrix.Translate(Gdiplus::REAL(-pixelWidth / 2.0), Gdiplus::REAL(-pixelHeight / 2.0));
				gdi->SetTransform(&matrix);
				if (scaled != nullptr)
					gdi->DrawImage(scaled, 0, 0);
				else
					gdi->DrawImage(sourceBitmap, Gdiplus::REAL(0), Gdiplus::REAL(0), Gdiplus::REAL(pixelWidth), Gdiplus::REAL(pixelHeight));
				gdi->Restore(state);

				const double rotationRadians = rotationDegrees * 3.14159265358979323846 / 180.0;
				const double absCos = std::abs(std::cos(rotationRadians));
				const double absSin = std::abs(std::sin(rotationRadians));
				const int rotatedWidth = static_cast<int>(std::ceil(pixelWidth * absCos + pixelHeight * absSin));
				const int rotatedHeight = static_cast<int>(std::ceil(pixelWidth * absSin + pixelHeight * absCos));
				return max(rotatedWidth, rotatedHeight);
			}
		}

		const double pixelsPerMeter = max(0.0, forwardLength / 50.0);
		double lengthPixels = 20.0;
		double halfWidthPixels = 12.0;
		if (targetPresentation.fixedPixelSize)
		{
			const double configuredFactor = targetPresentation.smallIconBoostEnabled
				? targetPresentation.smallIconBoostFactor : 1.0;
			const double fixedScale = configuredFactor * targetPresentation.resolutionScale;
			lengthPixels = std::clamp(lengthPixels * fixedScale, 6.0, 160.0);
			halfWidthPixels = std::clamp(halfWidthPixels * fixedScale, 3.0, 80.0);
		}
		else if (pixelsPerMeter > 0.0)
		{
			lengthPixels = std::clamp(20.0 * pixelsPerMeter, 6.0, 120.0);
			halfWidthPixels = std::clamp(12.0 * pixelsPerMeter, 3.0, 60.0);
			if (targetPresentation.smallIconBoostEnabled)
			{
				const double currentExtent = lengthPixels + halfWidthPixels;
				const double minimumExtent = 14.0 * targetPresentation.smallIconBoostFactor * targetPresentation.resolutionScale;
				const double boostScale = std::clamp(
					minimumExtent / max(0.01, currentExtent),
					1.0,
					2.0 * targetPresentation.smallIconBoostFactor * targetPresentation.resolutionScale);
				lengthPixels *= boostScale;
				halfWidthPixels *= boostScale;
			}
		}
		lengthPixels = std::clamp(lengthPixels * targetPresentation.fixedTriangleScale, 1.0, 220.0);
		halfWidthPixels = std::clamp(halfWidthPixels * targetPresentation.fixedTriangleScale, 1.0, 110.0);

		SolidBrush symbolBrush(drawColor);
		if (target.style.icon == VsmrScene::IconStyle::Diamond)
		{
			const double halfDiagonal = std::clamp((lengthPixels + halfWidthPixels) / 2.0, 5.0, 110.0);
			PointF diamond[] = {
				PointF(static_cast<REAL>(center.x + forwardX * halfDiagonal), static_cast<REAL>(center.y + forwardY * halfDiagonal)),
				PointF(static_cast<REAL>(center.x + rightX * halfDiagonal), static_cast<REAL>(center.y + rightY * halfDiagonal)),
				PointF(static_cast<REAL>(center.x - forwardX * halfDiagonal), static_cast<REAL>(center.y - forwardY * halfDiagonal)),
				PointF(static_cast<REAL>(center.x - rightX * halfDiagonal), static_cast<REAL>(center.y - rightY * halfDiagonal))
			};
			gdi->FillPolygon(&symbolBrush, diamond, static_cast<INT>(_countof(diamond)));
			return static_cast<int>(std::ceil(halfDiagonal * 2.0));
		}

		PointF arrow[] = {
			PointF(static_cast<REAL>(center.x + forwardX * lengthPixels), static_cast<REAL>(center.y + forwardY * lengthPixels)),
			PointF(static_cast<REAL>(center.x - forwardX * lengthPixels * 0.33 + rightX * halfWidthPixels), static_cast<REAL>(center.y - forwardY * lengthPixels * 0.33 + rightY * halfWidthPixels)),
			PointF(static_cast<REAL>(center.x - forwardX * lengthPixels * 0.05), static_cast<REAL>(center.y - forwardY * lengthPixels * 0.05)),
			PointF(static_cast<REAL>(center.x - forwardX * lengthPixels * 0.33 - rightX * halfWidthPixels), static_cast<REAL>(center.y - forwardY * lengthPixels * 0.33 - rightY * halfWidthPixels))
		};
		gdi->FillPolygon(&symbolBrush, arrow, static_cast<INT>(_countof(arrow)));
		return static_cast<int>(std::ceil(max(lengthPixels * 1.33, halfWidthPixels * 2.0)));
	};

	if (radarScene != nullptr)
	for (const VsmrScene::Target& sceneTarget : radarScene->targets)
	{
		const std::string& rtCallsign = sceneTarget.callsign;
		if (rtCallsign.empty() || !sceneTarget.position.valid)
			continue;

		CPosition RtPos2;
		RtPos2.m_Latitude = sceneTarget.position.latitude;
		RtPos2.m_Longitude = sceneTarget.position.longitude;
		if (sceneTarget.groundSpeed < 60 ||
			!m_AirportPositionValid ||
			sceneTarget.pressureAltitude > m_Filter ||
			RtPos2.DistanceTo(m_AirportPosition) > radarRangeNm)
			continue;

		if (!sceneTarget.passesDisplayMode)
			continue;
		const CRimcas::RimcasAlertTypes rimcasStage = static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
		const auto getBottomLine = [&]() -> const char*
		{
			return sceneTarget.bottomLine.c_str();
		};

		// Filtering the targets

		POINT RtPoint;

		RtPoint = projectPoint(RtPos2);

		int renderedIconSize = 12;
		if (windowAreaCRect.PtInRect(RtPoint)) {
			renderedIconSize = max(12, drawSceneTargetSymbol(sceneTarget, RtPoint));
			CRect TargetArea(
				RtPoint.x - renderedIconSize / 2,
				RtPoint.y - renderedIconSize / 2,
				RtPoint.x + renderedIconSize / 2,
				RtPoint.y + renderedIconSize / 2);
			TargetArea.NormalizeRect();
			const CRect clippedTargetArea = clipToWindowContent(TargetArea);
			if (!clippedTargetArea.IsRectEmpty())
				radar_screen->AddScreenObject(DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE), rtCallsign.c_str(), clippedTargetArea, false, getBottomLine());
		}

		if (mouseWithin(mouseLocation, windowAreaCRect) &&
			mouseWithin(mouseLocation, { RtPoint.x - renderedIconSize / 2, RtPoint.y - renderedIconSize / 2, RtPoint.x + renderedIconSize / 2, RtPoint.y + renderedIconSize / 2 })) {
			CPen* previousHoverPen = dc.SelectObject(&WhitePen);
			dc.MoveTo(RtPoint.x, RtPoint.y - 6);
			dc.LineTo(RtPoint.x - 4, RtPoint.y - 10);
			dc.MoveTo(RtPoint.x, RtPoint.y - 6);
			dc.LineTo(RtPoint.x + 4, RtPoint.y - 10);

			dc.MoveTo(RtPoint.x, RtPoint.y + 6);
			dc.LineTo(RtPoint.x - 4, RtPoint.y + 10);
			dc.MoveTo(RtPoint.x, RtPoint.y + 6);
			dc.LineTo(RtPoint.x + 4, RtPoint.y + 10);

			dc.MoveTo(RtPoint.x - 6, RtPoint.y);
			dc.LineTo(RtPoint.x - 10, RtPoint.y - 4);
			dc.MoveTo(RtPoint.x - 6, RtPoint.y);
			dc.LineTo(RtPoint.x - 10, RtPoint.y + 4);

			dc.MoveTo(RtPoint.x + 6, RtPoint.y);
			dc.LineTo(RtPoint.x + 10, RtPoint.y - 4);
			dc.MoveTo(RtPoint.x + 6, RtPoint.y);
			dc.LineTo(RtPoint.x + 10, RtPoint.y + 4);
			dc.SelectObject(previousHoverPen);
		}
		const int leaderLength = 50;

		POINT TagCenter;
		m_TargetPoints[rtCallsign] = RtPoint;
		auto customTagOffsetIt = m_TagOffsets.find(rtCallsign);
		if (customTagOffsetIt != m_TagOffsets.end())
		{
			TagCenter.x = RtPoint.x + customTagOffsetIt->second.x;
			TagCenter.y = RtPoint.y + customTagOffsetIt->second.y;
		}
		else
		{
			if (m_TagAngles.find(rtCallsign) == m_TagAngles.end())
			{
				// Use a stable default leader angle until the tag is positioned manually.
				m_TagAngles[rtCallsign] = 45.0;
			}

			TagCenter.x = long(RtPoint.x + float(leaderLength * cos(DegToRad(m_TagAngles[rtCallsign]))));
			TagCenter.y = long(RtPoint.y + float(leaderLength * sin(DegToRad(m_TagAngles[rtCallsign]))));
		}
		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//	

		// First we need to figure out the tag size

		int TagWidth = 0, TagHeight = 0;
		RectF mesureRect;
		if (tagRegularFont == nullptr)
			continue;

		struct RenderedTagElement
		{
			std::string text;
			int action = TAG_CITEM_NO;
			bool bold = false;
			VsmrScene::Color effectiveColor;
			int measuredWidth = 0;
			int measuredHeight = 0;
		};
		vector<vector<RenderedTagElement>> ReplacedLabelLines;

		for (const VsmrScene::TagLine& sceneLine : sceneTarget.tag.normal.lines)
		{
			if (sceneLine.elements.empty())
				continue;

			vector<RenderedTagElement> renderedLine;
			renderedLine.reserve(sceneLine.elements.size());

			int TempTagWidth = 0;

			for (const VsmrScene::TagElement& sceneElement : sceneLine.elements)
			{
				RenderedTagElement renderedElement;
				renderedElement.text = sceneElement.text;
				renderedElement.action = sceneElement.action;
				renderedElement.bold = sceneElement.bold;
				renderedElement.effectiveColor = sceneElement.effectiveColor;
				if (!sceneElement.text.empty())
				{
					mesureRect = RectF(0, 0, 0, 0);
					wstring wstr(sceneElement.text.begin(), sceneElement.text.end());
					Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (measureFont == nullptr)
						measureFont = tagRegularFont;
					gdi->MeasureString(wstr.c_str(), static_cast<INT>(wstr.size()),
						measureFont, PointF(0, 0), &defaultStringFormat, &mesureRect);
					renderedElement.measuredWidth = static_cast<int>(mesureRect.GetRight());
					renderedElement.measuredHeight = static_cast<int>(mesureRect.GetBottom());
				}
				TempTagWidth += renderedElement.measuredWidth;

				renderedLine.push_back(std::move(renderedElement));
			}

			if (renderedLine.empty())
				continue;

			if (!renderedLine.empty())
				TempTagWidth += (int)blankWidth * (int(renderedLine.size()) - 1);

			TagHeight += oneLineHeight;
			TagWidth = max(TagWidth, TempTagWidth);
			ReplacedLabelLines.push_back(std::move(renderedLine));
		}
		if (TagHeight > 0)
			TagHeight = TagHeight - 2;

		// Pfiou, done with that, now we can draw the actual rectangle.

		// We need to figure out if the tag color changes according to RIMCAS alerts, or not
		bool rimcasLabelOnly = rimcasLabelOnlySetting;

		const VsmrScene::TagPalette& tagPalette = sceneTarget.tag.normalPalette;
		const Color definedBackgroundColor = SceneColorToGdi(tagPalette.background);
		const Color definedBackgroundOnRunwayColor = SceneColorToGdi(tagPalette.backgroundOnRunway);

		auto resolveRimcasBackground = [&](bool includeAlertStage) -> Color
		{
			if (includeAlertStage && rimcasStage == CRimcas::StageOne)
				return rimcasStageOneColor;
			if (includeAlertStage && rimcasStage == CRimcas::StageTwo)
				return rimcasStageTwoColor;
			return sceneTarget.rimcas.onRunway ? definedBackgroundOnRunwayColor : definedBackgroundColor;
		};
		Color TagBackgroundColor = resolveRimcasBackground(true);

		if (rimcasLabelOnly)
			TagBackgroundColor = resolveRimcasBackground(false);

		CRect TagBackgroundRect(TagCenter.x - (TagWidth / 2), TagCenter.y - (TagHeight / 2), TagCenter.x + (TagWidth / 2), TagCenter.y + (TagHeight / 2));
		const int padding = 1;
		TagBackgroundRect.InflateRect(padding, padding);
		CRect visibleTagRect;

		if (windowAreaCRect.PtInRect(RtPoint) &&
			visibleTagRect.IntersectRect(windowAreaCRect, TagBackgroundRect) &&
			!visibleTagRect.IsRectEmpty()) {

			int textLeft = TagBackgroundRect.left + padding;
			int textTop = TagBackgroundRect.top + padding;
			int textWidth = max(0, TagBackgroundRect.Width() - (padding * 2));

			// SRW keeps its lower-opacity presentation, but preserves the semantic
			// color already resolved by the shared scene (including status and rules).
			if (rimcasStage == CRimcas::NoAlert)
			{
				const BYTE srwAlpha = static_cast<BYTE>(
					(static_cast<unsigned int>(TagBackgroundColor.GetAlpha()) * 160u) / 255u);
				TagBackgroundColor = Color(
					srwAlpha,
					TagBackgroundColor.GetR(),
					TagBackgroundColor.GetG(),
					TagBackgroundColor.GetB());
			}

			// Slightly enlarge tag hitbox and draw rounded background.
			auto MakeRoundedRect = [](GraphicsPath &path, Rect r, int radius) {
				path.Reset();
				int d = radius * 2;
				path.AddArc(r.X, r.Y, d, d, 180, 90);
				path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
				path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
				path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
				path.CloseFigure();
			};
			Rect RoundedRect = CopyRect(TagBackgroundRect);
			GraphicsPath roundedPath;
			MakeRoundedRect(roundedPath, RoundedRect, 4);

			SolidBrush TagBackgroundBrush(TagBackgroundColor);
			gdi->FillPath(&TagBackgroundBrush, &roundedPath);

			auto getRimcasEditorColor = [&](const char* key, const Color& fallback) -> Color
			{
				const Value& activeProfile = radar_screen->CurrentConfig->getActiveProfile();
				if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
				{
					const Value& rimcas = activeProfile["rimcas"];
					if (rimcas.HasMember(key) && rimcas[key].IsObject())
						return radar_screen->CurrentConfig->getConfigColor(rimcas[key]);
				}
				return fallback;
			};

			SolidBrush AlertTextColorCaution(
				getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30)));
			SolidBrush AlertTextColorWarning(
				getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255)));

			m_TagAreas[rtCallsign] = TagBackgroundRect;
			const CRect clippedTagArea = clipToWindowContent(TagBackgroundRect);
			if (!clippedTagArea.IsRectEmpty())
				radar_screen->AddScreenObject(m_Id, rtCallsign.c_str(), clippedTagArea, true, getBottomLine());

			int heightOffset = 0;
			for (auto&& line : ReplacedLabelLines)
			{
				int lineWidth = 0;
				for (auto&& renderedElement : line)
					lineWidth += renderedElement.measuredWidth;
				if (!line.empty())
					lineWidth += blankWidth * (int(line.size()) - 1);

				int widthOffset = max(0, (textWidth - lineWidth) / 2);
				for (auto&& renderedElement : line)
				{
					const std::string& element = renderedElement.text;
					Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (drawFont == nullptr)
						drawFont = tagRegularFont;

					SolidBrush elementBrush(SceneColorToGdi(renderedElement.effectiveColor));

					wstring welement = wstring(element.begin(), element.end());
					int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
					gdi->DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
						PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
						&defaultStringFormat, &elementBrush);

					const int clickItemType = renderedElement.action;

					int itemWidth = renderedElement.measuredWidth;
					int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);
					if (itemWidth > 0 && itemHeight > 0)
					{
						CRect ItemRect(textLeft + widthOffset, textTop + heightOffset,
							textLeft + widthOffset + itemWidth, textTop + heightOffset + itemHeight);
						const CRect clippedItemRect = clipToWindowContent(ItemRect);
						if (!clippedItemRect.IsRectEmpty())
							radar_screen->AddScreenObject(clickItemType, rtCallsign.c_str(), clippedItemRect, true, getBottomLine());
					}

					widthOffset += renderedElement.measuredWidth;
					widthOffset += blankWidth;
				}

				heightOffset += oneLineHeight;
			}

			// Drawing the leader line
			RECT TagBackRectData = TagBackgroundRect;
			POINT toDraw1, toDraw2;
			if (LiangBarsky(TagBackRectData, RtPoint, TagBackgroundRect.CenterPoint(), toDraw1, toDraw2))
			{
				Pen leaderPen(whiteColor);
				gdi->DrawLine(&leaderPen, PointF(Gdiplus::REAL(RtPoint.x), Gdiplus::REAL(RtPoint.y)), PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));
			}

			// If we use a RIMCAS label only, we display it, and adapt the rectangle
			CRect oldCrectSave = TagBackgroundRect;

			if (rimcasLabelOnly) {
				const Color RimcasLabelColor = rimcasStage == CRimcas::StageOne
					? rimcasStageOneColor
					: (rimcasStage == CRimcas::StageTwo ? rimcasStageTwoColor : aliceBlueColor);

				if (RimcasLabelColor.ToCOLORREF() != aliceBlueColor.ToCOLORREF()) {
					int rimcas_height = 0;

					wstring wrimcas_height = wstring(L"ALERT");

					RectF RectRimcas_height;

					gdi->MeasureString(wrimcas_height.c_str(), wcslen(wrimcas_height.c_str()), tagRegularFont, PointF(0, 0), &defaultStringFormat, &RectRimcas_height);
					rimcas_height = int(RectRimcas_height.GetBottom());

					// Drawing the rectangle

					CRect RimcasLabelRect(TagBackgroundRect.left, TagBackgroundRect.top - rimcas_height, TagBackgroundRect.right, TagBackgroundRect.top);
					SolidBrush rimcasLabelBrush(RimcasLabelColor);
					gdi->FillRectangle(&rimcasLabelBrush, CopyRect(RimcasLabelRect));
					TagBackgroundRect.top -= rimcas_height;

					// Drawing the text

					wstring rimcasw = wstring(L"ALERT");
					StringFormat stformat;
					stformat.SetAlignment(StringAlignment::StringAlignmentCenter);
					SolidBrush* rimcasTextBrush = (rimcasStage == CRimcas::StageTwo)
						? &AlertTextColorWarning
						: &AlertTextColorCaution;
					gdi->DrawString(rimcasw.c_str(), wcslen(rimcasw.c_str()), tagRegularFont, PointF(Gdiplus::REAL((TagBackgroundRect.left + TagBackgroundRect.right) / 2), Gdiplus::REAL(TagBackgroundRect.top)), &stformat, rimcasTextBrush);

				}
			}

			// Adding the tag screen object

			//radar_screen->AddScreenObject(DRAWING_TAG, rt.GetCallsign(), TagBackgroundRect, true, GetBottomLine(rt.GetCallsign()).c_str());

			TagBackgroundRect = oldCrectSave;

			// Now adding the clickable zones
		}
	}

	// Render the same transmission snapshot through the SRW airport-relative
	// scale/offset/rotation transform before releasing its content clip.
	if (m_AirportPositionValid)
	{
		gdi->Flush(Gdiplus::FlushIntentionSync);
		const auto rdfStarted = std::chrono::steady_clock::now();
		VsmrRdf::Draw(
			hDC,
			radar_screen,
			windowAreaCRect,
			[this](const CPosition& position) -> POINT
			{
				return projectPoint(position);
			});
		m_LastRdfRenderMilliseconds += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - rdfStarted).count();
	}

	gdi->Restore(srwGraphicsState);
	if (srwSavedDc != 0)
		::RestoreDC(hDC, srwSavedDc);

	CBrush frameBrush(RGB(5, 7, 8));
	dc.FrameRect(windowAreaCRect, &frameBrush);
	const std::string title = "SRW " + std::to_string(m_Id - APPWINDOW_BASE);
	DrawInsetWindowChrome(
		dc,
		radar_screen,
		m_Id,
		m_AvisoLayoutMode,
		m_Area,
		title,
		true,
		mouseLocation,
		true,
		&m_LastChromeRenderMilliseconds);

	dc.Detach();
}

