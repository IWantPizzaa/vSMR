#include "stdafx.h"
#include "SMRRadar.hpp"
#include "VsmrControlCenterDialog.hpp"

#include <cctype>

namespace
{
	CRect BuildDefaultControlCenterWindowRect()
	{
		CRect fallback(90, 90, 1180, 790);
		CWnd* mainWindow = AfxGetMainWnd();
		if (mainWindow != nullptr && ::IsWindow(mainWindow->GetSafeHwnd()))
		{
			CRect mainRect;
			mainWindow->GetWindowRect(&mainRect);
			if (!mainRect.IsRectEmpty())
			{
				fallback.left = mainRect.left + 70;
				fallback.top = mainRect.top + 70;
				fallback.right = fallback.left + 1090;
				fallback.bottom = fallback.top + 700;
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

		if (normalized == "profile" || normalized == "profiles" || normalized == "tags" || normalized == "tag")
			return CVsmrControlCenterDialog::Page::Profiles;
		if (normalized == "aviso" || normalized == "geojson")
			return CVsmrControlCenterDialog::Page::Aviso;
		if (normalized == "maps" || normalized == "map")
			return CVsmrControlCenterDialog::Page::Maps;
		if (normalized == "settings" || normalized == "config")
			return CVsmrControlCenterDialog::Page::Settings;
		return CVsmrControlCenterDialog::Page::Overview;
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

	VsmrControlCenterDialog = std::make_unique<CVsmrControlCenterDialog>(this, AfxGetMainWnd());
	if (!VsmrControlCenterDialog->Create(CVsmrControlCenterDialog::IDD, AfxGetMainWnd()))
	{
		VsmrControlCenterDialog.reset();
		return false;
	}

	const CRect windowRect = BuildDefaultControlCenterWindowRect();
	VsmrControlCenterDialog->SetWindowPos(
		nullptr,
		windowRect.left,
		windowRect.top,
		max(880, windowRect.Width()),
		max(620, windowRect.Height()),
		SWP_NOZORDER | SWP_NOACTIVATE);
	VsmrControlCenterDialog->ShowWindow(SW_HIDE);
	return true;
}

void CSMRRadar::OpenVsmrControlCenterWindow()
{
	OpenVsmrControlCenterWindow("overview");
}

void CSMRRadar::OpenVsmrControlCenterWindow(const std::string& pageName)
{
	if (!EnsureVsmrControlCenterWindowCreated())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Control Center", "Failed to open vSMR window.", true, true, false, false, false);
		RequestRefresh();
		return;
	}

	VsmrControlCenterDialog->ShowWindow(SW_SHOW);
	VsmrControlCenterDialog->BringWindowToTop();
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
