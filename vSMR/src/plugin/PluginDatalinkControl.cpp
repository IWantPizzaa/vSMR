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
	state.cdmAutoEnabled = CdmAutoModeEnabled.load(std::memory_order_relaxed);
	state.cdmDelayMinutes = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	state.cdmCooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
	state.vacdmConfigured = VacdmPollingEnabled.load(std::memory_order_relaxed);
	state.vacdmReady = IsVacdmSnapshotReadyForCdm();
	state.activeAirport = ResolveActiveAirportFilterUpper();

	std::string aliasMessage;
	state.cdmAliasReady = TryReadCdmReminderMessageFromAlias(
		const_cast<CSMRPlugin*>(this),
		aliasMessage,
		state.cdmAliasPath);
	return state;
}

bool CSMRPlugin::UpdateDatalinkControlSettings(
	const std::string& callsign,
	const std::string& password,
	bool replacePassword,
	bool cdmAutoEnabled,
	int delayMinutes,
	int cooldownMinutes,
	std::string& error,
	bool updateConnectionSettings)
{
	error.clear();
	const bool previousAutoEnabled =
		CdmAutoModeEnabled.load(std::memory_order_relaxed);
	const int previousDelayMinutes =
		CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	const bool stoppedAutomaticallyBeforeValidation =
		previousAutoEnabled && !cdmAutoEnabled;
	if (stoppedAutomaticallyBeforeValidation)
	{
		// Stop is fail-safe: malformed or unavailable CPDLC credentials must never
		// keep an already-running automatic reminder session alive.
		CdmAutoModeEnabled.store(false, std::memory_order_relaxed);
		ClearCdmAutoTrackingState(true);
		SaveDataToSettings(
			"cdm_auto_enabled",
			"Enable automatic CDM reminder messaging for this session",
			"0");
	}
	const std::string normalizedCallsign =
		ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	const std::string normalizedPassword =
		replacePassword ? TrimAsciiWhitespaceCopy(password) : std::string();
	if (updateConnectionSettings && normalizedCallsign.empty())
	{
		error = "The CPDLC logon callsign is required.";
		return false;
	}
	if (updateConnectionSettings && replacePassword && normalizedPassword.empty())
	{
		error = "Enter a Hoppie code before replacing the saved code.";
		return false;
	}
	if (delayMinutes < 0 || delayMinutes > CdmMaximumMinutes)
	{
		error = "The CDM auto delay must be between 0 and 1440 minutes.";
		return false;
	}
	if (cooldownMinutes < 0 || cooldownMinutes > CdmMaximumMinutes)
	{
		error = "The CDM reminder cooldown must be between 0 and 1440 minutes.";
		return false;
	}

	if (cdmAutoEnabled && !previousAutoEnabled)
	{
		if (!ControllerMyself().IsController())
		{
			error = "EuroScope is not connected as a controller.";
			return false;
		}
		if (ResolveActiveAirportFilterUpper().empty())
		{
			error = "Select an active airport before starting CDM reminders.";
			return false;
		}
		if (!VacdmPollingEnabled.load(std::memory_order_acquire))
		{
			error = "Configure a vACDM server for the active profile before starting CDM reminders.";
			return false;
		}
		if (!IsVacdmSnapshotReadyForCdm())
		{
			error = "Wait for a current vACDM snapshot before starting CDM reminders.";
			return false;
		}
		std::string reminderMessage;
		std::string aliasPath;
		if (!TryReadCdmReminderMessageFromAlias(this, reminderMessage, aliasPath))
		{
			error = "Add a valid .cdm alias before starting CDM reminders.";
			return false;
		}
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
		SetDatalinkStatusMessage("Credentials changed. Reconnect CPDLC to apply them.");
	}
	else if (credentialsChanged)
	{
		SetDatalinkStatusMessage("Credentials updated. Ready to connect.");
	}
	CdmAutoModeEnabled.store(cdmAutoEnabled, std::memory_order_relaxed);
	CdmAutoDelayMinutes.store(delayMinutes, std::memory_order_relaxed);
	CdmReminderCooldownMinutes.store(cooldownMinutes, std::memory_order_relaxed);

	if (!stoppedAutomaticallyBeforeValidation &&
		(previousAutoEnabled != cdmAutoEnabled ||
			previousDelayMinutes != delayMinutes))
	{
		ClearCdmAutoTrackingState(true);
	}

	if (updateConnectionSettings)
	{
		SaveDataToSettings(
			"cpdlc_logon",
			"The CPDLC logon callsign",
			normalizedCallsign.c_str());
		SaveDataToSettings(
			"cpdlc_password",
			"The protected CPDLC Hoppie code",
			protectedPasswordToPersist.c_str());
	}
	SaveDataToSettings(
		"cdm_auto_enabled",
		"Enable automatic CDM reminder messaging for this session",
		"0");
	SaveDataToSettings(
		"cdm_auto_delay_min",
		"CDM auto reminder delay in minutes",
		std::to_string(delayMinutes).c_str());
	SaveDataToSettings(
		"cdm_cooldown_min",
		"CDM reminder resend cooldown in minutes",
		std::to_string(cooldownMinutes).c_str());
	return true;
}

bool CSMRPlugin::ConnectDatalink(std::string& error)
{
	error.clear();
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		error = "The CPDLC service is shutting down.";
		return false;
	}
	if (!ControllerMyself().IsController())
	{
		error = "You are not logged in as a controller.";
		return false;
	}
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		error = "CPDLC is already connected.";
		return false;
	}

	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	if (TrimAsciiWhitespaceCopy(credentials.callsign).empty() ||
		TrimAsciiWhitespaceCopy(credentials.password).empty())
	{
		error = "A CPDLC logon callsign and Hoppie code are required.";
		return false;
	}

	bool expected = false;
	if (!HoppieConnecting.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel))
	{
		error = "A CPDLC connection attempt is already in progress.";
		return false;
	}
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		HoppieConnecting.store(false, std::memory_order_release);
		error = "CPDLC is already connected.";
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
		error = "Unable to queue the CPDLC connection request.";
		SetDatalinkStatusMessage(error);
		return false;
	}
	return true;
}

bool CSMRPlugin::DisconnectDatalink(std::string& error)
{
	error.clear();
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
	return StartDatalinkPoll(true, error);
}

bool CSMRPlugin::RunCdmReminderScan(std::string& result, std::string& error)
{
	result.clear();
	error.clear();
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		error = "The CDM reminder service is shutting down.";
		return false;
	}
	if (!ControllerMyself().IsController())
	{
		error = "EuroScope is not connected as a controller.";
		return false;
	}
	const std::string activeAirport = ResolveActiveAirportFilterUpper();
	if (activeAirport.empty())
	{
		error = "Select an active airport before checking CDM reminders.";
		return false;
	}
	if (!VacdmPollingEnabled.load(std::memory_order_acquire))
	{
		error = "Configure a vACDM server for the active profile before checking CDM reminders.";
		return false;
	}
	if (!IsVacdmSnapshotReadyForCdm())
	{
		error = "Wait for a current vACDM snapshot before checking CDM reminders.";
		return false;
	}

	const std::vector<std::string> candidateCallsigns =
		CollectFlightPlanCandidateCallsignsForActiveAirport(this, activeAirport);
	const auto now = std::chrono::steady_clock::now();
	std::string reminderMessage;
	std::string aliasPath;
	if (!TryReadCdmReminderMessageFromAlias(this, reminderMessage, aliasPath))
	{
		error = "Missing or invalid .cdm alias";
		if (!aliasPath.empty())
			error += " in " + aliasPath;
		error += ".";
		return false;
	}

	int alreadyNotifiedCount = 0;
	int alreadyQueuedCount = 0;
	int alreadyClearedCount = 0;
	int hasTobtCount = 0;
	int queuedCount = 0;
	int failedCount = 0;
	int missingVacdmCount = 0;

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(now);
	}

	for (const std::string& callsign : candidateCallsigns)
	{
		bool vacdmEvaluated = false;
		bool hasVacdmData = false;
		const CdmQueueReminderOutcome outcome =
			TryQueueCdmReminderForCallsign(
				this,
				callsign,
				reminderMessage,
				now,
				&vacdmEvaluated,
				&hasVacdmData);
		if (vacdmEvaluated && !hasVacdmData)
			++missingVacdmCount;

		switch (outcome)
		{
		case CdmQueueReminderOutcome::Queued:
			++queuedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyNotified:
			++alreadyNotifiedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyQueued:
			++alreadyQueuedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyCleared:
			++alreadyClearedCount;
			break;
		case CdmQueueReminderOutcome::HasSubmittedTobt:
			++hasTobtCount;
			break;
		case CdmQueueReminderOutcome::Failed:
		default:
			++failedCount;
			break;
		}
	}

	const int checkedCount = static_cast<int>(candidateCallsigns.size());
	result = "CDM check: ";
	result += std::to_string(checkedCount) + " checked, ";
	result += std::to_string(queuedCount) + " queued, ";
	result += std::to_string(hasTobtCount) + " already has TOBT, ";
	result += std::to_string(alreadyNotifiedCount) + " already notified, ";
	result += std::to_string(alreadyQueuedCount) + " already queued, ";
	result += std::to_string(alreadyClearedCount) + " already cleared, ";
	result += std::to_string(missingVacdmCount) + " missing VACDM, ";
	result += std::to_string(failedCount) + " failed.";
	return true;
}

bool CSMRPlugin::EditDatalinkCredentials(std::string& error)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	error.clear();
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	auto applyValues = [&](const CCPDLCSettingsDialog& dialog) -> bool
	{
		const DatalinkControlState state = GetDatalinkControlState();
		return UpdateDatalinkControlSettings(
			state.logonCallsign,
			static_cast<const char*>(CStringA(dialog.m_Password)),
			true,
			state.cdmAutoEnabled,
			state.cdmDelayMinutes,
			state.cdmCooldownMinutes,
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
		error = "Failed to open CPDLC credentials window (GetLastError=" +
			std::to_string(static_cast<unsigned long>(lastError)) +
			", resource=" + std::string(dialogResource != nullptr ? "ok" : "missing") + ").";
		return false;
	}

	// Cancel is a successful no-op.
	return true;
}


void CSMRPlugin::ResetDatalinkRuntime()
{
	VsmrEuroScopeCommandLine::Cancel(
		VsmrEuroScopeCommandLine::Owner::CdmReminder);
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		logonCallsign = "EGKK";
		logonCode.clear();
		DatalinkStatusMessage = "Disconnected.";
	}
	CdmAutoModeEnabled.store(false, std::memory_order_relaxed);
	CdmAutoDelayMinutes.store(5, std::memory_order_relaxed);
	CdmReminderCooldownMinutes.store(60, std::memory_order_relaxed);
	ResetCdmReminderSessionState();
}

void CSMRPlugin::LoadDatalinkSettings()
{
	messageId.store(rand() % 10000 + 1789);

	DatalinkLastPollAt = PluginSteadyClock::now();
	VacdmLastFetchTick = 0;

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
				"The protected CPDLC Hoppie code",
				migratedProtectedCredential.c_str());
			Logger::info("CPDLC saved credential migrated to Windows DPAPI protection");
		}
		else
		{
			SaveDataToSettings(
				"cpdlc_password",
				"The protected CPDLC Hoppie code",
				"");
			Logger::info("CPDLC plaintext credential migration failed; persistent copy removed");
		}
	}
	if ((p_value = GetDataFromSettings("cdm_auto_delay_min")) != NULL)
	{
		int parsedDelayMinutes = 0;
		if (TryParseNonNegativeInt(p_value, parsedDelayMinutes))
			CdmAutoDelayMinutes.store(parsedDelayMinutes, std::memory_order_relaxed);
	}
	if ((p_value = GetDataFromSettings("cdm_cooldown_min")) != NULL)
	{
		int parsedCooldownMinutes = 0;
		if (TryParseNonNegativeInt(p_value, parsedCooldownMinutes))
			CdmReminderCooldownMinutes.store(parsedCooldownMinutes, std::memory_order_relaxed);
	}
}

void CSMRPlugin::ResetDatalinkProfileSource()
{
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		ActiveProfilesConfigPath.clear();
		ActiveProfilesConfigPathClaimed = false;
		ProfilesSourceGeneration = 0;
		VacdmConfiguredServerUrl.clear();
	}
	PublishActiveProfilesConfigPath(
		ResolveDefaultProfilesConfigPath().u8string(),
		false);
}

void CSMRPlugin::BeginDatalinkShutdown()
{
	CdmAutoModeEnabled.store(false, std::memory_order_relaxed);
	ClearCdmAutoTrackingState(true);
}

void CSMRPlugin::PrepareDatalinkRuntimeForExit()
{
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	VacdmPollingEnabled.store(false, std::memory_order_relaxed);
}

void CSMRPlugin::PersistDatalinkSettings()
{
	// Persisting CPDLC settings through EuroScope
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", credentials.callsign.c_str());
	std::string protectedCredential;
	if (ProtectHoppieCredential(credentials.password, protectedCredential))
	{
		SaveDataToSettings(
			"cpdlc_password",
			"The protected CPDLC Hoppie code",
			protectedCredential.c_str());
	}
	else
	{
		Logger::info("CPDLC credential was not persisted because DPAPI protection failed");
	}
	// Run/Stop is deliberately session-only. Persist a safe value for older builds.
	SaveDataToSettings("cdm_auto_enabled", "Enable automatic CDM reminder messaging", "0");
	int cdmAutoDelayToPersist = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	if (cdmAutoDelayToPersist < 0)
		cdmAutoDelayToPersist = 0;
	SaveDataToSettings("cdm_auto_delay_min", "CDM auto reminder delay in minutes", std::to_string(cdmAutoDelayToPersist).c_str());
	int cdmCooldownToPersist = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
	if (cdmCooldownToPersist < 0)
		cdmCooldownToPersist = 0;
	SaveDataToSettings("cdm_cooldown_min", "CDM reminder resend cooldown in minutes", std::to_string(cdmCooldownToPersist).c_str());
}
