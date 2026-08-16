#pragma once

// MFC-free normal-runtime support shared by vSMR and the isolated crash
// harness. None of these functions run in the faulting process's exception
// path; the WER helper only consumes the directory selected here.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace VsmrCrashSupport
{
	constexpr std::uintmax_t kDefaultRetentionBytes = 256ULL * 1024ULL * 1024ULL;
	constexpr std::size_t kDefaultRetentionGroups = 10U;

	struct RetentionResult
	{
		std::size_t groupsRetained = 0;
		std::size_t groupsRemoved = 0;
		std::size_t filesRemoved = 0;
		std::uintmax_t bytesRetained = 0;
	};

	inline std::wstring MakeNativePath(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(path, error);
		std::wstring value = (error ? path : absolute).lexically_normal().wstring();
		if (value.rfind(L"\\\\?\\", 0) == 0)
			return value;
		if (value.rfind(L"\\\\", 0) == 0)
			return L"\\\\?\\UNC\\" + value.substr(2);
		if (value.size() >= MAX_PATH)
			return L"\\\\?\\" + value;
		return value;
	}

	inline std::wstring DisplayPath(const std::wstring& path)
	{
		if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
			return L"\\\\" + path.substr(8);
		if (path.rfind(L"\\\\?\\", 0) == 0)
			return path.substr(4);
		return path;
	}

	inline std::wstring JoinNativePath(
		const std::wstring& directory,
		const wchar_t* name)
	{
		if (directory.empty())
			return {};
		std::wstring result = directory;
		if (result.back() != L'\\' && result.back() != L'/')
			result.push_back(L'\\');
		result += name;
		return result;
	}

	// Creating a directory is not enough: an existing read-only directory must
	// be rejected. The disposable byte proves create/write/flush/delete access.
	inline bool ProbeWritableDirectory(const std::filesystem::path& directory)
	{
		const std::wstring nativeDirectory = MakeNativePath(directory);
		if (nativeDirectory.empty())
			return false;
		const std::filesystem::path filesystemDirectory(nativeDirectory);

		std::error_code error;
		std::filesystem::create_directories(filesystemDirectory, error);
		if (error || !std::filesystem::is_directory(filesystemDirectory, error) || error)
			return false;

		static volatile LONG probeSequence = 0;
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			std::array<wchar_t, 128> name{};
			_snwprintf_s(
				name.data(),
				name.size(),
				_TRUNCATE,
				L".vsmr-write-probe-%lu-%lu-%ld.tmp",
				static_cast<unsigned long>(::GetCurrentProcessId()),
				static_cast<unsigned long>(::GetTickCount()),
				static_cast<long>(::InterlockedIncrement(&probeSequence)));
			const std::wstring probePath = JoinNativePath(nativeDirectory, name.data());
			const HANDLE probe = ::CreateFileW(
				probePath.c_str(),
				GENERIC_WRITE | DELETE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_WRITE_THROUGH,
				nullptr);
			if (probe == INVALID_HANDLE_VALUE)
			{
				if (::GetLastError() == ERROR_FILE_EXISTS)
					continue;
				return false;
			}

			const BYTE marker = 0xA5U;
			DWORD written = 0;
			const bool writable =
				::WriteFile(probe, &marker, sizeof(marker), &written, nullptr) != FALSE &&
				written == sizeof(marker) &&
				::FlushFileBuffers(probe) != FALSE;
			::CloseHandle(probe);
			::DeleteFileW(probePath.c_str());
			return writable;
		}
		return false;
	}

	inline std::filesystem::path LocalAppDataReportDirectory()
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetEnvironmentVariableW(
				L"LOCALAPPDATA",
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (required == 0)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data()) / L"vSMR" / L"CrashReports";
			if (required > 32768U)
				return {};
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	inline std::filesystem::path TemporaryReportDirectory()
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetTempPathW(
				static_cast<DWORD>(buffer.size()),
				buffer.data());
			if (required == 0)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data()) / L"vSMR" / L"CrashReports";
			if (required > 32768U)
				return {};
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	template <std::size_t Count>
	inline std::filesystem::path SelectFirstWritableDirectory(
		const std::array<std::filesystem::path, Count>& candidates,
		std::size_t* selectedIndex = nullptr)
	{
		for (std::size_t index = 0; index < candidates.size(); ++index)
		{
			if (!candidates[index].empty() && ProbeWritableDirectory(candidates[index]))
			{
				if (selectedIndex != nullptr)
					*selectedIndex = index;
				return candidates[index];
			}
		}
		return {};
	}

	inline std::filesystem::path SelectReportDirectory(
		const std::filesystem::path& moduleDirectory,
		std::size_t* selectedIndex = nullptr)
	{
		const std::array<std::filesystem::path, 3> candidates = {
			LocalAppDataReportDirectory(),
			moduleDirectory / L"vSMR_Data" / L"CrashReports",
			TemporaryReportDirectory()
		};
		return SelectFirstWritableDirectory(candidates, selectedIndex);
	}

	inline bool IsCrashArtifact(
		const std::filesystem::path& path,
		std::wstring& groupName,
		bool& isTemporary)
	{
		const std::wstring filename = path.filename().wstring();
		if (filename.rfind(L"vSMR-crash-", 0) != 0)
			return false;
		const std::array<std::wstring, 5> suffixes = {
			L".dmp.tmp", L".txt.tmp", L".claim", L".dmp", L".txt"
		};
		for (std::size_t index = 0; index < suffixes.size(); ++index)
		{
			const auto& suffix = suffixes[index];
			if (filename.size() > suffix.size() &&
				filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				groupName = filename.substr(0, filename.size() - suffix.size());
				isTemporary = index < 3U;
				return true;
			}
		}
		return false;
	}

	inline RetentionResult ApplyRetention(
		const std::filesystem::path& directory,
		std::size_t maximumGroups = kDefaultRetentionGroups,
		std::uintmax_t maximumBytes = kDefaultRetentionBytes)
	{
		struct ArtifactGroup
		{
			std::vector<std::filesystem::path> files;
			std::filesystem::file_time_type newest{};
			std::uintmax_t bytes = 0;
		};

		RetentionResult result;
		const std::wstring nativeDirectory = MakeNativePath(directory);
		if (nativeDirectory.empty())
			return result;

		std::error_code error;
		std::map<std::wstring, ArtifactGroup> grouped;
		for (std::filesystem::directory_iterator iterator(
			std::filesystem::path(nativeDirectory), error), end;
			!error && iterator != end;
			iterator.increment(error))
		{
			if (!iterator->is_regular_file(error) || error)
			{
				error.clear();
				continue;
			}
			std::wstring groupName;
			bool temporary = false;
			if (!IsCrashArtifact(iterator->path(), groupName, temporary))
				continue;
			if (temporary)
			{
				if (std::filesystem::remove(iterator->path(), error) && !error)
					++result.filesRemoved;
				error.clear();
				continue;
			}

			auto& group = grouped[groupName];
			group.files.push_back(iterator->path());
			const auto modified = iterator->last_write_time(error);
			if (!error && (group.files.size() == 1U || modified > group.newest))
				group.newest = modified;
			error.clear();
			const std::uintmax_t size = iterator->file_size(error);
			if (!error)
				group.bytes += size;
			error.clear();
		}

		std::vector<ArtifactGroup*> ordered;
		ordered.reserve(grouped.size());
		for (auto& entry : grouped)
			ordered.push_back(&entry.second);
		std::sort(ordered.begin(), ordered.end(), [](const ArtifactGroup* left, const ArtifactGroup* right)
			{
				return left->newest > right->newest;
			});

		for (ArtifactGroup* group : ordered)
		{
			const bool withinByteLimit = group->bytes <= maximumBytes &&
				result.bytesRetained <= maximumBytes - group->bytes;
			const bool keep = result.groupsRetained < maximumGroups && withinByteLimit;
			if (keep)
			{
				++result.groupsRetained;
				result.bytesRetained += group->bytes;
				continue;
			}
			++result.groupsRemoved;
			for (const auto& path : group->files)
			{
				if (std::filesystem::remove(path, error) && !error)
					++result.filesRemoved;
				error.clear();
			}
		}
		return result;
	}
}
