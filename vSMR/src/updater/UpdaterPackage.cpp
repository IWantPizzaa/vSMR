#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "updater/UpdaterVerification.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rapidjson/document.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	using release_model::ParseSemVer;
	using release_model::SameSemVerIdentity;

	bool IsX86PortableExecutable(const fs::path& path);

	bool RunRollback(
		Context& context,
		const StartupResult& update,
		fs::path& restoredRuntimePath,
		std::string& error,
		HANDLE existingSessionLock,
		const char* quarantineReason)
	{
		if (update.rollbackBackupPath.empty() ||
			!IsPathBelow(update.rollbackBackupPath, context.storageRoot / L"backups"))
		{
			error = "rollback_backup_unsafe";
			return false;
		}
		const fs::path backupRuntime = update.rollbackBackupPath /
			L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
		std::string expectedRuntimeHash;
		if (!IsRegularFile(backupRuntime) || !IsX86PortableExecutable(backupRuntime) ||
			!verification::Sha256File(backupRuntime, expectedRuntimeHash) ||
			(!update.previousRuntimeSha256.empty() &&
				ToLowerAscii(expectedRuntimeHash) != ToLowerAscii(update.previousRuntimeSha256)))
		{
			error = "rollback_runtime_invalid";
			return false;
		}
		const fs::path restoreScript = RecoveryScriptPath(
			context.storageRoot, context.options.installRoot, update.selectedVersion);
		if (!IsRegularFile(restoreScript))
		{
			error = "rollback_script_missing";
			return false;
		}
		UniqueHandle sessionLock;
		if (existingSessionLock == nullptr || existingSessionLock == INVALID_HANDLE_VALUE)
			sessionLock = AcquireExclusiveSessionLock(
				context.sessionLockStorageRoot, context.options.installRoot);
		if ((existingSessionLock == nullptr || existingSessionLock == INVALID_HANDLE_VALUE) && !sessionLock)
		{
			error = "active_session";
			return false;
		}
		DWORD exitCode = 0;
		const HANDLE effectiveSessionLock = sessionLock ? sessionLock.get() : existingSessionLock;
		if (!RunProcess(
			PowerShellPath(),
			{ L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
			  L"-File", restoreScript.wstring(),
			  L"-DestinationDirectory", context.options.installRoot.wstring(),
			  L"-BackupDirectory", update.rollbackBackupPath.wstring(),
			  L"-PreserveLoader" },
			INFINITE, exitCode, false,
			[&]() { Report(context, ProgressStage::Fallback, -1, L"Rolling back vSMR update..."); },
			{ effectiveSessionLock }))
		{
			error = exitCode == ERROR_TIMEOUT ? "rollback_timeout" : "rollback_failed";
			return false;
		}
		restoredRuntimePath = fs::absolute(context.options.canonicalRuntimePath).lexically_normal();
		std::string restoredRuntimeHash;
		if (!IsRegularFile(restoredRuntimePath) ||
			!IsX86PortableExecutable(restoredRuntimePath) ||
			!verification::Sha256File(restoredRuntimePath, restoredRuntimeHash) ||
			ToLowerAscii(restoredRuntimeHash) != ToLowerAscii(expectedRuntimeHash))
		{
			error = "restored_runtime_missing";
			return false;
		}
		std::string restoredVersion;
		if (!ReadReleaseVersion(context.options.dataRoot, restoredVersion) ||
			!SameSemVerIdentity(restoredVersion, update.previousVersion))
		{
			error = "rollback_verification_failed";
			return false;
		}
		if (quarantineReason != nullptr && quarantineReason[0] != '\0')
			WriteQuarantineMarker(context.storageRoot, update.selectedVersion, quarantineReason);
		if (!update.healthMarkerPath.empty())
			::DeleteFileW(update.healthMarkerPath.c_str());
		return true;
	}

	bool IsX86PortableExecutable(const fs::path& path)
	{
		UniqueHandle file(::CreateFileW(
			path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file)
			return false;
		IMAGE_DOS_HEADER dos{};
		DWORD read = 0;
		if (!::ReadFile(file.get(), &dos, sizeof(dos), &read, nullptr) ||
			read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
			dos.e_lfanew <= 0 || dos.e_lfanew > 16 * 1024 * 1024)
		{
			return false;
		}
		LARGE_INTEGER offset{};
		offset.QuadPart = dos.e_lfanew;
		if (!::SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN))
			return false;
		DWORD signature = 0;
		IMAGE_FILE_HEADER header{};
		if (!::ReadFile(file.get(), &signature, sizeof(signature), &read, nullptr) ||
			read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
			!::ReadFile(file.get(), &header, sizeof(header), &read, nullptr) ||
			read != sizeof(header))
		{
			return false;
		}
		return header.Machine == IMAGE_FILE_MACHINE_I386 &&
			(header.Characteristics & IMAGE_FILE_DLL) != 0;
	}

	bool ValidateExtractedPackage(
		Context& context,
		const fs::path& packageRoot,
		const Manifest& manifest,
		std::string& error)
	{
		const fs::path packagedLoader = packageRoot / L"vSMR.dll";
		const fs::path packagedRuntime = packageRoot / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
		std::error_code filesystemError;
		if (!IsRegularFile(packagedLoader) || !IsRegularFile(packagedRuntime) ||
			fs::file_size(packagedLoader, filesystemError) != manifest.loaderSize || filesystemError)
		{
			error = "package_binaries_missing";
			return false;
		}
		if (context.options.testFeedDirectory.empty())
		{
			std::string metadataJson;
			if (!ReadText(
				packageRoot / L"vSMR_Data" / L"RELEASE-METADATA.json",
				metadataJson, 256 * 1024))
			{
				error = "package_metadata_missing";
				return false;
			}
			rapidjson::Document metadata;
			metadata.Parse<0>(metadataJson.c_str());
			if (metadata.HasParseError() || !metadata.IsObject() ||
				!JsonBool(metadata, "publishable", false) ||
				JsonString(metadata, "version") != manifest.version.normalized ||
				!metadata.HasMember("automatic_update") ||
				!metadata["automatic_update"].IsObject() ||
				!JsonBool(metadata["automatic_update"], "publishable", false))
			{
				error = "package_not_publishable";
				return false;
			}
		}
		std::string loaderHash;
		if (!verification::Sha256File(packagedLoader, loaderHash) ||
			ToLowerAscii(loaderHash) != manifest.loaderSha256)
		{
			error = "packaged_loader_hash_mismatch";
			return false;
		}
		if (!IsX86PortableExecutable(packagedLoader) || !IsX86PortableExecutable(packagedRuntime))
		{
			error = "package_architecture_invalid";
			return false;
		}
		const fs::path installer = packageRoot / L"vSMR_Data" / L"Tools" / L"install_vsmr.ps1";
		const fs::path restore = packageRoot / L"vSMR_Data" / L"Tools" / L"restore_vsmr_backup.ps1";
		if (!IsRegularFile(installer) || !IsRegularFile(restore))
		{
			error = "package_transaction_tools_missing";
			return false;
		}
		return true;
	}

	bool VerifyArchive(const fs::path& archive, const Manifest& manifest, std::string& error)
	{
		std::error_code filesystemError;
		const auto size = fs::file_size(archive, filesystemError);
		if (filesystemError || size != manifest.archiveSize)
		{
			error = "archive_size_mismatch";
			return false;
		}
		std::string digest;
		if (!verification::Sha256File(archive, digest) || ToLowerAscii(digest) != manifest.archiveSha256)
		{
			error = "archive_hash_mismatch";
			return false;
		}
		return true;
	}

	bool PrepareRemoteArchive(
		Context& context,
		const Release& release,
		const Manifest& manifest,
		const std::vector<std::uint8_t>& manifestBytes,
		fs::path& archivePath,
		std::string& error)
	{
		const ReleaseAsset* asset = FindAsset(release, manifest.archiveName);
		if (asset == nullptr)
		{
			error = "archive_asset_missing";
			return false;
		}
		const fs::path versionRoot = context.storageRoot / L"staging" /
			HashName(context.options.installRoot) / Utf8ToWide(manifest.version.normalized);
		std::error_code filesystemError;
		fs::create_directories(versionRoot, filesystemError);
		if (filesystemError)
		{
			error = "staging_directory";
			return false;
		}
		AtomicWrite(
			versionRoot / Utf8ToWide("vSMR-" + manifest.version.normalized + ".update.json"),
			manifestBytes.data(), manifestBytes.size());
		archivePath = versionRoot / Utf8ToWide(manifest.archiveName);
		if (IsRegularFile(archivePath) && VerifyArchive(archivePath, manifest, error))
			return true;
		const fs::path partial = archivePath.wstring() + L".part";
		if (IsRegularFile(partial))
		{
			std::error_code sizeError;
			if (fs::file_size(partial, sizeError) == manifest.archiveSize && !sizeError)
			{
				if (VerifyArchive(partial, manifest, error) &&
					::MoveFileExW(partial.c_str(), archivePath.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					return true;
				}
				::DeleteFileW(partial.c_str());
			}
		}
		const DWORD timeout = RemainingMs(context, kArchiveTimeoutMs);
		if (timeout < 1000)
		{
			error = "deadline";
			return false;
		}
		HttpResponse response = HttpGetTransport(
			context, asset->url, timeout, kMaximumArchiveBytes,
			{}, partial, manifest.archiveSize);
		if ((response.statusCode != 200 && response.statusCode != 206) || !response.error.empty())
		{
			error = response.error.empty() ? "archive_download_failed" : response.error;
			return false;
		}
		if (!VerifyArchive(partial, manifest, error) ||
			!::MoveFileExW(partial.c_str(), archivePath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			if (error.empty())
				error = "archive_commit_failed";
			return false;
		}
		return true;
	}

	StartupResult FailedOpen(
		Context& context,
		StartupResult result,
		const std::string& errorCode,
		const std::wstring& message,
		const std::string& stateStatus)
	{
		result.status = errorCode == "cancelled"
			? StartupStatus::Cancelled : StartupStatus::FailedOpen;
		result.errorCode = errorCode;
		result.message = message;
		result.selectedRuntimePath = fs::absolute(context.options.canonicalRuntimePath).lexically_normal();
		result.selectedVersion = context.options.currentVersion;
		context.state.status = stateStatus;
		context.state.errorCode = errorCode;
		context.state.error = WideToUtf8(message);
		context.state.message = WideToUtf8(message);
		context.state.downloadPercent = -1;
		if (context.state.nextCheckUtc.empty())
			context.state.nextCheckUtc = UtcAfterSeconds(5 * 60);
		PersistState(context);
		Report(context, ProgressStage::Fallback, -1, message);
		return result;
	}

	StartupResult IntegrityFailure(
		Context& context,
		StartupResult result,
		const std::string& errorCode,
		const std::wstring& message)
	{
		result.status = StartupStatus::FailedOpen;
		result.selectedRuntimePath.clear();
		result.errorCode = errorCode;
		result.message = message;
		context.state.status = "error";
		context.state.errorCode = errorCode;
		context.state.error = WideToUtf8(message);
		context.state.message = context.state.error;
		PersistState(context);
		Report(context, ProgressStage::Fallback, -1, message);
		return result;
	}

	bool NormalizeOptions(const StartupOptions& source, StartupOptions& normalized)
	{
		try
		{
			normalized = source;
			normalized.installRoot = fs::absolute(source.installRoot).lexically_normal();
			normalized.dataRoot = fs::absolute(source.dataRoot).lexically_normal();
			normalized.canonicalRuntimePath = fs::absolute(source.canonicalRuntimePath).lexically_normal();
			normalized.loaderPath = fs::absolute(source.loaderPath).lexically_normal();
			if (!source.testFeedDirectory.empty())
				normalized.testFeedDirectory = fs::absolute(source.testFeedDirectory).lexically_normal();
			if (!source.testStorageDirectory.empty())
				normalized.testStorageDirectory = fs::absolute(source.testStorageDirectory).lexically_normal();
		}
		catch (...)
		{
			return false;
		}
		return !normalized.installRoot.empty() &&
			!normalized.dataRoot.empty() &&
			!normalized.canonicalRuntimePath.empty() &&
			!normalized.loaderPath.empty() &&
			IsPathBelow(normalized.dataRoot, normalized.installRoot) &&
			IsPathBelow(normalized.canonicalRuntimePath, normalized.dataRoot) &&
			ParseSemVer(normalized.currentVersion).valid &&
			ParseSemVer(normalized.loaderVersion).valid &&
			(normalized.testStorageDirectory.empty() ||
				(!normalized.testFeedDirectory.empty() && normalized.allowUnsignedTestManifest));
	}

	void CleanupNormalUpdaterState(const Context& context) noexcept;

}
