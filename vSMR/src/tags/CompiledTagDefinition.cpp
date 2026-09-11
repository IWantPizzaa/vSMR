#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/CompiledTagDefinition.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "radar/RadarUiSupport.hpp"

namespace TagColorRules = VsmrTagColorRules;
namespace
{
	using namespace VsmrScene;
	int ActionForTagToken(const std::string& token)
	{
		const std::string key = ToLowerAsciiCopy(token);
		if (key == "callsign") return TAG_CITEM_CALLSIGN;
		if (key == "systemid") return TAG_CITEM_MANUALCORRELATE;
		if (key == "actype" || key == "sctype" || key == "sqerror" || key == "wake" || key == "origin" || key == "dest") return TAG_CITEM_FPBOX;
		if (key == "deprwy" || key == "seprwy" || key == "arvrwy" || key == "srvrwy" || key == "vsid_rwy") return TAG_CITEM_RWY;
		if (key == "gate" || key == "sate") return TAG_CITEM_GATE;
		if (key == "asid" || key == "ssid" || key == "sid" || key == "shid" || key == "vsid_sid") return TAG_CITEM_SID;
		if (key == "groundstatus" || key == "gstatus") return TAG_CITEM_GROUNDSTATUS;
		if (key == "clearance" || key == "cleared") return TAG_CITEM_CLEARANCE;
		if (key == "uk_stand") return TAG_CITEM_UKSTAND;
		if (key == "remark") return TAG_CITEM_REMARK;
		if (key == "scratchpad") return TAG_CITEM_SCRATCHPAD;
		if (key == "holdingpoint") return TAG_CITEM_HOLDINGPOINT;
		if (key == "ready_startup") return TAG_CITEM_READY_STARTUP;
		return TAG_CITEM_NO;
	}

	const rapidjson::Value* ResolveTagDefinition(
		const rapidjson::Value& labels,
		const std::string& type,
		const std::string& status,
		bool detailed)
	{
		if (!labels.IsObject() || !labels.HasMember(type.c_str()) || !labels[type.c_str()].IsObject())
			return nullptr;

		const rapidjson::Value& section = labels[type.c_str()];
		bool inheritDetailed = false;
		auto readInheritance = [&](const rapidjson::Value& object, bool& value) -> bool
		{
			if (object.HasMember("definition_detailed_inherits_normal") && object["definition_detailed_inherits_normal"].IsBool())
			{
				value = object["definition_detailed_inherits_normal"].GetBool();
				return true;
			}
			if (object.HasMember("definition_detailed_same_as_definition") && object["definition_detailed_same_as_definition"].IsBool())
			{
				value = object["definition_detailed_same_as_definition"].GetBool();
				return true;
			}
			return false;
		};

		readInheritance(labels, inheritDetailed);
		readInheritance(section, inheritDetailed);
		const rapidjson::Value* statusSection = nullptr;
		if (!status.empty() && status != "default" &&
			section.HasMember("status_definitions") && section["status_definitions"].IsObject())
		{
			const rapidjson::Value& statuses = section["status_definitions"];
			auto findStatus = [&](const std::string& key) -> const rapidjson::Value*
			{
				if (statuses.HasMember(key.c_str()) && statuses[key.c_str()].IsObject())
					return &statuses[key.c_str()];
				return nullptr;
			};
			statusSection = findStatus(status);
			if (statusSection == nullptr && status == "airdep_onrunway")
				statusSection = findStatus("airdep");
			else if (statusSection == nullptr && status == "airarr_onrunway")
				statusSection = findStatus("airarr");
			if (statusSection != nullptr)
				readInheritance(*statusSection, inheritDetailed);
		}

		const char* key = (detailed && !inheritDetailed) ? "definition_detailed" : "definition";
		const char* legacyKey = (detailed && !inheritDetailed) ? "definitionDetailled" : nullptr;
		auto fromObject = [&](const rapidjson::Value& object) -> const rapidjson::Value*
		{
			if (object.HasMember(key) && object[key].IsArray())
				return &object[key];
			if (legacyKey != nullptr && object.HasMember(legacyKey) && object[legacyKey].IsArray())
				return &object[legacyKey];
			return nullptr;
		};

		if (statusSection != nullptr)
		{
			if (const rapidjson::Value* definition = fromObject(*statusSection))
				return definition;
		}
		if (const rapidjson::Value* definition = fromObject(section))
			return definition;

		if (detailed && !inheritDetailed)
		{
			if (statusSection != nullptr && statusSection->HasMember("definition") && (*statusSection)["definition"].IsArray())
				return &(*statusSection)["definition"];
			if (section.HasMember("definition") && section["definition"].IsArray())
				return &section["definition"];
		}
		return nullptr;
	}

	std::vector<VsmrTags::TextPart> CompileText(const std::string& text)
	{
		static constexpr std::string_view keys[] = { "event_booking", "ready_startup", "groundstatus", "holdingpoint", "flightlevel", "scratchpad", "clearance", "callsign", "systemid", "tendency", "uk_stand", "vsid_cfl", "vsid_rwy", "vsid_sid", "sqerror", "actype", "arvrwy", "deprwy", "origin", "remark", "sctype", "seprwy", "srvrwy", "aobt", "aort", "asat", "asid", "asrt", "atot", "ctot", "dest", "gate", "sate", "ssid", "tobt", "tsac", "tsat", "ttot", "wake", "ssr", "gs" };
		std::vector<VsmrTags::TextPart> result;
		std::string literal;
		for (std::size_t offset = 0; offset < text.size();)
		{
			std::string_view match;
			for (const auto key : keys)
			{
				if (text.compare(offset, key.size(), key) == 0) { match = key; break; }
			}
			if (match.empty()) { literal += text[offset++]; continue; }
			if (!literal.empty()) { result.push_back({ std::move(literal), false }); literal.clear(); }
			result.push_back({ std::string(match), true });
			offset += match.size();
		}
		if (!literal.empty()) result.push_back({ std::move(literal), false });
		return result;
	}
}

const VsmrTags::CompiledDefinition& VsmrTags::DefinitionCache::Get(
	const rapidjson::Value& labels, const std::string& type, const std::string& status, bool detailed)
{
	const auto key = std::make_tuple(type, status, detailed);
	const auto found = definitions_.find(key);
	if (found != definitions_.end()) return found->second;
	CompiledDefinition result;
	const auto* definition = ResolveTagDefinition(labels, type, status, detailed);
	if (definition != nullptr)
	{
		const auto lineTexts = TagColorRules::ConvertDefinitionValueToLineTexts(*definition);
		TagColorRules::CollectCdmColorRulesFromLineTexts(lineTexts, result.cdm);
		TagColorRules::CollectRunwayColorRulesFromLineTexts(lineTexts, result.runway);
		for (const auto& sourceLine : definition->GetArray())
		{
			std::vector<CompiledElement> line;
			auto add = [&](const rapidjson::Value& value)
			{
				if (!value.IsString()) return;
				const auto styled = ParseDefinitionTokenStyle(std::string(value.GetString(), value.GetStringLength()));
				TagColorRules::CdmColorRuleDefinition cdmRule;
				TagColorRules::RunwayColorRuleDefinition runwayRule;
				if (TagColorRules::TryParseCdmColorRuleToken(styled.token, cdmRule) ||
					TagColorRules::TryParseRunwayColorRuleToken(styled.token, runwayRule)) return;
				CompiledElement element;
				element.style.token = styled.token;
				element.style.bold = styled.bold;
				element.style.hasCustomColor = styled.hasCustomColor;
				element.style.customColor = { 255, static_cast<std::uint8_t>(styled.colorR),
					static_cast<std::uint8_t>(styled.colorG), static_cast<std::uint8_t>(styled.colorB) };
				element.style.clearanceToken = TryParseClearanceTokenDisplay(styled.token,
					element.notClearedText, element.clearedText);
				element.style.action = element.style.clearanceToken ? TAG_CITEM_CLEARANCE : ActionForTagToken(styled.token);
				if (!element.style.clearanceToken)
				{
					element.text = CompileText(styled.token);
					for (const auto& part : element.text)
						if (part.token) result.dependencies.push_back(part.text);
				}
				line.push_back(std::move(element));
			};
			if (sourceLine.IsArray()) for (const auto& token : sourceLine.GetArray()) add(token);
			else add(sourceLine);
			if (!line.empty()) result.lines.push_back(std::move(line));
		}
	}
	std::sort(result.dependencies.begin(), result.dependencies.end());
	result.dependencies.erase(std::unique(result.dependencies.begin(), result.dependencies.end()), result.dependencies.end());
	return definitions_.emplace(key, std::move(result)).first->second;
}

bool VsmrTags::UpdateTagVariant(const CompiledDefinition& definition,
	const VsmrScene::Target& target, bool detailed, VsmrScene::TagVariant& result)
{
	const unsigned flags = (detailed ? 1U : 0U) | (target.hasFlightPlan ? 2U : 0U) |
		(target.correlated ? 4U : 0U) | (target.tag.clearanceReceived ? 8U : 0U);
	const bool definitionChanged = result.definitionIdentity != definition.identity;
	bool changed = definitionChanged || flags != result.evaluationFlags;
	result.evaluatedInputs.resize(definition.dependencies.size());
	for (std::size_t i = 0; i < definition.dependencies.size(); ++i)
	{
		const auto& key = definition.dependencies[i];
		const auto value = target.tag.tokens.find(key);
		const auto& text = value == target.tag.tokens.end() ? key : value->second;
		if (result.evaluatedInputs[i] != text)
		{
			result.evaluatedInputs[i] = text;
			changed = true;
		}
	}
	if (!changed) return false;
	result.definitionIdentity = definition.identity;
	result.evaluationFlags = flags;
	result.lines.resize(definition.lines.size());
	for (std::size_t i = 0; i < definition.lines.size(); ++i)
	{
		const auto& sourceLine = definition.lines[i];
		auto& line = result.lines[i];
		line.elements.resize(sourceLine.size());
		line.visible = false;
		for (std::size_t j = 0; j < sourceLine.size(); ++j)
		{
			const auto& source = sourceLine[j];
			auto& element = line.elements[j];
			if (definitionChanged) element = source.style;
			element.text.clear();
			if (element.clearanceToken)
			{
				if (target.hasFlightPlan && target.correlated)
					element.text = target.tag.clearanceReceived ? source.clearedText : source.notClearedText;
			}
			else for (const auto& part : source.text)
			{
				const auto value = part.token ? target.tag.tokens.find(part.text) : target.tag.tokens.end();
				element.text += value == target.tag.tokens.end() ? part.text : value->second;
			}
			if (!detailed && ToLowerAsciiCopy(element.token) == "scratchpad" && element.text == "...") element.text.clear();
			if (detailed && element.action == TAG_CITEM_HOLDINGPOINT && element.text.empty()) element.text = "HP";
			line.visible = line.visible || !element.text.empty();
		}
	}
	return true;
}

VsmrScene::TagVariant VsmrTags::BuildTagVariant(const CompiledDefinition& definition,
	const VsmrScene::Target& target, bool detailed)
{
	VsmrScene::TagVariant result;
	UpdateTagVariant(definition, target, detailed, result);
	return result;
}
