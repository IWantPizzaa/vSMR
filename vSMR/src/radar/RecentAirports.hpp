#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <utility>

namespace VsmrRadar
{
	inline void RememberAirport(std::vector<std::string>& history, std::string airport)
	{
		if (airport.size() != 4U) return;
		for (char& c : airport)
		{
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return;
		}
		history.erase(std::remove(history.begin(), history.end(), airport), history.end());
		// The startup fallback must not appear in the recent-airport menu.
		history.erase(std::remove(history.begin(), history.end(), "EGKK"), history.end());
		if (airport == "EGKK") return;
		history.insert(history.begin(), std::move(airport));
		if (history.size() > 5U) history.resize(5U);
	}
}
