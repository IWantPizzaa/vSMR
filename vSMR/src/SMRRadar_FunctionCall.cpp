#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"
#include "VsmrControlCenterDialog.hpp"

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
	auto reopenList = [&](const std::string& listName, bool refreshAfterOpen = false)
	{
		ShowLists[listName] = true;
		if (refreshAfterOpen)
			RequestRefresh();
	};
	if (FunctionId == RIMCAS_AVISO_PRESET_RENAME)
	{
		if (!hasItemString)
			return;

		const std::string oldName = GetActiveAvisoPresetName();
		if (!oldName.empty() && RenameAvisoPreset(oldName, itemString))
		{
			showAvisoPresetMessage("Renamed AVISO preset: " + GetActiveAvisoPresetName());
			if (VsmrControlCenterDialog != nullptr)
				VsmrControlCenterDialog->SyncFromRadar();
			RequestRefresh();
		}
		else
			showConfigError("Failed to rename active AVISO preset.");
		return;
	}

	if (FunctionId == RIMCAS_ACTIVE_AIRPORT_FUNC) {
		if (hasItemString)
		{
			setActiveAirport(itemString);
			SaveDataToAsr("Airport", "Active airport", getActiveAirport().c_str());
		}
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
		if (appWindow != nullptr && !appWindow->IsAvisoViewport())
		{
			appWindow->m_Scale = atoi(itemString);
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

		reopenList("Day / Night", true);
	}

	if (FunctionId == RIMCAS_OPEN_LIST) {
		if (!hasItemString)
			return;

		reopenList(itemString);
		ListAreas[string(itemString)] = Area;

		RequestRefresh();
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
