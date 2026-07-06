#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"

#include <cctype>

namespace
{
	const char* kAvisoPresetsKey = "aviso_presets";
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

	const rapidjson::Value* GetPresetItems(const rapidjson::Value& profile)
	{
		const rapidjson::Value* section = GetObjectMember(profile, kAvisoPresetsKey);
		return section != nullptr ? GetArrayMember(*section, kPresetItemsKey) : nullptr;
	}

	rapidjson::Value& EnsurePresetItems(rapidjson::Value& profile, rapidjson::Document::AllocatorType& allocator)
	{
		rapidjson::Value& section = EnsureObjectMember(profile, kAvisoPresetsKey, allocator);
		return EnsureArrayMember(section, kPresetItemsKey, allocator);
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
			baseName = "AVISO Preset";

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

		if (minLat >= maxLat || minLon >= maxLon)
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
			out.secondaryVisible = ReadBoolMember(*secondary, "visible", true);
			out.secondaryArea.left = ReadIntMember(*secondary, "left", out.secondaryArea.left);
			out.secondaryArea.top = ReadIntMember(*secondary, "top", out.secondaryArea.top);
			out.secondaryArea.right = ReadIntMember(*secondary, "right", out.secondaryArea.right);
			out.secondaryArea.bottom = ReadIntMember(*secondary, "bottom", out.secondaryArea.bottom);
			out.secondaryScale = std::clamp(ReadIntMember(*secondary, "scale", out.secondaryScale), 1, 2400);
			ReadDoubleMember(*secondary, "center_latitude", out.secondaryCenterLatitude);
			ReadDoubleMember(*secondary, "center_longitude", out.secondaryCenterLongitude);
			if (secondary->HasMember("layout_mode"))
				out.secondaryLayoutMode = LayoutModeFromValue((*secondary)["layout_mode"], out.secondaryLayoutMode);
			else if (secondary->HasMember("layout_mode_id"))
				out.secondaryLayoutMode = LayoutModeFromValue((*secondary)["layout_mode_id"], out.secondaryLayoutMode);
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
}

std::vector<CSMRRadar::AvisoPreset> CSMRRadar::GetAvisoPresets() const
{
	std::vector<AvisoPreset> presets;
	if (CurrentConfig == nullptr)
		return presets;

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	const rapidjson::Value* items = GetPresetItems(profile);
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

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	const rapidjson::Value* section = GetObjectMember(profile, kAvisoPresetsKey);
	if (section == nullptr)
		return "";

	return TrimAsciiWhitespaceCopy(ReadStringMember(*section, kDefaultPresetKey));
}

std::string CSMRRadar::GetActiveAvisoPresetName() const
{
	return ActiveAvisoPresetName;
}

bool CSMRRadar::SaveAvisoPreset(const std::string& requestedName, bool overwriteExisting, std::string* outSavedName)
{
	if (CurrentConfig == nullptr)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& items = EnsurePresetItems(profile, allocator);
	std::string presetName = TrimAsciiWhitespaceCopy(requestedName);
	if (presetName.empty())
		presetName = "AVISO Preset";
	const rapidjson::SizeType existingIndex = FindPresetIndexNoCase(items, presetName);
	if (existingIndex != kInvalidPresetIndex && !overwriteExisting)
		return false;

	AvisoPreset preset;
	preset.name = presetName;
	preset.linkedMovement = AvisoViewsLinked;

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

	rapidjson::Value presetValue;
	WriteAvisoPreset(preset, presetValue, allocator);

	if (existingIndex != kInvalidPresetIndex)
	{
		items[existingIndex] = presetValue;
	}
	else
	{
		items.PushBack(presetValue, allocator);
	}

	if (!CurrentConfig->saveConfig())
		return false;

	ActiveAvisoPresetName = presetName;
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

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	const rapidjson::Value* items = GetPresetItems(profile);
	if (items == nullptr)
		return false;

	const rapidjson::SizeType index = FindPresetIndexNoCase(*items, requestedName);
	if (index == kInvalidPresetIndex)
		return false;

	AvisoPreset preset;
	if (!ParseAvisoPreset((*items)[index], preset))
		return false;

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
	AvisoViewsLinked = preset.linkedMovement;
	ActiveAvisoPresetName = preset.name;
	if (AvisoViewsLinked)
		SyncLinkedAvisoSecondaryToMainView();

	RequestRefresh();
	return true;
}

bool CSMRRadar::RenameAvisoPreset(const std::string& oldName, const std::string& newName)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedOldName = TrimAsciiWhitespaceCopy(oldName);
	const std::string trimmedNewName = TrimAsciiWhitespaceCopy(newName);
	if (trimmedOldName.empty() || trimmedNewName.empty())
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& items = EnsurePresetItems(profile, allocator);
	const rapidjson::SizeType oldIndex = FindPresetIndexNoCase(items, trimmedOldName);
	if (oldIndex == kInvalidPresetIndex)
		return false;

	const rapidjson::SizeType newIndex = FindPresetIndexNoCase(items, trimmedNewName);
	if (newIndex != kInvalidPresetIndex && newIndex != oldIndex)
		return false;

	AvisoPreset preset;
	if (!ParseAvisoPreset(items[oldIndex], preset))
		return false;

	const std::string oldCanonicalName = preset.name;
	preset.name = trimmedNewName;
	rapidjson::Value presetValue;
	WriteAvisoPreset(preset, presetValue, allocator);
	items[oldIndex] = presetValue;

	rapidjson::Value& section = EnsureObjectMember(profile, kAvisoPresetsKey, allocator);
	const std::string defaultName = GetDefaultAvisoPresetName();
	if (!defaultName.empty() && EqualsNoCase(defaultName, oldCanonicalName))
		SetStringMember(section, kDefaultPresetKey, trimmedNewName, allocator);

	if (EqualsNoCase(ActiveAvisoPresetName, oldCanonicalName))
		ActiveAvisoPresetName = trimmedNewName;

	return CurrentConfig->saveConfig();
}

bool CSMRRadar::DuplicateAvisoPreset(const std::string& sourceName, const std::string& requestedName, std::string* outSavedName)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedSourceName = TrimAsciiWhitespaceCopy(sourceName);
	if (trimmedSourceName.empty())
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& items = EnsurePresetItems(profile, allocator);
	const rapidjson::SizeType sourceIndex = FindPresetIndexNoCase(items, trimmedSourceName);
	if (sourceIndex == kInvalidPresetIndex)
		return false;

	AvisoPreset preset;
	if (!ParseAvisoPreset(items[sourceIndex], preset))
		return false;

	const std::string uniqueName = MakeUniquePresetName(items, requestedName.empty() ? ("Copy of " + preset.name) : requestedName);
	preset.name = uniqueName;

	rapidjson::Value presetValue;
	WriteAvisoPreset(preset, presetValue, allocator);
	items.PushBack(presetValue, allocator);
	if (!CurrentConfig->saveConfig())
		return false;

	ActiveAvisoPresetName = uniqueName;
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

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& section = EnsureObjectMember(profile, kAvisoPresetsKey, allocator);
	rapidjson::Value& items = EnsureArrayMember(section, kPresetItemsKey, allocator);
	const rapidjson::SizeType index = FindPresetIndexNoCase(items, trimmedName);
	if (index == kInvalidPresetIndex)
		return false;

	std::string canonicalName = trimmedName;
	AvisoPreset preset;
	if (ParseAvisoPreset(items[index], preset))
		canonicalName = preset.name;

	RemoveArrayElement(items, index);
	const std::string defaultName = GetDefaultAvisoPresetName();
	if (!defaultName.empty() && EqualsNoCase(defaultName, canonicalName) && section.HasMember(kDefaultPresetKey))
		section.RemoveMember(kDefaultPresetKey);

	if (EqualsNoCase(ActiveAvisoPresetName, canonicalName))
		ActiveAvisoPresetName.clear();

	return CurrentConfig->saveConfig();
}

bool CSMRRadar::SetDefaultAvisoPreset(const std::string& name)
{
	if (CurrentConfig == nullptr)
		return false;

	const std::string trimmedName = TrimAsciiWhitespaceCopy(name);
	if (trimmedName.empty())
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& items = EnsurePresetItems(profile, allocator);
	const rapidjson::SizeType index = FindPresetIndexNoCase(items, trimmedName);
	if (index == kInvalidPresetIndex)
		return false;

	AvisoPreset preset;
	if (!ParseAvisoPreset(items[index], preset))
		return false;

	rapidjson::Value& section = EnsureObjectMember(profile, kAvisoPresetsKey, allocator);
	SetStringMember(section, kDefaultPresetKey, preset.name, allocator);
	return CurrentConfig->saveConfig();
}

bool CSMRRadar::ClearDefaultAvisoPreset()
{
	if (CurrentConfig == nullptr)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	rapidjson::Value& section = EnsureObjectMember(profile, kAvisoPresetsKey, CurrentConfig->document.GetAllocator());
	if (section.HasMember(kDefaultPresetKey))
		section.RemoveMember(kDefaultPresetKey);
	return CurrentConfig->saveConfig();
}

bool CSMRRadar::ApplyDefaultAvisoPresetIfConfigured()
{
	const std::string defaultPreset = GetDefaultAvisoPresetName();
	if (defaultPreset.empty())
		return false;
	return LoadAvisoPreset(defaultPreset);
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
	AvisoViewsLinked = linked;
	if (ActiveAvisoPresetName.empty())
	{
		if (AvisoViewsLinked)
			SyncLinkedAvisoSecondaryToMainView();
		RequestRefresh();
		return true;
	}

	if (CurrentConfig == nullptr)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	rapidjson::Value& items = EnsurePresetItems(profile, allocator);
	const rapidjson::SizeType index = FindPresetIndexNoCase(items, ActiveAvisoPresetName);
	if (index == kInvalidPresetIndex || !items[index].IsObject())
		return false;

	SetBoolMember(items[index], "linked_movement", linked, allocator);
	if (!CurrentConfig->saveConfig())
		return false;

	if (AvisoViewsLinked)
		SyncLinkedAvisoSecondaryToMainView();
	RequestRefresh();
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
