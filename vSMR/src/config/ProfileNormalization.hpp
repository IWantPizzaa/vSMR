#pragma once
#include "rapidjson/document.h"
#include <string>

namespace VsmrProfile
{
	// Normalizes the supplied in-memory profile; callers decide whether to save.
	bool Normalize(rapidjson::Value& profile, rapidjson::Document::AllocatorType& allocator);
	std::string NormalizeTargetIconStyle(const std::string& style);
	std::string NormalizeTagDefinitionDepartureStatus(const std::string& status);
	std::string NormalizeStructuredRuleSource(const std::string& source);
	std::string NormalizeStructuredRuleToken(const std::string& source, const std::string& token);
	std::string NormalizeStructuredRuleCondition(const std::string& source, const std::string& condition);
	std::string NormalizeStructuredRuleTagType(const std::string& tagType);
	std::string NormalizeStructuredRuleStatus(const std::string& status);
	std::string NormalizeStructuredRuleDetail(const std::string& detail);
}
