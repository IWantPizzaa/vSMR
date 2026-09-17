#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "crash/CrashRuntime.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"

#include <atomic>

void CSMRPlugin::RefreshControllerDependentOverlays()
{
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->IsShutdownRequested())
			continue;
		radar->MarkPerformanceRefreshReason(
			VsmrPerformance::FrameRefreshReason::ControllerUpdate);
		radar->RequestRefresh();
	}
}

void CSMRPlugin::OnControllerPositionUpdate(CController Controller)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnControllerPositionUpdate");
	(void)Controller;
	RefreshControllerDependentOverlays();
}

void CSMRPlugin::OnControllerDisconnect(CController Controller)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnControllerDisconnect");
	(void)Controller;
	RefreshControllerDependentOverlays();
}

void CSMRPlugin::OnAirportRunwayActivityChanged()
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnAirportRunwayActivityChanged");
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	Logger::info("EuroScope airport/runway activity changed");
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->IsShutdownRequested())
			continue;

		// EuroScope exposes airport/runway activity as read-only sector data.
		// The ASR/runtime airport remains authoritative even when another airport
		// is the only one with selected runways.
		SelectScreenSectorfile(radar);
		radar->RefreshAfterAirportRunwayActivityChange();
	}

	// Leave the plug-in enumeration source in EuroScope's normal active-file
	// state for callbacks that are not associated with a particular screen.
	SelectActiveSectorfile();
}
