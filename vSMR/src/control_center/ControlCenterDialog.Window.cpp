#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterDialog.Internal.hpp"

#include "control_center/ControlCenterBridge.hpp"
#include "radar/RadarScreen.hpp"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

using namespace VsmrControlCenterDialogInternal;

namespace
{
	std::wstring LocalAppDataPath()
	{
		wchar_t buffer[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(
			L"LOCALAPPDATA",
			buffer,
			static_cast<DWORD>(std::size(buffer)));
		if (length > 0 && length < std::size(buffer))
			return std::wstring(buffer, length);

		const DWORD tempLength = ::GetTempPathW(
			static_cast<DWORD>(std::size(buffer)),
			buffer);
		if (tempLength > 0 && tempLength < std::size(buffer))
			return std::wstring(buffer, tempLength);
		return L".";
	}

	bool TryGetEuroScopeClientBounds(HWND dialogWindow, CRect& bounds)
	{
		bounds.SetRectEmpty();
		if (!::IsWindow(dialogWindow))
			return false;

		const HWND ownerWindow = ::GetWindow(dialogWindow, GW_OWNER);
		if (!::IsWindow(ownerWindow))
			return false;

		RECT client = {};
		if (!::GetClientRect(ownerWindow, &client))
			return false;
		POINT topLeft = { client.left, client.top };
		POINT bottomRight = { client.right, client.bottom };
		if (!::ClientToScreen(ownerWindow, &topLeft) ||
			!::ClientToScreen(ownerWindow, &bottomRight))
		{
			return false;
		}
		bounds = CRect(topLeft, bottomRight);
		bounds.NormalizeRect();
		return !bounds.IsRectEmpty();
	}

	CRect ClampControlCenterRectToEuroScope(
		HWND dialogWindow,
		const CRect& requested)
	{
		CRect available;
		if (!TryGetEuroScopeClientBounds(dialogWindow, available))
		{
			RECT requestedRect = requested;
			const HMONITOR monitor = ::MonitorFromRect(
				&requestedRect,
				MONITOR_DEFAULTTONEAREST);
			MONITORINFO monitorInfo = {};
			monitorInfo.cbSize = sizeof(monitorInfo);
			if (monitor != nullptr &&
				::GetMonitorInfoW(monitor, &monitorInfo))
			{
				available = CRect(monitorInfo.rcWork);
			}
			else
			{
				return CRect(
					requested.left,
					requested.top,
					requested.left + kFixedWindowWidth,
					requested.top + kFixedWindowHeight);
			}
		}

		const int left = (std::clamp)(
			requested.left,
			available.left,
			(std::max)(available.left, available.right - kFixedWindowWidth));
		const int top = (std::clamp)(
			requested.top,
			available.top,
			(std::max)(available.top, available.bottom - kFixedWindowHeight));
		return CRect(
			left,
			top,
			left + kFixedWindowWidth,
			top + kFixedWindowHeight);
	}

	bool WriteTextFileAtomically(
		const std::filesystem::path& path,
		const std::string& text)
	{
		try
		{
			if (path.has_parent_path())
				std::filesystem::create_directories(path.parent_path());
			std::filesystem::path temp = path;
			temp += L".tmp." + std::to_wstring(::GetCurrentProcessId());
			{
				std::ofstream output(
					temp,
					std::ios::binary | std::ios::trunc);
				if (!output.is_open())
					return false;
				output.write(
					text.data(),
					static_cast<std::streamsize>(text.size()));
				output.flush();
				output.close();
				if (!output)
				{
					std::error_code ignored;
					std::filesystem::remove(temp, ignored);
					return false;
				}
			}
			if (!::MoveFileExW(
				temp.c_str(),
				path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				std::error_code ignored;
				std::filesystem::remove(temp, ignored);
				return false;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}

void CVsmrControlCenterDialog::BeginNativeWindowDrag()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;
	if (WebHost)
	{
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
		{
			::PostMessage(
				threadWindow,
				kWebViewBeginWindowDragMessage,
				0,
				0);
			return;
		}
	}
	::ReleaseCapture();
	SendMessage(WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

std::wstring CVsmrControlCenterDialog::WebViewUserDataFolder() const
{
	return (
		std::filesystem::path(LocalAppDataPath()) /
		"vSMR" /
		"WebView2").wstring();
}

std::wstring CVsmrControlCenterDialog::WindowPlacementPath() const
{
	return (
		std::filesystem::path(LocalAppDataPath()) /
		"vSMR" /
		"control-center-window.json").wstring();
}

void CVsmrControlCenterDialog::RestoreWindowPlacementOrDefault(
	const CRect& fallback)
{
	CRect requested(
		fallback.left,
		fallback.top,
		fallback.left + kFixedWindowWidth,
		fallback.top + kFixedWindowHeight);
	std::string text;
	const std::filesystem::path path(WindowPlacementPath());
	if (ReadTextFile(path, text, kMaximumWindowPlacementBytes))
	{
		rapidjson::Document document;
		document.Parse<0>(text.c_str());
		if (!document.HasParseError() && document.IsObject())
		{
			auto readInt = [&](const char* key, int fallbackValue)
			{
				return document.HasMember(key) && document[key].IsInt()
					? document[key].GetInt()
					: fallbackValue;
			};
			requested.left = readInt("x", fallback.left);
			requested.top = readInt("y", fallback.top);
			requested.right = requested.left + kFixedWindowWidth;
			requested.bottom = requested.top + kFixedWindowHeight;
		}
	}

	requested = ClampControlCenterRectToEuroScope(
		GetSafeHwnd(),
		requested);
	SetWindowPos(
		nullptr,
		requested.left,
		requested.top,
		requested.Width(),
		requested.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE);
	WindowPlacementDirty = false;
}

void CVsmrControlCenterDialog::ConstrainToEuroScopeWindow()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;

	CRect current;
	GetWindowRect(&current);
	const CRect constrained = ClampControlCenterRectToEuroScope(
		GetSafeHwnd(),
		current);
	if (current == constrained)
		return;

	SetWindowPos(
		nullptr,
		constrained.left,
		constrained.top,
		constrained.Width(),
		constrained.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE);
}

void CVsmrControlCenterDialog::SaveWindowPlacement()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;
	CRect window;
	GetWindowRect(&window);
	if (window.IsRectEmpty())
		return;

	rapidjson::Document document;
	document.SetObject();
	document.AddMember("x", window.left, document.GetAllocator());
	document.AddMember("y", window.top, document.GetAllocator());
	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	document.Accept(writer);
	if (WriteTextFileAtomically(
		std::filesystem::path(WindowPlacementPath()),
		std::string(buffer.GetString(), buffer.Size())))
		WindowPlacementDirty = false;
}

void CVsmrControlCenterDialog::OnClose()
{
	SaveWindowPlacement();
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnVsmrControlCenterWindowClosed();
}

void CVsmrControlCenterDialog::OnCancel()
{
	OnClose();
}

void CVsmrControlCenterDialog::OnOK()
{
	// Enter belongs to the focused editor, not to the modeless host dialog.
}

void CVsmrControlCenterDialog::OnDestroy()
{
	Closing = true;
	KillTimer(kEuroScopeBoundsTimerId);
	if (LifetimeToken)
		LifetimeToken->store(false);
	SaveWindowPlacement();
	StopGithubDownload();
	StopWebViewThread();
	Bridge.reset();
	CDialogEx::OnDestroy();
}

void CVsmrControlCenterDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (nType != SIZE_MINIMIZED)
	{
		WindowPlacementDirty = true;
		ResizeWebView();
	}
}

void CVsmrControlCenterDialog::OnMove(int x, int y)
{
	CDialogEx::OnMove(x, y);
	if (!Closing)
	{
		WindowPlacementDirty = true;
		if (WebHost)
		{
			const HWND threadWindow = WebHost->threadWindow.load();
			if (::IsWindow(threadWindow))
				::PostMessage(
					threadWindow,
					kWebViewParentMovedMessage,
					0,
					0);
		}
	}
}

void CVsmrControlCenterDialog::OnMoving(UINT fwSide, LPRECT pRect)
{
	CDialogEx::OnMoving(fwSide, pRect);
	if (pRect == nullptr)
		return;

	const CRect requested(
		pRect->left,
		pRect->top,
		pRect->left + kFixedWindowWidth,
		pRect->top + kFixedWindowHeight);
	const CRect constrained = ClampControlCenterRectToEuroScope(
		GetSafeHwnd(),
		requested);
	*pRect = constrained;
}

void CVsmrControlCenterDialog::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == kEuroScopeBoundsTimerId && IsWindowVisible())
		ConstrainToEuroScopeWindow();
	CDialogEx::OnTimer(nIDEvent);
}

void CVsmrControlCenterDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI != nullptr)
	{
		lpMMI->ptMinTrackSize.x = kFixedWindowWidth;
		lpMMI->ptMinTrackSize.y = kFixedWindowHeight;
		lpMMI->ptMaxTrackSize.x = kFixedWindowWidth;
		lpMMI->ptMaxTrackSize.y = kFixedWindowHeight;
	}
}
