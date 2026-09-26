#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginRuntimeAudio.hpp"

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "crash/CrashRuntime.hpp"
#include "insets/InsetWindow.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "rdf/RdfOverlay.hpp"
#include "scene/TargetRoleLogic.hpp"

#include <atomic>
#include <string>

void CSMRPlugin::OnTimer(int Counter)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnTimer");
	(void)Counter;
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (Logger::is_verbose_mode())
		Logger::info(std::string(__FUNCSIG__));
	BLINK = !BLINK;
	VsmrRdf::OnTimer();

	// ----- Cleaning airborne holding points -----
	for (const std::string& callsign : VsmrHoldingPoint::KnownCallsigns())
	{
		CFlightPlan flightPlan = FlightPlanSelect(callsign.c_str());
		if (!flightPlan.IsValid() || !flightPlan.GetTrackingControllerIsMe())
			continue;

		CRadarTarget radarTarget = flightPlan.GetCorrelatedRadarTarget();
		if (!radarTarget.IsValid())
			continue;
		const CRadarTargetPositionData position = radarTarget.GetPosition();
		if (!position.IsValid() || position.GetReportedGS() <= 50)
			continue;

		CFlightPlanData flightPlanData = flightPlan.GetFlightPlanData();
		const char* rawRemarks = flightPlanData.GetRemarks();
		const std::string remarks = rawRemarks != nullptr ? rawRemarks : "";
		if (VsmrHoldingPoint::Read(remarks).empty())
		{
			(void)VsmrHoldingPoint::Resolve(callsign, remarks);
			continue;
		}

		const std::string updatedRemarks = VsmrHoldingPoint::Write(remarks, "");
		if (updatedRemarks != remarks &&
			flightPlanData.SetRemarks(updatedRemarks.c_str()) &&
			flightPlanData.AmendFlightPlan())
		{
			VsmrHoldingPoint::RememberPending(callsign, "");
			FlightDataRefreshPending.store(true, std::memory_order_release);
		}
	}
	// ----- Clearing stale shared ground states -----
	// A shared state only holds while the aircraft is on the ground and the
	// EuroScope status is still the one vSMR wrote with it. Once either stops
	// being true the reserved assigned speed is stale and must not be left on the
	// aircraft for the next controller.
	for (const std::string& callsign : VsmrGroundState::SharedStateCallsigns())
	{
		CFlightPlan flightPlan = FlightPlanSelect(callsign.c_str());
		if (!flightPlan.IsValid())
		{
			VsmrGroundState::ForgetAircraft(callsign.c_str());
			continue;
		}

		CFlightPlanControllerAssignedData assignedData = flightPlan.GetControllerAssignedData();
		const int assignedSpeed = assignedData.GetAssignedSpeed();
		if (!VsmrGroundStateSync::IsReservedAssignedSpeed(assignedSpeed))
		{
			VsmrGroundState::ForgetAircraft(callsign.c_str());
			continue;
		}

		int reportedGs = 0;
		CRadarTarget radarTarget = flightPlan.GetCorrelatedRadarTarget();
		if (radarTarget.IsValid())
		{
			const CRadarTargetPositionData position = radarTarget.GetPosition();
			if (position.IsValid())
				reportedGs = position.GetReportedGS();
		}
		// Every shared state is a departure state, so the departure threshold is
		// the one that turns the tag airborne here.
		const bool airborne = VsmrTargetRoleLogic::IsAirborneForTagRole(false, reportedGs);
		// A controller who picks another status from outside vSMR takes the
		// aircraft off its shared state. The status and the assigned speed reach a
		// client as two updates, so only a mismatch that outlives the grace counts.
		const bool statusChangedOutsideVsmr = VsmrGroundState::HasSettledStatusMismatch(
			callsign.c_str(),
			classifyGroundState(flightPlan.GetGroundState(), reportedGs, false) !=
				VsmrGroundStateSync::CompanionCategoryForAssignedSpeed(assignedSpeed));
		if (!airborne && !statusChangedOutsideVsmr)
			continue;

		// EuroScope only accepts controller assigned data from the tracking
		// controller, so anybody may clean up an aircraft nobody tracks.
		const char* trackingController = flightPlan.GetTrackingControllerCallsign();
		const bool untracked = trackingController == nullptr || trackingController[0] == '\0';
		if (!flightPlan.GetTrackingControllerIsMe() && !untracked)
			continue;

		if (assignedData.SetAssignedSpeed(0))
		{
			VsmrGroundState::ForgetAircraft(callsign.c_str());
			FlightDataRefreshPending.store(true, std::memory_order_release);
		}
	}

	// One bridge attach and one flight-plan scan per tick, shared by every provider.
	const VsmrPluginBridge::Tick bridgeTick = VsmrPluginBridge::BeginTick(*this);
	const bool vsidChanged = VsmrVsid::Poll(bridgeTick);
	const bool cdmChanged = VsmrCdm::Poll(bridgeTick);
	const bool rampAgentChanged = VsmrRampAgent::Poll(bridgeTick);
	if (vsidChanged || cdmChanged || rampAgentChanged)
		FlightDataRefreshPending.store(true, std::memory_order_release);

	// Refreshing screens after synchronized flight-plan changes
	if (FlightDataRefreshPending.exchange(false, std::memory_order_acq_rel))
	{
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->IsShutdownRequested())
				continue;
			radar->MarkPerformanceRefreshReason(
				VsmrPerformance::FrameRefreshReason::ControllerUpdate);
			radar->RequestRefresh();
		}
	}
	RunDatalinkTimerCycle();

	// ----- Updating runtime insets -----
	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	bool timerAlarmDue = false;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->IsShutdownRequested())
			continue;
		bool refresh = false;
		if (radar->IsAppWindowDisplayed(weatherWindowId))
		{
			QueueWeatherFetch(radar->getActiveAirport());
			refresh = true;
		}
		if (radar->UpdateTimerInsetCountdowns())
		{
			timerAlarmDue = true;
		}
		if (radar->IsAppWindowDisplayed(timerWindowId))
			refresh = true;
		if (refresh)
			radar->RequestRefresh();
	}
	if (timerAlarmDue && !PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		VsmrPluginRuntimeAudio::Play(L"Alarm.wav", "Timer alarm");
	}
};
