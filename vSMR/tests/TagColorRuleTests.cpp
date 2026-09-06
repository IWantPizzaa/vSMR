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
	CdmColorRuleDefinition legacyCdmRule;
	Check(
		TryParseCdmColorRuleToken(
			"TOBT(state_confirmed=[target, tag, color='(12,34,56)', color_text='(7,8,9)'])",
			legacyCdmRule) &&
			legacyCdmRule.token == "tobt" && legacyCdmRule.expectedState == "confirmed" &&
			legacyCdmRule.hasTargetColor && legacyCdmRule.targetR == 12 && legacyCdmRule.targetG == 34 && legacyCdmRule.targetB == 56 &&
			legacyCdmRule.hasTagColor && legacyCdmRule.tagR == 12 && legacyCdmRule.tagG == 34 && legacyCdmRule.tagB == 56 &&
			legacyCdmRule.hasTextColor && legacyCdmRule.textR == 7 && legacyCdmRule.textG == 8 && legacyCdmRule.textB == 9,
		"legacy tag color parser preserves CDM token, state, and channel colors",
		failures);

	CdmColorRuleDefinition invalidCdmRule;
	Check(
		!TryParseCdmColorRuleToken(
			"tobt(confirmed=[tag, color='(1,2,3)'])",
			invalidCdmRule),
		"legacy tag color parser rejects a CDM rule without the state prefix",
		failures);
	Check(
		!TryParseCdmColorRuleToken(
			"tobt(state_confirmed=[tag, color='(256,2,3)'])",
			invalidCdmRule),
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
	const TagColorRuleOverrides runwayOverrides = EvaluateRunwayColorRules({ runwayRule }, matchingRunway);
	Check(
		runwayOverrides.hasTextColor && runwayOverrides.textR == 20 &&
			runwayOverrides.textG == 30 && runwayOverrides.textB == 40,
		"tag color evaluator applies a matching runway prefix",
		failures);
	Check(
		!EvaluateRunwayColorRules({ runwayRule }, { { "deprwy", "26R" } }).hasTextColor,
		"tag color evaluator ignores a non-matching runway",
		failures);

	CdmColorRuleDefinition missingCdmRule;
	missingCdmRule.token = "tsat";
	missingCdmRule.expectedState = "missing";
	missingCdmRule.hasTargetColor = true;
	missingCdmRule.targetR = 90;
	missingCdmRule.targetG = 80;
	missingCdmRule.targetB = 70;
	const TagColorRuleOverrides missingOverrides = EvaluateCdmColorRules({ missingCdmRule }, nullptr);
	Check(
		missingOverrides.hasTargetColor && missingOverrides.targetR == 90 &&
			missingOverrides.targetG == 80 && missingOverrides.targetB == 70,
		"tag color evaluator applies a missing-data CDM rule",
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
	const TagColorRuleOverrides structuredOverrides = EvaluateStructuredTagColorRules(
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
	const TagColorRuleOverrides vsidOverrides = EvaluateStructuredTagColorRules(
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

	StructuredTagColorRule cdmRule;
	cdmRule.source = "cdm";
	cdmRule.token = "deice";
	cdmRule.condition = "in:REMOTE,STAND";
	cdmRule.applyText = true;
	cdmRule.textR = 41;
	cdmRule.textG = 42;
	cdmRule.textB = 43;
	const TagColorRuleOverrides cdmOverrides = EvaluateStructuredTagColorRules(
		{ cdmRule },
		"departure",
		"taxi",
		false,
		{ { "cdm_deice", "REMOTE" } },
		nullptr);
	Check(
		cdmOverrides.hasTextColor && cdmOverrides.textR == 41 &&
			cdmOverrides.textG == 42 && cdmOverrides.textB == 43,
		"tag color evaluator exposes CDM bridge strings to structured rules",
		failures);
	cdmRule.token = "manual_ctot";
	cdmRule.condition = "in:true";
	Check(
		EvaluateStructuredTagColorRules(
			{ cdmRule },
			"departure",
			"taxi",
			false,
			{ { "cdm_manual_ctot", "true" } },
			nullptr).hasTextColor,
		"tag color evaluator exposes CDM bridge booleans to structured rules",
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
