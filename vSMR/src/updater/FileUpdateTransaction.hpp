#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vsmr::updater::files
{
    struct File
    {
        std::string path;
        std::string sha256;
        std::uint64_t size = 0;
    };

    struct Manifest
    {
        std::string version;
        std::string contentCommit;
        std::string minimumLoaderVersion;
        std::uint32_t runtimeAbi = 0;
        std::vector<File> files;
    };

    struct Change
    {
        File file;
        bool existed = false;
        std::string previousHash;
    };

    struct Transaction
    {
        std::filesystem::path root;
        Manifest target;
        std::string targetJson;
        std::string phase = "ready";
        std::vector<Change> changes;
        bool previousManifestExisted = false;
        std::string previousManifestHash;
        std::string previousVersion;
        std::uint32_t ownerPid = 0;
        std::uint64_t ownerCreated = 0;
    };

    bool IsManagedPath(const std::string& path);
    bool SafePath(const std::filesystem::path& root, const std::filesystem::path& path);
    bool ParseManifest(const std::string& json, Manifest& manifest, std::string& error);
    bool ReadText(const std::filesystem::path& path, std::string& text, std::uint64_t maximumBytes = 2 * 1024 * 1024);
    bool WriteText(const std::filesystem::path& path, const std::string& text);
    bool CopyAtomically(const std::filesystem::path& source, const std::filesystem::path& target);
    bool Matches(const std::filesystem::path& path, const File& file);
    bool ValidateDefaults(const Transaction& transaction, std::string& error);
    bool IsCommitted(const Transaction& transaction);
    std::filesystem::path UpdateRoot(const std::filesystem::path& installRoot);
    bool SaveTransaction(const Transaction& transaction, std::string& error);
    bool LoadTransaction(const std::filesystem::path& installRoot, Transaction& transaction, std::string& error);
    // Caller must hold the installation's exclusive lease and updater mutex.
    bool Apply(Transaction& transaction, std::string& error,
        const std::function<void(const File&)>& beforeReplaceForTest = {});
    bool Rollback(Transaction& transaction, std::string& error);
    bool Cleanup(const Transaction& transaction);
    bool MarkAttempt(Transaction& transaction, std::string& error);
    bool OwnerAlive(const Transaction& transaction);
}
