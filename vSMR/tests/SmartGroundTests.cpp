#include "SmartGroundTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include "shared/JsonDocument.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace
{
std::filesystem::path WriteTrackingFixture(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    const auto directory = root / "smart-ground" / "tmp" / "native_tracking_fixture";
    std::filesystem::create_directories(directory);
    std::ofstream graph(directory / "lfpg.json", std::ios::binary);
    graph << std::setprecision(16) << "{\"nodes\":[";
    const std::array<Point, 6> points{ Point{0,0}, Point{100,0}, Point{200,0}, Point{300,0}, Point{100,100}, Point{200,100} };
    const std::array<const char*, 6> ids{ "A", "B", "C", "D", "U", "V" };
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i) graph << ',';
        const RoutePoint geographic = Geographic(points[i]);
        graph << "{\"id\":\"" << ids[i] << "\",\"x\":" << points[i].x << ",\"y\":" << points[i].y << ",\"lat\":" << geographic.latitude << ",\"lon\":" << geographic.longitude << '}';
    }
    graph << R"(],"edges":[{"id":"AB","from":"A","to":"B","length":100,"kind":"taxiway"},{"id":"BC","from":"B","to":"C","length":100,"kind":"taxiway"},{"id":"CD","from":"C","to":"D","length":100,"kind":"taxiway"},{"id":"BU","from":"B","to":"U","length":100,"kind":"taxiway"},{"id":"UV","from":"U","to":"V","length":100,"kind":"taxiway"},{"id":"VC","from":"V","to":"C","length":100,"kind":"taxiway"}],"locations":[{"id":"stand:A","nodeId":"A","kind":"stand"},{"id":"operation:departure:08L:D","nodeId":"D"}]})";
    std::ofstream(directory / "lfpg_operations.json", std::ios::binary) << R"({"airport":"LFPG","runwayAccess":[{"locationId":"operation:departure:08L:D","runway":"08L","role":"departure"}]})";
    std::ofstream(directory / "lfpg_corridors.json", std::ios::binary) << R"({"airport":"LFPG"})";
    std::ofstream(directory / "lfpg_aircraft_restrictions.json", std::ios::binary) << R"({"airport":"LFPG"})";
    return directory;
}
}

std::vector<std::string> RunSmartGroundTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    std::vector<std::string> failures;
    const auto expect = [&](bool condition, const std::string& message) { if (!condition) failures.push_back("Smart Ground: " + message); };
    Engine engine; std::string error;
    const auto dataRoot = root / "vSMR" / "data" / "SmartGround";
    expect(engine.Load(dataRoot.u8string(), error), "load reviewed native data: " + error);
    if (!engine.IsReady()) return failures;
    std::ifstream input(root / "vSMR" / "tests" / "fixtures" / "smart_ground_routes.json", std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    rapidjson::Document vectors; VsmrJson::ParseDocument(vectors, json);
    expect(!vectors.HasParseError() && vectors.IsObject() && vectors.HasMember("vectors") && vectors["vectors"].IsArray(), "read TypeScript route parity vectors");
    if (vectors.HasParseError() || !vectors.IsObject() || !vectors.HasMember("vectors") || !vectors["vectors"].IsArray()) return failures;
    for (const auto& vector : vectors["vectors"].GetArray()) {
        Context context; context.configuration = vector["configuration"].GetString(); context.movement = vector["movement"].GetString(); context.runway = vector["runway"].GetString();
        context.profile.typeCode = vector["profile"]["typeCode"].GetString(); context.profile.wingspanMetres = vector["profile"]["wingspanMetres"].GetDouble();
        const std::string from = vector["origin"].GetString(), to = vector["destination"].GetString();
        const auto result = engine.FindRoute(from, to, context);
        expect(result.available, from + " to " + to + " exists for " + context.profile.typeCode);
        const double reference = vector["distance"].GetDouble();
        if (result.available) {
            std::ostringstream message; message << from << " to " << to << " native distance " << result.distanceMetres << " matches TS " << reference;
            expect(std::abs(result.distanceMetres - reference) < .01, message.str());
            expect(result.edgeIds.size() == vector["edgeIds"].Size(), from + " has the same source-edge count as TS");
            const bool identical = result.edgeIds.size() == vector["edgeIds"].Size() && std::equal(result.edgeIds.begin(), result.edgeIds.end(), vector["edgeIds"].Begin(), [](const std::string& a, const rapidjson::Value& b) { return a == b.GetString(); });
            expect(identical, from + " follows the exact TS source-edge sequence");
        }
    }
    Data data; try { data.Load(dataRoot.u8string()); } catch (const std::exception& e) { failures.push_back(e.what()); return failures; }
    Context context; context.configuration = "east"; context.movement = "departure"; context.standId = "M09"; context.runway = "08L"; context.holdingPointId = "T3"; context.profile = { "A320", 35.8, 37.57 };
    engine.SetContext("SGTEST1", context);
    const auto stand = data.locations.find("stand:M09"); expect(stand != data.locations.end(), "M09 is connected"); if (stand == data.locations.end()) return failures;
    Sample sample; sample.callsign = "SGTEST1"; sample.sessionId = "session-1"; sample.sequence = 1; sample.receivedAt = 100.0; sample.latitude = data.nodes[stand->second.node].latitude; sample.longitude = data.nodes[stand->second.node].longitude; sample.altitudeFeet = 350; sample.groundSpeedKnots = 0;
    expect(engine.Observe(sample), "accept independent position callback"); expect(!engine.Observe(sample), "reject duplicate callback sequence");
    const auto before = engine.GetSnapshot(sample.callsign, 100.0); expect(before.routeAvailable && before.remainingRoute.size() > 3, "selected native route is available"); expect(!before.controllerRoute, "initial route is standard");
    const auto delayed = engine.GetSnapshot(sample.callsign, 104.0); expect(delayed.reportAgeSeconds == 4.0, "render frames do not freshen report age");
    if (!before.remainingRoute.empty() && !delayed.remainingRoute.empty()) expect(Distance(Project(before.remainingRoute.front().latitude, before.remainingRoute.front().longitude), Project(delayed.remainingRoute.front().latitude, delayed.remainingRoute.front().longitude)) < .001, "display does not hide merely predicted future progress");
    expect(engine.EditRoute(sample.callsign, "osm:2318941305", "osm:8968901466", error), "actual M09/T3 drag commits: " + error);
    const auto edited = engine.GetSnapshot(sample.callsign, 104.0); expect(edited.controllerRoute, "edited route becomes controller-assigned"); expect(edited.reportAgeSeconds == 4.0, "route edit does not fabricate a report");
    expect(engine.UndoRoute(sample.callsign, error), "safe undo at unchanged position: " + error); expect(!engine.GetSnapshot(sample.callsign, 104.0).controllerRoute, "undo restores standard route color");
    const auto path = engine.FindRoute("stand:M09", "operation:departure:08L:T3", context);
    if (path.nodeIds.size() > 12) {
        Path measured; for (const auto& id : path.nodeIds) measured.nodes.push_back(data.nodeById.at(id)); for (const auto& id : path.edgeIds) measured.edges.push_back(data.edgeById.at(id)); data.Rebuild(measured);
        for (int report = 1; report <= 6; ++report) { const Match position = data.PointAt(measured, report * 25.0); const RoutePoint geographic = Geographic(position.p); sample.sequence = static_cast<std::uint64_t>(report + 1); sample.receivedAt = 100.0 + report * 5.0; sample.latitude = geographic.latitude; sample.longitude = geographic.longitude; sample.headingTrueDegrees = std::fmod(450.0 - position.heading * 180.0 / Pi, 360.0); sample.groundSpeedKnots = 10; expect(engine.Observe(sample), "accept physically plausible 25m/five-second measured progress"); }
        const auto progressed = engine.GetSnapshot(sample.callsign, 130.0);
        expect(progressed.distanceRemainingMetres < before.distanceRemainingMetres, "travelled source sections disappear from overlay");
        if (progressed.remainingRoute.size() > 1) expect(progressed.remainingRoute[1].nodeId != path.nodeIds[1], "overlay does not retain old starting segments");
    }
    Context heavy = context; heavy.profile = { "A388", 79.75, 72.73 };
    const int g3 = data.edgeById.at("osm:159161262:20"); expect(!data.AllowArc(g3, data.edges[g3].from, heavy), "A380 excluded from orange G3 footprint");
    Context b744 = context; b744.profile = { "B744", 64.92, 70.67 }; expect(data.AllowArc(g3, data.edges[g3].from, b744) || data.AllowArc(g3, data.edges[g3].to, b744), "B744 does not inherit B748-specific G3 exclusion");
    b744.coloredDualAxis = true; expect(!data.AllowArc(g3, data.edges[g3].from, b744) && !data.AllowArc(g3, data.edges[g3].to, b744), "orange/blue side-axis 36m condition enforced");
    const int forbiddenNode = data.nodeById.at("osm:1690796501"), incoming = data.edgeById.at("osm:222939612:0"), outgoing = data.edgeById.at("osm:156840791:43");
    expect(!data.AllowTurn(incoming, forbiddenNode, outgoing, context), "MD2/D prohibited turn stays forbidden");
    Context arrival = context; arrival.movement = "arrival"; arrival.runway = "09L";
    const auto hold = data.locations.find("operation:arrival:09L:K6"), gate = data.locations.find("stand:F02");
    if (hold != data.locations.end() && gate != data.locations.end()) { const Path arrivalPath = data.Search(hold->second.node, gate->second.node, arrival, hold->first, gate->first); expect(!arrivalPath.empty() && data.NextStop(arrivalPath, arrival, 0.0) == 0.0, "being at the current runway hold does not imply crossing permission"); }
    engine.Forget(sample.callsign); expect(!engine.GetSnapshot(sample.callsign, 110.0).routeAvailable, "disconnect forgets route and alert state");
    Sample first; first.callsign = "SGC1"; first.sessionId = "collision"; first.sequence = 1; first.receivedAt = 200; first.latitude = 49.0097; first.longitude = 2.5479; first.altitudeFeet = 350; first.headingTrueDegrees = 90;
    Sample second = first; second.callsign = "SGC2"; second.longitude += 10.0 / (111320.0 * std::cos(49.0097 * Pi / 180.0));
    Context physical; physical.profile = { "A320", 35.8, 37.57 }; physical.airport = "LFPG";
    engine.SetContext(first.callsign, physical); engine.SetContext(second.callsign, physical); engine.Observe(first); engine.Observe(second); engine.Advance(200.0);
    expect(engine.GetSnapshot(first.callsign, 200).collisionAlert && engine.GetSnapshot(second.callsign, 200).collisionAlert, "overlapping observed aircraft footprints alert on both tags");
    second.sequence = 2; second.receivedAt = 205; second.longitude += 200.0 / (111320.0 * std::cos(49.0097 * Pi / 180.0)); first.sequence = 2; first.receivedAt = 205; engine.Observe(first); engine.Observe(second); engine.Advance(205);
    second.sequence = 3; second.receivedAt = 210; first.sequence = 3; first.receivedAt = 210; engine.Observe(first); engine.Observe(second); engine.Advance(210);
    expect(!engine.GetSnapshot(first.callsign, 210).collisionAlert, "two independent separated reports release collision alert");
    const auto lost = engine.GetSnapshot(first.callsign, 231); expect(lost.dataQuality == "LOST" && !lost.collisionAlert && !lost.routeAvailable, "lost data cannot keep confident alerts or route projection");

    const Profile small{ "TEST", 4.0, 4.0 };
    const std::vector<PhysicalMotion> early{ {0, 20, {0, 0}, {10, 0}, 0, 0} }, late{ {19, 20, {190, 0}, {10, 0}, 0, 0} };
    expect(std::abs(SweptEnvelopeProximity(small, early, small, late, 0.0) - 19.0) < 1e-8, "collision broad phase compares motions at the same overlap time");

    Engine deviations;
    expect(deviations.Load(WriteTrackingFixture(root).u8string(), error), "load deterministic tracking fixture: " + error);
    Context assigned; assigned.configuration = "east"; assigned.standId = "A"; assigned.runway = "08L"; assigned.holdingPointId = "D"; assigned.profile = physical.profile;
    deviations.SetContext("SGDEV", assigned);
    Sample observed; observed.callsign = "SGDEV"; observed.sessionId = "deviation"; observed.altitudeFeet = 350; observed.groundSpeedKnots = 10;
    const auto report = [&](Point position, double heading, double time, std::uint64_t sequence) {
        const auto geographic = Geographic(position); observed.latitude = geographic.latitude; observed.longitude = geographic.longitude; observed.headingTrueDegrees = heading; observed.receivedAt = time; observed.sequence = sequence;
        expect(deviations.Observe(observed), "accept independent deviation fixture report");
    };
    report({70, 0}, 90, 100, 0);
    expect(deviations.GetSnapshot("SGDEV", 100).routeAvailable, "tracking fixture starts on an assigned standard route");
    report({100, 22}, 0, 105, 1);
    const auto suspected = deviations.GetSnapshot("SGDEV", 105);
    expect(suspected.suspectedOffTrack && !suspected.offTrackAlert, "first wrong-branch report is suspicion only");
    for (int frame = 0; frame < 16; ++frame) { deviations.Advance(105.0 + frame * .25); expect(!deviations.Observe(observed), "replayed callback cannot vote for Off Track"); }
    expect(!deviations.GetSnapshot("SGDEV", 109).offTrackAlert, "renders and duplicate reports cannot confirm Off Track");
    report({100, 48}, 0, 110, 2);
    const auto confirmed = deviations.GetSnapshot("SGDEV", 110);
    expect(confirmed.offTrackAlert && confirmed.recoveryProposed, "second fresh wrong-branch report confirms Off Track and proposes recovery");
    if (!confirmed.remainingRoute.empty()) expect(Distance(Project(confirmed.remainingRoute.front().latitude, confirmed.remainingRoute.front().longitude), {100, 48}) < .001, "recovery starts at the currently observed arc");
    report({122, 100}, 90, 115, 3);
    const auto nextArc = deviations.GetSnapshot("SGDEV", 115);
    expect(nextArc.recoveryProposed, "recovery stays available after entering its next arc");
    if (!nextArc.remainingRoute.empty()) expect(Distance(Project(nextArc.remainingRoute.front().latitude, nextArc.remainingRoute.front().longitude), {122, 100}) < .001, "recovery clips its new current arc without rewinding to the old first edge");
    const std::array<Point, 9> rejoin{ Point{147,100}, Point{172,100}, Point{197,100}, Point{200,78}, Point{200,53}, Point{200,28}, Point{200,3}, Point{222,0}, Point{247,0} };
    for (std::size_t i = 0; i < rejoin.size(); ++i) {
        report(rejoin[i], i < 3 || i >= 7 ? 90.0 : 180.0, 120.0 + static_cast<double>(i) * 5.0, static_cast<std::uint64_t>(i + 4));
        if (i == 7) expect(deviations.GetSnapshot("SGDEV", 155).offTrackAlert, "one rejoining report does not immediately clear Off Track");
    }
    expect(!deviations.GetSnapshot("SGDEV", 160).offTrackAlert, "two fresh correct-route reports clear Off Track");
    return failures;
}
