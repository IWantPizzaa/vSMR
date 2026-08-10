#pragma once

#include <cctype>
#include <string>

enum class GroundStateCategory { Unknown, Gate, Push, Stup, Taxi, Lnup, Nsts, Depa, Arr };

inline static GroundStateCategory classifyGroundState(const std::string& rawState, int reportedGs, bool onRunway)
{
	std::string normalized;
	normalized.reserve(rawState.size());
	for (char c : rawState) {
		if (c == ' ' || c == '_' || c == '-')
			continue;
		normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}

	if (normalized.find("NSTS") != std::string::npos)
		return GroundStateCategory::Nsts;

	if (normalized.find("DEPA") != std::string::npos)
		return GroundStateCategory::Depa;

	if (normalized.find("ARR") != std::string::npos)
		return GroundStateCategory::Arr;

	if (normalized.find("STUP") != std::string::npos || normalized.find("STARTUP") != std::string::npos || normalized == "S/U" || normalized == "SU")
		return GroundStateCategory::Stup;

	if (normalized.find("PUSH") != std::string::npos || normalized.find("P/B") != std::string::npos || normalized == "PB" || normalized == "P/B")
		return GroundStateCategory::Push;

	if (normalized.find("TAX") != std::string::npos || normalized == "TXI" || normalized == "TXIN" || normalized == "TAXIIN")
		return GroundStateCategory::Taxi;

	if (normalized == "LNUP" || normalized == "LINEUP" || normalized == "L/UP")
		return GroundStateCategory::Lnup;

	if (normalized.find("GATE") != std::string::npos || normalized.find("STAND") != std::string::npos || normalized.find("PARK") != std::string::npos || normalized.find("STBY") != std::string::npos)
		return GroundStateCategory::Gate;

	if (normalized.empty() && reportedGs < 2 && !onRunway)
		return GroundStateCategory::Gate;

	return GroundStateCategory::Unknown;
}

inline static GroundStateCategory classifyGroundState(const char* rawState, int reportedGs, bool onRunway)
{
	return classifyGroundState(rawState != nullptr ? std::string(rawState) : std::string(), reportedGs, onRunway);
}

namespace VsmrGroundState
{
	bool SetLineupOverride(const char* callsign);
	void ClearLineupOverride(const char* callsign);
	void ClearAllLineupOverrides();
	bool IsLineupOverrideActive(const char* callsign, GroundStateCategory observedCategory);
}

inline static GroundStateCategory classifyGroundStateForCallsign(
	const char* callsign,
	const char* rawState,
	int reportedGs,
	bool onRunway)
{
	const GroundStateCategory observedCategory = classifyGroundState(rawState, reportedGs, onRunway);
	return VsmrGroundState::IsLineupOverrideActive(callsign, observedCategory)
		? GroundStateCategory::Lnup
		: observedCategory;
}

inline static bool shouldDisplayTagInTowerMode(const char* rawState, int reportedGs, bool onRunway)
{
	if (rawState == nullptr)
		return false;

	bool hasStatusText = false;
	for (const char* current = rawState; *current != '\0'; ++current)
	{
		if (std::isspace(static_cast<unsigned char>(*current)) == 0)
		{
			hasStatusText = true;
			break;
		}
	}
	if (!hasStatusText)
		return false;

	switch (classifyGroundState(rawState, reportedGs, onRunway))
	{
	case GroundStateCategory::Nsts:
	case GroundStateCategory::Push:
	case GroundStateCategory::Stup:
		return false;
	default:
		return true;
	}
}
