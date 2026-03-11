#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern HCURSOR smrCursor;
extern bool standardCursor;
extern bool customCursor;
extern map<int, CInsetWindow*> appWindows;

void CSMRRadar::OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released) {
	Logger::info(string(__FUNCSIG__));

	if (ObjectType == APPWINDOW_ONE || ObjectType == APPWINDOW_TWO) {
		int appWindowId = ObjectType - APPWINDOW_BASE;

		bool toggleCursor = appWindows[appWindowId]->OnMoveScreenObject(sObjectId, Pt, Area, Released);

		if (!toggleCursor)
		{
			if (standardCursor)
			{
				if (strcmp(sObjectId, "topbar") == 0)
					smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRMOVEWINDOW), IMAGE_CURSOR, 0, 0, LR_SHARED));
				else if (strcmp(sObjectId, "resize") == 0)
					smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRRESIZE), IMAGE_CURSOR, 0, 0, LR_SHARED));

				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = false;
			}
		} else
		{
			if (!standardCursor)
			{
				if (customCursor) {
					smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));
				}
				else {
					smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
				}

				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = true;
			}
		}
	}

	if (ObjectType == DRAWING_TAG || ObjectType == TAG_CITEM_MANUALCORRELATE || ObjectType == TAG_CITEM_CALLSIGN || ObjectType == TAG_CITEM_FPBOX || ObjectType == TAG_CITEM_RWY || ObjectType == TAG_CITEM_SID || ObjectType == TAG_CITEM_GATE || ObjectType == TAG_CITEM_NO || ObjectType == TAG_CITEM_GROUNDSTATUS || ObjectType == TAG_CITEM_CLEARANCE || ObjectType == TAG_CITEM_UKSTAND || ObjectType == TAG_CITEM_REMARK || ObjectType == TAG_CITEM_SCRATCHPAD) {
		auto routeMoveToInsetWindow = [&]() -> bool
		{
			if (sObjectId == nullptr || sObjectId[0] == '\0')
				return false;

			for (auto& kv : appWindows)
			{
				CInsetWindow* insetWindow = kv.second;
				if (insetWindow == nullptr)
					continue;

				const bool draggingThisWindowTag =
					!insetWindow->m_TagBeingDragged.empty() &&
					insetWindow->m_TagBeingDragged == sObjectId;
				auto insetTagAreaIt = insetWindow->m_TagAreas.find(sObjectId);
				const bool windowHasTag = insetTagAreaIt != insetWindow->m_TagAreas.end();

				CRect windowRect(insetWindow->m_Area);
				windowRect.NormalizeRect();
				const bool pointerInWindow =
					Pt.x >= windowRect.left && Pt.x <= windowRect.right &&
					Pt.y >= windowRect.top && Pt.y <= windowRect.bottom;

				if (!draggingThisWindowTag)
				{
					if (!windowHasTag || !pointerInWindow)
						continue;
				}

				insetWindow->OnMoveScreenObject(sObjectId, Pt, Area, Released);
				return true;
			}

			return false;
		};
		if (routeMoveToInsetWindow())
		{
			mouseLocation = Pt;
			RequestRefresh();
			return;
		}

		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);

		if (!Released)
		{
			if (standardCursor)
			{
				smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRMOVETAG), IMAGE_CURSOR, 0, 0, LR_SHARED));
				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = false;
			}
		}
		else
		{
			if (!standardCursor)
			{
				if (customCursor) {
					smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));
				}
				else {
					smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
				}

				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = true;
			}
		}

		if (rt.IsValid() && rt.GetPosition().IsValid()) {
			POINT TagCenterPix;

			// First frame of drag: capture offset between tag center and grab point.
			bool firstDragFrame = (!Released && TagBeingDragged != sObjectId);
			if (firstDragFrame) {
				POINT fullCenter{};
				auto fullRectIt = tagAreas.find(sObjectId);
				if (fullRectIt != tagAreas.end()) {
					fullCenter = fullRectIt->second.CenterPoint();
				}
				else {
					CRect tmp = Area;
					fullCenter = tmp.CenterPoint();
				}
				POINT offset = { fullCenter.x - Pt.x, fullCenter.y - Pt.y };
				TagDragOffsetFromCenter[sObjectId] = offset;
			}

			// Always apply stored offset if available (even on release) to avoid snap.
			auto offIt = TagDragOffsetFromCenter.find(sObjectId);
			if (offIt != TagDragOffsetFromCenter.end()) {
				TagCenterPix.x = Pt.x + offIt->second.x;
				TagCenterPix.y = Pt.y + offIt->second.y;
			}
			else {
				// Fallbacks
				CRect Temp = Area;
				if (ObjectType == DRAWING_TAG)
					TagCenterPix = Temp.CenterPoint();
				else
					TagCenterPix = Pt;
			}

			POINT AcPosPix = ConvertCoordFromPositionToPixel(rt.GetPosition().GetPosition());
			POINT CustomTag = { TagCenterPix.x - AcPosPix.x, TagCenterPix.y - AcPosPix.y };

			
			TagsOffsets[sObjectId] = CustomTag;
			TagAngles[sObjectId] = fmod(atan2(double(CustomTag.y), double(CustomTag.x)) * 180.0 / PI, 360);
			TagLeaderLineLength[sObjectId] = static_cast<int>(sqrt(double(CustomTag.x * CustomTag.x + CustomTag.y * CustomTag.y)));

			GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));

			if (Released) {
				TagBeingDragged = "";
				TagDragOffsetFromCenter.erase(sObjectId);
			}
			else {
				TagBeingDragged = sObjectId;
			}

			RequestRefresh();
		}
		else if (Logger::is_verbose_mode())
		{
			Logger::info(
				"OnMoveScreenObject: skipped tag move update callsign=" +
				std::string(sObjectId != nullptr ? sObjectId : "<null>") +
				" target_valid=" + std::string(rt.IsValid() ? "1" : "0"));
		}
	}

	if (ObjectType == RIMCAS_IAW) {
		TimePopupAreas[sObjectId] = Area;

		if (!Released)
		{
			if (standardCursor)
			{
				smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRMOVEWINDOW), IMAGE_CURSOR, 0, 0, LR_SHARED));

				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = false;
			}
		}
		else
		{
			if (!standardCursor)
			{
				if (customCursor) {
					smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));					
				}
				else {
					smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
				}

				AFX_MANAGE_STATE(AfxGetStaticModuleState());
				ASSERT(smrCursor);
				SetCursor(smrCursor);
				standardCursor = true;
			}
		}
	}

	mouseLocation = Pt;
	RequestRefresh();

}

void CSMRRadar::OnOverScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area)
{
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;
	RequestRefresh();
}

void CSMRRadar::OnClickScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button)
{
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;

	if (ObjectType == APPWINDOW_ONE || ObjectType == APPWINDOW_TWO) {
		int appWindowId = ObjectType - APPWINDOW_BASE;
		
		if (strcmp(sObjectId, "close") == 0)
			appWindowDisplays[appWindowId] = false;
		if (strcmp(sObjectId, "range") == 0) {
			GetPlugIn()->OpenPopupList(Area, "SRW Zoom", 1);
			GetPlugIn()->AddPopupListElement("55", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 55));
			GetPlugIn()->AddPopupListElement("50", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 50));
			GetPlugIn()->AddPopupListElement("45", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 45));
			GetPlugIn()->AddPopupListElement("40", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 40));
			GetPlugIn()->AddPopupListElement("35", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 35));
			GetPlugIn()->AddPopupListElement("30", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 30));
			GetPlugIn()->AddPopupListElement("25", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 25));
			GetPlugIn()->AddPopupListElement("20", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 20));
			GetPlugIn()->AddPopupListElement("15", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 15));
			GetPlugIn()->AddPopupListElement("10", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 10));
			GetPlugIn()->AddPopupListElement("5", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 5));
			GetPlugIn()->AddPopupListElement("1", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindows[appWindowId]->m_Scale == 1));
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}
		if (strcmp(sObjectId, "filter") == 0) {
			GetPlugIn()->OpenPopupList(Area, "SRW Filter (ft)", 1);
			GetPlugIn()->AddPopupListElement("UNL", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 66000));
			GetPlugIn()->AddPopupListElement("9500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 9500));
			GetPlugIn()->AddPopupListElement("8500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 8500));
			GetPlugIn()->AddPopupListElement("7500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 7500));
			GetPlugIn()->AddPopupListElement("6500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 6500));
			GetPlugIn()->AddPopupListElement("5500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 5500));
			GetPlugIn()->AddPopupListElement("4500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 4500));
			GetPlugIn()->AddPopupListElement("3500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 3500));
			GetPlugIn()->AddPopupListElement("2500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 2500));
			GetPlugIn()->AddPopupListElement("1500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 1500));
			GetPlugIn()->AddPopupListElement("500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindows[appWindowId]->m_Filter == 500));
			string tmp = std::to_string(GetPlugIn()->GetTransitionAltitude());
			GetPlugIn()->AddPopupListElement(tmp.c_str(), "", RIMCAS_UPDATEFILTER + appWindowId, false, 2, false, true);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}
		if (strcmp(sObjectId, "rotate") == 0) {
			GetPlugIn()->OpenPopupList(Area, "SRW Rotate (deg)", 1);
			for (int k = 0; k <= 360; k++)
			{
				string tmp = std::to_string(k);
				GetPlugIn()->AddPopupListElement(tmp.c_str(), "", RIMCAS_UPDATEROTATE + appWindowId, false, int(appWindows[appWindowId]->m_Rotation == k));
			}
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}
	}

	if (ObjectType == RIMCAS_ACTIVE_AIRPORT) {
		GetPlugIn()->OpenPopupEdit(Area, RIMCAS_ACTIVE_AIRPORT_FUNC, getActiveAirport().c_str());
	}

	if (ObjectType == RIMCAS_ACTIVE_PROFILE) {
		if (Button == BUTTON_LEFT)
		{
			Area.top += 30;
			Area.bottom += 30;
			ShowLists["Profiles"] = true;
			ListAreas["Profiles"] = Area;
			RequestRefresh();
		}
		else if (Button == BUTTON_RIGHT)
		{
			OpenProfileEditorWindow();
		}
	}

	if (ObjectType == DRAWING_BACKGROUND_CLICK)
	{
		if (QDMSelectEnabled)
		{
			if (Button == BUTTON_LEFT)
			{
				QDMSelectPt = Pt;
				RequestRefresh();
			}

			if (Button == BUTTON_RIGHT)
			{
				QDMSelectEnabled = false;
				RequestRefresh();
			}
		}

		if (QDMenabled)
		{
			if (Button == BUTTON_RIGHT)
			{
				QDMenabled = false;
				RequestRefresh();
			}
		}
	}

	if (ObjectType == RIMCAS_MENU) {

		if (strcmp(sObjectId, "DisplayMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Display Menu", 1);
			GetPlugIn()->AddPopupListElement("QDR Fixed Reference", "", RIMCAS_QDM_TOGGLE);
			GetPlugIn()->AddPopupListElement("QDR Select Reference", "", RIMCAS_QDM_SELECT_TOGGLE);
			GetPlugIn()->AddPopupListElement("SRW 1", "", APPWINDOW_ONE, false, int(appWindowDisplays[1]));
			GetPlugIn()->AddPopupListElement("SRW 2", "", APPWINDOW_TWO, false, int(appWindowDisplays[2]));
			GetPlugIn()->AddPopupListElement("Profiles", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Profile Editor", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "TargetMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Target", 1);
			GetPlugIn()->AddPopupListElement("Label Font Size", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Tag Font", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Afterglow", "", RIMCAS_UPDATE_AFTERGLOW, false, int(Afterglow));
			GetPlugIn()->AddPopupListElement("GRND Trail Dots", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("APPR Trail Dots", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Predicted Track Line", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Acquire", "", RIMCAS_UPDATE_ACQUIRE);
			GetPlugIn()->AddPopupListElement("Release", "", RIMCAS_UPDATE_RELEASE);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "DefinitionMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Definitions", 1);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "MapMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Maps", 1);
			GetPlugIn()->AddPopupListElement("Airport Maps", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Custom Maps", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "ColourMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Colours", 1);
			GetPlugIn()->AddPopupListElement("Colour Settings", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Brightness", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "RIMCASMenu") == 0) {
			Area.top = Area.top + 30;
			Area.bottom = Area.bottom + 30;

			GetPlugIn()->OpenPopupList(Area, "Alerts", 1);
			GetPlugIn()->AddPopupListElement("Conflict Alert ARR", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Conflict Alert DEP", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Runway closed", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Visibility", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Active Alerts", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		}

		if (strcmp(sObjectId, "/") == 0)
		{
			if (Button == BUTTON_LEFT)
			{
				DistanceToolActive = !DistanceToolActive;
				if (!DistanceToolActive)
					ActiveDistance = pair<string, string>("", "");

				if (DistanceToolActive)
				{
					QDMenabled = false;
					QDMSelectEnabled = false;
				}
			}
			if (Button == BUTTON_RIGHT)
			{
				DistanceToolActive = false;
				ActiveDistance = pair<string, string>("", "");
				DistanceTools.clear();
			}

		}

	}

	if (ObjectType == DRAWING_TAG || ObjectType == DRAWING_AC_SYMBOL) {		
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		if (!rt.IsValid())
			return;

		const char* rtCallsign = rt.GetCallsign();
		const bool hasCallsign = (rtCallsign != nullptr && rtCallsign[0] != '\0');
		//GetPlugIn()->SetASELAircraft(rt); // NOTE: This does NOT work eventhough the api says it should?
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));  // make sure the correct aircraft is selected before calling 'StartTagFunction'
		
		if (rt.GetCorrelatedFlightPlan().IsValid() && hasCallsign) {
			StartTagFunction(rtCallsign, NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rtCallsign, NULL, EuroScopePlugIn::TAG_ITEM_FUNCTION_NO, Pt, Area);
		}		

		// Release & correlate actions

		if (ReleaseInProgress || AcquireInProgress)
		{
			const char* systemIdRaw = rt.GetSystemID();
			if (systemIdRaw == nullptr || systemIdRaw[0] == '\0')
			{
				ReleaseInProgress = NeedCorrelateCursor = false;
				AcquireInProgress = NeedCorrelateCursor = false;
				CorrelateCursor();
				return;
			}

			const std::string systemId = systemIdRaw;
			if (ReleaseInProgress)
			{
				ReleaseInProgress = NeedCorrelateCursor = false;

				ReleasedTracks.push_back(systemId);

				if (std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), systemId) != ManuallyCorrelated.end())
					ManuallyCorrelated.erase(std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), systemId));
			}

			if (AcquireInProgress)
			{
				AcquireInProgress = NeedCorrelateCursor = false;

				ManuallyCorrelated.push_back(systemId);

				if (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId) != ReleasedTracks.end())
					ReleasedTracks.erase(std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId));
			}


			CorrelateCursor();

			return;
		}

		if (ObjectType == DRAWING_AC_SYMBOL)
		{
			if (QDMSelectEnabled)
			{
				if (Button == BUTTON_LEFT)
				{
					QDMSelectPt = Pt;
					RequestRefresh();
				}
			}
			else if (DistanceToolActive) {
				if (ActiveDistance.first == "")
				{
					ActiveDistance.first = sObjectId;
				}
				else if (ActiveDistance.second == "")
				{
					ActiveDistance.second = sObjectId;
					DistanceTools.insert(ActiveDistance);
					ActiveDistance = pair<string, string>("", "");
					DistanceToolActive = false;
				}
				RequestRefresh();
			}
			else
			{
				if (TagsOffsets.find(sObjectId) != TagsOffsets.end())
					TagsOffsets.erase(sObjectId);

				if (Button == BUTTON_LEFT)
				{
					if (TagAngles.find(sObjectId) == TagAngles.end())
					{
						TagAngles[sObjectId] = 0;
					} else
					{
						TagAngles[sObjectId] = fmod(TagAngles[sObjectId] - 22.5, 360);
					}
				}

				if (Button == BUTTON_RIGHT)
				{
					if (TagAngles.find(sObjectId) == TagAngles.end())
					{
						TagAngles[sObjectId] = 0;
					}
					else
					{
						TagAngles[sObjectId] = fmod(TagAngles[sObjectId] + 22.5, 360);
					}
				}

				RequestRefresh();
			}
		}
	}

	if (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW1 || ObjectType == DRAWING_AC_SYMBOL_APPWINDOW2)
	{
		if (DistanceToolActive) {
			if (ActiveDistance.first == "")
			{
				ActiveDistance.first = sObjectId;
			}
			else if (ActiveDistance.second == "")
			{
				ActiveDistance.second = sObjectId;
				DistanceTools.insert(ActiveDistance);
				ActiveDistance = pair<string, string>("", "");
				DistanceToolActive = false;
			}
			RequestRefresh();
		} else
		{
			if (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW1)
				appWindows[1]->OnClickScreenObject(sObjectId, Pt, Button);

			if (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW2)
				appWindows[2]->OnClickScreenObject(sObjectId, Pt, Button);
		}
	}

	map <const int, const int> TagObjectMiddleTypes = {
		{ TAG_CITEM_CALLSIGN, TAG_ITEM_FUNCTION_COMMUNICATION_POPUP },
	};

	map <const int, const int> TagObjectRightTypes = {
		{ TAG_CITEM_CALLSIGN, TAG_ITEM_FUNCTION_HANDOFF_POPUP_MENU },
		{ TAG_CITEM_FPBOX, TAG_ITEM_FUNCTION_OPEN_FP_DIALOG },
		{ TAG_CITEM_RWY, TAG_ITEM_FUNCTION_ASSIGNED_RUNWAY },
		{ TAG_CITEM_SID, TAG_ITEM_FUNCTION_ASSIGNED_SID },
		{ TAG_CITEM_GATE, TAG_ITEM_FUNCTION_EDIT_SCRATCH_PAD },
		{ TAG_CITEM_GROUNDSTATUS, TAG_ITEM_FUNCTION_SET_GROUND_STATUS },
		{ TAG_CITEM_CLEARANCE, TAG_ITEM_FUNCTION_SET_CLEARED_FLAG },
		{ TAG_CITEM_UKSTAND, 999999},
		{ TAG_CITEM_SCRATCHPAD, TAG_ITEM_FUNCTION_EDIT_SCRATCH_PAD },
	};

	if (Button == BUTTON_LEFT) {
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		const char* rtCallsign = rt.IsValid() ? rt.GetCallsign() : nullptr;
		const bool hasCallsign = (rtCallsign != nullptr && rtCallsign[0] != '\0');
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		if (rt.IsValid() && rt.GetCorrelatedFlightPlan().IsValid() && hasCallsign) {
			if (ObjectType == TAG_CITEM_CALLSIGN) {
				// Shortcut: open ground status popup (clearance/push/taxi/depa) on callsign left-click.
				StartTagFunction(rtCallsign, NULL, TAG_ITEM_TYPE_CALLSIGN, rtCallsign, NULL, TAG_ITEM_FUNCTION_SET_GROUND_STATUS, Pt, Area);
			}
			else if (ObjectType == TAG_CITEM_CLEARANCE) {
				StartTagFunction(rtCallsign, NULL, TAG_ITEM_TYPE_CLEARENCE, rtCallsign, NULL, TAG_ITEM_FUNCTION_SET_CLEARED_FLAG, Pt, Area);
			}
			else {
				StartTagFunction(rtCallsign, NULL, TAG_ITEM_TYPE_CALLSIGN, rtCallsign, NULL, TAG_ITEM_FUNCTION_NO, Pt, Area);
			}
		}
	}

	auto middleTypeIt = TagObjectMiddleTypes.find(ObjectType);
	if (Button == BUTTON_MIDDLE && middleTypeIt != TagObjectMiddleTypes.end() && middleTypeIt->second != 0) {
		int TagMenu = middleTypeIt->second;
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		const char* rtCallsign = rt.IsValid() ? rt.GetCallsign() : nullptr;
		if (rtCallsign != nullptr && rtCallsign[0] != '\0')
			StartTagFunction(rtCallsign, NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rtCallsign, NULL, TagMenu, Pt, Area);
	}

	auto rightTypeIt = TagObjectRightTypes.find(ObjectType);
	if (Button == BUTTON_RIGHT && rightTypeIt != TagObjectRightTypes.end() && rightTypeIt->second != 0) {
		if (ObjectType == TAG_CITEM_UKSTAND) {
			CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
			GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
			const char* rtCallsign = rt.IsValid() ? rt.GetCallsign() : nullptr;
			if (rtCallsign != nullptr && rtCallsign[0] != '\0')
				StartTagFunction(rtCallsign, NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rtCallsign, "RampAgent", 0, Pt, Area);
		}
		else {
		int TagMenu = rightTypeIt->second;
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		const char* rtCallsign = rt.IsValid() ? rt.GetCallsign() : nullptr;
		if (rtCallsign != nullptr && rtCallsign[0] != '\0')
			StartTagFunction(rtCallsign, NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rtCallsign, NULL, TagMenu, Pt, Area);
	}
	}

	if (ObjectType == RIMCAS_DISTANCE_TOOL)
	{
		vector<string> s = split(sObjectId, ',');
		pair<string, string> toRemove = pair<string, string>(s.front(), s.back());

		typedef multimap<string, string>::iterator iterator;
		std::pair<iterator, iterator> iterpair = DistanceTools.equal_range(toRemove.first);

		iterator it = iterpair.first;
		for (; it != iterpair.second; ++it) {
			if (it->second == toRemove.second) {
				it = DistanceTools.erase(it);
				break;
			}
		}

	}

	RequestRefresh();
};

