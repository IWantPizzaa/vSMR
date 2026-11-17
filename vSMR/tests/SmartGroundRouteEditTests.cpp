#include "SmartGroundRouteEditTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include <fstream>
#include <iomanip>
#include <array>

namespace
{
std::filesystem::path WriteEditFixture(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    const auto dir = root / "smart-ground" / "tmp" / "native_route_edit_fixture"; std::filesystem::create_directories(dir);
    std::ofstream graph(dir / "lfpg.json"); graph << std::setprecision(16) << "{\"nodes\":[";
    const std::array<Point, 9> points{ Point{0,0}, Point{100,0}, Point{200,0}, Point{300,0}, Point{400,0}, Point{100,100}, Point{200,100}, Point{200,150}, Point{300,150} };
    const std::array<const char*, 9> ids{ "A", "B", "C", "D", "E", "U", "V", "W", "Z" };
    for (std::size_t i = 0; i < points.size(); ++i) { if (i) graph << ','; const auto geo = Geographic(points[i]); graph << "{\"id\":\"" << ids[i] << "\",\"x\":" << points[i].x << ",\"y\":" << points[i].y << ",\"lat\":" << geo.latitude << ",\"lon\":" << geo.longitude << '}'; }
    graph << R"(],"edges":[{"id":"AB","from":"A","to":"B","length":100,"name":"N","kind":"taxiway"},{"id":"BC","from":"B","to":"C","length":100,"name":"B3","kind":"taxiway"},{"id":"CD","from":"C","to":"D","length":100,"name":"D","kind":"taxiway"},{"id":"DE","from":"D","to":"E","length":100,"name":"C","kind":"taxiway"},{"id":"BU","from":"B","to":"U","length":100,"name":"X","kind":"taxiway"},{"id":"UV","from":"U","to":"V","length":100,"name":"Y","kind":"taxiway"},{"id":"VC","from":"V","to":"C","length":100,"name":"Z","kind":"taxiway"},{"id":"UW","from":"U","to":"W","length":111.803398875,"name":"P","kind":"taxiway"},{"id":"WV","from":"W","to":"V","length":50,"name":"Q","kind":"taxiway"},{"id":"WZ","from":"W","to":"Z","length":100,"name":"S","kind":"taxiway"},{"id":"ZD","from":"Z","to":"D","length":150,"name":"J","kind":"taxiway"}],"locations":[{"id":"stand:A","nodeId":"A","kind":"stand"},{"id":"operation:departure:08L:T3","nodeId":"E"},{"id":"operation:departure:08L:T4","nodeId":"D"}]})";
    std::ofstream(dir / "lfpg_operations.json") << R"({"airport":"LFPG","runwayAccess":[{"locationId":"operation:departure:08L:T3","runway":"08L","role":"departure"},{"locationId":"operation:departure:08L:T4","runway":"08L","role":"departure"}]})";
    std::ofstream(dir / "lfpg_corridors.json") << R"({"airport":"LFPG"})";
    std::ofstream(dir / "lfpg_aircraft_restrictions.json") << R"({"airport":"LFPG"})";
    return dir;
}
}
std::vector<std::string> RunSmartGroundRouteEditTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    std::vector<std::string> failures;
    const auto expect = [&](bool condition, const std::string& message) { if (!condition) failures.push_back("Route editing: " + message); };
    Path standardColors; standardColors.edges = {0,1,2,3,4}; standardColors.nodes = {0,1,2,3,4,5};
    Path loopingColors; loopingColors.controller = true; loopingColors.edges = {0,5,2,6,1,2,3,4}; loopingColors.nodes = {0,1,2,3,1,2,3,4,5};
    MarkControllerSegments(loopingColors, standardColors);
    expect(loopingColors.controllerSegments == std::vector<bool>({false,true,true,true,false,false,false,false}), "early touch of an official edge does not consume its later official suffix occurrence");
    const auto dir = WriteEditFixture(root); std::string error;
    Context context; context.configuration = "east"; context.movement = "departure"; context.standId = "A"; context.runway = "08L"; context.holdingPointId = "T3"; context.profile = { "A320", 35.8, 37.57 };
    Engine engine; expect(engine.Load(dir.u8string(), error), "load fixture");
    const auto sample = [](std::string callsign, Point point, double time, std::uint64_t sequence) {
        const auto geo = Geographic(point); Sample result; result.callsign = std::move(callsign); result.sessionId = "edit"; result.sequence = sequence; result.receivedAt = time; result.latitude = geo.latitude; result.longitude = geo.longitude; result.altitudeFeet = 350; result.groundSpeedKnots = 10; return result;
    };
    auto gate = sample("EDIT", {0,0}, 100, 1); gate.groundSpeedKnots = 0; engine.SetContext("EDIT", context); engine.Observe(gate);
    expect(engine.GetSnapshot("EDIT", 100).taxiwayRoute == "N B3 D C", "list reports official taxiways in order");
    expect(engine.EditRoute("EDIT", "C", "U", error), "drag assigns legal alternate branch: " + error);
    const auto edited = engine.GetSnapshot("EDIT", 100); bool green = false, blue = false;
    for (const auto& point : edited.remainingRoute) if (!point.nodeId.empty()) { green = green || !point.controllerSegment; blue = blue || point.controllerSegment; }
    expect(green && blue, "only changed route portions become blue");
    for (const auto& point : edited.remainingRoute) if (point.nodeId == "B" || point.nodeId == "D" || point.nodeId == "E") expect(!point.controllerSegment, "shared directed official prefix/suffix remain green");
    expect(engine.SetTaxiwayRoute("EDIT", "n x y z d c", error), "manual taxiway route is case insensitive and ordered: " + error);
    expect(engine.GetSnapshot("EDIT", 100).taxiwayRoute == "N X Y Z D C", "manual sequence visits every requested taxiway in order");
    const auto saved = engine.GetSnapshot("EDIT", 100).taxiwayRoute;
    expect(!engine.SetTaxiwayRoute("EDIT", "N UNKNOWN C", error), "unknown manual taxiway rejected");
    expect(engine.GetSnapshot("EDIT", 100).taxiwayRoute == saved, "invalid manual route is atomic");
    expect(!engine.SetTaxiwayRoute("EDIT", "C N", error), "impossible route order cannot silently skip a taxiway");
    expect(engine.ResetRoute("EDIT", error), "reset restores coherent official route: " + error);
    const auto reset = engine.GetSnapshot("EDIT", 100); expect(reset.taxiwayRoute == "N B3 D C" && !reset.controllerRoute, "reset removes manual assignment");
    for (const auto& point : reset.remainingRoute) expect(!point.controllerSegment, "reset official route is green");

    const auto deviate = [&](const std::string& callsign, double start) {
        engine.SetContext(callsign, context);
        engine.Observe(sample(callsign, {70,0}, start, 1));
        engine.Observe(sample(callsign, {100,22}, start + 5, 2));
        engine.Observe(sample(callsign, {100,48}, start + 10, 3));
    };
    deviate("DEV", 200); deviate("OTHER", 200);
    expect(engine.GetSnapshot("DEV", 210).offTrackAlert, "deviation fixture confirms off track");
    engine.DisregardOffTrack("DEV", false);
    expect(!engine.GetSnapshot("DEV", 210).offTrackAlert && engine.GetSnapshot("DEV", 210).offTrackDeviation, "disregard hides only the episode alert, retaining actual deviation");
    expect(!engine.GetSnapshot("DEV", 210).suspectedOffTrack, "ignored confirmed deviation is not reclassified as merely suspected");
    expect(engine.GetSnapshot("OTHER", 210).offTrackAlert, "disregard does not silence another aircraft");
    const auto recovery = engine.GetSnapshot("DEV", 210); bool editableRecovery = false;
    for (const auto& point : recovery.remainingRoute) editableRecovery = editableRecovery || point.editable;
    expect(editableRecovery, "off-track recovery has editable future handles");
    expect(engine.EditRoute("DEV", "V", "W", error), "off-track displayed recovery anchor accepts drag: " + error);
    const auto accepted = engine.GetSnapshot("DEV", 210);
    expect(!accepted.offTrackAlert && !accepted.offTrackDeviation && !accepted.suspectedOffTrack, "accepted reroute clears actual off-track and all old evidence");
    if (!accepted.remainingRoute.empty()) expect(Distance(Project(accepted.remainingRoute.front().latitude, accepted.remainingRoute.front().longitude), {100,48}) < .001, "accepted off-track route begins at observed position");
    expect(engine.SetTaxiwayRoute("OTHER", "x p s j c", error), "manual edit works while off-track: " + error);
    expect(!engine.GetSnapshot("OTHER", 210).offTrackDeviation, "manual edit also clears off-track");

    deviate("PERM", 300); engine.DisregardOffTrack("PERM", true);
    expect(!engine.GetSnapshot("PERM", 310).offTrackAlert, "permanent disregard suppresses the current aircraft");
    auto reconnect = sample("PERM", {70,0}, 320, 1); reconnect.sessionId = "new-session"; engine.Observe(reconnect);
    auto next = sample("PERM", {100,22}, 325, 2); next.sessionId = "new-session"; engine.Observe(next); next = sample("PERM", {100,48}, 330, 3); next.sessionId = "new-session"; engine.Observe(next);
    expect(engine.GetSnapshot("PERM", 330).offTrackAlert, "new aircraft session does not inherit permanent disregard");

    const auto rejoinAndDeviate = [&](const std::string& callsign, double time, bool permanent) {
        deviate(callsign, time); engine.DisregardOffTrack(callsign, permanent);
        const std::array<Point, 10> rejoin{ Point{122,100}, Point{147,100}, Point{172,100}, Point{197,100}, Point{200,78}, Point{200,53}, Point{200,28}, Point{200,3}, Point{222,0}, Point{247,0} };
        for (std::size_t i = 0; i < rejoin.size(); ++i) engine.Observe(sample(callsign, rejoin[i], time + 15 + i * 5, i + 4));
        expect(!engine.GetSnapshot(callsign, time + 60).offTrackDeviation, "two correct route reports finish the ignored episode");
        engine.Observe(sample(callsign, {175,0}, time + 65, 14)); engine.Observe(sample(callsign, {150,0}, time + 70, 15));
        const auto again = engine.GetSnapshot(callsign, time + 70);
        expect(again.offTrackDeviation && again.offTrackAlert != permanent, permanent ? "permanent disregard survives subsequent deviation episodes" : "one-time disregard re-arms after rejoining the route");
    };
    rejoinAndDeviate("EPISODE", 500, false); rejoinAndDeviate("PERMANENT", 500, true);

    // A named shortcut is not an authorized substitute between requested
    // taxiways, even if it could eventually visit all requested names.
    expect(!engine.SetTaxiwayRoute("EDIT", "n d c", error), "manual sequence cannot silently insert unlisted B3 between N and D");

    engine.SetContext("INSTRUCTIONS", context); auto instructedGate = sample("INSTRUCTIONS", {0,0}, 800, 1); instructedGate.groundSpeedKnots = 0; engine.Observe(instructedGate);
    expect(engine.SetTaxiwayRoute("INSTRUCTIONS", "N B3 D C", error), "explicit route instructions assigned");
    engine.Observe(sample("INSTRUCTIONS", {70,0}, 805, 2)); engine.Observe(sample("INSTRUCTIONS", {100,22}, 810, 3)); engine.Observe(sample("INSTRUCTIONS", {100,48}, 815, 4));
    const auto retainedInstructions = engine.GetSnapshot("INSTRUCTIONS", 815);
    expect(retainedInstructions.offTrackDeviation && !retainedInstructions.recoveryProposed, "recovery cannot silently omit still-required B3 after a wrong branch");
    expect(retainedInstructions.taxiwayRoute == "N B3 D C", "off-track list retains assigned taxiway instructions");

    engine.SetContext("MIDRESET", context); auto midGate = sample("MIDRESET", {0,0}, 900, 1); midGate.groundSpeedKnots = 0; engine.Observe(midGate);
    expect(engine.SetTaxiwayRoute("MIDRESET", "N X Y Z D C", error), "assign manual branch before mid-route reset");
    engine.Observe(sample("MIDRESET", {70,0}, 905, 2)); engine.Observe(sample("MIDRESET", {100,22}, 910, 3)); engine.Observe(sample("MIDRESET", {100,48}, 915, 4));
    expect(engine.ResetRoute("MIDRESET", error), "reset after entering edited branch succeeds from current legal arc: " + error);
    const auto midReset = engine.GetSnapshot("MIDRESET", 915);
    expect(!midReset.controllerRoute && !midReset.remainingRoute.empty() && Distance(Project(midReset.remainingRoute.front().latitude, midReset.remainingRoute.front().longitude), {100,48}) < .001, "mid-route reset preserves physical position instead of historical gate progress");

    Engine changed; expect(changed.Load(dir.u8string(), error), "load holding-point fixture"); changed.SetContext("HP", context);
    changed.Observe(sample("HP", {150,0}, 400, 1));
    Context newHold = context; newHold.holdingPointId = "T4"; changed.SetContext("HP", newHold);
    const auto hp = changed.GetSnapshot("HP", 400);
    expect(hp.routeAvailable && hp.remainingRoute.back().nodeId == "D", "existing holding point change recalculates the destination");
    if (!hp.remainingRoute.empty()) expect(Distance(Project(hp.remainingRoute.front().latitude, hp.remainingRoute.front().longitude), {150,0}) < .001, "holding-point change retains current observed origin");
    expect(!hp.offTrackDeviation && hp.reportAgeSeconds == 0, "holding-point intent change neither triggers off-track nor fabricates observation time");
    Engine ambiguous; expect(ambiguous.Load(dir.u8string(), error), "load ambiguous holding-point fixture"); ambiguous.SetContext("AMB", context);
    auto atFork = sample("AMB", {100,0}, 700, 1); atFork.groundSpeedKnots = 0; ambiguous.Observe(atFork); ambiguous.SetContext("AMB", newHold);
    const auto waiting = ambiguous.GetSnapshot("AMB", 700);
    expect(!waiting.routeAvailable && waiting.routeMessage.find("Waiting") != std::string::npos, "ambiguous HP change never falls back to the historical gate");
    return failures;
}
