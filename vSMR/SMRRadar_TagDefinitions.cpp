#include "stdafx.h"
#include "SMRRadar.hpp"

namespace
{
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

	std::string JoinStringList(const std::vector<std::string>& parts, const std::string& separator)
	{
		if (parts.empty())
			return "";

		std::ostringstream stream;
		for (size_t i = 0; i < parts.size(); ++i)
		{
			if (i != 0)
				stream << separator;
			stream << parts[i];
		}
		return stream.str();
	}
}

std::vector<std::string> CSMRRadar::GetTagDefinitionTokens() const
{
	return {
		"callsign",
		"actype",
		"sctype",
		"sqerror",
		"deprwy",
		"seprwy",
		"arvrwy",
		"srvrwy",
		"gate",
		"sate",
		"flightlevel",
		"gs",
		"tobt",
		"tsat",
		"ttot",
		"asat",
		"aobt",
		"atot",
		"asrt",
		"aort",
		"ctot",
		"event_booking",
		"tendency",
		"wake",
		"ssr",
		"asid",
		"csid",
		"ssid",
		"origin",
		"dest",
		"groundstatus",
		"clearance",
		"systemid",
		"uk_stand",
		"remark",
		"scratchpad"
	};
}

std::string CSMRRadar::NormalizeTagDefinitionType(const std::string& type) const
{
	std::string lowered = type;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (lowered == "departure" || lowered == "dep")
		return "departure";
	if (lowered == "arrival" || lowered == "arr")
		return "arrival";
	if (lowered == "airborne" || lowered == "air")
		return "airborne";
	if (lowered == "uncorrelated" || lowered == "uncorr")
		return "uncorrelated";

	return "departure";
}

std::string CSMRRadar::TagDefinitionTypeLabel(const std::string& type) const
{
	std::string normalized = NormalizeTagDefinitionType(type);
	if (normalized == "arrival")
		return "Arrival";
	if (normalized == "airborne")
		return "Airborne";
	if (normalized == "uncorrelated")
		return "Uncorrelated";
	return "Departure";
}

std::string CSMRRadar::NormalizeTagDefinitionDepartureStatus(const std::string& status) const
{
	std::string lowered = status;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string compact;
	compact.reserve(lowered.size());
	for (char c : lowered)
	{
		if (c == ' ' || c == '_' || c == '-')
			continue;
		compact.push_back(c);
	}

	if (lowered == "depa")
		return "depa";
	if (lowered == "arr" || lowered == "arrival")
		return "arr";
	if (compact == "airdep" || compact == "airbornedep" || compact == "airbornedeparture")
		return "airdep";
	if (compact == "airarr" || compact == "airbornearr" || compact == "airbornearrival")
		return "airarr";
	if (compact == "airdeponrunway" || compact == "airbornedeponrunway" || compact == "airbornedepartureonrunway")
		return "airdep_onrunway";
	if (compact == "airarronrunway" || compact == "airbornearronrunway" || compact == "airbornearrivalonrunway")
		return "airarr_onrunway";
	if (lowered == "taxi")
		return "taxi";
	if (lowered == "push")
		return "push";
	if (lowered == "stup" || lowered == "startup")
		return "stup";
	if (lowered == "nsts")
		return "nsts";
	if (compact == "nofpl" || compact == "noflightplan")
		return "nofpl";
	return "default";
}

std::string CSMRRadar::TagDefinitionDepartureStatusLabel(const std::string& status) const
{
	std::string normalized = NormalizeTagDefinitionDepartureStatus(status);
	if (normalized == "depa")
		return "DEPA";
	if (normalized == "arr")
		return "ARR";
	if (normalized == "airdep")
		return "Airborne Departure";
	if (normalized == "airarr")
		return "Airborne Arrival";
	if (normalized == "airdep_onrunway")
		return "Airborne Departure On Runway";
	if (normalized == "airarr_onrunway")
		return "Airborne Arrival On Runway";
	if (normalized == "taxi")
		return "TAXI";
	if (normalized == "push")
		return "PUSH";
	if (normalized == "stup")
		return "STUP";
	if (normalized == "nsts")
		return "NSTS";
	if (normalized == "nofpl")
		return "No FPL";
	return "Default";
}

std::vector<std::string> CSMRRadar::GetTagDefinitionStatusesForType(const std::string& type) const
{
	const std::string normalizedType = NormalizeTagDefinitionType(type);
	if (normalizedType == "departure")
		return { "default", "nofpl", "nsts", "push", "stup", "taxi", "depa" };
	if (normalizedType == "arrival")
		return { "default", "nofpl", "arr", "taxi" };
	if (normalizedType == "airborne")
		return { "default", "airdep", "airarr", "airdep_onrunway", "airarr_onrunway" };
	return { "default" };
}

bool CSMRRadar::IsTagDefinitionStatusAllowedForType(const std::string& type, const std::string& status) const
{
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(status);
	const std::vector<std::string> allowedStatuses = GetTagDefinitionStatusesForType(type);
	return std::find(allowedStatuses.begin(), allowedStatuses.end(), normalizedStatus) != allowedStatuses.end();
}

std::string CSMRRadar::GetTagEditorTargetColorPath() const
{
	const std::string normalizedType = NormalizeTagDefinitionType(TagDefinitionEditorType);
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);

	if (normalizedType == "departure")
	{
		if (normalizedStatus == "nofpl")
			return "targets.ground_icons.nofpl";
		if (normalizedStatus == "push" || normalizedStatus == "stup" || normalizedStatus == "taxi" || normalizedStatus == "nsts" || normalizedStatus == "depa")
			return std::string("targets.ground_icons.") + normalizedStatus;
		return "targets.ground_icons.departure_gate";
	}

	if (normalizedType == "arrival")
	{
		if (normalizedStatus == "nofpl")
			return "targets.ground_icons.nofpl";
		if (normalizedStatus == "arr")
			return "targets.ground_icons.arr";
		if (normalizedStatus == "taxi")
			return "targets.ground_icons.arrival_taxi";
		return "targets.ground_icons.arrival_gate";
	}

	if (normalizedType == "airborne")
	{
		if (normalizedStatus == "airarr" || normalizedStatus == "airarr_onrunway")
			return "targets.ground_icons.airborne_arrival";
		return "targets.ground_icons.airborne_departure";
	}

	return "";
}

std::string CSMRRadar::GetTagEditorLabelColorPath() const
{
	const std::string normalizedType = NormalizeTagDefinitionType(TagDefinitionEditorType);
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);

	if (normalizedType == "departure")
	{
		if (normalizedStatus == "nofpl")
			return "labels.departure.nofpl_color";
		if (normalizedStatus == "push" || normalizedStatus == "stup" || normalizedStatus == "taxi" || normalizedStatus == "nsts" || normalizedStatus == "depa")
			return std::string("labels.departure.status_background_colors.") + normalizedStatus;
		return "labels.departure.background_color";
	}

	if (normalizedType == "arrival")
	{
		if (normalizedStatus == "nofpl")
			return "labels.arrival.nofpl_color";
		if (normalizedStatus == "arr" || normalizedStatus == "taxi")
			return std::string("labels.arrival.status_background_colors.") + normalizedStatus;
		return "labels.arrival.background_color";
	}

	if (normalizedType == "airborne")
	{
		if (normalizedStatus == "airdep")
			return "labels.airborne.departure_background_color";
		if (normalizedStatus == "airarr")
			return "labels.airborne.arrival_background_color";
		if (normalizedStatus == "airdep_onrunway")
			return "labels.airborne.departure_background_color_on_runway";
		if (normalizedStatus == "airarr_onrunway")
			return "labels.airborne.arrival_background_color_on_runway";
		return "labels.airborne.background_color";
	}

	return "";
}

bool CSMRRadar::GetTagDefinitionArray(std::string type, bool detailed, rapidjson::Value*& outArray, bool createIfMissing, const std::string& departureStatus)
{
	outArray = nullptr;
	if (!CurrentConfig)
		return false;

	type = NormalizeTagDefinitionType(type);
	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject() || !profile.HasMember("labels") || !profile["labels"].IsObject())
		return false;

	rapidjson::Value& labels = profile["labels"];
	if (!labels.HasMember(type.c_str()) || !labels[type.c_str()].IsObject())
		return false;

	rapidjson::Value& section = labels[type.c_str()];
	rapidjson::Value* targetSection = &section;
	const char* key = detailed ? "definitionDetailled" : "definition";
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(departureStatus);

	auto appendCopiedDefinition = [&](rapidjson::Value& targetArray, const rapidjson::Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		for (rapidjson::SizeType i = 0; i < sourceArray.Size(); ++i)
		{
			rapidjson::Value copiedLine(rapidjson::kArrayType);
			const rapidjson::Value& sourceLine = sourceArray[i];
			if (sourceLine.IsArray())
			{
				for (rapidjson::SizeType j = 0; j < sourceLine.Size(); ++j)
				{
					if (sourceLine[j].IsString())
					{
						rapidjson::Value tokenValue;
						tokenValue.SetString(sourceLine[j].GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine[j].GetString())), CurrentConfig->document.GetAllocator());
						copiedLine.PushBack(tokenValue, CurrentConfig->document.GetAllocator());
					}
				}
			}
			else if (sourceLine.IsString())
			{
				rapidjson::Value tokenValue;
				tokenValue.SetString(sourceLine.GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine.GetString())), CurrentConfig->document.GetAllocator());
				copiedLine.PushBack(tokenValue, CurrentConfig->document.GetAllocator());
			}

			targetArray.PushBack(copiedLine, CurrentConfig->document.GetAllocator());
		}
	};

	if ((type == "departure" || type == "arrival" || type == "airborne") && normalizedStatus != "default")
	{
		if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
		{
			if (!createIfMissing)
				return false;

			if (section.HasMember("status_definitions"))
				section.RemoveMember("status_definitions");
			rapidjson::Value statusDefsKey;
			statusDefsKey.SetString("status_definitions", CurrentConfig->document.GetAllocator());
			rapidjson::Value statusDefsObject(rapidjson::kObjectType);
			section.AddMember(statusDefsKey, statusDefsObject, CurrentConfig->document.GetAllocator());
		}

		rapidjson::Value& statusDefinitions = section["status_definitions"];
		if (!statusDefinitions.HasMember(normalizedStatus.c_str()) || !statusDefinitions[normalizedStatus.c_str()].IsObject())
		{
			if (!createIfMissing)
				return false;

			if (statusDefinitions.HasMember(normalizedStatus.c_str()))
				statusDefinitions.RemoveMember(normalizedStatus.c_str());

			rapidjson::Value statusKey;
			statusKey.SetString(normalizedStatus.c_str(), CurrentConfig->document.GetAllocator());
			rapidjson::Value statusObject(rapidjson::kObjectType);
			statusDefinitions.AddMember(statusKey, statusObject, CurrentConfig->document.GetAllocator());
		}

		targetSection = &statusDefinitions[normalizedStatus.c_str()];
		if (!targetSection->IsObject())
			targetSection = &section;
	}

	if (!targetSection->HasMember(key))
	{
		if (!createIfMissing)
			return false;

		rapidjson::Value newArray(rapidjson::kArrayType);
		if (detailed && targetSection != &section && section.HasMember("definitionDetailled") && section["definitionDetailled"].IsArray())
		{
			appendCopiedDefinition(newArray, section["definitionDetailled"]);
		}
		else if (targetSection != &section && section.HasMember("definition") && section["definition"].IsArray())
		{
			appendCopiedDefinition(newArray, section["definition"]);
		}
		else if (detailed && targetSection->HasMember("definition") && (*targetSection)["definition"].IsArray())
		{
			appendCopiedDefinition(newArray, (*targetSection)["definition"]);
		}

		rapidjson::Value keyValue;
		keyValue.SetString(key, CurrentConfig->document.GetAllocator());
		targetSection->AddMember(keyValue, newArray, CurrentConfig->document.GetAllocator());
	}

	if (!targetSection->HasMember(key))
		return false;

	if (!(*targetSection)[key].IsArray())
	{
		if (!createIfMissing)
			return false;

		targetSection->RemoveMember(key);
		rapidjson::Value keyValue;
		keyValue.SetString(key, CurrentConfig->document.GetAllocator());
		rapidjson::Value newArray(rapidjson::kArrayType);
		targetSection->AddMember(keyValue, newArray, CurrentConfig->document.GetAllocator());
	}

	outArray = &(*targetSection)[key];
	return true;
}

std::vector<std::string> CSMRRadar::GetTagDefinitionLineStrings(std::string type, bool detailed, int maxLines, bool createIfMissing, const std::string& departureStatus)
{
	std::vector<std::string> lines(maxLines, "");
	rapidjson::Value* definitionArray = nullptr;
	if (!GetTagDefinitionArray(type, detailed, definitionArray, createIfMissing, departureStatus) || !definitionArray)
		return lines;

	const rapidjson::SizeType limit = min(definitionArray->Size(), static_cast<rapidjson::SizeType>(maxLines));
	for (rapidjson::SizeType i = 0; i < limit; ++i)
	{
		rapidjson::Value& lineValue = (*definitionArray)[i];
		if (lineValue.IsArray())
		{
			std::vector<std::string> tokens;
			for (rapidjson::SizeType j = 0; j < lineValue.Size(); ++j)
			{
				if (lineValue[j].IsString())
					tokens.push_back(lineValue[j].GetString());
			}
			lines[i] = JoinStringList(tokens, " ");
		}
		else if (lineValue.IsString())
		{
			lines[i] = lineValue.GetString();
		}
	}

	return lines;
}

void CSMRRadar::SaveTagDefinitionConfig()
{
	if (!CurrentConfig)
		return;

	if (!CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save definitions to vSMR_Profiles.json", true, true, false, false, false);
	}
}

void CSMRRadar::SetTagDefinitionLineString(std::string type, bool detailed, int lineIndex, const std::string& lineText, const std::string& departureStatus)
{
	if (lineIndex < 0 || lineIndex >= TagDefinitionEditorMaxLines)
		return;

	rapidjson::Value* definitionArray = nullptr;
	if (!GetTagDefinitionArray(type, detailed, definitionArray, true, departureStatus) || !definitionArray)
		return;

	std::vector<std::string> tokens = SplitDefinitionTokens(lineText);
	while (definitionArray->Size() <= static_cast<rapidjson::SizeType>(lineIndex))
	{
		rapidjson::Value emptyLine(rapidjson::kArrayType);
		definitionArray->PushBack(emptyLine, CurrentConfig->document.GetAllocator());
	}

	rapidjson::Value newLine(rapidjson::kArrayType);
	for (const std::string& token : tokens)
	{
		rapidjson::Value tokenValue;
		tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), CurrentConfig->document.GetAllocator());
		newLine.PushBack(tokenValue, CurrentConfig->document.GetAllocator());
	}

	(*definitionArray)[lineIndex] = newLine;

	while (definitionArray->Size() > 0)
	{
		rapidjson::Value& tail = (*definitionArray)[definitionArray->Size() - 1];
		bool emptyLine = false;
		if (tail.IsArray())
			emptyLine = (tail.Size() == 0);
		else if (tail.IsString())
			emptyLine = (strlen(tail.GetString()) == 0);
		else
			emptyLine = true;

		if (!emptyLine)
			break;
		definitionArray->PopBack();
	}

	bool hasAnyContent = false;
	for (rapidjson::SizeType i = 0; i < definitionArray->Size() && !hasAnyContent; ++i)
	{
		rapidjson::Value& lineValue = (*definitionArray)[i];
		if (lineValue.IsArray())
		{
			for (rapidjson::SizeType j = 0; j < lineValue.Size(); ++j)
			{
				if (lineValue[j].IsString() && strlen(lineValue[j].GetString()) > 0)
				{
					hasAnyContent = true;
					break;
				}
			}
		}
		else if (lineValue.IsString() && strlen(lineValue.GetString()) > 0)
		{
			hasAnyContent = true;
		}
	}

	// Keep at least one visible token in a definition: fallback to callsign on L1 when fully empty.
	if (!hasAnyContent)
	{
		rapidjson::Value fallbackLine(rapidjson::kArrayType);
		rapidjson::Value callsignToken;
		callsignToken.SetString("callsign", CurrentConfig->document.GetAllocator());
		fallbackLine.PushBack(callsignToken, CurrentConfig->document.GetAllocator());

		if (definitionArray->Size() == 0)
			definitionArray->PushBack(fallbackLine, CurrentConfig->document.GetAllocator());
		else
			(*definitionArray)[static_cast<rapidjson::SizeType>(0)] = fallbackLine;

		while (definitionArray->Size() > 1)
		{
			rapidjson::Value& tail = (*definitionArray)[definitionArray->Size() - 1];
			bool emptyLine = false;
			if (tail.IsArray())
				emptyLine = (tail.Size() == 0);
			else if (tail.IsString())
				emptyLine = (strlen(tail.GetString()) == 0);
			else
				emptyLine = true;

			if (!emptyLine)
				break;
			definitionArray->PopBack();
		}
	}

	SaveTagDefinitionConfig();
	RequestRefresh();
}

void CSMRRadar::InsertTagDefinitionTokenIntoLine(const std::string& token, bool makeBold)
{
	int lineIndex = max(0, min(TagDefinitionEditorSelectedLine, TagDefinitionEditorMaxLines - 1));
	std::vector<std::string> lines = GetTagDefinitionLineStrings(TagDefinitionEditorType, TagDefinitionEditorDetailed, TagDefinitionEditorMaxLines, true, TagDefinitionEditorDepartureStatus);
	std::string currentLine = lines[lineIndex];
	const std::string styledToken = ApplyDefinitionTokenStyle(token, makeBold);
	if (styledToken.empty())
		return;
	if (!currentLine.empty())
		currentLine += " ";
	currentLine += styledToken;

	SetTagDefinitionLineString(TagDefinitionEditorType, TagDefinitionEditorDetailed, lineIndex, currentLine, TagDefinitionEditorDepartureStatus);
}

std::map<std::string, std::string> CSMRRadar::BuildTagDefinitionPreviewMap(const std::string& type)
{
	std::map<std::string, std::string> previewMap;
	for (const std::string& token : GetTagDefinitionTokens())
		previewMap[token] = token;

	previewMap["callsign"] = "AFR123";
	previewMap["actype"] = "A320";
	previewMap["sctype"] = "A320";
	previewMap["sqerror"] = "";
	previewMap["deprwy"] = "26R";
	previewMap["seprwy"] = "26R";
	previewMap["arvrwy"] = "08L";
	previewMap["srvrwy"] = "08L";
	previewMap["gate"] = "A12";
	previewMap["sate"] = "A12";
	previewMap["flightlevel"] = "A030";
	previewMap["gs"] = "14";
	previewMap["tobt"] = "1210";
	previewMap["tsat"] = "1215";
	previewMap["ttot"] = "1220";
	previewMap["asat"] = "";
	previewMap["aobt"] = "";
	previewMap["atot"] = "";
	previewMap["asrt"] = "1208";
	previewMap["aort"] = "";
	previewMap["ctot"] = "1220";
	previewMap["event_booking"] = "B";
	previewMap["tendency"] = "-";
	previewMap["wake"] = "M";
	previewMap["ssr"] = "1234";
	previewMap["asid"] = "LAM1X";
	previewMap["csid"] = "LAM1X";
	previewMap["ssid"] = "LAM1";
	previewMap["origin"] = "LFPG";
	previewMap["dest"] = "EGKK";
	previewMap["groundstatus"] = "TAXI";
	previewMap["clearance"] = "[ ]";
	previewMap["systemid"] = "T:123456";
	previewMap["uk_stand"] = "12";
	previewMap["remark"] = "RMK";
	previewMap["scratchpad"] = "...";

	CRadarTarget sampleTarget = GetPlugIn()->RadarTargetSelectASEL();
	if (!sampleTarget.IsValid())
	{
		for (CRadarTarget rt = GetPlugIn()->RadarTargetSelectFirst(); rt.IsValid(); rt = GetPlugIn()->RadarTargetSelectNext(rt))
		{
			sampleTarget = rt;
			break;
		}
	}

	if (sampleTarget.IsValid())
	{
		CFlightPlan fp = GetPlugIn()->FlightPlanSelect(sampleTarget.GetCallsign());
		bool isAseL = (GetPlugIn()->RadarTargetSelectASEL().IsValid() && strcmp(GetPlugIn()->RadarTargetSelectASEL().GetCallsign(), sampleTarget.GetCallsign()) == 0);
		bool isCorrelated = IsCorrelated(fp, sampleTarget);
		std::map<std::string, std::string> generated = GenerateTagData(
			sampleTarget,
			fp,
			isAseL,
			isCorrelated,
			CurrentConfig->getActiveProfile()["filters"]["pro_mode"]["enable"].GetBool(),
			GetPlugIn()->GetTransitionAltitude(),
			CurrentConfig->getActiveProfile()["labels"]["use_aspeed_for_gate"].GetBool(),
			getActiveAirport());

		for (const auto& kv : generated)
		{
			if (!kv.second.empty())
				previewMap[kv.first] = kv.second;
		}

		TagTypes requestedType = TagTypes::Departure;
		std::string normalizedType = NormalizeTagDefinitionType(type);
		if (normalizedType == "arrival")
			requestedType = TagTypes::Arrival;
		else if (normalizedType == "airborne")
			requestedType = TagTypes::Airborne;
		else if (normalizedType == "uncorrelated")
			requestedType = TagTypes::Uncorrelated;

		if (requestedType == TagTypes::Arrival)
		{
			previewMap["dest"] = getActiveAirport();
			if (previewMap["origin"] == previewMap["dest"])
				previewMap["origin"] = "LFPG";
		}

		if (requestedType == TagTypes::Departure || requestedType == TagTypes::Arrival || requestedType == TagTypes::Airborne)
		{
			const std::string status = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);
			if ((requestedType == TagTypes::Departure || requestedType == TagTypes::Arrival) && status == "nofpl")
			{
				previewMap["actype"] = "NoFPL";
			}
			else if (status != "default")
			{
				previewMap["groundstatus"] = TagDefinitionDepartureStatusLabel(status);
			}
		}
	}

	return previewMap;
}

std::vector<std::string> CSMRRadar::BuildTagDefinitionPreviewLines()
{
	std::vector<std::string> sourceLines = GetTagDefinitionLineStrings(TagDefinitionEditorType, TagDefinitionEditorDetailed, TagDefinitionEditorMaxLines, true, TagDefinitionEditorDepartureStatus);
	std::map<std::string, std::string> previewMap = BuildTagDefinitionPreviewMap(TagDefinitionEditorType);
	std::vector<std::string> previewLines;

	for (const std::string& line : sourceLines)
	{
		std::vector<std::string> tokens = SplitDefinitionTokens(line);
		if (tokens.empty())
			continue;

		std::vector<std::string> renderedTokens;
		bool allEmpty = true;
		for (const std::string& rawToken : tokens)
		{
			DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
			std::string lookupToken = styledToken.token.empty() ? rawToken : styledToken.token;
			std::string value;

			std::string notClearedText;
			std::string clearedText;
			if (TryParseClearanceTokenDisplay(lookupToken, notClearedText, clearedText))
			{
				// Preview starts in "not received" state.
				value = notClearedText;
			}
			else
			{
				value = lookupToken;
				auto it = previewMap.find(lookupToken);
				if (it != previewMap.end())
					value = it->second;
			}

			renderedTokens.push_back(value);
			if (!value.empty())
				allEmpty = false;
		}

		if (!allEmpty)
			previewLines.push_back(JoinStringList(renderedTokens, " "));
	}

	if (previewLines.empty())
		previewLines.push_back("(empty definition)");

	return previewLines;
}

