#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashReporter.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "integrations/CdmBridgeClient.hpp"
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


#include "plugin/PluginRuntimeAudio.hpp"

using VsmrDatalinkProtocol::RedactSensitiveValue;

HttpHelper& VsmrPluginRuntime::GetHttpHelper()
{
	static HttpHelper helper;
	return helper;
}

bool TryGetCdmPilotData(const std::string& callsign, CdmPilotData& outData)
{
	VsmrCdm::AircraftData bridgeData;
	if (!VsmrCdm::TryGetAircraftData(callsign, bridgeData))
		return false;

	auto toUtcTime = [](const std::optional<std::int64_t>& minutes)
	{
		if (!minutes.has_value() || *minutes < 0 || *minutes >= 24 * 60)
			return static_cast<std::time_t>(0);
		const std::time_t now = std::time(nullptr);
		std::tm utc = {};
		if (::gmtime_s(&utc, &now) != 0)
			return static_cast<std::time_t>(0);
		utc.tm_hour = static_cast<int>(*minutes / 60);
		utc.tm_min = static_cast<int>(*minutes % 60);
		utc.tm_sec = 0;
		std::time_t candidate = _mkgmtime(&utc);
		if (candidate - now > 12 * 60 * 60)
			candidate -= 24 * 60 * 60;
		else if (now - candidate > 12 * 60 * 60)
			candidate += 24 * 60 * 60;
		return candidate;
	};

	outData = CdmPilotData();
	outData.callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	outData.bridgeData = bridgeData;
	outData.tobtUtc = toUtcTime(bridgeData.tobt);
	outData.tsatUtc = toUtcTime(bridgeData.tsat);
	outData.ttotUtc = toUtcTime(bridgeData.ttot);
	outData.ctotUtc = toUtcTime(bridgeData.ctot);
	outData.tsacUtc = toUtcTime(bridgeData.tsac);
	outData.asrtUtc = toUtcTime(bridgeData.asrt);
	outData.asatUtc = toUtcTime(bridgeData.asat);
	outData.hasTobt = outData.tobtUtc != 0;
	outData.hasTsat = outData.tsatUtc != 0;
	outData.hasTtot = outData.ttotUtc != 0;
	outData.hasCtot = outData.ctotUtc != 0;
	outData.hasTsac = outData.tsacUtc != 0;
	outData.hasAsrt = outData.asrtUtc != 0;
	outData.hasAsat = outData.asatUtc != 0;
	// The CDM provider only publishes a TOBT after CDM has accepted it.
	outData.tobtState = outData.hasTobt ? "CONFIRMED" : "";
	return true;
}

namespace
{
	// A poll is cheap to repeat. An action is not: PINEDE waits for Hoppie (up
	// to 5 s) before answering, and giving up first would report a clearance
	// that was in fact sent, inviting the controller to send it twice.
	constexpr int kPollTimeoutMs = 6000;
	constexpr int kActionTimeoutMs = 15000;

	HttpResponse PostToPinede(
		const DatalinkCredentialsSnapshot& credentials,
		const char* path,
		const std::string& body,
		int timeoutMs = kActionTimeoutMs)
	{
		try
		{
			return VsmrPluginRuntime::GetHttpHelper().sendJsonRequest(
				VsmrPinede::JoinUrl(credentials.baseUrl, path),
				body,
				credentials.key,
				timeoutMs,
				&PluginShutdownRequested,
				DatalinkResponseLimitBytes);
		}
		catch (const std::exception& exception)
		{
			Logger::info(std::string("PDC request to PINEDE failed: ") +
				RedactSensitiveValue(exception.what(), credentials.key));
		}
		catch (...)
		{
			Logger::info("PDC request to PINEDE failed: unknown exception");
		}
		return HttpResponse{};
	}

	bool Succeeded(const HttpResponse& response)
	{
		return response.completed && response.status >= 200 && response.status < 300;
	}

	std::string Describe(const HttpResponse& response)
	{
		return VsmrPinede::DescribeFailure(response.completed, response.status, response.body);
	}

	bool IsCurrentSession(unsigned long long generation)
	{
		return !PluginShutdownRequested.load(std::memory_order_relaxed) &&
			generation == DatalinkConnectionGeneration.load(std::memory_order_acquire);
	}

	// Replaces the tag lists with what PINEDE says this station should show.
	// A request that newly asks for a clearance rings, as Hoppie requests did.
	void ApplyPollResult(const VsmrPinede::PollResult& result, unsigned long long stateRevision)
	{
		bool newRequest = false;
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			// an action of ours finished during this poll: its effect is newer than this answer
			if (stateRevision != DatalinkStateRevision.load(std::memory_order_acquire))
				return;

			VsmrPinede::TagLists lists = VsmrPinede::BuildTagLists(result, AircraftWilcoDismissed);
			for (const std::string& callsign : lists.demandingClearance)
			{
				if (!ContainsCallsignUnlocked(AircraftDemandingClearance, callsign))
					newRequest = true;
			}

			AircraftDemandingClearance = std::move(lists.demandingClearance);
			AircraftStandby = std::move(lists.standby);
			AircraftMessage = std::move(lists.message);
			AircraftWilco = std::move(lists.wilco);
			AircraftMessageSent = std::move(lists.messageSent);
			AircraftDatalinkClearedCallsigns = std::move(lists.cleared);
			for (auto it = AircraftWilcoDismissed.begin(); it != AircraftWilcoDismissed.end();)
				it = AircraftDatalinkClearedCallsigns.count(*it) ? std::next(it) : AircraftWilcoDismissed.erase(it);

			PendingMessages.clear();
			for (const auto& entry : lists.displayedText)
				PendingMessages[entry.first] = AcarsMessage{ entry.first, "dcl", entry.second };
		}
		if (newRequest)
			VsmrPluginRuntimeAudio::Play(L"Ding.wav", "PDC notification");
	}

	void MarkStateChanged()
	{
		DatalinkStateRevision.fetch_add(1, std::memory_order_acq_rel);
	}
}

void ClearDatalinkTagState()
{
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		MarkStateChanged();
		AircraftDemandingClearance.clear();
		AircraftStandby.clear();
		AircraftMessage.clear();
		AircraftWilco.clear();
		AircraftMessageSent.clear();
		PendingMessages.clear();
	}
	ResetDatalinkClearanceState();
}

bool IsDatalinkTrackedUnlocked(const std::string& callsign)
{
	const std::string normalized = NormalizeCallsignForState(callsign);
	return ContainsCallsignUnlocked(AircraftDemandingClearance, normalized) ||
		ContainsCallsignUnlocked(AircraftStandby, normalized) ||
		ContainsCallsignUnlocked(AircraftMessage, normalized) ||
		ContainsCallsignUnlocked(AircraftWilco, normalized) ||
		ContainsCallsignUnlocked(AircraftMessageSent, normalized) ||
		PendingMessages.count(normalized) != 0;
}

void datalinkLogin(DatalinkLoginRequest request) {
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;
	struct ResetConnectingFlag
	{
		unsigned long long generation = 0;
		~ResetConnectingFlag()
		{
			if (generation == DatalinkConnectionGeneration.load(std::memory_order_acquire))
				DatalinkConnecting.store(false, std::memory_order_release);
		}
	} resetConnecting{ request.generation };
	// A disconnect can invalidate a login while it is still waiting in the
	// network queue. Do not announce that stale session to PINEDE.
	if (!IsCurrentSession(request.generation))
		return;

	// The first poll is the login: it proves the key and puts the station online
	const unsigned long long stateRevision = DatalinkStateRevision.load(std::memory_order_acquire);
	const HttpResponse response = PostToPinede(
		request.credentials,
		"/v1/dcl/poll",
		VsmrPinede::BuildPollBody(request.credentials.callsign, request.credentials.frequency),
		kPollTimeoutMs);
	VsmrPinede::PollResult result;
	const bool connected = Succeeded(response) && VsmrPinede::ParsePollResponse(response.body, result);

	if (!IsCurrentSession(request.generation))
		return;

	if (connected)
	{
		ApplyPollResult(result, stateRevision);
		SetDatalinkStatusMessage("Connected as " + request.credentials.callsign + ".");
		DatalinkConnected.store(true, std::memory_order_release);
		DatalinkConnecting.store(false, std::memory_order_release);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		ConnectionMessage.store(true, std::memory_order_release);
	}
	else
	{
		const std::string failureMessage = Succeeded(response)
			? "PINEDE sent an answer vSMR cannot read."
			: Describe(response);
		ConnectionMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage(failureMessage);
		DatalinkConnected.store(false, std::memory_order_release);
		DatalinkConnecting.store(false, std::memory_order_release);
		Logger::info("PDC login failed: " + failureMessage);
		FailedToConnectMessage.store(true, std::memory_order_release);
	}
};

// Tells PINEDE the station is gone, so its requests go to the next controller
// at once instead of after PINEDE's 30 second timeout. Best effort.
void datalinkLogoff(DatalinkCredentialsSnapshot credentials) {
	if (credentials.callsign.empty() || credentials.key.empty())
		return;
	const HttpResponse response = PostToPinede(
		credentials,
		"/v1/dcl/logoff",
		VsmrPinede::BuildStationBody(credentials.callsign));
	if (!Succeeded(response))
		Logger::info("PDC logoff not confirmed: " + Describe(response));
};

void pollMessages(DatalinkPollRequest request) {
	struct ResetPollFlag
	{
		unsigned long long pollGeneration = 0;
		~ResetPollFlag()
		{
			if (pollGeneration == DatalinkPollGeneration.load(std::memory_order_acquire))
				DatalinkPollInProgress.store(false, std::memory_order_release);
		}
	} resetPoll{ request.pollGeneration };
	const auto completePoll = [&](bool succeeded)
	{
		if (request.pollGeneration ==
			DatalinkPollGeneration.load(std::memory_order_acquire))
		{
			DatalinkPollInProgress.store(false, std::memory_order_release);
		}
		if (!request.reportStatus || !IsCurrentSession(request.generation))
			return;
		SetDatalinkStatusMessage(succeeded ? "Poll complete." : "Poll failed.");
	};

	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		completePoll(false);
		return;
	}
	// A poll keeps the station online at PINEDE. A queued poll must never start
	// after the controller has disconnected this session.
	if (!IsCurrentSession(request.generation) ||
		request.pollGeneration != DatalinkPollGeneration.load(std::memory_order_acquire))
	{
		completePoll(false);
		return;
	}

	const HttpResponse response = PostToPinede(
		request.credentials,
		"/v1/dcl/poll",
		VsmrPinede::BuildPollBody(request.credentials.callsign, request.credentials.frequency),
		kPollTimeoutMs);

	if (!IsCurrentSession(request.generation))
	{
		completePoll(false);
		return;
	}

	if (response.completed && response.status == 401)
	{
		// the key was withdrawn: polling again would only be refused again
		DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		DatalinkConnected.store(false, std::memory_order_release);
		ClearDatalinkTagState();
		const std::string message = Describe(response);
		SetDatalinkStatusMessage(message);
		QueueDatalinkNotice(message);
		completePoll(false);
		return;
	}

	VsmrPinede::PollResult result;
	if (!Succeeded(response) || !VsmrPinede::ParsePollResponse(response.body, result))
	{
		completePoll(false);
		return;
	}

	ApplyPollResult(result, request.stateRevision);
	completePoll(true);
};

bool StartDatalinkPoll(bool reportStatus, std::string& error)
	{
		error.clear();
		if (PluginShutdownRequested.load(std::memory_order_relaxed))
		{
		error = "The PDC service is shutting down.";
			return false;
		}
		if (!DatalinkConnected.load(std::memory_order_acquire))
		{
		error = "PDC is not connected.";
			return false;
		}

		bool expected = false;
		if (!DatalinkPollInProgress.compare_exchange_strong(
			expected,
			true,
			std::memory_order_acq_rel))
		{
		error = "A PDC poll is already in progress.";
			return false;
		}

		CSMRPlugin* plugin = ActivePluginInstance.load(std::memory_order_acquire);
		if (plugin == nullptr)
		{
			DatalinkPollInProgress.store(false, std::memory_order_release);
		error = "The PDC service is unavailable.";
			return false;
		}
		DatalinkPollRequest request;
		request.plugin = plugin;
		request.credentials = SnapshotDatalinkCredentials();
		request.generation = DatalinkConnectionGeneration.load(std::memory_order_acquire);
		request.pollGeneration =
			DatalinkPollGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		request.stateRevision = DatalinkStateRevision.load(std::memory_order_acquire);
		request.reportStatus = reportStatus;
		if (!DatalinkConnected.load(std::memory_order_acquire) ||
			request.generation != DatalinkConnectionGeneration.load(std::memory_order_acquire))
		{
			if (request.pollGeneration == DatalinkPollGeneration.load(std::memory_order_acquire))
				DatalinkPollInProgress.store(false, std::memory_order_release);
		error = "PDC disconnected before the poll could start.";
			return false;
		}

		if (reportStatus)
			SetDatalinkStatusMessage("Polling...");

		if (!plugin->QueueNetworkJob([request]() { pollMessages(request); }))
		{
			if (request.pollGeneration == DatalinkPollGeneration.load(std::memory_order_acquire))
				DatalinkPollInProgress.store(false, std::memory_order_release);
		error = "Unable to queue the PDC poll request.";
			if (reportStatus)
				SetDatalinkStatusMessage(error);
			return false;
		}
		return true;
	}

namespace
{
	void sendDatalinkAction(DatalinkActionRequest request)
	{
		if (!IsCurrentSession(request.generation))
			return;

		const char* path = "/v1/dcl/standby";
		std::string body = VsmrPinede::BuildAircraftBody(request.credentials.callsign, request.callsign);
		switch (request.action)
		{
		case DatalinkAction::Standby: path = "/v1/dcl/standby"; break;
		case DatalinkAction::Voice: path = "/v1/dcl/voice"; break;
		case DatalinkAction::Reset: path = "/v1/dcl/reset"; break;
		case DatalinkAction::Message:
			path = "/v1/dcl/message";
			body = VsmrPinede::BuildMessageBody(request.credentials.callsign, request.callsign, request.text);
			break;
		}

		const HttpResponse response = PostToPinede(request.credentials, path, body);
		if (!IsCurrentSession(request.generation))
			return;
		if (!Succeeded(response))
		{
			QueueDatalinkNotice(request.callsign + ": " + Describe(response));
			return;
		}

		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		MarkStateChanged();
		switch (request.action)
		{
		case DatalinkAction::Standby:
			AddCallsignUniqueUnlocked(AircraftStandby, request.callsign);
			break;
		case DatalinkAction::Voice:
		case DatalinkAction::Reset:
			RemoveCallsignUnlocked(AircraftDemandingClearance, request.callsign);
			RemoveCallsignUnlocked(AircraftStandby, request.callsign);
			RemoveCallsignUnlocked(AircraftMessage, request.callsign);
			RemoveCallsignUnlocked(AircraftWilco, request.callsign);
			RemoveCallsignUnlocked(AircraftMessageSent, request.callsign);
			ClearDatalinkClearanceSentUnlocked(request.callsign);
			PendingMessages.erase(request.callsign);
			break;
		case DatalinkAction::Message:
			RemoveCallsignUnlocked(AircraftMessage, request.callsign);
			break;
		}
	}
}

bool QueueDatalinkAction(
	CSMRPlugin* plugin,
	DatalinkAction action,
	const std::string& callsign,
	const std::string& text)
{
	if (plugin == nullptr ||
		PluginShutdownRequested.load(std::memory_order_acquire) ||
		!DatalinkConnected.load(std::memory_order_acquire))
	{
		return false;
	}
	DatalinkActionRequest request;
	request.credentials = SnapshotDatalinkCredentials();
	request.generation = DatalinkConnectionGeneration.load(std::memory_order_acquire);
	request.action = action;
	request.callsign = NormalizeCallsignForState(callsign);
	request.text = text;
	return plugin->QueueNetworkJob([request]() { sendDatalinkAction(request); });
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
	} inFlightGuard{ request.fields.callsign };
	if (!IsCurrentSession(request.generation))
		return;

	// PINEDE builds the clearance text from these fields
	const HttpResponse response = PostToPinede(
		request.credentials,
		"/v1/dcl/clearance",
		VsmrPinede::BuildClearanceBody(request.credentials.callsign, request.fields));
	if (!IsCurrentSession(request.generation))
		return;
	if (!Succeeded(response))
	{
		QueueDatalinkNotice(request.fields.callsign + ": " + Describe(response));
		return;
	}

	std::lock_guard<std::mutex> guard(DatalinkStateMutex);
	MarkStateChanged();
	RemoveCallsignUnlocked(AircraftDemandingClearance, request.fields.callsign);
	RemoveCallsignUnlocked(AircraftStandby, request.fields.callsign);
	RemoveCallsignUnlocked(AircraftMessage, request.fields.callsign);
	PendingMessages.erase(request.fields.callsign);
	AddCallsignUniqueUnlocked(AircraftMessageSent, request.fields.callsign);
	MarkDatalinkClearanceSentUnlocked(request.fields.callsign);
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
	std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
	ActiveProfilesConfigPath = path;
	ActiveProfilesConfigPathClaimed = claimSelection;
}
