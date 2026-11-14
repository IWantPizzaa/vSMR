#include "smart_ground/SmartGroundInternal.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace VsmrSmartGround
{
void MarkControllerSegments(Path& candidate, const Path& standard)
{
    // Hunt-Szymanski alignment of directed source-edge occurrences. This is
    // the longest common subsequence without a quadratic route-size matrix;
    // an early detour touching an official edge cannot consume a later rejoin.
    candidate.controllerSegments.assign(candidate.edges.size(), candidate.controller);
    if (candidate.controller) {
        const auto key = [](int edge, int from) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(edge)) << 32) | static_cast<std::uint32_t>(from); };
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> positions;
        for (std::size_t i = 0; i < standard.edges.size(); ++i) positions[key(standard.edges[i], standard.nodes[i])].push_back(i);
        struct Link { std::size_t candidate = 0; int parent = -1; };
        std::vector<Link> links; std::vector<std::size_t> tails; std::vector<int> tailLinks;
        for (std::size_t i = 0; i < candidate.edges.size(); ++i) {
            const auto found = positions.find(key(candidate.edges[i], candidate.nodes[i]));
            if (found == positions.end()) continue;
            for (auto occurrence = found->second.rbegin(); occurrence != found->second.rend(); ++occurrence) {
                const auto at = static_cast<std::size_t>(std::lower_bound(tails.begin(), tails.end(), *occurrence) - tails.begin());
                const int link = static_cast<int>(links.size()); links.push_back({ i, at ? tailLinks[at - 1] : -1 });
                if (at == tails.size()) { tails.push_back(*occurrence); tailLinks.push_back(link); }
                else { tails[at] = *occurrence; tailLinks[at] = link; }
            }
        }
        if (!tailLinks.empty()) for (int link = tailLinks.back(); link >= 0; link = links[link].parent) candidate.controllerSegments[links[link].candidate] = false;
    }
}

bool Engine::Impl::CurrentPrefix(const Track& track, Path& prefix, std::string& error) const
{
    if (!track.hasSample || track.path.empty()) { error = "No assigned connected route"; return false; }
    if (track.offTrack) {
        if (!track.match.valid || track.match.ambiguous || !data.AllowArc(track.match.edge, track.match.from, track.context)) { error = "No legal directed continuation from the observed position"; return false; }
        prefix.nodes = { track.match.from, track.match.to }; prefix.edges = { track.match.edge };
    } else {
        const auto current = data.PointAt(track.path, track.progress);
        if (!current.valid) { error = "The current position is not established"; return false; }
        const std::size_t count = static_cast<std::size_t>(current.routeIndex) + (current.progress <= 1e-10 ? 1U : 2U);
        prefix.nodes.assign(track.path.nodes.begin(), track.path.nodes.begin() + static_cast<std::ptrdiff_t>(count));
        prefix.edges.assign(track.path.edges.begin(), track.path.edges.begin() + static_cast<std::ptrdiff_t>(count - 1));
    }
    data.Rebuild(prefix); return true;
}

void Engine::Impl::ApplyRoute(Track& track, Path candidate, bool controller) const
{
    candidate.controller = controller; candidate.revision = track.path.revision + 1;
    MarkControllerSegments(candidate, track.standard);
    const bool wasOffTrack = track.offTrack; const double previousProgress = track.progress;
    const auto oldPoint = data.PointAt(track.path, previousProgress);
    const auto entered = oldPoint.valid ? static_cast<std::size_t>(oldPoint.routeIndex) + 1 : 0;
    const bool keepPrefix = !wasOffTrack && entered && candidate.edges.size() >= entered &&
        candidate.nodes.front() == track.path.nodes.front() && std::equal(track.path.edges.begin(), track.path.edges.begin() + static_cast<std::ptrdiff_t>(entered), candidate.edges.begin());
    track.path = std::move(candidate); track.recovery = Path();
    track.match = data.Associate(track.raw, track.speed >= 1.5 ? track.movementHeading : std::numeric_limits<double>::quiet_NaN(), track.speed, track.path, keepPrefix ? previousProgress : 0.0, 0, keepPrefix);
    track.progress = track.match.routeIndex >= 0 ? (keepPrefix ? (std::max)(previousProgress, track.match.routeDistance) : track.match.routeDistance) : 0.0;
    track.offTrack = false; track.suspect = false; track.evidence = 0; track.normalEvidence = 0; track.deviationKind = 0; track.disregardEpisode = false; track.routeError.clear();

}

void Engine::Impl::CommitRoute(Track& track, Path candidate, bool controller)
{
    track.editHistory.push_back(track.path);
    if (track.editHistory.size() > 32) track.editHistory.erase(track.editHistory.begin());
    ApplyRoute(track, std::move(candidate), controller);
    track.previewRevision = nextPreviewRevision++;
    for (auto it = pairs.begin(); it != pairs.end();) { if (it->second.first == track.sample.callsign || it->second.second == track.sample.callsign) it = pairs.erase(it); else ++it; }
}

bool Engine::Impl::BuildRouteEdit(const Track& track, const std::string& anchorId, const std::string& targetId, Path& candidate, std::string& error) const
{
    const auto target = data.nodeById.find(targetId);
    if (target == data.nodeById.end() || !data.IsJunction(target->second)) { error = "Drop the point near a real taxiway intersection"; return false; }
    const bool recovering = track.offTrack && !track.recovery.empty(); const Path& display = recovering ? track.recovery : track.path;
    const double measured = recovering ? track.match.progress * data.edges[display.edges.front()].length : track.progress;
    auto anchor = display.nodes.end();
    for (std::size_t i = 1; i + 1 < display.nodes.size(); ++i) if (display.cumulative[i] > measured + 1.0 && data.nodes[display.nodes[i]].id == anchorId) { anchor = display.nodes.begin() + static_cast<std::ptrdiff_t>(i); break; }
    if (anchor == display.nodes.end()) { error = "The displayed route has changed; select a future point"; return false; }
    const auto anchorIndex = static_cast<std::size_t>(anchor - display.nodes.begin());
    if (anchorIndex == 0 || anchorIndex + 1 >= display.nodes.size() || display.cumulative[anchorIndex] <= measured + 1.0) { error = "Travelled route and destination cannot be moved"; return false; }
    if (*anchor == target->second) { error = "The intersection is unchanged"; return false; }
    Path prefix; if (!CurrentPrefix(track, prefix, error)) return false;
    if (std::find(prefix.nodes.begin(), prefix.nodes.end(), target->second) != prefix.nodes.end()) { error = "The intersection is already travelled or currently entered"; return false; }
    std::vector<int> waypoints;
    for (int waypoint : track.path.waypoints) {
        const auto position = std::find(display.nodes.begin(), display.nodes.end(), waypoint);
        if (position != display.nodes.end() && display.cumulative[static_cast<std::size_t>(position - display.nodes.begin())] > measured + 1.0) waypoints.push_back(waypoint);
    }
    const auto replacing = std::find(waypoints.begin(), waypoints.end(), *anchor);
    if (std::find(waypoints.begin(), waypoints.end(), target->second) != waypoints.end()) { error = "This intersection is already assigned"; return false; }
    if (replacing != waypoints.end()) *replacing = target->second;
    else {
        const auto insertion = std::find_if(waypoints.begin(), waypoints.end(), [&](int node) { return std::find(display.nodes.begin(), display.nodes.end(), node) > anchor; });
        waypoints.insert(insertion, target->second);
    }
    candidate = data.Search(prefix.nodes.back(), track.path.nodes.back(), track.context, track.originId, track.destinationId, true, &prefix, waypoints);
    if (candidate.empty()) { error = "No legal path through this intersection (direction, turn or aircraft restriction)"; return false; }
    error.clear(); return true;
}

bool Engine::EditRoute(const std::string& callsign, const std::string& anchorId, const std::string& targetId, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end() || found->second.path.empty() || !found->second.hasSample) { error = "No assigned connected route"; return false; }
    Path candidate;
    if (!impl_->BuildRouteEdit(found->second, anchorId, targetId, candidate, error)) return false;
    impl_->CommitRoute(found->second, std::move(candidate), true); error.clear(); return true;
}

bool Engine::PreviewEditRoute(const std::string& callsign, const std::string& anchorId, const std::string& targetId, Snapshot& preview, std::string& error) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    preview = Snapshot();
    const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end() || found->second.path.empty() || !found->second.hasSample) { error = "No assigned connected route"; return false; }
    Path candidate;
    if (!impl_->BuildRouteEdit(found->second, anchorId, targetId, candidate, error)) return false;
    Track proposed = found->second;
    impl_->ApplyRoute(proposed, std::move(candidate), true);
    preview = impl_->SnapshotFor(callsign, proposed, (std::max)(proposed.sample.receivedAt, impl_->lastAdvance));
    if (!preview.routeAvailable) { error = "The latest position report is too old for a route preview"; return false; }
    error.clear(); return true;
}

bool Engine::SetTaxiwayRoute(const std::string& callsign, const std::string& text, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end() || found->second.path.empty()) { error = "No assigned route or destination"; return false; }
    if (text.empty() || text.size() > 512) { error = "Enter a taxiway sequence, for example N B3 D C"; return false; }
    std::string normalized;
    for (unsigned char c : text) {
        if (std::isalnum(c)) normalized.push_back(static_cast<char>(std::toupper(c)));
        else if (std::isspace(c)) normalized.push_back(' ');
        else { error = "Use taxiway names separated by spaces"; return false; }
    }
    std::vector<std::string> names; std::istringstream input(normalized); std::string name;
    while (input >> name) {
        if (names.empty() || names.back() != name) names.push_back(name);
        if (names.size() > 64) { error = "Too many taxiways"; return false; }
    }
    if (names.empty()) { error = "Enter at least one taxiway"; return false; }
    for (const auto& requested : names) if (std::none_of(impl_->data.edges.begin(), impl_->data.edges.end(), [&](const Edge& edge) { return TaxiwayName(edge) == requested; })) { error = "Unknown taxiway: " + requested; return false; }
    Track& track = found->second; Path prefix;
    if (!impl_->CurrentPrefix(track, prefix, error)) return false;
    Path candidate = impl_->data.Search(prefix.nodes.back(), track.path.nodes.back(), track.context, track.originId, track.destinationId, true, &prefix, {}, names);
    if (candidate.empty()) { error = "No legal route through that taxiway sequence"; return false; }
    impl_->CommitRoute(track, std::move(candidate), true); error.clear(); return true;
}

bool Engine::ResetRoute(const std::string& callsign, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex); const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end() || found->second.path.empty()) { error = "No assigned route or destination"; return false; }
    Track& track = found->second; Path prefix;
    if (!impl_->CurrentPrefix(track, prefix, error)) return false;
    // Reset replans the untravelled future from the actual directed arc. A
    // historical gate/passed controller detour must not constrain the reset.
    const auto observed = track.offTrack ? track.match : impl_->data.PointAt(track.path, track.progress);
    if (!observed.valid) { error = "Current directed position is unavailable"; return false; }
    prefix.nodes = { observed.from, observed.to }; prefix.edges = { observed.edge }; impl_->data.Rebuild(prefix);
    const std::string origin = "observed:" + impl_->data.nodes[prefix.nodes.front()].id;
    Path candidate = impl_->data.Search(prefix.nodes.back(), track.path.nodes.back(), track.context, origin, track.destinationId, false, &prefix);
    if (candidate.empty()) { error = "No legal standard route from the observed position"; return false; }
    impl_->CommitRoute(track, std::move(candidate), false); track.standard = track.path; error.clear(); return true;
}

void Engine::DisregardOffTrack(const std::string& callsign, bool permanent)
{
    std::lock_guard<std::mutex> lock(impl_->mutex); const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end()) return;
    if (permanent) found->second.disregardPermanent = true;
    else if (found->second.offTrack) found->second.disregardEpisode = true;
    found->second.previewRevision = impl_->nextPreviewRevision++;
}

bool Engine::UndoRoute(const std::string& callsign, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex); const auto found = impl_->tracks.find(callsign);
    if (found == impl_->tracks.end() || found->second.editHistory.empty()) { error = "No route edit to undo"; return false; }
    Track& track = found->second; const Path previous = track.editHistory.back();
    if (track.offTrack || !impl_->data.Validate(previous, track.context)) { error = "The previous route is not legal from this observation"; return false; }
    const Match point = impl_->data.PointAt(track.path, track.progress); if (!point.valid) { error = "Current route is unavailable"; return false; }
    const std::size_t count = static_cast<std::size_t>(point.routeIndex) + 1;
    if (previous.edges.size() < count || !std::equal(track.path.edges.begin(), track.path.edges.begin() + static_cast<std::ptrdiff_t>(count), previous.edges.begin()) || previous.nodes.size() < count + 1 || !std::equal(track.path.nodes.begin(), track.path.nodes.begin() + static_cast<std::ptrdiff_t>(count + 1), previous.nodes.begin())) { error = "The aircraft has entered the changed branch; assign a new forward route"; return false; }
    // Commit performs tracking/evidence reset, but Undo must pop instead of
    // creating a new alternating edit-history entry.
    const auto history = track.editHistory; impl_->CommitRoute(track, previous, previous.controller);
    track.editHistory = history; track.editHistory.pop_back(); error.clear(); return true;
}
}
