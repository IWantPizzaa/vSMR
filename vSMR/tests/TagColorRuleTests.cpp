#include "TagColorRuleTests.hpp"
#include "tags/TagTokenValues.hpp"

#include "tags/TagColorRules.hpp"

#include "rapidjson/document.h"

#include <map>
#include <string>

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

	bool ParseV2(const std::string& json, StructuredTagColorRule& rule)
	{
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		return !document.HasParseError() && VsmrTagColorRules::TryParseStructuredRuleV2(document, rule);
	}

	std::string RuleJson(const std::string& condition, const std::string& effects =
		R"([{"type":"tag_color","color":{"r":100,"g":101,"b":102}}])")
	{
		return "{\"when\":" + condition + ",\"effects\":" + effects + "}";
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

	const VsmrTags::TokenValues matchingRunway = { { "deprwy", "RWY 08L" } };
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
	cdmRule.token = "asrt";
	cdmRule.condition = "in:0702";
	cdmRule.applyText = true;
	cdmRule.textR = 41;
	cdmRule.textG = 42;
	cdmRule.textB = 43;
	const TagColorRuleOverrides cdmOverrides = EvaluateStructuredTagColorRules(
		{ cdmRule },
		"departure",
		"taxi",
		false,
		{ { "asrt", "0702" } },
		nullptr);
	Check(
		cdmOverrides.hasTextColor && cdmOverrides.textR == 41 &&
			cdmOverrides.textG == 42 && cdmOverrides.textB == 43,
		"tag color evaluator reads unprefixed CDM values in structured rules",
		failures);

	rapidjson::Document definition;
	definition.Parse<0>("[\"callsign\",[\"deprwy\",\"scratchpad\"],42]");
	const std::vector<std::string> lines = ConvertDefinitionValueToLineTexts(definition);
	Check(
		lines.size() == 2U && lines[0] == "callsign" && lines[1] == "deprwy scratchpad",
		"tag color parser converts supported definition line shapes",
		failures);

	const std::time_t now = 172800 + 120; // 00:02 UTC; the TOBT below can be on the previous day.
	CdmPilotData timedPilot;
	timedPilot.hasTobt = true;
	timedPilot.tobtUtc = now + 300;
	timedPilot.hasTsat = true;
	timedPilot.tsatUtc = now + 300;
	timedPilot.tobtState = "CONFIRMED";
	const VsmrTags::TokenValues rawFlight = {
		{ "rule.sid", "OPALE6B" }, { "asid", "OPALE6B" },
		{ "rule.deprwy", "26R" }, { "rule.arvrwy", "27R" },
		{ "rule.scratchpad", "READY TAXI" }, { "rule.holdingpoint", "N4" },
		{ "rule.origin", "LFPG" }, { "rule.dest", "LFBO" },
		{ "rule.actype", "A320" }, { "rule.wake", "M" },
		{ "rule.callsign", "AFR123" }, { "rule.groundstatus", "TAXI" },
		{ "rule.gs", "12" }, { "rule.flightlevel", "8" },
		{ "rule.clearance", "false" }, { "rule.vsid_sid", "OPALE6B" },
		{ "rule.vsid_rwy", "26R" }, { "rule.vsid_cfl", "A50" }
	};
	auto evaluate = [&](const StructuredTagColorRule& rule, const VsmrTags::TokenValues& values,
		const CdmPilotData* pilot = nullptr, bool detailed = false) {
		return EvaluateStructuredTagColorRules({ rule }, "departure", "taxi", detailed, values, pilot, now);
	};
	StructuredTagColorRule combined;
	Check(ParseV2(R"({"name":"TOBT and departure assignment","tag_type":"departure","statuses":["taxi","lnup"],"detail":"normal","when":{"all":[{"field":"cdm.tobt","op":"between","min":-5,"max":5},{"field":"flight.sid","op":"in","values":["OPALE6B","KIRNE1A"]},{"field":"flight.deprwy","op":"in","values":["26R","27L"]},{"field":"flight.scratchpad","op":"equals","value":"ready taxi"},{"field":"flight.holdingpoint","op":"equals","value":"N4"}]},"effects":[{"type":"target_color","color":{"r":10,"g":20,"b":30,"a":200}},{"type":"tag_color","color":{"r":40,"g":50,"b":60}},{"type":"text_color","color":{"r":70,"g":80,"b":90}},{"type":"field_color","field":"callsign","color":{"r":100,"g":110,"b":120}},{"type":"field_background","field":"holdingpoint","color":{"r":130,"g":140,"b":150,"a":180}},{"type":"field_bold","field":"callsign"},{"type":"field_blink","field":"tobt","value":true}]})", combined),
		"v2 parser compiles TOBT/SID/runway/scratchpad/holding-point conjunction with all visual effects", failures);
	const auto combinedEffects = evaluate(combined, rawFlight, &timedPilot);
	Check(combinedEffects.hasTargetColor && combinedEffects.targetR == 10 && combinedEffects.targetA == 200 &&
		combinedEffects.hasTagColor && combinedEffects.tagR == 40 && combinedEffects.hasTextColor && combinedEffects.textR == 70,
		"v2 rules apply whole-target, tag and text colors", failures);
	const auto callsignEffects = combinedEffects.fieldEffects.find("callsign");
	const auto holdingEffects = combinedEffects.fieldEffects.find("holdingpoint");
	const auto tobtEffects = combinedEffects.fieldEffects.find("tobt");
	Check(callsignEffects != combinedEffects.fieldEffects.end() && callsignEffects->second.hasColor &&
		callsignEffects->second.colorR == 100 && callsignEffects->second.hasBold && callsignEffects->second.bold &&
		holdingEffects != combinedEffects.fieldEffects.end() && holdingEffects->second.hasBackground &&
		holdingEffects->second.backgroundA == 180 && tobtEffects != combinedEffects.fieldEffects.end() && tobtEffects->second.blink,
		"v2 rules retain per-field color, background, bold and blink independently", failures);
	Check(!evaluate(combined, rawFlight, &timedPilot, true).hasTagColor,
		"v2 rule scopes distinguish normal and detailed tags", failures);
	Check(IsStructuredRuleClockSensitive(combined), "CDM conditions/blink are marked for timer refresh", failures);
	combined.enabled = false;
	Check(!evaluate(combined, rawFlight, &timedPilot).hasTagColor && !IsStructuredRuleClockSensitive(combined),
		"disabled rules do not affect rendering or trigger timed refresh", failures);
	combined.enabled = true;

	StructuredTagColorRule timeWindow;
	Check(ParseV2(RuleJson(R"({"field":"cdm.tobt","op":"between","min":-5,"max":5})"), timeWindow),
		"v2 numeric CDM window parses", failures);
	for (int delta : { -301, -300, 0, 300, 301 })
	{
		timedPilot.tobtUtc = now - delta;
		Check(evaluate(timeWindow, rawFlight, &timedPilot).hasTagColor == (delta >= -300 && delta <= 300),
			"TOBT windows use signed elapsed minutes and inclusive exact-second boundaries", failures);
	}
	timedPilot.tobtUtc = now - 240;
	Check(evaluate(timeWindow, rawFlight, &timedPilot).hasTagColor,
		"TOBT delta is correct across UTC midnight rather than comparing formatted HHMM", failures);
	Check(!evaluate(timeWindow, rawFlight, nullptr).hasTagColor,
		"missing CDM values cannot satisfy a numeric window", failures);

	StructuredTagColorRule negated;
	Check(ParseV2(RuleJson(R"({"not":{"field":"cdm.tobt","op":"between","min":-5,"max":5}})"), negated),
		"v2 NOT group parses", failures);
	Check(!evaluate(negated, rawFlight, nullptr).hasTagColor,
		"NOT does not convert unknown missing numeric data into a match", failures);
	timedPilot.tobtUtc = now - 301;
	Check(evaluate(negated, rawFlight, &timedPilot).hasTagColor, "NOT negates a known false condition", failures);

	StructuredTagColorRule alternatives;
	Check(ParseV2(RuleJson(R"({"all":[{"any":[{"field":"flight.sid","op":"equals","value":"KIRNE1A"},{"field":"flight.deprwy","op":"equals","value":"26R"}]},{"not":{"field":"flight.holdingpoint","op":"equals","value":"N5"}}]})"), alternatives),
		"nested all/any/not groups parse", failures);
	Check(evaluate(alternatives, rawFlight).hasTagColor, "all/any/not apply the expected boolean grouping", failures);

	for (const auto& test : std::map<std::string, std::string> {
		{ "flight.sid", "OPALE6B" }, { "flight.deprwy", "26R" }, { "flight.arvrwy", "27R" },
		{ "flight.origin", "LFPG" }, { "flight.destination", "LFBO" }, { "flight.actype", "A320" },
		{ "flight.wake", "M" }, { "flight.callsign", "AFR123" }, { "flight.groundstatus", "TAXI" },
		{ "flight.clearance", "false" }, { "vsid.sid", "OPALE6B" }, { "vsid.rwy", "26R" }, { "vsid.cfl", "A50" }
	})
	{
		StructuredTagColorRule field;
		const bool parsed = ParseV2(RuleJson("{\"field\":\"" + test.first + "\",\"op\":\"equals\",\"value\":\"" + test.second + "\"}"), field);
		Check(parsed && evaluate(field, rawFlight).hasTagColor, "v2 field aliases resolve canonical raw flight/bridge values", failures);
	}
	StructuredTagColorRule numeric;
	Check(ParseV2(RuleJson(R"({"field":"flight.gs","op":"gte","value":12})"), numeric) && evaluate(numeric, rawFlight).hasTagColor,
		"numeric ground speed uses captured knots", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.flightlevel","op":"equals","value":8})"), numeric) && evaluate(numeric, rawFlight).hasTagColor,
		"numeric flight level uses hundreds of feet", failures);

	StructuredTagColorRule exact;
	Check(ParseV2(RuleJson(R"({"field":"flight.sid","op":"in","values":["OPALE"]})"), exact) && !evaluate(exact, rawFlight).hasTagColor,
		"new SID lists match exact SIDs, not legacy prefixes", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.deprwy","op":"in","values":["26"]})"), exact) && !evaluate(exact, rawFlight).hasTagColor,
		"new runway lists do not implicitly match all runway suffixes", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.sid","op":"starts_with","value":"OPALE"})"), exact) && evaluate(exact, rawFlight).hasTagColor,
		"explicit starts-with supports intentional SID families", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.scratchpad","op":"equals","value":"READYTAXI"})"), exact) && !evaluate(exact, rawFlight).hasTagColor,
		"exact scratchpad matching preserves spaces rather than collapsing text", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.scratchpad","op":"contains","value":"taxi"})"), exact) && evaluate(exact, rawFlight).hasTagColor,
		"text contains matches case-insensitively", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.sid","op":"ends_with","value":"6B"})"), exact) && evaluate(exact, rawFlight).hasTagColor,
		"text ends-with matches explicit suffixes", failures);

	const VsmrTags::TokenValues missingFlight = { { "rule.scratchpad", "" }, { "scratchpad", "..." },
		{ "rule.sid", "" }, { "asid", "SID" }, { "rule.deprwy", "" }, { "deprwy", "RWY" } };
	for (const std::string& condition : {
		std::string(R"({"field":"flight.scratchpad","op":"not_equals","value":"HOLD"})"),
		std::string(R"({"field":"flight.scratchpad","op":"not_in","values":["HOLD"]})"),
		std::string(R"({"not":{"field":"flight.scratchpad","op":"equals","value":"HOLD"}})"),
		std::string(R"({"field":"flight.sid","op":"equals","value":"SID"})"),
		std::string(R"({"field":"flight.deprwy","op":"set"})") })
	{
		StructuredTagColorRule missing;
		Check(ParseV2(RuleJson(condition), missing) && !evaluate(missing, missingFlight).hasTagColor,
			"missing raw values take precedence over placeholders and cannot satisfy negative conditions", failures);
	}
	Check(ParseV2(RuleJson(R"({"field":"flight.scratchpad","op":"missing"})"), exact) && evaluate(exact, missingFlight).hasTagColor,
		"explicit missing condition matches absent raw scratchpad", failures);
	Check(ParseV2(RuleJson(R"({"field":"flight.scratchpad","op":"equals","value":"..."})"), exact) &&
		evaluate(exact, { { "rule.scratchpad", "..." } }).hasTagColor,
		"an actual literal placeholder-like scratchpad remains a legitimate exact value", failures);

	StructuredTagColorRule legacyLeaf;
	Check(ParseV2(RuleJson(R"({"field":"legacy","source":"custom","token":"asid","condition":"in:OPALE,KIRNE"})"), legacyLeaf) &&
		evaluate(legacyLeaf, rawFlight).hasTagColor, "migrated legacy SID criteria retain their prefix semantics", failures);
	Check(ParseV2(RuleJson(R"({"field":"legacy","source":"runway","token":"deprwy","condition":"26"})"), legacyLeaf) &&
		evaluate(legacyLeaf, { { "deprwy", "26R" } }).hasTagColor, "migrated legacy runway criteria retain prefix matching", failures);
	Check(ParseV2(RuleJson(R"({"field":"cdm.tsat_state","op":"equals","value":"valid"})"), exact) &&
		evaluate(exact, rawFlight, &timedPilot).hasTagColor, "CDM state fields use the injected frame clock", failures);
	timedPilot.bridgeData.manualCtot = false;
	Check(ParseV2(RuleJson(R"({"field":"cdm.manual_ctot","op":"equals","value":"false"})"), exact) &&
		evaluate(exact, rawFlight, &timedPilot).hasTagColor, "published false CDM booleans are present rather than missing", failures);

	StructuredTagColorRule first, second;
	Check(ParseV2(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"tag_color","color":{"r":1,"g":2,"b":3}},{"type":"field_bold","field":"callsign","value":true},{"type":"field_blink","field":"callsign","value":true}])"), first) &&
		ParseV2(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"tag_color","color":{"r":4,"g":5,"b":6}},{"type":"field_bold","field":"callsign","value":false},{"type":"field_blink","field":"callsign","value":false}])"), second),
		"ordered override rules parse", failures);
	auto ordered = EvaluateStructuredTagColorRules({ first, second }, "departure", "taxi", false, rawFlight, nullptr, now);
	Check(ordered.tagR == 4 && ordered.fieldEffects.at("callsign").hasBold && !ordered.fieldEffects.at("callsign").bold &&
		ordered.fieldEffects.at("callsign").hasBlink && !ordered.fieldEffects.at("callsign").blink,
		"last matching effect wins per channel and false explicitly disables bold/blink", failures);
	first.stopProcessing = true;
	ordered = EvaluateStructuredTagColorRules({ first, second }, "departure", "taxi", false, rawFlight, nullptr, now);
	Check(ordered.tagR == 1 && ordered.fieldEffects.at("callsign").bold, "matching stop-processing prevents later rule effects", failures);
	first.enabled = false;
	ordered = EvaluateStructuredTagColorRules({ first, second }, "departure", "taxi", false, rawFlight, nullptr, now);
	Check(ordered.tagR == 4, "disabled stop-processing rules do not stop later rules", failures);
	first.enabled = true;
	first.when.value = "OTHER";
	first.when.op = "equals";
	ordered = EvaluateStructuredTagColorRules({ first, second }, "departure", "taxi", false, rawFlight, nullptr, now);
	Check(ordered.tagR == 4, "non-matching stop-processing rules do not stop later rules", failures);

	TagColorRuleOverrides fallback, preferred;
	fallback.fieldEffects["callsign"].hasColor = true;
	fallback.fieldEffects["callsign"].colorR = 19;
	fallback.fieldEffects["callsign"].hasBold = true;
	fallback.fieldEffects["callsign"].bold = true;
	preferred.fieldEffects["callsign"].hasBold = true;
	preferred.fieldEffects["callsign"].bold = false;
	MergeMissingColorRuleOverrides(preferred, fallback);
	Check(preferred.fieldEffects.at("callsign").hasColor && preferred.fieldEffects.at("callsign").colorR == 19 &&
		!preferred.fieldEffects.at("callsign").bold, "detailed fallback fills only unassigned per-field effects, retaining explicit false", failures);
	StructuredTagColorRule v2Normal = second;
	v2Normal.detail = "normal";
	const auto noV2Leak = EvaluateStructuredTagColorRules({ v2Normal }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(!noV2Leak.hasTagColor && noV2Leak.fieldEffects.empty(),
		"legacy inheritance never leaks v2 normal-only background/bold/blink effects into detailed tags", failures);
	StructuredTagColorRule inheritedLegacy;
	inheritedLegacy.source = "custom";
	inheritedLegacy.token = "asid";
	inheritedLegacy.condition = "in:OPALE";
	inheritedLegacy.detail = "normal";
	inheritedLegacy.applyTag = true;
	inheritedLegacy.tagR = 51;
	inheritedLegacy.tagG = 52;
	inheritedLegacy.tagB = 53;
	inheritedLegacy.applyText = true;
	inheritedLegacy.textR = 61;
	inheritedLegacy.textG = 62;
	inheritedLegacy.textB = 63;
	Check(!EvaluateStructuredTagColorRules({ inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now).hasTagColor,
		"the default pure detailed evaluator does not add implicit inheritance", failures);
	const auto inherited = EvaluateStructuredTagColorRules({ inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(inherited.hasTagColor && inherited.tagR == 51 && inherited.hasTextColor && inherited.textR == 61,
		"explicit legacy-normal inheritance preserves earlier normal tag/text colors for detailed tags", failures);
	StructuredTagColorRule v2Detailed = second;
	v2Detailed.detail = "detailed";
	const auto detailedFirst = EvaluateStructuredTagColorRules({ v2Detailed, inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	const auto detailedLast = EvaluateStructuredTagColorRules({ inheritedLegacy, v2Detailed }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(detailedFirst.tagR == 4 && detailedLast.tagR == 4 && detailedFirst.hasTextColor && detailedFirst.textR == 61 &&
		detailedFirst.fieldEffects.at("callsign").hasBold && !detailedFirst.fieldEffects.at("callsign").bold,
		"preferred detailed effects outrank legacy fallback regardless of order while missing channels inherit", failures);
	StructuredTagColorRule detailedStop = v2Detailed;
	detailedStop.effects.erase(detailedStop.effects.begin()); // Only explicit false field effects remain.
	detailedStop.stopProcessing = true;
	const auto stoppedFallback = EvaluateStructuredTagColorRules({ detailedStop, inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(!stoppedFallback.hasTagColor && !stoppedFallback.hasTextColor && stoppedFallback.fieldEffects.count("callsign") == 1,
		"matching detailed-only stop-processing prevents subsequent legacy-normal fallback", failures);
	const auto retainedFallback = EvaluateStructuredTagColorRules({ inheritedLegacy, detailedStop }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(retainedFallback.hasTagColor && retainedFallback.tagR == 51,
		"detailed stop-processing retains legacy fallback effects matched before the stop", failures);
	StructuredTagColorRule anyStop = detailedStop;
	anyStop.detail = "any";
	Check(!EvaluateStructuredTagColorRules({ anyStop, inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true).hasTagColor,
		"any-detail stop-processing also prevents later fallback in the actual detailed pipeline", failures);
	v2Normal.stopProcessing = true;
	Check(EvaluateStructuredTagColorRules({ v2Normal, inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true).tagR == 51,
		"out-of-scope v2 normal-only stop-processing does not stop the detailed pipeline", failures);
	detailedStop.enabled = false;
	Check(EvaluateStructuredTagColorRules({ detailedStop, inheritedLegacy }, "departure", "taxi", true, rawFlight, nullptr, now, true).tagR == 51,
		"disabled detailed stop-processing does not suppress legacy fallback", failures);
	inheritedLegacy.stopProcessing = true;
	const auto legacyStopped = EvaluateStructuredTagColorRules({ inheritedLegacy, v2Detailed }, "departure", "taxi", true, rawFlight, nullptr, now, true);
	Check(legacyStopped.tagR == 51 && legacyStopped.fieldEffects.empty(),
		"matching legacy fallback stop-processing prevents later preferred effects in the same bounded pass", failures);

	rapidjson::Document serialized;
	rapidjson::Value written;
	StructuredTagColorRule roundtrip;
	timedPilot.tobtUtc = now;
	Check(WriteStructuredRuleV2(combined, written, serialized.GetAllocator()) && TryParseStructuredRuleV2(written, roundtrip) &&
		roundtrip.when.kind == "all" && roundtrip.when.children.size() == 5 && roundtrip.effects.size() == 7 &&
		evaluate(roundtrip, rawFlight, &timedPilot).fieldEffects.count("callsign") == 1,
		"native serialization roundtrips expression trees, effects and scope without flattening", failures);
	StructuredTagColorRule wildcardStatus;
	Check(ParseV2(R"({"statuses":["any","taxi"],"when":{"field":"flight.sid","op":"set"},"effects":[{"type":"text_color","color":{"r":1,"g":2,"b":3}}]})", wildcardStatus) &&
		EvaluateStructuredTagColorRules({ wildcardStatus }, "departure", "push", false, rawFlight, nullptr, now).hasTextColor,
		"any in a status list preserves wildcard scope rather than narrowing to its other entries", failures);
	wildcardStatus.statuses = { "taxi" };
	Check(!EvaluateStructuredTagColorRules({ wildcardStatus }, "departure", "push", false, rawFlight, nullptr, now).hasTextColor &&
		evaluate(wildcardStatus, rawFlight).hasTextColor,
		"an explicit single-status scope remains restricted", failures);

	StructuredTagColorRule airborneScope;
	Check(ParseV2(R"({"tag_type":"airborne","when":{"field":"flight.sid","op":"set"},"effects":[{"type":"tag_color","color":{"r":1,"g":2,"b":3}}]})", airborneScope),
		"airborne rule scope parses", failures);
	auto airborneFlight = rawFlight;
	airborneFlight["rule.airborne"] = "true";
	Check(evaluate(airborneScope, airborneFlight).hasTagColor && !evaluate(airborneScope, rawFlight).hasTagColor,
		"airborne scope uses the actual captured role rather than requiring an impossible definition type", failures);
	airborneScope.tagType = "departure";
	Check(evaluate(airborneScope, airborneFlight).hasTagColor,
		"departure scope continues to include airborne departures independently of airborne scope", failures);
	StructuredTagColorRule sharedIcon;
	Check(ParseV2(R"({"detail":"detailed","when":{"field":"flight.sid","op":"set"},"effects":[{"type":"target_color","color":{"r":81,"g":82,"b":83}},{"type":"field_bold","field":"callsign"}]})", sharedIcon),
		"detailed tag rule may also assign a shared v2 aircraft icon effect", failures);
	const auto sharedIconEffects = EvaluateStructuredTargetColorRules({ sharedIcon }, "departure", "taxi", rawFlight, nullptr, now);
	Check(sharedIconEffects.hasTargetColor && sharedIconEffects.targetR == 81 && !sharedIconEffects.hasTagColor &&
		!sharedIconEffects.hasTextColor && sharedIconEffects.fieldEffects.empty(),
		"v2 aircraft icon colors ignore tag-detail scope and return only the target channel", failures);
	Check(!evaluate(sharedIcon, rawFlight).fieldEffects.count("callsign") &&
		evaluate(sharedIcon, rawFlight, nullptr, true).fieldEffects.at("callsign").bold,
		"ignoring icon detail scope does not broaden detailed-only tag field effects", failures);
	StructuredTagColorRule legacyIcon;
	legacyIcon.source = "custom";
	legacyIcon.token = "asid";
	legacyIcon.condition = "in:OPALE";
	legacyIcon.applyTarget = true;
	legacyIcon.targetR = 91;
	legacyIcon.targetG = 92;
	legacyIcon.targetB = 93;
	legacyIcon.detail = "detailed";
	Check(!EvaluateStructuredTargetColorRules({ legacyIcon }, "departure", "taxi", rawFlight, nullptr, now).hasTargetColor,
		"legacy detailed-only target-color rules retain normal-only icon evaluation", failures);
	legacyIcon.detail = "normal";
	Check(EvaluateStructuredTargetColorRules({ legacyIcon, sharedIcon }, "departure", "taxi", rawFlight, nullptr, now).targetR == 81 &&
		EvaluateStructuredTargetColorRules({ sharedIcon, legacyIcon }, "departure", "taxi", rawFlight, nullptr, now).targetR == 91,
		"mixed v1/v2 target effects retain array ordering rather than giving v2 unconditional priority", failures);
	StructuredTagColorRule iconStopper = sharedIcon;
	iconStopper.effects.erase(iconStopper.effects.begin());
	iconStopper.stopProcessing = true;
	Check(!EvaluateStructuredTargetColorRules({ iconStopper, legacyIcon }, "departure", "taxi", rawFlight, nullptr, now).hasTargetColor,
		"matching stop-processing terminates the target pipeline even without assigning an icon effect", failures);
	iconStopper.enabled = false;
	Check(EvaluateStructuredTargetColorRules({ iconStopper, legacyIcon }, "departure", "taxi", rawFlight, nullptr, now).targetR == 91,
		"disabled stop-processing does not prevent later target colors", failures);
	sharedIcon.enabled = false;
	Check(!EvaluateStructuredTargetColorRules({ sharedIcon }, "departure", "taxi", rawFlight, nullptr, now).hasTargetColor,
		"disabled v2 icon rules never affect the shared target", failures);

	for (const std::string& bad : {
		std::string(RuleJson(R"({"all":[]})")), std::string(RuleJson(R"({"any":[]})")),
		std::string(RuleJson(R"({"all":[],"any":[]})")), std::string(RuleJson(R"({"not":[]})")),
		std::string(RuleJson(R"({"field":"flight.unknown","op":"set"})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"regex","value":".*"})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"in","values":[]})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"in","values":[1]})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"gte","value":1})")),
		std::string(RuleJson(R"({"field":"flight.gs","op":"gte","value":"12"})")),
		std::string(RuleJson(R"({"field":"flight.gs","op":"between","min":10,"max":-10})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set","value":"ignored"})")),
		std::string(RuleJson(R"({"field":"flight.sid","field":"flight.gs","op":"set"})")),
		std::string(RuleJson(R"({"field":"legacy","source":"typo","token":"tobt","condition":"missing"})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"equals","value":"X\u0000Y"})")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", "[]")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"write_sid","value":true}])")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"field_bold","field":"unknown"}])")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"field_blink","field":"callsign","value":"true"}])")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"tag_color","color":{"r":256,"g":2,"b":3}}])")),
		std::string(RuleJson(R"({"field":"flight.sid","op":"set"})", R"([{"type":"tag_color","color":{"r":1,"g":2,"b":3,"a":-1}}])")) })
	{
		StructuredTagColorRule invalid;
		Check(!ParseV2(bad, invalid) && !invalid.usesStructuredCondition && invalid.effects.empty(),
			"malformed v2 rules are rejected atomically rather than dropping conditions/effects", failures);
	}
	std::string deep = R"({"field":"flight.sid","op":"set"})";
	for (int level = 0; level < 8; ++level) deep = "{\"not\":" + deep + "}";
	Check(!ParseV2(RuleJson(deep), exact), "condition depth beyond eight levels is rejected", failures);
	std::string wide = "{\"all\":[";
	for (int index = 0; index < 128; ++index) wide += (index ? "," : "") + std::string(R"({"field":"flight.sid","op":"set"})");
	wide += "]}";
	Check(!ParseV2(RuleJson(wide), exact), "condition node count includes groups and rejects more than 128 nodes", failures);
	std::string tooManyEffects = "[";
	for (int index = 0; index < 33; ++index) tooManyEffects += (index ? "," : "") + std::string(R"({"type":"field_bold","field":"callsign"})");
	tooManyEffects += "]";
	Check(!ParseV2(RuleJson(R"({"field":"flight.sid","op":"set"})", tooManyEffects), exact),
		"more than 32 effects is rejected", failures);
	std::string tooManyValues = "{\"field\":\"flight.sid\",\"op\":\"in\",\"values\":[";
	for (int index = 0; index < 129; ++index) tooManyValues += (index ? "," : "") + std::string("\"OPALE6B\"");
	tooManyValues += "]}";
	Check(!ParseV2(RuleJson(tooManyValues), exact), "more than 128 list values is rejected", failures);
	StructuredTagColorRule bounded;
	std::string acceptedDepth = R"({"field":"flight.sid","op":"set"})";
	for (int level = 0; level < 7; ++level) acceptedDepth = "{\"not\":" + acceptedDepth + "}";
	Check(ParseV2(RuleJson(acceptedDepth), bounded), "exactly eight nesting levels remains accepted", failures);
	std::string acceptedNodes = "{\"all\":[";
	for (int index = 0; index < 127; ++index) acceptedNodes += (index ? "," : "") + std::string(R"({"field":"flight.sid","op":"set"})");
	acceptedNodes += "]}";
	Check(ParseV2(RuleJson(acceptedNodes), bounded) && evaluate(bounded, rawFlight).hasTagColor,
		"exactly 128 total nodes remains accepted and evaluates normally", failures);
	std::string acceptedEffects = "[";
	for (int index = 0; index < 32; ++index) acceptedEffects += (index ? "," : "") + std::string(R"({"type":"field_bold","field":"callsign"})");
	acceptedEffects += "]";
	Check(ParseV2(RuleJson(R"({"field":"flight.sid","op":"set"})", acceptedEffects), bounded),
		"exactly 32 effects remains accepted", failures);
	std::string acceptedValues = "{\"field\":\"flight.sid\",\"op\":\"in\",\"values\":[";
	for (int index = 0; index < 128; ++index) acceptedValues += (index ? "," : "") + std::string("\"OPALE6B\"");
	acceptedValues += "]}";
	Check(ParseV2(RuleJson(acceptedValues), bounded) && evaluate(bounded, rawFlight).hasTagColor,
		"exactly 128 list values remains accepted", failures);
	Check(!ParseV2(RuleJson("{\"field\":\"flight.scratchpad\",\"op\":\"equals\",\"value\":\"" + std::string(513, 'X') + "\"}"), bounded),
		"condition strings over 512 bytes are rejected", failures);
	Check(!ParseV2(RuleJson(R"({"field":"flight.scratchpad","op":"equals","value":" \t "})"), bounded),
		"control characters and whitespace-only comparison strings are rejected", failures);
	Check(!ParseV2(R"({"enabled":"false","when":{"field":"flight.sid","op":"set"},"effects":[{"type":"field_bold","field":"callsign"}]})", bounded),
		"rule boolean metadata cannot be coerced from strings", failures);
	Check(!ParseV2(R"({"tag_type":"typo","when":{"field":"flight.sid","op":"set"},"effects":[{"type":"field_bold","field":"callsign"}]})", bounded),
		"unknown scope is rejected rather than converted into any", failures);
	Check(!ParseV2(RuleJson(R"({"all":[{"field":"flight.sid","op":"set"},{"field":"unknown","op":"set"}]})"), bounded),
		"one invalid child rejects the whole rule rather than broadening a conjunction", failures);

	return failures;
}
