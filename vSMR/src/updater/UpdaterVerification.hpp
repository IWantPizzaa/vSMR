#pragma once

#include "updater/UpdaterCore.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vsmr::updater::verification
{
	bool Sha256File(const std::filesystem::path& path, std::string& digest);
	std::string ResolveTrustedSignerHash(const StartupOptions& options);
	bool VerifyDetachedCms(
		const std::vector<std::uint8_t>& content,
		const std::vector<std::uint8_t>& signature,
		const std::string& expectedSignerSha256,
		std::string& error);
}
