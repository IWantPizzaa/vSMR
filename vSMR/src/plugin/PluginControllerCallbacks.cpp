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
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnControllerPositionUpdate");
	(void)Controller;
	RefreshControllerDependentOverlays();
}

void CSMRPlugin::OnControllerDisconnect(CController Controller)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnControllerDisconnect");
	(void)Controller;
	RefreshControllerDependentOverlays();
}

void CSMRPlugin::OnAirportRunwayActivityChanged()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnAirportRunwayActivityChanged");
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	// EuroScope invokes this while closing its runway activity dialog with OK.
	// Do not switch sector sources, enumerate partially committed selections or
	// re-enter rendering/UI synchronization from the host's save callback.
	AirportRunwayRefreshPending.store(true, std::memory_order_release);
}
