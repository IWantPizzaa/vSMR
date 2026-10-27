#pragma once

#include <cstdint>

namespace VsmrRuntimeMenuInput
{
	inline bool IsRepeatedAirportEdit(
		std::uint64_t now,
		std::uint64_t lastOpened,
		std::uint32_t doubleClickInterval)
	{
		return lastOpened != 0 && now >= lastOpened &&
			now - lastOpened <= doubleClickInterval;
	}
}
