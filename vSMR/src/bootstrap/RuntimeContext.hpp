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

	// Legacy string fields carry UTF-8 only at their filesystem boundary
	const std::string& InstallRootUtf8() noexcept;
}
