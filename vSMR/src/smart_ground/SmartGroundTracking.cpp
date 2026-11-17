#include "smart_ground/SmartGroundInternal.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace VsmrSmartGround
{
Match Data::Associate(Point point, double heading, double speed, const Path& path, double previousDistance, double expectedAdvance, bool useContinuity) const
{
    std::vector<Match> candidates;
    for (int index : NearbyEdges(point, 45.0)) {
        const auto& edge = edges[index]; const Point a = nodes[edge.from].p, b = nodes[edge.to].p;
        const double dx = b.x - a.x, dy = b.y - a.y, denominator = dx * dx + dy * dy; if (denominator < 1e-10) continue;
        const double fraction = Clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / denominator, 0.0, 1.0);
        const Point p{ a.x + fraction * dx, a.y + fraction * dy }; const double distance = Distance(point, p); if (distance > 45.0) continue;
        for (int direction = 0; direction < 2; ++direction) {
            Match match; match.valid = true; match.edge = index; match.from = direction ? edge.to : edge.from; match.to = direction ? edge.from : edge.to; match.progress = direction ? 1.0 - fraction : fraction;
            match.p = p; match.distance = distance; match.heading = std::atan2(direction ? -dy : dy, direction ? -dx : dx);
            for (std::size_t i = 0; i < path.edges.size(); ++i) if (path.edges[i] == index) {
                if (path.nodes[i] == match.from && path.nodes[i + 1] == match.to) {
                    if (match.routeIndex < 0 || useContinuity && std::abs(path.cumulative[i] + match.progress * edge.length - previousDistance - expectedAdvance) < std::abs(match.routeDistance - previousDistance - expectedAdvance)) { match.routeIndex = static_cast<int>(i); match.routeDistance = path.cumulative[i] + match.progress * edge.length; }
                } else match.opposite = true;
            }
            if (match.routeIndex >= 0) match.opposite = false;
            const double headingPenalty = std::isfinite(heading) && speed >= 1.5 ? 16.0 * std::abs(Wrap(heading - match.heading)) / Pi : 0.0;
            const double continuity = match.routeIndex >= 0 && useContinuity ? (std::min)(8.0, std::abs(match.routeDistance - previousDistance - expectedAdvance) * .06) : 0.0;
            match.score = distance + headingPenalty + continuity - (match.routeIndex >= 0 ? 1.0 : 0.0); candidates.push_back(match);
        }
    }
    if (candidates.empty()) return Match();
    std::sort(candidates.begin(), candidates.end(), [&](const Match& a, const Match& b) { return a.score != b.score ? a.score < b.score : edges[a.edge].id < edges[b.edge].id; });
    Match best = candidates.front();
    std::unordered_set<int> corridor{ best.edge }; std::vector<int> queue{ best.edge };
    for (std::size_t i = 0; i < queue.size() && i < 128; ++i) {
        const auto& current = edges[queue[i]];
        for (int node : { current.from, current.to }) {
            if (nodes[node].edges.size() != 2) continue;
            for (int candidate : nodes[node].edges) {
                if (corridor.count(candidate) || edges[candidate].kind != current.kind) continue;
                const Point via = nodes[node].p, before = nodes[current.from == node ? current.to : current.from].p, after = nodes[edges[candidate].from == node ? edges[candidate].to : edges[candidate].from].p;
                const double angle = std::abs(Wrap(std::atan2(after.y - via.y, after.x - via.x) - std::atan2(via.y - before.y, via.x - before.x)));
                if (angle <= 35.0 * Pi / 180.0 && Distance(point, via) <= 65.0) { corridor.insert(candidate); queue.push_back(candidate); }
            }
        }
    }
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const auto& alternative = candidates[i];
        if (alternative.edge == best.edge || corridor.count(alternative.edge)) continue;
        if (alternative.score - best.score < 4.0 && Distance(alternative.p, best.p) > 2.0) { best.ambiguous = true; break; }
    }
    // At a fork within source positional precision, do not choose a future turn.
    if (speed < 1.5) for (int n : { edges[best.edge].from, edges[best.edge].to }) if (IsJunction(n) && Distance(point, nodes[n].p) < 6.0) best.ambiguous = true;
    return best;
}

namespace
{
std::string StandKey(std::string id) { if (!id.empty() && id.rfind("stand:", 0) != 0) id = "stand:" + id; return id; }
bool Contains(const std::vector<std::string>& values, const std::string& value) { return values.empty() || std::find(values.begin(), values.end(), value) != values.end(); }
bool SameContext(const Context& a, const Context& b)
{
    return a.airport == b.airport && a.configuration == b.configuration && a.movement == b.movement && a.standId == b.standId && a.runway == b.runway && a.holdingPointId == b.holdingPointId
        && a.profile.typeCode == b.profile.typeCode && a.profile.wingspanMetres == b.profile.wingspanMetres && a.profile.lengthMetres == b.profile.lengthMetres && a.coloredDualAxis == b.coloredDualAxis && a.southwestDeicingOccupied == b.southwestDeicingOccupied
        && a.activeArrivalRunways == b.activeArrivalRunways && a.activeDepartureRunways == b.activeDepartureRunways;
}
}

void Engine::Impl::EnsureRoute(Track& track, double now)
{
    if (!data.ready || !track.hasContext || track.context.airport != "LFPG" || !track.hasSample || now < track.routeRetryAfter) return;
    track.routeRetryAfter = now + 10.0;
    const Context& context = track.context; const bool arrival = context.movement == "arrival";
    if (context.movement != "departure" && !arrival) { track.routeError = "No LFPG departure or arrival intent identified"; return; }
    if (context.configuration != "east" && context.configuration != "west") { track.routeError = "No active LFPG runway configuration identified"; return; }
    auto stand = data.locations.find(StandKey(context.standId));
    Path observedPrefix;
    std::string observedOrigin;
    // A new holding point/runway changes the destination, not the observed
    // origin. Preserve the currently entered legal arc instead of the old gate.
    const bool awayFromStand = stand == data.locations.end() || Distance(track.raw, data.nodes[stand->second.node].p) > 45.0 || track.speed >= 1.5;
    if (track.reanchorRoute && awayFromStand && (!track.match.valid || track.match.ambiguous || !data.AllowArc(track.match.edge, track.match.from, context))) {
        track.routeError = "Waiting for an unambiguous legal directed position after route intent changed"; return;
    }
    if (track.reanchorRoute && awayFromStand && track.match.valid && !track.match.ambiguous && data.AllowArc(track.match.edge, track.match.from, context)) {
        observedPrefix.nodes = { track.match.from, track.match.to }; observedPrefix.edges = { track.match.edge }; data.Rebuild(observedPrefix);
        observedOrigin = "observed:" + data.nodes[track.match.from].id;
        if (arrival) {
            if (stand == data.locations.end() || stand->second.kind != 1) { track.routeError = "No connected assigned stand"; return; }
            Path route = data.Search(observedPrefix.nodes.back(), stand->second.node, context, observedOrigin, stand->first, false, &observedPrefix);
            if (route.empty()) { track.routeError = "No legal standard route from the observed position"; return; }
            track.path = route; track.standard = route; track.originId = observedOrigin; track.destinationId = stand->first; track.progress = track.match.progress * data.edges[track.match.edge].length;
            track.match = data.Associate(track.raw, track.movementHeading, track.speed, track.path, track.progress, 0, false);
            track.reanchorRoute = false; track.routeError.clear(); return;
        }
    }
    if (observedPrefix.empty() && stand == data.locations.end() && !arrival && context.standId.empty()) {
        // A departure's observed origin is independent of Ramp Agent. Only a
        // real, clearly identified graph stand can anchor a stationary target.
        if (track.speed < 1.5) {
            double nearest = 45.0, second = Infinity;
            for (auto candidate = data.locations.begin(); candidate != data.locations.end(); ++candidate) {
                if (candidate->second.kind != 1) continue;
                const double distance = Distance(track.raw, data.nodes[candidate->second.node].p);
                if (distance < nearest || distance == nearest && stand != data.locations.end() && candidate->first < stand->first) {
                    if (stand != data.locations.end() && stand->second.node != candidate->second.node) second = nearest;
                    nearest = distance; stand = candidate;
                } else if (stand == data.locations.end() || stand->second.node != candidate->second.node) second = (std::min)(second, distance);
            }
            if (second - nearest < 6.0) stand = data.locations.end();
        }
        if (stand == data.locations.end()) {
            const bool headingKnown = track.movementHeadingMeasured;
            const Match observed = data.Associate(track.raw, headingKnown ? track.movementHeading : std::numeric_limits<double>::quiet_NaN(), track.speed, Path(), 0, 0, false);
            if (track.speed < 1.5 || !headingKnown || !observed.valid || observed.ambiguous || observed.distance > 30.0 || !data.AllowArc(observed.edge, observed.from, context)) {
                track.routeError = "No unambiguous departure stand or directed taxiway identified"; return;
            }
            observedPrefix.nodes = { observed.from, observed.to }; observedPrefix.edges = { observed.edge }; data.Rebuild(observedPrefix);
            observedOrigin = "observed:" + data.nodes[observed.from].id;
        }
    }
    if (observedPrefix.empty() && (stand == data.locations.end() || stand->second.kind != 1)) { track.routeError = "No connected assigned stand"; return; }
    const Point originPoint = !observedPrefix.empty() ? track.raw : data.nodes[stand->second.node].p;
    std::vector<const Access*> choices;
    for (const auto& access : data.accesses) {
        if (access.ppr || !(access.configuration & ConfigurationMask(context.configuration)) || access.role != (arrival ? "arrival" : "departure")) continue;
        if (!context.runway.empty() && access.runway != context.runway) continue;
        const auto& active = arrival ? context.activeArrivalRunways : context.activeDepartureRunways; if (!Contains(active, access.runway)) continue;
        if (!context.holdingPointId.empty()) {
            const auto location = data.locations.find(context.holdingPointId);
            const bool direct = access.locationId == context.holdingPointId || location != data.locations.end() && location->second.node == access.node;
            const bool suffix = access.locationId.size() >= context.holdingPointId.size() && access.locationId.compare(access.locationId.size() - context.holdingPointId.size(), context.holdingPointId.size(), context.holdingPointId) == 0;
            if (!direct && !suffix) continue;
        }
        if (arrival && Distance(track.raw, data.nodes[access.node].p) > 450.0) continue;
        choices.push_back(&access);
    }
    if (choices.empty()) { track.routeError = "No compatible runway access identified"; return; }
    std::sort(choices.begin(), choices.end(), [&](const Access* a, const Access* b) { return Distance(arrival ? track.raw : originPoint, data.nodes[a->node].p) < Distance(arrival ? track.raw : originPoint, data.nodes[b->node].p); });
    Path best; double bestDistance = Infinity; int attempts = 0;
    for (const auto* access : choices) {
        if (++attempts > 8) break;
        Context actual = context; actual.runway = access->runway;
        const int from = arrival ? access->node : !observedPrefix.empty() ? observedPrefix.nodes.back() : stand->second.node, to = arrival ? stand->second.node : access->node;
        const std::string origin = arrival ? access->locationId : !observedPrefix.empty() ? observedOrigin : stand->first, destination = arrival ? stand->first : access->locationId;
        Path route = data.Search(from, to, actual, origin, destination, false, observedPrefix.empty() ? nullptr : &observedPrefix);
        if (!route.empty() && route.distance < bestDistance) { bestDistance = route.distance; best = std::move(route); track.originId = origin; track.destinationId = destination; }
    }
    if (best.empty()) { track.routeError = "No legal standard route for this aircraft and configuration"; return; }
    track.path = best; track.standard = best; track.reanchorRoute = false; track.disregardEpisode = false; track.routeError.clear(); track.progress = 0.0; track.evidence = 0; track.offTrack = false; track.suspect = false;
    const double heading = track.speed >= 1.5 ? track.movementHeading : std::numeric_limits<double>::quiet_NaN();
    track.match = data.Associate(track.raw, heading, track.speed, track.path, 0, 0, false);
    if (track.match.routeIndex >= 0) track.progress = track.match.routeDistance;
}

Engine::Engine() : impl_(new Impl()) {}
Engine::~Engine() = default;
Engine& Service() { static Engine engine; return engine; }
bool Engine::Load(const std::string& directory, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->tracks.clear(); impl_->pairs.clear(); impl_->lastAdvance = -Infinity;
    try { impl_->data.Load(directory); impl_->tracks.clear(); impl_->pairs.clear(); impl_->lastAdvance = -Infinity; error.clear(); return true; }
    catch (const std::exception& exception) { impl_->data.ready = false; error = exception.what(); return false; }
}
bool Engine::IsReady() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->data.ready; }
void Engine::SetContext(const std::string& callsign, const Context& context)
{
    if (callsign.empty()) return;
    std::lock_guard<std::mutex> lock(impl_->mutex); auto& track = impl_->tracks[callsign];
    if (track.hasContext && SameContext(track.context, context)) return;
    const bool routeChanged = track.hasContext && (track.context.airport != context.airport || track.context.configuration != context.configuration || track.context.movement != context.movement || track.context.standId != context.standId || track.context.runway != context.runway || track.context.holdingPointId != context.holdingPointId || track.context.profile.typeCode != context.profile.typeCode || track.context.profile.wingspanMetres != context.profile.wingspanMetres || track.context.coloredDualAxis != context.coloredDualAxis || track.context.southwestDeicingOccupied != context.southwestDeicingOccupied || track.context.activeArrivalRunways != context.activeArrivalRunways || track.context.activeDepartureRunways != context.activeDepartureRunways);
    track.context = context; track.hasContext = true; track.previewRevision = impl_->nextPreviewRevision++;
    if (routeChanged) { track.reanchorRoute = track.hasSample && !track.path.empty(); track.disregardEpisode = false; track.path = Path(); track.standard = Path(); track.recovery = Path(); track.editHistory.clear(); track.offTrack = false; track.evidence = 0; }
    track.routeRetryAfter = 0.0;
    if (track.path.empty()) impl_->EnsureRoute(track, track.hasSample ? track.sample.receivedAt : 0.0);
}
bool Engine::Observe(const Sample& sample)
{
    if (sample.callsign.empty() || !std::isfinite(sample.receivedAt) || sample.receivedAt < 0 || !std::isfinite(sample.latitude) || !std::isfinite(sample.longitude) || !std::isfinite(sample.groundSpeedKnots) || sample.groundSpeedKnots < 0 || sample.groundSpeedKnots > 100.0 || !std::isfinite(sample.reportedAgeSeconds) || sample.reportedAgeSeconds < 0 || sample.reportedAgeSeconds > 20.0) return false;
    const Point raw = Project(sample.latitude, sample.longitude);
    if (!sample.surfaceTarget || std::abs(raw.x) > 6500 || std::abs(raw.y) > 4000 || sample.altitudeFeet > 1200.0 || sample.altitudeFeet < -200.0) { Forget(sample.callsign); return false; }
    std::lock_guard<std::mutex> lock(impl_->mutex); auto& track = impl_->tracks[sample.callsign];
    if (track.hasSample && track.sample.sessionId != sample.sessionId) { const Context context = track.context; const bool hasContext = track.hasContext; track = Track(); track.context = context; track.hasContext = hasContext; for (auto it = impl_->pairs.begin(); it != impl_->pairs.end();) { if (it->second.first == sample.callsign || it->second.second == sample.callsign) it = impl_->pairs.erase(it); else ++it; } }
    if (track.hasSample && (sample.sequence <= track.sample.sequence || sample.receivedAt <= track.sample.receivedAt)) return false;
    const double dt = track.hasSample ? sample.receivedAt - track.sample.receivedAt : 0.0, displacement = track.hasSample ? Distance(track.raw, raw) : 0.0;
    if (track.hasSample && displacement > 45.0 * dt + 48.0) return false;
    const double measured = dt > 0.0 && displacement > 12.0 ? displacement / dt : -1.0;
    const bool bodyValid = std::isfinite(sample.headingTrueDegrees) && sample.headingTrueDegrees >= 0.0 && sample.headingTrueDegrees < 360.0;
    const double body = bodyValid ? (90.0 - sample.headingTrueDegrees) * Pi / 180.0 : track.bodyHeading;
    const double direction = measured >= 0.0 && dt <= 15.0 ? std::atan2(raw.y - track.raw.y, raw.x - track.raw.x) : bodyValid ? body : std::numeric_limits<double>::quiet_NaN();
    const double rawSpeed = sample.groundSpeedKnots * KnotsToMetres;
    const double observedSpeed = measured >= 0.0 ? .7 * rawSpeed + .3 * measured : rawSpeed;
    const double speed = rawSpeed < .5 && measured < 0.0 ? 0.0 : track.hasSample ? .65 * observedSpeed + .35 * track.speed : observedSpeed;
    const bool continuity = track.hasSample && dt <= 10.0;
    track.previousRaw = track.raw; track.raw = raw; track.speed = speed; track.bodyHeading = body;
    const bool measuredDirection = measured >= 0.0 && dt <= 15.0;
    if (measuredDirection && !track.movementHeadingMeasured && track.path.empty()) track.routeRetryAfter = 0.0;
    if (std::isfinite(direction)) track.movementHeading = direction;
    track.movementHeadingMeasured = measuredDirection;
    const double expected = continuity ? track.speed * dt : 0.0;
    track.sample = sample; track.hasSample = true; track.previewRevision = impl_->nextPreviewRevision++;
    if (!impl_->data.ready) return true;
    if (track.path.empty()) impl_->EnsureRoute(track, sample.receivedAt);
    const Match match = impl_->data.Associate(raw, direction, speed, track.path, track.progress, expected, continuity);
    track.match = match;
    if (track.path.empty()) return true;
    int kind = 0;
    if (!match.valid) kind = 3;
    else if (!match.ambiguous && match.routeIndex < 0) {
        if (!match.opposite || speed >= 1.5 && displacement > 12.0) kind = match.opposite ? 2 : 1;
    }
    if (kind) {
        if (track.deviationKind != kind || dt > 15.0) { track.evidence = 1; track.evidenceStart = sample.receivedAt; track.lastEvidence = sample.receivedAt; }
        else if (sample.receivedAt - track.lastEvidence >= 3.0) { track.evidence++; track.lastEvidence = sample.receivedAt; }
        track.normalEvidence = 0; track.deviationKind = kind; track.suspect = true;
        if (track.evidence >= 2 && sample.receivedAt - track.evidenceStart >= 5.0) track.offTrack = true;
    } else if (match.valid && !match.ambiguous && match.routeIndex >= 0) {
        track.normalEvidence++; track.suspect = false; track.evidence = 0; track.deviationKind = 0;
        // Jitter cannot restore already travelled standard geometry.
        track.progress = (std::max)(track.progress, match.routeDistance);
        if (track.normalEvidence >= 2) { track.offTrack = false; track.disregardEpisode = false; track.recovery = Path(); }
    } else { track.evidence = 0; track.suspect = false; track.deviationKind = 0; }
    if (track.offTrack && (!match.valid || !track.recovery.empty() && (track.recovery.edges.front() != match.edge || track.recovery.nodes.front() != match.from))) track.recovery = Path();
    if (track.offTrack && match.valid && track.recovery.empty() && impl_->data.AllowArc(match.edge, match.from, track.context)) {
        Path prefix; prefix.nodes = { match.from, match.to }; prefix.edges = { match.edge }; impl_->data.Rebuild(prefix);
        std::vector<int> remaining;
        for (int waypoint : track.path.waypoints) { auto found = std::find(track.path.nodes.begin(), track.path.nodes.end(), waypoint); if (found != track.path.nodes.end() && track.path.cumulative[static_cast<std::size_t>(found - track.path.nodes.begin())] > track.progress + 1.0) remaining.push_back(waypoint); }
        // Actual current directed edge anchors the proposal; no reversal,
        // shortcut or route acceptance is inferred from this advisory.
        std::vector<std::string> remainingTaxiways;
        if (!track.path.requestedTaxiways.empty()) {
            std::size_t consumed = 0;
            const auto at = impl_->data.PointAt(track.path, track.progress);
            if (at.valid) for (std::size_t i = 0; i <= static_cast<std::size_t>(at.routeIndex) && consumed < track.path.requestedTaxiways.size(); ++i)
                if (TaxiwayName(impl_->data.edges[track.path.edges[i]]) == track.path.requestedTaxiways[consumed]) ++consumed;
            remainingTaxiways.assign(track.path.requestedTaxiways.begin() + static_cast<std::ptrdiff_t>(consumed), track.path.requestedTaxiways.end());
        }
        // Never silently discard remaining explicitly entered taxiway names.
        // If those instructions cannot be safely recovered, require a new edit.
        track.recovery = impl_->data.Search(match.to, track.path.nodes.back(), track.context, track.originId, track.destinationId, true, &prefix, remaining, remainingTaxiways);
    }
    return true;
}

Snapshot Engine::Impl::SnapshotFor(const std::string& callsign, const Track& track, double now) const
{
    Snapshot result; result.callsign = callsign; result.previewRevision = track.previewRevision;
    if (!track.hasSample) { result.dataQuality = "NO DATA"; result.routeMessage = track.routeError; return result; }
    const double age = (std::max)(0.0, now - track.sample.receivedAt) + track.sample.reportedAgeSeconds;
    result.reportAgeSeconds = age; result.dataQuality = age > 20.0 ? "LOST" : age > 10.0 ? "STALE" : track.match.ambiguous ? "AMBIGUOUS" : "TRACKED";
    result.uncertaintyMetres = (std::min)(75.0, 6.0 + age * (.55 + .15 * track.speed));
    const std::string pairKey = callsign < track.collisionWith ? callsign + "|" + track.collisionWith : track.collisionWith + "|" + callsign;
    const auto pair = pairs.find(pairKey);
    result.collisionAlert = age <= 20.0 && track.collision && pair != pairs.end() && pair->second.active; result.collisionCritical = result.collisionAlert && track.critical;
    result.collisionWith = track.collisionWith; result.collisionMessage = track.collisionMessage;
    result.offTrackDeviation = age <= 20.0 && track.offTrack; result.offTrackAlert = result.offTrackDeviation && !track.disregardEpisode && !track.disregardPermanent; result.suspectedOffTrack = !result.offTrackDeviation && track.suspect;
    if (result.offTrackAlert) result.offTrackMessage = track.deviationKind == 2 ? "Opposite movement to assigned route" : track.deviationKind == 3 ? "Position outside matched taxiway network" : "Observed taxiway differs from assigned route";
    result.controllerRoute = track.path.controller; result.provisionalRoute = !track.path.controller; result.routeMessage = track.routeError;
    if (track.path.empty() || age > 20.0) return result;
    const bool recovery = track.offTrack && !track.recovery.empty(); const Path& display = recovery ? track.recovery : track.path;
    double progress = track.progress;
    if (recovery) progress = track.match.progress * data.edges[display.edges.front()].length;
    // Only measured progress removes a displayed section. The base vSMR symbol
    // uses the SDK report, so prediction coasting must not create a line gap.
    progress = Clamp(progress, 0.0, display.distance);
    const Match current = data.PointAt(display, progress); if (!current.valid) return result;
    result.routeAvailable = true; result.recoveryProposed = recovery; result.distanceRemainingMetres = (std::max)(0.0, display.distance - progress);
    RoutePoint first = Geographic(current.p); result.remainingRoute.push_back(first);
    // The list describes the remaining assignment, rather than replacing it
    // with an unaccepted recovery proposal when the aircraft is off track.
    const auto assignedAt = data.PointAt(track.path, track.progress);
    std::string lastTaxiway;
    if (assignedAt.valid) for (std::size_t edge = static_cast<std::size_t>(assignedAt.routeIndex); edge < track.path.edges.size(); ++edge) {
        const auto name = TaxiwayName(data.edges[track.path.edges[edge]]);
        if (name.empty() || name == lastTaxiway) continue;
        if (!result.taxiwayRoute.empty()) result.taxiwayRoute += " ";
        result.taxiwayRoute += name; lastTaxiway = name;
    }
    for (std::size_t i = 0; i < display.nodes.size(); ++i) {
        if (display.cumulative[i] <= progress + .1) continue;
        const auto& node = data.nodes[display.nodes[i]]; RoutePoint point; point.latitude = node.latitude; point.longitude = node.longitude; point.nodeId = node.id;
        point.editable = i + 1 < display.nodes.size() && data.IsJunction(display.nodes[i]);
        point.controllerSegment = !recovery && i > 0 && i - 1 < display.controllerSegments.size() && display.controllerSegments[i - 1]; result.remainingRoute.push_back(std::move(point));
    }
    return result;
}
Snapshot Engine::GetSnapshot(const std::string& callsign, double now) const { std::lock_guard<std::mutex> lock(impl_->mutex); const auto found = impl_->tracks.find(callsign); return found == impl_->tracks.end() ? Snapshot() : impl_->SnapshotFor(callsign, found->second, now); }
std::vector<Snapshot> Engine::GetSnapshots(double now) const { std::lock_guard<std::mutex> lock(impl_->mutex); std::vector<Snapshot> results; results.reserve(impl_->tracks.size()); for (const auto& track : impl_->tracks) results.push_back(impl_->SnapshotFor(track.first, track.second, now)); return results; }
void Engine::Forget(const std::string& callsign) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->tracks.erase(callsign); for (auto it = impl_->pairs.begin(); it != impl_->pairs.end();) { if (it->second.first == callsign || it->second.second == callsign) it = impl_->pairs.erase(it); else ++it; } }
void Engine::Clear() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->tracks.clear(); impl_->pairs.clear(); impl_->lastAdvance = -Infinity; }
RoutePoint Engine::FindIntersection(double latitude, double longitude, double maximum) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    RoutePoint result;
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(maximum) || maximum <= 0) return result;
    const Point p = Project(latitude, longitude);
    for (std::size_t i = 0; i < impl_->data.nodes.size(); ++i) if (impl_->data.IsJunction(static_cast<int>(i))) {
        const auto& node = impl_->data.nodes[i]; const double distance = Distance(p, node.p);
        if (distance < maximum || distance == maximum && !result.nodeId.empty() && node.id < result.nodeId) {
            maximum = distance; result.latitude = node.latitude; result.longitude = node.longitude; result.nodeId = node.id; result.editable = true;
        }
    }
    return result;
}
std::string Engine::NearestIntersection(double latitude, double longitude, double maximum) const
{
    return FindIntersection(latitude, longitude, maximum).nodeId;
}

RouteResult Engine::FindRoute(const std::string& origin, const std::string& destination, const Context& context) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex); RouteResult result;
    const auto from = impl_->data.locations.find(origin), to = impl_->data.locations.find(destination);
    if (from == impl_->data.locations.end() || to == impl_->data.locations.end()) { result.error = "Unknown connected location"; return result; }
    const Path path = impl_->data.Search(from->second.node, to->second.node, context, origin, destination);
    result.available = !path.empty(); result.distanceMetres = path.distance;
    for (int node : path.nodes) result.nodeIds.push_back(impl_->data.nodes[node].id); for (int edge : path.edges) result.edgeIds.push_back(impl_->data.edges[edge].id);
    if (!result.available) result.error = "No legal route"; return result;
}
}
