#pragma once
#include <filesystem>

// Optional local-data validation; raw EuroScope logs never enter the build.
int RunSmartGroundReplayTests(const std::filesystem::path& repositoryRoot);
