#pragma once

#include <ctime>
#include <string>
#include <vector>

struct VacdmPilotData
{
	std::string callsign;
	std::string tobtState;
	std::time_t tobtUtc = 0;
	std::time_t tsatUtc = 0;
	std::time_t ttotUtc = 0;
	std::time_t asatUtc = 0;
	std::time_t aobtUtc = 0;
	std::time_t atotUtc = 0;
	std::time_t asrtUtc = 0;
	std::time_t aortUtc = 0;
	std::time_t ctotUtc = 0;
	bool hasTobt = false;
	bool hasTsat = false;
	bool hasTtot = false;
	bool hasAsat = false;
	bool hasAobt = false;
	bool hasAtot = false;
	bool hasAsrt = false;
	bool hasAort = false;
	bool hasCtot = false;
	bool hasBooking = false;
};

struct StructuredTagColorRule
{
	struct Criterion
	{
		std::string source = "vacdm";
		std::string token;
		std::string condition;
	};

	std::string source = "vacdm";
	std::string token;
	std::string condition;
	std::vector<Criterion> criteria;
	std::string name;
	std::string tagType = "any";
	std::string status = "any";
	std::vector<std::string> statuses;
	std::string detail = "any";
	bool applyTarget = false;
	int targetR = 255;
	int targetG = 255;
	int targetB = 255;
	int targetA = 255;
	bool applyTag = false;
	int tagR = 255;
	int tagG = 255;
	int tagB = 255;
	int tagA = 255;
	bool applyText = false;
	int textR = 255;
	int textG = 255;
	int textB = 255;
	int textA = 255;
};

bool TryGetVacdmPilotData(const std::string& callsign, VacdmPilotData& outData);
