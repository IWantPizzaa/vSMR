#include "stdafx.h"
#include "SMRRadar.hpp"
#include "VsmrControlCenterDialog.hpp"

#include <cctype>

namespace
{
	CRect BuildDefaultControlCenterWindowRect(CWnd* euroScopeWindow)
	{
		constexpr int defaultWidth = 728;
		constexpr int defaultHeight = 500;
		CRect fallback(90, 90, 90 + defaultWidth, 90 + defaultHeight);
		if (euroScopeWindow != nullptr &&
			::IsWindow(euroScopeWindow->GetSafeHwnd()))
		{
			CRect mainRect;
			euroScopeWindow->GetClientRect(&mainRect);
			euroScopeWindow->ClientToScreen(&mainRect);
			if (!mainRect.IsRectEmpty())
			{
				fallback.left = mainRect.left + max(24, (mainRect.Width() - defaultWidth) / 2);
				fallback.top = mainRect.top + max(24, (mainRect.Height() - defaultHeight) / 2);
				fallback.right = fallback.left + defaultWidth;
				fallback.bottom = fallback.top + defaultHeight;
			}
		}
		return fallback;
	}

	CVsmrControlCenterDialog::Page PageFromName(const std::string& pageName)
	{
		std::string normalized = pageName;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		if (normalized == "display" || normalized == "colors" || normalized == "icons" ||
			normalized == "tags" || normalized == "tag" || normalized == "overview")
			return CVsmrControlCenterDialog::Page::Display;
		if (normalized == "profile" || normalized == "profiles")
			return CVsmrControlCenterDialog::Page::Profiles;
		if (normalized == "aviso" || normalized == "geojson")
			return CVsmrControlCenterDialog::Page::Aviso;
		if (normalized == "alerts" || normalized == "rimcas")
			return CVsmrControlCenterDialog::Page::Alerts;
		if (normalized == "groups" || normalized == "group")
			return CVsmrControlCenterDialog::Page::Groups;
		if (normalized == "modes" || normalized == "mode")
			return CVsmrControlCenterDialog::Page::Modes;
		if (normalized == "datalink" || normalized == "cpdlc" ||
			normalized == "cdm")
			return CVsmrControlCenterDialog::Page::Settings;
		if (normalized == "settings" || normalized == "config" ||
			normalized == "maps" || normalized == "map")
			return CVsmrControlCenterDialog::Page::Settings;
		return CVsmrControlCenterDialog::Page::Display;
	}
}

bool CSMRRadar::EnsureVsmrControlCenterWindowCreated()
{
	if (VsmrControlCenterDialog && ::IsWindow(VsmrControlCenterDialog->GetSafeHwnd()))
	{
		VsmrControlCenterDialog->SetOwner(this);
		return true;
	}

	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	CWnd* candidateWindow = AfxGetMainWnd();
	if (candidateWindow == nullptr ||
		!::IsWindow(candidateWindow->GetSafeHwnd()))
	{
		return false;
	}
	HWND euroScopeHwnd = ::GetAncestor(
		candidateWindow->GetSafeHwnd(),
		GA_ROOTOWNER);
	if (!::IsWindow(euroScopeHwnd))
		return false;
	CWnd* euroScopeWindow = CWnd::FromHandle(euroScopeHwnd);
	if (euroScopeWindow == nullptr)
		return false;

	VsmrControlCenterDialog = std::make_unique<CVsmrControlCenterDialog>(
		this,
		euroScopeWindow);
	if (!VsmrControlCenterDialog->Create(
		CVsmrControlCenterDialog::IDD,
		euroScopeWindow))
	{
		VsmrControlCenterDialog.reset();
		return false;
	}

	VsmrControlCenterDialog->RestoreWindowPlacementOrDefault(
		BuildDefaultControlCenterWindowRect(euroScopeWindow));
	VsmrControlCenterDialog->ShowWindow(SW_HIDE);
	return true;
}

void CSMRRadar::OpenVsmrControlCenterWindow()
{
	OpenVsmrControlCenterWindow("display");
}

void CSMRRadar::OpenVsmrControlCenterWindow(const std::string& pageName)
{
	if (!EnsureVsmrControlCenterWindowCreated())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Control Center", "Failed to open vSMR window.", true, true, false, false, false);
		RequestRefresh();
		return;
	}

	VsmrControlCenterDialog->ConstrainToEuroScopeWindow();
	VsmrControlCenterDialog->SetWindowPos(
		&CWnd::wndTop,
		0,
		0,
		0,
		0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
	VsmrControlCenterDialog->ShowPage(PageFromName(pageName));
	VsmrControlCenterDialog->SyncFromRadar();
	RequestRefresh();
}

void CSMRRadar::CloseVsmrControlCenterWindow()
{
	if (!VsmrControlCenterDialog || !::IsWindow(VsmrControlCenterDialog->GetSafeHwnd()))
		return;

	VsmrControlCenterDialog->ShowWindow(SW_HIDE);
}

void CSMRRadar::DestroyVsmrControlCenterWindow()
{
	if (!VsmrControlCenterDialog)
		return;

	if (::IsWindow(VsmrControlCenterDialog->GetSafeHwnd()))
		VsmrControlCenterDialog->DestroyWindow();

	VsmrControlCenterDialog.reset();
}

void CSMRRadar::OnVsmrControlCenterWindowClosed()
{
	RequestRefresh();
}
