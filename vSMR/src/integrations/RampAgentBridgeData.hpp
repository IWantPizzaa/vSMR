#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace VsmrRampAgent
{
	// Ramp Agent's provider declaration (EuroscopeRampAgent RampAgent.h, schema 1.0):
	// "rampagent/stand" and "rampagent/remark" are aircraft-scope STR fields. Ramp
	// Agent sets both while it holds a stand for an aircraft and clears both otherwise.
	inline constexpr std::uint32_t StandMaximumBytes = 32U;
	inline constexpr std::uint32_t RemarkMaximumBytes = 128U;

	struct AircraftData
	{
		std::string stand;
		std::string remark;

		bool operator==(const AircraftData& other) const noexcept
		{
			return stand == other.stand && remark == other.remark;
		}

		bool operator!=(const AircraftData& other) const noexcept
		{
			return !(*this == other);
		}
	};

	// Bridge STR values are UTF-8 with an explicit length. Ramp Agent clamps long
	// values by bytes before publishing, which can split a multi-byte character at
	// the cap; that incomplete tail is dropped rather than discarding the value.
	inline std::string NormalizeText(std::string_view value, std::size_t maximumBytes)
	{
		if (std::find(value.begin(), value.end(), '\0') != value.end())
			return {};
		value = value.substr(0U, (std::min)(value.size(), maximumBytes));

		std::string text;
		text.reserve(value.size());
		for (std::size_t index = 0U; index < value.size();)
		{
			const unsigned char lead = static_cast<unsigned char>(value[index]);
			if (lead < 0x80U)
			{
				text.push_back(lead < 0x20U || lead == 0x7FU ? ' ' : static_cast<char>(lead));
				++index;
				continue;
			}

			std::size_t length = 0U;
			unsigned char secondMinimum = 0x80U;
			unsigned char secondMaximum = 0xBFU;
			if (lead >= 0xC2U && lead <= 0xDFU)
				length = 2U;
			else if (lead >= 0xE0U && lead <= 0xEFU)
			{
				length = 3U;
				if (lead == 0xE0U)
					secondMinimum = 0xA0U;   // overlong encodings
				else if (lead == 0xEDU)
					secondMaximum = 0x9FU;   // UTF-16 surrogates
			}
			else if (lead >= 0xF0U && lead <= 0xF4U)
			{
				length = 4U;
				if (lead == 0xF0U)
					secondMinimum = 0x90U;   // overlong encodings
				else if (lead == 0xF4U)
					secondMaximum = 0x8FU;   // beyond U+10FFFF
			}
			if (length == 0U)
			{
				text.push_back('?');
				++index;
				continue;
			}
			if (index + length > value.size())
				break;

			bool valid = true;
			for (std::size_t offset = 1U; offset < length; ++offset)
			{
				const unsigned char continuation = static_cast<unsigned char>(value[index + offset]);
				const unsigned char minimum = offset == 1U ? secondMinimum : 0x80U;
				const unsigned char maximum = offset == 1U ? secondMaximum : 0xBFU;
				valid = valid && continuation >= minimum && continuation <= maximum;
			}
			if (!valid)
			{
				text.push_back('?');
				++index;
				continue;
			}
			text.append(value.substr(index, length));
			index += length;
		}

		std::size_t first = 0U;
		std::size_t last = text.size();
		while (first < last && text[first] == ' ')
			++first;
		while (last > first && text[last - 1U] == ' ')
			--last;
		return text.substr(first, last - first);
	}

	// A remark describes a stand assignment, so it is only meaningful with a stand.
	inline bool HasPublishedAircraftData(const AircraftData& data) noexcept
	{
		return !data.stand.empty();
	}
}
