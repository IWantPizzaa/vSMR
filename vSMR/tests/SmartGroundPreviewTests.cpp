#include "SmartGroundPreviewTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include <array>
#include <fstream>
#include <iomanip>

namespace
{
std::filesystem::path WritePreviewFixture(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    const auto dir = root / "smart-ground" / "tmp" / "native_preview_fixture"; std::filesystem::create_directories(dir);
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
std::vector<std::string> RunSmartGroundPreviewTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    std::vector<std::string> failures;
    const auto expect = [&](bool value, const std::string& message) { if (!value) failures.push_back("Drag preview: " + message); };
    Engine engine; std::string error;
    expect(engine.Load(WritePreviewFixture(root).u8string(), error), "load fixture");
    Context context; context.configuration = "east"; context.standId = "A"; context.runway = "08L"; context.holdingPointId = "T3"; context.profile = { "A320", 35.8, 37.57 };
    const auto report = [&](const std::string& callsign, Point point, double now, std::uint64_t sequence, double speed) {
        const auto geo = Geographic(point); Sample sample; sample.callsign = callsign; sample.sessionId = "preview"; sample.sequence = sequence; sample.receivedAt = now; sample.latitude = geo.latitude; sample.longitude = geo.longitude; sample.altitudeFeet = 350; sample.groundSpeedKnots = speed;
        expect(engine.Observe(sample), "accept fixture report");
    };
    engine.SetContext("PREVIEW", context); report("PREVIEW", {0,0}, 100, 1, 0);
    const auto initial = engine.GetSnapshot("PREVIEW", 100);
    Snapshot proposed;
    expect(engine.PreviewEditRoute("PREVIEW", "C", "U", proposed, error), "valid alternative has a preview: " + error);
    expect(proposed.taxiwayRoute == "N X Y Z D C", "preview follows the computed legal alternate");
    bool green = false, blue = false;
    for (const auto& point : proposed.remainingRoute) { green = green || !point.controllerSegment; blue = blue || point.controllerSegment; }
    expect(green && blue, "preview retains changed/official segment colors");
    for (int i = 0; i < 12; ++i) expect(engine.PreviewEditRoute("PREVIEW", "C", "U", proposed, error), "repeated preview remains available");
    const auto after = engine.GetSnapshot("PREVIEW", 104);
    expect(after.taxiwayRoute == initial.taxiwayRoute && !after.controllerRoute && after.reportAgeSeconds == 4 && after.previewRevision == initial.previewRevision, "preview never assigns a route, refreshes data or changes revision");
    expect(!engine.UndoRoute("PREVIEW", error), "preview creates no undo history");
    expect(!engine.PreviewEditRoute("PREVIEW", "C", "missing", proposed, error) && !proposed.routeAvailable && proposed.remainingRoute.empty(), "invalid target clears the old ghost instead of displaying stale geometry");
    expect(engine.PreviewEditRoute("PREVIEW", "C", "U", proposed, error), "preview can recover after invalid target");
    expect(engine.EditRoute("PREVIEW", "C", "U", error), "release commits the same alternative");
    const auto committed = engine.GetSnapshot("PREVIEW", 100);
    expect(committed.taxiwayRoute == proposed.taxiwayRoute && committed.remainingRoute.size() == proposed.remainingRoute.size() && committed.previewRevision != initial.previewRevision, "commit matches preview and changes revision");
    if (committed.remainingRoute.size() == proposed.remainingRoute.size()) for (std::size_t i = 0; i < committed.remainingRoute.size(); ++i) {
        const auto& a = committed.remainingRoute[i]; const auto& b = proposed.remainingRoute[i];
        expect(a.nodeId == b.nodeId && a.latitude == b.latitude && a.longitude == b.longitude && a.controllerSegment == b.controllerSegment, "every preview segment matches the eventual committed segment");
    }
    expect(engine.UndoRoute("PREVIEW", error), "single commit has exactly its own undo");
    expect(!engine.UndoRoute("PREVIEW", error), "previews did not add hidden undo entries");
    const auto near = Geographic({104,102}); const auto snapped = engine.FindIntersection(near.latitude, near.longitude, 10);
    expect(snapped.nodeId == "U" && Distance(Project(snapped.latitude, snapped.longitude), {100,100}) < .001, "snap returns actual intersection coordinates, not cursor coordinates");
    expect(engine.NearestIntersection(near.latitude, near.longitude, 10) == snapped.nodeId, "legacy lookup agrees with exact snap lookup");
    expect(engine.FindIntersection(near.latitude, near.longitude, 1).nodeId.empty(), "outside snap radius remains unsnapped");
    expect(engine.FindIntersection(std::numeric_limits<double>::quiet_NaN(), near.longitude, 10).nodeId.empty(), "nonfinite input has no snap target");

    engine.SetContext("DEVIATED", context); report("DEVIATED", {70,0}, 200, 1, 10); report("DEVIATED", {100,22}, 205, 2, 10); report("DEVIATED", {100,48}, 210, 3, 10);
    const auto offTrack = engine.GetSnapshot("DEVIATED", 210);
    expect(offTrack.offTrackAlert, "fixture starts with a confirmed deviation");
    expect(engine.PreviewEditRoute("DEVIATED", "V", "W", proposed, error), "off-track recovery is previewable");
    const auto stillOffTrack = engine.GetSnapshot("DEVIATED", 214);
    expect(stillOffTrack.offTrackAlert && stillOffTrack.offTrackDeviation && stillOffTrack.previewRevision == offTrack.previewRevision && stillOffTrack.reportAgeSeconds == 4, "preview cannot clear or disregard a real off-track episode");
    expect(engine.EditRoute("DEVIATED", "V", "W", error) && !engine.GetSnapshot("DEVIATED", 210).offTrackDeviation, "only accepting the preview clears the real deviation");
    const auto version = engine.GetSnapshot("DEVIATED", 210).previewRevision;
    context.holdingPointId = "T4"; engine.SetContext("DEVIATED", context);
    expect(engine.GetSnapshot("DEVIATED", 210).previewRevision != version, "context changes invalidate cached previews even without a fresh radar report");
    return failures;
}
