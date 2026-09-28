#include "FileUpdateTests.hpp"
#include "updater/FileUpdater.hpp"
#include "updater/FileUpdateTransaction.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterVerification.hpp"
#include "updater/UpdaterUrlPolicy.hpp"
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace fu = vsmr::updater::files;
namespace up = vsmr::updater;

namespace
{
    const std::string Config = R"({"_migration":{"legacy_profiles":true,"legacy_aviso":true},"profiles":{"mine":{"null_override":null,"name":"Do not replace me"}}})";
    const std::string OldManifest = R"({"old":"manifest preserved until commit"})";
    const std::string OldDefaults = R"({"schema_version":1,"profiles":{"builtin-default":{"name":"Default","value":1}}})";
    const std::string NewDefaults = R"({"schema_version":1,"profiles":{"builtin-default":{"name":"Default","value":2,"new_setting":true}}})";

    std::string Dll(char marker)
    {
        std::string data(256, marker);
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 128;
        std::memcpy(data.data(), &dos, sizeof(dos));
        const DWORD signature = IMAGE_NT_SIGNATURE;
        std::memcpy(data.data() + 128, &signature, sizeof(signature));
        IMAGE_FILE_HEADER header{};
        header.Machine = IMAGE_FILE_MACHINE_I386;
        header.Characteristics = IMAGE_FILE_DLL;
        std::memcpy(data.data() + 132, &header, sizeof(header));
        return data;
    }

    struct Fixture
    {
        fs::path root;
        up::StartupOptions options;
        std::vector<std::string> paths{ "vSMR.dll", "vSMR_Data/Runtime/vSMR.Runtime.dll", "vSMR_Data/default.json", "vSMR_Data/Audio/timer.wav" };
        std::string manifest;

        Fixture()
        {
            static unsigned counter = 0;
            root = fs::temp_directory_path() / (L"vSMR-file-updater-tests-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++counter));
            options.installRoot = root / L"installation";
            options.dataRoot = options.installRoot / L"vSMR_Data";
            options.loaderPath = options.installRoot / L"vSMR.dll";
            options.canonicalRuntimePath = options.dataRoot / L"Runtime/vSMR.Runtime.dll";
            options.currentVersion = "2.0.0-beta.6";
            options.loaderVersion = "1.3.0";
            options.expectedRuntimeAbi = 1;
            options.testFeedDirectory = root / L"feed";
            options.testStorageDirectory = root / L"state";
            options.allowUnsignedTestManifest = true;
            Put(options.loaderPath, Dll('L'));
            Put(options.canonicalRuntimePath, Dll('R'));
            Put(options.dataRoot / L"default.json", OldDefaults);
            Put(options.dataRoot / L"Audio/timer.wav", "old audio");
            Put(options.dataRoot / L"config.json", Config);
            Put(options.dataRoot / L"version.json", OldManifest);
            for (const auto& path : paths)
                if (!fu::CopyAtomically(options.installRoot / fs::u8path(path), options.testFeedDirectory / L"payload" / fs::u8path(path)))
                    throw std::runtime_error("fixture payload copy failed");
            Put(options.testFeedDirectory / L"payload/vSMR_Data/default.json", NewDefaults);
            MakeManifest();
        }

        ~Fixture()
        {
            // Only the uniquely-created, explicit test directory is removable.
            std::error_code parentError;
            if (fs::equivalent(root.parent_path(), fs::temp_directory_path(), parentError) && !parentError &&
                root.filename().wstring().rfind(L"vSMR-file-updater-tests-", 0) == 0)
            {
                std::error_code ignored;
                fs::remove_all(root, ignored);
            }
        }

        static void Put(const fs::path& path, const std::string& contents)
        {
            if (!fu::WriteText(path, contents)) throw std::runtime_error("fixture write failed");
        }

        static std::string Get(const fs::path& path)
        {
            std::string value;
            if (!fu::ReadText(path, value, 16ULL * 1024 * 1024)) throw std::runtime_error("fixture read failed");
            return value;
        }

        void MakeManifest()
        {
            std::ostringstream json;
            json << R"({"schema":1,"version":"2.0.0","content_commit":")" << std::string(40, 'a') <<
                R"(","minimum_loader_version":"1.3.0","runtime_abi":1,"files":{)";
            bool first = true;
            for (const auto& path : paths)
            {
                const auto payload = options.testFeedDirectory / L"payload" / fs::u8path(path);
                std::string hash;
                if (!up::verification::Sha256File(payload, hash)) throw std::runtime_error("fixture hash failed");
                if (!first) json << ',';
                first = false;
                json << '"' << path << R"(":{"sha256":")" << hash << R"(","size":)" << fs::file_size(payload) << '}';
            }
            json << "}}";
            manifest = json.str();
            Put(options.testFeedDirectory / L"beta/version.json", manifest);
            Put(options.testFeedDirectory / L"stable/version.json", manifest);
        }

        fu::Transaction Stage()
        {
            fu::Transaction tx;
            tx.root = options.installRoot;
            tx.targetJson = manifest;
            tx.previousVersion = options.currentVersion;
            std::string error;
            if (!fu::ParseManifest(manifest, tx.target, error)) throw std::runtime_error(error);
            for (const auto& file : tx.target.files)
                if (!fu::Matches(options.installRoot / fs::u8path(file.path), file))
                {
                    tx.changes.push_back({file, false, {}});
                    if (!fu::CopyAtomically(options.testFeedDirectory / L"payload" / fs::u8path(file.path),
                        fu::UpdateRoot(options.installRoot) / L"new" / fs::u8path(file.path)))
                        throw std::runtime_error("stage copy failed for " + file.path + ": Win32 " + std::to_string(::GetLastError()));
                }
            Put(fu::UpdateRoot(options.installRoot) / L"target.json", manifest);
            if (!fu::SaveTransaction(tx, error)) throw std::runtime_error(error);
            return tx;
        }
    };
}

std::vector<std::string> RunFileUpdateTests()
{
    std::vector<std::string> failures;
    const auto check = [&](bool condition, const std::string& label) { if (!condition) failures.push_back("file updater: " + label); };
    try
    {
        for (const auto* path : { "../config.json", "vSMR_Data/config.json", "vSMR_Data/CONFIG.JSON", "vSMR_Data/UserData/mine.json",
            "vSMR_Data/UpdateBaselines/default.json", "vSMR_Data/profiles.json", "vSMR_Data/vSMR_Profiles.json", "vSMR_Data/version.json",
            "vSMR_Data/.update/journal.json", "vSMR_Data/logs/log.txt", "vSMR_Data/AVISO/LFPG.geojson.bak", "vSMR_Data/Runtime/con.dll",
            "vSMR_Data/Runtime/a.dll:alternate", "vSMR_Data/Runtime/../config.json", "vSMR_Data/Audio/a.wav.", "C:/vSMR.dll", "vSMR_Data/Runtime/a%2fb.dll" })
            check(!fu::IsManagedPath(path), std::string("rejects protected/unsafe path ") + path);
        check(fu::IsManagedPath("vSMR.dll") && fu::IsManagedPath("vSMR_Data/default.json") && fu::IsManagedPath("vSMR_Data/AVISO/LFLL.geojson"), "accepts managed assets");
        up::url_policy::ParsedHttpsUrl parsed;
        const std::wstring manifestUrl = L"https://raw.githubusercontent.com/IWantPizzaa/vSMR/update-feed/beta/version.json";
        check(up::url_policy::TryParseAllowedHttpsUrl(manifestUrl, parsed), "allows exact raw channel endpoint");
        check(!up::url_policy::TryParseAllowedHttpsUrl(L"https://raw.githubusercontent.com/attacker/vSMR/update-feed/beta/version.json", parsed), "rejects different raw repository");
        std::wstring redirect;
        check(!up::url_policy::TryResolveAllowedRedirect(manifestUrl, L"https://github.com/other", redirect), "raw download cannot redirect to another trust scope");
        check(!up::url_policy::TryResolveAllowedRedirect(manifestUrl, L"/IWantPizzaa/vSMR/update-feed/stable/version.json", redirect), "raw redirect cannot change channel");
        {
            Fixture f;
            fu::Manifest manifest;
            std::string error;
            check(fu::ParseManifest(f.manifest, manifest, error), "parses publisher schema");
            auto duplicate = f.manifest;
            duplicate.insert(1, "\"schema\":1,");
            check(!fu::ParseManifest(duplicate, manifest, error), "rejects duplicate JSON members");
            auto protectedFile = f.manifest;
            const auto offset = protectedFile.find("vSMR_Data/default.json");
            protectedFile.replace(offset, std::strlen("vSMR_Data/default.json"), "vSMR_Data/config.json");
            check(!fu::ParseManifest(protectedFile, manifest, error), "manifest can never manage user config");
        }
        {
            Fixture f;
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.status == up::StartupStatus::Updated && result.updateActivated, "installs changed files before runtime loading");
            check(Fixture::Get(f.options.dataRoot / L"version.json") == f.manifest, "commits target manifest after successful update");
            check(Fixture::Get(f.options.dataRoot / L"config.json") == Config, "keeps sparse config byte-for-byte");
            check(!fs::exists(fu::UpdateRoot(f.options.installRoot) / L"new/vSMR.dll"), "does not download an unchanged DLL");
            check(fu::ValidatePreparedGeneration(f.options, result), "validates generation after taking session lease");
            check(up::ConfirmRuntimeHealthy(result) && !fu::HasTransaction(f.options.installRoot), "healthy runtime cleans journal");
            Fixture::Put(f.options.dataRoot / L"version.json", OldManifest);
            check(!fu::ValidatePreparedGeneration(f.options, result), "detects generation change between prepare and runtime load");
        }
        {
            Fixture f;
            Fixture::Put(f.options.testFeedDirectory / L"payload/vSMR_Data/default.json", "corrupt transfer");
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.errorCode == "download_hash_mismatch", "rejects a corrupt download");
            check(Fixture::Get(f.options.dataRoot / L"default.json") == OldDefaults &&
                Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest, "corrupt download never alters installed files or manifest");
        }
        {
            Fixture f;
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            up::internal::UniqueHandle started(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            up::internal::UniqueHandle release(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            bool lockHeld = false;
            std::thread other([&]() {
                auto mutex = up::internal::AcquireUpdaterMutex(f.options.installRoot);
                lockHeld = static_cast<bool>(mutex);
                ::SetEvent(started.get());
                ::WaitForSingleObject(release.get(), 5000);
            });
            ::WaitForSingleObject(started.get(), 5000);
            const bool confirmed = up::ConfirmRuntimeHealthy(result);
            ::SetEvent(release.get());
            other.join();
            fu::Transaction tx;
            std::string error;
            check(lockHeld && confirmed && fu::LoadTransaction(f.options.installRoot, tx, error) && tx.phase == "healthy", "health confirmation survives busy updater mutex");
            const auto recovered = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(recovered.status == up::StartupStatus::Current && !fu::HasTransaction(f.options.installRoot) &&
                Fixture::Get(f.options.dataRoot / L"version.json") == f.manifest, "deferred healthy cleanup does not roll back successful update");
        }
        {
            Fixture f;
            Fixture::Put(f.options.dataRoot / L"config.json", R"({"_migration":{"legacy_profiles":true,"legacy_aviso":false}})");
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.errorCode == "configuration_migration_pending" && !fu::HasTransaction(f.options.installRoot), "waits for both profile and AVISO migration");
        }
        {
            Fixture f;
            Fixture::Put(f.options.testFeedDirectory / L"payload/vSMR_Data/default.json", R"({"schema_version":1,"profiles":{}})");
            f.MakeManifest(); // Hash is valid; semantic defaults are not.
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.errorCode == "default_json_invalid" && !fu::HasTransaction(f.options.installRoot) &&
                Fixture::Get(f.options.dataRoot / L"default.json") == OldDefaults && Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest,
                "hash-valid but unusable defaults never replace working configuration");
        }
        {
            Fixture f;
            const auto installed = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(up::ConfirmRuntimeHealthy(installed), "confirms current-version fixture");
            Fixture::Put(f.options.testStorageDirectory / L"config.json", R"({"auto_download":false})");
            fs::remove(f.options.testStorageDirectory / L"state.json");
            const auto current = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(current.status == up::StartupStatus::Current, "disabled auto-download does not report an already installed update");
        }
        {
            Fixture f;
            std::string large = Config.substr(0, Config.size() - 1) + ",\"padding\":[";
            for (int index = 0; index < 60; ++index)
                large += (index == 0 ? "\"" : ",\"") + std::string(60 * 1024, 'a') + "\"";
            large += "]}";
            Fixture::Put(f.options.dataRoot / L"config.json", large);
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.status == up::StartupStatus::Updated && Fixture::Get(f.options.dataRoot / L"config.json") == large, "supports large map override config without touching it");
            check(up::ConfirmRuntimeHealthy(result), "confirms update with large overrides");
        }
        {
            Fixture f;
            Fixture::Put(f.options.testFeedDirectory / L"payload/vSMR_Data/Audio/timer.wav", "new audio");
            f.MakeManifest();
            auto tx = f.Stage();
            // Lock the SECOND target after preflight, simulating an external
            // process mapping a file during installation.
            const auto lockedPath = f.options.dataRoot / L"Audio/timer.wav";
            HANDLE locked = INVALID_HANDLE_VALUE;
            std::string error;
            check(!fu::Apply(tx, error, [&](const fu::File& file) {
                if (file.path == "vSMR_Data/Audio/timer.wav")
                    locked = ::CreateFileW(lockedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            }) && tx.phase == "installing", "partial file replacement leaves durable journal");
            check(locked != INVALID_HANDLE_VALUE, "creates replacement fault fixture");
            check(Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest, "manifest is unchanged during partial installation");
            if (locked != INVALID_HANDLE_VALUE) ::CloseHandle(locked);
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.errorCode == "update_rolled_back" && !result.selectedRuntimePath.empty(), "recovers interrupted transaction before runtime load");
            check(Fixture::Get(f.options.dataRoot / L"default.json") == OldDefaults &&
                Fixture::Get(lockedPath) == "old audio" && Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest, "rollback restores all changed files and old manifest");
            check(Fixture::Get(f.options.dataRoot / L"config.json") == Config, "rollback never writes user config");
        }
        {
            Fixture f;
            auto tx = f.Stage();
            std::string error;
            check(fu::Apply(tx, error), "commits complete test transaction");
            tx.phase = "installing"; // Crash after manifest rename, before journal update.
            check(fu::SaveTransaction(tx, error), "persists interrupted commit fixture");
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.status == up::StartupStatus::Updated, "recognizes completed manifest commit after crash");
            check(up::ConfirmRuntimeHealthy(result), "confirms recovered committed generation");
        }
        {
            Fixture f;
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(up::MarkRuntimeUnhealthy(result), "records runtime construction failure");
            fs::path restored;
            std::wstring error;
            check(up::RollbackPreparedUpdate(f.options, result, &restored, &error) && restored == f.options.canonicalRuntimePath, "public rollback restores failed runtime generation");
            check(Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest && Fixture::Get(f.options.dataRoot / L"config.json") == Config, "runtime failure preserves config and restores manifest");
        }
        {
            Fixture f;
            Fixture::Put(f.options.testFeedDirectory / L"payload/vSMR.dll", Dll('N'));
            f.MakeManifest();
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            fu::Transaction tx;
            std::string error;
            check(result.status == up::StartupStatus::Deferred && fu::LoadTransaction(f.options.installRoot, tx, error) && tx.phase == "ready", "loader change defers entire transaction to offline helper");
            check(Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest && Fixture::Get(f.options.dataRoot / L"default.json") == OldDefaults, "pending DLL never leaves partially updated assets");
        }
        {
            Fixture f;
            const auto leasePath = up::internal::SessionLockPath(f.options.testStorageDirectory, f.options.installRoot);
            fs::create_directories(leasePath.parent_path());
            const HANDLE shared = ::CreateFileW(leasePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(shared != INVALID_HANDLE_VALUE, "creates another EuroScope session lease");
            const auto deferred = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(deferred.status == up::StartupStatus::Deferred && Fixture::Get(f.options.dataRoot / L"version.json") == OldManifest, "live session defers installation");
            if (shared != INVALID_HANDLE_VALUE) ::CloseHandle(shared);
            const auto applied = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(applied.status == up::StartupStatus::Updated && up::ConfirmRuntimeHealthy(applied), "pending transaction installs when last session closes");
        }
        {
            Fixture f;
            auto tx = f.Stage();
            tx.changes.front().file.path = "vSMR_Data/config.json";
            std::string error;
            check(fu::SaveTransaction(tx, error), "creates tampered journal fixture");
            const auto result = up::PrepareUpdateBeforeRuntimeLoad(f.options);
            check(result.selectedRuntimePath.empty() && result.status == up::StartupStatus::FailedOpen, "invalid journal fails closed");
            check(Fixture::Get(f.options.dataRoot / L"config.json") == Config, "tampered journal cannot target user settings");
        }
    }
    catch (const std::exception& error) { failures.push_back(std::string("file updater fixture: ") + error.what()); }
    return failures;
}
