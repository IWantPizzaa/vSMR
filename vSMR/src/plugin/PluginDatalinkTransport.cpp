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
		std::string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=SERVER&type=PING";
		const std::string raw = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
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

	std::string raw;
	try
	{
		std::string url = baseUrlDatalink;
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

	std::string delimiter = "}} ";
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

		std::string parsed;
		std::stringstream input_stringstream(token);
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

	std::string payload = "/data2/";
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

	std::string url = baseUrlDatalink;
	url += "?logon=";
	url += EncodeUrlQueryComponent(request.credentials.password);
	url += "&from=";
	url += EncodeUrlQueryComponent(request.credentials.callsign);
	url += "&to=";
	url += EncodeUrlQueryComponent(packet.callsign);
	url += "&type=CPDLC&packet=";
	url += EncodeUrlQueryComponent(payload);

	const std::string raw = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
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
	std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
	ActiveProfilesConfigPath = path;
	ActiveProfilesConfigPathClaimed = claimSelection;
}
