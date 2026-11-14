#include <Windows.h>
#include <objidl.h>
#include "StandArrivalTests.hpp"
#include "aircraft/StandArrival.hpp"
#include "aviso/StandHighlight.hpp"

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
