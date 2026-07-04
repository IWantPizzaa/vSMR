#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern HCURSOR smrCursor;
extern bool standardCursor;
extern bool customCursor;

void CSMRRadar::OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released) {
	Logger::info(string(__FUNCSIG__));
	const bool hasObjectId = (sObjectId != nullptr && sObjectId[0] != '\0');
	const char* objectId = hasObjectId ? sObjectId : "";
	auto isObjectId = [&](const char* expected) -> bool
	{
		return expected != nullptr && strcmp(objectId, expected) == 0;
	};
	auto isAppWindowObjectType = [](int objectType) -> bool
	{
		return objectType > APPWINDOW_BASE && objectType <= APPWINDOW_AVISO;
	};
	auto setCursorState = [&](HCURSOR cursor, bool keepStandardCursor)
	{
		AFX_MANAGE_STATE(AfxGetStaticModuleState());
		ASSERT(cursor);
		SetCursor(cursor);
		standardCursor = keepStandardCursor;
	};
	auto setInteractionCursorIfNeeded = [&](int resourceId)
	{
		if (!standardCursor)
			return;
		HCURSOR cursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(resourceId), IMAGE_CURSOR, 0, 0, LR_SHARED));
		setCursorState(cursor, false);
	};
	auto setDefaultCursorIfNeeded = [&]()
	{
		if (standardCursor)
			return;
		HCURSOR cursor = customCursor
			? CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED))
			: (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
		setCursorState(cursor, true);
	};
	auto isTagObjectType = [&](int objectType) -> bool
	{
		switch (objectType)
		{
		case DRAWING_TAG:
		case TAG_CITEM_MANUALCORRELATE:
		case TAG_CITEM_CALLSIGN:
		case TAG_CITEM_FPBOX:
		case TAG_CITEM_RWY:
		case TAG_CITEM_SID:
		case TAG_CITEM_GATE:
		case TAG_CITEM_NO:
		case TAG_CITEM_GROUNDSTATUS:
		case TAG_CITEM_CLEARANCE:
		case TAG_CITEM_UKSTAND:
		case TAG_CITEM_REMARK:
		case TAG_CITEM_SCRATCHPAD:
			return true;
		default:
			return false;
		}
	};

	if (isAppWindowObjectType(ObjectType)) {
		int appWindowId = ObjectType - APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			return;
		CInsetWindow* appWindow = appWindowIt->second.get();

		bool toggleCursor = appWindow->OnMoveScreenObject(sObjectId, Pt, Area, Released);

		if (!toggleCursor)
		{
			if (isObjectId("topbar"))
				setInteractionCursorIfNeeded(IDC_SMRMOVEWINDOW);
			else if (isObjectId("resize"))
				setInteractionCursorIfNeeded(IDC_SMRRESIZE);
		}
		else
		{
			setDefaultCursorIfNeeded();
		}
	}

	if (isTagObjectType(ObjectType)) {
		auto routeMoveToInsetWindow = [&]() -> bool
		{
			if (!hasObjectId)
				return false;

			for (auto& kv : appWindows)
			{
				CInsetWindow* insetWindow = kv.second.get();
				if (insetWindow == nullptr)
					continue;

				const bool draggingThisWindowTag =
					!insetWindow->m_TagBeingDragged.empty() &&
					insetWindow->m_TagBeingDragged == objectId;
				auto insetTagAreaIt = insetWindow->m_TagAreas.find(objectId);
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

				insetWindow->OnMoveScreenObject(objectId, Pt, Area, Released);
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
		if (!hasObjectId)
		{
			if (Logger::is_verbose_mode())
				Logger::info("OnMoveScreenObject: missing tag object id");
			mouseLocation = Pt;
			RequestRefresh();
			return;
		}

		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(objectId);

		if (!Released)
		{
			setInteractionCursorIfNeeded(IDC_SMRMOVETAG);
		}
		else
		{
			setDefaultCursorIfNeeded();
		}

		if (rt.IsValid() && rt.GetPosition().IsValid()) {
			POINT TagCenterPix;

			// First frame of drag: capture offset between tag center and grab point.
			bool firstDragFrame = (!Released && TagBeingDragged != objectId);
			if (firstDragFrame) {
				POINT fullCenter{};
				auto fullRectIt = tagAreas.find(objectId);
				if (fullRectIt != tagAreas.end()) {
					fullCenter = fullRectIt->second.CenterPoint();
				}
				else {
					CRect tmp = Area;
					fullCenter = tmp.CenterPoint();
				}
				POINT offset = { fullCenter.x - Pt.x, fullCenter.y - Pt.y };
				TagDragOffsetFromCenter[objectId] = offset;
			}

			// Always apply stored offset if available (even on release) to avoid snap.
			auto offIt = TagDragOffsetFromCenter.find(objectId);
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

			
			TagsOffsets[objectId] = CustomTag;
			TagAngles[objectId] = fmod(atan2(double(CustomTag.y), double(CustomTag.x)) * 180.0 / PI, 360);
			TagLeaderLineLength[objectId] = static_cast<int>(sqrt(double(CustomTag.x * CustomTag.x + CustomTag.y * CustomTag.y)));

			GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(objectId));

			if (Released) {
				TagBeingDragged = "";
				TagDragOffsetFromCenter.erase(objectId);
			}
			else {
				TagBeingDragged = objectId;
			}

			RequestRefresh();
		}
		else if (Logger::is_verbose_mode())
		{
			Logger::info(
				"OnMoveScreenObject: skipped tag move update callsign=" +
				std::string(objectId) +
				" target_valid=" + std::string(rt.IsValid() ? "1" : "0"));
		}
	}

	if (ObjectType == RIMCAS_IAW) {
		if (hasObjectId)
			TimePopupAreas[objectId] = Area;

		if (!Released)
		{
			setInteractionCursorIfNeeded(IDC_SMRMOVEWINDOW);
		}
		else
		{
			setDefaultCursorIfNeeded();
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
	const bool hasObjectId = (sObjectId != nullptr && sObjectId[0] != '\0');
	const char* objectId = hasObjectId ? sObjectId : "";
	auto isObjectId = [&](const char* expected) -> bool
	{
		return expected != nullptr && strcmp(objectId, expected) == 0;
	};
	auto isAppWindowObjectType = [](int objectType) -> bool
	{
		return objectType > APPWINDOW_BASE && objectType <= APPWINDOW_AVISO;
	};
	auto shiftPopupAreaDown = [&](int pixels)
	{
		Area.top += pixels;
		Area.bottom += pixels;
	};
	auto addPopupCloseItem = [&]()
	{
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
	};
	auto openPopupListWithClose = [&](const char* title, const auto& addItems)
	{
		GetPlugIn()->OpenPopupList(Area, title, 1);
		addItems();
		addPopupCloseItem();
	};
	auto selectDistanceToolTarget = [&](const char* targetId) -> bool
	{
		if (!DistanceToolActive || targetId == nullptr || targetId[0] == '\0')
			return false;

		if (ActiveDistance.first.empty())
		{
			ActiveDistance.first = targetId;
		}
		else if (ActiveDistance.second.empty())
		{
			ActiveDistance.second = targetId;
			DistanceTools.insert(ActiveDistance);
			ActiveDistance = pair<string, string>("", "");
			DistanceToolActive = false;
		}

		RequestRefresh();
		return true;
	};
	auto selectAseAndGetTarget = [&]() -> CRadarTarget
	{
		if (!hasObjectId)
			return CRadarTarget();

		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(objectId);
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(objectId));
		return rt;
	};
	auto startTagFunctionForObject = [&](int tagItemType, int tagMenu, const char* menuContext, bool requireCorrelatedFlightPlan) -> bool
	{
		CRadarTarget rt = selectAseAndGetTarget();
		const char* rtCallsign = rt.IsValid() ? rt.GetCallsign() : nullptr;
		const bool hasCallsign = (rtCallsign != nullptr && rtCallsign[0] != '\0');
		if (!hasCallsign)
			return false;
		if (requireCorrelatedFlightPlan && !rt.GetCorrelatedFlightPlan().IsValid())
			return false;

		StartTagFunction(rtCallsign, NULL, tagItemType, rtCallsign, menuContext, tagMenu, Pt, Area);
		return true;
	};
	auto getMiddleTagMenu = [&](int objectType) -> int
	{
		switch (objectType)
		{
		case TAG_CITEM_CALLSIGN:
			return TAG_ITEM_FUNCTION_COMMUNICATION_POPUP;
		default:
			return 0;
		}
	};
	auto getRightTagMenu = [&](int objectType) -> int
	{
		switch (objectType)
		{
		case TAG_CITEM_CALLSIGN:
			return TAG_ITEM_FUNCTION_HANDOFF_POPUP_MENU;
		case TAG_CITEM_FPBOX:
			return TAG_ITEM_FUNCTION_OPEN_FP_DIALOG;
		case TAG_CITEM_RWY:
			return TAG_ITEM_FUNCTION_ASSIGNED_RUNWAY;
		case TAG_CITEM_SID:
			return TAG_ITEM_FUNCTION_ASSIGNED_SID;
		case TAG_CITEM_GATE:
			return TAG_ITEM_FUNCTION_EDIT_SCRATCH_PAD;
		case TAG_CITEM_GROUNDSTATUS:
			return TAG_ITEM_FUNCTION_SET_GROUND_STATUS;
		case TAG_CITEM_CLEARANCE:
			return TAG_ITEM_FUNCTION_SET_CLEARED_FLAG;
		case TAG_CITEM_UKSTAND:
			return 999999;
		case TAG_CITEM_SCRATCHPAD:
			return TAG_ITEM_FUNCTION_EDIT_SCRATCH_PAD;
		default:
			return 0;
		}
	};
	auto removeDistanceToolPair = [&](const char* distanceToolId)
	{
		if (distanceToolId == nullptr || distanceToolId[0] == '\0')
			return;

		vector<string> parts = split(distanceToolId, ',');
		if (parts.size() < 2)
			return;

		pair<string, string> toRemove(parts.front(), parts.back());
		typedef multimap<string, string>::iterator iterator;
		std::pair<iterator, iterator> iterpair = DistanceTools.equal_range(toRemove.first);
		iterator it = iterpair.first;
		for (; it != iterpair.second; ++it)
		{
			if (it->second == toRemove.second)
			{
				DistanceTools.erase(it);
				break;
			}
		}
	};

	if (isAppWindowObjectType(ObjectType)) {
		int appWindowId = ObjectType - APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		CInsetWindow* appWindow = (appWindowIt != appWindows.end() && appWindowIt->second != nullptr) ? appWindowIt->second.get() : nullptr;
		
		if (isObjectId("close"))
		{
			auto appWindowDisplayIt = appWindowDisplays.find(appWindowId);
			if (appWindowDisplayIt != appWindowDisplays.end())
				appWindowDisplayIt->second = false;
		}
		if (isObjectId("range")) {
			if (appWindow == nullptr)
				return;
			if (appWindow->IsAvisoViewport())
			{
				openPopupListWithClose("AVISO Zoom", [&]()
				{
					const int values[] = { 1200, 900, 700, 500, 350, 250, 150, 100, 60, 30 };
					for (int value : values)
					{
						const string label = std::to_string(value);
						GetPlugIn()->AddPopupListElement(label.c_str(), "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_AvisoScale == value));
					}
				});
				return;
			}
			openPopupListWithClose("SRW Zoom", [&]()
			{
				GetPlugIn()->AddPopupListElement("55", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 55));
				GetPlugIn()->AddPopupListElement("50", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 50));
				GetPlugIn()->AddPopupListElement("45", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 45));
				GetPlugIn()->AddPopupListElement("40", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 40));
				GetPlugIn()->AddPopupListElement("35", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 35));
				GetPlugIn()->AddPopupListElement("30", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 30));
				GetPlugIn()->AddPopupListElement("25", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 25));
				GetPlugIn()->AddPopupListElement("20", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 20));
				GetPlugIn()->AddPopupListElement("15", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 15));
				GetPlugIn()->AddPopupListElement("10", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 10));
				GetPlugIn()->AddPopupListElement("5", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 5));
				GetPlugIn()->AddPopupListElement("1", "", RIMCAS_UPDATERANGE + appWindowId, false, int(appWindow->m_Scale == 1));
			});
		}
		if (isObjectId("filter")) {
			if (appWindow == nullptr)
				return;
			if (appWindow->IsAvisoViewport())
				return;
			openPopupListWithClose("SRW Filter (ft)", [&]()
			{
				GetPlugIn()->AddPopupListElement("UNL", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 66000));
				GetPlugIn()->AddPopupListElement("9500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 9500));
				GetPlugIn()->AddPopupListElement("8500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 8500));
				GetPlugIn()->AddPopupListElement("7500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 7500));
				GetPlugIn()->AddPopupListElement("6500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 6500));
				GetPlugIn()->AddPopupListElement("5500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 5500));
				GetPlugIn()->AddPopupListElement("4500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 4500));
				GetPlugIn()->AddPopupListElement("3500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 3500));
				GetPlugIn()->AddPopupListElement("2500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 2500));
				GetPlugIn()->AddPopupListElement("1500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 1500));
				GetPlugIn()->AddPopupListElement("500", "", RIMCAS_UPDATEFILTER + appWindowId, false, int(appWindow->m_Filter == 500));
				string tmp = std::to_string(GetPlugIn()->GetTransitionAltitude());
				GetPlugIn()->AddPopupListElement(tmp.c_str(), "", RIMCAS_UPDATEFILTER + appWindowId, false, 2, false, true);
			});
		}
		if (isObjectId("rotate")) {
			if (appWindow == nullptr)
				return;
			if (appWindow->IsAvisoViewport())
				return;
			openPopupListWithClose("SRW Rotate (deg)", [&]()
			{
				for (int k = 0; k <= 360; k++)
				{
					string tmp = std::to_string(k);
					GetPlugIn()->AddPopupListElement(tmp.c_str(), "", RIMCAS_UPDATEROTATE + appWindowId, false, int(appWindow->m_Rotation == k));
				}
			});
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

		if (isObjectId("DisplayMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Display Menu", [&]()
			{
				GetPlugIn()->AddPopupListElement("QDR Fixed Reference", "", RIMCAS_QDM_TOGGLE);
				GetPlugIn()->AddPopupListElement("QDR Select Reference", "", RIMCAS_QDM_SELECT_TOGGLE);
				const auto appWindowOneDisplayIt = appWindowDisplays.find(1);
				const bool appWindowOneVisible = (appWindowOneDisplayIt != appWindowDisplays.end()) && appWindowOneDisplayIt->second;
				const auto appWindowTwoDisplayIt = appWindowDisplays.find(2);
				const bool appWindowTwoVisible = (appWindowTwoDisplayIt != appWindowDisplays.end()) && appWindowTwoDisplayIt->second;
				const auto avisoWindowDisplayIt = appWindowDisplays.find(APPWINDOW_AVISO - APPWINDOW_BASE);
				const bool avisoWindowVisible = (avisoWindowDisplayIt != appWindowDisplays.end()) && avisoWindowDisplayIt->second;
				GetPlugIn()->AddPopupListElement("SRW 1", "", APPWINDOW_ONE, false, int(appWindowOneVisible));
				GetPlugIn()->AddPopupListElement("SRW 2", "", APPWINDOW_TWO, false, int(appWindowTwoVisible));
				GetPlugIn()->AddPopupListElement("AVISO View", "", APPWINDOW_AVISO, false, int(avisoWindowVisible));
				const std::string activeProfileName = GetActiveProfileNameForEditor();
				bool proModeEnabled = false;
				bool towerModeEnabled = false;
				if (!activeProfileName.empty())
				{
					GetProfileProModeEnabledForEditor(activeProfileName, proModeEnabled);
					GetProfileTowerModeEnabledForEditor(activeProfileName, towerModeEnabled);
				}
				GetPlugIn()->AddPopupListElement("Pro mode", "", RIMCAS_TOGGLE_PRO_MODE, false, int(proModeEnabled));
				GetPlugIn()->AddPopupListElement("Tower mode", "", RIMCAS_TOGGLE_TOWER_MODE, false, int(towerModeEnabled));
				GetPlugIn()->AddPopupListElement("Profiles", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Profile Editor", "", RIMCAS_OPEN_LIST);
			});
		}

		if (isObjectId("TargetMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Target", [&]()
			{
				GetPlugIn()->AddPopupListElement("Label Font Size", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Tag Font", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Afterglow", "", RIMCAS_UPDATE_AFTERGLOW, false, int(Afterglow));
				GetPlugIn()->AddPopupListElement("GRND Trail Dots", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("APPR Trail Dots", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Predicted Track Line", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Acquire", "", RIMCAS_UPDATE_ACQUIRE);
				GetPlugIn()->AddPopupListElement("Release", "", RIMCAS_UPDATE_RELEASE);
			});
		}

		if (isObjectId("DefinitionMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Definitions", [&]() {});
		}

		if (isObjectId("MapMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Maps", [&]()
			{
				GetPlugIn()->AddPopupListElement("Airport Maps", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Custom Maps", "", RIMCAS_OPEN_LIST);
			});
		}

		if (isObjectId("ColourMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Colours", [&]()
			{
				GetPlugIn()->AddPopupListElement("Colour Settings", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Brightness", "", RIMCAS_OPEN_LIST);
			});
		}

		if (isObjectId("RIMCASMenu")) {
			shiftPopupAreaDown(30);
			openPopupListWithClose("Alerts", [&]()
			{
				GetPlugIn()->AddPopupListElement("Conflict Alert ARR", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Conflict Alert DEP", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Runway closed", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Visibility", "", RIMCAS_OPEN_LIST);
				GetPlugIn()->AddPopupListElement("Active Alerts", "", RIMCAS_OPEN_LIST);
			});
		}

		if (isObjectId("/"))
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
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(objectId);
		if (!rt.IsValid())
			return;

		const char* rtCallsign = rt.GetCallsign();
		const bool hasCallsign = (rtCallsign != nullptr && rtCallsign[0] != '\0');
		//GetPlugIn()->SetASELAircraft(rt); // NOTE: This does NOT work eventhough the api says it should?
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(objectId));  // make sure the correct aircraft is selected before calling 'StartTagFunction'
		
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

				ReleasedTracks.insert(systemId);
				ManuallyCorrelated.erase(systemId);
			}

			if (AcquireInProgress)
			{
				AcquireInProgress = NeedCorrelateCursor = false;

				ManuallyCorrelated.insert(systemId);
				ReleasedTracks.erase(systemId);
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
			else if (selectDistanceToolTarget(objectId)) {
			}
			else
			{
				if (TagsOffsets.find(objectId) != TagsOffsets.end())
					TagsOffsets.erase(objectId);

				if (Button == BUTTON_LEFT)
				{
					if (TagAngles.find(objectId) == TagAngles.end())
					{
						TagAngles[objectId] = 0;
					} else
					{
						TagAngles[objectId] = fmod(TagAngles[objectId] - 22.5, 360);
					}
				}

				if (Button == BUTTON_RIGHT)
				{
					if (TagAngles.find(objectId) == TagAngles.end())
					{
						TagAngles[objectId] = 0;
					}
					else
					{
						TagAngles[objectId] = fmod(TagAngles[objectId] + 22.5, 360);
					}
				}

				RequestRefresh();
			}
		}
	}

	if (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW1 || ObjectType == DRAWING_AC_SYMBOL_APPWINDOW2)
	{
		if (selectDistanceToolTarget(objectId)) {
		} else
		{
			const int appWindowId = (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW1) ? 1 : 2;
			auto appWindowIt = appWindows.find(appWindowId);
			if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr)
				appWindowIt->second->OnClickScreenObject(objectId, Pt, Button);
		}
	}

	if (Button == BUTTON_LEFT) {
		if (ObjectType == TAG_CITEM_CALLSIGN) {
				// Shortcut: open ground status popup (clearance/push/taxi/depa) on callsign left-click.
			(void)startTagFunctionForObject(TAG_ITEM_TYPE_CALLSIGN, TAG_ITEM_FUNCTION_SET_GROUND_STATUS, NULL, true);
		}
		else if (ObjectType == TAG_CITEM_CLEARANCE) {
			(void)startTagFunctionForObject(TAG_ITEM_TYPE_CLEARENCE, TAG_ITEM_FUNCTION_SET_CLEARED_FLAG, NULL, true);
		}
		else {
			(void)startTagFunctionForObject(TAG_ITEM_TYPE_CALLSIGN, TAG_ITEM_FUNCTION_NO, NULL, true);
		}
	}

	const int middleTagMenu = getMiddleTagMenu(ObjectType);
	if (Button == BUTTON_MIDDLE && middleTagMenu != 0) {
		(void)startTagFunctionForObject(TAG_ITEM_TYPE_CALLSIGN, middleTagMenu, NULL, false);
	}

	const int rightTagMenu = getRightTagMenu(ObjectType);
	if (Button == BUTTON_RIGHT && rightTagMenu != 0) {
		if (ObjectType == TAG_CITEM_UKSTAND) {
			(void)startTagFunctionForObject(TAG_ITEM_TYPE_CALLSIGN, 0, "RampAgent", false);
		}
		else {
			(void)startTagFunctionForObject(TAG_ITEM_TYPE_CALLSIGN, rightTagMenu, NULL, false);
		}
	}

	if (ObjectType == RIMCAS_DISTANCE_TOOL)
	{
		removeDistanceToolPair(objectId);
	}

	RequestRefresh();
};

