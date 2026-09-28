#include "updater/FileUpdater.hpp"
#include "updater/FileUpdateTransaction.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterVerification.hpp"
#include "shared/JsonDocument.hpp"

#include <algorithm>
#include <array>

namespace fs = std::filesystem;
namespace vsmr::updater::files
{
    namespace
    {
        constexpr wchar_t FeedPrefix[] = L"https://raw.githubusercontent.com/IWantPizzaa/vSMR/";
        constexpr wchar_t HelperRelative[] = L"vSMR_Data/Tools/vSMR.ApplyUpdate.exe";

        bool Exists(const fs::path& path)
        {
            std::error_code error;
            return fs::exists(path, error) && !error;
        }

        bool MigrationReady(const StartupOptions& options)
        {
            std::string json;
            rapidjson::Document config;
            if (!ReadText(options.dataRoot / L"config.json", json, 16ULL * 1024 * 1024)) return false;
            VsmrJson::ParseDocument(config, json);
            if (config.HasParseError() || !config.IsObject() || !config.HasMember("_migration") || !config["_migration"].IsObject()) return false;
            const auto& migration = config["_migration"];
            return internal::JsonBool(migration, "legacy_profiles", false) && internal::JsonBool(migration, "legacy_aviso", false);
        }

        void CaptureGeneration(StartupResult& result, const StartupOptions& options)
        {
            result.fileProtocol = true;
            if (!verification::Sha256File(options.canonicalRuntimePath, result.selectedRuntimeSha256)) result.selectedRuntimeSha256.clear();
            const auto manifest = options.dataRoot / L"version.json";
            result.selectedManifestSha256 = "missing";
            if (Exists(manifest) && !verification::Sha256File(manifest, result.selectedManifestSha256)) result.selectedManifestSha256.clear();
        }

        StartupResult Result(internal::Context& context, StartupStatus status, const std::string& code,
            const std::wstring& message, bool safe = true)
        {
            StartupResult result;
            result.status = status;
            result.selectedRuntimePath = safe ? context.options.canonicalRuntimePath : fs::path{};
            result.selectedVersion = context.options.currentVersion;
            result.installationRoot = context.options.installRoot;
            result.errorCode = code;
            result.message = message;
            result.availableVersion = context.state.availableVersion;
            CaptureGeneration(result, context.options);
            context.state.status = status == StartupStatus::Deferred ? "deferred" : status == StartupStatus::FailedOpen ? "error" : "idle";
            context.state.errorCode = code;
            context.state.error = status == StartupStatus::FailedOpen ? internal::WideToUtf8(message) : "";
            context.state.message = internal::WideToUtf8(message);
            context.state.restartRequired = status == StartupStatus::Deferred;
            internal::PersistState(context);
            return result;
        }

        bool LoaderChanged(const Transaction& tx)
        {
            return std::any_of(tx.changes.begin(), tx.changes.end(), [](const Change& change) {
                return internal::ToLowerAscii(change.file.path) == "vsmr.dll";
            });
        }

        bool EnoughDiskSpace(const Transaction& tx)
        {
            ULARGE_INTEGER available{};
            if (!::GetDiskFreeSpaceExW(tx.root.c_str(), &available, nullptr, nullptr)) return false;
            std::uint64_t required = 4ULL * 1024 * 1024;
            for (const auto& change : tx.changes)
            {
                required += change.file.size * 2; // staged file plus replacement temporary
                std::error_code ec;
                const auto size = fs::file_size(tx.root / fs::u8path(change.file.path), ec);
                if (!ec) required += size; // rollback backup
            }
            return available.QuadPart >= required;
        }

        std::string BinaryFileVersion(const fs::path& path)
        {
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
            if (size == 0 || size > 1024 * 1024) return {};
            std::vector<BYTE> bytes(size);
            if (!::GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!::VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return {};
            return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." + std::to_string(HIWORD(info->dwFileVersionLS));
        }

        bool StartHelper(const StartupOptions& options)
        {
            // Run a COPY of the currently installed trusted helper, never the
            // incoming executable. Its canonical file may itself be updated.
            const auto source = options.installRoot / HelperRelative;
            const auto destination = UpdateRoot(options.installRoot) / L"apply-pending.exe";
            if (!SafePath(options.installRoot, source) || !SafePath(options.installRoot, destination) || !Exists(source)) return false;
            if (!CopyAtomically(source, destination))
            {
                // Another helper may already hold this exact executable open.
                std::string sourceHash, targetHash;
                if (!verification::Sha256File(source, sourceHash) || !verification::Sha256File(destination, targetHash) || sourceHash != targetHash) return false;
            }
            std::wstring command = internal::QuoteCommandLineArgument(destination.wstring()) + L" " +
                internal::QuoteCommandLineArgument(options.installRoot.wstring()) + L" " + std::to_wstring(::GetCurrentProcessId());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            if (!::CreateProcessW(destination.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, options.installRoot.c_str(), &startup, &process)) return false;
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            return true;
        }

        StartupResult Activated(internal::Context& context, Transaction& tx)
        {
            std::string error;
            if (tx.ownerPid == 0 && !MarkAttempt(tx, error))
                return Result(context, StartupStatus::FailedOpen, error, L"The installed update could not establish its startup journal.", false);
            StartupResult result;
            result.status = StartupStatus::Updated;
            result.selectedRuntimePath = context.options.canonicalRuntimePath;
            result.selectedVersion = tx.target.version;
            result.availableVersion = tx.target.version;
            result.previousVersion = tx.previousVersion;
            result.installationRoot = tx.root;
            result.healthMarkerPath = UpdateRoot(tx.root) / L"journal.json";
            result.rollbackBackupPath = UpdateRoot(tx.root) / L"backup";
            result.updaterStoragePath = context.storageRoot;
            // Only the process that owns this startup attempt may declare it
            // healthy/unhealthy. A second process must not erase its journal.
            result.updateActivated = tx.ownerPid == ::GetCurrentProcessId();
            CaptureGeneration(result, context.options);
            result.message = L"Changed vSMR files have been installed and verified.";
            context.state.status = "updated";
            context.state.installedVersion = tx.target.version;
            context.state.availableVersion = tx.target.version;
            context.state.downloadPercent = 100;
            context.state.message = internal::WideToUtf8(result.message);
            context.state.nextCheckUtc = internal::UtcAfterSeconds(internal::kMinimumCheckIntervalSeconds);
            internal::PersistState(context);
            return result;
        }

        StartupResult Recover(internal::Context& context)
        {
            Transaction tx;
            std::string error;
            if (!LoadTransaction(context.options.installRoot, tx, error))
                return Result(context, StartupStatus::FailedOpen, error, L"The pending file-update journal is invalid; vSMR will not load mixed files.", false);
            if (tx.phase == "healthy")
            {
                Cleanup(tx);
                return Result(context, StartupStatus::Current, "", L"The previous update completed successfully.");
            }
            if (tx.phase == "installed" && tx.ownerPid != 0 && OwnerAlive(tx))
                return Activated(context, tx);
            auto lease = internal::AcquireExclusiveSessionLock(context.sessionLockStorageRoot, context.options.installRoot);
            if (!lease)
                return Result(context, tx.phase == "ready" ? StartupStatus::Deferred : StartupStatus::FailedOpen,
                    "installation_in_use", L"Another EuroScope session is using this installation; the update remains pending.", tx.phase == "ready");
            if (tx.phase == "installing")
            {
                // The manifest rename is the commit point. A crash immediately
                // after it need not discard an already complete verified update.
                if (IsCommitted(tx))
                {
                    tx.phase = "installed";
                    tx.ownerPid = 0;
                    tx.ownerCreated = 0;
                    if (!SaveTransaction(tx, error))
                        return Result(context, StartupStatus::FailedOpen, error, L"The committed update journal could not be recovered.", false);
                }
            }
            if (tx.phase == "installed" && tx.ownerPid == 0)
            {
                for (const auto& file : tx.target.files)
                    if (!SafePath(tx.root, tx.root / fs::u8path(file.path)) || !Matches(tx.root / fs::u8path(file.path), file))
                        return Result(context, StartupStatus::FailedOpen, "installed_hash_mismatch", L"A pending update no longer matches its manifest.", false);
                return Activated(context, tx);
            }
            if (tx.phase == "ready")
            {
                if (LoaderChanged(tx))
                {
                    const bool started = context.options.testFeedDirectory.empty() && StartHelper(context.options);
                    return Result(context, StartupStatus::Deferred, started ? "" : "apply_helper_unavailable",
                        started ? L"The verified update will be applied after EuroScope closes." : L"The verified update is pending; its offline helper could not start.");
                }
                internal::Report(context, ProgressStage::Installing, -1, L"Installing verified vSMR files...");
                if (Apply(tx, error)) return Activated(context, tx);
                if (tx.phase == "ready")
                {
                    Cleanup(tx); // No installation writes occurred; safe to redownload next time.
                    return Result(context, StartupStatus::FailedOpen, error, L"Update preparation failed; the current installation is unchanged.");
                }
            }
            const bool failedRuntime = tx.phase == "failed" || (tx.phase == "installed" && tx.ownerPid != 0);
            if (failedRuntime) WriteText(UpdateRoot(tx.root) / L"failed-commit.txt", tx.target.contentCommit);
            if (!Rollback(tx, error))
            {
                if (LoaderChanged(tx) && context.options.testFeedDirectory.empty()) StartHelper(context.options);
                return Result(context, StartupStatus::FailedOpen, error, L"Update recovery is pending; close EuroScope so the previous files can be restored.", false);
            }
            auto result = Result(context, StartupStatus::FailedOpen, "update_rolled_back", L"The incomplete update was rolled back safely.");
            result.selectedVersion = tx.previousVersion;
            return result;
        }

        std::wstring EncodePath(const std::string& path)
        {
            // Manifest paths are restricted ASCII. Only spaces need escaping.
            std::wstring result;
            for (char c : path) { if (c == ' ') result += L"%20"; else result.push_back(static_cast<wchar_t>(c)); }
            return result;
        }
    }

    bool HasTransaction(const fs::path& root)
    {
        const auto path = UpdateRoot(root) / L"journal.json";
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) return true;
        const DWORD error = ::GetLastError();
        // Access denial is an unknown journal, never proof of no transaction.
        return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    }

    bool IsFileUpdate(const StartupResult& update)
    {
        return !update.installationRoot.empty() && update.healthMarkerPath == UpdateRoot(update.installationRoot) / L"journal.json";
    }

    bool ValidatePreparedGeneration(const StartupOptions& options, const StartupResult& update)
    {
        if (!update.fileProtocol) return true;
        auto mutex = internal::AcquireUpdaterMutex(options.installRoot);
        if (!mutex) return false;
        if (HasTransaction(options.installRoot))
        {
            Transaction tx;
            std::string error;
            if (!LoadTransaction(options.installRoot, tx, error) ||
                (tx.phase != "ready" && tx.phase != "installed" && tx.phase != "healthy")) return false;
            if (IsFileUpdate(update) && (tx.phase == "ready" || tx.target.version != update.selectedVersion)) return false;
        }
        StartupResult current;
        CaptureGeneration(current, options);
        return !update.selectedRuntimeSha256.empty() && !update.selectedManifestSha256.empty() &&
            current.selectedRuntimeSha256 == update.selectedRuntimeSha256 && current.selectedManifestSha256 == update.selectedManifestSha256;
    }

    StartupResult Prepare(const StartupOptions& options)
    {
        internal::Context context(options);
        if (!SafePath(options.installRoot, UpdateRoot(options.installRoot)))
            return Result(context, StartupStatus::FailedOpen, "unsafe_staging_path", L"The update storage path is unsafe.", false);
        auto mutex = internal::AcquireUpdaterMutex(options.installRoot);
        if (!mutex)
            return Result(context, HasTransaction(options.installRoot) ? StartupStatus::FailedOpen : StartupStatus::Deferred,
                "updater_busy", L"Another vSMR update is in progress.", !HasTransaction(options.installRoot));
        if (HasTransaction(options.installRoot)) return Recover(context);
        if (!MigrationReady(options))
            return Result(context, StartupStatus::Deferred, "configuration_migration_pending", L"User settings must be imported before updating defaults; vSMR will retry on the next startup.");
        const auto config = internal::LoadConfig(context.storageRoot / L"config.json", options.defaultChannel);
        const auto action = internal::ConsumeAction(context.storageRoot);
        const bool force = action.valid && (action.action == "check_now" || action.action == "retry_update" || action.action == "reload_aviso");
        const bool explicitInstall = action.valid && (action.action == "retry_update" || action.action == "reload_aviso");
        const auto previous = internal::LoadPreviousState(context.statePath);
        context.state.lastActionRequestId = action.valid ? action.requestId : previous.lastActionRequestId;
        context.state.lastCheckedUtc = previous.lastCheckedUtc;
        if ((!config.autoCheck || internal::IsFutureUtc(previous.nextCheckUtc)) && !force)
            return Result(context, StartupStatus::Current, "", L"No update check is due.");
        context.state.lastCheckedUtc = internal::UtcNow();
        context.state.nextCheckUtc = internal::UtcAfterSeconds(internal::kMinimumCheckIntervalSeconds);
        if (!internal::Report(context, ProgressStage::Checking, -1, L"Checking the vSMR file manifest..."))
            return Result(context, StartupStatus::Cancelled, "cancelled", L"Update check cancelled.");
        const std::wstring channel = config.channel == UpdateChannel::Beta ? L"beta" : L"stable";
        std::string json;
        if (!options.testFeedDirectory.empty())
        {
            if (!ReadText(options.testFeedDirectory / channel / L"version.json", json))
                return Result(context, StartupStatus::FailedOpen, "manifest_unavailable", L"The file-update fixture manifest is unavailable.");
        }
        else
        {
            const auto response = internal::HttpGetTransport(context, std::wstring(FeedPrefix) + L"update-feed/" + channel + L"/version.json",
                internal::RemainingMs(context, 5000), 2 * 1024 * 1024);
            if (response.statusCode != 200 || !response.error.empty())
                return Result(context, StartupStatus::FailedOpen, response.error.empty() ? "manifest_unavailable" : response.error, L"The update manifest could not be downloaded; using the installed version.");
            json.assign(response.body.begin(), response.body.end());
        }
        Transaction tx;
        tx.root = options.installRoot;
        tx.targetJson = json;
        tx.previousVersion = options.currentVersion;
        std::string error;
        if (!ParseManifest(json, tx.target, error)) return Result(context, StartupStatus::FailedOpen, error, L"The remote file manifest is invalid.");
        std::string failedCommit;
        if (!explicitInstall && ReadText(UpdateRoot(tx.root) / L"failed-commit.txt", failedCommit) && failedCommit == tx.target.contentCommit)
            return Result(context, StartupStatus::Deferred, "previous_update_failed", L"This update previously failed; use Retry update to try it again.");
        const auto remote = release_model::ParseSemVer(tx.target.version);
        const auto installed = release_model::ParseSemVer(options.currentVersion);
        if (!release_model::ChannelAccepts(remote, config.channel) || release_model::CompareSemVer(remote, installed) < 0 ||
            (tx.target.version == config.skippedVersion && !explicitInstall))
            return Result(context, StartupStatus::Current, "", L"The channel contains no applicable update.");
        context.state.availableVersion = tx.target.version;
        const auto directory = UpdateRoot(tx.root);
        if (!SafePath(tx.root, directory)) return Result(context, StartupStatus::FailedOpen, "unsafe_staging_path", L"The update staging path is unsafe.");
        for (const auto& file : tx.target.files)
        {
            if (!SafePath(tx.root, tx.root / fs::u8path(file.path)))
                return Result(context, StartupStatus::FailedOpen, "unsafe_install_path", L"An update path is unsafe.");
            if (!Matches(tx.root / fs::u8path(file.path), file)) tx.changes.push_back({file, false, {}});
        }
        std::string localJson;
        if (tx.changes.empty() && ReadText(options.dataRoot / L"version.json", localJson) && localJson == json)
            return Result(context, StartupStatus::Current, "", L"vSMR is up to date.");
        const bool needsNewLoader = tx.target.runtimeAbi != options.expectedRuntimeAbi ||
            release_model::CompareSemVer(release_model::ParseSemVer(options.loaderVersion), release_model::ParseSemVer(tx.target.minimumLoaderVersion)) < 0;
        if (needsNewLoader && !LoaderChanged(tx))
            return Result(context, StartupStatus::Deferred, "loader_incompatible", L"This release needs a newer loader but does not contain one; install its full package.");
        if (!EnoughDiskSpace(tx))
            return Result(context, StartupStatus::FailedOpen, "insufficient_disk_space", L"Insufficient disk space for staged files and rollback backups.");
        if (!config.autoDownload && !explicitInstall)
            return Result(context, StartupStatus::UpdateAvailable, "", L"A vSMR file update is available; automatic downloads are disabled.");
        // No per-file revision counters: actual SHA-256 is authoritative, also
        // repairing missing/corrupt managed files even at the same version.
        for (const auto& change : tx.changes)
        {
            const auto destination = directory / L"new" / fs::u8path(change.file.path);
            if (!SafePath(tx.root, destination)) return Result(context, StartupStatus::FailedOpen, "unsafe_staging_path", L"A download path is unsafe.");
            if (Matches(destination, change.file))
            {
                if (internal::EndsWithNoCase(destination.wstring(), L".dll") && !internal::IsX86PortableExecutable(destination))
                    return Result(context, StartupStatus::FailedOpen, "downloaded_dll_invalid", L"A cached DLL is not a valid 32-bit Windows library.");
                continue;
            }
            if (!internal::Report(context, ProgressStage::Downloading, -1, L"Downloading " + internal::Utf8ToWide(change.file.path)))
                return Result(context, StartupStatus::Cancelled, "cancelled", L"Update download cancelled; installed files are unchanged.");
            if (!options.testFeedDirectory.empty())
            {
                if (!CopyAtomically(options.testFeedDirectory / L"payload" / fs::u8path(change.file.path), destination))
                    return Result(context, StartupStatus::FailedOpen, "fixture_file_missing", L"A fixture payload file is missing.");
            }
            else
            {
                // Never append a previous failed download. Retries restart this
                // one changed file; completed SHA-verified files are reused.
                if (Exists(destination)) ::DeleteFileW(destination.c_str());
                const auto response = internal::HttpGetTransport(context,
                    std::wstring(FeedPrefix) + internal::Utf8ToWide(tx.target.contentCommit) + L"/payload/" + EncodePath(change.file.path),
                    internal::RemainingMs(context, options.overallDeadlineMs), (std::max)(change.file.size, std::uint64_t{1}), {}, destination, change.file.size);
                if (response.statusCode != 200 || !response.error.empty())
                    return Result(context, StartupStatus::FailedOpen, response.error.empty() ? "file_download_failed" : response.error, L"A changed file could not be downloaded; installed files are unchanged.");
            }
            if (!Matches(destination, change.file))
                return Result(context, StartupStatus::FailedOpen, "download_hash_mismatch", L"A downloaded file failed SHA-256 verification; installed files are unchanged.");
            if (internal::EndsWithNoCase(destination.wstring(), L".dll") && !internal::IsX86PortableExecutable(destination))
                return Result(context, StartupStatus::FailedOpen, "downloaded_dll_invalid", L"A downloaded DLL is not a valid 32-bit Windows library.");
        }
        if (needsNewLoader)
        {
            const auto version = release_model::ParseSemVer(BinaryFileVersion(directory / L"new/vSMR.dll"));
            if (!version.valid || release_model::CompareSemVer(version, release_model::ParseSemVer(tx.target.minimumLoaderVersion)) < 0 ||
                (tx.target.runtimeAbi != options.expectedRuntimeAbi && release_model::CompareSemVer(version, release_model::ParseSemVer(options.loaderVersion)) <= 0))
                return Result(context, StartupStatus::Deferred, "loader_incompatible", L"The downloaded loader does not satisfy the release's required version.");
        }
        if (!ValidateDefaults(tx, error))
            return Result(context, StartupStatus::FailedOpen, error, L"The new default configuration is invalid; installed files are unchanged.");
        if (!config.autoInstall && !explicitInstall)
            return Result(context, StartupStatus::UpdateAvailable, "", L"Changed files are downloaded and verified; automatic installation is disabled.");
        // Even a metadata-only update uses the same journal/manifest-last path.
        if (!WriteText(directory / L"target.json", json) || !SaveTransaction(tx, error))
            return Result(context, StartupStatus::FailedOpen, "journal_write_failed", L"The update journal could not be written; installed files are unchanged.");
        return Recover(context);
    }

    bool Confirm(const StartupResult& update, bool healthy)
    {
        // Do not lose a successful startup merely because another installation
        // is downloading under the global updater mutex. The health write is
        // independent; optional cleanup waits for the updater lock.
        auto marker = internal::AcquireHealthMarkerMutex(update.healthMarkerPath);
        if (!marker) return false;
        Transaction tx;
        std::string error;
        if (!HasTransaction(update.installationRoot)) return healthy;
        std::string targetHash;
        if (!LoadTransaction(update.installationRoot, tx, error) || tx.target.version != update.selectedVersion ||
            tx.ownerPid != ::GetCurrentProcessId() || !verification::Sha256File(UpdateRoot(tx.root) / L"target.json", targetHash) || targetHash != update.selectedManifestSha256 ||
            (tx.phase != "installed" && tx.phase != "failed" && tx.phase != "healthy")) return false;
        tx.phase = healthy ? "healthy" : "failed";
        if (!SaveTransaction(tx, error)) return false;
        if (!healthy) WriteText(UpdateRoot(tx.root) / L"failed-commit.txt", tx.target.contentCommit);
        if (healthy)
        {
            std::string failedCommit;
            const auto failedPath = UpdateRoot(tx.root) / L"failed-commit.txt";
            if (ReadText(failedPath, failedCommit) && failedCommit == tx.target.contentCommit) ::DeleteFileW(failedPath.c_str());
            auto mutex = internal::AcquireUpdaterMutex(update.installationRoot);
            if (mutex) Cleanup(tx);
        }
        return true;
    }

    bool Restore(const StartupOptions& options, const StartupResult& update, fs::path* restoredRuntime, std::wstring* message)
    {
        auto mutex = internal::AcquireUpdaterMutex(options.installRoot);
        auto lease = internal::AcquireExclusiveSessionLock(internal::Context(options).sessionLockStorageRoot, options.installRoot);
        std::string error = "installation_in_use";
        Transaction tx;
        std::string targetHash;
        const bool matching = mutex && lease && options.installRoot == update.installationRoot &&
            LoadTransaction(options.installRoot, tx, error) && tx.target.version == update.selectedVersion &&
            verification::Sha256File(UpdateRoot(tx.root) / L"target.json", targetHash) && targetHash == update.selectedManifestSha256;
        if (matching) WriteText(UpdateRoot(options.installRoot) / L"failed-commit.txt", tx.target.contentCommit);
        const bool ok = matching && Rollback(tx, error);
        if (ok && restoredRuntime != nullptr) *restoredRuntime = options.canonicalRuntimePath;
        if (!ok)
        {
            if (matching && IsFileUpdate(update) && LoaderChanged(tx) && options.testFeedDirectory.empty()) StartHelper(options);
            if (message != nullptr) *message = internal::Utf8ToWide(error);
        }
        return ok;
    }

    int ApplyPending(const fs::path& root, std::uint32_t parentPid)
    {
        if (!root.is_absolute() || !SafePath(root, UpdateRoot(root))) return 2;
        // A real process handle avoids PID reuse after it has been opened.
        internal::UniqueHandle parent(::OpenProcess(SYNCHRONIZE, FALSE, parentPid));
        if (parent) ::WaitForSingleObject(parent.get(), INFINITE);
        StartupOptions options;
        options.installRoot = root.lexically_normal();
        options.dataRoot = root / L"vSMR_Data";
        options.canonicalRuntimePath = options.dataRoot / L"Runtime" / L"vSMR.Runtime.dll";
        options.loaderPath = root / L"vSMR.dll";
        internal::Context context(options);
        // Other EuroScope instances may still own the installation. Leave the
        // durable journal pending if they remain open; next startup retries.
        for (int attempt = 0; attempt < 600; ++attempt)
        {
            {
                auto mutex = internal::AcquireUpdaterMutex(root);
                if (mutex && !HasTransaction(root)) return 0;
                auto lease = mutex ? internal::AcquireExclusiveSessionLock(context.sessionLockStorageRoot, root) : internal::UniqueHandle{};
                if (mutex && lease)
                {
                    Transaction tx;
                    std::string error;
                    if (!LoadTransaction(root, tx, error)) return 3;
                    if (tx.phase == "installing" && IsCommitted(tx))
                    {
                        tx.phase = "installed";
                        tx.ownerPid = 0;
                        tx.ownerCreated = 0;
                        return SaveTransaction(tx, error) ? 0 : 11;
                    }
                    if (tx.phase == "ready")
                    {
                        if (Apply(tx, error)) return 0;
                        if (tx.phase == "ready")
                        {
                            if (error != "target_locked") return 4;
                            // A just-starting host can map the loader before
                            // taking our session lease. Retry after releasing
                            // both locks, without quarantining a valid update.
                        }
                        else return Rollback(tx, error) ? 5 : 6;
                    }
                    else
                    {
                        if (tx.phase == "healthy") return Cleanup(tx) ? 0 : 7;
                        if (tx.phase == "installed" && (tx.ownerPid == 0 || OwnerAlive(tx))) return 0;
                        if (tx.phase == "failed" || tx.phase == "installed")
                            WriteText(UpdateRoot(root) / L"failed-commit.txt", tx.target.contentCommit);
                        return Rollback(tx, error) ? 0 : 8;
                    }
                }
            }
            ::Sleep(1000);
        }
        return 9;
    }
}
