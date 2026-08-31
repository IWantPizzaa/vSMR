#pragma once

#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "updater/UpdaterTransport.hpp"

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
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vsmr::updater::internal
{
	inline constexpr wchar_t kApiUrl[] =
		L"https://api.github.com/repos/IWantPizzaa/vSMR/releases?per_page=30";
	inline constexpr std::uint64_t kMaximumMetadataBytes = 2ULL * 1024ULL * 1024ULL;
	inline constexpr std::uint64_t kMaximumManifestBytes = 64ULL * 1024ULL;
	inline constexpr std::uint64_t kMaximumSignatureBytes = 256ULL * 1024ULL;
	inline constexpr std::uint64_t kMaximumArchiveBytes = 256ULL * 1024ULL * 1024ULL;
	inline constexpr std::uint64_t kMinimumCheckIntervalSeconds = 15ULL * 60ULL;
	inline constexpr DWORD kMetadataTimeoutMs = 5000;
	inline constexpr DWORD kAssetMetadataTimeoutMs = 4000;
	inline constexpr DWORD kArchiveTimeoutMs = 75000;

	inline constexpr DWORD ClassifyProcessFailureExitCode(
		bool timedOut,
		DWORD windowsError) noexcept
	{
		return timedOut
			? ERROR_TIMEOUT
			: (windowsError != ERROR_SUCCESS ? windowsError : ERROR_GEN_FAILURE);
	}

	class UniqueHandle
	{
	public:
		UniqueHandle() = default;
		explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
		~UniqueHandle() { reset(); }
		UniqueHandle(const UniqueHandle&) = delete;
		UniqueHandle& operator=(const UniqueHandle&) = delete;
		UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
		UniqueHandle& operator=(UniqueHandle&& other) noexcept
		{
			if (this != &other)
				reset(other.release());
			return *this;
		}
		HANDLE get() const noexcept { return value_; }
		explicit operator bool() const noexcept
		{
			return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
		}
		HANDLE release() noexcept
		{
			HANDLE value = value_;
			value_ = nullptr;
			return value;
		}
		void reset(HANDLE value = nullptr) noexcept
		{
			if (*this)
				::CloseHandle(value_);
			value_ = value;
		}

	private:
		HANDLE value_ = nullptr;
	};

	class OwnedMutex
	{
	public:
		OwnedMutex() = default;
		explicit OwnedMutex(UniqueHandle handle) noexcept
			: handle_(std::move(handle)), owned_(true) {}
		~OwnedMutex()
		{
			if (owned_ && handle_)
				::ReleaseMutex(handle_.get());
		}
		OwnedMutex(const OwnedMutex&) = delete;
		OwnedMutex& operator=(const OwnedMutex&) = delete;
		OwnedMutex(OwnedMutex&& other) noexcept
			: handle_(std::move(other.handle_)), owned_(other.owned_)
		{
			other.owned_ = false;
		}
		explicit operator bool() const noexcept
		{
			return owned_ && static_cast<bool>(handle_);
		}

	private:
		UniqueHandle handle_;
		bool owned_ = false;
	};

	struct Config
	{
		bool autoCheck = true;
		bool autoDownload = true;
		bool autoInstall = true;
		bool protectModifiedAviso = true;
		UpdateChannel channel = UpdateChannel::Beta;
		std::string skippedVersion;
	};

	struct State
	{
		std::string status = "idle";
		std::string installedVersion;
		std::string selectedVersion;
		std::string availableVersion;
		int downloadPercent = -1;
		std::string lastCheckedUtc;
		std::string nextCheckUtc;
		std::string message;
		std::string errorCode;
		std::string error;
		bool restartRequired = false;
		bool loaderUpdateDeferred = false;
		std::string releaseUrl;
		std::string lastActionRequestId;
	};

	struct Action
	{
		std::string requestId;
		std::string action;
		bool valid = false;
	};

	struct ReleaseAsset
	{
		std::string name;
		std::wstring url;
		std::uint64_t size = 0;
		std::string digest;
	};

	struct Release
	{
		release_model::SemVer version;
		std::string htmlUrl;
		std::vector<ReleaseAsset> assets;
	};

	struct Manifest
	{
		release_model::SemVer version;
		std::string channel;
		std::string archiveName;
		std::uint64_t archiveSize = 0;
		std::string archiveSha256;
		release_model::SemVer minimumLoaderVersion;
		std::string runtimeRelativePath;
		std::string loaderName;
		std::string loaderVersion;
		std::uint64_t loaderSize = 0;
		std::string loaderSha256;
		bool publishable = false;
		std::uint32_t runtimeAbi = 0;
	};

	using HttpResponse = transport::Response;

	std::filesystem::path GetProductionSessionLockStorageRoot() noexcept;

	struct Context
	{
		const StartupOptions& options;
		std::filesystem::path storageRoot;
		std::filesystem::path sessionLockStorageRoot;
		std::filesystem::path statePath;
		State state;
		ULONGLONG startedTick = ::GetTickCount64();
		bool cancelled = false;

		explicit Context(const StartupOptions& startupOptions)
			: options(startupOptions),
			storageRoot(startupOptions.testStorageDirectory.empty()
				? GetUpdaterStorageDirectory() : startupOptions.testStorageDirectory),
			sessionLockStorageRoot(startupOptions.testStorageDirectory.empty()
				? GetProductionSessionLockStorageRoot() : startupOptions.testStorageDirectory),
			statePath(storageRoot / L"state.json")
		{
			state.installedVersion = startupOptions.currentVersion;
		}
	};

	struct FixtureCandidate
	{
		Manifest manifest;
		std::filesystem::path manifestPath;
		std::filesystem::path archivePath;
		std::vector<std::uint8_t> manifestBytes;
	};

	std::string WideToUtf8(const std::wstring& value);
	std::wstring Utf8ToWide(const std::string& value);
	std::string ToLowerAscii(std::string value);
	std::wstring ToLowerWide(std::wstring value);
	bool IsHex(const std::string& value, std::size_t length);
	std::string Hex(const BYTE* bytes, DWORD size);
	std::string SecureRandomHex(DWORD byteCount);
	bool ProbeWritableDirectory(const std::filesystem::path& directory) noexcept;
	std::filesystem::path LocalAppDataUpdaterCandidate() noexcept;
	std::filesystem::path TemporaryUpdaterCandidate() noexcept;
	std::string UtcNow();
	std::string UtcAfterSeconds(std::uint64_t seconds);
	std::uint64_t Fnv1a64(const std::wstring& value);
	std::wstring HashName(const std::filesystem::path& installRoot);
	std::filesystem::path SessionLockPath(
		const std::filesystem::path& storageRoot,
		const std::filesystem::path& installRoot);
	bool ReadBytes(
		const std::filesystem::path& path,
		std::vector<std::uint8_t>& output,
		std::uint64_t maximum);
	bool ReadText(
		const std::filesystem::path& path,
		std::string& output,
		std::uint64_t maximum);
	bool AtomicWrite(
		const std::filesystem::path& path,
		const void* data,
		std::size_t size);
	bool AtomicWriteText(
		const std::filesystem::path& path,
		const std::string& text);
	bool IsRegularFile(const std::filesystem::path& path);
	std::string JsonString(const rapidjson::Value& object, const char* name);
	bool JsonBool(const rapidjson::Value& object, const char* name, bool fallback);
	std::uint64_t JsonUint64(const rapidjson::Value& object, const char* name);
	void AddJsonString(
		rapidjson::Document& document,
		rapidjson::Value& object,
		const char* name,
		const std::string& value);
	std::string SerializeState(const State& state);
	void PersistState(Context& context);
	bool Report(
		Context& context,
		ProgressStage stage,
		int percent,
		const std::wstring& message);
	DWORD RemainingMs(const Context& context, DWORD operationMaximum);
	Config LoadConfig(const std::filesystem::path& path, UpdateChannel defaultChannel);
	Action ConsumeAction(const std::filesystem::path& storageRoot);
	bool ParseUtcFileTime(const std::string& text, FILETIME& result);
	bool IsFutureUtc(const std::string& text);
	State LoadPreviousState(const std::filesystem::path& path);
	bool EndsWithNoCase(const std::wstring& value, const std::wstring& suffix);
	HttpResponse HttpGetTransport(
		Context& context,
		const std::wstring& initialUrl,
		DWORD timeoutMs,
		std::uint64_t maximumBytes,
		const std::string& ifNoneMatch = {},
		const std::filesystem::path& outputFile = {},
		std::uint64_t expectedSize = 0);
	const ReleaseAsset* FindAsset(const Release& release, const std::string& name);
	std::vector<Release> ParseReleases(const std::vector<std::uint8_t>& bytes);
	std::optional<Release> SelectRelease(
		const std::vector<Release>& releases,
		const release_model::SemVer& installed,
		UpdateChannel channel,
		const std::string& skippedVersion,
		const std::filesystem::path& storageRoot,
		bool selectInstalledVersion);
	bool LoadRemoteReleases(
		Context& context,
		std::vector<Release>& releases,
		std::string& error);
	bool ParseManifest(
		const std::vector<std::uint8_t>& bytes,
		Manifest& manifest,
		std::string& error);
	bool ValidateManifestForRelease(
		const Manifest& manifest,
		const Release& release,
		const ReleaseAsset& archiveAsset,
		std::string& error);
	bool LoadAndVerifyRemoteManifest(
		Context& context,
		const Release& release,
		const std::string& trustedSigner,
		Manifest& manifest,
		std::vector<std::uint8_t>& manifestBytes,
		std::string& error);
	std::optional<FixtureCandidate> SelectFixture(
		const StartupOptions& options,
		const release_model::SemVer& installed,
		UpdateChannel channel,
		const std::string& skippedVersion,
		const std::filesystem::path& storageRoot,
		bool selectInstalledVersion,
		std::string& error);
	std::wstring QuoteCommandLineArgument(const std::wstring& argument);
	std::filesystem::path PowerShellPath();
	bool RunProcess(
		const std::filesystem::path& executable,
		const std::vector<std::wstring>& arguments,
		DWORD timeoutMs,
		DWORD& exitCode,
		bool terminateOnTimeout,
		const std::function<void()>& pulse = {},
		const std::vector<HANDLE>& handlesToInherit = {});
	bool IsPathBelow(
		const std::filesystem::path& child,
		const std::filesystem::path& parent);
	bool SafelyExtractArchive(
		Context& context,
		const std::filesystem::path& archive,
		const std::filesystem::path& destination,
		std::string& error);
	OwnedMutex AcquireUpdaterMutex(const std::filesystem::path& installRoot);
	OwnedMutex AcquireHealthMarkerMutex(const std::filesystem::path& markerPath);
	UniqueHandle AcquireExclusiveSessionLock(
		const std::filesystem::path& storageRoot,
		const std::filesystem::path& installRoot);
	bool CopyFileAtomically(
		const std::filesystem::path& source,
		const std::filesystem::path& destination);
	bool ReadInstallationMetadata(
		const std::filesystem::path& dataRoot,
		std::string& installedVersion,
		std::filesystem::path& rollbackBackup);
	std::filesystem::path FindNewestRollbackBackup(
		const std::filesystem::path& storageRoot,
		const std::filesystem::path& installRoot,
		const std::string& installingVersion);
	bool ReadReleaseVersion(
		const std::filesystem::path& dataRoot,
		std::string& version);
	std::filesystem::path RecoveryScriptPath(
		const std::filesystem::path& storageRoot,
		const std::filesystem::path& installRoot,
		const std::string& version);
	bool StageRecoveryScript(
		Context& context,
		const std::filesystem::path& packageRoot,
		const Manifest& manifest,
		std::string& error);
	bool RunInstaller(
		Context& context,
		const std::filesystem::path& packageRoot,
		bool preserveLoader,
		bool reloadAviso,
		bool replaceModifiedAviso,
		HANDLE installationSessionLock,
		std::string& error);
	std::filesystem::path HealthMarkerPath(
		const std::filesystem::path& storageRoot,
		const std::filesystem::path& installRoot);
	bool ProcessCreationStamp(
		DWORD processId,
		std::uint64_t& stamp,
		bool& alive);
	bool WriteHealthMarker(
		const StartupOptions& options,
		const std::filesystem::path& storageRoot,
		StartupResult& result,
		const std::string& phase = "attempting");
	bool ReadHealthMarker(
		const StartupOptions& options,
		const std::filesystem::path& markerPath,
		StartupResult& result,
		std::string& phase,
		std::uint32_t& attemptProcessId,
		std::uint64_t& attemptProcessCreated);
	bool RewriteHealthMarkerPhase(
		const StartupResult& update,
		const std::set<std::string>& allowedCurrentPhases,
		const char* newPhase);
	bool WriteQuarantineMarker(
		const std::filesystem::path& storageRoot,
		const std::string& version,
		const std::string& reason);
	bool RunRollback(
		Context& context,
		const StartupResult& update,
		std::filesystem::path& restoredRuntimePath,
		std::string& error,
		HANDLE existingSessionLock = nullptr,
		const char* quarantineReason = nullptr);
	bool IsX86PortableExecutable(const std::filesystem::path& path);
	bool ValidateExtractedPackage(
		Context& context,
		const std::filesystem::path& packageRoot,
		const Manifest& manifest,
		std::string& error);
	bool VerifyArchive(
		const std::filesystem::path& archive,
		const Manifest& manifest,
		std::string& error);
	bool PrepareRemoteArchive(
		Context& context,
		const Release& release,
		const Manifest& manifest,
		const std::vector<std::uint8_t>& manifestBytes,
		std::filesystem::path& archivePath,
		std::string& error);
	StartupResult FailedOpen(
		Context& context,
		StartupResult result,
		const std::string& errorCode,
		const std::wstring& message,
		const std::string& stateStatus = "error");
	StartupResult IntegrityFailure(
		Context& context,
		StartupResult result,
		const std::string& errorCode,
		const std::wstring& message);
	bool NormalizeOptions(const StartupOptions& source, StartupOptions& normalized);
	StartupResult PrepareUpdateImpl(const StartupOptions& startupOptions);
	void PruneDirectoryHistory(
		const std::filesystem::path& root,
		const std::filesystem::path& alwaysKeep,
		std::size_t additionalToKeep) noexcept;
	std::uint64_t DirectoryBytes(const std::filesystem::path& root) noexcept;
	void PruneDirectoryBudget(
		const std::filesystem::path& root,
		const std::filesystem::path& alwaysKeep,
		std::uint64_t budgetBytes) noexcept;
	void CleanupNormalUpdaterState(const Context& context) noexcept;
	void CleanupAfterHealthyRuntime(const StartupResult& update) noexcept;
}
