#pragma once

#include "scene/TargetRoleLogic.hpp"

#include <cctype>
#include <string>
#include <vector>

enum class GroundStateCategory { Unknown, Gate, Push, Stup, Taxi, Lnup, Nsts, Depa, Arr };

// ----- Ground states shared between clients -----
// EuroScope synchronizes the controller assigned speed. When no real speed is
// assigned, vSMR uses a reserved band for states EuroScope does not provide.
// Every vSMR client reads the same value and shows
// the same state. Values outside the band are real speed assignments and are left
// untouched.
namespace VsmrGroundStateSync
{
	inline constexpr int ReservedFirst = 10;
	inline constexpr int ReservedLast = 19;
	inline constexpr int LineupAssignedSpeed = 10;
	// The EuroScope ground status vSMR writes together with each shared state.
	// A controller who picks a different status from outside vSMR takes the
	// aircraft off its shared state, so the two are always written as a pair.
	inline constexpr const char* LineupEuroScopeStatus = "TAXI";

	constexpr bool IsReservedAssignedSpeed(int assignedSpeed) noexcept
	{
		return assignedSpeed >= ReservedFirst && assignedSpeed <= ReservedLast;
	}

	constexpr bool CanWriteSharedState(int assignedSpeed) noexcept
	{
		return assignedSpeed == 0 || IsReservedAssignedSpeed(assignedSpeed);
	}

	// Unknown for every value a newer vSMR may reserve, so an older client falls
	// back to the EuroScope ground status instead of showing the wrong state.
	constexpr GroundStateCategory CategoryForAssignedSpeed(int assignedSpeed) noexcept
	{
		switch (assignedSpeed)
		{
		case LineupAssignedSpeed:
			return GroundStateCategory::Lnup;
		default:
			return GroundStateCategory::Unknown;
		}
	}

	// The EuroScope status a shared state expects to be paired with. Anything else
	// means the status was changed from outside vSMR and the shared value is stale.
	constexpr GroundStateCategory CompanionCategoryForAssignedSpeed(int assignedSpeed) noexcept
	{
		switch (assignedSpeed)
		{
		case LineupAssignedSpeed:
			return GroundStateCategory::Taxi;
		default:
			return GroundStateCategory::Unknown;
		}
	}

	// 0 for the states EuroScope already synchronizes through the ground status.
	constexpr int AssignedSpeedForCategory(GroundStateCategory category) noexcept
	{
		switch (category)
		{
		case GroundStateCategory::Lnup:
			return LineupAssignedSpeed;
		default:
			return 0;
		}
	}
}

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

	// Empty, stationary off-runway targets are treated as parked departures
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
	// Callsigns retained for the cleanup in CSMRPlugin::OnTimer. Any client that
	// sees a reserved assigned speed records the callsign, so a state another
	// controller shared is cleaned up as well.
	void ObserveSharedState(const char* callsign);
	void ForgetAircraft(const char* callsign);
	void ForgetAllAircraft();
	std::vector<std::string> SharedStateCallsigns();
	// True once the EuroScope status has disagreed with the shared state for
	// longer than the synchronization grace. The two are written one after the
	// other, so a client must not act on the moment between the two updates.
	bool HasSettledStatusMismatch(const char* callsign, bool mismatched);
}

// A shared ground state wins over the EuroScope status vSMR writes with it: both
// are set at the same time, and the shared value is the one a controller picked
// in vSMR. It is dropped when the EuroScope status no longer matches the one it
// was written with, because a controller changed it from outside vSMR, and as
// soon as the tag turns airborne, because the aircraft has left the ground and
// the client that wrote the value may no longer be able to clear it.
inline static GroundStateCategory sharedGroundStateCategory(
	const char* rawState,
	int reportedGs,
	bool onRunway,
	int assignedSpeed)
{
	if (VsmrTargetRoleLogic::IsAirborneForTagRole(false, reportedGs))
		return GroundStateCategory::Unknown;

	const GroundStateCategory sharedCategory = VsmrGroundStateSync::CategoryForAssignedSpeed(assignedSpeed);
	if (sharedCategory == GroundStateCategory::Unknown)
		return GroundStateCategory::Unknown;

	return classifyGroundState(rawState, reportedGs, onRunway) ==
		VsmrGroundStateSync::CompanionCategoryForAssignedSpeed(assignedSpeed)
		? sharedCategory
		: GroundStateCategory::Unknown;
}

inline static GroundStateCategory classifyGroundStateWithSharedState(
	const char* rawState,
	int reportedGs,
	bool onRunway,
	int assignedSpeed)
{
	const GroundStateCategory sharedCategory = sharedGroundStateCategory(rawState, reportedGs, onRunway, assignedSpeed);
	return sharedCategory != GroundStateCategory::Unknown
		? sharedCategory
		: classifyGroundState(rawState, reportedGs, onRunway);
}

inline static bool shouldDisplayTagInTowerMode(const char* rawState, int reportedGs, bool onRunway, int assignedSpeed)
{
	if (sharedGroundStateCategory(rawState, reportedGs, onRunway, assignedSpeed) == GroundStateCategory::Unknown)
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
	}

	// Tower mode starts at taxi; startup and push remain hidden
	switch (classifyGroundStateWithSharedState(rawState, reportedGs, onRunway, assignedSpeed))
	{
	case GroundStateCategory::Nsts:
	case GroundStateCategory::Push:
	case GroundStateCategory::Stup:
		return false;
	default:
		return true;
	}
}
