#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
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
	bool IsAvisoGroupedItemVisible(
		const std::vector<std::string>& groupIds,
		const std::unordered_map<std::string, bool>* visibilityById)
	{
		if (groupIds.empty() || visibilityById == nullptr)
			return true;

		bool foundKnownGroup = false;
		for (const std::string& groupId : groupIds)
		{
			const auto found = visibilityById->find(groupId);
			if (found == visibilityById->end())
			{
				// Legacy/foreign group references remain visible until a
				// definition explicitly controls them.
				return true;
			}

			foundKnownGroup = true;
			if (found->second)
				return true;
		}

		// Multiple membership uses union semantics: an item is visible when
		// at least one of its configured groups is visible.
		return !foundKnownGroup;
	}

	bool AvisoWithinTolerance(double left, double right, double tolerance)
	{
		const double delta = left - right;
		return delta >= -tolerance && delta <= tolerance;
	}

	class ScopedHBitmap
	{
	public:
		~ScopedHBitmap()
		{
			Reset();
		}

		void Reset(HBITMAP bitmap = nullptr)
		{
			if (bitmap_ != nullptr)
				::DeleteObject(bitmap_);
			bitmap_ = bitmap;
		}

		HBITMAP Get() const
		{
			return bitmap_;
		}

		HBITMAP Release()
		{
			HBITMAP bitmap = bitmap_;
			bitmap_ = nullptr;
			return bitmap;
		}

	private:
		HBITMAP bitmap_ = nullptr;
	};

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
	if (request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0 ||
		request.rasterWidth > 6400 ||
		request.rasterHeight > 6400 ||
		static_cast<std::uint64_t>(request.rasterWidth) *
			static_cast<std::uint64_t>(request.rasterHeight) > 32000000ULL)
	{
		return nullptr;
	}

	auto renderCancelled = [&]() -> bool
	{
		return IsAvisoRasterRenderRequestCancelled(request);
	};
	if (renderCancelled())
		return nullptr;

	BITMAPINFO bitmapInfo = {};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = request.rasterWidth;
	bitmapInfo.bmiHeader.biHeight = -request.rasterHeight;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;

	void* dibBits = nullptr;
	ScopedHBitmap dibBitmap;
	dibBitmap.Reset(::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &dibBits, nullptr, 0));
	if (dibBitmap.Get() == nullptr || dibBits == nullptr)
		return nullptr;

	const int rasterStride = request.rasterWidth * 4;
	auto raster = std::make_unique<Bitmap>(
		request.rasterWidth,
		request.rasterHeight,
		rasterStride,
		PixelFormat32bppPARGB,
		static_cast<BYTE*>(dibBits));
	if (raster == nullptr || raster->GetLastStatus() != Ok)
		return nullptr;

	Graphics rasterGraphics(raster.get());
	if (rasterGraphics.GetLastStatus() != Ok)
		return nullptr;

	rasterGraphics.SetPageUnit(UnitPixel);
	rasterGraphics.Clear(Color(0, 0, 0, 0));
	rasterGraphics.SetSmoothingMode(SmoothingModeAntiAlias);
	rasterGraphics.SetPixelOffsetMode(PixelOffsetModeHalf);
	rasterGraphics.SetCompositingQuality(CompositingQualityHighSpeed);

	const double displayMinLon = request.displayMinLongitude;
	const double displayMaxLon = request.displayMaxLongitude;
	const double displayMinLat = request.displayMinLatitude;
	const double displayMaxLat = request.displayMaxLatitude;
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
		return nullptr;
	if (renderCancelled())
		return nullptr;
	const double centerLatitudeRadians = ((displayMinLat + displayMaxLat) * 0.5) * 3.14159265358979323846 / 180.0;
	const double metersPerPixelLon = (lonSpan * 111320.0 * std::cos(centerLatitudeRadians)) /
		AvisoMax(request.scaleX * lonSpan, 1.0);
	const double metersPerPixelLat = (latSpan * 110540.0) /
		AvisoMax(request.scaleY * latSpan, 1.0);
	const double metersPerPixel = AvisoMax(metersPerPixelLon, metersPerPixelLat);

	auto projectScreenPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(request.projectedTopLeft.X) + static_cast<double>(request.projectedTopRight.X - request.projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(request.projectedBottomLeft.X) + static_cast<double>(request.projectedBottomRight.X - request.projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(request.projectedTopLeft.Y) + static_cast<double>(request.projectedTopRight.Y - request.projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(request.projectedBottomLeft.Y) + static_cast<double>(request.projectedBottomRight.Y - request.projectedBottomLeft.Y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};

	const PointF rasterRenderTopLeft = projectScreenPoint(request.renderMinLongitude, request.renderMaxLatitude);
	const PointF rasterRenderTopRight = projectScreenPoint(request.renderMaxLongitude, request.renderMaxLatitude);
	const PointF rasterRenderBottomLeft = projectScreenPoint(request.renderMinLongitude, request.renderMinLatitude);
	const PointF rasterRenderBottomRight = projectScreenPoint(request.renderMaxLongitude, request.renderMinLatitude);
	const double rasterRenderLeft = AvisoMin(AvisoMin(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMin(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderTop = AvisoMin(AvisoMin(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMin(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderRight = AvisoMax(AvisoMax(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMax(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderBottom = AvisoMax(AvisoMax(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMax(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderWidth = rasterRenderRight - rasterRenderLeft;
	const double rasterRenderHeight = rasterRenderBottom - rasterRenderTop;
	if (rasterRenderWidth <= 0.0 || rasterRenderHeight <= 0.0)
		return nullptr;
	const double rasterCoordinateScaleX = static_cast<double>(request.rasterWidth) / rasterRenderWidth;
	const double rasterCoordinateScaleY = static_cast<double>(request.rasterHeight) / rasterRenderHeight;

	auto projectRasterPoint = [&](const AvisoPoint& coordinate) -> PointF
	{
		const PointF screenPoint = projectScreenPoint(coordinate.longitude, coordinate.latitude);
		const double x = (static_cast<double>(screenPoint.X) - rasterRenderLeft) * rasterCoordinateScaleX;
		const double y = (static_cast<double>(screenPoint.Y) - rasterRenderTop) * rasterCoordinateScaleY;
		return PointF(static_cast<REAL>(x), static_cast<REAL>(y));
	};

	const double minRasterPointDistance = AvisoMax(0.35 * request.rasterScale, 0.5);
	const double minRasterPointDistanceSquared = minRasterPointDistance * minRasterPointDistance;
	auto appendRasterPoint = [&](std::vector<PointF>& points, AvisoPoint& lastCoordinate, bool& hasLastCoordinate, const AvisoPoint& coordinate, bool force)
	{
		if (!force && hasLastCoordinate)
		{
			const double approxDx = (coordinate.longitude - lastCoordinate.longitude) * request.scaleX * rasterCoordinateScaleX;
			const double approxDy = (coordinate.latitude - lastCoordinate.latitude) * request.scaleY * rasterCoordinateScaleY;
			if ((approxDx * approxDx + approxDy * approxDy) < minRasterPointDistanceSquared)
				return;
		}

		const PointF point = projectRasterPoint(coordinate);
		if (!force && !points.empty())
		{
			const PointF& lastPoint = points.back();
			const double dx = static_cast<double>(point.X - lastPoint.X);
			const double dy = static_cast<double>(point.Y - lastPoint.Y);
			if ((dx * dx + dy * dy) < minRasterPointDistanceSquared)
				return;
		}

		points.push_back(point);
		lastCoordinate = coordinate;
		hasLastCoordinate = true;
	};

	std::vector<PointF> rasterPoints;
	for (const AvisoFeature& feature : *request.features)
	{
		if (renderCancelled())
			return nullptr;
		if (!IsAvisoGroupedItemVisible(feature.groupIds, request.groupVisibility.get()))
			continue;
		if (feature.minimumZoomLevel > 0 && request.viewportZoomLevel < feature.minimumZoomLevel)
			continue;

		Color featureFillColor = request.colorPalette == "real"
			? feature.realFillColor
			: request.colorPalette == "light" ? feature.lightFillColor : feature.fillColor;
		Color featureStrokeColor = request.colorPalette == "real"
			? feature.realStrokeColor
			: request.colorPalette == "light" ? feature.lightStrokeColor : feature.strokeColor;

		if (feature.maxLatitude < request.renderMinLatitude ||
			feature.minLatitude > request.renderMaxLatitude ||
			feature.maxLongitude < request.renderMinLongitude ||
			feature.minLongitude > request.renderMaxLongitude)
		{
			continue;
		}

		const double featurePixelWidth = (feature.maxLongitude - feature.minLongitude) * request.scaleX * rasterCoordinateScaleX;
		const double featurePixelHeight = (feature.maxLatitude - feature.minLatitude) * request.scaleY * rasterCoordinateScaleY;
		if (featurePixelWidth < 0.5 && featurePixelHeight < 0.5)
			continue;
		if (feature.polygon)
		{
			for (const std::vector<AvisoPoint>& ring : feature.paths)
			{
				if (renderCancelled())
					return nullptr;
				if (ring.size() < 3)
					continue;

				rasterPoints.clear();
				rasterPoints.reserve(ring.size());
				AvisoPoint lastCoordinate{};
				bool hasLastCoordinate = false;
				for (size_t pointIndex = 0; pointIndex < ring.size(); ++pointIndex)
				{
					if ((pointIndex & 0xff) == 0 && renderCancelled())
						return nullptr;
					appendRasterPoint(rasterPoints, lastCoordinate, hasLastCoordinate, ring[pointIndex], pointIndex == 0);
				}

				if (rasterPoints.size() < 3)
					continue;
				if (renderCancelled())
					return nullptr;

				if (featureFillColor.GetAlpha() > 0)
				{
					SolidBrush fillBrush(featureFillColor);
					rasterGraphics.FillPolygon(&fillBrush, rasterPoints.data(), static_cast<INT>(rasterPoints.size()), FillModeAlternate);
				}
			}
			continue;
		}

		if (featureStrokeColor.GetAlpha() == 0 || feature.strokeWidth <= 0.0f)
			continue;

		Pen linePen(featureStrokeColor, feature.strokeWidth * static_cast<float>(request.rasterScale));
		linePen.SetLineJoin(LineJoinRound);
		linePen.SetStartCap(LineCapRound);
		linePen.SetEndCap(LineCapRound);
		for (const std::vector<AvisoPoint>& line : feature.paths)
		{
			if (renderCancelled())
				return nullptr;
			if (line.size() < 2)
				continue;

			rasterPoints.clear();
			rasterPoints.reserve(line.size());
			AvisoPoint lastCoordinate{};
			bool hasLastCoordinate = false;
			for (size_t pointIndex = 0; pointIndex < line.size(); ++pointIndex)
			{
				if ((pointIndex & 0xff) == 0 && renderCancelled())
					return nullptr;
				appendRasterPoint(rasterPoints, lastCoordinate, hasLastCoordinate, line[pointIndex], pointIndex == 0 || pointIndex + 1 == line.size());
			}

			if (rasterPoints.size() >= 2)
			{
				if (renderCancelled())
					return nullptr;
				rasterGraphics.DrawLines(&linePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
			}
		}
	}

	auto isDenseLabelVisible = [&](const AvisoLabel& label) -> bool
	{
		if (label.minimumZoomLevel > 0 && request.viewportZoomLevel < label.minimumZoomLevel)
			return false;
		if (label.maxMetersPerPixel > 0.0 && metersPerPixel > label.maxMetersPerPixel)
			return false;
		return true;
	};
	auto labelRectForAnchor = [](const PointF& point, REAL widthPx, REAL heightPx, const std::string& anchor) -> RectF
	{
		const std::string normalizedAnchor = ToUpperAscii(anchor);
		REAL x = point.X - (widthPx * 0.5f);
		REAL y = point.Y - (heightPx * 0.5f);
		if (normalizedAnchor.find("LEFT") != std::string::npos)
			x = point.X;
		else if (normalizedAnchor.find("RIGHT") != std::string::npos)
			x = point.X - widthPx;
		if (normalizedAnchor.find("TOP") != std::string::npos)
			y = point.Y;
		else if (normalizedAnchor.find("BOTTOM") != std::string::npos)
			y = point.Y - heightPx;
		return RectF(x, y, widthPx, heightPx);
	};

	if (!request.labels->empty())
	{
		rasterGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		FontFamily fallbackLabelFontFamily(L"Arial");
		StringFormat labelFormat;
		labelFormat.SetAlignment(StringAlignmentCenter);
		labelFormat.SetLineAlignment(StringAlignmentCenter);
		labelFormat.SetFormatFlags(StringFormatFlagsNoWrap);
		auto getLabelEmSize = [&](float textSize) -> REAL
		{
			const float scaledSize = static_cast<float>(std::clamp(static_cast<double>(textSize * static_cast<float>(request.rasterScale)), 6.0, 40.0));
			const int fontKey = static_cast<int>(std::lround(static_cast<double>(scaledSize) * 10.0));
			return static_cast<REAL>(fontKey) / 10.0f;
		};

		for (const AvisoLabel& label : *request.labels)
		{
			if (renderCancelled())
				return nullptr;
			if (!IsAvisoGroupedItemVisible(label.groupIds, request.groupVisibility.get()))
				continue;

			const std::wstring* renderedText = &label.text;

			if (label.position.latitude < request.renderMinLatitude ||
				label.position.latitude > request.renderMaxLatitude ||
				label.position.longitude < request.renderMinLongitude ||
				label.position.longitude > request.renderMaxLongitude ||
				renderedText->empty() ||
				!isDenseLabelVisible(label))
			{
				continue;
			}

			const REAL labelEmSize = getLabelEmSize(label.textSize);
			const PointF labelPoint = projectRasterPoint(label.position);
			const REAL textLength = static_cast<REAL>(renderedText->length());
			const REAL scaledTextSize = static_cast<REAL>(label.textSize * static_cast<float>(request.rasterScale));
			const REAL haloPadding = static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale), 0.0) * 3.0);
			const REAL layoutWidth = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * AvisoMax(static_cast<double>(textLength), 1.0) * 0.9f + haloPadding * 2.0f), 14.0));
			const REAL layoutHeight = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * 1.65f + haloPadding * 2.0f), 10.0));
			const RectF layoutRect = labelRectForAnchor(labelPoint, layoutWidth, layoutHeight, label.textAnchor);
			FontFamily labelFontFamily(label.fontFamily.empty() ? L"Arial" : label.fontFamily.c_str());
			const FontFamily* fontFamily = labelFontFamily.GetLastStatus() == Ok ? &labelFontFamily : &fallbackLabelFontFamily;

			GraphicsPath textPath;
			textPath.AddString(
				renderedText->c_str(),
				static_cast<INT>(renderedText->length()),
				fontFamily,
				FontStyleRegular,
				labelEmSize,
				layoutRect,
				&labelFormat);

			if (textPath.GetPointCount() <= 0)
				continue;

			const Color labelHaloColor = request.colorPalette == "real"
				? label.realHaloColor
				: request.colorPalette == "light" ? label.lightHaloColor : label.haloColor;
			const Color labelTextColor = request.colorPalette == "real"
				? label.realTextColor
				: request.colorPalette == "light" ? label.lightTextColor : label.textColor;
			if (label.haloWidth > 0.0f && labelHaloColor.GetAlpha() > 0)
			{
				Pen haloPen(labelHaloColor, static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * 2.0f), 1.0)));
				haloPen.SetLineJoin(LineJoinRound);
				rasterGraphics.DrawPath(&haloPen, &textPath);
			}

			if (labelTextColor.GetAlpha() > 0)
			{
				SolidBrush textBrush(labelTextColor);
				rasterGraphics.FillPath(&textBrush, &textPath);
			}
		}
	}

	if (renderCancelled())
		return nullptr;

	auto result = std::make_unique<AvisoRasterRenderResult>();
	result->requestId = request.requestId;
	result->groupGeneration = request.groupGeneration;
	result->colorPalette = request.colorPalette;
	result->bitmap = dibBitmap.Release();
	result->path = request.path;
	result->rasterWidth = request.rasterWidth;
	result->rasterHeight = request.rasterHeight;
	result->displayMinLongitude = request.displayMinLongitude;
	result->displayMinLatitude = request.displayMinLatitude;
	result->displayMaxLongitude = request.displayMaxLongitude;
	result->displayMaxLatitude = request.displayMaxLatitude;
	result->renderMinLongitude = request.renderMinLongitude;
	result->renderMinLatitude = request.renderMinLatitude;
	result->renderMaxLongitude = request.renderMaxLongitude;
	result->renderMaxLatitude = request.renderMaxLatitude;
	result->projectedTopLeft = request.projectedTopLeft;
	result->projectedTopRight = request.projectedTopRight;
	result->projectedBottomLeft = request.projectedBottomLeft;
	result->projectedBottomRight = request.projectedBottomRight;
	return result;
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
		if (inset->IsTimer())
			continue;
		CRect insetArea(inset->m_Area);
		insetArea.NormalizeRect();
		switch (inset->m_AvisoLayoutMode)
		{
		case CInsetWindow::AvisoLayoutMode::SplitLeft:
			availableLeft = max(availableLeft, insetArea.right);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitRight:
			availableRight = min(availableRight, insetArea.left);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitTop:
			availableTop = max(availableTop, insetArea.bottom);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitBottom:
			availableBottom = min(availableBottom, insetArea.top);
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
	if (!backgroundArea.IsRectEmpty())
		CDC::FromHandle(hDC)->FillSolidRect(backgroundArea, GetAvisoBackgroundColor());
	if (AvisoGeoJsonFeatures.empty() && AvisoGeoJsonLabels.empty())
		return;
	std::shared_ptr<const std::vector<AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> labelSnapshot;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	unsigned long long groupGeneration = 0;
	if (!GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		groupGeneration))
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

	const double projectedWidthTop = std::abs(static_cast<double>(projectedTopRight.X - projectedTopLeft.X));
	const double projectedWidthBottom = std::abs(static_cast<double>(projectedBottomRight.X - projectedBottomLeft.X));
	const double projectedHeightLeft = std::abs(static_cast<double>(projectedBottomLeft.Y - projectedTopLeft.Y));
	const double projectedHeightRight = std::abs(static_cast<double>(projectedBottomRight.Y - projectedTopRight.Y));
	const double projectedWidth = AvisoMax(AvisoMax(projectedWidthTop, projectedWidthBottom), 1.0);
	const double projectedHeight = AvisoMax(AvisoMax(projectedHeightLeft, projectedHeightRight), 1.0);
	const double scaleX = projectedWidth > 1.0 ? projectedWidth / lonSpan : fallbackScaleX;
	const double scaleY = projectedHeight > 1.0 ? projectedHeight / latSpan : fallbackScaleY;

	auto projectScreenPoint = [&](double longitude, double latitude) -> PointF
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
	};

	const double viewPixelTolerance = 1.15;
	const double lonPixelTolerance = (1.0 / scaleX) * viewPixelTolerance;
	const double latPixelTolerance = (1.0 / scaleY) * viewPixelTolerance;
	const double transformPixelTolerance = 4.0;

	auto rasterCacheTransformMatchesCurrentView = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr || !AvisoGeoJsonRasterAnchorValid)
			return false;
		const double cachedLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedLonSpan <= 0.0 || cachedLatSpan <= 0.0 || lonSpan <= 0.0 || latSpan <= 0.0)
			return false;

		const double cachedHorizontalX = AvisoGeoJsonRasterProjectedTopRight.X - AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedHorizontalY = AvisoGeoJsonRasterProjectedTopRight.Y - AvisoGeoJsonRasterProjectedTopLeft.Y;
		const double cachedVerticalX = AvisoGeoJsonRasterProjectedBottomLeft.X - AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedVerticalY = AvisoGeoJsonRasterProjectedBottomLeft.Y - AvisoGeoJsonRasterProjectedTopLeft.Y;
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
	};

	auto cacheMatchesCurrentView = [&]() -> bool
	{
		return AvisoGeoJsonRasterCache != nullptr &&
			AvisoGeoJsonRasterCachePath == path &&
			AvisoGeoJsonRasterGroupGeneration == groupGeneration &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMinLongitude, displayMinLon, lonPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMinLatitude, displayMinLat, latPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMaxLongitude, displayMaxLon, lonPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMaxLatitude, displayMaxLat, latPixelTolerance) &&
			rasterCacheTransformMatchesCurrentView();
	};

	auto drawRasterCacheTransformed = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr || AvisoGeoJsonRasterWidth <= 0 || AvisoGeoJsonRasterHeight <= 0)
			return false;
		if (AvisoGeoJsonRasterCachePath != path)
			return false;
		if (AvisoGeoJsonRasterGroupGeneration != groupGeneration)
			return false;
		if (!AvisoGeoJsonRasterAnchorValid)
			return false;
		if (!rasterCacheTransformMatchesCurrentView())
			return false;

		const double cachedViewportLonSpan = std::abs(AvisoGeoJsonRasterBottomRightLongitude - AvisoGeoJsonRasterAnchorLongitude);
		const double cachedViewportLatSpan = std::abs(AvisoGeoJsonRasterAnchorLatitude - AvisoGeoJsonRasterBottomRightLatitude);
		if (cachedViewportLonSpan <= 0.0 || cachedViewportLatSpan <= 0.0)
			return false;

		const PointF destTopLeft = projectScreenPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF destTopRight = projectScreenPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF destBottomLeft = projectScreenPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const PointF destBottomRight = projectScreenPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterBottomRightLatitude);
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

		const double sourceScaleX = static_cast<double>(AvisoGeoJsonRasterWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(AvisoGeoJsonRasterHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, AvisoGeoJsonRasterHeight);
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
		if (AvisoRasterBlitterInstance == nullptr)
			AvisoRasterBlitterInstance = std::make_unique<VsmrAviso::AvisoRasterBlitter>();
		return AvisoRasterBlitterInstance->Blend(
			graphics,
			hDC,
			AvisoGeoJsonRasterCache,
			sourceRect,
			destinationRect,
			radarArea);
	};

	auto drawRasterCacheViewportAligned = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr ||
			AvisoGeoJsonRasterCachePath != path ||
			AvisoGeoJsonRasterColorPalette != AvisoColorPalette ||
			AvisoGeoJsonRasterWidth <= 0 ||
			AvisoGeoJsonRasterHeight <= 0 ||
			!AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		auto projectCachedPoint = [&](double longitude, double latitude) -> PointF
		{
			const double u = (longitude - AvisoGeoJsonRasterMinLongitude) / cachedDisplayLonSpan;
			const double v = (AvisoGeoJsonRasterMaxLatitude - latitude) / cachedDisplayLatSpan;
			const double topX = static_cast<double>(AvisoGeoJsonRasterProjectedTopLeft.X) + static_cast<double>(AvisoGeoJsonRasterProjectedTopRight.X - AvisoGeoJsonRasterProjectedTopLeft.X) * u;
			const double bottomX = static_cast<double>(AvisoGeoJsonRasterProjectedBottomLeft.X) + static_cast<double>(AvisoGeoJsonRasterProjectedBottomRight.X - AvisoGeoJsonRasterProjectedBottomLeft.X) * u;
			const double topY = static_cast<double>(AvisoGeoJsonRasterProjectedTopLeft.Y) + static_cast<double>(AvisoGeoJsonRasterProjectedTopRight.Y - AvisoGeoJsonRasterProjectedTopLeft.Y) * u;
			const double bottomY = static_cast<double>(AvisoGeoJsonRasterProjectedBottomLeft.Y) + static_cast<double>(AvisoGeoJsonRasterProjectedBottomRight.Y - AvisoGeoJsonRasterProjectedBottomLeft.Y) * u;
			return PointF(
				static_cast<REAL>(topX + (bottomX - topX) * v),
				static_cast<REAL>(topY + (bottomY - topY) * v));
		};

		const PointF cachedRenderTopLeft = projectCachedPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderTopRight = projectCachedPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderBottomLeft = projectCachedPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const PointF cachedRenderBottomRight = projectCachedPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterBottomRightLatitude);
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

		const double sourceScaleX = static_cast<double>(AvisoGeoJsonRasterWidth) / cachedRenderWidth;
		const double sourceScaleY = static_cast<double>(AvisoGeoJsonRasterHeight) / cachedRenderHeight;
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
			sourceRight > static_cast<double>(AvisoGeoJsonRasterWidth) + coverageTolerance ||
			sourceBottom > static_cast<double>(AvisoGeoJsonRasterHeight) + coverageTolerance)
		{
			return false;
		}

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceRight));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceBottom));
		sourceXInt = std::clamp(sourceXInt, 0, AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, AvisoGeoJsonRasterHeight);
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
		if (AvisoRasterBlitterInstance == nullptr)
			AvisoRasterBlitterInstance = std::make_unique<VsmrAviso::AvisoRasterBlitter>();
		return AvisoRasterBlitterInstance->Blend(
			graphics,
			hDC,
			AvisoGeoJsonRasterCache,
			sourceRect,
			destinationRect,
			radarArea);
	};

	auto rasterCacheHasCompatibleZoom = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr ||
			AvisoGeoJsonRasterCachePath != path ||
			AvisoGeoJsonRasterGroupGeneration != groupGeneration ||
			!AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		return
			lonScaleRatio >= 0.985 && lonScaleRatio <= 1.015 &&
			latScaleRatio >= 0.985 && latScaleRatio <= 1.015;
	};

	auto rasterCacheHasWorkingMargin = [&]() -> bool
	{
		if (!rasterCacheHasCompatibleZoom())
			return false;

		const double cachedRenderMinLon = AvisoMin(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMaxLon = AvisoMax(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMinLat = AvisoMin(AvisoGeoJsonRasterAnchorLatitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double cachedRenderMaxLat = AvisoMax(AvisoGeoJsonRasterAnchorLatitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double requiredLonMargin = lonSpan * 0.25;
		const double requiredLatMargin = latSpan * 0.25;
		return
			cachedRenderMinLon <= displayMinLon - requiredLonMargin &&
			cachedRenderMaxLon >= displayMaxLon + requiredLonMargin &&
			cachedRenderMinLat <= displayMinLat - requiredLatMargin &&
			cachedRenderMaxLat >= displayMaxLat + requiredLatMargin;
	};

	ApplyCompletedAvisoGeoJsonRaster();
	if (cacheMatchesCurrentView() && drawRasterCacheTransformed())
	{
		PerformanceDiagnostics.RecordAvisoCacheOutcome(
			VsmrPerformance::AvisoViewport::Main,
			VsmrPerformance::AvisoCacheOutcome::Exact,
			false,
			false);
		return;
	}
	auto avisoRasterUpdatePending = [&]() -> bool
	{
		return AvisoGeoJsonRenderPipeline != nullptr &&
			AvisoGeoJsonRenderPipeline->HasPendingWork();
	};

	// Half a viewport of overscan still doubles each raster dimension and
	// comfortably exceeds the 25% refresh margin, while bounding allocation.
	const double overscanRatio = 0.50;
	const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
	const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
	const double renderMinLat = displayMinLat - (latSpan * overscanRatio);
	const double renderMaxLat = displayMaxLat + (latSpan * overscanRatio);
	const PointF renderTopLeft = projectScreenPoint(renderMinLon, renderMaxLat);
	const PointF renderTopRight = projectScreenPoint(renderMaxLon, renderMaxLat);
	const PointF renderBottomLeft = projectScreenPoint(renderMinLon, renderMinLat);
	const PointF renderBottomRight = projectScreenPoint(renderMaxLon, renderMinLat);
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
	request.groupVisibility = groupVisibility;
	request.colorPalette = AvisoColorPalette;
	request.rasterWidth = rasterWidth;
	request.rasterHeight = rasterHeight;
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
	request.viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(
		SMRGeometry::DistanceMeters(
			makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon),
			makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon)));
	request.projectedTopLeft = projectedTopLeft;
	request.projectedTopRight = projectedTopRight;
	request.projectedBottomLeft = projectedBottomLeft;
	request.projectedBottomRight = projectedBottomRight;

	// A divider resize changes the visible geographic span, but not the map's
	// pixel scale. Prefer the geo-anchored cache whenever its transform matches;
	// the compatibility/margin check still decides whether to refresh it.
	if (drawRasterCacheTransformed())
	{
		bool updateRequested = false;
		if (!rasterCacheHasWorkingMargin())
		{
			updateRequested = true;
			QueueAvisoGeoJsonRasterRender(std::move(request));
		}
		const bool delayedByAvisoUpdate = updateRequested && avisoRasterUpdatePending();
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
	const bool fallbackCacheDrawn = drawRasterCacheViewportAligned();
	const bool delayedByAvisoUpdate = avisoRasterUpdatePending();
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
