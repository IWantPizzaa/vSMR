#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace VsmrAviso
{
	struct FrequencyOwnershipMetadata
	{
		bool dynamicItem = false;
		std::string service;
		std::string ownerKey;
		std::vector<std::string> takeoverChain;
		std::string ruleKey;
	};

	struct FrequencyOwner
	{
		bool connected = false;
		bool ownedByMe = false;
		bool useSourceFrequency = false;
		std::string positionId;
		std::wstring frequencyLabel;
	};

	struct FrequencyOwnershipSnapshot
	{
		std::unordered_map<std::string, FrequencyOwner> ownersByRule;
	};
}
