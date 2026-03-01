#include "stdafx.h"
#include "SMRRadar.hpp"

std::string CSMRRadar::NormalizeTargetIconStyle(const std::string& style) const
{
	std::string lowered = style;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (lowered.find("diamond") != std::string::npos ||
		lowered.find("square") != std::string::npos ||
		lowered.find("rhomb") != std::string::npos)
		return "diamond";

	if (lowered.find("triang") != std::string::npos ||
		lowered.find("arrow") != std::string::npos ||
		lowered.find("draw") != std::string::npos ||
		lowered.find("legacy") != std::string::npos)
		return "triangle";

	return "realistic";
}

std::string CSMRRadar::GetActiveTargetIconStyle() const
{
	if (!CurrentConfig)
		return "realistic";

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return "realistic";

	const Value& targets = profile["targets"];
	if (!targets.HasMember("icon_style") || !targets["icon_style"].IsString())
		return "realistic";

	return NormalizeTargetIconStyle(targets["icon_style"].GetString());
}

bool CSMRRadar::SetActiveTargetIconStyle(const std::string& style, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	const std::string normalizedStyle = NormalizeTargetIconStyle(style);
	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("icon_style") || !targets["icon_style"].IsString())
	{
		if (targets.HasMember("icon_style"))
			targets.RemoveMember("icon_style");

		Value key;
		key.SetString("icon_style", allocator);
		Value value;
		value.SetString(normalizedStyle.c_str(), static_cast<rapidjson::SizeType>(normalizedStyle.size()), allocator);
		targets.AddMember(key, value, allocator);
		changed = true;
	}
	else if (normalizedStyle != targets["icon_style"].GetString())
	{
		targets["icon_style"].SetString(normalizedStyle.c_str(), static_cast<rapidjson::SizeType>(normalizedStyle.size()), allocator);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

bool CSMRRadar::GetFixedPixelTargetIconSizeEnabled() const
{
	if (!CurrentConfig)
		return false;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return false;

	const Value& targets = profile["targets"];
	if (!targets.HasMember("fixed_pixel_icon_size") || !targets["fixed_pixel_icon_size"].IsBool())
		return false;

	return targets["fixed_pixel_icon_size"].GetBool();
}

bool CSMRRadar::SetFixedPixelTargetIconSizeEnabled(bool enabled, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("fixed_pixel_icon_size") || !targets["fixed_pixel_icon_size"].IsBool())
	{
		if (targets.HasMember("fixed_pixel_icon_size"))
			targets.RemoveMember("fixed_pixel_icon_size");

		Value key;
		key.SetString("fixed_pixel_icon_size", allocator);
		Value boolValue;
		boolValue.SetBool(enabled);
		targets.AddMember(key, boolValue, allocator);
		changed = true;
	}
	else if (targets["fixed_pixel_icon_size"].GetBool() != enabled)
	{
		targets["fixed_pixel_icon_size"].SetBool(enabled);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

double CSMRRadar::GetFixedPixelTriangleIconScale() const
{
	if (!CurrentConfig)
		return 1.0;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return 1.0;

	const Value& targets = profile["targets"];
	if (!targets.HasMember("fixed_pixel_triangle_scale") || !targets["fixed_pixel_triangle_scale"].IsNumber())
		return 1.0;

	return std::clamp(targets["fixed_pixel_triangle_scale"].GetDouble(), 0.1, 3.0);
}

bool CSMRRadar::SetFixedPixelTriangleIconScale(double scale, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	scale = std::clamp(scale, 0.1, 3.0);

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("fixed_pixel_triangle_scale") || !targets["fixed_pixel_triangle_scale"].IsNumber())
	{
		if (targets.HasMember("fixed_pixel_triangle_scale"))
			targets.RemoveMember("fixed_pixel_triangle_scale");

		Value key;
		key.SetString("fixed_pixel_triangle_scale", allocator);
		Value numberValue;
		numberValue.SetDouble(scale);
		targets.AddMember(key, numberValue, allocator);
		changed = true;
	}
	else if (fabs(targets["fixed_pixel_triangle_scale"].GetDouble() - scale) > 0.0001)
	{
		targets["fixed_pixel_triangle_scale"].SetDouble(scale);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

bool CSMRRadar::GetSmallTargetIconBoostEnabled() const
{
	if (!CurrentConfig)
		return false;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return false;

	const Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost") || !targets["small_icon_boost"].IsBool())
		return false;

	return targets["small_icon_boost"].GetBool();
}

bool CSMRRadar::SetSmallTargetIconBoostEnabled(bool enabled, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost") || !targets["small_icon_boost"].IsBool())
	{
		if (targets.HasMember("small_icon_boost"))
			targets.RemoveMember("small_icon_boost");

		Value key;
		key.SetString("small_icon_boost", allocator);
		Value boolValue;
		boolValue.SetBool(enabled);
		targets.AddMember(key, boolValue, allocator);
		changed = true;
	}
	else if (targets["small_icon_boost"].GetBool() != enabled)
	{
		targets["small_icon_boost"].SetBool(enabled);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

double CSMRRadar::GetSmallTargetIconBoostFactor() const
{
	if (!CurrentConfig)
		return 1.0;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return 1.0;

	const Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost_factor") || !targets["small_icon_boost_factor"].IsNumber())
		return 1.0;

	return std::clamp(targets["small_icon_boost_factor"].GetDouble(), 0.5, 4.0);
}

bool CSMRRadar::SetSmallTargetIconBoostFactor(double factor, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	factor = std::clamp(factor, 0.5, 4.0);

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost_factor") || !targets["small_icon_boost_factor"].IsNumber())
	{
		if (targets.HasMember("small_icon_boost_factor"))
			targets.RemoveMember("small_icon_boost_factor");

		Value key;
		key.SetString("small_icon_boost_factor", allocator);
		Value numberValue;
		numberValue.SetDouble(factor);
		targets.AddMember(key, numberValue, allocator);
		changed = true;
	}
	else if (fabs(targets["small_icon_boost_factor"].GetDouble() - factor) > 0.0001)
	{
		targets["small_icon_boost_factor"].SetDouble(factor);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

std::string CSMRRadar::NormalizeSmallTargetIconBoostResolutionPreset(const std::string& preset) const
{
	std::string lowered = preset;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (lowered.find("4k") != std::string::npos || lowered.find("2160") != std::string::npos || lowered.find("uhd") != std::string::npos)
		return "4k";
	if (lowered.find("2k") != std::string::npos || lowered.find("1440") != std::string::npos || lowered.find("qhd") != std::string::npos)
		return "2k";
	return "1080p";
}

std::string CSMRRadar::GetSmallTargetIconBoostResolutionPreset() const
{
	if (!CurrentConfig)
		return "1080p";

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("targets") || !profile["targets"].IsObject())
		return "1080p";

	const Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost_resolution") || !targets["small_icon_boost_resolution"].IsString())
		return "1080p";

	return NormalizeSmallTargetIconBoostResolutionPreset(targets["small_icon_boost_resolution"].GetString());
}

bool CSMRRadar::SetSmallTargetIconBoostResolutionPreset(const std::string& preset, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	const std::string normalizedPreset = NormalizeSmallTargetIconBoostResolutionPreset(preset);

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("targets") || !profile["targets"].IsObject())
	{
		if (profile.HasMember("targets"))
			profile.RemoveMember("targets");

		Value key;
		key.SetString("targets", allocator);
		Value targetsObject(kObjectType);
		profile.AddMember(key, targetsObject, allocator);
		changed = true;
	}

	Value& targets = profile["targets"];
	if (!targets.HasMember("small_icon_boost_resolution") || !targets["small_icon_boost_resolution"].IsString())
	{
		if (targets.HasMember("small_icon_boost_resolution"))
			targets.RemoveMember("small_icon_boost_resolution");

		Value key;
		key.SetString("small_icon_boost_resolution", allocator);
		Value presetValue;
		presetValue.SetString(normalizedPreset.c_str(), static_cast<rapidjson::SizeType>(normalizedPreset.size()), allocator);
		targets.AddMember(key, presetValue, allocator);
		changed = true;
	}
	else if (normalizedPreset != targets["small_icon_boost_resolution"].GetString())
	{
		targets["small_icon_boost_resolution"].SetString(normalizedPreset.c_str(), static_cast<rapidjson::SizeType>(normalizedPreset.size()), allocator);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

double CSMRRadar::GetSmallTargetIconBoostResolutionScale() const
{
	const std::string preset = GetSmallTargetIconBoostResolutionPreset();
	if (preset == "4k")
		return 1.55;
	if (preset == "2k")
		return 1.25;
	return 1.0;
}

std::vector<std::string> CSMRRadar::GetAvailableTagFonts() const
{
	std::vector<std::string> fallbackFonts = {
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

	if (!CurrentConfig)
		return fallbackFonts;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("font") || !profile["font"].IsObject())
		return fallbackFonts;

	const Value& font = profile["font"];
	if (!font.HasMember("available_fonts") || !font["available_fonts"].IsArray())
		return fallbackFonts;

	auto normalizeText = [](std::string value) -> std::string
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	};

	std::vector<std::string> fonts;
	auto appendIfMissing = [&](const std::string& fontName)
	{
		if (fontName.empty())
			return;

		const std::string normalized = normalizeText(fontName);
		for (const std::string& existing : fonts)
		{
			if (normalizeText(existing) == normalized)
				return;
		}
		fonts.push_back(fontName);
	};

	const Value& configuredFonts = font["available_fonts"];
	for (rapidjson::SizeType i = 0; i < configuredFonts.Size(); ++i)
	{
		if (!configuredFonts[i].IsString())
			continue;
		appendIfMissing(configuredFonts[i].GetString());
	}

	if (fonts.empty())
		return fallbackFonts;

	return fonts;
}

int CSMRRadar::GetActiveLabelFontSize() const
{
	if (!CurrentConfig)
		return 1;

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("font") || !profile["font"].IsObject())
		return 1;

	const Value& font = profile["font"];
	if (!font.HasMember("label_font_size") || !font["label_font_size"].IsInt())
		return 1;

	return std::clamp(font["label_font_size"].GetInt(), 1, 5);
}

bool CSMRRadar::SetActiveLabelFontSize(int size, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	size = std::clamp(size, 1, 5);

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("font") || !profile["font"].IsObject())
	{
		if (profile.HasMember("font"))
			profile.RemoveMember("font");

		Value fontKey;
		fontKey.SetString("font", allocator);
		Value fontObject(kObjectType);
		profile.AddMember(fontKey, fontObject, allocator);
		changed = true;
	}

	Value& font = profile["font"];
	if (!font.HasMember("label_font_size") || !font["label_font_size"].IsInt())
	{
		if (font.HasMember("label_font_size"))
			font.RemoveMember("label_font_size");

		Value sizeKey;
		sizeKey.SetString("label_font_size", allocator);
		Value sizeValue;
		sizeValue.SetInt(size);
		font.AddMember(sizeKey, sizeValue, allocator);
		changed = true;
	}
	else if (font["label_font_size"].GetInt() != size)
	{
		font["label_font_size"].SetInt(size);
		changed = true;
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}

std::string CSMRRadar::GetActiveTagFontName() const
{
	if (!CurrentConfig)
		return "EuroScope";

	const Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("font") || !profile["font"].IsObject())
		return "EuroScope";

	const Value& font = profile["font"];
	if (!font.HasMember("font_name") || !font["font_name"].IsString())
		return "EuroScope";

	const std::string fontName = font["font_name"].GetString();
	if (fontName.empty())
		return "EuroScope";

	return fontName;
}

bool CSMRRadar::SetActiveTagFontName(const std::string& fontName, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	const std::string normalizedFontName = fontName.empty() ? "EuroScope" : fontName;
	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	if (!profile.HasMember("font") || !profile["font"].IsObject())
	{
		if (profile.HasMember("font"))
			profile.RemoveMember("font");

		Value fontKey;
		fontKey.SetString("font", allocator);
		Value fontObject(kObjectType);

		Value fontNameKey;
		fontNameKey.SetString("font_name", allocator);
		Value fontNameValue;
		fontNameValue.SetString(normalizedFontName.c_str(), static_cast<rapidjson::SizeType>(normalizedFontName.size()), allocator);
		fontObject.AddMember(fontNameKey, fontNameValue, allocator);

		Value weightKey;
		weightKey.SetString("weight", allocator);
		Value weightValue;
		weightValue.SetString("Regular", allocator);
		fontObject.AddMember(weightKey, weightValue, allocator);

		Value sizesKey;
		sizesKey.SetString("sizes", allocator);
		Value sizesObject(kObjectType);
		{
			Value key;
			key.SetString("one", allocator);
			Value value;
			value.SetInt(10);
			sizesObject.AddMember(key, value, allocator);
		}
		{
			Value key;
			key.SetString("two", allocator);
			Value value;
			value.SetInt(11);
			sizesObject.AddMember(key, value, allocator);
		}
		{
			Value key;
			key.SetString("three", allocator);
			Value value;
			value.SetInt(12);
			sizesObject.AddMember(key, value, allocator);
		}
		{
			Value key;
			key.SetString("four", allocator);
			Value value;
			value.SetInt(13);
			sizesObject.AddMember(key, value, allocator);
		}
		{
			Value key;
			key.SetString("five", allocator);
			Value value;
			value.SetInt(14);
			sizesObject.AddMember(key, value, allocator);
		}
		fontObject.AddMember(sizesKey, sizesObject, allocator);

		profile.AddMember(fontKey, fontObject, allocator);
		changed = true;
	}
	else
	{
		Value& font = profile["font"];
		if (!font.HasMember("font_name") || !font["font_name"].IsString())
		{
			if (font.HasMember("font_name"))
				font.RemoveMember("font_name");

			Value key;
			key.SetString("font_name", allocator);
			Value value;
			value.SetString(normalizedFontName.c_str(), static_cast<rapidjson::SizeType>(normalizedFontName.size()), allocator);
			font.AddMember(key, value, allocator);
			changed = true;
		}
		else if (normalizedFontName != font["font_name"].GetString())
		{
			font["font_name"].SetString(normalizedFontName.c_str(), static_cast<rapidjson::SizeType>(normalizedFontName.size()), allocator);
			changed = true;
		}
	}

	if (!changed)
		return true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}



