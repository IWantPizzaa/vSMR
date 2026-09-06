#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagColorRules.Internal.hpp"

#include "shared/TextUtils.hpp"
#include "tags/TagDefinitionUtils.hpp"

namespace VsmrTagColorRules
{
	namespace
	{
		bool IsCdmRuleTokenName(const std::string& tokenName)
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

		bool IsRunwayRuleTokenName(const std::string& tokenName)
		{
			const std::string lowered = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(tokenName));
			return lowered == "deprwy" ||
				lowered == "seprwy" ||
				lowered == "arvrwy" ||
				lowered == "srvrwy";
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
				if (ch == '"' || ch == '\'')
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
				const char first = trimmed.front();
				const char last = trimmed.back();
				if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
					return trimmed.substr(1, trimmed.size() - 2);
			}
			return trimmed;
		}

		bool TryParseRuleRgb(const std::string& value, int& outR, int& outG, int& outB)
		{
			const std::string content = StripWrappingQuotes(value);
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

		bool TrySplitColorRuleToken(
			const std::string& rawToken,
			std::string& outBaseToken,
			std::string& outCondition,
			std::string& outPayload)
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
				if (!TryParseRuleRgb(value, r, g, b))
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

		template <typename RuleType, typename Parser>
		void CollectColorRulesFromLineTexts(
			const std::vector<std::string>& lineTexts,
			std::vector<RuleType>& outRules,
			Parser parser)
		{
			for (const std::string& line : lineTexts)
			{
				const std::vector<std::string> tokens = SplitDefinitionTokens(line);
				for (const std::string& rawToken : tokens)
				{
					const DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
					const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;
					RuleType parsedRule;
					if (parser(baseToken, parsedRule))
						outRules.push_back(parsedRule);
				}
			}
		}
	}

	// Parse color-rule token syntax:
	// token(state_<name>=[target|tag|text, color=(r,g,b), color_target=(r,g,b), ...]).
	bool TryParseCdmColorRuleToken(const std::string& rawToken, CdmColorRuleDefinition& outRule)
	{
		outRule = CdmColorRuleDefinition();

		std::string baseToken;
		std::string condition;
		std::string payload;
		if (!TrySplitColorRuleToken(rawToken, baseToken, condition, payload))
			return false;
		if (!IsCdmRuleTokenName(baseToken))
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

		const std::string runwayCondition =
			Internal::NormalizeRunwayRuleConditionName(condition);
		if (runwayCondition.empty())
			return false;

		if (!TryParseColorRuleChannels(payload, outRule))
			return false;

		outRule.token = baseToken;
		outRule.expectedRunway = runwayCondition;
		return true;
	}

	void CollectCdmColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<CdmColorRuleDefinition>& outRules)
	{
		CollectColorRulesFromLineTexts(lineTexts, outRules, TryParseCdmColorRuleToken);
	}

	void CollectRunwayColorRulesFromLineTexts(
		const std::vector<std::string>& lineTexts,
		std::vector<RunwayColorRuleDefinition>& outRules)
	{
		CollectColorRulesFromLineTexts(lineTexts, outRules, TryParseRunwayColorRuleToken);
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
}
