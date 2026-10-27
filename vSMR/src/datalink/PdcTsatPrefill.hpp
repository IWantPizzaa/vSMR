#pragma once

#include "rapidjson/document.h"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace VsmrPdcTsatPrefill
{
	inline bool Allowed(const rapidjson::Value* pdc, std::string_view departureAirport)
	{
		if (pdc == nullptr || !pdc->IsObject() || departureAirport.size() != 4)
			return false;
		const auto airports = pdc->FindMember("tsat_prefill_airports");
		if (airports == pdc->MemberEnd() || !airports->value.IsArray())
			return false;
		for (const auto& candidate : airports->value.GetArray())
		{
			if (!candidate.IsString() || candidate.GetStringLength() != 4)
				continue;
			bool matches = true;
			for (std::size_t index = 0; index < 4; ++index)
			{
				const unsigned char actual = static_cast<unsigned char>(departureAirport[index]);
				if (std::toupper(actual) != candidate.GetString()[index])
				{
					matches = false;
					break;
				}
			}
			if (matches)
				return true;
		}
		return false;
	}
}
