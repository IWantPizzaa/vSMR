#include "SmartGroundOfflineReplayTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include "shared/JsonDocument.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
    using namespace VsmrSmartGround;
    rapidjson::Document ReadOfflineJson(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Missing local replay input: " + path.u8string());
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        rapidjson::Document value; VsmrJson::ParseDocument(value, bytes);
        if (value.HasParseError() || !value.IsObject()) throw std::runtime_error("Invalid offline replay JSON");
        return value;
    }
    std::string ReplayText(const rapidjson::Value& value, const char* key)
    {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString()) throw std::runtime_error("Invalid offline replay text field");
        return value[key].GetString();
    }
    double ReplayNumber(const rapidjson::Value& value, const char* key)
    {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsNumber() || !std::isfinite(value[key].GetDouble())) throw std::runtime_error("Invalid offline replay numeric field");
        return value[key].GetDouble();
    }
    double ClosestStand(const Data& data, Point point, double* second = nullptr)
    {
        std::map<int, double> distances;
        for (const auto& item : data.locations) if (item.second.kind == 1 && item.second.node >= 0)
            distances[item.second.node] = Distance(point, data.nodes[item.second.node].p);
        std::vector<double> ordered;
        for (const auto& item : distances) ordered.push_back(item.second);
        std::sort(ordered.begin(), ordered.end());
        if (second) *second = ordered.size() >= 2 ? ordered[1] : Infinity;
        return ordered.empty() ? Infinity : ordered.front();
    }
    Sample ToSample(const rapidjson::Value& value)
    {
        Sample sample; sample.callsign = ReplayText(value, "aircraftId"); sample.sessionId = "offline-replay-14";
        sample.receivedAt = ReplayNumber(value, "observedAt"); sample.latitude = ReplayNumber(value, "latitude"); sample.longitude = ReplayNumber(value, "longitude");
        sample.altitudeFeet = ReplayNumber(value, "altitudeFeet"); sample.groundSpeedKnots = ReplayNumber(value, "groundSpeedKnots");
        // Replay PBH has no verified true/magnetic reference. Route direction is
        // established from independent measured displacement, not a fake heading.
        sample.headingTrueDegrees = -1;
        return sample;
    }
}

int RunSmartGroundOfflineReplayTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    try {
        const auto cache = root / "smart-ground" / "tmp" / "replay-cache";
        const auto metadata = ReadOfflineJson(cache / "replay-14.metadata.json");
        if (ReplayText(metadata, "fileName") != "PBN_09_04_26.txt" || !metadata.HasMember("targets") || !metadata["targets"].IsObject()) throw std::runtime_error("Wrong offline replay corpus");
        const auto references = ReadOfflineJson(root / "smart-ground" / "docs" / "replay-validation-results.json");
        if (!references.HasMember("comparedTracks") || !references["comparedTracks"].IsArray()) throw std::runtime_error("Missing replay track selection");
        std::set<std::string> departures;
        for (const auto& value : references["comparedTracks"].GetArray())
            if (ReplayText(value, "source") == "PBN_09_04_26.txt" && ReplayText(value, "movement") == "departure" && ReplayText(value, "type") == "A320") departures.insert(ReplayText(value, "id"));
        // The historical report is used only to select sustained A320 departure
        // tracks and the west configuration. No inferred stand or access is read.
        Context context; context.configuration = "west"; context.activeDepartureRunways = { "26R", "27L" }; context.movement = "departure"; context.profile = { "A320", 35.8, 37.57 };
        Data data; data.Load((root / "vSMR" / "data" / "SmartGround").u8string());
        Engine engine; std::string error;
        if (!engine.Load((root / "vSMR" / "data" / "SmartGround").u8string(), error)) throw std::runtime_error(error);
        std::ifstream input(cache / "replay-14.jsonl");
        if (!input) throw std::runtime_error("Missing anonymized PBN_09_04_26 cache");
        std::map<std::string, Sample> previous;
        std::set<std::string> gateChecked, taxiChecked, arrivalChecked;
        std::size_t gateRoutes = 0, taxiRoutes = 0, arrivalChecks = 0;
        const auto require = [](bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); };
        const auto verifyRoute = [&](const Snapshot& snapshot, const Sample& sample, const char* category) {
            require(snapshot.routeAvailable && snapshot.remainingRoute.size() >= 2, std::string(category) + " departure has no route: " + sample.callsign + ": " + snapshot.routeMessage);
            require(!snapshot.controllerRoute && snapshot.provisionalRoute, "No-provider route must remain a standard proposal");
            require(snapshot.distanceRemainingMetres > 0 && std::isfinite(snapshot.distanceRemainingMetres), "Invalid no-provider route distance");
            const auto& first = snapshot.remainingRoute.front();
            require(Distance(Project(first.latitude, first.longitude), Project(sample.latitude, sample.longitude)) <= 45.0, "No-provider route starts at an old stand instead of the observed position");
            const auto rendered = engine.GetSnapshot(sample.callsign, sample.receivedAt + 4);
            require(rendered.routeAvailable && !rendered.remainingRoute.empty() && Distance(Project(first.latitude, first.longitude), Project(rendered.remainingRoute.front().latitude, rendered.remainingRoute.front().longitude)) < .001, "Rendering consumed unmeasured route progress");
        };
        std::string line;
        while (std::getline(input, line)) {
            rapidjson::Document value; VsmrJson::ParseDocument(value, line);
            if (value.HasParseError() || !value.IsObject()) throw std::runtime_error("Malformed offline telemetry");
            Sample sample = ToSample(value);
            if (sample.altitudeFeet < 200 || sample.altitudeFeet > 600 || sample.groundSpeedKnots > 35) continue;
            const Point point = Project(sample.latitude, sample.longitude);
            if (departures.count(sample.callsign)) {
                double second = Infinity; const double standDistance = ClosestStand(data, point, &second);
                if (gateChecked.count(sample.callsign) == 0 && sample.groundSpeedKnots <= 1 && standDistance < 40 && second - standDistance >= 8) {
                    engine.Forget(sample.callsign); engine.SetContext(sample.callsign, context); sample.sequence = 1;
                    require(engine.Observe(sample), "Real stationary departure report was rejected");
                    verifyRoute(engine.GetSnapshot(sample.callsign, sample.receivedAt), sample, "Gate");
                    gateChecked.insert(sample.callsign); ++gateRoutes;
                }
                const auto prior = previous.find(sample.callsign);
                if (taxiChecked.count(sample.callsign) == 0 && prior != previous.end() && sample.groundSpeedKnots >= 5 && prior->second.groundSpeedKnots >= 5 && standDistance > 90) {
                    const double dt = sample.receivedAt - prior->second.receivedAt;
                    const Point before = Project(prior->second.latitude, prior->second.longitude);
                    const double displacement = Distance(point, before);
                    if (dt >= 3 && dt <= 8 && displacement >= 14 && displacement <= 150) {
                        const double heading = std::atan2(point.y - before.y, point.x - before.x);
                        const auto match = data.Associate(point, heading, sample.groundSpeedKnots * KnotsToMetres, Path(), 0, 0, false);
                        if (match.valid && !match.ambiguous && match.distance < 10 && data.edges[match.edge].kind == 0 && data.AllowArc(match.edge, match.from, context)) {
                            engine.Forget(sample.callsign); engine.SetContext(sample.callsign, context);
                            auto first = prior->second; first.sequence = 1; sample.sequence = 2;
                            require(engine.Observe(first) && engine.Observe(sample), "Cold-start real taxi reports were rejected");
                            verifyRoute(engine.GetSnapshot(sample.callsign, sample.receivedAt), sample, "Cold taxi");
                            taxiChecked.insert(sample.callsign); ++taxiRoutes;
                        }
                    }
                }
                previous[sample.callsign] = sample;
            } else if (arrivalChecks < 3 && arrivalChecked.count(sample.callsign) == 0 && metadata["targets"].HasMember(sample.callsign.c_str())) {
                const auto& target = metadata["targets"][sample.callsign.c_str()];
                if (ReplayText(target, "destination") == "LFPG") {
                    Context arrival = context; arrival.movement = "arrival"; arrival.profile.typeCode = ReplayText(target, "typeCode");
                    engine.Forget(sample.callsign); engine.SetContext(sample.callsign, arrival); sample.sequence = 1;
                    require(engine.Observe(sample), "Arrival sample rejected");
                    require(!engine.GetSnapshot(sample.callsign, sample.receivedAt).routeAvailable, "Arrival invented a destination stand without Ramp Agent");
                    arrivalChecked.insert(sample.callsign);
                    ++arrivalChecks;
                }
            }
        }
        require(gateRoutes >= 3 && taxiRoutes >= 3 && arrivalChecks == 3, "Offline replay did not exercise enough gate, cold taxi and missing-arrival-destination cases");
        std::cout << "PBN_09_04_26 offline Smart Ground passed: " << gateRoutes << " gate departures, " << taxiRoutes << " cold taxi departures, " << arrivalChecks << " arrivals without an invented stand. No Ramp Agent stands or holding points were injected.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Offline Smart Ground replay: " << exception.what() << '\n';
        return 1;
    }
}
