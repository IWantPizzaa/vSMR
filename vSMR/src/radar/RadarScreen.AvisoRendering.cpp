#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterGeometry.hpp"
#include "aviso/AvisoViewportRenderer.hpp"
#include "aviso/AvisoProjection.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.AvisoRuntimeState.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"
#include "insets/InsetWindow.hpp"

#include <cmath>
#include <unordered_map>

using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::AvisoMin;

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
	AvisoNativeViewportRenderer.reset();
	AvisoRefreshHostWindow.store(nullptr, std::memory_order_release);

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

void CSMRRadar::ClearAvisoGeoJsonRasterCache()
{
	if (AvisoNativeViewportRenderer != nullptr)
		AvisoNativeViewportRenderer->Clear();
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

void CSMRRadar::MarkPerformanceRefreshReason(
	VsmrPerformance::FrameRefreshReason reason) noexcept
{
	const std::uint32_t reasonMask = VsmrPerformance::RefreshReasonMask(reason);
	if (reasonMask != 0)
		PendingPerformanceRefreshReasonMask.fetch_or(reasonMask, std::memory_order_relaxed);
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

	CRect fullRadarArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	fullRadarArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		fullRadarArea.bottom = chatArea.top;
	fullRadarArea.NormalizeRect();
	// EuroScopePlugIn.h returns CPosition's double latitude/longitude from a
	// known integer POINT. Recover the full host basis from that precise inverse,
	// then validate six additional positions before accepting an affine mapping.
	// A nonlinear/rounded inverse retains the established four-corner fallback.
	const VsmrAviso::GeoBounds fullBounds{fullDisplayMinLon,fullDisplayMinLat,fullDisplayMaxLon,fullDisplayMaxLat};
	VsmrAviso::BilinearProjection preciseProjection;
	const auto samplePixels = VsmrAviso::InverseProjectionSamplePoints(
		{double(fullRadarArea.left),double(fullRadarArea.top)}, fullRadarArea.Width(),fullRadarArea.Height());
	std::array<VsmrAviso::ProjectionSample,9> projectionSamples;
	for (std::size_t i = 0; i < projectionSamples.size(); ++i)
	{
		const POINT pixel{static_cast<LONG>(samplePixels[i].X),static_cast<LONG>(samplePixels[i].Y)};
		const CPosition geographic = ConvertCoordFromPixelToPosition(pixel);
		projectionSamples[i] = {samplePixels[i],{geographic.m_Longitude,geographic.m_Latitude}};
	}
	if (!VsmrAviso::TryRecoverAffineProjection(fullBounds,projectionSamples,preciseProjection))
	{
		const POINT topLeft = ConvertCoordFromPositionToPixel(makeDisplayPosition(fullDisplayMaxLat,fullDisplayMinLon));
		const POINT topRight = ConvertCoordFromPositionToPixel(makeDisplayPosition(fullDisplayMaxLat,fullDisplayMaxLon));
		const POINT bottomLeft = ConvertCoordFromPositionToPixel(makeDisplayPosition(fullDisplayMinLat,fullDisplayMinLon));
		const POINT bottomRight = ConvertCoordFromPositionToPixel(makeDisplayPosition(fullDisplayMinLat,fullDisplayMaxLon));
		preciseProjection = VsmrAviso::BilinearProjection::FromCorners(fullBounds,
			VsmrAviso::PointD{double(topLeft.x),double(topLeft.y)},VsmrAviso::PointD{double(topRight.x),double(topRight.y)},
			VsmrAviso::PointD{double(bottomLeft.x),double(bottomLeft.y)},VsmrAviso::PointD{double(bottomRight.x),double(bottomRight.y)});
	}
	auto projectFullDisplayPoint = [&](double longitude, double latitude) -> PointF
	{
		const auto projected = preciseProjection.Project(longitude, latitude);
		return { static_cast<REAL>(projected.X), static_cast<REAL>(projected.Y) };
	};
	double displayMinLat = fullDisplayMinLat;
	double displayMaxLat = fullDisplayMaxLat;
	double displayMinLon = fullDisplayMinLon;
	double displayMaxLon = fullDisplayMaxLon;

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
			VsmrAviso::GeoPoint position;
			if (!preciseProjection.TryUnproject({ double(corner.x), double(corner.y) }, position))
			{
				visibleBoundsValid = false;
				break;
			}
			visibleMinLat = AvisoMin(visibleMinLat, position.latitude);
			visibleMaxLat = AvisoMax(visibleMaxLat, position.latitude);
			visibleMinLon = AvisoMin(visibleMinLon, position.longitude);
			visibleMaxLon = AvisoMax(visibleMaxLon, position.longitude);
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

	// Render exactly the visible native pixels. The projection is geographic
	// and independent of a bitmap's bounds, crop rounding or completion time.
	const auto& projection = preciseProjection;
	double renderMinLon = displayMinLon, renderMaxLon = displayMaxLon;
	double renderMinLat = displayMinLat, renderMaxLat = displayMaxLat;
	for (const auto& corner : { VsmrAviso::PointD{ double(radarArea.left), double(radarArea.top) },
		VsmrAviso::PointD{ double(radarArea.right), double(radarArea.top) },
		VsmrAviso::PointD{ double(radarArea.left), double(radarArea.bottom) },
		VsmrAviso::PointD{ double(radarArea.right), double(radarArea.bottom) } })
	{
		VsmrAviso::GeoPoint geographic;
		if (projection.TryUnproject(corner, geographic))
		{
			renderMinLon = AvisoMin(renderMinLon, geographic.longitude);
			renderMaxLon = AvisoMax(renderMaxLon, geographic.longitude);
			renderMinLat = AvisoMin(renderMinLat, geographic.latitude);
			renderMaxLat = AvisoMax(renderMaxLat, geographic.latitude);
		}
	}
	// These margins select offscreen labels/strokes, not extra raster pixels.
	const double longitudeMargin = (renderMaxLon - renderMinLon) * 0.5;
	const double latitudeMargin = (renderMaxLat - renderMinLat) * 0.5;
	AvisoRasterRenderRequest request;
	request.groupGeneration = groupGeneration;
	request.path = path;
	request.features = featureSnapshot;
	request.labels = labelSnapshot;
	request.scene = renderScene;
	request.preciseProjection = preciseProjection;
	request.groupVisibility = groupVisibility;
	request.colorPalette = AvisoColorPalette;
	request.backgroundColor = GetAvisoRasterBackgroundColor();
	request.rasterWidth = radarArea.Width();
	request.rasterHeight = radarArea.Height();
	request.rasterScale = 1.0;
	request.displayScale = GetDisplayScale();
	request.displayMinLongitude = displayMinLon;
	request.displayMinLatitude = displayMinLat;
	request.displayMaxLongitude = displayMaxLon;
	request.displayMaxLatitude = displayMaxLat;
	request.renderMinLongitude = renderMinLon - longitudeMargin;
	request.renderMinLatitude = renderMinLat - latitudeMargin;
	request.renderMaxLongitude = renderMaxLon + longitudeMargin;
	request.renderMaxLatitude = renderMaxLat + latitudeMargin;
	request.scaleX = scaleX;
	request.scaleY = scaleY;
	request.viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(
		SMRGeometry::DistanceMeters(makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon),
			makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon)));
	request.projectedTopLeft = projectedTopLeft;
	request.projectedTopRight = projectedTopRight;
	request.projectedBottomLeft = projectedBottomLeft;
	request.projectedBottomRight = projectedBottomRight;
	if (AvisoNativeViewportRenderer == nullptr)
		AvisoNativeViewportRenderer = std::make_unique<VsmrAviso::AvisoViewportRenderer>();
	const auto outcome = AvisoNativeViewportRenderer->Draw(graphics, hDC, radarArea, request, [&]() {
		DrawAvisoBackground(graphics, radarArea);
		backgroundSatisfied = true;
	});
	backgroundSatisfied = backgroundSatisfied || outcome.drawn;
	if (outcome.buildAttempted)
	{
		PerformanceDiagnostics.RecordAvisoRasterBuild(VsmrPerformance::AvisoViewport::Main,
			outcome.buildMilliseconds, 0.0, outcome.drawn);
		if (outcome.drawn)
			PerformanceDiagnostics.RecordAvisoResultApplied(VsmrPerformance::AvisoViewport::Main);
	}
	PerformanceDiagnostics.RecordAvisoCacheOutcome(VsmrPerformance::AvisoViewport::Main,
		outcome.cacheHit && outcome.drawn ? VsmrPerformance::AvisoCacheOutcome::Exact : VsmrPerformance::AvisoCacheOutcome::Miss,
		false, !outcome.drawn);
}
