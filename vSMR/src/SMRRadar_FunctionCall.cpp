#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"

extern CPoint mouseLocation;

void CSMRRadar::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area) {
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;
	const bool hasItemString = (sItemString != nullptr && sItemString[0] != '\0');
	const char* itemString = hasItemString ? sItemString : "";
	auto getAppWindowById = [&](int id) -> CInsetWindow*
	{
		auto appWindowIt = appWindows.find(id);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			return nullptr;
		return appWindowIt->second.get();
	};
	auto showConfigError = [&](const char* message) {
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", message, true, true, false, false, false);
	};
	auto showAvisoPresetMessage = [&](const std::string& message) {
		GetPlugIn()->DisplayUserMessage("vSMR", "AVISO Presets", message.c_str(), true, true, false, false, false);
	};
	auto persistProfileUpdate = [&](bool updateResult, const char* message) -> bool
	{
		if (!updateResult || !CurrentConfig->saveConfig())
		{
			showConfigError(message);
			return false;
		}
		return true;
	};
	auto reopenList = [&](const std::string& listName, bool refreshAfterOpen = false)
	{
		ShowLists[listName] = true;
		if (refreshAfterOpen)
			RequestRefresh();
	};
	auto parseFontSizeSelection = [&](const char* selection, int fallback) -> int
	{
		if (selection == nullptr)
			return fallback;
		if (strcmp(selection, "Size 1") == 0)
			return 1;
		if (strcmp(selection, "Size 2") == 0)
			return 2;
		if (strcmp(selection, "Size 3") == 0)
			return 3;
		if (strcmp(selection, "Size 4") == 0)
			return 4;
		if (strcmp(selection, "Size 5") == 0)
			return 5;
		return fallback;
	};
	auto isAppWindowFunction = [](int functionId) -> bool
	{
		return functionId > APPWINDOW_BASE && functionId <= APPWINDOW_AVISO;
	};
	if (isAppWindowFunction(FunctionId)) {
		int id = FunctionId - APPWINDOW_BASE;
		auto appWindowDisplayIt = appWindowDisplays.find(id);
		if (appWindowDisplayIt != appWindowDisplays.end())
		{
			appWindowDisplayIt->second = !appWindowDisplayIt->second;
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_CREATE_PROMPT)
	{
		GetPlugIn()->OpenPopupEdit(Area, RIMCAS_AVISO_PRESET_CREATE, "AVISO Preset");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_RENAME_PROMPT)
	{
		const std::string activePreset = GetActiveAvisoPresetName();
		if (activePreset.empty())
		{
			showAvisoPresetMessage("No active AVISO preset to rename.");
			return;
		}

		GetPlugIn()->OpenPopupEdit(Area, RIMCAS_AVISO_PRESET_RENAME, activePreset.c_str());
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_DUPLICATE_PROMPT)
	{
		const std::string activePreset = GetActiveAvisoPresetName();
		if (activePreset.empty())
		{
			showAvisoPresetMessage("No active AVISO preset to duplicate.");
			return;
		}

		const std::string suggestedName = "Copy of " + activePreset;
		GetPlugIn()->OpenPopupEdit(Area, RIMCAS_AVISO_PRESET_DUPLICATE, suggestedName.c_str());
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_CREATE)
	{
		if (!hasItemString)
			return;

		std::string savedName;
		if (SaveAvisoPreset(itemString, false, &savedName))
			showAvisoPresetMessage("Saved AVISO preset: " + savedName);
		else
			showConfigError("Failed to save AVISO preset to vSMR_Profiles.json");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_LOAD)
	{
		if (!hasItemString)
			return;

		if (LoadAvisoPreset(itemString))
			showAvisoPresetMessage("Loaded AVISO preset: " + GetActiveAvisoPresetName());
		else
			showAvisoPresetMessage("AVISO preset not found.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_UPDATE)
	{
		const std::string activePreset = GetActiveAvisoPresetName();
		if (UpdateActiveAvisoPreset())
			showAvisoPresetMessage("Updated AVISO preset: " + activePreset);
		else
			showConfigError("Failed to update active AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_RENAME)
	{
		if (!hasItemString)
			return;

		const std::string oldName = GetActiveAvisoPresetName();
		if (!oldName.empty() && RenameAvisoPreset(oldName, itemString))
			showAvisoPresetMessage("Renamed AVISO preset: " + GetActiveAvisoPresetName());
		else
			showConfigError("Failed to rename active AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_DUPLICATE)
	{
		if (!hasItemString)
			return;

		const std::string sourceName = GetActiveAvisoPresetName();
		std::string savedName;
		if (!sourceName.empty() && DuplicateAvisoPreset(sourceName, itemString, &savedName))
			showAvisoPresetMessage("Duplicated AVISO preset: " + savedName);
		else
			showConfigError("Failed to duplicate active AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_DELETE)
	{
		const std::string activePreset = GetActiveAvisoPresetName();
		if (!activePreset.empty() && DeleteAvisoPreset(activePreset))
			showAvisoPresetMessage("Deleted AVISO preset: " + activePreset);
		else
			showConfigError("Failed to delete active AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_SET_DEFAULT)
	{
		const std::string activePreset = GetActiveAvisoPresetName();
		if (!activePreset.empty() && SetDefaultAvisoPreset(activePreset))
			showAvisoPresetMessage("Default AVISO preset: " + activePreset);
		else
			showConfigError("Failed to set default AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_CLEAR_DEFAULT)
	{
		if (ClearDefaultAvisoPreset())
			showAvisoPresetMessage("Cleared default AVISO preset.");
		else
			showConfigError("Failed to clear default AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_RESET)
	{
		if (ResetActiveAvisoPreset())
			showAvisoPresetMessage("Reset AVISO view to preset: " + GetActiveAvisoPresetName());
		else
			showAvisoPresetMessage("No active or default AVISO preset to reset.");
		return;
	}

	if (FunctionId == RIMCAS_AVISO_PRESET_TOGGLE_LINK)
	{
		const bool linked = !IsAvisoPresetLinkedMovementEnabled();
		if (SetActiveAvisoPresetLinkedMovement(linked))
			showAvisoPresetMessage(std::string("AVISO linked movement ") + (linked ? "enabled." : "disabled."));
		else
			showConfigError("Failed to update AVISO linked movement.");
		return;
	}

	if (FunctionId == RIMCAS_ACTIVE_AIRPORT_FUNC) {
		if (hasItemString)
		{
			setActiveAirport(itemString);
			SaveDataToAsr("Airport", "Active airport", getActiveAirport().c_str());
		}
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
					showConfigError("Failed to rename active profile in vSMR_Profiles.json");
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
		currentFontSize = parseFontSizeSelection(itemString, currentFontSize);

		// Persist profile label font size even when selecting the currently active size.
		persistProfileUpdate(SetActiveLabelFontSize(currentFontSize, false), "Failed to save label font size to vSMR_Profiles.json");
		reopenList("Label Font Size");
	}

	if (FunctionId == RIMCAS_UPDATE_TAG_FONT)
	{
		if (sItemString != nullptr)
		{
			// Keep font selection persisted in profile JSON even when selecting the current value again.
			if (persistProfileUpdate(SetActiveTagFontName(sItemString, false), "Failed to save tag font to vSMR_Profiles.json"))
			{
				LoadCustomFont();
			}
		}

		reopenList("Tag Font", true);
	}

	if (FunctionId == RIMCAS_QDM_TOGGLE) {
		QDMenabled = !QDMenabled;
		QDMSelectEnabled = false;
	}

	if (FunctionId == RIMCAS_QDM_SELECT_TOGGLE)
	{
		if (!QDMSelectEnabled)
		{
			CPosition activeAirportPosition;
			if (TryGetActiveAirportPosition(activeAirportPosition))
				QDMSelectPt = ConvertCoordFromPositionToPixel(activeAirportPosition);
			else
				QDMSelectPt = Pt;
		}
		QDMSelectEnabled = !QDMSelectEnabled;
		QDMenabled = false;
	}

	if (FunctionId == RIMCAS_TOGGLE_PRO_MODE || FunctionId == RIMCAS_TOGGLE_TOWER_MODE)
	{
		const std::string activeProfileName = GetActiveProfileNameForEditor();
		if (activeProfileName.empty())
		{
			showConfigError("No active profile available for mode update");
		}
		else if (FunctionId == RIMCAS_TOGGLE_PRO_MODE)
		{
			bool enabled = false;
			if (!GetProfileProModeEnabledForEditor(activeProfileName, enabled) ||
				!SetProfileProModeEnabledForEditor(activeProfileName, !enabled))
			{
				showConfigError("Failed to save Pro mode to vSMR_Profiles.json");
			}
		}
		else
		{
			bool enabled = false;
			if (!GetProfileTowerModeEnabledForEditor(activeProfileName, enabled) ||
				!SetProfileTowerModeEnabledForEditor(activeProfileName, !enabled))
			{
				showConfigError("Failed to save Tower mode to vSMR_Profiles.json");
			}
		}

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_PROFILE) {
		const std::string requestedProfile = (sItemString != nullptr) ? std::string(sItemString) : "";
		if (!SetActiveProfileForEditor(requestedProfile, false))
		{
			showConfigError("Failed to switch active profile");
		}

		reopenList("Profiles");
	}

	if (FunctionId == RIMCAS_UPDATE_ICON_STYLE)
	{
		if (sItemString != nullptr)
		{
			if (!SetActiveTargetIconStyle(sItemString, true))
			{
				showConfigError("Failed to save icon style to vSMR_Profiles.json");
			}
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TOGGLE_FIXED_PIXEL_ICON_SIZE)
	{
		const bool nextEnabled = !GetFixedPixelTargetIconSizeEnabled();
		if (!SetFixedPixelTargetIconSizeEnabled(nextEnabled, true))
		{
			showConfigError("Failed to save fixed pixel icon size to vSMR_Profiles.json");
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
				showConfigError("Failed to save fixed size to vSMR_Profiles.json");
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_TOGGLE_SMALL_ICON_BOOST)
	{
		const bool nextEnabled = !GetSmallTargetIconBoostEnabled();
		if (!SetSmallTargetIconBoostEnabled(nextEnabled, true))
		{
			showConfigError("Failed to save small icon boost to vSMR_Profiles.json");
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
				showConfigError("Failed to save icon boost factor to vSMR_Profiles.json");
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
				showConfigError("Failed to save icon boost resolution to vSMR_Profiles.json");
			}
		}
		RequestRefresh();
	}

	if (FunctionId > RIMCAS_UPDATEFILTER && FunctionId <= RIMCAS_UPDATEFILTER3) {
		int id = FunctionId - RIMCAS_UPDATEFILTER;
		const char* filterValue = itemString;
		if (startsWith("UNL", filterValue))
			filterValue = "66000";
		CInsetWindow* appWindow = getAppWindowById(id);
		if (appWindow != nullptr && !appWindow->IsAvisoViewport())
		{
			appWindow->m_Filter = atoi(filterValue);
			RequestRefresh();
		}
	}

	if (FunctionId > RIMCAS_UPDATERANGE && FunctionId <= RIMCAS_UPDATERANGE3) {
		int id = FunctionId - RIMCAS_UPDATERANGE;
		CInsetWindow* appWindow = getAppWindowById(id);
		if (appWindow != nullptr)
		{
			if (appWindow->IsAvisoViewport())
			{
				appWindow->m_AvisoScale = max(1, atoi(itemString));
				appWindow->ClearAvisoViewportCache();
			}
			else
			{
				appWindow->m_Scale = atoi(itemString);
			}
			RequestRefresh();
		}
	}

	if (FunctionId > RIMCAS_UPDATEROTATE && FunctionId <= RIMCAS_UPDATEROTATE3) {
		int id = FunctionId - RIMCAS_UPDATEROTATE;
		CInsetWindow* appWindow = getAppWindowById(id);
		if (appWindow != nullptr && !appWindow->IsAvisoViewport())
		{
			appWindow->m_Rotation = atoi(itemString);
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_UPDATE_BRIGHNESS) {
		if (strcmp(itemString, "Day") == 0)
			ColorSettingsDay = true;
		else
			ColorSettingsDay = false;

		reopenList("Colour Settings", true);
	}

	if (FunctionId == RIMCAS_ALERTS_TOGGLE_FUNC) {
		if (hasItemString)
		{
			RimcasInstance->toggleActiveAlert(string(itemString));
			CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
			if (!CurrentConfig->saveConfig())
			{
				showConfigError("Failed to save active alerts to vSMR_Profiles.json");
			}
		}
		reopenList("Active Alerts", true);
	}

	if (FunctionId == RIMCAS_CA_ARRIVAL_FUNC) {
		if (hasItemString)
			RimcasInstance->toggleMonitoredRunwayArr(string(itemString));

		reopenList("Conflict Alert ARR", true);
	}

	if (FunctionId == RIMCAS_CA_MONITOR_FUNC) {
		if (hasItemString)
			RimcasInstance->toggleMonitoredRunwayDep(string(itemString));

		reopenList("Conflict Alert DEP", true);
	}

	if (FunctionId == RIMCAS_CLOSED_RUNWAYS_FUNC) {
		if (hasItemString)
			RimcasInstance->toggleClosedRunway(string(itemString));

		reopenList("Runway closed", true);
	}

	if (FunctionId == RIMCAS_OPEN_LIST) {
		if (hasItemString &&
			(strcmp(itemString, "Tag Definitions") == 0 || strcmp(itemString, "Profile Editor") == 0))
		{
			OpenProfileEditorWindow();
			return;
		}
		if (!hasItemString)
			return;

		reopenList(itemString);
		ListAreas[string(itemString)] = Area;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_LVP) {
		const bool oldLvp = isLVP;
		if (strcmp(itemString, "Normal") == 0)
			isLVP = false;
		if (strcmp(itemString, "Low") == 0)
			isLVP = true;
		if (isLVP != oldLvp)
			InvalidateRunwayGeometryCache();

		reopenList("Visibility", true);
	}

	if (FunctionId == RIMCAS_UPDATE_AFTERGLOW)
	{
		Afterglow = !Afterglow;
	}

	if (FunctionId == RIMCAS_UPDATE_GND_TRAIL)
	{
		if (hasItemString)
			Trail_Gnd = atoi(itemString);

		reopenList("GRND Trail Dots");
	}

	if (FunctionId == RIMCAS_UPDATE_APP_TRAIL)
	{
		if (hasItemString)
			Trail_App = atoi(itemString);

		reopenList("APPR Trail Dots");
	}

	if (FunctionId == RIMCAS_UPDATE_PTL)
	{
		if (hasItemString)
			PredictedLength = atoi(itemString);

		reopenList("Predicted Track Line");
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_LABEL)
	{
		if (hasItemString)
			ColorManager->update_brightness("label", std::atoi(itemString));
		reopenList("Label");
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_AFTERGLOW)
	{
		if (hasItemString)
			ColorManager->update_brightness("afterglow", std::atoi(itemString));
		reopenList("Afterglow");
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_SYMBOL)
	{
		if (hasItemString)
			ColorManager->update_brightness("symbol", std::atoi(itemString));
		reopenList("Symbol");
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
