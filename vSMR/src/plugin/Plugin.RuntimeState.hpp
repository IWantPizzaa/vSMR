#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Process-wide plugin state remains defined and owned by Plugin.cpp.
extern std::atomic<bool> PluginShutdownRequested;
extern bool BLINK;
extern std::mutex DatalinkControlMutex;
extern std::mutex DatalinkStateMutex;
extern std::string logonCallsign;
extern std::string logonCode;
extern std::vector<std::string> AircraftDemandingClearance;
extern std::vector<std::string> AircraftMessageSent;
extern std::vector<std::string> AircraftMessage;
extern std::vector<std::string> AircraftWilco;
extern std::vector<std::string> AircraftStandby;
