#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
#include "insets/InsetWindow.hpp"
#include "aircraft/GroundState.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashRuntime.hpp"

extern std::vector<CSMRRadar*> RadarScreensOpened;
extern CPoint mouseLocation;
extern string TagBeingDragged;
extern HCURSOR smrCursor;
extern bool standardCursor;
extern bool customCursor;

namespace
{
	bool gInsetCursorOverride = false;

	bool IsAppWindowObjectType(int objectType)
	{
		return objectType == APPWINDOW_ONE ||
			objectType == APPWINDOW_AVISO ||
			objectType == APPWINDOW_WEATHER ||
			objectType == APPWINDOW_TIMER;
	}

	bool IsAppWindowVisible(CSMRRadar* radar, int appWindowId)
	{
		if (radar == nullptr)
			return false;

		const auto displayIt = radar->appWindowDisplays.find(appWindowId);
		return displayIt != radar->appWindowDisplays.end() && displayIt->second;
	}

	HCURSOR LoadDefaultRadarCursor()
	{
		AFX_MANAGE_STATE(AfxGetStaticModuleState());
		if (customCursor)
		{
			HCURSOR cursor = reinterpret_cast<HCURSOR>(::LoadImage(
				AfxGetInstanceHandle(),
				MAKEINTRESOURCE(IDC_SMRCURSOR),
				IMAGE_CURSOR,
				0,
				0,
				LR_SHARED));
			if (cursor != nullptr)
				return cursor;
		}
		return ::LoadCursor(nullptr, IDC_ARROW);
	}

	void ApplyRadarCursor(HCURSOR cursor, bool isDefault)
	{
		if (cursor == nullptr)
			return;
		smrCursor = cursor;
		::SetCursor(cursor);
		standardCursor = isDefault;
	}

	HCURSOR ResizeCursorForRegion(CInsetWindow::ResizeRegion region)
	{
		switch (region)
		{
		case CInsetWindow::ResizeRegion::Left:
		case CInsetWindow::ResizeRegion::Right:
			return ::LoadCursor(nullptr, IDC_SIZEWE);
		case CInsetWindow::ResizeRegion::Top:
		case CInsetWindow::ResizeRegion::Bottom:
			return ::LoadCursor(nullptr, IDC_SIZENS);
		case CInsetWindow::ResizeRegion::TopLeft:
		case CInsetWindow::ResizeRegion::BottomRight:
			return ::LoadCursor(nullptr, IDC_SIZENWSE);
		case CInsetWindow::ResizeRegion::TopRight:
		case CInsetWindow::ResizeRegion::BottomLeft:
			return ::LoadCursor(nullptr, IDC_SIZENESW);
		default:
			return nullptr;
		}
	}

	CInsetWindow::ResizeRegion ResizeRegionFromObjectId(const char* objectId)
	{
		if (objectId == nullptr)
			return CInsetWindow::ResizeRegion::None;
		if (strcmp(objectId, "resize_left") == 0) return CInsetWindow::ResizeRegion::Left;
		if (strcmp(objectId, "resize_right") == 0) return CInsetWindow::ResizeRegion::Right;
		if (strcmp(objectId, "resize_top") == 0) return CInsetWindow::ResizeRegion::Top;
		if (strcmp(objectId, "resize_bottom") == 0) return CInsetWindow::ResizeRegion::Bottom;
		if (strcmp(objectId, "resize_tl") == 0) return CInsetWindow::ResizeRegion::TopLeft;
		if (strcmp(objectId, "resize_tr") == 0) return CInsetWindow::ResizeRegion::TopRight;
		if (strcmp(objectId, "resize_bl") == 0) return CInsetWindow::ResizeRegion::BottomLeft;
		if (strcmp(objectId, "resize_br") == 0) return CInsetWindow::ResizeRegion::BottomRight;
		return CInsetWindow::ResizeRegion::None;
	}

	void ApplyResizeCursor(CInsetWindow::ResizeRegion region)
	{
		ApplyRadarCursor(ResizeCursorForRegion(region), false);
		gInsetCursorOverride = true;
	}

	void ApplyMoveCursor()
	{
		ApplyRadarCursor(::LoadCursor(nullptr, IDC_SIZEALL), false);
		gInsetCursorOverride = true;
	}

	void RestoreRadarCursor()
	{
		if (standardCursor)
			return;
		ApplyRadarCursor(LoadDefaultRadarCursor(), true);
	}

	void RestoreInsetCursor()
	{
		if (!gInsetCursorOverride)
			return;
		gInsetCursorOverride = false;
		RestoreRadarCursor();
	}

	bool IsPointInMainRadarArea(CSMRRadar* radar, POINT pt)
	{
		if (radar == nullptr)
			return false;

		CRect radarArea = radar->ResolveMainAvisoRenderArea();
		radarArea.NormalizeRect();
		return
			pt.x >= radarArea.left &&
			pt.x <= radarArea.right &&
			pt.y >= radarArea.top &&
			pt.y <= radarArea.bottom;
	}

	bool IsPointInRuntimeMenuOverlay(CSMRRadar* radar, POINT pt)
	{
		if (radar == nullptr)
			return false;

		if (!radar->RuntimeMenuArea.IsRectEmpty() && radar->RuntimeMenuArea.PtInRect(pt))
			return true;

		return
			radar->ActiveRuntimeMenuPopup != CSMRRadar::RuntimeMenuPopup::None &&
			!radar->RuntimeMenuPopupArea.IsRectEmpty() &&
			radar->RuntimeMenuPopupArea.PtInRect(pt);
	}

	CRect AvisoViewportLayoutBounds(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return CRect(0, 0, 0, 0);

		CRect radarArea(radar->GetRadarArea());
		CRect chatArea(radar->GetChatArea());
		radarArea.NormalizeRect();
		chatArea.NormalizeRect();
		if (!chatArea.IsRectEmpty())
			radarArea.bottom = chatArea.top;
		radarArea.NormalizeRect();
		return radarArea;
	}

	bool WindowsShareHierarchy(HWND first, HWND second)
	{
		if (first == nullptr || second == nullptr || !::IsWindow(first) || !::IsWindow(second))
			return false;
		for (HWND current = first; current != nullptr && ::IsWindow(current); current = ::GetParent(current))
		{
			if (current == second)
				return true;
		}
		for (HWND current = second; current != nullptr && ::IsWindow(current); current = ::GetParent(current))
		{
			if (current == first)
				return true;
		}
		return false;
	}

	bool HasInsetRenderedInWindow(CSMRRadar* radar, HWND hwnd)
	{
		if (radar == nullptr || hwnd == nullptr || !::IsWindow(hwnd))
			return false;
		for (const auto& window : radar->appWindows)
		{
			if (!IsAppWindowVisible(radar, window.first) || window.second == nullptr)
				continue;
			if (WindowsShareHierarchy(hwnd, window.second->m_AvisoRenderWindow))
				return true;
		}
		return false;
	}

	CInsetWindow* TopmostVisibleInsetFrameAtPoint(CSMRRadar* radar, POINT pt, int inflation = 0)
	{
		if (radar == nullptr)
			return nullptr;

		auto containsPoint = [&](CInsetWindow* appWindow) -> bool
		{
			if (appWindow == nullptr)
				return false;
			CRect frame = appWindow->GetWindowFrameRect();
			frame.NormalizeRect();
			if (inflation > 0)
				frame.InflateRect(inflation, inflation);
			return frame.PtInRect(pt) != FALSE;
		};
		for (auto it = radar->appWindows.rbegin(); it != radar->appWindows.rend(); ++it)
		{
			CInsetWindow* appWindow = it->second.get();
			if (appWindow != nullptr && appWindow->m_AvisoScrollSelected &&
				IsAppWindowVisible(radar, it->first) && containsPoint(appWindow))
			{
				return appWindow;
			}
		}
		for (auto it = radar->appWindows.rbegin(); it != radar->appWindows.rend(); ++it)
		{
			const int appWindowId = it->first;
			CInsetWindow* appWindow = it->second.get();
			if (appWindow == nullptr || appWindow->m_AvisoScrollSelected ||
				!IsAppWindowVisible(radar, appWindowId))
				continue;
			if (containsPoint(appWindow))
				return appWindow;
		}
		return nullptr;
	}

	CInsetWindow* VisibleAppWindowAtPoint(CSMRRadar* radar, POINT pt)
	{
		CInsetWindow* appWindow = TopmostVisibleInsetFrameAtPoint(radar, pt);
		return appWindow != nullptr && appWindow->IsPointInside(pt) ? appWindow : nullptr;
	}

	CInsetWindow* VisibleAvisoViewportAtPoint(CSMRRadar* radar, POINT pt)
	{
		CInsetWindow* appWindow = VisibleAppWindowAtPoint(radar, pt);
		return appWindow != nullptr && appWindow->SupportsPanAndZoom()
			? appWindow
			: nullptr;
	}

	CInsetWindow* ActiveAvisoPanViewport(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return nullptr;

		for (auto& kv : radar->appWindows)
		{
			CInsetWindow* appWindow = kv.second.get();
			if (appWindow != nullptr && appWindow->m_AvisoRightPanning)
				return appWindow;
		}

		return nullptr;
	}

	CInsetWindow* ActiveInsetWindowInteraction(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return nullptr;
		for (auto& kv : radar->appWindows)
		{
			CInsetWindow* appWindow = kv.second.get();
			if (appWindow != nullptr &&
				(appWindow->IsWindowMoveActive() || appWindow->IsWindowResizeActive()))
			{
				return appWindow;
			}
		}
		return nullptr;
	}

	void SelectAvisoViewport(CSMRRadar* radar, CInsetWindow* selectedViewport)
	{
		if (radar == nullptr || selectedViewport == nullptr)
			return;

		radar->AvisoGeoJsonScrollSelected = false;
		for (auto& kv : radar->appWindows)
		{
			CInsetWindow* appWindow = kv.second.get();
			if (appWindow != nullptr)
				appWindow->m_AvisoScrollSelected = (appWindow == selectedViewport);
		}
	}

	void SelectMainAviso(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return;

		radar->AvisoGeoJsonScrollSelected = true;
		for (auto& kv : radar->appWindows)
		{
			CInsetWindow* appWindow = kv.second.get();
			if (appWindow != nullptr)
				appWindow->m_AvisoScrollSelected = false;
		}
	}

	bool SelectAvisoScrollTargetAtPoint(CSMRRadar* radar, POINT pt)
	{
		if (radar == nullptr)
			return false;

		CInsetWindow* insetAtPoint = TopmostVisibleInsetFrameAtPoint(radar, pt);
		if (insetAtPoint != nullptr)
		{
			// Selection also owns visual z-order. Non-map insets such as
			// Weather and Timer must therefore be selectable even though they
			// do not pan or zoom.
			SelectAvisoViewport(radar, insetAtPoint);
			return insetAtPoint->SupportsPanAndZoom();
		}
		if (IsPointInMainRadarArea(radar, pt))
		{
			SelectMainAviso(radar);
			return true;
		}

		return false;
	}

	bool EndAvisoViewportPans(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return false;

		bool endedPan = false;
		for (auto& kv : radar->appWindows)
		{
			CInsetWindow* appWindow = kv.second.get();
			if (appWindow != nullptr && appWindow->m_AvisoRightPanning)
			{
				appWindow->EndAvisoPan();
				endedPan = true;
			}
		}
		return endedPan;
	}
}

void CSMRRadar::OnButtonDownScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnButtonDownScreenObject",
		reinterpret_cast<std::uintptr_t>(this));
	Logger::info(string(__FUNCSIG__));
	UNREFERENCED_PARAMETER(sObjectId);
	UNREFERENCED_PARAMETER(Area);
	mouseLocation = Pt;
	if (IsPointInRuntimeMenuOverlay(this, Pt))
		return;

	if (Button == BUTTON_LEFT || Button == BUTTON_RIGHT)
	{
		bool selectedByObject = false;
		if (IsAppWindowObjectType(ObjectType))
		{
			const int appWindowId = ObjectType - APPWINDOW_BASE;
			const auto appWindow = appWindows.find(appWindowId);
			if (appWindow != appWindows.end() && appWindow->second != nullptr &&
				IsAppWindowVisible(this, appWindowId))
			{
				SelectAvisoViewport(this, appWindow->second.get());
				selectedByObject = true;
			}
		}
		if (!selectedByObject)
			SelectAvisoScrollTargetAtPoint(this, Pt);
	}

	if (Button != BUTTON_RIGHT)
		return;

	CInsetWindow* appWindow = VisibleAvisoViewportAtPoint(this, Pt);
	if (appWindow == nullptr)
		return;

	SelectAvisoViewport(this, appWindow);
	appWindow->BeginAvisoPan(Pt);
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::InsetPanZoom);
	RequestRefresh();
}

void CSMRRadar::OnButtonUpScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnButtonUpScreenObject",
		reinterpret_cast<std::uintptr_t>(this));
	Logger::info(string(__FUNCSIG__));
	UNREFERENCED_PARAMETER(ObjectType);
	UNREFERENCED_PARAMETER(sObjectId);
	UNREFERENCED_PARAMETER(Area);
	mouseLocation = Pt;

	if (Button == BUTTON_RIGHT)
	{
		if (EndAvisoViewportPans(this))
		{
			MarkPerformanceRefreshReason(
				VsmrPerformance::FrameRefreshReason::InsetPanZoom);
		}
		RequestRefresh();
	}
}

void CSMRRadar::OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released) {
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnMoveScreenObject",
		reinterpret_cast<std::uintptr_t>(this));
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (HandleRuntimeMenuMove(ObjectType, sObjectId, Pt, Area, Released))
	{
		mouseLocation = Pt;
		return;
	}
	const bool hasObjectId = (sObjectId != nullptr && sObjectId[0] != '\0');
	const char* objectId = hasObjectId ? sObjectId : "";
	auto isObjectId = [&](const char* expected) -> bool
	{
		return expected != nullptr && strcmp(objectId, expected) == 0;
	};
	auto setCursorState = [&](HCURSOR cursor, bool keepStandardCursor)
	{
		AFX_MANAGE_STATE(AfxGetStaticModuleState());
		ASSERT(cursor);
		ApplyRadarCursor(cursor, keepStandardCursor);
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
		RestoreRadarCursor();
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

	if (IsAppWindowObjectType(ObjectType)) {
		int appWindowId = ObjectType - APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			return;
		CInsetWindow* appWindow = appWindowIt->second.get();
		const CInsetWindow::ResizeRegion resizeRegion = ResizeRegionFromObjectId(objectId);

		if (isObjectId("topbar") || resizeRegion != CInsetWindow::ResizeRegion::None)
		{
			CRect avisoLayoutBounds = AvisoViewportLayoutBounds(this);
			appWindow->OnMoveScreenObject(sObjectId, Pt, Area, Released, &avisoLayoutBounds);
			if (Released)
			{
				RestoreInsetCursor();
				SaveInsetStateToAsrForAirport(getActiveAirport());
			}
			else if (resizeRegion != CInsetWindow::ResizeRegion::None)
			{
				ApplyResizeCursor(resizeRegion);
			}
			else
			{
				ApplyMoveCursor();
			}
			mouseLocation = Pt;
			MarkPerformanceRefreshReason(
				VsmrPerformance::FrameRefreshReason::InsetMoveResize);
			RequestRefresh();
			return;
		}

		const bool obsoleteChromeMove =
			isObjectId("window") || isObjectId("resize") ||
			isObjectId("divider") || isObjectId("divider_x") || isObjectId("divider_y");
		if (obsoleteChromeMove)
			return;

		// SRW tags remain EuroScope-moveable alongside the inset chrome objects.
		appWindow->OnMoveScreenObject(sObjectId, Pt, Area, Released);
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
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnOverScreenObject",
		reinterpret_cast<std::uintptr_t>(this));
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	UNREFERENCED_PARAMETER(Area);
	UNREFERENCED_PARAMETER(sObjectId);
	mouseLocation = Pt;
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::Hover);
	CInsetWindow* activeWindowInteraction = ActiveInsetWindowInteraction(this);
	if (activeWindowInteraction != nullptr)
	{
		if (activeWindowInteraction->IsWindowResizeActive())
			ApplyResizeCursor(activeWindowInteraction->GetActiveResizeRegion());
		else
			ApplyMoveCursor();
	}
	else if (IsAppWindowObjectType(ObjectType))
	{
		const int appWindowId = ObjectType - APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		CInsetWindow* appWindow = appWindowIt != appWindows.end() ? appWindowIt->second.get() : nullptr;
		if (appWindow != nullptr)
		{
			const CInsetWindow::ResizeRegion resizeRegion = appWindow->HitTestResize(Pt);
			if (resizeRegion != CInsetWindow::ResizeRegion::None)
				ApplyResizeCursor(resizeRegion);
			else if (appWindow->HitTestTitleBar(Pt))
				ApplyMoveCursor();
			else
				RestoreInsetCursor();
		}
		else
		{
			RestoreInsetCursor();
		}
	}
	else
	{
		RestoreInsetCursor();
	}
	CInsetWindow* activePanViewport = ActiveAvisoPanViewport(this);
	if (activePanViewport != nullptr)
	{
		if ((::GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0)
		{
			activePanViewport->EndAvisoPan();
			MarkPerformanceRefreshReason(
				VsmrPerformance::FrameRefreshReason::InsetPanZoom);
			RequestRefresh();
			return;
		}

		if (activePanViewport->UpdateAvisoPan(Pt))
		{
			MarkPerformanceRefreshReason(
				VsmrPerformance::FrameRefreshReason::InsetPanZoom);
		}
		RequestRefresh();
		return;
	}
	RequestRefresh();
}

bool CSMRRadar::HandleInsetSetCursor(HWND hwnd)
{
	if (hwnd == nullptr || !::IsWindow(hwnd) || !HasInsetRenderedInWindow(this, hwnd))
	{
		RestoreInsetCursor();
		return false;
	}

	POINT screenPoint = {};
	if (!::GetCursorPos(&screenPoint))
	{
		RestoreInsetCursor();
		return false;
	}

	auto pointForWindow = [&](CInsetWindow* insetWindow, POINT& clientPoint) -> bool
	{
		if (insetWindow == nullptr)
			return false;
		HWND renderWindow = insetWindow->m_AvisoRenderWindow;
		if (renderWindow == nullptr || !::IsWindow(renderWindow))
			renderWindow = hwnd;
		clientPoint = screenPoint;
		return renderWindow != nullptr && ::ScreenToClient(renderWindow, &clientPoint) != FALSE;
	};

	CInsetWindow* activeWindow = ActiveInsetWindowInteraction(this);
	if (activeWindow != nullptr)
	{
		POINT activePoint = {};
		const bool pointMapped = pointForWindow(activeWindow, activePoint);
		if ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
		{
			if (activeWindow->IsWindowResizeActive())
				ApplyResizeCursor(activeWindow->GetActiveResizeRegion());
			else
				ApplyMoveCursor();
			return true;
		}

		// Recover cleanly if EuroScope misses the final move callback. This only
		// ends vSMR's state machine; EuroScope keeps ownership of mouse capture.
		if (pointMapped)
		{
			CRect layoutBounds = AvisoViewportLayoutBounds(this);
			if (activeWindow->IsWindowResizeActive())
				activeWindow->EndWindowResize(activePoint, &layoutBounds);
			else
				activeWindow->EndWindowMove(activePoint, &layoutBounds);
		}
		else
		{
			activeWindow->CancelWindowInteraction();
		}
		RestoreInsetCursor();
		SaveInsetStateToAsrForAirport(getActiveAirport());
		MarkPerformanceRefreshReason(
			VsmrPerformance::FrameRefreshReason::InsetMoveResize);
		RequestRefresh();
	}

	auto applyCursorForWindow = [&](int appWindowId, CInsetWindow* insetWindow) -> int
	{
		if (insetWindow == nullptr || !IsAppWindowVisible(this, appWindowId))
			return 0;

		POINT clientPoint = {};
		if (!pointForWindow(insetWindow, clientPoint))
			return 0;
		const CInsetWindow::ResizeRegion resizeRegion = insetWindow->HitTestResize(clientPoint);
		if (resizeRegion != CInsetWindow::ResizeRegion::None)
		{
			ApplyResizeCursor(resizeRegion);
			return 1;
		}
		if (insetWindow->HitTestTitleBar(clientPoint))
		{
			ApplyMoveCursor();
			return 1;
		}

		CRect frame = insetWindow->GetWindowFrameRect();
		frame.NormalizeRect();
		if (frame.PtInRect(clientPoint))
		{
			RestoreInsetCursor();
			return 2;
		}
		return 0;
	};

	// Match the render order: the selected inset is painted last and must own
	// cursor hit-testing before any numerically higher window behind it.
	for (auto it = appWindows.rbegin(); it != appWindows.rend(); ++it)
	{
		CInsetWindow* insetWindow = it->second.get();
		if (insetWindow == nullptr || !insetWindow->m_AvisoScrollSelected)
			continue;
		const int result = applyCursorForWindow(it->first, insetWindow);
		if (result != 0)
			return result == 1;
	}
	for (auto it = appWindows.rbegin(); it != appWindows.rend(); ++it)
	{
		CInsetWindow* insetWindow = it->second.get();
		if (insetWindow != nullptr && insetWindow->m_AvisoScrollSelected)
			continue;
		const int result = applyCursorForWindow(it->first, insetWindow);
		if (result != 0)
			return result == 1;
	}

	RestoreInsetCursor();
	return false;
}

void CSMRRadar::CancelInsetWindowInteractions()
{
	bool changed = EndAvisoViewportPans(this);
	for (auto& kv : appWindows)
	{
		CInsetWindow* appWindow = kv.second.get();
		if (appWindow == nullptr)
			continue;
		if (appWindow->IsWindowMoveActive() || appWindow->IsWindowResizeActive())
			changed = true;
		appWindow->CancelWindowInteraction();
	}
	// EuroScope owns capture while moving its screen objects. Never release a
	// capture that may belong to the host or another plug-in from this cleanup.
	RestoreInsetCursor();
	if (changed)
	{
		SaveInsetStateToAsrForAirport(getActiveAirport());
		MarkPerformanceRefreshReason(
			VsmrPerformance::FrameRefreshReason::InsetMoveResize);
		RequestRefresh();
	}
}

bool CSMRRadar::HandleAvisoMouseWheel(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	if (hwnd == nullptr || !::IsWindow(hwnd))
		return false;

	const int wheelDelta = static_cast<short>(HIWORD(wParam));
	if (wheelDelta == 0)
		return false;

	POINTS wheelPoint = MAKEPOINTS(lParam);
	POINT wheelScreenPoint = { static_cast<LONG>(wheelPoint.x), static_cast<LONG>(wheelPoint.y) };
	return HandleAvisoMouseWheelAtScreenPoint(wheelScreenPoint, wheelDelta, hwnd);
}

bool CSMRRadar::HandleAvisoMouseWheelAtScreenPoint(POINT screenPoint, int wheelDelta, HWND sourceHwnd)
{
	UNREFERENCED_PARAMETER(sourceHwnd);

	if (wheelDelta == 0)
		return false;

	const double scaleMultiplier = (wheelDelta > 0) ? 1.18 : (1.0 / 1.18);

	// A viewport may only claim a wheel event when its owning radar screen is the
	// one currently on screen at the cursor. Multiple views (switched with F7) live
	// in the same EuroScope instance and share the thread-wide wheel hook, so we
	// gate on the window the viewport last rendered into matching the window under
	// the cursor. Without this, a view that is enabled but not displayed (e.g. an
	// AVISO view snapped to the bottom half) keeps stealing scrolls over the bottom
	// half of whatever view F7 has brought to the front.
	auto windowMatches = [](HWND target, HWND renderWindow) -> bool
	{
		if (target == nullptr || renderWindow == nullptr)
			return false;
		for (HWND current = target; current != nullptr && ::IsWindow(current); current = ::GetParent(current))
		{
			if (current == renderWindow)
				return true;
		}
		return false;
	};
	auto zoomViewportAtScreenPoint = [&](POINT point) -> bool
	{
		const HWND targetWindow = ::WindowFromPoint(point);
		if (targetWindow == nullptr)
			return false;

		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr)
				continue;

			POINT insetPoint = {};
			bool pointMapped = false;
			for (auto it = radar->appWindows.rbegin(); it != radar->appWindows.rend(); ++it)
			{
				const int appWindowId = it->first;
				CInsetWindow* appWindow = it->second.get();
				if (appWindow == nullptr || !IsAppWindowVisible(radar, appWindowId))
					continue;
				if (!windowMatches(targetWindow, appWindow->m_AvisoRenderWindow))
					continue;

				if (!appWindow->TryMapAvisoScreenPoint(point, insetPoint))
					continue;
				pointMapped = true;
				break;
			}
			if (!pointMapped)
				continue;

			if (IsPointInRuntimeMenuOverlay(radar, insetPoint))
				return true;
			CInsetWindow* appWindow = TopmostVisibleInsetFrameAtPoint(radar, insetPoint);
			if (appWindow == nullptr)
				continue;
			if (!appWindow->SupportsPanAndZoom())
				return true;

			mouseLocation = insetPoint;
			SelectAvisoViewport(radar, appWindow);
			if (appWindow->ZoomAvisoAtPoint(insetPoint, scaleMultiplier))
			{
				radar->MarkPerformanceRefreshReason(
					VsmrPerformance::FrameRefreshReason::InsetPanZoom);
				radar->RequestRefresh();
			}
			return true;
		}

		return false;
	};

	if (zoomViewportAtScreenPoint(screenPoint))
		return true;

	POINT cursorScreenPoint{};
	if (::GetCursorPos(&cursorScreenPoint))
	{
		if ((cursorScreenPoint.x != screenPoint.x || cursorScreenPoint.y != screenPoint.y) &&
			zoomViewportAtScreenPoint(cursorScreenPoint))
		{
			return true;
		}
	}

	return false;
}

void CSMRRadar::OnClickScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnClickScreenObject",
		reinterpret_cast<std::uintptr_t>(this));
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::UserActionExternal);
	if (HandleRuntimeMenuClick(ObjectType, sObjectId, Pt, Area, Button))
		return;
	if (ActiveRuntimeMenuPopup != RuntimeMenuPopup::None)
		CloseRuntimeMenuPopup();
	const bool hasObjectId = (sObjectId != nullptr && sObjectId[0] != '\0');
	const char* objectId = hasObjectId ? sObjectId : "";
	auto isObjectId = [&](const char* expected) -> bool
	{
		return expected != nullptr && strcmp(objectId, expected) == 0;
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
	auto openGroundStatusPopup = [&]() -> bool
	{
		CRadarTarget rt = selectAseAndGetTarget();
		if (!rt.IsValid())
			return false;
		CFlightPlan fp = rt.GetCorrelatedFlightPlan();
		if (!fp.IsValid())
			return false;

		const char* callsign = fp.GetCallsign();
		if (callsign == nullptr || callsign[0] == '\0')
			return false;
		PendingGroundStatusCallsign = callsign;
		std::transform(
			PendingGroundStatusCallsign.begin(),
			PendingGroundStatusCallsign.end(),
			PendingGroundStatusCallsign.begin(),
			[](unsigned char c) { return static_cast<char>(std::toupper(c)); });

		const CRadarTargetPositionData position = rt.GetPosition();
		const int reportedGs = position.IsValid() ? position.GetReportedGS() : 0;
		const GroundStateCategory currentStatus = classifyGroundStateForCallsign(
			callsign,
			fp.GetGroundState(),
			reportedGs,
			false);
		std::string rawStatus = fp.GetGroundState() != nullptr ? fp.GetGroundState() : "";
		rawStatus.erase(
			std::remove_if(rawStatus.begin(), rawStatus.end(), [](unsigned char c) { return std::isspace(c) != 0 || c == '-' || c == '_'; }),
			rawStatus.end());
		std::transform(rawStatus.begin(), rawStatus.end(), rawStatus.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		const bool rawTaxiIn = rawStatus == "TXIN";
		const bool rawParked = rawStatus == "PARK" || rawStatus == "PARKED";
		const std::string activeAirport = getActiveAirport();
		const char* origin = fp.GetFlightPlanData().GetOrigin();
		const char* destination = fp.GetFlightPlanData().GetDestination();
		const bool isDeparture = origin != nullptr && origin[0] != '\0' &&
			!activeAirport.empty() && _stricmp(origin, activeAirport.c_str()) == 0;
		const bool isArrival = !isDeparture && destination != nullptr && destination[0] != '\0' &&
			!activeAirport.empty() && _stricmp(destination, activeAirport.c_str()) == 0;
		openPopupListWithClose("vSMR ground status", [&]()
		{
			GetPlugIn()->AddPopupListElement("No Status", "", VSMR_GROUND_STATUS_SELECT, !rawParked && (currentStatus == GroundStateCategory::Gate || currentStatus == GroundStateCategory::Nsts || currentStatus == GroundStateCategory::Unknown));
			if (isDeparture)
			{
				GetPlugIn()->AddPopupListElement("Startup", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Stup);
				GetPlugIn()->AddPopupListElement("Push", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Push);
				GetPlugIn()->AddPopupListElement("Taxi", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Taxi);
				GetPlugIn()->AddPopupListElement("Line Up", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Lnup);
				GetPlugIn()->AddPopupListElement("Departure", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Depa);
			}
			else if (isArrival)
			{
				GetPlugIn()->AddPopupListElement("Taxi In", "", VSMR_GROUND_STATUS_SELECT, rawTaxiIn);
				GetPlugIn()->AddPopupListElement("Parked", "", VSMR_GROUND_STATUS_SELECT, rawParked);
				GetPlugIn()->AddPopupListElement("Arrival", "", VSMR_GROUND_STATUS_SELECT, currentStatus == GroundStateCategory::Arr);
			}
		});
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
			return 0;
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
	if (Button == BUTTON_LEFT || Button == BUTTON_RIGHT)
		SelectAvisoScrollTargetAtPoint(this, Pt);
	if (Button == BUTTON_RIGHT &&
		!IsPointInRuntimeMenuOverlay(this, Pt) &&
		VisibleAvisoViewportAtPoint(this, Pt) != nullptr)
	{
		RequestRefresh();
		return;
	}

	if (IsAppWindowObjectType(ObjectType)) {
		int appWindowId = ObjectType - APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		CInsetWindow* appWindow = (appWindowIt != appWindows.end() && appWindowIt->second != nullptr) ? appWindowIt->second.get() : nullptr;
		if (appWindow != nullptr && appWindow->IsTimer() &&
			strncmp(objectId, "timer.", 6) == 0)
		{
			if (Button == BUTTON_LEFT || Button == BUTTON_RIGHT)
			{
				appWindow->OnClickScreenObject(objectId, Pt, Button);
				RequestRefresh();
			}
			return;
		}
		if (Button != BUTTON_LEFT)
			return;
		
		if (isObjectId("close"))
		{
			if (appWindow == nullptr)
				return;

			auto appWindowDisplayIt = appWindowDisplays.find(appWindowId);
			if (appWindowDisplayIt != appWindowDisplays.end())
			{
				appWindowDisplayIt->second = false;
				appWindow->ResetAvisoInteractionState();
				SaveInsetStateToAsrForAirport(getActiveAirport());
				if (VsmrControlCenterDialog != nullptr)
					VsmrControlCenterDialog->SyncFromRadar();
			}
			RequestRefresh();
			return;
		}
		if (isObjectId("filter")) {
			if (appWindow == nullptr)
				return;
			if (!appWindow->IsSecondaryRadar())
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

		if (ObjectType == DRAWING_AC_SYMBOL)
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

	if (ObjectType == DRAWING_AC_SYMBOL_APPWINDOW1 ||
		ObjectType == DRAWING_AC_SYMBOL_APPWINDOW3)
	{
		const int appWindowId = ObjectType - DRAWING_AC_SYMBOL_APPWINDOW_BASE;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr)
			appWindowIt->second->OnClickScreenObject(objectId, Pt, Button);
	}

	if (Button == BUTTON_LEFT) {
		if (ObjectType == TAG_CITEM_CALLSIGN) {
			// Keep the standard ground states and vSMR's session-local LNUP state
			// in one menu so every selection consistently clears or sets LNUP.
			(void)openGroundStatusPopup();
		}
		else if (ObjectType == TAG_CITEM_GROUNDSTATUS) {
			(void)openGroundStatusPopup();
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
	if (Button == BUTTON_RIGHT && ObjectType == TAG_CITEM_GROUNDSTATUS) {
		(void)openGroundStatusPopup();
		return;
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

	RequestRefresh();
};

