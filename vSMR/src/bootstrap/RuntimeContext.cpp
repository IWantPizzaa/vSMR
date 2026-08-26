#include "platform/windows/PrecompiledHeader.hpp"
#include "bootstrap/RuntimeContext.hpp"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace
{
	struct StoredContext
	{
		std::filesystem::path loaderPath;
		std::filesystem::path installRoot;
		std::filesystem::path dataRoot;
		std::filesystem::path canonicalRuntimePath;
		std::filesystem::path loadedRuntimePath;
		std::string installRootNarrow;
	};

	StoredContext gContext;
	std::mutex gContextMutex;
	std::atomic<bool> gConfigured{ false };

	std::string ToAnsi(const std::wstring& value)
	{
		if (value.empty())
			return {};
		BOOL usedDefaultCharacter = FALSE;
		const int required = ::WideCharToMultiByte(
			CP_ACP,
			WC_NO_BEST_FIT_CHARS,
			value.c_str(),
			static_cast<int>(value.size()),
			nullptr,
			0,
			nullptr,
			&usedDefaultCharacter);
		if (required <= 0 || usedDefaultCharacter)
			return {};
		std::string converted(static_cast<std::size_t>(required), '\0');
		usedDefaultCharacter = FALSE;
		if (::WideCharToMultiByte(
			CP_ACP,
			WC_NO_BEST_FIT_CHARS,
			value.c_str(),
			static_cast<int>(value.size()),
			converted.data(),
			required,
			nullptr,
			&usedDefaultCharacter) <= 0 || usedDefaultCharacter)
		{
			return {};
		}
		return converted;
	}

	bool IsAbsoluteDirectory(const wchar_t* value)
	{
		if (value == nullptr || *value == L'\0')
			return false;
		const std::filesystem::path path(value);
		return path.is_absolute() && path.has_root_name();
	}

	bool IsAbsoluteFile(const wchar_t* value)
	{
		if (value == nullptr || *value == L'\0')
			return false;
		const std::filesystem::path path(value);
		return path.is_absolute() && path.has_root_name() && path.has_filename();
	}

	bool IsContainedBy(
		const std::filesystem::path& candidate,
		const std::filesystem::path& parent)
	{
		const std::filesystem::path relative =
			candidate.lexically_normal().lexically_relative(parent.lexically_normal());
		if (relative.empty() || relative.is_absolute())
			return false;
		const auto first = relative.begin();
		return first != relative.end() && *first != L"..";
	}

	const std::filesystem::path& EmptyPath() noexcept
	{
		static const std::filesystem::path empty;
		return empty;
	}

	const std::string& EmptyString() noexcept
	{
		static const std::string empty;
		return empty;
	}
}

namespace VsmrRuntimeContext
{
	bool Configure(const VsmrRuntimeApi::BootstrapContext& context) noexcept
	{
		try
		{
			// Validating loader-owned paths before exposing them to the runtime
			if (context.structureSize < sizeof(VsmrRuntimeApi::BootstrapContext) ||
				context.abiVersion != VsmrRuntimeApi::AbiVersion ||
				!IsAbsoluteFile(context.loaderPath) ||
				!IsAbsoluteFile(context.canonicalRuntimePath) ||
				!IsAbsoluteFile(context.loadedRuntimePath) ||
				!IsAbsoluteDirectory(context.installRoot) ||
				!IsAbsoluteDirectory(context.dataRoot))
			{
				return false;
			}

			StoredContext configured;
			configured.loaderPath = std::filesystem::path(context.loaderPath).lexically_normal();
			configured.installRoot = std::filesystem::path(context.installRoot).lexically_normal();
			configured.dataRoot = std::filesystem::path(context.dataRoot).lexically_normal();
			configured.canonicalRuntimePath =
				std::filesystem::path(context.canonicalRuntimePath).lexically_normal();
			configured.loadedRuntimePath =
				std::filesystem::path(context.loadedRuntimePath).lexically_normal();
			if (configured.loaderPath.parent_path() != configured.installRoot ||
				!IsContainedBy(configured.dataRoot, configured.installRoot) ||
				!IsContainedBy(configured.canonicalRuntimePath, configured.dataRoot))
				return false;
			configured.installRootNarrow = ToAnsi(configured.installRoot.wstring());
			if (configured.installRootNarrow.empty())
				return false;

			// The first loader context remains authoritative for this DLL generation
			std::lock_guard<std::mutex> guard(gContextMutex);
			if (gConfigured.load(std::memory_order_acquire))
			{
				return gContext.installRoot == configured.installRoot &&
					gContext.dataRoot == configured.dataRoot &&
					gContext.loadedRuntimePath == configured.loadedRuntimePath;
			}
			gContext = std::move(configured);
			gConfigured.store(true, std::memory_order_release);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool IsConfigured() noexcept
	{
		return gConfigured.load(std::memory_order_acquire);
	}

	const std::filesystem::path& LoaderPath() noexcept
	{
		return IsConfigured() ? gContext.loaderPath : EmptyPath();
	}

	const std::filesystem::path& InstallRoot() noexcept
	{
		return IsConfigured() ? gContext.installRoot : EmptyPath();
	}

	const std::filesystem::path& DataRoot() noexcept
	{
		return IsConfigured() ? gContext.dataRoot : EmptyPath();
	}

	const std::filesystem::path& CanonicalRuntimePath() noexcept
	{
		return IsConfigured() ? gContext.canonicalRuntimePath : EmptyPath();
	}

	const std::filesystem::path& LoadedRuntimePath() noexcept
	{
		return IsConfigured() ? gContext.loadedRuntimePath : EmptyPath();
	}

	const std::string& InstallRootNarrow() noexcept
	{
		return IsConfigured() ? gContext.installRootNarrow : EmptyString();
	}
}
