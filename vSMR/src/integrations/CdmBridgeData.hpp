#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace VsmrCdm
{
	inline constexpr std::size_t MaximumStringFieldBytes = 512U;

	inline std::string NormalizeStringField(std::string_view value)
	{
		if (value.size() > MaximumStringFieldBytes ||
			std::find(value.begin(), value.end(), '\0') != value.end())
		{
			return {};
		}

		std::size_t first = 0U;
		std::size_t last = value.size();
		while (first < last && std::isspace(
			static_cast<unsigned char>(value[first])) != 0)
		{
			++first;
		}
		while (last > first && std::isspace(
			static_cast<unsigned char>(value[last - 1U])) != 0)
		{
			--last;
		}

		for (std::size_t index = first; index < last; ++index)
		{
			const unsigned char character = static_cast<unsigned char>(value[index]);
			if (character < 0x20U || character == 0x7fU)
				return {};
		}
		return std::string(value.substr(first, last - first));
	}

	struct AircraftData
	{
		std::optional<std::int64_t> tobt;
		std::optional<std::int64_t> tsat;
		std::optional<std::int64_t> ttot;
		std::optional<std::int64_t> ctot;
		std::optional<std::int64_t> tsac;
		std::optional<std::int64_t> asrt;
		std::optional<std::int64_t> asat;
		std::string deice;
		std::string tobtSetBy;
		std::string flowRestriction;
		std::string ecfmpRestriction;
		std::optional<bool> manualCtot;

		bool operator==(const AircraftData& other) const noexcept
		{
			return tobt == other.tobt && tsat == other.tsat &&
				ttot == other.ttot && ctot == other.ctot &&
				tsac == other.tsac && asrt == other.asrt &&
				asat == other.asat && deice == other.deice &&
				tobtSetBy == other.tobtSetBy &&
				flowRestriction == other.flowRestriction &&
				ecfmpRestriction == other.ecfmpRestriction &&
				manualCtot == other.manualCtot;
		}

		bool operator!=(const AircraftData& other) const noexcept
		{
			return !(*this == other);
		}
	};

	inline std::string FormatTimeToken(const std::optional<std::int64_t>& minutes)
	{
		if (!minutes.has_value() || *minutes < 0 || *minutes >= 24 * 60)
			return {};

		char text[5] = {};
		const int hours = static_cast<int>(*minutes / 60);
		const int minute = static_cast<int>(*minutes % 60);
		std::snprintf(text, sizeof(text), "%02d%02d", hours, minute);
		return text;
	}

	inline bool HasPublishedAircraftData(const AircraftData& data) noexcept
	{
		return data.tobt.has_value() || data.tsat.has_value() ||
			data.ttot.has_value() || data.ctot.has_value() ||
			data.tsac.has_value() || data.asrt.has_value() ||
			data.asat.has_value() || !data.deice.empty() ||
			!data.tobtSetBy.empty() || !data.flowRestriction.empty() ||
			!data.ecfmpRestriction.empty() || data.manualCtot.has_value();
	}

	inline bool IsReadyStartup(const AircraftData* data) noexcept
	{
		return data != nullptr && data->asrt.has_value();
	}

	inline void AddTagTokens(
		std::map<std::string, std::string>& tokens,
		const AircraftData* data)
	{
		const AircraftData empty;
		const AircraftData& value = data != nullptr ? *data : empty;
		tokens["cdm_ready_startup"] = "RDY";
		tokens["cdm_tobt"] = FormatTimeToken(value.tobt);
		tokens["cdm_tsat"] = FormatTimeToken(value.tsat);
		tokens["cdm_ttot"] = FormatTimeToken(value.ttot);
		tokens["cdm_ctot"] = FormatTimeToken(value.ctot);
		tokens["cdm_tsac"] = FormatTimeToken(value.tsac);
		tokens["cdm_asrt"] = FormatTimeToken(value.asrt);
		tokens["cdm_asat"] = FormatTimeToken(value.asat);
		tokens["cdm_deice"] = value.deice;
		tokens["cdm_tobt_set_by"] = value.tobtSetBy;
		tokens["cdm_flow_restriction"] = value.flowRestriction;
		tokens["cdm_ecfmp_restriction"] = value.ecfmpRestriction;
		tokens["cdm_manual_ctot"] = value.manualCtot.has_value()
			? (*value.manualCtot ? "true" : "false")
			: std::string();
	}
}
