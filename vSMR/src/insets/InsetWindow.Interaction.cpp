#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/InsetWindow.Internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

using VsmrInsetWindowInternal::AvisoCornerRectForFrameSize;
using VsmrInsetWindowInternal::AvisoCosLatitude;
using VsmrInsetWindowInternal::ClampAvisoDividerX;
using VsmrInsetWindowInternal::ClampAvisoDividerY;
using VsmrInsetWindowInternal::ClampAvisoLatitude;
using VsmrInsetWindowInternal::InsetCloseButtonRect;
using VsmrInsetWindowInternal::InsetContentRect;
using VsmrInsetWindowInternal::InsetFilterButtonRect;
using VsmrInsetWindowInternal::InsetFrameRect;
using VsmrInsetWindowInternal::InsetResizeObjectRect;
using VsmrInsetWindowInternal::InsetTitleBarMoveRect;
using VsmrInsetWindowInternal::InsetTitleBarRect;
using VsmrInsetWindowInternal::IsAvisoCornerBottomAnchored;
using VsmrInsetWindowInternal::IsAvisoCornerLayout;
using VsmrInsetWindowInternal::IsAvisoCornerRightAnchored;
using VsmrInsetWindowInternal::IsAvisoSplitLayout;
using VsmrInsetWindowInternal::IsSnappedResizeRegionSupported;
using VsmrInsetWindowInternal::NormalizedAvisoLayoutBounds;
using VsmrInsetWindowInternal::ResizeAvisoCornerRectToPoint;
using VsmrInsetWindowInternal::ResizeAvisoSplitRectToPoint;
using VsmrInsetWindowInternal::ResizeRegionHasHorizontalEdge;
using VsmrInsetWindowInternal::ResizeRegionHasVerticalEdge;
using VsmrInsetWindowInternal::ResolveAvisoSnapTarget;
using VsmrInsetWindowInternal::RotateAvisoVector;
using VsmrInsetWindowInternal::TryParseInsetResizeObjectId;
using VsmrInsetWindowInternal::kAvisoCornerSnapThresholdPx;
using VsmrInsetWindowInternal::kAvisoLatMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoLonMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoMetersPerNm;
using VsmrInsetWindowInternal::kAvisoMinLayoutHeight;
using VsmrInsetWindowInternal::kAvisoMinLayoutWidth;
using VsmrInsetWindowInternal::kAvisoSnapThresholdPx;
using VsmrInsetWindowInternal::kAvisoViewportTopBarHeight;
using VsmrInsetWindowInternal::kInsetDragThresholdPx;
using VsmrInsetWindowInternal::kTimerContentHeight;
using VsmrInsetWindowInternal::kTimerContentWidth;

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
		-GetAvisoViewportScreenRotationDeg());
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
		-GetAvisoViewportScreenRotationDeg());
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
