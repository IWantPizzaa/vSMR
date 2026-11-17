#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/SmartGroundListLogic.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "smart_ground/SmartGroundEngine.hpp"
#include "integrations/SmartGroundIntegration.hpp"
#include "shared/TextUtils.hpp"

namespace
{
    double RouteNow() { return static_cast<double>(::GetTickCount64()) / 1000.0; }
}

void CSMRPlugin::ForgetSmartGroundRouteMenus(const std::string& callsign)
{
    std::lock_guard<std::mutex> guard(SmartGroundRouteActionsMutex);
    SmartGroundRouteActions.Forget(callsign);
}

bool CSMRPlugin::HandleSmartGroundRouteFunctionCall(int functionId, const char* itemString, RECT area)
{
    using namespace VsmrSmartGroundList;
    const auto refresh = [&]() {
        FlightDataRefreshPending.store(true, std::memory_order_release);
        for (CSMRRadar* radar : RadarScreensOpened)
            if (radar != nullptr && !radar->IsShutdownRequested()) radar->RequestRefresh();
    };
    const auto add = [this](const std::string& callsign, Action action) {
        std::lock_guard<std::mutex> guard(SmartGroundRouteActionsMutex);
        return SmartGroundRouteActions.Add(callsign, action);
    };
    if (functionId == TAG_FUNC_ROUTE_MENU)
    {
        if (!VsmrSmartGroundBridge::IsEnabled()) return true;
        const auto flightPlan = FlightPlanSelectASEL();
        if (!flightPlan.IsValid()) return true;
        const char* raw = flightPlan.GetCallsign();
        const std::string callsign = raw ? ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(raw)) : "";
        if (callsign.empty()) return true;
        const std::string title = "Route " + callsign;
        OpenPopupList(area, title.c_str(), 1);
        AddPopupListElement("Edit manually", "", add(callsign, Action::Edit), false);
        AddPopupListElement("Reset route", "", add(callsign, Action::Reset), false);
        AddPopupListElement("Close", "", add(callsign, Action::Close), false);
        return true;
    }
    PendingAction pending;
    {
        std::lock_guard<std::mutex> guard(SmartGroundRouteActionsMutex);
        if (!SmartGroundRouteActions.Take(functionId, pending)) return false;
    }
    if (!VsmrSmartGroundBridge::IsEnabled() || pending.action == Action::Close) return true;
    // The initial click captured this aircraft; a later ASEL change must not
    // redirect either the popup input or a reset to another aircraft.
    const auto flightPlan = FlightPlanSelect(pending.callsign.c_str());
    if (!flightPlan.IsValid()) return true;
    if (pending.action == Action::Edit)
    {
        const auto snapshot = VsmrSmartGround::Service().GetSnapshot(pending.callsign, RouteNow());
        OpenPopupEdit(area, add(pending.callsign, Action::Commit), snapshot.taxiwayRoute.c_str());
        return true;
    }
    std::string error;
    const bool success = pending.action == Action::Reset
        ? VsmrSmartGround::Service().ResetRoute(pending.callsign, error)
        : VsmrSmartGround::Service().SetTaxiwayRoute(pending.callsign, itemString ? itemString : "", error);
    if (!success)
        DisplayUserMessage("vSMR", "Route", error.empty() ? "No legal taxi route is available." : error.c_str(), true, true, false, false, false);
    else refresh();
    return true;
}
