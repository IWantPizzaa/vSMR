#pragma once
#include "tags/TagTokenValues.hpp"

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

	struct CdmColorRuleDefinition : ColorRuleChannels
	{
		std::string token;
		std::string expectedState;
	};

	struct FieldRuleEffects
	{
		bool hasColor = false;
		int colorR = 255;
		int colorG = 255;
		int colorB = 255;
		int colorA = 255;
		bool hasBackground = false;
		int backgroundR = 255;
		int backgroundG = 255;
		int backgroundB = 255;
		int backgroundA = 255;
		bool hasBold = false;
		bool bold = false;
		bool hasBlink = false;
		bool blink = false;
	};

	struct TagColorRuleOverrides : ColorRuleChannels
	{
		std::map<std::string, FieldRuleEffects> fieldEffects;
	};

	struct RunwayColorRuleDefinition : ColorRuleChannels
	{
		std::string token;
		std::string expectedRunway;
	};

	void MergeColorRuleOverrides(
		TagColorRuleOverrides& target,
		const TagColorRuleOverrides& source);
	void MergeMissingColorRuleOverrides(
		TagColorRuleOverrides& target,
		const TagColorRuleOverrides& fallback);
	bool TryParseCdmColorRuleToken(
		const std::string& rawToken,
		CdmColorRuleDefinition& outRule);
	bool TryParseRunwayColorRuleToken(
		const std::string& rawToken,
		RunwayColorRuleDefinition& outRule);
	std::string ResolveCdmRuleStateName(
		const std::string& token,
		const CdmPilotData* pilotData,
		std::time_t now = 0);
	bool TryParseStructuredRuleV2(
		const rapidjson::Value& value,
		StructuredTagColorRule& outRule,
		std::string* error = nullptr);
	bool WriteStructuredRuleV2(
		const StructuredTagColorRule& rule,
		rapidjson::Value& output,
		rapidjson::Document::AllocatorType& allocator);
	bool IsStructuredRuleClockSensitive(const StructuredTagColorRule& rule);
	void CollectCdmColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<CdmColorRuleDefinition>& outRules);
	void CollectRunwayColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<RunwayColorRuleDefinition>& outRules);
	std::vector<std::string> ConvertDefinitionValueToLineTexts(
		const rapidjson::Value& labelLines);
	TagColorRuleOverrides EvaluateCdmColorRules(
		const std::vector<CdmColorRuleDefinition>& rules,
		const CdmPilotData* pilotData,
		std::time_t now = 0);
	TagColorRuleOverrides EvaluateRunwayColorRules(
		const std::vector<RunwayColorRuleDefinition>& rules,
		const VsmrTags::TokenValues& replacingMap);
	// Optional inheritance fills only unassigned detailed channels from v1
	// normal rules; v2 effects always retain their explicit tag-detail scope.
	TagColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		bool isTagDetailed,
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now = 0,
		bool inheritLegacyNormal = false);
	// Aircraft icons are shared by all viewports: v2 tag-detail scope applies
	// only to tag effects. Legacy target effects retain their normal-only scope.
	TagColorRuleOverrides EvaluateStructuredTargetColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now = 0);
}
