#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "radar/RadarScreen.Registry.hpp"

namespace
{
    std::string Text(const char* value) { return value != nullptr ? value : ""; }
}

void CSMRPlugin::SetStandOptions(bool highlight)
{
    HighlightArrivalStand = highlight;
    SaveDataToSettings("HighlightArrivalStand", "Highlight selected arrival stand on AVISO", highlight ? "1" : "0");
    for (auto* radar : RadarScreensOpened)
        if (radar != nullptr && !radar->IsShutdownRequested()) radar->RequestRefresh();
}

void CSMRPlugin::UpdateArrivalsAtStands()
{
    if (HighlightArrivalStand)
    {
        // EuroScope lists can change ASEL without a radar click or a new report.
        // Observe selection through the existing SDK timer; create no new timer.
        const auto selected = FlightPlanSelectASEL();
        const auto callsign = selected.IsValid() ? Text(selected.GetCallsign()) : std::string{};
        if (callsign != SelectedStandCallsign)
        {
            SelectedStandCallsign = callsign;
            FlightDataRefreshPending.store(true, std::memory_order_release);
        }
    }
}
