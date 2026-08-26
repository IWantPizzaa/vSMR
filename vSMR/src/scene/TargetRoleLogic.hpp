#pragma once

namespace VsmrTargetRoleLogic
{
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
