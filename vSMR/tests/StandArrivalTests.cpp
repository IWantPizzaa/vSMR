#include <Windows.h>
#include <objidl.h>
#include "StandArrivalTests.hpp"
#include "aircraft/StandArrival.hpp"
#include "aviso/StandHighlight.hpp"

namespace
{
    struct AssignedData
    {
        int speed = 0;
        std::string scratchpad = "KEEP", status = "TXIN";
        bool rejectSpeed = false, rejectStatus = false, rejectRestore = false;
        int GetAssignedSpeed() const { return speed; }
        const char* GetScratchPadString() const { return scratchpad.c_str(); }
        bool SetAssignedSpeed(int value) { if (rejectSpeed) return false; speed = value; return true; }
        bool SetScratchPadString(const char* value)
        {
            const std::string text = value;
            if (text == "NSTS" && rejectStatus) return false;
            if (text == "KEEP" && rejectRestore) { rejectRestore = false; return false; }
            if (text == "NSTS" || text == "TXIN") status = text;
            scratchpad = text;
            return true;
        }
    };
}
std::vector<std::string> RunStandArrivalTests()
{
    using namespace VsmrStand;
    std::vector<std::string> failures;
    const auto check = [&](bool ok, const char* text) { if (!ok) failures.emplace_back(text); };
    check(Key(" lfpg ", " b-008 ") == "LFPG:B8" && Key("LFMN", "08A") == "LFMN:8A", "Stand aliases normalize case and zero padding");
    check(Key("LFPG", "B08/B09").empty() && Key("../PG", "B8").empty(), "Ambiguous assignments and invalid airports do not match labels");
    check(IsStandLabel("Gates") && !IsStandLabel("Taxiways") && !IsStandLabel("Terminaux"), "A taxiway sharing a stand name is never highlighted");
    check(IsArrival("LFPG", "LFMN", "lfpg") && !IsArrival("LFPG", "LFPG", "LFMN") &&
        !IsArrival("LFPG", "LFPG", "LFPG"), "Arrival role excludes departures and local flights");
    check(CanResetStatus("TXIN") && CanResetStatus("ARR") && !CanResetStatus("NSTS") &&
        !CanResetStatus("PUSH") && !CanResetStatus("STUP") && CanResetStatus("DEPA"), "Arrivals may clear a leftover DEPA while deliberate push/startup choices remain protected");
    std::vector<Zone> zones;
    constexpr auto json = R"({"B08":{"Coordinates":"49.005535:2.562912:20"},"M09":{"Coordinates":"48.999940:2.542992:30"},"bad":{"Coordinates":"NaN:2:20"}})";
    check(ParseZones(json, zones) && zones.size() == 2U, "RampAgent stand coordinates and radii parse, invalid zones are excluded");
    check(ContainingZone(zones, 49.005535, 2.562912, "B8") == "B8", "B08 arrival is inside the actual RampAgent zone");
    check(ContainingZone(zones, 49.005383333, 2.562847222, "B08") == "B8", "AVISO stand text can be offset from the real stop position");
    check(ContainingZone(zones, 49.005535, 2.562912, "M09").empty() &&
        ContainingZone(zones, 49.006, 2.562912, "B08").empty(), "Wrong stand and adjacent taxiway cannot complete an assigned arrival");
    check(ContainingZone(zones, 48.999940, 2.542992, "") == "M9", "An unassigned arrival can use a known occupied stand zone");
    check(!ParseZones(R"({"X":{"Coordinates":"49:2:99999"}})", zones) &&
        !ParseZones(R"({"B08":{"Coordinates":"49:2:20"},"B8":{"Coordinates":"49:2:20"}})", zones) &&
        !ParseZones(std::string(MaximumZoneResponseBytes+1U, ' '), zones), "Invalid sizes, radii and duplicate identities fail closed");

    ArrivalTracker tracker;
    Observation sample; sample.flightIdentity = "LFMN|LFPG|1200"; sample.zone = "B8"; sample.eligible = true;
    sample.report = 1; sample.nowMilliseconds = 100000;
    check(!tracker.Update("TEST", sample), "One stopped report does not reset a status");
    sample.nowMilliseconds += 5000;
    check(!tracker.Update("TEST", sample), "Repeated timer reads do not become new position reports");
    sample.report = 2;
    check(!tracker.Update("TEST", sample), "A short stop in a stand is not enough");
    sample.report = 3; sample.nowMilliseconds += 5000;
    check(tracker.Update("TEST", sample), "Three five-second reports confirm the aircraft stopped at its gate");
    tracker.Complete("TEST"); sample.report = 4; sample.nowMilliseconds += 5000;
    check(!tracker.Update("TEST", sample), "Completed arrivals are not continually rewritten");
    sample.flightIdentity = "LFPG|LFMN|1300"; sample.eligible = false;
    check(!tracker.Update("TEST", sample), "A turnaround with the same callsign is independent of the arrival");
    tracker.Clear(); sample.eligible = true; sample.flightIdentity = "LFMN|LFPG|1200";
    for (int i = 0; i < 4; ++i) { ++sample.report; sample.nowMilliseconds += 5000; sample.speedKnots = 5; check(!tracker.Update("TEST", sample), "Taxiing through a stand cannot clear its status"); }
    sample.speedKnots = 0; ++sample.report; sample.nowMilliseconds += 5000;
    check(!tracker.Update("TEST", sample), "Stopping starts a new confirmation period");
    sample.ageSeconds = 20; sample.nowMilliseconds += 30000;
    check(!tracker.Update("TEST", sample), "A paused replay or stale radar report cannot confirm arrival");
    sample.ageSeconds = 0; ++sample.report; sample.nowMilliseconds += 5000;
    check(!tracker.Update("TEST", sample), "Reports after a pause restart arrival confirmation");
    ++sample.report; sample.nowMilliseconds += 5000; sample.zone = "M9";
    check(!tracker.Update("TEST", sample), "A changed assigned or occupied stand restarts confirmation");
    tracker.Forget("TEST"); ++sample.report; sample.nowMilliseconds += 5000;
    check(!tracker.Update("TEST", sample), "Disconnecting forgets a previous flight's gate progress");

    for (int speed : {0, 10, 250})
    {
        AssignedData data; data.speed = speed;
        check(ResetToNoStatus(data, "TXIN") && data.status == "NSTS" && data.scratchpad == "KEEP" &&
            data.speed == (speed == 10 ? 0 : speed), "No Status preserves scratchpad and real speed, clears only a shared marker");
    }
    AssignedData rejected; rejected.rejectStatus = true; rejected.speed = 10;
    check(!ResetToNoStatus(rejected, "TXIN") && rejected.speed == 10 && rejected.scratchpad == "KEEP", "A rejected status write restores its shared speed");
    AssignedData restore; restore.rejectRestore = true; restore.speed = 10;
    check(!ResetToNoStatus(restore, "TXIN") && restore.status == "TXIN" && restore.scratchpad == "KEEP" && restore.speed == 10,
        "A rejected scratchpad restoration rolls back the status and shared speed");
    VsmrRadarTypes::AvisoLabel gate, taxiway;
    gate.standKey = Key("LFPG", "B08");
    VsmrRadarTypes::AvisoRasterRenderRequest request; request.highlightedStand = Key("LFPG", "B8");
    check(VsmrAviso::StandLabelTextColor(gate, request).GetValue() != gate.textColor.GetValue() &&
        VsmrAviso::StandLabelTextColor(taxiway, request).GetValue() == taxiway.textColor.GetValue(), "Selection recolors only matching stand text");
    request.highlightedStand = Key("LFMN", "B08");
    check(VsmrAviso::StandLabelTextColor(gate, request).GetValue() == gate.textColor.GetValue(), "A selection at another airport cannot highlight the same gate name");
    request.highlightedStand.clear();
    check(VsmrAviso::StandLabelTextColor(gate, request).GetValue() == gate.textColor.GetValue(), "Clearing selection or disabling the option restores the original color");
    return failures;
}
