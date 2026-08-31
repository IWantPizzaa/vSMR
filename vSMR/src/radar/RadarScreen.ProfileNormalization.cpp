#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "shared/RapidJsonUtils.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagDefinitionUtils.hpp"

namespace TagColorRules = VsmrTagColorRules;

void CSMRRadar::EnsureTargetGroundStatusColorEntries(bool persistChanges)
{
	// Backward-compatible profile migration and normalization:
	// ensure required nested objects, color entries and editor settings exist.
	if (!CurrentConfig || CurrentConfig->getProfileCount() == 0)
		return;

	Value& profile = CurrentConfig->getMutableActiveProfile();
	if (!profile.IsObject())
		return;

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
			Value newObject(kObjectType);
			parent.AddMember(keyValue, newObject, allocator);
			changed = true;
		}

		return parent[key];
	};

	auto ensureColorMember = [&](Value& parent, const char* key, int r, int g, int b, int a)
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			colorObject.AddMember("a", a, allocator);
			parent.AddMember(keyValue, colorObject, allocator);
			changed = true;
			return;
		}

		Value& colorObject = parent[key];
		auto ensureComponent = [&](const char* component, int value)
		{
			if (!colorObject.HasMember(component))
			{
				Value componentKey;
				componentKey.SetString(component, allocator);
				Value componentValue;
				componentValue.SetInt(value);
				colorObject.AddMember(componentKey, componentValue, allocator);
				changed = true;
				return;
			}

			if (!colorObject[component].IsInt() || colorObject[component].GetInt() < 0 || colorObject[component].GetInt() > 255)
			{
				colorObject[component].SetInt(value);
				changed = true;
			}
		};

		ensureComponent("r", r);
		ensureComponent("g", g);
		ensureComponent("b", b);
		ensureComponent("a", a);
	};

	auto replaceColorMember = [&](Value& parent, const char* key, const Value& sourceColor)
	{
		if (!sourceColor.IsObject())
			return;

		auto readColorComponent = [&](const char* component, int fallback) -> int
		{
			if (sourceColor.HasMember(component) && sourceColor[component].IsInt())
				return min(255, max(0, sourceColor[component].GetInt()));
			return fallback;
		};

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value colorObject(kObjectType);
		colorObject.AddMember("r", readColorComponent("r", 0), allocator);
		colorObject.AddMember("g", readColorComponent("g", 0), allocator);
		colorObject.AddMember("b", readColorComponent("b", 0), allocator);
		colorObject.AddMember("a", readColorComponent("a", 255), allocator);
		parent.AddMember(keyValue, colorObject, allocator);
		changed = true;
	};

	auto ensureBoolMember = [&](Value& parent, const char* key, bool defaultValue)
	{
		if (parent.HasMember(key) && parent[key].IsBool())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value boolValue;
		boolValue.SetBool(defaultValue);
		parent.AddMember(keyValue, boolValue, allocator);
		changed = true;
	};

	auto ensureIntMember = [&](Value& parent, const char* key, int defaultValue, int minValue, int maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = std::clamp(current, minValue, maxValue);
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value intValue;
		intValue.SetInt(defaultValue);
		parent.AddMember(keyValue, intValue, allocator);
		changed = true;
	};

	auto ensureDoubleMember = [&](Value& parent, const char* key, double defaultValue, double minValue, double maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsNumber())
		{
			double currentValue = parent[key].GetDouble();
			double boundedValue = std::clamp(currentValue, minValue, maxValue);
			if (fabs(currentValue - boundedValue) > 0.0001)
			{
				parent[key].SetDouble(boundedValue);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value numberValue;
		numberValue.SetDouble(defaultValue);
		parent.AddMember(keyValue, numberValue, allocator);
		changed = true;
	};

	auto ensureResolutionPresetMember = [&](Value& parent, const char* key, const char* defaultValue)
	{
		auto normalizePreset = [](const std::string& raw) -> std::string
		{
			std::string lowered = raw;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered.find("4k") != std::string::npos || lowered.find("2160") != std::string::npos || lowered.find("uhd") != std::string::npos)
				return "4k";
			if (lowered.find("2k") != std::string::npos || lowered.find("1440") != std::string::npos || lowered.find("qhd") != std::string::npos)
				return "2k";
			return "1080p";
		};

		if (parent.HasMember(key) && parent[key].IsString())
		{
			const std::string normalized = normalizePreset(parent[key].GetString());
			if (normalized != parent[key].GetString())
			{
				parent[key].SetString(normalized.c_str(), static_cast<rapidjson::SizeType>(normalized.size()), allocator);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		const std::string normalizedDefault = normalizePreset(defaultValue ? defaultValue : "1080p");
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value presetValue;
		presetValue.SetString(normalizedDefault.c_str(), static_cast<rapidjson::SizeType>(normalizedDefault.size()), allocator);
		parent.AddMember(keyValue, presetValue, allocator);
		changed = true;
	};

	auto appendCopiedDefinition = [&](Value& targetArray, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		for (rapidjson::SizeType i = 0; i < sourceArray.Size(); ++i)
		{
			const Value& sourceLine = sourceArray[i];
			Value copiedLine(kArrayType);
			if (sourceLine.IsArray())
			{
				for (rapidjson::SizeType j = 0; j < sourceLine.Size(); ++j)
				{
					if (sourceLine[j].IsString())
					{
						Value tokenValue;
						tokenValue.SetString(sourceLine[j].GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine[j].GetString())), allocator);
						copiedLine.PushBack(tokenValue, allocator);
					}
				}
			}
			else if (sourceLine.IsString())
			{
				Value tokenValue;
				tokenValue.SetString(sourceLine.GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine.GetString())), allocator);
				copiedLine.PushBack(tokenValue, allocator);
			}

			targetArray.PushBack(copiedLine, allocator);
		}
	};

	auto ensureDefinitionArrayMember = [&](Value& parent, const char* key, const Value* fallbackSource)
	{
		if (parent.HasMember(key) && parent[key].IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		if (fallbackSource != nullptr)
			appendCopiedDefinition(definitionArray, *fallbackSource);

		if (definitionArray.Size() == 0)
		{
			Value fallbackLine(kArrayType);
			Value fallbackToken;
			fallbackToken.SetString("callsign", allocator);
			fallbackLine.PushBack(fallbackToken, allocator);
			definitionArray.PushBack(fallbackLine, allocator);
		}

		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto replaceDefinitionArrayMember = [&](Value& parent, const char* key, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		appendCopiedDefinition(definitionArray, sourceArray);
		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto renameMemberIfPresent = [&](Value& parent, const char* oldKey, const char* newKey)
	{
		if (!parent.IsObject() || oldKey == nullptr || newKey == nullptr || strcmp(oldKey, newKey) == 0)
			return;
		if (!parent.HasMember(oldKey))
			return;

		if (parent.HasMember(newKey))
		{
			parent.RemoveMember(oldKey);
			changed = true;
			return;
		}

		Value keyValue;
		keyValue.SetString(newKey, allocator);
		Value copiedValue;
		VsmrRapidJson::CloneJsonValue(parent[oldKey], copiedValue, allocator);
		parent.AddMember(keyValue, copiedValue, allocator);
		parent.RemoveMember(oldKey);
		changed = true;
	};

	auto copyBoolMemberIfPresent = [&](Value& parent, const char* key, const Value& sourceObject)
	{
		if (!sourceObject.IsObject() || !sourceObject.HasMember(key) || !sourceObject[key].IsBool())
			return;

		const bool sourceValue = sourceObject[key].GetBool();
		if (!parent.HasMember(key) || !parent[key].IsBool())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value boolValue(sourceValue);
			parent.AddMember(keyValue, boolValue, allocator);
			changed = true;
			return;
		}

		if (parent[key].GetBool() != sourceValue)
		{
			parent[key].SetBool(sourceValue);
			changed = true;
		}
	};

	auto ensureStringArrayMember = [&](Value& parent, const char* key, const std::vector<std::string>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (const std::string& item : defaults)
		{
			Value itemValue;
			itemValue.SetString(item.c_str(), static_cast<rapidjson::SizeType>(item.size()), allocator);
			outputArray.PushBack(itemValue, allocator);
		}
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	auto ensureIntArrayMember = [&](Value& parent, const char* key, const std::vector<int>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			if (existingArray.Size() == 0)
			{
				rebuild = true;
			}
			else
			{
				for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
				{
					if (!existingArray[i].IsInt())
					{
						rebuild = true;
						break;
					}
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (int value : defaults)
			outputArray.PushBack(value, allocator);
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	ensureIntMember(profile, "schema_version", 2, 2, 9999);

	Value& filters = ensureObjectMember(profile, "filters");
	int legacyMaximumAirborneAltitudeFt = 5500;
	int legacyMaximumAirborneSpeedKt = 250;
	if (filters.HasMember("max_altitude_ft") && filters["max_altitude_ft"].IsInt())
		legacyMaximumAirborneAltitudeFt = std::clamp(filters["max_altitude_ft"].GetInt(), 0, 60000);
	if (filters.HasMember("max_speed_kt") && filters["max_speed_kt"].IsInt())
		legacyMaximumAirborneSpeedKt = std::clamp(filters["max_speed_kt"].GetInt(), 0, 1000);
	changed = filters.RemoveMember("hide_above_alt") || changed;
	changed = filters.RemoveMember("hide_above_spd") || changed;
	changed = filters.RemoveMember("max_altitude_ft") || changed;
	changed = filters.RemoveMember("max_speed_kt") || changed;
	changed = filters.RemoveMember("radar_range_nm") || changed;
	changed = filters.RemoveMember("night_alpha_setting") || changed;
	changed = filters.RemoveMember("night_overlay_alpha") || changed;
	bool legacyProModeEnabled = false;
	bool legacyTowerModeEnabled = false;
	if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
	{
		Value& proMode = filters["pro_mode"];
		renameMemberIfPresent(proMode, "enable", "enabled");
		changed = proMode.RemoveMember("do_not_autocorrelate_squawks") || changed;
		changed = proMode.RemoveMember("blocked_auto_correlate_squawks") || changed;
		if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
			legacyProModeEnabled = proMode["enabled"].GetBool();
	}
	if (filters.HasMember("tower_mode") && filters["tower_mode"].IsObject())
	{
		Value& towerMode = filters["tower_mode"];
		renameMemberIfPresent(towerMode, "enable", "enabled");
		if (towerMode.HasMember("enabled") && towerMode["enabled"].IsBool())
			legacyTowerModeEnabled = towerMode["enabled"].GetBool();
	}
	auto addDisplayMode = [&](Value& items, const char* name, bool requireAssignedSquawk)
	{
		Value mode(kObjectType);
		Value nameKey;
		nameKey.SetString("name", allocator);
		Value nameValue;
		nameValue.SetString(name, allocator);
		mode.AddMember(nameKey, nameValue, allocator);
		mode.AddMember("require_assigned_squawk", requireAssignedSquawk, allocator);
		mode.AddMember("require_clearance", false, allocator);
		mode.AddMember("require_valid_tsat", false, allocator);
		mode.AddMember("require_active_tobt", false, allocator);
		mode.AddMember("max_airborne_altitude_ft", legacyMaximumAirborneAltitudeFt, allocator);
		mode.AddMember("max_airborne_speed_kt", legacyMaximumAirborneSpeedKt, allocator);
		Value statuses(kObjectType);
		statuses.AddMember("no_status", true, allocator);
		statuses.AddMember("push", true, allocator);
		statuses.AddMember("startup", true, allocator);
		statuses.AddMember("taxi", true, allocator);
		statuses.AddMember("lineup", true, allocator);
		statuses.AddMember("departure", true, allocator);
		statuses.AddMember("on_runway", true, allocator);
		statuses.AddMember("airborne", true, allocator);
		statuses.AddMember("arrivals", true, allocator);
		statuses.AddMember("no_fpl", true, allocator);
		statuses.AddMember("uncorrelated", true, allocator);
		mode.AddMember("statuses", statuses, allocator);
		items.PushBack(mode, allocator);
	};
	Value& displayModes = ensureObjectMember(filters, "display_modes");
	if (!displayModes.HasMember("items") || !displayModes["items"].IsArray() || displayModes["items"].Empty())
	{
		if (displayModes.HasMember("items"))
			displayModes.RemoveMember("items");
		Value items(kArrayType);
		addDisplayMode(items, "Normal", false);
		addDisplayMode(items, "Pro", true);
		addDisplayMode(items, "Tower", false);
		addDisplayMode(items, "Pro + Tower", true);
		displayModes.AddMember("items", items, allocator);
		changed = true;
	}
	if (displayModes.HasMember("items") && displayModes["items"].IsArray())
	{
		Value& items = displayModes["items"];
		for (SizeType i = 0; i < items.Size(); ++i)
		{
			if (!items[i].IsObject())
				continue;
			changed = items[i].RemoveMember("do_not_autocorrelate_squawks") || changed;
			changed = items[i].RemoveMember("blocked_auto_correlate_squawks") || changed;
			ensureIntMember(items[i], "max_airborne_altitude_ft", legacyMaximumAirborneAltitudeFt, 0, 60000);
			ensureIntMember(items[i], "max_airborne_speed_kt", legacyMaximumAirborneSpeedKt, 0, 1000);
			Value& statuses = ensureObjectMember(items[i], "statuses");
			if (!statuses.HasMember("lineup") || !statuses["lineup"].IsBool())
			{
				const bool visible = statuses.HasMember("lnup") && statuses["lnup"].IsBool()
					? statuses["lnup"].GetBool()
					: (statuses.HasMember("taxi") && statuses["taxi"].IsBool() ? statuses["taxi"].GetBool() : true);
				ensureBoolMember(statuses, "lineup", visible);
			}
			if (statuses.HasMember("lnup"))
			{
				statuses.RemoveMember("lnup");
				changed = true;
			}
		}
	}
	std::string activeDisplayMode = "Normal";
	if (legacyProModeEnabled && legacyTowerModeEnabled)
		activeDisplayMode = "Pro + Tower";
	else if (legacyProModeEnabled)
		activeDisplayMode = "Pro";
	else if (legacyTowerModeEnabled)
		activeDisplayMode = "Tower";
	if (!displayModes.HasMember("active") || !displayModes["active"].IsString() || displayModes["active"].GetStringLength() == 0)
	{
		if (displayModes.HasMember("active"))
			displayModes.RemoveMember("active");
		Value activeValue;
		activeValue.SetString(activeDisplayMode.c_str(), static_cast<SizeType>(activeDisplayMode.size()), allocator);
		displayModes.AddMember("active", activeValue, allocator);
		changed = true;
	}
	if (filters.HasMember("pro_mode"))
	{
		filters.RemoveMember("pro_mode");
		changed = true;
	}
	if (filters.HasMember("tower_mode"))
	{
		filters.RemoveMember("tower_mode");
		changed = true;
	}

	Value& rimcas = ensureObjectMember(profile, "rimcas");
	changed = rimcas.RemoveMember("rimcas_stage_two_speed_threshold") || changed;
	changed = rimcas.RemoveMember("stage_two_speed_threshold_kt") || changed;
	changed = rimcas.RemoveMember("enabled") || changed;
	changed = rimcas.RemoveMember("rimcas_label_only") || changed;
	changed = rimcas.RemoveMember("use_red_symbol_for_emergencies") || changed;
	ensureIntArrayMember(rimcas, "timer", { 60, 45, 30, 15, 0 });
	ensureIntArrayMember(rimcas, "timer_lvp", { 120, 90, 60, 30, 0 });
	ensureColorMember(rimcas, "background_color_stage_one", 160, 90, 30, 255);
	ensureColorMember(rimcas, "background_color_stage_two", 150, 0, 0, 255);
	ensureColorMember(rimcas, "caution_alert_text_color", 0, 0, 0, 255);
	ensureColorMember(rimcas, "warning_alert_text_color", 255, 255, 255, 255);
	ensureColorMember(rimcas, "caution_alert_background_color", 255, 255, 0, 255);
	ensureColorMember(rimcas, "warning_alert_background_color", 255, 0, 0, 255);
	ensureStringArrayMember(rimcas, "inactive_alerts", {});

	const std::vector<std::string> defaultTagFonts = {
		"EuroScope",
		"Consolas",
		"Lucida Console",
		"Courier New",
		"Segoe UI",
		"Tahoma",
		"Arial",
		"ods",
		"Deesse Medium"
	};

	Value& font = ensureObjectMember(profile, "font");
	if (!font.HasMember("font_name") || !font["font_name"].IsString() || strlen(font["font_name"].GetString()) == 0)
	{
		if (font.HasMember("font_name"))
			font.RemoveMember("font_name");

		Value keyValue;
		keyValue.SetString("font_name", allocator);
		Value fontNameValue;
		fontNameValue.SetString("EuroScope", allocator);
		font.AddMember(keyValue, fontNameValue, allocator);
		changed = true;
	}

	if (!font.HasMember("weight") || !font["weight"].IsString())
	{
		if (font.HasMember("weight"))
			font.RemoveMember("weight");

		Value keyValue;
		keyValue.SetString("weight", allocator);
		Value weightValue;
		weightValue.SetString("Regular", allocator);
		font.AddMember(keyValue, weightValue, allocator);
		changed = true;
	}

	Value& fontSizes = ensureObjectMember(font, "sizes");
	auto ensureFontSizeMember = [&](Value& parent, const char* key, int fallback)
	{
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = (current < 6) ? 6 : current;
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value value;
		value.SetInt((fallback < 6) ? 6 : fallback);
		parent.AddMember(keyValue, value, allocator);
		changed = true;
	};

	ensureFontSizeMember(fontSizes, "one", 10);
	ensureFontSizeMember(fontSizes, "two", 11);
	ensureFontSizeMember(fontSizes, "three", 12);
	ensureFontSizeMember(fontSizes, "four", 13);
	ensureFontSizeMember(fontSizes, "five", 14);

	auto ensureAvailableFontList = [&](Value& fontObject, const char* key)
	{
		bool rebuild = false;
		if (!fontObject.HasMember(key) || !fontObject[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			Value& existingArray = fontObject[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (rebuild)
		{
			if (fontObject.HasMember(key))
				fontObject.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value fontArray(kArrayType);
			for (const std::string& fontName : defaultTagFonts)
			{
				Value fontValue;
				fontValue.SetString(fontName.c_str(), static_cast<rapidjson::SizeType>(fontName.size()), allocator);
				fontArray.PushBack(fontValue, allocator);
			}
			fontObject.AddMember(keyValue, fontArray, allocator);
			changed = true;
			return;
		}
	};

	ensureAvailableFontList(font, "available_fonts");
	ensureIntMember(font, "label_font_size", 1, 1, 5);

	Value& targets = ensureObjectMember(profile, "targets");
	renameMemberIfPresent(targets, "small_icon_boost_resolution", "small_icon_boost_resolution_preset");
	if (!targets.HasMember("icon_style") || !targets["icon_style"].IsString())
	{
		if (targets.HasMember("icon_style"))
			targets.RemoveMember("icon_style");

		Value keyValue;
		keyValue.SetString("icon_style", allocator);
		Value value;
		value.SetString("realistic", allocator);
		targets.AddMember(keyValue, value, allocator);
		changed = true;
	}
	else
	{
		const std::string normalizedIconStyle = NormalizeTargetIconStyle(targets["icon_style"].GetString());
		if (normalizedIconStyle != targets["icon_style"].GetString())
		{
			targets["icon_style"].SetString(normalizedIconStyle.c_str(), static_cast<rapidjson::SizeType>(normalizedIconStyle.size()), allocator);
			changed = true;
		}
	}
	ensureResolutionPresetMember(targets, "small_icon_boost_resolution_preset", "1080p");
	ensureBoolMember(targets, "trail_enabled", true);
	ensureIntMember(targets, "trail_ground_points", 4, 0, 16);
	ensureIntMember(targets, "trail_airborne_points", 8, 0, 16);
	ensureDoubleMember(targets, "symbol_scale", 1.0, 0.5, 1.5);
	changed = targets.RemoveMember("small_icon_boost") || changed;
	changed = targets.RemoveMember("small_icon_boost_factor") || changed;
	changed = targets.RemoveMember("fixed_pixel_icon_size") || changed;
	changed = targets.RemoveMember("fixed_pixel_icon_scale") || changed;
	changed = targets.RemoveMember("fixed_pixel_triangle_scale") || changed;
	changed = targets.RemoveMember("show_primary_target") || changed;
	ensureColorMember(targets, "target_color", 255, 242, 73, 255);
	changed = targets.RemoveMember("history_one_color") || changed;
	changed = targets.RemoveMember("history_two_color") || changed;
	changed = targets.RemoveMember("history_three_color") || changed;

	Value* legacyGroundIcons = nullptr;
	if (targets.HasMember("ground_icons") && targets["ground_icons"].IsObject())
		legacyGroundIcons = &targets["ground_icons"];

	Value& departureIcons = ensureObjectMember(targets, "departure");
	Value& arrivalIcons = ensureObjectMember(targets, "arrival");

	auto migrateTargetIconColor = [&](Value& destination, const char* destinationKey, int r, int g, int b, int a, const char* legacyPrimary, const char* legacySecondary = nullptr, const char* legacyTertiary = nullptr)
	{
		if ((!destination.HasMember(destinationKey) || !destination[destinationKey].IsObject()) && legacyGroundIcons != nullptr)
		{
			const Value* sourceColor = nullptr;
			auto pickLegacyColor = [&](const char* legacyKey) -> bool
			{
				if (legacyKey == nullptr || !legacyGroundIcons->HasMember(legacyKey))
					return false;
				const Value& candidate = (*legacyGroundIcons)[legacyKey];
				if (!candidate.IsObject())
					return false;
				sourceColor = &candidate;
				return true;
			};

			if (!pickLegacyColor(legacyPrimary))
				if (!pickLegacyColor(legacySecondary))
					pickLegacyColor(legacyTertiary);

			if (sourceColor != nullptr)
				replaceColorMember(destination, destinationKey, *sourceColor);
		}

		ensureColorMember(destination, destinationKey, r, g, b, a);
	};

	migrateTargetIconColor(departureIcons, "airborne", 240, 240, 240, 255, "airborne_departure");
	migrateTargetIconColor(departureIcons, "departure", 240, 240, 240, 255, "depa");
	migrateTargetIconColor(departureIcons, "gate", 165, 165, 165, 255, "departure_gate", "gate");
	migrateTargetIconColor(departureIcons, "no_fpl", 128, 128, 128, 255, "nofpl");
	migrateTargetIconColor(departureIcons, "no_status", 165, 165, 165, 255, "nsts");
	migrateTargetIconColor(departureIcons, "push", 253, 218, 13, 255, "push");
	migrateTargetIconColor(departureIcons, "startup", 253, 218, 13, 255, "stup");
	migrateTargetIconColor(departureIcons, "taxi", 240, 240, 240, 255, "taxi");
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) && legacyGroundIcons != nullptr)
	{
		const Value* legacyLineupColor = nullptr;
		if (legacyGroundIcons->HasMember("lnup") && (*legacyGroundIcons)["lnup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lnup"];
		else if (legacyGroundIcons->HasMember("lineup") && (*legacyGroundIcons)["lineup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lineup"];
		if (legacyLineupColor != nullptr)
			replaceColorMember(departureIcons, "lineup", *legacyLineupColor);
	}
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) &&
		departureIcons.HasMember("taxi") && departureIcons["taxi"].IsObject())
	{
		Value taxiColorCopy;
		VsmrRapidJson::CloneJsonValue(departureIcons["taxi"], taxiColorCopy, allocator);
		replaceColorMember(departureIcons, "lineup", taxiColorCopy);
	}
	ensureColorMember(departureIcons, "lineup", 240, 240, 240, 255);

	migrateTargetIconColor(arrivalIcons, "airborne", 120, 190, 240, 255, "airborne_arrival");
	migrateTargetIconColor(arrivalIcons, "gate", 165, 165, 165, 255, "arrival_gate", "gate");
	migrateTargetIconColor(arrivalIcons, "on_ground", 165, 165, 165, 255, "arr", "arrival_taxi");

	if (targets.HasMember("ground_icons"))
	{
		targets.RemoveMember("ground_icons");
		changed = true;
	}

	if (!profile.HasMember("sid_text_colors") || !profile["sid_text_colors"].IsArray())
	{
		if (profile.HasMember("sid_text_colors"))
			profile.RemoveMember("sid_text_colors");

		Value sidTextColorsKey;
		sidTextColorsKey.SetString("sid_text_colors", allocator);
		Value sidTextColorsArray(kArrayType);
		profile.AddMember(sidTextColorsKey, sidTextColorsArray, allocator);
		changed = true;
	}

	Value& labels = ensureObjectMember(profile, "labels");
	changed = labels.RemoveMember("use_aspeed_for_gate") || changed;
	changed = labels.RemoveMember("use_speed_for_gate") || changed;
	changed = labels.RemoveMember("leader_line_length") || changed;
	changed = labels.RemoveMember("use_departure_arrival_coloring") || changed;
	renameMemberIfPresent(labels, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	ensureBoolMember(labels, "auto_deconfliction", true);
	ensureBoolMember(labels, "rounded_corners", true);
	ensureBoolMember(labels, "definition_detailed_inherits_normal", false);
	if (labels.HasMember("sid_text_colors"))
	{
		labels.RemoveMember("sid_text_colors");
		changed = true;
	}

	Value& departureLabel = ensureObjectMember(labels, "departure");
	renameMemberIfPresent(departureLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(departureLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(departureLabel, "background_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "gate_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "on_runway_color", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(departureLabel, "nofpl_color", "background_no_fpl_color");
	renameMemberIfPresent(departureLabel, "nosid_color", "background_no_sid_color");
	renameMemberIfPresent(departureLabel, "push_color", "background_push_color");
	renameMemberIfPresent(departureLabel, "startup_color", "background_startup_color");
	renameMemberIfPresent(departureLabel, "taxi_color", "background_taxi_color");
	renameMemberIfPresent(departureLabel, "lineup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "lnup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "departure_color", "background_departure_color");
	if (departureLabel.HasMember("status_background_colors") && departureLabel["status_background_colors"].IsObject())
	{
		Value& departureStatusColors = departureLabel["status_background_colors"];
		if (departureStatusColors.HasMember("nsts") && departureStatusColors["nsts"].IsObject())
			replaceColorMember(departureLabel, "background_no_status_color", departureStatusColors["nsts"]);
		if (departureStatusColors.HasMember("push") && departureStatusColors["push"].IsObject())
			replaceColorMember(departureLabel, "background_push_color", departureStatusColors["push"]);
		if (departureStatusColors.HasMember("stup") && departureStatusColors["stup"].IsObject())
			replaceColorMember(departureLabel, "background_startup_color", departureStatusColors["stup"]);
		if (departureStatusColors.HasMember("taxi") && departureStatusColors["taxi"].IsObject())
			replaceColorMember(departureLabel, "background_taxi_color", departureStatusColors["taxi"]);
		if (departureStatusColors.HasMember("lnup") && departureStatusColors["lnup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lnup"]);
		else if (departureStatusColors.HasMember("lineup") && departureStatusColors["lineup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lineup"]);
		if (departureStatusColors.HasMember("depa") && departureStatusColors["depa"].IsObject())
			replaceColorMember(departureLabel, "background_departure_color", departureStatusColors["depa"]);
		departureLabel.RemoveMember("status_background_colors");
		changed = true;
	}
	ensureColorMember(departureLabel, "background_no_status_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "background_on_runway_color", 40, 50, 200, 255);
	ensureColorMember(departureLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(departureLabel, "background_push_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_startup_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_taxi_color", 240, 240, 240, 255);
	if ((!departureLabel.HasMember("background_lineup_color") || !departureLabel["background_lineup_color"].IsObject()) &&
		departureLabel.HasMember("background_taxi_color") && departureLabel["background_taxi_color"].IsObject())
	{
		Value taxiColorCopy;
		VsmrRapidJson::CloneJsonValue(departureLabel["background_taxi_color"], taxiColorCopy, allocator);
		replaceColorMember(departureLabel, "background_lineup_color", taxiColorCopy);
	}
	ensureColorMember(departureLabel, "background_lineup_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_departure_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_no_fpl_color", 128, 128, 128, 255);
	ensureColorMember(departureLabel, "background_no_sid_color", 53, 126, 187, 255);

	Value& arrivalLabel = ensureObjectMember(labels, "arrival");
	renameMemberIfPresent(arrivalLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(arrivalLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(arrivalLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(arrivalLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "nofpl_color", "background_no_fpl_color");
	ensureColorMember(arrivalLabel, "background_on_ground_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "background_on_runway_color", 170, 50, 50, 255);
	ensureColorMember(arrivalLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_no_fpl_color", 128, 128, 128, 255);
	if (arrivalLabel.HasMember("status_background_colors") && arrivalLabel["status_background_colors"].IsObject())
	{
		Value& arrivalStatusColors = arrivalLabel["status_background_colors"];
		if (arrivalStatusColors.HasMember("arr") && arrivalStatusColors["arr"].IsObject())
			replaceColorMember(arrivalLabel, "background_on_ground_color", arrivalStatusColors["arr"]);
		arrivalLabel.RemoveMember("status_background_colors");
		changed = true;
	}

	Value& airborneLabel = ensureObjectMember(labels, "airborne");
	if (airborneLabel.HasMember("departure_background_color") && airborneLabel["departure_background_color"].IsObject())
		replaceColorMember(departureLabel, "background_airborne_color", airborneLabel["departure_background_color"]);
	if (airborneLabel.HasMember("departure_text_color") && airborneLabel["departure_text_color"].IsObject())
		replaceColorMember(departureLabel, "text_airborne_color", airborneLabel["departure_text_color"]);
	if (airborneLabel.HasMember("arrival_background_color") && airborneLabel["arrival_background_color"].IsObject())
		replaceColorMember(arrivalLabel, "background_airborne_color", airborneLabel["arrival_background_color"]);
	if (airborneLabel.HasMember("arrival_text_color") && airborneLabel["arrival_text_color"].IsObject())
		replaceColorMember(arrivalLabel, "text_airborne_color", airborneLabel["arrival_text_color"]);
	ensureColorMember(departureLabel, "background_airborne_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "text_airborne_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_airborne_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "text_airborne_color", 255, 255, 255, 255);
	if (airborneLabel.HasMember("background_color"))
	{
		airborneLabel.RemoveMember("background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("background_color_on_runway"))
	{
		airborneLabel.RemoveMember("background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("text_color"))
	{
		airborneLabel.RemoveMember("text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("departure_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("arrival_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color"))
	{
		airborneLabel.RemoveMember("departure_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color"))
	{
		airborneLabel.RemoveMember("arrival_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_text_color"))
	{
		airborneLabel.RemoveMember("departure_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_text_color"))
	{
		airborneLabel.RemoveMember("arrival_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("use_departure_arrival_coloring"))
	{
		airborneLabel.RemoveMember("use_departure_arrival_coloring");
		changed = true;
	}

	Value& uncorrelatedLabel = ensureObjectMember(labels, "uncorrelated");
	renameMemberIfPresent(uncorrelatedLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(uncorrelatedLabel, "background_color_on_runway", "background_on_runway_color");
	ensureColorMember(uncorrelatedLabel, "background_on_ground_color", 150, 22, 135, 255);
	ensureColorMember(uncorrelatedLabel, "background_on_runway_color", 150, 22, 135, 50);

	ensureDefinitionArrayMember(departureLabel, "definition", nullptr);
	const Value* baseDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(departureLabel, "definition_detailed", baseDefinition);
	Value& departureStatusDefinitions = ensureObjectMember(departureLabel, "status_definitions");
	renameMemberIfPresent(departureStatusDefinitions, "lineup", "lnup");
	if (departureStatusDefinitions.HasMember("nsts") && departureStatusDefinitions["nsts"].IsObject())
	{
		Value& departureNstsSection = departureStatusDefinitions["nsts"];
		renameMemberIfPresent(departureNstsSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(departureNstsSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		if (departureNstsSection.HasMember("definition") && departureNstsSection["definition"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition", departureNstsSection["definition"]);
		if (departureNstsSection.HasMember("definition_detailed") && departureNstsSection["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition_detailed", departureNstsSection["definition_detailed"]);
		copyBoolMemberIfPresent(departureLabel, "definition_detailed_inherits_normal", departureNstsSection);
		departureStatusDefinitions.RemoveMember("nsts");
		changed = true;
	}

	auto ensureStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(departureStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (departureLabel.HasMember("definition_detailed") && departureLabel["definition_detailed"].IsArray()) ? &departureLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	if ((!departureStatusDefinitions.HasMember("lnup") || !departureStatusDefinitions["lnup"].IsObject()) &&
		departureStatusDefinitions.HasMember("taxi") && departureStatusDefinitions["taxi"].IsObject())
	{
		if (departureStatusDefinitions.HasMember("lnup"))
			departureStatusDefinitions.RemoveMember("lnup");
		Value keyValue;
		keyValue.SetString("lnup", allocator);
		Value copiedValue;
		VsmrRapidJson::CloneJsonValue(departureStatusDefinitions["taxi"], copiedValue, allocator);
		departureStatusDefinitions.AddMember(keyValue, copiedValue, allocator);
		changed = true;
	}
	ensureStatusDefinitionEntries("taxi");
	ensureStatusDefinitionEntries("lnup");
	ensureStatusDefinitionEntries("push");
	ensureStatusDefinitionEntries("stup");
	ensureStatusDefinitionEntries("depa");
	ensureStatusDefinitionEntries("nofpl");
	ensureStatusDefinitionEntries("airdep");
	ensureStatusDefinitionEntries("airdep_onrunway");

	ensureDefinitionArrayMember(arrivalLabel, "definition", nullptr);
	const Value* arrivalBaseDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(arrivalLabel, "definition_detailed", arrivalBaseDefinition);
	Value& arrivalStatusDefinitions = ensureObjectMember(arrivalLabel, "status_definitions");
	if (arrivalStatusDefinitions.HasMember("arr"))
	{
		arrivalStatusDefinitions.RemoveMember("arr");
		changed = true;
	}
	if (arrivalStatusDefinitions.HasMember("taxi"))
	{
		arrivalStatusDefinitions.RemoveMember("taxi");
		changed = true;
	}
	auto ensureArrivalStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(arrivalStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (arrivalLabel.HasMember("definition_detailed") && arrivalLabel["definition_detailed"].IsArray()) ? &arrivalLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	ensureArrivalStatusDefinitionEntries("nofpl");
	ensureArrivalStatusDefinitionEntries("airarr");
	ensureArrivalStatusDefinitionEntries("airarr_onrunway");

	auto copyStatusDefinitionFromSource = [&](Value& destinationStatusDefinitions, const char* destinationStatusKey, const Value* sourceSection)
	{
		if (destinationStatusKey == nullptr || sourceSection == nullptr || !sourceSection->IsObject())
			return;

		Value& destinationSection = ensureObjectMember(destinationStatusDefinitions, destinationStatusKey);
		renameMemberIfPresent(destinationSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(destinationSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");

		if (sourceSection->HasMember("definition") && (*sourceSection)["definition"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition", (*sourceSection)["definition"]);
		if (sourceSection->HasMember("definition_detailed") && (*sourceSection)["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definition_detailed"]);
		else if (sourceSection->HasMember("definitionDetailled") && (*sourceSection)["definitionDetailled"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definitionDetailled"]);

		copyBoolMemberIfPresent(destinationSection, "definition_detailed_inherits_normal", *sourceSection);
	};

	const Value* airborneDefinition = nullptr;
	if (airborneLabel.HasMember("definition") && airborneLabel["definition"].IsArray())
		airborneDefinition = &airborneLabel["definition"];

	auto findAirborneStatusSection = [&](const char* statusKey) -> const Value*
	{
		if (statusKey == nullptr)
			return nullptr;
		if (!airborneLabel.HasMember("status_definitions") || !airborneLabel["status_definitions"].IsObject())
			return nullptr;
		const Value& airborneStatusDefinitions = airborneLabel["status_definitions"];
		if (!airborneStatusDefinitions.HasMember(statusKey) || !airborneStatusDefinitions[statusKey].IsObject())
			return nullptr;
		return &airborneStatusDefinitions[statusKey];
	};

	const Value* sourceDepartureAirborne = findAirborneStatusSection("airdep");
	if (sourceDepartureAirborne == nullptr && airborneDefinition != nullptr)
		sourceDepartureAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep", sourceDepartureAirborne);

	const Value* sourceDepartureOnRunway = findAirborneStatusSection("airdep_onrunway");
	if (sourceDepartureOnRunway == nullptr)
		sourceDepartureOnRunway = sourceDepartureAirborne;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep_onrunway", sourceDepartureOnRunway);

	const Value* sourceArrivalAirborne = findAirborneStatusSection("airarr");
	if (sourceArrivalAirborne == nullptr && airborneDefinition != nullptr)
		sourceArrivalAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr", sourceArrivalAirborne);

	const Value* sourceArrivalOnRunway = findAirborneStatusSection("airarr_onrunway");
	if (sourceArrivalOnRunway == nullptr)
		sourceArrivalOnRunway = sourceArrivalAirborne;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr_onrunway", sourceArrivalOnRunway);

	Value legacyLabelRules(kObjectType);
	bool hasLegacyLabelRules = false;
	if (labels.HasMember("rules") && labels["rules"].IsObject())
	{
		VsmrRapidJson::CloneJsonValue(labels["rules"], legacyLabelRules, allocator);
		hasLegacyLabelRules = true;
	}

	Value& structuredRules = ensureObjectMember(profile, "rules");
	const bool structuredRulesHasItems = structuredRules.HasMember("items") && structuredRules["items"].IsArray();
	if (!structuredRulesHasItems && hasLegacyLabelRules &&
		legacyLabelRules.HasMember("items") && legacyLabelRules["items"].IsArray())
	{
		VsmrRapidJson::CloneJsonValue(legacyLabelRules, structuredRules, allocator);
		changed = true;
	}

	if (hasLegacyLabelRules)
	{
		Value& labelsForRulesCleanup = profile["labels"];
		labelsForRulesCleanup.RemoveMember("rules");
		changed = true;
	}

	ensureIntMember(structuredRules, "version", 1, 1, 1000);
	if (!structuredRules.HasMember("items") || !structuredRules["items"].IsArray())
	{
		if (structuredRules.HasMember("items"))
			structuredRules.RemoveMember("items");
		Value itemsKey;
		itemsKey.SetString("items", allocator);
		Value itemsArray(kArrayType);
		structuredRules.AddMember(itemsKey, itemsArray, allocator);
		changed = true;
	}
	Value& structuredRuleItems = structuredRules["items"];

	auto parseRuleColor = [&](const Value& item, const char* key, bool& outApply, int& outR, int& outG, int& outB)
	{
		outApply = false;
		outR = 255;
		outG = 255;
		outB = 255;
		if (!item.IsObject() || !item.HasMember(key) || !item[key].IsObject())
			return false;

		const Value& color = item[key];
		if (!color.HasMember("r") || !color["r"].IsInt() ||
			!color.HasMember("g") || !color["g"].IsInt() ||
			!color.HasMember("b") || !color["b"].IsInt())
		{
			return false;
		}

		outApply = true;
		outR = std::clamp(color["r"].GetInt(), 0, 255);
		outG = std::clamp(color["g"].GetInt(), 0, 255);
		outB = std::clamp(color["b"].GetInt(), 0, 255);
		return true;
	};

	auto buildRuleSignature = [&](const std::string& source, const std::string& token, const std::string& condition,
		const std::string& tagType, const std::string& status, const std::string& detail,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB) -> std::string
	{
		std::ostringstream signature;
		signature << source << "|"
			<< token << "|"
			<< condition << "|"
			<< tagType << "|"
			<< status << "|"
			<< detail << "|"
			<< (applyTarget ? 1 : 0) << ":" << targetR << "," << targetG << "," << targetB << "|"
			<< (applyTag ? 1 : 0) << ":" << tagR << "," << tagG << "," << tagB << "|"
			<< (applyText ? 1 : 0) << ":" << textR << "," << textG << "," << textB;
		return signature.str();
	};

	std::set<std::string> existingRuleSignatures;
	for (rapidjson::SizeType i = 0; i < structuredRuleItems.Size(); ++i)
	{
		if (!structuredRuleItems[i].IsObject())
			continue;

		const Value& item = structuredRuleItems[i];
		std::string source = "vacdm";
		if (item.HasMember("source") && item["source"].IsString())
			source = item["source"].GetString();
		else if (item.HasMember("kind") && item["kind"].IsString())
			source = item["kind"].GetString();
		source = NormalizeStructuredRuleSource(source);

		std::string token;
		if (item.HasMember("token") && item["token"].IsString())
			token = item["token"].GetString();
		token = NormalizeStructuredRuleToken(source, token);
		if (token.empty())
			continue;

		std::string condition = "any";
		if (item.HasMember("condition") && item["condition"].IsString())
			condition = item["condition"].GetString();
		else if (source == "runway" && item.HasMember("runway") && item["runway"].IsString())
			condition = item["runway"].GetString();
		else if (source != "runway" && item.HasMember("state") && item["state"].IsString())
			condition = item["state"].GetString();
		condition = NormalizeStructuredRuleCondition(source, condition);

		std::string tagType = "any";
		if (item.HasMember("tag_type") && item["tag_type"].IsString())
			tagType = item["tag_type"].GetString();
		tagType = NormalizeStructuredRuleTagType(tagType);

		std::string status = "any";
		if (item.HasMember("status") && item["status"].IsString())
			status = item["status"].GetString();
		status = NormalizeStructuredRuleStatus(status);

		std::string detail = "any";
		if (item.HasMember("detail") && item["detail"].IsString())
			detail = item["detail"].GetString();
		detail = NormalizeStructuredRuleDetail(detail);

		bool applyTarget = false;
		bool applyTag = false;
		bool applyText = false;
		int targetR = 255, targetG = 255, targetB = 255;
		int tagR = 255, tagG = 255, tagB = 255;
		int textR = 255, textG = 255, textB = 255;
		parseRuleColor(item, "target_color", applyTarget, targetR, targetG, targetB);
		parseRuleColor(item, "tag_color", applyTag, tagR, tagG, tagB);
		parseRuleColor(item, "text_color", applyText, textR, textG, textB);
		if (!applyTarget && !applyTag && !applyText)
			continue;

		existingRuleSignatures.insert(buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB));
	}

	auto appendStructuredRule = [&](const std::string& sourceRaw, const std::string& tokenRaw, const std::string& conditionRaw,
		const std::string& tagTypeRaw, const std::string& statusRaw, const std::string& detailRaw,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB)
	{
		const std::string source = NormalizeStructuredRuleSource(sourceRaw);
		const std::string token = NormalizeStructuredRuleToken(source, tokenRaw);
		if (token.empty())
			return;
		const std::string condition = NormalizeStructuredRuleCondition(source, conditionRaw);
		const std::string tagType = NormalizeStructuredRuleTagType(tagTypeRaw);
		const std::string status = NormalizeStructuredRuleStatus(statusRaw);
		const std::string detail = NormalizeStructuredRuleDetail(detailRaw);

		targetR = std::clamp(targetR, 0, 255);
		targetG = std::clamp(targetG, 0, 255);
		targetB = std::clamp(targetB, 0, 255);
		tagR = std::clamp(tagR, 0, 255);
		tagG = std::clamp(tagG, 0, 255);
		tagB = std::clamp(tagB, 0, 255);
		textR = std::clamp(textR, 0, 255);
		textG = std::clamp(textG, 0, 255);
		textB = std::clamp(textB, 0, 255);

		if (!applyTarget && !applyTag && !applyText)
			return;

		const std::string signature = buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB);
		if (!existingRuleSignatures.insert(signature).second)
			return;

		Value ruleObject(kObjectType);
		Value sourceKey;
		sourceKey.SetString("source", allocator);
		Value sourceValue;
		sourceValue.SetString(source.c_str(), static_cast<rapidjson::SizeType>(source.size()), allocator);
		ruleObject.AddMember(sourceKey, sourceValue, allocator);

		Value tokenKey;
		tokenKey.SetString("token", allocator);
		Value tokenValue;
		tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
		ruleObject.AddMember(tokenKey, tokenValue, allocator);

		Value conditionKey;
		conditionKey.SetString("condition", allocator);
		Value conditionValue;
		conditionValue.SetString(condition.c_str(), static_cast<rapidjson::SizeType>(condition.size()), allocator);
		ruleObject.AddMember(conditionKey, conditionValue, allocator);

		Value tagTypeKey;
		tagTypeKey.SetString("tag_type", allocator);
		Value tagTypeValue;
		tagTypeValue.SetString(tagType.c_str(), static_cast<rapidjson::SizeType>(tagType.size()), allocator);
		ruleObject.AddMember(tagTypeKey, tagTypeValue, allocator);

		Value statusKey;
		statusKey.SetString("status", allocator);
		Value statusValue;
		statusValue.SetString(status.c_str(), static_cast<rapidjson::SizeType>(status.size()), allocator);
		ruleObject.AddMember(statusKey, statusValue, allocator);

		Value detailKey;
		detailKey.SetString("detail", allocator);
		Value detailValue;
		detailValue.SetString(detail.c_str(), static_cast<rapidjson::SizeType>(detail.size()), allocator);
		ruleObject.AddMember(detailKey, detailValue, allocator);

		auto appendRuleColor = [&](const char* key, bool apply, int r, int g, int b)
		{
			if (!apply)
				return;
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			Value colorKey;
			colorKey.SetString(key, allocator);
			ruleObject.AddMember(colorKey, colorObject, allocator);
		};

		appendRuleColor("target_color", applyTarget, targetR, targetG, targetB);
		appendRuleColor("tag_color", applyTag, tagR, tagG, tagB);
		appendRuleColor("text_color", applyText, textR, textG, textB);

		structuredRuleItems.PushBack(ruleObject, allocator);
		changed = true;
	};

	auto migrateDefinitionArray = [&](Value& definitionArray, const std::string& tagType, const std::string& status, const std::string& detail)
	{
		// Move legacy inline color-rule tokens into structured rules and keep display tokens intact.
		if (!definitionArray.IsArray())
			return;

		for (rapidjson::SizeType lineIndex = 0; lineIndex < definitionArray.Size(); ++lineIndex)
		{
			Value& lineValue = definitionArray[lineIndex];
			std::vector<std::string> sourceTokens;
			if (lineValue.IsArray())
			{
				for (rapidjson::SizeType tokenIndex = 0; tokenIndex < lineValue.Size(); ++tokenIndex)
				{
					if (lineValue[tokenIndex].IsString())
						sourceTokens.push_back(lineValue[tokenIndex].GetString());
				}
			}
			else if (lineValue.IsString())
			{
				sourceTokens = SplitDefinitionTokens(lineValue.GetString());
			}
			else
			{
				continue;
			}

			if (sourceTokens.empty())
				continue;

			bool removedRuleToken = false;
			std::vector<std::string> keptTokens;
			keptTokens.reserve(sourceTokens.size());

			for (const std::string& rawToken : sourceTokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;

				TagColorRules::VacdmColorRuleDefinition vacdmRuleToken;
				if (TagColorRules::TryParseVacdmColorRuleToken(baseToken, vacdmRuleToken))
				{
					appendStructuredRule("vacdm", vacdmRuleToken.token, vacdmRuleToken.expectedState, tagType, status, detail,
						vacdmRuleToken.hasTargetColor, vacdmRuleToken.targetR, vacdmRuleToken.targetG, vacdmRuleToken.targetB,
						vacdmRuleToken.hasTagColor, vacdmRuleToken.tagR, vacdmRuleToken.tagG, vacdmRuleToken.tagB,
						vacdmRuleToken.hasTextColor, vacdmRuleToken.textR, vacdmRuleToken.textG, vacdmRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				TagColorRules::RunwayColorRuleDefinition runwayRuleToken;
				if (TagColorRules::TryParseRunwayColorRuleToken(baseToken, runwayRuleToken))
				{
					appendStructuredRule("runway", runwayRuleToken.token, runwayRuleToken.expectedRunway, tagType, status, detail,
						runwayRuleToken.hasTargetColor, runwayRuleToken.targetR, runwayRuleToken.targetG, runwayRuleToken.targetB,
						runwayRuleToken.hasTagColor, runwayRuleToken.tagR, runwayRuleToken.tagG, runwayRuleToken.tagB,
						runwayRuleToken.hasTextColor, runwayRuleToken.textR, runwayRuleToken.textG, runwayRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				keptTokens.push_back(rawToken);
			}

			if (!removedRuleToken)
				continue;

			Value newLine(kArrayType);
			for (const std::string& token : keptTokens)
			{
				Value tokenValue;
				tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
				newLine.PushBack(tokenValue, allocator);
			}

			lineValue = newLine;
			changed = true;
		}
	};

	auto migrateTypeDefinitions = [&](const char* typeKey)
	{
		Value& labelsForMigration = profile["labels"];
		if (!labelsForMigration.HasMember(typeKey) || !labelsForMigration[typeKey].IsObject())
			return;

		Value& section = labelsForMigration[typeKey];
		if (section.HasMember("definition") && section["definition"].IsArray())
			migrateDefinitionArray(section["definition"], typeKey, "default", "normal");
		if (section.HasMember("definition_detailed") && section["definition_detailed"].IsArray())
			migrateDefinitionArray(section["definition_detailed"], typeKey, "default", "detailed");

		if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
			return;

		Value& statusDefinitions = section["status_definitions"];
		for (auto statusIt = statusDefinitions.MemberBegin(); statusIt != statusDefinitions.MemberEnd(); ++statusIt)
		{
			if (!statusIt->name.IsString() || !statusIt->value.IsObject())
				continue;
			const std::string statusKey = statusIt->name.GetString();
			Value& statusSection = statusIt->value;
			if (statusSection.HasMember("definition") && statusSection["definition"].IsArray())
				migrateDefinitionArray(statusSection["definition"], typeKey, statusKey, "normal");
			if (statusSection.HasMember("definition_detailed") && statusSection["definition_detailed"].IsArray())
				migrateDefinitionArray(statusSection["definition_detailed"], typeKey, statusKey, "detailed");
		}
	};

	migrateTypeDefinitions("departure");
	migrateTypeDefinitions("arrival");
	migrateTypeDefinitions("airborne");
	migrateTypeDefinitions("uncorrelated");

	// A validated backup or a migrated read-only source may be active in memory.
	// Normalize that working copy for runtime use, but leave recovery to the
	// explicit Settings flow instead of showing a spurious startup save error.
	if (changed && persistChanges && CurrentConfig->isConfigHealthy() && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save status settings to vSMR_Profiles.json", true, true, false, false, false);
	}
}
