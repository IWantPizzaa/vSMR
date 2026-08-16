#pragma once

#include "config/RuntimeConfig.hpp"
#include "platform/windows/GdiColorUtils.hpp"
#include "tags/TagDataTypes.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "shared/TextUtils.hpp"
#include "rapidjson/document.h"

#include <array>
#include <cctype>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace
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

	bool IsVacdmRuleTokenName(const std::string& tokenName)
	{
		const std::string lowered = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(tokenName));
		return lowered == "tobt" ||
			lowered == "tsat" ||
			lowered == "ttot" ||
			lowered == "asat" ||
			lowered == "aobt" ||
			lowered == "atot" ||
			lowered == "asrt" ||
			lowered == "aort" ||
			lowered == "ctot";
	}

	std::vector<std::string> SplitCommaSeparatedItems(const std::string& text)
	{
		std::vector<std::string> items;
		std::string current;
		char quote = '\0';

		auto flushItem = [&]()
		{
			const std::string trimmed = TrimAsciiWhitespaceCopy(current);
			if (!trimmed.empty())
				items.push_back(trimmed);
			current.clear();
		};

		for (char ch : text)
		{
			if ((ch == '"' || ch == '\''))
			{
				if (quote == '\0')
					quote = ch;
				else if (quote == ch)
					quote = '\0';
				current.push_back(ch);
				continue;
			}

			if (ch == ',' && quote == '\0')
			{
				flushItem();
				continue;
			}

			current.push_back(ch);
		}

		flushItem();
		return items;
	}

	std::string StripWrappingQuotes(const std::string& value)
	{
		std::string trimmed = TrimAsciiWhitespaceCopy(value);
		if (trimmed.size() >= 2)
		{
			char first = trimmed.front();
			char last = trimmed.back();
			if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
				return trimmed.substr(1, trimmed.size() - 2);
		}
		return trimmed;
	}

	bool TryParseVacdmRuleRgb(const std::string& value, int& outR, int& outG, int& outB)
	{
		std::string content = StripWrappingQuotes(value);
		const std::vector<int> values = ExtractIntegers(content);
		if (values.size() != 3)
			return false;
		for (int component : values)
		{
			if (component < 0 || component > 255)
				return false;
		}
		outR = values[0];
		outG = values[1];
		outB = values[2];
		return true;
	}

	void ApplyColorRuleChannels(VacdmColorRuleOverrides& target, const ColorRuleChannels& source)
	{
		if (source.hasTargetColor)
		{
			target.hasTargetColor = true;
			target.targetR = source.targetR;
			target.targetG = source.targetG;
			target.targetB = source.targetB;
			target.targetA = source.targetA;
		}
		if (source.hasTagColor)
		{
			target.hasTagColor = true;
			target.tagR = source.tagR;
			target.tagG = source.tagG;
			target.tagB = source.tagB;
			target.tagA = source.tagA;
		}
		if (source.hasTextColor)
		{
			target.hasTextColor = true;
			target.textR = source.textR;
			target.textG = source.textG;
			target.textB = source.textB;
			target.textA = source.textA;
		}
	}

	void ApplyStructuredRuleColors(VacdmColorRuleOverrides& target, const StructuredTagColorRule& rule)
	{
		if (rule.applyTarget)
		{
			target.hasTargetColor = true;
			target.targetR = rule.targetR;
			target.targetG = rule.targetG;
			target.targetB = rule.targetB;
			target.targetA = rule.targetA;
		}
		if (rule.applyTag)
		{
			target.hasTagColor = true;
			target.tagR = rule.tagR;
			target.tagG = rule.tagG;
			target.tagB = rule.tagB;
			target.tagA = rule.tagA;
		}
		if (rule.applyText)
		{
			target.hasTextColor = true;
			target.textR = rule.textR;
			target.textG = rule.textG;
			target.textB = rule.textB;
			target.textA = rule.textA;
		}
	}

	void MergeColorRuleOverrides(VacdmColorRuleOverrides& target, const VacdmColorRuleOverrides& source)
	{
		ApplyColorRuleChannels(target, source);
	}

	void MergeMissingColorRuleOverrides(VacdmColorRuleOverrides& target, const VacdmColorRuleOverrides& fallback)
	{
		if (!target.hasTargetColor && fallback.hasTargetColor)
		{
			target.hasTargetColor = true;
			target.targetR = fallback.targetR;
			target.targetG = fallback.targetG;
			target.targetB = fallback.targetB;
			target.targetA = fallback.targetA;
		}
		if (!target.hasTagColor && fallback.hasTagColor)
		{
			target.hasTagColor = true;
			target.tagR = fallback.tagR;
			target.tagG = fallback.tagG;
			target.tagB = fallback.tagB;
			target.tagA = fallback.tagA;
		}
		if (!target.hasTextColor && fallback.hasTextColor)
		{
			target.hasTextColor = true;
			target.textR = fallback.textR;
			target.textG = fallback.textG;
			target.textB = fallback.textB;
			target.textA = fallback.textA;
		}
	}

	bool TrySplitColorRuleToken(const std::string& rawToken, std::string& outBaseToken, std::string& outCondition, std::string& outPayload)
	{
		outBaseToken.clear();
		outCondition.clear();
		outPayload.clear();

		const std::string token = TrimAsciiWhitespaceCopy(rawToken);
		if (token.empty())
			return false;

		const size_t openPos = token.find('(');
		const size_t closePos = token.rfind(')');
		if (openPos == std::string::npos || closePos == std::string::npos || closePos <= openPos || closePos != token.size() - 1)
			return false;

		outBaseToken = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(token.substr(0, openPos)));
		if (outBaseToken.empty())
			return false;

		const std::string expression = TrimAsciiWhitespaceCopy(token.substr(openPos + 1, closePos - openPos - 1));
		const size_t eqPos = expression.find('=');
		if (eqPos == std::string::npos)
			return false;

		outCondition = TrimAsciiWhitespaceCopy(expression.substr(0, eqPos));
		outPayload = TrimAsciiWhitespaceCopy(expression.substr(eqPos + 1));
		if (outCondition.empty() || outPayload.size() < 2 || outPayload.front() != '[' || outPayload.back() != ']')
			return false;

		outPayload = TrimAsciiWhitespaceCopy(outPayload.substr(1, outPayload.size() - 2));
		return !outPayload.empty();
	}

	bool TryParseColorRuleChannels(const std::string& payload, ColorRuleChannels& outChannels)
	{
		outChannels = ColorRuleChannels();

		bool scopeTargetRequested = false;
		bool scopeTagRequested = false;
		bool scopeTextRequested = false;
		bool hasSharedColor = false;
		int sharedR = 255;
		int sharedG = 255;
		int sharedB = 255;

		const std::vector<std::string> items = SplitCommaSeparatedItems(payload);
		for (const std::string& itemRaw : items)
		{
			const std::string item = TrimAsciiWhitespaceCopy(itemRaw);
			if (item.empty())
				continue;

			const std::string loweredItem = ToLowerAsciiCopy(item);
			if (loweredItem == "target")
			{
				scopeTargetRequested = true;
				continue;
			}
			if (loweredItem == "tag")
			{
				scopeTagRequested = true;
				continue;
			}
			if (loweredItem == "text")
			{
				scopeTextRequested = true;
				continue;
			}

			const size_t keyEqPos = item.find('=');
			if (keyEqPos == std::string::npos)
				continue;

			const std::string key = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(item.substr(0, keyEqPos)));
			const std::string value = TrimAsciiWhitespaceCopy(item.substr(keyEqPos + 1));

			int r = 0;
			int g = 0;
			int b = 0;
			if (!TryParseVacdmRuleRgb(value, r, g, b))
				continue;

			if (key == "color")
			{
				hasSharedColor = true;
				sharedR = r;
				sharedG = g;
				sharedB = b;
				continue;
			}
			if (key == "color_target")
			{
				scopeTargetRequested = true;
				outChannels.hasTargetColor = true;
				outChannels.targetR = r;
				outChannels.targetG = g;
				outChannels.targetB = b;
				continue;
			}
			if (key == "color_tag")
			{
				scopeTagRequested = true;
				outChannels.hasTagColor = true;
				outChannels.tagR = r;
				outChannels.tagG = g;
				outChannels.tagB = b;
				continue;
			}
			if (key == "color_text")
			{
				scopeTextRequested = true;
				outChannels.hasTextColor = true;
				outChannels.textR = r;
				outChannels.textG = g;
				outChannels.textB = b;
				continue;
			}
		}

		if (scopeTargetRequested && !outChannels.hasTargetColor && hasSharedColor)
		{
			outChannels.hasTargetColor = true;
			outChannels.targetR = sharedR;
			outChannels.targetG = sharedG;
			outChannels.targetB = sharedB;
		}
		if (scopeTagRequested && !outChannels.hasTagColor && hasSharedColor)
		{
			outChannels.hasTagColor = true;
			outChannels.tagR = sharedR;
			outChannels.tagG = sharedG;
			outChannels.tagB = sharedB;
		}
		if (scopeTextRequested && !outChannels.hasTextColor && hasSharedColor)
		{
			outChannels.hasTextColor = true;
			outChannels.textR = sharedR;
			outChannels.textG = sharedG;
			outChannels.textB = sharedB;
		}

		if (scopeTargetRequested && !outChannels.hasTargetColor)
			return false;
		if (scopeTagRequested && !outChannels.hasTagColor)
			return false;
		if (scopeTextRequested && !outChannels.hasTextColor)
			return false;
		return outChannels.hasTargetColor || outChannels.hasTagColor || outChannels.hasTextColor;
	}

	// Parse color-rule token syntax:
	// token(state_<name>=[target|tag|text, color=(r,g,b), color_target=(r,g,b), ...]).
	bool TryParseVacdmColorRuleToken(const std::string& rawToken, VacdmColorRuleDefinition& outRule)
	{
		outRule = VacdmColorRuleDefinition();

		std::string baseToken;
		std::string condition;
		std::string payload;
		if (!TrySplitColorRuleToken(rawToken, baseToken, condition, payload))
			return false;
		if (!IsVacdmRuleTokenName(baseToken))
			return false;

		std::string stateName = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(condition));
		if (stateName.rfind("state_", 0) != 0 || stateName.size() <= 6)
			return false;
		stateName = stateName.substr(6);
		if (stateName.empty())
			return false;

		if (!TryParseColorRuleChannels(payload, outRule))
			return false;

		outRule.token = baseToken;
		outRule.expectedState = stateName;
		return true;
	}

	bool TryGetVacdmRuleTokenValue(const VacdmPilotData& pilot, const std::string& token, std::time_t& outTime, bool& outHas)
	{
		const std::string lowered = ToLowerAsciiCopy(token);
		outTime = 0;
		outHas = false;
		if (lowered == "tobt")
		{
			outTime = pilot.tobtUtc;
			outHas = pilot.hasTobt;
			return true;
		}
		if (lowered == "tsat")
		{
			outTime = pilot.tsatUtc;
			outHas = pilot.hasTsat;
			return true;
		}
		if (lowered == "ttot")
		{
			outTime = pilot.ttotUtc;
			outHas = pilot.hasTtot;
			return true;
		}
		if (lowered == "asat")
		{
			outTime = pilot.asatUtc;
			outHas = pilot.hasAsat;
			return true;
		}
		if (lowered == "aobt")
		{
			outTime = pilot.aobtUtc;
			outHas = pilot.hasAobt;
			return true;
		}
		if (lowered == "atot")
		{
			outTime = pilot.atotUtc;
			outHas = pilot.hasAtot;
			return true;
		}
		if (lowered == "asrt")
		{
			outTime = pilot.asrtUtc;
			outHas = pilot.hasAsrt;
			return true;
		}
		if (lowered == "aort")
		{
			outTime = pilot.aortUtc;
			outHas = pilot.hasAort;
			return true;
		}
		if (lowered == "ctot")
		{
			outTime = pilot.ctotUtc;
			outHas = pilot.hasCtot;
			return true;
		}
		return false;
	}

	// Resolve a canonical runtime state name for a vACDM token.
	// These states are consumed by both legacy inline rules and structured rules.
	std::string ResolveVacdmRuleStateName(const std::string& token, const VacdmPilotData* pilotData)
	{
		const std::string lowered = ToLowerAsciiCopy(token);
		if (pilotData == nullptr)
			return "missing";

		const VacdmPilotData& pilot = *pilotData;
		if (lowered == "tobt")
		{
			if (!pilot.hasTobt)
				return "missing";
			if (!pilot.hasTsat || pilot.hasAsat)
				return "inactive";

			const std::time_t now = std::time(nullptr);
			const long long timeSinceTobt = static_cast<long long>(std::difftime(now, pilot.tobtUtc));
			const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));
			const long long diffTsatTobt = static_cast<long long>(std::difftime(pilot.tsatUtc, pilot.tobtUtc));
			const std::string tobtState = ToUpperAsciiCopy(pilot.tobtState);

			if ((timeSinceTobt > 0 && (timeSinceTsat >= 5 * 60 || !pilot.hasTsat)) || pilot.tobtUtc >= now + 60 * 60)
				return "expired";
			if (diffTsatTobt >= 5 * 60 && (tobtState == "GUESS" || tobtState == "FLIGHTPLAN"))
				return "unconfirmed_delay";
			if (diffTsatTobt >= 5 * 60 && tobtState == "CONFIRMED")
				return "confirmed_delay";
			if (diffTsatTobt < 5 * 60 && tobtState == "CONFIRMED")
				return "confirmed";
			if (tobtState != "CONFIRMED")
				return "unconfirmed";
			return "unknown";
		}

		if (lowered == "tsat")
		{
			if (!pilot.hasTsat)
				return "missing";
			if (pilot.hasAsat)
				return "inactive";

			const std::time_t now = std::time(nullptr);
			const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));

			if (timeSinceTsat <= 5 * 60 && timeSinceTsat >= -5 * 60)
				return pilot.hasCtot ? "valid_ctot" : "valid";
			if (timeSinceTsat < -5 * 60)
				return pilot.hasCtot ? "future_ctot" : "future";
			if (timeSinceTsat > 5 * 60)
				return pilot.hasCtot ? "expired_ctot" : "expired";
			return "unknown";
		}

		std::time_t tokenTime = 0;
		bool hasToken = false;
		if (!TryGetVacdmRuleTokenValue(pilot, lowered, tokenTime, hasToken) || !hasToken)
			return "missing";
		if (tokenTime <= 0)
			return "missing";

		const std::time_t now = std::time(nullptr);
		const long long deltaSeconds = static_cast<long long>(std::difftime(tokenTime, now));
		return deltaSeconds >= 0 ? "future" : "past";
	}

	std::string NormalizeVacdmStateName(const std::string& rawState)
	{
		std::string normalized = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(rawState));
		if (normalized.rfind("state_", 0) == 0)
			normalized = normalized.substr(6);
		for (char& ch : normalized)
		{
			if (ch == ' ' || ch == '-')
				ch = '_';
		}
		return normalized;
	}

	// Map aliases and legacy labels to one canonical set so profile rules remain backward-compatible.
	std::string CanonicalVacdmStateName(const std::string& rawState)
	{
		const std::string state = NormalizeVacdmStateName(rawState);
		if (state.empty())
			return "";

		if (state == "any" || state == "*")
			return "any";
		if (state == "set" || state == "present" || state == "available")
			return "set";
		if (state == "missing" || state == "unset" || state == "none" || state == "empty")
			return "missing";
		if (state == "active")
			return "active";
		if (state == "inactive" || state == "grey" || state == "gray")
			return "inactive";

		if (state == "confirmed_no_delay" || state == "confirmed_without_delay" || state == "confirmed_tobt_without_startup_delay" || state == "green")
			return "confirmed";
		if (state == "unconfirmed_no_delay" || state == "unconfirmed_without_delay" || state == "unconfirmed_tobt_without_startup_delay" || state == "light_green" || state == "lightgreen")
			return "unconfirmed";
		if (state == "confirmed_with_delay" || state == "confirmed_tobt_with_startup_delay" || state == "yellow")
			return "confirmed_delay";
		if (state == "unconfirmed_with_delay" || state == "unconfirmed_tobt_with_startup_delay" || state == "light_yellow" || state == "lightyellow")
			return "unconfirmed_delay";

		if (state == "valid_tsat")
			return "valid";
		if (state == "valid_slot" || state == "valid_ctot" || state == "blue")
			return "valid_ctot";
		if (state == "future_not_valid")
			return "future";
		if (state == "future_slot" || state == "future_ctot" || state == "light_blue" || state == "lightblue")
			return "future_ctot";
		if (state == "expired_slot" || state == "expired_ctot" || state == "red")
			return "expired_ctot";
		if (state == "orange")
			return "expired";
		if (state == "done")
			return "past";
		if (state == "pending")
			return "future";

		return state;
	}

	// Evaluate profile rule predicates against canonical state names.
	bool VacdmRuleStateMatches(const std::string& expectedStateRaw, const std::string& actualStateRaw)
	{
		const std::string expected = CanonicalVacdmStateName(expectedStateRaw);
		const std::string actual = CanonicalVacdmStateName(actualStateRaw);
		if (expected.empty())
			return false;
		if (expected == "any")
			return true;
		if (expected == actual)
			return true;
		if (expected == "set")
			return actual != "missing" && actual != "unknown";
		if (expected == "active")
			return actual != "missing" && actual != "inactive" && actual != "unknown";
		if (expected == "future")
			return actual == "future" || actual == "future_ctot";
		if (expected == "valid")
			return actual == "valid" || actual == "valid_ctot";
		if (expected == "expired")
			return actual == "expired" || actual == "expired_ctot" || actual == "past";
		if (expected == "past")
			return actual == "past" || actual == "expired" || actual == "expired_ctot";
		if (expected == "ctot_linked")
			return actual.find("_ctot") != std::string::npos;
		if (expected == "not_ctot")
			return actual.find("_ctot") == std::string::npos;
		return false;
	}

	template <typename RuleType, typename Parser>
	void CollectColorRulesFromLineTexts(const std::vector<std::string>& lineTexts, std::vector<RuleType>& outRules, Parser parser)
	{
		for (const std::string& line : lineTexts)
		{
			const std::vector<std::string> tokens = SplitDefinitionTokens(line);
			for (const std::string& rawToken : tokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;
				RuleType parsedRule;
				if (parser(baseToken, parsedRule))
					outRules.push_back(parsedRule);
			}
		}
	}

	template <typename RuleType, typename Matcher>
	VacdmColorRuleOverrides EvaluateColorRules(const std::vector<RuleType>& rules, Matcher matcher)
	{
		VacdmColorRuleOverrides overrides;
		for (const RuleType& rule : rules)
		{
			if (matcher(rule))
				ApplyColorRuleChannels(overrides, rule);
		}
		return overrides;
	}

	// Collect only vACDM color-rule tokens from tag definition lines.
	void CollectVacdmColorRulesFromLineTexts(const std::vector<std::string>& lineTexts, std::vector<VacdmColorRuleDefinition>& outRules)
	{
		CollectColorRulesFromLineTexts(lineTexts, outRules, TryParseVacdmColorRuleToken);
	}

	std::vector<std::string> ConvertDefinitionValueToLineTexts(const rapidjson::Value& labelLines)
	{
		std::vector<std::string> lines;
		if (!labelLines.IsArray())
			return lines;

		for (rapidjson::SizeType i = 0; i < labelLines.Size(); ++i)
		{
			const rapidjson::Value& line = labelLines[i];
			if (line.IsString())
			{
				lines.push_back(line.GetString());
				continue;
			}

			if (!line.IsArray())
				continue;

			std::string joined;
			for (rapidjson::SizeType j = 0; j < line.Size(); ++j)
			{
				if (!line[j].IsString())
					continue;
				if (!joined.empty())
					joined.append(" ");
				joined.append(line[j].GetString());
			}
			lines.push_back(joined);
		}

		return lines;
	}

	VacdmColorRuleOverrides EvaluateVacdmColorRules(const std::vector<VacdmColorRuleDefinition>& rules, const VacdmPilotData* pilotData)
	{
		return EvaluateColorRules(rules, [&](const VacdmColorRuleDefinition& rule) {
			const std::string actualState = ResolveVacdmRuleStateName(rule.token, pilotData);
			return VacdmRuleStateMatches(rule.expectedState, actualState);
			});
	}

	std::string NormalizeSidMatchText(const std::string& value)
	{
		std::string normalized;
		normalized.reserve(value.size());
		for (char ch : value)
		{
			if (ch == ' ' || ch == '-' || ch == '_')
				continue;

			normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
		}
		return normalized;
	}

	std::string NormalizeRunwayMatchText(const std::string& value)
	{
		std::string normalized = NormalizeSidMatchText(value);
		if (normalized.rfind("RWY", 0) == 0)
			normalized = normalized.substr(3);
		return normalized;
	}

	bool IsRunwayRuleTokenName(const std::string& tokenName)
	{
		const std::string lowered = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(tokenName));
		return lowered == "deprwy" ||
			lowered == "seprwy" ||
			lowered == "arvrwy" ||
			lowered == "srvrwy";
	}

	std::string NormalizeRunwayRuleConditionName(const std::string& rawCondition)
	{
		std::string normalized = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(rawCondition));
		if (normalized.rfind("runway_", 0) == 0)
			normalized = normalized.substr(7);
		else if (normalized.rfind("rwy_", 0) == 0)
			normalized = normalized.substr(4);
		else if (normalized.rfind("value_", 0) == 0)
			normalized = normalized.substr(6);
		else if (normalized.rfind("match_", 0) == 0)
			normalized = normalized.substr(6);
		return TrimAsciiWhitespaceCopy(normalized);
	}

	bool TryParseRunwayColorRuleToken(const std::string& rawToken, RunwayColorRuleDefinition& outRule)
	{
		outRule = RunwayColorRuleDefinition();

		std::string baseToken;
		std::string condition;
		std::string payload;
		if (!TrySplitColorRuleToken(rawToken, baseToken, condition, payload))
			return false;
		if (!IsRunwayRuleTokenName(baseToken))
			return false;

		const std::string runwayCondition = NormalizeRunwayRuleConditionName(condition);
		if (runwayCondition.empty())
			return false;

		if (!TryParseColorRuleChannels(payload, outRule))
			return false;

		outRule.token = baseToken;
		outRule.expectedRunway = runwayCondition;
		return true;
	}

	bool RunwayRuleConditionMatches(const std::string& expectedConditionRaw, const std::string& actualRunwayRaw)
	{
		const std::string actualRunwayNormalized = NormalizeRunwayMatchText(actualRunwayRaw);
		auto matchesSingleCondition = [&](const std::string& conditionRaw) -> bool
		{
			const std::string expectedCondition = NormalizeRunwayRuleConditionName(conditionRaw);
			const std::string expectedLower = ToLowerAsciiCopy(expectedCondition);

			if (expectedLower == "any" || expectedLower == "*" || expectedLower == "all")
				return !actualRunwayNormalized.empty();
			if (expectedLower == "set" || expectedLower == "present" || expectedLower == "available")
				return !actualRunwayNormalized.empty();
			if (expectedLower == "missing" || expectedLower == "unset" || expectedLower == "none" || expectedLower == "empty")
				return actualRunwayNormalized.empty();

			std::string expectedRunwayNormalized = NormalizeRunwayMatchText(expectedCondition);
			if (expectedRunwayNormalized.empty() || actualRunwayNormalized.empty())
				return false;

			if (actualRunwayNormalized == expectedRunwayNormalized)
				return true;

			if (actualRunwayNormalized.size() >= expectedRunwayNormalized.size() &&
				actualRunwayNormalized.compare(0, expectedRunwayNormalized.size(), expectedRunwayNormalized) == 0)
			{
				return true;
			}

			return false;
		};

		if (expectedConditionRaw.find(',') == std::string::npos &&
			expectedConditionRaw.find(';') == std::string::npos &&
			expectedConditionRaw.find('|') == std::string::npos)
		{
			return matchesSingleCondition(expectedConditionRaw);
		}

		std::string token;
		bool hasToken = false;
		for (size_t i = 0; i <= expectedConditionRaw.size(); ++i)
		{
			const char ch = (i < expectedConditionRaw.size()) ? expectedConditionRaw[i] : ',';
			if (ch == ',' || ch == ';' || ch == '|')
			{
				const std::string condition = NormalizeRunwayRuleConditionName(token);
				token.clear();
				if (condition.empty())
					continue;
				hasToken = true;
				if (matchesSingleCondition(condition))
					return true;
				continue;
			}

			token.push_back(ch);
		}

		return !hasToken ? matchesSingleCondition(expectedConditionRaw) : false;
	}

	bool StructuredRuleContextMatches(const StructuredTagColorRule& rule, const std::string& tagTypeKey, const std::string& statusKey, const std::string& detailKey)
	{
		auto matchesField = [](const std::string& value, const std::string& current) -> bool
		{
			const std::string normalized = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(value));
			if (normalized.empty() || normalized == "any" || normalized == "all" || normalized == "*")
				return true;
			return normalized == ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(current));
		};

		bool statusMatches = rule.statuses.empty()
			? matchesField(rule.status, statusKey)
			: false;
		for (const std::string& status : rule.statuses)
		{
			if (matchesField(status, statusKey))
			{
				statusMatches = true;
				break;
			}
		}

		return matchesField(rule.tagType, tagTypeKey) &&
			statusMatches &&
			matchesField(rule.detail, detailKey);
	}

	bool StructuredRuleContextMatches(const StructuredTagColorRule& rule, const std::string& tagTypeKey, const char* statusDefinitionKey, bool isTagDetailed)
	{
		const std::string statusKey = statusDefinitionKey != nullptr ? statusDefinitionKey : "default";
		const std::string detailKey = isTagDetailed ? "detailed" : "normal";
		return StructuredRuleContextMatches(rule, tagTypeKey, statusKey, detailKey);
	}

	bool CustomRuleConditionMatches(const std::string& expectedConditionRaw, const std::string& actualValueRaw)
	{
		const std::string actualNormalized = NormalizeSidMatchText(actualValueRaw);
		const std::string expectedTrimmed = TrimAsciiWhitespaceCopy(expectedConditionRaw);
		const std::string expectedLower = ToLowerAsciiCopy(expectedTrimmed);

		if (expectedLower.empty() || expectedLower == "any" || expectedLower == "*" || expectedLower == "all")
			return !actualNormalized.empty();
		if (expectedLower == "set" || expectedLower == "present" || expectedLower == "available")
			return !actualNormalized.empty();
		if (expectedLower == "missing" || expectedLower == "unset" || expectedLower == "none" || expectedLower == "empty")
			return actualNormalized.empty();

		bool invert = false;
		std::string listText = expectedTrimmed;
		if (expectedLower.rfind("not_in:", 0) == 0)
		{
			invert = true;
			listText = expectedTrimmed.substr(7);
		}
		else if (expectedLower.rfind("notin:", 0) == 0)
		{
			invert = true;
			listText = expectedTrimmed.substr(6);
		}
		else if (expectedLower.rfind("not:", 0) == 0)
		{
			invert = true;
			listText = expectedTrimmed.substr(4);
		}
		else if (expectedLower.rfind("in:", 0) == 0)
		{
			listText = expectedTrimmed.substr(3);
		}
		else if (expectedLower.rfind("list:", 0) == 0)
		{
			listText = expectedTrimmed.substr(5);
		}
		else if (expectedLower.rfind("sid:", 0) == 0)
		{
			listText = expectedTrimmed.substr(4);
		}

		auto matchesSinglePattern = [&](const std::string& rawPattern) -> bool
		{
			const std::string pattern = NormalizeSidMatchText(rawPattern);
			if (pattern.empty() || actualNormalized.empty())
				return false;
			if (actualNormalized == pattern)
				return true;
			if (actualNormalized.size() >= pattern.size() && actualNormalized.compare(0, pattern.size(), pattern) == 0)
				return true;
			return false;
		};

		bool anyPattern = false;
		bool anyMatch = false;
		std::string token;
		for (size_t i = 0; i <= listText.size(); ++i)
		{
			const char ch = (i < listText.size()) ? listText[i] : ',';
			if (ch == ',' || ch == ';' || ch == '|')
			{
				const std::string trimmedToken = TrimAsciiWhitespaceCopy(token);
				token.clear();
				if (trimmedToken.empty())
					continue;
				anyPattern = true;
				if (matchesSinglePattern(trimmedToken))
				{
					anyMatch = true;
					if (!invert)
						return true;
				}
				continue;
			}
			token.push_back(ch);
		}

		if (!anyPattern)
			anyMatch = matchesSinglePattern(listText);

		if (!invert)
			return anyMatch;
		if (actualNormalized.empty())
			return false;
		return !anyMatch;
	}

	bool StructuredRuleCriterionMatches(
		const std::string& sourceText,
		const std::string& token,
		const std::string& condition,
		const std::map<std::string, std::string>& replacingMap,
		const VacdmPilotData* pilotData)
	{
		const std::string source = ToLowerAsciiCopy(sourceText);
		if (source == "runway")
		{
			std::string actualRunway;
			auto it = replacingMap.find(token);
			if (it != replacingMap.end())
				actualRunway = it->second;
			return RunwayRuleConditionMatches(condition, actualRunway);
		}
		if (source == "custom")
		{
			std::string actualValue;
			auto it = replacingMap.find(token);
			if (it != replacingMap.end())
				actualValue = it->second;
			return CustomRuleConditionMatches(condition, actualValue);
		}

		const std::string actualState = ResolveVacdmRuleStateName(token, pilotData);
		return VacdmRuleStateMatches(condition, actualState);
	}

	VacdmColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const std::string& statusKey,
		const std::string& detailKey,
		const std::map<std::string, std::string>& replacingMap,
		const VacdmPilotData* pilotData)
	{
		VacdmColorRuleOverrides overrides;
		for (const StructuredTagColorRule& rule : rules)
		{
			if (!StructuredRuleContextMatches(rule, tagTypeKey, statusKey, detailKey))
				continue;

			bool ruleMatches = true;
			if (!rule.criteria.empty())
			{
				for (const StructuredTagColorRule::Criterion& criterion : rule.criteria)
				{
					if (!StructuredRuleCriterionMatches(criterion.source, criterion.token, criterion.condition, replacingMap, pilotData))
					{
						ruleMatches = false;
						break;
					}
				}
			}
			else
			{
				ruleMatches = StructuredRuleCriterionMatches(rule.source, rule.token, rule.condition, replacingMap, pilotData);
			}

			if (ruleMatches)
				ApplyStructuredRuleColors(overrides, rule);
		}

		return overrides;
	}

	VacdmColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		bool isTagDetailed,
		const std::map<std::string, std::string>& replacingMap,
		const VacdmPilotData* pilotData)
	{
		const std::string statusKey = statusDefinitionKey != nullptr ? statusDefinitionKey : "default";
		const std::string detailKey = isTagDetailed ? "detailed" : "normal";
		return EvaluateStructuredTagColorRules(rules, tagTypeKey, statusKey, detailKey, replacingMap, pilotData);
	}

	void CollectRunwayColorRulesFromLineTexts(const std::vector<std::string>& lineTexts, std::vector<RunwayColorRuleDefinition>& outRules)
	{
		CollectColorRulesFromLineTexts(lineTexts, outRules, TryParseRunwayColorRuleToken);
	}

	VacdmColorRuleOverrides EvaluateRunwayColorRules(const std::vector<RunwayColorRuleDefinition>& rules, const std::map<std::string, std::string>& replacingMap)
	{
		return EvaluateColorRules(rules, [&](const RunwayColorRuleDefinition& rule) {
			std::string actualRunway;
			auto it = replacingMap.find(rule.token);
			if (it != replacingMap.end())
				actualRunway = it->second;

			return RunwayRuleConditionMatches(rule.expectedRunway, actualRunway);
			});
	}

	bool SidMatchesPatterns(const rapidjson::Value& patternsValue, const std::string& sidNormalized)
	{
		auto sidMatches = [&](const std::string& rawPattern) -> bool
		{
			const std::string pattern = NormalizeSidMatchText(rawPattern);
			if (pattern.empty())
				return false;

			if (sidNormalized == pattern)
				return true;

			if (sidNormalized.size() >= pattern.size() && sidNormalized.compare(0, pattern.size(), pattern) == 0)
				return true;

			return false;
		};

		if (patternsValue.IsString())
			return sidMatches(patternsValue.GetString());

		if (!patternsValue.IsArray())
			return false;

		for (rapidjson::SizeType i = 0; i < patternsValue.Size(); ++i)
		{
			if (!patternsValue[i].IsString())
				continue;

			if (sidMatches(patternsValue[i].GetString()))
				return true;
		}

		return false;
	}

	bool RunwayMatchesPatterns(const rapidjson::Value& patternsValue, const std::string& runwayNormalized)
	{
		auto runwayMatches = [&](const std::string& rawPattern) -> bool
		{
			std::string pattern = NormalizeRunwayMatchText(rawPattern);
			if (pattern.empty())
				return false;

			if (pattern == "*" || pattern == "ALL" || pattern == "ANY")
				return true;

			if (runwayNormalized == pattern)
				return true;

			if (runwayNormalized.size() >= pattern.size() && runwayNormalized.compare(0, pattern.size(), pattern) == 0)
				return true;

			return false;
		};

		if (patternsValue.IsString())
			return runwayMatches(patternsValue.GetString());

		if (!patternsValue.IsArray())
			return false;

		for (rapidjson::SizeType i = 0; i < patternsValue.Size(); ++i)
		{
			if (!patternsValue[i].IsString())
				continue;

			if (runwayMatches(patternsValue[i].GetString()))
				return true;
		}

		return false;
	}

	bool TryResolveSidColorFromGroups(const rapidjson::Value& groups, const std::string& sid, const std::string& runway, CConfig* config, Gdiplus::Color& outColor)
	{
		if (!config || !groups.IsArray())
			return false;

		const std::string sidNormalized = NormalizeSidMatchText(sid);
		const std::string runwayNormalized = NormalizeRunwayMatchText(runway);
		if (sidNormalized.empty() || runwayNormalized.empty())
			return false;

		for (rapidjson::SizeType i = 0; i < groups.Size(); ++i)
		{
			const rapidjson::Value& group = groups[i];
			if (!group.IsObject())
				continue;

			const rapidjson::Value* colorValue = nullptr;
			if (group.HasMember("color") && group["color"].IsObject())
				colorValue = &group["color"];
			else if (group.HasMember("rgb") && group["rgb"].IsObject())
				colorValue = &group["rgb"];

			if (!colorValue)
				continue;

			bool match = false;
			if (group.HasMember("names"))
				match = SidMatchesPatterns(group["names"], sidNormalized);
			if (!match && group.HasMember("sids"))
				match = SidMatchesPatterns(group["sids"], sidNormalized);
			if (!match && group.HasMember("sid"))
				match = SidMatchesPatterns(group["sid"], sidNormalized);

			if (!match)
				continue;

			bool runwayMatch = false;
			if (group.HasMember("runways"))
				runwayMatch = RunwayMatchesPatterns(group["runways"], runwayNormalized);
			else if (group.HasMember("runway"))
				runwayMatch = RunwayMatchesPatterns(group["runway"], runwayNormalized);
			else if (group.HasMember("rwys"))
				runwayMatch = RunwayMatchesPatterns(group["rwys"], runwayNormalized);

			if (!runwayMatch)
				continue;

			outColor = config->getConfigColor(*colorValue);
			return true;
		}

		return false;
	}

	bool TryResolveColoredSidTextColor(const rapidjson::Value& profile, const std::string& sid, const std::string& runway, CConfig* config, Gdiplus::Color& outColor)
	{
		if (!config || !profile.IsObject())
			return false;

		auto tryColorContainer = [&](const rapidjson::Value& container) -> bool
		{
			if (!container.IsObject())
				return false;

			static const std::array<const char*, 3> sidColorKeys = { "sid_text_colors", "csid_colors", "colored_sids" };
			for (const char* key : sidColorKeys)
			{
				if (!container.HasMember(key))
					continue;

				if (TryResolveSidColorFromGroups(container[key], sid, runway, config, outColor))
					return true;
			}

			return false;
		};

		// Colored SID text is configured at profile level (one list per profile).
		return tryColorContainer(profile);
	}
}
