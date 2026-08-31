#pragma once

#include "insets/InsetWindow.hpp"

class CSMRRadar;

namespace VsmrScene
{
	struct Color;
}

// Shared geometry and chrome primitives used by each inset implementation.
// This stays internal to the inset translation units so the public window API
// remains focused on the operations owned by CInsetWindow.
namespace VsmrInsetWindowInternal
{
	using AvisoLayoutMode = CInsetWindow::AvisoLayoutMode;
	using ResizeRegion = CInsetWindow::ResizeRegion;

	inline constexpr double kAvisoMetersPerNm = 1852.0;
	inline constexpr double kAvisoLatMetersPerDegree = 110540.0;
	inline constexpr double kAvisoLonMetersPerDegree = 111320.0;
	inline constexpr int kAvisoViewportTopBarHeight = 15;
	inline constexpr int kAvisoSnapThresholdPx = 28;
	inline constexpr int kAvisoCornerSnapThresholdPx = 48;
	inline constexpr int kAvisoMinLayoutWidth = 300;
	inline constexpr int kAvisoMinLayoutHeight = 120;
	inline constexpr int kInsetResizeHitPx = 7;
	inline constexpr int kInsetResizeInsidePx = 5;
	inline constexpr int kInsetResizeCornerPx = 18;
	inline constexpr int kInsetDragThresholdPx = 4;
	inline constexpr int kTimerContentWidth = 84;
	inline constexpr int kTimerContentHeight = 56;

	double ClampAvisoLatitude(double latitude);
	Gdiplus::Color SceneColorToGdi(const VsmrScene::Color& color);
	double AvisoCosLatitude(double latitude);
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
		double tolerance);
	Gdiplus::PointF RotateAvisoPointAround(
		double x,
		double y,
		const Gdiplus::PointF& center,
		double degrees);
	Gdiplus::PointF RotateAvisoVector(double x, double y, double degrees);
	double ResolveAvisoViewportScreenRotationDeg(
		CSMRRadar* radarScreen,
		double latitude,
		double longitude);

	CRect NormalizedAvisoLayoutBounds(const RECT* layoutBounds);
	int ClampAvisoDividerX(int x, const CRect& bounds);
	int ClampAvisoDividerY(int y, const CRect& bounds);
	bool IsAvisoSplitLayout(AvisoLayoutMode mode);
	bool IsAvisoCornerLayout(AvisoLayoutMode mode);
	bool IsAvisoSnappedLayout(AvisoLayoutMode mode);
	bool IsAvisoCornerRightAnchored(AvisoLayoutMode mode);
	bool IsAvisoCornerBottomAnchored(AvisoLayoutMode mode);
	CRect AvisoCornerRectForFrameSize(
		AvisoLayoutMode mode,
		const CRect& bounds,
		CSize requestedSize);
	bool ResolveAvisoSnapTarget(
		POINT point,
		const CRect& bounds,
		CSize cornerFrameSize,
		AvisoLayoutMode& mode,
		CRect& area);

	CRect InsetFrameRect(AvisoLayoutMode mode, const RECT& areaValue);
	void DrawRadarInsetBorder(CDC& dc, AvisoLayoutMode mode, const RECT& areaValue);
	CRect InsetContentRect(AvisoLayoutMode mode, const RECT& areaValue);
	CRect InsetTitleBarRect(AvisoLayoutMode mode, const RECT& areaValue);
	CRect InsetCloseButtonRect(AvisoLayoutMode mode, const RECT& areaValue);
	CRect InsetFilterButtonRect(AvisoLayoutMode mode, const RECT& areaValue);
	CRect InsetTitleBarMoveRect(
		AvisoLayoutMode mode,
		const RECT& areaValue,
		bool showFilter,
		bool allowResize);
	bool TryParseInsetResizeObjectId(const char* objectId, ResizeRegion& region);
	CRect InsetResizeObjectRect(
		AvisoLayoutMode mode,
		const RECT& areaValue,
		ResizeRegion region);
	bool ResizeRegionHasHorizontalEdge(ResizeRegion region);
	bool ResizeRegionHasVerticalEdge(ResizeRegion region);
	bool IsSnappedResizeRegionSupported(AvisoLayoutMode mode, ResizeRegion region);
	bool ResizeAvisoSplitRectToPoint(
		AvisoLayoutMode mode,
		POINT point,
		const CRect& bounds,
		RECT& area);
	bool ResizeAvisoCornerRectToPoint(
		AvisoLayoutMode mode,
		POINT point,
		const CRect& bounds,
		RECT& area,
		bool resizeX,
		bool resizeY);
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
		double* elapsedMilliseconds);
	bool AvisoRectIntersects(const CRect& one, const CRect& two);
}
