#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace VsmrLanguage
{
	inline std::string Marker(std::string token)
	{
		std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		if (token == "/FR" || token == "RMK/FR") return "/FR";
		if (token == "/EN" || token == "RMK/EN") return "/EN";
		return {};
	}
	inline std::string Read(const std::string& remarks)
	{
		std::istringstream input(remarks);
		std::string token, result;
		while (input >> token) if (const auto marker = Marker(token); !marker.empty()) result = marker;
		return result;
	}
	inline std::string Write(const std::string& remarks, const std::string& language)
	{
		std::istringstream input(remarks);
		std::string token, result;
		while (input >> token)
		{
			if (!Marker(token).empty()) continue;
			if (!result.empty()) result += ' ';
			result += token;
		}
		if (const auto marker = Marker(language); !marker.empty())
		{
			if (!result.empty()) result += ' ';
			result += marker;
		}
		return result;
	}
}
