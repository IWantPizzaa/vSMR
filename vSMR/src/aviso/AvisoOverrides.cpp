#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoOverrides.hpp"
#include "aviso/AvisoSharedConfig.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoFeatureMetadata.hpp"
#include "config/LayeredConfig.hpp"
#include "config/RuntimeConfig.hpp"
#include "shared/JsonDocument.hpp"
#include "shared/logging/Logger.hpp"
#include "updater/UpdaterVerification.hpp"
#include "rapidjson/pointer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

namespace VsmrAvisoOverrides
{
    namespace
    {
        using Value = rapidjson::Value;
        using Document = rapidjson::Document;
        using Allocator = Document::AllocatorType;
        using VsmrLayeredConfig::Member;
        using VsmrLayeredConfig::Put;

        std::string Upper(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return text;
        }

        std::string Identity(const Value& value, bool feature)
        {
            if (feature) return VsmrAvisoFeatureMetadata::ReadFeatureIdentity(value);
            const Value* id = Member(value, "id");
            if (id == nullptr) id = Member(value, "group_id");
            return id != nullptr && id->IsString() ? id->GetString() : "";
        }

        bool Normalize(const Value& source, Document& target, std::string& error)
        {
            target.SetObject();
            if (!source.IsObject() || !source.HasMember("features") || !source["features"].IsArray())
            {
                error = "AVISO overrides require a GeoJSON FeatureCollection.";
                return false;
            }
            auto& allocator = target.GetAllocator();
            Value root(source, allocator);
            root.RemoveMember("features");
            root.RemoveMember("vsmr_groups");
            target.AddMember("document", root, allocator);
            for (const bool features : {true, false})
            {
                const char* sourceKey = features ? "features" : "vsmr_groups";
                const char* targetKey = features ? "features" : "groups";
                Value keyed(rapidjson::kObjectType);
                const Value* items = Member(source, sourceKey);
                if (items != nullptr)
                {
                    if (!items->IsArray())
                    {
                        error = "AVISO feature/group collections must be arrays.";
                        return false;
                    }
                    for (const auto& item : items->GetArray())
                    {
                        const std::string id = Identity(item, features);
                        if (id.empty() || keyed.HasMember(id.c_str()))
                        {
                            error = "Stable unique AVISO feature/group IDs are required for sparse overrides.";
                            return false;
                        }
                        Put(keyed, id.c_str(), item, allocator);
                    }
                }
                Put(target, targetKey, keyed, allocator);
            }
            return true;
        }

        std::string EscapePointer(const std::string& key)
        {
            std::string escaped;
            for (const char c : key)
                escaped += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
            return escaped;
        }

        void ClearRemoval(Value& removed, const std::string& path)
        {
            if (!removed.IsObject()) return;
            for (auto it = removed.MemberBegin(); it != removed.MemberEnd();)
            {
                const std::string candidate = it->name.GetString();
                if (candidate == path || candidate.rfind(path + "/", 0) == 0)
                    it = removed.EraseMember(it);
                else ++it;
            }
        }

        // Deletion is an explicit map-edit operation, separate from JSON null.
        // Keeping tombstones outside the patch leaves generic merge semantics
        // (missing=inherited, null=literal) unchanged.
        void TrackRemovals(const Value* defaults, const Value* before,
            const Value& after, Value& removed, Allocator& allocator,
            const std::string& path = "")
        {
            if (before != nullptr && *before == after) return;
            if (!after.IsObject() || before == nullptr || !before->IsObject())
            {
                ClearRemoval(removed, path);
                return;
            }
            for (auto it = before->MemberBegin(); it != before->MemberEnd(); ++it)
            {
                const std::string child = path + "/" + EscapePointer(it->name.GetString());
                if (!after.HasMember(it->name.GetString()))
                {
                    ClearRemoval(removed, child);
                    if (defaults != nullptr && Member(*defaults, it->name.GetString()) != nullptr)
                    {
                        Value yes(true);
                        Put(removed, child.c_str(), yes, allocator);
                    }
                }
            }
            for (auto it = after.MemberBegin(); it != after.MemberEnd(); ++it)
            {
                const char* key = it->name.GetString();
                const std::string child = path + "/" + EscapePointer(key);
                TrackRemovals(defaults != nullptr ? Member(*defaults, key) : nullptr,
                    Member(*before, key), it->value, removed, allocator, child);
            }
        }

        bool ReadJson(const std::filesystem::path& path, Document& result, std::string& error)
        {
            std::string text;
            return AvisoDocumentModel::ReadBoundedSourceFile(path, text, error) &&
                !VsmrJson::ParseDocument(result, text).HasParseError() && result.IsObject();
        }

        bool ReadVerifiedBaseline(const std::filesystem::path& dataRoot,
            const Value& index, const std::string& filename, Document& baseline)
        {
            const Value* files = Member(index, "files");
            const std::string relative = "AVISO/" + filename;
            const Value* expected = files != nullptr ? Member(*files, relative.c_str()) : nullptr;
            if (expected == nullptr || !expected->IsString() || expected->GetStringLength() != 64) return false;
            const auto path = dataRoot / "UpdateBaselines" / "AVISO" / std::filesystem::u8path(filename);
            std::string hash, error;
            return vsmr::updater::verification::Sha256File(path, hash) &&
                hash == expected->GetString() && ReadJson(path, baseline, error);
        }
    }

    std::string AirportKey(const std::filesystem::path& source)
    {
        const std::string key = Upper(source.stem().u8string());
        if (key.size() != 4 || !std::all_of(key.begin(), key.end(),
            [](unsigned char c) { return std::isalnum(c) != 0; })) return {};
        return key;
    }

    bool IsManagedSource(const std::filesystem::path& dataRoot, const std::filesystem::path& source)
    {
        try
        {
            if (AirportKey(source).empty() || Upper(source.extension().u8string()) != ".GEOJSON") return false;
            const auto expected = std::filesystem::weakly_canonical(dataRoot / "AVISO");
            const auto parent = std::filesystem::weakly_canonical(source.parent_path());
            return _wcsicmp(expected.c_str(), parent.c_str()) == 0;
        }
        catch (...) { return false; }
    }

    const Value* Find(const CConfig& config, const std::string& airport)
    {
        const Value* section = config.getEffectiveConfigSection("aviso");
        return section != nullptr ? Member(*section, Upper(airport).c_str()) : nullptr;
    }

    std::string CustomSource(const CConfig& config, const std::string& airport,
        const std::filesystem::path& dataRoot)
    {
        const auto shared = !config.isLayeredConfig()
            ? VsmrAvisoSharedConfig::Read(dataRoot) : VsmrAvisoSharedConfig::Snapshot{};
        const Value* sharedSection = shared.document != nullptr ? Member(*shared.document, "aviso") : nullptr;
        const Value* settings = config.isLayeredConfig() ? Find(config, airport)
            : (sharedSection != nullptr ? Member(*sharedSection, Upper(airport).c_str()) : nullptr);
        const Value* source = settings != nullptr ? Member(*settings, "custom_file") : nullptr;
        if (source == nullptr || !source->IsString() || source->GetStringLength() == 0) return {};
        const auto path = std::filesystem::u8path(source->GetString());
        return (path.is_absolute() ? path : dataRoot / path).lexically_normal().u8string();
    }

    bool Apply(Document& source, const Value* overrides, std::string& error)
    {
        if (overrides == nullptr) return true;
        if (!overrides->IsObject()) { error = "AVISO overrides must be an object."; return false; }
        const Value* patch = Member(*overrides, "patch");
        const Value* removed = Member(*overrides, "removed");
        if (patch == nullptr && removed == nullptr) return true;
        if ((patch != nullptr && !patch->IsObject()) || (removed != nullptr && !removed->IsObject()))
        {
            error = "Invalid AVISO patch/removal section.";
            return false;
        }
        Document keyed;
        if (!Normalize(source, keyed, error)) return false;
        if (patch != nullptr) VsmrLayeredConfig::Merge(keyed, *patch, keyed.GetAllocator());
        if (removed != nullptr)
        {
            for (auto it = removed->MemberBegin(); it != removed->MemberEnd(); ++it)
            {
                const std::string path(it->name.GetString(), it->name.GetStringLength());
                rapidjson::Pointer pointer(path.c_str(), path.size());
                if (!it->value.IsBool() || !it->value.GetBool() || !pointer.IsValid() ||
                    (path.rfind("/document/", 0) != 0 && path.rfind("/features/", 0) != 0 && path.rfind("/groups/", 0) != 0))
                {
                    error = "Invalid AVISO removal path.";
                    return false;
                }
                pointer.Erase(keyed);
            }
        }
        const Value* root = Member(keyed, "document");
        if (root == nullptr || !root->IsObject()) { error = "Invalid AVISO document override."; return false; }
        Document result;
        result.CopyFrom(*root, result.GetAllocator());
        for (const bool features : {true, false})
        {
            const char* key = features ? "features" : "groups";
            const Value* entries = Member(keyed, key);
            if (entries == nullptr || !entries->IsObject()) { error = "Invalid keyed AVISO collection."; return false; }
            Value array(rapidjson::kArrayType);
            for (auto it = entries->MemberBegin(); it != entries->MemberEnd(); ++it)
            {
                // A patch for a removed upstream feature must not resurrect an
                // incomplete object. Keep the override on disk for later review.
                const Value& item = it->value;
                if (Identity(item, features).empty() || (features && Member(item, "geometry") == nullptr))
                {
                    Logger::info("Retained orphan AVISO override: " + std::string(it->name.GetString()));
                    continue;
                }
                Value copy(item, result.GetAllocator());
                array.PushBack(copy, result.GetAllocator());
            }
            Put(result, features ? "features" : "vsmr_groups", array, result.GetAllocator());
        }
        source.Swap(result);
        return true;
    }

    std::string DefaultSource(const CConfig& config, const std::filesystem::path& dataRoot,
        const std::filesystem::path& source, std::string& error)
    {
        error.clear();
        if (!IsManagedSource(dataRoot, source)) return source.u8string();
        const auto shared = !config.isLayeredConfig()
            ? VsmrAvisoSharedConfig::Read(dataRoot) : VsmrAvisoSharedConfig::Snapshot{};
        if (!shared.error.empty()) { error = shared.error; return {}; }
        const Value* migration = config.isLayeredConfig() ? config.getUserConfigSection("_migration")
            : (shared.document != nullptr ? Member(*shared.document, "_migration") : nullptr);
        const Value* baselines = migration != nullptr ? Member(*migration, "aviso_baselines") : nullptr;
        const std::string airport = AirportKey(source);
        const Value* baseline = baselines != nullptr ? Member(*baselines, airport.c_str()) : nullptr;
        if (baseline == nullptr) return source.u8string();
        const Value* oldHash = Member(*baseline, "source_sha256");
        const Value* baseHash = Member(*baseline, "baseline_sha256");
        if (oldHash == nullptr || !oldHash->IsString() || baseHash == nullptr || !baseHash->IsString())
        { error = "Invalid legacy AVISO baseline metadata."; return {}; }
        std::string actualHash;
        if (!vsmr::updater::verification::Sha256File(source, actualHash))
        { error = "Cannot verify the official AVISO source."; return {}; }
        if (actualHash != oldHash->GetString()) return source.u8string();
        const std::string expected = baseHash->GetString();
        if (expected.size() != 64 || !std::all_of(expected.begin(), expected.end(),
            [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        { error = "Invalid legacy AVISO baseline hash."; return {}; }
        // Derive the only allowed backup filename instead of trusting a path
        // embedded in user JSON. It is immutable and outside updater ownership.
        const auto path = dataRoot / "UserData" / "Baselines" / (airport + "-" + expected + ".geojson");
        std::string hash;
        if (!vsmr::updater::verification::Sha256File(path, hash) || hash != expected)
        { error = "The preserved AVISO baseline is missing or changed. Restore its backup before editing."; return {}; }
        return path.u8string();
    }

    bool Build(const Value& defaults, const Value& previous, const Value& edited,
        const Value* existing, Document& result, std::string& error)
    {
        Document base, before, after;
        if (!Normalize(defaults, base, error) || !Normalize(previous, before, error) ||
            !Normalize(edited, after, error)) return false;
        result.SetObject();
        if (existing != nullptr)
        {
            if (!existing->IsObject()) { error = "Invalid existing AVISO overrides."; return false; }
            result.CopyFrom(*existing, result.GetAllocator());
        }
        auto& allocator = result.GetAllocator();
        Value patch(rapidjson::kObjectType), removed(rapidjson::kObjectType);
        if (const Value* saved = Member(result, "patch")) patch.CopyFrom(*saved, allocator);
        if (const Value* saved = Member(result, "removed")) removed.CopyFrom(*saved, allocator);
        if (!patch.IsObject() || !removed.IsObject()) { error = "Invalid existing AVISO patch."; return false; }
        VsmrLayeredConfig::ApplyEdits(&base, &before, after, patch, allocator);
        TrackRemovals(&base, &before, after, removed, allocator);
        result.RemoveMember("patch");
        result.RemoveMember("removed");
        if (patch.MemberCount() != 0) Put(result, "patch", patch, allocator);
        if (removed.MemberCount() != 0) Put(result, "removed", removed, allocator);
        return true;
    }

    bool MigrateLegacy(CConfig& config, const std::filesystem::path& dataRoot, std::string& error)
    {
        if (!config.isLayeredConfig() || !config.isConfigHealthy()) return true;
        static std::mutex migrationMutex;
        std::lock_guard<std::mutex> guard(migrationMutex);
        const Value* marker = config.getUserConfigSection("_migration");
        const Value* ready = marker != nullptr ? Member(*marker, "legacy_aviso") : nullptr;
        if (ready != nullptr && ready->IsBool() && ready->GetBool()) return true;
        try
        {
            Document sections(rapidjson::kObjectType);
            Value migration(rapidjson::kObjectType);
            if (marker != nullptr && marker->IsObject()) migration.CopyFrom(*marker, sections.GetAllocator());
            Value baselineRecords(rapidjson::kObjectType);
            if (const Value* saved = Member(migration, "aviso_baselines"))
                baselineRecords.CopyFrom(*saved, sections.GetAllocator());
            if (!baselineRecords.IsObject()) { error = "Invalid legacy AVISO baselines."; return false; }
            Value airports(rapidjson::kObjectType);
            if (const Value* saved = config.getUserConfigSection("aviso"))
            {
                if (!saved->IsObject()) { error = "Invalid AVISO configuration section."; return false; }
                airports.CopyFrom(*saved, sections.GetAllocator());
            }
            Document index;
            std::string ignored;
            ReadJson(dataRoot / "UpdateBaselines" / "BASELINE-HASHES.json", index, ignored);
            const auto directory = dataRoot / "AVISO";
            if (std::filesystem::is_directory(directory))
            {
                for (const auto& file : std::filesystem::directory_iterator(directory))
                {
                    if (!file.is_regular_file() || !IsManagedSource(dataRoot, file.path())) continue;
                    const std::string airport = AirportKey(file.path());
                    if (airports.HasMember(airport.c_str())) continue;
                    const Value* hashes = config.getDefaultConfigSection("asset_hashes");
                    const std::string asset = "AVISO/" + file.path().filename().u8string();
                    const Value* cleanHash = hashes != nullptr ? Member(*hashes, asset.c_str()) : nullptr;
                    std::string installedHash;
                    if (cleanHash != nullptr && cleanHash->IsString() &&
                        vsmr::updater::verification::Sha256File(file.path(), installedHash) &&
                        installedHash == cleanHash->GetString()) continue;
                    Document installed, baseline, overrides;
                    if (!ReadJson(file.path(), installed, error))
                    {
                        error = "Cannot preserve legacy AVISO " + airport + ": " + error;
                        return false;
                    }
                    const bool verifiedBaseline = ReadVerifiedBaseline(dataRoot, index,
                        file.path().filename().u8string(), baseline);
                    if (verifiedBaseline && baseline == installed) continue;
                    if (verifiedBaseline && Build(baseline, baseline, installed, nullptr, overrides, error))
                    {
                        Document check;
                        check.CopyFrom(baseline, check.GetAllocator());
                        Document normalizedInstalled, normalizedCheck;
                        if (Apply(check, &overrides, error) && Normalize(installed, normalizedInstalled, error) &&
                            Normalize(check, normalizedCheck, error) && normalizedInstalled == normalizedCheck)
                        {
                            const auto oldBase = dataRoot / "UpdateBaselines" / "AVISO" / file.path().filename();
                            std::string baseHash, sourceHash;
                            if (!vsmr::updater::verification::Sha256File(oldBase, baseHash) ||
                                !vsmr::updater::verification::Sha256File(file.path(), sourceHash))
                            { error = "Cannot preserve legacy AVISO baseline hashes."; return false; }
                            const auto backup = dataRoot / "UserData" / "Baselines" / (airport + "-" + baseHash + ".geojson");
                            std::filesystem::create_directories(backup.parent_path());
                            if (!std::filesystem::exists(backup))
                                std::filesystem::copy_file(oldBase, backup, std::filesystem::copy_options::none);
                            std::string backupHash;
                            if (!vsmr::updater::verification::Sha256File(backup, backupHash) || baseHash != backupHash)
                            { error = "Legacy AVISO baseline copy verification failed."; return false; }
                            Value record(rapidjson::kObjectType);
                            Value beforeHash(sourceHash.c_str(), sections.GetAllocator());
                            Value defaultHash(baseHash.c_str(), sections.GetAllocator());
                            record.AddMember("source_sha256", beforeHash, sections.GetAllocator());
                            record.AddMember("baseline_sha256", defaultHash, sections.GetAllocator());
                            Put(baselineRecords, airport.c_str(), record, sections.GetAllocator());
                            Put(airports, airport.c_str(), overrides, sections.GetAllocator());
                            continue;
                        }
                    }
                    // No matching baseline: intent cannot be inferred. Preserve
                    // exact bytes in a never-overwritten, content-addressed copy.
                    std::string hash;
                    if (!vsmr::updater::verification::Sha256File(file.path(), hash))
                    { error = "Cannot hash legacy AVISO " + airport; return false; }
                    const auto relative = std::filesystem::path("UserData") / "Maps" /
                        (airport + "-legacy-" + hash + ".geojson");
                    const auto copy = dataRoot / relative;
                    std::filesystem::create_directories(copy.parent_path());
                    if (!std::filesystem::exists(copy))
                        std::filesystem::copy_file(file.path(), copy, std::filesystem::copy_options::none);
                    std::string copyHash;
                    if (!vsmr::updater::verification::Sha256File(copy, copyHash) || hash != copyHash)
                    { error = "Legacy AVISO backup verification failed for " + airport; return false; }
                    overrides.SetObject();
                    const std::string source = relative.generic_u8string();
                    Value value(source.c_str(), static_cast<rapidjson::SizeType>(source.size()), overrides.GetAllocator());
                    overrides.AddMember("custom_file", value, overrides.GetAllocator());
                    Put(airports, airport.c_str(), overrides, sections.GetAllocator());
                    Logger::info("Preserved legacy AVISO as a custom map (no usable matching baseline): " + airport);
                }
            }
            if (!baselineRecords.ObjectEmpty()) Put(migration, "aviso_baselines", baselineRecords, sections.GetAllocator());
            Value yes(true);
            Put(migration, "legacy_aviso", yes, sections.GetAllocator());
            Put(sections, "aviso", airports, sections.GetAllocator());
            Put(sections, "_migration", migration, sections.GetAllocator());
            error.clear();
            return config.saveConfig({}, config.getConfigRevision(), &error, false, &sections);
        }
        catch (const std::exception& exception)
        { error = std::string("Legacy AVISO migration failed: ") + exception.what(); return false; }
    }
}
