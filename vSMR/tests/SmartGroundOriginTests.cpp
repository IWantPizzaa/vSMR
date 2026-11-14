#include "SmartGroundOriginTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include <algorithm>

std::vector<std::string> RunSmartGroundOriginTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    std::vector<std::string> failures;
    const auto expect = [&](bool condition, const std::string& message) { if (!condition) failures.push_back("Smart Ground offline origin: " + message); };
    const auto directory = root / "vSMR" / "data" / "SmartGround";
    Data data; data.Load(directory.u8string());
    Engine engine; std::string error;
    expect(engine.Load(directory.u8string(), error), "catalogues load: " + error);
    Context context; context.configuration = "east"; context.runway = "08L"; context.holdingPointId = "T3"; context.profile = { "A320", 35.8, 37.57 };
    const auto stand = data.locations.find("stand:M09");
    expect(stand != data.locations.end(), "M09 graph stand exists"); if (stand == data.locations.end()) return failures;
    Sample gate; gate.callsign = "OFFLINEGATE"; gate.sessionId = "offline"; gate.sequence = 1; gate.receivedAt = 100;
    gate.altitudeFeet = 350; gate.latitude = data.nodes[stand->second.node].latitude; gate.longitude = data.nodes[stand->second.node].longitude;
    engine.SetContext(gate.callsign, context); expect(engine.Observe(gate), "stationary report accepted without stand provider");
    const auto initial = engine.GetSnapshot(gate.callsign, 100);
    expect(initial.routeAvailable && initial.remainingRoute.size() > 2, "stationary departure at a real gate has a route without Ramp Agent");
    expect(initial.provisionalRoute && !initial.controllerRoute, "inferred gate origin remains a standard proposal");
    const auto assigned = engine.FindRoute("stand:M09", "operation:departure:08L:T3", context);
    expect(assigned.available && std::abs(initial.distanceRemainingMetres - assigned.distanceMetres) < .01, "gate inference retains exact standard route");
    engine.SetContext(gate.callsign, context);
    const auto unchanged = engine.GetSnapshot(gate.callsign, 101);
    expect(std::abs(unchanged.distanceRemainingMetres - initial.distanceRemainingMetres) < .001 && unchanged.reportAgeSeconds == 1.0, "unchanged empty provider context retains inferred origin and report age");

    Context arrival = context; arrival.movement = "arrival"; arrival.runway = "09L"; arrival.holdingPointId.clear();
    gate.callsign = "OFFLINEARR"; engine.SetContext(gate.callsign, arrival); engine.Observe(gate);
    expect(!engine.GetSnapshot(gate.callsign, 100).routeAvailable, "arrival destination stand is never guessed from current position");
    Context unknown = context; unknown.configuration.clear();
    gate.callsign = "UNKNOWNFLOW"; engine.SetContext(gate.callsign, unknown); engine.Observe(gate);
    expect(!engine.GetSnapshot(gate.callsign, 100).routeAvailable, "unknown flow cannot choose an arbitrary east or west route");

    Path standard;
    for (const auto& id : assigned.nodeIds) standard.nodes.push_back(data.nodeById.at(id));
    for (const auto& id : assigned.edgeIds) standard.edges.push_back(data.edgeById.at(id));
    data.Rebuild(standard);
    Match origin;
    for (double distance = 300; distance < standard.distance - 100; distance += 30) {
        const auto at = data.PointAt(standard, distance);
        if (!at.valid || data.edges[at.edge].kind != 0) continue;
        const auto matched = data.Associate(at.p, at.heading, 5.0, Path(), 0, 0, false);
        if (matched.valid && !matched.ambiguous && matched.edge == at.edge && matched.from == at.from && data.AllowArc(matched.edge, matched.from, context)) {
            const auto next = data.PointAt(standard, distance + 25.0);
            const double heading = std::atan2(next.p.y - at.p.y, next.p.x - at.p.x);
            const auto nextMatch = data.Associate(next.p, heading, 5.0, Path(), 0, 0, false);
            if (Distance(next.p, at.p) > 12 && nextMatch.valid && !nextMatch.ambiguous && nextMatch.edge == next.edge && nextMatch.from == next.from && data.AllowArc(nextMatch.edge, nextMatch.from, context)) { origin = at; break; }
        }
    }
    expect(origin.valid, "real directed taxiway supports cold-start fixture");
    if (origin.valid) {
        Sample taxi; taxi.callsign = "OFFLINETAXI"; taxi.sessionId = "cold"; taxi.sequence = 1; taxi.receivedAt = 200; taxi.altitudeFeet = 350; taxi.groundSpeedKnots = 10;
        const auto point = Geographic(origin.p); taxi.latitude = point.latitude; taxi.longitude = point.longitude;
        taxi.headingTrueDegrees = std::fmod(630.0 - origin.heading * 180.0 / Pi, 360.0);
        engine.SetContext(taxi.callsign, context); expect(engine.Observe(taxi), "cold taxi report accepted");
        expect(!engine.GetSnapshot(taxi.callsign, 200).routeAvailable, "a body heading alone cannot choose cold taxi direction during possible pushback");
        const auto next = data.PointAt(standard, origin.routeDistance + 25.0);
        const auto nextPoint = Geographic(next.p);
        taxi.latitude = nextPoint.latitude; taxi.longitude = nextPoint.longitude; taxi.sequence = 2; taxi.receivedAt = 205;
        // Deliberately keep the old fuselage heading; measured motion wins.
        expect(engine.Observe(taxi), "second independent cold taxi position accepted");
        const auto route = engine.GetSnapshot(taxi.callsign, 205);
        expect(route.routeAvailable, "already-taxiing departure has a legal route without a historical gate");
        if (route.routeAvailable && route.remainingRoute.size() > 1) {
            expect(Distance(Project(route.remainingRoute.front().latitude, route.remainingRoute.front().longitude), next.p) < .01, "cold route starts at measured position without a connector");
            expect(route.remainingRoute[1].nodeId == data.nodes[next.to].id, "cold route retains observed direction through the next intersection");
            expect(route.distanceRemainingMetres < assigned.distanceMetres, "cold route excludes historical gate geometry");
        }
        const auto retained = engine.GetSnapshot(taxi.callsign, 209);
        expect(std::abs(retained.distanceRemainingMetres - route.distanceRemainingMetres) < .001, "cold route does not advance on render frames");
    }
    return failures;
}
