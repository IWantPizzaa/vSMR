#include "stdafx.h"
#include "SMRRadar.hpp"
#include "ProfileEditorDialog.hpp"
#include <cctype>
#include <cstring>

namespace
{
	int ClampInt(int value, int minValue, int maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

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

	std::string TrimAsciiWhitespaceCopy(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;

		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	}

	bool EqualsNoCase(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
				return false;
		}
		return true;
	}

	bool ContainsProfileNameNoCase(const std::vector<std::string>& names, const std::string& candidate)
	{
		for (const std::string& name : names)
		{
			if (EqualsNoCase(name, candidate))
				return true;
		}
		return false;
	}

	std::string FindCanonicalProfileNameNoCase(const std::vector<std::string>& names, const std::string& name)
	{
		for (const std::string& profileName : names)
		{
			if (EqualsNoCase(profileName, name))
				return profileName;
		}
		return "";
	}

	std::string MakeUniqueProfileName(const std::vector<std::string>& existingNames, const std::string& requestedName)
	{
		std::string baseName = TrimAsciiWhitespaceCopy(requestedName);
		if (baseName.empty())
			baseName = "Profile";

		if (!ContainsProfileNameNoCase(existingNames, baseName))
			return baseName;

		for (int i = 2; i < 1000; ++i)
		{
			const std::string candidate = baseName + " (" + std::to_string(i) + ")";
			if (!ContainsProfileNameNoCase(existingNames, candidate))
				return candidate;
		}

		return baseName + " Copy";
	}

	void CloneJsonValue(const rapidjson::Value& source, rapidjson::Value& out, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (source.IsObject())
		{
			out.SetObject();
			for (Value::ConstMemberIterator it = source.MemberBegin(); it != source.MemberEnd(); ++it)
			{
				Value key(it->name.GetString(), static_cast<rapidjson::SizeType>(it->name.GetStringLength()), allocator);
				Value val;
				CloneJsonValue(it->value, val, allocator);
				out.AddMember(key, val, allocator);
			}
			return;
		}
		if (source.IsArray())
		{
			out.SetArray();
			for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
			{
				Value entry;
				CloneJsonValue(source[i], entry, allocator);
				out.PushBack(entry, allocator);
			}
			return;
		}
		if (source.IsString())
		{
			out.SetString(source.GetString(), static_cast<rapidjson::SizeType>(source.GetStringLength()), allocator);
			return;
		}
		if (source.IsBool()) { out.SetBool(source.GetBool()); return; }
		if (source.IsInt()) { out.SetInt(source.GetInt()); return; }
		if (source.IsUint()) { out.SetUint(source.GetUint()); return; }
		if (source.IsInt64()) { out.SetInt64(source.GetInt64()); return; }
		if (source.IsUint64()) { out.SetUint64(source.GetUint64()); return; }
		if (source.IsDouble()) { out.SetDouble(source.GetDouble()); return; }
		out.SetNull();
	}

	rapidjson::SizeType FindProfileIndexNoCase(const rapidjson::Document& document, const std::string& name)
	{
		const rapidjson::SizeType invalidIndex = static_cast<rapidjson::SizeType>(-1);
		if (!document.IsArray())
			return invalidIndex;

		for (rapidjson::SizeType i = 0; i < document.Size(); ++i)
		{
			const rapidjson::Value& profile = document[i];
			if (!profile.IsObject() || !profile.HasMember("name") || !profile["name"].IsString())
				continue;
			if (EqualsNoCase(profile["name"].GetString(), name))
				return i;
		}
		return invalidIndex;
	}

	void EnsureProfileProModeDefaults(rapidjson::Value& profile, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;

		if (!profile.IsObject())
			return;

		if (!profile.HasMember("filters") || !profile["filters"].IsObject())
		{
			if (profile.HasMember("filters"))
				profile.RemoveMember("filters");
			Value filters(rapidjson::kObjectType);
			profile.AddMember("filters", filters, allocator);
		}

		Value& filters = profile["filters"];
		if (!filters.HasMember("pro_mode") || !filters["pro_mode"].IsObject())
		{
			if (filters.HasMember("pro_mode"))
				filters.RemoveMember("pro_mode");
			Value proMode(rapidjson::kObjectType);
			filters.AddMember("pro_mode", proMode, allocator);
		}

		Value& proMode = filters["pro_mode"];
		bool enabledValue = false;
		if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
			enabledValue = proMode["enabled"].GetBool();
		else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
			enabledValue = proMode["enable"].GetBool();
		if (proMode.HasMember("enable"))
			proMode.RemoveMember("enable");
		if (!proMode.HasMember("enabled") || !proMode["enabled"].IsBool())
		{
			if (proMode.HasMember("enabled"))
				proMode.RemoveMember("enabled");
			proMode.AddMember("enabled", enabledValue, allocator);
		}
		if (!proMode.HasMember("accept_pilot_squawk") || !proMode["accept_pilot_squawk"].IsBool())
		{
			if (proMode.HasMember("accept_pilot_squawk"))
				proMode.RemoveMember("accept_pilot_squawk");
			proMode.AddMember("accept_pilot_squawk", true, allocator);
		}

		const Value* blockedSource = nullptr;
		if (proMode.HasMember("blocked_auto_correlate_squawks") && proMode["blocked_auto_correlate_squawks"].IsArray())
			blockedSource = &proMode["blocked_auto_correlate_squawks"];
		else if (proMode.HasMember("do_not_autocorrelate_squawks") && proMode["do_not_autocorrelate_squawks"].IsArray())
			blockedSource = &proMode["do_not_autocorrelate_squawks"];

		if (!proMode.HasMember("blocked_auto_correlate_squawks") || !proMode["blocked_auto_correlate_squawks"].IsArray())
		{
			if (proMode.HasMember("blocked_auto_correlate_squawks"))
				proMode.RemoveMember("blocked_auto_correlate_squawks");
			Value squawks(rapidjson::kArrayType);

			if (blockedSource != nullptr)
			{
				for (rapidjson::SizeType i = 0; i < blockedSource->Size(); ++i)
				{
					if (!(*blockedSource)[i].IsString())
						continue;

					Value squawkValue;
					squawkValue.SetString((*blockedSource)[i].GetString(), static_cast<rapidjson::SizeType>(std::strlen((*blockedSource)[i].GetString())), allocator);
					squawks.PushBack(squawkValue, allocator);
				}
			}

			if (squawks.Empty())
			{
				static const char* const defaultSquawks[] = { "2000", "2200", "1200", "7000" };
				for (const char* squawk : defaultSquawks)
				{
					Value squawkValue;
					squawkValue.SetString(squawk, static_cast<rapidjson::SizeType>(std::strlen(squawk)), allocator);
					squawks.PushBack(squawkValue, allocator);
				}
			}

			proMode.AddMember("blocked_auto_correlate_squawks", squawks, allocator);
		}
		if (proMode.HasMember("do_not_autocorrelate_squawks"))
			proMode.RemoveMember("do_not_autocorrelate_squawks");
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
	if (!EnsureProfileEditorWindowCreated())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Profile Editor", "Failed to open detached Profile Editor window.", true, true, false, false, false);
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
	ProfileEditorDialog->SyncFromRadar();

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

std::vector<std::string> CSMRRadar::GetProfileColorPathsForEditor()
{
	RebuildProfileColorEntries();
	return ProfileColorPaths;
}

std::string CSMRRadar::GetSelectedProfileColorPathForEditor() const
{
	return SelectedProfileColorPath;
}

bool CSMRRadar::SelectProfileColorPathForEditor(const std::string& path)
{
	if (!IsProfileColorPathValid(path))
		return false;

	SelectedProfileColorPath = path;
	RequestRefresh();
	return true;
}

bool CSMRRadar::GetSelectedProfileColorForEditor(int& r, int& g, int& b, int& a, bool& hasAlpha) const
{
	r = 0;
	g = 0;
	b = 0;
	a = 255;
	hasAlpha = false;

	if (SelectedProfileColorPath.empty())
		return false;

	bool colorHasAlpha = false;
	if (!const_cast<CSMRRadar*>(this)->IsProfileColorPathValid(SelectedProfileColorPath, &colorHasAlpha))
		return false;

	r = const_cast<CSMRRadar*>(this)->GetProfileColorComponentValue(SelectedProfileColorPath, 'r', 0);
	g = const_cast<CSMRRadar*>(this)->GetProfileColorComponentValue(SelectedProfileColorPath, 'g', 0);
	b = const_cast<CSMRRadar*>(this)->GetProfileColorComponentValue(SelectedProfileColorPath, 'b', 0);
	a = const_cast<CSMRRadar*>(this)->GetProfileColorComponentValue(SelectedProfileColorPath, 'a', 255);
	hasAlpha = colorHasAlpha;
	return true;
}

bool CSMRRadar::SetSelectedProfileColorForEditor(int r, int g, int b, int a, bool useAlpha, bool persistToDisk)
{
	if (SelectedProfileColorPath.empty())
		return false;

	bool hasAlphaInPath = false;
	if (!IsProfileColorPathValid(SelectedProfileColorPath, &hasAlphaInPath))
		return false;

	r = ClampInt(r, 0, 255);
	g = ClampInt(g, 0, 255);
	b = ClampInt(b, 0, 255);
	a = ClampInt(a, 0, 255);

	const bool okR = UpdateProfileColorComponent(SelectedProfileColorPath, 'r', r);
	const bool okG = UpdateProfileColorComponent(SelectedProfileColorPath, 'g', g);
	const bool okB = UpdateProfileColorComponent(SelectedProfileColorPath, 'b', b);
	bool okA = true;
	if (useAlpha || hasAlphaInPath || a != 255)
		okA = UpdateProfileColorComponent(SelectedProfileColorPath, 'a', a);

	if (!(okR && okG && okB && okA))
		return false;

	if (persistToDisk && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save profile color to vSMR_Profiles.json", true, true, false, false, false);
		return false;
	}

	RebuildProfileColorEntries();
	RequestRefresh();
	return true;
}

std::vector<std::string> CSMRRadar::GetProfileNamesForEditor() const
{
	if (!CurrentConfig)
		return {};
	return CurrentConfig->getAllProfiles();
}

std::string CSMRRadar::GetActiveProfileNameForEditor() const
{
	if (!CurrentConfig)
		return "";
	return const_cast<CConfig*>(CurrentConfig)->getActiveProfileName();
}

bool CSMRRadar::SetActiveProfileForEditor(const std::string& name, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	const std::vector<std::string> names = CurrentConfig->getAllProfiles();
	const std::string canonicalName = FindCanonicalProfileNameNoCase(names, name);
	if (canonicalName.empty())
		return false;

	LoadProfile(canonicalName);
	if (persistToDisk)
		CurrentConfig->saveConfig();
	RequestRefresh();
	return true;
}

bool CSMRRadar::GetProfileProModeEnabledForEditor(const std::string& name, bool& outEnabled) const
{
	outEnabled = false;
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, name);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	const rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject() || !profile.HasMember("filters") || !profile["filters"].IsObject())
		return true;

	const rapidjson::Value& filters = profile["filters"];
	if (!filters.HasMember("pro_mode") || !filters["pro_mode"].IsObject())
		return true;

	const rapidjson::Value& proMode = filters["pro_mode"];
	if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
		outEnabled = proMode["enabled"].GetBool();
	else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
		outEnabled = proMode["enable"].GetBool();

	return true;
}

bool CSMRRadar::SetProfileProModeEnabledForEditor(const std::string& name, bool enabled)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, name);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	EnsureProfileProModeDefaults(profile, CurrentConfig->document.GetAllocator());
	profile["filters"]["pro_mode"]["enabled"].SetBool(enabled);
	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? name : activeBefore);
	RequestRefresh();
	return true;
}

bool CSMRRadar::AddProfileForEditor(const std::string& requestedName, bool duplicateActiveProfile, std::string* outCreatedName)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	std::vector<std::string> existingNames = CurrentConfig->getAllProfiles();
	const std::string createdName = MakeUniqueProfileName(existingNames, requestedName);

	rapidjson::Value newProfile(rapidjson::kObjectType);
	if (duplicateActiveProfile && CurrentConfig->getActiveProfile().IsObject())
	{
		CloneJsonValue(CurrentConfig->getActiveProfile(), newProfile, CurrentConfig->document.GetAllocator());
	}
	else if (CurrentConfig->document.Size() > 0 && CurrentConfig->document[static_cast<rapidjson::SizeType>(0)].IsObject())
	{
		CloneJsonValue(CurrentConfig->document[static_cast<rapidjson::SizeType>(0)], newProfile, CurrentConfig->document.GetAllocator());
	}
	else
	{
		newProfile.SetObject();
	}

	rapidjson::Value profileNameValue;
	profileNameValue.SetString(createdName.c_str(), static_cast<rapidjson::SizeType>(createdName.size()), CurrentConfig->document.GetAllocator());
	if (newProfile.HasMember("name"))
		newProfile["name"].SetString(createdName.c_str(), static_cast<rapidjson::SizeType>(createdName.size()), CurrentConfig->document.GetAllocator());
	else
		newProfile.AddMember("name", profileNameValue, CurrentConfig->document.GetAllocator());

	CurrentConfig->document.PushBack(newProfile, CurrentConfig->document.GetAllocator());
	if (!CurrentConfig->saveConfig())
		return false;

	CurrentConfig->reload();
	LoadProfile(createdName);
	RequestRefresh();
	if (outCreatedName != nullptr)
		*outCreatedName = createdName;
	return true;
}

bool CSMRRadar::RenameProfileForEditor(const std::string& oldName, const std::string& newName)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const std::string trimmedNewName = TrimAsciiWhitespaceCopy(newName);
	if (trimmedNewName.empty())
		return false;

	const std::vector<std::string> existingNames = CurrentConfig->getAllProfiles();
	for (const std::string& existing : existingNames)
	{
		if (EqualsNoCase(existing, oldName))
			continue;
		if (EqualsNoCase(existing, trimmedNewName))
			return false;
	}

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, oldName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	CurrentConfig->document[targetIndex]["name"].SetString(
		trimmedNewName.c_str(),
		static_cast<rapidjson::SizeType>(trimmedNewName.size()),
		CurrentConfig->document.GetAllocator());
	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	if (EqualsNoCase(activeBefore, oldName))
		LoadProfile(trimmedNewName);
	else
	{
		std::string fallbackActive = activeBefore;
		const std::vector<std::string> names = CurrentConfig->getAllProfiles();
		if (fallbackActive.empty() || !ContainsProfileNameNoCase(names, fallbackActive))
			fallbackActive = names.empty() ? "Default" : names.front();
		LoadProfile(fallbackActive);
	}
	RequestRefresh();
	return true;
}

bool CSMRRadar::DeleteProfileForEditor(const std::string& name)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;
	if (CurrentConfig->document.Size() <= 1)
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	const rapidjson::SizeType removeIndex = FindProfileIndexNoCase(CurrentConfig->document, name);
	if (removeIndex >= CurrentConfig->document.Size())
		return false;

	for (rapidjson::SizeType i = removeIndex; (i + 1) < CurrentConfig->document.Size(); ++i)
		CurrentConfig->document[i] = CurrentConfig->document[i + 1];
	CurrentConfig->document.PopBack();
	if (!CurrentConfig->saveConfig())
		return false;

	CurrentConfig->reload();
	std::string nextActive = activeBefore;
	if (EqualsNoCase(activeBefore, name))
	{
		const std::vector<std::string> names = CurrentConfig->getAllProfiles();
		nextActive = names.empty() ? "Default" : names.front();
	}

	LoadProfile(nextActive);
	RequestRefresh();
	return true;
}
