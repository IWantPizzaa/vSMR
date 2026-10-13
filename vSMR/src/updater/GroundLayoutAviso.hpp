#pragma once

#include <filesystem>
#include <string>

namespace vsmr::updater::ground_layout_aviso
{
	// The cache is separate from updater-owned AVISO files. A completed snapshot
	// is immutable, so another EuroScope process cannot change an active map.
	std::filesystem::path ActiveDirectory(const std::filesystem::path& dataRoot);
	bool Sync(const std::filesystem::path& dataRoot, std::wstring& message) noexcept;
}
