#include "updater/FileUpdateTransaction.hpp"
#include "updater/UpdaterVerification.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "shared/JsonDocument.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <regex>

namespace fs = std::filesystem;
namespace vsmr::updater::files
{
    namespace
    {
        constexpr std::uint64_t MaximumJson = 2 * 1024 * 1024;
        constexpr std::uint64_t MaximumFile = 256ULL * 1024 * 1024;
        constexpr std::uint64_t MaximumTotal = 1024ULL * 1024 * 1024;

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            return text;
        }

        bool Hex(const std::string& text, std::size_t count)
        {
            return text.size() == count && std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }

        std::string String(const rapidjson::Value& object, const char* key)
        {
            return object.IsObject() && object.HasMember(key) && object[key].IsString()
                ? std::string(object[key].GetString(), object[key].GetStringLength()) : std::string{};
        }

        bool UniqueMembers(const rapidjson::Value& value)
        {
            if (value.IsObject())
            {
                std::set<std::string> keys;
                for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
                    if (!keys.insert(std::string(it->name.GetString(), it->name.GetStringLength())).second || !UniqueMembers(it->value)) return false;
            }
            else if (value.IsArray())
                for (const auto& item : value.GetArray()) if (!UniqueMembers(item)) return false;
            return true;
        }

        bool Decode(const std::string& text, rapidjson::Document& document)
        {
            if (text.size() > MaximumJson) return false;
            VsmrJson::ParseDocument(document, text);
            return !document.HasParseError() && document.IsObject() && UniqueMembers(document);
        }

        void Add(rapidjson::Document& document, rapidjson::Value& value, const char* key, const std::string& text)
        {
            auto& allocator = document.GetAllocator();
            value.AddMember(rapidjson::Value(key, allocator), rapidjson::Value(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), allocator), allocator);
        }

        std::string Encode(const rapidjson::Value& value)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value.Accept(writer);
            return std::string(buffer.GetString(), buffer.GetSize());
        }

        bool HashMatches(const fs::path& path, const std::string& expected)
        {
            std::string hash;
            return Hex(expected, 64) && verification::Sha256File(path, hash) && hash == expected;
        }

        bool Exists(const fs::path& path)
        {
            std::error_code ec;
            return fs::exists(path, ec) && !ec;
        }

        bool WriteBytes(const fs::path& path, const void* bytes, std::size_t length)
        {
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            bool ok = true;
            const auto* cursor = static_cast<const BYTE*>(bytes);
            while (length != 0 && ok)
            {
                const DWORD size = static_cast<DWORD>((std::min)(length, static_cast<std::size_t>(65536)));
                DWORD written = 0;
                ok = ::WriteFile(file, cursor, size, &written, nullptr) && written == size;
                cursor += size;
                length -= size;
            }
            ok = ok && ::FlushFileBuffers(file);
            ::CloseHandle(file);
            return ok;
        }

        fs::path Temporary(const fs::path& target)
        {
            return target.wstring() + L".vsmr-new-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
        }

        bool FinishTemporary(const fs::path& temporary, const fs::path& target, bool written)
        {
            if (written && ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            ::DeleteFileW(temporary.c_str());
            return false;
        }

        bool ProcessStamp(DWORD pid, std::uint64_t& stamp, bool& alive)
        {
            alive = false;
            const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (process == nullptr) return ::GetLastError() == ERROR_INVALID_PARAMETER;
            alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            FILETIME created{}, exited{}, kernel{}, user{};
            const bool ok = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
            ::CloseHandle(process);
            stamp = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            return ok;
        }
    }

    bool IsManagedPath(const std::string& path)
    {
        if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/') return false;
        const std::string lower = Lower(path);
        if (lower != "vsmr.dll" && lower.rfind("vsmr_data/", 0) != 0) return false;
        // Same closed ownership inventory as create_update_feed.ps1. User
        // backups, logs, baselines and arbitrary files are never update targets.
        static const std::regex owned(
            R"(^(vsmr\.dll|vsmr_data/(default\.json|airports_hp\.json|icao_aircraft\.json|aviso-update-policy\.json|aviso/[a-z0-9]{4}\.geojson|aircraft_icons/[a-z0-9_-]+\.png|audio/[a-z0-9_.-]+\.wav|runtime/[a-z0-9_.-]+\.dll|crashreporter/vsmrcrashhandler\.dll|tools/[a-z0-9_.-]+\.(exe|ps1|cs|patch)|licenses/[a-z0-9_.-]+\.(txt|md)|vsmr_webui/(index\.html|styles\.css|data\.js|app-bundle\.js)))$)");
        if (!std::regex_match(lower, owned)) return false;
        if (lower == "vsmr_data/version.json" || lower == "vsmr_data/profiles.json" || lower == "vsmr_data/profiles_backup.json" ||
            lower == "vsmr_data/vsmr_profiles.json") return false;
        std::size_t start = 0;
        while (start < path.size())
        {
            const auto slash = path.find('/', start);
            const std::string part = lower.substr(start, slash == std::string::npos ? slash : slash - start);
            if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
                part == "config.json" || part == "userdata" || part == ".update" || part == "user-data") return false;
            if (!std::all_of(part.begin(), part.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
            })) return false;
            const auto base = part.substr(0, part.find('.'));
            if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                (base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") && base[3] >= '1' && base[3] <= '9')) return false;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return true;
    }

    bool SafePath(const fs::path& root, const fs::path& path)
    {
        if (!root.is_absolute() || !path.is_absolute()) return false;
        const auto normalized = path.lexically_normal();
        const auto relative = normalized.lexically_relative(root.lexically_normal());
        if (relative.empty() || relative.is_absolute()) return false;
        for (const auto& part : relative) if (part == L"..") return false;
        // Do not follow a junction/symlink either at the installation root or below it.
        fs::path current = root.root_path();
        for (const auto& part : normalized.relative_path())
        {
            current /= part;
            const DWORD attributes = ::GetFileAttributesW(current.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            if (attributes == INVALID_FILE_ATTRIBUTES && ::GetLastError() != ERROR_FILE_NOT_FOUND && ::GetLastError() != ERROR_PATH_NOT_FOUND) return false;
        }
        return true;
    }

    bool ParseManifest(const std::string& json, Manifest& manifest, std::string& error)
    {
        manifest = {};
        rapidjson::Document document;
        error = "manifest_invalid";
        if (!Decode(json, document) || !document.HasMember("schema") || !document["schema"].IsUint() || document["schema"].GetUint() != 1) return false;
        manifest.version = String(document, "version");
        manifest.contentCommit = String(document, "content_commit");
        manifest.minimumLoaderVersion = String(document, "minimum_loader_version");
        if (!release_model::ParseSemVer(manifest.version).valid || !release_model::ParseSemVer(manifest.minimumLoaderVersion).valid ||
            !Hex(manifest.contentCommit, 40) || !document.HasMember("runtime_abi") || !document["runtime_abi"].IsUint() || document["runtime_abi"].GetUint() == 0 ||
            !document.HasMember("files") || !document["files"].IsObject()) return false;
        manifest.runtimeAbi = document["runtime_abi"].GetUint();
        const auto& entries = document["files"];
        if (entries.MemberCount() == 0 || entries.MemberCount() > 4096) return false;
        std::set<std::string> names;
        std::uint64_t total = 0;
        for (auto it = entries.MemberBegin(); it != entries.MemberEnd(); ++it)
        {
            File file;
            file.path.assign(it->name.GetString(), it->name.GetStringLength());
            file.sha256 = String(it->value, "sha256");
            if (!IsManagedPath(file.path) || !names.insert(Lower(file.path)).second || !Hex(file.sha256, 64) || !it->value.IsObject() ||
                !it->value.HasMember("size") || !it->value["size"].IsUint64()) return false;
            file.size = it->value["size"].GetUint64();
            if (file.size > MaximumFile || file.size > MaximumTotal - total) return false;
            total += file.size;
            manifest.files.push_back(file);
        }
        if (!names.count("vsmr.dll") || !names.count("vsmr_data/runtime/vsmr.runtime.dll") || !names.count("vsmr_data/default.json")) return false;
        // A file cannot also be an ancestor of another managed file.
        for (const auto& name : names)
            for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
                if (names.count(name.substr(0, slash))) return false;
        error.clear();
        return true;
    }

    bool ReadText(const fs::path& path, std::string& text, std::uint64_t maximumBytes)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size > maximumBytes) return false;
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        text.resize(static_cast<std::size_t>(size));
        if (size != 0 && !file.read(text.data(), static_cast<std::streamsize>(size))) return false;
        // A concurrently growing file cannot turn this bounded read into an
        // unbounded allocation or sneak a trailing JSON document past parsing.
        return file.peek() == std::char_traits<char>::eof();
    }

    bool WriteText(const fs::path& path, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) return false;
        const auto temporary = Temporary(path);
        return FinishTemporary(temporary, path, WriteBytes(temporary, text.data(), text.size()));
    }

    bool CopyAtomically(const fs::path& source, const fs::path& target)
    {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) return false;
        const auto temporary = Temporary(target);
        if (!::CopyFileW(source.c_str(), temporary.c_str(), TRUE)) return false;
        const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool flushed = file != INVALID_HANDLE_VALUE && ::FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
        return FinishTemporary(temporary, target, flushed);
    }

    bool Matches(const fs::path& path, const File& file)
    {
        std::error_code ec;
        return fs::file_size(path, ec) == file.size && !ec && HashMatches(path, file.sha256);
    }

    fs::path UpdateRoot(const fs::path& root) { return root / L"vSMR_Data" / L".update"; }

    bool ValidateDefaults(const Transaction& tx, std::string& error)
    {
        error = "default_json_invalid";
        const auto file = std::find_if(tx.target.files.begin(), tx.target.files.end(), [](const File& entry) { return Lower(entry.path) == "vsmr_data/default.json"; });
        if (file == tx.target.files.end()) return false;
        const bool changed = std::any_of(tx.changes.begin(), tx.changes.end(), [&](const Change& change) { return change.file.path == file->path; });
        const auto path = (changed ? UpdateRoot(tx.root) / L"new" : tx.root) / fs::u8path(file->path);
        std::string json;
        rapidjson::Document doc;
        if (!SafePath(tx.root, path) || !ReadText(path, json, 16ULL * 1024 * 1024)) return false;
        VsmrJson::ParseDocument(doc, json);
        if (doc.HasParseError() || !doc.IsObject() || !UniqueMembers(doc) || !doc.HasMember("schema_version") ||
            !doc["schema_version"].IsUint() || doc["schema_version"].GetUint() == 0 || !doc.HasMember("profiles") ||
            !doc["profiles"].IsObject() || doc["profiles"].ObjectEmpty() || doc["profiles"].MemberCount() > 256) return false;
        for (const auto* section : { "metadata", "hidden_profiles", "_migration", "asset_hashes" })
            if (doc.HasMember(section) && !doc[section].IsObject()) return false;
        std::set<std::string> names;
        for (auto it = doc["profiles"].MemberBegin(); it != doc["profiles"].MemberEnd(); ++it)
        {
            const std::string id(it->name.GetString(), it->name.GetStringLength());
            const auto name = String(it->value, "name");
            if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            }) || !it->value.IsObject() || name.empty() || name.size() > 256 || !names.insert(Lower(name)).second) return false;
        }
        if (doc.HasMember("hidden_profiles"))
            for (auto it = doc["hidden_profiles"].MemberBegin(); it != doc["hidden_profiles"].MemberEnd(); ++it) if (!it->value.IsBool()) return false;
        if (doc.HasMember("asset_hashes"))
            for (auto it = doc["asset_hashes"].MemberBegin(); it != doc["asset_hashes"].MemberEnd(); ++it)
                if (!it->value.IsString() || !Hex(std::string(it->value.GetString(), it->value.GetStringLength()), 64)) return false;
        error.clear();
        return true;
    }

    bool IsCommitted(const Transaction& tx)
    {
        std::string committed;
        const auto path = tx.root / L"vSMR_Data/version.json";
        if (!SafePath(tx.root, path) || !ReadText(path, committed) || committed != tx.targetJson) return false;
        for (const auto& file : tx.target.files)
            if (!SafePath(tx.root, tx.root / fs::u8path(file.path)) || !Matches(tx.root / fs::u8path(file.path), file)) return false;
        return true;
    }

    bool SaveTransaction(const Transaction& tx, std::string& error)
    {
        const auto directory = UpdateRoot(tx.root);
        if (!SafePath(tx.root, directory)) { error = "unsafe_staging_path"; return false; }
        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        doc.AddMember("schema", 1, a);
        Add(doc, doc, "install_root", tx.root.u8string());
        Add(doc, doc, "phase", tx.phase);
        Add(doc, doc, "content_commit", tx.target.contentCommit);
        Add(doc, doc, "version", tx.target.version);
        Add(doc, doc, "previous_version", tx.previousVersion);
        doc.AddMember("previous_manifest_existed", tx.previousManifestExisted, a);
        Add(doc, doc, "previous_manifest_sha256", tx.previousManifestHash);
        doc.AddMember("owner_pid", tx.ownerPid, a);
        doc.AddMember("owner_created", tx.ownerCreated, a);
        rapidjson::Value entries(rapidjson::kArrayType);
        for (const auto& change : tx.changes)
        {
            rapidjson::Value item(rapidjson::kObjectType);
            Add(doc, item, "path", change.file.path);
            item.AddMember("existed", change.existed, a);
            Add(doc, item, "previous_sha256", change.previousHash);
            entries.PushBack(item, a);
        }
        doc.AddMember("changes", entries, a);
        if (!WriteText(directory / L"journal.json", Encode(doc))) { error = "journal_write_failed"; return false; }
        return true;
    }

    bool LoadTransaction(const fs::path& root, Transaction& tx, std::string& error)
    {
        tx = {};
        tx.root = root;
        const auto directory = UpdateRoot(root);
        error = "journal_invalid";
        std::string json;
        rapidjson::Document doc;
        if (!SafePath(root, directory) || !ReadText(directory / L"journal.json", json) || !Decode(json, doc) ||
            String(doc, "install_root") != root.u8string() || !ReadText(directory / L"target.json", tx.targetJson) || !ParseManifest(tx.targetJson, tx.target, error) ||
            String(doc, "content_commit") != tx.target.contentCommit || String(doc, "version") != tx.target.version) return false;
        error = "journal_invalid";
        if (!doc.HasMember("schema") || !doc["schema"].IsUint() || doc["schema"].GetUint() != 1) return false;
        tx.phase = String(doc, "phase");
        const std::set<std::string> phases{ "ready", "installing", "installed", "healthy", "failed", "rolling_back" };
        if (!phases.count(tx.phase) || !doc.HasMember("changes") || !doc["changes"].IsArray() || doc["changes"].Size() > tx.target.files.size() ||
            !doc.HasMember("previous_manifest_existed") || !doc["previous_manifest_existed"].IsBool() ||
            !doc.HasMember("owner_pid") || !doc["owner_pid"].IsUint() || !doc.HasMember("owner_created") || !doc["owner_created"].IsUint64()) return false;
        tx.previousVersion = String(doc, "previous_version");
        tx.previousManifestExisted = doc["previous_manifest_existed"].GetBool();
        tx.previousManifestHash = String(doc, "previous_manifest_sha256");
        tx.ownerPid = doc["owner_pid"].GetUint();
        tx.ownerCreated = doc["owner_created"].GetUint64();
        if (tx.phase != "ready" && tx.previousManifestExisted && !Hex(tx.previousManifestHash, 64)) return false;
        std::set<std::string> names;
        for (const auto& item : doc["changes"].GetArray())
        {
            const auto name = String(item, "path");
            const auto file = std::find_if(tx.target.files.begin(), tx.target.files.end(), [&](const File& candidate) { return candidate.path == name; });
            if (file == tx.target.files.end() || !names.insert(Lower(name)).second || !item.IsObject() || !item.HasMember("existed") || !item["existed"].IsBool()) return false;
            Change change{ *file, item["existed"].GetBool(), String(item, "previous_sha256") };
            if (tx.phase != "ready" && change.existed && !Hex(change.previousHash, 64)) return false;
            if (!SafePath(root, root / fs::u8path(name)) || !SafePath(root, directory / L"backup" / fs::u8path(name)) || !SafePath(root, directory / L"new" / fs::u8path(name))) return false;
            tx.changes.push_back(change);
        }
        error.clear();
        return true;
    }

    bool Apply(Transaction& tx, std::string& error, const std::function<void(const File&)>& beforeReplaceForTest)
    {
        const auto directory = UpdateRoot(tx.root);
        const auto localManifest = tx.root / L"vSMR_Data" / L"version.json";
        error = "staged_file_invalid";
        if (tx.phase != "ready" || !SafePath(tx.root, directory) || !SafePath(tx.root, localManifest)) return false;
        if (!ValidateDefaults(tx, error)) return false;
        for (const auto& change : tx.changes)
        {
            const auto path = fs::u8path(change.file.path);
            if (!SafePath(tx.root, tx.root / path) || !SafePath(tx.root, directory / L"new" / path) || !Matches(directory / L"new" / path, change.file)) return false;
            if (Exists(tx.root / path))
            {
                const HANDLE target = ::CreateFileW((tx.root / path).c_str(), GENERIC_WRITE | DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (target == INVALID_HANDLE_VALUE) { error = "target_locked"; return false; }
                ::CloseHandle(target);
            }
        }
        // Back up only changed files. All backups are complete and durable before
        // installing is journaled, so crash recovery never needs the network.
        error = "backup_failed";
        for (auto& change : tx.changes)
        {
            const auto path = fs::u8path(change.file.path);
            change.existed = Exists(tx.root / path);
            if (change.existed && (!verification::Sha256File(tx.root / path, change.previousHash) ||
                !SafePath(tx.root, directory / L"backup" / path) || !CopyAtomically(tx.root / path, directory / L"backup" / path) ||
                !HashMatches(directory / L"backup" / path, change.previousHash))) return false;
        }
        tx.previousManifestExisted = Exists(localManifest);
        if (tx.previousManifestExisted && (!verification::Sha256File(localManifest, tx.previousManifestHash) ||
            !CopyAtomically(localManifest, directory / L"old-version.json") || !HashMatches(directory / L"old-version.json", tx.previousManifestHash))) return false;
        tx.phase = "installing";
        if (!SaveTransaction(tx, error)) return false;
        error = "replace_failed";
        for (const auto& change : tx.changes)
        {
            const auto path = fs::u8path(change.file.path);
            if (beforeReplaceForTest) beforeReplaceForTest(change.file);
            if (!SafePath(tx.root, tx.root / path) || !CopyAtomically(directory / L"new" / path, tx.root / path) || !Matches(tx.root / path, change.file)) return false;
        }
        error = "installed_hash_mismatch";
        for (const auto& file : tx.target.files)
            if (!SafePath(tx.root, tx.root / fs::u8path(file.path)) || !Matches(tx.root / fs::u8path(file.path), file)) return false;
        // The version manifest is the LAST managed installation write.
        error = "manifest_commit_failed";
        if (!WriteText(localManifest, tx.targetJson)) return false;
        tx.phase = "installed";
        tx.ownerPid = 0;
        tx.ownerCreated = 0;
        return SaveTransaction(tx, error);
    }

    bool Rollback(Transaction& tx, std::string& error)
    {
        if (tx.phase == "ready") return Cleanup(tx);
        const auto directory = UpdateRoot(tx.root);
        const auto localManifest = tx.root / L"vSMR_Data" / L"version.json";
        error = "rollback_backup_invalid";
        // Validate EVERY backup before replacing any files.
        for (const auto& change : tx.changes)
        {
            const auto path = fs::u8path(change.file.path);
            if (!SafePath(tx.root, tx.root / path) || !SafePath(tx.root, directory / L"backup" / path) ||
                (change.existed && !HashMatches(directory / L"backup" / path, change.previousHash))) return false;
        }
        if (!SafePath(tx.root, localManifest) || (tx.previousManifestExisted && !HashMatches(directory / L"old-version.json", tx.previousManifestHash))) return false;
        tx.phase = "rolling_back";
        if (!SaveTransaction(tx, error)) return false;
        error = "rollback_replace_failed";
        for (auto it = tx.changes.rbegin(); it != tx.changes.rend(); ++it)
        {
            const auto path = fs::u8path(it->file.path);
            if (it->existed)
            {
                if (!HashMatches(tx.root / path, it->previousHash) && !CopyAtomically(directory / L"backup" / path, tx.root / path)) return false;
            }
            else if (Exists(tx.root / path))
            {
                if (!Matches(tx.root / path, it->file) || !::DeleteFileW((tx.root / path).c_str())) return false;
            }
        }
        if (tx.previousManifestExisted)
        {
            if (!CopyAtomically(directory / L"old-version.json", localManifest) || !HashMatches(localManifest, tx.previousManifestHash)) return false;
        }
        else if (Exists(localManifest) && !::DeleteFileW(localManifest.c_str())) return false;
        return Cleanup(tx);
    }

    bool Cleanup(const Transaction& tx)
    {
        const auto directory = UpdateRoot(tx.root);
        if (!SafePath(tx.root, directory)) return false;
        // Delete journal first only after successful rollback/healthy commit.
        // Never recursively delete this directory: a copied applier may be running.
        if (Exists(directory / L"journal.json") && !::DeleteFileW((directory / L"journal.json").c_str())) return false;
        for (const auto& change : tx.changes)
            for (const auto* subdirectory : { L"new", L"backup" })
            {
                const auto path = directory / subdirectory / fs::u8path(change.file.path);
                if (SafePath(tx.root, path)) ::DeleteFileW(path.c_str());
            }
        ::DeleteFileW((directory / L"target.json").c_str());
        ::DeleteFileW((directory / L"old-version.json").c_str());
        return true;
    }

    bool MarkAttempt(Transaction& tx, std::string& error)
    {
        tx.ownerPid = ::GetCurrentProcessId();
        bool alive = false;
        if (!ProcessStamp(tx.ownerPid, tx.ownerCreated, alive) || !alive) { error = "process_identity_unavailable"; return false; }
        return SaveTransaction(tx, error);
    }

    bool OwnerAlive(const Transaction& tx)
    {
        if (tx.ownerPid == 0) return false;
        std::uint64_t created = 0;
        bool alive = false;
        // Unknown identity must never cause a rollback beneath another process.
        if (!ProcessStamp(tx.ownerPid, created, alive)) return true;
        return alive && created == tx.ownerCreated;
    }
}
