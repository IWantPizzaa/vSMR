#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>

#include "SMRRadar_TagShared.hpp"

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern int LeaderLineDefaultlenght;

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

bool StructuredRuleContextMatches(const StructuredTagColorRule& rule, const std::string& tagTypeKey, const char* statusDefinitionKey, bool isTagDetailed)
{
	const std::string currentType = ToLowerAsciiCopy(tagTypeKey);
	const std::string currentStatus = statusDefinitionKey != nullptr ? ToLowerAsciiCopy(statusDefinitionKey) : "default";
	const std::string currentDetail = isTagDetailed ? "detailed" : "normal";

	auto matchesField = [](const std::string& value, const std::string& current) -> bool
	{
		const std::string normalized = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(value));
		if (normalized.empty() || normalized == "any" || normalized == "all" || normalized == "*")
			return true;
		return normalized == current;
	};

	return matchesField(rule.tagType, currentType) &&
		matchesField(rule.status, currentStatus) &&
		matchesField(rule.detail, currentDetail);
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

VacdmColorRuleOverrides EvaluateStructuredTagColorRules(
	const std::vector<StructuredTagColorRule>& rules,
	const std::string& tagTypeKey,
	const char* statusDefinitionKey,
	bool isTagDetailed,
	const std::map<std::string, std::string>& replacingMap,
	const VacdmPilotData* pilotData)
{
	VacdmColorRuleOverrides overrides;
	for (const StructuredTagColorRule& rule : rules)
	{
		if (!StructuredRuleContextMatches(rule, tagTypeKey, statusDefinitionKey, isTagDetailed))
			continue;

		auto criterionMatches = [&](const std::string& sourceText, const std::string& token, const std::string& condition) -> bool
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
				auto itValue = replacingMap.find(token);
				if (itValue != replacingMap.end())
					actualValue = itValue->second;
				return CustomRuleConditionMatches(condition, actualValue);
			}

			const std::string actualState = ResolveVacdmRuleStateName(token, pilotData);
			return VacdmRuleStateMatches(condition, actualState);
		};

		bool ruleMatches = true;
		if (!rule.criteria.empty())
		{
			for (const StructuredTagColorRule::Criterion& criterion : rule.criteria)
			{
				if (!criterionMatches(criterion.source, criterion.token, criterion.condition))
				{
					ruleMatches = false;
					break;
				}
			}
		}
		else
		{
			ruleMatches = criterionMatches(rule.source, rule.token, rule.condition);
		}

		if (!ruleMatches)
			continue;

		if (rule.applyTarget)
		{
			overrides.hasTargetColor = true;
			overrides.targetR = rule.targetR;
			overrides.targetG = rule.targetG;
			overrides.targetB = rule.targetB;
			overrides.targetA = rule.targetA;
		}
		if (rule.applyTag)
		{
			overrides.hasTagColor = true;
			overrides.tagR = rule.tagR;
			overrides.tagG = rule.tagG;
			overrides.tagB = rule.tagB;
			overrides.tagA = rule.tagA;
		}
		if (rule.applyText)
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
}

void CSMRRadar::RenderTags(Graphics& graphics, CDC& dc, bool frameProModeEnabled, const FrameTagDataCache& frameTagDataCache, const FrameVacdmLookupCache& frameVacdmLookupCache)
{
	// Drawing the Tags
	VSMR_REFRESH_LOG("Tags loop");
	if (CurrentConfig == nullptr || ColorManager == nullptr || RimcasInstance == nullptr)
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: skipped (missing config/color/rimcas dependency)");
		return;
	}

	const bool tagProModeEnabled = frameProModeEnabled;
	const int transitionAltitude = GetPlugIn()->GetTransitionAltitude();
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	if (!activeProfile.IsObject() ||
		!activeProfile.HasMember("labels") ||
		!activeProfile["labels"].IsObject())
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: active profile has no valid labels object");
		return;
	}

	const Value& LabelsSettings = activeProfile["labels"];
	const bool tagDetailedSameAsDefinition =
		(LabelsSettings.HasMember("definition_detailed_inherits_normal") &&
			LabelsSettings["definition_detailed_inherits_normal"].IsBool() &&
			LabelsSettings["definition_detailed_inherits_normal"].GetBool()) ||
		(LabelsSettings.HasMember("definition_detailed_same_as_definition") &&
			LabelsSettings["definition_detailed_same_as_definition"].IsBool() &&
			LabelsSettings["definition_detailed_same_as_definition"].GetBool());
	const auto verboseTargetStep = [&](const std::string& callsign, const std::string& step)
	{
		if (!Logger::is_verbose_mode())
			return;
		Logger::info("RenderTags: " + callsign + " " + step);
	};
	const auto isDetailedSameAsDefinitionForContext = [&](const std::string& tagTypeKey, const char* statusDefinitionKey) -> bool
	{
		if (!LabelsSettings.IsObject() ||
			!LabelsSettings.HasMember(tagTypeKey.c_str()) ||
			!LabelsSettings[tagTypeKey.c_str()].IsObject())
		{
			return tagDetailedSameAsDefinition;
		}

		const Value& labelSection = LabelsSettings[tagTypeKey.c_str()];
		const char* key = "definition_detailed_inherits_normal";
		const char* legacyKey = "definition_detailed_same_as_definition";

		if (statusDefinitionKey != nullptr &&
			labelSection.HasMember("status_definitions") &&
			labelSection["status_definitions"].IsObject() &&
			labelSection["status_definitions"].HasMember(statusDefinitionKey) &&
			labelSection["status_definitions"][statusDefinitionKey].IsObject())
		{
			const Value& statusSection = labelSection["status_definitions"][statusDefinitionKey];
			if (statusSection.HasMember(key) && statusSection[key].IsBool())
				return statusSection[key].GetBool();
			if (statusSection.HasMember(legacyKey) && statusSection[legacyKey].IsBool())
				return statusSection[legacyKey].GetBool();
		}

		if (labelSection.HasMember(key) && labelSection[key].IsBool())
			return labelSection[key].GetBool();
		if (labelSection.HasMember(legacyKey) && labelSection[legacyKey].IsBool())
			return labelSection[legacyKey].GetBool();

		return tagDetailedSameAsDefinition;
	};
	const bool useAspeedForGate =
		(LabelsSettings.HasMember("use_speed_for_gate") &&
			LabelsSettings["use_speed_for_gate"].IsBool() &&
			LabelsSettings["use_speed_for_gate"].GetBool()) ||
		(LabelsSettings.HasMember("use_aspeed_for_gate") &&
			LabelsSettings["use_aspeed_for_gate"].IsBool() &&
			LabelsSettings["use_aspeed_for_gate"].GetBool());
	const bool airborneUseDepartureArrivalColoring =
		(LabelsSettings.HasMember("use_departure_arrival_coloring") &&
			LabelsSettings["use_departure_arrival_coloring"].IsBool() &&
			LabelsSettings["use_departure_arrival_coloring"].GetBool()) ||
		(LabelsSettings.HasMember("airborne") &&
			LabelsSettings["airborne"].IsObject() &&
			LabelsSettings["airborne"].HasMember("use_departure_arrival_coloring") &&
			LabelsSettings["airborne"]["use_departure_arrival_coloring"].IsBool() &&
			LabelsSettings["airborne"]["use_departure_arrival_coloring"].GetBool());
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

	auto fontIt = customFonts.find(currentFontSize);
	Gdiplus::Font* tagRegularFont = (fontIt != customFonts.end()) ? fontIt->second.get() : nullptr;
	if (tagRegularFont == nullptr)
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: no font loaded for size=" + std::to_string(currentFontSize));
		return;
	}

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
	const std::vector<StructuredTagColorRule>& structuredTagRules = GetStructuredTagColorRules();

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
		const bool useDetailedDefinition =
			isTagDetailled &&
			!isDetailedSameAsDefinitionForContext(tagTypeKey, statusDefinitionKey) &&
			(labelSection.HasMember("definition_detailed") || labelSection.HasMember("definitionDetailled"));
		const char* configKey = useDetailedDefinition ? "definition_detailed" : "definition";
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

			if (strcmp(definitionKey, "definition_detailed") == 0 &&
				statusDefinitionKey != nullptr &&
				labelSection.HasMember("status_definitions") &&
				labelSection["status_definitions"].IsObject() &&
				labelSection["status_definitions"].HasMember(statusDefinitionKey) &&
				labelSection["status_definitions"][statusDefinitionKey].IsObject() &&
				labelSection["status_definitions"][statusDefinitionKey].HasMember("definitionDetailled") &&
				labelSection["status_definitions"][statusDefinitionKey]["definitionDetailled"].IsArray())
			{
				return &labelSection["status_definitions"][statusDefinitionKey]["definitionDetailled"];
			}

			if (labelSection.HasMember(definitionKey) && labelSection[definitionKey].IsArray())
				return &labelSection[definitionKey];
			if (strcmp(definitionKey, "definition_detailed") == 0 &&
				labelSection.HasMember("definitionDetailled") && labelSection["definitionDetailled"].IsArray())
			{
				return &labelSection["definitionDetailled"];
			}

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
	auto hasStatusDefinition = [&](const std::string& tagTypeKey, const char* statusDefinitionKey) -> bool
	{
		if (statusDefinitionKey == nullptr || statusDefinitionKey[0] == '\0')
			return false;
		if (!LabelsSettings.HasMember(tagTypeKey.c_str()) || !LabelsSettings[tagTypeKey.c_str()].IsObject())
			return false;
		const Value& labelSection = LabelsSettings[tagTypeKey.c_str()];
		if (!labelSection.HasMember("status_definitions") || !labelSection["status_definitions"].IsObject())
			return false;
		return labelSection["status_definitions"].HasMember(statusDefinitionKey) &&
			labelSection["status_definitions"][statusDefinitionKey].IsObject();
	};

	CRadarTarget rt;
	CRadarTarget aselTarget = GetPlugIn()->RadarTargetSelectASEL();
	for (rt = GetPlugIn()->RadarTargetSelectFirst();
		rt.IsValid();
		rt = GetPlugIn()->RadarTargetSelectNext(rt))
	{
		if (!rt.IsValid() || !rt.GetPosition().IsValid())
			continue;
		const char* rtCallsignRaw = rt.GetCallsign();
		if (rtCallsignRaw == nullptr || rtCallsignRaw[0] == '\0')
		{
			static bool loggedMissingTagCallsign = false;
			if (!loggedMissingTagCallsign)
			{
				Logger::info("RenderTags: skipped target with missing callsign");
				loggedMissingTagCallsign = true;
			}
			continue;
		}
		const std::string rtCallsign = rtCallsignRaw;

		const char* aselCallsign = aselTarget.IsValid() ? aselTarget.GetCallsign() : nullptr;
		bool isASEL = (aselCallsign != nullptr && strcmp(aselCallsign, rtCallsign.c_str()) == 0);

		CRadarTargetPositionData RtPos = rt.GetPosition();
		POINT acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());
		CFlightPlan fp = GetPlugIn()->FlightPlanSelect(rtCallsign.c_str());
		const char* fpDestination = fp.IsValid() ? fp.GetFlightPlanData().GetDestination() : nullptr;
		const char* fpOrigin = fp.IsValid() ? fp.GetFlightPlanData().GetOrigin() : nullptr;
		int reportedGs = RtPos.GetReportedGS();

		// Filtering the targets

		bool isAcDisplayed = isVisible(rt);

		bool AcisCorrelated = IsCorrelated(fp, rt);
		const char* assignedSquawk = fp.IsValid() ? fp.GetControllerAssignedData().GetSquawk() : nullptr;
		const char* reportedSquawk = RtPos.GetSquawk();
		const bool hasAssignedSquawk = (assignedSquawk != nullptr && assignedSquawk[0] != '\0');
		const bool hasReportedSquawk = (reportedSquawk != nullptr && reportedSquawk[0] != '\0');
		const bool hasWrongAssignedSquawk = hasAssignedSquawk && hasReportedSquawk &&
			strcmp(assignedSquawk, reportedSquawk) != 0;

		if (tagProModeEnabled && (!hasAssignedSquawk || hasWrongAssignedSquawk))
		{
			// In pro mode, tags are hidden when assigned squawk is missing or mismatched.
			AcisCorrelated = false;
			isAcDisplayed = false;
		}

		if (!AcisCorrelated && reportedGs < 3)
			isAcDisplayed = false;

		const char* systemId = rt.GetSystemID();
		if (systemId != nullptr && systemId[0] != '\0' &&
			std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId) != ReleasedTracks.end())
			isAcDisplayed = false;

		if (!isAcDisplayed)
			continue;

		// Getting the tag center/offset

		POINT TagCenter;
		map<string, POINT>::iterator it = TagsOffsets.find(rtCallsign);
		if (it != TagsOffsets.end()) {
			TagCenter = { acPosPix.x + it->second.x, acPosPix.y + it->second.y };
		}
		else {
			// Use angle:

			if (TagAngles.find(rtCallsign) == TagAngles.end())
				TagAngles[rtCallsign] = 270.0f;

			int lenght = LeaderLineDefaultlenght;
			if (TagLeaderLineLength.find(rtCallsign) != TagLeaderLineLength.end())
				lenght = TagLeaderLineLength[rtCallsign];

			TagCenter.x = long(acPosPix.x + float(lenght * cos(DegToRad(TagAngles[rtCallsign]))));
			TagCenter.y = long(acPosPix.y + float(lenght * sin(DegToRad(TagAngles[rtCallsign]))));
		}

		TagTypes TagType = TagTypes::Departure;		
		TagTypes ColorTagType = TagTypes::Departure;

		if (fpDestination != nullptr && strcmp(fpDestination, activeAirport.c_str()) == 0) {
			// Circuit aircraft are treated as departures; not arrivals
			if (fpOrigin == nullptr || strcmp(fpOrigin, activeAirport.c_str()) != 0) {
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


		const std::string targetCallsign = ToUpperAsciiCopy(rtCallsign);
		map<string, string> TagReplacingMap;
		auto cachedTagData = frameTagDataCache.find(targetCallsign);
		if (cachedTagData != frameTagDataCache.end())
			TagReplacingMap = cachedTagData->second;
		else
			TagReplacingMap = GenerateTagData(rt, fp, isASEL, AcisCorrelated, tagProModeEnabled, transitionAltitude, useAspeedForGate, activeAirport, rtCallsign);
		verboseTargetStep(rtCallsign, "after_tag_data");
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
		addClickableToken("ssid", TAG_CITEM_SID);
		addClickableToken("origin", TAG_CITEM_FPBOX);
		addClickableToken("dest", TAG_CITEM_FPBOX);
		addClickableToken("systemid", TAG_CITEM_NO);
		addClickableToken("groundstatus", TAG_CITEM_GROUNDSTATUS);
		addClickableToken("clearance", TAG_CITEM_CLEARANCE);
		addClickableToken("uk_stand", TAG_CITEM_UKSTAND);
		addClickableToken("remark", TAG_CITEM_REMARK);
		addClickableToken("scratchpad", TAG_CITEM_SCRATCHPAD);
		verboseTargetStep(rtCallsign, "after_clickable_map");

		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//

		// First we need to figure out the tag size
		int TagWidth = 0, TagHeight = 0;
		const int blankWidth = tagBlankWidth;
		const int oneLineHeight = tagOneLineHeight;
		RectF mesureRect;

		CRect previousRect;
		auto itPrev = previousTagSize.find(rtCallsign);
		if (itPrev != previousTagSize.end()) {
			const int prevW = itPrev->second.Width();
			const int prevH = itPrev->second.Height();
			const int prevLeft = TagCenter.x - (prevW / 2);
			const int prevTop = TagCenter.y - (prevH / 2);
			previousRect = CRect(prevLeft,
				prevTop,
				prevLeft + prevW,
				prevTop + prevH);
		}
		else {
			const int prevLeft = TagCenter.x - (TagWidth / 2);
			const int prevTop = TagCenter.y - (TagHeight / 2);
			previousRect = CRect(prevLeft,
				prevTop,
				prevLeft + TagWidth,
				prevTop + TagHeight);
		}

		bool isTagDetailled = isMouseWithin(previousRect) || isTagBeingDragged(rtCallsign);
		verboseTargetStep(rtCallsign, std::string("detail_mode=") + (isTagDetailled ? "1" : "0"));

		std::string ruleTagTypeKey = TagTypeToConfigKey(TagType);
		std::string definitionTagTypeKey = ruleTagTypeKey;
		verboseTargetStep(rtCallsign, "before_onrunway_lookup");

		const bool targetOnRunway = RimcasInstance->isAcOnRunway(rtCallsign);
		verboseTargetStep(rtCallsign, "after_onrunway_lookup");
		const char* statusDefinitionKey = nullptr;
		const auto actypeIt = TagReplacingMap.find("actype");
		const bool isNoFplTag = (actypeIt != TagReplacingMap.end() && actypeIt->second == "NoFPL");
		if (isNoFplTag && (TagType == TagTypes::Departure || TagType == TagTypes::Arrival))
		{
			statusDefinitionKey = "nofpl";
		}
		else if (TagType == TagTypes::Airborne)
		{
			bool isAirborneArrival = false;
			if (fpDestination != nullptr &&
				strcmp(fpDestination, activeAirport.c_str()) == 0 &&
				(fpOrigin == nullptr || strcmp(fpOrigin, activeAirport.c_str()) != 0))
			{
				isAirborneArrival = true;
			}

			definitionTagTypeKey = isAirborneArrival ? "arrival" : "departure";
			ruleTagTypeKey = definitionTagTypeKey;
			const char* airborneStatusKey = isAirborneArrival ? "airarr" : "airdep";
			const char* onRunwayStatusKey = isAirborneArrival ? "airarr_onrunway" : "airdep_onrunway";
			if (targetOnRunway && hasStatusDefinition(definitionTagTypeKey, onRunwayStatusKey))
				statusDefinitionKey = onRunwayStatusKey;
			else
				statusDefinitionKey = airborneStatusKey;
		}
		else if (fp.IsValid())
		{
			GroundStateCategory targetStatus = classifyGroundState(fp.GetGroundState(), reportedGs, targetOnRunway);
			switch (targetStatus)
			{
			case GroundStateCategory::Taxi:
				if (TagType == TagTypes::Departure)
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
				break;
			default:
				break;
			}
		}

		const TagDefinitionCacheEntry* cachedTagDefinition = getCachedTagDefinition(definitionTagTypeKey, isTagDetailled, statusDefinitionKey);
		if (cachedTagDefinition == nullptr)
			continue;
		verboseTargetStep(rtCallsign, "after_tag_definition");

		VacdmPilotData vacdmRulePilotData;
		bool hasVacdmRulePilotData = false;
		auto cachedVacdmLookup = frameVacdmLookupCache.find(targetCallsign);
		if (cachedVacdmLookup != frameVacdmLookupCache.end())
		{
			hasVacdmRulePilotData = cachedVacdmLookup->second.hasData;
			if (hasVacdmRulePilotData)
				vacdmRulePilotData = cachedVacdmLookup->second.data;
		}
		else
		{
			hasVacdmRulePilotData = TryGetVacdmPilotDataForTarget(rt, fp, vacdmRulePilotData);
		}
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
			vacdmTagColorOverrides.targetA = runwayTagColorOverrides.targetA;
		}
		if (runwayTagColorOverrides.hasTagColor)
		{
			vacdmTagColorOverrides.hasTagColor = true;
			vacdmTagColorOverrides.tagR = runwayTagColorOverrides.tagR;
			vacdmTagColorOverrides.tagG = runwayTagColorOverrides.tagG;
			vacdmTagColorOverrides.tagB = runwayTagColorOverrides.tagB;
			vacdmTagColorOverrides.tagA = runwayTagColorOverrides.tagA;
		}
		if (runwayTagColorOverrides.hasTextColor)
		{
			vacdmTagColorOverrides.hasTextColor = true;
			vacdmTagColorOverrides.textR = runwayTagColorOverrides.textR;
			vacdmTagColorOverrides.textG = runwayTagColorOverrides.textG;
			vacdmTagColorOverrides.textB = runwayTagColorOverrides.textB;
			vacdmTagColorOverrides.textA = runwayTagColorOverrides.textA;
		}
		const VacdmColorRuleOverrides structuredTagColorOverrides =
			EvaluateStructuredTagColorRules(
				structuredTagRules,
				ruleTagTypeKey,
				statusDefinitionKey,
				isTagDetailled,
				TagReplacingMap,
				hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		if (structuredTagColorOverrides.hasTargetColor)
		{
			vacdmTagColorOverrides.hasTargetColor = true;
			vacdmTagColorOverrides.targetR = structuredTagColorOverrides.targetR;
			vacdmTagColorOverrides.targetG = structuredTagColorOverrides.targetG;
			vacdmTagColorOverrides.targetB = structuredTagColorOverrides.targetB;
			vacdmTagColorOverrides.targetA = structuredTagColorOverrides.targetA;
		}
		if (structuredTagColorOverrides.hasTagColor)
		{
			vacdmTagColorOverrides.hasTagColor = true;
			vacdmTagColorOverrides.tagR = structuredTagColorOverrides.tagR;
			vacdmTagColorOverrides.tagG = structuredTagColorOverrides.tagG;
			vacdmTagColorOverrides.tagB = structuredTagColorOverrides.tagB;
			vacdmTagColorOverrides.tagA = structuredTagColorOverrides.tagA;
		}
		if (structuredTagColorOverrides.hasTextColor)
		{
			vacdmTagColorOverrides.hasTextColor = true;
			vacdmTagColorOverrides.textR = structuredTagColorOverrides.textR;
			vacdmTagColorOverrides.textG = structuredTagColorOverrides.textG;
			vacdmTagColorOverrides.textB = structuredTagColorOverrides.textB;
			vacdmTagColorOverrides.textA = structuredTagColorOverrides.textA;
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
		verboseTargetStep(rtCallsign, "after_tag_measurement");

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
		CRimcas::RimcasAlerts alert = RimcasInstance->getMovementAlert(rtCallsign);
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
					tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);

				alertTextWidth = static_cast<int>(mesureRect.GetRight());
				alertTextHeight = static_cast<int>(mesureRect.GetBottom());
				int TempTagWidth = alertTextWidth;
				TagWidth = max(TagWidth, TempTagWidth);
				TagHeight += oneLineHeight;
				CollisionTagWidth = max(CollisionTagWidth, TempTagWidth);
				CollisionTagHeight += oneLineHeight;
			}
		}

		const int tagRectLeft = TagCenter.x - (TagWidth / 2);
		const int tagRectTop = TagCenter.y - (TagHeight / 2);
		previousTagSize[rtCallsign] = CRect(tagRectLeft, tagRectTop, tagRectLeft + TagWidth, tagRectTop + TagHeight);

		const std::string colorTagTypeKey = TagTypeToConfigKey(ColorTagType);
		const Value* colorTagLabelSection = nullptr;
		if (LabelsSettings.HasMember(colorTagTypeKey.c_str()) && LabelsSettings[colorTagTypeKey.c_str()].IsObject())
			colorTagLabelSection = &LabelsSettings[colorTagTypeKey.c_str()];

		auto getTagColorFromConfigOrDefault = [&](const char* key, const Color& fallback) -> Color
		{
			if (colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(key) &&
				(*colorTagLabelSection)[key].IsObject())
			{
				return CurrentConfig->getConfigColor((*colorTagLabelSection)[key]);
			}

			if (Logger::is_verbose_mode())
				Logger::info("RenderTags: missing color key=" + std::string(key) + " for type=" + colorTagTypeKey);
			return fallback;
		};
		auto getTagColorWithLegacy = [&](const char* preferredKey, const char* legacyKey, const Color& fallback) -> Color
		{
			if (colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(preferredKey) &&
				(*colorTagLabelSection)[preferredKey].IsObject())
			{
				return CurrentConfig->getConfigColor((*colorTagLabelSection)[preferredKey]);
			}
			if (legacyKey != nullptr &&
				colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(legacyKey) &&
				(*colorTagLabelSection)[legacyKey].IsObject())
			{
				return CurrentConfig->getConfigColor((*colorTagLabelSection)[legacyKey]);
			}
			return fallback;
		};

		Color definedBackgroundColor = Color(255, 53, 126, 187);
		Color definedBackgroundOnRunwayColor = definedBackgroundColor;
		Color definedTextColor = Color::White;
		if (ColorTagType == TagTypes::Departure)
		{
			definedBackgroundColor = getTagColorWithLegacy("background_no_status_color", "gate_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getTagColorWithLegacy("background_on_runway_color", "on_runway_color", definedBackgroundColor);
			definedTextColor = getTagColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == TagTypes::Arrival)
		{
			definedBackgroundColor = getTagColorWithLegacy("background_on_ground_color", "background_color", Color(255, 191, 87, 91));
			definedBackgroundOnRunwayColor = getTagColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getTagColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == TagTypes::Uncorrelated)
		{
			definedBackgroundColor = getTagColorWithLegacy("background_on_ground_color", "background_color", Color(255, 150, 22, 135));
			definedBackgroundOnRunwayColor = getTagColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getTagColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else
		{
			definedBackgroundColor = getTagColorFromConfigOrDefault("background_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getTagColorFromConfigOrDefault("background_color_on_runway", definedBackgroundColor);
			definedTextColor = getTagColorFromConfigOrDefault("text_color", Color::White);
		}

		if (ColorTagType == TagTypes::Departure) {
			if (!TagReplacingMap["asid"].empty() && CurrentConfig->isSidColorAvail(TagReplacingMap["asid"], activeAirport)) {
				definedBackgroundColor = CurrentConfig->getSidColor(TagReplacingMap["asid"], activeAirport);
			}
			if (fp.IsValid() &&
				fp.GetFlightPlanData().GetPlanType() != nullptr &&
				fp.GetFlightPlanData().GetPlanType()[0] == 'I' &&
				TagReplacingMap["asid"].empty() &&
				colorTagLabelSection != nullptr) {
				if (colorTagLabelSection->HasMember("background_no_sid_color") && (*colorTagLabelSection)["background_no_sid_color"].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor((*colorTagLabelSection)["background_no_sid_color"]);
				else if (colorTagLabelSection->HasMember("nosid_color") && (*colorTagLabelSection)["nosid_color"].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor((*colorTagLabelSection)["nosid_color"]);
			}

			if (LabelsSettings.HasMember("departure") && LabelsSettings["departure"].IsObject())
			{
				const Value& departureLabel = LabelsSettings["departure"];
				GroundStateCategory departureStatus = GroundStateCategory::Unknown;
				if (fp.IsValid())
					departureStatus = classifyGroundState(fp.GetGroundState(), reportedGs, targetOnRunway);

				const char* statusColorKey = nullptr;
				const char* legacyStatusColorKey = nullptr;
				switch (departureStatus)
				{
				case GroundStateCategory::Taxi:
					statusColorKey = "background_taxi_color";
					legacyStatusColorKey = "taxi";
					break;
				case GroundStateCategory::Push:
					statusColorKey = "background_push_color";
					legacyStatusColorKey = "push";
					break;
				case GroundStateCategory::Stup:
					statusColorKey = "background_startup_color";
					legacyStatusColorKey = "stup";
					break;
				case GroundStateCategory::Depa:
					statusColorKey = "background_departure_color";
					legacyStatusColorKey = "depa";
					break;
				default:
					statusColorKey = "background_no_status_color";
					legacyStatusColorKey = "nsts";
					break;
				}

				if (statusColorKey != nullptr &&
					departureLabel.HasMember(statusColorKey) &&
					departureLabel[statusColorKey].IsObject())
				{
					definedBackgroundColor = CurrentConfig->getConfigColor(departureLabel[statusColorKey]);
				}
				else if (legacyStatusColorKey != nullptr &&
					departureLabel.HasMember("status_background_colors") &&
					departureLabel["status_background_colors"].IsObject() &&
					departureLabel["status_background_colors"].HasMember(legacyStatusColorKey) &&
					departureLabel["status_background_colors"][legacyStatusColorKey].IsObject())
				{
					definedBackgroundColor = CurrentConfig->getConfigColor(departureLabel["status_background_colors"][legacyStatusColorKey]);
				}
			}
		}
		if (TagReplacingMap["actype"] == "NoFPL" &&
			colorTagLabelSection != nullptr) {
			if (colorTagLabelSection->HasMember("background_no_fpl_color") && (*colorTagLabelSection)["background_no_fpl_color"].IsObject())
				definedBackgroundColor = CurrentConfig->getConfigColor((*colorTagLabelSection)["background_no_fpl_color"]);
			else if (colorTagLabelSection->HasMember("nofpl_color") && (*colorTagLabelSection)["nofpl_color"].IsObject())
				definedBackgroundColor = CurrentConfig->getConfigColor((*colorTagLabelSection)["nofpl_color"]);
		}

		if (TagType == TagTypes::Airborne &&
			fp.IsValid() &&
			AcisCorrelated)
		{
			bool isAirborneDeparture = true;
			std::string originAirport = fpOrigin != nullptr ? fpOrigin : "";
			std::string activeAirportUpper = activeAirport;
			if (!originAirport.empty() && !activeAirportUpper.empty())
			{
				std::transform(originAirport.begin(), originAirport.end(), originAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				std::transform(activeAirportUpper.begin(), activeAirportUpper.end(), activeAirportUpper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				isAirborneDeparture = (originAirport == activeAirportUpper);
			}

			const char* runwaySectionKey = isAirborneDeparture ? "departure" : "arrival";
			if (LabelsSettings.HasMember(runwaySectionKey) && LabelsSettings[runwaySectionKey].IsObject())
			{
				const Value& runwaySection = LabelsSettings[runwaySectionKey];
				if (runwaySection.HasMember("background_airborne_color") && runwaySection["background_airborne_color"].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor(runwaySection["background_airborne_color"]);
				if (runwaySection.HasMember("text_airborne_color") && runwaySection["text_airborne_color"].IsObject())
					definedTextColor = CurrentConfig->getConfigColor(runwaySection["text_airborne_color"]);

				const char* runwayColorKey = "background_on_runway_color";
				if (runwaySection.HasMember(runwayColorKey) && runwaySection[runwayColorKey].IsObject())
				{
					definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(runwaySection[runwayColorKey]);
				}
				else if (isAirborneDeparture &&
					runwaySection.HasMember("on_runway_color") &&
					runwaySection["on_runway_color"].IsObject())
				{
					definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(runwaySection["on_runway_color"]);
				}
				else if (runwaySection.HasMember("background_color_on_runway") &&
					runwaySection["background_color_on_runway"].IsObject())
				{
					definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(runwaySection["background_color_on_runway"]);
				}
			}
			else if (LabelsSettings.HasMember("airborne") && LabelsSettings["airborne"].IsObject())
			{
				const Value& airborneLabel = LabelsSettings["airborne"];
				const char* bgKey = isAirborneDeparture ? "departure_background_color" : "arrival_background_color";
				const char* textKey = isAirborneDeparture ? "departure_text_color" : "arrival_text_color";
				if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
					definedBackgroundColor = CurrentConfig->getConfigColor(airborneLabel[bgKey]);
				if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
					definedTextColor = CurrentConfig->getConfigColor(airborneLabel[textKey]);
				const char* bgOnRunwayKey = isAirborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
				if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
					definedBackgroundOnRunwayColor = CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
			}
		}

		if (vacdmTagColorOverrides.hasTagColor)
		{
			definedBackgroundColor = Color(vacdmTagColorOverrides.tagA, vacdmTagColorOverrides.tagR, vacdmTagColorOverrides.tagG, vacdmTagColorOverrides.tagB);
			definedBackgroundOnRunwayColor = definedBackgroundColor;
		}
		if (vacdmTagColorOverrides.hasTextColor)
		{
			definedTextColor = Color(vacdmTagColorOverrides.textA, vacdmTagColorOverrides.textR, vacdmTagColorOverrides.textG, vacdmTagColorOverrides.textB);
		}
		verboseTargetStep(rtCallsign, "after_color_resolution");

		Color rimcasStageOneColor(255, 160, 90, 30);
		Color rimcasStageTwoColor(255, 150, 0, 0);
		bool rimcasLabelOnly = true;
		if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		{
			const Value& rimcasSection = activeProfile["rimcas"];
			if (rimcasSection.HasMember("background_color_stage_one") && rimcasSection["background_color_stage_one"].IsObject())
				rimcasStageOneColor = CurrentConfig->getConfigColor(rimcasSection["background_color_stage_one"]);
			if (rimcasSection.HasMember("background_color_stage_two") && rimcasSection["background_color_stage_two"].IsObject())
				rimcasStageTwoColor = CurrentConfig->getConfigColor(rimcasSection["background_color_stage_two"]);
			if (rimcasSection.HasMember("rimcas_label_only") && rimcasSection["rimcas_label_only"].IsBool())
				rimcasLabelOnly = rimcasSection["rimcas_label_only"].GetBool();
		}

		Color TagBackgroundColor = RimcasInstance->GetAircraftColor(rtCallsign,
			definedBackgroundColor,
			definedBackgroundOnRunwayColor,
			rimcasStageOneColor,
			rimcasStageTwoColor);

		// We need to figure out if the tag color changes according to RIMCAS alerts, or not
		if (rimcasLabelOnly)
			TagBackgroundColor = RimcasInstance->GetAircraftColor(rtCallsign,
			definedBackgroundColor,
			definedBackgroundOnRunwayColor);

		TagBackgroundColor = ColorManager->get_corrected_color("label", TagBackgroundColor);
		verboseTargetStep(rtCallsign, "after_background_color");

		// Drawing the tag background

		// Slightly enlarge tag hitbox, center it, and draw rounded background.
		const int padding = 3;
		const int tagBackgroundLeft = TagCenter.x - (TagWidth / 2) - padding;
		const int tagBackgroundTop = TagCenter.y - (TagHeight / 2) - padding;
		CRect TagBackgroundRect(tagBackgroundLeft, tagBackgroundTop, tagBackgroundLeft + TagWidth + (padding * 2), tagBackgroundTop + TagHeight + (padding * 2));
		const int tagCollisionLeft = TagCenter.x - (CollisionTagWidth / 2) - padding;
		const int tagCollisionTop = TagCenter.y - (CollisionTagHeight / 2) - padding;
		CRect TagCollisionRect(tagCollisionLeft, tagCollisionTop, tagCollisionLeft + CollisionTagWidth + (padding * 2), tagCollisionTop + CollisionTagHeight + (padding * 2));
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
		if (isMouseWithin(TagBackgroundRect) || isTagBeingDragged(rtCallsign))
		{
			Pen pw(ColorManager->get_corrected_color("label", Color::White));
			graphics.DrawPath(&pw, &roundedPath);
		}

		// Drawing the tag text

		SolidBrush FontColor(ColorManager->get_corrected_color("label", definedTextColor));
		Color squawkErrorColorValue(255, 255, 0, 0);
		if (LabelsSettings.HasMember("squawk_error_color") && LabelsSettings["squawk_error_color"].IsObject())
			squawkErrorColorValue = CurrentConfig->getConfigColor(LabelsSettings["squawk_error_color"]);
		SolidBrush SquawkErrorColor(ColorManager->get_corrected_color("label",
			squawkErrorColorValue));
		SolidBrush ClearanceNotReceivedColor(ColorManager->get_corrected_color("label", Color(255, 235, 70, 70)));
		SolidBrush ClearanceReceivedColor(ColorManager->get_corrected_color("label", Color(255, 95, 225, 120)));
		auto getRimcasEditorColor = [&](const char* key, const Color& fallback) -> Color
		{
			if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
			{
				const Value& rimcas = activeProfile["rimcas"];
				if (rimcas.HasMember(key) && rimcas[key].IsObject())
					return CurrentConfig->getConfigColor(rimcas[key]);
			}
			return fallback;
		};
		SolidBrush AlertTextColorCaution(ColorManager->get_corrected_color("label",
			getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30))));
		SolidBrush AlertTextColorWarning(ColorManager->get_corrected_color("label",
			getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255))));
		SolidBrush AlertColorCaution(ColorManager->get_corrected_color("label",
			getRimcasEditorColor("caution_alert_background_color", Color(230, 255, 215, 0))));
		SolidBrush AlertColorWarning(ColorManager->get_corrected_color("label",
			getRimcasEditorColor("warning_alert_background_color", Color(230, 200, 40, 40))));
		const bool isClearanceReceived = (fp.IsValid() && fp.GetClearenceFlag());
		std::unique_ptr<SolidBrush> VacdmTobtTextBrush;
		std::unique_ptr<SolidBrush> VacdmTsatTextBrush;

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
		const std::string tagBottomLine = GetBottomLine(rtCallsign.c_str());
		const char* tagBottomLineText = tagBottomLine.c_str();

		// Adding the tag screen object
		tagAreas[rtCallsign] = TagBackgroundRect;
		tagCollisionAreas[rtCallsign] = TagCollisionRect;
		AddScreenObject(DRAWING_TAG, rtCallsign.c_str(), TagBackgroundRect, true, tagBottomLineText);

		TagBackgroundRect = oldCrectSave;

		// Clickable zones
		int heightOffset = 0;
		// Drawing Alert
		if (alert != CRimcas::NONE &&
			TagType == TagTypes::Departure &&
			!alertStr.empty())
		{
			wstring welement = wstring(alertStr.begin(), alertStr.end());
			const int alertLineHeight = max(alertTextHeight, oneLineHeight);
			const int alertTop = TagBackgroundRect.top + heightOffset;
			const int alertBottom = TagBackgroundRect.top + padding + heightOffset + alertLineHeight;
			CRect ItemRect(TagBackgroundRect.left,
				alertTop,
				TagBackgroundRect.right,
				alertBottom);
			CRimcas::RimcasAlertSeverity severity = RimcasInstance->getAlertSeverity(alert);
			SolidBrush* AlertColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertColorWarning : &AlertColorCaution;
			SolidBrush* RimcasTextColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertTextColorWarning : &AlertTextColorCaution;
			graphics.SetClip(&roundedPath, CombineModeReplace);
			graphics.FillRectangle(AlertColor, CopyRect(ItemRect));
			graphics.ResetClip();

			wstring walertStr = wstring(alertStr.begin(), alertStr.end());
			const int alertTextOffsetY = max(0, (alertLineHeight - alertTextHeight + 1) / 2);
			graphics.DrawString(walertStr.c_str(), wcslen(walertStr.c_str()), tagRegularFont,
				PointF(Gdiplus::REAL(textLeft), Gdiplus::REAL(textTop + heightOffset + alertTextOffsetY)),
				&Gdiplus::StringFormat(), RimcasTextColor);

			CRect alertRect(TagBackgroundRect.left, TagBackgroundRect.top + heightOffset,
				TagBackgroundRect.left + alertTextWidth, TagBackgroundRect.top + heightOffset + max(alertTextHeight, oneLineHeight));

			AddScreenObject(TagClickableMap[alertStr], rtCallsign.c_str(), alertRect, true, tagBottomLineText);
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
				const int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
				graphics.DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
					PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
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

				AddScreenObject(clickItemType, rtCallsign.c_str(), ItemRect, true, tagBottomLineText);

				widthOffset += renderedElement.measuredWidth;
				widthOffset += blankWidth;
			}

			heightOffset += oneLineHeight;
		}


	}

}
