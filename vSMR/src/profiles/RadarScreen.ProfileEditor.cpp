#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include <cctype>
#include <cstring>

namespace
{
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

	std::string FindCanonicalProfileNameNoCase(const std::vector<std::string>& names, const std::string& name)
	{
		for (const std::string& profileName : names)
		{
			if (EqualsNoCase(profileName, name))
				return profileName;
		}
		return "";
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

	int ReadIntMember(const rapidjson::Value& objectValue, const char* key, int fallback, int minValue, int maxValue)
	{
		if (!objectValue.IsObject() || !objectValue.HasMember(key) || !objectValue[key].IsInt())
			return std::clamp(fallback, minValue, maxValue);
		return std::clamp(objectValue[key].GetInt(), minValue, maxValue);
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
		Value boolValue(value);
		objectValue.AddMember(keyValue, boolValue, allocator);
	}

	void WriteIntMember(rapidjson::Value& objectValue, const char* key, int value, rapidjson::Document::AllocatorType& allocator)
	{
		using rapidjson::Value;
		if (objectValue.HasMember(key))
			objectValue.RemoveMember(key);
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value intValue;
		intValue.SetInt(value);
		objectValue.AddMember(keyValue, intValue, allocator);
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
		WriteBoolMember(statusValue, "lineup", statuses.lineup, allocator);
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
		WriteIntMember(modeValue, "max_airborne_altitude_ft", std::clamp(settings.maximumAirborneAltitudeFt, 0, 60000), allocator);
		WriteIntMember(modeValue, "max_airborne_speed_kt", std::clamp(settings.maximumAirborneSpeedKt, 0, 1000), allocator);
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
		settings.maximumAirborneAltitudeFt = ReadIntMember(modeValue, "max_airborne_altitude_ft", settings.maximumAirborneAltitudeFt, 0, 60000);
		settings.maximumAirborneSpeedKt = ReadIntMember(modeValue, "max_airborne_speed_kt", settings.maximumAirborneSpeedKt, 0, 1000);

		if (modeValue.HasMember("statuses") && modeValue["statuses"].IsObject())
		{
			const rapidjson::Value& statuses = modeValue["statuses"];
			settings.statuses.noStatus = ReadBoolMember(statuses, "no_status", settings.statuses.noStatus);
			settings.statuses.push = ReadBoolMember(statuses, "push", settings.statuses.push);
			settings.statuses.startup = ReadBoolMember(statuses, "startup", ReadBoolMember(statuses, "stup", settings.statuses.startup));
			settings.statuses.taxi = ReadBoolMember(statuses, "taxi", settings.statuses.taxi);
			settings.statuses.lineup = ReadBoolMember(statuses, "lineup", ReadBoolMember(statuses, "lnup", settings.statuses.taxi));
			settings.statuses.departure = ReadBoolMember(statuses, "departure", ReadBoolMember(statuses, "depa", settings.statuses.departure));
			settings.statuses.onRunway = ReadBoolMember(statuses, "on_runway", settings.statuses.onRunway);
			settings.statuses.airborne = ReadBoolMember(statuses, "airborne", settings.statuses.airborne);
			settings.statuses.arrivals = ReadBoolMember(statuses, "arrivals", settings.statuses.arrivals);
			settings.statuses.noFlightPlan = ReadBoolMember(statuses, "no_fpl", settings.statuses.noFlightPlan);
			settings.statuses.uncorrelated = ReadBoolMember(statuses, "uncorrelated", settings.statuses.uncorrelated);
		}

		return settings;
	}

	void ReadLegacyDisplayModeFlags(const rapidjson::Value& filters, bool& outProModeEnabled, bool& outTowerModeEnabled, bool& outAcceptPilotSquawk)
	{
		outProModeEnabled = false;
		outTowerModeEnabled = false;
		outAcceptPilotSquawk = true;

		if (!filters.IsObject())
			return;

		if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
		{
			const rapidjson::Value& proMode = filters["pro_mode"];
			outProModeEnabled = ReadBoolMember(proMode, "enabled", ReadBoolMember(proMode, "enable", false));
			outAcceptPilotSquawk = ReadBoolMember(proMode, "accept_pilot_squawk", true);
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
		ReadLegacyDisplayModeFlags(filters, legacyProModeEnabled, legacyTowerModeEnabled, acceptPilotSquawk);

		std::vector<CSMRRadar::DisplayModeSettings> modes;
		modes.push_back(MakeDisplayModeSettings("Normal", false, false));
		modes.push_back(MakeDisplayModeSettings("Pro", true, false));
		modes.push_back(MakeDisplayModeSettings("Tower", false, true));
		modes.push_back(MakeDisplayModeSettings("Pro + Tower", true, true));
		for (CSMRRadar::DisplayModeSettings& mode : modes)
			mode.acceptPilotSquawk = acceptPilotSquawk;
		return modes;
	}

	std::string GetLegacyActiveDisplayModeName(const rapidjson::Value& filters)
	{
		bool legacyProModeEnabled = false;
		bool legacyTowerModeEnabled = false;
		bool acceptPilotSquawk = true;
		ReadLegacyDisplayModeFlags(filters, legacyProModeEnabled, legacyTowerModeEnabled, acceptPilotSquawk);
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

		// Profiles without display_modes retain the old Pro and Tower combinations
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
	return CurrentConfig->getActiveProfileName();
}

bool CSMRRadar::SetActiveProfileForEditor(const std::string& name, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;
	UNREFERENCED_PARAMETER(persistToDisk);

	// Immediate profile changes are shared by every radar using this file. Start
	// from the latest on-disk revision so a stale screen cannot mutate its local
	// document and then report a profile that was never persisted.
	if (!ReloadConfig())
		return false;

	const std::string canonicalName =
		FindCanonicalProfileNameNoCase(CurrentConfig->getAllProfiles(), name);
	if (canonicalName.empty())
		return false;

	// The active profile is session-global for radar screens sharing this source.
	// Persist it once, then reload every live CConfig from that one authoritative
	// write.  Saving independently from every screen races their revision tokens.
	if (RimcasInstance != nullptr)
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
	if (!CurrentConfig->setLastActiveProfileName(canonicalName) ||
		!CurrentConfig->saveConfig())
	{
		// saveConfig is fail-closed on revision conflicts. Discard the staged
		// metadata as well, otherwise this radar would display an unsaved profile.
		ReloadConfig();
		return false;
	}

	bool appliedToAnyRadar = false;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->CurrentConfig == nullptr ||
			!radar->CurrentConfig->sharesConfigFileWith(*CurrentConfig))
		{
			continue;
		}

		if (!radar->ReloadConfig())
			continue;
		const std::string radarCanonicalName = FindCanonicalProfileNameNoCase(
			radar->CurrentConfig->getAllProfiles(),
			canonicalName);
		if (radarCanonicalName.empty())
			continue;

		radar->LoadProfile(radarCanonicalName, false);
		radar->LoadCustomFont();
		const std::string activeProfile = radar->CurrentConfig->getActiveProfileName();
		RememberSessionActiveProfile(activeProfile);
		radar->SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfile.c_str());
		radar->RequestRefresh();
		if (radar->VsmrControlCenterDialog != nullptr)
			radar->VsmrControlCenterDialog->SyncFromRadar("profile");
		appliedToAnyRadar = true;
	}

	if (!appliedToAnyRadar && CurrentConfig != nullptr)
	{
		LoadProfile(canonicalName, false);
		LoadCustomFont();
		RememberSessionActiveProfile(CurrentConfig->getActiveProfileName());
		SaveDataToAsr("ActiveProfile", "vSMR active profile", canonicalName.c_str());
		RequestRefresh();
		if (VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar("profile");
		appliedToAnyRadar = true;
	}

	if (!appliedToAnyRadar)
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

	// Mode activation is an immediate shared-file transaction. Rebase on the
	// latest revision before editing so multiple radar screens cannot retain an
	// unsaved local mode after a stale-write rejection.
	if (!ReloadConfig() || !CurrentConfig->document.IsArray())
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
	{
		ReloadConfig();
		return false;
	}

	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->CurrentConfig == nullptr ||
			!radar->CurrentConfig->sharesConfigFileWith(*CurrentConfig))
		{
			continue;
		}
		radar->ReloadConfig();
		radar->InvalidateStructuredTagRuleCache();
		radar->RequestRefresh();
		if (radar->VsmrControlCenterDialog != nullptr)
			radar->VsmrControlCenterDialog->SyncFromRadar("mode");
	}
	return true;
}
