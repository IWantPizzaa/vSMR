#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashReporter.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
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
#include <sstream>

#include "rapidjson/document.h"

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


// Snapshot cache of the latest vACDM pilot data keyed by normalized callsign.
std::mutex VacdmPilotsMutex;
std::map<std::string, VacdmPilotData> VacdmPilots;
std::atomic<bool> VacdmFetchInProgress(false);
std::atomic<PluginSteadyTick> VacdmLastFetchTick(0);
const int VacdmFetchIntervalSeconds = 15;
const std::string VacdmPilotsUrlDefault = "https://app.vacdm.net/api/v1/pilots";
std::mutex ProfilesSourceMutex;
std::string ActiveProfilesConfigPath;
bool ActiveProfilesConfigPathClaimed = false;
unsigned long long ProfilesSourceGeneration = 0;
// Guarded by ProfilesSourceMutex. Workers consume only a copied snapshot.
std::string VacdmConfiguredServerUrl;
std::atomic<bool> VacdmPollingEnabled(false);
std::atomic<unsigned long> VacdmFetchCounter(0);
std::atomic<unsigned long> VacdmLastSehCode(0);
std::mutex VacdmDebugStateMutex;
std::string VacdmDebugAselCallsign;
unsigned long long VacdmSuccessfulSnapshotSourceGeneration = 0;
std::chrono::steady_clock::time_point VacdmSuccessfulSnapshotAt;

	const std::time_t CdmWarningCooldownSeconds = 60;
	const int CdmReminderQueueMaxSendAttempts = 20;
	const int CdmReminderRetryDelaySeconds = 5;
	const int CdmMaximumMinutes = 24 * 60;
	const int VacdmSnapshotMaximumAgeSeconds = VacdmFetchIntervalSeconds * 4;
	const size_t HoppieResponseLimitBytes = 1024U * 1024U;
	const size_t VacdmResponseLimitBytes = 16U * 1024U * 1024U;

PluginSteadyTick CurrentSteadyTick() noexcept
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		PluginSteadyClock::now().time_since_epoch()).count();
}

bool HasSteadyIntervalElapsed(
	PluginSteadyTick now,
	PluginSteadyTick previous,
	std::chrono::seconds interval) noexcept
{
	return previous == 0 ||
		now - previous >=
			std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();
}
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

	bool HasSubmittedTobtState(const VacdmPilotData& pilotData);

	std::string KeepAsciiAlnumCopy(const std::string& text)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (char c : text)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}
		return normalized;
	}

	std::string StripAtFirstCallsignDelimiter(const std::string& text)
	{
		const size_t pos = text.find_first_of("/\\ _.-");
		if (pos == std::string::npos)
			return text;
		return text.substr(0, pos);
	}

	std::vector<std::string> BuildVacdmLookupCandidates(const std::string& callsign)
	{
		std::vector<std::string> candidates;
		auto pushUnique = [&](const std::string& value) {
			const std::string candidate = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(value));
			if (candidate.empty())
				return;
			if (std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
				return;
			candidates.push_back(candidate);
			};

		const std::string trimmed = TrimAsciiWhitespaceCopy(callsign);
		pushUnique(trimmed);
		pushUnique(StripAtFirstCallsignDelimiter(trimmed));
		pushUnique(KeepAsciiAlnumCopy(trimmed));

		const size_t slashPos = trimmed.find('/');
		if (slashPos != std::string::npos)
			pushUnique(trimmed.substr(0, slashPos));

		return candidates;
	}

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

	bool IsGroundTargetForCdm(const CFlightPlan& fp)
	{
		CRadarTarget correlatedTarget = fp.GetCorrelatedRadarTarget();
		if (!correlatedTarget.IsValid())
			return false;
		return correlatedTarget.GetGS() <= 60;
	}

	std::string ResolveActiveAirportFilterUpper()
	{
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr)
				continue;
			std::string airport = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rd->getActiveAirport()));
			if (airport.size() > 4)
				airport = airport.substr(0, 4);
			return airport;
		}

		return "";
	}

	bool IsVacdmSnapshotReadyForCdm()
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		if (!VacdmPollingEnabled.load(std::memory_order_acquire) ||
			VacdmSuccessfulSnapshotSourceGeneration != ProfilesSourceGeneration ||
			VacdmSuccessfulSnapshotAt == std::chrono::steady_clock::time_point())
		{
			return false;
		}

		return std::chrono::steady_clock::now() - VacdmSuccessfulSnapshotAt <=
			std::chrono::seconds(VacdmSnapshotMaximumAgeSeconds);
	}

	std::vector<std::string> CollectFlightPlanCandidateCallsignsForActiveAirport(
		EuroScopePlugIn::CPlugIn* plugIn,
		const std::string& activeAirportFilter)
	{
		std::vector<std::string> candidateCallsigns;
		if (plugIn == nullptr || activeAirportFilter.empty())
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
			if (origin != activeAirportFilter)
				continue;
			if (!IsGroundTargetForCdm(fp))
				continue;
			if (!IsNoStatusGroundState(fp.GetGroundState()))
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
		const std::string& message,
		bool automatic)
	{
		if (callsign.empty() || message.empty())
			return false;
		if (IsCdmReminderQueuedUnlocked(callsign))
			return false;

		QueuedCdmReminderMessage queued;
		queued.callsign = callsign;
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
		const std::string& callsign,
		const std::string& reminderMessage,
		std::chrono::steady_clock::time_point now,
		bool* outVacdmEvaluated,
		bool* outHasVacdmData,
		bool automatic)
	{
		if (outVacdmEvaluated != nullptr)
			*outVacdmEvaluated = false;
		if (outHasVacdmData != nullptr)
			*outHasVacdmData = false;

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		if (normalizedCallsign.empty() || reminderMessage.empty())
			return CdmQueueReminderOutcome::Failed;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, now))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign) ||
				HasDatalinkClearanceInFlightUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
		}

		VacdmPilotData pilotData;
		const bool hasVacdmData = TryGetVacdmPilotData(normalizedCallsign, pilotData);
		if (outVacdmEvaluated != nullptr)
			*outVacdmEvaluated = true;
		if (outHasVacdmData != nullptr)
			*outHasVacdmData = hasVacdmData;

		if (hasVacdmData && HasSubmittedTobtState(pilotData))
			return CdmQueueReminderOutcome::HasSubmittedTobt;

		// Rechecking after the unlocked vACDM lookup before committing to the queue
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, now))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign) ||
				HasDatalinkClearanceInFlightUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
			if (!QueueCdmReminderUnlocked(normalizedCallsign, reminderMessage, automatic))
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

	std::string NormalizeVacdmServerUrl(std::string value)
	{
		value = TrimAsciiWhitespaceCopy(value);
		while (!value.empty() && value.back() == '/')
			value.pop_back();
		std::string host;
		if (value.find('?') != std::string::npos ||
			!HttpHelper::IsValidHttpsUrl(value, &host))
		{
			return "";
		}
		const bool numericHost = !host.empty() &&
			std::all_of(host.begin(), host.end(), [](unsigned char character) {
				return std::isdigit(character) != 0 || character == '.';
			});
		const bool localHost = host == "localhost" ||
			(host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0) ||
			(host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0);
		if (numericHost || localHost || host.find('.') == std::string::npos)
			return "";
		return value;
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

	bool TryReadVacdmServerUrl(
		const std::filesystem::path& configPath,
		std::string& outServerUrl)
	{
		outServerUrl.clear();
		if (configPath.empty())
			return false;
		std::ifstream input(configPath, std::ios::binary);

		if (!input.is_open())
			return false;

		std::stringstream buffer;
		buffer << input.rdbuf();
		std::string json = buffer.str();
		if (json.size() >= 3 &&
			static_cast<unsigned char>(json[0]) == 0xEF &&
			static_cast<unsigned char>(json[1]) == 0xBB &&
			static_cast<unsigned char>(json[2]) == 0xBF)
		{
			json = json.substr(3);
		}

		rapidjson::Document document;
		if (document.Parse<0>(json.c_str()).HasParseError() || !document.IsArray())
			return false;

		for (rapidjson::SizeType i = 0; i < document.Size(); ++i)
		{
			const rapidjson::Value& entry = document[i];
			if (!entry.IsObject() ||
				!entry.HasMember("_vsmr") ||
				!entry["_vsmr"].IsObject())
			{
				continue;
			}

			const rapidjson::Value& metadata = entry["_vsmr"];
			if (!metadata.HasMember("vacdm") || !metadata["vacdm"].IsObject())
				continue;

			const rapidjson::Value& vacdm = metadata["vacdm"];
			if (!vacdm.HasMember("server_url") || !vacdm["server_url"].IsString())
				continue;

			const std::string value = NormalizeVacdmServerUrl(vacdm["server_url"].GetString());
			if (value.empty())
				continue;

			outServerUrl = value;
			return true;
		}

		return false;
	}

	std::string ResolveVacdmPilotsUrl(unsigned long long* sourceGeneration)
	{
		std::string serverUrl;
		{
			std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
			serverUrl = VacdmConfiguredServerUrl;
			if (sourceGeneration != nullptr)
				*sourceGeneration = ProfilesSourceGeneration;
		}
		if (!serverUrl.empty())
			return serverUrl + "/api/v1/pilots";
		return VacdmPilotsUrlDefault;
	}

	bool TryParseIsoUtcTimestamp(const std::string& iso, std::time_t& outUtc)
	{
		outUtc = 0;
		if (iso.size() < 19)
			return false;

		int year = 0;
		int month = 0;
		int day = 0;
		int hour = 0;
		int minute = 0;
		int second = 0;
		if (::sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
			return false;

		std::tm tmUtc = {};
		tmUtc.tm_year = year - 1900;
		tmUtc.tm_mon = month - 1;
		tmUtc.tm_mday = day;
		tmUtc.tm_hour = hour;
		tmUtc.tm_min = minute;
		tmUtc.tm_sec = second;
		tmUtc.tm_isdst = 0;
		std::time_t parsed = _mkgmtime(&tmUtc);
		if (parsed <= 0)
			return false;

		outUtc = parsed;
		return true;
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

	std::string FormatSehCode(unsigned long code)
	{
		char buffer[16] = {};
		sprintf_s(buffer, "0x%08lX", code);
		return std::string(buffer);
	}

	int CaptureVacdmSehCode(unsigned long sehCode)
	{
		VacdmLastSehCode.store(sehCode, std::memory_order_relaxed);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool HasSubmittedTobtState(const VacdmPilotData& pilotData)
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
			!IsVacdmSnapshotReadyForCdm())
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

		VacdmPilotData pilotData;
		if (TryGetVacdmPilotData(callsign, pilotData) && HasSubmittedTobtState(pilotData))
			return false;

		return true;
	}

	bool IsLikelyCommandEditControl(HWND hwnd)
	{
		if (hwnd == nullptr || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd) || !::IsWindowEnabled(hwnd))
			return false;

		char className[64] = {};
		if (::GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0)
			return false;
		const std::string classUpper = ToUpperAsciiCopy(className);
		if (!(classUpper == "EDIT" || classUpper.find("RICHEDIT") != std::string::npos))
			return false;

		const LONG style = ::GetWindowLong(hwnd, GWL_STYLE);
		if ((style & ES_READONLY) != 0 || (style & ES_MULTILINE) != 0)
			return false;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return false;
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;
		if (width < 120 || height < 12)
			return false;

		return true;
	}

	struct MainWindowSearchContext
	{
		DWORD processId = 0;
		HWND bestWindow = nullptr;
		LONG bestArea = 0;
	};

	BOOL CALLBACK EnumMainWindowsForCurrentProcess(HWND hwnd, LPARAM lParam)
	{
		MainWindowSearchContext* context = reinterpret_cast<MainWindowSearchContext*>(lParam);
		if (context == nullptr)
			return TRUE;

		DWORD windowProcessId = 0;
		::GetWindowThreadProcessId(hwnd, &windowProcessId);
		if (windowProcessId != context->processId)
			return TRUE;
		if (!::IsWindowVisible(hwnd))
			return TRUE;
		if (::GetWindow(hwnd, GW_OWNER) != nullptr)
			return TRUE;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return TRUE;
		LONG width = rect.right - rect.left;
		LONG height = rect.bottom - rect.top;
		if (width < 0)
			width = 0;
		if (height < 0)
			height = 0;
		const LONG area = width * height;
		if (area > context->bestArea)
		{
			context->bestArea = area;
			context->bestWindow = hwnd;
		}

		return TRUE;
	}

	struct CommandEditSearchContext
	{
		RECT mainRect = {};
		HWND bestEdit = nullptr;
		LONG bestScore = LONG_MIN;
	};

	BOOL CALLBACK EnumCommandEditControls(HWND hwnd, LPARAM lParam)
	{
		CommandEditSearchContext* context = reinterpret_cast<CommandEditSearchContext*>(lParam);
		if (context == nullptr)
			return TRUE;
		if (!IsLikelyCommandEditControl(hwnd))
			return TRUE;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return TRUE;
		const LONG style = ::GetWindowLong(hwnd, GWL_STYLE);
		const int width = rect.right - rect.left;

		LONG score = rect.top;
		score += width / 4;
		if ((style & WS_TABSTOP) != 0)
			score += 1000;
		if ((style & ES_AUTOHSCROLL) != 0)
			score += 500;
		if (rect.bottom >= context->mainRect.bottom - 80)
			score += 2000;

		if (score > context->bestScore)
		{
			context->bestScore = score;
			context->bestEdit = hwnd;
		}

		return TRUE;
	}

	HWND FindEuroScopeCommandEditControl()
	{
		HWND focusedWindow = ::GetFocus();
		if (IsLikelyCommandEditControl(focusedWindow))
			return focusedWindow;

		MainWindowSearchContext mainContext;
		mainContext.processId = ::GetCurrentProcessId();
		::EnumWindows(EnumMainWindowsForCurrentProcess, reinterpret_cast<LPARAM>(&mainContext));
		if (mainContext.bestWindow == nullptr)
			return nullptr;

		CommandEditSearchContext editContext;
		::GetWindowRect(mainContext.bestWindow, &editContext.mainRect);
		::EnumChildWindows(mainContext.bestWindow, EnumCommandEditControls, reinterpret_cast<LPARAM>(&editContext));
		return editContext.bestEdit;
	}

	bool ExecuteEuroScopeCommandViaUi(const std::string& command)
	{
		const std::string trimmed = TrimAsciiWhitespaceCopy(command);
		if (trimmed.empty())
			return false;

		HWND editControl = FindEuroScopeCommandEditControl();
		if (editControl == nullptr)
			return false;

		// EuroScope does not expose an API for sending a private chat message, so
		// the CDM reminder uses its command edit. Preserve any command/message the
		// controller is composing, including the selection, and restore it after
		// the injected command is handled synchronously.
		constexpr int kMaximumPreservedCommandCharacters = 64 * 1024;
		const int originalTextLength = ::GetWindowTextLengthA(editControl);
		if (originalTextLength < 0 || originalTextLength > kMaximumPreservedCommandCharacters)
			return false;
		std::vector<char> originalTextBuffer(static_cast<size_t>(originalTextLength) + 1U, '\0');
		if (originalTextLength > 0 &&
			::GetWindowTextA(
				editControl,
				originalTextBuffer.data(),
				static_cast<int>(originalTextBuffer.size())) != originalTextLength)
		{
			return false;
		}
		const std::string originalText(originalTextBuffer.data(), static_cast<size_t>(originalTextLength));
		DWORD selectionStart = 0;
		DWORD selectionEnd = 0;
		::SendMessageA(
			editControl,
			EM_GETSEL,
			reinterpret_cast<WPARAM>(&selectionStart),
			reinterpret_cast<LPARAM>(&selectionEnd));
		HWND originalFocus = ::GetFocus();

		auto restoreControllerInput = [&]()
		{
			if (!::IsWindow(editControl))
				return;
			::SetWindowTextA(editControl, originalText.c_str());
			::SendMessageA(
				editControl,
				EM_SETSEL,
				static_cast<WPARAM>(selectionStart),
				static_cast<LPARAM>(selectionEnd));
			if (originalFocus != nullptr && ::IsWindow(originalFocus) && ::GetFocus() != originalFocus)
				::SetFocus(originalFocus);
		};

		DWORD_PTR messageResult = 0;
		if (::SendMessageTimeoutA(
			editControl,
			WM_SETTEXT,
			0,
			reinterpret_cast<LPARAM>(trimmed.c_str()),
			SMTO_ABORTIFHUNG,
			250,
			&messageResult) == 0)
		{
			restoreControllerInput();
			return false;
		}

		DWORD_PTR keyDownResult = 0;
		const bool keyDownHandled = ::SendMessageTimeoutA(
			editControl,
			WM_KEYDOWN,
			VK_RETURN,
			0,
			SMTO_ABORTIFHUNG,
			250,
			&keyDownResult) != 0;
		DWORD_PTR keyUpResult = 0;
		const bool keyUpHandled = ::SendMessageTimeoutA(
			editControl,
			WM_KEYUP,
			VK_RETURN,
			0,
			SMTO_ABORTIFHUNG,
			250,
			&keyUpResult) != 0;
		restoreControllerInput();
		// EuroScope submits the command on key-down. A delayed key-up must not
		// make us retry a message that was already sent.
		if (keyDownHandled && !keyUpHandled)
			Logger::info("CDM command key-up timed out after successful submission");
		return keyDownHandled;
	}

	bool SendPrivateChatMessageLikeDotMsg(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign, const std::string& message)
	{
		if (plugIn == nullptr)
			return false;

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		if (normalizedCallsign.empty() || message.empty())
			return false;

		const std::string command = ".msg " + normalizedCallsign + " " + message;
		if (ExecuteEuroScopeCommandViaUi(command))
			return true;

		static std::time_t lastInjectionWarningUtc = 0;
		const std::time_t nowUtc = std::time(nullptr);
		if (nowUtc > 0 && (lastInjectionWarningUtc == 0 || std::difftime(nowUtc, lastInjectionWarningUtc) >= static_cast<double>(CdmWarningCooldownSeconds)))
		{
			lastInjectionWarningUtc = nowUtc;
			plugIn->DisplayUserMessage("vSMR", "CDM", "Failed to inject .msg command into EuroScope command line.", true, true, false, true, false);
		}
		Logger::info("CDM .msg inject failed callsign=" + normalizedCallsign);
		return false;
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
