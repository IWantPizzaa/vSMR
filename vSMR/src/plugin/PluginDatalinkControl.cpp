#include "platform/windows/PrecompiledHeader.hpp"
#include "shared/Random.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

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
		ProxyDatalink.Publish(HoppieConnected.load(), HoppieConnecting.load(), HoppiePollInProgress.load(), login, flags);
	} else ProxyDatalink.Poll();
	if (previous != ProxyDatalink.flags || previousOnline != ProxyDatalink.connected ||
		previousConnecting != ProxyDatalink.connecting || previousRemote != ProxyDatalink.remote)
		RefreshControllerDependentOverlays();
}

DatalinkControlState CSMRPlugin::GetDatalinkControlState() const
{
	DatalinkControlState state;
	state.connected = HoppieConnected.load(std::memory_order_acquire);
	state.connecting = HoppieConnecting.load(std::memory_order_acquire);
	state.pollInProgress = HoppiePollInProgress.load(std::memory_order_acquire);
	state.controllerConnected = ControllerMyself().IsController();
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		state.logonCallsign = logonCallsign;
		state.hasPassword = !TrimAsciiWhitespaceCopy(logonCode).empty();
		state.statusMessage = DatalinkStatusMessage;
	}
	state.remoteSession = GetConnectionType() == CONNECTION_TYPE_VIA_PROXY;
	if (state.remoteSession) {
		state.connected = ProxyDatalink.connected; state.connecting = ProxyDatalink.connecting;
		state.pollInProgress = ProxyDatalink.polling; state.hasPassword = false;
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

void CSMRPlugin::SetPdcSelectedAirport(const std::string& airport)
{
	const std::string normalized = PdcLogonSync::NormalizeAirport(airport);
	if (normalized.empty())
		return;
	PdcSelectedAirport = normalized;
	SyncPdcLogonWithSelectedAirport();
}

void CSMRPlugin::SyncPdcLogonWithSelectedAirport()
{
	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		HoppieConnected.load(std::memory_order_acquire) ||
		HoppieConnecting.load(std::memory_order_acquire))
		return;
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY)
		return;
	const std::string airport = PdcLogonSynchronization.Resolve(PdcSelectedAirport);
	if (airport.empty())
		return;
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		if (logonCallsign == airport)
			return;
	}
	std::string error;
	if (UpdateDatalinkControlSettings(airport, "", false, error, true, true))
		RefreshControllerDependentOverlays();
}

bool CSMRPlugin::UpdateDatalinkControlSettings(
	const std::string& callsign,
	const std::string& password,
	bool replacePassword,
	std::string& error,
	bool updateConnectionSettings,
	bool automaticLogonUpdate)
{
	error.clear();
	if (updateConnectionSettings && GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) {
		error = "Edit PDC connection settings in the primary EuroScope instance."; return false;
	}
	const std::string normalizedCallsign =
		ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	const std::string normalizedPassword =
		replacePassword ? TrimAsciiWhitespaceCopy(password) : std::string();
	if (updateConnectionSettings && normalizedCallsign.empty())
	{
		error = "The PDC logon callsign is required.";
		return false;
	}
	if (updateConnectionSettings && replacePassword && normalizedPassword.empty())
	{
		error = "Enter a Hoppie code before replacing the saved code.";
		return false;
	}
	std::string protectedPasswordToPersist;
	if (updateConnectionSettings)
	{
		std::string effectivePassword;
		{
			std::lock_guard<std::mutex> guard(DatalinkControlMutex);
			effectivePassword = replacePassword ? normalizedPassword : logonCode;
		}
		if (!effectivePassword.empty() &&
			!ProtectHoppieCredential(
				effectivePassword,
				protectedPasswordToPersist))
		{
			error = "Windows could not protect the Hoppie code. Settings were not changed.";
			Logger::info("CPDLC settings update rejected because DPAPI protection failed");
			return false;
		}
	}
	bool credentialsChanged = false;
	if (updateConnectionSettings)
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		if (!automaticLogonUpdate && (!replacePassword ||
			ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(logonCallsign)) != normalizedCallsign))
			PdcLogonSynchronization.MarkManualEdit();
		credentialsChanged =
			ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(logonCallsign)) != normalizedCallsign ||
			(replacePassword && logonCode != normalizedPassword);
		logonCallsign = normalizedCallsign;
		if (replacePassword)
			logonCode = normalizedPassword;
	}
	if (credentialsChanged &&
		(HoppieConnected.load(std::memory_order_acquire) ||
			HoppieConnecting.load(std::memory_order_acquire)))
	{
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppieConnected.store(false, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		HoppiePollInProgress.store(false, std::memory_order_release);
		ConnectionMessage.store(false, std::memory_order_relaxed);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage("Credentials changed. Reconnect PDC to apply them.");
	}
	else if (credentialsChanged)
	{
		SetDatalinkStatusMessage("Credentials updated. Ready to connect.");
	}
	if (updateConnectionSettings)
	{
		SaveDataToSettings(
			"cpdlc_logon",
			"The PDC logon callsign",
			normalizedCallsign.c_str());
		SaveDataToSettings(
			"cpdlc_password",
			"The protected PDC Hoppie code",
			protectedPasswordToPersist.c_str());
	}
	return true;
}

bool CSMRPlugin::ConnectDatalink(std::string& error)
{
	SyncPdcLogonWithSelectedAirport();
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
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		error = "PDC is already connected.";
		return false;
	}

	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	if (TrimAsciiWhitespaceCopy(credentials.callsign).empty() ||
		TrimAsciiWhitespaceCopy(credentials.password).empty())
	{
		error = "A PDC logon callsign and Hoppie code are required.";
		return false;
	}
	// Hoppie polling consumes messages. Two EuroScope processes must never poll
	// the same logon independently, even when they use different plugin folders.
	if (DatalinkSessionLease.Owned())
	{
		error = "Previous PDC requests are still finishing. Try again shortly.";
		return false;
	}
	const std::string leaseIdentity = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(credentials.callsign));
	if (!DatalinkSessionLease.Open(L"Hoppie", std::filesystem::u8path(leaseIdentity).wstring()) ||
		!DatalinkSessionLease.Lock(0))
	{
		error = "This PDC logon is already in use by another EuroScope instance. Use that instance for PDC.";
		return false;
	}

	bool expected = false;
	if (!HoppieConnecting.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel))
	{
		error = "A PDC connection attempt is already in progress.";
		return false;
	}
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		HoppieConnecting.store(false, std::memory_order_release);
		error = "PDC is already connected.";
		return false;
	}

	const unsigned long long generation =
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Connecting...");

	DatalinkLoginRequest request;
	request.credentials = credentials;
	request.generation = generation;
	if (!QueueNetworkJob([request]() { datalinkLogin(request); }))
	{
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppieConnecting.store(false, std::memory_order_release);
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
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_release);
	HoppieConnecting.store(false, std::memory_order_release);
	HoppiePollInProgress.store(false, std::memory_order_release);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Disconnected.");
	return true;
}

bool CSMRPlugin::PollDatalink(std::string& error)
{
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) { error = "Poll PDC in the primary EuroScope instance."; return false; }
	return StartDatalinkPoll(true, error);
}

bool CSMRPlugin::EditDatalinkCredentials(std::string& error)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	error.clear();
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) { error = "Edit Hoppie credentials in the primary EuroScope instance."; return false; }
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	auto applyValues = [&](const CCPDLCSettingsDialog& dialog) -> bool
	{
		const DatalinkControlState state = GetDatalinkControlState();
		return UpdateDatalinkControlSettings(
			state.logonCallsign,
			static_cast<const char*>(CStringA(dialog.m_Password)),
			true,
			error);
	};

	CCPDLCSettingsDialog dialog(AfxGetMainWnd());
	dialog.m_Password = credentials.password.c_str();
	INT_PTR dialogResult = dialog.DoModal();
	if (dialogResult == IDOK)
		return applyValues(dialog);

	if (dialogResult == -1)
	{
		CCPDLCSettingsDialog fallbackDialog(nullptr);
		fallbackDialog.m_Password = credentials.password.c_str();
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
		error = "Failed to open PDC credentials window (GetLastError=" +
			std::to_string(static_cast<unsigned long>(lastError)) +
			", resource=" + std::string(dialogResource != nullptr ? "ok" : "missing") + ").";
		return false;
	}

	// Cancel is a successful no-op.
	return true;
}

void CSMRPlugin::ResetDatalinkRuntime()
{
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		logonCallsign.clear();
		PdcSelectedAirport.clear();
		logonCode.clear();
		DatalinkStatusMessage = "Disconnected.";
	}
	ResetDatalinkClearanceState();
}

void CSMRPlugin::LoadDatalinkSettings()
{
	messageId.store(VsmrRandom::UniformInt(1789, 11788));

	DatalinkLastPollAt = PluginSteadyClock::now();
	// Loading and migrating persisted CPDLC settings
	const char * p_value;
	bool migratePlaintextCredential = false;
	std::string migratedProtectedCredential;

	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		if ((p_value = GetDataFromSettings("cpdlc_logon")) != NULL)
			logonCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(p_value));
		if ((p_value = GetDataFromSettings("cpdlc_password")) != NULL)
		{
			bool wasPlaintext = false;
			std::string unprotectedCredential;
			if (UnprotectHoppieCredential(
				p_value,
				unprotectedCredential,
				wasPlaintext))
			{
				logonCode = std::move(unprotectedCredential);
				migratePlaintextCredential = wasPlaintext;
			}
			else
			{
				logonCode.clear();
				DatalinkStatusMessage =
					"The saved Hoppie code could not be unlocked. Enter it again.";
				Logger::info("CPDLC saved credential could not be decrypted");
			}
		}
	}
	if (migratePlaintextCredential)
	{
		const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
		if (ProtectHoppieCredential(
			credentials.password,
			migratedProtectedCredential))
		{
			SaveDataToSettings(
				"cpdlc_password",
				"The protected PDC Hoppie code",
				migratedProtectedCredential.c_str());
			Logger::info("CPDLC saved credential migrated to Windows DPAPI protection");
		}
		else
		{
			SaveDataToSettings(
				"cpdlc_password",
				"The protected PDC Hoppie code",
				"");
			Logger::info("CPDLC plaintext credential migration failed; persistent copy removed");
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
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
}

void CSMRPlugin::PersistDatalinkSettings()
{
	if (DatalinkProxyClient) return; // a closing secondary must not persist stale primary credentials
	// Persisting CPDLC settings through EuroScope
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	SaveDataToSettings("cpdlc_logon", "The PDC logon callsign", credentials.callsign.c_str());
	std::string protectedCredential;
	if (ProtectHoppieCredential(credentials.password, protectedCredential))
	{
		SaveDataToSettings(
			"cpdlc_password",
			"The protected PDC Hoppie code",
			protectedCredential.c_str());
	}
	else
	{
		Logger::info("CPDLC credential was not persisted because DPAPI protection failed");
	}

}
