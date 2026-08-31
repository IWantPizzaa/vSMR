#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"

#include <exception>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace vsmr::updater
{
	using namespace internal;

	fs::path GetUpdaterStorageDirectory() noexcept
	{
		try
		{
			// Durable config, state, staging, and health journals must have one
			// deterministic identity. Never switch an existing LocalAppData journal
			// to Temp merely because LocalAppData is temporarily read-only: doing so
			// could hide an unresolved transaction. Write failures are handled by the
			// caller; Temp is reserved for the stable cross-process session lease.
			return LocalAppDataUpdaterCandidate();
		}
		catch (...)
		{
		}
		return {};
	}

	fs::path GetInstallationSessionLockPath(const fs::path& installRoot) noexcept
	{
		try
		{
			const fs::path storage = GetProductionSessionLockStorageRoot();
			if (storage.empty())
				return {};
			return SessionLockPath(storage, installRoot);
		}
		catch (...)
		{
			return {};
		}
	}

	StartupResult PrepareUpdateBeforeRuntimeLoad(const StartupOptions& options) noexcept
	{
		StartupResult result;
		StartupOptions normalized;
		bool optionsNormalized = false;
		try
		{
			if (!NormalizeOptions(options, normalized))
			{
				result.status = StartupStatus::FailedOpen;
				result.selectedRuntimePath = options.canonicalRuntimePath;
				result.selectedVersion = options.currentVersion;
				result.errorCode = "invalid_startup_options";
				result.message = L"The updater received invalid installation paths or versions.";
				return result;
			}
			optionsNormalized = true;
			return PrepareUpdateImpl(normalized);
		}
		catch (const std::exception& exception)
		{
			result.status = StartupStatus::FailedOpen;
			bool transactionPending = false;
			try
			{
				if (optionsNormalized)
				{
					result.updaterStoragePath = normalized.testStorageDirectory.empty()
						? GetUpdaterStorageDirectory() : normalized.testStorageDirectory;
					transactionPending = !result.updaterStoragePath.empty() && IsRegularFile(
						HealthMarkerPath(result.updaterStoragePath, normalized.installRoot));
				}
			}
			catch (...)
			{
				transactionPending = optionsNormalized;
			}
			result.selectedRuntimePath = transactionPending
				? fs::path{} : options.canonicalRuntimePath;
			result.selectedVersion = options.currentVersion;
			result.errorCode = transactionPending
				? "unexpected_exception_with_pending_transaction" : "unexpected_exception";
			result.message = transactionPending
				? L"The updater failed while a package transaction was pending; no runtime will be loaded until recovery succeeds."
				: L"The updater failed unexpectedly: " + Utf8ToWide(exception.what());
			return result;
		}
		catch (...)
		{
			result.status = StartupStatus::FailedOpen;
			bool transactionPending = false;
			try
			{
				if (optionsNormalized)
				{
					result.updaterStoragePath = normalized.testStorageDirectory.empty()
						? GetUpdaterStorageDirectory() : normalized.testStorageDirectory;
					transactionPending = !result.updaterStoragePath.empty() && IsRegularFile(
						HealthMarkerPath(result.updaterStoragePath, normalized.installRoot));
				}
			}
			catch (...)
			{
				transactionPending = optionsNormalized;
			}
			result.selectedRuntimePath = transactionPending
				? fs::path{} : options.canonicalRuntimePath;
			result.selectedVersion = options.currentVersion;
			result.errorCode = transactionPending
				? "unexpected_exception_with_pending_transaction" : "unexpected_exception";
			result.message = transactionPending
				? L"The updater failed while a package transaction was pending; no runtime will be loaded until recovery succeeds."
				: L"The updater failed unexpectedly.";
			return result;
		}
	}

	bool RollbackPreparedUpdate(
		const StartupOptions& options,
		const StartupResult& update,
		fs::path* restoredRuntimePath,
		std::wstring* errorMessage) noexcept
	{
		try
		{
			if (!MarkRuntimeUnhealthy(update))
				throw std::runtime_error("runtime health marker could not be marked failed");
			StartupOptions normalized;
			if (!NormalizeOptions(options, normalized))
				throw std::runtime_error("invalid updater options");
			Context context(normalized);
			OwnedMutex updaterMutex = AcquireUpdaterMutex(normalized.installRoot);
			if (!updaterMutex)
				throw std::runtime_error("another updater is active");
			fs::path restored;
			std::string error;
			if (!RunRollback(
				context, update, restored, error, nullptr,
				"runtime_initialization_failed"))
				throw std::runtime_error(error);
			if (restoredRuntimePath != nullptr)
				*restoredRuntimePath = restored;
			std::string restoredVersion;
			ReadReleaseVersion(normalized.dataRoot, restoredVersion);
			context.state.status = "error";
			context.state.installedVersion = restoredVersion;
			context.state.availableVersion = update.selectedVersion;
			context.state.errorCode = "runtime_initialization_failed";
			context.state.error = "The new runtime failed initialization and was rolled back.";
			context.state.message = context.state.error;
			PersistState(context);
			return true;
		}
		catch (const std::exception& exception)
		{
			if (errorMessage != nullptr)
				*errorMessage = Utf8ToWide(exception.what());
			return false;
		}
		catch (...)
		{
			if (errorMessage != nullptr)
				*errorMessage = L"The update rollback failed unexpectedly.";
			return false;
		}
	}

	bool MarkRuntimeUnhealthy(const StartupResult& update) noexcept
	{
		try
		{
			if (!update.updateActivated || update.healthMarkerPath.empty())
				return false;
			OwnedMutex markerMutex = AcquireHealthMarkerMutex(update.healthMarkerPath);
			if (!markerMutex)
				return false;
			return RewriteHealthMarkerPhase(
				update, { "attempting", "failed" }, "failed");
		}
		catch (...)
		{
			return false;
		}
	}

	bool ConfirmRuntimeHealthy(const StartupResult& update) noexcept
	{
		try
		{
			if (!update.updateActivated || update.healthMarkerPath.empty())
				return true;
			const fs::path storage = update.updaterStoragePath.empty()
				? GetUpdaterStorageDirectory() : update.updaterStoragePath;
			if (storage.empty() || !IsPathBelow(update.healthMarkerPath, storage / L"health"))
				return false;
			OwnedMutex markerMutex = AcquireHealthMarkerMutex(update.healthMarkerPath);
			if (!markerMutex)
				return false;
			if (!IsRegularFile(update.healthMarkerPath))
				return true;
			if (!RewriteHealthMarkerPhase(
				update, { "attempting", "failed", "healthy" }, "healthy"))
			{
				return false;
			}
			// Cleanup touches staging shared by every installation. Only mutate it
			// while holding the same global mutex as Prepare. A durable `healthy`
			// marker lets cleanup be deferred without causing a future rollback.
			OwnedMutex updaterMutex = AcquireUpdaterMutex({});
			if (updaterMutex)
			{
				::DeleteFileW(update.healthMarkerPath.c_str());
				CleanupAfterHealthyRuntime(update);
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}
