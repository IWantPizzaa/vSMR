#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"
#include "VsmrControlCenterDialog.hpp"

extern CPoint mouseLocation;

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
			const std::string airport = NormalizeAirportInput(itemString);
			if (airport.empty())
				return;
			setActiveAirport(airport);
			SaveDataToAsr("Airport", "Active airport", getActiveAirport().c_str());
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

}
