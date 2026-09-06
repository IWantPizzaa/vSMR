#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "plugin/PluginCommandHandler.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "shared/TextUtils.hpp"

#include <atomic>
#include <memory>
#include <string>

bool CSMRPlugin::OnCompileCommand(const char * sCommandLine) {
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnCompileCommand");
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return false;
	return VsmrPluginCommandHandler::Handle(
		*this,
		sCommandLine,
		RadarScreensOpened);
}


void CSMRPlugin::OnFunctionCall(
	int FunctionId,
	const char* sItemString,
	POINT Pt,
	RECT Area)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnFunctionCall");
	(void)Pt;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (HandleHoldingPointFunctionCall(FunctionId, sItemString, Area))
		return;
	HandleDatalinkFunctionCall(FunctionId, sItemString, Area);
}

void CSMRPlugin::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnFlightPlanDisconnect");
	Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (!FlightPlan.IsValid())
		return;

	const char* callsign = FlightPlan.GetCallsign();
	if (callsign == nullptr || callsign[0] == '\0')
		return;
	const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	if (normalizedCallsign.empty())
		return;
	VsmrGroundState::ClearLineupOverride(normalizedCallsign.c_str());
	VsmrHoldingPoint::ForgetPending(normalizedCallsign);
	VsmrVsid::ForgetAircraft(normalizedCallsign);

	ForgetDatalinkFlightPlan(normalizedCallsign);
}

void CSMRPlugin::OnFlightPlanControllerAssignedDataUpdate(CFlightPlan FlightPlan, int DataType)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnFlightPlanControllerAssignedDataUpdate");
	(void)FlightPlan;
	if (DataType != CTR_DATA_TYPE_SCRATCH_PAD_STRING ||
		PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		return;
	}
	// EuroScope may dispatch synchronized scratchpad updates in bursts and may
	// invoke this callback while it still owns internal flight-plan state. Never
	// enter radar rendering from here. The normal UI timer consumes this flag and
	// coalesces any number of updates into one refresh for each open screen.
	FlightDataRefreshPending.store(true, std::memory_order_release);
}

void CSMRPlugin::OnFlightPlanFlightPlanDataUpdate(CFlightPlan FlightPlan)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnFlightPlanFlightPlanDataUpdate");
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (FlightPlan.IsValid())
	{
		const char* callsign = FlightPlan.GetCallsign();
		const char* remarks = FlightPlan.GetFlightPlanData().GetRemarks();
		(void)VsmrHoldingPoint::Resolve(
			callsign != nullptr ? callsign : "",
			remarks != nullptr ? remarks : "");
	}

	// Flight-plan amendments are synchronized between controllers. Defer the
	// visual refresh to the timer so a burst of server updates never re-enters
	// radar rendering from inside EuroScope's flight-plan callback.
	FlightDataRefreshPending.store(true, std::memory_order_release);
}

CRadarScreen * CSMRPlugin::OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnRadarScreenCreated");
	(void)NeedRadarContent;
	(void)GeoReferenced;
	(void)CanBeSaved;
	(void)CanBeCreated;
	Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return NULL;

	if (sDisplayName != nullptr && !strcmp(sDisplayName, VsmrPluginAvisoDisplayName))
	{
		try
		{
			// Keep ownership local until the screen is published in the registry.
			// This also tears down a partially registered screen if allocation fails.
			std::unique_ptr<CSMRRadar> radar = std::make_unique<CSMRRadar>();
			RadarScreensOpened.push_back(radar.get());
			return radar.release();
		}
		catch (...)
		{
			// Never let construction or registry allocation unwind into EuroScope.
			VsmrCrashReporter::RecordState("radar creation", "failed");
			return NULL;
		}
	}

	return NULL;
}
