#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashReporter.hpp"
#include "datalink/CdmReminderSafety.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "shared/TextUtils.hpp"
#include "weather/WeatherStore.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>

using VsmrDatalinkProtocol::BuildHoppieLoginFailureMessage;
using VsmrDatalinkProtocol::EncodeUrlQueryComponent;
using VsmrDatalinkProtocol::FormatPdcFrequency;
using VsmrDatalinkProtocol::IsHoppieOkResponse;
using VsmrDatalinkProtocol::PdcFrequencySelection;
using VsmrDatalinkProtocol::ProtectHoppieCredential;
using VsmrDatalinkProtocol::RedactSensitiveValue;
using VsmrDatalinkProtocol::ResolvePdcNextFrequency;
using VsmrDatalinkProtocol::UnprotectHoppieCredential;

// CPDLC/Hoppie connection state shared between timer and worker threads.
std::atomic<bool> HoppieConnected(false);
std::atomic<bool> HoppieConnecting(false);
std::atomic<bool> HoppiePollInProgress(false);
std::atomic<unsigned long long> HoppieConnectionGeneration(0);
std::atomic<unsigned long long> HoppiePollGeneration(0);
std::atomic<bool> ConnectionMessage(false);
std::atomic<bool> FailedToConnectMessage(false);

string logonCode = "";
string logonCallsign = "EGKK";

std::string DatalinkStatusMessage = "Disconnected.";
std::mutex DatalinkControlMutex;

const string baseUrlDatalink = "https://www.hoppie.nl/acars/system/connect.html";

vector<string> AircraftDemandingClearance;
vector<string> AircraftMessageSent;
vector<string> AircraftMessage;
vector<string> AircraftWilco;
vector<string> AircraftStandby;
std::set<std::string> AircraftDatalinkClearedCallsigns;
std::set<std::string> AircraftDatalinkClearanceInFlightCallsigns;
map<string, std::chrono::steady_clock::time_point> AircraftCdmTobtReminderSentAt;
std::set<std::string> AircraftCdmReminderSubmittedCallsigns;

std::deque<QueuedCdmReminderMessage> CdmReminderMessageQueue;

std::atomic<bool> CdmAutoModeEnabled(false);
std::atomic<int> CdmAutoDelayMinutes(5);

map<string, CdmAutoTrackedAircraftState> AircraftCdmAutoTracked;
std::string CdmAutoTrackedAirport;
unsigned long long CdmAutoSessionGeneration = 1;
map<string, AcarsMessage> PendingMessages;
// Guards all mutable CPDLC message state used by worker threads.
std::mutex DatalinkStateMutex;
std::atomic<int> CdmReminderCooldownMinutes(60);

std::atomic<int> messageId(0);

PluginSteadyClock::time_point DatalinkLastPollAt;


std::mutex ProfilesSourceMutex;
std::string ActiveProfilesConfigPath;
bool ActiveProfilesConfigPathClaimed = false;

	const std::time_t CdmWarningCooldownSeconds = 60;
	const int CdmReminderQueueMaxSendAttempts = 20;
	const int CdmReminderRetryDelaySeconds = 5;
	const int CdmMaximumMinutes = 24 * 60;
	const size_t HoppieResponseLimitBytes = 1024U * 1024U;

	DatalinkCredentialsSnapshot SnapshotDatalinkCredentials()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		DatalinkCredentialsSnapshot snapshot;
		snapshot.callsign = logonCallsign;
		snapshot.password = logonCode;
		return snapshot;
	}

	void SetDatalinkStatusMessage(const std::string& message)
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		DatalinkStatusMessage = message;
	}

	std::string GetDatalinkStatusMessageCopy()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		return DatalinkStatusMessage;
	}

	bool StartDatalinkPoll(bool reportStatus, std::string& error);

	bool HasSubmittedTobtState(const CdmPilotData& pilotData);

	bool TryParseNonNegativeInt(const std::string& text, int& outValue)
	{
		outValue = 0;
		const std::string trimmed = TrimAsciiWhitespaceCopy(text);
		if (trimmed.empty())
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(trimmed.c_str(), &end, 10);
		if (end == trimmed.c_str() || parsed < 0 || parsed > 24 * 60)
			return false;

		const std::string trailing = TrimAsciiWhitespaceCopy(end != nullptr ? std::string(end) : std::string());
		if (!trailing.empty())
			return false;

		outValue = static_cast<int>(parsed);
		return true;
	}

	bool IsNoStatusGroundState(const char* rawGroundState)
	{
		const std::string raw = rawGroundState != nullptr ? rawGroundState : "";
		const std::string trimmed = TrimAsciiWhitespaceCopy(raw);
		if (trimmed.empty())
			return true;

		std::string normalized;
		normalized.reserve(trimmed.size());
		for (char c : trimmed)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}

		return normalized.empty() || normalized == "NSTS" || normalized == "NOSTATUS";
	}

	std::string ResolveActiveAirportFilterUpper()
	{
		std::string resolvedAirport;
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr || rd->IsShutdownRequested())
				continue;
			std::string airport = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rd->getActiveAirport()));
			if (airport.size() > 4)
				airport = airport.substr(0, 4);
			if (airport.empty())
				continue;
			if (airport.size() != 4)
				return "";
			if (resolvedAirport.empty())
				resolvedAirport = airport;
			else if (resolvedAirport != airport)
				return "";
		}

		return resolvedAirport;
	}

	bool TryResolveActiveAirportPosition(
		const std::string& activeAirport,
		CPosition& outPosition)
	{
		if (activeAirport.empty() ||
			ResolveActiveAirportFilterUpper() != activeAirport)
		{
			return false;
		}

		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->IsShutdownRequested())
				continue;
			const std::string radarAirport = ToUpperAsciiCopy(
				TrimAsciiWhitespaceCopy(radar->getActiveAirport()));
			if (radarAirport == activeAirport &&
				radar->TryGetActiveAirportPosition(outPosition))
			{
				return true;
			}
		}

		return false;
	}

	bool IsCdmBridgeReady()
	{
		return VsmrCdm::GetInterfaceState().providerReady;
	}

	std::vector<std::string> CollectFlightPlanCandidateCallsignsForActiveAirport(
		EuroScopePlugIn::CPlugIn* plugIn,
		const std::string& activeAirportFilter)
	{
		std::vector<std::string> candidateCallsigns;
		if (plugIn == nullptr || activeAirportFilter.empty())
			return candidateCallsigns;
		if (ResolveActiveAirportFilterUpper() != activeAirportFilter)
			return candidateCallsigns;

		CPosition airportPosition;
		if (!TryResolveActiveAirportPosition(activeAirportFilter, airportPosition))
			return candidateCallsigns;

		candidateCallsigns.reserve(256);

		auto addUniqueCallsign = [&](const std::string& rawCallsign)
		{
			const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawCallsign));
			if (callsign.empty())
				return;
			if (std::find(candidateCallsigns.begin(), candidateCallsigns.end(), callsign) == candidateCallsigns.end())
				candidateCallsigns.push_back(callsign);
		};

		std::size_t flightPlanGuard = 0;
		for (CFlightPlan fp = plugIn->FlightPlanSelectFirst();
			fp.IsValid() && flightPlanGuard < 4096;
			fp = plugIn->FlightPlanSelectNext(fp), ++flightPlanGuard)
		{
			const char* fpCallsignRaw = fp.GetCallsign();
			if (fpCallsignRaw == nullptr || fpCallsignRaw[0] == '\0')
				continue;

			const char* originRaw = fp.GetFlightPlanData().GetOrigin();
			const std::string origin = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(originRaw != nullptr ? originRaw : ""));
			CRadarTarget correlatedTarget = fp.GetCorrelatedRadarTarget();
			CRadarTargetPositionData targetPosition;
			if (correlatedTarget.IsValid())
				targetPosition = correlatedTarget.GetPosition();

			VsmrCdmReminderSafety::EligibilitySnapshot safety;
			safety.activeAirportResolved = true;
			safety.originMatchesActiveAirport = origin == activeAirportFilter;
			safety.flightPlanNotStarted =
				fp.GetFPState() == FLIGHT_PLAN_STATE_NOT_STARTED;
			safety.simulatedFlightPlan = fp.GetSimulated();
			safety.radarTargetValid = correlatedTarget.IsValid();
			safety.radarPositionValid = targetPosition.IsValid();
			safety.noGroundStatus = IsNoStatusGroundState(fp.GetGroundState());
			if (targetPosition.IsValid())
			{
				safety.positionAgeSeconds = targetPosition.GetReceivedTime();
				safety.groundSpeedKnots = (std::max)(
					correlatedTarget.GetGS(),
					targetPosition.GetReportedGS());
				safety.verticalSpeedFeetPerMinute =
					correlatedTarget.GetVerticalSpeed();
				safety.airportDistanceNauticalMiles =
					airportPosition.DistanceTo(targetPosition.GetPosition());
			}
			if (!VsmrCdmReminderSafety::IsEligible(safety))
				continue;

			addUniqueCallsign(fpCallsignRaw);
		}

		return candidateCallsigns;
	}

	void PruneCdmReminderHistoryUnlocked(std::chrono::steady_clock::time_point now)
	{
		int cooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
		if (cooldownMinutes < 0)
			cooldownMinutes = 0;
		// Zero disables repeated reminders for the current eligibility period.
		if (cooldownMinutes == 0)
			return;
		const auto cooldown = std::chrono::minutes(cooldownMinutes);
		for (auto it = AircraftCdmTobtReminderSentAt.begin(); it != AircraftCdmTobtReminderSentAt.end();)
		{
			if (now - it->second >= cooldown)
				it = AircraftCdmTobtReminderSentAt.erase(it);
			else
				++it;
		}
	}

	bool HasRecentCdmReminderUnlocked(
		const std::string& callsign,
		std::chrono::steady_clock::time_point now)
	{
		auto it = AircraftCdmTobtReminderSentAt.find(callsign);
		if (it == AircraftCdmTobtReminderSentAt.end())
			return false;

		int cooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
		if (cooldownMinutes < 0)
			cooldownMinutes = 0;
		if (cooldownMinutes == 0)
			return true;
		if (now - it->second >= std::chrono::minutes(cooldownMinutes))
		{
			AircraftCdmTobtReminderSentAt.erase(it);
			return false;
		}

		return true;
	}

	void MarkCdmReminderSentUnlocked(
		const std::string& callsign,
		std::chrono::steady_clock::time_point now)
	{
		AircraftCdmTobtReminderSentAt[callsign] = now;
	}

	bool IsCdmReminderQueuedUnlocked(const std::string& callsign)
	{
		return std::any_of(
			CdmReminderMessageQueue.begin(),
			CdmReminderMessageQueue.end(),
			[&](const QueuedCdmReminderMessage& queued)
			{
				return queued.callsign == callsign;
			});
	}

	bool QueueCdmReminderUnlocked(
		const std::string& callsign,
		const std::string& activeAirport,
		const std::string& message,
		bool automatic)
	{
		if (callsign.empty() || activeAirport.empty() || message.empty())
			return false;
		if (IsCdmReminderQueuedUnlocked(callsign))
			return false;

		QueuedCdmReminderMessage queued;
		queued.callsign = callsign;
		queued.activeAirport = activeAirport;
		queued.message = message;
		queued.sendAttempts = 0;
		queued.automatic = automatic;
		queued.automaticSessionGeneration = automatic ? CdmAutoSessionGeneration : 0;
		queued.nextAttemptAt = std::chrono::steady_clock::now();
		CdmReminderMessageQueue.push_back(queued);
		return true;
	}

	void RemoveQueuedCdmReminderUnlocked(const std::string& callsign)
	{
		CdmReminderMessageQueue.erase(
			std::remove_if(
				CdmReminderMessageQueue.begin(),
				CdmReminderMessageQueue.end(),
				[&](const QueuedCdmReminderMessage& queued)
				{
					return queued.callsign == callsign;
				}),
			CdmReminderMessageQueue.end());
	}

	std::string NormalizeCallsignForState(const std::string& callsign)
	{
		return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	}

	bool HasDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return false;
		return AircraftDatalinkClearedCallsigns.find(normalizedCallsign) != AircraftDatalinkClearedCallsigns.end();
	}

	bool HasDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		return !normalizedCallsign.empty() &&
			AircraftDatalinkClearanceInFlightCallsigns.find(normalizedCallsign) !=
			AircraftDatalinkClearanceInFlightCallsigns.end();
	}

	bool HasCdmReminderSubmittedUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		return !normalizedCallsign.empty() &&
			AircraftCdmReminderSubmittedCallsigns.find(normalizedCallsign) !=
			AircraftCdmReminderSubmittedCallsigns.end();
	}

	void MarkCdmReminderSubmittedUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (!normalizedCallsign.empty())
			AircraftCdmReminderSubmittedCallsigns.insert(normalizedCallsign);
	}

	void MarkDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;
		AircraftDatalinkClearanceInFlightCallsigns.insert(normalizedCallsign);
		RemoveQueuedCdmReminderUnlocked(normalizedCallsign);
	}

	void ClearDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (!normalizedCallsign.empty())
			AircraftDatalinkClearanceInFlightCallsigns.erase(normalizedCallsign);
	}

	void MarkDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.insert(normalizedCallsign);
		AircraftDatalinkClearanceInFlightCallsigns.erase(normalizedCallsign);
		RemoveQueuedCdmReminderUnlocked(normalizedCallsign);
	}

	void ClearDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.erase(normalizedCallsign);
	}

	CdmQueueReminderOutcome TryQueueCdmReminderForCallsign(
		EuroScopePlugIn::CPlugIn* plugIn,
		const std::string& callsign,
		const std::string& reminderMessage,
		std::chrono::steady_clock::time_point now,
		bool* outCdmEvaluated,
		bool* outHasCdmData,
		bool automatic)
	{
		if (outCdmEvaluated != nullptr)
			*outCdmEvaluated = false;
		if (outHasCdmData != nullptr)
			*outHasCdmData = false;

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		const std::string activeAirport = ResolveActiveAirportFilterUpper();
		if (plugIn == nullptr || normalizedCallsign.empty() ||
			activeAirport.empty() || reminderMessage.empty())
			return CdmQueueReminderOutcome::Failed;
		if (!IsCallsignEligibleForCdmReminderNow(plugIn, normalizedCallsign))
			return CdmQueueReminderOutcome::Failed;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (automatic && HasCdmReminderSubmittedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, now))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign) ||
				HasDatalinkClearanceInFlightUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
		}

		CdmPilotData pilotData;
		const bool hasCdmData = TryGetCdmPilotData(normalizedCallsign, pilotData);
		if (outCdmEvaluated != nullptr)
			*outCdmEvaluated = true;
		if (outHasCdmData != nullptr)
			*outHasCdmData = hasCdmData;

		if (hasCdmData && HasSubmittedTobtState(pilotData))
			return CdmQueueReminderOutcome::HasSubmittedTobt;

		// Recheck after the unlocked CDM bridge lookup before committing to the queue.
		if (ResolveActiveAirportFilterUpper() != activeAirport ||
			!IsCallsignEligibleForCdmReminderNow(plugIn, normalizedCallsign))
		{
			return CdmQueueReminderOutcome::Failed;
		}
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (automatic && HasCdmReminderSubmittedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, now))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign) ||
				HasDatalinkClearanceInFlightUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
			if (!QueueCdmReminderUnlocked(
				normalizedCallsign,
				activeAirport,
				reminderMessage,
				automatic))
				return CdmQueueReminderOutcome::Failed;
		}

		return CdmQueueReminderOutcome::Queued;
	}

	void ClearCdmAutoTrackingState(bool clearQueuedAutomaticReminders)
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		const bool hadAutomaticQueue = std::any_of(
			CdmReminderMessageQueue.begin(),
			CdmReminderMessageQueue.end(),
			[](const QueuedCdmReminderMessage& reminder)
			{
				return reminder.automatic;
			});
		if (!AircraftCdmAutoTracked.empty() || !CdmAutoTrackedAirport.empty() ||
			(clearQueuedAutomaticReminders && hadAutomaticQueue))
		{
			++CdmAutoSessionGeneration;
		}
		AircraftCdmAutoTracked.clear();
		CdmAutoTrackedAirport.clear();
		if (clearQueuedAutomaticReminders)
		{
			CdmReminderMessageQueue.erase(
				std::remove_if(
					CdmReminderMessageQueue.begin(),
					CdmReminderMessageQueue.end(),
					[](const QueuedCdmReminderMessage& reminder)
					{
						return reminder.automatic;
					}),
				CdmReminderMessageQueue.end());
		}
	}

	void ResetCdmReminderSessionState()
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftCdmAutoTracked.clear();
		CdmAutoTrackedAirport.clear();
		CdmReminderMessageQueue.clear();
		AircraftCdmTobtReminderSentAt.clear();
		AircraftCdmReminderSubmittedCallsigns.clear();
		AircraftDatalinkClearedCallsigns.clear();
		AircraftDatalinkClearanceInFlightCallsigns.clear();
		++CdmAutoSessionGeneration;
	}

	bool ContainsCallsignUnlocked(const std::vector<std::string>& collection, const std::string& callsign)
	{
		return std::find(collection.begin(), collection.end(), callsign) != collection.end();
	}

	void AddCallsignUniqueUnlocked(std::vector<std::string>& collection, const std::string& callsign)
	{
		if (!ContainsCallsignUnlocked(collection, callsign))
			collection.push_back(callsign);
	}

	void RemoveCallsignUnlocked(std::vector<std::string>& collection, const std::string& callsign)
	{
		collection.erase(std::remove(collection.begin(), collection.end(), callsign), collection.end());
	}

	std::filesystem::path ResolveDefaultProfilesConfigPath()
	{
		const std::filesystem::path pluginDirectory =
			std::filesystem::u8path(Logger::DLL_PATH);
		const std::filesystem::path dataConfigPath = pluginDirectory / "vSMR_Data" / "vSMR_Profiles.json";

		std::error_code ec;
		if (std::filesystem::exists(dataConfigPath, ec))
			return dataConfigPath;
		return pluginDirectory / "vSMR_Profiles.json";
	}

	std::string FormatUtcHhmm(std::time_t utcTime)
	{
		if (utcTime <= 0)
			return "";
		std::tm utc = {};
		if (::gmtime_s(&utc, &utcTime) != 0)
			return "";
		char value[5] = {};
		if (std::strftime(value, sizeof(value), "%H%M", &utc) != 4)
			return "";
		return value;
	}

	bool HasSubmittedTobtState(const CdmPilotData& pilotData)
	{
		if (!pilotData.hasTobt)
			return false;

		const std::string state = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(pilotData.tobtState));
		if (state == "FLIGHTPLAN" || state == "GUESS" || state == "MISSING")
			return false;
		return true;
	}

	bool StartsWithTokenCaseInsensitive(const std::string& text, const std::string& token)
	{
		if (text.size() < token.size())
			return false;
		for (size_t i = 0; i < token.size(); ++i)
		{
			const unsigned char lhs = static_cast<unsigned char>(text[i]);
			const unsigned char rhs = static_cast<unsigned char>(token[i]);
			if (std::tolower(lhs) != std::tolower(rhs))
				return false;
		}
		if (text.size() == token.size())
			return true;
		return std::isspace(static_cast<unsigned char>(text[token.size()])) != 0;
	}

	std::string StripEnclosingQuotesCopy(const std::string& text)
	{
		if (text.size() >= 2)
		{
			const char first = text.front();
			const char last = text.back();
			if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
				return text.substr(1, text.size() - 2);
		}
		return text;
	}

	std::filesystem::path NormalizeAliasPathForLookup(const std::filesystem::path& path)
	{
		if (path.empty())
			return path;

		std::error_code ec;
		const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
		if (!ec && !absolute.empty())
			return absolute.lexically_normal();

		return path.lexically_normal();
	}

	void AppendAliasPathCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& path)
	{
		if (path.empty())
			return;

		const std::filesystem::path normalizedPath = NormalizeAliasPathForLookup(path);
		if (std::find(candidates.begin(), candidates.end(), normalizedPath) != candidates.end())
			return;

		candidates.push_back(normalizedPath);
	}

	std::filesystem::path ResolveCdmAliasPath(EuroScopePlugIn::CPlugIn* plugIn)
	{
		std::vector<std::filesystem::path> candidates;

		std::filesystem::path processDirectory;
		std::wstring modulePath(32768, L'\0');
		const DWORD modulePathLength = ::GetModuleFileNameW(
			nullptr,
			modulePath.data(),
			static_cast<DWORD>(modulePath.size()));
		if (modulePathLength > 0 && modulePathLength < modulePath.size())
		{
			modulePath.resize(modulePathLength);
			processDirectory = std::filesystem::path(modulePath).parent_path();
		}

		if (plugIn != nullptr)
		{
			const char* configuredAliasPathRaw = plugIn->GetDataFromSettings("alias");
			if (configuredAliasPathRaw != nullptr)
			{
				std::string configuredAliasPath = TrimAsciiWhitespaceCopy(configuredAliasPathRaw);
				configuredAliasPath = StripEnclosingQuotesCopy(configuredAliasPath);
				configuredAliasPath = TrimAsciiWhitespaceCopy(configuredAliasPath);
				if (!configuredAliasPath.empty())
				{
					const std::filesystem::path configuredPath(configuredAliasPath);
					AppendAliasPathCandidate(candidates, configuredPath);
					if (!configuredPath.is_absolute() && !processDirectory.empty())
						AppendAliasPathCandidate(candidates, processDirectory / configuredPath);
				}
			}
		}

		if (!processDirectory.empty())
			AppendAliasPathCandidate(candidates, processDirectory / "Alias" / "alias.txt");

		if (!Logger::DLL_PATH.empty())
		{
			const std::filesystem::path pluginDir =
				std::filesystem::u8path(Logger::DLL_PATH);
			AppendAliasPathCandidate(candidates, pluginDir / ".." / ".." / "Alias" / "alias.txt");
			AppendAliasPathCandidate(candidates, pluginDir / ".." / "Alias" / "alias.txt");
		}

		AppendAliasPathCandidate(candidates, std::filesystem::path("alias.txt"));

		std::error_code ec;
		for (const std::filesystem::path& candidate : candidates)
		{
			if (std::filesystem::exists(candidate, ec))
				return candidate;
			ec.clear();
		}

		if (!candidates.empty())
			return candidates.front();

		return std::filesystem::path("alias.txt");
	}

	bool TryReadCdmReminderMessageFromAlias(EuroScopePlugIn::CPlugIn* plugIn, std::string& outMessage, std::string& outAliasPath)
	{
		outMessage.clear();
		const std::filesystem::path aliasPath = ResolveCdmAliasPath(plugIn);
		outAliasPath = aliasPath.string();

		std::ifstream input(aliasPath);
		if (!input.is_open())
			return false;

		std::string line;
		while (std::getline(input, line))
		{
			std::string working = TrimAsciiWhitespaceCopy(line);
			if (working.empty())
				continue;
			if (working[0] == ';' || working[0] == '#')
				continue;
			if (!StartsWithTokenCaseInsensitive(working, ".cdm"))
				continue;

			working = TrimAsciiWhitespaceCopy(working.substr(4));
			if (StartsWithTokenCaseInsensitive(working, ".msg"))
				working = TrimAsciiWhitespaceCopy(working.substr(4));
			if (StartsWithTokenCaseInsensitive(working, "$aircraft"))
				working = TrimAsciiWhitespaceCopy(working.substr(9));
			if (working.empty())
				continue;

			outMessage = working;
			return true;
		}

		return false;
	}

	void NotifyMissingCdmAliasMessage(EuroScopePlugIn::CPlugIn* plugIn, const std::string& aliasPath)
	{
		static std::time_t lastWarningUtc = 0;
		const std::time_t nowUtc = std::time(nullptr);
		if (plugIn == nullptr || nowUtc <= 0)
			return;
		if (lastWarningUtc != 0 && std::difftime(nowUtc, lastWarningUtc) < static_cast<double>(CdmWarningCooldownSeconds))
			return;

		lastWarningUtc = nowUtc;
		const std::string detail = "Missing/invalid .cdm alias in " + aliasPath;
		plugIn->DisplayUserMessage("vSMR", "CDM", detail.c_str(), true, true, false, true, false);
		Logger::info("CDM alias load failed path=" + aliasPath);
	}

	bool TryLoadCdmReminderMessage(EuroScopePlugIn::CPlugIn* plugIn, std::string& outMessage)
	{
		std::string aliasPath;
		if (TryReadCdmReminderMessageFromAlias(plugIn, outMessage, aliasPath))
			return true;

		NotifyMissingCdmAliasMessage(plugIn, aliasPath);
		return false;
	}

	bool IsCallsignEligibleForCdmReminderNow(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign)
	{
		if (plugIn == nullptr || callsign.empty() ||
			!plugIn->ControllerMyself().IsController() ||
			!IsCdmBridgeReady())
			return false;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasDatalinkClearanceSentUnlocked(callsign) ||
				HasDatalinkClearanceInFlightUnlocked(callsign))
				return false;
		}

		const std::string activeAirport = ResolveActiveAirportFilterUpper();
		const std::vector<std::string> eligibleCallsigns =
			CollectFlightPlanCandidateCallsignsForActiveAirport(plugIn, activeAirport);
		if (std::find(eligibleCallsigns.begin(), eligibleCallsigns.end(), callsign) == eligibleCallsigns.end())
			return false;

		CdmPilotData pilotData;
		if (TryGetCdmPilotData(callsign, pilotData) && HasSubmittedTobtState(pilotData))
			return false;

		return true;
	}

	CdmChatSubmissionStatus PollPrivateChatMessageSubmission()
	{
		switch (VsmrEuroScopeCommandLine::Poll(
			VsmrEuroScopeCommandLine::Owner::CdmReminder))
		{
		case VsmrEuroScopeCommandLine::SubmissionStatus::Pending:
			return CdmChatSubmissionStatus::Pending;
		case VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed:
			return CdmChatSubmissionStatus::Confirmed;
		case VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous:
			return CdmChatSubmissionStatus::Ambiguous;
		default:
			return CdmChatSubmissionStatus::Idle;
		}
	}

	bool BeginPrivateChatMessageLikeDotMsg(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign, const std::string& message)
	{
		if (plugIn == nullptr)
			return false;

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		const std::string normalizedMessage = TrimAsciiWhitespaceCopy(message);
		if (normalizedCallsign.empty() || normalizedMessage.empty())
			return false;

		const std::string command =
			".msg " + normalizedCallsign + " " + normalizedMessage;
		return VsmrEuroScopeCommandLine::Begin(
			VsmrEuroScopeCommandLine::Owner::CdmReminder,
			command);
	}

	bool SendDatalinkPacketMessage(const DatalinkMessageRequest& request)
	{
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(
				std::memory_order_acquire))
			return false;

		string raw;
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=";
		url += EncodeUrlQueryComponent(request.destination);
		url += "&type=";
		url += EncodeUrlQueryComponent(request.type);
		url += "&packet=";
		url += EncodeUrlQueryComponent(request.packet);

		raw.assign(VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes));
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(
				std::memory_order_acquire))
			return false;

		if (!VsmrRadarUiSupport::startsWith("ok", raw.c_str()))
			return false;

		if (!request.callsign.empty())
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			PendingMessages.erase(request.callsign);
			RemoveCallsignUnlocked(AircraftMessage, request.callsign);
			AddCallsignUniqueUnlocked(AircraftMessageSent, request.callsign);
		}

		return true;
	}

	bool QueueDatalinkMessage(
		CSMRPlugin* plugin,
		const std::string& destination,
		const std::string& type,
		const std::string& packet,
		const std::string& callsign)
	{
		if (plugin == nullptr ||
			PluginShutdownRequested.load(std::memory_order_acquire))
		{
			return false;
		}
		DatalinkMessageRequest request;
		request.credentials = SnapshotDatalinkCredentials();
		request.generation = HoppieConnectionGeneration.load(
			std::memory_order_acquire);
		request.destination = destination;
		request.type = type;
		request.packet = packet;
		request.callsign = callsign;
		return plugin->QueueNetworkJob([request]() {
			(void)SendDatalinkPacketMessage(request);
		});
	}
