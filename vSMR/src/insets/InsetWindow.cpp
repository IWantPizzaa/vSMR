#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "radar/RadarScreen.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/logging/Logger.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
	using VsmrRadarUiSupport::BetterHarversine;
	using VsmrRadarUiSupport::CopyRect;
	using VsmrRadarUiSupport::DegToRad;
	using VsmrRadarUiSupport::LiangBarsky;
	using VsmrRadarUiSupport::RadToDeg;
	using VsmrRadarUiSupport::TrueBearing;
	using VsmrRadarUiSupport::mouseWithin;
	using VsmrRadarUiSupport::rotate_point;
	using VsmrRadarUiSupport::startsWith;

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

	void DrawRadarInsetBorder(CDC& dc, AvisoLayoutMode mode, const RECT& areaValue)
	{
		CRect frame = InsetFrameRect(mode, areaValue);
		frame.NormalizeRect();
		if (frame.IsRectEmpty())
			return;
		CBrush borderBrush(RGB(0, 0, 0));
		dc.FrameRect(frame, &borderBrush);
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
		StopRenderPipeline();
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
		if (renderPipeline != nullptr)
			renderPipeline->InvalidateRequests();
	}

	std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> TakeCompletedRender()
	{
		return renderPipeline != nullptr
			? renderPipeline->TakeCompleted()
			: nullptr;
	}

	void AllowRetryForDiscardedResult(std::uint64_t requestId)
	{
		if (renderPipeline != nullptr)
			renderPipeline->AllowRetryForDiscardedResult(requestId);
	}

	void QueueRender(
		CSMRRadar* radarScreen,
		CSMRRadar::AvisoRasterRenderRequest request)
	{
		if (radarScreen == nullptr ||
			request.path.empty() ||
			request.features == nullptr ||
			request.labels == nullptr ||
			request.rasterWidth <= 0 ||
			request.rasterHeight <= 0 ||
			radarScreen->IsShutdownRequested() ||
			radarScreen->IsAvisoGeoJsonRenderStopRequested())
		{
			return;
		}

		EnsureRenderPipeline(radarScreen);
		if (renderPipeline != nullptr)
			renderPipeline->Queue(std::move(request), cacheBitmap != nullptr);
	}

	void StopRenderPipeline()
	{
		if (renderPipeline != nullptr)
		{
			renderPipeline->Stop();
			renderPipeline.reset();
		}
		renderRadar = nullptr;
	}

	bool HasPendingRender() const noexcept
	{
		return renderPipeline != nullptr && renderPipeline->HasPendingWork();
	}

	VsmrPerformance::AvisoQueueDepth PerformanceQueueDepth() const
	{
		return renderPipeline != nullptr
			? renderPipeline->QueueDepth()
			: VsmrPerformance::AvisoQueueDepth{};
	}

	void EnsureRenderPipeline(CSMRRadar* radarScreen)
	{
		if (radarScreen == nullptr)
			return;
		if (renderPipeline != nullptr && renderRadar == radarScreen)
			return;
		if (renderPipeline != nullptr)
			StopRenderPipeline();

		renderRadar = radarScreen;
		VsmrAviso::AvisoRasterPipeline::Callbacks callbacks;
		callbacks.render =
			[radarScreen](const CSMRRadar::AvisoRasterRenderRequest& request)
		{
				return radarScreen->RenderAvisoGeoJsonRaster(request);
			};
		callbacks.isExternallyCancelled =
			[radarScreen](const CSMRRadar::AvisoRasterRenderRequest& request)
		{
				return radarScreen->IsAvisoRasterRenderRequestCancelled(request);
			};
		callbacks.isExternalStopRequested = [radarScreen]()
		{
			return radarScreen->IsShutdownRequested() ||
				radarScreen->IsAvisoGeoJsonRenderStopRequested();
		};
		callbacks.requestRefresh = [radarScreen]()
		{
			radarScreen->RequestRefreshFromWorker();
		};
		callbacks.workerThreadStarted = []()
		{
			ULONG stackGuarantee = 64U * 1024U;
			::SetThreadStackGuarantee(&stackGuarantee);
			VsmrCrashRuntime::RecordCurrentThreadRole("inset AVISO render worker");
		};
		callbacks.workerThreadStopped = []()
		{
			VsmrCrashRuntime::RecordCurrentThreadRole("inactive");
		};
		callbacks.renderStarted =
			[radarScreen](const CSMRRadar::AvisoRasterRenderRequest&)
		{
				VsmrCrashRuntime::RecordCurrentThreadCallback(
					"AvisoRasterPipeline::WorkerMain (inset)",
					reinterpret_cast<std::uintptr_t>(radarScreen));
			};
		callbacks.reportError = [](const std::string& message)
		{
			Logger::info(message);
		};
		callbacks.diagnostics.requestQueued = [radarScreen](bool superseded)
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoRequestQueued(
				VsmrPerformance::AvisoViewport::Inset,
				superseded);
		};
		callbacks.diagnostics.requestCoalesced = [radarScreen]()
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoRequestCoalesced(
				VsmrPerformance::AvisoViewport::Inset);
		};
		callbacks.diagnostics.requestDebounced = [radarScreen]()
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoRequestDebounced(
				VsmrPerformance::AvisoViewport::Inset);
		};
		callbacks.diagnostics.rasterBuilt =
			[radarScreen](double rebuildMilliseconds, double queueWaitMilliseconds, bool succeeded)
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoRasterBuild(
				VsmrPerformance::AvisoViewport::Inset,
				rebuildMilliseconds,
				queueWaitMilliseconds,
				succeeded);
		};
		callbacks.diagnostics.rasterBuildCancelled = [radarScreen]()
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoRasterBuildCancelled(
				VsmrPerformance::AvisoViewport::Inset);
		};
		callbacks.diagnostics.resultDiscarded = [radarScreen]()
		{
			radarScreen->PerformanceDiagnostics.RecordAvisoResultDiscarded(
				VsmrPerformance::AvisoViewport::Inset);
		};

		renderPipeline = std::make_unique<VsmrAviso::AvisoRasterPipeline>(
			std::move(callbacks),
			"inset AVISO render worker");
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
	CSMRRadar* renderRadar = nullptr;
	std::unique_ptr<VsmrAviso::AvisoRasterPipeline> renderPipeline;
	VsmrAviso::AvisoRasterBlitter rasterBlitter;
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

void CInsetWindow::DrawWindowChrome(
	CDC& dc,
	CSMRRadar* radarScreen,
	AvisoLayoutMode mode,
	const std::string& title,
	bool showFilter,
	POINT mouseLocation,
	bool allowResize)
{
	DrawInsetWindowChrome(
		dc,
		radarScreen,
		m_Id,
		mode,
		m_Area,
		title,
		showFilter,
		mouseLocation,
		allowResize,
		&m_LastChromeRenderMilliseconds);
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

	m_AvisoState->StopRenderPipeline();
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

	// ----- Preparing the viewport -----
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

	dc.FillSolidRect(viewportRect, radar_screen->GetAvisoBackgroundColor());
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
		DrawRadarInsetBorder(dc, m_AvisoLayoutMode, m_Area);
	};

	// ----- Loading the AVISO data -----
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
	dc.FillSolidRect(viewportRect, radar_screen->GetAvisoBackgroundColor());
	std::shared_ptr<const std::vector<CSMRRadar::AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<CSMRRadar::AvisoLabel>> labelSnapshot;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	unsigned long long groupGeneration = 0;
	if (!radar_screen->GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		groupGeneration))
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	// Initializing the view from the airport or dataset bounds
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

	// ----- Resolving the cached raster -----
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

		const RECT sourceRect = {
			sourceXInt,
			sourceYInt,
			sourceRightInt,
			sourceBottomInt
		};
		const RECT destinationRect = {
			destLeft,
			destTop,
			destRightInt,
			destBottomInt
		};
		return m_AvisoState->rasterBlitter.Blend(
			*gdi,
			hDC,
			m_AvisoState->cacheBitmap,
			sourceRect,
			destinationRect,
			viewportRect);
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

		const RECT sourceRect = {
			sourceXInt,
			sourceYInt,
			sourceRightInt,
			sourceBottomInt
		};
		const RECT destinationRect = {
			viewportRect.left,
			viewportRect.top,
			viewportRect.right,
			viewportRect.bottom
		};
		return m_AvisoState->rasterBlitter.Blend(
			*gdi,
			hDC,
			m_AvisoState->cacheBitmap,
			sourceRect,
			destinationRect,
			viewportRect);
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

	// ----- Drawing or rebuilding the raster -----
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
			request.useDayPalette = radar_screen->AvisoUseDayColorPalette;
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
			CPosition insetDownLeft;
			insetDownLeft.m_Latitude = displayMinLat;
			insetDownLeft.m_Longitude = displayMinLon;
			CPosition insetUpRight;
			insetUpRight.m_Latitude = displayMaxLat;
			insetUpRight.m_Longitude = displayMaxLon;
			request.viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(
				SMRGeometry::DistanceMeters(insetDownLeft, insetUpRight));
			request.projectedTopLeft = projectedTopLeft;
			request.projectedTopRight = projectedTopRight;
			request.projectedBottomLeft = projectedBottomLeft;
			request.projectedBottomRight = projectedBottomRight;

			updateRequested = true;
			m_AvisoState->QueueRender(radar_screen, std::move(request));
		}
	}
	const bool delayedByAvisoUpdate = updateRequested &&
		m_AvisoState->HasPendingRender();
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
		drawCenteredMessage(m_AvisoState->HasPendingRender() ? "Rendering AVISO" : "AVISO unavailable");

	// ----- Drawing aircraft and tags -----
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
		const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
		const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
		const VsmrScene::RadarScene* targetScene = radar_screen->GetCurrentRadarScene();
		const VsmrScene::TargetPresentation defaultTargetPresentation;
		const VsmrScene::TargetPresentation& targetPresentation = targetScene != nullptr
			? targetScene->targetPresentation
			: defaultTargetPresentation;
		const double pixPerMeter = max(
			0.0,
			static_cast<double>(max(1, m_AvisoScale)) / kAvisoMetersPerNm);

		VsmrTargetRendering::FrameSettings targetSettings;
		targetSettings.presentation = targetPresentation;
		targetSettings.pixelsPerMeter = pixPerMeter;
		targetSettings.projectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
		{
			CPosition position;
			position.m_Latitude = point.latitude;
			position.m_Longitude = point.longitude;
			return projectTargetPosition(position);
		};
		targetSettings.pointVisible = [&](const POINT& point, int margin) -> bool
		{
			return pointInViewport(point, margin);
		};
		targetSettings.iconCache = radar_screen->CreateTargetIconCacheCallbacks();
		VsmrTargetRendering::Frame targetRenderer(*gdi, std::move(targetSettings));
		VsmrTargetRendering::DrawOptions targetDrawOptions;
		const double avisoSymbolScale = std::isfinite(targetPresentation.symbolScale)
			? std::clamp(targetPresentation.symbolScale, 0.5, 1.5)
			: 1.0;
		targetDrawOptions.minimumHitSize = static_cast<int>(
			std::ceil(18.0 * avisoSymbolScale));

		CPen symbolPen(PS_SOLID, 1, RGB(255, 255, 255));

		auto tagFontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
		Gdiplus::Font* tagRegularFont =
			tagFontIt != radar_screen->customFonts.end() ? tagFontIt->second.get() : nullptr;
		VsmrTagRendering::FontContext tagFonts(*gdi, tagRegularFont, 2);
		const bool roundedTagCornersEnabled = radar_screen->GetTagRoundedCornersEnabledForEditor();

		auto drawTag = [&](const VsmrScene::Target& sceneTarget, const POINT& targetPoint)
		{
			if (!tagFonts.IsValid())
				return;

			const std::string& callsign = sceneTarget.callsign;
			const std::string& bottomLine = sceneTarget.bottomLine;
			POINT tagCenter = {};
			m_TargetPoints[callsign] = targetPoint;
			const auto customOffset = m_TagOffsets.find(callsign);
			if (customOffset != m_TagOffsets.end())
			{
				tagCenter.x = targetPoint.x + customOffset->second.x;
				tagCenter.y = targetPoint.y + customOffset->second.y;
			}
			else
			{
				if (m_TagAngles.find(callsign) == m_TagAngles.end())
					m_TagAngles[callsign] = 45.0;
				constexpr int leaderLength = 50;
				tagCenter.x = long(targetPoint.x + float(leaderLength * cos(DegToRad(m_TagAngles[callsign]))));
				tagCenter.y = long(targetPoint.y + float(leaderLength * sin(DegToRad(m_TagAngles[callsign]))));
			}

			VsmrTagRendering::Layout layout;
			if (!VsmrTagRendering::MeasureLayout(tagFonts, sceneTarget.tag.normal, layout))
			{
				VsmrScene::TagVariant fallback;
				VsmrScene::TagLine line;
				VsmrScene::TagElement element;
				const auto token = sceneTarget.tag.tokens.find("callsign");
				element.text = token != sceneTarget.tag.tokens.end() && !token->second.empty()
					? token->second
					: callsign;
				element.action = TAG_CITEM_NO;
				element.effectiveColor = sceneTarget.tag.normalPalette.text;
				line.elements.push_back(std::move(element));
				fallback.lines.push_back(std::move(line));
				if (!VsmrTagRendering::MeasureLayout(tagFonts, fallback, layout))
					return;
			}

			const VsmrScene::TagPalette& palette = sceneTarget.tag.normalPalette;
			VsmrTagRendering::PaintOptions options;
			options.targetPoint = targetPoint;
			options.tagCenter = tagCenter;
			options.background = SceneColorToGdi(
				sceneTarget.rimcas.onRunway ? palette.backgroundOnRunway : palette.background);
			options.leaderColor = Gdiplus::Color(255, 255, 255, 255);
			options.roundedCorners = roundedTagCornersEnabled;
			options.centerLines = true;
			options.contentHeightTrim = 2;
			options.symmetricBounds = true;
			const CRect expectedBounds =
				VsmrTagRendering::CalculateBounds(tagFonts, layout, options);
			if (!rectIntersectsViewport(expectedBounds) && !pointInViewport(targetPoint, 20))
				return;

			const VsmrTagRendering::PaintResult painted =
				VsmrTagRendering::Paint(*gdi, tagFonts, layout, options);
			if (painted.bounds.IsRectEmpty())
				return;

			m_TagAreas[callsign] = painted.bounds;
			const CRect clippedTag = clipToViewport(painted.bounds);
			if (!clippedTag.IsRectEmpty())
				radar_screen->AddScreenObject(m_Id, callsign.c_str(), clippedTag, true, bottomLine.c_str());
			for (const VsmrTagRendering::HitRegion& hit : painted.hitRegions)
			{
				const CRect clippedHit = clipToViewport(hit.area);
				if (!clippedHit.IsRectEmpty())
					radar_screen->AddScreenObject(hit.action, callsign.c_str(), clippedHit, true, bottomLine.c_str());
			}

			const CRimcas::RimcasAlertTypes stage =
				static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
			if (stage != CRimcas::StageOne && stage != CRimcas::StageTwo)
				return;

			VsmrTagRendering::DetachedTopBand alertBand;
			alertBand.text = "ALERT";
			alertBand.background =
				stage == CRimcas::StageOne ? rimcasStageOneColor : rimcasStageTwoColor;
			alertBand.textColor = stage == CRimcas::StageTwo
				? Gdiplus::Color(255, 255, 255, 255)
				: Gdiplus::Color(255, 30, 30, 30);
			VsmrTagRendering::PaintDetachedTopBand(
				*gdi,
				tagFonts,
				painted.bounds,
				alertBand);
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

			const VsmrTargetRendering::DrawResult renderedTarget =
				targetRenderer.DrawTarget(sceneTarget, targetDrawOptions);
			if (!renderedTarget.drawn)
				continue;

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

			CRect targetArea(renderedTarget.hitBounds);
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
	const Value* rimcasSection = getProfileObjectSection("rimcas");

	const COLORREF srwRunwayColor = getSectionColorRef(srwInsetSection, "runway_color", RGB(255, 255, 255));
	const COLORREF srwExtendedLineColor = getSectionColorRef(srwInsetSection, "extended_lines_color", RGB(180, 180, 180));
	const double srwExtendedLineLengthNm = max(0.1, getSectionDouble(srwInsetSection, "extended_lines_length", 15.0));
	const int srwExtendedLineTickSpacingNm = max(1, getSectionInt(srwInsetSection, "extended_lines_ticks_spacing", 1));
	const bool roundedTagCornersEnabled = radar_screen->GetTagRoundedCornersEnabledForEditor();
	const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
	const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));

	CRect windowAreaCRect = GetWindowContentRect();
	windowAreaCRect.NormalizeRect();

	// ----- Preparing the SRW viewport -----
	dc.FillSolidRect(windowAreaCRect, radar_screen->GetAvisoBackgroundColor());
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

	// ----- Drawing the active airport runways -----
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

	// ----- Drawing aircraft and tags -----

	CPen WhitePen(PS_SOLID, 1, RGB(255, 255, 255));

	auto fontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
	Gdiplus::Font* tagRegularFont =
		fontIt != radar_screen->customFonts.end() ? fontIt->second.get() : nullptr;
	Gdiplus::Font* tagBoldFont = tagRegularFont;
	if (tagRegularFont != nullptr)
	{
		Gdiplus::FontFamily family;
		WCHAR familyName[LF_FACESIZE] = {};
		const bool familyAvailable =
			tagRegularFont->GetFamily(&family) == Gdiplus::Ok &&
			family.GetFamilyName(familyName) == Gdiplus::Ok;
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
				m_SrwBoldFont = std::make_unique<Gdiplus::Font>(
					&family,
					currentSize,
					currentStyle | Gdiplus::FontStyleBold,
					Gdiplus::UnitPixel);
				if (m_SrwBoldFont->GetLastStatus() != Gdiplus::Ok)
					m_SrwBoldFont.reset();
			}

			VsmrTagRendering::FontContext measured(*gdi, tagRegularFont);
			m_SrwBlankWidth = measured.BlankWidth();
			m_SrwLineHeight = measured.LineHeight();
		}
		tagBoldFont = m_SrwBoldFont != nullptr ? m_SrwBoldFont.get() : tagRegularFont;
	}
	VsmrTagRendering::FontContext srwTagFonts(
		*gdi,
		tagRegularFont,
		tagBoldFont,
		m_SrwBlankWidth,
		m_SrwLineHeight);
	const Color whiteColor(255, 255, 255, 255);
	const auto getRimcasEditorColor = [&](const char* key, const Color& fallback) -> Color
	{
		if (radar_screen->CurrentConfig == nullptr)
			return fallback;
		const Value& activeProfile = radar_screen->CurrentConfig->getActiveProfile();
		if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		{
			const Value& rimcas = activeProfile["rimcas"];
			if (rimcas.HasMember(key) && rimcas[key].IsObject())
				return radar_screen->CurrentConfig->getConfigColor(rimcas[key]);
		}
		return fallback;
	};
	const Color alertTextCaution =
		getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30));
	const Color alertTextWarning =
		getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255));

	const VsmrScene::RadarScene* radarScene = radar_screen->GetCurrentRadarScene();
	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& targetPresentation = radarScene != nullptr
		? radarScene->targetPresentation
		: defaultTargetPresentation;

	VsmrTargetRendering::FrameSettings targetSettings;
	targetSettings.presentation = targetPresentation;
	targetSettings.pixelsPerMeter = max(0.0, m_Scale / kAvisoMetersPerNm);
	// SRW paints tags in the same pass, so retain its caller-selected GDI+ modes.
	targetSettings.optimizeRealisticBitmapQuality = false;
	targetSettings.projectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
	{
		CPosition position;
		position.m_Latitude = point.latitude;
		position.m_Longitude = point.longitude;
		return projectPoint(position);
	};
	targetSettings.pointVisible = [&](const POINT& point, int margin) -> bool
	{
		return point.x >= windowAreaCRect.left - margin &&
			point.x <= windowAreaCRect.right + margin &&
			point.y >= windowAreaCRect.top - margin &&
			point.y <= windowAreaCRect.bottom + margin;
	};
	targetSettings.iconCache = radar_screen->CreateTargetIconCacheCallbacks();
	VsmrTargetRendering::Frame targetRenderer(*gdi, std::move(targetSettings));

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
			sceneTarget.pressureAltitude > m_Filter)
			continue;

		if (!sceneTarget.passesDisplayMode)
			continue;
		const CRimcas::RimcasAlertTypes rimcasStage = static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
		const auto getBottomLine = [&]() -> const char*
		{
			return sceneTarget.bottomLine.c_str();
		};

		POINT RtPoint;

		RtPoint = projectPoint(RtPos2);

		int renderedIconSize = 12;
		if (windowAreaCRect.PtInRect(RtPoint)) {
			const VsmrTargetRendering::DrawResult renderedTarget =
				targetRenderer.DrawTarget(sceneTarget);
			if (!renderedTarget.drawn)
				continue;
			renderedIconSize = max(
				12,
				max(
					renderedTarget.hitBounds.right - renderedTarget.hitBounds.left,
					renderedTarget.hitBounds.bottom - renderedTarget.hitBounds.top));
			CRect TargetArea(renderedTarget.hitBounds);
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
		constexpr int leaderLength = 50;
		POINT tagCenter = {};
		m_TargetPoints[rtCallsign] = RtPoint;
		const auto customOffset = m_TagOffsets.find(rtCallsign);
		if (customOffset != m_TagOffsets.end())
		{
			tagCenter.x = RtPoint.x + customOffset->second.x;
			tagCenter.y = RtPoint.y + customOffset->second.y;
		}
		else
		{
			if (m_TagAngles.find(rtCallsign) == m_TagAngles.end())
			{
				// Keep the default stable until the controller moves this tag.
				m_TagAngles[rtCallsign] = 45.0;
			}
			tagCenter.x = long(RtPoint.x + float(leaderLength * cos(DegToRad(m_TagAngles[rtCallsign]))));
			tagCenter.y = long(RtPoint.y + float(leaderLength * sin(DegToRad(m_TagAngles[rtCallsign]))));
		}

		if (!srwTagFonts.IsValid())
			continue;
		VsmrTagRendering::Layout layout;
		if (!VsmrTagRendering::MeasureLayout(srwTagFonts, sceneTarget.tag.normal, layout))
			continue;

		const VsmrScene::TagPalette& palette = sceneTarget.tag.normalPalette;
		VsmrTagRendering::PaintOptions options;
		options.targetPoint = RtPoint;
		options.tagCenter = tagCenter;
		options.background = SceneColorToGdi(
			sceneTarget.rimcas.onRunway ? palette.backgroundOnRunway : palette.background);
		options.leaderColor = whiteColor;
		options.roundedCorners = roundedTagCornersEnabled;
		options.symmetricBounds = true;
		options.backgroundAlphaNumerator =
			rimcasStage == CRimcas::NoAlert ? 160U : 255U;
		const CRect expectedBounds =
			VsmrTagRendering::CalculateBounds(srwTagFonts, layout, options);
		CRect visibleTag;
		if (!windowAreaCRect.PtInRect(RtPoint) ||
			!visibleTag.IntersectRect(windowAreaCRect, expectedBounds) ||
			visibleTag.IsRectEmpty())
		{
			continue;
		}

		const VsmrTagRendering::PaintResult painted =
			VsmrTagRendering::Paint(*gdi, srwTagFonts, layout, options);
		if (painted.bounds.IsRectEmpty())
			continue;

		m_TagAreas[rtCallsign] = painted.bounds;
		const CRect clippedTag = clipToWindowContent(painted.bounds);
		if (!clippedTag.IsRectEmpty())
			radar_screen->AddScreenObject(m_Id, rtCallsign.c_str(), clippedTag, true, getBottomLine());
		for (const VsmrTagRendering::HitRegion& hit : painted.hitRegions)
		{
			const CRect clippedHit = clipToWindowContent(hit.area);
			if (!clippedHit.IsRectEmpty())
				radar_screen->AddScreenObject(hit.action, rtCallsign.c_str(), clippedHit, true, getBottomLine());
		}

		if (rimcasStage == CRimcas::StageOne || rimcasStage == CRimcas::StageTwo)
		{
			VsmrTagRendering::DetachedTopBand alertBand;
			alertBand.text = "ALERT";
			alertBand.background = rimcasStage == CRimcas::StageOne
				? rimcasStageOneColor
				: rimcasStageTwoColor;
			alertBand.textColor = rimcasStage == CRimcas::StageTwo
				? alertTextWarning
				: alertTextCaution;
			VsmrTagRendering::PaintDetachedTopBand(
				*gdi,
				srwTagFonts,
				painted.bounds,
				alertBand);
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
	DrawRadarInsetBorder(dc, m_AvisoLayoutMode, m_Area);

	dc.Detach();
}

