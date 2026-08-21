#pragma once

#include "bootstrap/RuntimeApi.hpp"

#include <filesystem>
#include <string>

namespace VsmrRuntimeContext
{
	bool Configure(const VsmrRuntimeApi::BootstrapContext& context) noexcept;
	bool IsConfigured() noexcept;

	const std::filesystem::path& LoaderPath() noexcept;
	const std::filesystem::path& InstallRoot() noexcept;
	const std::filesystem::path& DataRoot() noexcept;
	const std::filesystem::path& CanonicalRuntimePath() noexcept;
	const std::filesystem::path& LoadedRuntimePath() noexcept;

	// The existing renderer/configuration code uses narrow paths. This returns
	// the installation root using the current Windows ANSI code page, matching
	// the behavior it had before the loader/runtime split.
	const std::string& InstallRootNarrow() noexcept;
}
