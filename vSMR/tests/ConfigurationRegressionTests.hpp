#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::string> RunConfigurationRegressionTests(
	const std::filesystem::path& repositoryRoot);
