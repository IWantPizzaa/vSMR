#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class CSMRPlugin;

// Process-wide state used by the small callback and worker implementation
// units. Feature-specific state remains private to its owning source file.
extern std::atomic<bool> PluginShutdownRequested;
extern std::atomic<CSMRPlugin*> ActivePluginInstance;
extern std::atomic<bool> FlightDataRefreshPending;
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
