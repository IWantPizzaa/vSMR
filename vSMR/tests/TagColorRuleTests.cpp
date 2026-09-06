#include "TagColorRuleTests.hpp"

#include "tags/TagColorRules.hpp"

#include "rapidjson/document.h"

#include <map>

namespace
{
	void Check(
		bool condition,
		const char* message,
		std::vector<std::string>& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}
}

std::vector<std::string> RunTagColorRuleTests()
{
	using namespace VsmrTagColorRules;

	std::vector<std::string> failures;
	VacdmColorRuleDefinition vacdmRule;
	Check(
		TryParseVacdmColorRuleToken(
			"TOBT(state_confirmed=[target, tag, color='(12,34,56)', color_text='(7,8,9)'])",
			vacdmRule) &&
			vacdmRule.token == "tobt" && vacdmRule.expectedState == "confirmed" &&
			vacdmRule.hasTargetColor && vacdmRule.targetR == 12 && vacdmRule.targetG == 34 && vacdmRule.targetB == 56 &&
			vacdmRule.hasTagColor && vacdmRule.tagR == 12 && vacdmRule.tagG == 34 && vacdmRule.tagB == 56 &&
			vacdmRule.hasTextColor && vacdmRule.textR == 7 && vacdmRule.textG == 8 && vacdmRule.textB == 9,
		"tag color parser preserves vACDM token, state, and channel colors",
		failures);

	VacdmColorRuleDefinition invalidVacdmRule;
	Check(
		!TryParseVacdmColorRuleToken(
			"tobt(confirmed=[tag, color='(1,2,3)'])",
			invalidVacdmRule),
		"tag color parser rejects a vACDM rule without the state prefix",
		failures);
	Check(
		!TryParseVacdmColorRuleToken(
			"tobt(state_confirmed=[tag, color='(256,2,3)'])",
			invalidVacdmRule),
		"tag color parser rejects an out-of-range channel component",
		failures);

	RunwayColorRuleDefinition runwayRule;
	Check(
		TryParseRunwayColorRuleToken(
			"DEPRWY(runway_08=[text, color='(20,30,40)'])",
			runwayRule) &&
			runwayRule.token == "deprwy" && runwayRule.expectedRunway == "08" &&
			runwayRule.hasTextColor && runwayRule.textR == 20 && runwayRule.textG == 30 && runwayRule.textB == 40,
		"tag color parser normalizes runway tokens and channel colors",
		failures);

	const std::map<std::string, std::string> matchingRunway = { { "deprwy", "RWY 08L" } };
	const VacdmColorRuleOverrides runwayOverrides = EvaluateRunwayColorRules({ runwayRule }, matchingRunway);
	Check(
		runwayOverrides.hasTextColor && runwayOverrides.textR == 20 &&
			runwayOverrides.textG == 30 && runwayOverrides.textB == 40,
		"tag color evaluator applies a matching runway prefix",
		failures);
	Check(
		!EvaluateRunwayColorRules({ runwayRule }, { { "deprwy", "26R" } }).hasTextColor,
		"tag color evaluator ignores a non-matching runway",
		failures);

	VacdmColorRuleDefinition missingVacdmRule;
	missingVacdmRule.token = "tsat";
	missingVacdmRule.expectedState = "missing";
	missingVacdmRule.hasTargetColor = true;
	missingVacdmRule.targetR = 90;
	missingVacdmRule.targetG = 80;
	missingVacdmRule.targetB = 70;
	const VacdmColorRuleOverrides missingOverrides = EvaluateVacdmColorRules({ missingVacdmRule }, nullptr);
	Check(
		missingOverrides.hasTargetColor && missingOverrides.targetR == 90 &&
			missingOverrides.targetG == 80 && missingOverrides.targetB == 70,
		"tag color evaluator applies a missing-data vACDM rule",
		failures);

	StructuredTagColorRule structuredRule;
	structuredRule.source = "custom";
	structuredRule.token = "sid";
	structuredRule.condition = "in:KIRNE,OKIPA";
	structuredRule.tagType = "departure";
	structuredRule.status = "taxi";
	structuredRule.detail = "normal";
	structuredRule.applyTag = true;
	structuredRule.tagR = 101;
	structuredRule.tagG = 102;
	structuredRule.tagB = 103;
	const VacdmColorRuleOverrides structuredOverrides = EvaluateStructuredTagColorRules(
		{ structuredRule },
		"departure",
		"taxi",
		false,
		{ { "sid", "KIRNE1A" } },
		nullptr);
	Check(
		structuredOverrides.hasTagColor && structuredOverrides.tagR == 101 &&
			structuredOverrides.tagG == 102 && structuredOverrides.tagB == 103,
		"tag color evaluator applies matching structured context and custom criteria",
		failures);
	Check(
		!EvaluateStructuredTagColorRules(
			{ structuredRule },
			"departure",
			"push",
			false,
			{ { "sid", "KIRNE1A" } },
			nullptr).hasTagColor,
		"tag color evaluator rejects a structured rule outside its status context",
		failures);

	StructuredTagColorRule vsidRule;
	vsidRule.source = "vsid";
	vsidRule.token = "vsid_sid";
	vsidRule.condition = "in:LAM,OKIPA";
	vsidRule.applyText = true;
	vsidRule.textR = 31;
	vsidRule.textG = 32;
	vsidRule.textB = 33;
	const VacdmColorRuleOverrides vsidOverrides = EvaluateStructuredTagColorRules(
		{ vsidRule },
		"departure",
		"taxi",
		false,
		{ { "vsid_sid", "LAM1X" } },
		nullptr);
	Check(
		vsidOverrides.hasTextColor && vsidOverrides.textR == 31 &&
			vsidOverrides.textG == 32 && vsidOverrides.textB == 33,
		"tag color evaluator applies matching vSID bridge criteria",
		failures);
	vsidRule.token = "vsid_cfl";
	vsidRule.condition = "in:A5";
	Check(
		!EvaluateStructuredTagColorRules(
			{ vsidRule },
			"departure",
			"taxi",
			false,
			{ { "vsid_cfl", "A50" } },
			nullptr).hasTextColor,
		"vSID cleared-level criteria require an exact list value",
		failures);

	rapidjson::Document definition;
	definition.Parse<0>("[\"callsign\",[\"deprwy\",\"scratchpad\"],42]");
	const std::vector<std::string> lines = ConvertDefinitionValueToLineTexts(definition);
	Check(
		lines.size() == 2U && lines[0] == "callsign" && lines[1] == "deprwy scratchpad",
		"tag color parser converts supported definition line shapes",
		failures);

	return failures;
}
