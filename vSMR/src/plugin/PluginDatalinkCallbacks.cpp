#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "crash/CrashReporter.hpp"
#include "datalink/DataLinkDialog.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"
#include "tags/TagDataTypes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

using VsmrDatalinkProtocol::FormatPdcFrequency;
using VsmrDatalinkProtocol::PdcFrequencySelection;
using VsmrDatalinkProtocol::ResolvePdcNextFrequency;

void CSMRPlugin::ForgetDatalinkFlightPlan(const std::string& normalizedCallsign)
{
	std::lock_guard<std::mutex> guard(DatalinkStateMutex);
	AircraftCdmAutoTracked.erase(normalizedCallsign);
	RemoveQueuedCdmReminderUnlocked(normalizedCallsign);
	ClearDatalinkClearanceSentUnlocked(normalizedCallsign);
	ClearDatalinkClearanceInFlightUnlocked(normalizedCallsign);
	if (CdmReminderCooldownMinutes.load(std::memory_order_relaxed) == 0)
		AircraftCdmTobtReminderSentAt.erase(normalizedCallsign);
}

void CSMRPlugin::HandleDatalinkFunctionCall(
	int FunctionId,
	const char* sItemString,
	RECT Area)
{
	(void)sItemString;
	// ----- Handling CPDLC tag actions -----
	if (FunctionId == TAG_FUNC_DATALINK_MENU) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		bool menu_is_datalink = true;

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign != nullptr && fpCallsign[0] != '\0')
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				if (ContainsCallsignUnlocked(AircraftDemandingClearance, fpCallsign))
					menu_is_datalink = false;
			}
		}

		OpenPopupList(Area, "Datalink menu", 1);
		AddPopupListElement("Confirm", "", TAG_FUNC_DATALINK_CONFIRM, false, 2, menu_is_datalink);
		AddPopupListElement("Message", "", TAG_FUNC_DATALINK_MESSAGE, false, 2, false, true);
		AddPopupListElement("Standby", "", TAG_FUNC_DATALINK_STBY, false, 2, menu_is_datalink);
		AddPopupListElement("Voice", "", TAG_FUNC_DATALINK_VOICE, false, 2, menu_is_datalink);
		AddPopupListElement("Reset", "", TAG_FUNC_DATALINK_RESET, false, 2, false, true);
		AddPopupListElement("Close", "", EuroScopePlugIn::TAG_ITEM_FUNCTION_NO, false, 2, false, true);
	}

	if (FunctionId == TAG_FUNC_DATALINK_RESET) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
			RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
			RemoveCallsignUnlocked(AircraftMessageSent, fpCallsign);
			RemoveCallsignUnlocked(AircraftWilco, fpCallsign);
			RemoveCallsignUnlocked(AircraftMessage, fpCallsign);
			ClearDatalinkClearanceSentUnlocked(fpCallsign);
			PendingMessages.erase(fpCallsign);
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_STBY) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				AddCallsignUniqueUnlocked(AircraftStandby, fpCallsign);
			}
			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"CPDLC",
				"STANDBY",
				fpCallsign))
			{
				Logger::info("CPDLC STANDBY request could not be queued");
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_MESSAGE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			AFX_MANAGE_STATE(AfxGetStaticModuleState());
			CDataLinkDialog dia;
			dia.SetDialogMode(CDataLinkDialog::DialogMode::Message);
			dia.m_Callsign = fpCallsign;
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();

			AcarsMessage msg;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				auto msgIt = PendingMessages.find(fpCallsign);
				if (msgIt != PendingMessages.end())
					msg = msgIt->second;
			}
			dia.m_Req = msg.message.c_str();

			string toReturn = "";

			if (dia.DoModal() != IDOK)
				return;

			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"TELEX",
				static_cast<const char*>(dia.m_Message),
				fpCallsign))
			{
				Logger::info("CPDLC free-text message could not be queued");
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_VOICE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
				RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
				PendingMessages.erase(fpCallsign);
			}

			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"CPDLC",
				"UNABLE CALL ON FREQ",
				fpCallsign))
			{
				Logger::info("CPDLC voice fallback message could not be queued");
			}
		}

	}

	if (FunctionId == TAG_FUNC_DATALINK_CONFIRM) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			// Reserving the aircraft while the controller composes its clearance
			AFX_MANAGE_STATE(AfxGetStaticModuleState());
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				if (HasDatalinkClearanceSentUnlocked(fpCallsign) ||
					HasDatalinkClearanceInFlightUnlocked(fpCallsign))
				{
					return;
				}
				MarkDatalinkClearanceInFlightUnlocked(fpCallsign);
			}
			struct PdcCompositionGuard
			{
				std::string callsign;
				bool retainedByNetworkJob = false;
				~PdcCompositionGuard()
				{
					if (retainedByNetworkJob)
						return;
					std::lock_guard<std::mutex> guard(DatalinkStateMutex);
					ClearDatalinkClearanceInFlightUnlocked(callsign);
				}
			} compositionGuard{ fpCallsign };

			// Prefilling the PDC from the flight plan and current vACDM snapshot
			CDataLinkDialog dia;
			dia.SetDialogMode(CDataLinkDialog::DialogMode::Pdc);
			dia.m_Callsign = fpCallsign;
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();
			dia.m_Departure = FlightPlan.GetFlightPlanData().GetSidName();
			dia.m_Rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			dia.m_SSR = FlightPlan.GetControllerAssignedData().GetSquawk();
			const PdcFrequencySelection frequencySelection = ResolvePdcNextFrequency(this, FlightPlan);
			string freq = FormatPdcFrequency(frequencySelection.frequency);
			if (freq.empty())
				freq = FormatPdcFrequency(ControllerMyself().GetPrimaryFrequency());
			Logger::info(
				std::string("CPDLC PDC next frequency selected: callsign=") + fpCallsign +
				" controller=" + (frequencySelection.controller.empty() ? "none" : frequencySelection.controller) +
				" frequency=" + (freq.empty() ? "none" : freq) +
				" source=" + (frequencySelection.source.empty() ? "none" : frequencySelection.source));
			dia.m_Freq = freq.c_str();
			AcarsMessage msg;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				auto msgIt = PendingMessages.find(fpCallsign);
				if (msgIt != PendingMessages.end())
					msg = msgIt->second;
			}
			dia.m_Req = msg.message.c_str();
			VacdmPilotData vacdmPilot;
			if (IsVacdmSnapshotReadyForCdm() &&
				TryGetVacdmPilotData(fpCallsign, vacdmPilot))
			{
				if (vacdmPilot.hasTsat)
					dia.m_TSAT = FormatUtcHhmm(vacdmPilot.tsatUtc).c_str();
				if (vacdmPilot.hasCtot)
					dia.m_CTOT = FormatUtcHhmm(vacdmPilot.ctotUtc).c_str();
			}

			string toReturn = "";

			int ClearedAltitude = FlightPlan.GetControllerAssignedData().GetClearedAltitude();
			int Ta = GetTransitionAltitude();

			if (ClearedAltitude != 0) {
				if (ClearedAltitude > Ta && ClearedAltitude > 2) {
					string str = std::to_string(ClearedAltitude);
					for (size_t i = 0; i < 5 - str.length(); i++)
						str = "0" + str;
					if (str.size() > 3)
						str.erase(str.begin() + 3, str.end());
					toReturn = "FL";
					toReturn += str;
				}
				else if (ClearedAltitude <= Ta && ClearedAltitude > 2) {


					toReturn = std::to_string(ClearedAltitude);
					toReturn += "ft";
				}
			}
			dia.m_Climb = toReturn.c_str();

			if (dia.DoModal() != IDOK)
				return;

			// Queuing an immutable clearance snapshot
			DatalinkClearanceRequest request;
			request.credentials = SnapshotDatalinkCredentials();
			request.generation = HoppieConnectionGeneration.load(
				std::memory_order_acquire);
			request.packet.callsign = fpCallsign;
			request.packet.destination = FlightPlan.GetFlightPlanData().GetDestination();
			request.packet.rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			request.packet.sid = FlightPlan.GetFlightPlanData().GetSidName();
			request.packet.tsat = static_cast<const char*>(dia.m_TSAT);
			request.packet.ctot = static_cast<const char*>(dia.m_CTOT);
			request.packet.freq = static_cast<const char*>(dia.m_Freq);
			request.packet.message = static_cast<const char*>(dia.m_Message);
			request.packet.squawk = FlightPlan.GetControllerAssignedData().GetSquawk();
			request.packet.climb = toReturn;
			request.fallbackFrequency = FormatPdcFrequency(ControllerMyself().GetPrimaryFrequency());
			if (request.fallbackFrequency.empty())
				request.fallbackFrequency = freq;
			request.messageSequence = messageId.fetch_add(1) + 1;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				AircraftCdmAutoTracked.erase(NormalizeCallsignForState(fpCallsign));
			}
			if (!QueueNetworkJob([request]() { sendDatalinkClearance(request); }))
			{
				Logger::info("CPDLC clearance could not be queued");
			}
			else
			{
				compositionGuard.retainedByNetworkJob = true;
			}

		}

	}
}

void CSMRPlugin::RunDatalinkTimerCycle()
{
	// ----- Updating connection state -----
	static int lastConnectionType = -999;
	static PluginSteadyClock::time_point lastConnectionTypeChangeAt{};
	const int currentConnectionType = GetConnectionType();
	{
		const CController myself = ControllerMyself();
		const char* const callsign = myself.IsValid() && myself.GetCallsign() != nullptr
			? myself.GetCallsign()
			: "";
		const char* const position = myself.IsValid() && myself.GetPositionId() != nullptr
			? myself.GetPositionId()
			: "";
		char connectionState[192]{};
		_snprintf_s(
			connectionState,
			sizeof(connectionState),
			_TRUNCATE,
			"type=%d controller=%d callsign=%.31s position=%.31s cpdlc=%d connecting=%d",
			currentConnectionType,
			myself.IsValid() && myself.IsController() ? 1 : 0,
			callsign,
			position,
			HoppieConnected.load(std::memory_order_acquire) ? 1 : 0,
			HoppieConnecting.load(std::memory_order_acquire) ? 1 : 0);
		static char lastConnectionState[192]{};
		if (strcmp(lastConnectionState, connectionState) != 0)
		{
			strcpy_s(lastConnectionState, connectionState);
			VsmrCrashReporter::RecordState("connection", connectionState);
		}
	}

	if (currentConnectionType != lastConnectionType)
	{
		Logger::info("EuroScope connection_type=" + std::to_string(currentConnectionType));
		lastConnectionType = currentConnectionType;
		lastConnectionTypeChangeAt = PluginSteadyClock::now();
	}

	const unsigned long pendingVacdmSehCode = VacdmLastSehCode.exchange(0, std::memory_order_relaxed);
	if (pendingVacdmSehCode != 0)
		Logger::info("VACDM refresh SEH exception code=" + FormatSehCode(pendingVacdmSehCode));

	{
		std::string aselCallsign;
		const CFlightPlan aselFlightPlan = FlightPlanSelectASEL();
		if (aselFlightPlan.IsValid())
		{
			const char* callsign = aselFlightPlan.GetCallsign();
			if (callsign != NULL)
				aselCallsign = ToUpperAsciiCopy(callsign);
		}

		std::lock_guard<std::mutex> stateGuard(VacdmDebugStateMutex);
		VacdmDebugAselCallsign = aselCallsign;
	}

	if (HoppieConnected.load() && ConnectionMessage.load()) {
		DisplayUserMessage("CPDLC", "Server", "Logged in!", true, true, false, true, false);
		ConnectionMessage.store(false);
	}

	if (FailedToConnectMessage.load()) {
		std::string failureMessage = GetDatalinkStatusMessageCopy();
		if (failureMessage.empty())
			failureMessage = "Could not connect to Hoppie.";
		DisplayUserMessage("CPDLC", "Server", failureMessage.c_str(), true, true, false, true, false);
		FailedToConnectMessage.store(false);
	}

	if ((HoppieConnected.load(std::memory_order_acquire) ||
		HoppieConnecting.load(std::memory_order_acquire)) &&
		GetConnectionType() == CONNECTION_TYPE_NO) {
		std::string disconnectError;
		DisconnectDatalink(disconnectError);
		SetDatalinkStatusMessage("Automatically disconnected because EuroScope is offline.");
		DisplayUserMessage("CPDLC", "Server", "Automatically logged off!", true, true, false, true, false);
	}

	// ----- Polling CPDLC and vACDM -----
	const PluginSteadyClock::time_point timerNow = PluginSteadyClock::now();
	if (!PluginShutdownRequested.load(std::memory_order_relaxed) &&
		timerNow - DatalinkLastPollAt > std::chrono::seconds(10) &&
		HoppieConnected.load()) {
		std::string pollError;
		StartDatalinkPoll(false, pollError);
		DatalinkLastPollAt = timerNow;
	}

	const bool networkConnectionActive = (currentConnectionType != CONNECTION_TYPE_NO);
	const bool vacdmPollingEnabled = VacdmPollingEnabled.load(std::memory_order_relaxed);
	const bool connectionStableForVacdm = networkConnectionActive &&
		(lastConnectionTypeChangeAt == PluginSteadyClock::time_point{} ||
			timerNow - lastConnectionTypeChangeAt >= std::chrono::seconds(20));

	const PluginSteadyTick vacdmNow = CurrentSteadyTick();
	const PluginSteadyTick lastVacdmFetchTick = VacdmLastFetchTick.load();
	if (vacdmPollingEnabled &&
		!PluginShutdownRequested.load(std::memory_order_relaxed) &&
		connectionStableForVacdm &&
		HasSteadyIntervalElapsed(
			vacdmNow,
			lastVacdmFetchTick,
			std::chrono::seconds(VacdmFetchIntervalSeconds)) &&
		!VacdmFetchInProgress.load())
	{
		bool expected = false;
		if (VacdmFetchInProgress.compare_exchange_strong(expected, true))
		{
			if (PluginShutdownRequested.load(std::memory_order_relaxed))
			{
				VacdmFetchInProgress.store(false);
				VacdmLastFetchTick = CurrentSteadyTick();
			}
			else
			{
				if (!QueueNetworkJob([]() { refreshVacdmData(); }))
				{
					VacdmFetchInProgress.store(false);
					VacdmLastFetchTick = CurrentSteadyTick();
					Logger::info("VACDM refresh could not be queued");
				}
			}
		}
	}

	// ----- Processing CDM reminders -----
	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		ProcessCdmAutoMode(this);
		ProcessQueuedCdmReminderMessages(this);
	}

	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftWilco.erase(
			std::remove_if(
				AircraftWilco.begin(),
				AircraftWilco.end(),
				[&](const std::string& callsign)
				{
					CRadarTarget radarTarget = RadarTargetSelect(callsign.c_str());
					return radarTarget.IsValid() && radarTarget.GetGS() > 160;
				}),
			AircraftWilco.end());
	}
}
