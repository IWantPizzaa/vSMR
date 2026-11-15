#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "crash/CrashReporter.hpp"
#include "datalink/DatalinkDialog.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "datalink/PdcTsatPrefill.hpp"
#include "radar/RadarScreen.Registry.hpp"
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

namespace
{
	const rapidjson::Value* ActivePdcPolicy()
	{
		const std::string selectedPath = CSMRPlugin::GetActiveProfilesConfigPath();
		for (const CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->IsShutdownRequested())
				continue;
			const CConfig* config = radar->GetConfiguration();
			if (config != nullptr && config->isLayeredConfig() &&
				(selectedPath.empty() || config->getConfigPath() == selectedPath))
				return config->getEffectiveConfigSection("pdc");
		}
		return nullptr;
	}
}

void CSMRPlugin::ForgetDatalinkFlightPlan(const std::string& normalizedCallsign)
{
	std::lock_guard<std::mutex> guard(DatalinkStateMutex);
	ClearDatalinkClearanceSentUnlocked(normalizedCallsign);
	ClearDatalinkClearanceInFlightUnlocked(normalizedCallsign);
}

void CSMRPlugin::HandleDatalinkFunctionCall(
	int FunctionId,
	const char* sItemString,
	RECT Area)
{
	(void)sItemString;
	if (GetConnectionType() == CONNECTION_TYPE_VIA_PROXY) {
		DisplayUserMessage("vSMR", "Proxy PDC", "Use the primary EuroScope instance for PDC actions. This instance displays shared status only.", true, true, true, false, false);
		return;
	}
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

			bool tracked = false;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				tracked = IsDatalinkTrackedUnlocked(fpCallsign);
				RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
				RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
				RemoveCallsignUnlocked(AircraftMessageSent, fpCallsign);
				RemoveCallsignUnlocked(AircraftWilco, fpCallsign);
				RemoveCallsignUnlocked(AircraftMessage, fpCallsign);
				ClearDatalinkClearanceSentUnlocked(fpCallsign);
				PendingMessages.erase(fpCallsign);
			}
			// closes the request at PINEDE too, without telling the pilot; an
			// aircraft PINEDE never gave this station has nothing to close there
			if (tracked && !QueueDatalinkAction(this, DatalinkAction::Reset, fpCallsign))
				Logger::info("PDC reset was not sent to PINEDE (offline or queue full)");
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_STBY) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			// the tag shows STANDBY once PINEDE has sent it
			if (!QueueDatalinkAction(this, DatalinkAction::Standby, fpCallsign))
				DisplayUserMessage("PDC", "Server", "PDC is not connected.", true, true, false, true, false);
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

			std::string toReturn = "";

			if (dia.DoModal() != IDOK)
				return;

			const std::string text = static_cast<const char*>(dia.m_Message);
			if (!VsmrPinede::HasFreeText(text))
			{
				DisplayUserMessage("PDC", "Server", "Type a message before sending it.", true, true, false, true, false);
			}
			else if (!QueueDatalinkAction(this, DatalinkAction::Message, fpCallsign, text))
			{
				DisplayUserMessage("PDC", "Server", "PDC is not connected.", true, true, false, true, false);
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_VOICE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			// PINEDE sends UNABLE CALL ON FREQ and closes the request
			if (!QueueDatalinkAction(this, DatalinkAction::Voice, fpCallsign))
				DisplayUserMessage("PDC", "Server", "PDC is not connected.", true, true, false, true, false);
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

			// Prefilling the PDC from the flight plan and current CDM bridge data
			CDataLinkDialog dia;
			dia.SetDialogMode(CDataLinkDialog::DialogMode::Pdc);
			dia.m_Callsign = fpCallsign;
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();
			dia.m_Departure = FlightPlan.GetFlightPlanData().GetSidName();
			dia.m_Rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			dia.m_SSR = FlightPlan.GetControllerAssignedData().GetSquawk();
			const char* origin = FlightPlan.GetFlightPlanData().GetOrigin();
			const PdcFrequencySelection frequencySelection = ResolvePdcNextFrequency(this, FlightPlan);
			std::string freq = FormatPdcFrequency(frequencySelection.frequency);
			Logger::info(
				std::string("CPDLC PDC next frequency selected: callsign=") + fpCallsign +
				" origin=" + std::string(origin != nullptr ? origin : "") +
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
			const bool prefillTsat = VsmrPdcTsatPrefill::Allowed(
				ActivePdcPolicy(), origin != nullptr ? std::string_view(origin) : std::string_view{});
			CdmPilotData cdmPilot;
			const bool cdmReady = IsCdmBridgeReady();
			// Offer the editable reminder unless CDM positively reports a TOBT.
			dia.SetTobtMissing(true);
			if (cdmReady &&
				TryGetCdmPilotData(fpCallsign, cdmPilot))
			{
				dia.SetTobtMissing(!cdmPilot.hasTobt);
				if (prefillTsat && cdmPilot.hasTsat)
					dia.m_TSAT = FormatUtcHhmm(cdmPilot.tsatUtc).c_str();
				if (cdmPilot.hasCtot)
					dia.m_CTOT = FormatUtcHhmm(cdmPilot.ctotUtc).c_str();
			}

			std::string toReturn = "";

			int ClearedAltitude = FlightPlan.GetControllerAssignedData().GetClearedAltitude();
			int Ta = GetTransitionAltitude();

			if (ClearedAltitude != 0) {
				if (ClearedAltitude > Ta && ClearedAltitude > 2) {
					std::string str = std::to_string(ClearedAltitude);
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
			request.generation = DatalinkConnectionGeneration.load(
				std::memory_order_acquire);
			// PINEDE turns these fields into the clearance text
			request.fields.callsign = NormalizeCallsignForState(fpCallsign);
			request.fields.destination = FlightPlan.GetFlightPlanData().GetDestination();
			request.fields.runway = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			request.fields.sid = FlightPlan.GetFlightPlanData().GetSidName();
			request.fields.tsat = static_cast<const char*>(dia.m_TSAT);
			request.fields.ctot = static_cast<const char*>(dia.m_CTOT);
			request.fields.frequency = static_cast<const char*>(dia.m_Freq);
			request.fields.message = static_cast<const char*>(dia.m_Message);
			request.fields.squawk = FlightPlan.GetControllerAssignedData().GetSquawk();
			request.fields.initialClimb = toReturn;
			// The dialog has resolved or explicitly supplied the departure frequency.
			// Never substitute the issuing station when it serves a different airport.
			request.fields.fallbackFrequency = request.fields.frequency;
			if (!DatalinkConnected.load(std::memory_order_acquire))
			{
				DisplayUserMessage("PDC", "Server", "PDC is not connected.", true, true, false, true, false);
			}
			else if (!QueueNetworkJob([request]() { sendDatalinkClearance(request); }))
			{
				Logger::info("PDC clearance could not be queued");
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
			"type=%d controller=%d callsign=%.31s position=%.31s pdc=%d connecting=%d",
			currentConnectionType,
			myself.IsValid() && myself.IsController() ? 1 : 0,
			callsign,
			position,
			DatalinkConnected.load(std::memory_order_acquire) ? 1 : 0,
			DatalinkConnecting.load(std::memory_order_acquire) ? 1 : 0);
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
	}

	// PINEDE routes by callsign: follow EuroScope's, for the next request
	if (GetConnectionType() == CONNECTION_TYPE_DIRECT)
		RefreshDatalinkStation();

	if (DatalinkConnected.load() && ConnectionMessage.load()) {
		const std::string message = GetDatalinkStatusMessageCopy();
		DisplayUserMessage("PDC", "Server", message.empty() ? "Logged in!" : message.c_str(), true, true, false, true, false);
		ConnectionMessage.store(false);
	}

	for (const std::string& notice : TakeDatalinkNotices())
		DisplayUserMessage("PDC", "Server", notice.c_str(), true, true, false, true, false);

	if (FailedToConnectMessage.load()) {
		std::string failureMessage = GetDatalinkStatusMessageCopy();
		if (failureMessage.empty())
			failureMessage = "Could not connect to PINEDE.";
		DisplayUserMessage("PDC", "Server", failureMessage.c_str(), true, true, false, true, false);
		FailedToConnectMessage.store(false);
	}

	if ((DatalinkConnected.load(std::memory_order_acquire) ||
		DatalinkConnecting.load(std::memory_order_acquire)) &&
		GetConnectionType() == CONNECTION_TYPE_NO) {
		std::string disconnectError;
		DisconnectDatalink(disconnectError);
		SetDatalinkStatusMessage("Automatically disconnected because EuroScope is offline.");
		DisplayUserMessage("PDC", "Server", "Automatically logged off!", true, true, false, true, false);
	}

	// ----- Polling PINEDE -----
	const PluginSteadyClock::time_point timerNow = PluginSteadyClock::now();
	if (!PluginShutdownRequested.load(std::memory_order_relaxed) &&
		timerNow - DatalinkLastPollAt > std::chrono::seconds(10) &&
		DatalinkConnected.load()) {
		std::string pollError;
		StartDatalinkPoll(false, pollError);
		DatalinkLastPollAt = timerNow;
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
					const bool airborne = radarTarget.IsValid() && radarTarget.GetGS() > 160;
					// PINEDE keeps reporting the WILCO; remember it is no longer needed
					if (airborne)
						AircraftWilcoDismissed.insert(callsign);
					return airborne;
				}),
			AircraftWilco.end());
	}
}
