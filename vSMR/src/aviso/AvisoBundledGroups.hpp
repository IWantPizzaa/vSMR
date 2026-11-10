#pragma once

#include "rapidjson/document.h"
#include <array>
#include <set>
#include <string>

namespace VsmrAvisoBundledGroups
{
	inline bool MergeLfpgLvp(
		rapidjson::Document& source,
		const rapidjson::Value& bundled,
		std::string& error)
	{
		error.clear();
		constexpr std::array<const char*, 4> groupIds{
			"lvp-critical-east", "lvp-critical-west", "lvp-loc-markers", "lvp-hp-cat1-cat3"
		};
		if (!source.IsObject() || !bundled.IsObject() ||
			!source.HasMember("features") || !source["features"].IsArray() ||
			!bundled.HasMember("features") || !bundled["features"].IsArray() ||
			!bundled.HasMember("vsmr_groups") || !bundled["vsmr_groups"].IsArray() ||
			!bundled.HasMember("styles") || !bundled["styles"].IsObject())
		{
			error = "Invalid bundled LFPG LVP geometry.";
			return false;
		}
		std::set<std::string> wanted(groupIds.begin(), groupIds.end());
		std::set<std::string> found;
		for (const auto& group : bundled["vsmr_groups"].GetArray())
			if (group.IsObject() && group.HasMember("id") && group["id"].IsString() &&
				wanted.count(group["id"].GetString()))
				found.insert(group["id"].GetString());
		if (found != wanted)
		{
			error = "Bundled LFPG LVP groups are incomplete.";
			return false;
		}

		std::set<std::string> existingFeatures;
		for (const auto& feature : source["features"].GetArray())
			if (feature.IsObject() && feature.HasMember("id") && feature["id"].IsString())
				existingFeatures.insert(feature["id"].GetString());
		std::set<std::string> neededStyles;
		std::set<std::string> seenGroups;
		for (const auto& feature : bundled["features"].GetArray())
		{
			if (!feature.IsObject() || !feature.HasMember("properties") ||
				!feature["properties"].IsObject()) continue;
			const auto& properties = feature["properties"];
			if (!properties.HasMember("vsmr_group_ids") || !properties["vsmr_group_ids"].IsArray()) continue;
			bool lvp = false;
			for (const auto& group : properties["vsmr_group_ids"].GetArray())
				if (group.IsString() && wanted.count(group.GetString()))
				{
					seenGroups.insert(group.GetString());
					lvp = true;
				}
			if (!lvp) continue;
			if (!feature.HasMember("id") || !feature["id"].IsString() ||
				!properties.HasMember("style_id") || !properties["style_id"].IsString())
			{
				error = "Bundled LFPG LVP feature is missing its identity or style.";
				return false;
			}
			const char* styleId = properties["style_id"].GetString();
			if (!bundled["styles"].HasMember(styleId))
			{
				error = "Bundled LFPG LVP feature references a missing style.";
				return false;
			}
			neededStyles.insert(styleId);
		}
		if (seenGroups != wanted)
		{
			error = "Bundled LFPG LVP group has no geometry.";
			return false;
		}

		auto& allocator = source.GetAllocator();
		if (!source.HasMember("styles"))
			source.AddMember("styles", rapidjson::Value(rapidjson::kObjectType), allocator);
		if (!source["styles"].IsObject())
		{
			error = "The current LFPG styles are invalid.";
			return false;
		}
		if (!source.HasMember("vsmr_groups"))
			source.AddMember("vsmr_groups", rapidjson::Value(rapidjson::kArrayType), allocator);
		if (!source["vsmr_groups"].IsArray())
		{
			error = "The current LFPG groups are invalid.";
			return false;
		}
		std::set<std::string> existingGroups;
		for (const auto& group : source["vsmr_groups"].GetArray())
			if (group.IsObject() && group.HasMember("id") && group["id"].IsString())
				existingGroups.insert(group["id"].GetString());
		for (const auto& group : bundled["vsmr_groups"].GetArray())
		{
			const char* id = group.IsObject() && group.HasMember("id") && group["id"].IsString()
				? group["id"].GetString() : "";
			if (!wanted.count(id) || existingGroups.count(id)) continue;
			rapidjson::Value copy(group, allocator);
			source["vsmr_groups"].PushBack(copy, allocator);
			existingGroups.insert(id);
		}
		for (const std::string& styleId : neededStyles)
		{
			if (source["styles"].HasMember(styleId.c_str())) continue;
			rapidjson::Value name(styleId.c_str(), static_cast<rapidjson::SizeType>(styleId.size()), allocator);
			rapidjson::Value copy(bundled["styles"][styleId.c_str()], allocator);
			source["styles"].AddMember(name, copy, allocator);
		}
		for (const auto& feature : bundled["features"].GetArray())
		{
			if (!feature.IsObject() || !feature.HasMember("id") || !feature["id"].IsString() ||
				!feature.HasMember("properties") || !feature["properties"].IsObject()) continue;
			const auto& properties = feature["properties"];
			if (!properties.HasMember("vsmr_group_ids") || !properties["vsmr_group_ids"].IsArray()) continue;
			bool lvp = false;
			for (const auto& group : properties["vsmr_group_ids"].GetArray())
				lvp = lvp || (group.IsString() && wanted.count(group.GetString()) != 0);
			if (!lvp || !existingFeatures.insert(feature["id"].GetString()).second) continue;
			rapidjson::Value copy(feature, allocator);
			source["features"].PushBack(copy, allocator);
		}
		if (bundled.HasMember("metadata") && bundled["metadata"].IsObject() &&
			bundled["metadata"].HasMember("lvp_overlay"))
		{
			if (!source.HasMember("metadata"))
				source.AddMember("metadata", rapidjson::Value(rapidjson::kObjectType), allocator);
			if (source["metadata"].IsObject() && !source["metadata"].HasMember("lvp_overlay"))
			{
				rapidjson::Value copy(bundled["metadata"]["lvp_overlay"], allocator);
				source["metadata"].AddMember("lvp_overlay", copy, allocator);
			}
		}
		return true;
	}
}
