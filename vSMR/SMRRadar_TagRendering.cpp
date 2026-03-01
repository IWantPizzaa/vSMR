#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern int LeaderLineDefaultlenght;

enum class GroundStateCategory { Unknown, Gate, Push, Stup, Taxi, Nsts, Depa, Arr };

static GroundStateCategory classifyGroundState(const std::string& rawState, int reportedGs, bool onRunway)
{
	std::string normalized;
	normalized.reserve(rawState.size());
	for (char c : rawState) {
		if (c == ' ')
			continue;
		normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}

	if (normalized.find("NSTS") != std::string::npos)
		return GroundStateCategory::Nsts;

	if (normalized.find("DEPA") != std::string::npos)
		return GroundStateCategory::Depa;

	if (normalized.find("ARR") != std::string::npos)
		return GroundStateCategory::Arr;

	if (normalized.find("STUP") != std::string::npos || normalized.find("STARTUP") != std::string::npos || normalized == "S/U" || normalized == "SU")
		return GroundStateCategory::Stup;

	if (normalized.find("PUSH") != std::string::npos || normalized.find("P/B") != std::string::npos || normalized == "PB" || normalized == "P/B")
		return GroundStateCategory::Push;

	if (normalized.find("TAX") != std::string::npos || normalized == "TXI")
		return GroundStateCategory::Taxi;

	if (normalized.find("GATE") != std::string::npos || normalized.find("STAND") != std::string::npos || normalized.find("PARK") != std::string::npos || normalized.find("STBY") != std::string::npos)
		return GroundStateCategory::Gate;

	if (normalized.empty() && reportedGs < 2 && !onRunway)
		return GroundStateCategory::Gate;

	return GroundStateCategory::Unknown;
}

namespace
{
	std::string ToUpperAsciiCopy(const std::string& value)
	{
		std::string normalized = value;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
			});
		return normalized;
	}

	std::string ToLowerAsciiCopy(const std::string& value)
	{
		std::string normalized = value;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
			});
		return normalized;
	}

	std::string TrimAsciiWhitespaceCopy(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;
		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	}

	std::string NormalizeVacdmLookupCallsign(const char* rawCallsign)
	{
		if (rawCallsign == NULL)
			return "";

		std::string callsign = TrimAsciiWhitespaceCopy(rawCallsign);
		if (callsign.empty())
			return "";

		size_t slashPos = callsign.find('/');
		if (slashPos != std::string::npos)
			callsign = callsign.substr(0, slashPos);

		return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	}

	bool TryGetVacdmPilotDataForTarget(const CRadarTarget& rt, const CFlightPlan& fp, VacdmPilotData& outData)
	{
		const std::string fpCallsign = NormalizeVacdmLookupCallsign(fp.IsValid() ? fp.GetCallsign() : NULL);
		if (!fpCallsign.empty() && TryGetVacdmPilotData(fpCallsign, outData))
			return true;

		const std::string radarCallsign = NormalizeVacdmLookupCallsign(rt.GetCallsign());
		if (!radarCallsign.empty() && radarCallsign != fpCallsign && TryGetVacdmPilotData(radarCallsign, outData))
			return true;

		return false;
	}

	std::string FormatVacdmTimeToken(std::time_t utcTime)
	{
		if (utcTime <= 0)
			return "";

		std::tm tmUtc = {};
		if (gmtime_s(&tmUtc, &utcTime) != 0)
			return "";

		char buffer[5] = {};
		if (std::strftime(buffer, sizeof(buffer), "%H%M", &tmUtc) != 4)
			return "";
		return std::string(buffer);
	}

	std::string NormalizeHhmmToken(const std::string& text)
	{
		std::string digits;
		for (char c : text)
		{
			if (std::isdigit(static_cast<unsigned char>(c)))
				digits.push_back(c);
		}
		if (digits.empty())
			return "";
		if (digits.size() > 4)
			digits = digits.substr(0, 4);
		while (digits.size() < 4)
			digits.insert(digits.begin(), '0');
		return digits;
	}

	bool TryResolveVacdmTobtTextColor(const VacdmPilotData& pilot, int& outR, int& outG, int& outB)
	{
		if (!pilot.hasTobt)
			return false;

		const COLORREF lightGreen = RGB(127, 252, 73);
		const COLORREF green = RGB(0, 181, 27);
		const COLORREF lightYellow = RGB(255, 255, 191);
		const COLORREF yellow = RGB(255, 255, 0);
		const COLORREF orange = RGB(255, 153, 0);
		const COLORREF grey = RGB(153, 153, 153);
		const COLORREF debug = RGB(255, 0, 255);

		const auto setColor = [&](COLORREF color) {
			outR = GetRValue(color);
			outG = GetGValue(color);
			outB = GetBValue(color);
			};

		if (!pilot.hasTsat || pilot.hasAsat)
		{
			setColor(grey);
			return true;
		}

		std::time_t now = std::time(nullptr);
		const long long timeSinceTobt = static_cast<long long>(std::difftime(now, pilot.tobtUtc));
		const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));
		const long long diffTsatTobt = static_cast<long long>(std::difftime(pilot.tsatUtc, pilot.tobtUtc));
		const std::string tobtState = ToUpperAsciiCopy(pilot.tobtState);

		if ((timeSinceTobt > 0 && (timeSinceTsat >= 5 * 60 || !pilot.hasTsat)) ||
			pilot.tobtUtc >= now + 60 * 60)
		{
			setColor(orange);
			return true;
		}

		if (diffTsatTobt >= 5 * 60 && (tobtState == "GUESS" || tobtState == "FLIGHTPLAN"))
		{
			setColor(lightYellow);
			return true;
		}

		if (diffTsatTobt >= 5 * 60 && tobtState == "CONFIRMED")
		{
			setColor(yellow);
			return true;
		}

		if (diffTsatTobt < 5 * 60 && tobtState == "CONFIRMED")
		{
			setColor(green);
			return true;
		}

		if (tobtState != "CONFIRMED")
		{
			setColor(lightGreen);
			return true;
		}

		setColor(debug);
		return true;
	}

	bool TryResolveVacdmTsatTextColor(const VacdmPilotData& pilot, int& outR, int& outG, int& outB)
	{
		if (!pilot.hasTsat)
			return false;

		const COLORREF lightGreen = RGB(127, 252, 73);
		const COLORREF lightBlue = RGB(53, 218, 235);
		const COLORREF green = RGB(0, 181, 27);
		const COLORREF blue = RGB(0, 0, 255);
		const COLORREF orange = RGB(255, 153, 0);
		const COLORREF red = RGB(255, 0, 0);
		const COLORREF grey = RGB(153, 153, 153);
		const COLORREF debug = RGB(255, 0, 255);

		const auto setColor = [&](COLORREF color) {
			outR = GetRValue(color);
			outG = GetGValue(color);
			outB = GetBValue(color);
			};

		if (pilot.hasAsat)
		{
			setColor(grey);
			return true;
		}

		std::time_t now = std::time(nullptr);
		const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));

		if (timeSinceTsat <= 5 * 60 && timeSinceTsat >= -5 * 60)
		{
			setColor(pilot.hasCtot ? blue : green);
			return true;
		}

		if (timeSinceTsat < -5 * 60)
		{
			setColor(pilot.hasCtot ? lightBlue : lightGreen);
			return true;
		}

		if (timeSinceTsat > 5 * 60)
		{
			setColor(pilot.hasCtot ? red : orange);
			return true;
		}

		setColor(debug);
		return true;
	}

	struct ProfileColorPathToken
	{
		enum class Type { Key, Index } type = Type::Key;
		std::string key;
		rapidjson::SizeType index = 0;
	};

	char NormalizeProfileColorComponent(char component)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(component)));
	}

	bool IsColorConfigObject(const rapidjson::Value& value, bool* hasAlphaOut = nullptr)
	{
		if (!value.IsObject())
			return false;

		auto hasIntMember = [&](const char* key) {
			return value.HasMember(key) && value[key].IsInt();
		};

		if (!hasIntMember("r") || !hasIntMember("g") || !hasIntMember("b"))
			return false;

		bool hasAlpha = value.HasMember("a") && value["a"].IsInt();
		if (value.HasMember("a") && !value["a"].IsInt())
			return false;

		if (hasAlphaOut)
			*hasAlphaOut = hasAlpha;

		return true;
	}

	void CollectProfileColorPaths(const rapidjson::Value& value, const std::string& path, std::vector<std::string>& outPaths, std::map<std::string, bool>& outHasAlpha)
	{
		bool hasAlpha = false;
		if (!path.empty() && IsColorConfigObject(value, &hasAlpha))
		{
			outPaths.push_back(path);
			outHasAlpha[path] = hasAlpha;
		}

		if (value.IsObject())
		{
			for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
			{
				const std::string key = member->name.GetString();
				const std::string childPath = path.empty() ? key : path + "." + key;
				CollectProfileColorPaths(member->value, childPath, outPaths, outHasAlpha);
			}
			return;
		}

		if (value.IsArray())
		{
			for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
			{
				const std::string childPath = path + "[" + std::to_string(i) + "]";
				CollectProfileColorPaths(value[i], childPath, outPaths, outHasAlpha);
			}
		}
	}

	std::vector<ProfileColorPathToken> ParseProfileColorPath(const std::string& path)
	{
		std::vector<ProfileColorPathToken> tokens;
		if (path.empty())
			return tokens;

		std::string key;
		for (size_t i = 0; i < path.size();)
		{
			const char ch = path[i];

			if (ch == '.')
			{
				if (!key.empty())
				{
					ProfileColorPathToken token;
					token.type = ProfileColorPathToken::Type::Key;
					token.key = key;
					tokens.push_back(token);
					key.clear();
				}
				++i;
				continue;
			}

			if (ch == '[')
			{
				if (!key.empty())
				{
					ProfileColorPathToken token;
					token.type = ProfileColorPathToken::Type::Key;
					token.key = key;
					tokens.push_back(token);
					key.clear();
				}

				const size_t closePos = path.find(']', i + 1);
				if (closePos == std::string::npos || closePos == i + 1)
					return std::vector<ProfileColorPathToken>();

				rapidjson::SizeType index = 0;
				for (size_t j = i + 1; j < closePos; ++j)
				{
					unsigned char digit = static_cast<unsigned char>(path[j]);
					if (!std::isdigit(digit))
						return std::vector<ProfileColorPathToken>();

					index = static_cast<rapidjson::SizeType>(index * 10 + (path[j] - '0'));
				}

				ProfileColorPathToken token;
				token.type = ProfileColorPathToken::Type::Index;
				token.index = index;
				tokens.push_back(token);
				i = closePos + 1;
				continue;
			}

			key.push_back(ch);
			++i;
		}

		if (!key.empty())
		{
			ProfileColorPathToken token;
			token.type = ProfileColorPathToken::Type::Key;
			token.key = key;
			tokens.push_back(token);
		}

		return tokens;
	}

	rapidjson::Value* ResolveProfilePath(rapidjson::Value& root, const std::vector<ProfileColorPathToken>& tokens)
	{
		rapidjson::Value* current = &root;

		for (const auto& token : tokens)
		{
			if (token.type == ProfileColorPathToken::Type::Key)
			{
				if (!current->IsObject())
					return nullptr;

				if (!current->HasMember(token.key.c_str()))
					return nullptr;

				current = &(*current)[token.key.c_str()];
				continue;
			}

			if (!current->IsArray() || token.index >= current->Size())
				return nullptr;

			current = &(*current)[token.index];
		}

		return current;
	}

	const char* ColorComponentKey(char component)
	{
		switch (NormalizeProfileColorComponent(component))
		{
		case 'r': return "r";
		case 'g': return "g";
		case 'b': return "b";
		case 'a': return "a";
		default: return nullptr;
		}
	}

	double ClampDouble(double value, double minValue, double maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	int ClampInt(int value, int minValue, int maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	void RgbToHsv(int r, int g, int b, double& h, double& s, double& v)
	{
		double rf = ClampDouble(r / 255.0, 0.0, 1.0);
		double gf = ClampDouble(g / 255.0, 0.0, 1.0);
		double bf = ClampDouble(b / 255.0, 0.0, 1.0);

		double cmax = max(rf, max(gf, bf));
		double cmin = min(rf, min(gf, bf));
		double delta = cmax - cmin;

		h = 0.0;
		if (delta > 1e-9)
		{
			if (cmax == rf)
				h = 60.0 * fmod(((gf - bf) / delta), 6.0);
			else if (cmax == gf)
				h = 60.0 * (((bf - rf) / delta) + 2.0);
			else
				h = 60.0 * (((rf - gf) / delta) + 4.0);
		}
		if (h < 0.0)
			h += 360.0;

		s = (cmax <= 1e-9) ? 0.0 : (delta / cmax);
		v = cmax;
	}

	Gdiplus::Color HsvToColor(double h, double s, double v, int a = 255)
	{
		double hue = fmod(h, 360.0);
		if (hue < 0.0)
			hue += 360.0;
		double sat = ClampDouble(s, 0.0, 1.0);
		double val = ClampDouble(v, 0.0, 1.0);

		double c = val * sat;
		double x = c * (1.0 - fabs(fmod(hue / 60.0, 2.0) - 1.0));
		double m = val - c;

		double rf = 0.0, gf = 0.0, bf = 0.0;
		if (hue < 60.0) {
			rf = c; gf = x; bf = 0.0;
		}
		else if (hue < 120.0) {
			rf = x; gf = c; bf = 0.0;
		}
		else if (hue < 180.0) {
			rf = 0.0; gf = c; bf = x;
		}
		else if (hue < 240.0) {
			rf = 0.0; gf = x; bf = c;
		}
		else if (hue < 300.0) {
			rf = x; gf = 0.0; bf = c;
		}
		else {
			rf = c; gf = 0.0; bf = x;
		}

		int r = ClampInt(static_cast<int>((rf + m) * 255.0 + 0.5), 0, 255);
		int g = ClampInt(static_cast<int>((gf + m) * 255.0 + 0.5), 0, 255);
		int b = ClampInt(static_cast<int>((bf + m) * 255.0 + 0.5), 0, 255);
		int alpha = ClampInt(a, 0, 255);
		return Gdiplus::Color(alpha, r, g, b);
	}

	std::vector<int> ExtractIntegers(const std::string& text)
	{
		std::vector<int> values;
		int sign = 1;
		int number = 0;
		bool inNumber = false;

		for (size_t i = 0; i < text.size(); ++i)
		{
			const char c = text[i];
			if (c == '-' && !inNumber)
			{
				sign = -1;
				continue;
			}
			if (std::isdigit(static_cast<unsigned char>(c)))
			{
				inNumber = true;
				number = number * 10 + (c - '0');
				continue;
			}

			if (inNumber)
			{
				values.push_back(sign * number);
				number = 0;
				sign = 1;
				inNumber = false;
			}
			else
			{
				sign = 1;
			}
		}

		if (inNumber)
			values.push_back(sign * number);

		return values;
	}

	bool TryParseHexByte(const std::string& text, size_t offset, int& outValue)
	{
		if (offset + 1 >= text.size())
			return false;

		auto hexValue = [](char ch) -> int
		{
			unsigned char c = static_cast<unsigned char>(ch);
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'A' && c <= 'F')
				return 10 + (c - 'A');
			if (c >= 'a' && c <= 'f')
				return 10 + (c - 'a');
			return -1;
		};

		int hi = hexValue(text[offset]);
		int lo = hexValue(text[offset + 1]);
		if (hi < 0 || lo < 0)
			return false;

		outValue = (hi << 4) | lo;
		return true;
	}

	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (char ch : text)
		{
			if (!std::isspace(static_cast<unsigned char>(ch)))
				normalized.push_back(ch);
		}

		if (normalized.empty())
			return false;

		if (normalized[0] == '#')
			normalized.erase(0, 1);

		if (normalized.size() != 6 && normalized.size() != 8)
			return false;

		int rr = 0, gg = 0, bb = 0, aa = 255;
		if (!TryParseHexByte(normalized, 0, rr) ||
			!TryParseHexByte(normalized, 2, gg) ||
			!TryParseHexByte(normalized, 4, bb))
		{
			return false;
		}

		hasAlpha = false;
		if (normalized.size() == 8)
		{
			if (!TryParseHexByte(normalized, 6, aa))
				return false;
			hasAlpha = true;
		}

		r = rr;
		g = gg;
		b = bb;
		a = aa;
		return true;
	}

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
		bool hasTagColor = false;
		int tagR = 255;
		int tagG = 255;
		int tagB = 255;
		bool hasTextColor = false;
		int textR = 255;
		int textG = 255;
		int textB = 255;
	};

	struct VacdmColorRuleOverrides
	{
		bool hasTargetColor = false;
		int targetR = 255;
		int targetG = 255;
		int targetB = 255;
		bool hasTagColor = false;
		int tagR = 255;
		int tagG = 255;
		int tagB = 255;
		bool hasTextColor = false;
		int textR = 255;
		int textG = 255;
		int textB = 255;
	};

	struct RunwayColorRuleDefinition
	{
		std::string token;
		std::string expectedRunway;
		bool hasTargetColor = false;
		int targetR = 255;
		int targetG = 255;
		int targetB = 255;
		bool hasTagColor = false;
		int tagR = 255;
		int tagG = 255;
		int tagB = 255;
		bool hasTextColor = false;
		int textR = 255;
		int textG = 255;
		int textB = 255;
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
			}
			if (rule.hasTagColor)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
			}
			if (rule.hasTextColor)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
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
		const std::string expectedCondition = NormalizeRunwayRuleConditionName(expectedConditionRaw);
		const std::string expectedLower = ToLowerAsciiCopy(expectedCondition);
		const std::string actualRunwayNormalized = NormalizeRunwayMatchText(actualRunwayRaw);

		if (expectedLower == "any" || expectedLower == "*")
			return !actualRunwayNormalized.empty();
		if (expectedLower == "set" || expectedLower == "present" || expectedLower == "available")
			return !actualRunwayNormalized.empty();
		if (expectedLower == "missing" || expectedLower == "unset" || expectedLower == "none" || expectedLower == "empty")
			return actualRunwayNormalized.empty();

		std::string expectedRunwayNormalized = NormalizeRunwayMatchText(expectedCondition);
		if (expectedRunwayNormalized.empty() || actualRunwayNormalized.empty())
			return false;

		if (expectedRunwayNormalized == "*" || expectedRunwayNormalized == "ANY" || expectedRunwayNormalized == "ALL")
			return true;

		if (actualRunwayNormalized == expectedRunwayNormalized)
			return true;

		if (actualRunwayNormalized.size() >= expectedRunwayNormalized.size() &&
			actualRunwayNormalized.compare(0, expectedRunwayNormalized.size(), expectedRunwayNormalized) == 0)
		{
			return true;
		}

		return false;
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
			}
			if (rule.hasTagColor)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
			}
			if (rule.hasTextColor)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
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

namespace
{
std::string TagTypeToConfigKey(CSMRRadar::TagTypes type)
{
    if (type == CSMRRadar::TagTypes::Departure)
        return "departure";
    if (type == CSMRRadar::TagTypes::Arrival)
        return "arrival";
    if (type == CSMRRadar::TagTypes::Uncorrelated)
        return "uncorrelated";
    return "airborne";
}
}

void CSMRRadar::RenderTags(Graphics& graphics, CDC& dc, bool frameProModeEnabled)
{
	// Drawing the Tags
	VSMR_REFRESH_LOG("Tags loop");
	const bool tagProModeEnabled = frameProModeEnabled;
	const int transitionAltitude = GetPlugIn()->GetTransitionAltitude();
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	const Value& LabelsSettings = activeProfile["labels"];
	const bool useAspeedForGate = LabelsSettings["use_aspeed_for_gate"].GetBool();
	const bool airborneUseDepartureArrivalColoring =
		LabelsSettings.HasMember("airborne") &&
		LabelsSettings["airborne"].IsObject() &&
		LabelsSettings["airborne"].HasMember("use_departure_arrival_coloring") &&
		LabelsSettings["airborne"]["use_departure_arrival_coloring"].IsBool() &&
		LabelsSettings["airborne"]["use_departure_arrival_coloring"].GetBool();
	const std::string activeAirport = getActiveAirport();
	const auto isTagBeingDragged = [](const string& callsign) -> bool
	{
		return TagBeingDragged == callsign;
	};
	const auto isMouseWithin = [](CRect rect) -> bool
	{
		return mouseLocation.x >= rect.left + 1 &&
			mouseLocation.x <= rect.right - 1 &&
			mouseLocation.y >= rect.top + 1 &&
			mouseLocation.y <= rect.bottom - 1;
	};

	Gdiplus::Font* tagRegularFont = customFonts[currentFontSize];
	Gdiplus::Font* tagBoldFont = tagRegularFont;
	std::unique_ptr<Gdiplus::Font> tagBoldFontOwned;
	if (tagRegularFont != nullptr)
	{
		Gdiplus::FontFamily baseFamily;
		if (tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok)
		{
			INT boldStyle = tagRegularFont->GetStyle() | Gdiplus::FontStyleBold;
			tagBoldFontOwned.reset(new Gdiplus::Font(&baseFamily, tagRegularFont->GetSize(), boldStyle, Gdiplus::UnitPixel));
			if (tagBoldFontOwned->GetLastStatus() == Gdiplus::Ok)
				tagBoldFont = tagBoldFontOwned.get();
		}
	}

	RectF tagMeasureRect;
	graphics.MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &tagMeasureRect);
	const int tagBlankWidth = static_cast<int>(tagMeasureRect.GetRight());
	tagMeasureRect = RectF(0, 0, 0, 0);
	graphics.MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
		tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &tagMeasureRect);
	int tagOneLineHeight = static_cast<int>(tagMeasureRect.GetBottom());
	if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
	{
		RectF boldMeasure;
		graphics.MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
			tagBoldFont, PointF(0, 0), &Gdiplus::StringFormat(), &boldMeasure);
		tagOneLineHeight = max(tagOneLineHeight, static_cast<int>(boldMeasure.GetBottom()));
	}

	struct ParsedTagTokenTemplate
	{
		std::string token;
		bool bold = false;
		bool hasCustomColor = false;
		int colorR = 255;
		int colorG = 255;
		int colorB = 255;
		bool isClearanceToken = false;
		std::string clearanceNotClearedText;
		std::string clearanceClearedText;
	};
	using ParsedTagLineTemplate = std::vector<ParsedTagTokenTemplate>;
	struct TagDefinitionCacheEntry
	{
		const Value* drawLabelLinesPtr = nullptr;
		const Value* collisionLabelLinesPtr = nullptr;
		std::vector<VacdmColorRuleDefinition> vacdmTagColorRules;
		std::vector<RunwayColorRuleDefinition> runwayTagColorRules;
		std::vector<ParsedTagLineTemplate> drawLineTemplates;
		std::vector<ParsedTagLineTemplate> collisionLineTemplates;
		bool valid = false;
	};
	std::unordered_map<std::string, TagDefinitionCacheEntry> tagDefinitionCache;

	auto buildParsedTagTemplates = [&](const Value& labelLines) -> std::vector<ParsedTagLineTemplate>
	{
		std::vector<ParsedTagLineTemplate> parsedLines;
		if (!labelLines.IsArray())
			return parsedLines;

		for (rapidjson::SizeType i = 0; i < labelLines.Size(); ++i)
		{
			const Value& line = labelLines[i];
			std::vector<std::string> elements;
			if (line.IsArray())
			{
				for (rapidjson::SizeType j = 0; j < line.Size(); ++j)
				{
					if (line[j].IsString())
						elements.push_back(line[j].GetString());
				}
			}
			else if (line.IsString())
			{
				elements.push_back(line.GetString());
			}

			if (elements.empty())
				continue;

			ParsedTagLineTemplate parsedLine;
			for (const std::string& rawElement : elements)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawElement);
				const std::string baseToken = styledToken.token.empty() ? rawElement : styledToken.token;
				VacdmColorRuleDefinition vacdmRuleToken;
				if (TryParseVacdmColorRuleToken(baseToken, vacdmRuleToken))
					continue;
				RunwayColorRuleDefinition runwayRuleToken;
				if (TryParseRunwayColorRuleToken(baseToken, runwayRuleToken))
					continue;

				ParsedTagTokenTemplate parsedToken;
				parsedToken.token = baseToken;
				parsedToken.bold = styledToken.bold;
				parsedToken.hasCustomColor = styledToken.hasCustomColor;
				parsedToken.colorR = styledToken.colorR;
				parsedToken.colorG = styledToken.colorG;
				parsedToken.colorB = styledToken.colorB;
				parsedToken.isClearanceToken =
					TryParseClearanceTokenDisplay(baseToken, parsedToken.clearanceNotClearedText, parsedToken.clearanceClearedText);
				parsedLine.push_back(parsedToken);
			}

			if (!parsedLine.empty())
				parsedLines.push_back(parsedLine);
		}

		return parsedLines;
	};

	auto getCachedTagDefinition = [&](const std::string& tagTypeKey, bool isTagDetailled, const char* statusDefinitionKey) -> const TagDefinitionCacheEntry*
	{
		std::string cacheKey = tagTypeKey;
		cacheKey += "|";
		cacheKey += isTagDetailled ? "d" : "n";
		cacheKey += "|";
		cacheKey += (statusDefinitionKey != nullptr ? statusDefinitionKey : "default");

		auto itCache = tagDefinitionCache.find(cacheKey);
		if (itCache != tagDefinitionCache.end())
			return itCache->second.valid ? &itCache->second : nullptr;

		TagDefinitionCacheEntry entry;
		if (!LabelsSettings.HasMember(tagTypeKey.c_str()) || !LabelsSettings[tagTypeKey.c_str()].IsObject())
		{
			entry.valid = false;
			tagDefinitionCache.emplace(cacheKey, std::move(entry));
			return nullptr;
		}

		const Value& labelSection = LabelsSettings[tagTypeKey.c_str()];
		const char* configKey = (isTagDetailled && labelSection.HasMember("definitionDetailled")) ? "definitionDetailled" : "definition";
		auto resolveLabelLines = [&](const char* definitionKey) -> const Value*
		{
			if (statusDefinitionKey != nullptr &&
				labelSection.HasMember("status_definitions") &&
				labelSection["status_definitions"].IsObject() &&
				labelSection["status_definitions"].HasMember(statusDefinitionKey) &&
				labelSection["status_definitions"][statusDefinitionKey].IsObject() &&
				labelSection["status_definitions"][statusDefinitionKey].HasMember(definitionKey) &&
				labelSection["status_definitions"][statusDefinitionKey][definitionKey].IsArray())
			{
				return &labelSection["status_definitions"][statusDefinitionKey][definitionKey];
			}

			if (labelSection.HasMember(definitionKey) && labelSection[definitionKey].IsArray())
				return &labelSection[definitionKey];

			return nullptr;
		};

		entry.drawLabelLinesPtr = resolveLabelLines(configKey);
		if (entry.drawLabelLinesPtr == nullptr)
		{
			entry.valid = false;
			tagDefinitionCache.emplace(cacheKey, std::move(entry));
			return nullptr;
		}

		entry.collisionLabelLinesPtr = resolveLabelLines("definition");
		if (entry.collisionLabelLinesPtr == nullptr)
			entry.collisionLabelLinesPtr = entry.drawLabelLinesPtr;

		const std::vector<std::string> vacdmRuleLineTexts = ConvertDefinitionValueToLineTexts(*entry.drawLabelLinesPtr);
		CollectVacdmColorRulesFromLineTexts(vacdmRuleLineTexts, entry.vacdmTagColorRules);
		CollectRunwayColorRulesFromLineTexts(vacdmRuleLineTexts, entry.runwayTagColorRules);
		entry.drawLineTemplates = buildParsedTagTemplates(*entry.drawLabelLinesPtr);
		if (entry.collisionLabelLinesPtr == entry.drawLabelLinesPtr)
			entry.collisionLineTemplates = entry.drawLineTemplates;
		else
			entry.collisionLineTemplates = buildParsedTagTemplates(*entry.collisionLabelLinesPtr);

		entry.valid = true;
		auto inserted = tagDefinitionCache.emplace(cacheKey, std::move(entry));
		return &inserted.first->second;
	};

	CRadarTarget rt;
	CRadarTarget aselTarget = GetPlugIn()->RadarTargetSelectASEL();
	for (rt = GetPlugIn()->RadarTargetSelectFirst();
		rt.IsValid();
		rt = GetPlugIn()->RadarTargetSelectNext(rt))
	{
		if (!rt.IsValid())
			continue;

		bool isASEL = (aselTarget.IsValid() && strcmp(aselTarget.GetCallsign(), rt.GetCallsign()) == 0);

		CRadarTargetPositionData RtPos = rt.GetPosition();
		POINT acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());
		CFlightPlan fp = GetPlugIn()->FlightPlanSelect(rt.GetCallsign());
		int reportedGs = RtPos.GetReportedGS();

		// Filtering the targets

		bool isAcDisplayed = isVisible(rt);

		bool AcisCorrelated = IsCorrelated(fp, rt);
		const bool hasAssignedSquawk = fp.IsValid() && strlen(fp.GetControllerAssignedData().GetSquawk()) != 0;
		const bool hasWrongAssignedSquawk = hasAssignedSquawk &&
			strcmp(fp.GetControllerAssignedData().GetSquawk(), RtPos.GetSquawk()) != 0;

		if (tagProModeEnabled && (!hasAssignedSquawk || hasWrongAssignedSquawk))
		{
			// In pro mode, tags are hidden when assigned squawk is missing or mismatched.
			AcisCorrelated = false;
			isAcDisplayed = false;
		}

		if (!AcisCorrelated && reportedGs < 3)
			isAcDisplayed = false;

		if (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()) != ReleasedTracks.end())
			isAcDisplayed = false;

		if (!isAcDisplayed)
			continue;

		// Getting the tag center/offset

		POINT TagCenter;
		map<string, POINT>::iterator it = TagsOffsets.find(rt.GetCallsign());
		if (it != TagsOffsets.end()) {
			TagCenter = { acPosPix.x + it->second.x, acPosPix.y + it->second.y };
		}
		else {
			// Use angle:

			if (TagAngles.find(rt.GetCallsign()) == TagAngles.end())
				TagAngles[rt.GetCallsign()] = 270.0f;

			int lenght = LeaderLineDefaultlenght;
			if (TagLeaderLineLength.find(rt.GetCallsign()) != TagLeaderLineLength.end())
				lenght = TagLeaderLineLength[rt.GetCallsign()];

			TagCenter.x = long(acPosPix.x + float(lenght * cos(DegToRad(TagAngles[rt.GetCallsign()]))));
			TagCenter.y = long(acPosPix.y + float(lenght * sin(DegToRad(TagAngles[rt.GetCallsign()]))));
		}

		TagTypes TagType = TagTypes::Departure;		
		TagTypes ColorTagType = TagTypes::Departure;

		if (fp.IsValid() && strcmp(fp.GetFlightPlanData().GetDestination(), activeAirport.c_str()) == 0) {
			// Circuit aircraft are treated as departures; not arrivals
			if (strcmp(fp.GetFlightPlanData().GetOrigin(), activeAirport.c_str()) != 0) {
				TagType = TagTypes::Arrival;
				ColorTagType = TagTypes::Arrival;
			}
		}

		if (reportedGs > 50) {
			TagType = TagTypes::Airborne;

			// Is "use_departure_arrival_coloring" enabled? if not, then use the airborne colors
			if (!airborneUseDepartureArrivalColoring) {
				ColorTagType = TagTypes::Airborne;
			}
		}

		if (!AcisCorrelated && reportedGs >= 3)
		{
			TagType = TagTypes::Uncorrelated;
			ColorTagType = TagTypes::Uncorrelated;
		}


		map<string, string> TagReplacingMap = GenerateTagData(rt, fp, isASEL, AcisCorrelated, tagProModeEnabled, transitionAltitude, useAspeedForGate, activeAirport);
		const auto sqErrorIt = TagReplacingMap.find("sqerror");
		const std::string* sqErrorText = (sqErrorIt != TagReplacingMap.end()) ? &sqErrorIt->second : nullptr;

		// ----- Generating the clickable map -----
		map<string, int> TagClickableMap;
		auto addClickableToken = [&](const char* tokenKey, int clickItemType)
		{
			auto itValue = TagReplacingMap.find(tokenKey);
			if (itValue != TagReplacingMap.end() && !itValue->second.empty())
				TagClickableMap[itValue->second] = clickItemType;
		};

		addClickableToken("callsign", TAG_CITEM_CALLSIGN);
		addClickableToken("actype", TAG_CITEM_FPBOX);
		addClickableToken("sctype", TAG_CITEM_FPBOX);
		addClickableToken("sqerror", TAG_CITEM_FPBOX);
		addClickableToken("deprwy", TAG_CITEM_RWY);
		addClickableToken("seprwy", TAG_CITEM_RWY);
		addClickableToken("arvrwy", TAG_CITEM_RWY);
		addClickableToken("srvrwy", TAG_CITEM_RWY);
		addClickableToken("gate", TAG_CITEM_GATE);
		addClickableToken("sate", TAG_CITEM_GATE);
		addClickableToken("flightlevel", TAG_CITEM_NO);
		addClickableToken("gs", TAG_CITEM_NO);
		addClickableToken("tobt", TAG_CITEM_NO);
		addClickableToken("tsat", TAG_CITEM_NO);
		addClickableToken("ttot", TAG_CITEM_NO);
		addClickableToken("asat", TAG_CITEM_NO);
		addClickableToken("aobt", TAG_CITEM_NO);
		addClickableToken("atot", TAG_CITEM_NO);
		addClickableToken("asrt", TAG_CITEM_NO);
		addClickableToken("aort", TAG_CITEM_NO);
		addClickableToken("ctot", TAG_CITEM_NO);
		addClickableToken("event_booking", TAG_CITEM_NO);
		addClickableToken("tendency", TAG_CITEM_NO);
		addClickableToken("wake", TAG_CITEM_FPBOX);
		addClickableToken("tssr", TAG_CITEM_NO);
		addClickableToken("asid", TAG_CITEM_SID);
		addClickableToken("csid", TAG_CITEM_SID);
		addClickableToken("ssid", TAG_CITEM_SID);
		addClickableToken("origin", TAG_CITEM_FPBOX);
		addClickableToken("dest", TAG_CITEM_FPBOX);
		addClickableToken("systemid", TAG_CITEM_NO);
		addClickableToken("groundstatus", TAG_CITEM_GROUNDSTATUS);
		addClickableToken("clearance", TAG_CITEM_CLEARANCE);
		addClickableToken("uk_stand", TAG_CITEM_UKSTAND);
		addClickableToken("remark", TAG_CITEM_REMARK);
		addClickableToken("scratchpad", TAG_CITEM_SCRATCHPAD);

		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//

		// First we need to figure out the tag size
		int TagWidth = 0, TagHeight = 0;
		const int blankWidth = tagBlankWidth;
		const int oneLineHeight = tagOneLineHeight;
		RectF mesureRect;

		CRect previousRect;
		auto itPrev = previousTagSize.find(rt.GetCallsign());
		if (itPrev != previousTagSize.end()) {
			const int prevW = itPrev->second.Width();
			const int prevH = itPrev->second.Height();
			previousRect = CRect(TagCenter.x - (prevW / 2),
				TagCenter.y - (prevH / 2),
				TagCenter.x + (prevW / 2),
				TagCenter.y + (prevH / 2));
		}
		else {
			previousRect = CRect(TagCenter.x - (TagWidth / 2),
				TagCenter.y - (TagHeight / 2),
				TagCenter.x + (TagWidth / 2),
				TagCenter.y + (TagHeight / 2));
		}

		bool isTagDetailled = isMouseWithin(previousRect) || isTagBeingDragged(rt.GetCallsign());

		const std::string tagTypeKey = TagTypeToConfigKey(TagType);

		const bool targetOnRunway = RimcasInstance->isAcOnRunway(rt.GetCallsign());
		const char* statusDefinitionKey = nullptr;
		if (TagReplacingMap["actype"] == "NoFPL" && (TagType == TagTypes::Departure || TagType == TagTypes::Arrival))
		{
			statusDefinitionKey = "nofpl";
		}
		else if (TagType == TagTypes::Airborne)
		{
			bool isAirborneArrival = false;
			if (fp.IsValid() &&
				strcmp(fp.GetFlightPlanData().GetDestination(), activeAirport.c_str()) == 0 &&
				strcmp(fp.GetFlightPlanData().GetOrigin(), activeAirport.c_str()) != 0)
			{
				isAirborneArrival = true;
			}

			if (isAirborneArrival)
				statusDefinitionKey = targetOnRunway ? "airarr_onrunway" : "airarr";
			else
				statusDefinitionKey = targetOnRunway ? "airdep_onrunway" : "airdep";
		}
		else if (fp.IsValid())
		{
			GroundStateCategory targetStatus = classifyGroundState(fp.GetGroundState(), reportedGs, targetOnRunway);
			switch (targetStatus)
			{
			case GroundStateCategory::Taxi:
				if (TagType == TagTypes::Departure || TagType == TagTypes::Arrival)
					statusDefinitionKey = "taxi";
				break;
			case GroundStateCategory::Push:
				if (TagType == TagTypes::Departure)
					statusDefinitionKey = "push";
				break;
			case GroundStateCategory::Stup:
				if (TagType == TagTypes::Departure)
					statusDefinitionKey = "stup";
				break;
			case GroundStateCategory::Nsts:
				if (TagType == TagTypes::Departure)
					statusDefinitionKey = "nsts";
				break;
			case GroundStateCategory::Depa:
				if (TagType == TagTypes::Departure)
					statusDefinitionKey = "depa";
				break;
			case GroundStateCategory::Arr:
				if (TagType == TagTypes::Arrival)
					statusDefinitionKey = "arr";
				break;
			default:
				break;
			}
		}

		const TagDefinitionCacheEntry* cachedTagDefinition = getCachedTagDefinition(tagTypeKey, isTagDetailled, statusDefinitionKey);
		if (cachedTagDefinition == nullptr)
			continue;

		VacdmPilotData vacdmRulePilotData;
		const bool hasVacdmRulePilotData = TryGetVacdmPilotDataForTarget(rt, fp, vacdmRulePilotData);
		VacdmColorRuleOverrides vacdmTagColorOverrides =
			EvaluateVacdmColorRules(cachedTagDefinition->vacdmTagColorRules, hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		const VacdmColorRuleOverrides runwayTagColorOverrides =
			EvaluateRunwayColorRules(cachedTagDefinition->runwayTagColorRules, TagReplacingMap);
		if (runwayTagColorOverrides.hasTargetColor)
		{
			vacdmTagColorOverrides.hasTargetColor = true;
			vacdmTagColorOverrides.targetR = runwayTagColorOverrides.targetR;
			vacdmTagColorOverrides.targetG = runwayTagColorOverrides.targetG;
			vacdmTagColorOverrides.targetB = runwayTagColorOverrides.targetB;
		}
		if (runwayTagColorOverrides.hasTagColor)
		{
			vacdmTagColorOverrides.hasTagColor = true;
			vacdmTagColorOverrides.tagR = runwayTagColorOverrides.tagR;
			vacdmTagColorOverrides.tagG = runwayTagColorOverrides.tagG;
			vacdmTagColorOverrides.tagB = runwayTagColorOverrides.tagB;
		}
		if (runwayTagColorOverrides.hasTextColor)
		{
			vacdmTagColorOverrides.hasTextColor = true;
			vacdmTagColorOverrides.textR = runwayTagColorOverrides.textR;
			vacdmTagColorOverrides.textG = runwayTagColorOverrides.textG;
			vacdmTagColorOverrides.textB = runwayTagColorOverrides.textB;
		}

		struct RenderedTagElement
		{
			std::string token;
			std::string text;
			bool bold = false;
			bool hasCustomColor = false;
			int colorR = 255;
			int colorG = 255;
			int colorB = 255;
			int measuredWidth = 0;
			int measuredHeight = 0;
		};
		vector<vector<RenderedTagElement>> ReplacedLabelLines;
		int CollisionTagWidth = 0;
		int CollisionTagHeight = 0;
		const std::string emptyReplacementValue;

		auto measureTagDefinitionLines = [&](const std::vector<ParsedTagLineTemplate>& lineTemplates, bool isDetailedMode, int& outTagWidth, int& outTagHeight, vector<vector<RenderedTagElement>>* renderedLines) -> bool
		{
			if (lineTemplates.empty())
				return false;

			auto scratchpadIt = TagReplacingMap.find("scratchpad");
			const bool hideScratchpadPlaceholder =
				!isDetailedMode && scratchpadIt != TagReplacingMap.end() && scratchpadIt->second == "...";

			for (const ParsedTagLineTemplate& lineTemplate : lineTemplates)
			{
				if (lineTemplate.empty())
					continue;

				bool allEmpty = true;
				vector<RenderedTagElement> renderedLine;
				renderedLine.reserve(lineTemplate.size());
				int tempTagWidth = 0;

				for (const ParsedTagTokenTemplate& tokenTemplate : lineTemplate)
				{
					string element;
					if (tokenTemplate.isClearanceToken)
					{
						if (fp.IsValid() && AcisCorrelated)
							element = fp.GetClearenceFlag() ? tokenTemplate.clearanceClearedText : tokenTemplate.clearanceNotClearedText;
						else
							element = "";
					}
					else
					{
						auto exactMatch = TagReplacingMap.find(tokenTemplate.token);
						if (exactMatch != TagReplacingMap.end())
						{
							if (hideScratchpadPlaceholder && tokenTemplate.token == "scratchpad" && exactMatch->second == "...")
								element = "";
							else
								element = exactMatch->second;
						}
						else
						{
							element = tokenTemplate.token;
							for (const auto& kv : TagReplacingMap)
							{
								if (element.find(kv.first) == std::string::npos)
									continue;

								const std::string& replacementValue =
									(hideScratchpadPlaceholder && kv.first == "scratchpad" && kv.second == "...")
									? emptyReplacementValue : kv.second;
								replaceAll(element, kv.first, replacementValue);
							}
						}
					}

					RenderedTagElement renderedElement;
					renderedElement.token = tokenTemplate.token;
					renderedElement.text = element;
					renderedElement.bold = tokenTemplate.bold;
					renderedElement.hasCustomColor = tokenTemplate.hasCustomColor;
					renderedElement.colorR = tokenTemplate.colorR;
					renderedElement.colorG = tokenTemplate.colorG;
					renderedElement.colorB = tokenTemplate.colorB;

					if (!element.empty())
					{
						allEmpty = false;
						mesureRect = RectF(0, 0, 0, 0);
						wstring wstr = wstring(element.begin(), element.end());
						Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
						graphics.MeasureString(wstr.c_str(), wcslen(wstr.c_str()),
							measureFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);
						renderedElement.measuredWidth = static_cast<int>(mesureRect.GetRight());
						renderedElement.measuredHeight = static_cast<int>(mesureRect.GetBottom());
						tempTagWidth += renderedElement.measuredWidth;
					}

					renderedLine.push_back(std::move(renderedElement));
				}

				if (allEmpty)
					continue;

				if (!renderedLine.empty())
					tempTagWidth += (int)blankWidth * (int(renderedLine.size()) - 1);

				outTagHeight += oneLineHeight;
				outTagWidth = max(outTagWidth, tempTagWidth);

				if (renderedLines != nullptr)
					renderedLines->push_back(renderedLine);
			}

			return true;
		};

		if (!measureTagDefinitionLines(cachedTagDefinition->drawLineTemplates, isTagDetailled, TagWidth, TagHeight, &ReplacedLabelLines))
			continue;

		if (!measureTagDefinitionLines(cachedTagDefinition->collisionLineTemplates, false, CollisionTagWidth, CollisionTagHeight, nullptr))
		{
			CollisionTagWidth = TagWidth;
			CollisionTagHeight = TagHeight;
		}
		if (CollisionTagWidth <= 0 || CollisionTagHeight <= 0)
		{
			CollisionTagWidth = TagWidth;
			CollisionTagHeight = TagHeight;
		}

		string alertStr;
		int alertTextWidth = 0;
		int alertTextHeight = 0;
		CRimcas::RimcasAlerts alert = RimcasInstance->getMovementAlert(rt.GetCallsign());
		if (CRimcas::NONE != alert && TagType == TagTypes::Departure)
		{
			switch (alert)
			{
			case CRimcas::RimcasAlerts::STATRPA:
				alertStr = "STAT RPA";
				break;
			case CRimcas::RimcasAlerts::NOPUSH:
				alertStr = "NO PUSH CLR";
				break;
			case CRimcas::RimcasAlerts::NOTKOF:
				alertStr = "NO TKOF CLR";
				break;
			case CRimcas::RimcasAlerts::NOTAXI:
				alertStr = "NO TAXI CLR";
				break;
			case CRimcas::RimcasAlerts::RWYINC:
				alertStr = "RWY INCURSION";
				break;
			case CRimcas::RimcasAlerts::RWYTYPE:
				alertStr = "RWY TYPE";
				break;
			case CRimcas::RimcasAlerts::RWYCLSD:
				alertStr = "RWY CLOSED";
				break;
			case CRimcas::RimcasAlerts::HIGHSPD:
				alertStr = "HIGH SPEED";
				break;
			case CRimcas::RimcasAlerts::EMERG:
				alertStr = "EMERG";
				break;
			default:
				break;
			}

			if (!alertStr.empty())
			{
				wstring wstr = wstring(alertStr.begin(), alertStr.end());
				graphics.MeasureString(wstr.c_str(), wcslen(wstr.c_str()),
					customFonts[currentFontSize], PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);

				alertTextWidth = static_cast<int>(mesureRect.GetRight());
				alertTextHeight = static_cast<int>(mesureRect.GetBottom());
				int TempTagWidth = alertTextWidth;
				TagWidth = max(TagWidth, TempTagWidth);
				TagHeight += oneLineHeight;
				CollisionTagWidth = max(CollisionTagWidth, TempTagWidth);
				CollisionTagHeight += oneLineHeight;
			}
		}

		previousTagSize[rt.GetCallsign()] = CRect(TagCenter.x - (TagWidth / 2), TagCenter.y - (TagHeight / 2), TagCenter.x + (TagWidth / 2), TagCenter.y + (TagHeight / 2));

		Color definedBackgroundColor = CurrentConfig->getConfigColor(LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()]["background_color"]);
		Color definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()]["background_color_on_runway"]);
		Color definedTextColor = CurrentConfig->getConfigColor(LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()]["text_color"]);
		
		if (ColorTagType == TagTypes::Departure) {
			if (!TagReplacingMap["asid"].empty() && CurrentConfig->isSidColorAvail(TagReplacingMap["asid"], activeAirport)) {
				definedBackgroundColor = CurrentConfig->getSidColor(TagReplacingMap["asid"], activeAirport);
			}
			if (fp.GetFlightPlanData().GetPlanType()[0] == 'I' && TagReplacingMap["asid"].empty() && LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()].HasMember("nosid_color")) {
				definedBackgroundColor = CurrentConfig->getConfigColor(LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()]["nosid_color"]);
			}

			const Value& departureLabel = LabelsSettings["departure"];
			if (departureLabel.HasMember("status_background_colors") && departureLabel["status_background_colors"].IsObject())
			{
				const Value& statusBackgroundColors = departureLabel["status_background_colors"];
				GroundStateCategory departureStatus = GroundStateCategory::Unknown;
				if (fp.IsValid())
					departureStatus = classifyGroundState(fp.GetGroundState(), reportedGs, targetOnRunway);

				const char* statusColorKey = nullptr;
				switch (departureStatus)
				{
				case GroundStateCategory::Taxi:
					statusColorKey = "taxi";
					break;
				case GroundStateCategory::Push:
					statusColorKey = "push";
					break;
				case GroundStateCategory::Stup:
					statusColorKey = "stup";
					break;
				case GroundStateCategory::Nsts:
					statusColorKey = "nsts";
					break;
				case GroundStateCategory::Depa:
					statusColorKey = "depa";
					break;
				default:
					break;
				}

				if (statusColorKey != nullptr && statusBackgroundColors.HasMember(statusColorKey) && statusBackgroundColors[statusColorKey].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor(statusBackgroundColors[statusColorKey]);
			}
		}
		else if (ColorTagType == TagTypes::Arrival) {
			const Value& arrivalLabel = LabelsSettings["arrival"];
			if (arrivalLabel.HasMember("status_background_colors") && arrivalLabel["status_background_colors"].IsObject())
			{
				const Value& statusBackgroundColors = arrivalLabel["status_background_colors"];
				GroundStateCategory arrivalStatus = GroundStateCategory::Unknown;
				if (fp.IsValid())
					arrivalStatus = classifyGroundState(fp.GetGroundState(), reportedGs, targetOnRunway);

				const char* statusColorKey = nullptr;
				switch (arrivalStatus)
				{
				case GroundStateCategory::Arr:
					statusColorKey = "arr";
					break;
				case GroundStateCategory::Taxi:
					statusColorKey = "taxi";
					break;
				default:
					break;
				}

				if (statusColorKey != nullptr && statusBackgroundColors.HasMember(statusColorKey) && statusBackgroundColors[statusColorKey].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor(statusBackgroundColors[statusColorKey]);
			}
		}
			if (TagReplacingMap["actype"] == "NoFPL" && LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()].HasMember("nofpl_color")) {
				definedBackgroundColor = CurrentConfig->getConfigColor(LabelsSettings[TagTypeToConfigKey(ColorTagType).c_str()]["nofpl_color"]);
		}

		if (TagType == TagTypes::Airborne &&
			fp.IsValid() &&
			AcisCorrelated &&
			LabelsSettings.HasMember("airborne") &&
			LabelsSettings["airborne"].IsObject())
		{
			bool isAirborneDeparture = true;
			std::string originAirport = fp.GetFlightPlanData().GetOrigin();
			std::string activeAirportUpper = activeAirport;
			if (!originAirport.empty() && !activeAirportUpper.empty())
			{
				std::transform(originAirport.begin(), originAirport.end(), originAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				std::transform(activeAirportUpper.begin(), activeAirportUpper.end(), activeAirportUpper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				isAirborneDeparture = (originAirport == activeAirportUpper);
			}

			const Value& airborneLabel = LabelsSettings["airborne"];
			const char* bgKey = isAirborneDeparture ? "departure_background_color" : "arrival_background_color";
			const char* bgOnRunwayKey = isAirborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
			const char* textKey = isAirborneDeparture ? "departure_text_color" : "arrival_text_color";

			if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
				definedBackgroundColor = CurrentConfig->getConfigColor(airborneLabel[bgKey]);
			if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
				definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
			if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
				definedTextColor = CurrentConfig->getConfigColor(airborneLabel[textKey]);
		}

		if (vacdmTagColorOverrides.hasTagColor)
		{
			definedBackgroundColor = Color(255, vacdmTagColorOverrides.tagR, vacdmTagColorOverrides.tagG, vacdmTagColorOverrides.tagB);
			definedBackgroundOnRunwayColor = definedBackgroundColor;
		}
		if (vacdmTagColorOverrides.hasTextColor)
		{
			definedTextColor = Color(255, vacdmTagColorOverrides.textR, vacdmTagColorOverrides.textG, vacdmTagColorOverrides.textB);
		}

		Color TagBackgroundColor = RimcasInstance->GetAircraftColor(rt.GetCallsign(),
			definedBackgroundColor,
			definedBackgroundOnRunwayColor,
			CurrentConfig->getConfigColor(activeProfile["rimcas"]["background_color_stage_one"]),
			CurrentConfig->getConfigColor(activeProfile["rimcas"]["background_color_stage_two"]));

		// We need to figure out if the tag color changes according to RIMCAS alerts, or not
		bool rimcasLabelOnly = activeProfile["rimcas"]["rimcas_label_only"].GetBool();

		if (rimcasLabelOnly)
			TagBackgroundColor = RimcasInstance->GetAircraftColor(rt.GetCallsign(),
			definedBackgroundColor,
			definedBackgroundOnRunwayColor);

		TagBackgroundColor = ColorManager->get_corrected_color("label", TagBackgroundColor);

		// Drawing the tag background

		// Slightly enlarge tag hitbox, center it, and draw rounded background.
		const int padding = 3;
		CRect TagBackgroundRect(TagCenter.x - (TagWidth / 2) - padding, TagCenter.y - (TagHeight / 2) - padding, TagCenter.x + (TagWidth / 2) + padding, TagCenter.y + (TagHeight / 2) + padding);
		CRect TagCollisionRect(TagCenter.x - (CollisionTagWidth / 2) - padding, TagCenter.y - (CollisionTagHeight / 2) - padding, TagCenter.x + (CollisionTagWidth / 2) + padding, TagCenter.y + (CollisionTagHeight / 2) + padding);
		int textLeft = TagBackgroundRect.left + padding;
		int textTop = TagBackgroundRect.top + padding;
		auto MakeRoundedRect = [](GraphicsPath &path, Rect r, int radius) {
			path.Reset();
			int d = radius * 2;
			path.AddArc(r.X, r.Y, d, d, 180, 90);
			path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
			path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
			path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
			path.CloseFigure();
		};

		Rect RoundedRect = CopyRect(TagBackgroundRect);
		GraphicsPath roundedPath;
		MakeRoundedRect(roundedPath, RoundedRect, 4);

		SolidBrush TagBackgroundBrush(TagBackgroundColor);
		graphics.FillPath(&TagBackgroundBrush, &roundedPath);
		if (isMouseWithin(TagBackgroundRect) || isTagBeingDragged(rt.GetCallsign()))
		{
			Pen pw(ColorManager->get_corrected_color("label", Color::White));
			graphics.DrawPath(&pw, &roundedPath);
		}

		// Drawing the tag text

		SolidBrush FontColor(ColorManager->get_corrected_color("label", definedTextColor));
		SolidBrush SquawkErrorColor(ColorManager->get_corrected_color("label",
			CurrentConfig->getConfigColor(LabelsSettings["squawk_error_color"])));
		SolidBrush ClearanceNotReceivedColor(ColorManager->get_corrected_color("label", Color(255, 235, 70, 70)));
		SolidBrush ClearanceReceivedColor(ColorManager->get_corrected_color("label", Color(255, 95, 225, 120)));
		SolidBrush AlertTextColorCaution(Color(255, 30, 30, 30)); // dark text
		SolidBrush AlertTextColorWarning(Color(255, 255, 255, 255)); // bright text
		SolidBrush AlertColorCaution(Color(230, 255, 215, 0)); // yellow-ish with alpha
		SolidBrush AlertColorWarning(Color(230, 200, 40, 40));  // red-ish with alpha
		const bool isClearanceReceived = (fp.IsValid() && fp.GetClearenceFlag());
		std::unique_ptr<SolidBrush> ColoredSidTextBrush;
		std::unique_ptr<SolidBrush> VacdmTobtTextBrush;
		std::unique_ptr<SolidBrush> VacdmTsatTextBrush;
		{
			const std::string csid = TagReplacingMap["csid"];
			const std::string departureRunway = TagReplacingMap["deprwy"];
			bool hasColoredSid = false;
			Color coloredSidText;
			if (!csid.empty() && csid != "SID")
			{
				hasColoredSid = TryResolveColoredSidTextColor(activeProfile, csid, departureRunway, CurrentConfig, coloredSidText);
			}

			if (hasColoredSid)
			{
				ColoredSidTextBrush = std::make_unique<SolidBrush>(ColorManager->get_corrected_color("label", coloredSidText));
			}
		}
		if (hasVacdmRulePilotData && !vacdmTagColorOverrides.hasTextColor)
		{
			int tobtR = 255;
			int tobtG = 255;
			int tobtB = 255;
			if (TryResolveVacdmTobtTextColor(vacdmRulePilotData, tobtR, tobtG, tobtB))
			{
				VacdmTobtTextBrush = std::make_unique<SolidBrush>(
					ColorManager->get_corrected_color("label", Color(255, tobtR, tobtG, tobtB)));
			}

			int tsatR = 255;
			int tsatG = 255;
			int tsatB = 255;
			if (TryResolveVacdmTsatTextColor(vacdmRulePilotData, tsatR, tsatG, tsatB))
			{
				VacdmTsatTextBrush = std::make_unique<SolidBrush>(
					ColorManager->get_corrected_color("label", Color(255, tsatR, tsatG, tsatB)));
			}
		}

		const Color leaderLineColor = ColorManager->get_corrected_color("symbol", Color::White);


		// Drawing the leader line
		RECT TagBackRectData = TagBackgroundRect;
		POINT toDraw1, toDraw2;
		if (LiangBarsky(TagBackRectData, acPosPix, TagBackgroundRect.CenterPoint(), toDraw1, toDraw2))
			graphics.DrawLine(&Pen(leaderLineColor), PointF(Gdiplus::REAL(acPosPix.x), Gdiplus::REAL(acPosPix.y)), PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));

		// If we use a RIMCAS label only, we display it, and adapt the rectangle
		CRect oldCrectSave = TagBackgroundRect;

		// Adding the tag screen object
		tagAreas[rt.GetCallsign()] = TagBackgroundRect;
		tagCollisionAreas[rt.GetCallsign()] = TagCollisionRect;
		AddScreenObject(DRAWING_TAG, rt.GetCallsign(), TagBackgroundRect, true, GetBottomLine(rt.GetCallsign()).c_str());

		TagBackgroundRect = oldCrectSave;

		// Clickable zones
		int heightOffset = 0;
		// Drawing Alert
		if (alert != CRimcas::NONE &&
			TagType == TagTypes::Departure &&
			!alertStr.empty())
		{
			wstring welement = wstring(alertStr.begin(), alertStr.end());
			CRect ItemRect(textLeft,
				textTop + heightOffset,
				textLeft + TagWidth,
				textTop + heightOffset + max(alertTextHeight, oneLineHeight));
			CRimcas::RimcasAlertSeverity severity = RimcasInstance->getAlertSeverity(alert);
			SolidBrush* AlertColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertColorWarning : &AlertColorCaution;
			SolidBrush* RimcasTextColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertTextColorWarning : &AlertTextColorCaution;
			graphics.FillRectangle(AlertColor, CopyRect(ItemRect));

			wstring walertStr = wstring(alertStr.begin(), alertStr.end());

			graphics.DrawString(walertStr.c_str(), wcslen(walertStr.c_str()), customFonts[currentFontSize],
				PointF(Gdiplus::REAL(textLeft), Gdiplus::REAL(textTop + heightOffset)),
				&Gdiplus::StringFormat(), RimcasTextColor);

			CRect alertRect(TagBackgroundRect.left, TagBackgroundRect.top + heightOffset,
				TagBackgroundRect.left + alertTextWidth, TagBackgroundRect.top + heightOffset + max(alertTextHeight, oneLineHeight));

			AddScreenObject(TagClickableMap[alertStr], rt.GetCallsign(), alertRect, true, GetBottomLine(rt.GetCallsign()).c_str());
			heightOffset += oneLineHeight;
		}
			for (auto&& line : ReplacedLabelLines)
			{

				int widthOffset = 0;
				for (auto&& renderedElement : line)
				{
				const std::string& rawToken = renderedElement.token;
				const std::string& element = renderedElement.text;
				Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;

				SolidBrush* color = &FontColor;
				if (sqErrorText != nullptr && !sqErrorText->empty() && element == *sqErrorText)
					color = &SquawkErrorColor;
				else if (rawToken == "csid" && ColoredSidTextBrush)
					color = ColoredSidTextBrush.get();
				else if (rawToken == "tobt" && VacdmTobtTextBrush)
					color = VacdmTobtTextBrush.get();
				else if (rawToken == "tsat" && VacdmTsatTextBrush)
					color = VacdmTsatTextBrush.get();
				else if (IsClearanceDefinitionToken(rawToken))
					color = isClearanceReceived ? &ClearanceReceivedColor : &ClearanceNotReceivedColor;

				std::unique_ptr<SolidBrush> tokenCustomColorBrush;
				if (renderedElement.hasCustomColor)
				{
					Color customColor = ColorManager->get_corrected_color("label",
						Color(255, renderedElement.colorR, renderedElement.colorG, renderedElement.colorB));
					tokenCustomColorBrush.reset(new SolidBrush(customColor));
					color = tokenCustomColorBrush.get();
				}

				wstring welement = wstring(element.begin(), element.end());

				graphics.DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
					PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset)),
					&Gdiplus::StringFormat(), color);

				int clickItemType = TAG_CITEM_NO;
				auto clickItemIt = TagClickableMap.find(element);
				if (clickItemIt != TagClickableMap.end())
					clickItemType = clickItemIt->second;
				if (IsClearanceDefinitionToken(rawToken))
					clickItemType = TAG_CITEM_CLEARANCE;

				int itemWidth = renderedElement.measuredWidth;
				if (clickItemType == TAG_CITEM_SCRATCHPAD) {
					// Extend scratchpad hit area to the tag border without growing the drawn tag.
					int rightBound = TagBackgroundRect.right - padding;
					int extendedWidth = rightBound - (textLeft + widthOffset);
					if (extendedWidth > itemWidth)
						itemWidth = extendedWidth;
				}
				int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);

				CRect ItemRect(textLeft + widthOffset, textTop + heightOffset,
					textLeft + widthOffset + itemWidth, textTop + heightOffset + itemHeight);

				AddScreenObject(clickItemType, rt.GetCallsign(), ItemRect, true, GetBottomLine(rt.GetCallsign()).c_str());

				widthOffset += renderedElement.measuredWidth;
				widthOffset += blankWidth;
			}

			heightOffset += oneLineHeight;
		}


	}

}
