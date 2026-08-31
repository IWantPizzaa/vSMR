#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterUpdater.hpp"

#include "crash/CrashReportSupport.hpp"
#include "plugin/PluginMetadata.hpp"
#include "shared/RapidJsonUtils.hpp"
#include "shared/TextUtils.hpp"

#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr std::uintmax_t kMaximumUpdaterJsonBytes = 256u * 1024u;
	// Every radar window reads the same updater files. Hold this mutex across
	// complete read-modify-write transactions so one window cannot overwrite
	// settings that another window has just accepted.
	std::mutex gUpdaterFileMutex;

	using Allocator = rapidjson::Document::AllocatorType;
	using VsmrRapidJson::AddString;
	using VsmrRapidJson::CloneJsonValue;
	using VsmrRapidJson::SetBoolMember;
	using VsmrRapidJson::SetStringMember;

	std::string ReadString(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) ||
			!object[key].IsString())
		{
			return {};
		}
		return object[key].GetString();
	}

	bool ReadBool(const rapidjson::Value& object, const char* key, bool fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) ||
			!object[key].IsBool())
		{
			return fallback;
		}
		return object[key].GetBool();
	}

	std::string SerializePretty(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.SetIndent('\t', 1);
		value.Accept(writer);
		return std::string(buffer.GetString(), buffer.Size());
	}

	std::filesystem::path EnvironmentDirectory(const wchar_t* variable)
	{
		if (variable == nullptr || variable[0] == L'\0')
			return {};
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetEnvironmentVariableW(
				variable,
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (required == 0 || required > 32768U)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data());
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	std::filesystem::path UpdaterDirectory()
	{
		PWSTR knownFolder = nullptr;
		if (SUCCEEDED(::SHGetKnownFolderPath(
			FOLDERID_LocalAppData,
			KF_FLAG_CREATE,
			nullptr,
			&knownFolder)) && knownFolder != nullptr)
		{
			const std::filesystem::path result =
				std::filesystem::path(knownFolder) / L"vSMR" / L"Updater";
			::CoTaskMemFree(knownFolder);
			return result;
		}
		if (knownFolder != nullptr)
			::CoTaskMemFree(knownFolder);

		const std::filesystem::path fallback = EnvironmentDirectory(L"LOCALAPPDATA");
		return fallback.empty()
			? std::filesystem::path{}
			: fallback / L"vSMR" / L"Updater";
	}

	std::string InstalledVersion()
	{
		std::string version = VsmrPluginVersion;
		if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
			version.erase(version.begin());
		return version;
	}

	std::string DefaultUpdateChannel()
	{
		return InstalledVersion().find('-') == std::string::npos ? "stable" : "beta";
	}

	std::string CurrentUtcText()
	{
		SYSTEMTIME utc = {};
		::GetSystemTime(&utc);
		char text[32] = {};
		_snprintf_s(
			text,
			_TRUNCATE,
			"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			utc.wMilliseconds);
		return text;
	}

	bool ReadUpdaterJson(
		const std::filesystem::path& path,
		rapidjson::Document& document,
		bool& exists,
		std::string& error)
	{
		exists = false;
		error.clear();
		document.SetObject();
		std::error_code filesystemError;
		if (!std::filesystem::exists(path, filesystemError))
		{
			if (filesystemError)
				error = "Unable to inspect the updater state file.";
			return !filesystemError;
		}
		exists = true;
		const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
		if (filesystemError || size == 0 || size > kMaximumUpdaterJsonBytes)
		{
			error = "The updater state file is empty or too large.";
			return false;
		}

		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			error = "Unable to read the updater state file.";
			return false;
		}
		std::string json(
			(std::istreambuf_iterator<char>(input)),
			std::istreambuf_iterator<char>());
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
		{
			error = "The updater state file contains invalid JSON.";
			return false;
		}
		return true;
	}

	bool WriteUpdaterJsonAtomically(
		const std::filesystem::path& path,
		const rapidjson::Value& document,
		std::string& error)
	{
		error.clear();
		const std::string json = SerializePretty(document) + "\n";
		std::error_code filesystemError;
		std::filesystem::create_directories(path.parent_path(), filesystemError);
		if (filesystemError)
		{
			error = "Unable to create the updater settings directory.";
			return false;
		}

		static volatile LONG temporarySequence = 0;
		const LONG sequence = ::InterlockedIncrement(&temporarySequence);
		const std::filesystem::path temporary = path.parent_path() /
			(L"." + path.filename().wstring() + L"." +
				std::to_wstring(::GetCurrentProcessId()) + L"." +
				std::to_wstring(::GetTickCount64()) + L"." +
				std::to_wstring(sequence) + L".tmp");
		const std::wstring nativeTemporary = VsmrCrashSupport::MakeNativePath(temporary);
		const std::wstring nativeTarget = VsmrCrashSupport::MakeNativePath(path);
		HANDLE output = ::CreateFileW(
			nativeTemporary.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (output == INVALID_HANDLE_VALUE)
		{
			error = "Unable to create a temporary updater settings file.";
			return false;
		}

		bool succeeded = true;
		std::size_t offset = 0;
		while (offset < json.size())
		{
			const DWORD requested = static_cast<DWORD>((std::min)(
				json.size() - offset,
				static_cast<std::size_t>(1024U * 1024U)));
			DWORD written = 0;
			if (::WriteFile(output, json.data() + offset, requested, &written, nullptr) == FALSE ||
				written != requested)
			{
				succeeded = false;
				break;
			}
			offset += written;
		}
		// Flush the complete temporary file before replacement. A crash therefore
		// leaves either the previous document or the new document, never a partial
		// updater configuration.
		if (succeeded)
			succeeded = ::FlushFileBuffers(output) != FALSE;
		::CloseHandle(output);
		if (succeeded)
		{
			succeeded = ::MoveFileExW(
				nativeTemporary.c_str(),
				nativeTarget.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		}
		if (!succeeded)
		{
			::DeleteFileW(nativeTemporary.c_str());
			error = "Unable to commit the updater settings file.";
			return false;
		}
		return true;
	}

	void BuildDefaultUpdaterConfig(rapidjson::Document& config)
	{
		config.SetObject();
		Allocator& allocator = config.GetAllocator();
		config.AddMember("schema_version", 1, allocator);
		config.AddMember("auto_check", true, allocator);
		config.AddMember("auto_download", true, allocator);
		config.AddMember("auto_install", true, allocator);
		config.AddMember("protect_modified_aviso", true, allocator);
		AddString(config, "channel", DefaultUpdateChannel(), allocator);
		AddString(config, "skipped_version", "", allocator);
	}

	bool LoadUpdaterConfig(
		rapidjson::Document& config,
		bool& writable,
		std::string& error)
	{
		writable = true;
		error.clear();
		const std::filesystem::path directory = UpdaterDirectory();
		if (directory.empty())
		{
			BuildDefaultUpdaterConfig(config);
			writable = false;
			error = "LOCALAPPDATA is unavailable; updater settings cannot be stored.";
			return false;
		}

		bool exists = false;
		if (!ReadUpdaterJson(directory / L"config.json", config, exists, error))
		{
			BuildDefaultUpdaterConfig(config);
			writable = false;
			return false;
		}
		if (!exists)
		{
			BuildDefaultUpdaterConfig(config);
			return true;
		}
		if (!config.HasMember("schema_version") || !config["schema_version"].IsInt() ||
			config["schema_version"].GetInt() != 1)
		{
			BuildDefaultUpdaterConfig(config);
			writable = false;
			error = "The updater configuration uses an unsupported schema version.";
			return false;
		}

		const std::string channel = ToLowerAsciiCopy(ReadString(config, "channel"));
		if (channel != "stable" && channel != "beta")
			SetStringMember(config, "channel", DefaultUpdateChannel(), config.GetAllocator());
		SetBoolMember(config, "auto_check", ReadBool(config, "auto_check", true), config.GetAllocator());
		SetBoolMember(config, "auto_download", ReadBool(config, "auto_download", true), config.GetAllocator());
		SetBoolMember(config, "auto_install", ReadBool(config, "auto_install", true), config.GetAllocator());
		SetBoolMember(
			config,
			"protect_modified_aviso",
			ReadBool(config, "protect_modified_aviso", true),
			config.GetAllocator());
		SetStringMember(
			config,
			"skipped_version",
			ReadString(config, "skipped_version"),
			config.GetAllocator());
		return true;
	}
}

void VsmrControlCenterUpdater::BuildStatePayload(
	rapidjson::Value& payload,
	rapidjson::Document::AllocatorType& allocator)
{
	std::lock_guard<std::mutex> lock(gUpdaterFileMutex);
	rapidjson::Document config;
	bool configWritable = true;
	std::string configError;
	LoadUpdaterConfig(config, configWritable, configError);

	rapidjson::Document updaterState;
	updaterState.SetObject();
	bool stateExists = false;
	std::string stateError;
	const std::filesystem::path directory = UpdaterDirectory();
	const bool stateLoaded = !directory.empty() && ReadUpdaterJson(
		directory / L"state.json",
		updaterState,
		stateExists,
		stateError);
	if (!stateLoaded || !stateExists ||
		!updaterState.HasMember("schema_version") ||
		!updaterState["schema_version"].IsInt() ||
		updaterState["schema_version"].GetInt() != 1)
	{
		const bool invalidState = stateExists;
		updaterState.SetObject();
		Allocator& stateAllocator = updaterState.GetAllocator();
		updaterState.AddMember("schema_version", 1, stateAllocator);
		AddString(updaterState, "status", invalidState ? "error" : "idle", stateAllocator);
		AddString(updaterState, "installed_version", InstalledVersion(), stateAllocator);
		AddString(updaterState, "selected_version", "", stateAllocator);
		AddString(updaterState, "available_version", "", stateAllocator);
		updaterState.AddMember("download_percent", 0, stateAllocator);
		AddString(updaterState, "last_checked_utc", "", stateAllocator);
		AddString(updaterState, "next_check_utc", "", stateAllocator);
		AddString(
			updaterState,
			"message",
			invalidState
				? "The updater state could not be read. The installed runtime remains active."
				: "Updates are checked before the vSMR runtime loads.",
			stateAllocator);
		AddString(updaterState, "error_code", invalidState ? "state_invalid" : "", stateAllocator);
		AddString(
			updaterState,
			"error",
			invalidState
				? (stateError.empty() ? "Unsupported updater state schema." : stateError)
				: "",
			stateAllocator);
		updaterState.AddMember("restart_required", false, stateAllocator);
		AddString(updaterState, "release_url", "", stateAllocator);
	}
	else if (!updaterState.HasMember("installed_version") ||
		!updaterState["installed_version"].IsString())
	{
		AddString(
			updaterState,
			"installed_version",
			InstalledVersion(),
			updaterState.GetAllocator());
	}

	payload.SetObject();
	payload.AddMember("schemaVersion", 1, allocator);
	payload.AddMember("available", !directory.empty(), allocator);
	payload.AddMember("configWritable", configWritable, allocator);
	if (!configError.empty())
		AddString(payload, "configError", configError, allocator);
	rapidjson::Value configValue;
	CloneJsonValue(config, configValue, allocator);
	payload.AddMember("config", configValue, allocator);
	rapidjson::Value stateValue;
	CloneJsonValue(updaterState, stateValue, allocator);
	payload.AddMember("state", stateValue, allocator);
}

bool VsmrControlCenterUpdater::ApplySettings(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Updater settings payload must be an object.";
		return false;
	}
	std::lock_guard<std::mutex> lock(gUpdaterFileMutex);
	rapidjson::Document config;
	bool writable = true;
	if (!LoadUpdaterConfig(config, writable, error) || !writable)
	{
		if (error.empty())
			error = "Updater settings cannot be changed in this environment.";
		return false;
	}

	auto applyBoolean = [&](const char* key)
	{
		if (!payload->HasMember(key))
			return true;
		if (!(*payload)[key].IsBool())
		{
			error = std::string("Updater setting '") + key + "' must be boolean.";
			return false;
		}
		SetBoolMember(config, key, (*payload)[key].GetBool(), config.GetAllocator());
		return true;
	};
	if (!applyBoolean("auto_check") ||
		!applyBoolean("auto_download") ||
		!applyBoolean("auto_install") ||
		!applyBoolean("protect_modified_aviso"))
	{
		return false;
	}

	if (payload->HasMember("channel"))
	{
		if (!(*payload)["channel"].IsString())
		{
			error = "Updater channel must be 'stable' or 'beta'.";
			return false;
		}
		const std::string channel = ToLowerAsciiCopy(
			TrimAsciiWhitespaceCopy((*payload)["channel"].GetString()));
		if (channel != "stable" && channel != "beta")
		{
			error = "Updater channel must be 'stable' or 'beta'.";
			return false;
		}
		SetStringMember(config, "channel", channel, config.GetAllocator());
	}

	if (payload->HasMember("skipped_version"))
	{
		if (!(*payload)["skipped_version"].IsString())
		{
			error = "Skipped updater version must be a string.";
			return false;
		}
		const std::string skipped =
			TrimAsciiWhitespaceCopy((*payload)["skipped_version"].GetString());
		if (skipped.size() > 64 ||
			std::any_of(skipped.begin(), skipped.end(), [](unsigned char character) {
				return std::isalnum(character) == 0 && character != '.' && character != '-' &&
					character != '+';
			}))
		{
			error = "Skipped updater version contains unsupported characters.";
			return false;
		}
		SetStringMember(config, "skipped_version", skipped, config.GetAllocator());
	}

	return WriteUpdaterJsonAtomically(UpdaterDirectory() / L"config.json", config, error);
}

bool VsmrControlCenterUpdater::QueueAction(
	const rapidjson::Value* payload,
	const std::string& requestId,
	std::string& action,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Updater action payload must be an object.";
		return false;
	}
	action = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(ReadString(*payload, "action")));
	if (action != "retry_update" && action != "reload_aviso")
	{
		error = "Unsupported updater action.";
		return false;
	}
	if (requestId.empty())
	{
		error = "Updater action request ID is required.";
		return false;
	}

	rapidjson::Document request;
	request.SetObject();
	Allocator& allocator = request.GetAllocator();
	request.AddMember("schema_version", 1, allocator);
	AddString(request, "request_id", requestId, allocator);
	AddString(request, "action", action, allocator);
	AddString(request, "requested_utc", CurrentUtcText(), allocator);
	std::lock_guard<std::mutex> lock(gUpdaterFileMutex);
	const std::filesystem::path directory = UpdaterDirectory();
	if (directory.empty())
	{
		error = "LOCALAPPDATA is unavailable; the updater request cannot be queued.";
		return false;
	}
	return WriteUpdaterJsonAtomically(directory / L"action.json", request, error);
}

bool VsmrControlCenterUpdater::OpenRelease(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject() ||
		!payload->HasMember("url") || !(*payload)["url"].IsString())
	{
		error = "A GitHub release URL is required.";
		return false;
	}
	const std::string url = TrimAsciiWhitespaceCopy((*payload)["url"].GetString());
	constexpr const char* kReleasesUrl = "https://github.com/IWantPizzaa/vSMR/releases";
	constexpr const char* kLatestUrl = "https://github.com/IWantPizzaa/vSMR/releases/latest";
	constexpr const char* kTagPrefix = "https://github.com/IWantPizzaa/vSMR/releases/tag/";
	if (url.empty() || url.size() > 512U)
	{
		error = "Only the official vSMR GitHub release page can be opened.";
		return false;
	}

	bool supportedUrl = url == kReleasesUrl || url == kLatestUrl;
	if (!supportedUrl &&
		url.size() > std::strlen(kTagPrefix) &&
		url.compare(0, std::strlen(kTagPrefix), kTagPrefix) == 0)
	{
		const std::string tag = url.substr(std::strlen(kTagPrefix));
		supportedUrl = tag != "." && tag != "..";
		for (const unsigned char character : tag)
		{
			if (std::isalnum(character) == 0 && character != '.' && character != '-' &&
				character != '_' && character != '+' && character != '~')
			{
				supportedUrl = false;
				break;
			}
		}
	}
	if (!supportedUrl)
	{
		error = "Only an official vSMR release, release tag, or latest-release URL can be opened.";
		return false;
	}
	const HINSTANCE opened = ::ShellExecuteA(
		nullptr,
		"open",
		url.c_str(),
		nullptr,
		nullptr,
		SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(opened) <= 32)
	{
		error = "Windows could not open the GitHub release page.";
		return false;
	}
	return true;
}
