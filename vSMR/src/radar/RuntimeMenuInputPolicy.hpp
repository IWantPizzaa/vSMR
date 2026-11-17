#pragma once

#include <cstdint>
#include <string_view>

namespace VsmrRuntimeMenuInput
{
	inline bool NeedsHostClickContext(std::string_view id) noexcept
	{
		constexpr std::string_view vsidPrefix = "runtime.vsid.";
		constexpr std::string_view cdmPrefix = "runtime.cdm.";
		return id == "runtime.airport" ||
			id == "runtime.datalink.credentials" ||
			id == "runtime.preset.rename" ||
			id == "runtime.button.control-center" ||
			id.substr(0, vsidPrefix.size()) == vsidPrefix ||
			id.substr(0, cdmPrefix.size()) == cdmPrefix;
	}

	inline bool OpensWindow(std::string_view id) noexcept
	{
		constexpr std::string_view vsidPrefix = "runtime.vsid.";
		constexpr std::string_view cdmPrefix = "runtime.cdm.";
		return NeedsHostClickContext(id) && id.substr(0, vsidPrefix.size()) != vsidPrefix &&
			id.substr(0, cdmPrefix.size()) != cdmPrefix;
	}

	inline bool IsRepeatedWindowOpen(
		std::uint64_t now,
		std::uint64_t lastOpened,
		std::uint32_t doubleClickInterval)
	{
		return lastOpened != 0 && now >= lastOpened &&
			now - lastOpened <= doubleClickInterval;
	}
}
