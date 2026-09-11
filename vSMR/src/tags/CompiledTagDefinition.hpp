#pragma once

#include "scene/RadarScene.hpp"
#include "tags/TagColorRules.hpp"
#include "rapidjson/document.h"

#include <map>
#include <tuple>

namespace VsmrTags
{
	struct TextPart
	{
		std::string text;
		bool token = false;
	};

	struct CompiledElement
	{
		VsmrScene::TagElement style;
		std::vector<TextPart> text;
		std::string notClearedText;
		std::string clearedText;
	};

	struct CompiledDefinition
	{
		std::vector<std::vector<CompiledElement>> lines;
		std::vector<VsmrTagColorRules::CdmColorRuleDefinition> cdm;
		std::vector<VsmrTagColorRules::RunwayColorRuleDefinition> runway;
	};

	class DefinitionCache
	{
	public:
		void Clear() { definitions_.clear(); }
		const CompiledDefinition& Get(const rapidjson::Value& labels,
			const std::string& type, const std::string& status, bool detailed);
	private:
		std::map<std::tuple<std::string, std::string, bool>, CompiledDefinition> definitions_;
	};

	VsmrScene::TagVariant BuildTagVariant(const CompiledDefinition& definition,
		const VsmrScene::Target& target, bool detailed);
}
