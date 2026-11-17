#pragma once

namespace VsmrTargetRoleLogic
{
	constexpr bool HasUnknownAirportGroundRole(bool airborne, bool correlated, bool receivedPlan,
		bool departure, bool arrival) noexcept
	{
		return !airborne && correlated && receivedPlan && !departure && !arrival;
	}
	inline constexpr int ArrivalAirborneThresholdKt = 40;
	inline constexpr int DefaultAirborneThresholdKt = 50;

	constexpr bool IsAirborneForTagRole(
		bool arrival,
		int reportedGroundSpeedKt) noexcept
	{
		return reportedGroundSpeedKt >
			(arrival ? ArrivalAirborneThresholdKt : DefaultAirborneThresholdKt);
	}
}
