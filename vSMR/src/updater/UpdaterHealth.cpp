#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	using release_model::ParseSemVer;

	fs::path HealthMarkerPath(const fs::path& storageRoot, const fs::path& installRoot)
	{
		return storageRoot / L"health" / (HashName(installRoot) + L".pending.json");
	}

	bool ProcessCreationStamp(DWORD processId, std::uint64_t& stamp, bool& alive)
	{
		alive = false;
		stamp = 0;
		UniqueHandle process(::OpenProcess(
			PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
			FALSE, processId));
		if (!process)
		{
			// ERROR_INVALID_PARAMETER is the documented result for a PID that no
			// longer exists. Other failures (notably access denied) leave the
			// identity unknown and must not trigger a rollback.
			if (::GetLastError() == ERROR_INVALID_PARAMETER)
				return true;
			return false;
		}
		alive = ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
		FILETIME created{}, exited{}, kernel{}, user{};
		if (!::GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
			return false;
		ULARGE_INTEGER value{};
		value.LowPart = created.dwLowDateTime;
		value.HighPart = created.dwHighDateTime;
		stamp = value.QuadPart;
		return true;
	}

	bool WriteHealthMarker(
		const StartupOptions& options,
		const fs::path& storageRoot,
		StartupResult& result,
		const std::string& phase)
	{
		result.healthMarkerPath = HealthMarkerPath(storageRoot, options.installRoot);
		result.updaterStoragePath = storageRoot;
		rapidjson::Document document;
		document.SetObject();
		document.AddMember("schema_version", 1, document.GetAllocator());
		AddJsonString(document, document, "install_root", WideToUtf8(options.installRoot.wstring()));
		AddJsonString(document, document, "version", result.selectedVersion);
		AddJsonString(document, document, "previous_version", result.previousVersion);
		AddJsonString(document, document, "previous_runtime_sha256", result.previousRuntimeSha256);
		AddJsonString(document, document, "phase", phase);
		AddJsonString(document, document, "rollback_backup", WideToUtf8(result.rollbackBackupPath.wstring()));
		AddJsonString(document, document, "previous_runtime", WideToUtf8(result.previousRuntimePath.wstring()));
		if (phase == "attempting")
		{
			const DWORD processId = options.hostProcessId == 0
				? ::GetCurrentProcessId() : options.hostProcessId;
			std::uint64_t processCreated = 0;
			bool alive = false;
			if (!ProcessCreationStamp(processId, processCreated, alive) || !alive)
				return false;
			document.AddMember("attempt_pid", static_cast<std::uint64_t>(processId), document.GetAllocator());
			document.AddMember("attempt_process_created_100ns", processCreated, document.GetAllocator());
		}
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document.Accept(writer);
		return AtomicWriteText(
			result.healthMarkerPath,
			std::string(buffer.GetString(), buffer.Size()) + "\n");
	}

	bool ReadHealthMarker(
		const StartupOptions& options,
		const fs::path& markerPath,
		StartupResult& result,
		std::string& phase,
		std::uint32_t& attemptProcessId,
		std::uint64_t& attemptProcessCreated)
	{
		std::string json;
		if (!ReadText(markerPath, json, 128 * 1024))
			return false;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject() ||
			!document.HasMember("schema_version") || !document["schema_version"].IsInt() ||
			document["schema_version"].GetInt() != 1 ||
			JsonString(document, "install_root") != WideToUtf8(options.installRoot.wstring()))
		{
			return false;
		}
		result.selectedVersion = JsonString(document, "version");
		result.previousVersion = JsonString(document, "previous_version");
		result.previousRuntimeSha256 = ToLowerAscii(JsonString(document, "previous_runtime_sha256"));
		phase = JsonString(document, "phase");
		const std::uint64_t processId = JsonUint64(document, "attempt_pid");
		attemptProcessId = processId <= (std::numeric_limits<std::uint32_t>::max)()
			? static_cast<std::uint32_t>(processId) : 0;
		attemptProcessCreated = JsonUint64(document, "attempt_process_created_100ns");
		result.rollbackBackupPath = Utf8ToWide(JsonString(document, "rollback_backup"));
		result.previousRuntimePath = Utf8ToWide(JsonString(document, "previous_runtime"));
		result.healthMarkerPath = markerPath;
		result.installationRoot = options.installRoot;
		return (phase == "attempting" || phase == "installing" ||
			phase == "failed" || phase == "healthy") &&
			(phase != "attempting" || (attemptProcessId != 0 && attemptProcessCreated != 0)) &&
			ParseSemVer(result.selectedVersion).valid &&
			ParseSemVer(result.previousVersion).valid &&
			IsHex(result.previousRuntimeSha256, 64) &&
			(phase == "installing" ||
				(!result.rollbackBackupPath.empty() && !result.previousRuntimePath.empty()));
	}

	bool RewriteHealthMarkerPhase(
		const StartupResult& update,
		const std::set<std::string>& allowedCurrentPhases,
		const char* newPhase)
	{
		const fs::path storage = update.updaterStoragePath.empty()
			? GetUpdaterStorageDirectory() : update.updaterStoragePath;
		if (storage.empty() || update.healthMarkerPath.empty() ||
			!IsPathBelow(update.healthMarkerPath, storage / L"health"))
		{
			return false;
		}
		std::string json;
		if (!ReadText(update.healthMarkerPath, json, 128 * 1024))
			return false;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject() ||
			!document.HasMember("schema_version") || !document["schema_version"].IsInt() ||
			document["schema_version"].GetInt() != 1 ||
			update.installationRoot.empty() ||
			JsonString(document, "install_root") != WideToUtf8(update.installationRoot.wstring()) ||
			JsonString(document, "version") != update.selectedVersion ||
			JsonString(document, "previous_version") != update.previousVersion ||
			ToLowerAscii(JsonString(document, "previous_runtime_sha256")) !=
				ToLowerAscii(update.previousRuntimeSha256) ||
			JsonString(document, "rollback_backup") != WideToUtf8(update.rollbackBackupPath.wstring()) ||
			JsonString(document, "previous_runtime") != WideToUtf8(update.previousRuntimePath.wstring()) ||
			allowedCurrentPhases.find(JsonString(document, "phase")) == allowedCurrentPhases.end())
		{
			return false;
		}
		document["phase"].SetString(newPhase, document.GetAllocator());
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document.Accept(writer);
		return AtomicWriteText(
			update.healthMarkerPath,
			std::string(buffer.GetString(), buffer.Size()) + "\n");
	}

	bool WriteQuarantineMarker(
		const fs::path& storageRoot,
		const std::string& version,
		const std::string& reason)
	{
		if (!ParseSemVer(version).valid)
			return false;
		rapidjson::Document document;
		document.SetObject();
		document.AddMember("schema_version", 1, document.GetAllocator());
		AddJsonString(document, document, "version", version);
		AddJsonString(document, document, "quarantined_utc", UtcNow());
		AddJsonString(document, document, "reason", reason);
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document.Accept(writer);
		return AtomicWriteText(
			storageRoot / L"quarantine" / (Utf8ToWide(version) + L".json"),
			std::string(buffer.GetString(), buffer.Size()) + "\n");
	}

}
