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
		return rule == "linked" || rule == "unlinked" || rule == "paris_auto";
	}

	enum class Flow { Unknown, East, West, Mixed };

	// Only known east/west runway families count. Crosswind/unknown runways
	// make a configuration ambiguous rather than guessing from its heading.
	inline Flow RunwayFlow(std::string_view airport, std::string_view runway)
	{
		if (airport == "LFPG")
		{
			if (runway == "08L" || runway == "08R" || runway == "09L" || runway == "09R") return Flow::East;
			if (runway == "26L" || runway == "26R" || runway == "27L" || runway == "27R") return Flow::West;
		}
		if (airport == "LFPO")
		{
			if (runway == "06" || runway == "07") return Flow::East;
			if (runway == "24" || runway == "25") return Flow::West;
		}
		return Flow::Mixed;
	}

	inline Flow MergeFlow(Flow current, Flow next)
	{
		if (current == Flow::Unknown) return next;
		if (next == Flow::Unknown || current == next) return current;
		return Flow::Mixed;
	}

	inline std::optional<bool> Linked(Flow pg, Flow po)
	{
		if ((pg != Flow::East && pg != Flow::West) ||
			(po != Flow::East && po != Flow::West)) return std::nullopt;
		return pg == po;
	}

	struct State
	{
		Flow pg = Flow::Unknown;
		std::optional<bool> linked;
		bool automatic = true;
		bool operator==(const State& other) const
		{
			return pg == other.pg && linked == other.linked && automatic == other.automatic;
		}
	};

	inline std::string RegionalRule(const State& state)
	{
		if (!state.linked.has_value() || (state.pg != Flow::West && state.pg != Flow::East)) return {};
		return std::string(state.pg == Flow::West ? "w" : "e") + (*state.linked ? "lpg" : "ipg");
	}

	template<class Rules>
	State Resolve(const Rules& rules, Flow pg, Flow po)
	{
		State state;
		state.pg = pg;
		const auto automatic = rules.find("paris_auto");
		state.automatic = automatic != rules.end() && automatic->second;
		if (state.automatic) state.linked = Linked(pg, po);
		else
		{
			const auto linked = rules.find("linked");
			const auto unlinked = rules.find("unlinked");
			if (linked != rules.end() && unlinked != rules.end() && linked->second != unlinked->second)
				state.linked = linked->second;
		}
		return state;
	}

	template<class Rules>
	bool Apply(Rules& rules, const State& state)
	{
		// An ambiguous automatic configuration preserves the controller's last
		// usable rules, while the published state explicitly reports Unknown.
		if (!state.linked.has_value()) return false;
		bool changed = false;
		auto set = [&](const char* name, bool value) {
			const auto found = rules.find(name);
			if (found != rules.end() && found->second != value)
			{
				found->second = value;
				changed = true;
			}
		};
		set("linked", *state.linked);
		set("unlinked", !*state.linked);
		set("opposing", !*state.linked);
		const auto regional = RegionalRule(state);
		if (!regional.empty())
		{
			for (const char* rule : { "wlpg", "elpg", "wipg", "eipg" }) set(rule, regional == rule);
			// Beauvais' existing SID alternatives depend on PG direction only.
			set("pgeast", state.pg == Flow::East);
		}
		return changed;
	}

	// Schema 1.2 global: nine-byte ICAO=WLA; records. W/E/? = PG flow,
	// L/U/? = linked state, A/M = automatic/manual. No optimistic UI state.
	inline std::string Serialize(std::string_view airport, const State& state)
	{
		return std::string(airport) + "=" +
			(state.pg == Flow::West ? "W" : state.pg == Flow::East ? "E" : "?") +
			(!state.linked.has_value() ? "?" : *state.linked ? "L" : "U") +
			(state.automatic ? "A;" : "M;");
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
			state.automatic = record[7] == 'A';
			if (!result.emplace(std::string(airport), state).second) return {};
		}
		return result;
	}
}
