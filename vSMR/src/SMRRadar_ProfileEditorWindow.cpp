#include "stdafx.h"
#include "SMRRadar.hpp"
#include "ProfileEditorDialog.hpp"
#include <cctype>
#include <cstring>

extern std::vector<CSMRRadar*> RadarScreensOpened;

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

	std::vector<std::string> DefaultBlockedAutoCorrelateSquawks()
	{
		return { "2000", "2200", "1200", "7000" };
	}

	CSMRRadar::DisplayModeSettings MakeDisplayModeSettings(
		const std::string& name,
		bool requireAssignedSquawk,
		bool towerFilter,
		bool structuredRulesEnabled = true)
	{
		CSMRRadar::DisplayModeSettings settings;
		settings.name = name;
		settings.requireAssignedSquawk = requireAssignedSquawk;
		settings.acceptPilotSquawk = true;
		settings.blockedAutoCorrelateSquawks = DefaultBlockedAutoCorrelateSquawks();
		settings.towerFilter = towerFilter;
		settings.structuredRulesEnabled = structuredRulesEnabled;
		return settings;
	}

	bool ContainsDisplayModeNameNoCase(const std::vector<CSMRRadar::DisplayModeSettings>& modes, const std::string& candidate)
	{
		for (const CSMRRadar::DisplayModeSettings& mode : modes)
		{
			if (EqualsNoCase(mode.name, candidate))
				return true;
		}
		return false;
	}

	std::string MakeUniqueDisplayModeName(const std::vector<CSMRRadar::DisplayModeSettings>& modes, const std::string& requestedName)
	{
		std::string baseName = TrimAsciiWhitespaceCopy(requestedName);
		if (baseName.empty())
			baseName = "Display Mode";

		if (!ContainsDisplayModeNameNoCase(modes, baseName))
			return baseName;

		for (int i = 2; i < 1000; ++i)
		{
			const std::string candidate = baseName + " (" + std::to_string(i) + ")";
			if (!ContainsDisplayModeNameNoCase(modes, candidate))
				return candidate;
		}

		return baseName + " Copy";
	}

	int FindDisplayModeIndexNoCase(const std::vector<CSMRRadar::DisplayModeSettings>& modes, const std::string& name)
	{
		for (size_t i = 0; i < modes.size(); ++i)
		{
			if (EqualsNoCase(modes[i].name, name))
				return static_cast<int>(i);
		}
		return -1;
	}

	bool ReadBoolMember(const rapidjson::Value& objectValue, const char* key, bool fallback)
	{
		if (objectValue.IsObject() && objectValue.HasMember(key) && objectValue[key].IsBool())
			return objectValue[key].GetBool();
		return fallback;
	}

	std::vector<std::string> ReadSquawkArray(const rapidjson::Value& objectValue, const char* key)
	{
		std::vector<std::string> values;
		if (!objectValue.IsObject() || !objectValue.HasMember(key) || !objectValue[key].IsArray())
			return values;

		const rapidjson::Value& arrayValue = objectValue[key];
		for (rapidjson::SizeType i = 0; i < arrayValue.Size(); ++i)
		{
			if (arrayValue[i].IsString() && arrayValue[i].GetStringLength() > 0)
				values.push_back(arrayValue[i].GetString());
		}
		return values;
	}

	void WriteStringMember(rapidjson::Value& objectValue, const char* key, const std::string& value, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (objectValue.HasMember(key))
			objectValue.RemoveMember(key);
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value stringValue;
		stringValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
		objectValue.AddMember(keyValue, stringValue, allocator);
	}

	void WriteBoolMember(rapidjson::Value& objectValue, const char* key, bool value, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (objectValue.HasMember(key))
			objectValue.RemoveMember(key);
		Value keyValue;
		keyValue.SetString(key, allocator);
		objectValue.AddMember(keyValue, Value(value), allocator);
	}

	void WriteStringArrayMember(rapidjson::Value& objectValue, const char* key, const std::vector<std::string>& values, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (objectValue.HasMember(key))
			objectValue.RemoveMember(key);
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value arrayValue(rapidjson::kArrayType);
		for (const std::string& value : values)
		{
			if (value.empty())
				continue;
			Value itemValue;
			itemValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
			arrayValue.PushBack(itemValue, allocator);
		}
		objectValue.AddMember(keyValue, arrayValue, allocator);
	}

	void WriteStatusVisibility(rapidjson::Value& modeValue, const CSMRRadar::DisplayModeStatusVisibility& statuses, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (modeValue.HasMember("statuses"))
			modeValue.RemoveMember("statuses");

		Value keyValue;
		keyValue.SetString("statuses", allocator);
		Value statusValue(rapidjson::kObjectType);
		WriteBoolMember(statusValue, "no_status", statuses.noStatus, allocator);
		WriteBoolMember(statusValue, "push", statuses.push, allocator);
		WriteBoolMember(statusValue, "startup", statuses.startup, allocator);
		WriteBoolMember(statusValue, "taxi", statuses.taxi, allocator);
		WriteBoolMember(statusValue, "departure", statuses.departure, allocator);
		WriteBoolMember(statusValue, "on_runway", statuses.onRunway, allocator);
		WriteBoolMember(statusValue, "airborne", statuses.airborne, allocator);
		WriteBoolMember(statusValue, "arrivals", statuses.arrivals, allocator);
		WriteBoolMember(statusValue, "no_fpl", statuses.noFlightPlan, allocator);
		WriteBoolMember(statusValue, "uncorrelated", statuses.uncorrelated, allocator);
		modeValue.AddMember(keyValue, statusValue, allocator);
	}

	void WriteDisplayModeValue(rapidjson::Value& modeValue, const CSMRRadar::DisplayModeSettings& settings, rapidjson::Document::AllocatorType& allocator)
	{
		modeValue.SetObject();
		WriteStringMember(modeValue, "name", settings.name.empty() ? "Display Mode" : settings.name, allocator);
		WriteBoolMember(modeValue, "require_assigned_squawk", settings.requireAssignedSquawk, allocator);
		WriteBoolMember(modeValue, "accept_pilot_squawk", settings.acceptPilotSquawk, allocator);
		WriteBoolMember(modeValue, "require_clearance", settings.requireClearance, allocator);
		WriteBoolMember(modeValue, "require_valid_tsat", settings.requireValidTsat, allocator);
		WriteBoolMember(modeValue, "require_active_tobt", settings.requireActiveTobt, allocator);
		WriteBoolMember(modeValue, "tower_filter", settings.towerFilter, allocator);
		WriteBoolMember(modeValue, "structured_rules", settings.structuredRulesEnabled, allocator);
		WriteStringArrayMember(modeValue, "blocked_auto_correlate_squawks", settings.blockedAutoCorrelateSquawks.empty() ? DefaultBlockedAutoCorrelateSquawks() : settings.blockedAutoCorrelateSquawks, allocator);
		WriteStatusVisibility(modeValue, settings.statuses, allocator);
	}

	CSMRRadar::DisplayModeSettings ReadDisplayModeValue(const rapidjson::Value& modeValue, const CSMRRadar::DisplayModeSettings& fallback)
	{
		CSMRRadar::DisplayModeSettings settings = fallback;
		if (!modeValue.IsObject())
			return settings;

		if (modeValue.HasMember("name") && modeValue["name"].IsString())
		{
			const std::string name = TrimAsciiWhitespaceCopy(modeValue["name"].GetString());
			if (!name.empty())
				settings.name = name;
		}
		settings.requireAssignedSquawk = ReadBoolMember(modeValue, "require_assigned_squawk", ReadBoolMember(modeValue, "squawk_rule", settings.requireAssignedSquawk));
		settings.requireClearance = ReadBoolMember(modeValue, "require_clearance", settings.requireClearance);
		settings.requireValidTsat = ReadBoolMember(modeValue, "require_valid_tsat", settings.requireValidTsat);
		settings.requireActiveTobt = ReadBoolMember(modeValue, "require_active_tobt", settings.requireActiveTobt);
		settings.acceptPilotSquawk = ReadBoolMember(modeValue, "accept_pilot_squawk", settings.acceptPilotSquawk);
		settings.towerFilter = ReadBoolMember(modeValue, "tower_filter", ReadBoolMember(modeValue, "tower_mode", settings.towerFilter));
		settings.structuredRulesEnabled = ReadBoolMember(modeValue, "structured_rules", ReadBoolMember(modeValue, "structured_rules_enabled", settings.structuredRulesEnabled));

		std::vector<std::string> squawks = ReadSquawkArray(modeValue, "blocked_auto_correlate_squawks");
		if (squawks.empty())
			squawks = ReadSquawkArray(modeValue, "do_not_autocorrelate_squawks");
		settings.blockedAutoCorrelateSquawks = squawks.empty() ? DefaultBlockedAutoCorrelateSquawks() : squawks;

		if (modeValue.HasMember("statuses") && modeValue["statuses"].IsObject())
		{
			const rapidjson::Value& statuses = modeValue["statuses"];
			settings.statuses.noStatus = ReadBoolMember(statuses, "no_status", settings.statuses.noStatus);
			settings.statuses.push = ReadBoolMember(statuses, "push", settings.statuses.push);
			settings.statuses.startup = ReadBoolMember(statuses, "startup", ReadBoolMember(statuses, "stup", settings.statuses.startup));
			settings.statuses.taxi = ReadBoolMember(statuses, "taxi", settings.statuses.taxi);
			settings.statuses.departure = ReadBoolMember(statuses, "departure", ReadBoolMember(statuses, "depa", settings.statuses.departure));
			settings.statuses.onRunway = ReadBoolMember(statuses, "on_runway", settings.statuses.onRunway);
			settings.statuses.airborne = ReadBoolMember(statuses, "airborne", settings.statuses.airborne);
			settings.statuses.arrivals = ReadBoolMember(statuses, "arrivals", settings.statuses.arrivals);
			settings.statuses.noFlightPlan = ReadBoolMember(statuses, "no_fpl", settings.statuses.noFlightPlan);
			settings.statuses.uncorrelated = ReadBoolMember(statuses, "uncorrelated", settings.statuses.uncorrelated);
		}

		return settings;
	}

	void ReadLegacyDisplayModeFlags(const rapidjson::Value& filters, bool& outProModeEnabled, bool& outTowerModeEnabled, bool& outAcceptPilotSquawk, std::vector<std::string>& outBlockedSquawks)
	{
		outProModeEnabled = false;
		outTowerModeEnabled = false;
		outAcceptPilotSquawk = true;
		outBlockedSquawks = DefaultBlockedAutoCorrelateSquawks();

		if (!filters.IsObject())
			return;

		if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
		{
			const rapidjson::Value& proMode = filters["pro_mode"];
			outProModeEnabled = ReadBoolMember(proMode, "enabled", ReadBoolMember(proMode, "enable", false));
			outAcceptPilotSquawk = ReadBoolMember(proMode, "accept_pilot_squawk", true);
			std::vector<std::string> blocked = ReadSquawkArray(proMode, "blocked_auto_correlate_squawks");
			if (blocked.empty())
				blocked = ReadSquawkArray(proMode, "do_not_autocorrelate_squawks");
			if (!blocked.empty())
				outBlockedSquawks = blocked;
		}

		if (filters.HasMember("tower_mode") && filters["tower_mode"].IsObject())
		{
			const rapidjson::Value& towerMode = filters["tower_mode"];
			outTowerModeEnabled = ReadBoolMember(towerMode, "enabled", ReadBoolMember(towerMode, "enable", false));
		}
	}

	std::vector<CSMRRadar::DisplayModeSettings> BuildLegacyDisplayModes(const rapidjson::Value& filters)
	{
		bool legacyProModeEnabled = false;
		bool legacyTowerModeEnabled = false;
		bool acceptPilotSquawk = true;
		std::vector<std::string> blockedSquawks;
		ReadLegacyDisplayModeFlags(filters, legacyProModeEnabled, legacyTowerModeEnabled, acceptPilotSquawk, blockedSquawks);

		std::vector<CSMRRadar::DisplayModeSettings> modes;
		modes.push_back(MakeDisplayModeSettings("Normal", false, false));
		modes.push_back(MakeDisplayModeSettings("Pro", true, false));
		modes.push_back(MakeDisplayModeSettings("Tower", false, true));
		modes.push_back(MakeDisplayModeSettings("Pro + Tower", true, true));
		for (CSMRRadar::DisplayModeSettings& mode : modes)
		{
			mode.acceptPilotSquawk = acceptPilotSquawk;
			mode.blockedAutoCorrelateSquawks = blockedSquawks.empty() ? DefaultBlockedAutoCorrelateSquawks() : blockedSquawks;
		}
		return modes;
	}

	std::string GetLegacyActiveDisplayModeName(const rapidjson::Value& filters)
	{
		bool legacyProModeEnabled = false;
		bool legacyTowerModeEnabled = false;
		bool acceptPilotSquawk = true;
		std::vector<std::string> blockedSquawks;
		ReadLegacyDisplayModeFlags(filters, legacyProModeEnabled, legacyTowerModeEnabled, acceptPilotSquawk, blockedSquawks);
		if (legacyProModeEnabled && legacyTowerModeEnabled)
			return "Pro + Tower";
		if (legacyProModeEnabled)
			return "Pro";
		if (legacyTowerModeEnabled)
			return "Tower";
		return "Normal";
	}

	void ReadProfileDisplayModes(const rapidjson::Value& profile, std::vector<CSMRRadar::DisplayModeSettings>& outModes, std::string& outActiveName)
	{
		outModes.clear();
		outActiveName = "Normal";
		if (!profile.IsObject())
			return;

		const rapidjson::Value* filters = nullptr;
		if (profile.HasMember("filters") && profile["filters"].IsObject())
			filters = &profile["filters"];

		if (filters == nullptr)
		{
			outModes.push_back(MakeDisplayModeSettings("Normal", false, false));
			return;
		}

		if (filters->HasMember("display_modes") && (*filters)["display_modes"].IsObject())
		{
			const rapidjson::Value& displayModes = (*filters)["display_modes"];
			if (displayModes.HasMember("active") && displayModes["active"].IsString())
			{
				const std::string active = TrimAsciiWhitespaceCopy(displayModes["active"].GetString());
				if (!active.empty())
					outActiveName = active;
			}

			if (displayModes.HasMember("items") && displayModes["items"].IsArray())
			{
				const rapidjson::Value& items = displayModes["items"];
				for (rapidjson::SizeType i = 0; i < items.Size(); ++i)
				{
					CSMRRadar::DisplayModeSettings fallback = MakeDisplayModeSettings("Display Mode", false, false);
					CSMRRadar::DisplayModeSettings mode = ReadDisplayModeValue(items[i], fallback);
					mode.name = MakeUniqueDisplayModeName(outModes, mode.name);
					outModes.push_back(mode);
				}
			}
		}

		if (outModes.empty())
			outModes = BuildLegacyDisplayModes(*filters);
		if (FindDisplayModeIndexNoCase(outModes, outActiveName) < 0)
			outActiveName = outModes.empty() ? "Normal" : outModes.front().name;
	}

	CSMRRadar::DisplayModeSettings ReadActiveDisplayModeFromProfile(const rapidjson::Value& profile)
	{
		std::vector<CSMRRadar::DisplayModeSettings> modes;
		std::string activeName;
		ReadProfileDisplayModes(profile, modes, activeName);
		const int activeIndex = FindDisplayModeIndexNoCase(modes, activeName);
		if (activeIndex >= 0)
			return modes[activeIndex];
		return MakeDisplayModeSettings("Normal", false, false);
	}

	void EnsureProfileDisplayModeDefaults(rapidjson::Value& profile, rapidjson::Document::AllocatorType& allocator)
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
		std::vector<CSMRRadar::DisplayModeSettings> modes;
		std::string activeName;
		ReadProfileDisplayModes(profile, modes, activeName);
		if (modes.empty())
			modes.push_back(MakeDisplayModeSettings("Normal", false, false));

		if (!filters.HasMember("display_modes") || !filters["display_modes"].IsObject())
		{
			if (filters.HasMember("display_modes"))
				filters.RemoveMember("display_modes");
			Value displayModes(rapidjson::kObjectType);
			filters.AddMember("display_modes", displayModes, allocator);
		}

		Value& displayModes = filters["display_modes"];
		if (displayModes.HasMember("active"))
			displayModes.RemoveMember("active");
		Value activeKey;
		activeKey.SetString("active", allocator);
		Value activeValue;
		activeValue.SetString(activeName.c_str(), static_cast<rapidjson::SizeType>(activeName.size()), allocator);
		displayModes.AddMember(activeKey, activeValue, allocator);

		if (displayModes.HasMember("items"))
			displayModes.RemoveMember("items");
		Value itemsKey;
		itemsKey.SetString("items", allocator);
		Value items(rapidjson::kArrayType);
		for (const CSMRRadar::DisplayModeSettings& mode : modes)
		{
			Value modeValue(rapidjson::kObjectType);
			WriteDisplayModeValue(modeValue, mode, allocator);
			items.PushBack(modeValue, allocator);
		}
		displayModes.AddMember(itemsKey, items, allocator);

		if (filters.HasMember("pro_mode"))
			filters.RemoveMember("pro_mode");
		if (filters.HasMember("tower_mode"))
			filters.RemoveMember("tower_mode");
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
	OpenVsmrControlCenterWindow("profiles");
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
	return GetOrderedProfileNamesForUi();
}

std::vector<std::string> CSMRRadar::GetOrderedProfileNamesForUi() const
{
	if (!CurrentConfig)
		return {};

	std::vector<std::string> names = CurrentConfig->getAllProfiles();
	std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
	{
		const bool aIsDefault = EqualsNoCase(a, "Default");
		const bool bIsDefault = EqualsNoCase(b, "Default");
		if (aIsDefault != bIsDefault)
			return aIsDefault;

		std::string lowerA = a;
		std::string lowerB = b;
		std::transform(lowerA.begin(), lowerA.end(), lowerA.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::transform(lowerB.begin(), lowerB.end(), lowerB.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowerA != lowerB)
			return lowerA < lowerB;
		return a < b;
	});
	return names;
}

std::string CSMRRadar::GetActiveProfileNameForEditor() const
{
	if (!CurrentConfig)
		return "";
	return const_cast<CConfig*>(CurrentConfig.get())->getActiveProfileName();
}

bool CSMRRadar::SetActiveProfileForEditor(const std::string& name, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	bool appliedToAnyRadar = false;
	auto applyProfileToRadar = [&](CSMRRadar* radar) -> bool
	{
		if (radar == nullptr || radar->CurrentConfig == nullptr)
			return false;

		const std::vector<std::string> radarProfileNames = radar->CurrentConfig->getAllProfiles();
		const std::string canonicalName = FindCanonicalProfileNameNoCase(radarProfileNames, name);
		if (canonicalName.empty())
			return false;

		radar->LoadProfile(canonicalName);
		radar->LoadCustomFont();
		const std::string activeProfile = radar->CurrentConfig->getActiveProfileName();
		RememberSessionActiveProfile(activeProfile);
		radar->WriteLastActiveProfileToConfig(activeProfile);
		radar->SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
		radar->RequestRefresh();
		return true;
	};

	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (applyProfileToRadar(radar))
			appliedToAnyRadar = true;
	}

	if (!appliedToAnyRadar)
		appliedToAnyRadar = applyProfileToRadar(this);

	if (!appliedToAnyRadar)
		return false;

	if (persistToDisk && !CurrentConfig->saveConfig())
		return false;
	return true;
}

std::vector<CSMRRadar::DisplayModeSettings> CSMRRadar::GetProfileDisplayModesForEditor(const std::string& profileName) const
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return {};

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return {};

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(CurrentConfig->document[targetIndex], modes, activeName);
	return modes;
}

std::string CSMRRadar::GetActiveProfileDisplayModeForEditor(const std::string& profileName) const
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return "";

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return "";

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(CurrentConfig->document[targetIndex], modes, activeName);
	return activeName;
}

CSMRRadar::DisplayModeSettings CSMRRadar::GetActiveDisplayModeSettings() const
{
	if (!CurrentConfig)
		return MakeDisplayModeSettings("Normal", false, false);

	const Value& profile = CurrentConfig->getActiveProfile();
	return ReadActiveDisplayModeFromProfile(profile);
}

bool CSMRRadar::SetProfileDisplayModeActiveForEditor(const std::string& profileName, const std::string& modeName)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	EnsureProfileDisplayModeDefaults(profile, CurrentConfig->document.GetAllocator());
	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(profile, modes, activeName);
	const int modeIndex = FindDisplayModeIndexNoCase(modes, modeName);
	if (modeIndex < 0)
		return false;

	rapidjson::Value& displayModes = profile["filters"]["display_modes"];
	displayModes["active"].SetString(modes[modeIndex].name.c_str(), static_cast<rapidjson::SizeType>(modes[modeIndex].name.size()), CurrentConfig->document.GetAllocator());
	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? profileName : activeBefore);
	InvalidateStructuredTagRuleCache();
	RequestRefresh();
	return true;
}

bool CSMRRadar::AddProfileDisplayModeForEditor(const std::string& profileName, const std::string& requestedName, bool duplicateSelectedMode, const std::string& selectedModeName, std::string* outCreatedName)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	EnsureProfileDisplayModeDefaults(profile, allocator);

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(profile, modes, activeName);

	DisplayModeSettings newMode = MakeDisplayModeSettings("Display Mode", false, false);
	if (duplicateSelectedMode)
	{
		const int selectedIndex = FindDisplayModeIndexNoCase(modes, selectedModeName.empty() ? activeName : selectedModeName);
		if (selectedIndex >= 0)
			newMode = modes[selectedIndex];
	}
	newMode.name = MakeUniqueDisplayModeName(modes, requestedName.empty() ? newMode.name : requestedName);

	rapidjson::Value modeValue(rapidjson::kObjectType);
	WriteDisplayModeValue(modeValue, newMode, allocator);
	profile["filters"]["display_modes"]["items"].PushBack(modeValue, allocator);
	profile["filters"]["display_modes"]["active"].SetString(newMode.name.c_str(), static_cast<rapidjson::SizeType>(newMode.name.size()), allocator);

	if (!CurrentConfig->saveConfig())
		return false;

	if (outCreatedName != nullptr)
		*outCreatedName = newMode.name;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? profileName : activeBefore);
	InvalidateStructuredTagRuleCache();
	RequestRefresh();
	return true;
}

bool CSMRRadar::RenameProfileDisplayModeForEditor(const std::string& profileName, const std::string& oldName, const std::string& newName)
{
	const std::string trimmedNewName = TrimAsciiWhitespaceCopy(newName);
	if (trimmedNewName.empty())
		return false;
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	EnsureProfileDisplayModeDefaults(profile, allocator);

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(profile, modes, activeName);
	const int modeIndex = FindDisplayModeIndexNoCase(modes, oldName);
	if (modeIndex < 0)
		return false;

	for (size_t i = 0; i < modes.size(); ++i)
	{
		if (static_cast<int>(i) != modeIndex && EqualsNoCase(modes[i].name, trimmedNewName))
			return false;
	}

	rapidjson::Value& displayModes = profile["filters"]["display_modes"];
	rapidjson::Value& items = displayModes["items"];
	items[static_cast<rapidjson::SizeType>(modeIndex)]["name"].SetString(trimmedNewName.c_str(), static_cast<rapidjson::SizeType>(trimmedNewName.size()), allocator);
	if (EqualsNoCase(activeName, oldName))
		displayModes["active"].SetString(trimmedNewName.c_str(), static_cast<rapidjson::SizeType>(trimmedNewName.size()), allocator);

	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? profileName : activeBefore);
	InvalidateStructuredTagRuleCache();
	RequestRefresh();
	return true;
}

bool CSMRRadar::DeleteProfileDisplayModeForEditor(const std::string& profileName, const std::string& modeName)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	EnsureProfileDisplayModeDefaults(profile, allocator);

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(profile, modes, activeName);
	if (modes.size() <= 1)
		return false;

	const int modeIndex = FindDisplayModeIndexNoCase(modes, modeName);
	if (modeIndex < 0)
		return false;

	rapidjson::Value& displayModes = profile["filters"]["display_modes"];
	rapidjson::Value& items = displayModes["items"];
	for (rapidjson::SizeType i = static_cast<rapidjson::SizeType>(modeIndex); (i + 1) < items.Size(); ++i)
		items[i] = items[i + 1];
	items.PopBack();

	if (EqualsNoCase(activeName, modeName))
	{
		std::string replacementName = "Normal";
		ReadProfileDisplayModes(profile, modes, activeName);
		if (!modes.empty())
			replacementName = modes.front().name;
		displayModes["active"].SetString(replacementName.c_str(), static_cast<rapidjson::SizeType>(replacementName.size()), allocator);
	}

	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? profileName : activeBefore);
	InvalidateStructuredTagRuleCache();
	RequestRefresh();
	return true;
}

bool CSMRRadar::UpdateProfileDisplayModeForEditor(const std::string& profileName, const DisplayModeSettings& settings)
{
	if (settings.name.empty() || !CurrentConfig || !CurrentConfig->document.IsArray())
		return false;

	const rapidjson::SizeType targetIndex = FindProfileIndexNoCase(CurrentConfig->document, profileName);
	if (targetIndex >= CurrentConfig->document.Size())
		return false;

	rapidjson::Value& profile = CurrentConfig->document[targetIndex];
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	EnsureProfileDisplayModeDefaults(profile, allocator);

	std::vector<DisplayModeSettings> modes;
	std::string activeName;
	ReadProfileDisplayModes(profile, modes, activeName);
	const int modeIndex = FindDisplayModeIndexNoCase(modes, settings.name);
	if (modeIndex < 0)
		return false;

	rapidjson::Value& modeValue = profile["filters"]["display_modes"]["items"][static_cast<rapidjson::SizeType>(modeIndex)];
	WriteDisplayModeValue(modeValue, settings, allocator);
	if (!CurrentConfig->saveConfig())
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	CurrentConfig->reload();
	LoadProfile(activeBefore.empty() ? profileName : activeBefore);
	InvalidateStructuredTagRuleCache();
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
	else
	{
		for (rapidjson::SizeType i = 0; i < CurrentConfig->document.Size(); ++i)
		{
			if (CurrentConfig->document[i].IsObject() &&
				CurrentConfig->document[i].HasMember("name") &&
				CurrentConfig->document[i]["name"].IsString())
			{
				CloneJsonValue(CurrentConfig->document[i], newProfile, CurrentConfig->document.GetAllocator());
				break;
			}
		}

		if (!newProfile.IsObject())
			newProfile.SetObject();
	}

	rapidjson::Value profileNameValue;
	profileNameValue.SetString(createdName.c_str(), static_cast<rapidjson::SizeType>(createdName.size()), CurrentConfig->document.GetAllocator());
	if (newProfile.HasMember("name"))
		newProfile["name"].SetString(createdName.c_str(), static_cast<rapidjson::SizeType>(createdName.size()), CurrentConfig->document.GetAllocator());
	else
		newProfile.AddMember("name", profileNameValue, CurrentConfig->document.GetAllocator());

	EnsureProfileDisplayModeDefaults(newProfile, CurrentConfig->document.GetAllocator());
	CurrentConfig->document.PushBack(newProfile, CurrentConfig->document.GetAllocator());
	if (!CurrentConfig->saveConfig())
		return false;

	CurrentConfig->reload();
	LoadProfile(createdName);
	LoadCustomFont();
	const std::string activeProfile = CurrentConfig->getActiveProfileName();
	RememberSessionActiveProfile(activeProfile);
	WriteLastActiveProfileToConfig(activeProfile);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
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
	const std::vector<CConfig::ProfileSaveIdentity> profileIdentities = {
		{ trimmedNewName, oldName }
	};
	if (!CurrentConfig->saveConfig(profileIdentities))
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
	LoadCustomFont();
	const std::string activeProfile = CurrentConfig->getActiveProfileName();
	RememberSessionActiveProfile(activeProfile);
	WriteLastActiveProfileToConfig(activeProfile);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
	RequestRefresh();
	return true;
}

bool CSMRRadar::DeleteProfileForEditor(const std::string& name)
{
	if (!CurrentConfig || !CurrentConfig->document.IsArray())
		return false;
	if (CurrentConfig->getProfileCount() <= 1)
		return false;

	const std::string activeBefore = CurrentConfig->getActiveProfileName();
	const bool deletingActive = EqualsNoCase(activeBefore, name);
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
	if (deletingActive)
	{
		const std::vector<std::string> names = CurrentConfig->getAllProfiles();
		nextActive = names.empty() ? "Default" : names.front();
	}

	LoadProfile(nextActive);
	LoadCustomFont();
	const std::string activeProfile = CurrentConfig->getActiveProfileName();
	RememberSessionActiveProfile(activeProfile);
	WriteLastActiveProfileToConfig(activeProfile);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
	RequestRefresh();
	return true;
}
