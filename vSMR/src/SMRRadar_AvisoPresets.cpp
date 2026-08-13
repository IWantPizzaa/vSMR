#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"
#include "VsmrControlCenterDialog.hpp"

#include <cctype>

extern std::vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	const char* kAvisoPresetsKey = "aviso_presets";
	const char* kAirportPresetStoresKey = "airports";
	const char* kPresetItemsKey = "items";
	const char* kDefaultPresetKey = "default";
	const rapidjson::SizeType kInvalidPresetIndex = static_cast<rapidjson::SizeType>(-1);

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

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	std::string NormalizeAirportKey(std::string value)
	{
		value = TrimAsciiWhitespaceCopy(value);
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	void CloneJsonValue(
		const rapidjson::Value& source,
		rapidjson::Value& output,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (source.IsObject())
		{
			output.SetObject();
			for (rapidjson::Value::ConstMemberIterator it = source.MemberBegin();
				it != source.MemberEnd(); ++it)
			{
				rapidjson::Value key(
					it->name.GetString(),
					static_cast<rapidjson::SizeType>(it->name.GetStringLength()),
					allocator);
				rapidjson::Value value;
				CloneJsonValue(it->value, value, allocator);
				output.AddMember(key, value, allocator);
			}
			return;
		}
		if (source.IsArray())
		{
			output.SetArray();
			for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
			{
				rapidjson::Value value;
				CloneJsonValue(source[i], value, allocator);
				output.PushBack(value, allocator);
			}
			return;
		}
		if (source.IsString())
		{
			output.SetString(
				source.GetString(),
				static_cast<rapidjson::SizeType>(source.GetStringLength()),
				allocator);
			return;
		}
		if (source.IsBool()) { output.SetBool(source.GetBool()); return; }
		if (source.IsInt()) { output.SetInt(source.GetInt()); return; }
		if (source.IsUint()) { output.SetUint(source.GetUint()); return; }
		if (source.IsInt64()) { output.SetInt64(source.GetInt64()); return; }
		if (source.IsUint64()) { output.SetUint64(source.GetUint64()); return; }
		if (source.IsDouble()) { output.SetDouble(source.GetDouble()); return; }
		output.SetNull();
	}

	const rapidjson::Value* GetObjectMember(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsObject())
			return nullptr;
		return &object[key];
	}

	const rapidjson::Value* GetArrayMember(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsArray())
			return nullptr;
		return &object[key];
	}

	rapidjson::Value& EnsureObjectMember(rapidjson::Value& object, const char* key, rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key) && object[key].IsObject())
			return object[key];

		if (object.HasMember(key))
			object.RemoveMember(key);

		rapidjson::Value keyValue(key, allocator);
		rapidjson::Value objectValue(rapidjson::kObjectType);
		object.AddMember(keyValue, objectValue, allocator);
		return object[key];
	}

	rapidjson::Value& EnsureArrayMember(rapidjson::Value& object, const char* key, rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key) && object[key].IsArray())
			return object[key];

		if (object.HasMember(key))
			object.RemoveMember(key);

		rapidjson::Value keyValue(key, allocator);
		rapidjson::Value arrayValue(rapidjson::kArrayType);
		object.AddMember(keyValue, arrayValue, allocator);
		return object[key];
	}

	void AddStringMember(rapidjson::Value& object, const char* key, const std::string& value, rapidjson::Document::AllocatorType& allocator)
	{
		rapidjson::Value keyValue(key, allocator);
		rapidjson::Value stringValue;
		stringValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
		object.AddMember(keyValue, stringValue, allocator);
	}

	void AddDoubleMember(rapidjson::Value& object, const char* key, double value, rapidjson::Document::AllocatorType& allocator)
	{
		object.AddMember(key, value, allocator);
	}

	void AddIntMember(rapidjson::Value& object, const char* key, int value, rapidjson::Document::AllocatorType& allocator)
	{
		object.AddMember(key, value, allocator);
	}

	void AddBoolMember(rapidjson::Value& object, const char* key, bool value, rapidjson::Document::AllocatorType& allocator)
	{
		object.AddMember(key, value, allocator);
	}

	void SetStringMember(rapidjson::Value& object, const char* key, const std::string& value, rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key))
			object.RemoveMember(key);
		AddStringMember(object, key, value, allocator);
	}

	void SetBoolMember(rapidjson::Value& object, const char* key, bool value, rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key))
			object.RemoveMember(key);
		AddBoolMember(object, key, value, allocator);
	}

	bool ReadDoubleMember(const rapidjson::Value& object, const char* key, double& out)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsNumber())
			return false;

		const double value = object[key].GetDouble();
		if (!std::isfinite(value))
			return false;

		out = value;
		return true;
	}

	int ReadIntMember(const rapidjson::Value& object, const char* key, int fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key))
			return fallback;
		if (object[key].IsInt())
			return object[key].GetInt();
		if (object[key].IsNumber())
			return static_cast<int>(std::lround(object[key].GetDouble()));
		return fallback;
	}

	bool ReadValidWindowRect(const rapidjson::Value& object, RECT& out)
	{
		if (!object.IsObject())
			return false;
		int values[4] = {};
		const char* keys[4] = { "left", "top", "right", "bottom" };
		for (int index = 0; index < 4; ++index)
		{
			if (!object.HasMember(keys[index]) || !object[keys[index]].IsInt())
				return false;
			values[index] = object[keys[index]].GetInt();
			if (values[index] < -100000 || values[index] > 100000)
				return false;
		}
		if (values[2] <= values[0] || values[3] <= values[1] ||
			values[2] - values[0] > 20000 || values[3] - values[1] > 20000)
			return false;
		out.left = values[0];
		out.top = values[1];
		out.right = values[2];
		out.bottom = values[3];
		return true;
	}

	bool ReadBoolMember(const rapidjson::Value& object, const char* key, bool fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsBool())
			return fallback;
		return object[key].GetBool();
	}

	std::string ReadStringMember(const rapidjson::Value& object, const char* key, const std::string& fallback = "")
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsString())
			return fallback;
		return object[key].GetString();
	}

	std::string LayoutModeToString(int mode)
	{
		switch (mode)
		{
		case 1: return "SplitLeft";
		case 2: return "SplitRight";
		case 3: return "CornerTopLeft";
		case 4: return "CornerTopRight";
		case 5: return "CornerBottomLeft";
		case 6: return "CornerBottomRight";
		case 7: return "SplitTop";
		case 8: return "SplitBottom";
		default: return "Floating";
		}
	}

	int LayoutModeFromValue(const rapidjson::Value& object, int fallback)
	{
		if (object.IsInt())
			return std::clamp(object.GetInt(), 0, 8);

		if (!object.IsString())
			return std::clamp(fallback, 0, 8);

		const std::string value = ToLowerAscii(TrimAsciiWhitespaceCopy(object.GetString()));
		if (value == "splitleft" || value == "split_left")
			return 1;
		if (value == "splitright" || value == "split_right")
			return 2;
		if (value == "cornertopleft" || value == "corner_top_left")
			return 3;
		if (value == "cornertopright" || value == "corner_top_right")
			return 4;
		if (value == "cornerbottomleft" || value == "corner_bottom_left")
			return 5;
		if (value == "cornerbottomright" || value == "corner_bottom_right")
			return 6;
		if (value == "splittop" || value == "split_top")
			return 7;
		if (value == "splitbottom" || value == "split_bottom")
			return 8;
		return 0;
	}

	const rapidjson::Value* GetAirportPresetSection(const rapidjson::Value& container, const std::string& airport)
	{
		const rapidjson::Value* section = GetObjectMember(container, kAvisoPresetsKey);
		const rapidjson::Value* airports = section != nullptr ? GetObjectMember(*section, kAirportPresetStoresKey) : nullptr;
		const std::string airportKey = NormalizeAirportKey(airport);
		if (airports == nullptr || airportKey.empty() || !airports->HasMember(airportKey.c_str()) ||
			!(*airports)[airportKey.c_str()].IsObject())
		{
			return nullptr;
		}
		return &(*airports)[airportKey.c_str()];
	}

	rapidjson::Value& EnsureAirportPresetSection(
		rapidjson::Value& container,
		const std::string& airport,
		rapidjson::Document::AllocatorType& allocator)
	{
		rapidjson::Value& section = EnsureObjectMember(container, kAvisoPresetsKey, allocator);
		rapidjson::Value& airports = EnsureObjectMember(section, kAirportPresetStoresKey, allocator);
		const std::string airportKey = NormalizeAirportKey(airport);
		return EnsureObjectMember(airports, airportKey.c_str(), allocator);
	}

	const rapidjson::Value* GetPresetItems(const rapidjson::Value& container, const std::string& airport)
	{
		const rapidjson::Value* section = GetAirportPresetSection(container, airport);
		return section != nullptr ? GetArrayMember(*section, kPresetItemsKey) : nullptr;
	}

	rapidjson::Value& EnsurePresetItems(
		rapidjson::Value& container,
		const std::string& airport,
		rapidjson::Document::AllocatorType& allocator)
	{
		rapidjson::Value& section = EnsureAirportPresetSection(container, airport, allocator);
		return EnsureArrayMember(section, kPresetItemsKey, allocator);
	}

	bool MigrateLegacyPresetStore(
		rapidjson::Value& container,
		const std::string& airport,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!container.IsObject() || !container.HasMember(kAvisoPresetsKey) ||
			!container[kAvisoPresetsKey].IsObject() || NormalizeAirportKey(airport).empty())
		{
			return false;
		}

		rapidjson::Value& legacySection = container[kAvisoPresetsKey];
		const bool hasItemsMember = legacySection.HasMember(kPresetItemsKey);
		const bool hasDefaultMember = legacySection.HasMember(kDefaultPresetKey);
		if ((!hasItemsMember && !hasDefaultMember) ||
			(hasItemsMember && !legacySection[kPresetItemsKey].IsArray()) ||
			(hasDefaultMember && !legacySection[kDefaultPresetKey].IsString()))
		{
			return false;
		}
		// Never assign an unscoped legacy store to whichever airport happens to
		// open first. A migration is automatic only when the legacy store itself
		// explicitly identifies the same airport.
		if (!legacySection.HasMember("airport") || !legacySection["airport"].IsString() ||
			NormalizeAirportKey(legacySection["airport"].GetString()) != NormalizeAirportKey(airport))
		{
			return false;
		}

		// The legacy root has an explicit owner, so merge it into that airport even
		// when canonical stores for other airports already exist. Canonical entries
		// win name collisions; otherwise every legacy item is retained. This avoids
		// making an explicitly owned store disappear merely because another airport
		// was opened first.
		rapidjson::Value& airports =
			EnsureObjectMember(legacySection, kAirportPresetStoresKey, allocator);
		const std::string airportKey = NormalizeAirportKey(airport);
		rapidjson::Value& airportSection =
			EnsureObjectMember(airports, airportKey.c_str(), allocator);

		if (hasItemsMember)
		{
			const rapidjson::Value& legacyItems = legacySection[kPresetItemsKey];
			rapidjson::Value& targetItems =
				EnsureArrayMember(airportSection, kPresetItemsKey, allocator);
			for (rapidjson::SizeType i = 0; i < legacyItems.Size(); ++i)
			{
				const rapidjson::Value& legacyItem = legacyItems[i];
				bool duplicateName = false;
				if (legacyItem.IsObject() && legacyItem.HasMember("name") &&
					legacyItem["name"].IsString())
				{
					for (rapidjson::SizeType targetIndex = 0;
						targetIndex < targetItems.Size(); ++targetIndex)
					{
						const rapidjson::Value& targetItem = targetItems[targetIndex];
						if (targetItem.IsObject() && targetItem.HasMember("name") &&
							targetItem["name"].IsString() &&
							EqualsNoCase(
								targetItem["name"].GetString(),
								legacyItem["name"].GetString()))
						{
							duplicateName = true;
							break;
						}
					}
				}
				if (!duplicateName)
				{
					rapidjson::Value itemCopy;
					CloneJsonValue(legacyItem, itemCopy, allocator);
					targetItems.PushBack(itemCopy, allocator);
				}
			}
		}
		if (hasDefaultMember && !airportSection.HasMember(kDefaultPresetKey))
		{
			rapidjson::Value defaultValue;
			CloneJsonValue(
				legacySection[kDefaultPresetKey],
				defaultValue,
				allocator);
			rapidjson::Value defaultKey(kDefaultPresetKey, allocator);
			airportSection.AddMember(defaultKey, defaultValue, allocator);
		}

		if (hasItemsMember)
			legacySection.RemoveMember(kPresetItemsKey);
		if (hasDefaultMember)
			legacySection.RemoveMember(kDefaultPresetKey);
		legacySection.RemoveMember("airport");
		return true;
	}

	rapidjson::SizeType FindPresetIndexNoCase(const rapidjson::Value& items, const std::string& name)
	{
		if (!items.IsArray())
			return kInvalidPresetIndex;

		for (rapidjson::SizeType i = 0; i < items.Size(); ++i)
		{
			if (!items[i].IsObject() || !items[i].HasMember("name") || !items[i]["name"].IsString())
				continue;

			if (EqualsNoCase(items[i]["name"].GetString(), name))
				return i;
		}

		return kInvalidPresetIndex;
	}

	std::string MakeUniquePresetName(const rapidjson::Value& items, const std::string& requestedName)
	{
		std::string baseName = TrimAsciiWhitespaceCopy(requestedName);
		if (baseName.empty())
			baseName = "Inset Preset";

		if (FindPresetIndexNoCase(items, baseName) == kInvalidPresetIndex)
			return baseName;

		for (int i = 2; i < 1000; ++i)
		{
			const std::string candidate = baseName + " (" + std::to_string(i) + ")";
			if (FindPresetIndexNoCase(items, candidate) == kInvalidPresetIndex)
				return candidate;
		}

		return baseName + " Copy";
	}

	void RemoveArrayElement(rapidjson::Value& array, rapidjson::SizeType index)
	{
		if (!array.IsArray() || index >= array.Size())
			return;

		for (rapidjson::SizeType i = index; i + 1 < array.Size(); ++i)
			array[i] = array[i + 1];
		array.PopBack();
	}

	bool ParseMainViewPreset(const rapidjson::Value& value, CSMRRadar::AvisoMainViewPreset& out)
	{
		double minLat = 0.0;
		double minLon = 0.0;
		double maxLat = 0.0;
		double maxLon = 0.0;
		if (!ReadDoubleMember(value, "min_latitude", minLat) ||
			!ReadDoubleMember(value, "min_longitude", minLon) ||
			!ReadDoubleMember(value, "max_latitude", maxLat) ||
			!ReadDoubleMember(value, "max_longitude", maxLon))
		{
			return false;
		}

		if (minLat < -90.0 || maxLat > 90.0 ||
			minLon < -180.0 || maxLon > 180.0 ||
			minLat >= maxLat || minLon >= maxLon)
			return false;

		out.valid = true;
		out.minLatitude = minLat;
		out.minLongitude = minLon;
		out.maxLatitude = maxLat;
		out.maxLongitude = maxLon;
		out.zoomLevel = ReadIntMember(value, "zoom_level", 0);
		return true;
	}

	bool ParseAvisoPreset(const rapidjson::Value& value, CSMRRadar::AvisoPreset& out)
	{
		if (!value.IsObject() || !value.HasMember("name") || !value["name"].IsString())
			return false;

		const std::string name = TrimAsciiWhitespaceCopy(value["name"].GetString());
		if (name.empty())
			return false;

		out = CSMRRadar::AvisoPreset();
		out.name = name;
		out.linkedMovement = ReadBoolMember(value, "linked_movement", false);

		if (const rapidjson::Value* main = GetObjectMember(value, "main"))
			ParseMainViewPreset(*main, out.mainView);

		if (const rapidjson::Value* secondary = GetObjectMember(value, "secondary"))
		{
			if (!ReadValidWindowRect(*secondary, out.secondaryArea))
				return false;
			out.secondaryVisible = ReadBoolMember(*secondary, "visible", true);
			out.secondaryScale = std::clamp(ReadIntMember(*secondary, "scale", out.secondaryScale), 1, 2400);
			ReadDoubleMember(*secondary, "center_latitude", out.secondaryCenterLatitude);
			ReadDoubleMember(*secondary, "center_longitude", out.secondaryCenterLongitude);
			if (secondary->HasMember("layout_mode"))
				out.secondaryLayoutMode = LayoutModeFromValue((*secondary)["layout_mode"], out.secondaryLayoutMode);
			else if (secondary->HasMember("layout_mode_id"))
				out.secondaryLayoutMode = LayoutModeFromValue((*secondary)["layout_mode_id"], out.secondaryLayoutMode);
		}

		if (const rapidjson::Value* srw = GetArrayMember(value, "srw"))
		{
			for (rapidjson::SizeType index = 0; index < srw->Size(); ++index)
			{
				const rapidjson::Value& item = (*srw)[index];
				const int id = ReadIntMember(item, "id", static_cast<int>(index) + 1);
				if (!item.IsObject() || id != 1)
					continue;

				CSMRRadar::AvisoPreset::SecondaryRadarWindow& window =
					out.srw[static_cast<size_t>(id - 1)];
				if (!ReadValidWindowRect(item, window.area))
					return false;
				window.valid = true;
				window.visible = ReadBoolMember(item, "visible", window.visible);
				window.offset.x = ReadIntMember(item, "offset_x", window.offset.x);
				window.offset.y = ReadIntMember(item, "offset_y", window.offset.y);
				window.scale = std::clamp(
					ReadIntMember(item, "scale", window.scale),
					1,
					2400);
				window.filter = std::clamp(
					ReadIntMember(item, "filter", window.filter),
					0,
					66000);
				ReadDoubleMember(item, "rotation", window.rotation);
				if (item.HasMember("layout_mode"))
					window.layoutMode = LayoutModeFromValue(item["layout_mode"], window.layoutMode);
				else if (item.HasMember("layout_mode_id"))
					window.layoutMode = LayoutModeFromValue(item["layout_mode_id"], window.layoutMode);
			}
		}

		if (const rapidjson::Value* weather = GetObjectMember(value, "weather"))
		{
			if (!ReadValidWindowRect(*weather, out.weather.area))
				return false;
			out.weather.valid = true;
			out.weather.visible = ReadBoolMember(*weather, "visible", out.weather.visible);
			if (weather->HasMember("layout_mode"))
				out.weather.layoutMode = LayoutModeFromValue((*weather)["layout_mode"], out.weather.layoutMode);
			else if (weather->HasMember("layout_mode_id"))
				out.weather.layoutMode = LayoutModeFromValue((*weather)["layout_mode_id"], out.weather.layoutMode);
		}

		if (const rapidjson::Value* timer = GetObjectMember(value, "timer"))
		{
			out.timer.valid = true;
			out.timer.area = { 100, 180, 184, 236 };
			if (!ReadValidWindowRect(*timer, out.timer.area))
				return false;
			out.timer.visible = ReadBoolMember(*timer, "visible", out.timer.visible);
			if (timer->HasMember("layout_mode"))
				out.timer.layoutMode = LayoutModeFromValue((*timer)["layout_mode"], out.timer.layoutMode);
			else if (timer->HasMember("layout_mode_id"))
				out.timer.layoutMode = LayoutModeFromValue((*timer)["layout_mode_id"], out.timer.layoutMode);
		}

		return true;
	}

	void WriteAvisoPreset(const CSMRRadar::AvisoPreset& preset, rapidjson::Value& out, rapidjson::Document::AllocatorType& allocator)
	{
		out.SetObject();
		AddStringMember(out, "name", preset.name, allocator);
		AddBoolMember(out, "linked_movement", preset.linkedMovement, allocator);

		rapidjson::Value main(rapidjson::kObjectType);
		AddDoubleMember(main, "min_latitude", preset.mainView.minLatitude, allocator);
		AddDoubleMember(main, "min_longitude", preset.mainView.minLongitude, allocator);
		AddDoubleMember(main, "max_latitude", preset.mainView.maxLatitude, allocator);
		AddDoubleMember(main, "max_longitude", preset.mainView.maxLongitude, allocator);
		AddIntMember(main, "zoom_level", preset.mainView.zoomLevel, allocator);
		rapidjson::Value mainKey("main", allocator);
		out.AddMember(mainKey, main, allocator);

		rapidjson::Value secondary(rapidjson::kObjectType);
		AddBoolMember(secondary, "visible", preset.secondaryVisible, allocator);
		AddIntMember(secondary, "left", preset.secondaryArea.left, allocator);
		AddIntMember(secondary, "top", preset.secondaryArea.top, allocator);
		AddIntMember(secondary, "right", preset.secondaryArea.right, allocator);
		AddIntMember(secondary, "bottom", preset.secondaryArea.bottom, allocator);
		AddDoubleMember(secondary, "center_latitude", preset.secondaryCenterLatitude, allocator);
		AddDoubleMember(secondary, "center_longitude", preset.secondaryCenterLongitude, allocator);
		AddIntMember(secondary, "scale", std::clamp(preset.secondaryScale, 1, 2400), allocator);
		AddStringMember(secondary, "layout_mode", LayoutModeToString(preset.secondaryLayoutMode), allocator);
		AddIntMember(secondary, "layout_mode_id", std::clamp(preset.secondaryLayoutMode, 0, 8), allocator);
		rapidjson::Value secondaryKey("secondary", allocator);
		out.AddMember(secondaryKey, secondary, allocator);

		rapidjson::Value srw(rapidjson::kArrayType);
		for (size_t index = 0; index < preset.srw.size(); ++index)
		{
			const CSMRRadar::AvisoPreset::SecondaryRadarWindow& window =
				preset.srw[index];
			if (!window.valid)
				continue;

			rapidjson::Value item(rapidjson::kObjectType);
			AddIntMember(item, "id", static_cast<int>(index) + 1, allocator);
			AddBoolMember(item, "visible", window.visible, allocator);
			AddIntMember(item, "left", window.area.left, allocator);
			AddIntMember(item, "top", window.area.top, allocator);
			AddIntMember(item, "right", window.area.right, allocator);
			AddIntMember(item, "bottom", window.area.bottom, allocator);
			AddIntMember(item, "offset_x", window.offset.x, allocator);
			AddIntMember(item, "offset_y", window.offset.y, allocator);
			AddIntMember(
				item,
				"scale",
				std::clamp(window.scale, 1, 2400),
				allocator);
			AddIntMember(
				item,
				"filter",
				std::clamp(window.filter, 0, 66000),
				allocator);
			AddDoubleMember(item, "rotation", window.rotation, allocator);
			AddStringMember(item, "layout_mode", LayoutModeToString(window.layoutMode), allocator);
			AddIntMember(item, "layout_mode_id", std::clamp(window.layoutMode, 0, 8), allocator);
			srw.PushBack(item, allocator);
		}
		rapidjson::Value srwKey("srw", allocator);
		out.AddMember(srwKey, srw, allocator);

		if (preset.weather.valid)
		{
			rapidjson::Value weather(rapidjson::kObjectType);
			AddBoolMember(weather, "visible", preset.weather.visible, allocator);
			AddIntMember(weather, "left", preset.weather.area.left, allocator);
			AddIntMember(weather, "top", preset.weather.area.top, allocator);
			AddIntMember(weather, "right", preset.weather.area.right, allocator);
			AddIntMember(weather, "bottom", preset.weather.area.bottom, allocator);
			AddStringMember(weather, "layout_mode", LayoutModeToString(preset.weather.layoutMode), allocator);
			AddIntMember(weather, "layout_mode_id", std::clamp(preset.weather.layoutMode, 0, 8), allocator);
			rapidjson::Value weatherKey("weather", allocator);
			out.AddMember(weatherKey, weather, allocator);
		}

		if (preset.timer.valid)
		{
			rapidjson::Value timer(rapidjson::kObjectType);
			AddBoolMember(timer, "visible", preset.timer.visible, allocator);
			AddIntMember(timer, "left", preset.timer.area.left, allocator);
			AddIntMember(timer, "top", preset.timer.area.top, allocator);
			AddIntMember(timer, "right", preset.timer.area.right, allocator);
			AddIntMember(timer, "bottom", preset.timer.area.bottom, allocator);
			AddStringMember(timer, "layout_mode", LayoutModeToString(preset.timer.layoutMode), allocator);
			AddIntMember(timer, "layout_mode_id", std::clamp(preset.timer.layoutMode, 0, 8), allocator);
			rapidjson::Value timerKey("timer", allocator);
			out.AddMember(timerKey, timer, allocator);
		}
	}

	CInsetWindow* GetSecondaryAvisoWindow(CSMRRadar* radar)
	{
		if (radar == nullptr)
			return nullptr;

		const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
		auto it = radar->appWindows.find(avisoWindowId);
		if (it == radar->appWindows.end() || it->second == nullptr || !it->second->IsAvisoViewport())
			return nullptr;
		return it->second.get();
	}

	const CInsetWindow* GetSecondaryAvisoWindow(const CSMRRadar* radar)
	{
		return GetSecondaryAvisoWindow(const_cast<CSMRRadar*>(radar));
	}

	void ReconcileLiveRadarPresetContexts(
		CSMRRadar* source,
		const std::string& airport,
		const std::string& renamedFrom = "",
		const std::string& renamedTo = "")
	{
		if (source == nullptr || source->CurrentConfig == nullptr)
			return;

		auto reconcile = [&](CSMRRadar* radar)
		{
			if (radar == nullptr || radar->IsShutdownRequested() ||
				radar->CurrentConfig == nullptr ||
				!radar->CurrentConfig->sharesConfigFileWith(*source->CurrentConfig) ||
				!EqualsNoCase(NormalizeAirportKey(radar->getActiveAirport()), airport))
			{
				return;
			}

			if (!renamedFrom.empty() && !renamedTo.empty() &&
				EqualsNoCase(radar->ActiveAvisoPresetName, renamedFrom))
			{
				radar->ActiveAvisoPresetName = renamedTo;
			}

			if (!radar->ActiveAvisoPresetName.empty())
			{
				const std::vector<CSMRRadar::AvisoPreset> presets = radar->GetAvisoPresets();
				const auto active = std::find_if(
					presets.begin(),
					presets.end(),
					[&](const CSMRRadar::AvisoPreset& preset) {
						return EqualsNoCase(preset.name, radar->ActiveAvisoPresetName);
					});
				if (active == presets.end())
				{
					radar->ActiveAvisoPresetName.clear();
					radar->AvisoViewsLinked = false;
				}
				else
				{
					radar->ActiveAvisoPresetName = active->name;
					radar->AvisoViewsLinked = active->linkedMovement;
					if (radar->AvisoViewsLinked)
						radar->SyncLinkedAvisoSecondaryToMainView();
				}
			}

			radar->SaveInsetStateToAsrForAirport(radar->getActiveAirport());
			radar->RequestRefresh();
			if (radar != source && radar->VsmrControlCenterDialog != nullptr)
				radar->VsmrControlCenterDialog->SyncFromRadar("preset");
		};

		bool sourceSeen = false;
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			sourceSeen = sourceSeen || radar == source;
			reconcile(radar);
		}
		if (!sourceSeen)
			reconcile(source);
	}
}

std::vector<CSMRRadar::AvisoPreset> CSMRRadar::GetAvisoPresets() const
{
	std::vector<AvisoPreset> presets;
	if (CurrentConfig == nullptr)
		return presets;

	const rapidjson::Value* sharedMetadata =
		CurrentConfig->getSharedAvisoPresetContainer();
	const rapidjson::Value* items = sharedMetadata != nullptr
		? GetPresetItems(*sharedMetadata, getActiveAirport())
		: nullptr;
	if (items == nullptr)
		return presets;

	for (rapidjson::SizeType i = 0; i < items->Size(); ++i)
	{
		AvisoPreset preset;
		if (ParseAvisoPreset((*items)[i], preset))
			presets.push_back(preset);
	}

	return presets;
}

std::string CSMRRadar::GetDefaultAvisoPresetName() const
{
	if (CurrentConfig == nullptr)
		return "";

	const rapidjson::Value* sharedMetadata =
		CurrentConfig->getSharedAvisoPresetContainer();
	const rapidjson::Value* section = sharedMetadata != nullptr
		? GetAirportPresetSection(*sharedMetadata, getActiveAirport())
		: nullptr;
	if (section == nullptr)
		return "";

	return TrimAsciiWhitespaceCopy(ReadStringMember(*section, kDefaultPresetKey));
}

std::string CSMRRadar::GetActiveAvisoPresetName() const
{
	return ActiveAvisoPresetName;
}

bool CSMRRadar::SaveAvisoPreset(
	const std::string& requestedName,
	bool overwriteExisting,
	std::string* outSavedName,
	std::optional<bool> linkedMovementOverride)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	std::string presetName = TrimAsciiWhitespaceCopy(requestedName);
	if (presetName.empty())
		presetName = "Inset Preset";

	AvisoPreset preset;
	preset.name = presetName;
	preset.linkedMovement = linkedMovementOverride.value_or(AvisoViewsLinked);

	CPosition displayA;
	CPosition displayB;
	GetDisplayArea(&displayA, &displayB);
	preset.mainView.valid = true;
	preset.mainView.minLatitude = (std::min)(displayA.m_Latitude, displayB.m_Latitude);
	preset.mainView.maxLatitude = (std::max)(displayA.m_Latitude, displayB.m_Latitude);
	preset.mainView.minLongitude = (std::min)(displayA.m_Longitude, displayB.m_Longitude);
	preset.mainView.maxLongitude = (std::max)(displayA.m_Longitude, displayB.m_Longitude);
	preset.mainView.zoomLevel = RadarViewZoomLevel;

	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const CInsetWindow* avisoWindow = GetSecondaryAvisoWindow(this);
	if (avisoWindow == nullptr)
		return false;

	preset.secondaryArea = avisoWindow->m_Area;
	preset.secondaryScale = std::clamp(avisoWindow->m_AvisoScale, 1, 2400);
	preset.secondaryCenterLatitude = avisoWindow->m_AvisoCenterLatitude;
	preset.secondaryCenterLongitude = avisoWindow->m_AvisoCenterLongitude;
	preset.secondaryLayoutMode = std::clamp(static_cast<int>(avisoWindow->m_AvisoLayoutMode), 0, 8);
	const auto displayIt = appWindowDisplays.find(avisoWindowId);
	preset.secondaryVisible = displayIt != appWindowDisplays.end() && displayIt->second;
	for (const int id : { 1 })
	{
		const auto windowIt = appWindows.find(id);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		AvisoPreset::SecondaryRadarWindow& window =
			preset.srw[static_cast<size_t>(id - 1)];
		window.valid = true;
		window.area = windowIt->second->m_Area;
		window.offset = windowIt->second->m_Offset;
		window.scale = windowIt->second->m_Scale;
		window.filter = windowIt->second->m_Filter;
		window.rotation = windowIt->second->m_Rotation;
		window.layoutMode = std::clamp(static_cast<int>(windowIt->second->m_AvisoLayoutMode), 0, 8);
		const auto visibleIt = appWindowDisplays.find(id);
		window.visible =
			visibleIt != appWindowDisplays.end() && visibleIt->second;
	}
	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const auto weatherWindowIt = appWindows.find(weatherWindowId);
	if (weatherWindowIt != appWindows.end() && weatherWindowIt->second != nullptr)
	{
		preset.weather.valid = true;
		preset.weather.area = weatherWindowIt->second->m_Area;
		preset.weather.layoutMode = std::clamp(
			static_cast<int>(weatherWindowIt->second->m_AvisoLayoutMode),
			0,
			8);
		const auto visibleIt = appWindowDisplays.find(weatherWindowId);
		preset.weather.visible = visibleIt != appWindowDisplays.end() && visibleIt->second;
	}
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	const auto timerWindowIt = appWindows.find(timerWindowId);
	if (timerWindowIt != appWindows.end() && timerWindowIt->second != nullptr)
	{
		preset.timer.valid = true;
		preset.timer.area = timerWindowIt->second->m_Area;
		preset.timer.layoutMode = std::clamp(
			static_cast<int>(timerWindowIt->second->m_AvisoLayoutMode),
			0,
			8);
		const auto visibleIt = appWindowDisplays.find(timerWindowId);
		preset.timer.visible = visibleIt != appWindowDisplays.end() && visibleIt->second;
	}

	const bool saved = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsurePresetItems(sharedMetadata, airport, allocator);
			const rapidjson::SizeType existingIndex = FindPresetIndexNoCase(items, presetName);

			// overwriteExisting is the update path. If another screen deleted this
			// preset after it was displayed, abort instead of recreating stale state.
			if ((overwriteExisting && existingIndex == kInvalidPresetIndex) ||
				(!overwriteExisting && existingIndex != kInvalidPresetIndex))
			{
				return CConfig::AvisoPresetTransactionAction::Abort;
			}

			rapidjson::Value presetValue;
			WriteAvisoPreset(preset, presetValue, allocator);
			if (existingIndex != kInvalidPresetIndex)
				items[existingIndex] = presetValue;
			else
				items.PushBack(presetValue, allocator);
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (!saved)
		return false;

	ActiveAvisoPresetName = presetName;
	AvisoViewsLinked = preset.linkedMovement;
	ReconcileLiveRadarPresetContexts(this, airport);
	if (outSavedName != nullptr)
		*outSavedName = presetName;
	return true;
}

bool CSMRRadar::LoadAvisoPreset(const std::string& name)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string requestedName = TrimAsciiWhitespaceCopy(name);
	if (requestedName.empty())
		return false;

	const rapidjson::Value* sharedMetadata =
		CurrentConfig->getSharedAvisoPresetContainer();
	const rapidjson::Value* items = sharedMetadata != nullptr
		? GetPresetItems(*sharedMetadata, getActiveAirport())
		: nullptr;
	if (items == nullptr)
		return false;

	const rapidjson::SizeType index = FindPresetIndexNoCase(*items, requestedName);
	if (index == kInvalidPresetIndex)
		return false;

	AvisoPreset preset;
	if (!ParseAvisoPreset((*items)[index], preset))
		return false;
	CancelInsetWindowInteractions();

	if (preset.mainView.valid)
	{
		CPosition downLeft;
		downLeft.m_Latitude = preset.mainView.minLatitude;
		downLeft.m_Longitude = preset.mainView.minLongitude;

		CPosition upRight;
		upRight.m_Latitude = preset.mainView.maxLatitude;
		upRight.m_Longitude = preset.mainView.maxLongitude;

		SetDisplayArea(downLeft, upRight);
		RadarViewZoomLevel = -1;
		ClearAvisoGeoJsonRasterCache();
		AvisoGeoJsonLastViewValid = false;
	}

	CInsetWindow* avisoWindow = GetSecondaryAvisoWindow(this);
	if (avisoWindow != nullptr)
	{
		avisoWindow->m_Area = preset.secondaryArea;
		avisoWindow->m_AvisoScale = std::clamp(preset.secondaryScale, 1, 2400);
		avisoWindow->m_AvisoCenterLatitude = std::clamp(preset.secondaryCenterLatitude, -85.0, 85.0);
		avisoWindow->m_AvisoCenterLongitude = preset.secondaryCenterLongitude;
		avisoWindow->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(std::clamp(preset.secondaryLayoutMode, 0, 8));
		avisoWindow->m_AvisoViewInitialized = true;
		avisoWindow->ResetAvisoInteractionState();
		avisoWindow->ClearAvisoViewportCache();
	}

	appWindowDisplays[APPWINDOW_AVISO - APPWINDOW_BASE] = preset.secondaryVisible;
	for (const int id : { 1 })
	{
		const AvisoPreset::SecondaryRadarWindow& window =
			preset.srw[static_cast<size_t>(id - 1)];
		if (!window.valid)
			continue;

		const auto windowIt = appWindows.find(id);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;
		windowIt->second->m_Area = window.area;
		windowIt->second->m_Offset = window.offset;
		windowIt->second->m_Scale = std::clamp(window.scale, 1, 2400);
		windowIt->second->m_Filter = std::clamp(window.filter, 0, 66000);
		windowIt->second->m_Rotation = window.rotation;
		windowIt->second->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(
			std::clamp(window.layoutMode, 0, 8));
		windowIt->second->ResetAvisoInteractionState();
		appWindowDisplays[id] = window.visible;
	}
	if (preset.weather.valid)
	{
		const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
		const auto weatherWindowIt = appWindows.find(weatherWindowId);
		if (weatherWindowIt != appWindows.end() && weatherWindowIt->second != nullptr)
		{
			weatherWindowIt->second->m_Area = preset.weather.area;
			weatherWindowIt->second->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(
				std::clamp(preset.weather.layoutMode, 0, 8));
			weatherWindowIt->second->ResetAvisoInteractionState();
			appWindowDisplays[weatherWindowId] = preset.weather.visible;
		}
	}
	if (preset.timer.valid)
	{
		const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
		const auto timerWindowIt = appWindows.find(timerWindowId);
		if (timerWindowIt != appWindows.end() && timerWindowIt->second != nullptr)
		{
			timerWindowIt->second->m_Area = preset.timer.area;
			timerWindowIt->second->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(
				std::clamp(preset.timer.layoutMode, 0, 8));
			timerWindowIt->second->ResetAvisoInteractionState();
			appWindowDisplays[timerWindowId] = preset.timer.visible;
		}
	}
	AvisoViewsLinked = preset.linkedMovement;
	ActiveAvisoPresetName = preset.name;
	if (AvisoViewsLinked)
		SyncLinkedAvisoSecondaryToMainView();

	SaveInsetStateToAsrForAirport(getActiveAirport());
	RequestRefresh();
	return true;
}

bool CSMRRadar::RenameAvisoPreset(
	const std::string& oldName,
	const std::string& newName,
	std::optional<bool> linkedMovementOverride)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedOldName = TrimAsciiWhitespaceCopy(oldName);
	const std::string trimmedNewName = TrimAsciiWhitespaceCopy(newName);
	if (trimmedOldName.empty() || trimmedNewName.empty())
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	std::string oldCanonicalName;
	const bool renamed = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsurePresetItems(sharedMetadata, airport, allocator);
			const rapidjson::SizeType oldIndex = FindPresetIndexNoCase(items, trimmedOldName);
			if (oldIndex == kInvalidPresetIndex)
				return CConfig::AvisoPresetTransactionAction::Abort;

			const rapidjson::SizeType newIndex = FindPresetIndexNoCase(items, trimmedNewName);
			if (newIndex != kInvalidPresetIndex && newIndex != oldIndex)
				return CConfig::AvisoPresetTransactionAction::Abort;

			AvisoPreset preset;
			if (!ParseAvisoPreset(items[oldIndex], preset))
				return CConfig::AvisoPresetTransactionAction::Abort;

			oldCanonicalName = preset.name;
			preset.name = trimmedNewName;
			if (linkedMovementOverride.has_value())
				preset.linkedMovement = *linkedMovementOverride;
			rapidjson::Value presetValue;
			WriteAvisoPreset(preset, presetValue, allocator);
			items[oldIndex] = presetValue;

			rapidjson::Value& section = EnsureAirportPresetSection(sharedMetadata, airport, allocator);
			const std::string defaultName = TrimAsciiWhitespaceCopy(
				ReadStringMember(section, kDefaultPresetKey));
			if (!defaultName.empty() && EqualsNoCase(defaultName, oldCanonicalName))
				SetStringMember(section, kDefaultPresetKey, trimmedNewName, allocator);
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (!renamed)
		return false;

	if (EqualsNoCase(ActiveAvisoPresetName, oldCanonicalName))
		ActiveAvisoPresetName = trimmedNewName;
	ReconcileLiveRadarPresetContexts(
		this,
		airport,
		oldCanonicalName,
		trimmedNewName);
	return true;
}

bool CSMRRadar::DuplicateAvisoPreset(const std::string& sourceName, const std::string& requestedName, std::string* outSavedName)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedSourceName = TrimAsciiWhitespaceCopy(sourceName);
	if (trimmedSourceName.empty())
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	std::string uniqueName;
	const bool duplicated = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsurePresetItems(sharedMetadata, airport, allocator);
			const rapidjson::SizeType sourceIndex = FindPresetIndexNoCase(items, trimmedSourceName);
			if (sourceIndex == kInvalidPresetIndex)
				return CConfig::AvisoPresetTransactionAction::Abort;

			AvisoPreset preset;
			if (!ParseAvisoPreset(items[sourceIndex], preset))
				return CConfig::AvisoPresetTransactionAction::Abort;

			uniqueName = MakeUniquePresetName(
				items,
				requestedName.empty() ? ("Copy of " + preset.name) : requestedName);
			preset.name = uniqueName;
			rapidjson::Value presetValue;
			WriteAvisoPreset(preset, presetValue, allocator);
			items.PushBack(presetValue, allocator);
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (!duplicated)
		return false;

	ActiveAvisoPresetName = uniqueName;
	ReconcileLiveRadarPresetContexts(this, airport);
	if (outSavedName != nullptr)
		*outSavedName = uniqueName;
	return true;
}

bool CSMRRadar::DeleteAvisoPreset(const std::string& name)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedName = TrimAsciiWhitespaceCopy(name);
	if (trimmedName.empty())
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	std::string canonicalName = trimmedName;
	const bool deleted = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& section = EnsureAirportPresetSection(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsureArrayMember(section, kPresetItemsKey, allocator);
			const rapidjson::SizeType index = FindPresetIndexNoCase(items, trimmedName);
			if (index == kInvalidPresetIndex)
				return CConfig::AvisoPresetTransactionAction::Abort;

			AvisoPreset preset;
			if (ParseAvisoPreset(items[index], preset))
				canonicalName = preset.name;
			RemoveArrayElement(items, index);

			const std::string defaultName = TrimAsciiWhitespaceCopy(
				ReadStringMember(section, kDefaultPresetKey));
			if (!defaultName.empty() && EqualsNoCase(defaultName, canonicalName) &&
				section.HasMember(kDefaultPresetKey))
			{
				section.RemoveMember(kDefaultPresetKey);
			}
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (!deleted)
		return false;

	if (EqualsNoCase(ActiveAvisoPresetName, canonicalName))
	{
		ActiveAvisoPresetName.clear();
		AvisoViewsLinked = false;
	}
	ReconcileLiveRadarPresetContexts(this, airport);

	return true;
}

bool CSMRRadar::SetDefaultAvisoPreset(const std::string& name)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedName = TrimAsciiWhitespaceCopy(name);
	if (trimmedName.empty())
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	const bool saved = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			const bool migrated = MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsurePresetItems(sharedMetadata, airport, allocator);
			const rapidjson::SizeType index = FindPresetIndexNoCase(items, trimmedName);
			if (index == kInvalidPresetIndex)
				return CConfig::AvisoPresetTransactionAction::Abort;

			AvisoPreset preset;
			if (!ParseAvisoPreset(items[index], preset))
				return CConfig::AvisoPresetTransactionAction::Abort;

			rapidjson::Value& section = EnsureAirportPresetSection(sharedMetadata, airport, allocator);
			const std::string currentDefault = TrimAsciiWhitespaceCopy(
				ReadStringMember(section, kDefaultPresetKey));
			if (EqualsNoCase(currentDefault, preset.name))
			{
				return migrated
					? CConfig::AvisoPresetTransactionAction::Save
					: CConfig::AvisoPresetTransactionAction::NoChange;
			}

			SetStringMember(section, kDefaultPresetKey, preset.name, allocator);
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (saved)
		ReconcileLiveRadarPresetContexts(this, airport);
	return saved;
}

bool CSMRRadar::ClearDefaultAvisoPreset()
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	if (activeProfileName.empty() || airport.empty())
		return false;

	const bool saved = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			const bool migrated = MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			const rapidjson::Value* existingSection = GetAirportPresetSection(sharedMetadata, airport);
			if (existingSection != nullptr && existingSection->HasMember(kDefaultPresetKey))
			{
				rapidjson::Value& section = EnsureAirportPresetSection(
					sharedMetadata,
					airport,
					allocator);
				section.RemoveMember(kDefaultPresetKey);
				return CConfig::AvisoPresetTransactionAction::Save;
			}
			return migrated
				? CConfig::AvisoPresetTransactionAction::Save
				: CConfig::AvisoPresetTransactionAction::NoChange;
		});
	if (saved)
		ReconcileLiveRadarPresetContexts(this, airport);
	return saved;
}

bool CSMRRadar::ApplyDefaultAvisoPresetIfConfigured()
{
	const std::string defaultPreset = GetDefaultAvisoPresetName();
	if (defaultPreset.empty())
		return false;
	return LoadAvisoPreset(defaultPreset);
}

void CSMRRadar::ResetAvisoPresetStateForActiveAirport(bool applyDefaultPreset)
{
	if (CurrentConfig != nullptr)
	{
		const std::string activeProfileName = CurrentConfig->getActiveProfileName();
		const std::string airport = NormalizeAirportKey(getActiveAirport());
		if (!activeProfileName.empty() && !airport.empty())
		{
			CurrentConfig->transactAvisoPresetStore(
				activeProfileName,
				airport,
				[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
					return MigrateLegacyPresetStore(sharedMetadata, airport, allocator)
						? CConfig::AvisoPresetTransactionAction::Save
						: CConfig::AvisoPresetTransactionAction::NoChange;
				});
		}
	}

	ActiveAvisoPresetName.clear();
	AvisoViewsLinked = false;
	if (applyDefaultPreset)
		ApplyDefaultAvisoPresetIfConfigured();
}

bool CSMRRadar::UpdateActiveAvisoPreset()
{
	if (ActiveAvisoPresetName.empty())
		return false;
	return SaveAvisoPreset(ActiveAvisoPresetName, true, nullptr);
}

bool CSMRRadar::ResetActiveAvisoPreset()
{
	if (!ActiveAvisoPresetName.empty())
		return LoadAvisoPreset(ActiveAvisoPresetName);

	const std::string defaultPreset = GetDefaultAvisoPresetName();
	if (!defaultPreset.empty())
		return LoadAvisoPreset(defaultPreset);

	return false;
}

bool CSMRRadar::SetActiveAvisoPresetLinkedMovement(bool linked)
{
	if (ActiveAvisoPresetName.empty())
	{
		AvisoViewsLinked = linked;
		if (AvisoViewsLinked)
			SyncLinkedAvisoSecondaryToMainView();
		SaveInsetStateToAsrForAirport(getActiveAirport());
		RequestRefresh();
		return true;
	}

	if (CurrentConfig == nullptr)
		return false;

	const std::string activeProfileName = CurrentConfig->getActiveProfileName();
	const std::string airport = NormalizeAirportKey(getActiveAirport());
	const std::string presetName = ActiveAvisoPresetName;
	if (activeProfileName.empty() || airport.empty())
		return false;

	const bool saved = CurrentConfig->transactAvisoPresetStore(
		activeProfileName,
		airport,
		[&](rapidjson::Value& sharedMetadata, rapidjson::Document::AllocatorType& allocator) {
			const bool migrated = MigrateLegacyPresetStore(sharedMetadata, airport, allocator);
			rapidjson::Value& items = EnsurePresetItems(sharedMetadata, airport, allocator);
			const rapidjson::SizeType index = FindPresetIndexNoCase(items, presetName);
			if (index == kInvalidPresetIndex || !items[index].IsObject())
				return CConfig::AvisoPresetTransactionAction::Abort;

			const bool currentValue = ReadBoolMember(items[index], "linked_movement", false);
			if (currentValue == linked && items[index].HasMember("linked_movement"))
			{
				return migrated
					? CConfig::AvisoPresetTransactionAction::Save
					: CConfig::AvisoPresetTransactionAction::NoChange;
			}
			SetBoolMember(items[index], "linked_movement", linked, allocator);
			return CConfig::AvisoPresetTransactionAction::Save;
		});
	if (!saved)
		return false;

	AvisoViewsLinked = linked;
	ReconcileLiveRadarPresetContexts(this, airport);
	return true;
}

bool CSMRRadar::IsAvisoPresetLinkedMovementEnabled() const
{
	return AvisoViewsLinked;
}

void CSMRRadar::SyncLinkedAvisoSecondaryToMainView()
{
	if (!AvisoViewsLinked)
		return;

	CInsetWindow* avisoWindow = GetSecondaryAvisoWindow(this);
	if (avisoWindow == nullptr)
		return;

	CPosition displayA;
	CPosition displayB;
	GetDisplayArea(&displayA, &displayB);

	const double minLat = (std::min)(displayA.m_Latitude, displayB.m_Latitude);
	const double maxLat = (std::max)(displayA.m_Latitude, displayB.m_Latitude);
	const double minLon = (std::min)(displayA.m_Longitude, displayB.m_Longitude);
	const double maxLon = (std::max)(displayA.m_Longitude, displayB.m_Longitude);
	const double centerLat = (minLat + maxLat) * 0.5;
	const double centerLon = (minLon + maxLon) * 0.5;

	CRect radarArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	radarArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		radarArea.bottom = chatArea.top;

	const double widthPixels = static_cast<double>((std::max)(1, radarArea.Width()));
	const double heightPixels = static_cast<double>((std::max)(1, radarArea.Height()));

	CPosition leftMid;
	leftMid.m_Latitude = centerLat;
	leftMid.m_Longitude = minLon;
	CPosition rightMid;
	rightMid.m_Latitude = centerLat;
	rightMid.m_Longitude = maxLon;
	CPosition bottomMid;
	bottomMid.m_Latitude = minLat;
	bottomMid.m_Longitude = centerLon;
	CPosition topMid;
	topMid.m_Latitude = maxLat;
	topMid.m_Longitude = centerLon;

	const double widthMeters = Haversine(leftMid, rightMid);
	const double heightMeters = Haversine(bottomMid, topMid);
	const double pixPerMeterX = widthMeters > 1.0 ? widthPixels / widthMeters : 0.0;
	const double pixPerMeterY = heightMeters > 1.0 ? heightPixels / heightMeters : 0.0;
	double pixPerMeter = 0.0;
	if (pixPerMeterX > 0.0 && pixPerMeterY > 0.0)
		pixPerMeter = (std::min)(pixPerMeterX, pixPerMeterY);
	else
		pixPerMeter = (std::max)(pixPerMeterX, pixPerMeterY);

	const int linkedScale = pixPerMeter > 0.0
		? std::clamp(static_cast<int>(std::lround(pixPerMeter * 1852.0)), 1, 2400)
		: avisoWindow->m_AvisoScale;

	const bool changed =
		std::abs(avisoWindow->m_AvisoCenterLatitude - centerLat) > 1e-9 ||
		std::abs(avisoWindow->m_AvisoCenterLongitude - centerLon) > 1e-9 ||
		avisoWindow->m_AvisoScale != linkedScale ||
		!avisoWindow->m_AvisoViewInitialized;
	if (!changed)
		return;

	avisoWindow->m_AvisoCenterLatitude = std::clamp(centerLat, -85.0, 85.0);
	avisoWindow->m_AvisoCenterLongitude = centerLon;
	avisoWindow->m_AvisoScale = linkedScale;
	avisoWindow->m_AvisoViewInitialized = true;
	avisoWindow->ClearAvisoViewportCache();
}
