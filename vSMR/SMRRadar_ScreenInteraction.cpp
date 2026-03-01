#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"

namespace
{
	double ClampDouble(double value, double minValue, double maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	int ClampInt(double value, int minValue, int maxValue)
	{
		int converted = static_cast<int>(value);
		if (converted < minValue) return minValue;
		if (converted > maxValue) return maxValue;
		return converted;
	}

	Gdiplus::Color HsvToColor(double h, double s, double v, int a = 255)
	{
		double hue = fmod(h, 360.0);
		if (hue < 0.0)
			hue += 360.0;
		double sat = ClampDouble(s, 0.0, 1.0);
		double val = ClampDouble(v, 0.0, 1.0);

		double c = val * sat;
		double x = c * (1.0 - fabs(fmod(hue / 60.0, 2.0) - 1.0));
		double m = val - c;

		double rf = 0.0, gf = 0.0, bf = 0.0;
		if (hue < 60.0) {
			rf = c; gf = x; bf = 0.0;
		}
		else if (hue < 120.0) {
			rf = x; gf = c; bf = 0.0;
		}
		else if (hue < 180.0) {
			rf = 0.0; gf = c; bf = x;
		}
		else if (hue < 240.0) {
			rf = 0.0; gf = x; bf = c;
		}
		else if (hue < 300.0) {
			rf = x; gf = 0.0; bf = c;
		}
		else {
			rf = c; gf = 0.0; bf = x;
		}

		int r = ClampInt((rf + m) * 255.0 + 0.5, 0, 255);
		int g = ClampInt((gf + m) * 255.0 + 0.5, 0, 255);
		int b = ClampInt((bf + m) * 255.0 + 0.5, 0, 255);
		int alpha = ClampInt(a, 0, 255);
		return Gdiplus::Color(alpha, r, g, b);
	}
}

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern HCURSOR smrCursor;
extern bool standardCursor;
extern bool customCursor;
extern map<int, CInsetWindow*> appWindows;

void CSMRRadar::OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released) {
	Logger::info(string(__FUNCSIG__));

	if (ObjectType == DRAWING_TAGDEF_EDITOR)
	{
		mouseLocation = Pt;
		RequestRefresh();
		return;
	}

	if (ObjectType == RIMCAS_PROFILE_COLOR_PICKER)
	{
		const std::string controlId = (sObjectId != nullptr) ? sObjectId : "";
		if (!Released)
		{
			if (controlId == "picker_wheel")
			{
				ProfileColorPickerDragWheel = true;
				UpdateProfileColorPickerFromPoint(controlId, Pt, false);
			}
			else if (controlId == "picker_value")
			{
				ProfileColorPickerDragValue = true;
				UpdateProfileColorPickerFromPoint(controlId, Pt, false);
			}
			else if (controlId == "picker_alpha")
			{
				ProfileColorPickerDragAlpha = true;
				UpdateProfileColorPickerFromPoint(controlId, Pt, false);
			}
		}
		else
		{
			if (ProfileColorPickerDragWheel || ProfileColorPickerDragValue || ProfileColorPickerDragAlpha)
			{
				UpdateProfileColorPickerFromPoint(controlId, Pt, true);
			}
			else if (ProfileColorPickerDirty)
			{
				ApplyProfileColorPicker(true);
			}

			ProfileColorPickerDragWheel = false;
			ProfileColorPickerDragValue = false;
			ProfileColorPickerDragAlpha = false;
		}

		mouseLocation = Pt;
		RequestRefresh();
		return;
	}

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

	if (ObjectType == DRAWING_TAG || ObjectType == TAG_CITEM_MANUALCORRELATE || ObjectType == TAG_CITEM_CALLSIGN || ObjectType == TAG_CITEM_FPBOX || ObjectType == TAG_CITEM_RWY || ObjectType == TAG_CITEM_SID || ObjectType == TAG_CITEM_GATE || ObjectType == TAG_CITEM_NO || ObjectType == TAG_CITEM_GROUNDSTATUS || ObjectType == TAG_CITEM_CLEARANCE || TAG_CITEM_UKSTAND || TAG_CITEM_REMARK || TAG_CITEM_SCRATCHPAD) {
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

		if (rt.IsValid()) {
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

			POINT AcPosPix = ConvertCoordFromPositionToPixel(GetPlugIn()->RadarTargetSelect(sObjectId).GetPosition().GetPosition());
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

	if (ObjectType == RIMCAS_PROFILE_COLOR_PICKER)
	{
		const std::string controlId = (sObjectId != nullptr) ? sObjectId : "";
		if (Button == BUTTON_LEFT)
		{
			if (controlId == "picker_close")
			{
				if (ProfileColorPickerDirty)
					ApplyProfileColorPicker(true);
				ShowProfileColorPicker = false;
				ProfileColorPickerDragWheel = false;
				ProfileColorPickerDragValue = false;
				ProfileColorPickerDragAlpha = false;
				RequestRefresh();
				return;
			}

			if (controlId == "picker_rgb_text")
			{
				Color rgb = HsvToColor(ProfileColorPickerHue, ProfileColorPickerSaturation, ProfileColorPickerValue, 255);
				string initial = std::to_string(rgb.GetR()) + "," + std::to_string(rgb.GetG()) + "," + std::to_string(rgb.GetB());
				GetPlugIn()->OpenPopupEdit(Area, RIMCAS_PROFILE_COLOR_EDIT_RGB, initial.c_str());
				return;
			}

			if (controlId == "picker_select_color")
			{
				if (ProfileColorPickerDirty)
					ApplyProfileColorPicker(true);

				RebuildProfileColorEntries();
				GetPlugIn()->OpenPopupList(Area, "Profile Colors", 1);
				for (const std::string& colorPath : ProfileColorPaths)
				{
					GetPlugIn()->AddPopupListElement(colorPath.c_str(), "", RIMCAS_PROFILE_COLOR_SELECT, false, int(colorPath == SelectedProfileColorPath));
				}
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId == "picker_alpha_text")
			{
				string initial = std::to_string(ProfileColorPickerAlpha);
				GetPlugIn()->OpenPopupEdit(Area, RIMCAS_PROFILE_COLOR_EDIT_ALPHA, initial.c_str());
				return;
			}

			if (controlId == "picker_hex_text")
			{
				Color rgb = HsvToColor(ProfileColorPickerHue, ProfileColorPickerSaturation, ProfileColorPickerValue, 255);
				const bool includeAlpha = ProfileColorPickerHasAlpha || ProfileColorPickerAlpha != 255;
				char hexBuffer[16] = { 0 };
				if (includeAlpha)
					sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X%02X", rgb.GetR(), rgb.GetG(), rgb.GetB(), ProfileColorPickerAlpha);
				else
					sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X", rgb.GetR(), rgb.GetG(), rgb.GetB());

				GetPlugIn()->OpenPopupEdit(Area, RIMCAS_PROFILE_COLOR_EDIT_HEX, hexBuffer);
				return;
			}

			if (controlId == "picker_wheel" || controlId == "picker_value" || controlId == "picker_alpha")
			{
				UpdateProfileColorPickerFromPoint(controlId, Pt, true);
				RequestRefresh();
				return;
			}
		}

		if (Button == BUTTON_RIGHT)
		{
			if (ProfileColorPickerDirty)
				ApplyProfileColorPicker(true);
			ShowProfileColorPicker = false;
			ProfileColorPickerDragWheel = false;
			ProfileColorPickerDragValue = false;
			ProfileColorPickerDragAlpha = false;
			RequestRefresh();
			return;
		}
	}

	if (ObjectType == DRAWING_TAGDEF_EDITOR)
	{
		const std::string controlId = (sObjectId != nullptr) ? sObjectId : "";
		if (Button == BUTTON_LEFT)
		{
			if (controlId == "tagdef_close")
			{
				ShowTagDefinitionEditor = false;
				RequestRefresh();
				return;
			}

			if (controlId == "tagdef_type")
			{
				GetPlugIn()->OpenPopupList(Area, "Tag Type", 1);
				GetPlugIn()->AddPopupListElement("Departure", "", RIMCAS_TAGDEF_SELECT_TYPE, false, int(TagDefinitionEditorType == "departure"));
				GetPlugIn()->AddPopupListElement("Arrival", "", RIMCAS_TAGDEF_SELECT_TYPE, false, int(TagDefinitionEditorType == "arrival"));
				GetPlugIn()->AddPopupListElement("Airborne", "", RIMCAS_TAGDEF_SELECT_TYPE, false, int(TagDefinitionEditorType == "airborne"));
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId == "tagdef_mode")
			{
				GetPlugIn()->OpenPopupList(Area, "Definition Mode", 1);
				GetPlugIn()->AddPopupListElement("Definition", "", RIMCAS_TAGDEF_SELECT_MODE, false, int(!TagDefinitionEditorDetailed));
				GetPlugIn()->AddPopupListElement("Definition Detailed", "", RIMCAS_TAGDEF_SELECT_MODE, false, int(TagDefinitionEditorDetailed));
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId == "tagdef_status")
			{
				const std::string normalizedType = NormalizeTagDefinitionType(TagDefinitionEditorType);
				GetPlugIn()->OpenPopupList(Area, "Tag Status", 1);
				for (const std::string& statusOption : GetTagDefinitionStatusesForType(normalizedType))
				{
					std::string statusLabel = TagDefinitionDepartureStatusLabel(statusOption);
					GetPlugIn()->AddPopupListElement(statusLabel.c_str(), "", RIMCAS_TAGDEF_SELECT_STATUS, false, int(TagDefinitionEditorDepartureStatus == statusOption));
				}
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId == "tagdef_target_color" || controlId == "tagdef_label_color")
			{
				const std::string colorPath = (controlId == "tagdef_target_color") ? GetTagEditorTargetColorPath() : GetTagEditorLabelColorPath();
				if (!colorPath.empty() && IsProfileColorPathValid(colorPath))
				{
					OpenProfileColorPicker(colorPath, true);
				}
				else
				{
					GetPlugIn()->DisplayUserMessage("vSMR", "Profile Editor", "Selected color is not available in this profile", true, true, false, false, false);
				}
				return;
			}

			if (controlId == "tagdef_insert_token")
			{
				GetPlugIn()->OpenPopupList(Area, "Insert Token", 1);
				for (const std::string& token : GetTagDefinitionTokens())
				{
					GetPlugIn()->AddPopupListElement(token.c_str(), "", RIMCAS_TAGDEF_INSERT_TOKEN, false, 0);
				}
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId == "tagdef_insert_token_bold")
			{
				GetPlugIn()->OpenPopupList(Area, "Insert Bold Token", 1);
				for (const std::string& token : GetTagDefinitionTokens())
				{
					GetPlugIn()->AddPopupListElement(token.c_str(), "", RIMCAS_TAGDEF_INSERT_TOKEN_BOLD, false, 0);
				}
				GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
				return;
			}

			if (controlId.find("tagdef_line_") == 0)
			{
				int lineIndex = atoi(controlId.substr(strlen("tagdef_line_")).c_str());
				lineIndex = max(0, min(lineIndex, TagDefinitionEditorMaxLines - 1));
				TagDefinitionEditorSelectedLine = lineIndex;
				std::vector<std::string> lines = GetTagDefinitionLineStrings(TagDefinitionEditorType, TagDefinitionEditorDetailed, TagDefinitionEditorMaxLines, true, TagDefinitionEditorDepartureStatus);
				std::string initial = lines[lineIndex];
				GetPlugIn()->OpenPopupEdit(Area, RIMCAS_TAGDEF_EDIT_LINE_BASE + lineIndex, initial.c_str());
				RequestRefresh();
				return;
			}
		}

		if (Button == BUTTON_RIGHT)
		{
			ShowTagDefinitionEditor = false;
			RequestRefresh();
			return;
		}
	}

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
			GetPlugIn()->AddPopupListElement("Icons", "", RIMCAS_OPEN_LIST);
			GetPlugIn()->AddPopupListElement("Icon Size", "", RIMCAS_OPEN_LIST);
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
			GetPlugIn()->AddPopupListElement("Profile Colors", "", RIMCAS_OPEN_LIST);
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
		//GetPlugIn()->SetASELAircraft(rt); // NOTE: This does NOT work eventhough the api says it should?
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));  // make sure the correct aircraft is selected before calling 'StartTagFunction'
		
		if (rt.GetCorrelatedFlightPlan().IsValid()) {
			StartTagFunction(rt.GetCallsign(), NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), NULL, EuroScopePlugIn::TAG_ITEM_FUNCTION_NO, Pt, Area);
		}		

		// Release & correlate actions

		if (ReleaseInProgress || AcquireInProgress)
		{
			if (ReleaseInProgress)
			{
				ReleaseInProgress = NeedCorrelateCursor = false;

				ReleasedTracks.push_back(rt.GetSystemID());

				if (std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()) != ManuallyCorrelated.end())
					ManuallyCorrelated.erase(std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()));
			}

			if (AcquireInProgress)
			{
				AcquireInProgress = NeedCorrelateCursor = false;

				ManuallyCorrelated.push_back(rt.GetSystemID());

				if (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()) != ReleasedTracks.end())
					ReleasedTracks.erase(std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()));
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
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		if (rt.GetCorrelatedFlightPlan().IsValid()) {
			if (ObjectType == TAG_CITEM_CALLSIGN) {
				// Shortcut: open ground status popup (clearance/push/taxi/depa) on callsign left-click.
				StartTagFunction(rt.GetCallsign(), NULL, TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), NULL, TAG_ITEM_FUNCTION_SET_GROUND_STATUS, Pt, Area);
			}
			else if (ObjectType == TAG_CITEM_CLEARANCE) {
				StartTagFunction(rt.GetCallsign(), NULL, TAG_ITEM_TYPE_CLEARENCE, rt.GetCallsign(), NULL, TAG_ITEM_FUNCTION_SET_CLEARED_FLAG, Pt, Area);
			}
			else {
				StartTagFunction(rt.GetCallsign(), NULL, TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), NULL, TAG_ITEM_FUNCTION_NO, Pt, Area);
			}
		}
	}

	if (Button == BUTTON_MIDDLE && TagObjectMiddleTypes[ObjectType]) {
		int TagMenu = TagObjectMiddleTypes[ObjectType];
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		StartTagFunction(rt.GetCallsign(), NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), NULL, TagMenu, Pt, Area);
	}

	if (Button == BUTTON_RIGHT && TagObjectRightTypes[ObjectType]) {
		if (ObjectType == TAG_CITEM_UKSTAND) {
			CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
			GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
			StartTagFunction(rt.GetCallsign(), NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), "RampAgent", 0, Pt, Area);
		}
		else {
		int TagMenu = TagObjectRightTypes[ObjectType];
		CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
		GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
		StartTagFunction(rt.GetCallsign(), NULL, EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN, rt.GetCallsign(), NULL, TagMenu, Pt, Area);
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

