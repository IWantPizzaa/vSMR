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
		if (!airport || !airport->IsString() || airport->GetStringLength() != 4) return false;
		rule.airport.assign(airport->GetString(), airport->GetStringLength());
		for (char& c : rule.airport)
		{
			if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
			if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return false;
		}
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

	template<typename Groups>
	bool ApplyRunwayVisibilityRules(const rapidjson::Value* rules, const AirportRunwayActivity& activity,
		Groups& groups, std::string& error)
	{
		error.clear();
		if (!rules) return false;
		if (!rules->IsObject() || rules->MemberCount() > 256) { error = "Expected at most 256 group rules."; return false; }
		bool changed = false;
		for (auto& group : groups)
		{
			const auto* value = RuleMember(*rules, group.id.c_str());
			if (!value) continue;
			RunwayVisibilityRule rule;
			if (!ParseRunwayVisibilityRule(*value, rule)) { error = "Invalid runway visibility rule for group " + group.id; continue; }
			if (!rule.enabled) continue;
			const bool visible = RunwayRuleMatches(rule, activity) ? rule.matched : rule.unmatched;
			changed = changed || group.visible != visible;
			group.visible = visible;
		}
		return changed;
	}
}
