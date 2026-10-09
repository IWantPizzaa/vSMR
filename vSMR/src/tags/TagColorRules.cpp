#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagTokenValues.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagColorRules.Internal.hpp"

#include "shared/TextUtils.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>

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

	static void ApplyColorRuleChannels(TagColorRuleOverrides& target, const ColorRuleChannels& source)
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

	static void ApplyStructuredRuleColors(TagColorRuleOverrides& target, const StructuredTagColorRule& rule)
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

	void MergeColorRuleOverrides(TagColorRuleOverrides& target, const TagColorRuleOverrides& source)
	{
		ApplyColorRuleChannels(target, source);
		for (const auto& entry : source.fieldEffects)
		{
			FieldRuleEffects& destination = target.fieldEffects[entry.first];
			const FieldRuleEffects& value = entry.second;
			if (value.hasColor)
			{
				destination.hasColor = true;
				destination.colorR = value.colorR;
				destination.colorG = value.colorG;
				destination.colorB = value.colorB;
				destination.colorA = value.colorA;
			}
			if (value.hasBackground)
			{
				destination.hasBackground = true;
				destination.backgroundR = value.backgroundR;
				destination.backgroundG = value.backgroundG;
				destination.backgroundB = value.backgroundB;
				destination.backgroundA = value.backgroundA;
			}
			if (value.hasBold) { destination.hasBold = true; destination.bold = value.bold; }
			if (value.hasBlink) { destination.hasBlink = true; destination.blink = value.blink; }
		}
	}

	void MergeMissingColorRuleOverrides(TagColorRuleOverrides& target, const TagColorRuleOverrides& fallback)
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
		for (const auto& entry : fallback.fieldEffects)
		{
			FieldRuleEffects& destination = target.fieldEffects[entry.first];
			const FieldRuleEffects& value = entry.second;
			if (!destination.hasColor && value.hasColor)
			{
				destination.hasColor = true;
				destination.colorR = value.colorR;
				destination.colorG = value.colorG;
				destination.colorB = value.colorB;
				destination.colorA = value.colorA;
			}
			if (!destination.hasBackground && value.hasBackground)
			{
				destination.hasBackground = true;
				destination.backgroundR = value.backgroundR;
				destination.backgroundG = value.backgroundG;
				destination.backgroundB = value.backgroundB;
				destination.backgroundA = value.backgroundA;
			}
			if (!destination.hasBold && value.hasBold) { destination.hasBold = true; destination.bold = value.bold; }
			if (!destination.hasBlink && value.hasBlink) { destination.hasBlink = true; destination.blink = value.blink; }
		}
	}

	static bool TryGetCdmRuleTokenValue(const CdmPilotData& pilot, const std::string& token, std::time_t& outTime, bool& outHas)
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
		if (lowered == "tsac")
		{
			outTime = pilot.tsacUtc;
			outHas = pilot.hasTsac;
			return true;
		}
		return false;
	}

	// Resolve a canonical runtime state name for a CDM time token.
	// These states are consumed by both legacy inline rules and structured rules.
	std::string ResolveCdmRuleStateName(const std::string& token, const CdmPilotData* pilotData, std::time_t now)
	{
		if (now == 0) now = std::time(nullptr);
		const std::string lowered = ToLowerAsciiCopy(token);
		if (pilotData == nullptr)
			return "missing";

		const CdmPilotData& pilot = *pilotData;
		if (lowered == "tobt")
		{
			if (!pilot.hasTobt)
				return "missing";
			if (!pilot.hasTsat || pilot.hasAsat)
				return "inactive";

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
		if (!TryGetCdmRuleTokenValue(pilot, lowered, tokenTime, hasToken) || !hasToken)
			return "missing";
		if (tokenTime <= 0)
			return "missing";

		const long long deltaSeconds = static_cast<long long>(std::difftime(tokenTime, now));
		return deltaSeconds >= 0 ? "future" : "past";
	}

	static std::string NormalizeCdmStateName(const std::string& rawState)
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
	static std::string CanonicalCdmStateName(const std::string& rawState)
	{
		const std::string state = NormalizeCdmStateName(rawState);
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
	static bool CdmRuleStateMatches(const std::string& expectedStateRaw, const std::string& actualStateRaw)
	{
		const std::string expected = CanonicalCdmStateName(expectedStateRaw);
		const std::string actual = CanonicalCdmStateName(actualStateRaw);
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
	static TagColorRuleOverrides EvaluateColorRules(const std::vector<RuleType>& rules, Matcher matcher)
	{
		TagColorRuleOverrides overrides;
		for (const RuleType& rule : rules)
		{
			if (matcher(rule))
				ApplyColorRuleChannels(overrides, rule);
		}
		return overrides;
	}

	TagColorRuleOverrides EvaluateCdmColorRules(const std::vector<CdmColorRuleDefinition>& rules, const CdmPilotData* pilotData, std::time_t now)
	{
		if (now == 0) now = std::time(nullptr);
		return EvaluateColorRules(rules, [&](const CdmColorRuleDefinition& rule) {
			const std::string actualState = ResolveCdmRuleStateName(rule.token, pilotData, now);
			return CdmRuleStateMatches(rule.expectedState, actualState);
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

	static bool StructuredRuleContextMatches(const StructuredTagColorRule& rule, const std::string& tagTypeKey,
		const std::string& statusKey, const std::string& detailKey, const VsmrTags::TokenValues& tokens,
		bool ignoreDetail = false)
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

		const std::string requestedType = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(rule.tagType));
		const auto airborne = tokens.find("rule.airborne");
		const bool typeMatches = requestedType == "airborne"
			? airborne != tokens.end() && ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(airborne->second)) == "true"
			: matchesField(rule.tagType, tagTypeKey);
		return typeMatches &&
			statusMatches &&
			(ignoreDetail || matchesField(rule.detail, detailKey));
	}

	static bool CustomRuleConditionMatches(
		const std::string& expectedConditionRaw,
		const std::string& actualValueRaw,
		bool allowPrefixMatch = true)
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
			if (allowPrefixMatch && actualNormalized.size() >= pattern.size() &&
				actualNormalized.compare(0, pattern.size(), pattern) == 0)
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
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now)
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
		if (source == "custom" || source == "vsid" || source == "cdm")
		{
			const bool cdmTimeToken = source == "cdm" &&
				(token == "tobt" || token == "tsat" || token == "ttot" ||
					token == "ctot" || token == "tsac" || token == "asrt" ||
					token == "asat");
			const std::string normalizedCondition =
				ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(condition));
			const bool valueCondition =
				normalizedCondition.rfind("in:", 0) == 0 ||
				normalizedCondition.rfind("not:", 0) == 0 ||
				normalizedCondition.rfind("not_in:", 0) == 0 ||
				normalizedCondition.rfind("notin:", 0) == 0;
			if (cdmTimeToken && !valueCondition)
			{
				const std::string actualState =
					ResolveCdmRuleStateName(token, pilotData, now);
				return CdmRuleStateMatches(condition, actualState);
			}

			std::string actualValue;
			const std::string& mapToken = token;
			auto it = replacingMap.find(mapToken);
			if (it != replacingMap.end())
				actualValue = it->second;
			if (source == "vsid" && token == "vsid_rwy")
				return RunwayRuleConditionMatches(condition, actualValue);
			return CustomRuleConditionMatches(
				condition,
				actualValue,
				source == "custom" || token == "vsid_sid");
		}

		const std::string actualState = ResolveCdmRuleStateName(token, pilotData, now);
		return CdmRuleStateMatches(condition, actualState);
	}

	namespace
	{
		constexpr unsigned MaximumConditionDepth = 8;
		constexpr unsigned MaximumConditionNodes = 128;
		constexpr unsigned MaximumRuleEffects = 32;
		constexpr unsigned MaximumRuleListValues = 128;
		constexpr unsigned MaximumStructuredRules = 256;
		constexpr unsigned MaximumRuleStringBytes = 512;
		using RuleCondition = StructuredTagColorRule::RuleCondition;
		using RuleEffect = StructuredTagColorRule::RuleEffect;

		bool InNames(std::string_view value, std::initializer_list<std::string_view> names)
		{
			return std::find(names.begin(), names.end(), value) != names.end();
		}

		bool RuleFailure(std::string* error, const char* message)
		{
			if (error != nullptr) *error = message;
			return false;
		}

		bool ReadRuleString(const rapidjson::Value& value, std::string& output, bool allowEmpty = false)
		{
			if (!value.IsString() || value.GetStringLength() > MaximumRuleStringBytes) return false;
			const std::string_view text(value.GetString(), value.GetStringLength());
			for (const unsigned char ch : text)
				if (ch < 0x20 || ch == 0x7f) return false;
			output = TrimAsciiWhitespaceCopy(std::string(text));
			return allowEmpty || !output.empty();
		}

		bool KnownMembers(const rapidjson::Value& object, std::initializer_list<std::string_view> names)
		{
			if (!object.IsObject() || object.MemberCount() > names.size()) return false;
			std::set<std::string_view> seen;
			for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member)
			{
				const std::string_view name(member->name.GetString(), member->name.GetStringLength());
				if (!InNames(name, names) || !seen.insert(name).second) return false;
			}
			return true;
		}

		bool IsCdmTimeToken(std::string_view token)
		{
			return InNames(token, { "tobt", "tsat", "ttot", "ctot", "tsac", "asrt", "asat", "aobt", "atot", "aort" });
		}

		bool IsRuleField(std::string_view field)
		{
			if (InNames(field, { "flight.sid", "flight.deprwy", "flight.arvrwy", "flight.scratchpad",
				"flight.holdingpoint", "flight.origin", "flight.destination", "flight.actype", "flight.wake",
				"flight.groundstatus", "flight.callsign", "flight.gs", "flight.flightlevel", "flight.clearance",
				"vsid.sid", "vsid.rwy", "vsid.cfl", "cdm.deice", "cdm.tobt_set_by", "cdm.flow_restriction",
				"cdm.ecfmp_restriction", "cdm.manual_ctot" })) return true;
			if (field.substr(0, 4) != "cdm.") return false;
			const std::string_view token = field.substr(4);
			if (IsCdmTimeToken(token)) return true;
			return token.size() > 6 && token.substr(token.size() - 6) == "_state" &&
				IsCdmTimeToken(token.substr(0, token.size() - 6));
		}

		bool IsNumericRuleField(std::string_view field)
		{
			return field == "flight.gs" || field == "flight.flightlevel" ||
				(field.substr(0, 4) == "cdm." && IsCdmTimeToken(field.substr(4)));
		}

		bool IsRuleEffectField(std::string_view field)
		{
			return InNames(field, { "event_booking", "ready_startup", "groundstatus", "holdingpoint",
				"flightlevel", "scratchpad", "clearance", "callsign", "systemid", "tendency", "uk_stand",
				"vsid_cfl", "vsid_rwy", "vsid_sid", "sqerror", "actype", "arvrwy", "deprwy", "origin",
				"remark", "sctype", "seprwy", "srvrwy", "aobt", "aort", "asat", "asid", "asrt", "atot",
				"ctot", "dest", "gate", "sate", "ssid", "tobt", "tsac", "tsat", "ttot", "wake", "ssr", "gs" });
		}

		bool IsRuleStatus(std::string_view status)
		{
			// Gate/arr are retained for roundtripping earlier profiles; the editor
			// offers only statuses emitted by the current scene role classifier.
			return InNames(status, { "any", "default", "nofpl", "gate", "nsts", "push", "stup", "taxi", "lnup",
				"depa", "arr", "airdep", "airdep_onrunway", "airarr", "airarr_onrunway" });
		}

		bool ReadFiniteRuleNumber(const rapidjson::Value& value, double& output)
		{
			if (!value.IsNumber()) return false;
			output = value.GetDouble();
			return std::isfinite(output);
		}

		bool ValidLegacyField(const RuleCondition& condition)
		{
			if (condition.source == "runway")
				return InNames(condition.token, { "deprwy", "seprwy", "arvrwy", "srvrwy" });
			if (condition.source == "custom")
				return InNames(condition.token, { "sid", "asid", "ssid", "deprwy", "seprwy", "arvrwy", "srvrwy" });
			if (condition.source == "vsid")
				return InNames(condition.token, { "vsid_sid", "vsid_rwy", "vsid_cfl" });
			if (condition.source == "cdm")
				return IsCdmTimeToken(condition.token) || InNames(condition.token, { "deice", "tobt_set_by",
					"flow_restriction", "ecfmp_restriction", "manual_ctot" });
			return false;
		}

		bool ParseRuleCondition(const rapidjson::Value& object, RuleCondition& output,
			unsigned depth, unsigned& nodes, std::string* error)
		{
			if (depth > MaximumConditionDepth || ++nodes > MaximumConditionNodes)
				return RuleFailure(error, "Rule condition exceeds the depth or node limit.");
			if (!object.IsObject()) return RuleFailure(error, "Rule condition must be an object.");
			if (object.HasMember("all") || object.HasMember("any") || object.HasMember("not"))
			{
				if (object.MemberCount() != 1) return RuleFailure(error, "A condition group has exactly one operator.");
				output.kind = object.HasMember("all") ? "all" : object.HasMember("any") ? "any" : "not";
				const rapidjson::Value& children = object[output.kind.c_str()];
				if (output.kind == "not")
				{
					output.children.emplace_back();
					return ParseRuleCondition(children, output.children.back(), depth + 1, nodes, error);
				}
				if (!children.IsArray() || children.Empty() || children.Size() > MaximumConditionNodes)
					return RuleFailure(error, "All/any groups need a non-empty bounded array of conditions.");
				for (const rapidjson::Value& child : children.GetArray())
				{
					output.children.emplace_back();
					if (!ParseRuleCondition(child, output.children.back(), depth + 1, nodes, error)) return false;
				}
				return true;
			}
			if (!object.HasMember("field") || !ReadRuleString(object["field"], output.field))
				return RuleFailure(error, "A condition needs a valid field.");
			output.field = ToLowerAsciiCopy(output.field);
			if (output.field == "legacy")
			{
				if (!KnownMembers(object, { "field", "source", "token", "condition" }) ||
					!object.HasMember("source") || !ReadRuleString(object["source"], output.source) ||
					!object.HasMember("token") || !ReadRuleString(object["token"], output.token) ||
					!object.HasMember("condition") || !ReadRuleString(object["condition"], output.condition))
					return RuleFailure(error, "Legacy conditions need valid source, token and condition fields.");
				output.source = ToLowerAsciiCopy(output.source);
				output.token = ToLowerAsciiCopy(output.token);
				return ValidLegacyField(output) || RuleFailure(error, "Unknown legacy condition source or token.");
			}
			if (!KnownMembers(object, { "field", "op", "value", "values", "min", "max" }) ||
				!IsRuleField(output.field) || !object.HasMember("op") || !ReadRuleString(object["op"], output.op))
				return RuleFailure(error, "Unknown condition field or malformed operator.");
			output.op = ToLowerAsciiCopy(output.op);
			if (!InNames(output.op, { "equals", "not_equals", "in", "not_in", "contains", "starts_with", "ends_with",
				"set", "missing", "lt", "lte", "gt", "gte", "between" }))
				return RuleFailure(error, "Unknown condition operator.");
			if (output.op == "set" || output.op == "missing")
				return object.MemberCount() == 2 || RuleFailure(error, "Set/missing conditions do not accept operands.");
			const bool numericField = IsNumericRuleField(output.field);
			if (output.op == "between")
			{
				if (!numericField || object.MemberCount() != 4 || !object.HasMember("min") || !object.HasMember("max") ||
					!ReadFiniteRuleNumber(object["min"], output.minimum) || !ReadFiniteRuleNumber(object["max"], output.maximum) ||
					output.minimum > output.maximum)
					return RuleFailure(error, "Between needs a numeric field and finite ordered min/max values.");
				return true;
			}
			if (output.op == "in" || output.op == "not_in")
			{
				if (numericField || object.MemberCount() != 3 || !object.HasMember("values") || !object["values"].IsArray() ||
					object["values"].Empty() || object["values"].Size() > MaximumRuleListValues)
					return RuleFailure(error, "In/not-in needs a non-empty bounded text-value list.");
				for (const rapidjson::Value& item : object["values"].GetArray())
				{
					std::string value;
					if (!ReadRuleString(item, value)) return RuleFailure(error, "Rule list values must be non-empty strings.");
					output.values.push_back(std::move(value));
				}
				return true;
			}
			const bool numericComparison = InNames(output.op, { "lt", "lte", "gt", "gte" });
			const bool textComparison = InNames(output.op, { "contains", "starts_with", "ends_with" });
			if ((numericComparison && !numericField) || (textComparison && numericField) ||
				object.MemberCount() != 3 || !object.HasMember("value"))
				return RuleFailure(error, "Condition operator does not match its field or operand.");
			if (numericField)
			{
				output.valueIsNumber = true;
				return ReadFiniteRuleNumber(object["value"], output.numberValue) || RuleFailure(error, "Numeric conditions need a finite number.");
			}
			return ReadRuleString(object["value"], output.value) || RuleFailure(error, "Text conditions need a non-empty string.");
		}

		bool ParseRuleEffect(const rapidjson::Value& object, RuleEffect& output, std::string* error)
		{
			if (!KnownMembers(object, { "type", "field", "color", "value" }) || !object.HasMember("type") ||
				!ReadRuleString(object["type"], output.type)) return RuleFailure(error, "Malformed rule effect.");
			output.type = ToLowerAsciiCopy(output.type);
			const bool fieldEffect = InNames(output.type, { "field_color", "field_background", "field_bold", "field_blink" });
			const bool colorEffect = InNames(output.type, { "target_color", "tag_color", "text_color", "field_color", "field_background" });
			if (!fieldEffect && !colorEffect) return RuleFailure(error, "Unknown rule effect type.");
			if (fieldEffect)
			{
				if (!object.HasMember("field") || !ReadRuleString(object["field"], output.field))
					return RuleFailure(error, "A field effect needs a tag-token field.");
				output.field = ToLowerAsciiCopy(output.field);
				if (!IsRuleEffectField(output.field)) return RuleFailure(error, "Unknown tag-token effect field.");
			}
			else if (object.HasMember("field")) return RuleFailure(error, "Whole-tag/target effects do not accept a field.");
			if (colorEffect)
			{
				if (object.HasMember("value") || !object.HasMember("color") ||
					!KnownMembers(object["color"], { "r", "g", "b", "a" })) return RuleFailure(error, "A color effect needs an RGBA color.");
				const rapidjson::Value& color = object["color"];
				for (const char* channel : { "r", "g", "b", "a" })
				{
					if (!color.HasMember(channel))
					{
						if (std::string_view(channel) == "a") continue;
						return RuleFailure(error, "RGB color channels are required.");
					}
					if (!color[channel].IsInt() || color[channel].GetInt() < 0 || color[channel].GetInt() > 255)
						return RuleFailure(error, "Color channels must be integers from 0 to 255.");
				}
				output.r = color["r"].GetInt(); output.g = color["g"].GetInt(); output.b = color["b"].GetInt();
				if (color.HasMember("a")) output.a = color["a"].GetInt();
			}
			else
			{
				if (object.HasMember("color")) return RuleFailure(error, "Bold/blink effects do not accept a color.");
				if (object.HasMember("value"))
				{
					if (!object["value"].IsBool()) return RuleFailure(error, "Bold/blink effect value must be boolean.");
					output.value = object["value"].GetBool();
				}
			}
			return true;
		}

		enum class ConditionTruth { False, True, Unknown };
		struct RuleValue
		{
			bool present = false;
			bool numeric = false;
			double number = 0.0;
			std::string text;
		};

		bool ParseNumberText(const std::string& value, double& output)
		{
			if (value.empty()) return false;
			const char* begin = value.data();
			const char* end = begin + value.size();
			if (*begin == '+') ++begin;
			const auto result = std::from_chars(begin, end, output);
			return result.ec == std::errc{} && result.ptr == end && std::isfinite(output);
		}

		std::string FlightTokenForField(std::string_view field)
		{
			if (field == "flight.destination") return "dest";
			return std::string(field.substr(7));
		}

		RuleValue ReadConditionValue(const std::string& field, const VsmrTags::TokenValues& tokens,
			const CdmPilotData* pilot, std::time_t now)
		{
			RuleValue result;
			if (field.rfind("cdm.", 0) == 0)
			{
				const std::string token = field.substr(4);
				if (token.size() > 6 && token.compare(token.size() - 6, 6, "_state") == 0)
				{
					result.text = ResolveCdmRuleStateName(token.substr(0, token.size() - 6), pilot, now);
					result.present = true;
					return result;
				}
				if (IsCdmTimeToken(token))
				{
					std::time_t timestamp = 0;
					bool available = false;
					result.numeric = true;
					if (pilot != nullptr && TryGetCdmRuleTokenValue(*pilot, token, timestamp, available) && available && timestamp > 0)
					{
						result.present = true;
						result.number = std::difftime(now, timestamp) / 60.0;
					}
					return result;
				}
				if (pilot == nullptr) return result;
				const auto& bridge = pilot->bridgeData;
				if (token == "deice") result.text = bridge.deice;
				else if (token == "tobt_set_by") result.text = bridge.tobtSetBy;
				else if (token == "flow_restriction") result.text = bridge.flowRestriction;
				else if (token == "ecfmp_restriction") result.text = bridge.ecfmpRestriction;
				else if (token == "manual_ctot" && bridge.manualCtot.has_value()) result.text = *bridge.manualCtot ? "true" : "false";
			}
			else
			{
				const bool flight = field.rfind("flight.", 0) == 0;
				const std::string token = flight ? FlightTokenForField(field) : "vsid_" + field.substr(5);
				auto raw = tokens.find("rule." + token);
				if (raw != tokens.end()) result.text = raw->second;
				else
				{
					auto displayed = tokens.find(token == "sid" ? "asid" : token);
					if (displayed == tokens.end() && token == "sid") displayed = tokens.find("sid");
					if (displayed != tokens.end()) result.text = displayed->second;
					// Fallback is only for pre-v2 callers/tests. Runtime publishes raw rule.* keys.
					if ((token == "scratchpad" && result.text == "...") ||
						((token == "deprwy" || token == "arvrwy") && result.text == "RWY") ||
						(token == "sid" && result.text == "SID") ||
						((token == "origin" || token == "dest") && result.text == "????") ||
						(token == "groundstatus" && result.text == "STS")) result.text.clear();
				}
			}
			result.text = TrimAsciiWhitespaceCopy(result.text);
			result.present = !result.text.empty();
			result.numeric = IsNumericRuleField(field);
			if (result.numeric) result.present = result.present && ParseNumberText(result.text, result.number);
			return result;
		}

		ConditionTruth EvaluateRuleCondition(const RuleCondition& node, const VsmrTags::TokenValues& tokens,
			const CdmPilotData* pilot, std::time_t now, unsigned depth, unsigned& nodes)
		{
			if (depth > MaximumConditionDepth || ++nodes > MaximumConditionNodes) return ConditionTruth::Unknown;
			if (node.kind == "not")
			{
				if (node.children.size() != 1) return ConditionTruth::Unknown;
				const ConditionTruth child = EvaluateRuleCondition(node.children.front(), tokens, pilot, now, depth + 1, nodes);
				return child == ConditionTruth::Unknown ? child : child == ConditionTruth::True ? ConditionTruth::False : ConditionTruth::True;
			}
			if (node.kind == "all" || node.kind == "any")
			{
				if (node.children.empty()) return ConditionTruth::Unknown;
				bool unknown = false;
				for (const RuleCondition& child : node.children)
				{
					const ConditionTruth truth = EvaluateRuleCondition(child, tokens, pilot, now, depth + 1, nodes);
					if (node.kind == "all" && truth == ConditionTruth::False) return ConditionTruth::False;
					if (node.kind == "any" && truth == ConditionTruth::True) return ConditionTruth::True;
					unknown = unknown || truth == ConditionTruth::Unknown;
				}
				return unknown ? ConditionTruth::Unknown : node.kind == "all" ? ConditionTruth::True : ConditionTruth::False;
			}
			if (node.kind != "leaf") return ConditionTruth::Unknown;
			if (node.field == "legacy")
				return StructuredRuleCriterionMatches(node.source, node.token, node.condition, tokens, pilot, now)
					? ConditionTruth::True : ConditionTruth::False;
			if (!IsRuleField(node.field)) return ConditionTruth::Unknown;
			const RuleValue actual = ReadConditionValue(node.field, tokens, pilot, now);
			if (node.op == "set") return actual.present ? ConditionTruth::True : ConditionTruth::False;
			if (node.op == "missing") return actual.present ? ConditionTruth::False : ConditionTruth::True;
			if (!actual.present) return ConditionTruth::Unknown;
			bool matches = false;
			if (actual.numeric)
			{
				if (node.op == "between") matches = actual.number >= node.minimum && actual.number <= node.maximum;
				else if (!node.valueIsNumber || !std::isfinite(node.numberValue)) return ConditionTruth::Unknown;
				else if (node.op == "equals") matches = actual.number == node.numberValue;
				else if (node.op == "not_equals") matches = actual.number != node.numberValue;
				else if (node.op == "lt") matches = actual.number < node.numberValue;
				else if (node.op == "lte") matches = actual.number <= node.numberValue;
				else if (node.op == "gt") matches = actual.number > node.numberValue;
				else if (node.op == "gte") matches = actual.number >= node.numberValue;
				else return ConditionTruth::Unknown;
			}
			else
			{
				const std::string value = ToUpperAsciiCopy(actual.text);
				const std::string expected = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(node.value));
				if (node.op == "equals") matches = value == expected;
				else if (node.op == "not_equals") matches = value != expected;
				else if (node.op == "contains") matches = value.find(expected) != std::string::npos;
				else if (node.op == "starts_with") matches = value.rfind(expected, 0) == 0;
				else if (node.op == "ends_with") matches = value.size() >= expected.size() &&
					value.compare(value.size() - expected.size(), expected.size(), expected) == 0;
				else if (node.op == "in" || node.op == "not_in")
				{
					if (node.values.empty()) return ConditionTruth::Unknown;
					for (const std::string& entry : node.values)
						if (value == ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(entry))) { matches = true; break; }
					if (node.op == "not_in") matches = !matches;
				}
				else return ConditionTruth::Unknown;
			}
			return matches ? ConditionTruth::True : ConditionTruth::False;
		}

		void ApplyRuleEffects(TagColorRuleOverrides& overrides, const StructuredTagColorRule& rule)
		{
			for (const RuleEffect& effect : rule.effects)
			{
				if (effect.type == "target_color")
				{
					overrides.hasTargetColor = true;
					overrides.targetR = effect.r; overrides.targetG = effect.g; overrides.targetB = effect.b; overrides.targetA = effect.a;
				}
				else if (effect.type == "tag_color")
				{
					overrides.hasTagColor = true;
					overrides.tagR = effect.r; overrides.tagG = effect.g; overrides.tagB = effect.b; overrides.tagA = effect.a;
				}
				else if (effect.type == "text_color")
				{
					overrides.hasTextColor = true;
					overrides.textR = effect.r; overrides.textG = effect.g; overrides.textB = effect.b; overrides.textA = effect.a;
				}
				else if (IsRuleEffectField(effect.field))
				{
					FieldRuleEffects& field = overrides.fieldEffects[effect.field];
					if (effect.type == "field_color")
					{
						field.hasColor = true;
						field.colorR = effect.r; field.colorG = effect.g; field.colorB = effect.b; field.colorA = effect.a;
					}
					else if (effect.type == "field_background")
					{
						field.hasBackground = true;
						field.backgroundR = effect.r; field.backgroundG = effect.g; field.backgroundB = effect.b; field.backgroundA = effect.a;
					}
					else if (effect.type == "field_bold") { field.hasBold = true; field.bold = effect.value; }
					else if (effect.type == "field_blink") { field.hasBlink = true; field.blink = effect.value; }
				}
			}
		}

		bool ConditionUsesClock(const RuleCondition& node, unsigned depth, unsigned& nodes)
		{
			if (depth > MaximumConditionDepth || ++nodes > MaximumConditionNodes) return false;
			if (node.field.rfind("cdm.", 0) == 0)
			{
				const std::string token = node.field.substr(4);
				if (IsCdmTimeToken(token) || (token.size() > 6 && token.compare(token.size() - 6, 6, "_state") == 0 &&
					IsCdmTimeToken(token.substr(0, token.size() - 6)))) return true;
			}
			if (node.field == "legacy" && node.source == "cdm" && IsCdmTimeToken(node.token)) return true;
			for (const RuleCondition& child : node.children)
				if (ConditionUsesClock(child, depth + 1, nodes)) return true;
			return false;
		}

		void AddRuleString(rapidjson::Value& object, const char* key, const std::string& value,
			rapidjson::Document::AllocatorType& allocator)
		{
			rapidjson::Value name(key, allocator), text(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
			object.AddMember(name, text, allocator);
		}

		bool WriteRuleCondition(const RuleCondition& node, rapidjson::Value& output,
			rapidjson::Document::AllocatorType& allocator, unsigned depth, unsigned& nodes)
		{
			if (depth > MaximumConditionDepth || ++nodes > MaximumConditionNodes) return false;
			output.SetObject();
			if (node.kind == "not")
			{
				if (node.children.size() != 1) return false;
				rapidjson::Value child;
				if (!WriteRuleCondition(node.children.front(), child, allocator, depth + 1, nodes)) return false;
				output.AddMember("not", child, allocator);
				return true;
			}
			if (node.kind == "all" || node.kind == "any")
			{
				if (node.children.empty() || node.children.size() > MaximumConditionNodes) return false;
				rapidjson::Value children(rapidjson::kArrayType);
				for (const RuleCondition& item : node.children)
				{
					rapidjson::Value child;
					if (!WriteRuleCondition(item, child, allocator, depth + 1, nodes)) return false;
					children.PushBack(child, allocator);
				}
				rapidjson::Value key(node.kind.c_str(), allocator);
				output.AddMember(key, children, allocator);
				return true;
			}
			if (node.kind != "leaf") return false;
			AddRuleString(output, "field", node.field, allocator);
			if (node.field == "legacy")
			{
				AddRuleString(output, "source", node.source, allocator);
				AddRuleString(output, "token", node.token, allocator);
				AddRuleString(output, "condition", node.condition, allocator);
				return true;
			}
			AddRuleString(output, "op", node.op, allocator);
			if (node.op == "set" || node.op == "missing") return true;
			if (node.op == "between")
			{
				output.AddMember("min", node.minimum, allocator); output.AddMember("max", node.maximum, allocator);
			}
			else if (node.op == "in" || node.op == "not_in")
			{
				if (node.values.size() > MaximumRuleListValues) return false;
				rapidjson::Value values(rapidjson::kArrayType);
				for (const std::string& item : node.values)
				{
					rapidjson::Value value(item.c_str(), static_cast<rapidjson::SizeType>(item.size()), allocator);
					values.PushBack(value, allocator);
				}
				output.AddMember("values", values, allocator);
			}
			else if (node.valueIsNumber) output.AddMember("value", node.numberValue, allocator);
			else AddRuleString(output, "value", node.value, allocator);
			return true;
		}
	}

	bool TryParseStructuredRuleV2(const rapidjson::Value& value, StructuredTagColorRule& outRule, std::string* error)
	{
		outRule = StructuredTagColorRule{};
		if (error != nullptr) error->clear();
		if (!KnownMembers(value, { "name", "enabled", "stop_processing", "tag_type", "status", "statuses", "detail", "when", "effects" }))
			return RuleFailure(error, "Unknown, duplicate or malformed rule fields.");
		StructuredTagColorRule parsed;
		parsed.usesStructuredCondition = true;
		if (value.HasMember("name") && !ReadRuleString(value["name"], parsed.name, true)) return RuleFailure(error, "Invalid rule name.");
		for (const char* flag : { "enabled", "stop_processing" })
		{
			if (!value.HasMember(flag)) continue;
			if (!value[flag].IsBool()) return RuleFailure(error, "Enabled/stop-processing flags must be boolean.");
			if (std::string_view(flag) == "enabled") parsed.enabled = value[flag].GetBool();
			else parsed.stopProcessing = value[flag].GetBool();
		}
		if (value.HasMember("tag_type"))
		{
			if (!ReadRuleString(value["tag_type"], parsed.tagType)) return RuleFailure(error, "Invalid tag type scope.");
			parsed.tagType = ToLowerAsciiCopy(parsed.tagType);
			if (!InNames(parsed.tagType, { "any", "departure", "arrival", "airborne", "uncorrelated" })) return RuleFailure(error, "Unknown tag type scope.");
		}
		if (value.HasMember("detail"))
		{
			if (!ReadRuleString(value["detail"], parsed.detail)) return RuleFailure(error, "Invalid tag detail scope.");
			parsed.detail = ToLowerAsciiCopy(parsed.detail);
			if (!InNames(parsed.detail, { "any", "normal", "detailed" })) return RuleFailure(error, "Unknown tag detail scope.");
		}
		if (value.HasMember("status"))
		{
			if (!ReadRuleString(value["status"], parsed.status)) return RuleFailure(error, "Invalid status scope.");
			parsed.status = ToLowerAsciiCopy(parsed.status);
			if (!IsRuleStatus(parsed.status)) return RuleFailure(error, "Unknown status scope.");
		}
		if (value.HasMember("statuses"))
		{
			const rapidjson::Value& statuses = value["statuses"];
			if (!statuses.IsArray() || statuses.Empty() || statuses.Size() > 32) return RuleFailure(error, "Status scope needs a bounded non-empty list.");
			for (const rapidjson::Value& item : statuses.GetArray())
			{
				std::string status;
				if (!ReadRuleString(item, status)) return RuleFailure(error, "Invalid status list value.");
				status = ToLowerAsciiCopy(status);
				if (!IsRuleStatus(status)) return RuleFailure(error, "Unknown status list value.");
				if (std::find(parsed.statuses.begin(), parsed.statuses.end(), status) == parsed.statuses.end()) parsed.statuses.push_back(status);
			}
		}
		unsigned nodes = 0;
		if (!value.HasMember("when") || !ParseRuleCondition(value["when"], parsed.when, 1, nodes, error))
			return value.HasMember("when") ? false : RuleFailure(error, "Rule needs a condition tree.");
		if (!value.HasMember("effects") || !value["effects"].IsArray() || value["effects"].Empty() || value["effects"].Size() > MaximumRuleEffects)
			return RuleFailure(error, "Rule needs a bounded non-empty effects list.");
		for (const rapidjson::Value& item : value["effects"].GetArray())
		{
			parsed.effects.emplace_back();
			if (!ParseRuleEffect(item, parsed.effects.back(), error)) return false;
		}
		outRule = std::move(parsed);
		return true;
	}

	bool WriteStructuredRuleV2(const StructuredTagColorRule& rule, rapidjson::Value& output,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!rule.usesStructuredCondition || rule.effects.empty() || rule.effects.size() > MaximumRuleEffects) return false;
		rapidjson::Value result(rapidjson::kObjectType);
		AddRuleString(result, "name", rule.name, allocator);
		result.AddMember("enabled", rule.enabled, allocator);
		result.AddMember("stop_processing", rule.stopProcessing, allocator);
		AddRuleString(result, "tag_type", rule.tagType, allocator);
		AddRuleString(result, "status", rule.status, allocator);
		AddRuleString(result, "detail", rule.detail, allocator);
		if (!rule.statuses.empty())
		{
			if (rule.statuses.size() > 32) return false;
			rapidjson::Value statuses(rapidjson::kArrayType);
			for (const std::string& item : rule.statuses)
			{
				rapidjson::Value status(item.c_str(), static_cast<rapidjson::SizeType>(item.size()), allocator);
				statuses.PushBack(status, allocator);
			}
			result.AddMember("statuses", statuses, allocator);
		}
		unsigned nodes = 0;
		rapidjson::Value when;
		if (!WriteRuleCondition(rule.when, when, allocator, 1, nodes)) return false;
		result.AddMember("when", when, allocator);
		rapidjson::Value effects(rapidjson::kArrayType);
		for (const RuleEffect& item : rule.effects)
		{
			rapidjson::Value effect(rapidjson::kObjectType);
			AddRuleString(effect, "type", item.type, allocator);
			if (item.type.rfind("field_", 0) == 0) AddRuleString(effect, "field", item.field, allocator);
			if (item.type == "field_bold" || item.type == "field_blink") effect.AddMember("value", item.value, allocator);
			else
			{
				rapidjson::Value color(rapidjson::kObjectType);
				color.AddMember("r", item.r, allocator); color.AddMember("g", item.g, allocator);
				color.AddMember("b", item.b, allocator); color.AddMember("a", item.a, allocator);
				effect.AddMember("color", color, allocator);
			}
			effects.PushBack(effect, allocator);
		}
		result.AddMember("effects", effects, allocator);
		StructuredTagColorRule checked;
		if (!TryParseStructuredRuleV2(result, checked)) return false;
		output = std::move(result);
		return true;
	}

	bool IsStructuredRuleClockSensitive(const StructuredTagColorRule& rule)
	{
		if (!rule.enabled) return false;
		if (rule.usesStructuredCondition)
		{
			for (const RuleEffect& effect : rule.effects)
				if (effect.type == "field_blink" && effect.value) return true;
			unsigned nodes = 0;
			return ConditionUsesClock(rule.when, 1, nodes);
		}
		if (!rule.criteria.empty())
		{
			for (const auto& criterion : rule.criteria)
				if (criterion.source == "cdm" && IsCdmTimeToken(criterion.token)) return true;
			return false;
		}
		return rule.source == "cdm" && IsCdmTimeToken(rule.token);
	}

	static bool StructuredRuleMatches(const StructuredTagColorRule& rule,
		const VsmrTags::TokenValues& replacingMap, const CdmPilotData* pilotData, std::time_t now)
	{
		if (rule.usesStructuredCondition)
		{
			unsigned nodes = 0;
			return !rule.effects.empty() && rule.effects.size() <= MaximumRuleEffects &&
				EvaluateRuleCondition(rule.when, replacingMap, pilotData, now, 1, nodes) == ConditionTruth::True;
		}
		if (!rule.criteria.empty())
		{
			for (const StructuredTagColorRule::Criterion& criterion : rule.criteria)
				if (!StructuredRuleCriterionMatches(criterion.source, criterion.token, criterion.condition, replacingMap, pilotData, now))
					return false;
			return true;
		}
		return StructuredRuleCriterionMatches(rule.source, rule.token, rule.condition, replacingMap, pilotData, now);
	}

	static TagColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const std::string& statusKey,
		const std::string& detailKey,
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now,
		bool inheritLegacyNormal)
	{
		if (now == 0) now = std::time(nullptr);
		TagColorRuleOverrides overrides;
		TagColorRuleOverrides legacyFallback;
		for (std::size_t index = 0; index < rules.size() && index < MaximumStructuredRules; ++index)
		{
			const StructuredTagColorRule& rule = rules[index];
			if (!rule.enabled) continue;
			const bool preferredEligible = StructuredRuleContextMatches(rule, tagTypeKey, statusKey, detailKey, replacingMap);
			const bool fallbackEligible = !preferredEligible && inheritLegacyNormal && detailKey == "detailed" &&
				!rule.usesStructuredCondition && StructuredRuleContextMatches(rule, tagTypeKey, statusKey, "normal", replacingMap);
			if (!preferredEligible && !fallbackEligible) continue;

			if (StructuredRuleMatches(rule, replacingMap, pilotData, now))
			{
				if (rule.usesStructuredCondition) ApplyRuleEffects(overrides, rule);
				else ApplyStructuredRuleColors(preferredEligible ? overrides : legacyFallback, rule);
				if (rule.stopProcessing) break;
			}
		}

		if (inheritLegacyNormal && detailKey == "detailed") MergeMissingColorRuleOverrides(overrides, legacyFallback);
		return overrides;
	}

	TagColorRuleOverrides EvaluateStructuredTagColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		bool isTagDetailed,
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now,
		bool inheritLegacyNormal)
	{
		const std::string statusKey = statusDefinitionKey != nullptr ? statusDefinitionKey : "default";
		const std::string detailKey = isTagDetailed ? "detailed" : "normal";
		return EvaluateStructuredTagColorRules(rules, tagTypeKey, statusKey, detailKey, replacingMap, pilotData, now, inheritLegacyNormal);
	}

	TagColorRuleOverrides EvaluateStructuredTargetColorRules(
		const std::vector<StructuredTagColorRule>& rules,
		const std::string& tagTypeKey,
		const char* statusDefinitionKey,
		const VsmrTags::TokenValues& replacingMap,
		const CdmPilotData* pilotData,
		std::time_t now)
	{
		if (now == 0) now = std::time(nullptr);
		const std::string statusKey = statusDefinitionKey != nullptr ? statusDefinitionKey : "default";
		TagColorRuleOverrides overrides;
		for (std::size_t index = 0; index < rules.size() && index < MaximumStructuredRules; ++index)
		{
			const StructuredTagColorRule& rule = rules[index];
			if (!rule.enabled || !StructuredRuleContextMatches(rule, tagTypeKey, statusKey, "normal", replacingMap,
				rule.usesStructuredCondition) || !StructuredRuleMatches(rule, replacingMap, pilotData, now)) continue;
			if (rule.usesStructuredCondition)
			{
				for (const RuleEffect& effect : rule.effects)
				{
					if (effect.type != "target_color") continue;
					overrides.hasTargetColor = true;
					overrides.targetR = effect.r; overrides.targetG = effect.g;
					overrides.targetB = effect.b; overrides.targetA = effect.a;
				}
			}
			else if (rule.applyTarget)
			{
				overrides.hasTargetColor = true;
				overrides.targetR = rule.targetR; overrides.targetG = rule.targetG;
				overrides.targetB = rule.targetB; overrides.targetA = rule.targetA;
			}
			// Stop-processing terminates the matching scope's entire rule pipeline,
			// not just rules that happen to assign this particular color channel.
			if (rule.stopProcessing) break;
		}
		return overrides;
	}

	TagColorRuleOverrides EvaluateRunwayColorRules(const std::vector<RunwayColorRuleDefinition>& rules, const VsmrTags::TokenValues& replacingMap)
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
