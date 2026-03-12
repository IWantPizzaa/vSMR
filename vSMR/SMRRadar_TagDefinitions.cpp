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
		return "Departure";
	if (normalized == "arr")
		return "On Ground";
	if (normalized == "airdep")
		return "Airborne Departure";
	if (normalized == "airarr")
		return "Airborne Arrival";
	if (normalized == "airdep_onrunway")
		return "Airborne Departure On Runway";
	if (normalized == "airarr_onrunway")
		return "Airborne Arrival On Runway";
	if (normalized == "taxi")
		return "Taxi";
	if (normalized == "push")
		return "Push";
	if (normalized == "stup")
		return "Startup";
	if (normalized == "nsts")
		return "No Status";
	if (normalized == "nofpl")
		return "No FPL";
	return "Default";
}

std::vector<std::string> CSMRRadar::GetTagDefinitionStatusesForType(const std::string& type) const
{
	const std::string normalizedType = NormalizeTagDefinitionType(type);
	if (normalizedType == "departure")
		return { "default", "nofpl", "push", "stup", "taxi", "depa", "airdep", "airdep_onrunway" };
	if (normalizedType == "arrival")
		return { "default", "nofpl", "airarr", "airarr_onrunway" };
	if (normalizedType == "airborne")
		return { "airdep", "airarr", "airdep_onrunway", "airarr_onrunway" };
	return { "default" };
}

bool CSMRRadar::IsTagDefinitionStatusAllowedForType(const std::string& type, const std::string& status) const
{
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(status);
	const std::vector<std::string> allowedStatuses = GetTagDefinitionStatusesForType(type);
	return std::find(allowedStatuses.begin(), allowedStatuses.end(), normalizedStatus) != allowedStatuses.end();
}

bool CSMRRadar::GetTagDefinitionDetailedSameAsDefinition() const
{
	if (!CurrentConfig)
		return false;

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("labels") || !profile["labels"].IsObject())
		return false;

	const rapidjson::Value& labels = profile["labels"];
	if (labels.HasMember("definition_detailed_inherits_normal") &&
		labels["definition_detailed_inherits_normal"].IsBool())
	{
		return labels["definition_detailed_inherits_normal"].GetBool();
	}
	if (labels.HasMember("definition_detailed_same_as_definition") &&
		labels["definition_detailed_same_as_definition"].IsBool())
	{
		return labels["definition_detailed_same_as_definition"].GetBool();
	}
	return false;
}

bool CSMRRadar::SetTagDefinitionDetailedSameAsDefinition(bool sameAsDefinition, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	auto ensureObjectMember = [&](rapidjson::Value& parent, const char* key) -> rapidjson::Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value objectValue(rapidjson::kObjectType);
			parent.AddMember(keyValue, objectValue, allocator);
		}
		return parent[key];
	};

	rapidjson::Value& labels = ensureObjectMember(profile, "labels");
	bool changed = false;
	if (labels.HasMember("definition_detailed_same_as_definition"))
	{
		labels.RemoveMember("definition_detailed_same_as_definition");
		changed = true;
	}
	if (!labels.HasMember("definition_detailed_inherits_normal") ||
		!labels["definition_detailed_inherits_normal"].IsBool())
	{
		if (labels.HasMember("definition_detailed_inherits_normal"))
			labels.RemoveMember("definition_detailed_inherits_normal");

		rapidjson::Value keyValue;
		keyValue.SetString("definition_detailed_inherits_normal", allocator);
		rapidjson::Value value(sameAsDefinition);
		labels.AddMember(keyValue, value, allocator);
		changed = true;
	}
	else if (labels["definition_detailed_inherits_normal"].GetBool() != sameAsDefinition)
	{
		labels["definition_detailed_inherits_normal"].SetBool(sameAsDefinition);
		changed = true;
	}

	if (changed)
	{
		RequestRefresh();
		if (persistToDisk && !CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save detailed-definition mode to vSMR_Profiles.json", true, true, false, false, false);
			return false;
		}
	}

	return true;
}

bool CSMRRadar::GetTagDefinitionDetailedSameAsDefinition(const std::string& type, const std::string& status) const
{
	if (!CurrentConfig)
		return GetTagDefinitionDetailedSameAsDefinition();

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject() || !profile.HasMember("labels") || !profile["labels"].IsObject())
		return GetTagDefinitionDetailedSameAsDefinition();

	const rapidjson::Value& labels = profile["labels"];
	const std::string normalizedType = NormalizeTagDefinitionType(type);
	std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(status);
	if (!IsTagDefinitionStatusAllowedForType(normalizedType, normalizedStatus))
		normalizedStatus = "default";

	if (!labels.HasMember(normalizedType.c_str()) || !labels[normalizedType.c_str()].IsObject())
		return GetTagDefinitionDetailedSameAsDefinition();

	const rapidjson::Value& section = labels[normalizedType.c_str()];
	const char* key = "definition_detailed_inherits_normal";
	const char* legacyKey = "definition_detailed_same_as_definition";
	if (normalizedStatus != "default")
	{
		if (section.HasMember("status_definitions") &&
			section["status_definitions"].IsObject() &&
			section["status_definitions"].HasMember(normalizedStatus.c_str()) &&
			section["status_definitions"][normalizedStatus.c_str()].IsObject())
		{
			const rapidjson::Value& statusSection = section["status_definitions"][normalizedStatus.c_str()];
			if (statusSection.HasMember(key) && statusSection[key].IsBool())
				return statusSection[key].GetBool();
			if (statusSection.HasMember(legacyKey) && statusSection[legacyKey].IsBool())
				return statusSection[legacyKey].GetBool();
		}
	}

	if (section.HasMember(key) && section[key].IsBool())
		return section[key].GetBool();
	if (section.HasMember(legacyKey) && section[legacyKey].IsBool())
		return section[legacyKey].GetBool();

	return GetTagDefinitionDetailedSameAsDefinition();
}

bool CSMRRadar::SetTagDefinitionDetailedSameAsDefinition(
	const std::string& type,
	const std::string& status,
	bool sameAsDefinition,
	bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	auto ensureObjectMember = [&](rapidjson::Value& parent, const char* key) -> rapidjson::Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value objectValue(rapidjson::kObjectType);
			parent.AddMember(keyValue, objectValue, allocator);
		}
		return parent[key];
	};

	rapidjson::Value& labels = ensureObjectMember(profile, "labels");
	const std::string normalizedType = NormalizeTagDefinitionType(type);
	std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(status);
	if (!IsTagDefinitionStatusAllowedForType(normalizedType, normalizedStatus))
		normalizedStatus = "default";

	rapidjson::Value& section = ensureObjectMember(labels, normalizedType.c_str());
	rapidjson::Value* targetSection = &section;
	if (normalizedStatus != "default")
	{
		rapidjson::Value& statusDefs = ensureObjectMember(section, "status_definitions");
		targetSection = &ensureObjectMember(statusDefs, normalizedStatus.c_str());
	}

	const char* key = "definition_detailed_inherits_normal";
	const char* legacyKey = "definition_detailed_same_as_definition";
	bool changed = false;
	if (targetSection->HasMember(legacyKey))
	{
		targetSection->RemoveMember(legacyKey);
		changed = true;
	}
	if (!targetSection->HasMember(key) || !(*targetSection)[key].IsBool())
	{
		if (targetSection->HasMember(key))
			targetSection->RemoveMember(key);
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value value(sameAsDefinition);
		targetSection->AddMember(keyValue, value, allocator);
		changed = true;
	}
	else if ((*targetSection)[key].GetBool() != sameAsDefinition)
	{
		(*targetSection)[key].SetBool(sameAsDefinition);
		changed = true;
	}

	if (changed)
	{
		RequestRefresh();
		if (persistToDisk && !CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save detailed-definition mode to vSMR_Profiles.json", true, true, false, false, false);
			return false;
		}
	}

	return true;
}

void CSMRRadar::GetTagDefinitionEditorContext(std::string& type, bool& detailed, std::string& status) const
{
	type = NormalizeTagDefinitionType(TagDefinitionEditorType);
	detailed = TagDefinitionEditorDetailed;
	status = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);
	if (type == "airborne")
	{
		type = (status == "airarr" || status == "airarr_onrunway") ? "arrival" : "departure";
	}
	if (!IsTagDefinitionStatusAllowedForType(type, status))
		status = "default";
}

void CSMRRadar::SetTagDefinitionEditorContext(const std::string& type, bool detailed, const std::string& status)
{
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(status);
	TagDefinitionEditorType = NormalizeTagDefinitionType(type);
	if (TagDefinitionEditorType == "airborne")
	{
		TagDefinitionEditorType = (normalizedStatus == "airarr" || normalizedStatus == "airarr_onrunway") ? "arrival" : "departure";
	}
	TagDefinitionEditorDetailed = detailed;
	TagDefinitionEditorDepartureStatus = normalizedStatus;
	if (!IsTagDefinitionStatusAllowedForType(TagDefinitionEditorType, TagDefinitionEditorDepartureStatus))
		TagDefinitionEditorDepartureStatus = "default";
}

std::string CSMRRadar::GetTagEditorTargetColorPath() const
{
	const std::string normalizedType = NormalizeTagDefinitionType(TagDefinitionEditorType);
	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);

	if (normalizedType == "departure")
	{
		if (normalizedStatus == "nofpl")
			return "targets.departure.no_fpl";
		if (normalizedStatus == "push")
			return "targets.departure.push";
		if (normalizedStatus == "stup")
			return "targets.departure.startup";
		if (normalizedStatus == "taxi")
			return "targets.departure.taxi";
		if (normalizedStatus == "depa")
			return "targets.departure.departure";
		if (normalizedStatus == "airdep" || normalizedStatus == "airdep_onrunway")
			return "targets.departure.airborne";
		if (normalizedStatus == "nsts")
			return "targets.departure.no_status";
		return "targets.departure.gate";
	}

	if (normalizedType == "arrival")
	{
		if (normalizedStatus == "nofpl")
			return "targets.departure.no_fpl";
		if (normalizedStatus == "airarr" || normalizedStatus == "airarr_onrunway")
			return "targets.arrival.airborne";
		if (normalizedStatus == "arr" || normalizedStatus == "taxi")
			return "targets.arrival.on_ground";
		return "targets.arrival.gate";
	}

	if (normalizedType == "airborne")
	{
		if (normalizedStatus == "airarr" || normalizedStatus == "airarr_onrunway")
			return "targets.arrival.airborne";
		return "targets.departure.airborne";
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
			return "labels.departure.background_no_fpl_color";
		if (normalizedStatus == "push")
			return "labels.departure.background_push_color";
		if (normalizedStatus == "stup")
			return "labels.departure.background_startup_color";
		if (normalizedStatus == "taxi")
			return "labels.departure.background_taxi_color";
		if (normalizedStatus == "depa")
			return "labels.departure.background_departure_color";
		if (normalizedStatus == "airdep")
			return "labels.departure.background_airborne_color";
		if (normalizedStatus == "airdep_onrunway")
			return "labels.departure.background_on_runway_color";
		return "labels.departure.background_no_status_color";
	}

	if (normalizedType == "arrival")
	{
		if (normalizedStatus == "nofpl")
			return "labels.arrival.background_no_fpl_color";
		if (normalizedStatus == "airarr")
			return "labels.arrival.background_airborne_color";
		if (normalizedStatus == "airarr_onrunway")
			return "labels.arrival.background_on_runway_color";
		return "labels.arrival.background_on_ground_color";
	}

	if (normalizedType == "airborne")
	{
		if (normalizedStatus == "airdep")
			return "labels.departure.background_airborne_color";
		if (normalizedStatus == "airarr")
			return "labels.arrival.background_airborne_color";
		if (normalizedStatus == "airdep_onrunway")
			return "labels.departure.background_on_runway_color";
		if (normalizedStatus == "airarr_onrunway")
			return "labels.arrival.background_on_runway_color";
		return "labels.departure.background_airborne_color";
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
	const char* key = detailed ? "definition_detailed" : "definition";
	const char* legacyDetailedKey = "definitionDetailled";
	std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(departureStatus);
	if (!IsTagDefinitionStatusAllowedForType(type, normalizedStatus))
		normalizedStatus = "default";

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
		if (detailed && targetSection->HasMember(legacyDetailedKey) && (*targetSection)[legacyDetailedKey].IsArray())
		{
			outArray = &(*targetSection)[legacyDetailedKey];
			return true;
		}

		if (!createIfMissing)
			return false;

		rapidjson::Value newArray(rapidjson::kArrayType);
		if (detailed && targetSection != &section && section.HasMember("definition_detailed") && section["definition_detailed"].IsArray())
		{
			appendCopiedDefinition(newArray, section["definition_detailed"]);
		}
		else if (detailed && targetSection != &section && section.HasMember(legacyDetailedKey) && section[legacyDetailedKey].IsArray())
		{
			appendCopiedDefinition(newArray, section[legacyDetailedKey]);
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
	previewMap["ssid"] = "LAM1";
	previewMap["origin"] = "LFPG";
	previewMap["dest"] = "EGKK";
	previewMap["groundstatus"] = "TAXI";
	previewMap["clearance"] = "[ ]";
	previewMap["systemid"] = "T:123456";
	previewMap["uk_stand"] = "12";
	previewMap["remark"] = "RMK";
	previewMap["scratchpad"] = "...";

	// Keep the preview self-contained. Older EuroScope builds can crash when
	// radar-target APIs are queried during detached dialog initialization.
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
		const std::string activeAirport = getActiveAirport();
		if (!activeAirport.empty())
			previewMap["dest"] = activeAirport;
		if (previewMap["origin"] == previewMap["dest"])
			previewMap["origin"] = "LFPG";
	}
	else if (requestedType == TagTypes::Uncorrelated)
	{
		previewMap["systemid"] = "PSR:123456";
		previewMap["callsign"] = "";
		previewMap["actype"] = "";
		previewMap["sctype"] = "";
		previewMap["asid"] = "";
		previewMap["ssid"] = "";
	}

	if (requestedType == TagTypes::Departure || requestedType == TagTypes::Arrival || requestedType == TagTypes::Airborne)
	{
		std::string status = NormalizeTagDefinitionDepartureStatus(TagDefinitionEditorDepartureStatus);
		if (!IsTagDefinitionStatusAllowedForType(normalizedType, status))
			status = "default";
		if ((requestedType == TagTypes::Departure || requestedType == TagTypes::Arrival) && status == "nofpl")
		{
			previewMap["actype"] = "NoFPL";
			previewMap["sctype"] = "NoFPL";
		}
		else if (requestedType == TagTypes::Departure && status == "default")
		{
			previewMap["groundstatus"] = "NSTS";
		}
		else if (status != "default")
		{
			previewMap["groundstatus"] = TagDefinitionDepartureStatusLabel(status);
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

std::vector<std::string> CSMRRadar::BuildTagDefinitionPreviewLinesForContext(const std::string& type, bool detailed, const std::string& departureStatus)
{
	const std::string previousType = TagDefinitionEditorType;
	const bool previousDetailed = TagDefinitionEditorDetailed;
	const std::string previousStatus = TagDefinitionEditorDepartureStatus;

	SetTagDefinitionEditorContext(type, detailed, departureStatus);
	std::vector<std::string> previewLines = BuildTagDefinitionPreviewLines();

	TagDefinitionEditorType = previousType;
	TagDefinitionEditorDetailed = previousDetailed;
	TagDefinitionEditorDepartureStatus = previousStatus;
	return previewLines;
}

namespace
{
	int ClampRuleColorComponent(int value)
	{
		return std::clamp(value, 0, 255);
	}

	bool TryReadRuleColor(const rapidjson::Value& item, const char* key, bool& outApply, int& outR, int& outG, int& outB, int& outA)
	{
		outApply = false;
		outR = 255;
		outG = 255;
		outB = 255;
		outA = 255;

		if (!item.IsObject() || !item.HasMember(key) || !item[key].IsObject())
			return false;

		const rapidjson::Value& color = item[key];
		if (!color.HasMember("r") || !color["r"].IsInt() ||
			!color.HasMember("g") || !color["g"].IsInt() ||
			!color.HasMember("b") || !color["b"].IsInt())
		{
			return false;
		}

		outApply = true;
		outR = ClampRuleColorComponent(color["r"].GetInt());
		outG = ClampRuleColorComponent(color["g"].GetInt());
		outB = ClampRuleColorComponent(color["b"].GetInt());
		if (color.HasMember("a") && color["a"].IsInt())
			outA = ClampRuleColorComponent(color["a"].GetInt());
		return true;
	}

	void AppendRuleColor(rapidjson::Value& parent, const char* key, bool apply, int r, int g, int b, int a, rapidjson::Document::AllocatorType& allocator)
	{
		if (!apply)
			return;

		rapidjson::Value colorObject(rapidjson::kObjectType);
		colorObject.AddMember("r", ClampRuleColorComponent(r), allocator);
		colorObject.AddMember("g", ClampRuleColorComponent(g), allocator);
		colorObject.AddMember("b", ClampRuleColorComponent(b), allocator);
		colorObject.AddMember("a", ClampRuleColorComponent(a), allocator);

		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		parent.AddMember(keyValue, colorObject, allocator);
	}

	bool StructuredRulesEqual(const StructuredTagColorRule& a, const StructuredTagColorRule& b)
	{
		if (a.criteria.size() != b.criteria.size())
			return false;
		for (size_t i = 0; i < a.criteria.size(); ++i)
		{
			if (a.criteria[i].source != b.criteria[i].source ||
				a.criteria[i].token != b.criteria[i].token ||
				a.criteria[i].condition != b.criteria[i].condition)
			{
				return false;
			}
		}

		return a.source == b.source &&
			a.token == b.token &&
			a.condition == b.condition &&
			a.name == b.name &&
			a.tagType == b.tagType &&
			a.status == b.status &&
			a.detail == b.detail &&
			a.applyTarget == b.applyTarget &&
			a.targetR == b.targetR &&
			a.targetG == b.targetG &&
			a.targetB == b.targetB &&
			a.targetA == b.targetA &&
			a.applyTag == b.applyTag &&
			a.tagR == b.tagR &&
			a.tagG == b.tagG &&
			a.tagB == b.tagB &&
			a.tagA == b.tagA &&
			a.applyText == b.applyText &&
			a.textR == b.textR &&
			a.textG == b.textG &&
			a.textB == b.textB &&
			a.textA == b.textA;
	}
}

std::string CSMRRadar::NormalizeStructuredRuleSource(const std::string& source) const
{
	std::string lowered = TrimAsciiWhitespace(source);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lowered.find("custom") != std::string::npos || lowered == "list" || lowered == "sidlist" || lowered == "sid")
		return "custom";
	if (lowered.find("runway") != std::string::npos || lowered == "rwy")
		return "runway";
	return "vacdm";
}

std::string CSMRRadar::NormalizeStructuredRuleToken(const std::string& source, const std::string& token) const
{
	std::string normalizedToken = TrimAsciiWhitespace(token);
	std::transform(normalizedToken.begin(), normalizedToken.end(), normalizedToken.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (normalizedToken.empty())
		return "";

	const std::string normalizedSource = NormalizeStructuredRuleSource(source);
	if (normalizedSource == "runway")
	{
		if (normalizedToken == "deprwy" || normalizedToken == "seprwy" || normalizedToken == "arvrwy" || normalizedToken == "srvrwy")
			return normalizedToken;
		return "";
	}
	if (normalizedSource == "custom")
	{
		if (normalizedToken == "sid")
			return "asid";
		if (normalizedToken == "asid" || normalizedToken == "ssid" ||
			normalizedToken == "deprwy" || normalizedToken == "seprwy" || normalizedToken == "arvrwy" || normalizedToken == "srvrwy")
		{
			return normalizedToken;
		}
		return "";
	}

	if (normalizedToken == "tobt" || normalizedToken == "tsat" || normalizedToken == "ttot" ||
		normalizedToken == "asat" || normalizedToken == "aobt" || normalizedToken == "atot" ||
		normalizedToken == "asrt" || normalizedToken == "aort" || normalizedToken == "ctot")
	{
		return normalizedToken;
	}

	return "";
}

std::string CSMRRadar::NormalizeStructuredRuleCondition(const std::string& source, const std::string& condition) const
{
	const std::string normalizedSource = NormalizeStructuredRuleSource(source);
	std::string text = TrimAsciiWhitespace(condition);
	if (text.empty())
		return "any";

	if (normalizedSource == "runway" || normalizedSource == "custom")
		return text;

	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (char& c : text)
	{
		if (c == ' ' || c == '-')
			c = '_';
	}
	if (text.rfind("state_", 0) == 0)
		text = text.substr(6);
	if (text.empty())
		return "any";
	return text;
}

std::string CSMRRadar::NormalizeStructuredRuleTagType(const std::string& tagType) const
{
	std::string normalized = TrimAsciiWhitespace(tagType);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (normalized.empty() || normalized == "all" || normalized == "*")
		return "any";
	if (normalized == "dep")
		return "departure";
	if (normalized == "arr")
		return "arrival";
	if (normalized == "air")
		return "airborne";
	if (normalized == "uncorr" || normalized == "uncor")
		return "uncorrelated";
	if (normalized == "departure" || normalized == "arrival" || normalized == "airborne" || normalized == "uncorrelated")
		return normalized;
	return "any";
}

std::string CSMRRadar::NormalizeStructuredRuleStatus(const std::string& status) const
{
	std::string normalized = TrimAsciiWhitespace(status);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string compact;
	compact.reserve(normalized.size());
	for (char c : normalized)
	{
		if (c == ' ' || c == '_' || c == '-')
			continue;
		compact.push_back(c);
	}

	if (normalized.empty() || normalized == "all" || normalized == "*")
		return "any";
	if (normalized == "any")
		return "any";
	if (normalized == "def" || compact == "default" || compact == "nostatus" || compact == "onground")
		return "default";

	if (compact == "departure")
		return "depa";
	if (compact == "startup")
		return "stup";

	const std::string normalizedStatus = NormalizeTagDefinitionDepartureStatus(normalized);
	if (normalizedStatus == "nsts" || normalizedStatus == "arr")
		return "default";
	return normalizedStatus;
}

std::string CSMRRadar::NormalizeStructuredRuleDetail(const std::string& detail) const
{
	std::string normalized = TrimAsciiWhitespace(detail);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (normalized.empty() || normalized == "all" || normalized == "*")
		return "any";
	if (normalized == "normal" || normalized == "basic" || normalized == "simple")
		return "normal";
	if (normalized == "detailed" || normalized == "detail" || normalized == "expanded")
		return "detailed";
	return "any";
}

const std::vector<StructuredTagColorRule>& CSMRRadar::GetStructuredTagColorRules() const
{
	if (StructuredTagRulesCacheValid)
		return StructuredTagRulesCache;

	StructuredTagRulesCache.clear();
	if (!CurrentConfig)
	{
		StructuredTagRulesCacheValid = true;
		return StructuredTagRulesCache;
	}

	const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
	if (!profile.IsObject())
	{
		StructuredTagRulesCacheValid = true;
		return StructuredTagRulesCache;
	}

	const rapidjson::Value* rulesObject = nullptr;
	if (profile.HasMember("rules") && profile["rules"].IsObject())
	{
		rulesObject = &profile["rules"];
	}
	else if (profile.HasMember("labels") && profile["labels"].IsObject() &&
		profile["labels"].HasMember("rules") && profile["labels"]["rules"].IsObject())
	{
		rulesObject = &profile["labels"]["rules"];
	}

	if (rulesObject == nullptr)
	{
		StructuredTagRulesCacheValid = true;
		return StructuredTagRulesCache;
	}

	if (!rulesObject->HasMember("items") || !(*rulesObject)["items"].IsArray())
	{
		StructuredTagRulesCacheValid = true;
		return StructuredTagRulesCache;
	}

	const rapidjson::Value& items = (*rulesObject)["items"];
	for (rapidjson::SizeType i = 0; i < items.Size(); ++i)
	{
		if (!items[i].IsObject())
			continue;

		const rapidjson::Value& item = items[i];
		StructuredTagColorRule rule;

		auto appendCriterion = [&](const std::string& rawSource, const std::string& rawToken, const std::string& rawCondition) {
			const std::string normalizedSource = NormalizeStructuredRuleSource(rawSource);
			const std::string normalizedToken = NormalizeStructuredRuleToken(normalizedSource, rawToken);
			if (normalizedToken.empty())
				return;

			StructuredTagColorRule::Criterion criterion;
			criterion.source = normalizedSource;
			criterion.token = normalizedToken;
			criterion.condition = NormalizeStructuredRuleCondition(normalizedSource, rawCondition);
			rule.criteria.push_back(criterion);
			};

		if (item.HasMember("criteria") && item["criteria"].IsArray())
		{
			const rapidjson::Value& criteria = item["criteria"];
			for (rapidjson::SizeType c = 0; c < criteria.Size(); ++c)
			{
				if (!criteria[c].IsObject())
					continue;

				const rapidjson::Value& criterionObject = criteria[c];
				std::string source = "vacdm";
				if (criterionObject.HasMember("source") && criterionObject["source"].IsString())
					source = criterionObject["source"].GetString();
				else if (criterionObject.HasMember("kind") && criterionObject["kind"].IsString())
					source = criterionObject["kind"].GetString();

				std::string token;
				if (criterionObject.HasMember("token") && criterionObject["token"].IsString())
					token = criterionObject["token"].GetString();

				std::string condition;
				if (criterionObject.HasMember("condition") && criterionObject["condition"].IsString())
					condition = criterionObject["condition"].GetString();
				else if (NormalizeStructuredRuleSource(source) == "runway" && criterionObject.HasMember("runway") && criterionObject["runway"].IsString())
					condition = criterionObject["runway"].GetString();
				else if (NormalizeStructuredRuleSource(source) != "runway" && criterionObject.HasMember("state") && criterionObject["state"].IsString())
					condition = criterionObject["state"].GetString();

				appendCriterion(source, token, condition);
			}
		}

		if (rule.criteria.empty())
		{
			std::string source = "vacdm";
			if (item.HasMember("source") && item["source"].IsString())
				source = item["source"].GetString();
			else if (item.HasMember("kind") && item["kind"].IsString())
				source = item["kind"].GetString();

			std::string token;
			if (item.HasMember("token") && item["token"].IsString())
				token = item["token"].GetString();

			std::string condition;
			if (item.HasMember("condition") && item["condition"].IsString())
				condition = item["condition"].GetString();
			else if (NormalizeStructuredRuleSource(source) == "runway" && item.HasMember("runway") && item["runway"].IsString())
				condition = item["runway"].GetString();
			else if (NormalizeStructuredRuleSource(source) != "runway" && item.HasMember("state") && item["state"].IsString())
				condition = item["state"].GetString();

			appendCriterion(source, token, condition);
		}

		if (rule.criteria.empty())
			continue;
		rule.source = rule.criteria.front().source;
		rule.token = rule.criteria.front().token;
		rule.condition = rule.criteria.front().condition;
		if (item.HasMember("name") && item["name"].IsString())
			rule.name = TrimAsciiWhitespace(item["name"].GetString());

		std::string tagType = "any";
		if (item.HasMember("tag_type") && item["tag_type"].IsString())
			tagType = item["tag_type"].GetString();
		rule.tagType = NormalizeStructuredRuleTagType(tagType);

		std::string status = "any";
		if (item.HasMember("status") && item["status"].IsString())
			status = item["status"].GetString();
		rule.status = NormalizeStructuredRuleStatus(status);

		std::string detail = "any";
		if (item.HasMember("detail") && item["detail"].IsString())
			detail = item["detail"].GetString();
		rule.detail = NormalizeStructuredRuleDetail(detail);

		TryReadRuleColor(item, "target_color", rule.applyTarget, rule.targetR, rule.targetG, rule.targetB, rule.targetA);
		TryReadRuleColor(item, "tag_color", rule.applyTag, rule.tagR, rule.tagG, rule.tagB, rule.tagA);
		TryReadRuleColor(item, "text_color", rule.applyText, rule.textR, rule.textG, rule.textB, rule.textA);

		if (!rule.applyTarget && !rule.applyTag && !rule.applyText)
			continue;

		StructuredTagRulesCache.push_back(rule);
	}

	StructuredTagRulesCacheValid = true;
	return StructuredTagRulesCache;
}

bool CSMRRadar::SetStructuredTagColorRules(const std::vector<StructuredTagColorRule>& rules, bool persistToDisk)
{
	if (!CurrentConfig)
		return false;

	rapidjson::Value& profile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return false;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	auto ensureObjectMember = [&](rapidjson::Value& parent, const char* key) -> rapidjson::Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value objectValue(rapidjson::kObjectType);
			parent.AddMember(keyValue, objectValue, allocator);
			changed = true;
		}

		return parent[key];
	};

	if (profile.HasMember("labels") && profile["labels"].IsObject() && profile["labels"].HasMember("rules"))
	{
		profile["labels"].RemoveMember("rules");
		changed = true;
	}

	rapidjson::Value& rulesObject = ensureObjectMember(profile, "rules");

	if (!rulesObject.HasMember("version") || !rulesObject["version"].IsInt() || rulesObject["version"].GetInt() != 1)
	{
		if (rulesObject.HasMember("version"))
			rulesObject.RemoveMember("version");
		rapidjson::Value keyValue;
		keyValue.SetString("version", allocator);
		rapidjson::Value versionValue;
		versionValue.SetInt(1);
		rulesObject.AddMember(keyValue, versionValue, allocator);
		changed = true;
	}

	std::vector<StructuredTagColorRule> normalizedRules;
	normalizedRules.reserve(rules.size());
	for (const StructuredTagColorRule& rawRule : rules)
	{
		StructuredTagColorRule normalizedRule = rawRule;
		normalizedRule.criteria.clear();
		auto appendNormalizedCriterion = [&](const std::string& rawSource, const std::string& rawToken, const std::string& rawCondition) {
			const std::string normalizedSource = NormalizeStructuredRuleSource(rawSource);
			const std::string normalizedToken = NormalizeStructuredRuleToken(normalizedSource, rawToken);
			if (normalizedToken.empty())
				return;
			StructuredTagColorRule::Criterion criterion;
			criterion.source = normalizedSource;
			criterion.token = normalizedToken;
			criterion.condition = NormalizeStructuredRuleCondition(normalizedSource, rawCondition);
			normalizedRule.criteria.push_back(criterion);
			};

		if (!rawRule.criteria.empty())
		{
			for (const StructuredTagColorRule::Criterion& rawCriterion : rawRule.criteria)
				appendNormalizedCriterion(rawCriterion.source, rawCriterion.token, rawCriterion.condition);
		}
		else
		{
			appendNormalizedCriterion(rawRule.source, rawRule.token, rawRule.condition);
		}

		if (normalizedRule.criteria.empty())
			continue;

		normalizedRule.source = normalizedRule.criteria.front().source;
		normalizedRule.token = normalizedRule.criteria.front().token;
		normalizedRule.condition = normalizedRule.criteria.front().condition;
		normalizedRule.name = TrimAsciiWhitespace(rawRule.name);
		normalizedRule.tagType = NormalizeStructuredRuleTagType(rawRule.tagType);
		normalizedRule.status = NormalizeStructuredRuleStatus(rawRule.status);
		normalizedRule.detail = NormalizeStructuredRuleDetail(rawRule.detail);
		normalizedRule.targetR = ClampRuleColorComponent(rawRule.targetR);
		normalizedRule.targetG = ClampRuleColorComponent(rawRule.targetG);
		normalizedRule.targetB = ClampRuleColorComponent(rawRule.targetB);
		normalizedRule.targetA = ClampRuleColorComponent(rawRule.targetA);
		normalizedRule.tagR = ClampRuleColorComponent(rawRule.tagR);
		normalizedRule.tagG = ClampRuleColorComponent(rawRule.tagG);
		normalizedRule.tagB = ClampRuleColorComponent(rawRule.tagB);
		normalizedRule.tagA = ClampRuleColorComponent(rawRule.tagA);
		normalizedRule.textR = ClampRuleColorComponent(rawRule.textR);
		normalizedRule.textG = ClampRuleColorComponent(rawRule.textG);
		normalizedRule.textB = ClampRuleColorComponent(rawRule.textB);
		normalizedRule.textA = ClampRuleColorComponent(rawRule.textA);

		if (!normalizedRule.applyTarget && !normalizedRule.applyTag && !normalizedRule.applyText)
			continue;

		normalizedRules.push_back(normalizedRule);
	}

	const std::vector<StructuredTagColorRule>& existingRules = GetStructuredTagColorRules();
	if (existingRules.size() != normalizedRules.size())
	{
		changed = true;
	}
	else
	{
		for (size_t i = 0; i < existingRules.size(); ++i)
		{
			if (!StructuredRulesEqual(existingRules[i], normalizedRules[i]))
			{
				changed = true;
				break;
			}
		}
	}

	if (changed)
	{
		if (rulesObject.HasMember("items"))
			rulesObject.RemoveMember("items");

		rapidjson::Value rulesKey;
		rulesKey.SetString("items", allocator);
		rapidjson::Value rulesArray(rapidjson::kArrayType);
		for (const StructuredTagColorRule& rule : normalizedRules)
		{
			rapidjson::Value ruleObject(rapidjson::kObjectType);

			rapidjson::Value sourceKey;
			sourceKey.SetString("source", allocator);
			rapidjson::Value sourceValue;
			sourceValue.SetString(rule.source.c_str(), static_cast<rapidjson::SizeType>(rule.source.size()), allocator);
			ruleObject.AddMember(sourceKey, sourceValue, allocator);

			rapidjson::Value tokenKey;
			tokenKey.SetString("token", allocator);
			rapidjson::Value tokenValue;
			tokenValue.SetString(rule.token.c_str(), static_cast<rapidjson::SizeType>(rule.token.size()), allocator);
			ruleObject.AddMember(tokenKey, tokenValue, allocator);

			rapidjson::Value conditionKey;
			conditionKey.SetString("condition", allocator);
			rapidjson::Value conditionValue;
			conditionValue.SetString(rule.condition.c_str(), static_cast<rapidjson::SizeType>(rule.condition.size()), allocator);
			ruleObject.AddMember(conditionKey, conditionValue, allocator);

			if (!rule.name.empty())
			{
				rapidjson::Value nameKey;
				nameKey.SetString("name", allocator);
				rapidjson::Value nameValue;
				nameValue.SetString(rule.name.c_str(), static_cast<rapidjson::SizeType>(rule.name.size()), allocator);
				ruleObject.AddMember(nameKey, nameValue, allocator);
			}

			rapidjson::Value criteriaKey;
			criteriaKey.SetString("criteria", allocator);
			rapidjson::Value criteriaArray(rapidjson::kArrayType);
			for (const StructuredTagColorRule::Criterion& criterion : rule.criteria)
			{
				rapidjson::Value criterionObject(rapidjson::kObjectType);

				rapidjson::Value criterionSourceKey;
				criterionSourceKey.SetString("source", allocator);
				rapidjson::Value criterionSourceValue;
				criterionSourceValue.SetString(criterion.source.c_str(), static_cast<rapidjson::SizeType>(criterion.source.size()), allocator);
				criterionObject.AddMember(criterionSourceKey, criterionSourceValue, allocator);

				rapidjson::Value criterionTokenKey;
				criterionTokenKey.SetString("token", allocator);
				rapidjson::Value criterionTokenValue;
				criterionTokenValue.SetString(criterion.token.c_str(), static_cast<rapidjson::SizeType>(criterion.token.size()), allocator);
				criterionObject.AddMember(criterionTokenKey, criterionTokenValue, allocator);

				rapidjson::Value criterionConditionKey;
				criterionConditionKey.SetString("condition", allocator);
				rapidjson::Value criterionConditionValue;
				criterionConditionValue.SetString(criterion.condition.c_str(), static_cast<rapidjson::SizeType>(criterion.condition.size()), allocator);
				criterionObject.AddMember(criterionConditionKey, criterionConditionValue, allocator);

				criteriaArray.PushBack(criterionObject, allocator);
			}
			ruleObject.AddMember(criteriaKey, criteriaArray, allocator);

			rapidjson::Value tagTypeKey;
			tagTypeKey.SetString("tag_type", allocator);
			rapidjson::Value tagTypeValue;
			tagTypeValue.SetString(rule.tagType.c_str(), static_cast<rapidjson::SizeType>(rule.tagType.size()), allocator);
			ruleObject.AddMember(tagTypeKey, tagTypeValue, allocator);

			rapidjson::Value statusKey;
			statusKey.SetString("status", allocator);
			rapidjson::Value statusValue;
			statusValue.SetString(rule.status.c_str(), static_cast<rapidjson::SizeType>(rule.status.size()), allocator);
			ruleObject.AddMember(statusKey, statusValue, allocator);

			rapidjson::Value detailKey;
			detailKey.SetString("detail", allocator);
			rapidjson::Value detailValue;
			detailValue.SetString(rule.detail.c_str(), static_cast<rapidjson::SizeType>(rule.detail.size()), allocator);
			ruleObject.AddMember(detailKey, detailValue, allocator);

			AppendRuleColor(ruleObject, "target_color", rule.applyTarget, rule.targetR, rule.targetG, rule.targetB, rule.targetA, allocator);
			AppendRuleColor(ruleObject, "tag_color", rule.applyTag, rule.tagR, rule.tagG, rule.tagB, rule.tagA, allocator);
			AppendRuleColor(ruleObject, "text_color", rule.applyText, rule.textR, rule.textG, rule.textB, rule.textA, allocator);

			rulesArray.PushBack(ruleObject, allocator);
		}

		rulesObject.AddMember(rulesKey, rulesArray, allocator);
	}

	if (!changed)
	{
		StructuredTagRulesCache = existingRules;
		StructuredTagRulesCacheValid = true;
		return true;
	}

	StructuredTagRulesCache = normalizedRules;
	StructuredTagRulesCacheValid = true;

	if (!persistToDisk)
		return true;

	return CurrentConfig->saveConfig();
}
