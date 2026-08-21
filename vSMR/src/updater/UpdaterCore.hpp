#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vsmr::updater
{
	enum class UpdateChannel
	{
		Stable,
		Beta
	};

	enum class ProgressStage
	{
		Idle,
		Checking,
		Downloading,
		Verifying,
		Installing,
		Complete,
		Fallback
	};

	struct Progress
	{
		ProgressStage stage = ProgressStage::Idle;
		int percent = -1;
		std::wstring message;
	};

	struct StartupOptions
	{
		std::filesystem::path installRoot;
		std::filesystem::path dataRoot;
		std::filesystem::path canonicalRuntimePath;
		std::filesystem::path loaderPath;
		std::string currentVersion;
		std::string loaderVersion;
		UpdateChannel defaultChannel = UpdateChannel::Beta;
		std::uint32_t hostProcessId = 0;
		std::uint32_t expectedRuntimeAbi = 1;

		// The deadline covers release discovery, download, and verification. Once
		// the transactional installer starts it is deliberately non-cancellable.
		std::uint32_t overallDeadlineMs = 30000;
		std::function<bool(const Progress&)> progressCallback;

		// Test-only injection seam. Production callers must leave these unset.
		// A fixture directory contains one or more vSMR-*.update.json manifests
		// and the archives named by them.
		std::filesystem::path testFeedDirectory;
		std::filesystem::path testStorageDirectory;
		bool allowUnsignedTestManifest = false;
	};

	enum class StartupStatus
	{
		Current,
		Updated,
		UpdateAvailable,
		Deferred,
		Cancelled,
		FailedOpen
	};

	struct StartupResult
	{
		StartupStatus status = StartupStatus::Current;
		std::filesystem::path selectedRuntimePath;
		std::filesystem::path previousRuntimePath;
		std::filesystem::path rollbackBackupPath;
		std::filesystem::path healthMarkerPath;
		std::filesystem::path updaterStoragePath;
		std::filesystem::path installationRoot;
		std::string selectedVersion;
		std::string previousVersion;
		std::string previousRuntimeSha256;
		std::string availableVersion;
		std::string errorCode;
		std::wstring message;
		bool updateActivated = false;
		bool loaderUpdateDeferred = false;
	};

	// Checks, downloads, verifies, and activates a compatible runtime before the
	// loader creates the real EuroScope plug-in. Ordinary failures before a data
	// transaction fail open to the existing canonical runtime. An inconsistent
	// durable transaction/health journal deliberately returns an empty runtime so
	// the loader fails closed instead of combining unverified runtime and data.
	StartupResult PrepareUpdateBeforeRuntimeLoad(const StartupOptions& options) noexcept;
	bool RollbackPreparedUpdate(
		const StartupOptions& options,
		const StartupResult& update,
		std::filesystem::path* restoredRuntimePath,
		std::wstring* errorMessage = nullptr) noexcept;
	// Call immediately when runtime construction fails, before the loader drops
	// its shared installation lock. This prevents another process from treating
	// the still-live attempt owner as evidence that the runtime is healthy.
	bool MarkRuntimeUnhealthy(const StartupResult& update) noexcept;
	bool ConfirmRuntimeHealthy(const StartupResult& update) noexcept;

	std::filesystem::path GetUpdaterStorageDirectory() noexcept;
	std::filesystem::path GetInstallationSessionLockPath(
		const std::filesystem::path& installRoot) noexcept;
}
