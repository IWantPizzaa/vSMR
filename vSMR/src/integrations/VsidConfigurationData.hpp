#pragma once

#include "integrations/VsidBridgeData.hpp"
#include "shared/JsonDocument.hpp"

namespace VsmrVsid
{
	inline constexpr std::size_t MaximumConfigurationBytes = 65536U;
	using RuleValues = std::map<std::string, bool>;
	using AirportRuleValues = std::map<std::string, RuleValues>;

	struct ConfigurationStatus
	{
		std::string status;
		std::string profile;
		std::string detail;
		bool manual = false;
		bool operator==(const ConfigurationStatus& other) const
		{
			return status == other.status && profile == other.profile && detail == other.detail && manual == other.manual;
		}
	};
	using ConfigurationStatuses = std::map<std::string, ConfigurationStatus>;

	inline bool CanResumeConfiguration(const std::optional<ConfigurationStatus>& status)
	{
		return status && status->manual && status->status == "MANUAL";
	}

	inline std::string ConfigurationSectionLabel(const std::optional<ConfigurationStatus>& configuration)
	{
		if (!configuration) return "CONFIG";
		const auto& status = configuration->status;
		if (status == "MATCHED") return "CONFIG - AUTO";
		if (status == "MANUAL" || status == "OFF") return "CONFIG - " + status;
		if (status == "UNDETERMINED" || status == "AMBIGUOUS") return "CONFIG - CHECK RUNWAYS";
		if (status == "MISSING_RULE") return "CONFIG - MISSING RULE";
		if (status == "NOT_LOADED") return "CONFIG - NOT LOADED";
		if (status == "UNMANAGED") return "CONFIG - MANUAL ONLY";
		return "CONFIG";
	}

	inline AirportRuleValues ParseRuleValues(std::string_view input)
	{
		if (input.size() > MaximumConfigurationBytes) return {};
		rapidjson::Document document;
		VsmrJson::ParseDocument(document, input);
		if (document.HasParseError() || !document.IsObject()) return {};
		AirportRuleValues result;
		for (auto airport = document.MemberBegin(); airport != document.MemberEnd(); ++airport)
		{
			const auto icao = NormalizeAirport({airport->name.GetString(), airport->name.GetStringLength()});
			if (icao.empty() || !airport->value.IsObject()) return {};
			RuleValues rules;
			for (auto rule = airport->value.MemberBegin(); rule != airport->value.MemberEnd(); ++rule)
			{
				std::string key(rule->name.GetString(), rule->name.GetStringLength());
				std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (key.empty() || key.find('\0') != std::string::npos || !rule->value.IsBool() ||
					!rules.emplace(key, rule->value.GetBool()).second) return {};
			}
			if (!result.emplace(icao, std::move(rules)).second) return {};
		}
		return result;
	}

	inline ConfigurationStatuses ParseConfigurationStatuses(std::string_view input)
	{
		if (input.size() > MaximumConfigurationBytes) return {};
		rapidjson::Document document;
		VsmrJson::ParseDocument(document, input);
		if (document.HasParseError() || !document.IsObject()) return {};
		ConfigurationStatuses result;
		for (auto airport = document.MemberBegin(); airport != document.MemberEnd(); ++airport)
		{
			const auto icao = NormalizeAirport({airport->name.GetString(), airport->name.GetStringLength()});
			const auto& value = airport->value;
			if (icao.empty() || !value.IsObject() || value.MemberCount() != 4 || !value.HasMember("status") || !value["status"].IsString() ||
				!value.HasMember("profile") || !value["profile"].IsString() || !value.HasMember("detail") || !value["detail"].IsString() ||
				!value.HasMember("manual") || !value["manual"].IsBool()) return {};
			const auto text = [&](const char* key) {
				return std::string(value[key].GetString(), value[key].GetStringLength());
			};
			ConfigurationStatus state{text("status"), text("profile"), text("detail"), value["manual"].GetBool()};
			for (const auto* field : { &state.status, &state.profile, &state.detail })
				if (std::any_of(field->begin(), field->end(), [](unsigned char c) { return c < 0x20U || c == 0x7fU; })) return {};
			if (state.status != "MATCHED" && state.status != "OFF" && state.status != "MANUAL" && state.status != "UNDETERMINED" &&
				state.status != "AMBIGUOUS" && state.status != "UNMANAGED" && state.status != "NOT_LOADED" && state.status != "MISSING_RULE") return {};
			if (!result.emplace(icao, std::move(state)).second) return {};
		}
		return result;
	}

	inline VsmrParis::State ResolveConfiguration(std::string_view airport, const RuleValues& rules)
	{
		VsmrParis::State state;
		if (airport == "LFOB")
		{
			const auto east = rules.find("pgeast");
			if (east != rules.end()) state.pg = east->second ? VsmrParis::Flow::East : VsmrParis::Flow::West;
			return state;
		}
		if (airport == "LFPG" || airport == "LFPO" || airport == "LFPB")
		{
			const auto opposing = rules.find("opposing");
			if (opposing != rules.end()) state.linked = !opposing->second;
			return state;
		}
		return VsmrParis::Resolve(rules, airport);
	}

	inline std::optional<LfpgTaxiMode> ResolveLfpgTaxi(const AirportRuleValues& areas)
	{
		const auto airport = areas.find("LFPG");
		if (airport == areas.end()) return {};
		const auto north = airport->second.find("north"), south = airport->second.find("south");
		if (north == airport->second.end() || south == airport->second.end() || north->second != south->second) return {};
		for (const auto& [name, enabled] : airport->second)
			if (name != "north" && name != "south" && enabled) return {};
		return north->second ? LfpgTaxiMode::MinimumTaxiing : LfpgTaxiMode::GroundCrossing;
	}

	inline bool HasLfpgTaxiAreas(const AirportRuleValues& areas)
	{
		const auto airport = areas.find("LFPG");
		return airport != areas.end() && airport->second.count("north") && airport->second.count("south");
	}

	inline std::string BuildGenericRuleCommand(CommandAction action, std::string_view airport, const RuleValues& rules)
	{
		const auto icao = NormalizeAirport(airport);
		if (icao.empty()) return {};
		std::string assignments;
		if ((icao == "LFPG" || icao == "LFPO" || icao == "LFPB") &&
			(action == CommandAction::LfpgLinked || action == CommandAction::LfpgUnlinked) && rules.count("opposing"))
			assignments = action == CommandAction::LfpgLinked ? "OPPOSING=off" : "OPPOSING=on";
		else if (icao == "LFOB" && rules.count("pgeast") &&
			(action == CommandAction::BeauvaisWest || action == CommandAction::BeauvaisEast))
			assignments = action == CommandAction::BeauvaisEast ? "PGEAST=on" : "PGEAST=off";
		else if (icao != "LFOB" && VsmrParis::IsRegional(icao) && IsRegionalAction(action))
		{
			const std::string selected = action == CommandAction::ParisWLPG ? "wlpg" : action == CommandAction::ParisELPG ? "elpg" :
				action == CommandAction::ParisWIPG ? "wipg" : "eipg";
			for (const auto rule : VsmrParis::RegionalRules)
			{
				if (!rules.count(std::string(rule))) return {};
				if (!assignments.empty()) assignments += ' ';
				assignments += std::string(rule) + (rule == selected ? "=on" : "=off");
			}
		}
		return assignments.empty() ? std::string{} : ".vsid rules " + icao + " " + assignments;
	}
}
