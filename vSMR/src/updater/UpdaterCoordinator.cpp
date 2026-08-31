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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	using release_model::CompareSemVer;
	using release_model::ParseSemVer;
	using release_model::SameSemVerIdentity;
	using release_model::SemVer;

	StartupResult PrepareUpdateImpl(const StartupOptions& startupOptions)
	{
		StartupResult result;
		result.selectedRuntimePath = startupOptions.canonicalRuntimePath;
		result.selectedVersion = startupOptions.currentVersion;
		result.installationRoot = startupOptions.installRoot;
		Context context(startupOptions);
		result.updaterStoragePath = context.storageRoot;
		std::error_code filesystemError;
		fs::create_directories(context.storageRoot, filesystemError);
		if (filesystemError)
			return FailedOpen(context, result, "storage_unavailable", L"Updater storage is unavailable.");

		OwnedMutex updaterMutex = AcquireUpdaterMutex(startupOptions.installRoot);
		if (!updaterMutex)
		{
			result.status = StartupStatus::Deferred;
			result.message = L"Another vSMR updater is already running.";
			context.state.status = "deferred";
			context.state.message = WideToUtf8(result.message);
			context.state.nextCheckUtc = UtcAfterSeconds(60);
			PersistState(context);
			return result;
		}

		// A marker survives process termination during runtime construction. Never
		// retry that runtime against its newly installed data; restore the complete
		// previous data package first and quarantine the failed version.
		const fs::path pendingHealth = HealthMarkerPath(context.storageRoot, startupOptions.installRoot);
		if (IsRegularFile(pendingHealth))
		{
			StartupResult unhealthy;
			unhealthy.updaterStoragePath = context.storageRoot;
			std::string healthPhase;
			std::uint32_t attemptProcessId = 0;
			std::uint64_t attemptProcessCreated = 0;
			if (!ReadHealthMarker(
				startupOptions, pendingHealth, unhealthy, healthPhase,
				attemptProcessId, attemptProcessCreated))
			{
				return IntegrityFailure(
					context, result, "health_marker_invalid",
					L"The pending update health marker is invalid.");
			}

			if (healthPhase == "healthy")
			{
				// A successful initializer may have confirmed while another Prepare
				// held the global mutex. The marker is authoritative and cleanup can
				// now be completed under the mutex held by this call.
				::DeleteFileW(pendingHealth.c_str());
			}
			else
			{
				UniqueHandle installingRecoveryLock;
				if (healthPhase == "installing")
				{
					// The installer child inherits this same exclusive file lock. If an
					// orphan transaction is still running, never inspect/delete its
					// journal or race it with a rollback.
					installingRecoveryLock = AcquireExclusiveSessionLock(
						context.sessionLockStorageRoot, startupOptions.installRoot);
					if (!installingRecoveryLock)
					{
						return IntegrityFailure(
							context, result, "install_transaction_still_active",
							L"An update transaction is still active; vSMR will not load until it finishes.");
					}
					std::string activeVersion;
					std::string activeRuntimeHash;
					if (ReadReleaseVersion(startupOptions.dataRoot, activeVersion) &&
						SameSemVerIdentity(activeVersion, unhealthy.previousVersion) &&
						IsX86PortableExecutable(startupOptions.canonicalRuntimePath) &&
						verification::Sha256File(startupOptions.canonicalRuntimePath, activeRuntimeHash) &&
						ToLowerAscii(activeRuntimeHash) == ToLowerAscii(unhealthy.previousRuntimeSha256))
					{
						::DeleteFileW(pendingHealth.c_str());
						result.status = StartupStatus::FailedOpen;
						result.errorCode = "interrupted_update_was_not_committed";
						result.message = L"An interrupted update left the previous runtime intact.";
						context.state.status = "error";
						context.state.errorCode = result.errorCode;
						context.state.error = WideToUtf8(result.message);
						context.state.message = context.state.error;
						PersistState(context);
						return result;
					}
					unhealthy.rollbackBackupPath = FindNewestRollbackBackup(
						context.storageRoot, startupOptions.installRoot, unhealthy.selectedVersion);
					unhealthy.previousRuntimePath = unhealthy.rollbackBackupPath /
						L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
					if (unhealthy.rollbackBackupPath.empty() || !IsRegularFile(unhealthy.previousRuntimePath))
					{
						return IntegrityFailure(
							context, result, "interrupted_update_backup_missing",
							L"An interrupted update could not be matched to a complete rollback backup.");
					}
				}
				else if (healthPhase == "attempting")
				{
					bool attemptAlive = false;
					std::uint64_t liveCreation = 0;
					const bool attemptIdentityKnown = ProcessCreationStamp(
						attemptProcessId, liveCreation, attemptAlive);
					if (attemptIdentityKnown && attemptAlive && liveCreation == attemptProcessCreated)
					{
						unhealthy.status = StartupStatus::Updated;
						unhealthy.updateActivated = true;
						unhealthy.selectedRuntimePath = fs::absolute(startupOptions.canonicalRuntimePath).lexically_normal();
						unhealthy.availableVersion = unhealthy.selectedVersion;
						unhealthy.message = L"Another EuroScope process is validating this newly installed runtime.";
						return unhealthy;
					}
					if (!attemptIdentityKnown)
					{
						return IntegrityFailure(
							context, result, "health_attempt_owner_unknown",
							L"The updater could not safely determine whether another process is validating this runtime.");
					}
				}

				// Installing with a committed new tree, a dead/reused attempt owner,
				// or an explicit failed marker all require a complete data rollback.
				fs::path restored;
				std::string rollbackError;
				if (!RunRollback(
					context, unhealthy, restored, rollbackError,
					healthPhase == "installing" ? installingRecoveryLock.get() : nullptr,
					healthPhase == "installing" ? nullptr : "runtime_initialization_failed"))
				{
					return IntegrityFailure(
						context, result, rollbackError,
						L"The pending update could not be rolled back safely.");
				}
				result.status = StartupStatus::FailedOpen;
				result.selectedRuntimePath = restored;
				std::string restoredVersion;
				ReadReleaseVersion(startupOptions.dataRoot, restoredVersion);
				result.selectedVersion = restoredVersion;
				result.availableVersion = unhealthy.selectedVersion;
				result.errorCode = healthPhase == "installing"
					? "interrupted_update_rolled_back" : "unhealthy_update_rolled_back";
				result.message = healthPhase == "installing"
					? L"An interrupted update was rolled back before vSMR started."
					: L"A runtime that failed during initialization was rolled back.";
				context.state.status = "error";
				context.state.installedVersion = restoredVersion;
				context.state.availableVersion = unhealthy.selectedVersion;
				context.state.errorCode = result.errorCode;
				context.state.error = WideToUtf8(result.message);
				context.state.message = WideToUtf8(result.message);
				PersistState(context);
				return result;
			}
		}
		CleanupNormalUpdaterState(context);

		// Loading updater state and the next-startup action
		const State previousState = LoadPreviousState(context.statePath);
		context.state.lastCheckedUtc = previousState.lastCheckedUtc;
		context.state.nextCheckUtc = previousState.nextCheckUtc;
		context.state.lastActionRequestId = previousState.lastActionRequestId;
		const Action action = ConsumeAction(context.storageRoot);
		if (action.valid)
			context.state.lastActionRequestId = action.requestId;
		const bool forceAvisoReload = action.valid && action.action == "reload_aviso";
		const bool forceDiscovery = action.valid &&
			(action.action == "check_now" || action.action == "retry_update" || forceAvisoReload);
		const bool forceRetryInstall = action.valid && action.action == "retry_update";
		const bool forceExplicitInstall = forceRetryInstall || forceAvisoReload;
		if (forceRetryInstall && ParseSemVer(previousState.availableVersion).valid)
		{
			// An explicit retry is the only operation allowed to clear a runtime
			// quarantine. Discovery and all trust checks still run again below.
			::DeleteFileW((context.storageRoot / L"quarantine" /
				(Utf8ToWide(previousState.availableVersion) + L".json")).c_str());
		}
		const Config config = LoadConfig(context.storageRoot / L"config.json", startupOptions.defaultChannel);
		if ((!config.autoCheck && !forceDiscovery) ||
			(!forceDiscovery && IsFutureUtc(previousState.nextCheckUtc)))
		{
			context.state.status = "idle";
			context.state.message = !config.autoCheck
				? "Automatic update checks are disabled."
				: "The next update check is scheduled later.";
			context.state.error.clear();
			context.state.errorCode.clear();
			PersistState(context);
			return result;
		}

		// ----- Finding an eligible release -----
		context.state.status = "checking";
		context.state.error.clear();
		context.state.errorCode.clear();
		const wchar_t* checkingMessage = forceAvisoReload
			? L"Finding the installed vSMR release for AVISO reload..."
			: L"Checking for vSMR updates...";
		if (!Report(context, ProgressStage::Checking, -1, checkingMessage))
			return FailedOpen(context, result, "cancelled", L"Update check cancelled.", "idle");

		const SemVer installed = ParseSemVer(startupOptions.currentVersion);
		Manifest manifest;
		std::vector<std::uint8_t> manifestBytes;
		fs::path archivePath;
		std::string error;
		std::optional<Release> remoteRelease;
		std::optional<FixtureCandidate> fixture;
		if (!startupOptions.testFeedDirectory.empty())
		{
			fixture = SelectFixture(
				startupOptions, installed, config.channel,
				config.skippedVersion, context.storageRoot, forceAvisoReload, error);
			context.state.lastCheckedUtc = UtcNow();
			context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
			if (!fixture)
			{
				if (!error.empty())
					return FailedOpen(context, result, error, L"The local updater fixture could not be loaded.");
				if (forceAvisoReload)
				{
					return FailedOpen(
						context, result, "installed_release_not_found",
						L"The installed vSMR release is not available in the updater fixture; AVISOs were not changed.");
				}
				context.state.status = "up_to_date";
				context.state.message = "vSMR is up to date.";
				PersistState(context);
				return result;
			}
			manifest = fixture->manifest;
			manifestBytes = fixture->manifestBytes;
			archivePath = fixture->archivePath;
		}
		else
		{
			std::vector<Release> releases;
			if (!LoadRemoteReleases(context, releases, error))
			{
				context.state.lastCheckedUtc = UtcNow();
				return FailedOpen(
					context, result, error,
					error == "github_rate_limited"
						? L"GitHub rate-limited the updater; the installed runtime will be used."
						: L"The update check failed; the installed runtime will be used.",
					error == "github_rate_limited" ? "rate_limited" : "error");
			}
			context.state.lastCheckedUtc = UtcNow();
			context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
			remoteRelease = SelectRelease(
				releases, installed, config.channel,
				config.skippedVersion, context.storageRoot, forceAvisoReload);
			if (!remoteRelease)
			{
				if (forceAvisoReload)
				{
					return FailedOpen(
						context, result, "installed_release_not_found",
						L"The installed vSMR release is not available on GitHub; AVISOs were not changed.");
				}
				context.state.status = "up_to_date";
				context.state.message = "vSMR is up to date.";
				context.state.error.clear();
				context.state.errorCode.clear();
				PersistState(context);
				Report(context, ProgressStage::Complete, 100, L"vSMR is up to date.");
				return result;
			}
			result.availableVersion = remoteRelease->version.normalized;
			context.state.availableVersion = result.availableVersion;
			context.state.releaseUrl = remoteRelease->htmlUrl;
			const std::string trustedSigner = verification::ResolveTrustedSignerHash(startupOptions);
			if (trustedSigner.empty())
			{
				return FailedOpen(
					context, result, "signature_required",
					L"A signed updater loader or pinned release certificate is required before automatic updates can be installed.");
			}
			if (!LoadAndVerifyRemoteManifest(
				context, *remoteRelease, trustedSigner,
				manifest, manifestBytes, error))
			{
				return FailedOpen(context, result, error, L"The release manifest could not be authenticated.");
			}
		}

		result.availableVersion = manifest.version.normalized;
		if (manifest.runtimeAbi != startupOptions.expectedRuntimeAbi)
		{
			return FailedOpen(
				context, result, "runtime_abi_incompatible",
				L"The available runtime uses an unsupported loader ABI.");
		}
		context.state.availableVersion = result.availableVersion;
		context.state.selectedVersion = result.availableVersion;
		if (!config.autoDownload && !forceExplicitInstall)
		{
			result.status = StartupStatus::UpdateAvailable;
			result.message = L"A vSMR update is available; automatic download is disabled.";
			context.state.status = "idle";
			context.state.message = WideToUtf8(result.message);
			PersistState(context);
			return result;
		}

		// ----- Downloading and staging the release -----
		context.state.status = "downloading";
		const wchar_t* downloadMessage = forceAvisoReload
			? L"Downloading the signed release for AVISO reload..."
			: L"Downloading vSMR update...";
		if (!Report(context, ProgressStage::Downloading, 0, downloadMessage))
			return FailedOpen(context, result, "cancelled", L"Update download cancelled.", "idle");
		if (fixture)
		{
			if (!VerifyArchive(archivePath, manifest, error))
				return FailedOpen(context, result, error, L"The local fixture archive failed verification.");
		}
		else if (!PrepareRemoteArchive(
			context, *remoteRelease, manifest, manifestBytes, archivePath, error))
		{
			return FailedOpen(
				context, result, error,
				error == "deadline" || error == "timeout"
					? L"The update download will resume on the next launch."
					: L"The update archive could not be downloaded or verified.",
				error == "deadline" || error == "timeout" ? "deferred" : "error");
		}

		context.state.status = "verifying";
		if (!Report(context, ProgressStage::Verifying, -1, L"Verifying vSMR update..."))
			return FailedOpen(context, result, "cancelled", L"Update verification cancelled.", "idle");
		const std::string attemptId = SecureRandomHex(16);
		if (attemptId.empty())
			return FailedOpen(context, result, "staging_nonce_failed", L"Secure staging could not be created.");
		const fs::path packageRoot = context.storageRoot / L"staging" /
			HashName(startupOptions.installRoot) /
			Utf8ToWide("attempt-" + manifest.version.normalized + "-" + attemptId) /
			L"package";
		if (!SafelyExtractArchive(context, archivePath, packageRoot, error) ||
			!ValidateExtractedPackage(context, packageRoot, manifest, error) ||
			!StageRecoveryScript(context, packageRoot, manifest, error))
		{
			return FailedOpen(context, result, error, L"The downloaded vSMR package failed validation.");
		}
		std::string expectedRuntimeHash;
		if (!verification::Sha256File(
			packageRoot / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll",
			expectedRuntimeHash))
		{
			return FailedOpen(context, result, "runtime_hash_unavailable", L"The packaged runtime could not be hashed.");
		}

		const SemVer currentLoaderVersion = ParseSemVer(startupOptions.loaderVersion);
		const bool loaderTooOld = !currentLoaderVersion.valid ||
			CompareSemVer(currentLoaderVersion, manifest.minimumLoaderVersion) < 0;
		if (loaderTooOld)
		{
			result.status = StartupStatus::Deferred;
			result.loaderUpdateDeferred = true;
			result.errorCode = "manual_loader_update_required";
			result.message = L"This release requires a newer vSMR loader. Install the full package manually.";
			context.state.status = "deferred";
			context.state.loaderUpdateDeferred = true;
			context.state.restartRequired = false;
			context.state.errorCode = result.errorCode;
			context.state.message = WideToUtf8(result.message);
			PersistState(context);
			return result;
		}

		if (!config.autoInstall && !forceExplicitInstall)
		{
			result.status = StartupStatus::UpdateAvailable;
			result.message = L"A verified vSMR update is ready for the next startup.";
			context.state.status = "idle";
			context.state.message = WideToUtf8(result.message);
			PersistState(context);
			return result;
		}
		if (RemainingMs(context, startupOptions.overallDeadlineMs) == 0)
		{
			return FailedOpen(
				context, result, "deadline",
				L"The startup update deadline expired before installation; the installed runtime will be used.",
				"deferred");
		}

		// ----- Installing the verified release -----
		UniqueHandle sessionLock = AcquireExclusiveSessionLock(
			context.sessionLockStorageRoot, startupOptions.installRoot);
		if (!sessionLock)
		{
			result.status = StartupStatus::Deferred;
			result.message = L"Another EuroScope session is using this vSMR installation; the update remains staged.";
			context.state.status = "deferred";
			context.state.message = WideToUtf8(result.message);
			context.state.restartRequired = true;
			PersistState(context);
			return result;
		}

		context.state.status = "installing";
		Report(
			context, ProgressStage::Installing, -1,
			forceAvisoReload ? L"Reloading AVISO data..." : L"Installing vSMR update...");
		result.selectedVersion = manifest.version.normalized;
		result.availableVersion = manifest.version.normalized;
		result.previousRuntimePath = startupOptions.canonicalRuntimePath;
		if (!ReadReleaseVersion(startupOptions.dataRoot, result.previousVersion))
			result.previousVersion = startupOptions.currentVersion;
		if (!verification::Sha256File(startupOptions.canonicalRuntimePath, result.previousRuntimeSha256))
		{
			return FailedOpen(
				context, result, "current_runtime_hash_unavailable",
				L"The installed runtime could not be verified before updating.");
		}
		if (!WriteHealthMarker(startupOptions, context.storageRoot, result, "installing"))
		{
			return FailedOpen(
				context, result, "install_journal_unavailable",
				L"The updater could not create its durable installation journal.");
		}
		if (!RunInstaller(
			context, packageRoot, true, forceAvisoReload,
			!config.protectModifiedAviso, sessionLock.get(), error))
		{
			std::string activeVersion;
			std::string activeRuntimeHash;
			if (ReadReleaseVersion(startupOptions.dataRoot, activeVersion) &&
				SameSemVerIdentity(activeVersion, result.previousVersion) &&
				IsX86PortableExecutable(startupOptions.canonicalRuntimePath) &&
				verification::Sha256File(startupOptions.canonicalRuntimePath, activeRuntimeHash) &&
				ToLowerAscii(activeRuntimeHash) == ToLowerAscii(result.previousRuntimeSha256))
			{
				::DeleteFileW(result.healthMarkerPath.c_str());
				return FailedOpen(context, result, error, L"The update installation failed and was rolled back.");
			}
			result.selectedVersion = manifest.version.normalized;
			result.rollbackBackupPath = FindNewestRollbackBackup(
				context.storageRoot, startupOptions.installRoot, manifest.version.normalized);
			fs::path restored;
			std::string rollbackError;
			if (!result.rollbackBackupPath.empty() &&
				RunRollback(context, result, restored, rollbackError, sessionLock.get(), nullptr))
			{
				result.selectedRuntimePath = restored;
				return FailedOpen(context, result, error, L"The interrupted update was rolled back.");
			}
			return IntegrityFailure(
				context, result, "installer_state_unknown",
				L"The update transaction did not complete and a safe runtime could not be verified.");
		}

		// Verifying the installed runtime before activation
		std::string installedVersion;
		fs::path rollbackBackup;
		std::string installedRuntimeHash;
		const bool installationMetadataValid =
			ReadInstallationMetadata(startupOptions.dataRoot, installedVersion, rollbackBackup);
		if (!installationMetadataValid)
			rollbackBackup = FindNewestRollbackBackup(
				context.storageRoot, startupOptions.installRoot, manifest.version.normalized);
		if (!installationMetadataValid ||
			!SameSemVerIdentity(installedVersion, manifest.version.normalized) ||
			!IsRegularFile(startupOptions.canonicalRuntimePath) ||
			!IsX86PortableExecutable(startupOptions.canonicalRuntimePath) ||
			!verification::Sha256File(startupOptions.canonicalRuntimePath, installedRuntimeHash) ||
			ToLowerAscii(installedRuntimeHash) != ToLowerAscii(expectedRuntimeHash))
		{
			result.selectedVersion = manifest.version.normalized;
			result.rollbackBackupPath = rollbackBackup;
			fs::path restored;
			std::string rollbackError;
			if (!rollbackBackup.empty() && RunRollback(
				context, result, restored, rollbackError, sessionLock.get(),
				"post_install_verification_failed"))
			{
				result.selectedRuntimePath = restored;
				return FailedOpen(
					context, result, "post_install_verification_failed",
					L"The installed runtime failed verification; the previous version was restored.");
			}
			return IntegrityFailure(
				context, result, "post_install_rollback_failed",
				L"The installed runtime failed verification and the previous data could not be restored safely.");
		}

		result.status = StartupStatus::Updated;
		result.updateActivated = true;
		result.selectedRuntimePath = fs::absolute(startupOptions.canonicalRuntimePath).lexically_normal();
		result.selectedVersion = manifest.version.normalized;
		result.rollbackBackupPath = fs::absolute(rollbackBackup).lexically_normal();
		result.previousRuntimePath = result.rollbackBackupPath /
			L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
		result.message = forceAvisoReload
			? L"AVISO data was reloaded from the installed signed vSMR release."
			: L"vSMR was updated and will start with the new runtime.";
		if (!IsRegularFile(result.previousRuntimePath) ||
			!WriteHealthMarker(startupOptions, context.storageRoot, result))
		{
			fs::path restored;
			std::string rollbackError;
			if (RunRollback(context, result, restored, rollbackError, sessionLock.get(), nullptr))
			{
				result.selectedRuntimePath = restored;
				return FailedOpen(
					context, result, "rollback_safety_unavailable",
					L"The update could not establish its rollback marker; the previous version was restored.");
			}
			return IntegrityFailure(
				context, result, "rollback_safety_failure",
				L"The update could not establish or restore a safe runtime/data pair.");
		}

		context.state.status = "updated";
		context.state.installedVersion = result.selectedVersion;
		context.state.selectedVersion = result.selectedVersion;
		context.state.downloadPercent = 100;
		context.state.message = WideToUtf8(result.message);
		context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
		PersistState(context);
		Report(context, ProgressStage::Complete, 100, result.message);
		return result;
	}

	void PruneDirectoryHistory(
		const fs::path& root,
		const fs::path& alwaysKeep,
		std::size_t additionalToKeep) noexcept
	{
		try
		{
			struct Candidate
			{
				fs::path path;
				fs::file_time_type modified;
			};
			std::error_code error;
			if (!fs::is_directory(root, error) || error)
				return;
			std::vector<Candidate> candidates;
			for (fs::directory_iterator iterator(root, error), end;
				!error && iterator != end; iterator.increment(error))
			{
				if (!iterator->is_directory(error) || error ||
					(!alwaysKeep.empty() && iterator->path() == alwaysKeep))
				{
					error.clear();
					continue;
				}
				const auto modified = fs::last_write_time(iterator->path(), error);
				if (!error)
					candidates.push_back({ iterator->path(), modified });
				else
					error.clear();
			}
			std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
				return left.modified > right.modified;
			});
			for (std::size_t index = additionalToKeep; index < candidates.size(); ++index)
			{
				if (IsPathBelow(candidates[index].path, root))
					fs::remove_all(candidates[index].path, error);
				error.clear();
			}
		}
		catch (...)
		{
		}
	}

	std::uint64_t DirectoryBytes(const fs::path& root) noexcept
	{
		std::uint64_t total = 0;
		try
		{
			std::error_code error;
			for (fs::recursive_directory_iterator iterator(
				root, fs::directory_options::skip_permission_denied, error), end;
				!error && iterator != end; iterator.increment(error))
			{
				if (iterator->is_regular_file(error))
				{
					const auto size = iterator->file_size(error);
					if (!error && total <= (std::numeric_limits<std::uint64_t>::max)() - size)
						total += size;
				}
				error.clear();
			}
		}
		catch (...)
		{
		}
		return total;
	}

	void PruneDirectoryBudget(
		const fs::path& root,
		const fs::path& alwaysKeep,
		std::uint64_t budgetBytes) noexcept
	{
		try
		{
			struct Candidate
			{
				fs::path path;
				fs::file_time_type modified;
				std::uint64_t bytes = 0;
			};
			std::error_code error;
			std::vector<Candidate> candidates;
			std::uint64_t total = 0;
			for (fs::directory_iterator iterator(root, error), end;
				!error && iterator != end; iterator.increment(error))
			{
				if (!iterator->is_directory(error) || error)
				{
					error.clear();
					continue;
				}
				Candidate candidate;
				candidate.path = iterator->path();
				candidate.modified = fs::last_write_time(candidate.path, error);
				if (error)
				{
					error.clear();
					continue;
				}
				candidate.bytes = DirectoryBytes(candidate.path);
				total += candidate.bytes;
				candidates.push_back(std::move(candidate));
			}
			std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
				return left.modified < right.modified;
			});
			for (const auto& candidate : candidates)
			{
				if (total <= budgetBytes)
					break;
				if (!alwaysKeep.empty() && candidate.path == alwaysKeep)
					continue;
				if (IsPathBelow(candidate.path, root))
				{
					fs::remove_all(candidate.path, error);
					if (!error)
						total = total >= candidate.bytes ? total - candidate.bytes : 0;
					error.clear();
				}
			}
		}
		catch (...)
		{
		}
	}

	void CleanupNormalUpdaterState(const Context& context) noexcept
	{
		const std::wstring installKey = HashName(context.options.installRoot);
		const fs::path staging = context.storageRoot / L"staging" / installKey;
		const fs::path recovery = context.storageRoot / L"recovery" / installKey;
		const fs::path backups = context.storageRoot / L"backups" / installKey;
		PruneDirectoryHistory(staging, {}, 2);
		PruneDirectoryHistory(recovery, {}, 3);
		PruneDirectoryHistory(backups, {}, 4);
		PruneDirectoryBudget(staging, {}, 768ULL * 1024ULL * 1024ULL);
		PruneDirectoryBudget(recovery, {}, 8ULL * 1024ULL * 1024ULL);
		PruneDirectoryBudget(backups, {}, 1536ULL * 1024ULL * 1024ULL);
	}

	void CleanupAfterHealthyRuntime(const StartupResult& update) noexcept
	{
		try
		{
			const fs::path storage = update.updaterStoragePath.empty()
				? GetUpdaterStorageDirectory() : update.updaterStoragePath;
			if (storage.empty() || update.rollbackBackupPath.empty() ||
				!IsPathBelow(update.rollbackBackupPath, storage / L"backups"))
			{
				return;
			}
			const fs::path backupInstallRoot = update.rollbackBackupPath.parent_path();
			const std::wstring installKey = backupInstallRoot.filename().wstring();
			PruneDirectoryHistory(backupInstallRoot, update.rollbackBackupPath, 2);
			PruneDirectoryHistory(storage / L"staging" / installKey, {}, 2);
			PruneDirectoryHistory(storage / L"recovery" / installKey, {}, 3);
			std::error_code error;
			const fs::path staging = storage / L"staging" / installKey;
			for (fs::recursive_directory_iterator iterator(
				staging, fs::directory_options::skip_permission_denied, error), end;
				!error && iterator != end; iterator.increment(error))
			{
				if (iterator->is_regular_file(error) && iterator->path().extension() == L".part")
					fs::remove(iterator->path(), error);
				error.clear();
			}
		}
		catch (...)
		{
		}
	}
}
