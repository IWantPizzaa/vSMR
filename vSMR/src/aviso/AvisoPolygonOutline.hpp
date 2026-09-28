#pragma once

#include "rapidjson/document.h"

namespace VsmrAviso
{
	// Opt-in only: existing filled polygons must retain their original appearance.
	// The caller supplies palette objects using the same inheritance as colours.
	inline bool ResolvePolygonOutline(
		const rapidjson::Value* sharedPaint,
		const rapidjson::Value* featurePaint,
		const rapidjson::Value* sharedPalette,
		const rapidjson::Value* featurePalette)
	{
		const rapidjson::Value* sources[] = { featurePalette, featurePaint, sharedPalette, sharedPaint };
		for (const auto* source : sources)
		{
			if (source && source->IsObject() && source->HasMember("polygon-outline") &&
				(*source)["polygon-outline"].IsBool())
				return (*source)["polygon-outline"].GetBool();
		}
		return false;
	}
}
