#pragma once

#include <filesystem>
#include <map>
#include <string>

class CCallsignLookup
{
public:
	CCallsignLookup() = default;
	~CCallsignLookup() = default;

	void readFile(const std::filesystem::path& fileName);
	std::string getCallsign(const std::string& airlineCode) const;

private:
	std::map<std::string, std::string> callsigns;
};
