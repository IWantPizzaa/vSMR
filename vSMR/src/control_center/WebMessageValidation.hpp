#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace VsmrWebMessageValidation
{
	inline constexpr std::size_t MaximumInboundMessageBytes =
		32U * 1024U * 1024U;

	bool TryGetInboundWebMessageSelector(
		std::string_view json,
		std::string& selector);
	bool HasValidInboundWebMessageShape(std::string_view json);
}
