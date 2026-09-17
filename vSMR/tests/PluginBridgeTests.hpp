#pragma once

#include <string>
#include <vector>

std::vector<std::string> RunPluginBridgeTests();

void RunPluginBridgePollingTests(std::vector<std::string>& failures);
