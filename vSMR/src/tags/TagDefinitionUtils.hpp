#pragma once

#include "platform/windows/GdiColorUtils.hpp"
#include "shared/TextUtils.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

inline static std::vector<std::string> SplitDefinitionTokens(const std::string& text)
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

inline static std::string TrimAsciiWhitespace(const std::string& text)
{
	return TrimAsciiWhitespaceCopy(text);
}

inline static bool TryParseDefinitionTokenColorSuffix(const std::string& token, std::string& outBaseToken, int& outR, int& outG, int& outB)
{
	outBaseToken = "";
	outR = 255;
	outG = 255;
	outB = 255;

	const std::string trimmedToken = TrimAsciiWhitespaceCopy(token);
	if (trimmedToken.empty())
		return false;

	const size_t openPos = trimmedToken.rfind('(');
	const size_t closePos = trimmedToken.rfind(')');
	if (openPos == std::string::npos || closePos == std::string::npos || closePos != trimmedToken.size() - 1 || closePos <= openPos + 1)
		return false;

	const std::string baseToken = TrimAsciiWhitespaceCopy(trimmedToken.substr(0, openPos));
	if (baseToken.empty())
		return false;

	// Keep clearance(...) syntax reserved for clearance display customization.
	const std::string loweredBaseToken = ToLowerAsciiCopy(baseToken);
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

inline static DefinitionTokenStyleData ParseDefinitionTokenStyle(const std::string& rawToken)
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
		const std::string lowered = ToLowerAsciiCopy(rawToken);
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

inline static std::string ApplyDefinitionTokenStyle(const std::string& token, bool makeBold)
{
	if (!makeBold || token.empty())
		return token;
	return "b:" + token;
}

inline static bool IsClearanceTokenName(const std::string& tokenName)
{
	const std::string lowered = ToLowerAsciiCopy(tokenName);
	return lowered == "clearance" || lowered == "cleared";
}

inline static bool TryParseClearanceTokenDisplay(const std::string& rawToken, std::string& notClearedText, std::string& clearedText)
{
	notClearedText = "[ ]";
	clearedText = "[x]";

	const std::string token = TrimAsciiWhitespaceCopy(rawToken);
	if (token.empty())
		return false;

	const size_t openPos = token.find('(');
	if (openPos == std::string::npos)
		return IsClearanceTokenName(token);

	const size_t closePos = token.rfind(')');
	if (closePos == std::string::npos || closePos <= openPos || closePos != token.size() - 1)
		return false;

	const std::string tokenName = TrimAsciiWhitespaceCopy(token.substr(0, openPos));
	if (!IsClearanceTokenName(tokenName))
		return false;

	const std::string args = token.substr(openPos + 1, closePos - openPos - 1);
	if (TrimAsciiWhitespaceCopy(args).empty())
	{
		// clearance() => both states hidden
		notClearedText.clear();
		clearedText.clear();
		return true;
	}

	const size_t commaPos = args.find(',');
	if (commaPos == std::string::npos)
	{
		const std::string customNotCleared = TrimAsciiWhitespaceCopy(args);
		if (!customNotCleared.empty())
			notClearedText = customNotCleared;
		else
			notClearedText.clear();

		// second item missing => hide token when cleared
		clearedText.clear();
		return true;
	}

	const std::string customNotCleared = TrimAsciiWhitespaceCopy(args.substr(0, commaPos));
	const std::string customCleared = TrimAsciiWhitespaceCopy(args.substr(commaPos + 1));
	// each side can be intentionally empty to hide its respective state.
	notClearedText = customNotCleared;
	clearedText = customCleared;

	return true;
}

inline static bool IsClearanceDefinitionToken(const std::string& rawToken)
{
	std::string notClearedText;
	std::string clearedText;
	return TryParseClearanceTokenDisplay(rawToken, notClearedText, clearedText);
}
