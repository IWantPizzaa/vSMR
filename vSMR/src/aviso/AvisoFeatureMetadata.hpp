#pragma once

#include "rapidjson/document.h"

#include <string>
#include <vector>

// Feature identity and group membership are consumed by both the dataset
// loader and live group editing. Keep their compatibility rules identical.
namespace VsmrAvisoFeatureMetadata
{
	std::string TrimAirportCode(const std::string& value);
	bool TryReadGroupIds(
		const rapidjson::Value* properties,
		std::vector<std::string>& groupIds);
	std::vector<std::string> ReadGroupIds(const rapidjson::Value* properties);
	std::string ReadFeatureIdentity(const rapidjson::Value& feature);
}
