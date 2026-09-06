#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginRuntimeAudio.hpp"

#include "aircraft/HoldingPoint.hpp"
#include "crash/CrashRuntime.hpp"
#include "insets/InsetWindow.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "rdf/RdfOverlay.hpp"

#include <atomic>
#include <string>

void CSMRPlugin::OnTimer(int Counter)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnTimer");
	(void)Counter;
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
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
	if (VsmrVsid::Poll(*this))
		FlightDataRefreshPending.store(true, std::memory_order_release);
	if (VsmrCdm::Poll(*this))
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
