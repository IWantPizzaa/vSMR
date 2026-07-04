#pragma once

#include "SMRColorUtils.hpp"
#include "SMRGroundState.hpp"
#include "SMRProfileColorPaths.hpp"
#include "SMRTextUtils.hpp"
#include "SMRVacdmTagHelpers.hpp"

namespace
{
	std::vector<std::string> SplitDefinitionTokens(const std::string& text)
	{
		std::vector<std::string> tokens;
		std::string token;
		int parenDepth = 0;

		auto flushToken = [&]()
		{
			if (!token.empty())
			{
				tokens.push_back(token);
				token.clear();
			}
		};

		for (char c : text)
		{
			if (c == '(')
			{
				++parenDepth;
				token.push_back(c);
				continue;
			}
			if (c == ')')
			{
				if (parenDepth > 0)
					--parenDepth;
				token.push_back(c);
				continue;
			}

			const bool isSeparatorOutsideParens =
				(parenDepth == 0) &&
				(std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';' || c == '|');
			if (isSeparatorOutsideParens)
			{
				flushToken();
				continue;
			}

			token.push_back(c);
		}

		flushToken();
		return tokens;
	}

	struct DefinitionTokenStyleData
	{
		std::string token;
		bool bold = false;
		bool hasCustomColor = false;
		int colorR = 255;
		int colorG = 255;
		int colorB = 255;
	};

	bool TryParseDefinitionTokenColorSuffix(const std::string& token, std::string& outBaseToken, int& outR, int& outG, int& outB);

	DefinitionTokenStyleData ParseDefinitionTokenStyle(const std::string& rawToken)
	{
		DefinitionTokenStyleData parsed;
		parsed.token = rawToken;
		if (rawToken.empty())
			return parsed;

		if (rawToken[0] == '*')
		{
			parsed.bold = true;
			parsed.token = rawToken.substr(1);
		}
		else
		{
			std::string lowered = rawToken;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered.rfind("b:", 0) == 0)
			{
				parsed.bold = true;
				parsed.token = rawToken.substr(2);
			}
			else if (lowered.rfind("bold:", 0) == 0)
			{
				parsed.bold = true;
				parsed.token = rawToken.substr(5);
			}
		}

		std::string baseToken;
		int colorR = 255;
		int colorG = 255;
		int colorB = 255;
		if (TryParseDefinitionTokenColorSuffix(parsed.token, baseToken, colorR, colorG, colorB))
		{
			parsed.token = baseToken;
			parsed.hasCustomColor = true;
			parsed.colorR = colorR;
			parsed.colorG = colorG;
			parsed.colorB = colorB;
		}

		return parsed;
	}

	std::string ApplyDefinitionTokenStyle(const std::string& token, bool makeBold)
	{
		if (!makeBold || token.empty())
			return token;
		return "b:" + token;
	}

	std::string TrimAsciiWhitespace(const std::string& text)
	{
		size_t begin = 0;
		size_t end = text.size();
		while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
			++begin;
		while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
			--end;
		return text.substr(begin, end - begin);
	}

	bool TryParseDefinitionTokenColorSuffix(const std::string& token, std::string& outBaseToken, int& outR, int& outG, int& outB)
	{
		outBaseToken = "";
		outR = 255;
		outG = 255;
		outB = 255;

		const std::string trimmedToken = TrimAsciiWhitespace(token);
		if (trimmedToken.empty())
			return false;

		const size_t openPos = trimmedToken.rfind('(');
		const size_t closePos = trimmedToken.rfind(')');
		if (openPos == std::string::npos || closePos == std::string::npos || closePos != trimmedToken.size() - 1 || closePos <= openPos + 1)
			return false;

		const std::string baseToken = TrimAsciiWhitespace(trimmedToken.substr(0, openPos));
		if (baseToken.empty())
			return false;

		// Keep clearance(...) syntax reserved for clearance display customization.
		std::string loweredBaseToken = baseToken;
		std::transform(loweredBaseToken.begin(), loweredBaseToken.end(), loweredBaseToken.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (loweredBaseToken == "clearance" || loweredBaseToken == "cleared")
			return false;

		const std::string args = trimmedToken.substr(openPos + 1, closePos - openPos - 1);
		if (args.empty())
			return false;

		for (char c : args)
		{
			if (!(std::isdigit(static_cast<unsigned char>(c)) || std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '+' || c == '-'))
				return false;
		}

		const std::vector<int> values = ExtractIntegers(args);
		if (values.size() != 3)
			return false;

		for (int value : values)
		{
			if (value < 0 || value > 255)
				return false;
		}

		outBaseToken = baseToken;
		outR = values[0];
		outG = values[1];
		outB = values[2];
		return true;
	}

	bool IsClearanceTokenName(const std::string& tokenName)
	{
		std::string lowered = tokenName;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return lowered == "clearance" || lowered == "cleared";
	}

	bool TryParseClearanceTokenDisplay(const std::string& rawToken, std::string& notClearedText, std::string& clearedText)
	{
		notClearedText = "[ ]";
		clearedText = "[x]";

		const std::string token = TrimAsciiWhitespace(rawToken);
		if (token.empty())
			return false;

		const size_t openPos = token.find('(');
		if (openPos == std::string::npos)
			return IsClearanceTokenName(token);

		const size_t closePos = token.rfind(')');
		if (closePos == std::string::npos || closePos <= openPos || closePos != token.size() - 1)
			return false;

		const std::string tokenName = TrimAsciiWhitespace(token.substr(0, openPos));
		if (!IsClearanceTokenName(tokenName))
			return false;

		const std::string args = token.substr(openPos + 1, closePos - openPos - 1);
		if (TrimAsciiWhitespace(args).empty())
		{
			// clearance() => both states hidden
			notClearedText.clear();
			clearedText.clear();
			return true;
		}

		const size_t commaPos = args.find(',');
		if (commaPos == std::string::npos)
		{
			const std::string customNotCleared = TrimAsciiWhitespace(args);
			if (!customNotCleared.empty())
				notClearedText = customNotCleared;
			else
				notClearedText.clear();

			// second item missing => hide token when cleared
			clearedText.clear();
			return true;
		}

		const std::string customNotCleared = TrimAsciiWhitespace(args.substr(0, commaPos));
		const std::string customCleared = TrimAsciiWhitespace(args.substr(commaPos + 1));
		// each side can be intentionally empty to hide its respective state.
		notClearedText = customNotCleared;
		clearedText = customCleared;

		return true;
	}

	bool IsClearanceDefinitionToken(const std::string& rawToken)
	{
		std::string notClearedText;
		std::string clearedText;
		return TryParseClearanceTokenDisplay(rawToken, notClearedText, clearedText);
	}

	struct VacdmColorRuleDefinition
	{
		std::string token;
		std::string expectedState;
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

	struct VacdmColorRuleOverrides
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

	struct RunwayColorRuleDefinition
	{
		std::string token;
		std::string expectedRunway;
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

	// Parse color-rule token syntax:
	// token(state_<name>=[target|tag|text, color=(r,g,b), color_target=(r,g,b), ...]).
	bool TryParseVacdmColorRuleToken(const std::string& rawToken, VacdmColorRuleDefinition& outRule)
	{
		outRule = VacdmColorRuleDefinition();

		const std::string token = TrimAsciiWhitespaceCopy(rawToken);
		if (token.empty())
			return false;

		const size_t openPos = token.find('(');
		const size_t closePos = token.rfind(')');
		if (openPos == std::string::npos || closePos == std::string::npos || closePos <= openPos || closePos != token.size() - 1)
			return false;

		const std::string baseToken = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(token.substr(0, openPos)));
		if (!IsVacdmRuleTokenName(baseToken))
			return false;

		const std::string expression = TrimAsciiWhitespaceCopy(token.substr(openPos + 1, closePos - openPos - 1));
		const size_t eqPos = expression.find('=');
		if (eqPos == std::string::npos)
			return false;

		std::string lhs = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(expression.substr(0, eqPos)));
		if (lhs.rfind("state_", 0) != 0 || lhs.size() <= 6)
			return false;
		std::string stateName = lhs.substr(6);
		if (stateName.empty())
			return false;

		std::string rhs = TrimAsciiWhitespaceCopy(expression.substr(eqPos + 1));
		if (rhs.size() < 2 || rhs.front() != '[' || rhs.back() != ']')
			return false;
		rhs = TrimAsciiWhitespaceCopy(rhs.substr(1, rhs.size() - 2));
		if (rhs.empty())
			return false;

		bool scopeTargetRequested = false;
		bool scopeTagRequested = false;
		bool scopeTextRequested = false;
		bool hasSharedColor = false;
		int sharedR = 255;
		int sharedG = 255;
		int sharedB = 255;

		const std::vector<std::string> items = SplitCommaSeparatedItems(rhs);
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

			int r = 0, g = 0, b = 0;
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
				outRule.hasTargetColor = true;
				outRule.targetR = r;
				outRule.targetG = g;
				outRule.targetB = b;
				continue;
			}
			if (key == "color_tag")
			{
				scopeTagRequested = true;
				outRule.hasTagColor = true;
				outRule.tagR = r;
				outRule.tagG = g;
				outRule.tagB = b;
				continue;
			}
			if (key == "color_text")
			{
				scopeTextRequested = true;
				outRule.hasTextColor = true;
				outRule.textR = r;
				outRule.textG = g;
				outRule.textB = b;
				continue;
			}
		}

		if (scopeTargetRequested && !outRule.hasTargetColor && hasSharedColor)
		{
			outRule.hasTargetColor = true;
			outRule.targetR = sharedR;
			outRule.targetG = sharedG;
			outRule.targetB = sharedB;
		}
		if (scopeTagRequested && !outRule.hasTagColor && hasSharedColor)
		{
			outRule.hasTagColor = true;
			outRule.tagR = sharedR;
			outRule.tagG = sharedG;
			outRule.tagB = sharedB;
		}
		if (scopeTextRequested && !outRule.hasTextColor && hasSharedColor)
		{
			outRule.hasTextColor = true;
			outRule.textR = sharedR;
			outRule.textG = sharedG;
			outRule.textB = sharedB;
		}

		if (scopeTargetRequested && !outRule.hasTargetColor)
			return false;
		if (scopeTagRequested && !outRule.hasTagColor)
			return false;
		if (scopeTextRequested && !outRule.hasTextColor)
			return false;
		if (!outRule.hasTargetColor && !outRule.hasTagColor && !outRule.hasTextColor)
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

	// Collect only vACDM color-rule tokens from tag definition lines.
	void CollectVacdmColorRulesFromLineTexts(const std::vector<std::string>& lineTexts, std::vector<VacdmColorRuleDefinition>& outRules)
	{
		for (const std::string& line : lineTexts)
		{
			const std::vector<std::string> tokens = SplitDefinitionTokens(line);
			for (const std::string& rawToken : tokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;
				VacdmColorRuleDefinition parsedRule;
				if (TryParseVacdmColorRuleToken(baseToken, parsedRule))
					outRules.push_back(parsedRule);
			}
		}
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
		VacdmColorRuleOverrides overrides;
		for (const VacdmColorRuleDefinition& rule : rules)
		{
			const std::string actualState = ResolveVacdmRuleStateName(rule.token, pilotData);
			if (!VacdmRuleStateMatches(rule.expectedState, actualState))
				continue;

			if (rule.hasTargetColor)
			{
				overrides.hasTargetColor = true;
				overrides.targetR = rule.targetR;
				overrides.targetG = rule.targetG;
				overrides.targetB = rule.targetB;
				overrides.targetA = rule.targetA;
			}
			if (rule.hasTagColor)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
				overrides.tagA = rule.tagA;
			}
			if (rule.hasTextColor)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
				overrides.textA = rule.textA;
			}
		}
		return overrides;
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

		const std::string token = TrimAsciiWhitespaceCopy(rawToken);
		if (token.empty())
			return false;

		const size_t openPos = token.find('(');
		const size_t closePos = token.rfind(')');
		if (openPos == std::string::npos || closePos == std::string::npos || closePos <= openPos || closePos != token.size() - 1)
			return false;

		const std::string baseToken = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(token.substr(0, openPos)));
		if (!IsRunwayRuleTokenName(baseToken))
			return false;

		const std::string expression = TrimAsciiWhitespaceCopy(token.substr(openPos + 1, closePos - openPos - 1));
		const size_t eqPos = expression.find('=');
		if (eqPos == std::string::npos)
			return false;

		const std::string lhsRaw = TrimAsciiWhitespaceCopy(expression.substr(0, eqPos));
		const std::string runwayCondition = NormalizeRunwayRuleConditionName(lhsRaw);
		if (runwayCondition.empty())
			return false;

		std::string rhs = TrimAsciiWhitespaceCopy(expression.substr(eqPos + 1));
		if (rhs.size() < 2 || rhs.front() != '[' || rhs.back() != ']')
			return false;
		rhs = TrimAsciiWhitespaceCopy(rhs.substr(1, rhs.size() - 2));
		if (rhs.empty())
			return false;

		bool scopeTargetRequested = false;
		bool scopeTagRequested = false;
		bool scopeTextRequested = false;
		bool hasSharedColor = false;
		int sharedR = 255;
		int sharedG = 255;
		int sharedB = 255;

		const std::vector<std::string> items = SplitCommaSeparatedItems(rhs);
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
				outRule.hasTargetColor = true;
				outRule.targetR = r;
				outRule.targetG = g;
				outRule.targetB = b;
				continue;
			}
			if (key == "color_tag")
			{
				scopeTagRequested = true;
				outRule.hasTagColor = true;
				outRule.tagR = r;
				outRule.tagG = g;
				outRule.tagB = b;
				continue;
			}
			if (key == "color_text")
			{
				scopeTextRequested = true;
				outRule.hasTextColor = true;
				outRule.textR = r;
				outRule.textG = g;
				outRule.textB = b;
				continue;
			}
		}

		if (scopeTargetRequested && !outRule.hasTargetColor && hasSharedColor)
		{
			outRule.hasTargetColor = true;
			outRule.targetR = sharedR;
			outRule.targetG = sharedG;
			outRule.targetB = sharedB;
		}
		if (scopeTagRequested && !outRule.hasTagColor && hasSharedColor)
		{
			outRule.hasTagColor = true;
			outRule.tagR = sharedR;
			outRule.tagG = sharedG;
			outRule.tagB = sharedB;
		}
		if (scopeTextRequested && !outRule.hasTextColor && hasSharedColor)
		{
			outRule.hasTextColor = true;
			outRule.textR = sharedR;
			outRule.textG = sharedG;
			outRule.textB = sharedB;
		}

		if (scopeTargetRequested && !outRule.hasTargetColor)
			return false;
		if (scopeTagRequested && !outRule.hasTagColor)
			return false;
		if (scopeTextRequested && !outRule.hasTextColor)
			return false;
		if (!outRule.hasTargetColor && !outRule.hasTagColor && !outRule.hasTextColor)
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

	void CollectRunwayColorRulesFromLineTexts(const std::vector<std::string>& lineTexts, std::vector<RunwayColorRuleDefinition>& outRules)
	{
		for (const std::string& line : lineTexts)
		{
			const std::vector<std::string> tokens = SplitDefinitionTokens(line);
			for (const std::string& rawToken : tokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;
				RunwayColorRuleDefinition parsedRule;
				if (TryParseRunwayColorRuleToken(baseToken, parsedRule))
					outRules.push_back(parsedRule);
			}
		}
	}

	VacdmColorRuleOverrides EvaluateRunwayColorRules(const std::vector<RunwayColorRuleDefinition>& rules, const std::map<std::string, std::string>& replacingMap)
	{
		VacdmColorRuleOverrides overrides;
		for (const RunwayColorRuleDefinition& rule : rules)
		{
			std::string actualRunway;
			auto it = replacingMap.find(rule.token);
			if (it != replacingMap.end())
				actualRunway = it->second;

			if (!RunwayRuleConditionMatches(rule.expectedRunway, actualRunway))
				continue;

			if (rule.hasTargetColor)
			{
				overrides.hasTargetColor = true;
				overrides.targetR = rule.targetR;
				overrides.targetG = rule.targetG;
				overrides.targetB = rule.targetB;
				overrides.targetA = rule.targetA;
			}
			if (rule.hasTagColor)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
				overrides.tagA = rule.tagA;
			}
			if (rule.hasTextColor)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
				overrides.textA = rule.textA;
			}
		}
		return overrides;
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

	std::string JoinStringList(const std::vector<std::string>& parts, const std::string& separator)
	{
		std::string out;
		for (size_t i = 0; i < parts.size(); ++i)
		{
			if (i != 0)
				out += separator;
			out += parts[i];
		}
		return out;
	}
}
