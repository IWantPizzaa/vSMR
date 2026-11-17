#include "SmartGroundListTests.hpp"
#include "plugin/SmartGroundListLogic.hpp"
#include "smart_ground/SmartGroundActivation.hpp"
#include "radar/RadarUiSupport.hpp"

std::vector<std::string> RunSmartGroundListTests()
{
    using namespace VsmrSmartGroundList;
    std::vector<std::string> failures;
    const auto check = [&](bool condition, const char* message) { if (!condition) failures.emplace_back(message); };
    using namespace VsmrSmartGround;
    Activation activation;
    check(!activation.IsEnabled(), "Smart Ground is disabled in every new runtime session.");
    check(activation.SetEnabled(true) && activation.IsEnabled() && !activation.SetEnabled(true),
        "Explicit activation enables a session and repeated on commands do not reset it.");
    check(activation.SetEnabled(false) && !activation.IsEnabled(), "Smart Ground can be turned off again.");
    Activation restarted;
    check(!restarted.IsEnabled(), "Activation is not inherited by a new session.");
    check(ParseActivationCommand(".smr sg") == ActivationCommand::Toggle &&
        ParseActivationCommand(".smr smart ground") == ActivationCommand::Toggle,
        "Both normalized command aliases toggle Smart Ground.");
    check(ParseActivationCommand(".smr sg on") == ActivationCommand::Enable &&
        ParseActivationCommand(".smr smart ground off") == ActivationCommand::Disable &&
        ParseActivationCommand(".smr sg status") == ActivationCommand::Status,
        "Explicit on, off and status commands are supported.");
    check(ParseActivationCommand(".smr sgfoo") == ActivationCommand::Unrecognized &&
        ParseActivationCommand(".smr smart groundwork") == ActivationCommand::Unrecognized &&
        ParseActivationCommand(".smr sg on extra") == ActivationCommand::Unrecognized &&
        ParseActivationCommand(".smr reload") == ActivationCommand::Unrecognized,
        "Only exact Smart Ground commands can change activation.");
    check(DisplayRoute("", false) == "No Data" && DisplayRoute("N B3 D C", false) == "No Data",
        "Disabled Route cells show No Data even when an old route is available.");
    check(TAG_ITEM_ROUTE != TAG_ITEM_LANGUAGE && TAG_FUNC_ROUTE_MENU != TAG_FUNC_LANGUAGE_EDIT &&
        TAG_FUNC_ROUTE_MENU != TAG_FUNC_LANGUAGE_SELECT, "Route and Language retain distinct SDK item and function identifiers.");
    check(DisplayRoute("") == " ", "An empty Route cell remains clickable.");
    check(DisplayRoute("N B3 D C") == "N B3 D C", "Short route sequence retains full taxiway names.");
    check(DisplayRoute("N B3 D C Q E1 E2 E3") == "N B3 D C Q...", "Long list route shows an explicit truncation indicator at a taxiway boundary.");
    check(DisplayRoute("VERYLONGTAXIWAYNAME").size() == 15, "Every Route item fits the SDK's fifteen-character payload.");
    Actions actions; PendingAction result;
    const int first = actions.Add("AFR123", Action::Edit);
    const int second = actions.Add("BAW456", Action::Reset);
    check(actions.Take(first, result) && result.callsign == "AFR123" && result.action == Action::Edit,
        "Opening a menu for another selected aircraft does not retarget the first menu callback.");
    check(!actions.Take(first, result), "A route menu callback cannot be applied twice.");
    check(actions.Take(second, result) && result.callsign == "BAW456" && result.action == Action::Reset,
        "A reset remains bound to its original aircraft.");
    const int commit = actions.Add("AFR123", Action::Commit);
    const int other = actions.Add("BAW456", Action::Commit);
    actions.Forget("AFR123");
    check(!actions.Take(commit, result), "Disconnect invalidates a pending route edit before callsign reuse.");
    check(actions.Take(other, result) && result.callsign == "BAW456", "Invalidating one aircraft leaves another route edit intact.");
    const int expired = actions.Add("AFR123", Action::Edit);
    for (int i = 0; i < 130; ++i) actions.Add("BAW456", Action::Close);
    check(!actions.Take(expired, result), "Expired popup callbacks cannot modify a newer selected aircraft.");
    check(actions.Add("", Action::Commit) == 0, "A popup action cannot bind an empty aircraft identity.");
    const int beforeDisable = actions.Add("AFR123", Action::Commit);
    actions.Clear();
    const int afterEnable = actions.Add("AFR123", Action::Commit);
    check(!actions.Take(beforeDisable, result) && afterEnable != beforeDisable && actions.Take(afterEnable, result),
        "Disabling invalidates pending edits and reactivation never reuses their callback IDs.");
    return failures;
}
