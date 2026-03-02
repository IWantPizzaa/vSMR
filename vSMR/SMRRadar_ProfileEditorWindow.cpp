#include "stdafx.h"
#include "SMRRadar.hpp"
#include "ProfileEditorDialog.hpp"

namespace
{
	CRect BuildDefaultProfileEditorWindowRect()
	{
		const int defaultWidth = 640;
		const int defaultHeight = 520;

		int left = 120;
		int top = 120;
		const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
		const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
		if (screenWidth > defaultWidth + 80)
			left = (screenWidth - defaultWidth) / 2;
		if (screenHeight > defaultHeight + 80)
			top = (screenHeight - defaultHeight) / 2;

		return CRect(left, top, left + defaultWidth, top + defaultHeight);
	}
}

bool CSMRRadar::EnsureProfileEditorWindowCreated()
{
	if (ProfileEditorDialog && ::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
	{
		ProfileEditorDialog->SetOwner(this);
		return true;
	}

	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	ProfileEditorDialog = std::make_unique<CProfileEditorDialog>(this, AfxGetMainWnd());
	if (!ProfileEditorDialog->Create(CProfileEditorDialog::IDD, AfxGetMainWnd()))
	{
		ProfileEditorDialog.reset();
		return false;
	}

	const CRect windowRect = GetProfileEditorWindowRectFromConfig();
	ProfileEditorDialog->SetWindowPos(
		nullptr,
		windowRect.left,
		windowRect.top,
		max(320, windowRect.Width()),
		max(220, windowRect.Height()),
		SWP_NOZORDER | SWP_NOACTIVATE);
	ProfileEditorDialog->ShowWindow(SW_HIDE);
	return true;
}

bool CSMRRadar::IsProfileEditorWindowVisible() const
{
	return ProfileEditorDialog && ::IsWindow(ProfileEditorDialog->GetSafeHwnd()) && ProfileEditorDialog->IsWindowVisible();
}

void CSMRRadar::OpenProfileEditorWindow()
{
	ShowTagDefinitionEditor = false;
	ShowProfileColorPicker = false;

	if (!EnsureProfileEditorWindowCreated())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Profile Editor", "Failed to open detached Profile Editor window. Using legacy editor.", true, true, false, false, false);
		ShowTagDefinitionEditor = true;
		RequestRefresh();
		return;
	}

	const CRect windowRect = GetProfileEditorWindowRectFromConfig();
	ProfileEditorDialog->SetWindowPos(
		nullptr,
		windowRect.left,
		windowRect.top,
		max(320, windowRect.Width()),
		max(220, windowRect.Height()),
		SWP_NOZORDER | SWP_NOACTIVATE);
	ProfileEditorDialog->ShowWindow(SW_SHOW);
	ProfileEditorDialog->BringWindowToTop();

	PersistProfileEditorWindowLayout(windowRect, true, true);
	RequestRefresh();
}

void CSMRRadar::CloseProfileEditorWindow(bool persistVisibility)
{
	if (!ProfileEditorDialog || !::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
		return;

	CRect windowRect;
	ProfileEditorDialog->GetWindowRect(&windowRect);
	PersistProfileEditorWindowLayout(windowRect, false, persistVisibility);
	ProfileEditorDialog->ShowWindow(SW_HIDE);
}

void CSMRRadar::DestroyProfileEditorWindow()
{
	if (!ProfileEditorDialog)
		return;

	if (::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
		ProfileEditorDialog->DestroyWindow();

	ProfileEditorDialog.reset();
}

void CSMRRadar::OnProfileEditorWindowClosed()
{
	if (ProfileEditorDialog && ::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
	{
		CRect windowRect;
		ProfileEditorDialog->GetWindowRect(&windowRect);
		PersistProfileEditorWindowLayout(windowRect, false, true);
	}

	RequestRefresh();
}

void CSMRRadar::OnProfileEditorWindowLayoutChanged(const CRect& windowRect)
{
	PersistProfileEditorWindowLayout(windowRect, true, false);
}

CRect CSMRRadar::GetProfileEditorWindowRectFromConfig() const
{
	CRect fallback = BuildDefaultProfileEditorWindowRect();

	if (!CurrentConfig)
		return fallback;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("ui_layout") || !profile["ui_layout"].IsObject())
		return fallback;

	const Value& uiLayout = profile["ui_layout"];
	if (!uiLayout.HasMember("profile_editor_window") || !uiLayout["profile_editor_window"].IsObject())
		return fallback;

	const Value& window = uiLayout["profile_editor_window"];
	auto readInt = [&](const char* key, int defaultValue) -> int
	{
		if (!window.HasMember(key) || !window[key].IsInt())
			return defaultValue;
		return window[key].GetInt();
	};

	const int x = readInt("x", fallback.left);
	const int y = readInt("y", fallback.top);
	const int width = max(320, readInt("width", fallback.Width()));
	const int height = max(220, readInt("height", fallback.Height()));
	return CRect(x, y, x + width, y + height);
}

bool CSMRRadar::PersistProfileEditorWindowLayout(const CRect& windowRect, bool visible, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	auto ensureObjectMember = [&](Value& parent, const char* key) -> Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value objectValue(rapidjson::kObjectType);
			parent.AddMember(keyValue, objectValue, allocator);
			changed = true;
		}

		return parent[key];
	};

	auto upsertInt = [&](Value& parent, const char* key, int value)
	{
		if (!parent.HasMember(key) || !parent[key].IsInt())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value intValue;
			intValue.SetInt(value);
			parent.AddMember(keyValue, intValue, allocator);
			changed = true;
			return;
		}

		if (parent[key].GetInt() != value)
		{
			parent[key].SetInt(value);
			changed = true;
		}
	};

	auto upsertBool = [&](Value& parent, const char* key, bool value)
	{
		if (!parent.HasMember(key) || !parent[key].IsBool())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value boolValue;
			boolValue.SetBool(value);
			parent.AddMember(keyValue, boolValue, allocator);
			changed = true;
			return;
		}

		if (parent[key].GetBool() != value)
		{
			parent[key].SetBool(value);
			changed = true;
		}
	};

	Value& uiLayout = ensureObjectMember(profile, "ui_layout");
	Value& profileEditorWindow = ensureObjectMember(uiLayout, "profile_editor_window");

	const int width = max(320, windowRect.Width());
	const int height = max(220, windowRect.Height());
	upsertInt(profileEditorWindow, "x", windowRect.left);
	upsertInt(profileEditorWindow, "y", windowRect.top);
	upsertInt(profileEditorWindow, "width", width);
	upsertInt(profileEditorWindow, "height", height);
	upsertBool(profileEditorWindow, "visible", visible);

	if (changed && persistToDisk && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save profile editor layout to vSMR_Profiles.json", true, true, false, false, false);
		return false;
	}

	return true;
}
