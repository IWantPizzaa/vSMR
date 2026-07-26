#include "stdafx.h"
#include "InsetWindow.h"
#include "SMRRadar.hpp"
#include "SMRGroundState.hpp"
#include "SMRTagColorRules.hpp"
#include "SMRTagDefinitionUtils.hpp"
#include "SMRVacdmTagHelpers.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
	constexpr double kAvisoMetersPerNm = 1852.0;
	constexpr double kAvisoLatMetersPerDegree = 110540.0;
	constexpr double kAvisoLonMetersPerDegree = 111320.0;
	constexpr int kAvisoViewportTopBarHeight = 15;
	constexpr int kAvisoTopMenuClearancePx = 22;
	constexpr int kAvisoSnapThresholdPx = 28;
	constexpr int kAvisoCornerSnapThresholdPx = 48;
	constexpr int kAvisoMinLayoutWidth = 300;
	constexpr int kAvisoMinLayoutHeight = 120;

	using AvisoLayoutMode = CInsetWindow::AvisoLayoutMode;

	double ClampAvisoLatitude(double latitude)
	{
		return std::clamp(latitude, -85.0, 85.0);
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

	bool AvisoVectorWithinTolerance(
		const Gdiplus::PointF& leftStart,
		const Gdiplus::PointF& leftEnd,
		const Gdiplus::PointF& rightStart,
		const Gdiplus::PointF& rightEnd,
		double tolerance)
	{
		return AvisoWithinTolerance(static_cast<double>(leftEnd.X - leftStart.X), static_cast<double>(rightEnd.X - rightStart.X), tolerance) &&
			AvisoWithinTolerance(static_cast<double>(leftEnd.Y - leftStart.Y), static_cast<double>(rightEnd.Y - rightStart.Y), tolerance);
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
		return kInsetToolbarRightMargin + buttonIndexFromRight * (kInsetToolbarButtonSize + kInsetToolbarButtonGap);
	}

	CRect DrawInsetButton(CDC& dc, const char* label, CRect rect, POINT mouseLocation)
	{
		rect.NormalizeRect();
		CBrush buttonBrush(RGB(60, 60, 60));
		dc.FillRect(rect, &buttonBrush);

		const COLORREF oldTextColor = dc.SetTextColor(RGB(0, 0, 0));
		const int oldBkMode = dc.SetBkMode(TRANSPARENT);
		dc.DrawTextA(label, -1, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		dc.SetBkMode(oldBkMode);
		dc.SetTextColor(oldTextColor);

		if (mouseWithin(mouseLocation, rect))
			dc.Draw3dRect(rect, RGB(45, 45, 45), RGB(75, 75, 75));
		else
			dc.Draw3dRect(rect, RGB(75, 75, 75), RGB(45, 45, 45));

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
		return std::clamp(x, minX, maxX);
	}

	int ClampAvisoDividerY(int y, const CRect& bounds)
	{
		const int minY = static_cast<int>(bounds.top) + kAvisoMinLayoutHeight;
		const int maxY = static_cast<int>(bounds.bottom) - kAvisoMinLayoutHeight;
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

	CRect DefaultAvisoCornerRect(AvisoLayoutMode mode, const CRect& bounds)
	{
		const int midX = (bounds.left + bounds.right) / 2;
		const int midY = (bounds.top + bounds.bottom) / 2;

		switch (mode)
		{
		case AvisoLayoutMode::CornerTopLeft:
			return CRect(bounds.left, bounds.top, midX, midY);
		case AvisoLayoutMode::CornerTopRight:
			return CRect(midX, bounds.top, bounds.right, midY);
		case AvisoLayoutMode::CornerBottomLeft:
			return CRect(bounds.left, midY, midX, bounds.bottom);
		case AvisoLayoutMode::CornerBottomRight:
			return CRect(midX, midY, bounds.right, bounds.bottom);
		default:
			return CRect(0, 0, 0, 0);
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
		if (current.Width() < kAvisoMinLayoutWidth || current.Height() < kAvisoMinLayoutHeight)
			current = DefaultAvisoCornerRect(mode, bounds);

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
				top = std::clamp(Pt.y, bounds.top, bounds.bottom - kAvisoMinLayoutHeight);
			else
				bottom = std::clamp(Pt.y, bounds.top + kAvisoMinLayoutHeight, bounds.bottom);
		}

		area = { left, top, right, bottom };
		return true;
	}

	CRect AvisoSplitDividerRect(AvisoLayoutMode mode, const CRect& viewportRect)
	{
		if (IsAvisoVerticalSplit(mode))
		{
			const int dividerX = (mode == AvisoLayoutMode::SplitLeft) ? viewportRect.right : viewportRect.left;
			return CRect(dividerX - 3, viewportRect.top, dividerX + 3, viewportRect.bottom);
		}

		if (IsAvisoHorizontalSplit(mode))
		{
			const int dividerY = (mode == AvisoLayoutMode::SplitTop) ? viewportRect.bottom : viewportRect.top;
			return CRect(viewportRect.left, dividerY - 3, viewportRect.right, dividerY + 3);
		}

		return CRect(0, 0, 0, 0);
	}

	CRect AvisoCornerVerticalDividerRect(AvisoLayoutMode mode, const CRect& viewportRect)
	{
		if (!IsAvisoCornerLayout(mode))
			return CRect(0, 0, 0, 0);

		const int dividerX = IsAvisoCornerRightAnchored(mode) ? viewportRect.left : viewportRect.right;
		return CRect(dividerX - 3, viewportRect.top, dividerX + 3, viewportRect.bottom);
	}

	CRect AvisoCornerHorizontalDividerRect(AvisoLayoutMode mode, const CRect& viewportRect)
	{
		if (!IsAvisoCornerLayout(mode))
			return CRect(0, 0, 0, 0);

		const int dividerY = IsAvisoCornerBottomAnchored(mode) ? viewportRect.top : viewportRect.bottom;
		return CRect(viewportRect.left, dividerY - 3, viewportRect.right, dividerY + 3);
	}

	CRect AvisoCornerToolbarButtonRect(AvisoLayoutMode mode, const CRect& viewportRect, bool oppositeCorner, int buttonIndex)
	{
		const int buttonSize = 17;
		const int margin = 5;
		const int gap = 2;
		bool rightSide = IsAvisoCornerRightAnchored(mode);
		bool bottomSide = IsAvisoCornerBottomAnchored(mode);
		if (oppositeCorner)
		{
			rightSide = !rightSide;
			bottomSide = !bottomSide;
		}

		const int step = max(0, buttonIndex) * (buttonSize + gap);
		const int left = rightSide ? viewportRect.right - margin - buttonSize - step : viewportRect.left + margin + step;
		const int top = bottomSide ? viewportRect.bottom - margin - buttonSize : viewportRect.top + margin;
		return CRect(left, top, left + buttonSize, top + buttonSize);
	}

	CRect AvisoCornerButtonRect(AvisoLayoutMode mode, const CRect& viewportRect, bool oppositeCorner)
	{
		return AvisoCornerToolbarButtonRect(mode, viewportRect, oppositeCorner, 0);
	}

	void DrawAvisoDivider(CDC& dc, CSMRRadar* radarScreen, int objectType, const char* objectId, const CRect& dividerRect)
	{
		if (radarScreen == nullptr || objectId == nullptr || dividerRect.IsRectEmpty())
			return;

		CBrush dividerBrush(RGB(105, 105, 105));
		dc.FillRect(dividerRect, &dividerBrush);
		dc.Draw3dRect(dividerRect, RGB(70, 70, 70), RGB(150, 150, 150));
		radarScreen->AddScreenObject(objectType, objectId, dividerRect, true, "");
	}

	std::string AvisoTagTypeKey(CSMRRadar::TagTypes type)
	{
		if (type == CSMRRadar::TagTypes::Departure)
			return "departure";
		if (type == CSMRRadar::TagTypes::Arrival)
			return "arrival";
		if (type == CSMRRadar::TagTypes::Uncorrelated)
			return "uncorrelated";
		return "airborne";
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

			const bool sameRequest =
				lastRequestValid &&
				lastRequestPath == request.path &&
				lastRequestRasterWidth == request.rasterWidth &&
				lastRequestRasterHeight == request.rasterHeight &&
				AvisoWithinTolerance(lastRequestMinLongitude, request.displayMinLongitude, 1e-10) &&
				AvisoWithinTolerance(lastRequestMinLatitude, request.displayMinLatitude, 1e-10) &&
				AvisoWithinTolerance(lastRequestMaxLongitude, request.displayMaxLongitude, 1e-10) &&
				AvisoWithinTolerance(lastRequestMaxLatitude, request.displayMaxLatitude, 1e-10) &&
				AvisoPointWithinTolerance(lastRequestProjectedTopLeft, request.projectedTopLeft, 0.25) &&
				AvisoPointWithinTolerance(lastRequestProjectedTopRight, request.projectedTopRight, 0.25) &&
				AvisoPointWithinTolerance(lastRequestProjectedBottomLeft, request.projectedBottomLeft, 0.25) &&
				AvisoPointWithinTolerance(lastRequestProjectedBottomRight, request.projectedBottomRight, 0.25);
			if (sameRequest)
				return;

			request.requestId = ++nextRequestId;
			latestRequestId = request.requestId;
			lastRequestValid = true;
			lastRequestPath = request.path;
			lastRequestMinLongitude = request.displayMinLongitude;
			lastRequestMinLatitude = request.displayMinLatitude;
			lastRequestMaxLongitude = request.displayMaxLongitude;
			lastRequestMaxLatitude = request.displayMaxLatitude;
			lastRequestRasterWidth = request.rasterWidth;
			lastRequestRasterHeight = request.rasterHeight;
			lastRequestProjectedTopLeft = request.projectedTopLeft;
			lastRequestProjectedTopRight = request.projectedTopRight;
			lastRequestProjectedBottomLeft = request.projectedBottomLeft;
			lastRequestProjectedBottomRight = request.projectedBottomRight;
			pendingRenderRadar = radarScreen;
			pendingRenderRequest = std::make_unique<CSMRRadar::AvisoRasterRenderRequest>(std::move(request));
			renderPending.store(true, std::memory_order_relaxed);

			if (!renderThreadStarted)
			{
				renderStopRequested = false;
				renderThread = std::thread(&AvisoViewportState::RenderThreadMain, this);
				renderThreadStarted = true;
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

			std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> result;
			try
			{
				result = radarScreen->RenderAvisoGeoJsonRaster(*request);
			}
			catch (...)
			{
				result.reset();
			}

			bool shouldRefresh = false;
			{
				std::lock_guard<std::mutex> guard(renderMutex);
				if (renderStopRequested)
					return;

				if (result != nullptr &&
					result->requestId == latestRequestId &&
					!radarScreen->IsShutdownRequested() &&
					!radarScreen->IsAvisoGeoJsonRenderStopRequested())
				{
					completedRenderResult = std::move(result);
					shouldRefresh = true;
				}

				renderInFlight = false;
				renderPending.store(pendingRenderRequest != nullptr || completedRenderResult != nullptr, std::memory_order_relaxed);
			}

			if (shouldRefresh)
			{
				try
				{
					radarScreen->RequestRefresh();
				}
				catch (...)
				{
				}
			}
		}
	}

	HBITMAP cacheBitmap = nullptr;
	string cachePath;
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
	std::mutex renderMutex;
	std::condition_variable renderCondition;
	std::thread renderThread;
	bool renderThreadStarted = false;
	bool renderStopRequested = false;
	bool renderInFlight = false;
	unsigned long lastZoomInteractionTick = 0;
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
	Gdiplus::PointF lastRequestProjectedTopLeft;
	Gdiplus::PointF lastRequestProjectedTopRight;
	Gdiplus::PointF lastRequestProjectedBottomLeft;
	Gdiplus::PointF lastRequestProjectedBottomRight;
};

CInsetWindow::CInsetWindow(int Id)
{
	m_Id = Id;
	m_AvisoState = std::make_unique<AvisoViewportState>();
}

CInsetWindow::~CInsetWindow()
{
	CancelAvisoViewportRender();
}

bool CInsetWindow::IsAvisoViewport() const
{
	return m_Mode == Mode::AvisoViewport;
}

void CInsetWindow::ClearAvisoViewportCache()
{
	if (m_AvisoState != nullptr)
	{
		m_AvisoState->InvalidateRenderRequests();
		m_AvisoState->ClearCache();
	}
}

void CInsetWindow::CancelAvisoViewportRender()
{
	if (m_AvisoState == nullptr)
		return;

	m_AvisoState->StopRenderThread();
}

void CInsetWindow::ResetAvisoInteractionState()
{
	if (!IsAvisoViewport())
		return;

	m_AvisoRightPanning = false;
	m_AvisoScrollSelected = false;
	m_AvisoScreenArea = { 0, 0, 0, 0 };
	m_AvisoScreenAreaValid = false;
	m_AvisoRenderWindow = nullptr;
	m_Grip = false;
}

bool CInsetWindow::IsPointInside(POINT Pt) const
{
	CRect areaRect(m_Area);
	areaRect.NormalizeRect();
	return
		Pt.x >= areaRect.left &&
		Pt.x <= areaRect.right &&
		Pt.y >= areaRect.top &&
			Pt.y <= areaRect.bottom;
}

void CInsetWindow::ApplyAvisoLayoutBounds(const RECT* layoutBounds)
{
	if (!IsAvisoViewport())
		return;

	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
		return;

	const int contentTop = bounds.top + kAvisoViewportTopBarHeight;
	if (contentTop >= bounds.bottom)
		return;

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
		const bool rightAnchored = IsAvisoCornerRightAnchored(m_AvisoLayoutMode);
		const bool bottomAnchored = IsAvisoCornerBottomAnchored(m_AvisoLayoutMode);
		const int left = rightAnchored
			? std::clamp(static_cast<int>(area.left), static_cast<int>(bounds.left), static_cast<int>(bounds.right) - kAvisoMinLayoutWidth)
			: static_cast<int>(bounds.left);
		const int right = rightAnchored
			? static_cast<int>(bounds.right)
			: std::clamp(static_cast<int>(area.right), static_cast<int>(bounds.left) + kAvisoMinLayoutWidth, static_cast<int>(bounds.right));
		const int top = bottomAnchored
			? std::clamp(static_cast<int>(area.top), static_cast<int>(bounds.top), static_cast<int>(bounds.bottom) - kAvisoMinLayoutHeight)
			: static_cast<int>(bounds.top);
		const int bottom = bottomAnchored
			? static_cast<int>(bounds.bottom)
			: std::clamp(static_cast<int>(area.bottom), static_cast<int>(bounds.top) + kAvisoMinLayoutHeight, static_cast<int>(bounds.bottom));
		m_Area = { left, top, right, bottom };
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
	if (!IsAvisoViewport())
		return;

	CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
	if (bounds.IsRectEmpty())
	{
		m_AvisoLayoutMode = AvisoLayoutMode::Floating;
		return;
	}

	const bool nearLeft = Pt.x <= bounds.left + kAvisoSnapThresholdPx;
	const bool nearRight = Pt.x >= bounds.right - kAvisoSnapThresholdPx;
	const bool nearTop = Pt.y <= bounds.top + kAvisoSnapThresholdPx;
	const bool nearBottom = Pt.y >= bounds.bottom - kAvisoSnapThresholdPx;
	const bool nearCornerLeft = Pt.x <= bounds.left + kAvisoCornerSnapThresholdPx;
	const bool nearCornerRight = Pt.x >= bounds.right - kAvisoCornerSnapThresholdPx;
	const bool nearCornerTop = Pt.y <= bounds.top + kAvisoCornerSnapThresholdPx;
	const bool nearCornerBottom = Pt.y >= bounds.bottom - kAvisoCornerSnapThresholdPx;
	AvisoLayoutMode snappedMode = AvisoLayoutMode::Floating;
	if (nearCornerLeft && nearCornerTop)
		snappedMode = AvisoLayoutMode::CornerTopLeft;
	else if (nearCornerRight && nearCornerTop)
		snappedMode = AvisoLayoutMode::CornerTopRight;
	else if (nearCornerLeft && nearCornerBottom)
		snappedMode = AvisoLayoutMode::CornerBottomLeft;
	else if (nearCornerRight && nearCornerBottom)
		snappedMode = AvisoLayoutMode::CornerBottomRight;
	else if (nearLeft)
		snappedMode = AvisoLayoutMode::SplitLeft;
	else if (nearRight)
		snappedMode = AvisoLayoutMode::SplitRight;
	else if (nearTop)
		snappedMode = AvisoLayoutMode::SplitTop;
	else if (nearBottom)
		snappedMode = AvisoLayoutMode::SplitBottom;

	m_AvisoLayoutMode = snappedMode;
	if (IsAvisoSplitLayout(snappedMode))
	{
		CRect splitRect = DefaultAvisoSplitRect(snappedMode, bounds);
		if (!splitRect.IsRectEmpty())
			m_Area = splitRect;
	}
	else if (IsAvisoCornerLayout(snappedMode))
	{
		CRect cornerRect = DefaultAvisoCornerRect(snappedMode, bounds);
		if (!cornerRect.IsRectEmpty())
			m_Area = cornerRect;
	}

	ApplyAvisoLayoutBounds(layoutBounds);
}

void CInsetWindow::UpdateAvisoScreenArea(HWND hwnd)
{
	m_AvisoScreenArea = { 0, 0, 0, 0 };
	m_AvisoScreenAreaValid = false;
	m_AvisoRenderWindow = nullptr;
	if (!IsAvisoViewport() || hwnd == nullptr || !::IsWindow(hwnd))
		return;

	CRect areaRect(m_Area);
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
	if (!IsAvisoViewport() || !m_AvisoScreenAreaValid)
		return false;

	CRect screenRect(m_AvisoScreenArea);
	screenRect.NormalizeRect();
	if (screenRect.Width() <= 0 || screenRect.Height() <= 0)
		return false;
	if (screenPoint.x < screenRect.left || screenPoint.x > screenRect.right ||
		screenPoint.y < screenRect.top || screenPoint.y > screenRect.bottom)
	{
		return false;
	}

	CRect areaRect(m_Area);
	areaRect.NormalizeRect();
	if (areaRect.Width() <= 0 || areaRect.Height() <= 0)
		return false;

	const double fx = static_cast<double>(screenPoint.x - screenRect.left) / static_cast<double>(max(1, screenRect.Width()));
	const double fy = static_cast<double>(screenPoint.y - screenRect.top) / static_cast<double>(max(1, screenRect.Height()));
	avisoPoint.x = areaRect.left + static_cast<LONG>(std::lround(fx * static_cast<double>(areaRect.Width())));
	avisoPoint.y = areaRect.top + static_cast<LONG>(std::lround(fy * static_cast<double>(areaRect.Height())));
	return IsPointInside(avisoPoint);
}

void CInsetWindow::BeginAvisoPan(POINT Pt)
{
	if (!IsAvisoViewport())
		return;

	m_OffsetDrag = Pt;
	m_AvisoDragStartLatitude = m_AvisoCenterLatitude;
	m_AvisoDragStartLongitude = m_AvisoCenterLongitude;
	m_AvisoRightPanning = true;
	m_AvisoScrollSelected = true;
	m_Grip = false;
}

bool CInsetWindow::UpdateAvisoPan(POINT Pt)
{
	if (!IsAvisoViewport() || !m_AvisoRightPanning)
		return false;

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
	if (!IsAvisoViewport())
		return;

	ApplyAvisoLayoutBounds(layoutBounds);
	CRect currentArea(m_Area);
	currentArea.NormalizeRect();
	if (currentArea.Width() <= 0 || currentArea.Height() <= 0)
		return;

	const int detachedWidth = min(
		max(kAvisoMinLayoutWidth, currentArea.Width() - 40),
		std::clamp(currentArea.Width() / 2, kAvisoMinLayoutWidth, 620));
	const int detachedHeight = min(
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
	if (!IsAvisoViewport() || !IsPointInside(Pt) || !std::isfinite(scaleMultiplier) || scaleMultiplier <= 0.0)
		return false;

	CRect viewportRect(m_Area);
	viewportRect.NormalizeRect();
	const int viewportWidth = viewportRect.Width();
	const int viewportHeight = viewportRect.Height();
	if (viewportWidth <= 0 || viewportHeight <= 0)
		return false;

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
	if (m_AvisoState != nullptr)
		m_AvisoState->lastZoomInteractionTick = ::GetTickCount();
	return true;
}

void CInsetWindow::setAirport(string icao)
{
	this->icao = icao;
}

void CInsetWindow::OnClickScreenObject(const char * sItemString, POINT Pt, int Button)
{
	UNREFERENCED_PARAMETER(sItemString);
	UNREFERENCED_PARAMETER(Pt);
	UNREFERENCED_PARAMETER(Button);
}

bool CInsetWindow::OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool Released, const RECT* layoutBounds)
{
	if (sObjectId == nullptr)
		return true;

	if (strcmp(sObjectId, "divider") == 0 || strcmp(sObjectId, "divider_x") == 0 || strcmp(sObjectId, "divider_y") == 0)
	{
		if (!IsAvisoViewport())
			return true;

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

		if (!this->m_Grip)
		{
			m_OffsetInit = m_Offset;
			m_OffsetDrag = Pt;
			m_Grip = true;
		}

		POINT maxoffset = { (m_Area.right - m_Area.left) / 2, (m_Area.bottom - (m_Area.top + 15)) / 2 };
		m_Offset.x = max(-maxoffset.x, min(maxoffset.x, m_OffsetInit.x + (Pt.x - m_OffsetDrag.x)));
		m_Offset.y = max(-maxoffset.y, min(maxoffset.y, m_OffsetInit.y + (Pt.y - m_OffsetDrag.y)));

		if (Released)
		{
			m_Grip = false;
		}
	}
	if (strcmp(sObjectId, "resize") == 0) {
		if (IsAvisoViewport())
		{
			ApplyAvisoLayoutBounds(layoutBounds);
			CRect bounds = NormalizedAvisoLayoutBounds(layoutBounds);
			if (!bounds.IsRectEmpty() && IsAvisoCornerLayout(m_AvisoLayoutMode))
			{
				ResizeAvisoCornerRectToPoint(m_AvisoLayoutMode, Pt, bounds, m_Area, true, true);
				ApplyAvisoLayoutBounds(layoutBounds);
				return Released;
			}

			m_AvisoLayoutMode = AvisoLayoutMode::Floating;
		}

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
		if (IsAvisoViewport())
			ApplyAvisoLayoutBounds(layoutBounds);

		return Released;
	}
	if (strcmp(sObjectId, "topbar") == 0) {
		if (IsAvisoViewport())
		{
			ApplyAvisoLayoutBounds(layoutBounds);
			m_AvisoLayoutMode = AvisoLayoutMode::Floating;
		}

		CRect appWindowRect(m_Area);
		appWindowRect.NormalizeRect();

		POINT TopLeft = { Area.left, Area.bottom + 1 };
		POINT BottomRight = { TopLeft.x + appWindowRect.Width(), TopLeft.y + appWindowRect.Height() };
		CRect newPos(TopLeft, BottomRight);
		newPos.NormalizeRect();

		m_Area = newPos;
		if (IsAvisoViewport())
		{
			if (Released)
				SnapAvisoLayoutToPoint(Pt, layoutBounds);
			else
				ApplyAvisoLayoutBounds(layoutBounds);
		}

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
	CRect areaRect(m_Area);
	areaRect.NormalizeRect();

	POINT refPt = areaRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	POINT out = {0, 0};

	double dist = AptPositions[icao].DistanceTo(pos);
	double dir = TrueBearing(AptPositions[icao], pos);


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
	layoutBounds.top += kAvisoTopMenuClearancePx;
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect viewportRect(m_Area);
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
	radar_screen->AddScreenObject(m_Id, "window", m_Area, !IsAvisoSnappedLayout(m_AvisoLayoutMode), "");
	radar_screen->AddScreenObject(m_Id, "viewport", m_Area, false, "");

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
		CBrush frameBrush(RGB(127, 122, 122));
		dc.FrameRect(viewportRect, &frameBrush);

		if (IsAvisoSplitLayout(m_AvisoLayoutMode))
		{
			CRect floatRect(
				viewportRect.right - 23,
				viewportRect.top + 5,
				viewportRect.right - 6,
				viewportRect.top + 22);
			const CRect floatButtonRect = DrawInsetButton(dc, "F", floatRect, mouseLocation);
			radar_screen->AddScreenObject(m_Id, "float", floatButtonRect, false, "");

			CRect presetsRect(
				viewportRect.right - 42,
				viewportRect.top + 5,
				viewportRect.right - 25,
				viewportRect.top + 22);
			const CRect presetsButtonRect = DrawInsetButton(dc, "P", presetsRect, mouseLocation);
			radar_screen->AddScreenObject(m_Id, "presets", presetsButtonRect, false, "");

			DrawAvisoDivider(dc, radar_screen, m_Id, "divider", AvisoSplitDividerRect(m_AvisoLayoutMode, viewportRect));
			return;
		}

		if (IsAvisoCornerLayout(m_AvisoLayoutMode))
		{
			const CRect floatButtonRect = DrawInsetButton(
				dc,
				"F",
				AvisoCornerButtonRect(m_AvisoLayoutMode, viewportRect, false),
				mouseLocation);
			radar_screen->AddScreenObject(m_Id, "float", floatButtonRect, false, "");

			const CRect presetsButtonRect = DrawInsetButton(
				dc,
				"P",
				AvisoCornerToolbarButtonRect(m_AvisoLayoutMode, viewportRect, false, 1),
				mouseLocation);
			radar_screen->AddScreenObject(m_Id, "presets", presetsButtonRect, false, "");

			CRect resizeArea = AvisoCornerButtonRect(m_AvisoLayoutMode, viewportRect, true);
			resizeArea.NormalizeRect();
			CBrush resizeBrush(RGB(60, 60, 60));
			dc.FillRect(resizeArea, &resizeBrush);
			radar_screen->AddScreenObject(m_Id, "resize", resizeArea, true, "");
			dc.Draw3dRect(resizeArea, RGB(0, 0, 0), RGB(0, 0, 0));

			DrawAvisoDivider(dc, radar_screen, m_Id, "divider_x", AvisoCornerVerticalDividerRect(m_AvisoLayoutMode, viewportRect));
			DrawAvisoDivider(dc, radar_screen, m_Id, "divider_y", AvisoCornerHorizontalDividerRect(m_AvisoLayoutMode, viewportRect));
			return;
		}

		POINT bottomRight = { m_Area.right, m_Area.bottom };
		POINT resizeTopLeft = { bottomRight.x - 10, bottomRight.y - 10 };
		CRect resizeArea(resizeTopLeft, bottomRight);
		resizeArea.NormalizeRect();
		CBrush resizeBrush(RGB(60, 60, 60));
		dc.FillRect(resizeArea, &resizeBrush);
		radar_screen->AddScreenObject(m_Id, "resize", resizeArea, true, "");
		dc.Draw3dRect(resizeArea, RGB(0, 0, 0), RGB(0, 0, 0));

		POINT topBarTopLeft = viewportRect.TopLeft();
		topBarTopLeft.y -= kAvisoViewportTopBarHeight;
		POINT topBarBottomRight = { viewportRect.right, viewportRect.top };
		CRect topBar(topBarTopLeft, topBarBottomRight);
		topBar.NormalizeRect();
		dc.FillRect(topBar, &frameBrush);
		radar_screen->AddScreenObject(m_Id, "topbar", topBar, true, "");

		COLORREF oldTextColor = dc.SetTextColor(RGB(35, 35, 35));
		const string title = "AVISO";
		const CSize titleSize = dc.GetTextExtent(title.c_str());
		const int titleX = topBar.left + max(0, (topBar.Width() - titleSize.cx) / 2);
		const int titleY = topBar.bottom - titleSize.cy;
		dc.TextOutA(titleX, titleY, title.c_str());

		const CRect closeRect = DrawInsetToolbarButton(dc, "X", topBar, InsetToolbarRightOffset(0), mouseLocation);
		radar_screen->AddScreenObject(m_Id, "close", closeRect, false, "");
		const CRect presetsRect = DrawInsetToolbarButton(dc, "P", topBar, InsetToolbarRightOffset(1), mouseLocation);
		radar_screen->AddScreenObject(m_Id, "presets", presetsRect, false, "");
		const CRect reloadRect = DrawInsetToolbarButton(dc, "R", topBar, InsetToolbarRightOffset(2), mouseLocation);
		radar_screen->AddScreenObject(m_Id, "reload", reloadRect, false, "");
		const CRect editorRect = DrawInsetToolbarButton(dc, "E", topBar, InsetToolbarRightOffset(3), mouseLocation);
		radar_screen->AddScreenObject(m_Id, "editor", editorRect, false, "");
		dc.SetTextColor(oldTextColor);
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

	if (radar_screen->AvisoGeoJsonFeatureSnapshot == nullptr)
		radar_screen->AvisoGeoJsonFeatureSnapshot = std::make_shared<const std::vector<CSMRRadar::AvisoFeature>>(radar_screen->AvisoGeoJsonFeatures);
	if (radar_screen->AvisoGeoJsonLabelSnapshot == nullptr)
		radar_screen->AvisoGeoJsonLabelSnapshot = std::make_shared<const std::vector<CSMRRadar::AvisoLabel>>(radar_screen->AvisoGeoJsonLabels);
	if (radar_screen->AvisoGeoJsonFeatureSnapshot == nullptr || radar_screen->AvisoGeoJsonLabelSnapshot == nullptr)
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
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
	const unsigned long nowTick = ::GetTickCount();
	const unsigned long zoomPreviewMs = 320;
	const bool avisoZoomRecentlyChanged =
		m_AvisoState->lastZoomInteractionTick != 0 &&
		(nowTick - m_AvisoState->lastZoomInteractionTick) < zoomPreviewMs;
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

		const double transformPixelTolerance = 12.0;
		return AvisoVectorWithinTolerance(m_AvisoState->projectedTopLeft, m_AvisoState->projectedTopRight, projectedTopLeft, projectedTopRight, transformPixelTolerance) &&
			AvisoVectorWithinTolerance(m_AvisoState->projectedTopLeft, m_AvisoState->projectedBottomLeft, projectedTopLeft, projectedBottomLeft, transformPixelTolerance);
	};
	auto completedResultMatchesCurrentView = [&](const CSMRRadar::AvisoRasterRenderResult& result) -> bool
	{
		if (result.bitmap == nullptr ||
			result.path != path ||
			result.rasterWidth <= 0 ||
			result.rasterHeight <= 0)
		{
			return false;
		}

		const double transformPixelTolerance = 12.0;
		if (!AvisoVectorWithinTolerance(result.projectedTopLeft, result.projectedTopRight, projectedTopLeft, projectedTopRight, transformPixelTolerance) ||
			!AvisoVectorWithinTolerance(result.projectedTopLeft, result.projectedBottomLeft, projectedTopLeft, projectedBottomLeft, transformPixelTolerance))
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
	if (completedRenderResult != nullptr && completedResultMatchesCurrentView(*completedRenderResult))
	{
		m_AvisoState->ClearCache();
		m_AvisoState->cacheBitmap = completedRenderResult->bitmap;
		completedRenderResult->bitmap = nullptr;
		m_AvisoState->cachePath = completedRenderResult->path;
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
	}

	auto drawCache = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
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

		const int destLeft = static_cast<int>(std::floor(visibleLeft));
		const int destTop = static_cast<int>(std::floor(visibleTop));
		const int destRightInt = static_cast<int>(std::ceil(visibleRight));
		const int destBottomInt = static_cast<int>(std::ceil(visibleBottom));
		const int destWidthInt = destRightInt - destLeft;
		const int destHeightInt = destBottomInt - destTop;
		if (destWidthInt <= 0 || destHeightInt <= 0)
			return false;

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
	auto drawCacheFallbackDuringInteraction = [&]() -> bool
	{
		if (!m_Grip && !m_AvisoRightPanning && !avisoZoomRecentlyChanged)
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
		int sourceXInt = static_cast<int>(std::floor((sourceLeft - cachedRenderLeft) * sourceScaleX));
		int sourceYInt = static_cast<int>(std::floor((sourceTop - cachedRenderTop) * sourceScaleY));
		int sourceRightInt = static_cast<int>(std::ceil((sourceRight - cachedRenderLeft) * sourceScaleX));
		int sourceBottomInt = static_cast<int>(std::ceil((sourceBottom - cachedRenderTop) * sourceScaleY));
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
		cacheDrawn = drawCacheFallbackDuringInteraction();
	if (!cacheDrawn || !cacheHasWorkingMargin())
	{
		const double overscanRatio = 0.75;
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
			const double minRasterScale = 0.50;
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
			rasterScale = std::clamp(rasterScale, minRasterScale, targetRasterScale);

			CSMRRadar::AvisoRasterRenderRequest request;
			request.path = path;
			request.features = radar_screen->AvisoGeoJsonFeatureSnapshot;
			request.labels = radar_screen->AvisoGeoJsonLabelSnapshot;
			request.rasterWidth = max(1, static_cast<int>((renderPixelWidth * rasterScale) + 0.5));
			request.rasterHeight = max(1, static_cast<int>((renderPixelHeight * rasterScale) + 0.5));
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

			m_AvisoState->QueueRender(radar_screen, std::move(request));
		}
	}

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
		auto getColorWithLegacy = [&](const Value* section, const char* preferredKey, const char* legacyKey, const Color& fallback) -> Color
		{
			if (radar_screen->CurrentConfig != nullptr &&
				section != nullptr &&
				section->HasMember(preferredKey) &&
				(*section)[preferredKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*section)[preferredKey]);
			}
			if (radar_screen->CurrentConfig != nullptr &&
				legacyKey != nullptr &&
				section != nullptr &&
				section->HasMember(legacyKey) &&
				(*section)[legacyKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*section)[legacyKey]);
			}
			return fallback;
		};
		auto getConfigTargetColor = [&](const Value* targetsConfig, const char* key, Color fallback) -> Color
		{
			if (radar_screen->CurrentConfig == nullptr || targetsConfig == nullptr || key == nullptr || key[0] == '\0')
				return fallback;

			const Value& targets = *targetsConfig;
			Color resolvedColor;
			auto tryReadColor = [&](const Value& object, const char* colorKey) -> bool
			{
				if (!object.IsObject() || !object.HasMember(colorKey) || !object[colorKey].IsObject())
					return false;
				resolvedColor = radar_screen->CurrentConfig->getConfigColor(object[colorKey]);
				return true;
			};
			auto trySection = [&](const char* sectionKey, const char* colorKey) -> bool
			{
				if (!targets.HasMember(sectionKey) || !targets[sectionKey].IsObject())
					return false;
				return tryReadColor(targets[sectionKey], colorKey);
			};

			if (_stricmp(key, "airborne_departure") == 0 || _stricmp(key, "departure_airborne") == 0)
			{
				if (trySection("departure", "airborne"))
					return resolvedColor;
			}
			else if (_stricmp(key, "departure_gate") == 0)
			{
				if (trySection("departure", "gate"))
					return resolvedColor;
			}
			else if (_stricmp(key, "nofpl") == 0 || _stricmp(key, "no_fpl") == 0)
			{
				if (trySection("departure", "no_fpl"))
					return resolvedColor;
			}
			else if (_stricmp(key, "nsts") == 0 || _stricmp(key, "no_status") == 0)
			{
				if (trySection("departure", "no_status"))
					return resolvedColor;
			}
			else if (_stricmp(key, "push") == 0)
			{
				if (trySection("departure", "push"))
					return resolvedColor;
			}
			else if (_stricmp(key, "stup") == 0 || _stricmp(key, "startup") == 0)
			{
				if (trySection("departure", "startup"))
					return resolvedColor;
			}
			else if (_stricmp(key, "taxi") == 0)
			{
				if (trySection("departure", "taxi"))
					return resolvedColor;
			}
			else if (_stricmp(key, "depa") == 0 || _stricmp(key, "departure") == 0)
			{
				if (trySection("departure", "departure"))
					return resolvedColor;
			}
			else if (_stricmp(key, "airborne_arrival") == 0 || _stricmp(key, "arrival_airborne") == 0)
			{
				if (trySection("arrival", "airborne"))
					return resolvedColor;
			}
			else if (_stricmp(key, "arrival_gate") == 0)
			{
				if (trySection("arrival", "gate"))
					return resolvedColor;
			}
			else if (_stricmp(key, "arr") == 0 || _stricmp(key, "arrival_taxi") == 0 || _stricmp(key, "on_ground") == 0)
			{
				if (trySection("arrival", "on_ground"))
					return resolvedColor;
			}
			else if (_stricmp(key, "gate") == 0)
			{
				if (trySection("departure", "gate"))
					return resolvedColor;
				if (trySection("arrival", "gate"))
					return resolvedColor;
			}
			else
			{
				if (trySection("departure", key))
					return resolvedColor;
				if (trySection("arrival", key))
					return resolvedColor;
			}

			if (targets.HasMember("ground_icons") &&
				targets["ground_icons"].IsObject() &&
				tryReadColor(targets["ground_icons"], key))
			{
				return resolvedColor;
			}
			return fallback;
		};

		const Value* labelsSection = getProfileObjectSection("labels");
		const Value* rimcasSection = getProfileObjectSection("rimcas");
		const Value* targetsConfig = getProfileObjectSection("targets");
		const CSMRRadar::DisplayModeSettings displayModeSettings = radar_screen->GetActiveDisplayModeSettings();
		static const std::vector<StructuredTagColorRule> emptyStructuredTagRules;
		const std::vector<StructuredTagColorRule>& structuredTagRules =
			displayModeSettings.structuredRulesEnabled ? radar_screen->GetStructuredTagColorRules() : emptyStructuredTagRules;
		const bool rimcasLabelOnlySetting = getSectionBool(rimcasSection, "rimcas_label_only", true);
		const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
		const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
		const Color squawkErrorLabelColor = getSectionColor(labelsSection, "squawk_error_color", Color(255, 255, 0, 0));
		const bool showLegacyPrimaryTarget = getSectionBool(targetsConfig, "show_primary_target", false);
		auto getLegacyTargetColor = [&](const char* key, const Color& fallbackColor) -> Color
		{
			if (radar_screen->CurrentConfig != nullptr &&
				targetsConfig != nullptr &&
				key != nullptr &&
				key[0] != '\0' &&
				targetsConfig->HasMember(key) &&
				(*targetsConfig)[key].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*targetsConfig)[key]);
			}
			return fallbackColor;
		};

		bool tagProModeEnabled = displayModeSettings.requireAssignedSquawk;
		bool tagTowerModeEnabled = displayModeSettings.towerFilter;
		bool useAspeedForGate = false;
		bool airborneUseDepartureArrivalColoring = false;
		if (activeProfile.IsObject())
		{
			if (labelsSection != nullptr)
			{
				if (labelsSection->HasMember("use_speed_for_gate") && (*labelsSection)["use_speed_for_gate"].IsBool())
					useAspeedForGate = (*labelsSection)["use_speed_for_gate"].GetBool();
				else if (labelsSection->HasMember("use_aspeed_for_gate") && (*labelsSection)["use_aspeed_for_gate"].IsBool())
					useAspeedForGate = (*labelsSection)["use_aspeed_for_gate"].GetBool();

				if (labelsSection->HasMember("use_departure_arrival_coloring") && (*labelsSection)["use_departure_arrival_coloring"].IsBool())
					airborneUseDepartureArrivalColoring = (*labelsSection)["use_departure_arrival_coloring"].GetBool();
				else if (labelsSection->HasMember("airborne") &&
					(*labelsSection)["airborne"].IsObject() &&
					(*labelsSection)["airborne"].HasMember("use_departure_arrival_coloring") &&
					(*labelsSection)["airborne"]["use_departure_arrival_coloring"].IsBool())
				{
					airborneUseDepartureArrivalColoring = (*labelsSection)["airborne"]["use_departure_arrival_coloring"].GetBool();
				}
			}
		}

		const CSMRRadar::CorrelationSettings correlationSettings = radar_screen->BuildCorrelationSettings();
		const int transitionAltitude = radar_screen->GetPlugIn()->GetTransitionAltitude();
		const std::string activeAirport = radar_screen->getActiveAirport();
		const std::string activeAirportUpper = ToUpperAsciiCopy(activeAirport);
		const std::string frameIconStyle = radar_screen->GetActiveTargetIconStyle();
		const bool useNovaIconStyle = (frameIconStyle == "nova");
		const bool useDiamondIconStyle = (frameIconStyle == "diamond");
		const bool useRealisticIconStyle = (frameIconStyle == "realistic");
		const bool smallIconBoostEnabled = radar_screen->GetSmallTargetIconBoostEnabled();
		const bool fixedPixelIconSize = radar_screen->GetFixedPixelTargetIconSizeEnabled();
		const double smallIconBoostFactor = std::clamp(radar_screen->GetSmallTargetIconBoostFactor(), 0.5, 4.0);
		const double smallIconBoostResolutionScale = std::clamp(radar_screen->GetSmallTargetIconBoostResolutionScale(), 1.0, 2.0);
		const double fixedTriangleScale = std::clamp(radar_screen->GetFixedPixelTriangleIconScale(), 0.1, 3.0);
		const double pixPerMeter = max(0.0, static_cast<double>(max(1, m_AvisoScale)) / kAvisoMetersPerNm);
		const Color symbolWhiteColor = radar_screen->ColorManager != nullptr
			? radar_screen->ColorManager->get_corrected_color("symbol", Color::White)
			: Color::White;
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

		CRadarTarget aselTarget = radar_screen->GetPlugIn()->RadarTargetSelectASEL();
		const char* aselCallsign = aselTarget.IsValid() ? aselTarget.GetCallsign() : nullptr;
		vector<POINT> appAreaVect = {
			viewportRect.TopLeft(),
			{ viewportRect.right, viewportRect.top },
			viewportRect.BottomRight(),
			{ viewportRect.left, viewportRect.bottom }
		};
		std::vector<PointF> patatoidePolygonPoints;
		auto drawPatatoidePolygon = [&](const std::map<int, CSMRRadar::POINT2>& sourcePoints, const Color& fillColor)
		{
			if (sourcePoints.size() < 3)
				return;

			patatoidePolygonPoints.clear();
			patatoidePolygonPoints.reserve(sourcePoints.size());
			for (const auto& sourcePoint : sourcePoints)
			{
				const Gdiplus::PointF point = projectPoint(sourcePoint.second.y, sourcePoint.second.x);
				patatoidePolygonPoints.emplace_back(point);
			}

			if (patatoidePolygonPoints.size() < 3)
				return;

			const Color drawColor = radar_screen->ColorManager != nullptr
				? radar_screen->ColorManager->get_corrected_color("afterglow", fillColor)
				: fillColor;
			SolidBrush polygonBrush(drawColor);
			gdi->FillPolygon(&polygonBrush, patatoidePolygonPoints.data(), static_cast<INT>(patatoidePolygonPoints.size()));
		};

		auto drawTrailHistory = [&](const std::string& callsign, CRadarTarget target, int reportedGs)
		{
			if (reportedGs <= 5)
				return;

			const int trailNumber = reportedGs > 50 ? radar_screen->Trail_App : radar_screen->Trail_Gnd;
			if (trailNumber <= 0)
				return;

			CRadarTargetPositionData previousPos = target.GetPreviousPosition(target.GetPosition());
			if (useNovaIconStyle)
			{
				if (radar_screen->Afterglow && showLegacyPrimaryTarget)
				{
					const auto patatoideIt = radar_screen->Patatoides.find(callsign);
					if (patatoideIt != radar_screen->Patatoides.end())
					{
						drawPatatoidePolygon(
							patatoideIt->second.History_one_points,
							getLegacyTargetColor("history_one_color", Color(255, 0, 255, 255)));
						drawPatatoidePolygon(
							patatoideIt->second.History_two_points,
							getLegacyTargetColor("history_two_color", Color(255, 0, 219, 219)));
						drawPatatoidePolygon(
							patatoideIt->second.History_three_points,
							getLegacyTargetColor("history_three_color", Color(255, 0, 183, 183)));
					}
				}

				SolidBrush trailBrush(symbolWhiteColor);
				for (int j = 1; j <= trailNumber && previousPos.IsValid(); ++j)
				{
					const CPosition position = previousPos.GetPosition();
					if (positionNearViewport(position))
					{
						POINT point = projectTargetPosition(position);
						if (pointInViewport(point, 2))
							gdi->FillRectangle(&trailBrush, point.x - 1, point.y - 1, 2, 2);
					}
					previousPos = target.GetPreviousPosition(previousPos);
				}
				return;
			}

			for (int j = 1; j <= trailNumber && previousPos.IsValid(); ++j)
			{
				const CPosition position = previousPos.GetPosition();
				if (!positionNearViewport(position))
				{
					previousPos = target.GetPreviousPosition(previousPos);
					continue;
				}

				POINT point = projectTargetPosition(position);
				if (!pointInViewport(point, 60))
				{
					previousPos = target.GetPreviousPosition(previousPos);
					continue;
				}

				int diameterPx = 6;
				if (pixPerMeter > 0.0)
					diameterPx = int((pixPerMeter * 10.0) + 0.5);
				diameterPx = std::clamp(diameterPx, 2, 50);

				double shrink = 1.0 - (0.15 * (j - 1));
				if (shrink < 0.2)
					shrink = 0.2;
				diameterPx = max(2, int((diameterPx * shrink) + 0.5));
				const int radius = diameterPx / 2;
				const double t = (trailNumber > 1) ? double(j - 1) / double(trailNumber - 1) : 0.0;
				auto lerp = [](double a, double b, double amount) { return a + ((b - a) * amount); };
				Color bubbleColor(
					static_cast<BYTE>(std::clamp(int(lerp(200.0, 40.0, t) + 0.5), 0, 255)),
					static_cast<BYTE>(std::clamp(int(lerp(255.0, 120.0, t) + 0.5), 0, 255)),
					static_cast<BYTE>(std::clamp(int(lerp(255.0, 150.0, t) + 0.5), 0, 255)),
					static_cast<BYTE>(std::clamp(int(lerp(255.0, 190.0, t) + 0.5), 0, 255)));
				Gdiplus::Pen ringPen(bubbleColor, Gdiplus::REAL(1.5f));
				gdi->DrawEllipse(&ringPen, point.x - radius, point.y - radius, diameterPx, diameterPx);
				previousPos = target.GetPreviousPosition(previousPos);
			}
		};

		auto fallbackTypeForWtc = [](char wtcChar) -> std::string
		{
			switch (std::toupper(static_cast<unsigned char>(wtcChar)))
			{
			case 'L': return "c172";
			case 'M': return "a320";
			case 'H': return "b77w";
			case 'J': return "a388";
			default: return "a320";
			}
		};

		auto resolveTargetColor = [&](CFlightPlan fp, bool isCorrelated, int reportedGs, bool isOnRunway, bool isDepartureTarget, bool hasNoFlightPlan) -> Color
		{
			Color targetColor = symbolWhiteColor;
			auto applyTargetTint = [&](const Color& color)
			{
				targetColor = radar_screen->ColorManager != nullptr
					? radar_screen->ColorManager->get_corrected_color("symbol", color)
					: color;
			};

			const bool isAirborneTarget = reportedGs > 50;
			GroundStateCategory groundStateCat = GroundStateCategory::Unknown;
			if (fp.IsValid())
				groundStateCat = classifyGroundState(fp.GetGroundState(), reportedGs, isOnRunway);

			if (hasNoFlightPlan)
			{
				applyTargetTint(getConfigTargetColor(targetsConfig, "nofpl", getConfigTargetColor(targetsConfig, "gate", Color(255, 128, 128, 128))));
			}
			else if (isAirborneTarget)
			{
				if (isDepartureTarget)
					applyTargetTint(getConfigTargetColor(targetsConfig, "airborne_departure", getConfigTargetColor(targetsConfig, "depa", Color(255, 240, 240, 240))));
				else
					applyTargetTint(getConfigTargetColor(targetsConfig, "airborne_arrival", getConfigTargetColor(targetsConfig, "arr", Color(255, 120, 190, 240))));
			}
			else if (isDepartureTarget)
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
					applyTargetTint(getConfigTargetColor(targetsConfig, "departure_gate", getConfigTargetColor(targetsConfig, "gate", Color(255, 165, 165, 165))));
					break;
				case GroundStateCategory::Push:
					applyTargetTint(getConfigTargetColor(targetsConfig, "push", Color(255, 253, 218, 13)));
					break;
				case GroundStateCategory::Stup:
					applyTargetTint(getConfigTargetColor(targetsConfig, "stup", Color(255, 253, 218, 13)));
					break;
				case GroundStateCategory::Taxi:
					applyTargetTint(getConfigTargetColor(targetsConfig, "taxi", Color(255, 240, 240, 240)));
					break;
				case GroundStateCategory::Depa:
					applyTargetTint(getConfigTargetColor(targetsConfig, "depa", getConfigTargetColor(targetsConfig, "taxi", Color(255, 240, 240, 240))));
					break;
				case GroundStateCategory::Nsts:
					applyTargetTint(getConfigTargetColor(targetsConfig, "nsts", getConfigTargetColor(targetsConfig, "departure_gate", getConfigTargetColor(targetsConfig, "gate", Color(255, 165, 165, 165)))));
					break;
				default:
					break;
				}
			}
			else
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
				case GroundStateCategory::Nsts:
					applyTargetTint(getConfigTargetColor(targetsConfig, "arrival_gate", getConfigTargetColor(targetsConfig, "gate", Color(255, 165, 165, 165))));
					break;
				default:
					applyTargetTint(getConfigTargetColor(targetsConfig, "arr", getConfigTargetColor(targetsConfig, "arrival_gate", getConfigTargetColor(targetsConfig, "gate", Color(255, 165, 165, 165)))));
					break;
				}
			}

			if (!isCorrelated && reportedGs >= 3)
				applyTargetTint(getConfigTargetColor(targetsConfig, "uncorrelated", targetColor));

			return targetColor;
		};

		auto drawConfiguredIcon = [&](CRadarTarget rt, CFlightPlan fp, const std::string& rtCallsign, const CRadarTargetPositionData& rtPositionData, const POINT& targetPoint, bool isCorrelated, bool isDepartureTarget, bool hasNoFlightPlan, bool isOnRunway) -> int
		{
			const int reportedGs = rtPositionData.GetReportedGS();
			if (useNovaIconStyle)
				return 18;

			Color targetColor = resolveTargetColor(fp, isCorrelated, reportedGs, isOnRunway, isDepartureTarget, hasNoFlightPlan);
			if (targetColor.GetAlpha() < 32)
				targetColor = Color(255, targetColor.GetR(), targetColor.GetG(), targetColor.GetB());
			double headingDeg = static_cast<double>(rtPositionData.GetReportedHeadingTrueNorth());
			if (headingDeg < 0.0 || headingDeg >= 360.0)
				headingDeg = rt.GetTrackHeading();

			char wtc = fp.IsValid() ? fp.GetFlightPlanData().GetAircraftWtc() : '\0';
			std::string acType;
			if (fp.IsValid())
			{
				const char* acTypeRaw = fp.GetFlightPlanData().GetAircraftFPType();
				acType = acTypeRaw != nullptr ? acTypeRaw : "";
			}
			if (acType.size() > 4)
				acType = acType.substr(0, 4);
			std::transform(acType.begin(), acType.end(), acType.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			std::string iconType = acType;
			Gdiplus::Bitmap* iconBmp = nullptr;
			auto specIt = radar_screen->AircraftSpecs.end();
			if (useRealisticIconStyle)
			{
				iconBmp = radar_screen->GetAircraftIcon(iconType);
				if (iconBmp == nullptr)
				{
					iconType = fallbackTypeForWtc(wtc);
					iconBmp = radar_screen->GetAircraftIcon(iconType);
				}
				specIt = radar_screen->AircraftSpecs.find(acType);
				if (specIt == radar_screen->AircraftSpecs.end())
					specIt = radar_screen->AircraftSpecs.find(iconType);
				if (specIt == radar_screen->AircraftSpecs.end())
					specIt = radar_screen->AircraftSpecs.find(fallbackTypeForWtc(wtc));
			}

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
				double lengthMeters = 0.0;
				double spanMeters = 0.0;
				if (specIt != radar_screen->AircraftSpecs.end())
				{
					lengthMeters = specIt->second.length;
					spanMeters = specIt->second.wingspan;
				}
				if (lengthMeters <= 0.0 || spanMeters <= 0.0)
				{
					switch (std::toupper(static_cast<unsigned char>(wtc)))
					{
					case 'L': lengthMeters = 28.0; spanMeters = 28.0; break;
					case 'M': lengthMeters = 40.0; spanMeters = 36.0; break;
					case 'H': lengthMeters = 60.0; spanMeters = 60.0; break;
					case 'J': lengthMeters = 72.0; spanMeters = 80.0; break;
					default: lengthMeters = 40.0; spanMeters = 36.0; break;
					}
				}

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

				CPosition nosePos = radar_screen->Haversine(rtPositionData.GetPosition(), headingDeg, 50.0);
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
				}
				return max(drawPixelW, drawPixelH);
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

				CPosition nosePos = radar_screen->Haversine(rtPositionData.GetPosition(), headingDeg, 50.0);
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
			const CPosition acPos = rtPositionData.GetPosition();
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
			gdi->MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &fontMeasureRect);
			tagBlankWidth = max(2, static_cast<int>(fontMeasureRect.GetRight()));

			fontMeasureRect = RectF(0, 0, 0, 0);
			gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
				tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &fontMeasureRect);
			tagOneLineHeight = max(1, static_cast<int>(fontMeasureRect.GetBottom()));
			if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
			{
				RectF boldMeasureRect;
				gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
					tagBoldFont, PointF(0, 0), &Gdiplus::StringFormat(), &boldMeasureRect);
				tagOneLineHeight = max(tagOneLineHeight, static_cast<int>(boldMeasureRect.GetBottom()));
			}
		}

		std::map<std::string, std::vector<std::string>> tagDefinitionLineCache;
		auto getCachedDefinitionLines = [&](const std::string& typeKey, const char* statusKey) -> const std::vector<std::string>&
		{
			const std::string normalizedStatus = (statusKey != nullptr && statusKey[0] != '\0') ? statusKey : "default";
			const std::string cacheKey = typeKey + "|" + normalizedStatus;
			auto it = tagDefinitionLineCache.find(cacheKey);
			if (it == tagDefinitionLineCache.end())
			{
				it = tagDefinitionLineCache.emplace(
					cacheKey,
					radar_screen->GetTagDefinitionLineStrings(
						typeKey,
						false,
						CSMRRadar::TagDefinitionEditorMaxLines,
						false,
						normalizedStatus)).first;
			}
			return it->second;
		};
		auto definitionLinesHaveContent = [](const std::vector<std::string>& lines) -> bool
		{
			for (const std::string& line : lines)
			{
				if (!TrimAsciiWhitespaceCopy(line).empty())
					return true;
			}
			return false;
		};

		auto drawTag = [&](CRadarTarget rt, CFlightPlan fp, const std::string& rtCallsign, const POINT& targetPoint, bool isASEL, bool isCorrelated, int reportedGs, bool isOnRunway)
		{
			if (tagRegularFont == nullptr)
				return;
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

			const char* fpDestination = fp.IsValid() ? fp.GetFlightPlanData().GetDestination() : nullptr;
			const char* fpOrigin = fp.IsValid() ? fp.GetFlightPlanData().GetOrigin() : nullptr;
			const char* fpPlanType = fp.IsValid() ? fp.GetFlightPlanData().GetPlanType() : nullptr;

			CSMRRadar::TagTypes tagType = CSMRRadar::TagTypes::Departure;
			CSMRRadar::TagTypes colorTagType = CSMRRadar::TagTypes::Departure;
			if (fpDestination != nullptr && strcmp(fpDestination, activeAirport.c_str()) == 0)
			{
				if (fpOrigin == nullptr || strcmp(fpOrigin, activeAirport.c_str()) != 0)
				{
					tagType = CSMRRadar::TagTypes::Arrival;
					colorTagType = CSMRRadar::TagTypes::Arrival;
				}
			}
			if (reportedGs > 50)
			{
				tagType = CSMRRadar::TagTypes::Airborne;
				if (!airborneUseDepartureArrivalColoring)
					colorTagType = CSMRRadar::TagTypes::Airborne;
			}
			if (!isCorrelated && reportedGs >= 3)
			{
				tagType = CSMRRadar::TagTypes::Uncorrelated;
				colorTagType = CSMRRadar::TagTypes::Uncorrelated;
			}

			map<string, string> tagReplacingMap = CSMRRadar::GenerateTagData(
				rt,
				fp,
				isASEL,
				isCorrelated,
				tagProModeEnabled,
				transitionAltitude,
				useAspeedForGate,
				activeAirport,
				rtCallsign);

			map<string, int> tagClickableMap;
			auto addClickableToken = [&](const char* tokenKey, int clickType)
			{
				auto tokenIt = tagReplacingMap.find(tokenKey);
				if (tokenIt != tagReplacingMap.end() && !tokenIt->second.empty())
					tagClickableMap[tokenIt->second] = clickType;
			};
			addClickableToken("callsign", TAG_CITEM_CALLSIGN);
			addClickableToken("actype", TAG_CITEM_FPBOX);
			addClickableToken("sctype", TAG_CITEM_FPBOX);
			addClickableToken("sqerror", TAG_CITEM_FPBOX);
			addClickableToken("deprwy", TAG_CITEM_RWY);
			addClickableToken("seprwy", TAG_CITEM_RWY);
			addClickableToken("arvrwy", TAG_CITEM_RWY);
			addClickableToken("srvrwy", TAG_CITEM_RWY);
			addClickableToken("gate", TAG_CITEM_GATE);
			addClickableToken("sate", TAG_CITEM_GATE);
			addClickableToken("flightlevel", TAG_CITEM_NO);
			addClickableToken("gs", TAG_CITEM_NO);
			addClickableToken("tobt", TAG_CITEM_NO);
			addClickableToken("tsat", TAG_CITEM_NO);
			addClickableToken("ttot", TAG_CITEM_NO);
			addClickableToken("asat", TAG_CITEM_NO);
			addClickableToken("aobt", TAG_CITEM_NO);
			addClickableToken("atot", TAG_CITEM_NO);
			addClickableToken("asrt", TAG_CITEM_NO);
			addClickableToken("aort", TAG_CITEM_NO);
			addClickableToken("ctot", TAG_CITEM_NO);
			addClickableToken("event_booking", TAG_CITEM_NO);
			addClickableToken("tendency", TAG_CITEM_NO);
			addClickableToken("wake", TAG_CITEM_FPBOX);
			addClickableToken("tssr", TAG_CITEM_NO);
			addClickableToken("asid", TAG_CITEM_SID);
			addClickableToken("ssid", TAG_CITEM_SID);
			addClickableToken("sid", TAG_CITEM_SID);
			addClickableToken("shid", TAG_CITEM_SID);
			addClickableToken("origin", TAG_CITEM_FPBOX);
			addClickableToken("dest", TAG_CITEM_FPBOX);
			addClickableToken("systemid", TAG_CITEM_MANUALCORRELATE);
			addClickableToken("gstatus", TAG_CITEM_GROUNDSTATUS);
			addClickableToken("groundstatus", TAG_CITEM_GROUNDSTATUS);
			addClickableToken("clearance", TAG_CITEM_CLEARANCE);
			addClickableToken("uk_stand", TAG_CITEM_UKSTAND);
			addClickableToken("remark", TAG_CITEM_REMARK);
			addClickableToken("scratchpad", TAG_CITEM_SCRATCHPAD);

			std::string definitionTypeKey = AvisoTagTypeKey(tagType);
			std::string ruleTagTypeKey = definitionTypeKey;
			const char* statusDefinitionKey = nullptr;
			const auto actypeIt = tagReplacingMap.find("actype");
			const bool noFlightPlanTag = (actypeIt != tagReplacingMap.end() && actypeIt->second == "NoFPL");
			if (noFlightPlanTag && (tagType == CSMRRadar::TagTypes::Departure || tagType == CSMRRadar::TagTypes::Arrival))
			{
				statusDefinitionKey = "nofpl";
			}
			else if (tagType == CSMRRadar::TagTypes::Airborne)
			{
				bool airborneArrival = false;
				if (fpDestination != nullptr &&
					strcmp(fpDestination, activeAirport.c_str()) == 0 &&
					(fpOrigin == nullptr || strcmp(fpOrigin, activeAirport.c_str()) != 0))
				{
					airborneArrival = true;
				}
				definitionTypeKey = airborneArrival ? "arrival" : "departure";
				ruleTagTypeKey = definitionTypeKey;
				statusDefinitionKey = airborneArrival
					? (isOnRunway ? "airarr_onrunway" : "airarr")
					: (isOnRunway ? "airdep_onrunway" : "airdep");
			}
			else if (fp.IsValid())
			{
				const GroundStateCategory status = classifyGroundState(fp.GetGroundState(), reportedGs, isOnRunway);
				if (tagType == CSMRRadar::TagTypes::Departure)
				{
					switch (status)
					{
					case GroundStateCategory::Taxi: statusDefinitionKey = "taxi"; break;
					case GroundStateCategory::Push: statusDefinitionKey = "push"; break;
					case GroundStateCategory::Stup: statusDefinitionKey = "stup"; break;
					case GroundStateCategory::Nsts: statusDefinitionKey = "nsts"; break;
					case GroundStateCategory::Depa: statusDefinitionKey = "depa"; break;
					default: break;
					}
				}
			}

			const std::vector<std::string>* definitionLines = &getCachedDefinitionLines(definitionTypeKey, statusDefinitionKey);
			if (!definitionLinesHaveContent(*definitionLines))
			{
				if (statusDefinitionKey != nullptr &&
					(strcmp(statusDefinitionKey, "airarr_onrunway") == 0 || strcmp(statusDefinitionKey, "airdep_onrunway") == 0))
				{
					definitionLines = &getCachedDefinitionLines(
						definitionTypeKey,
						strcmp(statusDefinitionKey, "airarr_onrunway") == 0 ? "airarr" : "airdep");
				}

				if (!definitionLinesHaveContent(*definitionLines))
					definitionLines = &getCachedDefinitionLines(definitionTypeKey, "default");
			}

			std::vector<VacdmColorRuleDefinition> vacdmTagColorRules;
			std::vector<RunwayColorRuleDefinition> runwayTagColorRules;
			CollectVacdmColorRulesFromLineTexts(*definitionLines, vacdmTagColorRules);
			CollectRunwayColorRulesFromLineTexts(*definitionLines, runwayTagColorRules);

			VacdmPilotData vacdmRulePilotData;
			const bool hasVacdmRulePilotData = TryGetVacdmPilotDataForTarget(rt, fp, vacdmRulePilotData);
			VacdmColorRuleOverrides tagColorRuleOverrides =
				EvaluateVacdmColorRules(vacdmTagColorRules, hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
			MergeColorRuleOverrides(tagColorRuleOverrides, EvaluateRunwayColorRules(runwayTagColorRules, tagReplacingMap));
			MergeColorRuleOverrides(
				tagColorRuleOverrides,
				EvaluateStructuredTagColorRules(
					structuredTagRules,
					ruleTagTypeKey,
					statusDefinitionKey,
					false,
					tagReplacingMap,
					hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr));

			struct RenderedTagElement
			{
				std::string token;
				std::string text;
				bool bold = false;
				bool hasCustomColor = false;
				int colorR = 255;
				int colorG = 255;
				int colorB = 255;
				int measuredWidth = 0;
				int measuredHeight = 0;
				bool isClearanceToken = false;
			};
			vector<vector<RenderedTagElement>> renderedLines;
			int tagWidth = 0;
			int tagHeight = 0;
			for (const std::string& lineText : *definitionLines)
			{
				vector<string> rawElements = SplitDefinitionTokens(lineText);
				if (rawElements.empty())
					continue;

				vector<RenderedTagElement> renderedLine;
				renderedLine.reserve(rawElements.size());
				bool allEmpty = true;
				int tempTagWidth = 0;
				for (const std::string& rawElement : rawElements)
				{
					DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawElement);
					const std::string baseToken = styledToken.token.empty() ? rawElement : styledToken.token;
					VacdmColorRuleDefinition vacdmRuleToken;
					if (TryParseVacdmColorRuleToken(baseToken, vacdmRuleToken))
						continue;
					RunwayColorRuleDefinition runwayRuleToken;
					if (TryParseRunwayColorRuleToken(baseToken, runwayRuleToken))
						continue;

					string element;
					string clearanceNotClearedText;
					string clearanceClearedText;
					const bool isClearanceToken = TryParseClearanceTokenDisplay(baseToken, clearanceNotClearedText, clearanceClearedText);
					if (isClearanceToken)
					{
						if (fp.IsValid() && isCorrelated)
							element = fp.GetClearenceFlag() ? clearanceClearedText : clearanceNotClearedText;
					}
					else
					{
						auto exactMatch = tagReplacingMap.find(baseToken);
						if (exactMatch != tagReplacingMap.end())
							element = exactMatch->second;
						else
						{
							element = baseToken;
							for (const auto& kv : tagReplacingMap)
							{
								if (element.find(kv.first) != std::string::npos)
									replaceAll(element, kv.first, kv.second);
							}
						}
					}

					RenderedTagElement renderedElement;
					renderedElement.token = baseToken;
					renderedElement.text = element;
					renderedElement.bold = styledToken.bold;
					renderedElement.hasCustomColor = styledToken.hasCustomColor;
					renderedElement.colorR = styledToken.colorR;
					renderedElement.colorG = styledToken.colorG;
					renderedElement.colorB = styledToken.colorB;
					renderedElement.isClearanceToken = isClearanceToken;

					if (!element.empty())
					{
						allEmpty = false;
						wstring wstr(element.begin(), element.end());
						Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
						gdi->MeasureString(wstr.c_str(), wcslen(wstr.c_str()), measureFont, PointF(0, 0), &Gdiplus::StringFormat(), &measureRect);
						renderedElement.measuredWidth = static_cast<int>(measureRect.GetRight());
						renderedElement.measuredHeight = static_cast<int>(measureRect.GetBottom());
						tempTagWidth += renderedElement.measuredWidth;
					}
					renderedLine.push_back(std::move(renderedElement));
				}

				if (allEmpty)
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
				fallbackElement.token = "callsign";
				fallbackElement.text = callsignText;
				wstring wstr(callsignText.begin(), callsignText.end());
				gdi->MeasureString(wstr.c_str(), wcslen(wstr.c_str()), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &measureRect);
				fallbackElement.measuredWidth = static_cast<int>(measureRect.GetRight());
				fallbackElement.measuredHeight = static_cast<int>(measureRect.GetBottom());
				tagWidth = fallbackElement.measuredWidth;
				tagHeight = oneLineHeight;
				renderedLines.push_back({ fallbackElement });
			}
			if (tagHeight > 0)
				tagHeight -= 2;

			const std::string colorTagTypeKey = AvisoTagTypeKey(colorTagType);
			const Value* colorTagLabelSection = nullptr;
			if (labelsSection != nullptr &&
				labelsSection->HasMember(colorTagTypeKey.c_str()) &&
				(*labelsSection)[colorTagTypeKey.c_str()].IsObject())
			{
				colorTagLabelSection = &(*labelsSection)[colorTagTypeKey.c_str()];
			}

			Color definedBackgroundColor = Color(255, 53, 126, 187);
			Color definedBackgroundOnRunwayColor = definedBackgroundColor;
			Color definedTextColor = Color::White;
			if (colorTagType == CSMRRadar::TagTypes::Departure)
			{
				definedBackgroundColor = getColorWithLegacy(colorTagLabelSection, "background_no_status_color", "gate_color", Color(255, 53, 126, 187));
				definedBackgroundOnRunwayColor = getColorWithLegacy(colorTagLabelSection, "background_on_runway_color", "on_runway_color", definedBackgroundColor);
				definedTextColor = getColorWithLegacy(colorTagLabelSection, "text_on_ground_color", "text_color", Color::White);
			}
			else if (colorTagType == CSMRRadar::TagTypes::Arrival)
			{
				definedBackgroundColor = getColorWithLegacy(colorTagLabelSection, "background_on_ground_color", "background_color", Color(255, 191, 87, 91));
				definedBackgroundOnRunwayColor = getColorWithLegacy(colorTagLabelSection, "background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
				definedTextColor = getColorWithLegacy(colorTagLabelSection, "text_on_ground_color", "text_color", Color::White);
			}
			else if (colorTagType == CSMRRadar::TagTypes::Uncorrelated)
			{
				definedBackgroundColor = getColorWithLegacy(colorTagLabelSection, "background_on_ground_color", "background_color", Color(255, 150, 22, 135));
				definedBackgroundOnRunwayColor = getColorWithLegacy(colorTagLabelSection, "background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
				definedTextColor = getColorWithLegacy(colorTagLabelSection, "text_on_ground_color", "text_color", Color::White);
			}
			else
			{
				definedBackgroundColor = getSectionColor(colorTagLabelSection, "background_color", Color(255, 53, 126, 187));
				definedBackgroundOnRunwayColor = getSectionColor(colorTagLabelSection, "background_color_on_runway", definedBackgroundColor);
				definedTextColor = getSectionColor(colorTagLabelSection, "text_color", Color::White);
			}

			if (colorTagType == CSMRRadar::TagTypes::Departure)
			{
				std::string sidText;
				auto asidIt = tagReplacingMap.find("asid");
				if (asidIt != tagReplacingMap.end())
					sidText = asidIt->second;
				if (sidText.empty())
				{
					auto sidIt = tagReplacingMap.find("sid");
					if (sidIt != tagReplacingMap.end())
						sidText = sidIt->second;
				}
				if (!sidText.empty() &&
					radar_screen->CurrentConfig != nullptr &&
					radar_screen->CurrentConfig->isSidColorAvail(sidText, activeAirport))
				{
					definedBackgroundColor = radar_screen->CurrentConfig->getSidColor(sidText, activeAirport);
				}
				if (fpPlanType != nullptr &&
					fpPlanType[0] == 'I' &&
					sidText.empty() &&
					colorTagLabelSection != nullptr)
				{
					definedBackgroundColor = getColorWithLegacy(colorTagLabelSection, "background_no_sid_color", "nosid_color", definedBackgroundColor);
				}

				if (colorTagLabelSection != nullptr)
				{
					GroundStateCategory departureStatus = GroundStateCategory::Unknown;
					if (fp.IsValid())
						departureStatus = classifyGroundState(fp.GetGroundState(), reportedGs, isOnRunway);

					const char* statusColorKey = nullptr;
					const char* legacyStatusColorKey = nullptr;
					switch (departureStatus)
					{
					case GroundStateCategory::Taxi:
						statusColorKey = "background_taxi_color";
						legacyStatusColorKey = "taxi";
						break;
					case GroundStateCategory::Push:
						statusColorKey = "background_push_color";
						legacyStatusColorKey = "push";
						break;
					case GroundStateCategory::Stup:
						statusColorKey = "background_startup_color";
						legacyStatusColorKey = "stup";
						break;
					case GroundStateCategory::Depa:
						statusColorKey = "background_departure_color";
						legacyStatusColorKey = "depa";
						break;
					default:
						statusColorKey = "background_no_status_color";
						legacyStatusColorKey = "nsts";
						break;
					}

					if (statusColorKey != nullptr &&
						colorTagLabelSection->HasMember(statusColorKey) &&
						(*colorTagLabelSection)[statusColorKey].IsObject())
					{
						definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)[statusColorKey]);
					}
					else if (legacyStatusColorKey != nullptr &&
						colorTagLabelSection->HasMember("status_background_colors") &&
						(*colorTagLabelSection)["status_background_colors"].IsObject() &&
						(*colorTagLabelSection)["status_background_colors"].HasMember(legacyStatusColorKey) &&
						(*colorTagLabelSection)["status_background_colors"][legacyStatusColorKey].IsObject())
					{
						definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)["status_background_colors"][legacyStatusColorKey]);
					}
				}
			}
			if (noFlightPlanTag && colorTagLabelSection != nullptr)
				definedBackgroundColor = getColorWithLegacy(colorTagLabelSection, "background_no_fpl_color", "nofpl_color", definedBackgroundColor);

			if (tagType == CSMRRadar::TagTypes::Airborne && fp.IsValid() && isCorrelated)
			{
				bool airborneDeparture = true;
				if (fpOrigin != nullptr && fpOrigin[0] != '\0' && !activeAirport.empty())
					airborneDeparture = (_stricmp(fpOrigin, activeAirport.c_str()) == 0);

				const char* runwaySectionKey = airborneDeparture ? "departure" : "arrival";
				if (labelsSection != nullptr &&
					labelsSection->HasMember(runwaySectionKey) &&
					(*labelsSection)[runwaySectionKey].IsObject())
				{
					const Value& runwaySection = (*labelsSection)[runwaySectionKey];
					if (runwaySection.HasMember("background_airborne_color") && runwaySection["background_airborne_color"].IsObject())
						definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_airborne_color"]);
					if (runwaySection.HasMember("text_airborne_color") && runwaySection["text_airborne_color"].IsObject())
						definedTextColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["text_airborne_color"]);

					if (runwaySection.HasMember("background_on_runway_color") && runwaySection["background_on_runway_color"].IsObject())
						definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_on_runway_color"]);
					else if (airborneDeparture && runwaySection.HasMember("on_runway_color") && runwaySection["on_runway_color"].IsObject())
						definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["on_runway_color"]);
					else if (runwaySection.HasMember("background_color_on_runway") && runwaySection["background_color_on_runway"].IsObject())
						definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_color_on_runway"]);
				}
				else if (labelsSection != nullptr &&
					labelsSection->HasMember("airborne") &&
					(*labelsSection)["airborne"].IsObject())
				{
					const Value& airborneLabel = (*labelsSection)["airborne"];
					const char* bgKey = airborneDeparture ? "departure_background_color" : "arrival_background_color";
					const char* textKey = airborneDeparture ? "departure_text_color" : "arrival_text_color";
					const char* bgOnRunwayKey = airborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
					if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
						definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgKey]);
					if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
						definedTextColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[textKey]);
					if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
						definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
				}
			}

			if (tagColorRuleOverrides.hasTagColor)
			{
				definedBackgroundColor = Color(tagColorRuleOverrides.tagA, tagColorRuleOverrides.tagR, tagColorRuleOverrides.tagG, tagColorRuleOverrides.tagB);
				definedBackgroundOnRunwayColor = definedBackgroundColor;
			}
			if (tagColorRuleOverrides.hasTextColor)
			{
				definedTextColor = Color(tagColorRuleOverrides.textA, tagColorRuleOverrides.textR, tagColorRuleOverrides.textG, tagColorRuleOverrides.textB);
			}

			Color tagBackgroundColor = definedBackgroundColor;
			if (radar_screen->RimcasInstance != nullptr)
			{
				tagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(
					rtCallsign,
					definedBackgroundColor,
					definedBackgroundOnRunwayColor,
					rimcasStageOneColor,
					rimcasStageTwoColor);
				if (rimcasLabelOnlySetting)
				{
					tagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(
						rtCallsign,
						definedBackgroundColor,
						definedBackgroundOnRunwayColor);
				}
			}
			if (radar_screen->ColorManager != nullptr)
				tagBackgroundColor = radar_screen->ColorManager->get_corrected_color("label", tagBackgroundColor);

			CRect tagBackgroundRect(
				tagCenter.x - (tagWidth / 2),
				tagCenter.y - (tagHeight / 2),
				tagCenter.x + (tagWidth / 2),
				tagCenter.y + (tagHeight / 2));
			tagBackgroundRect.InflateRect(3, 3);
			tagBackgroundRect.NormalizeRect();
			if (!rectIntersectsViewport(tagBackgroundRect) && !pointInViewport(targetPoint, 20))
				return;

			m_TagAreas[rtCallsign] = tagBackgroundRect;
			radar_screen->AddScreenObject(m_Id, rtCallsign.c_str(), tagBackgroundRect, true, radar_screen->GetBottomLine(rtCallsign.c_str()).c_str());

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

			SolidBrush fontBrush(radar_screen->ColorManager != nullptr
				? radar_screen->ColorManager->get_corrected_color("label", definedTextColor)
				: definedTextColor);
			SolidBrush squawkErrorBrush(radar_screen->ColorManager != nullptr
				? radar_screen->ColorManager->get_corrected_color("label", squawkErrorLabelColor)
				: squawkErrorLabelColor);
			SolidBrush alertTextBrushStageOne(radar_screen->ColorManager != nullptr
				? radar_screen->ColorManager->get_corrected_color("label", Color(255, 30, 30, 30))
				: Color(255, 30, 30, 30));
			SolidBrush alertTextBrushStageTwo(radar_screen->ColorManager != nullptr
				? radar_screen->ColorManager->get_corrected_color("label", Color(255, 255, 255, 255))
				: Color(255, 255, 255, 255));

			const int textLeft = tagBackgroundRect.left + 3;
			const int textTop = tagBackgroundRect.top + 3;
			const int textWidth = max(0, tagBackgroundRect.Width() - 6);
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
					SolidBrush* drawBrush = &fontBrush;
					auto sqErrorIt = tagReplacingMap.find("sqerror");
					if (sqErrorIt != tagReplacingMap.end() && !sqErrorIt->second.empty() && renderedElement.text == sqErrorIt->second)
						drawBrush = &squawkErrorBrush;
					if (radar_screen->RimcasInstance != nullptr)
					{
						const CRimcas::RimcasAlertTypes rimcasStage = radar_screen->RimcasInstance->getAlert(rtCallsign);
						if (rimcasStage != CRimcas::NoAlert)
							drawBrush = (rimcasStage == CRimcas::StageTwo) ? &alertTextBrushStageTwo : &alertTextBrushStageOne;
					}
					std::unique_ptr<SolidBrush> tokenCustomBrush;
					if (renderedElement.hasCustomColor)
					{
						const Color customColor = radar_screen->ColorManager != nullptr
							? radar_screen->ColorManager->get_corrected_color("label", Color(255, renderedElement.colorR, renderedElement.colorG, renderedElement.colorB))
							: Color(255, renderedElement.colorR, renderedElement.colorG, renderedElement.colorB);
						tokenCustomBrush.reset(new SolidBrush(customColor));
						drawBrush = tokenCustomBrush.get();
					}

					wstring text(renderedElement.text.begin(), renderedElement.text.end());
					const int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
					gdi->DrawString(text.c_str(), wcslen(text.c_str()), drawFont,
						PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
						&Gdiplus::StringFormat(), drawBrush);

					int clickItemType = TAG_CITEM_NO;
					auto clickIt = tagClickableMap.find(renderedElement.text);
					if (clickIt != tagClickableMap.end())
						clickItemType = clickIt->second;
					if (renderedElement.isClearanceToken || IsClearanceDefinitionToken(renderedElement.token))
						clickItemType = TAG_CITEM_CLEARANCE;

					const int itemWidth = renderedElement.measuredWidth;
					const int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);
					if (itemWidth > 0 && itemHeight > 0)
					{
						CRect itemRect(
							textLeft + widthOffset,
							textTop + heightOffset,
							textLeft + widthOffset + itemWidth,
							textTop + heightOffset + itemHeight);
						radar_screen->AddScreenObject(clickItemType, rtCallsign.c_str(), itemRect, true, radar_screen->GetBottomLine(rtCallsign.c_str()).c_str());
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

			if (rimcasLabelOnlySetting && radar_screen->RimcasInstance != nullptr)
			{
				Color rimcasLabelColor = radar_screen->RimcasInstance->GetAircraftColor(
					rtCallsign,
					Color::AliceBlue,
					Color::AliceBlue,
					rimcasStageOneColor,
					rimcasStageTwoColor);
				if (rimcasLabelColor.ToCOLORREF() != Color(Color::AliceBlue).ToCOLORREF())
				{
					wstring alertText(L"ALERT");
					RectF alertMeasure;
					gdi->MeasureString(alertText.c_str(), wcslen(alertText.c_str()), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &alertMeasure);
					const int rimcasHeight = max(1, static_cast<int>(alertMeasure.GetBottom()));
					CRect rimcasLabelRect(tagBackgroundRect.left, tagBackgroundRect.top - rimcasHeight, tagBackgroundRect.right, tagBackgroundRect.top);
					SolidBrush rimcasBrush(rimcasLabelColor);
					gdi->FillRectangle(&rimcasBrush, CopyRect(rimcasLabelRect));
					StringFormat stringFormat;
					stringFormat.SetAlignment(StringAlignment::StringAlignmentCenter);
					SolidBrush* rimcasTextBrush = (radar_screen->RimcasInstance->getAlert(rtCallsign) == CRimcas::StageTwo)
						? &alertTextBrushStageTwo
						: &alertTextBrushStageOne;
					gdi->DrawString(alertText.c_str(), wcslen(alertText.c_str()), tagRegularFont,
						PointF(Gdiplus::REAL((rimcasLabelRect.left + rimcasLabelRect.right) / 2), Gdiplus::REAL(rimcasLabelRect.top)),
						&stringFormat,
						rimcasTextBrush);
				}
			}
		};

		for (CRadarTarget rt = radar_screen->GetPlugIn()->RadarTargetSelectFirst();
			rt.IsValid();
			rt = radar_screen->GetPlugIn()->RadarTargetSelectNext(rt))
		{
			if (!rt.IsValid() || !rt.GetPosition().IsValid() || !radar_screen->isVisible(rt))
				continue;

			const char* rtCallsignRaw = rt.GetCallsign();
			if (rtCallsignRaw == nullptr || rtCallsignRaw[0] == '\0')
				continue;
			const std::string rtCallsign = rtCallsignRaw;

			CRadarTargetPositionData rtPositionData = rt.GetPosition();
			const CPosition targetPosition = rtPositionData.GetPosition();
			if (!positionNearViewport(targetPosition))
				continue;

			const POINT targetPoint = projectTargetPosition(targetPosition);
			if (!pointInViewport(targetPoint, 180))
				continue;

			const int reportedGs = rtPositionData.GetReportedGS();
			CFlightPlan fp = radar_screen->GetPlugIn()->FlightPlanSelect(rtCallsign.c_str());
			bool acIsCorrelated = radar_screen->IsCorrelatedWithSettings(fp, rt, correlationSettings);
			const char* assignedSquawk = fp.IsValid() ? fp.GetControllerAssignedData().GetSquawk() : nullptr;
			const char* reportedSquawk = rtPositionData.GetSquawk();
			const bool hasAssignedSquawk = assignedSquawk != nullptr && assignedSquawk[0] != '\0';
			const bool hasReportedSquawk = reportedSquawk != nullptr && reportedSquawk[0] != '\0';
			const bool hasWrongAssignedSquawk = hasAssignedSquawk && hasReportedSquawk && strcmp(assignedSquawk, reportedSquawk) != 0;
			if (tagProModeEnabled && (!hasAssignedSquawk || hasWrongAssignedSquawk))
				acIsCorrelated = false;

			const bool keepIconForSquawkMismatch = tagProModeEnabled && (hasWrongAssignedSquawk || !hasAssignedSquawk);
			if (!acIsCorrelated && reportedGs < 1 && !radar_screen->ReleaseInProgress && !radar_screen->AcquireInProgress && !keepIconForSquawkMismatch)
				continue;

			const bool isOnRunway = radar_screen->RimcasInstance != nullptr && radar_screen->RimcasInstance->isAcOnRunway(rtCallsign);
			bool isDepartureTarget = false;
			if (fp.IsValid() && acIsCorrelated)
			{
				const char* originRaw = fp.GetFlightPlanData().GetOrigin();
				if (originRaw != nullptr && originRaw[0] != '\0' && !activeAirportUpper.empty())
					isDepartureTarget = (_stricmp(originRaw, activeAirportUpper.c_str()) == 0);
			}
			const bool hasNoFlightPlan = !fp.IsValid();

			drawTrailHistory(rtCallsign, rt, reportedGs);
			if (useNovaIconStyle && showLegacyPrimaryTarget)
			{
				const auto patatoideIt = radar_screen->Patatoides.find(rtCallsign);
				if (patatoideIt != radar_screen->Patatoides.end())
				{
					drawPatatoidePolygon(
						patatoideIt->second.points,
						getLegacyTargetColor("target_color", Color(255, 255, 242, 73)));
				}
			}
			const int iconSize = drawConfiguredIcon(rt, fp, rtCallsign, rtPositionData, targetPoint, acIsCorrelated, isDepartureTarget, hasNoFlightPlan, isOnRunway);

			if (reportedGs > 50 && radar_screen->PredictedLength > 0)
			{
				double distance = double(rtPositionData.GetReportedGS() * 0.514444) * 10.0;
				CPosition awayBase = BetterHarversine(targetPosition, rt.GetTrackHeading(), distance);
				distance = double(rtPositionData.GetReportedGS() * 0.514444) * (radar_screen->PredictedLength * 60) - 10.0;
				CPosition predictedEnd = BetterHarversine(awayBase, rt.GetTrackHeading(), distance);
				POINT lineOne, lineTwo;
				if (LiangBarsky(viewportRect, projectTargetPosition(awayBase), projectTargetPosition(predictedEnd), lineOne, lineTwo))
				{
					CPen* oldPen = dc.SelectObject(&symbolPen);
					dc.MoveTo(lineOne);
					dc.LineTo(lineTwo);
					dc.SelectObject(oldPen);
				}
			}

			const bool isAsel = aselCallsign != nullptr && strcmp(aselCallsign, rtCallsign.c_str()) == 0;

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
			radar_screen->AddScreenObject(
				DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE),
				rtCallsign.c_str(),
				targetArea,
				false,
				radar_screen->GetBottomLine(rtCallsign.c_str()).c_str());

			bool drawTargetTag = radar_screen->isVisible(rt);
			if (tagProModeEnabled && (!hasAssignedSquawk || hasWrongAssignedSquawk))
				drawTargetTag = false;
			if (!acIsCorrelated && reportedGs < 3)
				drawTargetTag = false;
			if (tagTowerModeEnabled)
			{
				const char* towerModeGroundState = fp.IsValid() ? fp.GetGroundState() : nullptr;
				const char* towerModeDestination = fp.IsValid() ? fp.GetFlightPlanData().GetDestination() : nullptr;
				const bool towerModeArrival =
					towerModeDestination != nullptr &&
					towerModeDestination[0] != '\0' &&
					_stricmp(towerModeDestination, activeAirport.c_str()) == 0;
				if (!towerModeArrival && !shouldDisplayTagInTowerMode(towerModeGroundState, reportedGs, isOnRunway))
					drawTargetTag = false;
			}
			if (!radar_screen->ShouldDisplayTargetForDisplayMode(fp, rt, acIsCorrelated, reportedGs, isOnRunway, displayModeSettings))
				drawTargetTag = false;
			if (drawTargetTag)
				drawTag(rt, fp, rtCallsign, targetPoint, isAsel, acIsCorrelated, reportedGs, isOnRunway);
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

	drawChrome();

	dc.Detach();
}

void CInsetWindow::render(HDC hDC, CSMRRadar * radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools)
{
	if (this->m_Id == -1)
		return;

	if (IsAvisoViewport())
	{
		renderAvisoViewport(hDC, radar_screen, gdi, mouseLocation);
		return;
	}

	CDC dc;
	dc.Attach(hDC);

	struct Utils
	{
		static string getEnumString(CSMRRadar::TagTypes type) {
			if (type == CSMRRadar::TagTypes::Departure)
				return "departure";
			if (type == CSMRRadar::TagTypes::Arrival)
				return "arrival";
			if (type == CSMRRadar::TagTypes::Uncorrelated)
				return "uncorrelated";
			return "airborne";
		}
		static RECT GetAreaFromText(CDC * dc, string text, POINT Pos) {
			RECT Area = { Pos.x, Pos.y, Pos.x + dc->GetTextExtent(text.c_str()).cx, Pos.y + dc->GetTextExtent(text.c_str()).cy };
			return Area;
		}

	};

	icao = radar_screen->ActiveAirport;
	AptPositions = radar_screen->AirportPositions;
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

	const Value* approachInsetSection = getProfileObjectSection("approach_insets");
	const Value* filterSection = getProfileObjectSection("filters");
	const Value* rimcasSection = getProfileObjectSection("rimcas");
	const Value* labelsSection = getProfileObjectSection("labels");

	const COLORREF qBackgroundColor = getSectionColorRef(approachInsetSection, "background_color", RGB(30, 30, 30));
	const COLORREF approachRunwayColor = getSectionColorRef(approachInsetSection, "runway_color", RGB(255, 255, 255));
	const COLORREF approachExtendedLineColor = getSectionColorRef(approachInsetSection, "extended_lines_color", RGB(180, 180, 180));
	const double approachExtendedLineLengthNm = max(0.1, getSectionDouble(approachInsetSection, "extended_lines_length", 15.0));
	const int approachExtendedLineTickSpacingNm = max(1, getSectionInt(approachInsetSection, "extended_lines_ticks_spacing", 1));
	const int radarRangeNm = max(1, getSectionInt(filterSection, "radar_range_nm", 999));
	const bool rimcasLabelOnlySetting = getSectionBool(rimcasSection, "rimcas_label_only", true);
	const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
	const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
	const Color squawkErrorLabelColor = getSectionColor(labelsSection, "squawk_error_color", Color(255, 255, 0, 0));

	CRect windowAreaCRect(m_Area);
	windowAreaCRect.NormalizeRect();

	// We create the radar
	dc.FillSolidRect(windowAreaCRect, qBackgroundColor);
	radar_screen->AddScreenObject(m_Id, "window", m_Area, true, "");

	auto scale = m_Scale;

	POINT refPt = windowAreaCRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	// Here we draw all runways for the airport
	CSectorElement rwy;
	for (rwy = radar_screen->GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = radar_screen->GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{

		if (startsWith(icao.c_str(), rwy.GetAirportName()))
		{

			CPen RunwayPen(PS_SOLID, 1, approachRunwayColor);
			CPen ExtendedCentreLinePen(PS_SOLID, 1, approachExtendedLineColor);
			CPen* oldPen = dc.SelectObject(&RunwayPen);

			CPosition EndOne, EndTwo;
			rwy.GetPosition(&EndOne, 0);
			rwy.GetPosition(&EndTwo, 1);

			POINT Pt1, Pt2;
			Pt1 = projectPoint(EndOne);
			Pt2 = projectPoint(EndTwo);

			POINT toDraw1, toDraw2;
			if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
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
				double lenght = approachExtendedLineLengthNm * 1852.0;

				// Drawing the extended centreline
				CPosition endExtended = BetterHarversine(Threshold, reverseHeading, lenght);

				Pt1 = projectPoint(Threshold);
				Pt2 = projectPoint(endExtended);

				if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
					dc.SelectObject(&ExtendedCentreLinePen);
					dc.MoveTo(toDraw1);
					dc.LineTo(toDraw2);
				}

				// Drawing the ticks
				int increment = approachExtendedLineTickSpacingNm * 1852;

				for (int j = increment; j <= int(approachExtendedLineLengthNm * 1852); j += increment) {

					CPosition tickPosition = BetterHarversine(Threshold, reverseHeading, j);
					CPosition tickBottom = BetterHarversine(tickPosition, fmod(reverseHeading - 90, 360), 500);
					CPosition tickTop = BetterHarversine(tickPosition, fmod(reverseHeading + 90, 360), 500);


					Pt1 = projectPoint(tickBottom);
					Pt2 = projectPoint(tickTop);

					if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
						dc.SelectObject(&ExtendedCentreLinePen);
						dc.MoveTo(toDraw1);
						dc.LineTo(toDraw2);
					}

				}
			} 

			dc.SelectObject(&oldPen);
		}
	}

	// Aircrafts

	vector<POINT> appAreaVect = { windowAreaCRect.TopLeft(),{ windowAreaCRect.right, windowAreaCRect.top }, windowAreaCRect.BottomRight(),{ windowAreaCRect.left, windowAreaCRect.bottom } };
	CPen WhitePen(PS_SOLID, 1, radar_screen->ColorManager->get_corrected_color("symbol", Color::White).ToCOLORREF());
	const CSMRRadar::CorrelationSettings insetCorrelationSettings = radar_screen->BuildCorrelationSettings();
	const CSMRRadar::DisplayModeSettings insetDisplayModeSettings = radar_screen->GetActiveDisplayModeSettings();
	const int insetTransitionAltitude = radar_screen->GetPlugIn()->GetTransitionAltitude();
	const std::string insetActiveAirport = radar_screen->getActiveAirport();
	bool insetTagProModeEnabled = insetDisplayModeSettings.requireAssignedSquawk;
	bool insetUseAspeedForGate = false;
	bool insetAirborneUseDepartureArrivalColoring = false;
	if (radar_screen->CurrentConfig != nullptr)
	{
		const Value& profile = radar_screen->CurrentConfig->getActiveProfile();
		if (profile.IsObject())
		{
			if (profile.HasMember("labels") &&
				profile["labels"].IsObject())
			{
				const Value& labels = profile["labels"];
				if (labels.HasMember("use_speed_for_gate") && labels["use_speed_for_gate"].IsBool())
				{
					insetUseAspeedForGate = labels["use_speed_for_gate"].GetBool();
				}
				else if (labels.HasMember("use_aspeed_for_gate") && labels["use_aspeed_for_gate"].IsBool())
				{
					insetUseAspeedForGate = labels["use_aspeed_for_gate"].GetBool();
				}

				if (labels.HasMember("use_departure_arrival_coloring") && labels["use_departure_arrival_coloring"].IsBool())
				{
					insetAirborneUseDepartureArrivalColoring = labels["use_departure_arrival_coloring"].GetBool();
				}
				else if (labels.HasMember("airborne") &&
					labels["airborne"].IsObject() &&
					labels["airborne"].HasMember("use_departure_arrival_coloring") &&
					labels["airborne"]["use_departure_arrival_coloring"].IsBool())
				{
					insetAirborneUseDepartureArrivalColoring = labels["airborne"]["use_departure_arrival_coloring"].GetBool();
				}
			}
		}
	}

	CRadarTarget aselTarget = radar_screen->GetPlugIn()->RadarTargetSelectASEL();
	CRadarTarget rt;
	for (rt = radar_screen->GetPlugIn()->RadarTargetSelectFirst();
		rt.IsValid();
		rt = radar_screen->GetPlugIn()->RadarTargetSelectNext(rt))
	{
		const char* rtCallsign = rt.GetCallsign();
		if (rtCallsign == nullptr || rtCallsign[0] == '\0')
			continue;
		const char* aselCallsign = aselTarget.IsValid() ? aselTarget.GetCallsign() : nullptr;
		bool isASEL = (aselCallsign != nullptr && strcmp(aselCallsign, rtCallsign) == 0);
		int radarRange = radarRangeNm;

		if (rt.GetGS() < 60 ||
			rt.GetPosition().GetPressureAltitude() > m_Filter ||
			!rt.IsValid() ||
			!rt.GetPosition().IsValid() ||
			rt.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		CPosition RtPos2 = rt.GetPosition().GetPosition();
		CRadarTargetPositionData RtPos = rt.GetPosition();
		auto fp = radar_screen->GetPlugIn()->FlightPlanSelect(rtCallsign);
		auto reportedGs = RtPos.GetReportedGS();
		const char* fpDestination = fp.IsValid() ? fp.GetFlightPlanData().GetDestination() : nullptr;
		const char* fpOrigin = fp.IsValid() ? fp.GetFlightPlanData().GetOrigin() : nullptr;
		const char* fpPlanType = fp.IsValid() ? fp.GetFlightPlanData().GetPlanType() : nullptr;
		const bool isOnRunway = radar_screen->RimcasInstance != nullptr && radar_screen->RimcasInstance->isAcOnRunway(rtCallsign);
		const bool acIsCorrelatedForMode = radar_screen->IsCorrelatedWithSettings(fp, rt, insetCorrelationSettings);
		if (!radar_screen->ShouldDisplayTargetForDisplayMode(fp, rt, acIsCorrelatedForMode, reportedGs, isOnRunway, insetDisplayModeSettings))
			continue;

		// Filtering the targets

		POINT RtPoint, hPoint;

		RtPoint = projectPoint(RtPos2);

		CRadarTargetPositionData hPos = rt.GetPreviousPosition(rt.GetPosition());
		for (int i = 1; i < radar_screen->Trail_App; i++) {
			if (!hPos.IsValid())
				continue;

			hPoint = projectPoint(hPos.GetPosition());

			if (Is_Inside(hPoint, appAreaVect)) {
				dc.SetPixel(hPoint, radar_screen->ColorManager->get_corrected_color("symbol", Color::White).ToCOLORREF());
			}

			hPos = rt.GetPreviousPosition(hPos);
		}

		if (Is_Inside(RtPoint, appAreaVect)) {
			dc.SelectObject(&WhitePen);

			if (RtPos.GetTransponderC()) {
				dc.MoveTo({ RtPoint.x, RtPoint.y - 4 });
				dc.LineTo({ RtPoint.x - 4, RtPoint.y });
				dc.LineTo({ RtPoint.x, RtPoint.y + 4 });
				dc.LineTo({ RtPoint.x + 4, RtPoint.y });
				dc.LineTo({ RtPoint.x, RtPoint.y - 4 });
			}
			else {
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x - 4, RtPoint.y - 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x + 4, RtPoint.y - 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x - 4, RtPoint.y + 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x + 4, RtPoint.y + 4);
			}

			CRect TargetArea(RtPoint.x - 4, RtPoint.y - 4, RtPoint.x + 4, RtPoint.y + 4);
			TargetArea.NormalizeRect();
			radar_screen->AddScreenObject(DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE), rtCallsign, TargetArea, false, radar_screen->GetBottomLine(rtCallsign).c_str());
		}

		// Predicted Track Line
		// It starts 10 seconds away from the ac
		if (radar_screen->PredictedLength > 0) {
			double d = double(rt.GetPosition().GetReportedGS() * 0.514444) * 10;
			CPosition AwayBase = BetterHarversine(rt.GetPosition().GetPosition(), rt.GetTrackHeading(), d);

			d = double(rt.GetPosition().GetReportedGS() * 0.514444) * (radar_screen->PredictedLength * 60) - 10;
			CPosition PredictedEnd = BetterHarversine(AwayBase, rt.GetTrackHeading(), d);

			POINT liangOne, liangTwo;

			if (LiangBarsky(m_Area, projectPoint(AwayBase), projectPoint(PredictedEnd), liangOne, liangTwo))
			{
				dc.SelectObject(&WhitePen);
				dc.MoveTo(liangOne);
				dc.LineTo(liangTwo);
			}
		}

		if (mouseWithin(mouseLocation, { RtPoint.x - 4, RtPoint.y - 4, RtPoint.x + 4, RtPoint.y + 4 })) {
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
		}

		int lenght = 50;

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
				m_TagAngles[rtCallsign] = 45.0; // TODO: Not the best, ah well
			}

			TagCenter.x = long(RtPoint.x + float(lenght * cos(DegToRad(m_TagAngles[rtCallsign]))));
			TagCenter.y = long(RtPoint.y + float(lenght * sin(DegToRad(m_TagAngles[rtCallsign]))));
		}
		// Drawing the tags, what a mess

			// ----- Generating the replacing map -----
			map<string, string> TagReplacingMap = CSMRRadar::GenerateTagData(
				rt,
				fp,
				isASEL,
				acIsCorrelatedForMode,
				insetTagProModeEnabled,
				insetTransitionAltitude,
				insetUseAspeedForGate,
				icao);

		// ----- Generating the clickable map -----
		map<string, int> TagClickableMap;
		TagClickableMap[TagReplacingMap["callsign"]] = TAG_CITEM_CALLSIGN;
		TagClickableMap[TagReplacingMap["actype"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["sctype"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["sqerror"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["deprwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["seprwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["arvrwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["srvrwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["gate"]] = TAG_CITEM_GATE;
		TagClickableMap[TagReplacingMap["sate"]] = TAG_CITEM_GATE;
		TagClickableMap[TagReplacingMap["flightlevel"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["gs"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["tendency"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["wake"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["tssr"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["sid"]] = TagClickableMap[TagReplacingMap["shid"]] = TAG_CITEM_SID;
		TagClickableMap[TagReplacingMap["origin"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["dest"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["systemid"]] = TAG_CITEM_MANUALCORRELATE;
		TagClickableMap[TagReplacingMap["gstatus"]] = TAG_CITEM_GROUNDSTATUS;
		TagClickableMap[TagReplacingMap["uk_stand"]] = TAG_CITEM_UKSTAND;
		TagClickableMap[TagReplacingMap["remark"]] = TAG_CITEM_REMARK;
		TagClickableMap[TagReplacingMap["scratchpad"]] = TAG_CITEM_SCRATCHPAD;


		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//	

		CSMRRadar::TagTypes TagType = CSMRRadar::TagTypes::Departure;
		CSMRRadar::TagTypes ColorTagType = CSMRRadar::TagTypes::Departure;

		if (fpDestination != nullptr && strcmp(fpDestination, insetActiveAirport.c_str()) == 0) {
				TagType = CSMRRadar::TagTypes::Arrival;
				ColorTagType = CSMRRadar::TagTypes::Arrival;
		}

			if (reportedGs > 50) {
				TagType = CSMRRadar::TagTypes::Airborne;

				// Is "use_departure_arrival_coloring" enabled? if not, then use the airborne colors
				const bool useDepArrColors = insetAirborneUseDepartureArrivalColoring;
				if (!useDepArrColors) {
					ColorTagType = CSMRRadar::TagTypes::Airborne;
				}
			}

		bool AcisCorrelated = acIsCorrelatedForMode;
		if (!AcisCorrelated && reportedGs >= 3)
		{
			TagType = CSMRRadar::TagTypes::Uncorrelated;
			ColorTagType = CSMRRadar::TagTypes::Uncorrelated;
		}

		// First we need to figure out the tag size

		int TagWidth = 0, TagHeight = 0;
		RectF mesureRect;
		auto fontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
		Gdiplus::Font* tagRegularFont = (fontIt != radar_screen->customFonts.end()) ? fontIt->second.get() : nullptr;
		if (tagRegularFont == nullptr)
			continue;
		Gdiplus::Font* tagBoldFont = tagRegularFont;
		std::unique_ptr<Gdiplus::Font> tagBoldFontOwned;
		Gdiplus::FontFamily baseFamily;
		if (tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok)
		{
			INT boldStyle = tagRegularFont->GetStyle() | Gdiplus::FontStyleBold;
			tagBoldFontOwned.reset(new Gdiplus::Font(&baseFamily, tagRegularFont->GetSize(), boldStyle, Gdiplus::UnitPixel));
			if (tagBoldFontOwned->GetLastStatus() == Gdiplus::Ok)
				tagBoldFont = tagBoldFontOwned.get();
		}

		gdi->MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);
		int blankWidth = (int)mesureRect.GetRight();

		mesureRect = RectF(0, 0, 0, 0);
		gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
			tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);
		int oneLineHeight = (int)mesureRect.GetBottom();
		if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
		{
			RectF boldMeasureRect;
			gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
				tagBoldFont, PointF(0, 0), &Gdiplus::StringFormat(), &boldMeasureRect);
			oneLineHeight = max(oneLineHeight, (int)boldMeasureRect.GetBottom());
		}

		static const Value emptyObject(kObjectType);
		const Value& LabelsSettings = (labelsSection != nullptr) ? *labelsSection : emptyObject;
		auto hasStatusDefinition = [&](const std::string& typeKey, const char* statusKey) -> bool
		{
			if (statusKey == nullptr || statusKey[0] == '\0')
				return false;
			if (!LabelsSettings.HasMember(typeKey.c_str()) || !LabelsSettings[typeKey.c_str()].IsObject())
				return false;
			const Value& section = LabelsSettings[typeKey.c_str()];
			if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
				return false;
			return section["status_definitions"].HasMember(statusKey) && section["status_definitions"][statusKey].IsObject();
		};
		auto resolveDefinitionLines = [&](const std::string& typeKey, const char* statusKey) -> const Value*
		{
			if (!LabelsSettings.HasMember(typeKey.c_str()) || !LabelsSettings[typeKey.c_str()].IsObject())
				return nullptr;

			const Value& section = LabelsSettings[typeKey.c_str()];
			if (statusKey != nullptr &&
				section.HasMember("status_definitions") &&
				section["status_definitions"].IsObject() &&
				section["status_definitions"].HasMember(statusKey) &&
				section["status_definitions"][statusKey].IsObject() &&
				section["status_definitions"][statusKey].HasMember("definition") &&
				section["status_definitions"][statusKey]["definition"].IsArray())
			{
				return &section["status_definitions"][statusKey]["definition"];
			}

			if (section.HasMember("definition") && section["definition"].IsArray())
				return &section["definition"];
			return nullptr;
		};

		std::string definitionTypeKey = Utils::getEnumString(TagType);
		const char* definitionStatusKey = nullptr;
		if (TagReplacingMap["actype"] == "NoFPL" && (TagType == CSMRRadar::TagTypes::Departure || TagType == CSMRRadar::TagTypes::Arrival))
		{
			definitionStatusKey = "nofpl";
		}
		else if (TagType == CSMRRadar::TagTypes::Airborne)
		{
			bool isAirborneArrival = false;
			if (fpDestination != nullptr &&
				strcmp(fpDestination, radar_screen->getActiveAirport().c_str()) == 0)
			{
				isAirborneArrival = true;
			}
			definitionTypeKey = isAirborneArrival ? "arrival" : "departure";
			const char* airborneStatusKey = isAirborneArrival ? "airarr" : "airdep";
			const char* onRunwayStatusKey = isAirborneArrival ? "airarr_onrunway" : "airdep_onrunway";
			const bool targetOnRunway = radar_screen->RimcasInstance->isAcOnRunway(rtCallsign);
			if (targetOnRunway && hasStatusDefinition(definitionTypeKey, onRunwayStatusKey))
				definitionStatusKey = onRunwayStatusKey;
			else
				definitionStatusKey = airborneStatusKey;
		}

		const Value* labelLinesPtr = resolveDefinitionLines(definitionTypeKey, definitionStatusKey);
		if (labelLinesPtr == nullptr)
			continue;
		const Value& LabelLines = *labelLinesPtr;
		struct RenderedTagElement
		{
			std::string token;
			std::string text;
			bool bold = false;
			bool hasCustomColor = false;
			int colorR = 255;
			int colorG = 255;
			int colorB = 255;
			int measuredWidth = 0;
			int measuredHeight = 0;
			bool isClearanceToken = false;
		};
		vector<vector<RenderedTagElement>> ReplacedLabelLines;

		if (!LabelLines.IsArray())
			continue;

		for (unsigned int i = 0; i < LabelLines.Size(); i++)
		{
			const Value& line = LabelLines[i];
			vector<string> rawElements;
			if (line.IsArray())
			{
				for (unsigned int j = 0; j < line.Size(); j++)
				{
					if (line[j].IsString())
						rawElements.push_back(line[j].GetString());
				}
			}
			else if (line.IsString())
			{
				rawElements.push_back(line.GetString());
			}

			if (rawElements.empty())
				continue;

			vector<RenderedTagElement> renderedLine;
			renderedLine.reserve(rawElements.size());
			bool allEmpty = true;

			int TempTagWidth = 0;

			for (const std::string& rawElement : rawElements)
			{
				mesureRect = RectF(0, 0, 0, 0);
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawElement);
				const std::string baseToken = styledToken.token.empty() ? rawElement : styledToken.token;
				string element;
				string clearanceNotClearedText;
				string clearanceClearedText;
				const bool isClearanceToken = TryParseClearanceTokenDisplay(baseToken, clearanceNotClearedText, clearanceClearedText);
				if (isClearanceToken)
				{
					if (fp.IsValid() && AcisCorrelated)
						element = fp.GetClearenceFlag() ? clearanceClearedText : clearanceNotClearedText;
					else
						element = "";
				}
				else
				{
					auto exactMatch = TagReplacingMap.find(baseToken);
					if (exactMatch != TagReplacingMap.end())
						element = exactMatch->second;
					else
					{
						element = baseToken;
						for (const auto& kv : TagReplacingMap)
						{
							if (element.find(kv.first) == std::string::npos)
								continue;
							replaceAll(element, kv.first, kv.second);
						}
					}
				}

				RenderedTagElement renderedElement;
				renderedElement.token = baseToken;
				renderedElement.text = element;
				renderedElement.bold = styledToken.bold;
				renderedElement.hasCustomColor = styledToken.hasCustomColor;
				renderedElement.colorR = styledToken.colorR;
				renderedElement.colorG = styledToken.colorG;
				renderedElement.colorB = styledToken.colorB;
				renderedElement.isClearanceToken = isClearanceToken;

				if (!element.empty())
				{
					allEmpty = false;
					wstring wstr = wstring(element.begin(), element.end());
					Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (measureFont == nullptr)
						measureFont = tagRegularFont;
					gdi->MeasureString(wstr.c_str(), wcslen(wstr.c_str()),
						measureFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);

					renderedElement.measuredWidth = (int)mesureRect.GetRight();
					renderedElement.measuredHeight = (int)mesureRect.GetBottom();
					TempTagWidth += renderedElement.measuredWidth;
				}

				renderedLine.push_back(std::move(renderedElement));
			}

			if (allEmpty)
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

		const std::string colorTagTypeKey = Utils::getEnumString(ColorTagType);
		const Value* colorTagLabelSection = nullptr;
		if (LabelsSettings.HasMember(colorTagTypeKey.c_str()) && LabelsSettings[colorTagTypeKey.c_str()].IsObject())
			colorTagLabelSection = &LabelsSettings[colorTagTypeKey.c_str()];

		auto getColorFromSectionOrDefault = [&](const Value* section, const char* key, const Color& fallback) -> Color
		{
			if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
				return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
			return fallback;
		};
		auto getColorWithLegacy = [&](const char* preferredKey, const char* legacyKey, const Color& fallback) -> Color
		{
			if (colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(preferredKey) &&
				(*colorTagLabelSection)[preferredKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)[preferredKey]);
			}
			if (legacyKey != nullptr &&
				colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(legacyKey) &&
				(*colorTagLabelSection)[legacyKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)[legacyKey]);
			}
			return fallback;
		};

		Color definedBackgroundColor = Color(255, 53, 126, 187);
		Color definedBackgroundOnRunwayColor = definedBackgroundColor;
		Color definedTextColor = Color::White;
		if (ColorTagType == CSMRRadar::TagTypes::Departure)
		{
			definedBackgroundColor = getColorWithLegacy("background_no_status_color", "gate_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "on_runway_color", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == CSMRRadar::TagTypes::Arrival)
		{
			definedBackgroundColor = getColorWithLegacy("background_on_ground_color", "background_color", Color(255, 191, 87, 91));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == CSMRRadar::TagTypes::Uncorrelated)
		{
			definedBackgroundColor = getColorWithLegacy("background_on_ground_color", "background_color", Color(255, 150, 22, 135));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else
		{
			definedBackgroundColor = getColorFromSectionOrDefault(colorTagLabelSection, "background_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getColorFromSectionOrDefault(colorTagLabelSection, "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorFromSectionOrDefault(colorTagLabelSection, "text_color", Color::White);
		}
		if (TagType == CSMRRadar::TagTypes::Departure) {
			if (!TagReplacingMap["sid"].empty() && radar_screen->CurrentConfig->isSidColorAvail(TagReplacingMap["sid"], radar_screen->getActiveAirport())) {
				definedBackgroundColor = radar_screen->CurrentConfig->getSidColor(TagReplacingMap["sid"], radar_screen->getActiveAirport());
			}

			if (fpPlanType != nullptr && fpPlanType[0] == 'I' && TagReplacingMap["asid"].empty()) {
				if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("background_no_sid_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_no_sid_color"]);
				else if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nosid_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nosid_color"]);
			}
		}
			if (TagReplacingMap["actype"] == "NoFPL") {
				if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("background_no_fpl_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_no_fpl_color"]);
				else if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nofpl_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nofpl_color"]);
		}

		if (TagType == CSMRRadar::TagTypes::Airborne &&
			fp.IsValid() &&
			AcisCorrelated)
		{
			bool isAirborneDeparture = true;
			std::string originAirport = fpOrigin != nullptr ? fpOrigin : "";
			std::string activeAirportUpper = radar_screen->getActiveAirport();
			if (!originAirport.empty() && !activeAirportUpper.empty())
			{
				std::transform(originAirport.begin(), originAirport.end(), originAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				std::transform(activeAirportUpper.begin(), activeAirportUpper.end(), activeAirportUpper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				isAirborneDeparture = (originAirport == activeAirportUpper);
			}

			const char* runwaySectionKey = isAirborneDeparture ? "departure" : "arrival";
			if (LabelsSettings.HasMember(runwaySectionKey) && LabelsSettings[runwaySectionKey].IsObject())
			{
				const Value& runwaySection = LabelsSettings[runwaySectionKey];
				if (runwaySection.HasMember("background_airborne_color") && runwaySection["background_airborne_color"].IsObject())
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_airborne_color"]);
				if (runwaySection.HasMember("text_airborne_color") && runwaySection["text_airborne_color"].IsObject())
					definedTextColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["text_airborne_color"]);

				const char* runwayColorKey = "background_on_runway_color";
				if (runwaySection.HasMember(runwayColorKey) && runwaySection[runwayColorKey].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection[runwayColorKey]);
				}
				else if (isAirborneDeparture &&
					runwaySection.HasMember("on_runway_color") &&
					runwaySection["on_runway_color"].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["on_runway_color"]);
				}
				else if (runwaySection.HasMember("background_color_on_runway") &&
					runwaySection["background_color_on_runway"].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_color_on_runway"]);
				}
			}
			else if (LabelsSettings.HasMember("airborne") && LabelsSettings["airborne"].IsObject())
			{
				const Value& airborneLabel = LabelsSettings["airborne"];
				const char* bgKey = isAirborneDeparture ? "departure_background_color" : "arrival_background_color";
				const char* textKey = isAirborneDeparture ? "departure_text_color" : "arrival_text_color";
				if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgKey]);
				if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
					definedTextColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[textKey]);
				const char* bgOnRunwayKey = isAirborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
				if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
			}
		}

		Color TagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign,
			definedBackgroundColor,
			definedBackgroundOnRunwayColor,
			rimcasStageOneColor,
			rimcasStageTwoColor);

		if (rimcasLabelOnly)
			TagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign,
				definedBackgroundColor,
				definedBackgroundOnRunwayColor);

		CRect TagBackgroundRect(TagCenter.x - (TagWidth / 2), TagCenter.y - (TagHeight / 2), TagCenter.x + (TagWidth / 2), TagCenter.y + (TagHeight / 2));

		if (Is_Inside(TagBackgroundRect.TopLeft(), appAreaVect) &&
			Is_Inside(RtPoint, appAreaVect) &&
			Is_Inside(TagBackgroundRect.BottomRight(), appAreaVect)) {

			const int padding = 3;
			TagBackgroundRect = CRect(TagBackgroundRect.left - padding, TagBackgroundRect.top - padding, TagBackgroundRect.right + padding, TagBackgroundRect.bottom + padding);
			int textLeft = TagBackgroundRect.left + padding;
			int textTop = TagBackgroundRect.top + padding;
			int textWidth = max(0, TagBackgroundRect.Width() - (padding * 2));

			// Semi-transparent background to reduce clutter while keeping arrival/departure color coding (unless RIMCAS alert overrides the color).
			if (radar_screen->RimcasInstance->getAlert(rtCallsign) == CRimcas::NoAlert) {
				auto blend = [](Color a, Color b, float t) {
					auto mix = [t](BYTE c1, BYTE c2) -> BYTE {
						return static_cast<BYTE>(c1 * t + c2 * (1.0f - t));
					};
					return Color(mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
				};
				Color neutralBlue(0x6E, 0xA5, 0xA8);  // from palette
				Color neutralRed(0x4E, 0x4E, 0x68);   // from palette
				Color baseBlue(60, 120, 200);
				Color baseRed(200, 70, 80);

				if (ColorTagType == CSMRRadar::TagTypes::Departure) {
					Color mixed = blend(baseBlue, neutralBlue, 0.65f);
					TagBackgroundColor = Color(160, mixed.GetR(), mixed.GetG(), mixed.GetB());
				}
				else if (ColorTagType == CSMRRadar::TagTypes::Arrival) {
					Color mixed = blend(baseRed, neutralRed, 0.65f);
					TagBackgroundColor = Color(160, mixed.GetR(), mixed.GetG(), mixed.GetB());
				}
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

			SolidBrush FontColor(radar_screen->ColorManager->get_corrected_color("label", definedTextColor));
			SolidBrush SquawkErrorColor(radar_screen->ColorManager->get_corrected_color("label",
				squawkErrorLabelColor));
			SolidBrush AlertTextColorCaution(radar_screen->ColorManager->get_corrected_color("label",
				getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30))));
			SolidBrush AlertTextColorWarning(radar_screen->ColorManager->get_corrected_color("label",
				getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255))));

			m_TagAreas[rtCallsign] = TagBackgroundRect;
			radar_screen->AddScreenObject(m_Id, rtCallsign, TagBackgroundRect, true, radar_screen->GetBottomLine(rtCallsign).c_str());

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
					const std::string& rawToken = renderedElement.token;
					Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (drawFont == nullptr)
						drawFont = tagRegularFont;

					SolidBrush* color = &FontColor;
					if (TagReplacingMap["sqerror"].size() > 0 && strcmp(element.c_str(), TagReplacingMap["sqerror"].c_str()) == 0)
						color = &SquawkErrorColor;

					CRimcas::RimcasAlertTypes rimcasStage = radar_screen->RimcasInstance->getAlert(rtCallsign);
					if (rimcasStage != CRimcas::NoAlert)
						color = (rimcasStage == CRimcas::StageTwo) ? &AlertTextColorWarning : &AlertTextColorCaution;

					std::unique_ptr<SolidBrush> tokenCustomColorBrush;
					if (renderedElement.hasCustomColor)
					{
						Color customColor = radar_screen->ColorManager->get_corrected_color("label",
							Color(255, renderedElement.colorR, renderedElement.colorG, renderedElement.colorB));
						tokenCustomColorBrush.reset(new SolidBrush(customColor));
						color = tokenCustomColorBrush.get();
					}

					wstring welement = wstring(element.begin(), element.end());
					int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
					gdi->DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
						PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
						&Gdiplus::StringFormat(), color);

					int clickItemType = TAG_CITEM_NO;
					auto clickItemIt = TagClickableMap.find(element);
					if (clickItemIt != TagClickableMap.end())
						clickItemType = clickItemIt->second;
					if (renderedElement.isClearanceToken || IsClearanceDefinitionToken(rawToken))
						clickItemType = TAG_CITEM_CLEARANCE;

					int itemWidth = renderedElement.measuredWidth;
					int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);
					if (itemWidth > 0 && itemHeight > 0)
					{
						CRect ItemRect(textLeft + widthOffset, textTop + heightOffset,
							textLeft + widthOffset + itemWidth, textTop + heightOffset + itemHeight);
						radar_screen->AddScreenObject(clickItemType, rtCallsign, ItemRect, true, radar_screen->GetBottomLine(rtCallsign).c_str());
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
				gdi->DrawLine(&Pen(radar_screen->ColorManager->get_corrected_color("symbol", Color::White)), PointF(Gdiplus::REAL(RtPoint.x), Gdiplus::REAL(RtPoint.y)), PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));

			// If we use a RIMCAS label only, we display it, and adapt the rectangle
			CRect oldCrectSave = TagBackgroundRect;

			if (rimcasLabelOnly) {
				Color RimcasLabelColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign, Color::AliceBlue, Color::AliceBlue,
					rimcasStageOneColor,
					rimcasStageTwoColor);

				if (RimcasLabelColor.ToCOLORREF() != Color(Color::AliceBlue).ToCOLORREF()) {
					int rimcas_height = 0;

					wstring wrimcas_height = wstring(L"ALERT");

					RectF RectRimcas_height;

					gdi->MeasureString(wrimcas_height.c_str(), wcslen(wrimcas_height.c_str()), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &RectRimcas_height);
					rimcas_height = int(RectRimcas_height.GetBottom());

					// Drawing the rectangle

					CRect RimcasLabelRect(TagBackgroundRect.left, TagBackgroundRect.top - rimcas_height, TagBackgroundRect.right, TagBackgroundRect.top);
					gdi->FillRectangle(&SolidBrush(RimcasLabelColor), CopyRect(RimcasLabelRect));
					TagBackgroundRect.top -= rimcas_height;

					// Drawing the text

					wstring rimcasw = wstring(L"ALERT");
					StringFormat stformat;
					stformat.SetAlignment(StringAlignment::StringAlignmentCenter);
					SolidBrush* rimcasTextBrush = (radar_screen->RimcasInstance->getAlert(rtCallsign) == CRimcas::StageTwo)
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

	// Distance tools here
	for (auto&& kv : DistanceTools)
	{
		CRadarTarget one = radar_screen->GetPlugIn()->RadarTargetSelect(kv.first.c_str());
		CRadarTarget two = radar_screen->GetPlugIn()->RadarTargetSelect(kv.second.c_str());

		int radarRange = radarRangeNm;

		if (one.GetGS() < 60 ||
			one.GetPosition().GetPressureAltitude() > m_Filter ||
			!one.IsValid() ||
			!one.GetPosition().IsValid() ||
			one.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		if (two.GetGS() < 60 ||
			two.GetPosition().GetPressureAltitude() > m_Filter ||
			!two.IsValid() ||
			!two.GetPosition().IsValid() ||
			two.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		CPen Pen(PS_SOLID, 1, RGB(255, 255, 255));
		CPen *oldPen = dc.SelectObject(&Pen);

		POINT onePoint = projectPoint(one.GetPosition().GetPosition());
		POINT twoPoint = projectPoint(two.GetPosition().GetPosition());

		POINT toDraw1, toDraw2;
		if (LiangBarsky(m_Area, onePoint, twoPoint, toDraw1, toDraw2)) {
			dc.MoveTo(toDraw1);
			dc.LineTo(toDraw2);
		}

		POINT TextPos = { twoPoint.x + 20, twoPoint.y };

		double Distance = one.GetPosition().GetPosition().DistanceTo(two.GetPosition().GetPosition());
		double Bearing = one.GetPosition().GetPosition().DirectionTo(two.GetPosition().GetPosition());

		string distances = std::to_string(Distance);
		size_t decimal_pos = distances.find(".");
		distances = distances.substr(0, decimal_pos + 2);

		string bearings = std::to_string(Bearing);
		decimal_pos = bearings.find(".");
		bearings = bearings.substr(0, decimal_pos + 2);

		string text = bearings;
		text += "° / ";
		text += distances;
		text += "nm";
		COLORREF old_color = dc.SetTextColor(RGB(0, 0, 0));

		CRect ClickableRect = { TextPos.x - 2, TextPos.y, TextPos.x + dc.GetTextExtent(text.c_str()).cx + 2, TextPos.y + dc.GetTextExtent(text.c_str()).cy };
		if (Is_Inside(ClickableRect.TopLeft(), appAreaVect) && Is_Inside(ClickableRect.BottomRight(), appAreaVect))
		{
			gdi->FillRectangle(&SolidBrush(Color(127, 122, 122)), CopyRect(ClickableRect));
			dc.Draw3dRect(ClickableRect, RGB(75, 75, 75), RGB(45, 45, 45));
			dc.TextOutA(TextPos.x, TextPos.y, text.c_str());

			radar_screen->AddScreenObject(RIMCAS_DISTANCE_TOOL, string(kv.first + "," + kv.second).c_str(), ClickableRect, false, "");
		}
		
		dc.SetTextColor(old_color);

		dc.SelectObject(oldPen);
	}

	// Resize square
	COLORREF resizeBackgroundColor = RGB(60, 60, 60);
	POINT BottomRight = { m_Area.right, m_Area.bottom };
	POINT TopLeft = { BottomRight.x - 10, BottomRight.y - 10 };
	CRect ResizeArea = { TopLeft, BottomRight };
	ResizeArea.NormalizeRect();
	dc.FillSolidRect(ResizeArea, resizeBackgroundColor);
	radar_screen->AddScreenObject(m_Id, "resize", ResizeArea, true, "");

	dc.Draw3dRect(ResizeArea, RGB(0, 0, 0), RGB(0, 0, 0));

	// Sides
	//CBrush FrameBrush(RGB(35, 35, 35));
	CBrush FrameBrush(RGB(127, 122, 122));
	COLORREF TopBarTextColor(RGB(35, 35, 35));
	dc.FrameRect(windowAreaCRect, &FrameBrush);

	// Topbar
	TopLeft = windowAreaCRect.TopLeft();
	TopLeft.y = TopLeft.y - 15;
	BottomRight = { windowAreaCRect.right, windowAreaCRect.top };
	CRect TopBar(TopLeft, BottomRight);
	TopBar.NormalizeRect();
	dc.FillRect(TopBar, &FrameBrush);
	POINT TopLeftText = { TopBar.left + 5, TopBar.bottom - dc.GetTextExtent("SRW 1").cy };
	COLORREF oldTextColorC = dc.SetTextColor(TopBarTextColor);

	radar_screen->AddScreenObject(m_Id, "topbar", TopBar, true, "");

	string Toptext = "SRW " + std::to_string(m_Id - APPWINDOW_BASE);
	dc.TextOutA(TopLeftText.x + (TopBar.right-TopBar.left) / 2 - dc.GetTextExtent("SRW 1").cx , TopLeftText.y, Toptext.c_str());

	// Range button
	CRect RangeRect = DrawInsetToolbarButton(dc, "Z", TopBar, InsetToolbarRightOffset(1), mouseLocation);
	radar_screen->AddScreenObject(m_Id, "range", RangeRect, false, "");

	// Filter button
	CRect FilterRect = DrawInsetToolbarButton(dc, "F", TopBar, InsetToolbarRightOffset(2), mouseLocation);
	radar_screen->AddScreenObject(m_Id, "filter", FilterRect, false, "");

	// Rotate button
	CRect RotateRect = DrawInsetToolbarButton(dc, "R", TopBar, InsetToolbarRightOffset(3), mouseLocation);
	radar_screen->AddScreenObject(m_Id, "rotate", RotateRect, false, "");

	dc.SetTextColor(oldTextColorC);

	// Close
	CRect CloseRect = DrawInsetToolbarButton(dc, "X", TopBar, InsetToolbarRightOffset(0), mouseLocation);
	radar_screen->AddScreenObject(m_Id, "close", CloseRect, false, "");

	dc.Detach();
}

