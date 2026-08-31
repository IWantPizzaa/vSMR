#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoFeatureMetadata.hpp"

namespace
{
	void PushUniqueGroupId(
		std::vector<std::string>& groupIds,
		const std::string& groupId)
	{
		if (groupId.empty() ||
			std::find(groupIds.begin(), groupIds.end(), groupId) != groupIds.end())
		{
			return;
		}
		groupIds.push_back(groupId);
	}
}

namespace VsmrAvisoFeatureMetadata
{
	std::string TrimAirportCode(const std::string& value)
	{
		std::size_t start = 0;
		while (start < value.size() &&
			std::isspace(static_cast<unsigned char>(value[start])) != 0)
		{
			++start;
		}

		std::size_t end = value.size();
		while (end > start &&
			std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
		{
			--end;
		}

		return value.substr(start, end - start);
	}

	bool TryReadGroupIds(
		const rapidjson::Value* properties,
		std::vector<std::string>& groupIds)
	{
		groupIds.clear();
		if (properties == nullptr || !properties->IsObject())
			return true;

		const char* arrayKeys[] = {
			"vsmr_group_ids",
			"vsmr_groups",
			"group_ids"
		};
		for (const char* key : arrayKeys)
		{
			if (!properties->HasMember(key))
				continue;

			const rapidjson::Value& value = (*properties)[key];
			if (value.IsArray())
			{
				for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
				{
					if (!value[i].IsString())
						return false;
					PushUniqueGroupId(groupIds, value[i].GetString());
				}
			}
			else if (value.IsString())
			{
				PushUniqueGroupId(groupIds, value.GetString());
			}
			else
			{
				return false;
			}

			// The first modern membership property is authoritative, including
			// an explicitly empty array used to remove legacy membership.
			return true;
		}

		const char* scalarKeys[] = {
			"group_id",
			"vsmr_group_id"
		};
		for (const char* key : scalarKeys)
		{
			if (!properties->HasMember(key))
				continue;
			const rapidjson::Value& value = (*properties)[key];
			if (!value.IsString())
				return false;
			PushUniqueGroupId(groupIds, value.GetString());
			return true;
		}

		return true;
	}

	std::vector<std::string> ReadGroupIds(const rapidjson::Value* properties)
	{
		std::vector<std::string> groupIds;
		if (!TryReadGroupIds(properties, groupIds))
			groupIds.clear();
		return groupIds;
	}

	std::string ReadFeatureIdentity(const rapidjson::Value& feature)
	{
		if (!feature.IsObject())
			return "";
		if (feature.HasMember("id") && feature["id"].IsString())
			return feature["id"].GetString();
		if (feature.HasMember("properties") &&
			feature["properties"].IsObject() &&
			feature["properties"].HasMember("id") &&
			feature["properties"]["id"].IsString())
		{
			return feature["properties"]["id"].GetString();
		}
		return "";
	}
}
