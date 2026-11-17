#pragma once

#include "plugin/Plugin.hpp"
#include "datalink/PinedeProtocol.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Internal PDC/CDM contract shared only by the focused implementation
// units. Keeping this out of Plugin.hpp prevents transport and reminder state
// from becoming part of the plug-in's public API.
//
// PDC goes through PINEDE: it receives the pilots' requests, decides which
// station sees each one, and sends every uplink. The tag lists are a copy of
// what PINEDE last said, rebuilt on every poll, plus the immediate effect of
// this station's own actions until the next poll confirms them.

struct AcarsMessage
{
	std::string from;
	std::string type;
	std::string message;
};

struct DatalinkCredentialsSnapshot
{
	// EuroScope callsign of this station, which PINEDE routes requests to
	std::string callsign;
	std::string key;
	std::string baseUrl;
	std::string frequency;
};

struct DatalinkLoginRequest
{
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
};

struct DatalinkPollRequest
{
	CSMRPlugin* plugin = nullptr;
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
	unsigned long long pollGeneration = 0;
	unsigned long long stateRevision = 0;
	bool reportStatus = false;
};

enum class DatalinkAction
{
	Standby,
	Voice,
	Message,
	Reset,
};

struct DatalinkActionRequest
{
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
	DatalinkAction action = DatalinkAction::Standby;
	std::string callsign;
	std::string text;
};

struct DatalinkClearanceRequest
{
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
	VsmrPinede::ClearanceFields fields;
};

using PluginSteadyClock = std::chrono::steady_clock;

extern std::atomic<bool> DatalinkConnected;
extern std::atomic<bool> DatalinkConnecting;
extern std::atomic<bool> DatalinkPollInProgress;
extern std::atomic<unsigned long long> DatalinkConnectionGeneration;
extern std::atomic<unsigned long long> DatalinkPollGeneration;
// DMAN handles this station's PDC through PINEDE. At most one client polls a
// station, and a logoff would disconnect DMAN: vSMR stays silent meanwhile.
extern std::atomic<bool> DatalinkYieldedToDman;
// Bumped by every action this station takes, so a poll that started before
// it cannot overwrite its effect with older server state
extern std::atomic<unsigned long long> DatalinkStateRevision;
extern std::atomic<bool> ConnectionMessage;
extern std::atomic<bool> FailedToConnectMessage;
extern std::string DatalinkStatusMessage;
extern std::string DatalinkBaseUrl;
extern std::string DatalinkStationFrequency;
extern std::set<std::string> AircraftDatalinkClearedCallsigns;
extern std::set<std::string> AircraftDatalinkClearanceInFlightCallsigns;
// Aircraft whose WILCO flag was cleared locally once airborne
extern std::set<std::string> AircraftWilcoDismissed;
extern std::map<std::string, AcarsMessage> PendingMessages;
extern PluginSteadyClock::time_point DatalinkLastPollAt;

extern std::mutex ProfilesSourceMutex;
extern std::string ActiveProfilesConfigPath;
extern bool ActiveProfilesConfigPathClaimed;

extern const std::size_t DatalinkResponseLimitBytes;

DatalinkCredentialsSnapshot SnapshotDatalinkCredentials();
void SetDatalinkStatusMessage(const std::string& message);
std::string GetDatalinkStatusMessageCopy();
// Worker threads cannot talk to EuroScope; their messages for the controller
// wait here until the timer shows them.
void QueueDatalinkNotice(const std::string& message);
std::vector<std::string> TakeDatalinkNotices();
std::string ResolveActiveAirportFilterUpper();
bool IsCdmBridgeReady();
void ResetDatalinkClearanceState();
bool ContainsCallsignUnlocked(
	const std::vector<std::string>& collection,
	const std::string& callsign);
void AddCallsignUniqueUnlocked(
	std::vector<std::string>& collection,
	const std::string& callsign);
void RemoveCallsignUnlocked(
	std::vector<std::string>& collection,
	const std::string& callsign);
std::string NormalizeCallsignForState(const std::string& callsign);
bool HasDatalinkClearanceSentUnlocked(const std::string& callsign);
bool HasDatalinkClearanceInFlightUnlocked(const std::string& callsign);
void MarkDatalinkClearanceInFlightUnlocked(const std::string& callsign);
void ClearDatalinkClearanceInFlightUnlocked(const std::string& callsign);
void MarkDatalinkClearanceSentUnlocked(const std::string& callsign);
void ClearDatalinkClearanceSentUnlocked(const std::string& callsign);
std::filesystem::path ResolveDefaultProfilesConfigPath();
std::filesystem::path ResolveAliasFilePath(EuroScopePlugIn::CPlugIn* plugIn);
std::string FormatUtcHhmm(std::time_t utcTime);
// Empties the tag lists, when this station can no longer act on them
void ClearDatalinkTagState();
// Whether PINEDE has given this station anything for the aircraft
bool IsDatalinkTrackedUnlocked(const std::string& callsign);
bool QueueDatalinkAction(
	CSMRPlugin* plugin,
	DatalinkAction action,
	const std::string& callsign,
	const std::string& text = "");
bool StartDatalinkPoll(bool reportStatus, std::string& error);
void datalinkLogin(DatalinkLoginRequest request);
void datalinkLogoff(DatalinkCredentialsSnapshot credentials);
void pollMessages(DatalinkPollRequest request);
void sendDatalinkClearance(DatalinkClearanceRequest request);
