#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashReporter.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "datalink/PinedeProtocol.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/DmanBridgeClient.hpp"
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

using VsmrDatalinkProtocol::FormatPdcFrequency;
using VsmrDatalinkProtocol::ProtectDatalinkSecret;
using VsmrDatalinkProtocol::UnprotectDatalinkSecret;

namespace
{
	// EuroScope settings. The PINEDE key is stored protected by Windows DPAPI.
	constexpr const char* kKeySetting = "pdc_key";
	constexpr const char* kKeySettingDescription = "The protected PINEDE PDC key";
	// Optional override of PINEDE's address, for a test server
	constexpr const char* kServerSetting = "pdc_server_url";
	// Hoppie settings of earlier versions, erased on first load
	constexpr const char* kLegacyHoppieSettings[] = { "cpdlc_logon", "cpdlc_password" };
	constexpr const char* kHandledByDmanMessage = "PDC handled by DMAN.";
}

// DMAN publishes the station whose PDC it drives through PINEDE. While that is
// this station, vSMR leaves PINEDE to it; once DMAN stops, the next timer
// cycle polls again, which is all PINEDE needs to route requests back here.
void CSMRPlugin::RefreshDatalinkDmanYield()
{
	const std::string station = SnapshotDatalinkCredentials().callsign;
	const bool yielded = GetConnectionType() != CONNECTION_TYPE_VIA_PROXY &&
		VsmrPinede::YieldsToDman(VsmrDman::GetPdcStation(), station);
	if (yielded == DatalinkYieldedToDman.exchange(yielded, std::memory_order_acq_rel))
		return;
	// the runtime menu and the tags show the change
	RefreshControllerDependentOverlays();
	// the controller is told only when vSMR's own PDC session changes
	const bool online = DatalinkConnected.load(std::memory_order_acquire) ||
		DatalinkConnecting.load(std::memory_order_acquire);
	if (yielded)
	{
		// what PINEDE gave this station is now DMAN's to act on
		ClearDatalinkTagState();
		Logger::info("PDC handled by DMAN for " + station + ": vSMR stops polling PINEDE");
		if (online)
			DisplayUserMessage("PDC", "Server", ("PDC handled by DMAN for " + station + ".").c_str(),
				true, true, false, true, false);
		return;
	}
	Logger::info("PDC no longer handled by DMAN for " + station);
	if (DatalinkConnected.load(std::memory_order_acquire))
	{
		// poll on this cycle rather than up to 10 seconds later
		DatalinkLastPollAt = PluginSteadyClock::now() - std::chrono::seconds(11);
		SetDatalinkStatusMessage("Connected as " + station + ".");
		DisplayUserMessage("PDC", "Server", "PDC no longer handled by DMAN: vSMR polls PINEDE again.",
			true, true, false, true, false);
	}
}

void CSMRPlugin::SynchronizeProxyDatalink()
{
	const auto previous = ProxyDatalink.flags;
	const bool previousOnline = ProxyDatalink.connected, previousConnecting = ProxyDatalink.connecting, previousRemote = ProxyDatalink.remote;
	if (GetConnectionType() == CONNECTION_TYPE_DIRECT) {
		std::map<std::string, unsigned> flags;
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			const auto add = [&](const std::vector<std::string>& list, unsigned bit) { for (const auto& call : list) flags[call] |= bit; };
			add(AircraftDemandingClearance, 1); add(AircraftStandby, 2); add(AircraftMessage, 4);
			add(AircraftWilco, 8); add(AircraftMessageSent, 16);
		}
		std::string login;
		{ std::lock_guard<std::mutex> guard(DatalinkControlMutex); login = logonCallsign; }
		ProxyDatalink.Publish(DatalinkConnected.load(), DatalinkConnecting.load(), DatalinkPollInProgress.load(), login, flags);
	} else ProxyDatalink.Poll();
	if (previous != ProxyDatalink.flags || previousOnline != ProxyDatalink.connected ||
		previousConnecting != ProxyDatalink.connecting || previousRemote != ProxyDatalink.remote)
		RefreshControllerDependentOverlays();
}

DatalinkControlState CSMRPlugin::GetDatalinkControlState() const
{
	DatalinkControlState state;
	state.connected = DatalinkConnected.load(std::memory_order_acquire);
	state.connecting = DatalinkConnecting.load(std::memory_order_acquire);
	state.pollInProgress = DatalinkPollInProgress.load(std::memory_order_acquire);
	state.controllerConnected = ControllerMyself().IsController();
	state.yieldedToDman = DatalinkYieldedToDman.load(std::memory_order_acquire);
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		state.logonCallsign = logonCallsign;
		state.hasKey = !TrimAsciiWhitespaceCopy(logonCode).empty();
		state.serverUrl = DatalinkBaseUrl;
		state.statusMessage = state.yieldedToDman ? kHandledByDmanMessage : DatalinkStatusMessage;
	}
	state.remoteSession = GetConnectionType() == CONNECTION_TYPE_VIA_PROXY;
	if (state.remoteSession) {
		state.connected = ProxyDatalink.connected; state.connecting = ProxyDatalink.connecting;
		state.pollInProgress = ProxyDatalink.polling; state.hasKey = false; state.yieldedToDman = false;
		if (ProxyDatalink.remote) state.logonCallsign = ProxyDatalink.logon;
		state.statusMessage = "PDC is managed in the primary EuroScope instance.";
	}
	const VsmrCdm::InterfaceState cdmState = VsmrCdm::GetInterfaceState();
	state.cdmBridgeLoaded = cdmState.bridgeLoaded;
	state.cdmBridgeReady = cdmState.providerReady;
	state.activeAirport = ResolveActiveAirportFilterUpper();

	state.aliasPath = ResolveAliasFilePath(const_cast<CSMRPlugin*>(this)).u8string();
	return state;
}

// PINEDE knows a station by its EuroScope callsign. Read on the main thread,
// for the workers to send with the next request.
void CSMRPlugin::RefreshDatalinkStation()
{
	const CController myself = ControllerMyself();
	// kept while EuroScope is offline, so the logoff still names the station
	if (!myself.IsValid() || !myself.IsController())
		return;
	const char* raw = myself.GetCallsign();
	const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(raw != nullptr ? raw : ""));
	if (callsign.empty())
		return;
	const std::string frequency = FormatPdcFrequency(myself.GetPrimaryFrequency());
	std::lock_guard<std::mutex> guard(DatalinkControlMutex);
	logonCallsign = callsign;
	DatalinkStationFrequency = frequency;
}

bool CSMRPlugin::UpdateDatalinkKey(const std::string& key, std::string& error)
{
	error.clear();
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) {
		error = "Edit the PDC key in the primary EuroScope instance."; return false;
	}
	const std::string normalizedKey = TrimAsciiWhitespaceCopy(key);
	if (normalizedKey.empty())
	{
		error = "Enter a PDC key before replacing the saved key.";
		return false;
	}
	std::string protectedKey;
	if (!ProtectDatalinkSecret(normalizedKey, protectedKey))
	{
		error = "Windows could not protect the PDC key. Settings were not changed.";
		Logger::info("PDC key update rejected because DPAPI protection failed");
		return false;
	}

	bool keyChanged = false;
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		keyChanged = logonCode != normalizedKey;
		logonCode = normalizedKey;
	}
	if (keyChanged &&
		(DatalinkConnected.load(std::memory_order_acquire) ||
			DatalinkConnecting.load(std::memory_order_acquire)))
	{
		DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		DatalinkPollGeneration.fetch_add(1, std::memory_order_acq_rel);
		DatalinkConnected.store(false, std::memory_order_release);
		DatalinkConnecting.store(false, std::memory_order_release);
		DatalinkPollInProgress.store(false, std::memory_order_release);
		ConnectionMessage.store(false, std::memory_order_relaxed);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage("Key changed. Reconnect PDC to apply it.");
	}
	else if (keyChanged)
	{
		SetDatalinkStatusMessage("Key updated. Ready to connect.");
	}
	SaveDataToSettings(kKeySetting, kKeySettingDescription, protectedKey.c_str());
	return true;
}

bool CSMRPlugin::ConnectDatalink(std::string& error)
{
	error.clear();
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) {
		error = "Connect PDC in the primary EuroScope instance."; return false;
	}
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		error = "The PDC service is shutting down.";
		return false;
	}
	if (!ControllerMyself().IsController())
	{
		error = "You are not logged in as a controller.";
		return false;
	}
	if (DatalinkConnected.load(std::memory_order_acquire))
	{
		error = "PDC is already connected.";
		return false;
	}
	// the login is a poll, and PINEDE takes one poller per station
	if (DatalinkYieldedToDman.load(std::memory_order_acquire))
	{
		error = "DMAN handles the PDC of this station.";
		return false;
	}

	RefreshDatalinkStation();
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	if (credentials.callsign.empty())
	{
		error = "EuroScope has not given vSMR a controller callsign yet.";
		return false;
	}
	if (TrimAsciiWhitespaceCopy(credentials.key).empty())
	{
		error = "A PDC key is required. Set it in the runtime menu.";
		return false;
	}
	if (!HttpHelper::IsAllowedApiUrl(VsmrPinede::JoinUrl(credentials.baseUrl, "/v1/dcl/poll")))
	{
		error = "The PDC server address is not a valid https address.";
		return false;
	}

	bool expected = false;
	if (!DatalinkConnecting.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel))
	{
		error = "A PDC connection attempt is already in progress.";
		return false;
	}
	if (DatalinkConnected.load(std::memory_order_acquire))
	{
		DatalinkConnecting.store(false, std::memory_order_release);
		error = "PDC is already connected.";
		return false;
	}

	const unsigned long long generation =
		DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Connecting...");

	DatalinkLoginRequest request;
	request.credentials = credentials;
	request.generation = generation;
	if (!QueueNetworkJob([request]() { datalinkLogin(request); }))
	{
		DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		DatalinkConnecting.store(false, std::memory_order_release);
		error = "Unable to queue the PDC connection request.";
		SetDatalinkStatusMessage(error);
		return false;
	}
	return true;
}

bool CSMRPlugin::DisconnectDatalink(std::string& error)
{
	error.clear();
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) { error = "Disconnect PDC in the primary EuroScope instance."; return false; }
	const bool wasOnline = DatalinkConnected.load(std::memory_order_acquire) ||
		DatalinkConnecting.load(std::memory_order_acquire);
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkPollGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkConnected.store(false, std::memory_order_release);
	DatalinkConnecting.store(false, std::memory_order_release);
	DatalinkPollInProgress.store(false, std::memory_order_release);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Disconnected.");
	// a logoff names the station: while DMAN polls it, it would disconnect DMAN
	if (wasOnline && DatalinkYieldedToDman.load(std::memory_order_acquire))
		Logger::info("PDC logoff not sent: DMAN handles the station");
	else if (wasOnline && !QueueNetworkJob([credentials]() { datalinkLogoff(credentials); }))
		Logger::info("PDC logoff could not be queued; PINEDE will time the station out");
	// what this station showed is no longer its to act on
	ClearDatalinkTagState();
	return true;
}

bool CSMRPlugin::PollDatalink(std::string& error)
{
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) { error = "Poll PDC in the primary EuroScope instance."; return false; }
	RefreshDatalinkStation();
	return StartDatalinkPoll(true, error);
}

bool CSMRPlugin::EditDatalinkCredentials(std::string& error)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	error.clear();
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) { error = "Edit the PDC key in the primary EuroScope instance."; return false; }
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	auto applyValues = [&](const CCPDLCSettingsDialog& dialog) -> bool
	{
		return UpdateDatalinkKey(static_cast<const char*>(CStringA(dialog.m_Password)), error);
	};

	CCPDLCSettingsDialog dialog(AfxGetMainWnd());
	dialog.m_Password = credentials.key.c_str();
	INT_PTR dialogResult = dialog.DoModal();
	if (dialogResult == IDOK)
		return applyValues(dialog);

	if (dialogResult == -1)
	{
		CCPDLCSettingsDialog fallbackDialog(nullptr);
		fallbackDialog.m_Password = credentials.key.c_str();
		dialogResult = fallbackDialog.DoModal();
		if (dialogResult == IDOK)
			return applyValues(fallbackDialog);
	}

	if (dialogResult == -1)
	{
		const DWORD lastError = ::GetLastError();
		const HRSRC dialogResource = ::FindResource(
			AfxGetResourceHandle(),
			MAKEINTRESOURCE(CCPDLCSettingsDialog::IDD),
			RT_DIALOG);
		error = "Failed to open the PDC key window (GetLastError=" +
			std::to_string(static_cast<unsigned long>(lastError)) +
			", resource=" + std::string(dialogResource != nullptr ? "ok" : "missing") + ").";
		return false;
	}

	// Cancel is a successful no-op.
	return true;
}

void CSMRPlugin::ResetDatalinkRuntime()
{
	DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkPollGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkConnected.store(false, std::memory_order_relaxed);
	DatalinkConnecting.store(false, std::memory_order_relaxed);
	DatalinkPollInProgress.store(false, std::memory_order_relaxed);
	DatalinkYieldedToDman.store(false, std::memory_order_relaxed);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		logonCallsign.clear();
		logonCode.clear();
		DatalinkBaseUrl = VsmrPinede::kDefaultBaseUrl;
		DatalinkStationFrequency.clear();
		DatalinkStatusMessage = "Disconnected.";
	}
	ResetDatalinkClearanceState();
}

void CSMRPlugin::LoadDatalinkSettings()
{
	DatalinkLastPollAt = PluginSteadyClock::now();
	const char * p_value;
	bool migratePlaintextKey = false;

	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		if ((p_value = GetDataFromSettings(kKeySetting)) != NULL)
		{
			bool wasPlaintext = false;
			std::string unprotectedKey;
			if (UnprotectDatalinkSecret(p_value, unprotectedKey, wasPlaintext))
			{
				logonCode = std::move(unprotectedKey);
				migratePlaintextKey = wasPlaintext && !logonCode.empty();
			}
			else
			{
				logonCode.clear();
				DatalinkStatusMessage = "The saved PDC key could not be unlocked. Enter it again.";
				Logger::info("PDC saved key could not be decrypted");
			}
		}

		DatalinkBaseUrl = VsmrPinede::kDefaultBaseUrl;
		if ((p_value = GetDataFromSettings(kServerSetting)) != NULL)
		{
			const std::string configured = TrimAsciiWhitespaceCopy(p_value);
			if (!configured.empty() &&
				HttpHelper::IsAllowedApiUrl(VsmrPinede::JoinUrl(configured, "/v1/dcl/poll")))
				DatalinkBaseUrl = configured;
			else if (!configured.empty())
				Logger::info("PDC server address setting ignored: not https, nor http to this machine");
		}
	}

	// A key typed straight into the settings file is protected on first load
	if (migratePlaintextKey)
	{
		const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
		std::string protectedKey;
		if (ProtectDatalinkSecret(credentials.key, protectedKey))
			SaveDataToSettings(kKeySetting, kKeySettingDescription, protectedKey.c_str());
		else
			SaveDataToSettings(kKeySetting, kKeySettingDescription, "");
	}

	// PDC no longer uses Hoppie: do not leave its code behind in the settings
	for (const char* legacy : kLegacyHoppieSettings)
	{
		p_value = GetDataFromSettings(legacy);
		if (p_value != NULL && p_value[0] != '\0')
		{
			SaveDataToSettings(legacy, "No longer used by vSMR", "");
			Logger::info(std::string("Erased the Hoppie setting ") + legacy);
		}
	}
}

void CSMRPlugin::ResetDatalinkProfileSource()
{
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		ActiveProfilesConfigPath.clear();
		ActiveProfilesConfigPathClaimed = false;
	}
	PublishActiveProfilesConfigPath(
		ResolveDefaultProfilesConfigPath().u8string(),
		false);
}

void CSMRPlugin::PrepareDatalinkRuntimeForExit()
{
	DatalinkConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkPollGeneration.fetch_add(1, std::memory_order_acq_rel);
	DatalinkConnected.store(false, std::memory_order_relaxed);
	DatalinkConnecting.store(false, std::memory_order_relaxed);
	DatalinkPollInProgress.store(false, std::memory_order_relaxed);
}

void CSMRPlugin::PersistDatalinkSettings()
{
	if (DatalinkProxyClient) return; // a closing secondary must not persist stale primary credentials
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	std::string protectedKey;
	if (ProtectDatalinkSecret(credentials.key, protectedKey))
		SaveDataToSettings(kKeySetting, kKeySettingDescription, protectedKey.c_str());
	else
		Logger::info("PDC key was not persisted because DPAPI protection failed");
}
