#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "aviso/AvisoRasterSizing.hpp"
#include "aviso/AvisoRasterGeometry.hpp"
#include "aviso/AvisoRasterRenderer.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.AvisoRuntimeState.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"
#include "insets/InsetWindow.hpp"
#include "crash/CrashRuntime.hpp"

#include <cmath>
#include <unordered_map>

using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::AvisoMin;
using VsmrAvisoSupport::ToUpperAscii;

namespace
{
	bool AvisoWithinTolerance(double left, double right, double tolerance)
	{
		const double delta = left - right;
		return delta >= -tolerance && delta <= tolerance;
	}

	class ScopedCdcDetach
	{
	public:
		explicit ScopedCdcDetach(CDC& dc) : dc_(dc)
		{
		}

		~ScopedCdcDetach()
		{
			Detach();
		}

		void Detach()
		{
			if (attached_ && dc_.GetSafeHdc() != nullptr)
				dc_.Detach();
			attached_ = false;
		}

	private:
		CDC& dc_;
		bool attached_ = true;
	};
}

void CSMRRadar::EnsureAvisoGeoJsonRenderPipeline()
{
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;
	if (AvisoGeoJsonRenderPipeline != nullptr)
		return;

	try
	{
		VsmrAviso::AvisoRasterPipeline::Callbacks callbacks;
		callbacks.render = [this](const AvisoRasterRenderRequest& request) {
			return RenderAvisoGeoJsonRaster(request);
		};
		callbacks.isExternallyCancelled = [this](const AvisoRasterRenderRequest& request) {
			return request.groupGeneration !=
				AvisoGroupGeneration.load(std::memory_order_relaxed);
		};
		callbacks.isExternalStopRequested = [this]() {
			return IsAvisoGeoJsonRenderStopRequested();
		};
		callbacks.requestRefresh = [this]() {
			RequestRefreshFromWorker();
		};
		callbacks.workerThreadStarted = []() {
			ULONG stackGuarantee = 64U * 1024U;
			::SetThreadStackGuarantee(&stackGuarantee);
			VsmrCrashRuntime::RecordCurrentThreadRole("main AVISO render worker");
		};
		callbacks.workerThreadStopped = []() {
			VsmrCrashRuntime::RecordCurrentThreadRole("inactive");
		};
		callbacks.renderStarted = [this](const AvisoRasterRenderRequest&) {
			VsmrCrashRuntime::RecordCurrentThreadCallback(
				"AvisoRasterPipeline::WorkerMain (main)",
				reinterpret_cast<std::uintptr_t>(this));
		};
		callbacks.reportError = [](const std::string& message) {
			Logger::info(message);
		};
		callbacks.diagnostics.requestQueued = [this](bool superseded) {
			PerformanceDiagnostics.RecordAvisoRequestQueued(
				VsmrPerformance::AvisoViewport::Main,
				superseded);
		};
		callbacks.diagnostics.requestCoalesced = [this]() {
			PerformanceDiagnostics.RecordAvisoRequestCoalesced(
				VsmrPerformance::AvisoViewport::Main);
		};
		callbacks.diagnostics.requestDebounced = [this]() {
			PerformanceDiagnostics.RecordAvisoRequestDebounced(
				VsmrPerformance::AvisoViewport::Main);
		};
		callbacks.diagnostics.rasterBuilt = [this](
			double rebuildMilliseconds,
			double queueWaitMilliseconds,
			bool succeeded) {
			PerformanceDiagnostics.RecordAvisoRasterBuild(
				VsmrPerformance::AvisoViewport::Main,
				rebuildMilliseconds,
				queueWaitMilliseconds,
				succeeded);
		};
		callbacks.diagnostics.rasterBuildCancelled = [this]() {
			PerformanceDiagnostics.RecordAvisoRasterBuildCancelled(
				VsmrPerformance::AvisoViewport::Main);
		};
		callbacks.diagnostics.resultDiscarded = [this]() {
			PerformanceDiagnostics.RecordAvisoResultDiscarded(
				VsmrPerformance::AvisoViewport::Main);
		};

		AvisoGeoJsonRenderPipeline =
			std::make_unique<VsmrAviso::AvisoRasterPipeline>(
				std::move(callbacks),
				"AVISO render worker");
	}
	catch (const std::exception& ex)
	{
		Logger::info("AVISO render pipeline creation failed: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("AVISO render pipeline creation failed: unknown exception");
	}
}

void CSMRRadar::StopAvisoGeoJsonRenderPipeline()
{
	AvisoGeoJsonRenderStop.store(true, std::memory_order_release);
	if (AvisoGeoJsonRenderPipeline != nullptr)
		AvisoGeoJsonRenderPipeline->Stop();
}

bool CSMRRadar::IsAvisoGeoJsonRenderStopRequested() const
{
	return IsShutdownRequested() || AvisoGeoJsonRenderStop.load(std::memory_order_relaxed);
}

bool CSMRRadar::IsShutdownRequested() const
{
	return ShutdownRequested.load(std::memory_order_relaxed);
}

bool CSMRRadar::CanUnloadRuntimeCallbacks() noexcept
{
	// A failed Win32 callback removal must retain the DLL. Otherwise Windows may
	// dispatch into unmapped code after EuroScope completes plug-in shutdown.
	return gInsetWindowRadarScreens.empty() &&
		gThreadMouseHook == nullptr &&
		gThreadKeyboardHook == nullptr;
}

void CSMRRadar::BeginShutdown()
{
	ShutdownRequested.store(true, std::memory_order_relaxed);
	AvisoRefreshHostWindow.store(nullptr, std::memory_order_release);
	AvisoGeoJsonRenderStop.store(true, std::memory_order_relaxed);
	StopAvisoGeoJsonRenderPipeline();

	for (auto& appWindow : appWindows)
	{
		if (appWindow.second != nullptr)
		{
			appWindow.second->EndAvisoPan();
			appWindow.second->CancelWindowInteraction();
			appWindow.second->CancelAvisoViewportRender();
		}
	}
}

void CSMRRadar::QueueAvisoGeoJsonRasterRender(AvisoRasterRenderRequest request)
{
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;

	if (request.path.empty() ||
		request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0)
	{
		return;
	}

	EnsureAvisoGeoJsonRenderPipeline();
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;
	if (AvisoGeoJsonRenderPipeline != nullptr)
	{
		AvisoGeoJsonRenderPipeline->Queue(
			std::move(request),
			AvisoGeoJsonRasterCache != nullptr);
	}
}

void CSMRRadar::ClearAvisoGeoJsonRasterCache()
{
	if (AvisoGeoJsonRasterCache != nullptr)
	{
		::DeleteObject(AvisoGeoJsonRasterCache);
		AvisoGeoJsonRasterCache = nullptr;
	}

	AvisoGeoJsonRasterCachePath.clear();
	AvisoGeoJsonRasterGroupGeneration = 0;
	AvisoGeoJsonRasterColorPalette = "dark";
	AvisoGeoJsonRasterBackgroundColor = Color(0, 0, 0, 0);
	AvisoGeoJsonRasterViewportZoomLevel = 0;
	AvisoGeoJsonRasterDisplayScale = 1.0;
	AvisoGeoJsonRasterMinLongitude = 0.0;
	AvisoGeoJsonRasterMinLatitude = 0.0;
	AvisoGeoJsonRasterMaxLongitude = 0.0;
	AvisoGeoJsonRasterMaxLatitude = 0.0;
	AvisoGeoJsonRasterWidth = 0;
	AvisoGeoJsonRasterHeight = 0;
	AvisoGeoJsonRasterAnchorLongitude = 0.0;
	AvisoGeoJsonRasterAnchorLatitude = 0.0;
	AvisoGeoJsonRasterBottomRightLongitude = 0.0;
	AvisoGeoJsonRasterBottomRightLatitude = 0.0;
	AvisoGeoJsonRasterProjectedTopLeft = PointF();
	AvisoGeoJsonRasterProjectedTopRight = PointF();
	AvisoGeoJsonRasterProjectedBottomLeft = PointF();
	AvisoGeoJsonRasterProjectedBottomRight = PointF();
	AvisoGeoJsonRasterAnchorValid = false;
}

void CSMRRadar::ApplyCompletedAvisoGeoJsonRaster()
{
	if (IsShutdownRequested())
		return;

	std::unique_ptr<AvisoRasterRenderResult> result =
		AvisoGeoJsonRenderPipeline != nullptr
		? AvisoGeoJsonRenderPipeline->TakeCompleted()
		: nullptr;

	if (result == nullptr || result->bitmap == nullptr)
		return;

	bool resultApplied = false;
	{
		std::lock_guard<std::mutex> groupGuard(AvisoGroupMutex);
		if (result->groupGeneration == AvisoGroupGeneration.load(std::memory_order_relaxed))
		{
			ClearAvisoGeoJsonRasterCache();
			AvisoGeoJsonRasterCache = result->bitmap;
			result->bitmap = nullptr;
			AvisoGeoJsonRasterCachePath = result->path;
			AvisoGeoJsonRasterGroupGeneration = result->groupGeneration;
			AvisoGeoJsonRasterColorPalette = result->colorPalette;
			AvisoGeoJsonRasterBackgroundColor = result->backgroundColor;
			AvisoGeoJsonRasterViewportZoomLevel = result->viewportZoomLevel;
			AvisoGeoJsonRasterDisplayScale = result->displayScale;
			AvisoGeoJsonRasterMinLongitude = result->displayMinLongitude;
			AvisoGeoJsonRasterMinLatitude = result->displayMinLatitude;
			AvisoGeoJsonRasterMaxLongitude = result->displayMaxLongitude;
			AvisoGeoJsonRasterMaxLatitude = result->displayMaxLatitude;
			AvisoGeoJsonRasterWidth = result->rasterWidth;
			AvisoGeoJsonRasterHeight = result->rasterHeight;
			AvisoGeoJsonRasterAnchorLongitude = result->renderMinLongitude;
			AvisoGeoJsonRasterAnchorLatitude = result->renderMaxLatitude;
			AvisoGeoJsonRasterBottomRightLongitude = result->renderMaxLongitude;
			AvisoGeoJsonRasterBottomRightLatitude = result->renderMinLatitude;
			AvisoGeoJsonRasterProjectedTopLeft = result->projectedTopLeft;
			AvisoGeoJsonRasterProjectedTopRight = result->projectedTopRight;
			AvisoGeoJsonRasterProjectedBottomLeft = result->projectedBottomLeft;
			AvisoGeoJsonRasterProjectedBottomRight = result->projectedBottomRight;
			AvisoGeoJsonRasterAnchorValid = true;
			resultApplied = true;
		}
	}
	if (resultApplied)
		PerformanceDiagnostics.RecordAvisoResultApplied(VsmrPerformance::AvisoViewport::Main);
	else
		PerformanceDiagnostics.RecordAvisoResultDiscarded(VsmrPerformance::AvisoViewport::Main);
}

void CSMRRadar::RequestRefreshFromWorker()
{
	if (IsShutdownRequested())
		return;

	const HWND hostWindow = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	const UINT refreshMessage = AvisoWorkerRefreshMessage();
	if (refreshMessage == 0 ||
		hostWindow == nullptr ||
		!::IsWindow(hostWindow))
		return;

	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::AvisoWorkerUpdate);
	if (!::PostMessage(
		hostWindow,
		refreshMessage,
		reinterpret_cast<WPARAM>(this),
		0))
	{
		Logger::info("AVISO render worker could not post a UI refresh request");
	}
}

bool CSMRRadar::IsAvisoRasterRenderRequestCancelled(
	const AvisoRasterRenderRequest& request) const noexcept
{
	return IsAvisoGeoJsonRenderStopRequested() ||
		request.groupGeneration != AvisoGroupGeneration.load(std::memory_order_relaxed) ||
		(request.cancellationToken != nullptr &&
			request.cancellationToken->load(std::memory_order_acquire) != request.requestId);
}

void CSMRRadar::MarkPerformanceRefreshReason(
	VsmrPerformance::FrameRefreshReason reason) noexcept
{
	const std::uint32_t reasonMask = VsmrPerformance::RefreshReasonMask(reason);
	if (reasonMask != 0)
		PendingPerformanceRefreshReasonMask.fetch_or(reasonMask, std::memory_order_relaxed);
}

std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> CSMRRadar::RenderAvisoGeoJsonRaster(const AvisoRasterRenderRequest& request) const
{
    // A worker owns its cache until the thread exits, including during shutdown.
    thread_local VsmrAviso::AvisoRasterRenderer renderer;
    return renderer.Render(request, [this, &request]() {
        return IsAvisoRasterRenderRequestCancelled(request);
    });
}

CRect CSMRRadar::ResolveMainAvisoRenderArea()
{
	CRect mainArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	mainArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		mainArea.bottom = chatArea.top;
	mainArea.NormalizeRect();
	if (mainArea.IsRectEmpty())
		return mainArea;

	LONG availableLeft = mainArea.left;
	LONG availableTop = mainArea.top;
	LONG availableRight = mainArea.right;
	LONG availableBottom = mainArea.bottom;
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		const auto windowIt = appWindows.find(display.first);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		const CInsetWindow* inset = windowIt->second.get();
		if (inset->UsesSizePreservingSnap())
			continue;
		CRect insetArea(inset->m_Area);
		insetArea.NormalizeRect();
		switch (inset->m_AvisoLayoutMode)
		{
		case CInsetWindow::AvisoLayoutMode::SplitLeft:
			availableLeft = (std::max)(availableLeft, insetArea.right);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitRight:
			availableRight = (std::min)(availableRight, insetArea.left);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitTop:
			availableTop = (std::max)(availableTop, insetArea.bottom);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitBottom:
			availableBottom = (std::min)(availableBottom, insetArea.top);
			break;
		default:
			continue;
		}
	}
	if (availableRight <= availableLeft || availableBottom <= availableTop)
		return CRect(0, 0, 0, 0);
	return CRect(availableLeft, availableTop, availableRight, availableBottom);
}

COLORREF CSMRRadar::GetAvisoBackgroundColor() const noexcept
{
	if (AvisoColorPalette == "real")
		return AvisoRealBackgroundColor;
	if (AvisoColorPalette == "light")
		return AvisoLightBackgroundColor;
	return AvisoDarkBackgroundColor;
}

Gdiplus::Color CSMRRadar::GetAvisoRasterBackgroundColor() const noexcept
{
	const size_t palette = AvisoColorPalette == "real" ? 2 : AvisoColorPalette == "light" ? 1 : 0;
	if (std::lround(AvisoBackgroundOpacities[palette] * 255) != 255)
		return Color(0, 0, 0, 0);
	const COLORREF rgb = GetAvisoBackgroundColor();
	return Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

// Alpha is applied once per viewport, not once per cache/render pass.
void CSMRRadar::DrawAvisoBackground(Gdiplus::Graphics& graphics, const RECT& area) const
{
	const COLORREF rgb = GetAvisoBackgroundColor();
	const size_t palette = AvisoColorPalette == "real" ? 2 : AvisoColorPalette == "light" ? 1 : 0;
	const auto previous = graphics.GetCompositingMode();
	graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
	Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(std::lround(AvisoBackgroundOpacities[palette] * 255)),
		GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
	graphics.FillRectangle(&brush, Gdiplus::Rect(static_cast<int>(area.left), static_cast<int>(area.top),
		static_cast<int>(area.right - area.left), static_cast<int>(area.bottom - area.top)));
	graphics.Flush(Gdiplus::FlushIntentionSync);
	graphics.SetCompositingMode(previous);
}

struct CSMRRadar::MainAvisoCacheView
{
	// References to this render pass's projection; no drawing state is owned here.
	CSMRRadar& radar;
	int viewportZoomLevel;
	bool& backgroundSatisfied;
	HDC hDC;
	Gdiplus::Graphics& graphics;
	const std::string& path;
	const unsigned long long& groupGeneration;
	const double& displayMinLat;
	const double& displayMaxLat;
	const double& displayMinLon;
	const double& displayMaxLon;
	const CRect& radarArea;
	const double& latSpan;
	const double& lonSpan;
	const PointF& projectedTopLeft;
	const PointF& projectedTopRight;
	const PointF& projectedBottomLeft;
	const PointF& projectedBottomRight;
	const double& lonPixelTolerance;
	const double& latPixelTolerance;
	const double& transformPixelTolerance;

	bool blendCache(const RECT& source, const RECT& destination)
	{
		VsmrAviso::RasterBlitPlan plan;
		if (!VsmrAviso::TryBuildRasterBlitPlan(source, destination, radarArea, plan))
			return false;
		const bool opaque = radar.AvisoGeoJsonRasterBackgroundColor.GetAlpha() == 255;
		const bool coversBackground = opaque && plan.destination.left <= radarArea.left &&
			plan.destination.top <= radarArea.top && plan.destination.right >= radarArea.right &&
			plan.destination.bottom >= radarArea.bottom;
		if (!coversBackground && !backgroundSatisfied)
		{
			radar.DrawAvisoBackground(graphics, radarArea);
			backgroundSatisfied = true;
		}
		if (radar.AvisoRasterBlitterInstance == nullptr)
			radar.AvisoRasterBlitterInstance = std::make_unique<VsmrAviso::AvisoRasterBlitter>();
		const bool drawn = radar.AvisoRasterBlitterInstance->Blend(graphics, hDC,
			radar.AvisoGeoJsonRasterCache, source, destination, radarArea, opaque);
		backgroundSatisfied = backgroundSatisfied || (drawn && coversBackground);
		return drawn;
	}

	PointF projectScreenPoint(double longitude, double latitude)
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(projectedTopLeft.X) + static_cast<double>(projectedTopRight.X - projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(projectedBottomLeft.X) + static_cast<double>(projectedBottomRight.X - projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(projectedTopLeft.Y) + static_cast<double>(projectedTopRight.Y - projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(projectedBottomLeft.Y) + static_cast<double>(projectedBottomRight.Y - projectedBottomLeft.Y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	}

	bool rasterCacheTransformMatchesCurrentView()
	{
		if (radar.AvisoGeoJsonRasterDisplayScale != radar.GetDisplayScale())
			return false;
		if (radar.AvisoGeoJsonRasterCache == nullptr || !radar.AvisoGeoJsonRasterAnchorValid)
			return false;
		const double cachedLonSpan = radar.AvisoGeoJsonRasterMaxLongitude - radar.AvisoGeoJsonRasterMinLongitude;
		const double cachedLatSpan = radar.AvisoGeoJsonRasterMaxLatitude - radar.AvisoGeoJsonRasterMinLatitude;
		if (cachedLonSpan <= 0.0 || cachedLatSpan <= 0.0 || lonSpan <= 0.0 || latSpan <= 0.0)
			return false;

		const double cachedHorizontalX = radar.AvisoGeoJsonRasterProjectedTopRight.X - radar.AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedHorizontalY = radar.AvisoGeoJsonRasterProjectedTopRight.Y - radar.AvisoGeoJsonRasterProjectedTopLeft.Y;
		const double cachedVerticalX = radar.AvisoGeoJsonRasterProjectedBottomLeft.X - radar.AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedVerticalY = radar.AvisoGeoJsonRasterProjectedBottomLeft.Y - radar.AvisoGeoJsonRasterProjectedTopLeft.Y;
		const double currentHorizontalX = static_cast<double>(projectedTopRight.X - projectedTopLeft.X);
		const double currentHorizontalY = static_cast<double>(projectedTopRight.Y - projectedTopLeft.Y);
		const double currentVerticalX = static_cast<double>(projectedBottomLeft.X - projectedTopLeft.X);
		const double currentVerticalY = static_cast<double>(projectedBottomLeft.Y - projectedTopLeft.Y);

		// Zooming changes the geographic span, not the viewport's projection
		// basis. The cached raster can therefore remain geo-anchored while the
		// worker produces the definitive bitmap for the new scale.
		const bool sameViewportBasis =
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY, transformPixelTolerance);
		if (sameViewportBasis)
			return true;

		// A snapped-divider resize keeps the geographic pixel scale while changing
		// the visible span, so retain the existing span-normalized compatibility.
		const double horizontalSpanRatio = cachedLonSpan / lonSpan;
		const double verticalSpanRatio = cachedLatSpan / latSpan;
		return
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX * horizontalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY * horizontalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX * verticalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY * verticalSpanRatio, transformPixelTolerance);
	}

	bool cacheMatchesCurrentView()
	{
		return radar.AvisoGeoJsonRasterCache != nullptr &&
			radar.AvisoGeoJsonRasterViewportZoomLevel == viewportZoomLevel &&
			radar.AvisoGeoJsonRasterCachePath == path &&
			radar.AvisoGeoJsonRasterGroupGeneration == groupGeneration &&
			AvisoWithinTolerance(radar.AvisoGeoJsonRasterMinLongitude, displayMinLon, lonPixelTolerance) &&
			AvisoWithinTolerance(radar.AvisoGeoJsonRasterMinLatitude, displayMinLat, latPixelTolerance) &&
			AvisoWithinTolerance(radar.AvisoGeoJsonRasterMaxLongitude, displayMaxLon, lonPixelTolerance) &&
			AvisoWithinTolerance(radar.AvisoGeoJsonRasterMaxLatitude, displayMaxLat, latPixelTolerance) &&
			rasterCacheTransformMatchesCurrentView();
	}

	bool drawRasterCacheTransformed()
	{
		if (radar.AvisoGeoJsonRasterCache == nullptr || radar.AvisoGeoJsonRasterWidth <= 0 || radar.AvisoGeoJsonRasterHeight <= 0)
			return false;
		if (radar.AvisoGeoJsonRasterCachePath != path)
			return false;
		if (radar.AvisoGeoJsonRasterColorPalette != radar.AvisoColorPalette ||
			radar.AvisoGeoJsonRasterBackgroundColor.GetValue() != radar.GetAvisoRasterBackgroundColor().GetValue())
			return false;
		if (radar.AvisoGeoJsonRasterGroupGeneration != groupGeneration)
			return false;
		if (!radar.AvisoGeoJsonRasterAnchorValid)
			return false;
		if (!rasterCacheTransformMatchesCurrentView())
			return false;

		const double cachedViewportLonSpan = std::abs(radar.AvisoGeoJsonRasterBottomRightLongitude - radar.AvisoGeoJsonRasterAnchorLongitude);
		const double cachedViewportLatSpan = std::abs(radar.AvisoGeoJsonRasterAnchorLatitude - radar.AvisoGeoJsonRasterBottomRightLatitude);
		if (cachedViewportLonSpan <= 0.0 || cachedViewportLatSpan <= 0.0)
			return false;

		const PointF destTopLeft = projectScreenPoint(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterAnchorLatitude);
		const PointF destTopRight = projectScreenPoint(radar.AvisoGeoJsonRasterBottomRightLongitude, radar.AvisoGeoJsonRasterAnchorLatitude);
		const PointF destBottomLeft = projectScreenPoint(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const PointF destBottomRight = projectScreenPoint(radar.AvisoGeoJsonRasterBottomRightLongitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const double destX = AvisoMin(AvisoMin(destTopLeft.X, destTopRight.X), AvisoMin(destBottomLeft.X, destBottomRight.X));
		const double destY = AvisoMin(AvisoMin(destTopLeft.Y, destTopRight.Y), AvisoMin(destBottomLeft.Y, destBottomRight.Y));
		const double destRight = AvisoMax(AvisoMax(destTopLeft.X, destTopRight.X), AvisoMax(destBottomLeft.X, destBottomRight.X));
		const double destBottom = AvisoMax(AvisoMax(destTopLeft.Y, destTopRight.Y), AvisoMax(destBottomLeft.Y, destBottomRight.Y));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		if (destRight < static_cast<double>(radarArea.left) ||
			destX > static_cast<double>(radarArea.right) ||
			destBottom < static_cast<double>(radarArea.top) ||
			destY > static_cast<double>(radarArea.bottom))
		{
			return false;
		}

		const double visibleLeft = AvisoMax(destX, static_cast<double>(radarArea.left));
		const double visibleTop = AvisoMax(destY, static_cast<double>(radarArea.top));
		const double visibleRight = AvisoMin(destRight, static_cast<double>(radarArea.right));
		const double visibleBottom = AvisoMin(destBottom, static_cast<double>(radarArea.bottom));
		const double visibleWidth = visibleRight - visibleLeft;
		const double visibleHeight = visibleBottom - visibleTop;
		if (visibleWidth < 1.0 || visibleHeight < 1.0)
			return false;

		const double sourceScaleX = static_cast<double>(radar.AvisoGeoJsonRasterWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(radar.AvisoGeoJsonRasterHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, radar.AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, radar.AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, radar.AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, radar.AvisoGeoJsonRasterHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		// Keep the expanded integer source crop on the same transform as the
		// floating-point destination. Mapping independently rounded rectangles
		// shifts the cached preview by one or more scaled source pixels.
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
		return blendCache(sourceRect, destinationRect);
	}

	bool drawRasterCacheViewportAligned()
	{
		if (radar.AvisoGeoJsonRasterCache == nullptr ||
			radar.AvisoGeoJsonRasterCachePath != path ||
			radar.AvisoGeoJsonRasterColorPalette != radar.AvisoColorPalette ||
			radar.AvisoGeoJsonRasterDisplayScale != radar.GetDisplayScale() ||
			radar.AvisoGeoJsonRasterBackgroundColor.GetValue() != radar.GetAvisoRasterBackgroundColor().GetValue() ||
			radar.AvisoGeoJsonRasterWidth <= 0 ||
			radar.AvisoGeoJsonRasterHeight <= 0 ||
			!radar.AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = radar.AvisoGeoJsonRasterMaxLongitude - radar.AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = radar.AvisoGeoJsonRasterMaxLatitude - radar.AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		auto projectCachedPoint = [&](double longitude, double latitude) -> PointF
		{
			const double u = (longitude - radar.AvisoGeoJsonRasterMinLongitude) / cachedDisplayLonSpan;
			const double v = (radar.AvisoGeoJsonRasterMaxLatitude - latitude) / cachedDisplayLatSpan;
			const double topX = static_cast<double>(radar.AvisoGeoJsonRasterProjectedTopLeft.X) + static_cast<double>(radar.AvisoGeoJsonRasterProjectedTopRight.X - radar.AvisoGeoJsonRasterProjectedTopLeft.X) * u;
			const double bottomX = static_cast<double>(radar.AvisoGeoJsonRasterProjectedBottomLeft.X) + static_cast<double>(radar.AvisoGeoJsonRasterProjectedBottomRight.X - radar.AvisoGeoJsonRasterProjectedBottomLeft.X) * u;
			const double topY = static_cast<double>(radar.AvisoGeoJsonRasterProjectedTopLeft.Y) + static_cast<double>(radar.AvisoGeoJsonRasterProjectedTopRight.Y - radar.AvisoGeoJsonRasterProjectedTopLeft.Y) * u;
			const double bottomY = static_cast<double>(radar.AvisoGeoJsonRasterProjectedBottomLeft.Y) + static_cast<double>(radar.AvisoGeoJsonRasterProjectedBottomRight.Y - radar.AvisoGeoJsonRasterProjectedBottomLeft.Y) * u;
			return PointF(
				static_cast<REAL>(topX + (bottomX - topX) * v),
				static_cast<REAL>(topY + (bottomY - topY) * v));
		};

		const PointF cachedRenderTopLeft = projectCachedPoint(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderTopRight = projectCachedPoint(radar.AvisoGeoJsonRasterBottomRightLongitude, radar.AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderBottomLeft = projectCachedPoint(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const PointF cachedRenderBottomRight = projectCachedPoint(radar.AvisoGeoJsonRasterBottomRightLongitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const double cachedRenderLeft = AvisoMin(AvisoMin(cachedRenderTopLeft.X, cachedRenderTopRight.X), AvisoMin(cachedRenderBottomLeft.X, cachedRenderBottomRight.X));
		const double cachedRenderTop = AvisoMin(AvisoMin(cachedRenderTopLeft.Y, cachedRenderTopRight.Y), AvisoMin(cachedRenderBottomLeft.Y, cachedRenderBottomRight.Y));
		const double cachedRenderRight = AvisoMax(AvisoMax(cachedRenderTopLeft.X, cachedRenderTopRight.X), AvisoMax(cachedRenderBottomLeft.X, cachedRenderBottomRight.X));
		const double cachedRenderBottom = AvisoMax(AvisoMax(cachedRenderTopLeft.Y, cachedRenderTopRight.Y), AvisoMax(cachedRenderBottomLeft.Y, cachedRenderBottomRight.Y));
		const double cachedRenderWidth = cachedRenderRight - cachedRenderLeft;
		const double cachedRenderHeight = cachedRenderBottom - cachedRenderTop;
		if (cachedRenderWidth < 1.0 || cachedRenderHeight < 1.0)
			return false;

		const PointF sourceTopLeft = projectCachedPoint(displayMinLon, displayMaxLat);
		const PointF sourceTopRight = projectCachedPoint(displayMaxLon, displayMaxLat);
		const PointF sourceBottomLeft = projectCachedPoint(displayMinLon, displayMinLat);
		const PointF sourceBottomRight = projectCachedPoint(displayMaxLon, displayMinLat);
		const double sourceScreenLeft = AvisoMin(AvisoMin(sourceTopLeft.X, sourceTopRight.X), AvisoMin(sourceBottomLeft.X, sourceBottomRight.X));
		const double sourceScreenTop = AvisoMin(AvisoMin(sourceTopLeft.Y, sourceTopRight.Y), AvisoMin(sourceBottomLeft.Y, sourceBottomRight.Y));
		const double sourceScreenRight = AvisoMax(AvisoMax(sourceTopLeft.X, sourceTopRight.X), AvisoMax(sourceBottomLeft.X, sourceBottomRight.X));
		const double sourceScreenBottom = AvisoMax(AvisoMax(sourceTopLeft.Y, sourceTopRight.Y), AvisoMax(sourceBottomLeft.Y, sourceBottomRight.Y));

		const double sourceScaleX = static_cast<double>(radar.AvisoGeoJsonRasterWidth) / cachedRenderWidth;
		const double sourceScaleY = static_cast<double>(radar.AvisoGeoJsonRasterHeight) / cachedRenderHeight;
		const double sourceX = (sourceScreenLeft - cachedRenderLeft) * sourceScaleX;
		const double sourceY = (sourceScreenTop - cachedRenderTop) * sourceScaleY;
		const double sourceRight = (sourceScreenRight - cachedRenderLeft) * sourceScaleX;
		const double sourceBottom = (sourceScreenBottom - cachedRenderTop) * sourceScaleY;
		const double sourceWidth = sourceRight - sourceX;
		const double sourceHeight = sourceBottom - sourceY;
		if (sourceWidth < 1.0 || sourceHeight < 1.0)
			return false;

		// A zoom preview must have the whole current viewport in the cached
		// overscan. Partial clamping would stretch the wrong geographic region.
		const double coverageTolerance = 1e-6;
		if (sourceX < -coverageTolerance ||
			sourceY < -coverageTolerance ||
			sourceRight > static_cast<double>(radar.AvisoGeoJsonRasterWidth) + coverageTolerance ||
			sourceBottom > static_cast<double>(radar.AvisoGeoJsonRasterHeight) + coverageTolerance)
		{
			return false;
		}

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceRight));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceBottom));
		sourceXInt = std::clamp(sourceXInt, 0, radar.AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, radar.AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, radar.AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, radar.AvisoGeoJsonRasterHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		const double destX = AvisoMin(AvisoMin(static_cast<double>(projectedTopLeft.X), static_cast<double>(projectedTopRight.X)), AvisoMin(static_cast<double>(projectedBottomLeft.X), static_cast<double>(projectedBottomRight.X)));
		const double destY = AvisoMin(AvisoMin(static_cast<double>(projectedTopLeft.Y), static_cast<double>(projectedTopRight.Y)), AvisoMin(static_cast<double>(projectedBottomLeft.Y), static_cast<double>(projectedBottomRight.Y)));
		const double destRight = AvisoMax(AvisoMax(static_cast<double>(projectedTopLeft.X), static_cast<double>(projectedTopRight.X)), AvisoMax(static_cast<double>(projectedBottomLeft.X), static_cast<double>(projectedBottomRight.X)));
		const double destBottom = AvisoMax(AvisoMax(static_cast<double>(projectedTopLeft.Y), static_cast<double>(projectedTopRight.Y)), AvisoMax(static_cast<double>(projectedBottomLeft.Y), static_cast<double>(projectedBottomRight.Y)));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		// Expand the destination by exactly the amount used to round the source
		// crop. This keeps the zoom center fixed when AlphaBlend receives integers.
		const double destPerSourceX = destWidth / sourceWidth;
		const double destPerSourceY = destHeight / sourceHeight;
		const double alignedDestLeft = destX + (static_cast<double>(sourceXInt) - sourceX) * destPerSourceX;
		const double alignedDestTop = destY + (static_cast<double>(sourceYInt) - sourceY) * destPerSourceY;
		const double alignedDestRight = destX + (static_cast<double>(sourceRightInt) - sourceX) * destPerSourceX;
		const double alignedDestBottom = destY + (static_cast<double>(sourceBottomInt) - sourceY) * destPerSourceY;
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
		return blendCache(sourceRect, destinationRect);
	}

	bool rasterCacheHasCompatibleZoom()
	{
		if (radar.AvisoGeoJsonRasterCache == nullptr ||
			radar.AvisoGeoJsonRasterViewportZoomLevel != viewportZoomLevel ||
			radar.AvisoGeoJsonRasterCachePath != path ||
			radar.AvisoGeoJsonRasterGroupGeneration != groupGeneration ||
			!radar.AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = radar.AvisoGeoJsonRasterMaxLongitude - radar.AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = radar.AvisoGeoJsonRasterMaxLatitude - radar.AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		return
			lonScaleRatio >= 0.985 && lonScaleRatio <= 1.015 &&
			latScaleRatio >= 0.985 && latScaleRatio <= 1.015;
	}

	bool rasterCacheHasWorkingMargin()
	{
		if (!rasterCacheHasCompatibleZoom())
			return false;

		const double cachedRenderMinLon = AvisoMin(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMaxLon = AvisoMax(radar.AvisoGeoJsonRasterAnchorLongitude, radar.AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMinLat = AvisoMin(radar.AvisoGeoJsonRasterAnchorLatitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const double cachedRenderMaxLat = AvisoMax(radar.AvisoGeoJsonRasterAnchorLatitude, radar.AvisoGeoJsonRasterBottomRightLatitude);
		const double requiredLonMargin = VsmrAviso::RasterWorkingMargin(lonSpan,
			radar.AvisoGeoJsonRasterMaxLongitude - radar.AvisoGeoJsonRasterMinLongitude,
			cachedRenderMaxLon - cachedRenderMinLon);
		const double requiredLatMargin = VsmrAviso::RasterWorkingMargin(latSpan,
			radar.AvisoGeoJsonRasterMaxLatitude - radar.AvisoGeoJsonRasterMinLatitude,
			cachedRenderMaxLat - cachedRenderMinLat);
		return
			cachedRenderMinLon <= displayMinLon - requiredLonMargin &&
			cachedRenderMaxLon >= displayMaxLon + requiredLonMargin &&
			cachedRenderMinLat <= displayMinLat - requiredLatMargin &&
			cachedRenderMaxLat >= displayMaxLat + requiredLatMargin;
	}

	bool avisoRasterUpdatePending()
	{
		return radar.AvisoGeoJsonRenderPipeline != nullptr &&
			radar.AvisoGeoJsonRenderPipeline->HasPendingWork();
	}
};

void CSMRRadar::RenderAvisoGeoJson(HDC hDC, Gdiplus::Graphics& graphics)
{
	if (IsShutdownRequested())
		return;

	const std::string path = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());
	if (path.empty())
		return;

	if (AvisoGeoJsonRenderDisabled)
	{
		bool sameFile = AvisoGeoJsonRenderDisabledPath.empty() || AvisoGeoJsonRenderDisabledPath == path;
		try
		{
			sameFile = sameFile && AvisoGeoJsonLoadedPath == path && AvisoGeoJsonLoadedWriteTime == fs::last_write_time(fs::u8path(path));
		}
		catch (...)
		{
			sameFile = true;
		}

		if (sameFile)
			return;

		AvisoGeoJsonRenderDisabled = false;
		AvisoGeoJsonRenderDisabledPath.clear();
	}

	if (!EnsureAvisoGeoJsonLoaded(path))
	{
		return;
	}
	CRect backgroundArea = ResolveMainAvisoRenderArea();
	backgroundArea.NormalizeRect();
	bool backgroundSatisfied = false;
	struct BackgroundOnExit
	{
		CSMRRadar& radar;
		Gdiplus::Graphics& graphics;
		const CRect& area;
		bool& satisfied;
		~BackgroundOnExit()
		{
			if (!satisfied && !area.IsRectEmpty())
				radar.DrawAvisoBackground(graphics, area);
		}
	} backgroundOnExit{ *this, graphics, backgroundArea, backgroundSatisfied };
	if (AvisoGeoJsonFeatureCount == 0 && AvisoGeoJsonLabelCount == 0)
		return;
	std::shared_ptr<const std::vector<AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> labelSnapshot;
	std::shared_ptr<const VsmrAviso::AvisoRenderScene> renderScene;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	unsigned long long groupGeneration = 0;
	if (!GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		groupGeneration,
		&renderScene))
	{
		return;
	}

	if (AvisoGeoJsonHasBounds && AvisoGeoJsonViewInitializedPath != path)
	{
		CPosition currentDisplayA;
		CPosition currentDisplayB;
		GetDisplayArea(&currentDisplayA, &currentDisplayB);

		const double displayMinLat = AvisoMin(currentDisplayA.m_Latitude, currentDisplayB.m_Latitude);
		const double displayMaxLat = AvisoMax(currentDisplayA.m_Latitude, currentDisplayB.m_Latitude);
		const double displayMinLon = AvisoMin(currentDisplayA.m_Longitude, currentDisplayB.m_Longitude);
		const double displayMaxLon = AvisoMax(currentDisplayA.m_Longitude, currentDisplayB.m_Longitude);
		const bool displayOverlapsAviso =
			AvisoGeoJsonMaxLatitude >= displayMinLat &&
			AvisoGeoJsonMinLatitude <= displayMaxLat &&
			AvisoGeoJsonMaxLongitude >= displayMinLon &&
			AvisoGeoJsonMinLongitude <= displayMaxLon;

		if (!displayOverlapsAviso)
		{
			const double latSpan = AvisoGeoJsonMaxLatitude - AvisoGeoJsonMinLatitude;
			const double lonSpan = AvisoGeoJsonMaxLongitude - AvisoGeoJsonMinLongitude;
			const double latPadding = AvisoMax(latSpan * 0.08, 0.001);
			const double lonPadding = AvisoMax(lonSpan * 0.08, 0.001);

			CPosition leftDown;
			leftDown.m_Latitude = AvisoGeoJsonMinLatitude - latPadding;
			leftDown.m_Longitude = AvisoGeoJsonMinLongitude - lonPadding;

			CPosition rightUp;
			rightUp.m_Latitude = AvisoGeoJsonMaxLatitude + latPadding;
			rightUp.m_Longitude = AvisoGeoJsonMaxLongitude + lonPadding;

			SetDisplayArea(leftDown, rightUp);
			Logger::info("AVISO GeoJSON fitted display path=" + path);
		}

		AvisoGeoJsonViewInitializedPath = path;
	}

	CPosition displayA;
	CPosition displayB;
	GetDisplayArea(&displayA, &displayB);

	const double fullDisplayMinLat = AvisoMin(displayA.m_Latitude, displayB.m_Latitude);
	const double fullDisplayMaxLat = AvisoMax(displayA.m_Latitude, displayB.m_Latitude);
	const double fullDisplayMinLon = AvisoMin(displayA.m_Longitude, displayB.m_Longitude);
	const double fullDisplayMaxLon = AvisoMax(displayA.m_Longitude, displayB.m_Longitude);
	const double fullDisplayLatSpan = fullDisplayMaxLat - fullDisplayMinLat;
	const double fullDisplayLonSpan = fullDisplayMaxLon - fullDisplayMinLon;
	if (fullDisplayLatSpan <= 0.0 || fullDisplayLonSpan <= 0.0)
		return;

	auto makeDisplayPosition = [](double latitude, double longitude) -> CPosition
	{
		CPosition position;
		position.m_Latitude = latitude;
		position.m_Longitude = longitude;
		return position;
	};

	// Keep one projection basis for the complete EuroScope view. The visible
	// main area may be cropped by a snapped inset, but rebuilding the basis from
	// pixel-to-coordinate-to-integer-pixel round trips makes the map twitch by a
	// pixel as the divider moves.
	const POINT fullProjectedTopLeft = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMaxLat, fullDisplayMinLon));
	const POINT fullProjectedTopRight = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon));
	const POINT fullProjectedBottomLeft = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon));
	const POINT fullProjectedBottomRight = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMinLat, fullDisplayMaxLon));
	auto projectFullDisplayPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - fullDisplayMinLon) / fullDisplayLonSpan;
		const double v = (fullDisplayMaxLat - latitude) / fullDisplayLatSpan;
		const double topX = static_cast<double>(fullProjectedTopLeft.x) +
			static_cast<double>(fullProjectedTopRight.x - fullProjectedTopLeft.x) * u;
		const double bottomX = static_cast<double>(fullProjectedBottomLeft.x) +
			static_cast<double>(fullProjectedBottomRight.x - fullProjectedBottomLeft.x) * u;
		const double topY = static_cast<double>(fullProjectedTopLeft.y) +
			static_cast<double>(fullProjectedTopRight.y - fullProjectedTopLeft.y) * u;
		const double bottomY = static_cast<double>(fullProjectedBottomLeft.y) +
			static_cast<double>(fullProjectedBottomRight.y - fullProjectedBottomLeft.y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};
	double displayMinLat = fullDisplayMinLat;
	double displayMaxLat = fullDisplayMaxLat;
	double displayMinLon = fullDisplayMinLon;
	double displayMaxLon = fullDisplayMaxLon;

	CRect fullRadarArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	fullRadarArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		fullRadarArea.bottom = chatArea.top;
	fullRadarArea.NormalizeRect();
	CRect radarArea = ResolveMainAvisoRenderArea();
	if (radarArea.IsRectEmpty())
		return;

	if (radarArea != fullRadarArea)
	{
		const POINT visibleCorners[] = {
			{ radarArea.left, radarArea.top },
			{ radarArea.right, radarArea.top },
			{ radarArea.left, radarArea.bottom },
			{ radarArea.right, radarArea.bottom }
		};
		double visibleMinLat = DBL_MAX;
		double visibleMaxLat = -DBL_MAX;
		double visibleMinLon = DBL_MAX;
		double visibleMaxLon = -DBL_MAX;
		bool visibleBoundsValid = true;
		for (const POINT& corner : visibleCorners)
		{
			const CPosition position = ConvertCoordFromPixelToPosition(corner);
			if (!std::isfinite(position.m_Latitude) || !std::isfinite(position.m_Longitude))
			{
				visibleBoundsValid = false;
				break;
			}
			visibleMinLat = AvisoMin(visibleMinLat, position.m_Latitude);
			visibleMaxLat = AvisoMax(visibleMaxLat, position.m_Latitude);
			visibleMinLon = AvisoMin(visibleMinLon, position.m_Longitude);
			visibleMaxLon = AvisoMax(visibleMaxLon, position.m_Longitude);
		}
		if (visibleBoundsValid)
		{
			displayMinLat = AvisoMax(fullDisplayMinLat, visibleMinLat);
			displayMaxLat = AvisoMin(fullDisplayMaxLat, visibleMaxLat);
			displayMinLon = AvisoMax(fullDisplayMinLon, visibleMinLon);
			displayMaxLon = AvisoMin(fullDisplayMaxLon, visibleMaxLon);
		}
	}
	const double latSpan = displayMaxLat - displayMinLat;
	const double lonSpan = displayMaxLon - displayMinLon;
	if (latSpan <= 0.0 || lonSpan <= 0.0)
		return;

	const double width = static_cast<double>(radarArea.right - radarArea.left);
	const double height = static_cast<double>(radarArea.bottom - radarArea.top);
	if (width <= 0.0 || height <= 0.0)
		return;

	const double fallbackScaleX = width / lonSpan;
	const double fallbackScaleY = height / latSpan;

	const PointF projectedTopLeft = projectFullDisplayPoint(displayMinLon, displayMaxLat);
	const PointF projectedTopRight = projectFullDisplayPoint(displayMaxLon, displayMaxLat);
	const PointF projectedBottomLeft = projectFullDisplayPoint(displayMinLon, displayMinLat);
	const PointF projectedBottomRight = projectFullDisplayPoint(displayMaxLon, displayMinLat);

	const double projectedWidthTop = VsmrAviso::ProjectedAxisLength(projectedTopLeft, projectedTopRight);
	const double projectedWidthBottom = VsmrAviso::ProjectedAxisLength(projectedBottomLeft, projectedBottomRight);
	const double projectedHeightLeft = VsmrAviso::ProjectedAxisLength(projectedTopLeft, projectedBottomLeft);
	const double projectedHeightRight = VsmrAviso::ProjectedAxisLength(projectedTopRight, projectedBottomRight);
	const double projectedWidth = AvisoMax(AvisoMax(projectedWidthTop, projectedWidthBottom), 1.0);
	const double projectedHeight = AvisoMax(AvisoMax(projectedHeightLeft, projectedHeightRight), 1.0);
	const double scaleX = projectedWidth > 1.0 ? projectedWidth / lonSpan : fallbackScaleX;
	const double scaleY = projectedHeight > 1.0 ? projectedHeight / latSpan : fallbackScaleY;

	const double viewPixelTolerance = 1.15;
	const double lonPixelTolerance = (1.0 / scaleX) * viewPixelTolerance;
	const double latPixelTolerance = (1.0 / scaleY) * viewPixelTolerance;
	const double transformPixelTolerance = 4.0;

	const int viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(
		SMRGeometry::DistanceMeters(makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon),
			makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon)));
	MainAvisoCacheView cacheView{ *this, viewportZoomLevel, backgroundSatisfied, hDC, graphics, path, groupGeneration, displayMinLat, displayMaxLat, displayMinLon, displayMaxLon, radarArea, latSpan, lonSpan, projectedTopLeft, projectedTopRight, projectedBottomLeft, projectedBottomRight, lonPixelTolerance, latPixelTolerance, transformPixelTolerance };

	ApplyCompletedAvisoGeoJsonRaster();
	if (cacheView.cacheMatchesCurrentView() && cacheView.drawRasterCacheTransformed())
	{
		PerformanceDiagnostics.RecordAvisoCacheOutcome(
			VsmrPerformance::AvisoViewport::Main,
			VsmrPerformance::AvisoCacheOutcome::Exact,
			false,
			false);
		return;
	}

	// Reduce off-screen overscan before reducing visible 2K/4K resolution.
	const double overscanRatio = VsmrAviso::NativeResolutionOverscan(
		projectedTopLeft, projectedTopRight, projectedBottomLeft, projectedBottomRight, 32000000.0);
	const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
	const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
	const double renderMinLat = displayMinLat - (latSpan * overscanRatio);
	const double renderMaxLat = displayMaxLat + (latSpan * overscanRatio);
	const PointF renderTopLeft = cacheView.projectScreenPoint(renderMinLon, renderMaxLat);
	const PointF renderTopRight = cacheView.projectScreenPoint(renderMaxLon, renderMaxLat);
	const PointF renderBottomLeft = cacheView.projectScreenPoint(renderMinLon, renderMinLat);
	const PointF renderBottomRight = cacheView.projectScreenPoint(renderMaxLon, renderMinLat);
	const double renderScreenLeft = AvisoMin(AvisoMin(renderTopLeft.X, renderTopRight.X), AvisoMin(renderBottomLeft.X, renderBottomRight.X));
	const double renderScreenTop = AvisoMin(AvisoMin(renderTopLeft.Y, renderTopRight.Y), AvisoMin(renderBottomLeft.Y, renderBottomRight.Y));
	const double renderScreenRight = AvisoMax(AvisoMax(renderTopLeft.X, renderTopRight.X), AvisoMax(renderBottomLeft.X, renderBottomRight.X));
	const double renderScreenBottom = AvisoMax(AvisoMax(renderTopLeft.Y, renderTopRight.Y), AvisoMax(renderBottomLeft.Y, renderBottomRight.Y));
	const double renderPixelWidth = renderScreenRight - renderScreenLeft;
	const double renderPixelHeight = renderScreenBottom - renderScreenTop;
	if (renderPixelWidth <= 0.0 || renderPixelHeight <= 0.0)
		return;

	const double maxDimension = renderPixelWidth > renderPixelHeight ? renderPixelWidth : renderPixelHeight;
	const double targetRasterScale = 1.0;
	const double maxRasterSide = 6400.0;
	const double maxRasterPixels = 32000000.0;
	double rasterScale = targetRasterScale;
	const double sideLimitedScale = maxRasterSide / maxDimension;
	if (sideLimitedScale > 0.0 && sideLimitedScale < rasterScale)
		rasterScale = sideLimitedScale;
	const double pixelLimitedScale = std::sqrt(maxRasterPixels / (renderPixelWidth * renderPixelHeight));
	if (pixelLimitedScale > 0.0 && pixelLimitedScale < rasterScale)
		rasterScale = pixelLimitedScale;
	// The side/pixel caps are hard bounds. Only very large viewports fall below
	// half resolution; allowing that is safer than defeating the allocation cap.
	rasterScale = AvisoMin(rasterScale, targetRasterScale);
	int rasterWidth = static_cast<int>(std::floor(renderPixelWidth * rasterScale));
	int rasterHeight = static_cast<int>(std::floor(renderPixelHeight * rasterScale));
	if (rasterWidth < 1)
		rasterWidth = 1;
	if (rasterHeight < 1)
		rasterHeight = 1;

	AvisoRasterRenderRequest request;
	request.groupGeneration = groupGeneration;
	request.path = path;
	request.features = featureSnapshot;
	request.labels = labelSnapshot;
	request.scene = renderScene;
	request.groupVisibility = groupVisibility;
	request.colorPalette = AvisoColorPalette;
	request.backgroundColor = GetAvisoRasterBackgroundColor();
	request.rasterWidth = rasterWidth;
	request.rasterHeight = rasterHeight;
	request.rasterScale = rasterScale;
	request.displayScale = GetDisplayScale();
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
	request.viewportZoomLevel = viewportZoomLevel;
	request.projectedTopLeft = projectedTopLeft;
	request.projectedTopRight = projectedTopRight;
	request.projectedBottomLeft = projectedBottomLeft;
	request.projectedBottomRight = projectedBottomRight;

	// A divider resize changes the visible geographic span, but not the map's
	// pixel scale. Prefer the geo-anchored cache whenever its transform matches;
	// the compatibility/margin check still decides whether to refresh it.
	if (cacheView.drawRasterCacheTransformed())
	{
		bool updateRequested = false;
		if (!cacheView.rasterCacheHasWorkingMargin())
		{
			updateRequested = true;
			QueueAvisoGeoJsonRasterRender(std::move(request));
		}
		const bool delayedByAvisoUpdate = updateRequested && cacheView.avisoRasterUpdatePending();
		PerformanceDiagnostics.RecordAvisoCacheOutcome(
			VsmrPerformance::AvisoViewport::Main,
			delayedByAvisoUpdate
				? VsmrPerformance::AvisoCacheOutcome::Preview
				: VsmrPerformance::AvisoCacheOutcome::Exact,
			delayedByAvisoUpdate,
			false);
		return;
	}

	QueueAvisoGeoJsonRasterRender(std::move(request));
	const bool fallbackCacheDrawn = cacheView.drawRasterCacheViewportAligned();
	const bool delayedByAvisoUpdate = cacheView.avisoRasterUpdatePending();
	PerformanceDiagnostics.RecordAvisoCacheOutcome(
		VsmrPerformance::AvisoViewport::Main,
		fallbackCacheDrawn
			? VsmrPerformance::AvisoCacheOutcome::Preview
			: VsmrPerformance::AvisoCacheOutcome::Miss,
		delayedByAvisoUpdate,
		!fallbackCacheDrawn);
	if (fallbackCacheDrawn)
		return;
}
