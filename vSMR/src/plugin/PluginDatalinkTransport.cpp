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

#include "plugin/PluginRuntimeAudio.hpp"

namespace
{
	bool CdmReminderSubmissionInFlight = false;
	QueuedCdmReminderMessage CdmReminderBeingSubmitted;
}

HttpHelper& VsmrPluginRuntime::GetHttpHelper()
{
	static HttpHelper helper;
	return helper;
}

bool TryGetVacdmPilotData(const std::string& callsign, VacdmPilotData& outData)
{
	std::lock_guard<std::mutex> guard(VacdmPilotsMutex);
	// Match with the same normalization strategy used during ingest.
	const std::vector<std::string> candidates = BuildVacdmLookupCandidates(callsign);
	for (const auto& candidate : candidates)
	{
		auto it = VacdmPilots.find(candidate);
		if (it != VacdmPilots.end())
		{
			outData = it->second;
			return true;
		}
	}
	return false;
}

void ProcessCdmAutoMode(CSMRPlugin* plugIn)
{
	if (plugIn == nullptr || !CdmAutoModeEnabled.load(std::memory_order_relaxed))
		return;

	const std::string activeAirport = ResolveActiveAirportFilterUpper();
	if (!plugIn->ControllerMyself().IsController() || activeAirport.empty() ||
		!IsVacdmSnapshotReadyForCdm())
	{
		ClearCdmAutoTrackingState(true);
		return;
	}

	int delayMinutes = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	if (delayMinutes < 0)
		delayMinutes = 0;
	const auto now = std::chrono::steady_clock::now();
	const auto delay = std::chrono::minutes(delayMinutes);

	// Collecting current reminder candidates
	const std::vector<std::string> connectedCallsigns =
		CollectFlightPlanCandidateCallsignsForActiveAirport(plugIn, activeAirport);
	struct CandidateSnapshot
	{
		std::string callsign;
		bool hasSubmittedTobt = false;
	};
	std::vector<CandidateSnapshot> candidates;
	candidates.reserve(connectedCallsigns.size());
	for (const std::string& callsign : connectedCallsigns)
	{
		VacdmPilotData pilotData;
		const bool hasPilotData = TryGetVacdmPilotData(callsign, pilotData);
		candidates.push_back({
			callsign,
			hasPilotData && HasSubmittedTobtState(pilotData)
		});
	}

	std::vector<std::string> callsignsToQueue;
	callsignsToQueue.reserve(candidates.size());

	// Updating the per-aircraft delay state
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(now);

		if (CdmAutoTrackedAirport != activeAirport)
		{
			AircraftCdmAutoTracked.clear();
			CdmReminderMessageQueue.erase(
				std::remove_if(
					CdmReminderMessageQueue.begin(),
					CdmReminderMessageQueue.end(),
					[](const QueuedCdmReminderMessage& reminder)
					{
						return reminder.automatic;
					}),
				CdmReminderMessageQueue.end());
			CdmAutoTrackedAirport = activeAirport;
			++CdmAutoSessionGeneration;
		}

		for (auto it = AircraftCdmAutoTracked.begin(); it != AircraftCdmAutoTracked.end();)
		{
			if (std::find(connectedCallsigns.begin(), connectedCallsigns.end(), it->first) == connectedCallsigns.end())
			{
				if (CdmReminderCooldownMinutes.load(std::memory_order_relaxed) == 0)
					AircraftCdmTobtReminderSentAt.erase(it->first);
				RemoveQueuedCdmReminderUnlocked(it->first);
				it = AircraftCdmAutoTracked.erase(it);
			}
			else
				++it;
		}

		for (const CandidateSnapshot& candidate : candidates)
		{
			auto trackedIt = AircraftCdmAutoTracked.find(candidate.callsign);
			if (trackedIt == AircraftCdmAutoTracked.end())
			{
				CdmAutoTrackedAircraftState state;
				state.dueAt = now + delay;
				state.eligibility = candidate.hasSubmittedTobt
					? CdmAutoEligibility::SuppressedBySubmittedTobt
					: CdmAutoEligibility::WaitingForMissingTobt;
				trackedIt = AircraftCdmAutoTracked.emplace(candidate.callsign, state).first;
			}

			CdmAutoTrackedAircraftState& tracked = trackedIt->second;
			if (candidate.hasSubmittedTobt)
			{
				tracked.eligibility = CdmAutoEligibility::SuppressedBySubmittedTobt;
				continue;
			}

			// A TOBT that is later removed starts a new complete delay. It never
			// inherits an already elapsed timer from the suppressed period.
			if (tracked.eligibility == CdmAutoEligibility::SuppressedBySubmittedTobt)
			{
				if (CdmReminderCooldownMinutes.load(std::memory_order_relaxed) == 0)
					AircraftCdmTobtReminderSentAt.erase(candidate.callsign);
				tracked.dueAt = now + delay;
				tracked.eligibility = CdmAutoEligibility::WaitingForMissingTobt;
			}
			if (tracked.eligibility == CdmAutoEligibility::RetryExhausted ||
				HasCdmReminderSubmittedUnlocked(candidate.callsign) ||
				HasRecentCdmReminderUnlocked(candidate.callsign, now) ||
				IsCdmReminderQueuedUnlocked(candidate.callsign) ||
				HasDatalinkClearanceSentUnlocked(candidate.callsign) ||
				HasDatalinkClearanceInFlightUnlocked(candidate.callsign))
				continue;

			if (now >= tracked.dueAt)
				callsignsToQueue.push_back(candidate.callsign);
		}
	}

	// Queuing aircraft whose full delay has elapsed
	int queuedCount = 0;
	std::string reminderMessage;
	if (!callsignsToQueue.empty() && !TryLoadCdmReminderMessage(plugIn, reminderMessage))
	{
		ClearCdmAutoTrackingState(true);
		return;
	}

	for (const std::string& callsign : callsignsToQueue)
	{
		const CdmQueueReminderOutcome outcome =
			TryQueueCdmReminderForCallsign(
				plugIn,
				callsign,
				reminderMessage,
				now,
				nullptr,
				nullptr,
				true);
		if (outcome == CdmQueueReminderOutcome::Queued)
			++queuedCount;
	}

	if (queuedCount > 0)
		Logger::info("CDM auto reminder queued count=" + std::to_string(queuedCount) + " delay_min=" + std::to_string(delayMinutes));
}

void ProcessQueuedCdmReminderMessages(CSMRPlugin* plugIn)
{
	if (plugIn == nullptr)
		return;

	const auto now = std::chrono::steady_clock::now();
	const CdmChatSubmissionStatus submissionStatus =
		PollPrivateChatMessageSubmission();
	if (CdmReminderSubmissionInFlight)
	{
		if (submissionStatus == CdmChatSubmissionStatus::Pending)
			return;

		const std::string submittedCallsign =
			CdmReminderBeingSubmitted.callsign;
		if (submissionStatus == CdmChatSubmissionStatus::Confirmed)
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			MarkCdmReminderSentUnlocked(submittedCallsign, now);
			Logger::info(
				"CDM reminder command consumed by EuroScope callsign=" +
				submittedCallsign);
		}
		else
		{
			// An Enter key was posted, so retrying an unconfirmed result could send
			// a duplicate. Keep automatic delivery one-shot and require an operator
			// to review the aircraft before any manual follow-up.
			Logger::info(
				"CDM reminder submission became ambiguous; automatic retry suppressed callsign=" +
				submittedCallsign);
			plugIn->DisplayUserMessage(
				"vSMR",
				"CDM",
				("Could not confirm the CDM reminder for " + submittedCallsign +
					". It was not retried to avoid a duplicate.").c_str(),
				true, true, false, true, false);
		}
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			RemoveQueuedCdmReminderUnlocked(submittedCallsign);
		}
		CdmReminderSubmissionInFlight = false;
		CdmReminderBeingSubmitted = {};
		return;
	}
	if (submissionStatus != CdmChatSubmissionStatus::Idle)
		return;

	QueuedCdmReminderMessage queuedReminder;
	// Dropping stale sessions and taking the next ready reminder
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(now);
		CdmReminderMessageQueue.erase(
			std::remove_if(
				CdmReminderMessageQueue.begin(),
				CdmReminderMessageQueue.end(),
				[](const QueuedCdmReminderMessage& reminder)
				{
					return reminder.automatic &&
						(!CdmAutoModeEnabled.load(std::memory_order_relaxed) ||
							reminder.automaticSessionGeneration != CdmAutoSessionGeneration);
				}),
			CdmReminderMessageQueue.end());
		auto readyIt = std::find_if(
			CdmReminderMessageQueue.begin(),
			CdmReminderMessageQueue.end(),
			[&](const QueuedCdmReminderMessage& reminder)
			{
				return reminder.nextAttemptAt <= now;
			});
		if (readyIt == CdmReminderMessageQueue.end())
			return;

		queuedReminder = *readyIt;
		CdmReminderMessageQueue.erase(readyIt);
	}

	const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(queuedReminder.callsign));
	const std::string queuedAirport = ToUpperAsciiCopy(
		TrimAsciiWhitespaceCopy(queuedReminder.activeAirport));
	const std::string message = TrimAsciiWhitespaceCopy(queuedReminder.message);
	if (callsign.empty() || queuedAirport.empty() || message.empty())
		return;
	if (ResolveActiveAirportFilterUpper() != queuedAirport)
		return;

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		if (HasRecentCdmReminderUnlocked(callsign, now))
			return;
	}

	if (!IsCallsignEligibleForCdmReminderNow(plugIn, callsign))
		return;
	if (queuedReminder.automatic)
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		if (!CdmAutoModeEnabled.load(std::memory_order_relaxed) ||
			queuedReminder.automaticSessionGeneration != CdmAutoSessionGeneration ||
			HasCdmReminderSubmittedUnlocked(callsign) ||
			HasDatalinkClearanceSentUnlocked(callsign) ||
			HasDatalinkClearanceInFlightUnlocked(callsign))
		{
			return;
		}
	}

	// Rechecking eligibility immediately before posting the command. A queued
	// reminder is bound to the airport that was active when it was created.
	if (ResolveActiveAirportFilterUpper() != queuedAirport ||
		!IsCallsignEligibleForCdmReminderNow(plugIn, callsign))
	{
		return;
	}
	if (BeginPrivateChatMessageLikeDotMsg(plugIn, callsign, message))
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		MarkCdmReminderSubmittedUnlocked(callsign);
		CdmReminderBeingSubmitted = queuedReminder;
		CdmReminderSubmissionInFlight = true;
		return;
	}

	// Retrying transient command-line injection failures
	queuedReminder.sendAttempts += 1;
	if (queuedReminder.sendAttempts >= CdmReminderQueueMaxSendAttempts)
	{
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (queuedReminder.automatic &&
				queuedReminder.automaticSessionGeneration == CdmAutoSessionGeneration)
			{
				auto trackedIt = AircraftCdmAutoTracked.find(callsign);
				if (trackedIt != AircraftCdmAutoTracked.end())
					trackedIt->second.eligibility = CdmAutoEligibility::RetryExhausted;
			}
		}
		Logger::info("CDM reminder dropped after repeated UI injection failures callsign=" + callsign);
		plugIn->DisplayUserMessage(
			"vSMR",
			"CDM",
			("The CDM reminder for " + callsign +
				" was not submitted because EuroScope's command line was unavailable.").c_str(),
			true, true, false, true, false);
		return;
	}
	queuedReminder.nextAttemptAt = now + std::chrono::seconds(CdmReminderRetryDelaySeconds);

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		const bool automaticStillCurrent = !queuedReminder.automatic ||
			(CdmAutoModeEnabled.load(std::memory_order_relaxed) &&
				queuedReminder.automaticSessionGeneration == CdmAutoSessionGeneration);
		if (automaticStillCurrent &&
			!HasRecentCdmReminderUnlocked(callsign, now) &&
			!IsCdmReminderQueuedUnlocked(callsign))
			CdmReminderMessageQueue.push_back(queuedReminder);
	}
}

void refreshVacdmDataImpl()
{
	unsigned long long sourceGeneration = 0;
	const std::string pilotsUrl = ResolveVacdmPilotsUrl(&sourceGeneration);

	struct ResetFetchFlag
	{
		unsigned long long sourceGeneration = 0;
		explicit ResetFetchFlag(unsigned long long generation)
			: sourceGeneration(generation) {}

		~ResetFetchFlag()
		{
			bool sourceStillCurrent = false;
			{
				std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
				sourceStillCurrent =
					ProfilesSourceGeneration == sourceGeneration;
			}
			if (sourceStillCurrent)
				VacdmLastFetchTick = CurrentSteadyTick();
			VacdmFetchInProgress.store(false);
		}
	} reset{ sourceGeneration };

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		!VacdmPollingEnabled.load(std::memory_order_acquire))
		return;

	try
	{
		std::string raw = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
			pilotsUrl,
			6000,
			&PluginShutdownRequested,
			VacdmResponseLimitBytes);
		if (PluginShutdownRequested.load(std::memory_order_relaxed))
			return;

		if (raw.empty())
		{
			Logger::info("VACDM refresh failed: empty response url=" + pilotsUrl);
			return;
		}

		rapidjson::Document doc;
		if (doc.Parse<0>(raw.c_str()).HasParseError() || !doc.IsArray())
		{
			Logger::info("VACDM refresh failed: invalid JSON array url=" + pilotsUrl);
			return;
		}

		// Parse into a temporary map so readers never observe a partially refreshed cache.
		std::map<std::string, VacdmPilotData> parsedData;

		for (rapidjson::SizeType i = 0; i < doc.Size(); ++i)
		{
			const rapidjson::Value& pilot = doc[i];
			if (!pilot.IsObject() || !pilot.HasMember("callsign") || !pilot["callsign"].IsString())
				continue;

			VacdmPilotData data;
			data.callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(pilot["callsign"].GetString()));

			const rapidjson::Value* vacdm = nullptr;
			if (pilot.HasMember("vacdm") && pilot["vacdm"].IsObject())
				vacdm = &pilot["vacdm"];

			auto readTime = [&](const char* key, std::time_t& outTime, bool& outHas) {
				outTime = 0;
				outHas = false;
				if (vacdm == nullptr || !vacdm->HasMember(key) || !(*vacdm)[key].IsString())
					return;
				std::time_t parsed = 0;
				if (TryParseIsoUtcTimestamp((*vacdm)[key].GetString(), parsed))
				{
					outTime = parsed;
					outHas = true;
				}
				};

			readTime("tobt", data.tobtUtc, data.hasTobt);
			readTime("tsat", data.tsatUtc, data.hasTsat);
			readTime("ttot", data.ttotUtc, data.hasTtot);
			readTime("asat", data.asatUtc, data.hasAsat);
			readTime("aobt", data.aobtUtc, data.hasAobt);
			readTime("atot", data.atotUtc, data.hasAtot);
			readTime("asrt", data.asrtUtc, data.hasAsrt);
			readTime("aort", data.aortUtc, data.hasAort);
			readTime("ctot", data.ctotUtc, data.hasCtot);

			if (vacdm != nullptr && vacdm->HasMember("tobt_state") && (*vacdm)["tobt_state"].IsString())
				data.tobtState = (*vacdm)["tobt_state"].GetString();

			if (pilot.HasMember("hasBooking") && pilot["hasBooking"].IsBool())
				data.hasBooking = pilot["hasBooking"].GetBool();

			parsedData[data.callsign] = data;
		}

		if (PluginShutdownRequested.load(std::memory_order_relaxed))
			return;

		std::string aselCallsign;
		{
			std::lock_guard<std::mutex> stateGuard(VacdmDebugStateMutex);
			aselCallsign = VacdmDebugAselCallsign;
		}
		const size_t parsedPilotCount = parsedData.size();
		const bool aselFound = !aselCallsign.empty() && parsedData.find(aselCallsign) != parsedData.end();

		// Publishing only if the profile source is still current
		{
			std::lock_guard<std::mutex> sourceGuard(ProfilesSourceMutex);
			if (ProfilesSourceGeneration != sourceGeneration)
				return;
			std::lock_guard<std::mutex> pilotsGuard(VacdmPilotsMutex);
			VacdmPilots.swap(parsedData);
			VacdmSuccessfulSnapshotSourceGeneration = sourceGeneration;
			VacdmSuccessfulSnapshotAt = std::chrono::steady_clock::now();
		}

		const unsigned long fetchIndex = ++VacdmFetchCounter;
		Logger::info(
			"VACDM refresh #" + std::to_string(fetchIndex) +
			" pilots=" + std::to_string(parsedPilotCount) +
			" asel=" + (aselCallsign.empty() ? std::string("<none>") : aselCallsign) +
			" asel_present=" + std::string(aselFound ? "1" : "0") +
			" url=" + pilotsUrl
		);
	}
	catch (const std::exception& ex)
	{
		Logger::info("VACDM refresh exception: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("VACDM refresh exception: unknown");
	}
}

void refreshVacdmData()
{
#if defined(_MSC_VER)
	__try
	{
		refreshVacdmDataImpl();
	}
	__except (CaptureVacdmSehCode(static_cast<unsigned long>(GetExceptionCode())))
	{
		VacdmLastFetchTick = CurrentSteadyTick();
		VacdmFetchInProgress.store(false);
	}
#else
	refreshVacdmDataImpl();
#endif
}

void datalinkLogin(DatalinkLoginRequest request) {
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;
	struct ResetConnectingFlag
	{
		unsigned long long generation = 0;
		~ResetConnectingFlag()
		{
			if (generation == HoppieConnectionGeneration.load(std::memory_order_acquire))
				HoppieConnecting.store(false, std::memory_order_release);
		}
	} resetConnecting{ request.generation };

	bool connected = false;
	std::string failureMessage;
	try
	{
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=SERVER&type=PING";
		const string raw = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes);
		connected = IsHoppieOkResponse(raw);
		if (!connected)
			failureMessage = BuildHoppieLoginFailureMessage(raw, request.credentials.password);
	}
	catch (const std::exception& exception)
	{
		connected = false;
		failureMessage = "Connection failed before Hoppie replied.";
		std::string exceptionDetail = RedactSensitiveValue(
			exception.what(),
			request.credentials.password);
		exceptionDetail = RedactSensitiveValue(
			exceptionDetail,
			EncodeUrlQueryComponent(request.credentials.password));
		Logger::info("CPDLC login exception: " + exceptionDetail);
	}
	catch (...)
	{
		connected = false;
		failureMessage = "Connection failed before Hoppie replied.";
		Logger::info("CPDLC login exception: unknown");
	}

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
	{
		return;
	}

	if (connected)
	{
		SetDatalinkStatusMessage("Connected.");
		HoppieConnected.store(true, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		ConnectionMessage.store(true, std::memory_order_release);
	}
	else
	{
		if (failureMessage.empty())
			failureMessage = "Connection failed: Hoppie rejected the login.";
		ConnectionMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage(failureMessage);
		HoppieConnected.store(false, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		Logger::info(failureMessage);
		FailedToConnectMessage.store(true, std::memory_order_release);
	}
};

void pollMessages(DatalinkPollRequest request) {
	struct ResetPollFlag
	{
		unsigned long long pollGeneration = 0;
		~ResetPollFlag()
		{
			if (pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
		}
	} resetPoll{ request.pollGeneration };
	const auto completePoll = [&](bool succeeded)
	{
		if (request.pollGeneration ==
			HoppiePollGeneration.load(std::memory_order_acquire))
		{
			HoppiePollInProgress.store(false, std::memory_order_release);
		}
		if (!request.reportStatus ||
			PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			return;
		}
		SetDatalinkStatusMessage(succeeded ? "Poll complete." : "Poll failed.");
	};

	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		completePoll(false);
		return;
	}

	string raw;
	try
	{
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=SERVER&type=POLL";
		raw.assign(VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes));
	}
	catch (...)
	{
		completePoll(false);
		return;
	}

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
	{
		completePoll(false);
		return;
	}

	if (!VsmrRadarUiSupport::startsWith("ok", raw.c_str()))
	{
		completePoll(false);
		return;
	}
	if (raw.size() <= 3)
	{
		completePoll(true);
		return;
	}

	raw = raw + " ";
	raw = raw.substr(3, raw.size() - 3);

	string delimiter = "}} ";
	size_t pos = 0;
	std::string token;
	while ((pos = raw.find(delimiter)) != std::string::npos) {
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			completePoll(false);
			return;
		}

		token = raw.substr(1, pos);

		string parsed;
		stringstream input_stringstream(token);
		struct AcarsMessage message;
		int i = 1;
		while (getline(input_stringstream, parsed, ' '))
		{
			if (i == 1)
				message.from = parsed;
			if (i == 2)
				message.type = parsed;
			if (i > 2)
			{
				message.message.append(" ");
				message.message.append(parsed);
			}

			i++;
		}
		if (message.type.find("telex") != std::string::npos || message.type.find("cpdlc") != std::string::npos) {
			if (message.message.find("REQ") != std::string::npos || message.message.find("CLR") != std::string::npos || message.message.find("PDC") != std::string::npos || message.message.find("PREDEP") != std::string::npos || message.message.find("REQUEST") != std::string::npos) {
				if (message.message.find("LOGON") != std::string::npos) {
					QueueDatalinkMessage(
						request.plugin,
						message.from,
						"CPDLC",
						"UNABLE",
						"");
				} else {
					VsmrPluginRuntimeAudio::Play(L"Ding.wav", "CPDLC notification");
					std::lock_guard<std::mutex> guard(DatalinkStateMutex);
					AddCallsignUniqueUnlocked(AircraftDemandingClearance, message.from);
				}
			}
			else if (message.message.find("WILCO") != std::string::npos || message.message.find("ROGER") != std::string::npos || message.message.find("RGR") != std::string::npos) {
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				if (ContainsCallsignUnlocked(AircraftMessageSent, message.from)) {
					AddCallsignUniqueUnlocked(AircraftWilco, message.from);
				}
			}
			else if (message.message.length() != 0 ){
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				AddCallsignUniqueUnlocked(AircraftMessage, message.from);
			}
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				PendingMessages[message.from] = message;
			}
		}

		raw.erase(0, pos + delimiter.length());
	}

	completePoll(true);
};

bool StartDatalinkPoll(bool reportStatus, std::string& error)
	{
		error.clear();
		if (PluginShutdownRequested.load(std::memory_order_relaxed))
		{
			error = "The CPDLC service is shutting down.";
			return false;
		}
		if (!HoppieConnected.load(std::memory_order_acquire))
		{
			error = "CPDLC is not connected.";
			return false;
		}

		bool expected = false;
		if (!HoppiePollInProgress.compare_exchange_strong(
			expected,
			true,
			std::memory_order_acq_rel))
		{
			error = "A CPDLC poll is already in progress.";
			return false;
		}

		CSMRPlugin* plugin = ActivePluginInstance.load(std::memory_order_acquire);
		if (plugin == nullptr)
		{
			HoppiePollInProgress.store(false, std::memory_order_release);
			error = "The CPDLC service is unavailable.";
			return false;
		}
		DatalinkPollRequest request;
		request.plugin = plugin;
		request.credentials = SnapshotDatalinkCredentials();
		request.generation = HoppieConnectionGeneration.load(std::memory_order_acquire);
		request.pollGeneration =
			HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		request.reportStatus = reportStatus;
		if (!HoppieConnected.load(std::memory_order_acquire) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			if (request.pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
			error = "CPDLC disconnected before the poll could start.";
			return false;
		}

		if (reportStatus)
			SetDatalinkStatusMessage("Polling...");

		if (!plugin->QueueNetworkJob([request]() { pollMessages(request); }))
		{
			if (request.pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
			error = "Unable to queue the CPDLC poll request.";
			if (reportStatus)
				SetDatalinkStatusMessage(error);
			return false;
		}
		return true;
	}

void sendDatalinkClearance(DatalinkClearanceRequest request) {
	struct InFlightGuard
	{
		std::string callsign;
		~InFlightGuard()
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			ClearDatalinkClearanceInFlightUnlocked(callsign);
		}
	} inFlightGuard{ request.packet.callsign };
	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(
			std::memory_order_acquire))
		return;
	const DatalinkPacket& packet = request.packet;

	string payload = "/data2/";
	payload += std::to_string(request.messageSequence);
	payload += "//R/";
	payload += "CLR TO @";
	payload += packet.destination;
	payload += "@ RWY @";
	payload += packet.rwy;
	payload += "@ DEP @";
	payload += packet.sid;
	payload += "@ INIT CLB @";
	payload += packet.climb;
	payload += "@ SQUAWK @";
	payload += packet.squawk;
	payload += "@ ";
	if (packet.tsat != "no" && packet.tsat.size() > 3) {
		payload += "TSAT @";
		payload += packet.tsat;
		payload += "@ ";
	}
	if (packet.ctot != "no" && packet.ctot.size() > 3) {
		payload += "CTOT @";
		payload += packet.ctot;
		payload += "@ ";
	}
	if (packet.freq != "no" && packet.freq.size() > 5) {
		payload += "WHEN RDY CALL FREQ @";
		payload += packet.freq;
		payload += "@";
	}
	else {
		payload += "WHEN RDY CALL @";
		payload += request.fallbackFrequency;
		payload += "@";
	}
	payload += " IF UNABLE CALL VOICE ";
	if (packet.message != "no" && packet.message.size() > 1)
		payload += packet.message;

	string url = baseUrlDatalink;
	url += "?logon=";
	url += EncodeUrlQueryComponent(request.credentials.password);
	url += "&from=";
	url += EncodeUrlQueryComponent(request.credentials.callsign);
	url += "&to=";
	url += EncodeUrlQueryComponent(packet.callsign);
	url += "&type=CPDLC&packet=";
	url += EncodeUrlQueryComponent(payload);

	const string raw = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
		url,
		6000,
		&PluginShutdownRequested,
		HoppieResponseLimitBytes);
	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(
			std::memory_order_acquire))
		return;

	if (VsmrRadarUiSupport::startsWith("ok", raw.c_str())) {
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		RemoveCallsignUnlocked(AircraftDemandingClearance, packet.callsign);
		RemoveCallsignUnlocked(AircraftStandby, packet.callsign);
		PendingMessages.erase(packet.callsign);
		AddCallsignUniqueUnlocked(AircraftMessageSent, packet.callsign);
		MarkDatalinkClearanceSentUnlocked(packet.callsign);
	}
};

std::string CSMRPlugin::GetActiveProfilesConfigPath(
	bool* selectionClaimed)
{
	std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
	if (selectionClaimed != nullptr)
		*selectionClaimed = ActiveProfilesConfigPathClaimed;
	return ActiveProfilesConfigPath;
}

void CSMRPlugin::PublishActiveProfilesConfigPath(
	const std::string& path,
	bool claimSelection)
{
	std::string configuredVacdmServerUrl;
	const bool vacdmConfigured = TryReadVacdmServerUrl(
		std::filesystem::u8path(path),
		configuredVacdmServerUrl);
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		ActiveProfilesConfigPath = path;
		ActiveProfilesConfigPathClaimed = claimSelection;
		VacdmConfiguredServerUrl = vacdmConfigured
			? configuredVacdmServerUrl
			: std::string();
		++ProfilesSourceGeneration;
		VacdmSuccessfulSnapshotSourceGeneration = 0;
		VacdmSuccessfulSnapshotAt = std::chrono::steady_clock::time_point();
		// Publish the enable state before workers can observe the new source
		// generation. This prevents an old `true` value from starting a fetch
		// against the fallback URL after switching to a profile without VACDM.
		VacdmPollingEnabled.store(vacdmConfigured, std::memory_order_release);
		std::lock_guard<std::mutex> pilotsGuard(VacdmPilotsMutex);
		VacdmPilots.clear();
	}

	VacdmLastFetchTick.store(0, std::memory_order_relaxed);
	if (vacdmConfigured)
	{
		Logger::info(
			"VACDM polling enabled profiles=" + path +
			" server_url=" + configuredVacdmServerUrl);
	}
	else
	{
		Logger::info(
			"VACDM polling disabled profiles=" + path +
			" (no _vsmr.vacdm.server_url)");
	}
}
