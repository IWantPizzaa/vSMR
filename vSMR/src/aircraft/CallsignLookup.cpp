#include "platform/windows/PrecompiledHeader.hpp"
#include "aircraft/CallsignLookup.hpp"

#include <fstream>
#include <sstream>
#include <vector>

// CCallsignLookup Class by Even Rognlien, used with permission

void CCallsignLookup::readFile(const std::filesystem::path& fileName)
{
	std::ifstream input(fileName);
	std::string line;
	while (std::getline(input, line))
	{
		std::istringstream lineStream(line);
		std::vector<std::string> tokens;
		std::string token;

		while (std::getline(lineStream, token, '\t'))
			tokens.push_back(token);

		if (tokens.size() >= 3)
		{
			callsigns[tokens.front()] = tokens[2];
		}
	}
}

std::string CCallsignLookup::getCallsign(const std::string& airlineCode) const
{
	const auto found = callsigns.find(airlineCode);
	if (found == callsigns.end())
		return "";

	return found->second;
}
