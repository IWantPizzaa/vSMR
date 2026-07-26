#include "stdafx.h"
#include "VsmrControlCenterDialog.hpp"
#include "SMRRadar.hpp"
#include "ProfileEditorDialog.hpp"
#include "AvisoEditorDialog.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <shellapi.h>

IMPLEMENT_DYNAMIC(CVsmrControlCenterDialog, CDialogEx)

namespace
{
	constexpr int kSidebarWidth = 178;
	constexpr int kBottomStatusHeight = 30;
	constexpr int kOuterPad = 18;
	constexpr int kControlGap = 8;
	constexpr int kPageTitleHeight = 50;

	std::string CStringToStdString(const CString& value)
	{
		return std::string(static_cast<LPCSTR>(value));
	}

	bool IsShellExecuteSuccess(HINSTANCE result)
	{
		return reinterpret_cast<INT_PTR>(result) > 32;
	}
}

CVsmrControlCenterDialog::CVsmrControlCenterDialog(CSMRRadar* owner, CWnd* parent)
	: CDialogEx(CVsmrControlCenterDialog::IDD, parent),
	Owner(owner)
{
}

CVsmrControlCenterDialog::~CVsmrControlCenterDialog()
{
}

void CVsmrControlCenterDialog::SetOwner(CSMRRadar* owner)
{
	Owner = owner;
}

void CVsmrControlCenterDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BOOL CVsmrControlCenterDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetWindowTextA("vSMR");
	CreateControls();
	ControlsCreated = true;
	ShowPage(Page::Overview);
	LayoutControls();
	return TRUE;
}

void CVsmrControlCenterDialog::CreateButton(CButton& button, const char* text, UINT id)
{
	button.Create(
		text,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		CRect(0, 0, 0, 0),
		this,
		id);
}

void CVsmrControlCenterDialog::CreateStatic(CStatic& label, const char* text)
{
	label.Create(text, WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
}

void CVsmrControlCenterDialog::CreateReadOnlyEdit(CEdit& edit)
{
	edit.Create(
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		CRect(0, 0, 0, 0),
		this,
		0);
}

void CVsmrControlCenterDialog::CreateControls()
{
	CreateStatic(NavigationTitleLabel, "vSMR Control Center");
	CreateStatic(PageTitleLabel, "");
	CreateStatic(PageSubtitleLabel, "");
	CreateStatic(StatusLabel, "");
	CreateStatic(MapsPathLabel, "vSMR_Maps.json");

	CreateButton(NavOverviewButton, "Overview", IDC_VCC_NAV_OVERVIEW);
	CreateButton(NavProfilesButton, "Profiles", IDC_VCC_NAV_PROFILES);
	CreateButton(NavAvisoButton, "AVISO", IDC_VCC_NAV_AVISO);
	CreateButton(NavMapsButton, "Maps", IDC_VCC_NAV_MAPS);
	CreateButton(NavSettingsButton, "Settings", IDC_VCC_NAV_SETTINGS);

	CreateButton(OverviewProfilesButton, "Edit Profiles", IDC_VCC_OVERVIEW_PROFILES);
	CreateButton(OverviewAvisoButton, "Edit AVISO", IDC_VCC_OVERVIEW_AVISO);
	CreateButton(OverviewMapsButton, "Edit Maps", IDC_VCC_OVERVIEW_MAPS);
	CreateButton(ReloadConfigButton, "Reload vSMR", IDC_VCC_RELOAD_CONFIG);
	CreateButton(ReloadAvisoButton, "Reload AVISO", IDC_VCC_RELOAD_AVISO);
	CreateButton(OpenMapsExternalButton, "Open file", IDC_VCC_OPEN_MAPS_EXTERNAL);
	CreateButton(ReloadMapsTextButton, "Reload", IDC_VCC_RELOAD_MAPS_TEXT);
	CreateButton(SaveMapsTextButton, "Save and reload", IDC_VCC_SAVE_MAPS_TEXT);
	CreateButton(OpenConfigExternalButton, "Open profiles JSON", IDC_VCC_OPEN_CONFIG_EXTERNAL);
	CreateButton(OpenDataFolderButton, "Open data folder", IDC_VCC_OPEN_DATA_FOLDER);
	CreateButton(OpenPluginFolderButton, "Open plugin folder", IDC_VCC_OPEN_PLUGIN_FOLDER);

	CreateReadOnlyEdit(OverviewEdit);
	CreateReadOnlyEdit(SettingsEdit);

	MapsPathEdit.Create(
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
		CRect(0, 0, 0, 0),
		this,
		0);
	MapsRawEdit.Create(
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
		CRect(0, 0, 0, 0),
		this,
		0);
}

void CVsmrControlCenterDialog::ShowPage(Page page)
{
	CurrentPage = page;
	HidePageControls();
	HideHostedEditors();
	UpdateNavState();

	switch (CurrentPage)
	{
	case Page::Overview:
		SetPageText("Overview", "Current airport, profile, AVISO, and the most common workflow actions.");
		OverviewEdit.SetWindowTextA(BuildOverviewText().c_str());
		OverviewEdit.ShowWindow(SW_SHOW);
		OverviewProfilesButton.ShowWindow(SW_SHOW);
		OverviewAvisoButton.ShowWindow(SW_SHOW);
		OverviewMapsButton.ShowWindow(SW_SHOW);
		ReloadConfigButton.ShowWindow(SW_SHOW);
		ReloadAvisoButton.ShowWindow(SW_SHOW);
		break;
	case Page::Profiles:
		SetPageText("Profiles", "Edit profile colors, target icons, tag rules, tag definitions, and profile modes.");
		if (!HostProfileEditor())
			SetStatusText("Unable to host the Profile editor.");
		break;
	case Page::Aviso:
		SetPageText("AVISO", "Edit AVISO GeoJSON objects, styles, labels, filters, and imported variants.");
		if (!HostAvisoEditor())
			SetStatusText("Unable to host the AVISO editor.");
		break;
	case Page::Maps:
		SetPageText("Maps", "Edit vSMR_Maps.json directly, then save and reload vSMR.");
		MapsPathLabel.ShowWindow(SW_SHOW);
		MapsPathEdit.ShowWindow(SW_SHOW);
		MapsRawEdit.ShowWindow(SW_SHOW);
		OpenMapsExternalButton.ShowWindow(SW_SHOW);
		ReloadMapsTextButton.ShowWindow(SW_SHOW);
		SaveMapsTextButton.ShowWindow(SW_SHOW);
		LoadMapsText();
		break;
	case Page::Settings:
		SetPageText("Settings", "Central actions for reloads, folders, and runtime state.");
		SettingsEdit.SetWindowTextA(BuildSettingsText().c_str());
		SettingsEdit.ShowWindow(SW_SHOW);
		ReloadConfigButton.ShowWindow(SW_SHOW);
		ReloadAvisoButton.ShowWindow(SW_SHOW);
		OpenConfigExternalButton.ShowWindow(SW_SHOW);
		OpenMapsExternalButton.ShowWindow(SW_SHOW);
		OpenDataFolderButton.ShowWindow(SW_SHOW);
		OpenPluginFolderButton.ShowWindow(SW_SHOW);
		break;
	}

	LayoutControls();
}

void CVsmrControlCenterDialog::SyncFromRadar()
{
	if (CurrentPage == Page::Overview)
		OverviewEdit.SetWindowTextA(BuildOverviewText().c_str());
	else if (CurrentPage == Page::Settings)
		SettingsEdit.SetWindowTextA(BuildSettingsText().c_str());
	else if (CurrentPage == Page::Maps)
		MapsPathEdit.SetWindowTextA(Owner != nullptr ? Owner->mapsPath.c_str() : "");
	else if (CurrentPage == Page::Profiles && Owner != nullptr && Owner->ProfileEditorDialog)
		Owner->ProfileEditorDialog->SyncFromRadar();
	else if (CurrentPage == Page::Aviso && Owner != nullptr && Owner->AvisoEditorDialog)
		Owner->AvisoEditorDialog->SyncFromRadar();
}

void CVsmrControlCenterDialog::HidePageControls()
{
	CWnd* controls[] = {
		&OverviewEdit,
		&SettingsEdit,
		&MapsPathLabel,
		&MapsPathEdit,
		&MapsRawEdit,
		&OverviewProfilesButton,
		&OverviewAvisoButton,
		&OverviewMapsButton,
		&ReloadConfigButton,
		&ReloadAvisoButton,
		&OpenMapsExternalButton,
		&ReloadMapsTextButton,
		&SaveMapsTextButton,
		&OpenConfigExternalButton,
		&OpenDataFolderButton,
		&OpenPluginFolderButton
	};

	for (CWnd* control : controls)
	{
		if (control != nullptr && ::IsWindow(control->GetSafeHwnd()))
			control->ShowWindow(SW_HIDE);
	}
}

void CVsmrControlCenterDialog::HideHostedEditors()
{
	if (Owner == nullptr)
		return;
	if (Owner->ProfileEditorDialog && ::IsWindow(Owner->ProfileEditorDialog->GetSafeHwnd()))
		Owner->ProfileEditorDialog->ShowWindow(SW_HIDE);
	if (Owner->AvisoEditorDialog && ::IsWindow(Owner->AvisoEditorDialog->GetSafeHwnd()))
		Owner->AvisoEditorDialog->ShowWindow(SW_HIDE);
}

void CVsmrControlCenterDialog::UpdateNavState()
{
	auto setNavText = [&](CButton& button, Page page, const char* label)
	{
		const std::string text = (CurrentPage == page ? "> " : "  ") + std::string(label);
		button.SetWindowTextA(text.c_str());
		button.EnableWindow(CurrentPage != page);
	};

	setNavText(NavOverviewButton, Page::Overview, "Overview");
	setNavText(NavProfilesButton, Page::Profiles, "Profiles");
	setNavText(NavAvisoButton, Page::Aviso, "AVISO");
	setNavText(NavMapsButton, Page::Maps, "Maps");
	setNavText(NavSettingsButton, Page::Settings, "Settings");
}

void CVsmrControlCenterDialog::SetStatusText(const std::string& text)
{
	StatusLabel.SetWindowTextA(text.c_str());
}

bool CVsmrControlCenterDialog::IsHostedEditorPage() const
{
	return CurrentPage == Page::Profiles || CurrentPage == Page::Aviso;
}

void CVsmrControlCenterDialog::SetPageText(const std::string& title, const std::string& subtitle)
{
	PageTitleLabel.SetWindowTextA(title.c_str());
	PageSubtitleLabel.SetWindowTextA(subtitle.c_str());
	const int showState = IsHostedEditorPage() ? SW_HIDE : SW_SHOW;
	PageTitleLabel.ShowWindow(showState);
	PageSubtitleLabel.ShowWindow(showState);
}

CRect CVsmrControlCenterDialog::ContentRect() const
{
	CRect client;
	const_cast<CVsmrControlCenterDialog*>(this)->GetClientRect(&client);
	const int contentTop = IsHostedEditorPage()
		? kOuterPad
		: kOuterPad + kPageTitleHeight;
	return CRect(
		kSidebarWidth + kOuterPad,
		contentTop,
		client.right - kOuterPad,
		client.bottom - kBottomStatusHeight - kOuterPad);
}

void CVsmrControlCenterDialog::LayoutControls()
{
	if (!ControlsCreated)
		return;

	CRect client;
	GetClientRect(&client);
	const int navLeft = 18;
	const int navWidth = kSidebarWidth - 36;
	NavigationTitleLabel.MoveWindow(navLeft, 18, navWidth, 18, TRUE);
	int navY = 76;
	const int navHeight = 34;
	auto moveNav = [&](CButton& button)
	{
		button.MoveWindow(navLeft, navY, navWidth, navHeight, TRUE);
		navY += navHeight + 8;
	};
	moveNav(NavOverviewButton);
	moveNav(NavProfilesButton);
	moveNav(NavAvisoButton);
	moveNav(NavMapsButton);
	moveNav(NavSettingsButton);

	PageTitleLabel.MoveWindow(kSidebarWidth + kOuterPad, kOuterPad, 240, 22, TRUE);
	PageSubtitleLabel.MoveWindow(kSidebarWidth + kOuterPad, kOuterPad + 24, max(100, client.Width() - kSidebarWidth - (kOuterPad * 2)), 18, TRUE);
	StatusLabel.MoveWindow(kSidebarWidth + kOuterPad, client.bottom - 24, max(100, client.Width() - kSidebarWidth - (kOuterPad * 2)), 18, TRUE);

	const CRect content = ContentRect();
	const int buttonWidth = 126;
	const int buttonHeight = 28;
	const int buttonTop = content.top;
	const int editTop = buttonTop + buttonHeight + 12;

	if (CurrentPage == Page::Overview)
	{
		OverviewProfilesButton.MoveWindow(content.left, buttonTop, buttonWidth, buttonHeight, TRUE);
		OverviewAvisoButton.MoveWindow(content.left + buttonWidth + kControlGap, buttonTop, buttonWidth, buttonHeight, TRUE);
		OverviewMapsButton.MoveWindow(content.left + ((buttonWidth + kControlGap) * 2), buttonTop, buttonWidth, buttonHeight, TRUE);
		ReloadConfigButton.MoveWindow(content.left + ((buttonWidth + kControlGap) * 3), buttonTop, buttonWidth, buttonHeight, TRUE);
		ReloadAvisoButton.MoveWindow(content.left + ((buttonWidth + kControlGap) * 4), buttonTop, buttonWidth, buttonHeight, TRUE);
		OverviewEdit.MoveWindow(content.left, editTop, content.Width(), max(40, content.bottom - editTop), TRUE);
	}
	else if (CurrentPage == Page::Maps)
	{
		MapsPathLabel.MoveWindow(content.left, content.top, 110, 18, TRUE);
		MapsPathEdit.MoveWindow(content.left + 116, content.top - 2, max(100, content.Width() - 116), 22, TRUE);
		const int mapsButtonTop = content.top + 30;
		OpenMapsExternalButton.MoveWindow(content.left, mapsButtonTop, 98, buttonHeight, TRUE);
		ReloadMapsTextButton.MoveWindow(content.left + 106, mapsButtonTop, 98, buttonHeight, TRUE);
		SaveMapsTextButton.MoveWindow(content.left + 212, mapsButtonTop, 132, buttonHeight, TRUE);
		MapsRawEdit.MoveWindow(content.left, mapsButtonTop + buttonHeight + 12, content.Width(), max(40, content.bottom - (mapsButtonTop + buttonHeight + 12)), TRUE);
	}
	else if (CurrentPage == Page::Settings)
	{
		ReloadConfigButton.MoveWindow(content.left, buttonTop, 112, buttonHeight, TRUE);
		ReloadAvisoButton.MoveWindow(content.left + 120, buttonTop, 112, buttonHeight, TRUE);
		OpenConfigExternalButton.MoveWindow(content.left + 240, buttonTop, 138, buttonHeight, TRUE);
		OpenMapsExternalButton.MoveWindow(content.left + 386, buttonTop, 98, buttonHeight, TRUE);
		OpenDataFolderButton.MoveWindow(content.left, buttonTop + buttonHeight + 10, 132, buttonHeight, TRUE);
		OpenPluginFolderButton.MoveWindow(content.left + 140, buttonTop + buttonHeight + 10, 132, buttonHeight, TRUE);
		SettingsEdit.MoveWindow(content.left, buttonTop + (buttonHeight * 2) + 24, content.Width(), max(40, content.bottom - (buttonTop + (buttonHeight * 2) + 24)), TRUE);
	}

	LayoutHostedEditors();
}

void CVsmrControlCenterDialog::LayoutHostedEditors()
{
	const CRect content = ContentRect();
	if (CurrentPage == Page::Profiles &&
		Owner != nullptr &&
		Owner->ProfileEditorDialog &&
		::IsWindow(Owner->ProfileEditorDialog->GetSafeHwnd()))
	{
		Owner->ProfileEditorDialog->MoveWindow(content, TRUE);
	}
	else if (CurrentPage == Page::Aviso &&
		Owner != nullptr &&
		Owner->AvisoEditorDialog &&
		::IsWindow(Owner->AvisoEditorDialog->GetSafeHwnd()))
	{
		Owner->AvisoEditorDialog->MoveWindow(content, TRUE);
	}
}

bool CVsmrControlCenterDialog::HostProfileEditor()
{
	if (Owner == nullptr || !Owner->EnsureProfileEditorWindowCreated() || !Owner->ProfileEditorDialog)
		return false;

	Owner->ProfileEditorDialog->SetOwner(Owner);
	PrepareHostedDialog(Owner->ProfileEditorDialog.get(), ContentRect());
	Owner->ProfileEditorDialog->SyncFromRadar();
	SetStatusText("Profile editor loaded in vSMR.");
	return true;
}

bool CVsmrControlCenterDialog::HostAvisoEditor()
{
	if (Owner == nullptr || !Owner->EnsureAvisoEditorWindowCreated() || !Owner->AvisoEditorDialog)
		return false;

	Owner->AvisoEditorDialog->SetOwner(Owner);
	PrepareHostedDialog(Owner->AvisoEditorDialog.get(), ContentRect());
	Owner->AvisoEditorDialog->SyncFromRadar();
	SetStatusText("AVISO editor loaded in vSMR.");
	return true;
}

void CVsmrControlCenterDialog::PrepareHostedDialog(CDialogEx* dialog, const CRect& targetRect)
{
	if (dialog == nullptr || !::IsWindow(dialog->GetSafeHwnd()))
		return;

	dialog->ShowWindow(SW_HIDE);
	dialog->SetParent(this);
	dialog->ModifyStyle(
		WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
		WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		SWP_FRAMECHANGED);
	dialog->ModifyStyleEx(WS_EX_APPWINDOW, WS_EX_CONTROLPARENT, SWP_FRAMECHANGED);
	dialog->MoveWindow(targetRect, TRUE);
	dialog->ShowWindow(SW_SHOW);
}

void CVsmrControlCenterDialog::LoadMapsText()
{
	if (Owner == nullptr || Owner->mapsPath.empty())
	{
		MapsPathEdit.SetWindowTextA("");
		MapsRawEdit.SetWindowTextA("");
		SetStatusText("vSMR_Maps.json path is unavailable.");
		return;
	}

	MapsPathEdit.SetWindowTextA(Owner->mapsPath.c_str());
	std::ifstream input(Owner->mapsPath, std::ios::binary);
	if (!input)
	{
		MapsRawEdit.SetWindowTextA("");
		SetStatusText("Unable to read vSMR_Maps.json.");
		return;
	}

	std::ostringstream buffer;
	buffer << input.rdbuf();
	MapsRawEdit.SetWindowTextA(buffer.str().c_str());
	SetStatusText("Loaded vSMR_Maps.json.");
}

bool CVsmrControlCenterDialog::SaveMapsText()
{
	if (Owner == nullptr || Owner->mapsPath.empty())
	{
		SetStatusText("vSMR_Maps.json path is unavailable.");
		return false;
	}

	CString text;
	MapsRawEdit.GetWindowText(text);
	std::ofstream output(Owner->mapsPath, std::ios::binary | std::ios::trunc);
	if (!output)
	{
		SetStatusText("Unable to save vSMR_Maps.json.");
		return false;
	}

	const std::string value = CStringToStdString(text);
	output.write(value.data(), static_cast<std::streamsize>(value.size()));
	if (!output.good())
	{
		SetStatusText("Failed while writing vSMR_Maps.json.");
		return false;
	}

	if (Owner != nullptr)
		Owner->ReloadConfig();
	SetStatusText("Saved vSMR_Maps.json and reloaded vSMR.");
	return true;
}

std::string CVsmrControlCenterDialog::BuildOverviewText() const
{
	std::ostringstream text;
	text
		<< "Runtime\r\n"
		<< "Active airport: " << ActiveAirportName() << "\r\n"
		<< "Active profile: " << ActiveProfileName() << "\r\n"
		<< "Active AVISO: " << (ActiveAvisoPath().empty() ? "not loaded" : ActiveAvisoPath()) << "\r\n\r\n"
		<< "Files\r\n"
		<< "Profiles: " << (Owner != nullptr ? Owner->ConfigPath : "") << "\r\n"
		<< "Maps: " << (Owner != nullptr ? Owner->mapsPath : "") << "\r\n"
		<< "Data: " << (Owner != nullptr ? Owner->DataPath : "") << "\r\n\r\n"
		<< "Workflow\r\n"
		<< "Use Profiles for colors, target icons, tag definitions, rules, and modes.\r\n"
		<< "Use AVISO for airport GeoJSON layers, labels, visibility, styles, and variants.\r\n"
		<< "Use Maps for vSMR_Maps.json, then save and reload from the same window.\r\n"
		<< "Use Settings for reloads and folder/file access.";
	return text.str();
}

std::string CVsmrControlCenterDialog::BuildSettingsText() const
{
	std::ostringstream text;
	text
		<< "Centralized runtime actions\r\n\r\n"
		<< "Reload vSMR: reloads vSMR_Profiles.json and vSMR_Maps.json.\r\n"
		<< "Reload AVISO: clears AVISO geometry/raster caches and reloads the active airport AVISO.\r\n"
		<< "Open profiles JSON: opens vSMR_Profiles.json in the associated editor.\r\n"
		<< "Open file: opens vSMR_Maps.json in the associated editor.\r\n"
		<< "Open data folder: opens the vSMR_Data folder used for AVISO and data overrides.\r\n"
		<< "Open plugin folder: opens the plugin installation folder.\r\n\r\n"
		<< "Current state\r\n"
		<< "Airport: " << ActiveAirportName() << "\r\n"
		<< "Profile: " << ActiveProfileName() << "\r\n"
		<< "AVISO: " << (ActiveAvisoPath().empty() ? "not loaded" : ActiveAvisoPath());
	return text.str();
}

std::string CVsmrControlCenterDialog::ActiveProfileName() const
{
	if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		return "";
	return Owner->CurrentConfig->getActiveProfileName();
}

std::string CVsmrControlCenterDialog::ActiveAirportName() const
{
	if (Owner == nullptr)
		return "";
	return Owner->getActiveAirport();
}

std::string CVsmrControlCenterDialog::ActiveAvisoPath() const
{
	if (Owner == nullptr)
		return "";
	return Owner->ResolveAvisoGeoJsonPathForAirport(ActiveAirportName());
}

bool CVsmrControlCenterDialog::OpenPathExternal(const std::string& path, bool folderMode)
{
	if (path.empty())
	{
		SetStatusText("Path is unavailable.");
		return false;
	}

	std::filesystem::path target(path);
	if (folderMode)
	{
		std::error_code ec;
		if (std::filesystem::is_regular_file(target, ec))
			target = target.parent_path();
	}

	const std::string targetText = target.string();
	const HINSTANCE result = ::ShellExecuteA(GetSafeHwnd(), "open", targetText.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (!IsShellExecuteSuccess(result))
	{
		SetStatusText("Unable to open path.");
		return false;
	}

	SetStatusText("Opened " + targetText);
	return true;
}

void CVsmrControlCenterDialog::OnCancel()
{
	ShowWindow(SW_HIDE);
}

void CVsmrControlCenterDialog::OnOK()
{
}

void CVsmrControlCenterDialog::OnClose()
{
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnVsmrControlCenterWindowClosed();
}

void CVsmrControlCenterDialog::OnDestroy()
{
	HideHostedEditors();
	CDialogEx::OnDestroy();
}

void CVsmrControlCenterDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	LayoutControls();
}

void CVsmrControlCenterDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI != nullptr)
	{
		lpMMI->ptMinTrackSize.x = 880;
		lpMMI->ptMinTrackSize.y = 620;
	}
}

void CVsmrControlCenterDialog::OnNavOverviewClicked()
{
	ShowPage(Page::Overview);
}

void CVsmrControlCenterDialog::OnNavProfilesClicked()
{
	ShowPage(Page::Profiles);
}

void CVsmrControlCenterDialog::OnNavAvisoClicked()
{
	ShowPage(Page::Aviso);
}

void CVsmrControlCenterDialog::OnNavMapsClicked()
{
	ShowPage(Page::Maps);
}

void CVsmrControlCenterDialog::OnNavSettingsClicked()
{
	ShowPage(Page::Settings);
}

void CVsmrControlCenterDialog::OnOverviewProfilesClicked()
{
	ShowPage(Page::Profiles);
}

void CVsmrControlCenterDialog::OnOverviewAvisoClicked()
{
	ShowPage(Page::Aviso);
}

void CVsmrControlCenterDialog::OnOverviewMapsClicked()
{
	ShowPage(Page::Maps);
}

void CVsmrControlCenterDialog::OnReloadConfigClicked()
{
	if (Owner != nullptr)
		Owner->ReloadConfig();
	SyncFromRadar();
	SetStatusText("Reloaded vSMR profiles and maps.");
}

void CVsmrControlCenterDialog::OnReloadAvisoClicked()
{
	const bool loaded = Owner != nullptr && Owner->ForceReloadAvisoGeoJson();
	SyncFromRadar();
	SetStatusText(loaded ? "Reloaded AVISO." : "No AVISO file loaded.");
}

void CVsmrControlCenterDialog::OnOpenMapsExternalClicked()
{
	OpenPathExternal(Owner != nullptr ? Owner->mapsPath : "", false);
}

void CVsmrControlCenterDialog::OnReloadMapsTextClicked()
{
	LoadMapsText();
}

void CVsmrControlCenterDialog::OnSaveMapsTextClicked()
{
	SaveMapsText();
}

void CVsmrControlCenterDialog::OnOpenConfigExternalClicked()
{
	OpenPathExternal(Owner != nullptr ? Owner->ConfigPath : "", false);
}

void CVsmrControlCenterDialog::OnOpenDataFolderClicked()
{
	OpenPathExternal(Owner != nullptr ? Owner->DataPath : "", true);
}

void CVsmrControlCenterDialog::OnOpenPluginFolderClicked()
{
	OpenPathExternal(Owner != nullptr ? Owner->DllPath : "", true);
}

BEGIN_MESSAGE_MAP(CVsmrControlCenterDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_BN_CLICKED(IDC_VCC_NAV_OVERVIEW, &CVsmrControlCenterDialog::OnNavOverviewClicked)
	ON_BN_CLICKED(IDC_VCC_NAV_PROFILES, &CVsmrControlCenterDialog::OnNavProfilesClicked)
	ON_BN_CLICKED(IDC_VCC_NAV_AVISO, &CVsmrControlCenterDialog::OnNavAvisoClicked)
	ON_BN_CLICKED(IDC_VCC_NAV_MAPS, &CVsmrControlCenterDialog::OnNavMapsClicked)
	ON_BN_CLICKED(IDC_VCC_NAV_SETTINGS, &CVsmrControlCenterDialog::OnNavSettingsClicked)
	ON_BN_CLICKED(IDC_VCC_OVERVIEW_PROFILES, &CVsmrControlCenterDialog::OnOverviewProfilesClicked)
	ON_BN_CLICKED(IDC_VCC_OVERVIEW_AVISO, &CVsmrControlCenterDialog::OnOverviewAvisoClicked)
	ON_BN_CLICKED(IDC_VCC_OVERVIEW_MAPS, &CVsmrControlCenterDialog::OnOverviewMapsClicked)
	ON_BN_CLICKED(IDC_VCC_RELOAD_CONFIG, &CVsmrControlCenterDialog::OnReloadConfigClicked)
	ON_BN_CLICKED(IDC_VCC_RELOAD_AVISO, &CVsmrControlCenterDialog::OnReloadAvisoClicked)
	ON_BN_CLICKED(IDC_VCC_OPEN_MAPS_EXTERNAL, &CVsmrControlCenterDialog::OnOpenMapsExternalClicked)
	ON_BN_CLICKED(IDC_VCC_RELOAD_MAPS_TEXT, &CVsmrControlCenterDialog::OnReloadMapsTextClicked)
	ON_BN_CLICKED(IDC_VCC_SAVE_MAPS_TEXT, &CVsmrControlCenterDialog::OnSaveMapsTextClicked)
	ON_BN_CLICKED(IDC_VCC_OPEN_CONFIG_EXTERNAL, &CVsmrControlCenterDialog::OnOpenConfigExternalClicked)
	ON_BN_CLICKED(IDC_VCC_OPEN_DATA_FOLDER, &CVsmrControlCenterDialog::OnOpenDataFolderClicked)
	ON_BN_CLICKED(IDC_VCC_OPEN_PLUGIN_FOLDER, &CVsmrControlCenterDialog::OnOpenPluginFolderClicked)
END_MESSAGE_MAP()
