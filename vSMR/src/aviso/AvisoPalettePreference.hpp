#pragma once

#include <string>

namespace VsmrAvisoPalettePreference
{
	inline std::string InitialPalette(const char* savedPalette, const std::string& airportDefault)
	{
		if (savedPalette != nullptr && savedPalette[0] != '\0')
			return savedPalette;
		return airportDefault.empty() ? "light" : airportDefault;
	}

	inline std::string ForAirport(const std::string& sessionChoice,
		const char* savedAirportPalette, const std::string& airportDefault)
	{
		return !sessionChoice.empty() ? sessionChoice : InitialPalette(savedAirportPalette, airportDefault);
	}
}
