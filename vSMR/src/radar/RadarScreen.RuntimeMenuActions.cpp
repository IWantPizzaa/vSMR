#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "insets/InsetWindow.hpp"
#include "plugin/Plugin.hpp"
#include "radar/RadarScreen.hpp"
#include "shared/TextUtils.hpp"

#include <cstdlib>
#include <limits>

namespace
{
	bool ParseIndexedObjectId(const char* objectId, const char* prefix, size_t& outIndex)
	{
		if (objectId == nullptr || prefix == nullptr)
			return false;

		const size_t prefixLength = std::strlen(prefix);
		if (std::strncmp(objectId, prefix, prefixLength) != 0)
			return false;

		const char* number = objectId + prefixLength;
		if (*number == '\0')
			return false;

		char* end = nullptr;
		const unsigned long parsed = std::strtoul(number, &end, 10);
		if (end == number || *end != '\0' || parsed > static_cast<unsigned long>((std::numeric_limits<size_t>::max)()))
			return false;

		outIndex = static_cast<size_t>(parsed);
		return true;
	}

	std::string MakeUniquePresetName(const std::vector<CSMRRadar::AvisoPreset>& presets, const std::string& requested)
	{
		auto exists = [&](const std::string& candidate)
		{
			for (const CSMRRadar::AvisoPreset& preset : presets)
			{
				if (AsciiCaseInsensitiveEquals(preset.name, candidate))
					return true;
			}
			return false;
		};

		if (!requested.empty() && !exists(requested))
			return requested;

		const std::string base = requested.empty() ? "Inset Preset" : requested;
		for (int suffix = 2; suffix < 10000; ++suffix)
		{
			const std::string candidate = base + " " + std::to_string(suffix);
			if (!exists(candidate))
				return candidate;
		}
		return base + " " + std::to_string(::GetTickCount());
	}

	bool TryParseAsrCoordinate(const char* text, LONG& value)
	{
		if (text == nullptr || text[0] == '\0')
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(text, &end, 10);
		if (end == text || *end != '\0' || parsed < -100000 || parsed > 100000)
			return false;
		value = static_cast<LONG>(parsed);
		return true;
	}
}

bool CSMRRadar::HandleRuntimeMenuClick(int objectType, const char* objectId, POINT point, RECT area, int button)
{
	UNREFERENCED_PARAMETER(point);
	if (objectType != RUNTIME_MENU_RAIL && objectType != RUNTIME_MENU_POPUP)
		return false;

	auto syncControlCenter = [&](const std::string& reason = "runtime")
	{
		if (VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar(reason);
	};

	const char* id = objectId != nullptr ? objectId : "";
	if (button == BUTTON_RIGHT &&
		objectType == RUNTIME_MENU_RAIL &&
		std::strcmp(id, "runtime.drag") == 0)
	{
		RuntimeMenuMinimized = !RuntimeMenuMinimized;
		ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
		RuntimeMenuPopupScrollOffset = 0;
		SaveRuntimeMenuPositionToAsr();
		RequestRefresh();
		return true;
	}
	if (button != BUTTON_LEFT)
	{
		if (objectType == RUNTIME_MENU_POPUP || std::strcmp(id, "runtime.drag") == 0)
			CloseRuntimeMenuPopup();
		return true;
	}

	if (objectType == RUNTIME_MENU_RAIL)
	{
		// Handling rail controls before popup actions
		auto togglePopup = [&](RuntimeMenuPopup popup)
		{
			ActiveRuntimeMenuPopup = ActiveRuntimeMenuPopup == popup ? RuntimeMenuPopup::None : popup;
			RuntimeMenuPopupScrollOffset = 0;
			RequestRefresh();
		};

		if (std::strcmp(id, "runtime.airport") == 0)
		{
			CloseRuntimeMenuPopup();
			GetPlugIn()->OpenPopupEdit(area, RIMCAS_ACTIVE_AIRPORT_FUNC, getActiveAirport().c_str());
		}
		else if (std::strcmp(id, "runtime.button.mode") == 0)
			togglePopup(RuntimeMenuPopup::Mode);
		else if (std::strcmp(id, "runtime.button.groups") == 0)
			togglePopup(RuntimeMenuPopup::Groups);
		else if (std::strcmp(id, "runtime.button.insets") == 0)
			togglePopup(RuntimeMenuPopup::Insets);
		else if (std::strcmp(id, "runtime.button.profile") == 0)
			togglePopup(RuntimeMenuPopup::Profile);
		else if (std::strcmp(id, "runtime.button.datalink") == 0)
			togglePopup(RuntimeMenuPopup::Datalink);
		else if (std::strcmp(id, "runtime.button.control-center") == 0)
		{
			CloseRuntimeMenuPopup();
			OpenVsmrControlCenterWindow();
		}
		return true;
	}

	if (std::strcmp(id, "runtime.close") == 0)
	{
		CloseRuntimeMenuPopup();
		return true;
	}
	if (std::strcmp(id, "runtime.popup") == 0)
		return true;

	// ----- Handling CPDLC and PDC actions -----
	CSMRPlugin* datalinkPlugin = static_cast<CSMRPlugin*>(GetPlugIn());
	auto showDatalinkMessage = [&](const std::string& message, bool error)
	{
		if (!message.empty())
		{
			GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"CPDLC / PDC",
				message.c_str(),
				true,
				true,
				false,
				true,
				false);
		}
		if (error)
			RequestRefresh();
	};
	if (std::strcmp(id, "runtime.datalink.credentials") == 0)
	{
		std::string error;
		if (datalinkPlugin == nullptr || !datalinkPlugin->EditDatalinkCredentials(error))
			showDatalinkMessage(error.empty() ? "The CPDLC service is unavailable." : error, true);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.callsign") == 0)
	{
		if (datalinkPlugin != nullptr)
		{
			const DatalinkControlState state = datalinkPlugin->GetDatalinkControlState();
			GetPlugIn()->OpenPopupEdit(
				area,
				RUNTIME_DATALINK_CALLSIGN_EDIT,
				state.logonCallsign.c_str());
		}
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.connection") == 0)
	{
		std::string error;
		if (datalinkPlugin == nullptr)
			error = "The CPDLC service is unavailable.";
		else
		{
			const DatalinkControlState state = datalinkPlugin->GetDatalinkControlState();
			if (state.connected || state.connecting)
				datalinkPlugin->DisconnectDatalink(error);
			else
				datalinkPlugin->ConnectDatalink(error);
		}
		if (!error.empty())
			showDatalinkMessage(error, true);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.poll") == 0)
	{
		std::string error;
		if (datalinkPlugin == nullptr || !datalinkPlugin->PollDatalink(error))
			showDatalinkMessage(error.empty() ? "The CPDLC service is unavailable." : error, true);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.delay") == 0 ||
		std::strcmp(id, "runtime.datalink.cooldown") == 0)
	{
		if (datalinkPlugin != nullptr)
		{
			const DatalinkControlState state = datalinkPlugin->GetDatalinkControlState();
			const bool editDelay = std::strcmp(id, "runtime.datalink.delay") == 0;
			const int currentValue = editDelay ? state.cdmDelayMinutes : state.cdmCooldownMinutes;
			GetPlugIn()->OpenPopupEdit(
				area,
				editDelay ? RUNTIME_DATALINK_DELAY_EDIT : RUNTIME_DATALINK_COOLDOWN_EDIT,
				std::to_string(currentValue).c_str());
		}
		return true;
	}
	const bool decrementDelay = std::strcmp(id, "runtime.datalink.delay.decrement") == 0;
	const bool incrementDelay = std::strcmp(id, "runtime.datalink.delay.increment") == 0;
	const bool decrementCooldown = std::strcmp(id, "runtime.datalink.cooldown.decrement") == 0;
	const bool incrementCooldown = std::strcmp(id, "runtime.datalink.cooldown.increment") == 0;
	if (decrementDelay || incrementDelay || decrementCooldown || incrementCooldown)
	{
		std::string error;
		if (datalinkPlugin == nullptr)
			error = "The CDM reminder service is unavailable.";
		else
		{
			const DatalinkControlState state = datalinkPlugin->GetDatalinkControlState();
			const int delayDelta = incrementDelay ? 1 : (decrementDelay ? -1 : 0);
			const int cooldownDelta = incrementCooldown ? 1 : (decrementCooldown ? -1 : 0);
			const int delay = std::clamp(state.cdmDelayMinutes + delayDelta, 0, 1440);
			const int cooldown = std::clamp(state.cdmCooldownMinutes + cooldownDelta, 0, 1440);
			datalinkPlugin->UpdateDatalinkControlSettings(
				state.logonCallsign,
				"",
				false,
				state.cdmAutoEnabled,
				delay,
				cooldown,
				error,
				false);
		}
		if (!error.empty())
			showDatalinkMessage(error, true);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.reminders") == 0)
	{
		std::string error;
		if (datalinkPlugin == nullptr)
			error = "The CDM reminder service is unavailable.";
		else
		{
			const DatalinkControlState state = datalinkPlugin->GetDatalinkControlState();
			datalinkPlugin->UpdateDatalinkControlSettings(
				state.logonCallsign,
				"",
				false,
				!state.cdmAutoEnabled,
				state.cdmDelayMinutes,
				state.cdmCooldownMinutes,
				error,
				false);
		}
		if (!error.empty())
			showDatalinkMessage(error, true);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.datalink.scan") == 0)
	{
		std::string result;
		std::string error;
		if (datalinkPlugin == nullptr || !datalinkPlugin->RunCdmReminderScan(result, error))
			showDatalinkMessage(error.empty() ? "The CDM reminder service is unavailable." : error, true);
		else
			showDatalinkMessage(result, false);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.page.previous") == 0)
	{
		RuntimeMenuPopupScrollOffset = (std::max)(0, RuntimeMenuPopupScrollOffset - 5);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.page.next") == 0)
	{
		RuntimeMenuPopupScrollOffset += 5;
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.preset.page.previous") == 0)
	{
		RuntimeMenuPopupScrollOffset = (std::max)(0, RuntimeMenuPopupScrollOffset - 4);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.preset.page.next") == 0)
	{
		RuntimeMenuPopupScrollOffset += 4;
		RequestRefresh();
		return true;
	}

	// ----- Applying list selections -----
	size_t index = 0;
	if (ParseIndexedObjectId(id, "runtime.mode.", index))
	{
		const std::string activeProfile = GetActiveProfileNameForEditor();
		const std::vector<DisplayModeSettings> modes = GetProfileDisplayModesForEditor(activeProfile);
		if (index < modes.size())
		{
			if (SetProfileDisplayModeActiveForEditor(activeProfile, modes[index].name))
			{
				syncControlCenter("mode");
			}
			else
			{
				GetPlugIn()->DisplayUserMessage(
					"vSMR", "Display mode",
					"The display mode could not be saved. vSMR reloaded the current file.",
					true, true, false, false, false);
			}
		}
		CloseRuntimeMenuPopup();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.profile.", index))
	{
		const std::vector<std::string> profiles = GetOrderedProfileNamesForUi();
		if (index < profiles.size())
		{
			if (SetActiveProfileForEditor(profiles[index], false))
			{
				syncControlCenter("profile");
			}
			else
			{
				GetPlugIn()->DisplayUserMessage(
					"vSMR", "Profile",
					"The profile could not be saved. vSMR reloaded the current file.",
					true, true, false, false, false);
			}
		}
		CloseRuntimeMenuPopup();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.group.", index))
	{
		const std::vector<AvisoGroup> groups = GetAvisoGroups();
		if (index < groups.size())
		{
			ToggleAvisoGroupVisibility(groups[index].id);
			syncControlCenter();
		}
		RequestRefresh();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.preset.", index))
	{
		const std::vector<AvisoPreset> presets = GetAvisoPresets();
		if (index < presets.size())
		{
			LoadAvisoPreset(presets[index].name);
			syncControlCenter("preset");
		}
		RequestRefresh();
		return true;
	}

	// ----- Handling inset actions -----
	auto toggleAppWindow = [&](int appWindowId)
	{
		CancelInsetWindowInteractions();
		auto display = appWindowDisplays.find(appWindowId);
		if (display != appWindowDisplays.end())
		{
			display->second = !display->second;
			if (!display->second)
			{
				auto window = appWindows.find(appWindowId);
				if (window != appWindows.end() && window->second != nullptr)
					window->second->ResetAvisoInteractionState();
			}
			SaveInsetStateToAsrForAirport(getActiveAirport());
		}
		syncControlCenter();
		RequestRefresh();
	};
	auto resetInsetWindow = [&](int appWindowId)
	{
		ResetInsetWindowState(appWindowId, true);
		SaveInsetStateToAsrForAirport(getActiveAirport());
		syncControlCenter();
		RequestRefresh();
	};
	if (std::strcmp(id, "runtime.inset.aviso") == 0)
	{
		toggleAppWindow(APPWINDOW_AVISO - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.srw1") == 0)
	{
		toggleAppWindow(1);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.weather") == 0)
	{
		toggleAppWindow(APPWINDOW_WEATHER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.timer") == 0)
	{
		toggleAppWindow(APPWINDOW_TIMER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.aviso") == 0)
	{
		resetInsetWindow(APPWINDOW_AVISO - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.srw1") == 0)
	{
		resetInsetWindow(1);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.weather") == 0)
	{
		resetInsetWindow(APPWINDOW_WEATHER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.timer") == 0)
	{
		resetInsetWindow(APPWINDOW_TIMER - APPWINDOW_BASE);
		return true;
	}

	// ----- Handling preset actions -----
	const std::vector<AvisoPreset> presets = GetAvisoPresets();
	std::string activePreset = GetActiveAvisoPresetName();
	const auto activePresetIt = std::find_if(
		presets.begin(),
		presets.end(),
		[&](const AvisoPreset& preset) { return AsciiCaseInsensitiveEquals(preset.name, activePreset); });
	if (activePresetIt == presets.end())
		activePreset.clear();
	else
		activePreset = activePresetIt->name;
	const std::string defaultPreset = GetDefaultAvisoPresetName();
	if (std::strcmp(id, "runtime.preset.none") == 0)
	{
		ActivateNoAvisoPreset();
	}
	else if (std::strcmp(id, "runtime.preset.save") == 0)
	{
		const std::string name = MakeUniquePresetName(presets, "Inset Preset");
		SaveAvisoPreset(name, false, nullptr);
	}
	else if (std::strcmp(id, "runtime.preset.update") == 0)
		UpdateActiveAvisoPreset();
	else if (std::strcmp(id, "runtime.preset.rename") == 0 && !activePreset.empty())
		GetPlugIn()->OpenPopupEdit(area, RIMCAS_AVISO_PRESET_RENAME, activePreset.c_str());
	else if (std::strcmp(id, "runtime.preset.duplicate") == 0 && !activePreset.empty())
	{
		const std::string name = MakeUniquePresetName(presets, "Copy of " + activePreset);
		DuplicateAvisoPreset(activePreset, name, nullptr);
	}
	else if (std::strcmp(id, "runtime.preset.default") == 0)
	{
		if (!defaultPreset.empty() &&
			(activePreset.empty() || AsciiCaseInsensitiveEquals(activePreset, defaultPreset)))
		{
			ClearDefaultAvisoPreset();
		}
		else if (!activePreset.empty())
		{
			SetDefaultAvisoPreset(activePreset);
		}
		else
		{
			return true;
		}
	}
	else if (std::strcmp(id, "runtime.preset.reset") == 0 && !activePreset.empty())
		ResetActiveAvisoPreset();
	else if (std::strcmp(id, "runtime.preset.delete") == 0 && !activePreset.empty())
		DeleteAvisoPreset(activePreset);
	else
		return true;

	syncControlCenter("preset");
	RequestRefresh();
	return true;
}

bool CSMRRadar::HandleRuntimeMenuMove(int objectType, const char* objectId, POINT point, RECT area, bool released)
{
	UNREFERENCED_PARAMETER(point);
	if (objectType != RUNTIME_MENU_RAIL ||
		objectId == nullptr ||
		std::strcmp(objectId, "runtime.drag") != 0)
	{
		return false;
	}

	// The draggable handle sits one pixel inside the framed rail.
	RuntimeMenuPosition.x = area.left - 1;
	RuntimeMenuPosition.y = area.top - 1;
	RuntimeMenuPositionInitialized = true;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	if (released)
		SaveRuntimeMenuPositionToAsr();
	RequestRefresh();
	return true;
}

void CSMRRadar::CloseRuntimeMenuPopup()
{
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::None)
		return;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	RequestRefresh();
}

void CSMRRadar::LoadRuntimeMenuPositionFromAsr()
{
	const char* minimizedText = GetDataFromAsr("RuntimeMenuMinimized");
	RuntimeMenuMinimized = minimizedText != nullptr && std::atoi(minimizedText) != 0;

	LONG x = 0;
	LONG y = 0;
	const char* xText = GetDataFromAsr("RuntimeMenuX");
	const bool hasX = TryParseAsrCoordinate(xText, x);
	const char* yText = GetDataFromAsr("RuntimeMenuY");
	const bool hasY = TryParseAsrCoordinate(yText, y);
	if (!hasX || !hasY)
		return;

	RuntimeMenuPosition = { x, y };
	RuntimeMenuPositionInitialized = true;
}

void CSMRRadar::SaveRuntimeMenuPositionToAsr()
{
	const std::string x = std::to_string(RuntimeMenuPosition.x);
	const std::string y = std::to_string(RuntimeMenuPosition.y);
	SaveDataToAsr("RuntimeMenuX", "vSMR runtime menu X position", x.c_str());
	SaveDataToAsr("RuntimeMenuY", "vSMR runtime menu Y position", y.c_str());
	SaveDataToAsr(
		"RuntimeMenuMinimized",
		"vSMR runtime menu minimized state",
		RuntimeMenuMinimized ? "1" : "0");
}
