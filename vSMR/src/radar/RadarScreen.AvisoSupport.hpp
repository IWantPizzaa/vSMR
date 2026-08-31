#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace VsmrAvisoSupport
{
	inline double AvisoMin(double left, double right)
	{
		return left < right ? left : right;
	}

	inline double AvisoMax(double left, double right)
	{
		return left > right ? left : right;
	}

	inline std::string ToUpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}
}
