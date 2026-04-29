#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"

extern CPoint mouseLocation;

void CSMRRadar::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area) {
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;
	if (FunctionId == APPWINDOW_ONE || FunctionId == APPWINDOW_TWO) {
		int id = FunctionId - APPWINDOW_BASE;
		appWindowDisplays[id] = !appWindowDisplays[id];
	}

	if (FunctionId == RIMCAS_ACTIVE_AIRPORT_FUNC) {
		setActiveAirport(sItemString);
		SaveDataToAsr("Airport", "Active airport", getActiveAirport().c_str());
	}

	if (FunctionId == RIMCAS_ACTIVE_PROFILE_FUNC) {
		if (sItemString != nullptr)
		{
			auto trimAsciiWhitespace = [](const std::string& text) -> std::string
			{
				size_t start = 0;
				while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
					++start;

				size_t end = text.size();
				while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
					--end;
				return text.substr(start, end - start);
			};

			const std::string oldName = GetActiveProfileNameForEditor();
			const std::string newName = trimAsciiWhitespace(std::string(sItemString));
			if (!oldName.empty() && !newName.empty() && newName != oldName)
			{
				if (!RenameProfileForEditor(oldName, newName))
				{
					GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to rename active profile in vSMR_Profiles.json", true, true, false, false, false);
				}
				else
				{
					LoadCustomFont();
					const std::string activeProfile = GetActiveProfileNameForEditor();
					RememberSessionActiveProfile(activeProfile);
					WriteLastActiveProfileToDisk(activeProfile);
					SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
				}
			}
		}
	}

	if (FunctionId == RIMCAS_UPDATE_FONTS) {
		if (strcmp(sItemString, "Size 1") == 0)
			currentFontSize = 1;
		if (strcmp(sItemString, "Size 2") == 0)
			currentFontSize = 2;
		if (strcmp(sItemString, "Size 3") == 0)
			currentFontSize = 3;
		if (strcmp(sItemString, "Size 4") == 0)
			currentFontSize = 4;
		if (strcmp(sItemString, "Size 5") == 0)
			currentFontSize = 5;

		// Persist profile label font size even when selecting the currently active size.
		if (!SetActiveLabelFontSize(currentFontSize, false))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save label font size to vSMR_Profiles.json", true, true, false, false, false);
		}
		else if (!CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save label font size to vSMR_Profiles.json", true, true, false, false, false);
		}

		ShowLists["Label Font Size"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_TAG_FONT)
	{
		if (sItemString != nullptr)
		{
			// Keep font selection persisted in profile JSON even when selecting the current value again.
			if (!SetActiveTagFontName(sItemString, false))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save tag font to vSMR_Profiles.json", true, true, false, false, false);
			}
			else if (!CurrentConfig->saveConfig())
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save tag font to vSMR_Profiles.json", true, true, false, false, false);
			}
			else
			{
				LoadCustomFont();
			}
		}

		ShowLists["Tag Font"] = true;
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_QDM_TOGGLE) {
		QDMenabled = !QDMenabled;
		QDMSelectEnabled = false;
	}

	if (FunctionId == RIMCAS_QDM_SELECT_TOGGLE)
	{
		if (!QDMSelectEnabled)
		{
			QDMSelectPt = ConvertCoordFromPositionToPixel(AirportPositions[getActiveAirport()]);
		}
		QDMSelectEnabled = !QDMSelectEnabled;
		QDMenabled = false;
	}

	if (FunctionId == RIMCAS_UPDATE_PROFILE) {
		const std::string requestedProfile = (sItemString != nullptr) ? std::string(sItemString) : "";
		if (!SetActiveProfileForEditor(requestedProfile, false))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to switch active profile", true, true, false, false, false);
		}

		ShowLists["Profiles"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_ICON_STYLE)
	{
		if (sItemString != nullptr)
		{
			if (!SetActiveTargetIconStyle(sItemString, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon style to vSMR_Profiles.json", true, true, false, false, false);
			}
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TOGGLE_FIXED_PIXEL_ICON_SIZE)
	{
		const bool nextEnabled = !GetFixedPixelTargetIconSizeEnabled();
		if (!SetFixedPixelTargetIconSizeEnabled(nextEnabled, true))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save fixed pixel icon size to vSMR_Profiles.json", true, true, false, false, false);
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_FIXED_PIXEL_TRIANGLE_SCALE)
	{
		if (sItemString != nullptr)
		{
			double scale = atof(sItemString);
			scale = std::clamp(scale, 0.1, 3.0);
			if (!SetFixedPixelTriangleIconScale(scale, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save fixed size to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_TOGGLE_SMALL_ICON_BOOST)
	{
		const bool nextEnabled = !GetSmallTargetIconBoostEnabled();
		if (!SetSmallTargetIconBoostEnabled(nextEnabled, true))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save small icon boost to vSMR_Profiles.json", true, true, false, false, false);
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_SMALL_ICON_BOOST_FACTOR)
	{
		if (sItemString != nullptr)
		{
			double factor = atof(sItemString);
			factor = std::clamp(factor, 0.5, 4.0);
			if (!SetSmallTargetIconBoostFactor(factor, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon boost factor to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_SMALL_ICON_BOOST_RESOLUTION)
	{
		if (sItemString != nullptr)
		{
			if (!SetSmallTargetIconBoostResolutionPreset(sItemString, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon boost resolution to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATEFILTER1 || FunctionId == RIMCAS_UPDATEFILTER2) {
		int id = FunctionId - RIMCAS_UPDATEFILTER;
		if (startsWith("UNL", sItemString))
			sItemString = "66000";
		appWindows[id]->m_Filter = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATERANGE1 || FunctionId == RIMCAS_UPDATERANGE2) {
		int id = FunctionId - RIMCAS_UPDATERANGE;
		appWindows[id]->m_Scale = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATEROTATE1 || FunctionId == RIMCAS_UPDATEROTATE2) {
		int id = FunctionId - RIMCAS_UPDATEROTATE;
		appWindows[id]->m_Rotation = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATE_BRIGHNESS) {
		if (strcmp(sItemString, "Day") == 0)
			ColorSettingsDay = true;
		else
			ColorSettingsDay = false;

		ShowLists["Colour Settings"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_ALERTS_TOGGLE_FUNC) {
		RimcasInstance->toggleActiveAlert(string(sItemString));
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
		if (!CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save active alerts to vSMR_Profiles.json", true, true, false, false, false);
		}
		ShowLists["Active Alerts"] = true;
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CA_ARRIVAL_FUNC) {
		RimcasInstance->toggleMonitoredRunwayArr(string(sItemString));

		ShowLists["Conflict Alert ARR"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CA_MONITOR_FUNC) {
		RimcasInstance->toggleMonitoredRunwayDep(string(sItemString));

		ShowLists["Conflict Alert DEP"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CLOSED_RUNWAYS_FUNC) {
		RimcasInstance->toggleClosedRunway(string(sItemString));

		ShowLists["Runway closed"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_OPEN_LIST) {
		if (sItemString != nullptr &&
			(strcmp(sItemString, "Tag Definitions") == 0 || strcmp(sItemString, "Profile Editor") == 0))
		{
			OpenProfileEditorWindow();
			return;
		}

		ShowLists[string(sItemString)] = true;
		ListAreas[string(sItemString)] = Area;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_LVP) {
		if (strcmp(sItemString, "Normal") == 0)
			isLVP = false;
		if (strcmp(sItemString, "Low") == 0)
			isLVP = true;

		ShowLists["Visibility"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_AFTERGLOW)
	{
		Afterglow = !Afterglow;
	}

	if (FunctionId == RIMCAS_UPDATE_GND_TRAIL)
	{
		Trail_Gnd = atoi(sItemString);

		ShowLists["GRND Trail Dots"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_APP_TRAIL)
	{
		Trail_App = atoi(sItemString);

		ShowLists["APPR Trail Dots"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_PTL)
	{
		PredictedLength = atoi(sItemString);

		ShowLists["Predicted Track Line"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_LABEL)
	{
		ColorManager->update_brightness("label", std::atoi(sItemString));
		ShowLists["Label"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_AFTERGLOW)
	{
		ColorManager->update_brightness("afterglow", std::atoi(sItemString));
		ShowLists["Afterglow"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_SYMBOL)
	{
		ColorManager->update_brightness("symbol", std::atoi(sItemString));
		ShowLists["Symbol"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_RELEASE)
	{
		ReleaseInProgress = !ReleaseInProgress;
		if (ReleaseInProgress)
			AcquireInProgress = false;
		NeedCorrelateCursor = ReleaseInProgress;

		CorrelateCursor();
	}

	if (FunctionId == RIMCAS_UPDATE_ACQUIRE)
	{
		AcquireInProgress = !AcquireInProgress;
		if (AcquireInProgress)
			ReleaseInProgress = false;
		NeedCorrelateCursor = AcquireInProgress;

		CorrelateCursor();
	}
}
