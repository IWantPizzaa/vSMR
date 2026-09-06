#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "insets/InsetWindow.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"

extern CPoint mouseLocation;
extern std::string TagBeingDragged;
extern int LeaderLineDefaultlenght;
extern bool initCursor;
extern HCURSOR smrCursor;
extern bool customCursor;

void EnsureAvisoWheelHooks(CSMRRadar* radarScreen);
void EnsureInsetWindowProcHook(HWND hwnd, CSMRRadar* radarScreen);

namespace
{
    double AvisoMax(double left, double right)
    {
        return left > right ? left : right;
    }

    double RefreshPerfNowMs()
    {
        static LARGE_INTEGER frequency = {};
        if (frequency.QuadPart == 0)
            ::QueryPerformanceFrequency(&frequency);

        LARGE_INTEGER counter = {};
        ::QueryPerformanceCounter(&counter);
        return (static_cast<double>(counter.QuadPart) * 1000.0) /
            static_cast<double>(frequency.QuadPart);
    }

    void OutputVsmrDebugLine(const std::string& message)
    {
        const std::string line = "[vSMR] " + message + "\n";
        ::OutputDebugStringA(line.c_str());
        Logger::info(message);
    }

    class ScopedCdcDetach
    {
    public:
        explicit ScopedCdcDetach(CDC& dc) : dc_(dc) {}
        ~ScopedCdcDetach() { Detach(); }

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

    class ScopedCdcTextColor
    {
    public:
        ScopedCdcTextColor(CDC& dc, COLORREF color) :
            Dc(dc),
            PreviousColor(dc.SetTextColor(color))
        {
        }

        ~ScopedCdcTextColor()
        {
            if (PreviousColor != CLR_INVALID && Dc.GetSafeHdc() != nullptr)
                Dc.SetTextColor(PreviousColor);
        }

    private:
        CDC& Dc;
        COLORREF PreviousColor = CLR_INVALID;
    };

    bool IsTagBeingDragged(const std::string& callsign)
    {
        return TagBeingDragged == callsign;
    }

    bool MouseWithin(const CRect& rect)
    {
        return VsmrRadarUiSupport::mouseWithin(mouseLocation, rect);
    }

	std::vector<std::string> SplitCommaList(const std::string& list)
	{
		std::vector<std::string> result;
		std::size_t start = 0;
		std::size_t end = list.find(',');
		while (end != std::string::npos)
		{
			result.push_back(list.substr(start, end - start));
			start = end + 1;
			end = list.find(',', start);
		}
		result.push_back(list.substr(start));
		return result;
	}
}

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

// Per-frame timings stay together so render stages can report their work
// without growing the EuroScope callback's local-variable surface.
struct CSMRRadar::RefreshPerformance
{
	double frameStartMilliseconds = 0.0;
	std::uint32_t refreshReasonMask = 0;
	double avisoMilliseconds = 0.0;
	double targetsMilliseconds = 0.0;
	double rimcasMilliseconds = 0.0;
	double tagsMilliseconds = 0.0;
	double srwMilliseconds = 0.0;
	double avisoInsetMilliseconds = 0.0;
	double rdfMilliseconds = 0.0;
	double insetChromeMilliseconds = 0.0;
	std::size_t visibleTargetCount = 0;
};

void CSMRRadar::RefreshSectorMap(
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	setRefreshStage("sector geometry cache");
	// Sector element enumeration is stateful in the EuroScope SDK. Always bind
	// it to this radar screen before reading airport geometry or ARR/DEP runway
	// activity; another callback may have selected a different sector source.
	CPlugIn* sectorPlugin = GetPlugIn();
	struct ActiveSectorSelectionGuard
	{
		CPlugIn* plugin = nullptr;
		~ActiveSectorSelectionGuard()
		{
			if (plugin != nullptr)
				plugin->SelectActiveSectorfile();
		}
	} sectorSelectionGuard{ sectorPlugin };
	sectorPlugin->SelectScreenSectorfile(this);
	EnsureAirportPositionCache();
	EnsureRunwayGeometryCache();
	RefreshRunwayStatuses(false);

	// Draw map elements based on zoom level
	CPosition radarDownLeft;
	CPosition radarUpRight;
	GetDisplayArea(&radarDownLeft, &radarUpRight);
	if (std::isfinite(radarDownLeft.m_Longitude) &&
		std::isfinite(radarDownLeft.m_Latitude) &&
		std::isfinite(radarUpRight.m_Longitude) &&
		std::isfinite(radarUpRight.m_Latitude))
	{
		const double minLongitude = radarDownLeft.m_Longitude;
		const double minLatitude = radarDownLeft.m_Latitude;
		const double maxLongitude = radarUpRight.m_Longitude;
		const double maxLatitude = radarUpRight.m_Latitude;
		constexpr double viewChangeEpsilon = 1.0e-8;
		if (PerformanceLastMainViewValid &&
			(std::abs(minLongitude - PerformanceLastMainViewMinLongitude) > viewChangeEpsilon ||
			 std::abs(minLatitude - PerformanceLastMainViewMinLatitude) > viewChangeEpsilon ||
			 std::abs(maxLongitude - PerformanceLastMainViewMaxLongitude) > viewChangeEpsilon ||
			 std::abs(maxLatitude - PerformanceLastMainViewMaxLatitude) > viewChangeEpsilon))
		{
			performance.refreshReasonMask |= VsmrPerformance::RefreshReasonMask(
				VsmrPerformance::FrameRefreshReason::MainViewChanged);
		}
		PerformanceLastMainViewValid = true;
		PerformanceLastMainViewMinLongitude = minLongitude;
		PerformanceLastMainViewMinLatitude = minLatitude;
		PerformanceLastMainViewMaxLongitude = maxLongitude;
		PerformanceLastMainViewMaxLatitude = maxLatitude;
	}
	double radarCrossDistance = Haversine(radarDownLeft, radarUpRight);
	int NewRadarViewZoomLevel = getZoomLevelFromCrossDistance(radarCrossDistance);
	const std::string currentMapAirport = getActiveAirport();
	const bool useAvisoGroundRenderer = !ResolveAvisoGeoJsonPathForAirport(currentMapAirport).empty();
	const auto& currentRunwayStatuses = RimcasInstance->GetRunwayStatuses();
	const bool needsMapRefresh =
		(NewRadarViewZoomLevel != RadarViewZoomLevel) ||
		(currentMapAirport != LastMapActiveAirport) ||
		(currentRunwayStatuses != LastMapRunwayStatuses);

	if (needsMapRefresh) {
		setRefreshStage("map refresh");
		RadarViewZoomLevel = NewRadarViewZoomLevel;
		if (!useAvisoGroundRenderer)
		{
			// Draw items based on asr config & zoom level
			vector<CConfig::mapData> allItems = CurrentConfig->getMapElementsForZoomLevel(maxZoomLevel);
			vector<CConfig::mapData> itemsToDraw = CurrentConfig->getMapElementsForZoomLevel(RadarViewZoomLevel);
			map<string, bool> drawItemMap;

			auto tokenDataStart = [](const string& s, const string& token) -> size_t {
				// Find token like "DEP" or "ARR" and return the index right after the token and the following separator (eg. "DEP:")
				size_t pos = s.find(token);
				if (pos == string::npos) return string::npos;
				// token length +1 for separator (':') -> matches previous code's +4 for "DEP" (3) + ':'
				return pos + token.length() + 1;
				};

			for (const auto& item : allItems) {
				// Consider element present if any map entry has the same element name (compare element only).
				bool present = std::any_of(itemsToDraw.begin(), itemsToDraw.end(),
					[&](const CConfig::mapData& m) { return m.element == item.element; });

				bool shouldDraw = present;

				// If the item has an "active" definition we need to evaluate DEP/ARR conditions
				if (present && item.active.size() > 4) {
					if (item.active.substr(0, 4) != currentMapAirport) {
						shouldDraw = false;
					}

					// airport prefix (first 4 chars) must match active airport

					if (shouldDraw) {
						size_t depPos = tokenDataStart(item.active, "DEP");
						size_t arrPos = tokenDataStart(item.active, "ARR");
						// If DEP present, extract substring between DEP: and ARR: (or end) and check runways
						if (depPos != string::npos) {
							size_t depEnd = (arrPos != string::npos) ? arrPos - 5 : item.active.size();
							string depList = item.active.substr(depPos, depEnd - depPos);
							vector<string> depRunways = SplitCommaList(depList);
							for (const auto& rwy : depRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::DEP && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}

						// If ARR present, extract substring after ARR: and check runways
						if (arrPos != string::npos && shouldDraw) {
							string arrList = item.active.substr(arrPos);
							vector<string> arrRunways = SplitCommaList(arrList);
							for (const auto& rwy : arrRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::ARR && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}
					}
				}

				// Always set an entry for this element (avoids missing keys and an empty draw map).
				drawItemMap[item.element] = shouldDraw;
			}

			// Now apply the map
			setRefreshStage("sector element visibility");
			for (const auto& [elementName, toDraw] : drawItemMap) {
				size_t slashPos = elementName.find("/");
				if (slashPos == string::npos) continue;
				string category = elementName.substr(0, slashPos);
				string name = elementName.substr(slashPos + 1);

				int elementCategory = getIntFromCategory(category);
				if (elementCategory == -1) continue;
				CSectorElement element = GetPlugIn()->SectorFileElementSelectFirst(elementCategory);
				while (element.IsValid()) {
					const char* elementNameRaw = element.GetName();
					if (elementNameRaw == nullptr || elementNameRaw[0] == '\0')
					{
						element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
						continue;
					}

					if (strncmp(name.c_str(), elementNameRaw, strlen(name.c_str())) == 0) {
						const char* componentName = element.GetComponentName(0);
						if (componentName != nullptr)
							ShowSectorFileElement(element, componentName, toDraw);
					}
					element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
				}
			}

			setRefreshStage("RefreshMapContent");
			RefreshMapContent();
		}
		LastMapRunwayStatuses = currentRunwayStatuses;
		LastMapActiveAirport = currentMapAirport;
	}

}

void CSMRRadar::PrepareRefreshInsetLayout(CRect& insetLayoutBounds)
{
	if (InitialInsetStateRestorePending && !insetLayoutBounds.IsRectEmpty())
	{
		const unsigned long restoreNowTick = ::GetTickCount();
		const bool sameBounds =
			InitialInsetStateRestoreBounds.left == insetLayoutBounds.left &&
			InitialInsetStateRestoreBounds.top == insetLayoutBounds.top &&
			InitialInsetStateRestoreBounds.right == insetLayoutBounds.right &&
			InitialInsetStateRestoreBounds.bottom == insetLayoutBounds.bottom;
		if (sameBounds)
		{
			++InitialInsetStateRestoreStableFrames;
		}
		else
		{
			InitialInsetStateRestoreBounds = insetLayoutBounds;
			InitialInsetStateRestoreStableFrames = 1;
			InitialInsetStateRestoreBoundsChangedTick = restoreNowTick;
		}

		if (InitialInsetStateRestoreStableFrames >= 2 &&
			restoreNowTick - InitialInsetStateRestoreBoundsChangedTick >= 250)
		{
			ResetAllInsetWindowStates(false);
			ResetAvisoPresetStateForActiveAirport(false);
			if (!LoadInsetStateFromAsrForAirport(getActiveAirport(), true))
				ApplyDefaultAvisoPresetIfConfigured();
			else if (!ActiveAvisoPresetName.empty())
			{
				const std::string activePresetName = ActiveAvisoPresetName;
				LoadAvisoPreset(activePresetName);
			}
			InitialInsetStateRestorePending = false;
			InitialInsetStateRestoreStableFrames = 0;
			InitialInsetStateRestoreBoundsChangedTick = 0;
		}
	}
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr)
			appWindowIt->second->ApplyAvisoLayoutBounds(&insetLayoutBounds);
	}
}

std::shared_ptr<const VsmrScene::RadarScene> CSMRRadar::BuildRefreshSceneAndRenderAviso(
	HDC hDC,
	Gdiplus::Graphics& graphics,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	auto disableAvisoGeoJsonRender = [&](const std::string& reason)
	{
		std::string disabledPath = AvisoGeoJsonLoadedPath;
		if (disabledPath.empty())
			disabledPath = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());

		AvisoGeoJsonRenderDisabled = true;
		AvisoGeoJsonRenderDisabledPath = disabledPath;
		ClearAvisoGeoJsonRasterCache();
		AvisoGeoJsonLastViewValid = false;
		Logger::info("AVISO GeoJSON render disabled path=" + disabledPath + " reason=" + reason);
	};

	// Capture all EuroScope-derived frame data once, finalize RIMCAS, and then
	// publish an immutable scene before any viewport renders traffic.  Main,
	// AVISO, and SRW projections consume this same snapshot later in the frame.
	setRefreshStage("shared radar scene build");
	double sceneRimcasMilliseconds = 0.0;
	const std::shared_ptr<const VsmrScene::RadarScene> frameSceneOwner = BuildRadarScene(
		isLVP,
		&sceneRimcasMilliseconds);
	performance.rimcasMilliseconds += sceneRimcasMilliseconds;

	const double perfAvisoStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("AVISO render");
		RenderAvisoGeoJson(hDC, graphics);
	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		disableAvisoGeoJsonRender("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		disableAvisoGeoJsonRender("MFC exception");
	}
	catch (const std::exception& ex)
	{
		disableAvisoGeoJsonRender(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		disableAvisoGeoJsonRender("unknown exception");
	}
	performance.avisoMilliseconds += RefreshPerfNowMs() - perfAvisoStartMs;
	return frameSceneOwner;
}

void CSMRRadar::RenderClosedRunwayOverlays(
	Gdiplus::Graphics& graphics,
	const std::function<void(const char*)>& setRefreshStage)
{
	VSMR_REFRESH_LOG("Runway overlay loop");
	setRefreshStage("runway overlay draw");
	for (const auto& runway : CachedRunwayGeometries)
	{
		const auto closedRunwayIt = RimcasInstance->ClosedRunway.find(runway.displayName);
		if (closedRunwayIt == RimcasInstance->ClosedRunway.end() || !closedRunwayIt->second)
			continue;

		const std::vector<CPosition>& closedDefinition =
			runway.closedDefinition.empty() ? runway.rimcasDefinition : runway.closedDefinition;
		if (closedDefinition.size() < 3)
			continue;

		std::vector<PointF> points;
		points.reserve(closedDefinition.size());
		for (const auto& point : closedDefinition)
		{
			POINT toDraw = ConvertCoordFromPositionToPixel(point);
			points.push_back({ REAL(toDraw.x), REAL(toDraw.y) });
		}

		SolidBrush closedRunwayBrush(Color(150, 0, 0));
		graphics.FillPolygon(&closedRunwayBrush, points.data(), static_cast<INT>(points.size()));
	}
}

void CSMRRadar::RenderRefreshTargets(
	Gdiplus::Graphics& graphics,
	CDC& dc,
	const RECT& radarArea,
	const VsmrScene::RadarScene* frameScene,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	// Drawing the symbols
	VSMR_REFRESH_LOG("Symbols loop");
	setRefreshStage("target symbol rendering");

	// Cache current view scaling once per frame for configured icon sizing.
	double framePixPerMeter = 0.0;
	{
		CRect frameScaleArea(radarArea);
		frameScaleArea.NormalizeRect();
		double pxW = (frameScaleArea.Width() > 0)
			? double(frameScaleArea.Width())
			: 1.0;
		double pxH = (frameScaleArea.Height() > 0)
			? double(frameScaleArea.Height())
			: 1.0;

		CPosition dispSW, dispNE;
		GetDisplayArea(&dispSW, &dispNE);
		double centerLat = (dispSW.m_Latitude + dispNE.m_Latitude) / 2.0;
		double centerLon = (dispSW.m_Longitude + dispNE.m_Longitude) / 2.0;

		CPosition leftMid; leftMid.m_Latitude = centerLat; leftMid.m_Longitude = dispSW.m_Longitude;
		CPosition rightMid; rightMid.m_Latitude = centerLat; rightMid.m_Longitude = dispNE.m_Longitude;
		CPosition bottomMid; bottomMid.m_Latitude = dispSW.m_Latitude; bottomMid.m_Longitude = centerLon;
		CPosition topMid; topMid.m_Latitude = dispNE.m_Latitude; topMid.m_Longitude = centerLon;

		double widthMeters = Haversine(leftMid, rightMid);
		double heightMeters = Haversine(bottomMid, topMid);

		double pixPerMeterX = (widthMeters > 1.0) ? (pxW / widthMeters) : 0.0;
		double pixPerMeterY = (heightMeters > 1.0) ? (pxH / heightMeters) : 0.0;

		if (pixPerMeterX > 0.0 && pixPerMeterY > 0.0)
			framePixPerMeter = (pixPerMeterX < pixPerMeterY) ? pixPerMeterX : pixPerMeterY;
		else if (pixPerMeterX > 0.0)
			framePixPerMeter = pixPerMeterX;
		else
			framePixPerMeter = pixPerMeterY;
	}

	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& frameTargetPresentation = frameScene != nullptr
		? frameScene->targetPresentation
		: defaultTargetPresentation;
	setRefreshStage("radar target loop");
	const double perfRimcasBeforeTargetsMs = performance.rimcasMilliseconds;
	const double perfTargetsStartMs = RefreshPerfNowMs();
	std::size_t frameVisibleTargetCount = 0;
	targetAreas.clear();
	CRect frameVisibleRadarArea(radarArea);
	frameVisibleRadarArea.NormalizeRect();
	static const std::vector<VsmrScene::Target> emptySceneTargets;
	const std::vector<VsmrScene::Target>& sceneTargets = frameScene != nullptr
		? frameScene->targets
		: emptySceneTargets;
	auto scenePosition = [](const VsmrScene::GeoPoint& source) -> CPosition
	{
		CPosition position;
		position.m_Latitude = source.latitude;
		position.m_Longitude = source.longitude;
		return position;
	};
	{
		VsmrTargetRendering::FrameSettings targetRenderSettings;
		targetRenderSettings.presentation = frameTargetPresentation;
		targetRenderSettings.pixelsPerMeter = framePixPerMeter;
		targetRenderSettings.projectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
		{
			return ConvertCoordFromPositionToPixel(scenePosition(point));
		};
		targetRenderSettings.pointVisible = [&](const POINT& point, int margin) -> bool
		{
			return point.x >= frameVisibleRadarArea.left - margin &&
				point.x <= frameVisibleRadarArea.right + margin &&
				point.y >= frameVisibleRadarArea.top - margin &&
				point.y <= frameVisibleRadarArea.bottom + margin;
		};
		targetRenderSettings.iconCache = CreateTargetIconCacheCallbacks();

		VsmrTargetRendering::Frame targetRenderer(graphics, std::move(targetRenderSettings));
		for (const VsmrScene::Target& sceneTarget : sceneTargets)
		{
			if (!sceneTarget.iconVisible || !sceneTarget.position.valid)
				continue;
			const std::string& rtCallsign = sceneTarget.callsign;
			auto iconVerboseStep = [&](const std::string& step)
			{
				if (!Logger::is_verbose_mode())
					return;
				Logger::info("IconRender: " + rtCallsign + " " + step);
			};

			const CPosition targetPosition = scenePosition(sceneTarget.position);
			iconVerboseStep("begin");

			const bool AcisCorrelated = sceneTarget.correlated;
			POINT acPosPix = ConvertCoordFromPositionToPixel(targetPosition);
			if (acPosPix.x >= frameVisibleRadarArea.left && acPosPix.x < frameVisibleRadarArea.right &&
				acPosPix.y >= frameVisibleRadarArea.top && acPosPix.y < frameVisibleRadarArea.bottom)
			{
				++frameVisibleTargetCount;
			}

			iconVerboseStep("after_scene_data");
			const VsmrTargetRendering::DrawResult drawResult =
				targetRenderer.DrawTarget(sceneTarget);
			acPosPix = drawResult.center;
			if (Logger::is_verbose_mode())
			{
				const char* iconDrawMode = sceneTarget.style.icon == VsmrScene::IconStyle::Nova
					? "nova"
					: (drawResult.realisticBitmapDrawn ? "realistic" : "symbol");
				Logger::info(
					"IconRender: " + rtCallsign +
					" mode=" + iconDrawMode +
					" icon_type=" + sceneTarget.style.assetKey);
			}
			iconVerboseStep("after_icon_draw");
			if (MouseWithin({ acPosPix.x - 5, acPosPix.y - 5, acPosPix.x + 5, acPosPix.y + 5 })) {
				dc.MoveTo(acPosPix.x, acPosPix.y - 8);
				dc.LineTo(acPosPix.x - 6, acPosPix.y - 12);
				dc.MoveTo(acPosPix.x, acPosPix.y - 8);
				dc.LineTo(acPosPix.x + 6, acPosPix.y - 12);

				dc.MoveTo(acPosPix.x, acPosPix.y + 8);
				dc.LineTo(acPosPix.x - 6, acPosPix.y + 12);
				dc.MoveTo(acPosPix.x, acPosPix.y + 8);
				dc.LineTo(acPosPix.x + 6, acPosPix.y + 12);

				dc.MoveTo(acPosPix.x - 8, acPosPix.y );
				dc.LineTo(acPosPix.x - 12, acPosPix.y -6);
				dc.MoveTo(acPosPix.x - 8, acPosPix.y);
				dc.LineTo(acPosPix.x - 12 , acPosPix.y + 6);

				dc.MoveTo(acPosPix.x + 8, acPosPix.y);
				dc.LineTo(acPosPix.x + 12, acPosPix.y - 6);
				dc.MoveTo(acPosPix.x + 8, acPosPix.y);
				dc.LineTo(acPosPix.x + 12, acPosPix.y + 6);
			}

			std::string hoverTextStorage;
			const char* hoverText = "";
			if (AcisCorrelated)
			{
				hoverTextStorage = sceneTarget.bottomLine;
				hoverText = hoverTextStorage.c_str();
			}
			else
			{
				hoverText = sceneTarget.systemId.c_str();
			}
			iconVerboseStep("before_add_screen_object");
			const CRect targetArea(
				drawResult.hitBounds.left,
				drawResult.hitBounds.top,
				drawResult.hitBounds.right,
				drawResult.hitBounds.bottom);
			targetAreas[rtCallsign] = targetArea;
			AddScreenObject(DRAWING_AC_SYMBOL, rtCallsign.c_str(), targetArea, false, hoverText);
			iconVerboseStep("after_add_screen_object");
		}
		performance.targetsMilliseconds += AvisoMax(
			0.0,
			(RefreshPerfNowMs() - perfTargetsStartMs) -
			(performance.rimcasMilliseconds - perfRimcasBeforeTargetsMs));
	}

	performance.visibleTargetCount = frameVisibleTargetCount;
}

void CSMRRadar::RenderRefreshRimcasPanels(
	CDC& dc,
	const VsmrScene::RadarScene* frameScene,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	CBrush BrushGrey(RGB(150, 150, 150));
	ScopedCdcTextColor textColorGuard(dc, RGB(33, 33, 33));

	int TextHeight = dc.GetTextExtent("60").cy;
	VSMR_REFRESH_LOG("RIMCAS Loop");
	setRefreshStage("RIMCAS list rendering");
	const double perfRimcasListStartMs = RefreshPerfNowMs();
	const vector<int>& TimeDefinition = isLVP
		? RimcasInstance->CountdownDefinitionLVP
		: RimcasInstance->CountdownDefinition;
	Color rimcasStageOneColor(255, 160, 90, 30);
	Color rimcasStageTwoColor(255, 150, 0, 0);
	bool rimcasColorsResolved = false;
	auto resolveRimcasColors = [&]()
	{
		if (rimcasColorsResolved)
			return;
		rimcasColorsResolved = true;
		const Value& activeProfile = CurrentConfig->getActiveProfile();
		if (!activeProfile.IsObject() || !activeProfile.HasMember("rimcas") || !activeProfile["rimcas"].IsObject())
			return;

		const Value& rimcasConfig = activeProfile["rimcas"];
		if (rimcasConfig.HasMember("background_color_stage_one") && rimcasConfig["background_color_stage_one"].IsObject())
			rimcasStageOneColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_one"]);
		if (rimcasConfig.HasMember("background_color_stage_two") && rimcasConfig["background_color_stage_two"].IsObject())
			rimcasStageTwoColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_two"]);
	};

	for (std::map<string, bool>::iterator it = RimcasInstance->MonitoredRunwayArr.begin(); it != RimcasInstance->MonitoredRunwayArr.end(); ++it)
	{
		const auto timeTableIt = RimcasInstance->TimeTable.find(it->first);
		if (!it->second || timeTableIt == RimcasInstance->TimeTable.end() || timeTableIt->second.empty())
			continue;
		const auto& runwayTimeTable = timeTableIt->second;
		resolveRimcasColors();

		auto timePopupAreaIt = TimePopupAreas.find(it->first);
		if (timePopupAreaIt == TimePopupAreas.end())
		{
			auto emplaceResult = TimePopupAreas.emplace(it->first, RECT{ 300, 300, 430, 300 + LONG(TextHeight * (TimeDefinition.size() + 1)) });
			timePopupAreaIt = emplaceResult.first;
		}

		CRect CRectTime = timePopupAreaIt->second;
		CRectTime.NormalizeRect();

		dc.FillRect(CRectTime, &BrushGrey);

		// Drawing the runway name
		string tempS = it->first;
		dc.TextOutA(CRectTime.left + CRectTime.Width() / 2 - dc.GetTextExtent(tempS.c_str()).cx / 2, CRectTime.top, tempS.c_str());

		int TopOffset = TextHeight;
		// Drawing the times
		for (const auto& Time : TimeDefinition)
		{
			dc.SetTextColor(RGB(33, 33, 33));

			const auto timeEntryIt = runwayTimeTable.find(Time);
			const std::string acCallsign = (timeEntryIt != runwayTimeTable.end()) ? timeEntryIt->second : "";
			tempS = std::to_string(Time) + ": " + acCallsign;
			const VsmrScene::Target* sceneTarget = frameScene != nullptr
				? frameScene->FindTarget(acCallsign)
				: nullptr;
			if (sceneTarget != nullptr &&
				sceneTarget->rimcas.alertStage != static_cast<int>(CRimcas::NoAlert))
			{
				const Color alertColor = sceneTarget->rimcas.alertStage == static_cast<int>(CRimcas::StageOne)
					? rimcasStageOneColor
					: rimcasStageTwoColor;
				CBrush RimcasBrush(alertColor.ToCOLORREF());

				CRect TempRect = { CRectTime.left, CRectTime.top + TopOffset, CRectTime.right, CRectTime.top + TopOffset + TextHeight };
				TempRect.NormalizeRect();

				dc.FillRect(TempRect, &RimcasBrush);
				dc.SetTextColor(RGB(238, 238, 208));
			}

			dc.TextOutA(CRectTime.left, CRectTime.top + TopOffset, tempS.c_str());

			TopOffset += TextHeight;
		}

		AddScreenObject(RIMCAS_IAW, it->first.c_str(), CRectTime, true, "");

	}
	performance.rimcasMilliseconds += RefreshPerfNowMs() - perfRimcasListStartMs;
}

void CSMRRadar::DeconflictRefreshTags(
	const VsmrScene::RadarScene* frameScene,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	VSMR_REFRESH_LOG("Tag deconfliction loop");
	setRefreshStage("tag deconfliction");
	const double perfTagDeconflictStartMs = RefreshPerfNowMs();
	bool autoDeconflictionEnabled = true;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() &&
			profile.HasMember("labels") &&
			profile["labels"].IsObject() &&
			profile["labels"].HasMember("auto_deconfliction") &&
			profile["labels"]["auto_deconfliction"].IsBool())
		{
			autoDeconflictionEnabled = profile["labels"]["auto_deconfliction"].GetBool();
		}
	}

	std::vector<std::string> staleTagCallsigns;
	auto isTagCoolingDown = [&](const std::string& callsign) -> bool
	{
		auto recentMoveIt = RecentlyAutoMovedTags.find(callsign);
		if (recentMoveIt == RecentlyAutoMovedTags.end())
			return false;

		if (std::chrono::steady_clock::now() - recentMoveIt->second >=
			std::chrono::milliseconds(800))
		{
			RecentlyAutoMovedTags.erase(recentMoveIt);
			return false;
		}
		return true;
	};
	auto intersectsAnyOtherTag = [&](const std::string& callsign, const CRect& rect, bool* outRotateAntiClockwise) -> bool
	{
		for (const auto& otherArea : tagCollisionAreas)
		{
			if (callsign == otherArea.first)
				continue;
			if (IsTagBeingDragged(otherArea.first))
				continue;

			CRect intersection;
			if (!intersection.IntersectRect(rect, otherArea.second))
				continue;

			if (outRotateAntiClockwise != nullptr)
				*outRotateAntiClockwise = (rect.left <= otherArea.second.left);
			return true;
		}
		return false;
	};

	if (autoDeconflictionEnabled)
	{
		for (const auto& areaEntry : tagCollisionAreas)
		{
			const std::string& callsign = areaEntry.first;
			const CRect& currentRect = areaEntry.second;

			if (IsTagBeingDragged(callsign))
				continue;
			if (isTagCoolingDown(callsign))
				continue;

			// Decide the preferred rotation direction from current overlaps.
			bool rotateAntiClockwise = false;
			(void)intersectsAnyOtherTag(callsign, currentRect, &rotateAntiClockwise);

			const VsmrScene::Target* deconflictTarget = frameScene != nullptr
				? frameScene->FindTarget(callsign)
				: nullptr;
			if (deconflictTarget == nullptr || !deconflictTarget->position.valid)
			{
				staleTagCallsigns.push_back(callsign);
				if (Logger::is_verbose_mode())
				{
					Logger::info(
						"OnRefresh deconfliction: pruned stale tag state callsign=" + callsign +
						" target_valid=" + std::string(deconflictTarget != nullptr ? "1" : "0"));
				}
				continue;
			}

			auto angleIt = TagAngles.find(callsign);
			if (angleIt == TagAngles.end())
				angleIt = TagAngles.emplace(callsign, 270.0f).first;
			const double baseAngle = angleIt->second;

			int leaderLength = LeaderLineDefaultlenght;
			auto leaderLengthIt = TagLeaderLineLength.find(callsign);
			if (leaderLengthIt != TagLeaderLineLength.end())
				leaderLength = leaderLengthIt->second;

			CPosition deconflictPosition;
			deconflictPosition.m_Latitude = deconflictTarget->position.latitude;
			deconflictPosition.m_Longitude = deconflictTarget->position.longitude;
			const POINT acPosPix = ConvertCoordFromPositionToPixel(deconflictPosition);
			const int width = currentRect.Width();
			const int height = currentRect.Height();

			for (double rotated = 0.0; abs(rotated) <= 360.0;)
			{
				const double candidateAngle = fmod(baseAngle + rotated, 360.0f);
				POINT tagCenter;
				tagCenter.x = long(acPosPix.x + float(leaderLength * cos(VsmrRadarUiSupport::DegToRad(candidateAngle))));
				tagCenter.y = long(acPosPix.y + float(leaderLength * sin(VsmrRadarUiSupport::DegToRad(candidateAngle))));

				CRect newRectangle(
					tagCenter.x - (width / 2),
					tagCenter.y - (height / 2),
					tagCenter.x + (width / 2),
					tagCenter.y + (height / 2));
				newRectangle.NormalizeRect();

				const bool hasConflict = intersectsAnyOtherTag(callsign, newRectangle, nullptr);
				if (!hasConflict)
				{
					angleIt->second = fmod(baseAngle + rotated, 360.0f);

					const POINT newCenter = newRectangle.CenterPoint();
					const POINT newOffset = { newCenter.x - acPosPix.x, newCenter.y - acPosPix.y };
					TagsOffsets[callsign] = newOffset;

					tagCollisionAreas[callsign] = newRectangle;
					RecentlyAutoMovedTags[callsign] = std::chrono::steady_clock::now();
					break;
				}

				rotated += rotateAntiClockwise ? -22.5f : 22.5f;
			}
		}
	}
	if (!staleTagCallsigns.empty())
	{
		for (const std::string& callsign : staleTagCallsigns)
		{
			TagsOffsets.erase(callsign);
			TagAngles.erase(callsign);
			TagLeaderLineLength.erase(callsign);
			tagAreas.erase(callsign);
			tagCollisionAreas.erase(callsign);
			previousTagSize.erase(callsign);
			TagDragOffsetFromCenter.erase(callsign);
			RecentlyAutoMovedTags.erase(callsign);
			Patatoides.erase(callsign);
		}

		if (Logger::is_verbose_mode())
		{
			Logger::info(
				"OnRefresh deconfliction: removed stale entries count=" +
				std::to_string(staleTagCallsigns.size()));
		}
	}
	performance.tagsMilliseconds += RefreshPerfNowMs() - perfTagDeconflictStartMs;
}

void CSMRRadar::RenderRefreshInsets(
	HDC hDC,
	Gdiplus::Graphics& graphics,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	VSMR_REFRESH_LOG("App window rendering");
	setRefreshStage("app window rendering");

	SyncLinkedAvisoSecondaryToMainView();

	CInsetWindow* activeInsetWindow = nullptr;
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
			(appWindowIt->second->IsWindowMoveActive() || appWindowIt->second->IsWindowResizeActive()))
		{
			activeInsetWindow = appWindowIt->second.get();
			break;
		}
	}
	CInsetWindow* foregroundInsetWindow = activeInsetWindow;
	if (foregroundInsetWindow == nullptr)
	{
		for (const auto& display : appWindowDisplays)
		{
			if (!display.second)
				continue;
			auto appWindowIt = appWindows.find(display.first);
			if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
				appWindowIt->second->m_AvisoScrollSelected)
			{
				foregroundInsetWindow = appWindowIt->second.get();
				break;
			}
		}
	}
	auto renderInsetWindow = [&](CInsetWindow* appWindow)
	{
		if (appWindow == nullptr)
			return;
		const double insetStartMs = RefreshPerfNowMs();
		appWindow->render(hDC, this, &graphics, mouseLocation);
		const double insetElapsedMs = RefreshPerfNowMs() - insetStartMs;
		if (appWindow->IsSecondaryRadar())
			performance.srwMilliseconds += insetElapsedMs;
		else if (appWindow->IsAvisoViewport())
			performance.avisoInsetMilliseconds += insetElapsedMs;
		performance.rdfMilliseconds += appWindow->GetLastRdfRenderMilliseconds();
		performance.insetChromeMilliseconds += appWindow->GetLastChromeRenderMilliseconds();
	};
	for (std::map<int, bool>::iterator it = appWindowDisplays.begin(); it != appWindowDisplays.end(); ++it)
	{
		if (!it->second)
			continue;

		int appWindowId = it->first;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			continue;

		CInsetWindow* appWindow = appWindowIt->second.get();
		if (appWindow != foregroundInsetWindow)
			renderInsetWindow(appWindow);
	}
	if (activeInsetWindow != nullptr)
		activeInsetWindow->RenderSnapPreview(graphics);
	if (foregroundInsetWindow != nullptr)
		renderInsetWindow(foregroundInsetWindow);
}

void CSMRRadar::RenderRefreshFpsOverlay(
	CDC& dc,
	const RECT& radarArea,
	const std::function<void(const char*)>& setRefreshStage)
{
	setRefreshStage("fps overlay");
	if (ShowFps)
	{
		const std::string fpsText = "FPS " + std::to_string(FpsDisplayValue);
		CFont* previousFont = RuntimeOverlayFont.GetSafeHandle() != nullptr
			? dc.SelectObject(&RuntimeOverlayFont)
			: nullptr;
		const CSize fpsSize = dc.GetTextExtent(fpsText.c_str());
		CRect fpsArea = ResolveMainAvisoRenderArea();
		if (fpsArea.IsRectEmpty())
			fpsArea = CRect(radarArea);
		fpsArea.NormalizeRect();
		auto overlapsVisibleInset = [&](const CRect& candidate, int& nextY) -> bool
		{
			bool overlaps = false;
			for (const auto& display : appWindowDisplays)
			{
				if (!display.second)
					continue;
				auto windowIt = appWindows.find(display.first);
				if (windowIt == appWindows.end() || windowIt->second == nullptr)
					continue;
				CRect overlap;
				const CRect frame = windowIt->second->GetWindowFrameRect();
				if (overlap.IntersectRect(candidate, frame) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, frame.bottom + 4);
				}
				CRect preview;
				if (windowIt->second->GetSnapPreviewRect(preview) &&
					overlap.IntersectRect(candidate, preview) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, preview.bottom + 4);
				}
			}
			return overlaps;
		};
		auto findFpsPosition = [&](bool alignRight, POINT& position) -> bool
		{
			int y = fpsArea.top + 4;
			for (size_t attempt = 0; attempt <= appWindows.size(); ++attempt)
			{
				const int x = alignRight
					? max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6)
					: fpsArea.left + 6;
				CRect candidate(x, y, x + fpsSize.cx, y + fpsSize.cy);
				if (candidate.right > fpsArea.right || candidate.bottom > fpsArea.bottom)
					return false;
				int nextY = y;
				if (!overlapsVisibleInset(candidate, nextY))
				{
					position = { x, y };
					return true;
				}
				if (nextY <= y)
					return false;
				y = nextY;
			}
			return false;
		};
		POINT fpsPosition = {
			max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6),
			fpsArea.top + 4
		};
		bool fpsPositionFound = findFpsPosition(true, fpsPosition);
		if (!fpsPositionFound)
			fpsPositionFound = findFpsPosition(false, fpsPosition);
		const int previousBackgroundMode = dc.SetBkMode(TRANSPARENT);
		const COLORREF previousTextColor = dc.SetTextColor(RGB(235, 245, 247));
		if (fpsPositionFound)
			dc.TextOutA(fpsPosition.x, fpsPosition.y, fpsText.c_str());
		dc.SetTextColor(previousTextColor);
		dc.SetBkMode(previousBackgroundMode);
		if (previousFont != nullptr)
			dc.SelectObject(previousFont);
	}
}

void CSMRRadar::RecordRefreshPerformance(
	const VsmrScene::RadarScene* frameScene,
	RefreshPerformance& performance)
{
	PerfLastFrameMs = RefreshPerfNowMs() - performance.frameStartMilliseconds;
	PerfLastAvisoMs = performance.avisoMilliseconds;
	PerfLastTargetsMs = performance.targetsMilliseconds;
	PerfLastRimcasMs = performance.rimcasMilliseconds;
	PerfLastTagsMs = performance.tagsMilliseconds;
	PerfLastSrwMs = performance.srwMilliseconds;
	PerfLastAvisoInsetMs = performance.avisoInsetMilliseconds;
	PerfLastRdfMs = performance.rdfMilliseconds;
	PerfLastInsetChromeMs = performance.insetChromeMilliseconds;
	VsmrPerformance::FrameSample performanceFrame;
	performanceFrame.frameId = frameScene != nullptr ? frameScene->frameId : RadarSceneFrameId;
	performanceFrame.timestampMilliseconds =
		VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
	performanceFrame.frameMilliseconds = PerfLastFrameMs;
	performanceFrame.sceneMilliseconds = PerfLastSceneBuildMs;
	performanceFrame.avisoMilliseconds = PerfLastAvisoMs;
	performanceFrame.targetsMilliseconds = PerfLastTargetsMs;
	performanceFrame.rimcasMilliseconds = PerfLastRimcasMs;
	performanceFrame.tagsMilliseconds = PerfLastTagsMs;
	performanceFrame.srwMilliseconds = PerfLastSrwMs;
	performanceFrame.avisoInsetMilliseconds = PerfLastAvisoInsetMs;
	performanceFrame.rdfMilliseconds = PerfLastRdfMs;
	performanceFrame.insetChromeMilliseconds = PerfLastInsetChromeMs;
	performanceFrame.visibleTargets = performance.visibleTargetCount;
	performanceFrame.visibleTags = tagAreas.size();
	if (frameScene != nullptr)
	{
		performanceFrame.sceneAvisoLoadMilliseconds = frameScene->stats.avisoLoadMilliseconds;
		performanceFrame.sceneControllerCaptureMilliseconds =
			frameScene->stats.controllerCaptureMilliseconds;
		performanceFrame.sceneTargetCaptureMilliseconds = frameScene->stats.targetCaptureMilliseconds;
		performanceFrame.sceneFinalizeMilliseconds = frameScene->stats.finalizeMilliseconds;
		performanceFrame.euroScopeLookupMilliseconds = frameScene->stats.sdkLookupMilliseconds;
		performance.refreshReasonMask |= frameScene->stats.refreshReasonMask;
		performanceFrame.processedTargets = frameScene->stats.sdkTargetEnumerations;
		performanceFrame.capturedTargets = frameScene->stats.targetCount;
		performanceFrame.radarFilteredTargets = frameScene->stats.radarFilteredTargetCount;
		performanceFrame.iconTargets = frameScene->stats.iconTargetCount;
		performanceFrame.tagTargets = frameScene->stats.tagTargetCount;
	}
	performanceFrame.refreshReasonMask = performance.refreshReasonMask;
	PerformanceDiagnostics.RecordFrame(performanceFrame);
	SamplePerformanceResourcesIfDue();
	const unsigned long perfNowTick = ::GetTickCount();
	if (PerfLastLogTick == 0 || perfNowTick - PerfLastLogTick >= 2000)
	{
		PerfLastLogTick = perfNowTick;
		Logger::info(
			"FramePerf ms frame=" + std::to_string(static_cast<int>(PerfLastFrameMs + 0.5)) +
			" scene=" + std::to_string(static_cast<int>(PerfLastSceneBuildMs + 0.5)) +
			" aviso=" + std::to_string(static_cast<int>(PerfLastAvisoMs + 0.5)) +
			" targets=" + std::to_string(static_cast<int>(PerfLastTargetsMs + 0.5)) +
			" rimcas=" + std::to_string(static_cast<int>(PerfLastRimcasMs + 0.5)) +
			" tags=" + std::to_string(static_cast<int>(PerfLastTagsMs + 0.5)) +
			" srw=" + std::to_string(static_cast<int>(PerfLastSrwMs + 0.5)) +
			" aviso_inset=" + std::to_string(static_cast<int>(PerfLastAvisoInsetMs + 0.5)) +
			" rdf=" + std::to_string(static_cast<int>(PerfLastRdfMs + 0.5)) +
			" inset_chrome=" + std::to_string(static_cast<int>(PerfLastInsetChromeMs + 0.5)) +
			(frameScene != nullptr
				? " scene_targets=" + std::to_string(frameScene->stats.targetCount) +
				  " scene_tags=" + std::to_string(frameScene->stats.tagElementCount) +
				  " scene_controllers=" + std::to_string(frameScene->stats.controllerCount) +
				  " scene_bytes_lower=" + std::to_string(frameScene->stats.lowerBoundOwnedBytes) +
				  " scene_es_targets=" + std::to_string(frameScene->stats.sdkTargetEnumerations) +
				  " scene_es_fp=" + std::to_string(frameScene->stats.sdkFlightPlanLookups) +
				  " scene_es_correlated_fp=" + std::to_string(frameScene->stats.sdkCorrelatedFlightPlanLookups) +
				  " scene_cdm=" + std::to_string(frameScene->stats.cdmLookups)
				: " scene_unavailable=1"));
	}
}

bool CSMRRadar::PrepareRefreshPhase(HDC hDC, int phase)
{
	if (Logger::is_verbose_mode())
	{
		Logger::info(
			"OnRefresh begin phase=" + std::to_string(phase) +
			" tag_collision_count=" + std::to_string(tagCollisionAreas.size()) +
			" tag_offset_count=" + std::to_string(TagsOffsets.size()) +
				" active_airport=" + getActiveAirport());
	}

	if (CurrentConfig == nullptr || RimcasInstance == nullptr)
	{
		static bool loggedMissingCoreObjects = false;
		if (!loggedMissingCoreObjects)
		{
			Logger::info("OnRefresh: skipped frame because core objects are not initialized");
			loggedMissingCoreObjects = true;
		}
		return false;
	}

	EnsureAvisoWheelHooks(this);
	// Refresh pipeline is phase-driven by EuroScope. Cursor setup stays on the UI thread.
	if (initCursor)
	{
		if (customCursor) {
			HCURSOR loadedCursor = reinterpret_cast<HCURSOR>(::LoadImage(
				AfxGetInstanceHandle(),
				MAKEINTRESOURCE(IDC_SMRCURSOR),
				IMAGE_CURSOR,
				0,
				0,
				LR_SHARED));
			if (loadedCursor != nullptr)
				smrCursor = loadedCursor;
			// EuroScope/MFC can still override the cursor occasionally; we therefore reapply it via window proc hook.

		}
		if (smrCursor == nullptr)
			smrCursor = ::LoadCursor(nullptr, IDC_ARROW);
		if (smrCursor != nullptr)
			::SetCursor(smrCursor);

		initCursor = false;
	}
	HWND insetHostWindow = ::WindowFromDC(hDC);
	if (insetHostWindow == nullptr || !::IsWindow(insetHostWindow))
		insetHostWindow = ::GetActiveWindow();
	EnsureInsetWindowProcHook(insetHostWindow, this);
	AvisoRefreshHostWindow.store(insetHostWindow, std::memory_order_release);

	if (phase == REFRESH_PHASE_AFTER_LISTS) {
		VSMR_REFRESH_LOG("phase == REFRESH_PHASE_AFTER_LISTS");
		VSMR_REFRESH_LOG("break phase == REFRESH_PHASE_AFTER_LISTS");
		return false;
	}

	if (phase != REFRESH_PHASE_BEFORE_TAGS)
		return false;

	VSMR_REFRESH_LOG("phase != REFRESH_PHASE_BEFORE_TAGS");
	const unsigned long fpsNowTick = ::GetTickCount();
	if (FpsLastSampleTick == 0)
		FpsLastSampleTick = fpsNowTick;
	++FpsFrameCount;
	const unsigned long fpsElapsedMs = fpsNowTick - FpsLastSampleTick;
	if (fpsElapsedMs >= 500)
	{
		FpsDisplayValue = static_cast<int>((static_cast<double>(FpsFrameCount) * 1000.0 / static_cast<double>(fpsElapsedMs)) + 0.5);
		FpsFrameCount = 0;
		FpsLastSampleTick = fpsNowTick;
	}
	return true;
}

void CSMRRadar::RenderRefreshTagsAndRdf(
	HDC hDC,
	Gdiplus::Graphics& graphics,
	CDC& dc,
	RefreshPerformance& performance,
	const std::function<void(const char*)>& setRefreshStage)
{
	tagAreas.clear();
	tagCollisionAreas.clear();

	graphics.SetSmoothingMode(SmoothingModeDefault);

	const double perfTagsStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("tag rendering");
		RenderTags(graphics, dc);
	}
	catch (const std::exception& ex)
	{
		Logger::info(std::string("RenderTags: std::exception caught: ") + ex.what());
	}
	catch (...)
	{
		Logger::info("RenderTags: unknown C++ exception caught");
	}
	performance.tagsMilliseconds += RefreshPerfNowMs() - perfTagsStartMs;

	// RDF is resolved and projected on the EuroScope/UI thread.  Drawing it
	// here keeps the main-view marker above vSMR targets and labels, while the
	// inset renderers below can cover it with their own independently projected
	// RDF overlays.
	graphics.Flush(Gdiplus::FlushIntentionSync);
	const CRect rdfMainArea = ResolveMainAvisoRenderArea();
	if (!rdfMainArea.IsRectEmpty())
	{
		const double perfRdfStartMs = RefreshPerfNowMs();
		VsmrRdf::Draw(
			hDC,
			this,
			rdfMainArea,
			[this](const CPosition& position) -> POINT
			{
				return ConvertCoordFromPositionToPixel(position);
			});
		performance.rdfMilliseconds += RefreshPerfNowMs() - perfRdfStartMs;
	}

}

void CSMRRadar::OnRefresh(HDC hDC, int Phase)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnRefresh",
		reinterpret_cast<std::uintptr_t>(this));
	if (IsShutdownRequested())
		return;
	PublishCrashRadarState("main");

	VSMR_REFRESH_LOG(string(__FUNCSIG__));
	const char* refreshStage = "entry";
	auto setRefreshStage = [&](const char* stage)
	{
		refreshStage = stage;
		VsmrCrashReporter::RecordBreadcrumb("render stage", stage);
	};
	VsmrCrashReporter::RecordBreadcrumb("render stage", refreshStage);
	auto logRefreshException = [&](const std::string& reason)
	{
		static DWORD lastLogTick = 0;
		static std::string lastMessage;
		const DWORD nowTick = ::GetTickCount();
		const std::string message = "OnRefresh caught exception stage=" + std::string(refreshStage) + " reason=" + reason;
		if (message != lastMessage || nowTick - lastLogTick > 2000)
		{
			OutputVsmrDebugLine(message);
			lastMessage = message;
			lastLogTick = nowTick;
		}
	};

	try
	{
		if (!PrepareRefreshPhase(hDC, Phase))
			return;
		RefreshPerformance performance;
		performance.frameStartMilliseconds = RefreshPerfNowMs();
		PerformanceDiagnostics.BeginFrame();
		performance.refreshReasonMask =
			PendingPerformanceRefreshReasonMask.exchange(0, std::memory_order_acq_rel);

		RefreshSectorMap(performance, setRefreshStage);

		POINT p;
		if (GetCursorPos(&p))
		{
			HWND activeWindow = GetActiveWindow();
			if (activeWindow != nullptr && ScreenToClient(activeWindow, &p))
				mouseLocation = p;
		}

		VSMR_REFRESH_LOG("Graphics set up");
		setRefreshStage("graphics setup");
		CDC dc;
		dc.Attach(hDC);
		ScopedCdcDetach dcDetach(dc);

		// Creating the gdi+ graphics
		Graphics graphics(hDC);
		graphics.SetPageUnit(Gdiplus::UnitPixel);
		graphics.SetSmoothingMode(SmoothingModeAntiAlias);

		RECT RadarArea = GetRadarArea();
		RECT ChatArea = GetChatArea();
		CRect normalizedChatArea(ChatArea);
		normalizedChatArea.NormalizeRect();
		if (!normalizedChatArea.IsRectEmpty())
			RadarArea.bottom = normalizedChatArea.top;
		CRect insetLayoutBounds(RadarArea);
		insetLayoutBounds.NormalizeRect();
		PrepareRefreshInsetLayout(insetLayoutBounds);

		const std::shared_ptr<const VsmrScene::RadarScene> frameSceneOwner =
			BuildRefreshSceneAndRenderAviso(
				hDC,
				graphics,
				performance,
				setRefreshStage);
		const VsmrScene::RadarScene* frameScene = frameSceneOwner.get();

		RenderClosedRunwayOverlays(graphics, setRefreshStage);
		RenderRefreshTargets(
			graphics,
			dc,
			RadarArea,
			frameScene,
			performance,
			setRefreshStage);
		RenderRefreshTagsAndRdf(hDC, graphics, dc, performance, setRefreshStage);
		RenderRefreshRimcasPanels(dc, frameScene, performance, setRefreshStage);

		// Tag deconflicting
		DeconflictRefreshTags(frameScene, performance, setRefreshStage);

		// App windows
		RenderRefreshInsets(
			hDC,
			graphics,
			performance,
			setRefreshStage);
		RenderRefreshFpsOverlay(dc, RadarArea, setRefreshStage);

		setRefreshStage("runtime menu");
		RenderRuntimeMenu(hDC, graphics);
		RecordRefreshPerformance(frameScene, performance);

		dcDetach.Detach();
		setRefreshStage("complete");
		VSMR_REFRESH_LOG("END " + string(__FUNCSIG__));
	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		logRefreshException("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		logRefreshException("MFC exception");
	}
	catch (const std::exception& ex)
	{
		logRefreshException(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		logRefreshException("unknown exception");
	}
}
