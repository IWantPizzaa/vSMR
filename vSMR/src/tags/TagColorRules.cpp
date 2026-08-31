#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagColorRules.Internal.hpp"

#include "shared/TextUtils.hpp"

#include <cctype>
#include <ctime>

namespace VsmrTagColorRules
{
	namespace Internal
	{
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
	}

	static void ApplyColorRuleChannels(VacdmColorRuleOverrides& target, const ColorRuleChannels& source)
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

	static void ApplyStructuredRuleColors(VacdmColorRuleOverrides& target, const StructuredTagColorRule& rule)
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

	static bool TryGetVacdmRuleTokenValue(const VacdmPilotData& pilot, const std::string& token, std::time_t& outTime, bool& outHas)
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

	static std::string NormalizeVacdmStateName(const std::string& rawState)
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
	static std::string CanonicalVacdmStateName(const std::string& rawState)
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
	static bool VacdmRuleStateMatches(const std::string& expectedStateRaw, const std::string& actualStateRaw)
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

	template <typename RuleType, typename Matcher>
	static VacdmColorRuleOverrides EvaluateColorRules(const std::vector<RuleType>& rules, Matcher matcher)
	{
		VacdmColorRuleOverrides overrides;
		for (const RuleType& rule : rules)
		{
			if (matcher(rule))
				ApplyColorRuleChannels(overrides, rule);
		}
		return overrides;
	}

	VacdmColorRuleOverrides EvaluateVacdmColorRules(const std::vector<VacdmColorRuleDefinition>& rules, const VacdmPilotData* pilotData)
	{
		return EvaluateColorRules(rules, [&](const VacdmColorRuleDefinition& rule) {
			const std::string actualState = ResolveVacdmRuleStateName(rule.token, pilotData);
			return VacdmRuleStateMatches(rule.expectedState, actualState);
			});
	}

	static std::string NormalizeSidMatchText(const std::string& value)
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

	static std::string NormalizeRunwayMatchText(const std::string& value)
	{
		std::string normalized = NormalizeSidMatchText(value);
		if (normalized.rfind("RWY", 0) == 0)
			normalized = normalized.substr(3);
		return normalized;
	}

	static bool RunwayRuleConditionMatches(const std::string& expectedConditionRaw, const std::string& actualRunwayRaw)
	{
		const std::string actualRunwayNormalized = NormalizeRunwayMatchText(actualRunwayRaw);
		const std::string expectedTrimmed = TrimAsciiWhitespaceCopy(expectedConditionRaw);
		const std::string expectedLower = ToLowerAsciiCopy(expectedTrimmed);
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
		auto matchesSingleCondition = [&](const std::string& conditionRaw) -> bool
		{
			const std::string expectedCondition =
				Internal::NormalizeRunwayRuleConditionName(conditionRaw);
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

		if (listText.find(',') == std::string::npos &&
			listText.find(';') == std::string::npos &&
			listText.find('|') == std::string::npos)
		{
			const bool matches = matchesSingleCondition(listText);
			return invert ? (!actualRunwayNormalized.empty() && !matches) : matches;
		}

		std::string token;
		bool hasToken = false;
		for (size_t i = 0; i <= listText.size(); ++i)
		{
			const char ch = (i < listText.size()) ? listText[i] : ',';
			if (ch == ',' || ch == ';' || ch == '|')
			{
				const std::string condition =
					Internal::NormalizeRunwayRuleConditionName(token);
				token.clear();
				if (condition.empty())
					continue;
				hasToken = true;
				if (matchesSingleCondition(condition))
				{
					if (!invert)
						return true;
					return false;
				}
				continue;
			}

			token.push_back(ch);
		}

		if (!hasToken)
		{
			const bool matches = matchesSingleCondition(listText);
			return invert ? (!actualRunwayNormalized.empty() && !matches) : matches;
		}
		return invert && !actualRunwayNormalized.empty();
	}

	static bool StructuredRuleContextMatches(const StructuredTagColorRule& rule, const std::string& tagTypeKey, const std::string& statusKey, const std::string& detailKey)
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

	static bool CustomRuleConditionMatches(const std::string& expectedConditionRaw, const std::string& actualValueRaw)
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

	static bool StructuredRuleCriterionMatches(
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

	static VacdmColorRuleOverrides EvaluateStructuredTagColorRules(
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
				// Every criterion must match before the rule can change a color channel
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

}
