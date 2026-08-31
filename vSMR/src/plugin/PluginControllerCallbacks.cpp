#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "crash/CrashRuntime.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <set>
#include <string>
#include <vector>

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

		// Radar screens may use different sector sources. Resolve the active
		// airport set independently for each one so a unique airport from another
		// screen can never replace this screen's surface airport.
		std::set<std::string> activeAirports;
		SelectScreenSectorfile(radar);
		CSectorElement airport;
		for (airport = SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
			airport.IsValid();
			airport = SectorFileElementSelectNext(airport, SECTOR_ELEMENT_AIRPORT))
		{
			const char* name = airport.GetName();
			if (name == nullptr || name[0] == '\0' ||
				(!airport.IsElementActive(true, 0) && !airport.IsElementActive(false, 0)))
			{
				continue;
			}
			std::string normalized(name);
			std::transform(
				normalized.begin(), normalized.end(), normalized.begin(),
				[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
			activeAirports.insert(normalized);
		}

		const std::string radarAirport = radar->getActiveAirport();
		const bool radarAirportStillActive = std::any_of(
			activeAirports.begin(), activeAirports.end(),
			[&](const std::string& candidate)
			{
				return _stricmp(candidate.c_str(), radarAirport.c_str()) == 0;
			});
		const bool adoptAirport = !radarAirportStillActive && activeAirports.size() == 1;
		if (adoptAirport)
			radar->setActiveAirport(*activeAirports.begin(), true, false);

		// EuroScope exposes airport/runway activity as read-only sector data.
		// Select this screen's sector source explicitly, then invalidate the
		// cached activity snapshot so map rules, RIMCAS and every inset repaint
		// from the choices that were just accepted in EuroScope's dialog.
		SelectScreenSectorfile(radar);
		radar->RefreshAfterAirportRunwayActivityChange(adoptAirport);
	}

	// Leave the plug-in enumeration source in EuroScope's normal active-file
	// state for callbacks that are not associated with a particular screen.
	SelectActiveSectorfile();
}
