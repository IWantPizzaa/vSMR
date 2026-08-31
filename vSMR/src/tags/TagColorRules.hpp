#pragma once

#include "tags/TagDataTypes.hpp"

#include "rapidjson/document.h"

#include <map>
#include <string>
#include <vector>

namespace VsmrTagColorRules
{
	struct ColorRuleChannels
	{
		bool hasTargetColor = false;
		int targetR = 255;
		int targetG = 255;
		int targetB = 255;
		int targetA = 255;
		bool hasTagColor = false;
		int tagR = 255;
		int tagG = 255;
		int tagB = 255;
		int tagA = 255;
		bool hasTextColor = false;
		int textR = 255;
		int textG = 255;
		int textB = 255;
		int textA = 255;
	};

	struct VacdmColorRuleDefinition : ColorRuleChannels
	{
		std::string token;
		std::string expectedState;
	};

	struct VacdmColorRuleOverrides : ColorRuleChannels
	{
	};

	struct RunwayColorRuleDefinition : ColorRuleChannels
	{
		std::string token;
		std::string expectedRunway;
	};

	void MergeColorRuleOverrides(
		VacdmColorRuleOverrides& target,
		const VacdmColorRuleOverrides& source);
	void MergeMissingColorRuleOverrides(
		VacdmColorRuleOverrides& target,
		const VacdmColorRuleOverrides& fallback);
	bool TryParseVacdmColorRuleToken(
		const std::string& rawToken,
		VacdmColorRuleDefinition& outRule);
	bool TryParseRunwayColorRuleToken(
		const std::string& rawToken,
		RunwayColorRuleDefinition& outRule);
	std::string ResolveVacdmRuleStateName(
		const std::string& token,
		const VacdmPilotData* pilotData);
	void CollectVacdmColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<VacdmColorRuleDefinition>& outRules);
	void CollectRunwayColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<RunwayColorRuleDefinition>& outRules);
	std::vector<std::string> ConvertDefinitionValueToLineTexts(
		const rapidjson::Value& labelLines);
	VacdmColorRuleOverrides EvaluateVacdmColorRules(
		const std::vector<VacdmColorRuleDefinition>& rules,
		const VacdmPilotData* pilotData);
	VacdmColorRuleOverrides EvaluateRunwayColorRules(
		const std::vector<RunwayColorRuleDefinition>& rules,
		const std::map<std::string, std::string>& replacingMap);
	VacdmColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		bool isTagDetailed,
		const std::map<std::string, std::string>& replacingMap,
		const VacdmPilotData* pilotData);
}
