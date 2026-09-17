#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace VsmrParis
{
	inline constexpr std::array<std::string_view, 6> Airports = {
		"LFPG", "LFPO", "LFPN", "LFPV", "LFPT", "LFOB"
	};

	inline bool Supports(std::string_view airport)
	{
		for (const auto candidate : Airports)
			if (candidate == airport) return true;
		return false;
	}

	inline bool IsControlRule(std::string_view rule)
	{
		// vSID stores configuration keys in uppercase in a case-insensitive map.
		for (const auto candidate : { "linked", "unlinked", "paris_auto", "paris_manual_config" })
		{
			const std::string_view expected(candidate);
			if (rule.size() != expected.size()) continue;
			bool matches = true;
			for (std::size_t i = 0; i < rule.size(); ++i)
			{
				const char c = rule[i] >= 'A' && rule[i] <= 'Z' ? static_cast<char>(rule[i] - 'A' + 'a') : rule[i];
				if (c != expected[i]) { matches = false; break; }
			}
			if (matches) return true;
		}
		return false;
	}

	inline bool IsRegional(std::string_view airport)
	{
		return airport == "LFPN" || airport == "LFPV" || airport == "LFPT" || airport == "LFOB";
	}

	inline constexpr std::array<std::string_view, 4> RegionalRules = { "wlpg", "elpg", "wipg", "eipg" };

	inline bool IsRegionalRule(std::string_view rule)
	{
		for (const auto candidate : RegionalRules)
			if (candidate == rule) return true;
		return false;
	}

	enum class Flow { Unknown, East, West };

	struct State
	{
		Flow pg = Flow::Unknown;
		std::optional<bool> linked;
		bool operator==(const State& other) const
		{
			return pg == other.pg && linked == other.linked;
		}
	};

	inline std::string RegionalRule(const State& state)
	{
		if (!state.linked.has_value() || (state.pg != Flow::West && state.pg != Flow::East)) return {};
		return std::string(state.pg == Flow::West ? "w" : "e") + (*state.linked ? "lpg" : "ipg");
	}

	template<class Rules>
	State Resolve(const Rules& rules, std::string_view airport)
	{
		State state;
		if (IsRegional(airport))
		{
			std::string_view selected;
			for (const auto rule : RegionalRules)
			{
				const auto found = rules.find(std::string(rule));
				if (found == rules.end() || !found->second) continue;
				if (!selected.empty()) return state;
				selected = rule;
			}
			if (!selected.empty())
			{
				state.pg = selected.front() == 'w' ? Flow::West : Flow::East;
				state.linked = selected[1] == 'l';
			}
		}
		else if (Supports(airport))
		{
			const auto opposing = rules.find("opposing");
			if (opposing != rules.end()) state.linked = !opposing->second;
			else
			{
				const auto linked = rules.find("linked");
				const auto unlinked = rules.find("unlinked");
				if (linked != rules.end() && unlinked != rules.end() && linked->second != unlinked->second)
					state.linked = linked->second;
			}
		}
		return state;
	}

	// Only an explicit controller selection changes vSID rules. There are no
	// runway inputs or timer-driven rule updates.
	template<class Rules>
	bool Select(Rules& rules, std::string_view airport, std::string_view selection)
	{
		const bool regional = IsRegionalRule(selection);
		if (!Supports(airport) || regional != IsRegional(airport) ||
			(!regional && selection != "linked" && selection != "unlinked")) return false;
		const bool linked = regional ? selection[1] == 'l' : selection == "linked";
		rules["linked"] = linked;
		rules["unlinked"] = !linked;
		if (regional)
		{
			for (const auto rule : RegionalRules) rules[std::string(rule)] = rule == selection;
			const auto east = rules.find("pgeast");
			if (east != rules.end()) east->second = selection.front() == 'e';
		}
		else rules["opposing"] = !linked;
		return true;
	}

	// Schema 1.2 global: nine-byte ICAO=WLA; records. W/E/? = PG flow,
	// L/U/? = linked state. Publish M (manual); accept legacy A snapshots for compatibility.
	inline std::string Serialize(std::string_view airport, const State& state)
	{
		return std::string(airport) + "=" +
			(state.pg == Flow::West ? "W" : state.pg == Flow::East ? "E" : "?") +
			(!state.linked.has_value() ? "?" : *state.linked ? "L" : "U") +
			"M;";
	}

	inline std::map<std::string, State> Parse(std::string_view value)
	{
		std::map<std::string, State> result;
		if (value.size() > Airports.size() * 9U || value.size() % 9U != 0U) return {};
		for (std::size_t i = 0; i < value.size(); i += 9U)
		{
			const auto record = value.substr(i, 9U);
			const auto airport = record.substr(0, 4);
			if (!Supports(airport) || record[4] != '=' || record[8] != ';' ||
				(record[5] != 'W' && record[5] != 'E' && record[5] != '?') ||
				(record[6] != 'L' && record[6] != 'U' && record[6] != '?') ||
				(record[7] != 'A' && record[7] != 'M')) return {};
			State state;
			state.pg = record[5] == 'W' ? Flow::West : record[5] == 'E' ? Flow::East : Flow::Unknown;
			if (record[6] != '?') state.linked = record[6] == 'L';
			if (!result.emplace(std::string(airport), state).second) return {};
		}
		return result;
	}
}
