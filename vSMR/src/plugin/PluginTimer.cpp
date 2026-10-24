#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"
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
#include "aviso/AvisoSharedConfig.hpp"

#include <atomic>
#include <string>

void CSMRPlugin::SynchronizeProxyRuntime()
{
	std::wstring identity;
	const int connection = GetConnectionType();
	if (connection == CONNECTION_TYPE_VIA_PROXY) DatalinkProxyClient = true;
	else if (connection == CONNECTION_TYPE_DIRECT) DatalinkProxyClient = false;
	if (connection == CONNECTION_TYPE_DIRECT || connection == CONNECTION_TYPE_VIA_PROXY) {
		const auto controller = ControllerMyself();
		if (controller.IsValid()) {
			const char* raw = controller.GetCallsign();
			const std::string callsign = raw ? raw : "";
			if (!callsign.empty()) {
				identity = std::filesystem::u8path(callsign).wstring();
				::CharUpperBuffW(identity.data(), static_cast<DWORD>(identity.size()));
			}
		}
	}
	VsmrPluginBridge::ConfigureProxy(identity);
	ProxyDatalink.Open(identity);
	if (!identity.empty()) ProxyTimers.Join(identity);
	else if (ProxyTimers.Active()) { ProxyTimers.Close(); TimerCountdowns = {}; }

	// Keep a Hoppie lease until old requests have drained after disconnect/error.
	if (DatalinkSessionLease.Owned() && !HoppieConnected.load(std::memory_order_acquire) &&
		!HoppieConnecting.load(std::memory_order_acquire)) {
		const auto workers = GetWorkerQueueSnapshot();
		if (!workers.networkQueued && !workers.networkInFlight) DatalinkSessionLease.Close();
	}
	std::map<std::string, std::string> revisions;
	std::map<std::string, VsmrAvisoSharedConfig::Snapshot> roots;
	for (auto* radar : RadarScreensOpened) {
		if (!radar || radar->IsShutdownRequested()) continue;
		const auto* config = radar->GetConfiguration();
		if (!config) continue;
		const std::string dataPath = radar->GetDataPath();
		const auto& path = config->getConfigPath();
		if (!revisions.count(path)) revisions[path] = config->getPersistedConfigRevision();
		const auto& revision = revisions[path];
		const bool configChanged = !revision.empty() && revision != config->getConfigRevision();
		if (!roots.count(dataPath)) roots.emplace(dataPath,
			VsmrAvisoSharedConfig::Read(std::filesystem::u8path(dataPath)));
		const auto& shared = roots.at(dataPath);
		const auto previous = ProxyRootRevisions.find(dataPath);
		const bool rootChanged = shared.error.empty() && previous != ProxyRootRevisions.end() && previous->second != shared.revision;
		const auto failed = ProxyFailedConfigRevisions.find(path);
		if ((!configChanged && !rootChanged) || (failed != ProxyFailedConfigRevisions.end() && failed->second == revision)) continue;
		if (!radar->ReloadConfig()) {
			if (!config->isConfigHealthy()) ProxyFailedConfigRevisions[path] = revision;
			continue;
		}
		ProxyFailedConfigRevisions.erase(path);
		const auto global = radar->GetGlobalAvisoPalette();
		const char* saved = radar->GetDataFromAsr("AvisoAppearanceGeneration");
		const std::string generation = saved ? saved : "";
		if (!global.first.empty() && generation != global.second) radar->SetAvisoColorPalette(global.first, true);
	}
	for (const auto& root : roots) if (root.second.error.empty()) ProxyRootRevisions[root.first] = root.second.revision;
}

void CSMRPlugin::ChangeTimerCountdown(int durationMinutes, bool reset)
{
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;
	SynchronizeProxyRuntime();
	const bool changed = ProxyTimers.Active() ? ProxyTimers.Change(durationMinutes, reset, ::GetTickCount64(), TimerCountdowns) : reset ? TimerCountdowns.Reset(durationMinutes) :
		TimerCountdowns.Start(durationMinutes, ::GetTickCount64());
	if (!changed)
		return;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar != nullptr && !radar->IsShutdownRequested() &&
			radar->IsAppWindowDisplayed(APPWINDOW_TIMER - APPWINDOW_BASE))
			radar->RequestRefresh();
	}
}

void CSMRPlugin::OnTimer(int Counter)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnTimer");
	(void)Counter;
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (Logger::is_verbose_mode())
		Logger::info(std::string(__FUNCSIG__));
	BLINK = (::GetTickCount64() / 1000U) % 2U != 0; // common phase across same-PC instances
	SynchronizeProxyRuntime();
	SyncPdcLogonWithSelectedAirport();
	VsmrRdf::OnTimer();

	// Consume runway notifications after EuroScope's dialog callback has returned.
	// Coalesce notifications and read the latest committed state for every ASR;
	// never change a view's configured airport to match another open screen.
	if (AirportRunwayRefreshPending.exchange(false, std::memory_order_acq_rel))
	{
		struct ActiveSectorSelectionGuard
		{
			CSMRPlugin* plugin;
			~ActiveSectorSelectionGuard() { plugin->SelectActiveSectorfile(); }
		} selectionGuard{ this };
		Logger::info("Refreshing committed EuroScope airport/runway activity");
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->IsShutdownRequested())
				continue;
			SelectScreenSectorfile(radar);
			radar->RefreshAfterAirportRunwayActivityChange();
		}
	}

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
	VsmrPluginBridge::EndTick(*this);
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
	SynchronizeProxyDatalink();

	// ----- Updating runtime insets -----
	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	bool timerAlarmDue = false;
	if (ProxyTimers.Active()) ProxyTimers.Poll(::GetTickCount64(), TimerCountdowns, timerAlarmDue);
	else timerAlarmDue = TimerCountdowns.Update(::GetTickCount64());
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->IsShutdownRequested())
			continue;
		// Bridge revisions do not change when a TOBT/TSAT threshold is crossed
		// or a rule-driven blink advances. Keep those rules live on both vSMR
		// screens and CoFrance inset hosts through the normal refresh API.
		bool refresh = radar->HasClockSensitiveTagRules();
		if (radar->IsAppWindowDisplayed(weatherWindowId))
		{
			for (const auto& station : radar->GetOpenWeatherAirports()) QueueWeatherFetch(station);
			refresh = true;
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
