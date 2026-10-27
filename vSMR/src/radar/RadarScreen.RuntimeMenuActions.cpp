#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/AvisoWindowPlacement.hpp"
#include "plugin/Plugin.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "radar/RecentAirports.hpp"
#include "radar/RuntimeMenuInputPolicy.hpp"
#include "shared/TextUtils.hpp"

#include <cstdlib>
#include <limits>

extern CPoint mouseLocation;

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

	const char* id = objectId != nullptr ? objectId : "";
	if (button == BUTTON_RIGHT &&
		objectType == RUNTIME_MENU_RAIL &&
		std::strcmp(id, "runtime.drag") == 0)
	{
		RuntimeMenuMinimized = !RuntimeMenuMinimized;
		ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
		RuntimeMenuPopupScrollOffset = 0;
		SaveRuntimeMenuMinimizedToAsr();
		RequestRefresh();
		return true;
	}
	if (button == BUTTON_RIGHT && objectType == RUNTIME_MENU_RAIL &&
		std::strcmp(id, "runtime.airport") == 0)
	{
		VsmrRadar::RememberAirport(RecentAirports, getActiveAirport());
		ActiveRuntimeMenuPopup = ActiveRuntimeMenuPopup == RuntimeMenuPopup::RecentAirports
			? RuntimeMenuPopup::None : RuntimeMenuPopup::RecentAirports;
		RuntimeMenuPopupScrollOffset = 0;
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
			// EuroScope can deliver the second click after OpenPopupEdit has
			// already created its editor. Opening a second editor crashes the host.
			const auto now = static_cast<std::uint64_t>(::GetTickCount64());
			if (VsmrRuntimeMenuInput::IsRepeatedAirportEdit(
				now, RuntimeAirportEditOpenedAt, ::GetDoubleClickTime()))
				return true;
			RuntimeAirportEditOpenedAt = now;
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

	constexpr char recentPrefix[] = "runtime.recent-airport.";
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::RecentAirports &&
		std::strncmp(id, recentPrefix, sizeof(recentPrefix) - 1U) == 0)
	{
		const std::string airport(id + sizeof(recentPrefix) - 1U);
		if (std::find(RecentAirports.begin(), RecentAirports.end(), airport) != RecentAirports.end())
		{
			CloseRuntimeMenuPopup();
			setActiveAirport(airport);
			RequestRefresh();
		}
		return true;
	}

	if (std::strcmp(id, "runtime.popup") == 0 || std::strcmp(id, "runtime.popup.cpdlc") == 0)
		return true;

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

	if (HandleRuntimeDatalinkClick(id, area) || HandleRuntimeListClick(id) || HandleRuntimeInsetClick(id)) return true;
	return HandleRuntimePresetClick(id, area);
}

void CSMRRadar::SyncRuntimeMenuControlCenter(const std::string& reason)
{
	if (VsmrControlCenterDialog != nullptr) VsmrControlCenterDialog->SyncFromRadar(reason);
}

bool CSMRRadar::HandleRuntimeDatalinkClick(const char* id, RECT area)
{
	// ----- Handling the fixed vSID interface actions -----
	VsmrVsid::CommandAction vsidAction{};
	if (VsmrVsid::TryParseRuntimeActionId(id, vsidAction))
	{
		std::string error;
		if (!VsmrVsid::SubmitCommand(vsidAction, getActiveAirport(), error))
		{
			const std::string message = error.empty()
				? "The vSID action could not be submitted."
				: error;
			GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"vSID",
				message.c_str(),
				true,
				false,
				false,
				false,
				false);
		}
		RequestRefresh();
		return true;
	}

	// ----- Handling PDC actions -----
	CSMRPlugin* datalinkPlugin = static_cast<CSMRPlugin*>(GetPlugIn());
	auto showDatalinkMessage = [&](const std::string& message, bool error)
	{
		if (!message.empty())
		{
			GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"PDC",
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
			showDatalinkMessage(error.empty() ? "The PDC service is unavailable." : error, true);
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
			error = "The PDC service is unavailable.";
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
			showDatalinkMessage(error.empty() ? "The PDC service is unavailable." : error, true);
		RequestRefresh();
		return true;
	}

	return false;
}

bool CSMRRadar::HandleRuntimeListClick(const char* id)
{
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
				SyncRuntimeMenuControlCenter("mode");
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
				SyncRuntimeMenuControlCenter("profile");
			}
			else
			{
				GetPlugIn()->DisplayUserMessage(
					"vSMR", "Profile",
					"The profile could not be selected. Check the profiles configuration.",
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
			SyncRuntimeMenuControlCenter();
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
			SyncRuntimeMenuControlCenter("preset");
		}
		RequestRefresh();
		return true;
	}

	return false;
}

bool CSMRRadar::HandleRuntimeInsetClick(const char* id)
{
	// ----- Handling inset actions -----
	auto toggleAppWindow = [&](int appWindowId)
	{
		CancelInsetWindowInteractions();
		auto display = appWindowDisplays.find(appWindowId);
		if (display != appWindowDisplays.end())
		{
			display->second = !display->second;
			if (display->second && KeepAvisoWindowPlacement &&
				appWindowId == APPWINDOW_AVISO - APPWINDOW_BASE)
			{
				const auto window = appWindows.find(appWindowId);
				if (window != appWindows.end() && window->second != nullptr)
					VsmrAvisoWindowPlacement::Remember(*window->second);
			}
			if (!display->second)
			{
				auto window = appWindows.find(appWindowId);
				if (window != appWindows.end() && window->second != nullptr)
					window->second->ResetAvisoInteractionState(true);
			}
			SaveInsetStateToAsrForAirport(getActiveAirport());
		}
		SyncRuntimeMenuControlCenter();
		RequestRefresh();
	};
	auto resetInsetWindow = [&](int appWindowId)
	{
		ResetInsetWindowState(appWindowId, true);
		if (KeepAvisoWindowPlacement && appWindowId == APPWINDOW_AVISO - APPWINDOW_BASE)
		{
			const auto window = appWindows.find(appWindowId);
			if (window != appWindows.end() && window->second != nullptr)
				VsmrAvisoWindowPlacement::Remember(*window->second);
		}
		SaveInsetStateToAsrForAirport(getActiveAirport());
		SyncRuntimeMenuControlCenter();
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
	return false;
}

bool CSMRRadar::HandleRuntimePresetClick(const char* id, RECT area)
{
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

	SyncRuntimeMenuControlCenter("preset");
	RequestRefresh();
	return true;}

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
		PublishSharedRuntimeMenuPosition(true);
	RequestRefresh();
	return true;
}

bool CSMRRadar::HandleOverlayMouseMessage(HWND sourceWindow, POINT screenPoint, UINT message)
{
	if (IsShutdownRequested() || sourceWindow == nullptr || !::IsWindow(sourceWindow))
		return false;
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// The Control Center is a native popup containing a WebView on another STA.
	// Its own messages must reach that window. If EuroScope receives a mouse
	// message underneath the visible popup, however, do not let a CDM screen
	// object at the same coordinates act on that click.
	if (VsmrControlCenterDialog != nullptr)
	{
		const HWND dialog = VsmrControlCenterDialog->GetSafeHwnd();
		if (::IsWindow(dialog) && ::IsWindowVisible(dialog))
		{
			if (sourceWindow == dialog || ::IsChild(dialog, sourceWindow))
				return false;
			RECT dialogBounds{};
			if (::GetWindowRect(dialog, &dialogBounds) &&
				::PtInRect(&dialogBounds, screenPoint))
			{
				return message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
					message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
					message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK;
			}
		}
	}

	if (RuntimeMenuHitTargets.empty())
		return false;
	POINT point{};
	const HWND foreground = ::GetForegroundWindow();
	if (!TagHoverPointer.Resolve(screenPoint, sourceWindow, foreground, point) &&
		!TagHoverPointer.ResolveOwnedMessage(screenPoint, sourceWindow, point))
	{
		const HWND renderWindow = AvisoRefreshHostWindow.load(std::memory_order_acquire);
		if (renderWindow == nullptr || !::IsWindow(renderWindow) ||
			(sourceWindow != renderWindow && !::IsChild(renderWindow, sourceWindow)))
			return false;
		point = screenPoint;
		if (!::ScreenToClient(renderWindow, &point))
			return false;
		point.x -= RuntimeMenuDcOrigin.x;
		point.y -= RuntimeMenuDcOrigin.y;
	}

	const bool overMenu = RuntimeMenuArea.PtInRect(point) != FALSE ||
		(ActiveRuntimeMenuPopup != RuntimeMenuPopup::None &&
			(RuntimeMenuPopupArea.PtInRect(point) != FALSE ||
				RuntimeMenuSecondaryPopupArea.PtInRect(point) != FALSE));
	const bool isLeftDown = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK;
	const bool isRightDown = message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK;
	if (message == WM_LBUTTONDBLCLK && overMenu)
	{
		// The first click already activated the menu control. Consume the
		// double-click message and its following button-up without reopening it.
		RuntimeMenuPress = {};
		return true;
	}
	if (isLeftDown || isRightDown)
	{
		if (!overMenu)
			return false;
		// The WH_MOUSE hook consumes menu clicks before EuroScope receives them.
		// On the first click back from another application, activate its window
		// explicitly so later input does not remain attached to the old foreground.
		const HWND root = ::GetAncestor(sourceWindow, GA_ROOT);
		if (root != nullptr && ::GetAncestor(foreground, GA_ROOT) != root)
			::SetForegroundWindow(root);
		RuntimeMenuPress = {};
		RuntimeMenuPress.active = true;
		RuntimeMenuPress.button = isLeftDown ? BUTTON_LEFT : BUTTON_RIGHT;
		RuntimeMenuPress.startPoint = point;
		for (auto it = RuntimeMenuHitTargets.rbegin(); it != RuntimeMenuHitTargets.rend(); ++it)
		{
			if (::PtInRect(&it->area, point))
			{
				RuntimeMenuPress.target = *it;
				break;
			}
		}
		return true;
	}
	if (message == WM_MOUSEMOVE && RuntimeMenuPress.active &&
		RuntimeMenuPress.button == BUTTON_LEFT &&
		RuntimeMenuPress.target.id == "runtime.drag")
	{
		const LONG deltaX = point.x - RuntimeMenuPress.startPoint.x;
		const LONG deltaY = point.y - RuntimeMenuPress.startPoint.y;
		if (!RuntimeMenuPress.dragging &&
			(std::abs(deltaX) >= 3 || std::abs(deltaY) >= 3))
			RuntimeMenuPress.dragging = true;
		if (RuntimeMenuPress.dragging)
		{
			RECT area = RuntimeMenuPress.target.area;
			::OffsetRect(&area, deltaX, deltaY);
			HandleRuntimeMenuMove(RUNTIME_MENU_RAIL, "runtime.drag", point, area, false);
		}
		mouseLocation = point;
		return true;
	}
	if (message == WM_MOUSEMOVE && overMenu)
	{
		if (mouseLocation.x != point.x || mouseLocation.y != point.y)
		{
			mouseLocation = point;
			RequestRefresh();
		}
		return true;
	}
	const bool isLeftUp = message == WM_LBUTTONUP;
	const bool isRightUp = message == WM_RBUTTONUP;
	if (isLeftUp || isRightUp)
	{
		if (!RuntimeMenuPress.active || RuntimeMenuPress.button != (isLeftUp ? BUTTON_LEFT : BUTTON_RIGHT))
			return overMenu;
		const RuntimeMenuPointerPress press = std::move(RuntimeMenuPress);
		RuntimeMenuPress = {};
		if (press.dragging)
		{
			RECT area = press.target.area;
			::OffsetRect(&area, point.x - press.startPoint.x, point.y - press.startPoint.y);
			HandleRuntimeMenuMove(RUNTIME_MENU_RAIL, "runtime.drag", point, area, true);
		}
		else if (!press.target.id.empty() && ::PtInRect(&press.target.area, point))
		{
			mouseLocation = point;
			HandleRuntimeMenuClick(press.target.type, press.target.id.c_str(), point,
				press.target.area, press.button);
		}
		return true;
	}
	return false;
}

void CSMRRadar::CloseRuntimeMenuPopup()
{
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::None)
		return;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	RequestRefresh();
}

std::string CSMRRadar::ShowInsetHostMenu()
{
	if (!IsInsetsOnly())
		return "Not a CoFrance inset host.";
	RuntimeMenuMinimized = false;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	std::string phases;
	for (unsigned int phase = 0; phase <= 3; ++phase)
	{
		if ((InsetHostRefreshPhasesSeen & (1u << phase)) != 0)
			phases += (phases.empty() ? "" : ",") + std::to_string(phase);
	}
	const CRect bounds(GetRadarArea());
	const std::string status = "CoFrance inset host attached; received phases=" +
		(phases.empty() ? "none" : phases) + "; radar area=" +
		std::to_string(bounds.Width()) + "x" + std::to_string(bounds.Height()) +
		". vSMR menu expanded.";
	Logger::info(status);
	RequestRefresh();
	return status;
}

void CSMRRadar::LoadRuntimeMenuState()
{
	RuntimeMenuStateLoaded = true;
	const char* minimizedText = GetDataFromAsr("RuntimeMenuMinimized");
	RuntimeMenuMinimized = minimizedText != nullptr && std::atoi(minimizedText) != 0;
	CSMRPlugin* plugin = static_cast<CSMRPlugin*>(GetPlugIn());
	if (plugin != nullptr && plugin->TryGetSharedRuntimeMenuPosition(RuntimeMenuPosition))
	{
		RuntimeMenuPositionInitialized = true;
		return;
	}

	// Migrate the first loaded ASR's old coordinates into the shared session.
	LONG x = 0;
	LONG y = 0;
	const char* xText = GetDataFromAsr("RuntimeMenuX");
	const bool hasX = TryParseAsrCoordinate(xText, x);
	const char* yText = GetDataFromAsr("RuntimeMenuY");
	const bool hasY = TryParseAsrCoordinate(yText, y);
	if (!hasX || !hasY)
	{
		if (RuntimeMenuPositionInitialized)
			PublishSharedRuntimeMenuPosition(false);
		return;
	}

	RuntimeMenuPosition = { x, y };
	RuntimeMenuPositionInitialized = true;
	PublishSharedRuntimeMenuPosition(false);
}

void CSMRRadar::SaveRuntimeMenuMinimizedToAsr()
{
	SaveDataToAsr(
		"RuntimeMenuMinimized",
		"vSMR runtime menu minimized state",
		RuntimeMenuMinimized ? "1" : "0");
}

void CSMRRadar::PublishSharedRuntimeMenuPosition(bool persist)
{
	CSMRPlugin* plugin = static_cast<CSMRPlugin*>(GetPlugIn());
	if (plugin != nullptr)
		plugin->SetSharedRuntimeMenuPosition(RuntimeMenuPosition, persist);
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar == this || radar->IsShutdownRequested())
			continue;
		radar->RuntimeMenuPosition = RuntimeMenuPosition;
		radar->RuntimeMenuPositionInitialized = true;
		radar->RequestRefresh();
	}
}
