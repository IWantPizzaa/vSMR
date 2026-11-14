#include "SmartGroundReplayTests.hpp"
#include "smart_ground/SmartGroundInternal.hpp"
#include "shared/JsonDocument.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>

namespace
{
    using namespace VsmrSmartGround;
    rapidjson::Document Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Missing local replay input: " + path.u8string());
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        rapidjson::Document result;
        VsmrJson::ParseDocument(result, bytes);
        if (result.HasParseError() || !result.IsObject()) throw std::runtime_error("Invalid local replay JSON");
        return result;
    }
    std::string Text(const rapidjson::Value& value, const char* key)
    {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString()) throw std::runtime_error("Missing replay text field");
        return value[key].GetString();
    }
    double Number(const rapidjson::Value& value, const char* key)
    {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsNumber() || !std::isfinite(value[key].GetDouble())) throw std::runtime_error("Invalid replay numeric field");
        return value[key].GetDouble();
    }
    struct Reference { Context context; std::uint64_t sequence = 0; Point previous; double previousAt = -1; };
    bool NearGround(const Data& data, Point point)
    {
        for (int id : data.NearbyEdges(point, 45.0)) {
            const auto& edge = data.edges[id];
            if (edge.kind == 2) continue; // Same source-kind mapping as Data::Load.
            const auto a = data.nodes[edge.from].p, b = data.nodes[edge.to].p;
            const double dx = b.x - a.x, dy = b.y - a.y, denominator = dx * dx + dy * dy;
            if (denominator <= 0) continue;
            const double fraction = Clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / denominator, 0, 1);
            if (Distance(point, { a.x + dx * fraction, a.y + dy * fraction }) <= 45.0) return true;
        }
        return false;
    }
}

int RunSmartGroundReplayTests(const std::filesystem::path& root)
{
    using namespace VsmrSmartGround;
    try {
        const auto start = std::chrono::steady_clock::now();
        const auto cache = root / "smart-ground" / "tmp" / "replay-cache";
        const auto inventory = Read(cache / "inventory.json");
        const auto prior = Read(root / "smart-ground" / "docs" / "replay-validation-results.json");
        if (!inventory.HasMember("files") || !inventory["files"].IsArray() || !prior.HasMember("comparedTracks") || !prior["comparedTracks"].IsArray()) throw std::runtime_error("Missing replay inventory/references");
        std::map<std::string, std::map<std::string, Reference>> references;
        for (const auto& value : prior["comparedTracks"].GetArray()) {
            Reference reference;
            auto& context = reference.context;
            context.configuration = Text(value, "configuration"); context.movement = Text(value, "movement"); context.profile.typeCode = Text(value, "type");
            const auto from = Text(value, "inferredFrom"), to = Text(value, "inferredTo");
            context.standId = context.movement == "arrival" ? to : from;
            const auto access = context.movement == "arrival" ? from : to;
            const auto last = access.rfind(':');
            const auto runway = last == std::string::npos ? last : access.rfind(':', last - 1);
            if (runway == std::string::npos || last == std::string::npos) throw std::runtime_error("Invalid provisional runway reference");
            context.runway = access.substr(runway + 1, last - runway - 1); context.holdingPointId = access;
            references[Text(value, "source")].emplace(Text(value, "id"), std::move(reference));
        }
        Data data; data.Load((root / "vSMR" / "data" / "SmartGround").u8string());
        Engine engine; std::string error;
        if (!engine.Load((root / "vSMR" / "data" / "SmartGround").u8string(), error)) throw std::runtime_error(error);
        std::size_t sessions = 0, tracks = 0, reports = 0, accepted = 0, routes = 0, differences = 0, collisions = 0, cycles = 0;
        for (const auto& source : inventory["files"].GetArray()) {
            const auto found = references.find(Text(source, "fileName"));
            if (found == references.end()) continue;
            auto active = found->second;
            const auto cacheName = Text(source, "cache");
            if (cacheName != std::filesystem::path(cacheName).filename().u8string()) throw std::runtime_error("Replay cache path is not a filename");
            std::ifstream input(cache / cacheName);
            if (!input) throw std::runtime_error("Missing anonymized replay cache");
            engine.Clear();
            for (const auto& item : active) engine.SetContext(item.first, item.second.context);
            std::set<std::string> seen;
            double batchAt = -1, predictionAt = -100;
            const auto check = [&](double now) {
                if (now < 0 || now - predictionAt < 5) return;
                predictionAt = now; engine.Advance(now); ++cycles;
                for (const auto& snapshot : engine.GetSnapshots(now)) {
                    if (!std::isfinite(snapshot.distanceRemainingMetres) || !std::isfinite(snapshot.uncertaintyMetres) || !std::isfinite(snapshot.reportAgeSeconds)) throw std::runtime_error("Nonfinite native replay prediction");
                    if (snapshot.reportAgeSeconds > 20 && (snapshot.routeAvailable || snapshot.collisionAlert || snapshot.offTrackAlert)) throw std::runtime_error("Lost data retained confident state");
                    for (const auto& point : snapshot.remainingRoute) if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude)) throw std::runtime_error("Nonfinite native replay route point");
                    if (snapshot.collisionAlert && snapshot.collisionWith.empty()) throw std::runtime_error("Collision without an identified opponent");
                    if (snapshot.collisionAlert) ++collisions;
                }
            };
            std::string line;
            while (std::getline(input, line)) {
                rapidjson::Document value; VsmrJson::ParseDocument(value, line);
                if (value.HasParseError() || !value.IsObject()) throw std::runtime_error("Invalid anonymized telemetry line");
                const auto id = Text(value, "aircraftId"); const auto track = active.find(id);
                if (track == active.end()) continue;
                const double altitude = Number(value, "altitudeFeet"), speed = Number(value, "groundSpeedKnots");
                const Point point{ Number(value, "x"), Number(value, "y") };
                if (altitude < 200 || altitude > 600 || speed > 40 || !NearGround(data, point)) continue;
                const double now = Number(value, "observedAt");
                if (now < batchAt) throw std::runtime_error("Nonmonotonic replay receipt clock");
                if (now > batchAt) { check(batchAt); batchAt = now; }
                auto& reference = track->second;
                Sample sample; sample.callsign = id; sample.sessionId = cacheName; sample.sequence = ++reference.sequence; sample.receivedAt = now;
                sample.latitude = Number(value, "latitude"); sample.longitude = Number(value, "longitude"); sample.altitudeFeet = altitude; sample.groundSpeedKnots = speed;
                // The replay PBH reference is unverified. A geometric motion axis
                // is an estimate only; these runs cannot establish collision truth.
                if (reference.previousAt >= 0 && now > reference.previousAt && now - reference.previousAt <= 30 && Distance(point, reference.previous) >= 12)
                    sample.headingTrueDegrees = std::fmod(450.0 - std::atan2(point.y - reference.previous.y, point.x - reference.previous.x) * 180.0 / Pi, 360.0);
                reference.previous = point; reference.previousAt = now;
                ++reports; seen.insert(id);
                engine.SetContext(id, reference.context);
                if (engine.Observe(sample)) {
                    ++accepted;
                    const auto snapshot = engine.GetSnapshot(id, now);
                    if (snapshot.routeAvailable) ++routes;
                    if (snapshot.offTrackAlert) ++differences;
                    if (engine.Observe(sample)) throw std::runtime_error("Duplicate replay report became independent evidence");
                    const auto rendered = engine.GetSnapshot(id, now + .1);
                    if (rendered.offTrackAlert != snapshot.offTrackAlert || std::abs(rendered.reportAgeSeconds - snapshot.reportAgeSeconds - .1) > 1e-5) throw std::runtime_error("Rendering fabricated fresh tracking evidence");
                }
            }
            check(batchAt);
            if (batchAt >= 0) {
                engine.Advance(batchAt + 21);
                for (const auto& snapshot : engine.GetSnapshots(batchAt + 21)) if (snapshot.routeAvailable || snapshot.collisionAlert || snapshot.offTrackAlert) throw std::runtime_error("Replay expiry did not retire alerts/route");
            }
            ++sessions; tracks += seen.size();
            std::cout << cacheName << ": " << seen.size() << " provisional reference tracks checked\n";
        }
        if (sessions < 6 || tracks < 200 || reports < 70000 || accepted < 65000 || routes < 1000) throw std::runtime_error("Replay validation did not exercise the expected local corpus");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::ofstream output(root / "docs" / "smart_ground_native_validation.json", std::ios::binary);
        output << "{\n  \"sessions\": " << sessions << ",\n  \"tracksWithProvisionalReferences\": " << tracks << ",\n  \"recordedReports\": " << reports << ",\n  \"acceptedReports\": " << accepted << ",\n  \"reportsWithRoute\": " << routes << ",\n  \"reportsWithConfirmedStandardDifference\": " << differences << ",\n  \"collisionTagSnapshots\": " << collisions << ",\n  \"predictionCycles\": " << cycles << ",\n  \"elapsedMilliseconds\": " << elapsed << ",\n  \"groundTruthTaxiClearancesAvailable\": false,\n  \"groundTruthCollisionLabelsAvailable\": false,\n  \"falsePositiveRateMeasured\": false,\n  \"orientation\": \"estimated geometric motion axis; replay PBH true/magnetic reference unverified\",\n  \"purpose\": \"Native engine ingestion, duplicate rejection, finite geometry, report-age and expiry invariants on local anonymized replay caches. Provisional standards are not issued taxi clearances; alert counts are not proven pilot errors or collisions.\"\n}\n";
        if (!output) throw std::runtime_error("Cannot write native replay validation report");
        std::cout << "Native Smart Ground replays passed: " << reports << " recorded reports, " << tracks << " provisional references, " << sessions << " sessions\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Native Smart Ground replay validation: " << exception.what() << '\n';
        return 1;
    }
}
