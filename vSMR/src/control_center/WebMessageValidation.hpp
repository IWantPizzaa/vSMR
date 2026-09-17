#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace VsmrWebMessageValidation
{
	inline constexpr std::size_t MaximumInboundMessageBytes =
		32U * 1024U * 1024U;

	[[nodiscard]] bool TryGetInboundWebMessageSelector(
		std::string_view json,
		std::string& selector);
	[[nodiscard]] bool HasValidInboundWebMessageShape(std::string_view json);
}
