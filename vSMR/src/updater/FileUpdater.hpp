#pragma once
#include "updater/UpdaterCore.hpp"

namespace vsmr::updater::files
{
    StartupResult Prepare(const StartupOptions& options);
    bool HasTransaction(const std::filesystem::path& installRoot);
    bool IsFileUpdate(const StartupResult& update);
    bool ValidatePreparedGeneration(const StartupOptions& options, const StartupResult& update);
    bool Confirm(const StartupResult& update, bool healthy);
    bool Restore(const StartupOptions& options, const StartupResult& update,
        std::filesystem::path* restoredRuntime, std::wstring* error);
    // Minimal offline helper entry point. No downloading or configuration writes.
    int ApplyPending(const std::filesystem::path& installRoot, std::uint32_t parentProcessId);
}
