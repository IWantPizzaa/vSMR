#pragma once

#include "rapidjson/document.h"
#include <filesystem>
#include <memory>
#include <string>

// Read-only access to canonical airport customizations while a controller uses
// an independent legacy profile source. This never creates/migrates user data.
namespace VsmrAvisoSharedConfig
{
	struct Snapshot
	{
		// Contains only effective 'aviso' and '_migration' roots. Null on error.
		std::shared_ptr<const rapidjson::Document> document;
		std::string revision;
		std::string error;
	};

	Snapshot Read(const std::filesystem::path& dataRoot, bool forceRefresh = false);
}
