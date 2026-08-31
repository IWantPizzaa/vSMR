#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return {};
		const int size = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0)
			return {};
		std::string result(static_cast<std::size_t>(size), '\0');
		if (::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size)
		{
			return {};
		}
		return result;
	}

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty())
			return {};
		const int size = ::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), nullptr, 0);
		if (size <= 0)
			return {};
		std::wstring result(static_cast<std::size_t>(size), L'\0');
		if (::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), result.data(), size) != size)
		{
			return {};
		}
		return result;
	}

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
		return value;
	}

	std::wstring ToLowerWide(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
			return static_cast<wchar_t>(std::towlower(character));
		});
		return value;
	}

	bool IsHex(const std::string& value, std::size_t length)
	{
		return value.size() == length &&
			std::all_of(value.begin(), value.end(), [](unsigned char character) {
				return std::isxdigit(character) != 0;
			});
	}

	std::string Hex(const BYTE* bytes, DWORD size)
	{
		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (DWORD index = 0; index < size; ++index)
			output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
		return output.str();
	}

	std::string SecureRandomHex(DWORD byteCount)
	{
		if (byteCount == 0 || byteCount > 64)
			return {};
		std::vector<BYTE> bytes(byteCount);
		if (BCryptGenRandom(
			nullptr, bytes.data(), byteCount,
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
		{
			return {};
		}
		return Hex(bytes.data(), byteCount);
	}

	bool ProbeWritableDirectory(const fs::path& directory) noexcept
	{
		try
		{
			if (directory.empty())
				return false;
			std::error_code error;
			fs::create_directories(directory, error);
			if (error || !fs::is_directory(directory, error) || error)
				return false;

			for (unsigned int attempt = 0; attempt < 4; ++attempt)
			{
				std::string nonce = SecureRandomHex(12);
				if (nonce.empty())
				{
					std::ostringstream fallback;
					fallback << std::hex << ::GetCurrentProcessId() << '-'
						<< ::GetTickCount64() << '-' << attempt;
					nonce = fallback.str();
				}
				const fs::path probe = directory /
					(L".vsmr-write-probe-" + Utf8ToWide(nonce) + L".tmp");
				UniqueHandle file(::CreateFileW(
					probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
					FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr));
				if (!file)
				{
					if (::GetLastError() == ERROR_FILE_EXISTS ||
						::GetLastError() == ERROR_ALREADY_EXISTS)
					{
						continue;
					}
					return false;
				}
				const BYTE value = 0xA5;
				DWORD written = 0;
				const bool writeSucceeded = ::WriteFile(
					file.get(), &value, sizeof(value), &written, nullptr) != FALSE &&
					written == sizeof(value) && ::FlushFileBuffers(file.get()) != FALSE;
				file.reset();
				const bool deleteSucceeded = ::DeleteFileW(probe.c_str()) != FALSE;
				return writeSucceeded && deleteSucceeded;
			}
		}
		catch (...)
		{
		}
		return false;
	}

	fs::path LocalAppDataUpdaterCandidate() noexcept
	{
		try
		{
			PWSTR knownFolder = nullptr;
			if (SUCCEEDED(::SHGetKnownFolderPath(
				FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder)) &&
				knownFolder != nullptr)
			{
				fs::path result = fs::path(knownFolder) / L"vSMR" / L"Updater";
				::CoTaskMemFree(knownFolder);
				return result;
			}
			if (knownFolder != nullptr)
				::CoTaskMemFree(knownFolder);
			std::array<wchar_t, 32768> buffer{};
			const DWORD length = ::GetEnvironmentVariableW(
				L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length > 0 && length < buffer.size())
				return fs::path(std::wstring(buffer.data(), length)) /
					L"vSMR" / L"Updater";
		}
		catch (...)
		{
		}
		return {};
	}

	fs::path TemporaryUpdaterCandidate() noexcept
	{
		try
		{
			std::array<wchar_t, 32768> buffer{};
			const DWORD length = ::GetTempPathW(
				static_cast<DWORD>(buffer.size()), buffer.data());
			if (length > 0 && length < buffer.size())
				return fs::path(std::wstring(buffer.data(), length)) /
					L"vSMR" / L"Updater";
		}
		catch (...)
		{
		}
		return {};
	}

	fs::path GetProductionSessionLockStorageRoot() noexcept
	{
		// Session leases must never switch between LocalAppData and Temp while
		// another EuroScope process is alive. Keep their deterministic root in
		// the per-user Temp directory even when persistent updater state uses
		// LocalAppData.
		const fs::path temporary = TemporaryUpdaterCandidate();
		return ProbeWritableDirectory(temporary) ? temporary : fs::path{};
	}

	std::string UtcNow()
	{
		SYSTEMTIME time{};
		::GetSystemTime(&time);
		char buffer[32]{};
		std::snprintf(
			buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
			time.wYear, time.wMonth, time.wDay,
			time.wHour, time.wMinute, time.wSecond);
		return buffer;
	}

	std::string UtcAfterSeconds(std::uint64_t seconds)
	{
		FILETIME now{};
		::GetSystemTimeAsFileTime(&now);
		ULARGE_INTEGER value{};
		value.LowPart = now.dwLowDateTime;
		value.HighPart = now.dwHighDateTime;
		value.QuadPart += seconds * 10000000ULL;
		FILETIME future{};
		future.dwLowDateTime = value.LowPart;
		future.dwHighDateTime = value.HighPart;
		SYSTEMTIME time{};
		if (!::FileTimeToSystemTime(&future, &time))
			return {};
		char buffer[32]{};
		std::snprintf(
			buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
			time.wYear, time.wMonth, time.wDay,
			time.wHour, time.wMinute, time.wSecond);
		return buffer;
	}

	std::uint64_t Fnv1a64(const std::wstring& value)
	{
		std::uint64_t hash = 14695981039346656037ULL;
		for (const wchar_t character : value)
		{
			const wchar_t folded = static_cast<wchar_t>(std::towlower(character));
			const BYTE* bytes = reinterpret_cast<const BYTE*>(&folded);
			for (std::size_t index = 0; index < sizeof(folded); ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ULL;
			}
		}
		return hash;
	}

	std::wstring HashName(const fs::path& installRoot)
	{
		std::error_code error;
		fs::path absolute = fs::absolute(installRoot, error);
		fs::path identity = (error ? installRoot : absolute).lexically_normal();
		UniqueHandle directory(::CreateFileW(
			identity.c_str(), 0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		if (directory)
		{
			const DWORD required = ::GetFinalPathNameByHandleW(
				directory.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (required > 0 && required < 32768)
			{
				std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
				const DWORD written = ::GetFinalPathNameByHandleW(
					directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
					FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
				if (written > 0 && written < buffer.size())
				{
					std::wstring finalPath(buffer.data(), written);
					if (finalPath.rfind(L"\\\\?\\", 0) == 0)
						finalPath.erase(0, 4);
					identity = fs::path(finalPath).lexically_normal();
				}
			}
		}
		const std::wstring normalized = identity.wstring();
		std::wostringstream output;
		output << std::hex << std::setfill(L'0') << std::setw(16) << Fnv1a64(normalized);
		return output.str();
	}

	fs::path SessionLockPath(const fs::path& storageRoot, const fs::path& installRoot)
	{
		return storageRoot / L"locks" / (HashName(installRoot) + L".session.lock");
	}

	bool ReadBytes(const fs::path& path, std::vector<std::uint8_t>& output, std::uint64_t maximum)
	{
		std::error_code error;
		const auto size = fs::file_size(path, error);
		if (error || size > maximum || size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
			return false;
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
			return false;
		output.resize(static_cast<std::size_t>(size));
		if (!output.empty())
			input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
		return input.good() || (input.eof() && input.gcount() == static_cast<std::streamsize>(output.size()));
	}

	bool ReadText(const fs::path& path, std::string& output, std::uint64_t maximum)
	{
		std::vector<std::uint8_t> bytes;
		if (!ReadBytes(path, bytes, maximum))
			return false;
		output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return true;
	}

	bool AtomicWrite(const fs::path& path, const void* data, std::size_t size)
	{
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		if (error)
			return false;
		const fs::path temporary = path.wstring() + L".tmp." +
			std::to_wstring(::GetCurrentProcessId()) + L"." +
			std::to_wstring(::GetTickCount64());
		UniqueHandle file(::CreateFileW(
			temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY, nullptr));
		if (!file)
			return false;
		const BYTE* cursor = static_cast<const BYTE*>(data);
		std::size_t remaining = size;
		while (remaining > 0)
		{
			const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(1024 * 1024)));
			DWORD written = 0;
			if (!::WriteFile(file.get(), cursor, chunk, &written, nullptr) || written != chunk)
			{
				file.reset();
				::DeleteFileW(temporary.c_str());
				return false;
			}
			cursor += written;
			remaining -= written;
		}
		if (!::FlushFileBuffers(file.get()))
		{
			file.reset();
			::DeleteFileW(temporary.c_str());
			return false;
		}
		file.reset();
		if (!::MoveFileExW(
			temporary.c_str(), path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}

	bool AtomicWriteText(const fs::path& path, const std::string& text)
	{
		return AtomicWrite(path, text.data(), text.size());
	}

	bool IsRegularFile(const fs::path& path)
	{
		std::error_code error;
		return fs::is_regular_file(path, error) && !error;
	}

	std::string JsonString(const rapidjson::Value& object, const char* name)
	{
		if (!object.IsObject() || !object.HasMember(name) || !object[name].IsString())
			return {};
		return object[name].GetString();
	}

	bool JsonBool(const rapidjson::Value& object, const char* name, bool fallback)
	{
		return object.IsObject() && object.HasMember(name) && object[name].IsBool()
			? object[name].GetBool() : fallback;
	}

	std::uint64_t JsonUint64(const rapidjson::Value& object, const char* name)
	{
		return object.IsObject() && object.HasMember(name) && object[name].IsUint64()
			? object[name].GetUint64() : 0;
	}

	void AddJsonString(
		rapidjson::Document& document,
		rapidjson::Value& object,
		const char* name,
		const std::string& value)
	{
		auto& allocator = document.GetAllocator();
		rapidjson::Value key(name, allocator);
		rapidjson::Value content;
		content.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
		object.AddMember(key, content, allocator);
	}

	std::string SerializeState(const State& state)
	{
		rapidjson::Document document;
		document.SetObject();
		document.AddMember("schema_version", 1, document.GetAllocator());
		AddJsonString(document, document, "status", state.status);
		AddJsonString(document, document, "installed_version", state.installedVersion);
		AddJsonString(document, document, "selected_version", state.selectedVersion);
		AddJsonString(document, document, "available_version", state.availableVersion);
		document.AddMember("download_percent", state.downloadPercent, document.GetAllocator());
		AddJsonString(document, document, "last_checked_utc", state.lastCheckedUtc);
		AddJsonString(document, document, "next_check_utc", state.nextCheckUtc);
		AddJsonString(document, document, "message", state.message);
		AddJsonString(document, document, "error_code", state.errorCode);
		AddJsonString(document, document, "error", state.error);
		document.AddMember("restart_required", state.restartRequired, document.GetAllocator());
		document.AddMember("loader_update_deferred", state.loaderUpdateDeferred, document.GetAllocator());
		AddJsonString(document, document, "release_url", state.releaseUrl);
		AddJsonString(document, document, "last_action_request_id", state.lastActionRequestId);
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document.Accept(writer);
		std::string result(buffer.GetString(), buffer.Size());
		result.push_back('\n');
		return result;
	}

	void PersistState(Context& context)
	{
		AtomicWriteText(context.statePath, SerializeState(context.state));
	}

	bool Report(Context& context, ProgressStage stage, int percent, const std::wstring& message)
	{
		context.state.message = WideToUtf8(message);
		context.state.downloadPercent = percent;
		PersistState(context);
		if (!context.options.progressCallback)
			return true;
		bool keepGoing = true;
		try
		{
			keepGoing = context.options.progressCallback(Progress{ stage, percent, message });
		}
		catch (...)
		{
			keepGoing = true;
		}
		context.cancelled = !keepGoing;
		return keepGoing;
	}

	DWORD RemainingMs(const Context& context, DWORD operationMaximum)
	{
		const ULONGLONG elapsed = ::GetTickCount64() - context.startedTick;
		if (elapsed >= context.options.overallDeadlineMs)
			return 0;
		const ULONGLONG remaining = context.options.overallDeadlineMs - elapsed;
		return static_cast<DWORD>((std::min)(remaining, static_cast<ULONGLONG>(operationMaximum)));
	}

	Config LoadConfig(const fs::path& path, UpdateChannel defaultChannel)
	{
		Config config;
		config.channel = defaultChannel;
		std::string json;
		if (!ReadText(path, json, 64 * 1024))
			return config;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
			return config;
		config.autoCheck = JsonBool(document, "auto_check", true);
		config.autoDownload = JsonBool(document, "auto_download", true);
		config.autoInstall = JsonBool(document, "auto_install", true);
		config.protectModifiedAviso = JsonBool(document, "protect_modified_aviso", true);
		config.skippedVersion = JsonString(document, "skipped_version");
		const std::string channel = ToLowerAscii(JsonString(document, "channel"));
		if (channel == "stable")
			config.channel = UpdateChannel::Stable;
		else if (channel == "beta")
			config.channel = UpdateChannel::Beta;
		return config;
	}

	Action ConsumeAction(const fs::path& storageRoot)
	{
		Action result;
		const fs::path actionPath = storageRoot / L"action.json";
		const fs::path processingPath = storageRoot / L"action.processing.json";
		::DeleteFileW(processingPath.c_str());
		if (!::MoveFileExW(actionPath.c_str(), processingPath.c_str(), MOVEFILE_WRITE_THROUGH))
			return result;
		std::string json;
		if (ReadText(processingPath, json, 64 * 1024))
		{
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (!document.HasParseError() && document.IsObject())
			{
				result.requestId = JsonString(document, "request_id");
				result.action = ToLowerAscii(JsonString(document, "action"));
				result.valid = !result.requestId.empty() &&
					(result.action == "check_now" || result.action == "retry_update" ||
					 result.action == "reload_aviso" || result.action == "clear_status");
			}
		}
		::DeleteFileW(processingPath.c_str());
		return result;
	}

	bool ParseUtcFileTime(const std::string& text, FILETIME& result)
	{
		if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
			text[13] != ':' || text[16] != ':' || text[19] != 'Z')
		{
			return false;
		}
		SYSTEMTIME time{};
		try
		{
			time.wYear = static_cast<WORD>(std::stoi(text.substr(0, 4)));
			time.wMonth = static_cast<WORD>(std::stoi(text.substr(5, 2)));
			time.wDay = static_cast<WORD>(std::stoi(text.substr(8, 2)));
			time.wHour = static_cast<WORD>(std::stoi(text.substr(11, 2)));
			time.wMinute = static_cast<WORD>(std::stoi(text.substr(14, 2)));
			time.wSecond = static_cast<WORD>(std::stoi(text.substr(17, 2)));
		}
		catch (...)
		{
			return false;
		}
		return ::SystemTimeToFileTime(&time, &result) == TRUE;
	}

	bool IsFutureUtc(const std::string& text)
	{
		FILETIME parsed{};
		if (!ParseUtcFileTime(text, parsed))
			return false;
		FILETIME now{};
		::GetSystemTimeAsFileTime(&now);
		return ::CompareFileTime(&parsed, &now) > 0;
	}

	State LoadPreviousState(const fs::path& path)
	{
		State state;
		std::string json;
		if (!ReadText(path, json, 128 * 1024))
			return state;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
			return state;
		state.lastCheckedUtc = JsonString(document, "last_checked_utc");
		state.nextCheckUtc = JsonString(document, "next_check_utc");
		state.lastActionRequestId = JsonString(document, "last_action_request_id");
		state.availableVersion = JsonString(document, "available_version");
		return state;
	}

	bool EndsWithNoCase(const std::wstring& value, const std::wstring& suffix)
	{
		if (suffix.size() > value.size())
			return false;
		return ToLowerWide(value.substr(value.size() - suffix.size())) == ToLowerWide(suffix);
	}

}
