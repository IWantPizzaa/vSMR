#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterSizing.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/InsetWindow.Internal.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "radar/RadarScreen.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/logging/Logger.hpp"
#include <chrono>
#include <mutex>

using VsmrRadarUiSupport::CopyRect;
using VsmrRadarUiSupport::DegToRad;
using VsmrRadarUiSupport::mouseWithin;
using VsmrInsetWindowInternal::AvisoCosLatitude;
using VsmrInsetWindowInternal::AvisoProjectionTransformWithinTolerance;
using VsmrInsetWindowInternal::AvisoRectIntersects;
using VsmrInsetWindowInternal::ClampAvisoLatitude;
using VsmrInsetWindowInternal::DrawInsetWindowChrome;
using VsmrInsetWindowInternal::DrawRadarInsetBorder;
using VsmrInsetWindowInternal::ResolveAvisoViewportScreenRotationDeg;
using VsmrInsetWindowInternal::RotateAvisoPointAround;
using VsmrInsetWindowInternal::SceneColorToGdi;
using VsmrInsetWindowInternal::kAvisoLatMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoLonMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoMetersPerNm;

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
	std::string cachePath;
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

void CInsetWindow::CancelAvisoViewportRender()
{
	if (m_AvisoState == nullptr)
		return;

	m_AvisoState->StopRenderPipeline();
}

double CInsetWindow::GetAvisoViewportScreenRotationDeg() const noexcept
{
	return m_AvisoState != nullptr ? m_AvisoState->screenRotationDeg : 0.0;
}

struct CInsetWindow::InsetAvisoCacheView
{
	// The viewport function owns these values and the lifetime of its GDI state.
	CInsetWindow& inset;
	HDC hDC;
	Gdiplus::Graphics* gdi;
	const CRect& viewportRect;
	const std::string& path;
	const unsigned long long& groupGeneration;
	const double& displayMinLon;
	const double& displayMaxLon;
	const double& displayMinLat;
	const double& displayMaxLat;
	const double& lonSpan;
	const double& latSpan;
	const Gdiplus::PointF& projectedTopLeft;
	const Gdiplus::PointF& projectedTopRight;
	const Gdiplus::PointF& projectedBottomLeft;
	const Gdiplus::PointF& projectedBottomRight;

	Gdiplus::PointF projectPoint(double longitude, double latitude) const
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
	}

	bool cacheTransformMatchesCurrentView()
	{
		if (inset.m_AvisoState->cacheBitmap == nullptr || !inset.m_AvisoState->anchorValid)
			return false;
		if (inset.m_AvisoState->cacheGroupGeneration != groupGeneration)
			return false;

		const double cachedLongitudeSpan =
			inset.m_AvisoState->displayMaxLongitude - inset.m_AvisoState->displayMinLongitude;
		const double cachedLatitudeSpan =
			inset.m_AvisoState->displayMaxLatitude - inset.m_AvisoState->displayMinLatitude;
		const double transformPixelTolerance = 12.0;
		return AvisoProjectionTransformWithinTolerance(
			inset.m_AvisoState->projectedTopLeft,
			inset.m_AvisoState->projectedTopRight,
			inset.m_AvisoState->projectedBottomLeft,
			cachedLongitudeSpan,
			cachedLatitudeSpan,
			projectedTopLeft,
			projectedTopRight,
			projectedBottomLeft,
			lonSpan,
			latSpan,
			transformPixelTolerance);
	}

	bool completedResultMatchesCurrentView(const CSMRRadar::AvisoRasterRenderResult& result)
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
	}

	bool drawCache()
	{
		if (inset.m_AvisoState->cacheBitmap == nullptr ||
			inset.m_AvisoState->cachePath != path ||
			inset.m_AvisoState->cacheGroupGeneration != groupGeneration ||
			inset.m_AvisoState->cacheWidth <= 0 ||
			inset.m_AvisoState->cacheHeight <= 0 ||
			!inset.m_AvisoState->anchorValid)
		{
			return false;
		}
		if (!cacheTransformMatchesCurrentView())
			return false;

		const Gdiplus::PointF destTopLeft = projectPoint(inset.m_AvisoState->renderMinLongitude, inset.m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF destTopRight = projectPoint(inset.m_AvisoState->renderMaxLongitude, inset.m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF destBottomLeft = projectPoint(inset.m_AvisoState->renderMinLongitude, inset.m_AvisoState->renderMinLatitude);
		const Gdiplus::PointF destBottomRight = projectPoint(inset.m_AvisoState->renderMaxLongitude, inset.m_AvisoState->renderMinLatitude);
		const double destX = (std::min)((std::min)(static_cast<double>(destTopLeft.X), static_cast<double>(destTopRight.X)), (std::min)(static_cast<double>(destBottomLeft.X), static_cast<double>(destBottomRight.X)));
		const double destY = (std::min)((std::min)(static_cast<double>(destTopLeft.Y), static_cast<double>(destTopRight.Y)), (std::min)(static_cast<double>(destBottomLeft.Y), static_cast<double>(destBottomRight.Y)));
		const double destRight = (std::max)((std::max)(static_cast<double>(destTopLeft.X), static_cast<double>(destTopRight.X)), (std::max)(static_cast<double>(destBottomLeft.X), static_cast<double>(destBottomRight.X)));
		const double destBottom = (std::max)((std::max)(static_cast<double>(destTopLeft.Y), static_cast<double>(destTopRight.Y)), (std::max)(static_cast<double>(destBottomLeft.Y), static_cast<double>(destBottomRight.Y)));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		const double visibleLeft = (std::max)(destX, static_cast<double>(viewportRect.left));
		const double visibleTop = (std::max)(destY, static_cast<double>(viewportRect.top));
		const double visibleRight = (std::min)(destRight, static_cast<double>(viewportRect.right));
		const double visibleBottom = (std::min)(destBottom, static_cast<double>(viewportRect.bottom));
		const double visibleWidth = visibleRight - visibleLeft;
		const double visibleHeight = visibleBottom - visibleTop;
		if (visibleWidth < 1.0 || visibleHeight < 1.0)
			return false;

		const double sourceScaleX = static_cast<double>(inset.m_AvisoState->cacheWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(inset.m_AvisoState->cacheHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, inset.m_AvisoState->cacheWidth);
		sourceYInt = std::clamp(sourceYInt, 0, inset.m_AvisoState->cacheHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, inset.m_AvisoState->cacheWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, inset.m_AvisoState->cacheHeight);
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
		return inset.m_AvisoState->rasterBlitter.Blend(
			*gdi,
			hDC,
			inset.m_AvisoState->cacheBitmap,
			sourceRect,
			destinationRect,
			viewportRect);
	}

	bool drawPreviousCacheViewportAligned()
	{
		// Retain a geographically covered previous raster while the definitive
		// view is debounced or rebuilt. A resize must not stretch a clamped crop
		// to a new aspect ratio; the normal geo-aligned path handles that case.
		if (inset.m_WindowResizeActive)
			return false;
		if (inset.m_AvisoState->cacheBitmap == nullptr ||
			inset.m_AvisoState->cachePath != path ||
			inset.m_AvisoState->cacheWidth <= 0 ||
			inset.m_AvisoState->cacheHeight <= 0 ||
			!inset.m_AvisoState->anchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = inset.m_AvisoState->displayMaxLongitude - inset.m_AvisoState->displayMinLongitude;
		const double cachedDisplayLatSpan = inset.m_AvisoState->displayMaxLatitude - inset.m_AvisoState->displayMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		auto projectCachedPoint = [&](double longitude, double latitude) -> Gdiplus::PointF
		{
			const double u = (longitude - inset.m_AvisoState->displayMinLongitude) / cachedDisplayLonSpan;
			const double v = (inset.m_AvisoState->displayMaxLatitude - latitude) / cachedDisplayLatSpan;
			const double topX = static_cast<double>(inset.m_AvisoState->projectedTopLeft.X) + static_cast<double>(inset.m_AvisoState->projectedTopRight.X - inset.m_AvisoState->projectedTopLeft.X) * u;
			const double bottomX = static_cast<double>(inset.m_AvisoState->projectedBottomLeft.X) + static_cast<double>(inset.m_AvisoState->projectedBottomRight.X - inset.m_AvisoState->projectedBottomLeft.X) * u;
			const double topY = static_cast<double>(inset.m_AvisoState->projectedTopLeft.Y) + static_cast<double>(inset.m_AvisoState->projectedTopRight.Y - inset.m_AvisoState->projectedTopLeft.Y) * u;
			const double bottomY = static_cast<double>(inset.m_AvisoState->projectedBottomLeft.Y) + static_cast<double>(inset.m_AvisoState->projectedBottomRight.Y - inset.m_AvisoState->projectedBottomLeft.Y) * u;
			return Gdiplus::PointF(
				static_cast<Gdiplus::REAL>(topX + (bottomX - topX) * v),
				static_cast<Gdiplus::REAL>(topY + (bottomY - topY) * v));
		};

		const Gdiplus::PointF renderTopLeft = projectCachedPoint(inset.m_AvisoState->renderMinLongitude, inset.m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF renderTopRight = projectCachedPoint(inset.m_AvisoState->renderMaxLongitude, inset.m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF renderBottomLeft = projectCachedPoint(inset.m_AvisoState->renderMinLongitude, inset.m_AvisoState->renderMinLatitude);
		const Gdiplus::PointF renderBottomRight = projectCachedPoint(inset.m_AvisoState->renderMaxLongitude, inset.m_AvisoState->renderMinLatitude);
		const double cachedRenderLeft = (std::min)((std::min)(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), (std::min)(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double cachedRenderTop = (std::min)((std::min)(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), (std::min)(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double cachedRenderRight = (std::max)((std::max)(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), (std::max)(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double cachedRenderBottom = (std::max)((std::max)(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), (std::max)(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double cachedRenderWidth = cachedRenderRight - cachedRenderLeft;
		const double cachedRenderHeight = cachedRenderBottom - cachedRenderTop;
		if (cachedRenderWidth < 1.0 || cachedRenderHeight < 1.0)
			return false;

		const Gdiplus::PointF sourceTopLeft = projectCachedPoint(displayMinLon, displayMaxLat);
		const Gdiplus::PointF sourceTopRight = projectCachedPoint(displayMaxLon, displayMaxLat);
		const Gdiplus::PointF sourceBottomLeft = projectCachedPoint(displayMinLon, displayMinLat);
		const Gdiplus::PointF sourceBottomRight = projectCachedPoint(displayMaxLon, displayMinLat);
		const double sourceLeft = (std::min)((std::min)(static_cast<double>(sourceTopLeft.X), static_cast<double>(sourceTopRight.X)), (std::min)(static_cast<double>(sourceBottomLeft.X), static_cast<double>(sourceBottomRight.X)));
		const double sourceTop = (std::min)((std::min)(static_cast<double>(sourceTopLeft.Y), static_cast<double>(sourceTopRight.Y)), (std::min)(static_cast<double>(sourceBottomLeft.Y), static_cast<double>(sourceBottomRight.Y)));
		const double sourceRight = (std::max)((std::max)(static_cast<double>(sourceTopLeft.X), static_cast<double>(sourceTopRight.X)), (std::max)(static_cast<double>(sourceBottomLeft.X), static_cast<double>(sourceBottomRight.X)));
		const double sourceBottom = (std::max)((std::max)(static_cast<double>(sourceTopLeft.Y), static_cast<double>(sourceTopRight.Y)), (std::max)(static_cast<double>(sourceBottomLeft.Y), static_cast<double>(sourceBottomRight.Y)));

		const double sourceScaleX = static_cast<double>(inset.m_AvisoState->cacheWidth) / cachedRenderWidth;
		const double sourceScaleY = static_cast<double>(inset.m_AvisoState->cacheHeight) / cachedRenderHeight;
		const double sourceX = (sourceLeft - cachedRenderLeft) * sourceScaleX;
		const double sourceY = (sourceTop - cachedRenderTop) * sourceScaleY;
		const double sourceRightRaster = (sourceRight - cachedRenderLeft) * sourceScaleX;
		const double sourceBottomRaster = (sourceBottom - cachedRenderTop) * sourceScaleY;
		const double coverageTolerance = 1e-6;
		if (sourceX < -coverageTolerance ||
			sourceY < -coverageTolerance ||
			sourceRightRaster > static_cast<double>(inset.m_AvisoState->cacheWidth) + coverageTolerance ||
			sourceBottomRaster > static_cast<double>(inset.m_AvisoState->cacheHeight) + coverageTolerance)
		{
			return false;
		}

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceRightRaster));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceBottomRaster));
		sourceXInt = std::clamp(sourceXInt, 0, inset.m_AvisoState->cacheWidth);
		sourceYInt = std::clamp(sourceYInt, 0, inset.m_AvisoState->cacheHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, inset.m_AvisoState->cacheWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, inset.m_AvisoState->cacheHeight);
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
		return inset.m_AvisoState->rasterBlitter.Blend(
			*gdi,
			hDC,
			inset.m_AvisoState->cacheBitmap,
			sourceRect,
			destinationRect,
			viewportRect);
	}

	bool cacheHasWorkingMargin()
	{
		if (inset.m_AvisoState->cacheBitmap == nullptr ||
			inset.m_AvisoState->cachePath != path ||
			inset.m_AvisoState->cacheGroupGeneration != groupGeneration ||
			!inset.m_AvisoState->anchorValid)
		{
			return false;
		}
		if (!cacheTransformMatchesCurrentView())
			return false;

		const double cachedDisplayLonSpan = inset.m_AvisoState->displayMaxLongitude - inset.m_AvisoState->displayMinLongitude;
		const double cachedDisplayLatSpan = inset.m_AvisoState->displayMaxLatitude - inset.m_AvisoState->displayMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		if (lonScaleRatio < 0.985 || lonScaleRatio > 1.015 ||
			latScaleRatio < 0.985 || latScaleRatio > 1.015)
		{
			return false;
		}

		const double requiredLonMargin = VsmrAviso::RasterWorkingMargin(lonSpan, cachedDisplayLonSpan,
			inset.m_AvisoState->renderMaxLongitude - inset.m_AvisoState->renderMinLongitude);
		const double requiredLatMargin = VsmrAviso::RasterWorkingMargin(latSpan, cachedDisplayLatSpan,
			inset.m_AvisoState->renderMaxLatitude - inset.m_AvisoState->renderMinLatitude);
		return
			inset.m_AvisoState->renderMinLongitude <= displayMinLon - requiredLonMargin &&
			inset.m_AvisoState->renderMaxLongitude >= displayMaxLon + requiredLonMargin &&
			inset.m_AvisoState->renderMinLatitude <= displayMinLat - requiredLatMargin &&
			inset.m_AvisoState->renderMaxLatitude >= displayMaxLat + requiredLatMargin;
	}
};

struct CInsetWindow::AvisoTagTarget
{
	const VsmrScene::Target* target = nullptr;
	POINT point = {};
};

struct CInsetWindow::AvisoTagPass
{
	CSMRRadar* radar_screen;
	Gdiplus::Graphics* gdi;
	const CRect& viewportRect;
	VsmrTagRendering::FontContext& tagFonts;
	const Value& activeProfile;
	const std::set<std::string>& previouslyDetailed;
	POINT mouseLocation;
	bool hoverAllowed;
	bool roundedTagCornersEnabled;
	Color rimcasStageOneColor;
	Color rimcasStageTwoColor;
};

void CInsetWindow::renderAvisoTags(std::vector<AvisoTagTarget>& visibleTagTargets, const AvisoTagPass& pass)
{
	const auto& radar_screen = pass.radar_screen;
	const auto& gdi = pass.gdi;
	const auto& viewportRect = pass.viewportRect;
	const auto& previouslyDetailed = pass.previouslyDetailed;
	const auto& mouseLocation = pass.mouseLocation;
	const auto& hoverAllowed = pass.hoverAllowed;
	const auto& roundedTagCornersEnabled = pass.roundedTagCornersEnabled;
	const auto& rimcasStageOneColor = pass.rimcasStageOneColor;
	const auto& rimcasStageTwoColor = pass.rimcasStageTwoColor;
	auto& tagFonts = pass.tagFonts;
	auto clipToViewport = [&](CRect rect) -> CRect
	{
		rect.NormalizeRect();
		CRect clipped;
		clipped.IntersectRect(rect, viewportRect);
		return clipped;
	};
	auto pointInViewport = [&](const POINT& point, int margin = 0) -> bool
	{
		return
			point.x >= viewportRect.left - margin &&
			point.x <= viewportRect.right + margin &&
			point.y >= viewportRect.top - margin &&
			point.y <= viewportRect.bottom + margin;
	};
	auto rectIntersectsViewport = [&](const CRect& rect) -> bool
	{
		return AvisoRectIntersects(rect, viewportRect);
	};
	// Symbols are complete before tag layout begins. This makes tag z-order
	// independent of the scene target order and keeps every tag above aircraft.
	std::sort(
		visibleTagTargets.begin(),
		visibleTagTargets.end(),
		[](const AvisoTagTarget& left, const AvisoTagTarget& right)
		{
			return left.target->callsign < right.target->callsign;
		});

	bool autoDeconflictionEnabled = true;
	const Value* labelsSection = (pass.activeProfile.IsObject() && pass.activeProfile.HasMember("labels") && pass.activeProfile["labels"].IsObject() ? &pass.activeProfile["labels"] : nullptr);
	if (labelsSection != nullptr &&
		labelsSection->HasMember("auto_deconfliction") &&
		(*labelsSection)["auto_deconfliction"].IsBool())
	{
		autoDeconflictionEnabled = (*labelsSection)["auto_deconfliction"].GetBool();
	}

	struct PreparedTag
	{
		const VsmrScene::Target* target = nullptr;
		VsmrTagRendering::Layout layout;
		VsmrTagRendering::PaintOptions options;
	};
	std::vector<PreparedTag> preparedTags;
	std::vector<CRect> occupiedTagBounds;
	constexpr int leaderLength = 50;
	constexpr double angleStep = 22.5;
	for (const AvisoTagTarget& visible : visibleTagTargets)
	{
		const VsmrScene::Target& sceneTarget = *visible.target;
		const std::string& callsign = sceneTarget.callsign;
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
				continue;
		}

		VsmrTagRendering::PaintOptions options;
		options.targetPoint = visible.point;
		const VsmrScene::TagPalette& palette = sceneTarget.tag.normalPalette;
		options.background = SceneColorToGdi(
			sceneTarget.rimcas.onRunway ? palette.backgroundOnRunway : palette.background);
		options.leaderColor = Gdiplus::Color(255, 255, 255, 255);
		options.roundedCorners = roundedTagCornersEnabled;
		options.centerLines = true;
		options.symmetricBounds = true;

		const auto customOffset = m_TagOffsets.find(callsign);
		const bool hasCustomOffset = customOffset != m_TagOffsets.end();
		double baseAngle = m_TagAngles.emplace(callsign, 45.0).first->second;
		int placementLeaderLength = leaderLength;
		if (hasCustomOffset)
		{
			placementLeaderLength = (std::max)(
				1,
				static_cast<int>(std::lround(std::hypot(
					static_cast<double>(customOffset->second.x),
					static_cast<double>(customOffset->second.y)))));
			baseAngle = VsmrRadarUiSupport::RadToDeg(std::atan2(
				static_cast<double>(customOffset->second.y),
				static_cast<double>(customOffset->second.x)));
			options.tagCenter = {
				visible.point.x + customOffset->second.x,
				visible.point.y + customOffset->second.y };
		}
		auto centerAtAngle = [&](double angle) -> POINT
		{
			return {
				static_cast<LONG>(visible.point.x + placementLeaderLength * cos(DegToRad(angle))),
				static_cast<LONG>(visible.point.y + placementLeaderLength * sin(DegToRad(angle))) };
		};
		if (!hasCustomOffset)
			options.tagCenter = centerAtAngle(baseAngle);
		auto collides = [&](const CRect& candidate) -> bool
		{
			CRect paddedCandidate(candidate);
			paddedCandidate.InflateRect(2, 2);
			for (const CRect& occupied : occupiedTagBounds)
			{
				CRect overlap;
				if (overlap.IntersectRect(paddedCandidate, occupied))
					return true;
			}
			return false;
		};

		if (autoDeconflictionEnabled)
		{
			double selectedAngle = baseAngle;
			for (int candidateIndex = 0; candidateIndex < 32; ++candidateIndex)
			{
				const int step = candidateIndex == 0
					? 0
					: ((candidateIndex + 1) / 2) * (candidateIndex % 2 == 1 ? 1 : -1);
				const double candidateAngle = baseAngle + step * angleStep;
				options.tagCenter = centerAtAngle(candidateAngle);
				const CRect candidateBounds =
					VsmrTagRendering::CalculateBounds(tagFonts, layout, options);
				if (!collides(candidateBounds))
				{
					selectedAngle = candidateAngle;
					break;
				}
			}
			selectedAngle = std::fmod(selectedAngle + 360.0, 360.0);
			if (!hasCustomOffset)
				m_TagAngles[callsign] = selectedAngle;
			options.tagCenter = centerAtAngle(selectedAngle);
		}

		const CRect expectedBounds =
			VsmrTagRendering::CalculateBounds(tagFonts, layout, options);
		if (!rectIntersectsViewport(expectedBounds) && !pointInViewport(visible.point, 20))
			continue;
		CRect occupiedBounds(expectedBounds);
		occupiedBounds.InflateRect(2, 2);
		occupiedTagBounds.push_back(occupiedBounds);
		// Use the normal footprint for placement so expansion cannot move the tag.
		const VsmrTagRendering::Layout normalLayout = layout;
		const bool dragged = m_TagBeingDragged == callsign;
		const bool detailed = VsmrTagRendering::SelectHoveredLayout(
			tagFonts, sceneTarget.tag, options, mouseLocation, hoverAllowed, dragged,
			previouslyDetailed.count(callsign) != 0, normalLayout, layout);
		if (detailed) m_DetailedTagCallsigns.insert(callsign);
		const auto& selectedPalette = detailed ? sceneTarget.tag.detailedPalette : sceneTarget.tag.normalPalette;
		options.background = SceneColorToGdi(sceneTarget.rimcas.onRunway ? selectedPalette.backgroundOnRunway : selectedPalette.background);
		options.highlighted = dragged || (hoverAllowed &&
			VsmrTagRendering::CalculateBounds(tagFonts, layout, options).PtInRect(mouseLocation));
		preparedTags.push_back({ &sceneTarget, std::move(layout), options });
	}

	for (const PreparedTag& prepared : preparedTags)
	{
		const VsmrScene::Target& sceneTarget = *prepared.target;
		const VsmrTagRendering::PaintResult painted =
			VsmrTagRendering::Paint(*gdi, tagFonts, prepared.layout, prepared.options);
		if (painted.bounds.IsRectEmpty())
			continue;

		m_TagAreas[sceneTarget.callsign] = painted.bounds;
		const CRect clippedTag = clipToViewport(painted.bounds);
		if (!clippedTag.IsRectEmpty())
		{
			radar_screen->AddScreenObject(
				m_Id,
				sceneTarget.callsign.c_str(),
				clippedTag,
				true,
				sceneTarget.bottomLine.c_str());
		}
		for (const VsmrTagRendering::HitRegion& hit : painted.hitRegions)
		{
			const CRect clippedHit = clipToViewport(hit.area);
			if (!clippedHit.IsRectEmpty())
			{
				radar_screen->AddScreenObject(
					hit.action,
					sceneTarget.callsign.c_str(),
					clippedHit,
					true,
					sceneTarget.bottomLine.c_str());
			}
		}

		const CRimcas::RimcasAlertTypes stage =
			static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
		if (stage == CRimcas::StageOne || stage == CRimcas::StageTwo)
		{
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
		}
	}

}

void CInsetWindow::renderAvisoAircraft(HDC hDC, CDC& dc, CSMRRadar* radar_screen,
	Gdiplus::Graphics* gdi, POINT mouseLocation, const InsetAvisoCacheView& cacheView)
{
	const auto& viewportRect = cacheView.viewportRect;
	const auto& lonSpan = cacheView.lonSpan;
	const auto& latSpan = cacheView.latSpan;
	const auto& displayMinLon = cacheView.displayMinLon;
	const auto& displayMaxLon = cacheView.displayMaxLon;
	const auto& displayMinLat = cacheView.displayMinLat;
	const auto& displayMaxLat = cacheView.displayMaxLat;

	const int savedDc = ::SaveDC(hDC);
	if (savedDc == 0)
		return;

	::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
	Gdiplus::GraphicsState graphicsState = gdi->Save();
	gdi->SetClip(CopyRect(viewportRect), Gdiplus::CombineModeIntersect);
	m_TargetPoints.clear();
	m_TagAreas.clear();
	const auto previouslyDetailed = std::move(m_DetailedTagCallsigns);
	m_DetailedTagCallsigns.clear();
	const bool hoverAllowed = radar_screen->CanHoverTags(mouseLocation, this);

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
		const Gdiplus::PointF projected = cacheView.projectPoint(position.m_Longitude, position.m_Latitude);
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
	const double pixPerMeter = (std::max)(
		0.0,
		static_cast<double>((std::max)(1, m_AvisoScale)) / kAvisoMetersPerNm);

	VsmrTargetRendering::FrameSettings targetSettings;
	targetSettings.presentation = targetPresentation;
	targetSettings.pixelsPerMeter = pixPerMeter;
	const auto targetSettingsProjectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
	{
		CPosition position;
		position.m_Latitude = point.latitude;
		position.m_Longitude = point.longitude;
		return projectTargetPosition(position);
	};
	const auto targetSettingsPointVisible = [&](const POINT& point, int margin) -> bool
	{
		return pointInViewport(point, margin);
	};
	targetSettings.iconCache = radar_screen->CreateTargetIconCacheCallbacks();
	VsmrTargetRendering::Frame targetRenderer(*gdi, std::move(targetSettings));
	VsmrTargetRendering::DrawOptions targetDrawOptions;
	const double avisoSymbolScale = std::isfinite(targetPresentation.symbolScale)
		? std::clamp(targetPresentation.symbolScale, 0.25, 5.0)
		: 1.0;
	targetDrawOptions.minimumHitSize = static_cast<int>(
		std::ceil(18.0 * avisoSymbolScale));

	CPen symbolPen(PS_SOLID, 1, RGB(255, 255, 255));

	auto tagFontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
	Gdiplus::Font* tagRegularFont =
		tagFontIt != radar_screen->customFonts.end() ? tagFontIt->second.get() : nullptr;
	VsmrTagRendering::FontContext tagFonts(*gdi, tagRegularFont, 2, &m_TagTextCache);
	const bool roundedTagCornersEnabled = radar_screen->GetTagRoundedCornersEnabledForEditor();

	const VsmrScene::RadarScene* radarScene = radar_screen->GetCurrentRadarScene();
	std::vector<AvisoTagTarget> visibleTagTargets;
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
			targetRenderer.DrawTarget(sceneTarget, targetSettingsProjectPoint, targetSettingsPointVisible, targetDrawOptions);
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

		m_TargetPoints[rtCallsign] = targetPoint;
		if (sceneTarget.tagVisible && tagFonts.IsValid())
			visibleTagTargets.push_back({ &sceneTarget, targetPoint });
	}


	// Keep the symbol frame and font context alive until tag painting completes.
	const AvisoTagPass tagPass{ radar_screen, gdi, viewportRect, tagFonts, activeProfile,
		previouslyDetailed, mouseLocation, hoverAllowed, roundedTagCornersEnabled,
		rimcasStageOneColor, rimcasStageTwoColor };
	renderAvisoTags(visibleTagTargets, tagPass);

	gdi->Restore(graphicsState);
	::RestoreDC(hDC, savedDc);
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
			radar_screen->GetUiColorTheme() == "day",
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

	const int scale = (std::max)(1, m_AvisoScale);
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

	InsetAvisoCacheView cacheView{ *this, hDC, gdi, viewportRect, path, groupGeneration, displayMinLon, displayMaxLon, displayMinLat, displayMaxLat, lonSpan, latSpan, projectedTopLeft, projectedTopRight, projectedBottomLeft, projectedBottomRight };


	bool completedResultApplied = false;
	if (completedRenderResult != nullptr && cacheView.completedResultMatchesCurrentView(*completedRenderResult))
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

	// ----- Drawing or rebuilding the raster -----
	bool cacheDrawn = cacheView.drawCache();
	if (!cacheDrawn)
		cacheDrawn = cacheView.drawPreviousCacheViewportAligned();
	bool updateRequested = false;
	if (!cacheDrawn || !cacheView.cacheHasWorkingMargin())
	{
		// Keep the visible viewport at native resolution within the inset budget.
		const double overscanRatio = VsmrAviso::NativeResolutionOverscan(
			projectedTopLeft, projectedTopRight, projectedBottomLeft, projectedBottomRight, 18000000.0);
		const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
		const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
		const double renderMinLat = displayMinLat - (latSpan * overscanRatio);
		const double renderMaxLat = displayMaxLat + (latSpan * overscanRatio);
		const Gdiplus::PointF renderTopLeft = cacheView.projectPoint(renderMinLon, renderMaxLat);
		const Gdiplus::PointF renderTopRight = cacheView.projectPoint(renderMaxLon, renderMaxLat);
		const Gdiplus::PointF renderBottomLeft = cacheView.projectPoint(renderMinLon, renderMinLat);
		const Gdiplus::PointF renderBottomRight = cacheView.projectPoint(renderMaxLon, renderMinLat);
		const double renderScreenLeft = (std::min)((std::min)(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), (std::min)(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double renderScreenTop = (std::min)((std::min)(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), (std::min)(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double renderScreenRight = (std::max)((std::max)(static_cast<double>(renderTopLeft.X), static_cast<double>(renderTopRight.X)), (std::max)(static_cast<double>(renderBottomLeft.X), static_cast<double>(renderBottomRight.X)));
		const double renderScreenBottom = (std::max)((std::max)(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderTopRight.Y)), (std::max)(static_cast<double>(renderBottomLeft.Y), static_cast<double>(renderBottomRight.Y)));
		const double renderPixelWidth = renderScreenRight - renderScreenLeft;
		const double renderPixelHeight = renderScreenBottom - renderScreenTop;
		if (renderPixelWidth > 0.0 && renderPixelHeight > 0.0)
		{
			const double targetRasterScale = 1.0;
			const double maxRasterSide = 6400.0;
			const double maxRasterPixels = 18000000.0;
			double rasterScale = targetRasterScale;
			const double maxDimension = (std::max)(renderPixelWidth, renderPixelHeight);
			const double sideLimitedScale = maxRasterSide / maxDimension;
			if (sideLimitedScale > 0.0 && sideLimitedScale < rasterScale)
				rasterScale = sideLimitedScale;
			const double pixelLimitedScale = std::sqrt(maxRasterPixels / (renderPixelWidth * renderPixelHeight));
			if (pixelLimitedScale > 0.0 && pixelLimitedScale < rasterScale)
				rasterScale = pixelLimitedScale;
			// Keep the allocation caps hard even on unusually large desktops.
			rasterScale = (std::min)(rasterScale, targetRasterScale);

			CSMRRadar::AvisoRasterRenderRequest request;
			request.groupGeneration = groupGeneration;
			request.path = path;
			request.features = featureSnapshot;
			request.labels = labelSnapshot;
			request.groupVisibility = groupVisibility;
			request.colorPalette = radar_screen->AvisoColorPalette;
			request.rasterWidth = (std::max)(1, static_cast<int>(std::floor(renderPixelWidth * rasterScale)));
			request.rasterHeight = (std::max)(1, static_cast<int>(std::floor(renderPixelHeight * rasterScale)));
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


	if (cacheDrawn)
		renderAvisoAircraft(hDC, dc, radar_screen, gdi, mouseLocation, cacheView);

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
			const Gdiplus::PointF projected = cacheView.projectPoint(
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
