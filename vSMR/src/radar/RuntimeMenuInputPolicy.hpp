#pragma once

#include <cstdint>
#include <string_view>

namespace VsmrRuntimeMenuInput
{
	inline bool NeedsHostClickContext(std::string_view id) noexcept
	{
		constexpr std::string_view vsidPrefix = "runtime.vsid.";
		return id == "runtime.airport" ||
			id == "runtime.datalink.callsign" ||
			id == "runtime.datalink.credentials" ||
			id == "runtime.preset.rename" ||
			id == "runtime.button.control-center" ||
			id.substr(0, vsidPrefix.size()) == vsidPrefix;
	}

	inline bool OpensWindow(std::string_view id) noexcept
	{
		constexpr std::string_view vsidPrefix = "runtime.vsid.";
		return NeedsHostClickContext(id) && id.substr(0, vsidPrefix.size()) != vsidPrefix;
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
