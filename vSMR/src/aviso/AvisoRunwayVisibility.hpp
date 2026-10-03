#pragma once

#include "rapidjson/document.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace VsmrAviso
{
	struct RunwayActivity { bool arrival = false; bool departure = false; };
	using AirportRunwayActivity = std::map<std::string, std::map<std::string, RunwayActivity>>;

	inline std::string NormalizeAirport(std::string_view value)
	{
		const auto first = value.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) return {};
		value.remove_prefix(first);
		value = value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
		if (value.size() != 4) return {};
		std::string airport(value);
		for (char& c : airport) {
			if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
			if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return {};
		}
		return airport;
	}

	inline std::string NormalizeRunway(std::string_view value)
	{
		const auto first = value.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) return {};
		value.remove_prefix(first);
		value = value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
		size_t digits = 0;
		int number = 0;
		while (digits < value.size() && digits < 2 && value[digits] >= '0' && value[digits] <= '9')
			number = number * 10 + value[digits++] - '0';
		if (digits == 0 || number < 1 || number > 36 || value.size() > digits + 1) return {};
		std::string result = (number < 10 ? "0" : "") + std::to_string(number);
		if (value.size() > digits)
		{
			char suffix = value[digits];
			if (suffix >= 'a' && suffix <= 'z') suffix -= 'a' - 'A';
			if (suffix != 'L' && suffix != 'C' && suffix != 'R') return {};
			result += suffix;
		}
		return result;
	}

	// Read operational selections from the active sector, not the geometry-only
	// sector attached to an ASR. Restore the caller's screen source afterwards.
	// Copy each SDK string before the next SDK call (returned storage is borrowed).
	template<typename Plugin, typename Screen>
	AirportRunwayActivity ReadActiveRunwayActivity(Plugin& plugin, Screen* screen, int runwayType)
	{
		struct RestoreScreen {
			Plugin& plugin; Screen* screen;
			~RestoreScreen() { plugin.SelectScreenSectorfile(screen); }
		} restore{ plugin, screen };
		plugin.SelectActiveSectorfile();
		AirportRunwayActivity result;
		for (auto element = plugin.SectorFileElementSelectFirst(runwayType); element.IsValid();
			element = plugin.SectorFileElementSelectNext(element, runwayType))
		{
			const char* rawAirport = element.GetAirportName();
			const std::string airport = NormalizeAirport(rawAirport ? rawAirport : "");
			if (airport.empty()) continue;
			for (int end = 0; end < 2; ++end) {
				const char* rawRunway = element.GetRunwayName(end);
				const std::string runway = NormalizeRunway(rawRunway ? rawRunway : "");
				if (runway.empty()) continue;
				auto& activity = result[airport][runway];
				// Multiple sector entries must not overwrite an active observation.
				const bool arrival = element.IsElementActive(false, end);
				const bool departure = element.IsElementActive(true, end);
				activity.arrival = activity.arrival || arrival;
				activity.departure = activity.departure || departure;
			}
		}
		return result;
	}

	inline const rapidjson::Value* RuleMember(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject()) return nullptr;
		const auto it = object.FindMember(key);
		return it == object.MemberEnd() ? nullptr : &it->value;
	}

	struct RunwayVisibilityRule
	{
		std::string airport;
		std::string operation = "either";
		bool all = false;
		bool enabled = true;
		bool matched = true;
		bool unmatched = false;
		std::vector<std::string> runways, excluded;
	};

	inline bool ParseRunwayVisibilityRule(const rapidjson::Value& value, RunwayVisibilityRule& rule)
	{
		rule = {};
		if (!value.IsObject()) return false;
		for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
		{
			const std::string_view key(it->name.GetString(), it->name.GetStringLength());
			if (key != "airport" && key != "runways" && key != "exclude_runways" && key != "operation" &&
				key != "match" && key != "enabled" && key != "visible_when_matched" && key != "visible_when_unmatched") return false;
		}
		auto boolean = [&](const char* key, bool& target) {
			const auto* item = RuleMember(value, key);
			if (!item) return true;
			if (!item->IsBool()) return false;
			target = item->GetBool(); return true;
		};
		if (!boolean("enabled", rule.enabled) || !boolean("visible_when_matched", rule.matched) ||
			!boolean("visible_when_unmatched", rule.unmatched)) return false;
		if (!rule.enabled) return true;
		const auto* airport = RuleMember(value, "airport");
		if (!airport || !airport->IsString()) return false;
		rule.airport = NormalizeAirport({ airport->GetString(), airport->GetStringLength() });
		if (rule.airport.empty()) return false;
		if (const auto* operation = RuleMember(value, "operation"))
		{
			if (!operation->IsString()) return false;
			rule.operation.assign(operation->GetString(), operation->GetStringLength());
			if (rule.operation != "either" && rule.operation != "arrival" && rule.operation != "departure") return false;
		}
		if (const auto* match = RuleMember(value, "match"))
		{
			if (!match->IsString()) return false;
			const std::string_view mode(match->GetString(), match->GetStringLength());
			if (mode != "any" && mode != "all") return false;
			rule.all = mode == "all";
		}
		auto list = [&](const char* key, std::vector<std::string>& target, bool required) {
			const auto* items = RuleMember(value, key);
			if (!items) return !required;
			if (!items->IsArray() || items->Size() > 64 || (required && items->Empty())) return false;
			for (const auto& item : items->GetArray())
			{
				if (!item.IsString()) return false;
				auto runway = NormalizeRunway({ item.GetString(), item.GetStringLength() });
				if (runway.empty()) return false;
				target.push_back(std::move(runway));
			}
			return true;
		};
		return list("runways", rule.runways, true) && list("exclude_runways", rule.excluded, false);
	}

	inline bool RunwayRuleMatches(const RunwayVisibilityRule& rule, const AirportRunwayActivity& activity)
	{
		const auto airport = activity.find(rule.airport);
		if (airport == activity.end()) return false;
		auto active = [&](const std::string& pattern) {
			for (const auto& entry : airport->second)
			{
				const auto runway = NormalizeRunway(entry.first);
				if (runway.empty() || (pattern.size() == 2 ? runway.substr(0, 2) != pattern : runway != pattern)) continue;
				if ((rule.operation != "departure" && entry.second.arrival) ||
					(rule.operation != "arrival" && entry.second.departure)) return true;
			}
			return false;
		};
		for (const auto& runway : rule.excluded) if (active(runway)) return false;
		bool matched = rule.all;
		for (const auto& runway : rule.runways)
			if (rule.all) matched = matched && active(runway); else matched = matched || active(runway);
		return matched;
	}

	struct RunwayVisibilityMemory
	{
		std::map<std::string, std::string> inputs;
	};

	inline std::string RunwayVisibilityInputs(const RunwayVisibilityRule& rule, const AirportRunwayActivity& activity)
	{
		// Compare actual inputs, not just the match result: changing 26L to 27R
		// must restore the automatic choice even though both mean West.
		std::string result = rule.airport + ":" + rule.operation + (rule.all ? ":all" : ":any") +
			(rule.matched ? ":1" : ":0") + (rule.unmatched ? ":1" : ":0");
		for (const auto& runway : rule.runways) result += ":include=" + runway;
		for (const auto& runway : rule.excluded) result += ":exclude=" + runway;
		const auto observed = activity.find(rule.airport);
		if (observed == activity.end()) return result + ":unavailable";
		result += ":available";
		for (const auto& runway : observed->second)
			if (runway.second.arrival || runway.second.departure)
				result += ":" + runway.first + (runway.second.arrival ? "/A" : "") + (runway.second.departure ? "/D" : "");
		return result;
	}

	template<typename Groups>
	bool ApplyRunwayVisibilityRules(const rapidjson::Value* rules, const AirportRunwayActivity& activity,
		Groups& groups, std::string& error, std::string* diagnostic = nullptr, RunwayVisibilityMemory* memory = nullptr)
	{
		error.clear();
		if (diagnostic) diagnostic->clear();
		if (!rules) { if (memory) memory->inputs.clear(); return false; }
		if (!rules->IsObject() || rules->MemberCount() > 256) { error = "Expected at most 256 group rules."; return false; }
		bool changed = false;
		std::map<std::string, std::string> nextInputs;
		for (auto& group : groups)
		{
			const auto* value = RuleMember(*rules, group.id.c_str());
			if (!value) continue;
			RunwayVisibilityRule rule;
			if (!ParseRunwayVisibilityRule(*value, rule)) { error = "Invalid runway visibility rule for group " + group.id; continue; }
			if (!rule.enabled) continue;
			bool apply = true;
			if (memory) {
				const auto signature = RunwayVisibilityInputs(rule, activity);
				const auto previous = memory->inputs.find(group.id);
				apply = previous == memory->inputs.end() || previous->second != signature;
				nextInputs.emplace(group.id, signature);
			}
			const bool automatic = RunwayRuleMatches(rule, activity) ? rule.matched : rule.unmatched;
			const bool visible = apply ? automatic : group.visible;
			if (diagnostic) {
				*diagnostic += " " + group.id + "=" + (visible ? "visible" : "hidden") + " source=" + rule.airport + " [";
				const auto observed = activity.find(rule.airport);
				if (observed == activity.end()) *diagnostic += "airport unavailable";
				else {
					bool any = false;
					for (const auto& runway : observed->second) {
						if (!runway.second.arrival && !runway.second.departure) continue;
						*diagnostic += runway.first + (runway.second.arrival ? ":ARR" : "") + (runway.second.departure ? ":DEP" : "") + " ";
						any = true;
					}
					if (!any) *diagnostic += "no active runways";
				}
				*diagnostic += "]";
				if (!apply && visible != automatic) *diagnostic += " manual override";
			}
			changed = changed || group.visible != visible;
			group.visible = visible;
		}
		if (memory) memory->inputs = std::move(nextInputs);
		return changed;
	}
}
