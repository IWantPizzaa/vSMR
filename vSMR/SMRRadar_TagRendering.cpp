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
		}
		if (rule.applyTag)
		{
			overrides.hasTagColor = true;
			overrides.tagR = rule.tagR;
			overrides.tagG = rule.tagG;
			overrides.tagB = rule.tagB;
		}
		if (rule.applyText)
		{
			overrides.hasTextColor = true;
			overrides.textR = rule.textR;
			overrides.textG = rule.textG;
			overrides.textB = rule.textB;
		}
	}

	return overrides;
}
}

void CSMRRadar::RenderTags(Graphics& graphics, CDC& dc, bool frameProModeEnabled, const FrameTagDataCache& frameTagDataCache, const FrameVacdmLookupCache& frameVacdmLookupCache)
{
	// Drawing the Tags
	VSMR_REFRESH_LOG("Tags loop");
	const bool tagProModeEnabled = frameProModeEnabled;
	const int transitionAltitude = GetPlugIn()->GetTransitionAltitude();
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	const Value& LabelsSettings = activeProfile["labels"];
	const bool tagDetailedSameAsDefinition =
		LabelsSettings.HasMember("definition_detailed_same_as_definition") &&
		LabelsSettings["definition_detailed_same_as_definition"].IsBool() &&
		LabelsSettings["definition_detailed_same_as_definition"].GetBool();
	const auto isDetailedSameAsDefinitionForContext = [&](const std::string& tagTypeKey, const char* statusDefinitionKey) -> bool
	{
		if (!LabelsSettings.IsObject() ||
			!LabelsSettings.HasMember(tagTypeKey.c_str()) ||
			!LabelsSettings[tagTypeKey.c_str()].IsObject())
		{
			return tagDetailedSameAsDefinition;
		}

		const Value& labelSection = LabelsSettings[tagTypeKey.c_str()];
		const char* key = "definition_detailed_same_as_definition";

		if (statusDefinitionKey != nullptr &&
			labelSection.HasMember("status_definitions") &&
			labelSection["status_definitions"].IsObject() &&
			labelSection["status_definitions"].HasMember(statusDefinitionKey) &&
			labelSection["status_definitions"][statusDefinitionKey].IsObject())
		{
			const Value& statusSection = labelSection["status_definitions"][statusDefinitionKey];
			if (statusSection.HasMember(key) && statusSection[key].IsBool())
				return statusSection[key].GetBool();
		}

		if (labelSection.HasMember(key) && labelSection[key].IsBool())
			return labelSection[key].GetBool();

		return tagDetailedSameAsDefinition;
	};
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
			labelSection.HasMember("definitionDetailled");
		const char* configKey = useDetailedDefinition ? "definitionDetailled" : "definition";
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


		const std::string targetCallsign = rt.GetCallsign() != nullptr ? ToUpperAsciiCopy(rt.GetCallsign()) : std::string();
		map<string, string> TagReplacingMap;
		auto cachedTagData = frameTagDataCache.find(targetCallsign);
		if (cachedTagData != frameTagDataCache.end())
			TagReplacingMap = cachedTagData->second;
		else
			TagReplacingMap = GenerateTagData(rt, fp, isASEL, AcisCorrelated, tagProModeEnabled, transitionAltitude, useAspeedForGate, activeAirport);
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
		const auto actypeIt = TagReplacingMap.find("actype");
		const bool isNoFplTag = (actypeIt != TagReplacingMap.end() && actypeIt->second == "NoFPL");
		if (isNoFplTag && (TagType == TagTypes::Departure || TagType == TagTypes::Arrival))
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
		const VacdmColorRuleOverrides structuredTagColorOverrides =
			EvaluateStructuredTagColorRules(
				structuredTagRules,
				tagTypeKey,
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
		}
		if (structuredTagColorOverrides.hasTagColor)
		{
			vacdmTagColorOverrides.hasTagColor = true;
			vacdmTagColorOverrides.tagR = structuredTagColorOverrides.tagR;
			vacdmTagColorOverrides.tagG = structuredTagColorOverrides.tagG;
			vacdmTagColorOverrides.tagB = structuredTagColorOverrides.tagB;
		}
		if (structuredTagColorOverrides.hasTextColor)
		{
			vacdmTagColorOverrides.hasTextColor = true;
			vacdmTagColorOverrides.textR = structuredTagColorOverrides.textR;
			vacdmTagColorOverrides.textG = structuredTagColorOverrides.textG;
			vacdmTagColorOverrides.textB = structuredTagColorOverrides.textB;
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
