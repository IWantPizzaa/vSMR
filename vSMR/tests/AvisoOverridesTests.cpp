#include <Windows.h>
#include <objidl.h>
#include "AvisoOverridesTests.hpp"
#include "aviso/AvisoOverrides.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoSharedConfig.hpp"
#include "config/LayeredConfig.hpp"
#include "config/RuntimeConfig.hpp"
#include "shared/JsonDocument.hpp"
#include "updater/UpdaterVerification.hpp"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <fstream>
#include <sstream>

namespace
{
    using rapidjson::Document;
    using rapidjson::Value;

    Document Map()
    {
        Document value;
        VsmrJson::ParseDocument(value, R"({"type":"FeatureCollection","metadata":{"icao":"LFXX"},"styles":{"a":{"paint":{"color":[1,2,3],"width":1}}},"vsmr_groups":[{"id":"g","name":"Original"}],"features":[{"type":"Feature","id":"f1","properties":{"name":"One","visible":true},"geometry":{"type":"Point","coordinates":[1,2]}},{"type":"Feature","id":"f2","properties":{"name":"Two"},"geometry":{"type":"Point","coordinates":[3,4]}}]})");
        return value;
    }

    void Write(const std::filesystem::path& path, const Value& value)
    {
        std::filesystem::create_directories(path.parent_path());
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        std::ofstream output(path, std::ios::binary);
        output.write(buffer.GetString(), buffer.GetSize());
        if (!output) throw std::runtime_error("AVISO test fixture write failed");
    }

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }
}

std::vector<std::string> RunAvisoOverridesTests(const std::filesystem::path& repositoryRoot)
{
    std::vector<std::string> failures;
    auto expect = [&](bool passed, const char* label) { if (!passed) failures.emplace_back(label); };
    const std::string preferredPalette = "real";
    expect(AvisoDocumentModel::ResolveColorPalette(preferredPalette, { "dark", "light", "real" }) == "real" &&
        AvisoDocumentModel::ResolveColorPalette(preferredPalette, { "dark", "light" }) == "light" &&
        AvisoDocumentModel::ResolveColorPalette(preferredPalette, { "dark", "light", "real" }) == "real" && preferredPalette == "real",
        "LFPG Real to LFPN Light to LFPG Real preserves the requested palette across an unsupported airport");
    {
        Document baseline, requested, changes;
        baseline.Parse(R"({"metadata":{"background_colors":{"dark":"#123456","light":"#abcdef"}},"styles":{"a":{"paint":{"fill":"#112233","stroke":"#222222","palette-overrides":{"light":{"fill":"#334455"}}}}}})");
        requested.Parse(R"({"palette":"light","styles":{"a":{"paint":{"fill-opacity":1,"palette-overrides":{"light":{"fill":"#334455"}}}}},"metadata":{"background_colors":{"light":"#ABCDEF"},"background_opacities":{"light":1}}})");
        std::string error;
        expect(VsmrAvisoOverrides::BuildAppearanceChanges(requested, baseline, changes, error) && changes.MemberCount() == 1,
            "Global appearance omits unchanged colors, case-only differences and implicit opaque defaults");
        requested["styles"]["a"]["paint"]["fill-opacity"].SetDouble(0.4);
        requested["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].SetString("#654321");
        expect(VsmrAvisoOverrides::BuildAppearanceChanges(requested, baseline, changes, error) &&
            changes["styles"]["a"]["paint"].MemberCount() == 2 && !changes.HasMember("metadata"),
            "Global appearance stores only edited fields rather than the whole airport theme");
        Document shared;
        shared.Parse(R"({"palette":"light","styles":{"a":{"paint":{"fill-opacity":0.2,"text-size":15,"palette-overrides":{"light":{"fill":"#999999","stroke":"#888888"},"real":{"fill":"#777777"}}}}},"metadata":{"background_opacities":{"light":0.5}}})");
        VsmrAvisoOverrides::MergeAppearanceChanges(shared, changes);
        expect(shared["styles"]["a"]["paint"]["fill-opacity"].GetDouble() == 0.4 &&
            !shared["styles"]["a"]["paint"].HasMember("text-size") && !shared.HasMember("metadata") &&
            !shared["styles"]["a"]["paint"]["palette-overrides"]["light"].HasMember("stroke") &&
            shared["styles"]["a"]["paint"]["palette-overrides"].HasMember("real"),
            "Applying a new sparse preset removes stale same-palette settings while preserving other palettes");
        requested["palette"].SetString("real");
        requested["styles"]["a"]["paint"].AddMember("text-size", 18, requested.GetAllocator());
        expect(VsmrAvisoOverrides::BuildAppearanceChanges(requested, baseline, changes, error) &&
            changes["styles"]["a"]["paint"].HasMember("text-size") && !changes["styles"]["a"]["paint"].HasMember("palette-overrides"),
            "Real global application carries text and geometry edits, never foreign Real colors");
        Document original = Map(), actual = Map();
        expect(VsmrAvisoOverrides::ApplyAppearance(actual, &changes, error) && actual["features"] == original["features"] &&
            actual["styles"]["a"]["paint"]["color"] == original["styles"]["a"]["paint"]["color"] &&
            actual["styles"]["a"]["paint"]["text-size"].GetInt() == 18,
            "Real keeps each airport's geometry and native colors while applying common text edits");
    }
    {
        Document appearance, source, old;
        source.Parse(R"({"type":"FeatureCollection","metadata":{"airport":"LFXX"},"styles":{"a":{"paint":{"fill":"#111111","palette-overrides":{"light":{"fill":"#222222"}}}}},"vsmr_groups":[{"id":"local","visible":false}],"features":[{"type":"Feature","id":"f1","properties":{"style_id":"a"},"geometry":{"type":"Point","coordinates":[1,2]}}]})");
        old.CopyFrom(source, old.GetAllocator());
        appearance.Parse(R"({"palette":"light","metadata":{"background_opacities":{"light":0.4}},"styles":{"a":{"paint":{"palette-overrides":{"light":{"fill":"#ABCDEF"}},"fill-opacity":0.5}},"unmatched":{"paint":{"fill":"#333333"}}}})");
        std::string error;
        expect(VsmrAvisoOverrides::ApplyAppearance(source, &appearance, error), "Global AVISO visual appearance applies");
        expect(source["features"][0]["geometry"] == old["features"][0]["geometry"] && source["vsmr_groups"] == old["vsmr_groups"] &&
            std::string(source["metadata"]["airport"].GetString()) == "LFXX", "Global appearance preserves geometry, groups and airport identity");
        expect(!source["styles"].HasMember("unmatched") && source["features"].Size() == 1, "Global appearance never creates foreign styles or features");
        expect(std::string(source["styles"]["a"]["paint"]["fill"].GetString()) == "#111111" &&
            std::string(source["styles"]["a"]["paint"]["palette-overrides"]["real"]["fill"].GetString()) == "#222222",
            "Applying Light preserves Dark and inherited Real colors");
        expect(std::string(source["features"][0]["properties"]["palette-overrides"]["light"]["fill"].GetString()) == "#ABCDEF",
            "Global appearance reaches feature-level colors rather than being masked by inline paints");
        Document local, customized;
        local.Parse(R"({"patch":{"document":{"metadata":{"background_opacities":{"light":0.8}},"styles":{"a":{"paint":{"palette-overrides":{"light":{"fill":"#987654"}},"fill-opacity":0.3}}}},"features":{"f1":{"properties":{"text-size":18}}}}})");
        customized.CopyFrom(old, customized.GetAllocator());
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error, &local), "Global defaults apply with local appearance overrides");
        expect(customized["metadata"]["background_opacities"]["light"].GetDouble() == 0.8 &&
            customized["styles"]["a"]["paint"]["fill-opacity"].GetDouble() == 0.3 &&
            std::string(customized["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].GetString()) == "#987654" &&
            customized["features"][0]["properties"]["text-size"].GetInt() == 18 &&
            !customized["features"][0]["properties"].HasMember("palette-overrides"),
            "Local palette, opacity and font edits remain authoritative without injected inline colors");
        local.Parse(R"({"patch":{"features":{"f1":{"properties":{"fill":"#FEDCBA","fill-opacity":0.2}}}}})");
        customized.CopyFrom(old, customized.GetAllocator());
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error, &local) &&
            std::string(customized["features"][0]["properties"]["fill"].GetString()) == "#FEDCBA" &&
            std::string(customized["features"][0]["properties"]["palette-overrides"]["light"]["fill"].GetString()) == "#ABCDEF" &&
            customized["features"][0]["properties"]["fill-opacity"].GetDouble() == 0.2 &&
            std::string(customized["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].GetString()) == "#ABCDEF",
            "Local inline Dark paint survives without blocking a different palette's shared colors");
        local.Parse(R"({"patch":{"document":{"styles":{"a":{"paint":{"palette-overrides":{"light":{"fill":"#999999"}},"fill-opacity":0.3}}}},"features":{"f1":{"properties":{"palette-overrides":{"light":{"fill":"#888888"}},"text-size":18}}}}})");
        VsmrAvisoOverrides::ClearAppearanceOverrides(local, appearance, old);
        customized.CopyFrom(old, customized.GetAllocator());
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error, &local) &&
            std::string(customized["features"][0]["properties"]["palette-overrides"]["light"]["fill"].GetString()) == "#ABCDEF" &&
            customized["styles"]["a"]["paint"]["fill-opacity"].GetDouble() == 0.5 &&
            customized["features"][0]["properties"]["text-size"].GetInt() == 18,
            "A newer global application supersedes matching local colors and opacity, preserving unrelated font edits");
        local.Parse(R"({"patch":{"features":{"f1":{"properties":{"palette-overrides":{"light":{"fill":"#FEDCBA"}}}}}}})");
        customized.CopyFrom(old, customized.GetAllocator());
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error, &local) &&
            std::string(customized["features"][0]["properties"]["palette-overrides"]["light"]["fill"].GetString()) == "#FEDCBA",
            "A later local edit takes precedence over the previously applied shared color");
        appearance.AddMember("generation", "global-one", appearance.GetAllocator());
        customized.CopyFrom(old, customized.GetAllocator());
        customized["metadata"].AddMember("appearance_generation", "global-one", customized.GetAllocator());
        customized["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].SetString("#LOCAL");
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error) &&
            std::string(customized["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].GetString()) == "#LOCAL",
            "A later custom-file save retains local appearance for the current shared generation");
        appearance["generation"].SetString("global-two");
        expect(VsmrAvisoOverrides::ApplyAppearance(customized, &appearance, error) &&
            std::string(customized["styles"]["a"]["paint"]["palette-overrides"]["light"]["fill"].GetString()) == "#ABCDEF" &&
            customized["features"][0]["geometry"] == old["features"][0]["geometry"],
            "A newer shared application reaches custom-file styles without changing their geometry");
        Document backgroundOnly, backgroundLocal;
        backgroundOnly.Parse(R"({"palette":"light","metadata":{"background_opacities":{"light":0.4}}})");
        backgroundLocal.Parse(R"({"patch":{"document":{"metadata":{"background_opacities":{"light":0.7,"dark":0.2}}}},"removed":{"/document/metadata/background_opacities/light":true}})");
        VsmrAvisoOverrides::ClearAppearanceOverrides(backgroundLocal, backgroundOnly, old);
        expect(!backgroundLocal["patch"]["document"]["metadata"]["background_opacities"].HasMember("light") &&
            backgroundLocal["patch"]["document"]["metadata"]["background_opacities"].HasMember("dark") &&
            !backgroundLocal["removed"].HasMember("/document/metadata/background_opacities/light"),
            "A background-only global application clears matching old overrides and tombstones without touching other palettes");
        Document record;
        record.Parse(R"({"patch":{"document":{"styles":{"a":{"paint":{"palette-overrides":{"light":{"fill":"#999999"}},"fill-opacity":0.3}}}},"features":{"f1":{"properties":{"fill-opacity":0.2,"visible":false},"geometry":{"coordinates":[8,9]}}}},"removed":{"/features/f1/properties/fill-opacity":true,"/features/f2":true}})");
        VsmrAvisoOverrides::ClearAppearanceOverrides(record, appearance, old);
        expect(!record["patch"]["features"]["f1"]["properties"].HasMember("fill-opacity") &&
            !record["removed"].HasMember("/features/f1/properties/fill-opacity"), "Replacing global appearance removes matching local paint patches and tombstones");
        expect(record["patch"]["features"]["f1"]["geometry"]["coordinates"][0].GetInt() == 8 &&
            !record["patch"]["features"]["f1"]["properties"]["visible"].GetBool() && record["removed"].HasMember("/features/f2"),
            "Appearance replacement preserves geometry edits, visibility and deleted features");
        Document unsafe;
        unsafe.Parse(R"({"palette":"light","features":[]})");
        expect(!VsmrAvisoOverrides::ValidateAppearance(unsafe, error), "Global appearance rejects geometry payloads");
    }
    Document defaults = Map(), edited = Map(), patch;
    std::string error;
    edited["features"][0]["properties"]["name"].SetString("Custom", edited.GetAllocator());
    edited["styles"]["a"]["paint"]["color"][0].SetInt(9);
    edited["vsmr_groups"][0]["name"].SetString("My group", edited.GetAllocator());
    expect(VsmrAvisoOverrides::Build(defaults, defaults, edited, nullptr, patch, error), "AVISO sparse patch builds");
    expect(patch.HasMember("patch") && patch["patch"]["features"].HasMember("f1") &&
        !patch["patch"]["features"]["f1"].HasMember("geometry"), "AVISO unchanged geometry is not pinned");

    Document newer = Map();
    newer["features"][0]["geometry"]["coordinates"][0].SetInt(10);
    newer["styles"]["a"]["paint"]["width"].SetInt(2);
    Value feature(newer["features"][1], newer.GetAllocator());
    feature["id"].SetString("f3", newer.GetAllocator());
    newer["features"].PushBack(feature, newer.GetAllocator());
    expect(VsmrAvisoOverrides::Apply(newer, &patch, error), "AVISO sparse patch applies to new geometry");
    expect(newer["features"].Size() == 3 && newer["features"][0]["geometry"]["coordinates"][0].GetInt() == 10 &&
        std::string(newer["features"][0]["properties"]["name"].GetString()) == "Custom",
        "AVISO update retains custom name while adopting new feature and coordinates");
    expect(newer["styles"]["a"]["paint"]["color"][0].GetInt() == 9 &&
        newer["styles"]["a"]["paint"]["width"].GetInt() == 2,
        "AVISO nested style overrides preserve new default properties");
    expect(std::string(newer["vsmr_groups"][0]["name"].GetString()) == "My group", "AVISO groups match stable IDs");

    Document removed = Map(), removalPatch, applied = Map();
    removed["features"].Erase(removed["features"].Begin());
    removed["vsmr_groups"].Clear();
    removed["styles"].RemoveMember("a");
    expect(VsmrAvisoOverrides::Build(defaults, defaults, removed, nullptr, removalPatch, error) &&
        VsmrAvisoOverrides::Apply(applied, &removalPatch, error) && applied == removed,
        "AVISO explicit removals do not abuse null or resurrect defaults");
    Document restoredPatch;
    expect(VsmrAvisoOverrides::Build(defaults, removed, defaults, &removalPatch, restoredPatch, error) &&
        restoredPatch.ObjectEmpty(), "AVISO restoring defaults prunes overrides and removal markers");

    Document customGeometry = Map(), geometryPatch, newerGeometry = Map();
    customGeometry["features"][0]["geometry"]["coordinates"][0].SetInt(8);
    expect(VsmrAvisoOverrides::Build(defaults, defaults, customGeometry, nullptr, geometryPatch, error) &&
        VsmrAvisoOverrides::Apply(newerGeometry, &geometryPatch, error) && newerGeometry == customGeometry,
        "AVISO coordinate arrays replace wholesale");

    Document orphan = Map();
    orphan["features"].Erase(orphan["features"].Begin());
    expect(VsmrAvisoOverrides::Apply(orphan, &patch, error) && orphan["features"].Size() == 1 &&
        patch["patch"]["features"].HasMember("f1"), "AVISO orphan feature edits retained but not resurrected");

    Document duplicate = Map(), invalidPatch;
    duplicate["features"][1]["id"].SetString("f1", duplicate.GetAllocator());
    expect(!VsmrAvisoOverrides::Build(duplicate, duplicate, duplicate, nullptr, invalidPatch, error),
        "AVISO duplicate IDs fail instead of guessing identity");
    Document invalidRemoval;
    VsmrJson::ParseDocument(invalidRemoval, R"({"removed":{"":true}})");
    Document original = Map();
    expect(!VsmrAvisoOverrides::Apply(original, &invalidRemoval, error) && original == defaults,
        "AVISO invalid removal leaves live document untouched");

    const auto testRoot = std::filesystem::temp_directory_path() /
        ("vsmr-aviso-overrides-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    try
    {
        if (!std::filesystem::create_directory(testRoot)) throw std::runtime_error("Test directory already exists");
        for (int mode = 0; mode < 3; ++mode)
        {
            const auto dataRoot = testRoot / std::to_string(mode) / "vSMR_Data";
            Document bundled;
            VsmrJson::ParseDocument(bundled, Read(repositoryRoot / "vSMR/data/default.json"));
            const auto mapPath = dataRoot / "AVISO/LFXX.geojson";
            Write(mapPath, mode == 0 ? defaults : edited);
            const auto remotePath = dataRoot / "UserData/France-Ground-Layouts" /
                std::string(40, 'a') / "GeoJSON/LFXX.geojson";
            Write(remotePath, defaults);
            expect(VsmrAvisoOverrides::IsManagedSource(dataRoot, remotePath),
                "Cached ground-layout AVISO is protected as an immutable default source");
            expect(!VsmrAvisoOverrides::IsManagedSource(dataRoot,
                dataRoot / "UserData/Maps/LFXX.geojson"),
                "Manually selected AVISO remains a custom source");
            std::string cleanHash;
            const auto baselinePath = dataRoot / "UpdateBaselines/AVISO/LFXX.geojson";
            Write(baselinePath, defaults);
            expect(vsmr::updater::verification::Sha256File(baselinePath, cleanHash), "AVISO baseline fixture hashed");
            Value hashes(rapidjson::kObjectType);
            Value hash(cleanHash.c_str(), bundled.GetAllocator());
            VsmrLayeredConfig::Put(hashes, "AVISO/LFXX.geojson", hash, bundled.GetAllocator());
            VsmrLayeredConfig::Put(bundled, "asset_hashes", hashes, bundled.GetAllocator());
            Write(dataRoot / "default.json", bundled);
            if (mode == 1)
            {
                Document index(rapidjson::kObjectType);
                index.AddMember("schema_version", 1, index.GetAllocator());
                VsmrLayeredConfig::Put(index, "files", hashes, index.GetAllocator());
                Write(dataRoot / "UpdateBaselines/BASELINE-HASHES.json", index);
            }
            const std::string originalBytes = Read(mapPath);
            CConfig config((dataRoot / "default.json").u8string(), (dataRoot / "vSMR_Maps.json").u8string());
            expect(config.isLayeredConfig() && config.isConfigHealthy(), "AVISO migration has healthy layered config");
            expect(VsmrAvisoOverrides::MigrateLegacy(config, dataRoot, error), "AVISO one-time migration succeeds");
            expect(Read(mapPath) == originalBytes, "AVISO migration never modifies original map");
            const Value* migration = config.getUserConfigSection("_migration");
            expect(migration != nullptr && migration->HasMember("legacy_aviso") && (*migration)["legacy_aviso"].GetBool(),
                "AVISO readiness marker only follows completed migration");
            const Value* user = VsmrAvisoOverrides::Find(config, "LFXX");
            const auto externalPath = dataRoot / "UserData/Profiles/external.json";
            Document legacyProfiles;
            VsmrJson::ParseDocument(legacyProfiles, Read(repositoryRoot / "vSMR/data/profile_templates.json"));
            Write(externalPath, legacyProfiles);
            CConfig external(externalPath.u8string(), (dataRoot / "vSMR_Maps.json").u8string());
            expect(!external.isLayeredConfig(), "External profile fixture remains user-owned legacy format");
            if (mode == 0) expect(user == nullptr, "Clean new-install maps need no overrides or custom copies");
            if (mode == 0)
            {
                Document profilesBefore;
                profilesBefore.CopyFrom(config.document, profilesBefore.GetAllocator());
                const std::string revision = config.getConfigRevision();
                Document sections;
                sections.Parse(R"({"aviso_display_defaults":{"palette":"light","styles":{}},"test_visual_section":{"unchanged_profiles":true}})");
                expect(config.saveUserConfigSections(sections, revision, error), "Global visual sections save in one transaction");
                expect(config.document == profilesBefore && config.getUserConfigSection("aviso_display_defaults") != nullptr,
                    "Atomic global visual writes preserve profile content and selection metadata");
                expect(!config.saveUserConfigSections(sections, revision, error), "Stale global writes cannot overwrite another configuration change");
                Document protectedSection;
                protectedSection.Parse(R"({"profiles":{}})");
                expect(!config.saveUserConfigSections(protectedSection, config.getConfigRevision(), error), "Global visual writes cannot replace profile-owned sections");
            }
            if (mode == 1) expect(user != nullptr && user->HasMember("patch") && !user->HasMember("custom_file"),
                "Verified legacy baseline produces sparse map overrides");
            if (mode == 1)
            {
                const std::string baseSource = VsmrAvisoOverrides::DefaultSource(config, dataRoot, mapPath, error);
                expect(!baseSource.empty() && Read(std::filesystem::u8path(baseSource)) == Read(baselinePath),
                    "Bridge uses verified clean baseline instead of already-edited source");
                expect(VsmrAvisoOverrides::DefaultSource(external, dataRoot, mapPath, error) == baseSource,
                    "External profiles still resolve canonical legacy map baseline");
                const auto shared = VsmrAvisoSharedConfig::Read(dataRoot, true);
                const auto* airports = shared.document != nullptr ? VsmrLayeredConfig::Member(*shared.document, "aviso") : nullptr;
                const auto* overrides = airports != nullptr ? VsmrLayeredConfig::Member(*airports, "LFXX") : nullptr;
                Document sharedMap = Map();
                expect(shared.error.empty() && overrides != nullptr &&
                    VsmrAvisoOverrides::Apply(sharedMap, overrides, error) && sharedMap == edited,
                    "External profiles retain canonical sparse AVISO edits");
                Value emptyOverrides(rapidjson::kObjectType);
                expect(config.saveUserConfigSection("aviso", &emptyOverrides, config.getConfigRevision(), error),
                    "Reset removes user map overrides");
                expect(VsmrAvisoOverrides::DefaultSource(config, dataRoot, mapPath, error) == baseSource,
                    "Reset keeps clean defaults before first raw update");
                Document updated = Map();
                updated["features"][0]["geometry"]["coordinates"][0].SetInt(25);
                Write(mapPath, updated);
                expect(VsmrAvisoOverrides::DefaultSource(config, dataRoot, mapPath, error) == mapPath.u8string(),
                    "Raw update switches from bridge baseline to new official geometry");
            }
            if (mode == 2)
            {
                const std::string source = VsmrAvisoOverrides::CustomSource(config, "LFXX", dataRoot);
                expect(!source.empty() && Read(std::filesystem::u8path(source)) == originalBytes,
                    "No baseline preserves exact custom map bytes");
                expect(VsmrAvisoOverrides::CustomSource(external, "LFXX", dataRoot) == source,
                    "External profiles still use preserved canonical custom map");
            }
            const std::string userBytes = Read(dataRoot / "config.json");
            expect(VsmrAvisoOverrides::MigrateLegacy(config, dataRoot, error) && Read(dataRoot / "config.json") == userBytes,
                "AVISO migration is idempotent");
        }
    }
    catch (const std::exception& exception) { failures.push_back(std::string("AVISO migration fixture: ") + exception.what()); }
    std::error_code cleanup;
    std::filesystem::remove_all(testRoot, cleanup);
    return failures;
}
