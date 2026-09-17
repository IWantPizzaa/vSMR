#pragma once

#include "shared/JsonInputLimits.hpp"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

namespace VsmrJson
{
	// Every DOM entry point uses the same bounded, full-input validation. The SAX
	// pass stops before allocating a DOM and both passes use iterative UTF-8 parsing.
	inline rapidjson::Document& ParseDocument(
		rapidjson::Document& document,
		std::string_view json)
	{
		document.SetNull();
		VsmrJsonInputLimits::Limits limits;
		limits.maximumValues = 2000000U;
		limits.maximumContainerEntries = 1000000U;
		limits.maximumStringBytes = 64U * 1024U;
		std::string error;
		if (json.size() > 64U * 1024U * 1024U ||
			!VsmrJsonInputLimits::Validate(json, limits, error))
		{
			// Preserve RapidJSON's HasParseError contract for existing callers. Also
			// clear any previous DOM so a caller cannot accidentally reuse stale data.
			document.SetNull();
			return document.Parse<rapidjson::kParseIterativeFlag>("", 0U);
		}
		return document.Parse<rapidjson::kParseIterativeFlag |
			rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
	}
}
