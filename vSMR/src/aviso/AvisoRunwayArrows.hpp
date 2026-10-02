#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace VsmrAviso
{
	struct RunwayArrowVisibility
	{
		bool east = false;
		bool west = false;
	};

	// Input contains only the live ARR/DEP runway ends, never their inactive
	// reciprocals or the GeoJSON's saved_active_runways metadata.
	inline RunwayArrowVisibility LfpgRunwayArrows(const std::vector<std::string>& activeRunways)
	{
		bool east = false, west = false;
		for (std::string_view runway : activeRunways)
		{
			const auto first = runway.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) continue;
			runway.remove_prefix(first);
			runway = runway.substr(0, runway.find_last_not_of(" \t\r\n") + 1);
			size_t digits = 0;
			int number = 0;
			while (digits < runway.size() && runway[digits] >= '0' && runway[digits] <= '9')
			{
				if (digits == 2) break;
				number = number * 10 + runway[digits++] - '0';
			}
			if (digits == 0) continue;
			const auto suffix = runway.substr(digits);
			if (!suffix.empty() && suffix != "L" && suffix != "R" && suffix != "C" &&
				suffix != "l" && suffix != "r" && suffix != "c") continue;
			east = east || number == 8 || number == 9;
			west = west || number == 26 || number == 27;
		}
		// No selection or conflicting flows: do not suggest a ground direction.
		return { east && !west, west && !east };
	}

	template<typename Groups>
	bool ApplyLfpgRunwayArrows(std::string_view airport, const std::vector<std::string>& activeRunways, Groups& groups)
	{
		if (airport != "LFPG") return false;
		const auto arrows = LfpgRunwayArrows(activeRunways);
		bool changed = false;
		for (auto& group : groups)
		{
			bool visible;
			if (group.id == "ground-layout-east") visible = arrows.east;
			else if (group.id == "ground-layout-west") visible = arrows.west;
			else continue;
			changed = changed || group.visible != visible;
			group.visible = visible;
		}
		return changed;
	}
}
