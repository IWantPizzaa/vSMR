#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/InsetWindow.Internal.hpp"
#include "radar/RadarScreen.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include <chrono>

using VsmrRadarUiSupport::BetterHarversine;
using VsmrRadarUiSupport::CopyRect;
using VsmrRadarUiSupport::DegToRad;
using VsmrRadarUiSupport::LiangBarsky;
using VsmrRadarUiSupport::RadToDeg;
using VsmrRadarUiSupport::TrueBearing;
using VsmrRadarUiSupport::mouseWithin;
using VsmrRadarUiSupport::rotate_point;
using VsmrRadarUiSupport::startsWith;
using VsmrInsetWindowInternal::DrawInsetWindowChrome;
using VsmrInsetWindowInternal::DrawRadarInsetBorder;
using VsmrInsetWindowInternal::SceneColorToGdi;
using VsmrInsetWindowInternal::kAvisoMetersPerNm;

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
