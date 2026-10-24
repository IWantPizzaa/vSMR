#pragma once

#include "rapidjson/document.h"

namespace VsmrUpdaterConfig
{
	// The bootstrap updater has always accepted schema-less settings. Older
	// installations therefore need a one-time migration before the Control
	// Center can edit the same file.
	inline bool NormalizeSchema(rapidjson::Document& config, bool& migrated)
	{
		migrated = false;
		if (!config.IsObject())
			return false;
		if (config.HasMember("schema_version"))
			return config["schema_version"].IsInt() &&
				config["schema_version"].GetInt() == 1;
		config.AddMember("schema_version", 1, config.GetAllocator());
		migrated = true;
		return true;
	}
}
