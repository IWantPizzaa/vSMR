#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"
#include "SMRGroundState.hpp"
#include "VsmrControlCenterDialog.hpp"

extern CPoint mouseLocation;
extern std::vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	std::string NormalizeAirportInput(const char* input)
	{
		std::string airport = input != nullptr ? input : "";
		const auto first = std::find_if_not(airport.begin(), airport.end(), [](unsigned char value) { return std::isspace(value) != 0; });
		const auto last = std::find_if_not(airport.rbegin(), airport.rend(), [](unsigned char value) { return std::isspace(value) != 0; }).base();
		if (first >= last)
			return "";
		airport = std::string(first, last);
		std::transform(airport.begin(), airport.end(), airport.begin(), [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
		return airport;
	}
}

void CSMRRadar::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area) {
	(void)Area;
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
		GetPlugIn()->DisplayUserMessage("vSMR", "Inset Presets", message.c_str(), true, true, false, false, false);
	};
	if (FunctionId == VSMR_GROUND_STATUS_SELECT)
	{
		const bool selectLineup = _stricmp(itemString, "Line Up") == 0;
		const char* euroScopeStatus = nullptr;
		if (_stricmp(itemString, "No Status") == 0)
			euroScopeStatus = "NSTS";
		else if (_stricmp(itemString, "Startup") == 0)
			euroScopeStatus = "STUP";
		else if (_stricmp(itemString, "Push") == 0)
			euroScopeStatus = "PUSH";
		else if (_stricmp(itemString, "Taxi") == 0 || selectLineup)
			euroScopeStatus = "TAXI";
		else if (_stricmp(itemString, "Departure") == 0)
			euroScopeStatus = "DEPA";
		else if (_stricmp(itemString, "Taxi In") == 0)
			euroScopeStatus = "TXIN";
		else if (_stricmp(itemString, "Parked") == 0)
			euroScopeStatus = "PARK";
		else if (_stricmp(itemString, "Arrival") == 0)
			euroScopeStatus = "ARR";

		if (euroScopeStatus == nullptr || PendingGroundStatusCallsign.empty())
			return;

		CFlightPlan fp = GetPlugIn()->FlightPlanSelect(PendingGroundStatusCallsign.c_str());
		if (!fp.IsValid())
		{
			PendingGroundStatusCallsign.clear();
			showConfigError("The selected flight plan is no longer available.");
			return;
		}
		const std::string activeAirport = getActiveAirport();
		const char* origin = fp.GetFlightPlanData().GetOrigin();
		const char* destination = fp.GetFlightPlanData().GetDestination();
		const bool isDeparture = origin != nullptr && origin[0] != '\0' &&
			!activeAirport.empty() && _stricmp(origin, activeAirport.c_str()) == 0;
		const bool isArrival = !isDeparture && destination != nullptr && destination[0] != '\0' &&
			!activeAirport.empty() && _stricmp(destination, activeAirport.c_str()) == 0;
		const bool departureChoice = selectLineup ||
			_stricmp(itemString, "Startup") == 0 || _stricmp(itemString, "Push") == 0 ||
			_stricmp(itemString, "Taxi") == 0 || _stricmp(itemString, "Departure") == 0;
		const bool arrivalChoice = _stricmp(itemString, "Taxi In") == 0 ||
			_stricmp(itemString, "Parked") == 0 || _stricmp(itemString, "Arrival") == 0;
		if ((departureChoice && !isDeparture) || (arrivalChoice && !isArrival))
		{
			PendingGroundStatusCallsign.clear();
			showConfigError("The selected ground status does not match this flight plan at the active airport.");
			return;
		}

		auto assignedData = fp.GetControllerAssignedData();
		const char* existingScratchpadRaw = assignedData.GetScratchPadString();
		const std::string existingScratchpad = existingScratchpadRaw != nullptr ? existingScratchpadRaw : "";
		if (!assignedData.SetScratchPadString(euroScopeStatus))
		{
			PendingGroundStatusCallsign.clear();
			showConfigError("EuroScope did not accept the selected ground status.");
			return;
		}

		const char* updatedScratchpadRaw = assignedData.GetScratchPadString();
		const std::string updatedScratchpad = updatedScratchpadRaw != nullptr ? updatedScratchpadRaw : "";
		if (updatedScratchpad != existingScratchpad && !assignedData.SetScratchPadString(existingScratchpad.c_str()))
		{
			PendingGroundStatusCallsign.clear();
			showConfigError("The ground status was not changed because the existing scratchpad could not be restored.");
			return;
		}

		if (selectLineup)
			VsmrGroundState::SetLineupOverride(PendingGroundStatusCallsign.c_str());
		else
			VsmrGroundState::ClearLineupOverride(PendingGroundStatusCallsign.c_str());
		PendingGroundStatusCallsign.clear();
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar != nullptr)
				radar->RequestRefresh();
		}
		return;
	}
	if (FunctionId == RIMCAS_AVISO_PRESET_RENAME)
	{
		if (!hasItemString)
			return;

		const std::string oldName = GetActiveAvisoPresetName();
		if (!oldName.empty() && RenameAvisoPreset(oldName, itemString))
		{
			showAvisoPresetMessage("Renamed inset preset: " + GetActiveAvisoPresetName());
			if (VsmrControlCenterDialog != nullptr)
				VsmrControlCenterDialog->SyncFromRadar("preset");
			RequestRefresh();
		}
		else
			showConfigError("Failed to rename active inset preset.");
		return;
	}

	if (FunctionId == RIMCAS_ACTIVE_AIRPORT_FUNC) {
		if (hasItemString)
		{
			const std::string airport = NormalizeAirportInput(itemString);
			if (airport.empty())
				return;
			setActiveAirport(airport);
			if (VsmrControlCenterDialog != nullptr)
				VsmrControlCenterDialog->SyncFromRadar();
			RequestRefresh();
		}
		return;
	}

	if (FunctionId > RIMCAS_UPDATEFILTER && FunctionId <= RIMCAS_UPDATEFILTER3) {
		int id = FunctionId - RIMCAS_UPDATEFILTER;
		const char* filterValue = itemString;
		if (startsWith("UNL", filterValue))
			filterValue = "66000";
		CInsetWindow* appWindow = getAppWindowById(id);
		if (appWindow != nullptr && appWindow->IsSecondaryRadar())
		{
			appWindow->m_Filter = atoi(filterValue);
			RequestRefresh();
		}
	}

}
