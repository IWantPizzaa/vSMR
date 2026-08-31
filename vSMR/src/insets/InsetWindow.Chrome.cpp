#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.Internal.hpp"
#include "radar/RadarScreen.hpp"
#include <chrono>
#include <cstring>

namespace VsmrInsetWindowInternal
{
	using VsmrRadarUiSupport::DegToRad;
	using VsmrRadarUiSupport::mouseWithin;

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
		bool allowResize)
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
		bool allowResize,
		double* elapsedMilliseconds)
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

void CInsetWindow::DrawWindowChrome(
	CDC& dc,
	CSMRRadar* radarScreen,
	AvisoLayoutMode mode,
	const std::string& title,
	bool showFilter,
	POINT mouseLocation,
	bool allowResize)
{
	VsmrInsetWindowInternal::DrawInsetWindowChrome(
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
