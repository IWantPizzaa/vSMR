#include "smart_ground/SmartGroundInternal.hpp"
#include <algorithm>

namespace VsmrSmartGround
{
namespace
{
struct Box { double offset = 0.0, halfLength = 0.0, halfWidth = 0.0; };
using Motion = PhysicalMotion;
struct Forecast { std::string callsign; const Track* track = nullptr; std::vector<Box> boxes; std::vector<Motion> motions; double radius = 0.0, age = 0.0, horizon = 120.0; bool reliable = false; Point minimum{ Infinity, Infinity }, maximum{ -Infinity, -Infinity }; };
struct Candidate { double time = Infinity; bool credible = false, contactNow = false; };

void AddMotion(Forecast& forecast, const Motion& motion)
{
    if (motion.end < motion.start) return;
    const double r = forecast.radius + 5.0 + motion.padding; const Point end{ motion.p.x + motion.velocity.x * (motion.end - motion.start), motion.p.y + motion.velocity.y * (motion.end - motion.start) };
    forecast.minimum.x = (std::min)(forecast.minimum.x, (std::min)(motion.p.x, end.x) - r); forecast.minimum.y = (std::min)(forecast.minimum.y, (std::min)(motion.p.y, end.y) - r);
    forecast.maximum.x = (std::max)(forecast.maximum.x, (std::max)(motion.p.x, end.x) + r); forecast.maximum.y = (std::max)(forecast.maximum.y, (std::max)(motion.p.y, end.y) + r);
    forecast.motions.push_back(motion);
}

Forecast Predict(const Data& data, const std::string& callsign, const Track& track, double now)
{
    Forecast f; f.callsign = callsign; f.track = &track; f.age = (std::max)(0.0, now - track.sample.receivedAt) + track.sample.reportedAgeSeconds;
    const Profile profile = data.EffectiveProfile(track.context.profile); const bool primary = profile.typeCode == "A320" || profile.typeCode == "A388" || profile.typeCode == "B744" || profile.typeCode == "B748";
    const double l = profile.lengthMetres, w = profile.wingspanMetres;
    if (primary) {
        const double bodyWidth = profile.typeCode == "A320" ? 3.95 : profile.typeCode == "A388" ? 7.14 : 6.50;
        f.boxes = { { 0.0, l / 2.0, bodyWidth / 2.0 }, { -.025 * l, .295 * l, w / 2.0 }, { -.37 * l, .13 * l, .225 * w } };
    } else f.boxes = { { 0.0, l / 2.0, w / 2.0 } };
    for (const auto& box : f.boxes) f.radius = (std::max)(f.radius, std::hypot(std::abs(box.offset) + box.halfLength, box.halfWidth));
    f.reliable = f.age <= 7.0 && track.match.valid && !track.match.ambiguous && !track.suspect;
    f.horizon = f.age > 10.0 ? 5.0 : track.offTrack || track.path.empty() || !f.reliable ? 10.0 : 120.0;
    // A report at a holding point does not grant the following nominal route.
    // Measured motion may still be projected on its currently observed arc.
    const bool onPath = !track.path.empty() && track.match.routeIndex >= 0 && !track.offTrack && !track.suspect && !track.match.ambiguous && data.NextStop(track.path, track.context, track.progress) > track.progress + 1e-8;
    if (track.speed < .5 || !track.match.valid || track.match.ambiguous || track.suspect) {
        AddMotion(f, { 0.0, f.horizon, track.match.valid && !track.match.ambiguous ? track.match.p : track.raw, {}, track.bodyHeading, 0.0 }); return f;
    }
    Path path;
    double distance;
    if (onPath) {
        path = track.path;
        distance = (std::min)(data.NextStop(path, track.context, track.progress), track.progress + track.speed * (std::min)(10.0, f.age));
    } else { path.nodes = { track.match.from, track.match.to }; path.edges = { track.match.edge }; data.Rebuild(path); distance = (std::min)(path.distance, track.match.progress * path.distance + track.speed * (std::min)(10.0, f.age)); }
    const Match current = data.PointAt(path, distance); if (!current.valid) { AddMotion(f, { 0, f.horizon, track.raw, {}, track.bodyHeading, 0 }); return f; }
    if (f.age > 10.0) { AddMotion(f, { 0.0, f.horizon, current.p, {}, current.heading + Wrap(track.bodyHeading - track.match.heading), 0.0 }); return f; }
    const double stop = onPath ? data.NextStop(path, track.context, track.progress) : path.distance;
    const double offset = Wrap(track.bodyHeading - track.match.heading); double previousBody = current.heading + offset;
    Point last = current.p; double lastTime = 0.0;
    for (std::size_t i = static_cast<std::size_t>(current.routeIndex); i < path.edges.size(); ++i) {
        const double fromDistance = (std::max)(distance, path.cumulative[i]), toDistance = (std::min)(stop, path.cumulative[i + 1]); if (toDistance < fromDistance || fromDistance >= stop) break;
        const double start = (fromDistance - distance) / track.speed, end = (std::min)(f.horizon, (toDistance - distance) / track.speed); if (start > f.horizon) break;
        const auto a = data.nodes[path.nodes[i]].p, b = data.nodes[path.nodes[i + 1]].p; const double edgeLength = data.edges[path.edges[i]].length;
        const Point p{ a.x + (b.x - a.x) * (fromDistance - path.cumulative[i]) / edgeLength, a.y + (b.y - a.y) * (fromDistance - path.cumulative[i]) / edgeLength };
        const Point velocity{ (b.x - a.x) / edgeLength * track.speed, (b.y - a.y) / edgeLength * track.speed };
        const double body = std::atan2(b.y - a.y, b.x - a.x) + offset, delta = Wrap(body - previousBody);
        if (std::abs(delta) > 1e-8) {
            const int count = static_cast<int>(std::ceil(std::abs(delta) / (3.0 * Pi / 180.0))); const double step = delta / count;
            for (int k = 0; k < count; ++k) AddMotion(f, { start, start, p, {}, previousBody + step * (k + .5), 2.0 * f.radius * std::sin(std::abs(step) / 4.0) });
        }
        AddMotion(f, { start, end, p, velocity, body, 0.0 }); previousBody = body; last = { p.x + velocity.x * (end - start), p.y + velocity.y * (end - start) }; lastTime = end;
        if (end >= f.horizon || toDistance >= stop) break;
    }
    if (lastTime < f.horizon) AddMotion(f, { lastTime, f.horizon, last, {}, previousBody, 0.0 });
    return f;
}

double Overlap(const Motion& a, const Motion& b, const Box& shapeA, const Box& shapeB, double start, double end, double margin)
{
    const double c1 = std::cos(a.body), s1 = std::sin(a.body), c2 = std::cos(b.body), s2 = std::sin(b.body);
    const Point pa{ a.p.x + a.velocity.x * (start - a.start) + shapeA.offset * c1, a.p.y + a.velocity.y * (start - a.start) + shapeA.offset * s1 }, pb{ b.p.x + b.velocity.x * (start - b.start) + shapeB.offset * c2, b.p.y + b.velocity.y * (start - b.start) + shapeB.offset * s2 };
    const double al = shapeA.halfLength + margin / 2.0 + a.padding, aw = shapeA.halfWidth + margin / 2.0 + a.padding, bl = shapeB.halfLength + margin / 2.0 + b.padding, bw = shapeB.halfWidth + margin / 2.0 + b.padding;
    double entry = 0.0, exit = end - start;
    for (Point axis : { Point{ c1, s1 }, Point{ -s1, c1 }, Point{ c2, s2 }, Point{ -s2, c2 } }) {
        const double radius = al * std::abs(c1 * axis.x + s1 * axis.y) + aw * std::abs(-s1 * axis.x + c1 * axis.y) + bl * std::abs(c2 * axis.x + s2 * axis.y) + bw * std::abs(-s2 * axis.x + c2 * axis.y);
        const double separation = (pa.x - pb.x) * axis.x + (pa.y - pb.y) * axis.y, velocity = (a.velocity.x - b.velocity.x) * axis.x + (a.velocity.y - b.velocity.y) * axis.y;
        if (std::abs(velocity) < 1e-12) { if (std::abs(separation) > radius + 1e-9) return Infinity; }
        else { const double t1 = (-radius - separation) / velocity, t2 = (radius - separation) / velocity; entry = (std::max)(entry, (std::min)(t1, t2)); exit = (std::min)(exit, (std::max)(t1, t2)); if (entry > exit + 1e-9) return Infinity; }
    }
    return start + (std::max)(0.0, entry);
}

Candidate Encounter(const Forecast& a, const Forecast& b, double margin)
{
    Candidate result; if (a.maximum.x < b.minimum.x || b.maximum.x < a.minimum.x || a.maximum.y < b.minimum.y || b.maximum.y < a.minimum.y) return result;
    std::size_t firstB = 0;
    for (const auto& first : a.motions) {
        while (firstB < b.motions.size() && b.motions[firstB].end < first.start - 1e-9) firstB++;
        for (std::size_t k = firstB; k < b.motions.size() && b.motions[k].start <= first.end + 1e-9; ++k) {
            const auto& second = b.motions[k]; const double start = (std::max)(first.start, second.start), end = (std::min)(first.end, second.end); if (end < start) continue;
            const Point atStartA{ first.p.x + first.velocity.x * (start - first.start), first.p.y + first.velocity.y * (start - first.start) }, atStartB{ second.p.x + second.velocity.x * (start - second.start), second.p.y + second.velocity.y * (start - second.start) };
            if (Distance(atStartA, atStartB) > a.radius + b.radius + 5.0 + (std::hypot(first.velocity.x, first.velocity.y) + std::hypot(second.velocity.x, second.velocity.y)) * (end - start) + first.padding + second.padding) continue;
            for (const auto& boxA : a.boxes) for (const auto& boxB : b.boxes) {
                const double time = Overlap(first, second, boxA, boxB, start, end, margin);
                if (time < result.time) result.time = time;
                if (time <= 1e-8 && Overlap(first, second, boxA, boxB, start, end, 0.0) <= 1e-8) result.contactNow = true;
            }
        }
    }
    result.credible = a.reliable && b.reliable; return result;
}
}

double SweptEnvelopeProximity(const Profile& first, const std::vector<PhysicalMotion>& a, const Profile& second, const std::vector<PhysicalMotion>& b, double margin)
{
    Data data;
    const auto forecast = [&](const Profile& input, const std::vector<PhysicalMotion>& motions) {
        Forecast result; const Profile profile = data.EffectiveProfile(input); result.reliable = true;
        const bool primary = profile.typeCode == "A320" || profile.typeCode == "A388" || profile.typeCode == "B744" || profile.typeCode == "B748";
        if (primary) { const double body = profile.typeCode == "A320" ? 3.95 : profile.typeCode == "A388" ? 7.14 : 6.5; const double l = profile.lengthMetres, w = profile.wingspanMetres; result.boxes = { {0, l / 2, body / 2}, {-.025 * l, .295 * l, w / 2}, {-.37 * l, .13 * l, .225 * w} }; }
        else result.boxes = { { 0, profile.lengthMetres / 2, profile.wingspanMetres / 2 } };
        for (const auto& box : result.boxes) result.radius = (std::max)(result.radius, std::hypot(std::abs(box.offset) + box.halfLength, box.halfWidth));
        for (const auto& motion : motions) AddMotion(result, motion);
        return result;
    };
    return Encounter(forecast(first, a), forecast(second, b), margin).time;
}

void Engine::Impl::EvaluateCollisions(double now)
{
    std::vector<Forecast> forecasts; forecasts.reserve(tracks.size());
    for (auto& target : tracks) {
        target.second.collision = false; target.second.critical = false; target.second.collisionWith.clear(); target.second.collisionMessage.clear();
        if (!target.second.hasSample || now - target.second.sample.receivedAt + target.second.sample.reportedAgeSeconds > 20.0) continue;
        forecasts.push_back(Predict(data, target.first, target.second, now));
    }
    std::sort(forecasts.begin(), forecasts.end(), [](const Forecast& a, const Forecast& b) { return a.callsign < b.callsign; });
    std::unordered_set<std::string> visited;
    for (std::size_t i = 0; i < forecasts.size(); ++i) for (std::size_t j = i + 1; j < forecasts.size(); ++j) {
        const auto& a = forecasts[i]; const auto& b = forecasts[j]; const std::string key = a.callsign + "|" + b.callsign;
        const Candidate positive = Encounter(a, b, 2.0); auto found = pairs.find(key);
        if (!std::isfinite(positive.time) && found == pairs.end()) continue;
        visited.insert(key);
        if (found == pairs.end()) { CollisionState state; state.first = a.callsign; state.second = b.callsign; state.firstEvidence = now; found = pairs.emplace(key, std::move(state)).first; }
        auto& state = found->second;
        const double sourceAt = (std::max)(a.track->sample.receivedAt, b.track->sample.receivedAt);
        const bool fresh = !state.hasReports || state.sequenceA != a.track->sample.sequence || state.sequenceB != b.track->sample.sequence;
        if (fresh) { state.hasReports = true; state.sequenceA = a.track->sample.sequence; state.sequenceB = b.track->sample.sequence; }
        if (std::isfinite(positive.time)) {
            if (fresh) { if (state.positives == 0) state.firstEvidence = sourceAt; state.positives++; state.lastPositive = sourceAt; state.negatives = 0; }
            const bool urgent = positive.credible && positive.time <= 15.0;
            if (urgent || positive.contactNow && a.age <= 7.0 && b.age <= 7.0 || state.positives >= 2 && sourceAt - state.firstEvidence >= 5.0) state.active = true;
            state.critical = state.active && urgent; state.timeToConflict = positive.time;
        } else {
            const Candidate release = Encounter(a, b, 5.0);
            if (!std::isfinite(release.time) && fresh) { if (state.negatives == 0) state.clearStart = sourceAt; state.negatives++; if (state.negatives >= 2 && sourceAt - state.clearStart >= 5.0) state.active = false; }
            state.critical = false;
        }
        if (state.active) {
            auto& first = tracks[state.first]; auto& second = tracks[state.second];
            if (!first.collision || state.critical) { first.collision = true; first.critical = state.critical; first.collisionWith = state.second; first.collisionMessage = "Predicted airframe proximity with " + state.second; }
            if (!second.collision || state.critical) { second.collision = true; second.critical = state.critical; second.collisionWith = state.first; second.collisionMessage = "Predicted airframe proximity with " + state.first; }
        }
    }
    for (auto it = pairs.begin(); it != pairs.end();) { if (!visited.count(it->first) || !it->second.active && now - it->second.lastPositive > 30.0) it = pairs.erase(it); else ++it; }
}

void Engine::Advance(double now)
{
    if (!std::isfinite(now) || now < 0.0) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (now < impl_->lastAdvance) { impl_->pairs.clear(); impl_->lastAdvance = -Infinity; }
    if (now - impl_->lastAdvance < .25) return;
    impl_->lastAdvance = now;
    for (auto it = impl_->tracks.begin(); it != impl_->tracks.end();) {
        if (it->second.hasSample && now - it->second.sample.receivedAt > 120.0) { it = impl_->tracks.erase(it); continue; }
        if (it->second.path.empty()) impl_->EnsureRoute(it->second, now);
        ++it;
    }
    impl_->EvaluateCollisions(now);
}
}
