#pragma once

#include "integrations/CdmBridgeData.hpp"

#include <ctime>
#include <string>
#include <vector>

struct CdmPilotData
{
	std::string callsign;
	VsmrCdm::AircraftData bridgeData;
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
	std::time_t tsacUtc = 0;
	bool hasTobt = false;
	bool hasTsat = false;
	bool hasTtot = false;
	bool hasAsat = false;
	bool hasAobt = false;
	bool hasAtot = false;
	bool hasAsrt = false;
	bool hasAort = false;
	bool hasCtot = false;
	bool hasTsac = false;
	bool hasBooking = false;
};

struct StructuredTagColorRule
{
	// A bounded expression tree is compiled once when a profile is loaded.
	// The "legacy" field retains the exact semantics of pre-v2 criteria.
	struct RuleCondition
	{
		std::string kind = "leaf";
		std::string field;
		std::string op;
		std::string source;
		std::string token;
		std::string condition;
		std::string value;
		bool valueIsNumber = false;
		double numberValue = 0.0;
		std::vector<std::string> values;
		double minimum = 0.0;
		double maximum = 0.0;
		std::vector<RuleCondition> children;
	};

	struct RuleEffect
	{
		std::string type;
		std::string field;
		int r = 255;
		int g = 255;
		int b = 255;
		int a = 255;
		bool value = true;
	};

	struct Criterion
	{
		std::string source = "cdm";
		std::string token;
		std::string condition;
	};

	std::string source = "cdm";
	std::string token;
	std::string condition;
	std::vector<Criterion> criteria;
	std::string name;
	std::string tagType = "any";
	std::string status = "any";
	std::vector<std::string> statuses;
	std::string detail = "any";
	bool enabled = true;
	bool stopProcessing = false;
	bool usesStructuredCondition = false;
	RuleCondition when;
	std::vector<RuleEffect> effects;
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

bool TryGetCdmPilotData(const std::string& callsign, CdmPilotData& outData);
