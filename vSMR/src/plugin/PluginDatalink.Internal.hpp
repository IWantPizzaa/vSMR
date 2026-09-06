#pragma once

#include "plugin/Plugin.hpp"

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

// Internal CPDLC/CDM contract shared only by the focused implementation
// units. Keeping this out of Plugin.hpp prevents transport and reminder state
// from becoming part of the plug-in's public API.

struct DatalinkPacket
{
	std::string callsign;
	std::string destination;
	std::string sid;
	std::string rwy;
	std::string freq;
	std::string ctot;
	std::string tsat;
	std::string squawk;
	std::string message;
	std::string climb;
};

struct AcarsMessage
{
	std::string from;
	std::string type;
	std::string message;
};

struct QueuedCdmReminderMessage
{
	std::string callsign;
	std::string activeAirport;
	std::string message;
	int sendAttempts = 0;
	bool automatic = false;
	unsigned long long automaticSessionGeneration = 0;
	std::chrono::steady_clock::time_point nextAttemptAt;
};

enum class CdmAutoEligibility
{
	WaitingForMissingTobt = 0,
	SuppressedBySubmittedTobt,
	RetryExhausted
};

struct CdmAutoTrackedAircraftState
{
	std::chrono::steady_clock::time_point dueAt;
	CdmAutoEligibility eligibility = CdmAutoEligibility::WaitingForMissingTobt;
};

enum class CdmQueueReminderOutcome
{
	Queued = 0,
	AlreadyNotified,
	AlreadyQueued,
	AlreadyCleared,
	HasSubmittedTobt,
	Failed
};

enum class CdmChatSubmissionStatus
{
	Idle = 0,
	Pending,
	Confirmed,
	Ambiguous
};

struct DatalinkCredentialsSnapshot
{
	std::string callsign;
	std::string password;
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
	bool reportStatus = false;
};

struct DatalinkMessageRequest
{
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
	std::string destination;
	std::string type;
	std::string packet;
	std::string callsign;
};

struct DatalinkClearanceRequest
{
	DatalinkCredentialsSnapshot credentials;
	unsigned long long generation = 0;
	DatalinkPacket packet;
	std::string fallbackFrequency;
	int messageSequence = 0;
};

using PluginSteadyClock = std::chrono::steady_clock;

extern std::atomic<bool> HoppieConnected;
extern std::atomic<bool> HoppieConnecting;
extern std::atomic<bool> HoppiePollInProgress;
extern std::atomic<unsigned long long> HoppieConnectionGeneration;
extern std::atomic<unsigned long long> HoppiePollGeneration;
extern std::atomic<bool> ConnectionMessage;
extern std::atomic<bool> FailedToConnectMessage;
extern std::string DatalinkStatusMessage;
extern const std::string baseUrlDatalink;
extern std::set<std::string> AircraftDatalinkClearedCallsigns;
extern std::set<std::string> AircraftDatalinkClearanceInFlightCallsigns;
extern std::map<std::string, std::chrono::steady_clock::time_point> AircraftCdmTobtReminderSentAt;
extern std::set<std::string> AircraftCdmReminderSubmittedCallsigns;
extern std::deque<QueuedCdmReminderMessage> CdmReminderMessageQueue;
extern std::atomic<bool> CdmAutoModeEnabled;
extern std::atomic<int> CdmAutoDelayMinutes;
extern std::map<std::string, CdmAutoTrackedAircraftState> AircraftCdmAutoTracked;
extern std::string CdmAutoTrackedAirport;
extern unsigned long long CdmAutoSessionGeneration;
extern std::map<std::string, AcarsMessage> PendingMessages;
extern std::atomic<int> CdmReminderCooldownMinutes;
extern std::atomic<int> messageId;
extern PluginSteadyClock::time_point DatalinkLastPollAt;

extern std::mutex ProfilesSourceMutex;
extern std::string ActiveProfilesConfigPath;
extern bool ActiveProfilesConfigPathClaimed;

extern const int CdmMaximumMinutes;
extern const int CdmReminderQueueMaxSendAttempts;
extern const int CdmReminderRetryDelaySeconds;
extern const std::size_t HoppieResponseLimitBytes;

DatalinkCredentialsSnapshot SnapshotDatalinkCredentials();
void SetDatalinkStatusMessage(const std::string& message);
std::string GetDatalinkStatusMessageCopy();
bool TryParseNonNegativeInt(const std::string& text, int& outValue);
std::string ResolveActiveAirportFilterUpper();
bool IsCdmBridgeReady();
std::vector<std::string> CollectFlightPlanCandidateCallsignsForActiveAirport(
	EuroScopePlugIn::CPlugIn* plugIn,
	const std::string& activeAirportFilter);
void PruneCdmReminderHistoryUnlocked(
	std::chrono::steady_clock::time_point now);
bool HasRecentCdmReminderUnlocked(
	const std::string& callsign,
	std::chrono::steady_clock::time_point now);
bool IsCdmReminderQueuedUnlocked(const std::string& callsign);
void MarkCdmReminderSentUnlocked(
	const std::string& callsign,
	std::chrono::steady_clock::time_point now);
CdmQueueReminderOutcome TryQueueCdmReminderForCallsign(
	EuroScopePlugIn::CPlugIn* plugIn,
	const std::string& callsign,
	const std::string& reminderMessage,
	std::chrono::steady_clock::time_point now,
	bool* outCdmEvaluated = nullptr,
	bool* outHasCdmData = nullptr,
	bool automatic = false);
void ClearCdmAutoTrackingState(bool clearQueuedAutomaticReminders = false);
void ResetCdmReminderSessionState();
bool ContainsCallsignUnlocked(
	const std::vector<std::string>& collection,
	const std::string& callsign);
void AddCallsignUniqueUnlocked(
	std::vector<std::string>& collection,
	const std::string& callsign);
void RemoveCallsignUnlocked(
	std::vector<std::string>& collection,
	const std::string& callsign);
void RemoveQueuedCdmReminderUnlocked(const std::string& callsign);
std::string NormalizeCallsignForState(const std::string& callsign);
bool HasDatalinkClearanceSentUnlocked(const std::string& callsign);
bool HasDatalinkClearanceInFlightUnlocked(const std::string& callsign);
bool HasCdmReminderSubmittedUnlocked(const std::string& callsign);
void MarkCdmReminderSubmittedUnlocked(const std::string& callsign);
void MarkDatalinkClearanceInFlightUnlocked(const std::string& callsign);
void ClearDatalinkClearanceInFlightUnlocked(const std::string& callsign);
void MarkDatalinkClearanceSentUnlocked(const std::string& callsign);
void ClearDatalinkClearanceSentUnlocked(const std::string& callsign);
std::filesystem::path ResolveDefaultProfilesConfigPath();
std::string FormatUtcHhmm(std::time_t utcTime);
bool HasSubmittedTobtState(const CdmPilotData& pilotData);
bool TryReadCdmReminderMessageFromAlias(
	EuroScopePlugIn::CPlugIn* plugIn,
	std::string& outMessage,
	std::string& outAliasPath);
bool TryLoadCdmReminderMessage(
	EuroScopePlugIn::CPlugIn* plugIn,
	std::string& outMessage);
bool IsCallsignEligibleForCdmReminderNow(
	EuroScopePlugIn::CPlugIn* plugIn,
	const std::string& callsign);
bool BeginPrivateChatMessageLikeDotMsg(
	EuroScopePlugIn::CPlugIn* plugIn,
	const std::string& callsign,
	const std::string& message);
CdmChatSubmissionStatus PollPrivateChatMessageSubmission();
bool QueueDatalinkMessage(
	CSMRPlugin* plugin,
	const std::string& destination,
	const std::string& type,
	const std::string& packet,
	const std::string& callsign);
bool StartDatalinkPoll(bool reportStatus, std::string& error);
void ProcessCdmAutoMode(CSMRPlugin* plugIn);
void ProcessQueuedCdmReminderMessages(CSMRPlugin* plugIn);
void datalinkLogin(DatalinkLoginRequest request);
void pollMessages(DatalinkPollRequest request);
void sendDatalinkClearance(DatalinkClearanceRequest request);
