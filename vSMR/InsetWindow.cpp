#include "stdafx.h"
#include "InsetWindow.h"
#include "SMRRadar_TagShared.hpp"


CInsetWindow::CInsetWindow(int Id)
{
	m_Id = Id;
}

CInsetWindow::~CInsetWindow()
{
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

bool CInsetWindow::OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool Released)
{
	if (strcmp(sObjectId, "window") == 0) {
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

		return Released;
	}
	if (strcmp(sObjectId, "topbar") == 0) {

		CRect appWindowRect(m_Area);
		appWindowRect.NormalizeRect();

		POINT TopLeft = { Area.left, Area.bottom + 1 };
		POINT BottomRight = { TopLeft.x + appWindowRect.Width(), TopLeft.y + appWindowRect.Height() };
		CRect newPos(TopLeft, BottomRight);
		newPos.NormalizeRect();

		m_Area = newPos;

		return Released;
	}

	if (sObjectId != nullptr && strcmp(sObjectId, "window") != 0 && strcmp(sObjectId, "resize") != 0 && strcmp(sObjectId, "topbar") != 0)
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

void CInsetWindow::render(HDC hDC, CSMRRadar * radar_screen, Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools)
{
	CDC dc;
	dc.Attach(hDC);

	if (this->m_Id == -1)
		return;

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

		static RECT drawToolbarButton(CDC * dc, string letter, CRect TopBar, int left, POINT mouseLocation)
		{
			POINT TopLeft = { TopBar.right - left, TopBar.top + 2 };
			POINT BottomRight = { TopBar.right - (left - 11), TopBar.bottom - 2 };
			CRect Rect(TopLeft, BottomRight);
			Rect.NormalizeRect();
			CBrush ButtonBrush(RGB(60, 60, 60));
			dc->FillRect(Rect, &ButtonBrush);
			dc->SetTextColor(RGB(0, 0, 0));
			dc->TextOutA(Rect.left + 2, Rect.top, letter.c_str());

			if (mouseWithin(mouseLocation, Rect))
				dc->Draw3dRect(Rect, RGB(45, 45, 45), RGB(75, 75, 75));
			else
				dc->Draw3dRect(Rect, RGB(75, 75, 75), RGB(45, 45, 45));

			return Rect;
		}
	};

	icao = radar_screen->ActiveAirport;
	AptPositions = radar_screen->AirportPositions;
	m_TargetPoints.clear();
	m_TagAreas.clear();

	COLORREF qBackgroundColor = radar_screen->CurrentConfig->getConfigColorRef(radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["background_color"]);
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

			CPen RunwayPen(PS_SOLID, 1, radar_screen->CurrentConfig->getConfigColorRef(radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["runway_color"]));
			CPen ExtendedCentreLinePen(PS_SOLID, 1, radar_screen->CurrentConfig->getConfigColorRef(radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["extended_lines_color"]));
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
				double lenght = double(radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["extended_lines_length"].GetDouble()) * 1852.0;

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
				int increment = radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["extended_lines_ticks_spacing"].GetInt() * 1852;

				for (int j = increment; j <= int(radar_screen->CurrentConfig->getActiveProfile()["approach_insets"]["extended_lines_length"].GetInt() * 1852); j += increment) {

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
		int radarRange = radar_screen->CurrentConfig->getActiveProfile()["filters"]["radar_range_nm"].GetInt();

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

			bool tagProModeEnabled = false;
			bool useAspeedForGate = false;
			if (radar_screen->CurrentConfig != nullptr)
			{
				const Value& profile = radar_screen->CurrentConfig->getActiveProfile();
				if (profile.IsObject())
				{
					if (profile.HasMember("filters") &&
						profile["filters"].IsObject() &&
						profile["filters"].HasMember("pro_mode") &&
						profile["filters"]["pro_mode"].IsObject())
					{
						const Value& proMode = profile["filters"]["pro_mode"];
						if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
						{
							tagProModeEnabled = proMode["enabled"].GetBool();
						}
						else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
						{
							tagProModeEnabled = proMode["enable"].GetBool();
						}
					}

					if (profile.HasMember("labels") &&
						profile["labels"].IsObject() &&
						profile["labels"].HasMember("use_speed_for_gate") &&
						profile["labels"]["use_speed_for_gate"].IsBool())
					{
						useAspeedForGate = profile["labels"]["use_speed_for_gate"].GetBool();
					}
					else if (profile.HasMember("labels") &&
						profile["labels"].IsObject() &&
						profile["labels"].HasMember("use_aspeed_for_gate") &&
						profile["labels"]["use_aspeed_for_gate"].IsBool())
					{
						useAspeedForGate = profile["labels"]["use_aspeed_for_gate"].GetBool();
					}
				}
			}

			// ----- Generating the replacing map -----
			map<string, string> TagReplacingMap = CSMRRadar::GenerateTagData(
				rt,
				fp,
				isASEL,
				radar_screen->IsCorrelated(fp, rt),
				tagProModeEnabled,
				radar_screen->GetPlugIn()->GetTransitionAltitude(),
				useAspeedForGate,
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

		if (fpDestination != nullptr && strcmp(fpDestination, radar_screen->getActiveAirport().c_str()) == 0) {
				TagType = CSMRRadar::TagTypes::Arrival;
				ColorTagType = CSMRRadar::TagTypes::Arrival;
		}

			if (reportedGs > 50) {
				TagType = CSMRRadar::TagTypes::Airborne;

				// Is "use_departure_arrival_coloring" enabled? if not, then use the airborne colors
				bool useDepArrColors = false;
				if (radar_screen->CurrentConfig != nullptr)
				{
					const Value& profile = radar_screen->CurrentConfig->getActiveProfile();
					if (profile.IsObject() &&
						profile.HasMember("labels") &&
						profile["labels"].IsObject() &&
						profile["labels"].HasMember("airborne") &&
						profile["labels"]["airborne"].IsObject() &&
						profile["labels"]["airborne"].HasMember("use_departure_arrival_coloring") &&
						profile["labels"]["airborne"]["use_departure_arrival_coloring"].IsBool())
					{
						useDepArrColors = profile["labels"]["airborne"]["use_departure_arrival_coloring"].GetBool();
					}
				}
				if (!useDepArrColors) {
					ColorTagType = CSMRRadar::TagTypes::Airborne;
				}
			}

		bool AcisCorrelated = radar_screen->IsCorrelated(radar_screen->GetPlugIn()->FlightPlanSelect(rtCallsign), rt);
		if (!AcisCorrelated && reportedGs >= 3)
		{
			TagType = CSMRRadar::TagTypes::Uncorrelated;
			ColorTagType = CSMRRadar::TagTypes::Uncorrelated;
		}

		// First we need to figure out the tag size

		int TagWidth = 0, TagHeight = 0;
		RectF mesureRect;
		Gdiplus::Font* tagRegularFont = radar_screen->customFonts[radar_screen->currentFontSize];
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

		const Value& LabelsSettings = radar_screen->CurrentConfig->getActiveProfile()["labels"];
		const Value& LabelLines = LabelsSettings[Utils::getEnumString(TagType).c_str()]["definition"];
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
			return;

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
		bool rimcasLabelOnly = radar_screen->CurrentConfig->getActiveProfile()["rimcas"]["rimcas_label_only"].GetBool();

		Color definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_color"]);
		Color definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_color_on_runway"]);
		Color definedTextColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["text_color"]);
		if (TagType == CSMRRadar::TagTypes::Departure) {
			if (!TagReplacingMap["sid"].empty() && radar_screen->CurrentConfig->isSidColorAvail(TagReplacingMap["sid"], radar_screen->getActiveAirport())) {
				definedBackgroundColor = radar_screen->CurrentConfig->getSidColor(TagReplacingMap["sid"], radar_screen->getActiveAirport());
			}

			if (fpPlanType != nullptr && fpPlanType[0] == 'I' && TagReplacingMap["asid"].empty() && LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nosid_color")) {
				definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nosid_color"]);
			}
		}
			if (TagReplacingMap["actype"] == "NoFPL" && LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nofpl_color")) {
				definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nofpl_color"]);
		}

		if (TagType == CSMRRadar::TagTypes::Airborne &&
			fp.IsValid() &&
			AcisCorrelated &&
			LabelsSettings.HasMember("airborne") &&
			LabelsSettings["airborne"].IsObject())
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

			const Value& airborneLabel = LabelsSettings["airborne"];
			const char* bgKey = isAirborneDeparture ? "departure_background_color" : "arrival_background_color";
			const char* bgOnRunwayKey = isAirborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
			const char* textKey = isAirborneDeparture ? "departure_text_color" : "arrival_text_color";

			if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
				definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgKey]);
			if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
				definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
			if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
				definedTextColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[textKey]);
		}

		Color TagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign,
			definedBackgroundColor,
			definedBackgroundOnRunwayColor,
			radar_screen->CurrentConfig->getConfigColor(radar_screen->CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_one"]),
			radar_screen->CurrentConfig->getConfigColor(radar_screen->CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_two"]));

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
				radar_screen->CurrentConfig->getConfigColor(LabelsSettings["squawk_error_color"])));
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
					radar_screen->CurrentConfig->getConfigColor(radar_screen->CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_one"]),
					radar_screen->CurrentConfig->getConfigColor(radar_screen->CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_two"]));

				if (RimcasLabelColor.ToCOLORREF() != Color(Color::AliceBlue).ToCOLORREF()) {
					int rimcas_height = 0;

					wstring wrimcas_height = wstring(L"ALERT");

					RectF RectRimcas_height;

					gdi->MeasureString(wrimcas_height.c_str(), wcslen(wrimcas_height.c_str()), radar_screen->customFonts[radar_screen->currentFontSize], PointF(0, 0), &Gdiplus::StringFormat(), &RectRimcas_height);
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
					gdi->DrawString(rimcasw.c_str(), wcslen(rimcasw.c_str()), radar_screen->customFonts[radar_screen->currentFontSize], PointF(Gdiplus::REAL((TagBackgroundRect.left + TagBackgroundRect.right) / 2), Gdiplus::REAL(TagBackgroundRect.top)), &stformat, rimcasTextBrush);

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

		int radarRange = radar_screen->CurrentConfig->getActiveProfile()["filters"]["radar_range_nm"].GetInt();

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
	qBackgroundColor = RGB(60, 60, 60);
	POINT BottomRight = { m_Area.right, m_Area.bottom };
	POINT TopLeft = { BottomRight.x - 10, BottomRight.y - 10 };
	CRect ResizeArea = { TopLeft, BottomRight };
	ResizeArea.NormalizeRect();
	dc.FillSolidRect(ResizeArea, qBackgroundColor);
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
	CRect RangeRect = Utils::drawToolbarButton(&dc, "Z", TopBar, 29, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "range", RangeRect, false, "");

	// Filter button
	CRect FilterRect = Utils::drawToolbarButton(&dc, "F", TopBar, 42, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "filter", FilterRect, false, "");

	// Rotate button
	CRect RotateRect = Utils::drawToolbarButton(&dc, "R", TopBar, 55, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "rotate", RotateRect, false, "");

	dc.SetTextColor(oldTextColorC);

	// Close
	POINT TopLeftClose = { TopBar.right - 16, TopBar.top + 2 };
	POINT BottomRightClose = { TopBar.right - 5, TopBar.bottom - 2 };
	CRect CloseRect(TopLeftClose, BottomRightClose);
	CloseRect.NormalizeRect();
	CBrush CloseBrush(RGB(60, 60, 60));
	dc.FillRect(CloseRect, &CloseBrush);
	CPen BlackPen(PS_SOLID, 1, RGB(0, 0, 0));
	dc.SelectObject(BlackPen);
	dc.MoveTo(CloseRect.TopLeft());
	dc.LineTo(CloseRect.BottomRight());
	dc.MoveTo({ CloseRect.right - 1, CloseRect.top });
	dc.LineTo({ CloseRect.left - 1, CloseRect.bottom });

	if (mouseWithin(mouseLocation, CloseRect))
		dc.Draw3dRect(CloseRect, RGB(45, 45, 45), RGB(75, 75, 75));
	else
		dc.Draw3dRect(CloseRect, RGB(75, 75, 75), RGB(45, 45, 45));

	radar_screen->AddScreenObject(m_Id, "close", CloseRect, false, "");

	dc.Detach();
}

