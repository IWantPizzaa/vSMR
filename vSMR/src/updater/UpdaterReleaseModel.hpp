#pragma once

#include "updater/UpdaterCore.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vsmr::updater::release_model
{
	struct SemVerIdentifier
	{
		std::string text;
		bool numeric = false;
		std::uint64_t number = 0;
	};

	struct SemVer
	{
		std::uint64_t major = 0;
		std::uint64_t minor = 0;
		std::uint64_t patch = 0;
		std::vector<SemVerIdentifier> prerelease;
		std::string normalized;
		bool valid = false;
	};

	SemVer ParseSemVer(std::string value);
	int CompareSemVer(const SemVer& left, const SemVer& right) noexcept;
	bool SameSemVerIdentity(const std::string& left, const std::string& right);
	bool ChannelAccepts(const SemVer& version, UpdateChannel channel) noexcept;
}
