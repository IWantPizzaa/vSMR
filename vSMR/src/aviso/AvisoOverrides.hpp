#pragma once

#include "rapidjson/document.h"
#include <filesystem>
#include <string>

class CConfig;

// Official maps are immutable. The user document stores sparse edits against
// stable feature/group IDs; complete imported maps remain user-owned files.
namespace VsmrAvisoOverrides
{
    bool IsManagedSource(const std::filesystem::path& dataRoot,
        const std::filesystem::path& source);
    std::string AirportKey(const std::filesystem::path& source);
    const rapidjson::Value* Find(const CConfig& config, const std::string& airport);
    std::string CustomSource(const CConfig& config, const std::string& airport,
        const std::filesystem::path& dataRoot);
    // During the bridge release, an official file can still contain legacy
    // edits. Use its verified old baseline until that exact source is updated.
    std::string DefaultSource(const CConfig& config, const std::filesystem::path& dataRoot,
        const std::filesystem::path& source, std::string& error);

    bool Apply(rapidjson::Document& source, const rapidjson::Value* overrides,
        std::string& error);
    bool Build(const rapidjson::Value& defaults, const rapidjson::Value& previous,
        const rapidjson::Value& edited, const rapidjson::Value* existing,
        rapidjson::Document& result, std::string& error);

    // One-time application-owned conversion. Never changes the original maps.
    // Unknown/unrepresentable legacy edits are preserved as custom map copies.
    bool MigrateLegacy(CConfig& config, const std::filesystem::path& dataRoot,
        std::string& error);
}
