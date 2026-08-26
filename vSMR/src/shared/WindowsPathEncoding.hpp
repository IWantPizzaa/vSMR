#pragma once

#include <Windows.h>

#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace VsmrWindowsPath
{
	inline bool TryDecode(
		const std::string& value,
		UINT codePage,
		DWORD flags,
		std::filesystem::path& result)
	{
		if (value.empty() || value.find('\0') != std::string::npos ||
			value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		{
			return false;
		}

		const int required = ::MultiByteToWideChar(
			codePage,
			flags,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0);
		if (required <= 0)
			return false;

		std::wstring wide(static_cast<std::size_t>(required), L'\0');
		if (::MultiByteToWideChar(
			codePage,
			flags,
			value.data(),
			static_cast<int>(value.size()),
			wide.data(),
			required) != required)
		{
			return false;
		}

		result = std::filesystem::path(wide);
		return true;
	}

	inline bool TryResolveExistingFile(
		const std::string& storedPath,
		std::filesystem::path& result) noexcept
	{
		result.clear();
		try
		{
			// Older ASR files can still contain Windows code-page bytes
			for (const auto encoding : {
				std::pair<UINT, DWORD>{ CP_UTF8, MB_ERR_INVALID_CHARS },
				std::pair<UINT, DWORD>{ CP_ACP, 0U } })
			{
				std::filesystem::path candidate;
				if (!TryDecode(storedPath, encoding.first, encoding.second, candidate))
					continue;

				std::error_code error;
				candidate = std::filesystem::absolute(candidate, error).lexically_normal();
				if (!error && !candidate.empty() &&
					std::filesystem::is_regular_file(candidate, error) && !error)
				{
					result = std::move(candidate);
					return true;
				}
			}
		}
		catch (...)
		{
		}
		return false;
	}
}
