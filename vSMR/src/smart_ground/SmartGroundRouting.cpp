#include "smart_ground/SmartGroundInternal.hpp"
#include <algorithm>
#include <queue>
#include <tuple>
#include <cctype>

namespace VsmrSmartGround
{
std::string TaxiwayName(const Edge& edge)
{
    if (edge.kind != 0 || edge.name.empty() || edge.name == "Taxiway") return {};
    std::string result;
    for (unsigned char c : edge.name) {
        if (std::isalnum(c)) result.push_back(static_cast<char>(std::toupper(c)));
        else if (c == ' ' && result.size() == 1) continue;
        else break;
    }
    return result == "LINK" || result == "AXE" || result.size() > 8 ? std::string() : result;
}

FlowContext Data::Compile(const Context& context, const std::string& origin, const std::string& destination, int from, int to, bool controller) const
{
    FlowContext result;
    const auto a = locations.find(origin), b = locations.find(destination);
    if (a != locations.end()) result.originZones = a->second.zones;
    if (b != locations.end()) result.destinationZones = b->second.zones;
    if (a == locations.end()) for (const auto& value : locations) if (value.second.node == from && value.second.kind == 1) result.originZones |= value.second.zones;
    // Cold-start departures can be anchored to an observed taxiway rather
    // than an assigned stand. Retain that source corridor's apron-zone policy.
    if (a == locations.end() && origin.rfind("observed:", 0) == 0)
        for (int edge : nodes[from].edges) result.originZones |= edges[edge].zones;
    if (b == locations.end()) for (const auto& value : locations) if (value.second.node == to && value.second.kind == 1) result.destinationZones |= value.second.zones;
    if (controller) return result;
    std::vector<const Policy*> selected;
    for (const auto& p : policies) {
        if (!(p.configuration & ConfigurationMask(context.configuration)) || !(p.movement & MovementMask(context.movement)) || p.originZones && !(p.originZones & result.originZones) || p.destinationZones && !(p.destinationZones & result.destinationZones)
            || !p.destinationNodes.empty() && !p.destinationNodes.count(to) || !p.runways.empty() && !p.runways.count(context.runway)
            || p.differentZones && (!result.originZones || !result.destinationZones || (result.originZones & result.destinationZones))) continue;
        selected.push_back(&p); result.first |= p.first; result.last |= p.last;
    }
    const auto add = [&](std::unordered_set<int> stage) { if (!stage.empty() && (result.stages.empty() || result.stages.back() != stage)) result.stages.push_back(std::move(stage)); };
    for (const auto* p : selected) add(p->originPortals);
    for (const auto* p : selected) for (std::uint64_t group : p->requiredAxes) {
        std::unordered_set<int> stage; for (std::size_t i = 0; i < axisEdges.size(); ++i) if (group & (std::uint64_t(1) << i)) stage.insert(axisEdges[i].begin(), axisEdges[i].end()); add(std::move(stage));
    }
    const bool requireMain = std::any_of(selected.begin(), selected.end(), [](const Policy* p) { return p->mainRequired; });
    const bool mainStage = std::any_of(result.stages.begin(), result.stages.end(), [&](const std::unordered_set<int>& stage) { return std::all_of(stage.begin(), stage.end(), [&](int e) { return edges[e].mainAxes != 0; }); });
    if (requireMain && !mainStage) {
        std::uint64_t preferred = 0; for (const auto* p : selected) preferred |= p->preferred;
        std::unordered_set<int> stage; for (std::size_t i = 0; i < edges.size(); ++i) if (edges[i].mainAxes & preferred) stage.insert(static_cast<int>(i));
        if (stage.empty()) for (std::size_t i = 0; i < edges.size(); ++i) if (edges[i].mainAxes) stage.insert(static_cast<int>(i));
        add(std::move(stage));
    }
    for (const auto* p : selected) add(p->destinationPortals);
    for (const auto* p : selected) {
        for (int direction = 0; direction < 2; ++direction) {
            const auto& allowed = direction == 0 ? p->originPortals : p->destinationPortals; if (allowed.empty()) continue;
            const std::uint32_t zones = direction == 0 ? (p->originZones ? p->originZones : result.originZones) : (p->destinationZones ? p->destinationZones : result.destinationZones);
            for (std::size_t i = 0; i < zonePortals.size(); ++i) if (zones & (std::uint32_t(1) << i)) for (int edge : zonePortals[i]) if (!allowed.count(edge)) result.forbidden.insert(edge);
        }
    }
    return result;
}

bool Data::Step(const FlowContext& context, const FlowState& previous, int index, FlowState& next, double& apron, int& reentries, int& axisChanges) const
{
    const auto& edge = edges[index]; if (edge.excluded || context.forbidden.count(index)) return false;
    next = previous;
    if (next.stage < static_cast<int>(context.stages.size()) && context.stages[next.stage].count(index)) next.stage++;
    const bool origin = (edge.zones & context.originZones) != 0, destination = (edge.zones & context.destinationZones) != 0, main = edge.mainAxes != 0;
    if (main && previous.mainPhase == 0 && context.first && !(edge.mainAxes & context.first)) return false;
    const bool isApron = !main && !edge.connector && (edge.zones || edge.kind == 1 || edge.apron);
    const bool foreign = isApron && edge.zones && !origin && !destination;
    const bool originReturn = isApron && origin && previous.originExited && !destination;
    const bool destinationEscape = previous.destinationEntered && isApron && !destination && !origin;
    apron = foreign || originReturn || destinationEscape ? edge.length : 0.0;
    reentries = main && previous.mainPhase == 2 ? 1 : 0;
    axisChanges = main && previous.lastAxes && !(edge.mainAxes & previous.lastAxes) ? 1 : 0;
    if (main) { next.mainPhase = 1; next.lastAxes = edge.mainAxes; if (context.last) next.lastMatches = (edge.mainAxes & context.last) != 0; }
    else if (isApron && previous.mainPhase > 0) next.mainPhase = 2;
    next.originExited = previous.originExited || context.originZones && (main || edge.connector || edge.zones && !origin);
    next.destinationEntered = previous.destinationEntered || isApron && destination;
    return true;
}

namespace
{
struct State { int node = -1, incoming = -1, parking = 0, waypoint = 0, taxiway = 0; FlowState flow;
    bool operator==(const State& s) const { return node == s.node && incoming == s.incoming && parking == s.parking && waypoint == s.waypoint && taxiway == s.taxiway && flow == s.flow; } };
struct StateHash { std::size_t operator()(const State& s) const { std::size_t h = static_cast<std::size_t>(s.node + 1); const auto add = [&](std::size_t x) { h ^= x + 0x9e3779b9U + (h << 6) + (h >> 2); }; add(static_cast<std::size_t>(s.incoming + 1)); add(static_cast<std::size_t>(s.parking)); add(static_cast<std::size_t>(s.waypoint)); add(static_cast<std::size_t>(s.taxiway)); add(static_cast<std::size_t>(s.flow.stage)); add(static_cast<std::size_t>(s.flow.mainPhase)); add(s.flow.originExited); add(s.flow.destinationEntered); add(s.flow.lastMatches); add(static_cast<std::size_t>(s.flow.lastAxes)); return h; } };
using Cost = std::array<double, 4>;
int Compare(const Cost& a, const Cost& b) { for (std::size_t i = 0; i < a.size(); ++i) if (std::abs(a[i] - b[i]) > 1e-7) return a[i] < b[i] ? -1 : 1; return 0; }
struct Record { State state; Cost cost{}; int parent = -1, edge = -1; };
struct QueueItem { Cost score{}; int index = -1; Cost cost{}; };
struct Order { bool operator()(const QueueItem& a, const QueueItem& b) const { return Compare(a.score, b.score) > 0; } };
}

Path Data::Search(int from, int to, const Context& context, const std::string& originId, const std::string& destinationId, bool controller, const Path* prefix, const std::vector<int>& waypoints, const std::vector<std::string>& taxiways) const
{
    Path failure;
    if (!ready || from < 0 || to < 0 || from >= static_cast<int>(nodes.size()) || to >= static_cast<int>(nodes.size())) return failure;
    const int origin = prefix && !prefix->nodes.empty() ? prefix->nodes.front() : from;
    const FlowContext compiled = Compile(context, originId, destinationId, origin, to, controller);
    State initial; initial.node = from;
    if (prefix && !prefix->edges.empty()) {
        if (!Validate(*prefix, context)) return failure;
        for (std::size_t i = 0; i < prefix->edges.size(); ++i) {
            initial.parking = ParkingPhase(prefix->edges[i], initial.parking, origin, to); if (initial.parking < 0) return failure;
            FlowState next; double apron = 0; int reentries = 0, changes = 0; if (!Step(compiled, initial.flow, prefix->edges[i], next, apron, reentries, changes)) return failure; initial.flow = next;
        }
        initial.incoming = prefix->edges.back();
    }
    while (initial.waypoint < static_cast<int>(waypoints.size()) && prefix && std::find(prefix->nodes.begin(), prefix->nodes.end(), waypoints[initial.waypoint]) != prefix->nodes.end()) initial.waypoint++;
    if (prefix && !prefix->edges.empty() && !taxiways.empty() && TaxiwayName(edges[prefix->edges.back()]) == taxiways.front()) initial.taxiway = 1;
    std::vector<Record> records; records.push_back({ initial, {}, -1, -1 });
    std::unordered_map<State, int, StateHash> indices; indices.emplace(initial, 0);
    std::priority_queue<QueueItem, std::vector<QueueItem>, Order> queue;
    const double heuristic = Distance(nodes[from].p, nodes[to].p); queue.push({ { 0, 0, heuristic, heuristic }, 0, {} });
    std::unordered_set<int> fixedNodes; if (prefix) fixedNodes.insert(prefix->nodes.begin(), prefix->nodes.end() - 1);
    std::size_t expanded = 0;
    while (!queue.empty() && expanded++ < 800000) {
        const QueueItem item = queue.top(); queue.pop(); const Record current = records[item.index]; if (Compare(current.cost, item.cost) != 0) continue;
        if (current.state.node == to && current.state.waypoint == static_cast<int>(waypoints.size()) && current.state.taxiway == static_cast<int>(taxiways.size()) && current.state.flow.stage == static_cast<int>(compiled.stages.size()) && (!compiled.last || current.state.flow.lastMatches)) {
            std::vector<int> routeNodes{ to }, routeEdges; int index = item.index;
            while (records[index].parent >= 0) { routeEdges.push_back(records[index].edge); index = records[index].parent; routeNodes.push_back(records[index].state.node); }
            std::reverse(routeNodes.begin(), routeNodes.end()); std::reverse(routeEdges.begin(), routeEdges.end());
            Path result; if (prefix) { result.nodes = prefix->nodes; result.edges = prefix->edges; result.nodes.insert(result.nodes.end(), routeNodes.begin() + 1, routeNodes.end()); result.edges.insert(result.edges.end(), routeEdges.begin(), routeEdges.end()); }
            else { result.nodes = std::move(routeNodes); result.edges = std::move(routeEdges); }
            result.controller = controller; result.waypoints = waypoints; result.requestedTaxiways = taxiways; Rebuild(result); return result;
        }
        for (int edgeIndex : nodes[current.state.node].edges) {
            const auto& edge = edges[edgeIndex]; const int nextNode = edge.from == current.state.node ? edge.to : edge.from;
            if (fixedNodes.count(nextNode) || !AllowArc(edgeIndex, current.state.node, context) || !AllowTurn(current.state.incoming, current.state.node, edgeIndex, context)) continue;
            State next = current.state; next.node = nextNode; next.incoming = edgeIndex; next.parking = ParkingPhase(edgeIndex, current.state.parking, origin, to); if (next.parking < 0) continue;
            double apron = 0.0; int reentries = 0, changes = 0; if (!Step(compiled, current.state.flow, edgeIndex, next.flow, apron, reentries, changes)) continue;
            if (next.waypoint < static_cast<int>(waypoints.size()) && waypoints[next.waypoint] == nextNode) next.waypoint++;
            if (!taxiways.empty()) {
                const auto taxiway = TaxiwayName(edge);
                // Between requested names, only continue the current taxiway,
                // enter the next requested one or use an unnamed connector.
                if (current.state.taxiway > 0 && current.state.taxiway < static_cast<int>(taxiways.size()) && !taxiway.empty() &&
                    taxiway != taxiways[current.state.taxiway - 1] && taxiway != taxiways[current.state.taxiway]) continue;
                if (next.taxiway < static_cast<int>(taxiways.size()) && taxiway == taxiways[next.taxiway]) next.taxiway++;
            }
            double curve = 0.0;
            if (current.state.incoming >= 0) { const auto& previous = edges[current.state.incoming]; const auto before = nodes[previous.from == current.state.node ? previous.to : previous.from].p, via = nodes[current.state.node].p, after = nodes[nextNode].p;
                curve = 90.0 * std::abs(std::atan2((via.x - before.x) * (after.y - via.y) - (via.y - before.y) * (after.x - via.x), (via.x - before.x) * (after.x - via.x) + (via.y - before.y) * (after.y - via.y))) / (Pi / 2.0); }
            Cost cost = { current.cost[0] + apron, current.cost[1] + reentries, current.cost[2] + edge.length + curve + changes * 120.0, current.cost[3] + edge.length };
            const auto found = indices.find(next); int recordIndex;
            if (found != indices.end()) { recordIndex = found->second; if (Compare(cost, records[recordIndex].cost) >= 0) continue; records[recordIndex] = { next, cost, item.index, edgeIndex }; }
            else { recordIndex = static_cast<int>(records.size()); indices.emplace(next, recordIndex); records.push_back({ next, cost, item.index, edgeIndex }); }
            Cost score = cost; const double distance = Distance(nodes[nextNode].p, nodes[to].p); score[2] += distance; score[3] += distance; queue.push({ score, recordIndex, cost });
        }
    }
    return failure;
}

bool Data::Validate(const Path& path, const Context& context) const
{
    if (path.nodes.size() != path.edges.size() + 1 || path.nodes.empty()) return false;
    for (std::size_t i = 0; i < path.edges.size(); ++i) {
        const int e = path.edges[i]; if (e < 0 || e >= static_cast<int>(edges.size())) return false; const auto& edge = edges[e];
        if (!((edge.from == path.nodes[i] && edge.to == path.nodes[i + 1]) || (edge.to == path.nodes[i] && edge.from == path.nodes[i + 1])) || !AllowArc(e, path.nodes[i], context) || !AllowTurn(i ? path.edges[i - 1] : -1, path.nodes[i], e, context)) return false;
    }
    return true;
}
void Data::Rebuild(Path& path) const { path.cumulative.assign(1, 0.0); for (int e : path.edges) path.cumulative.push_back(path.cumulative.back() + edges[e].length); path.distance = path.cumulative.back(); }
Match Data::PointAt(const Path& path, double distance) const
{
    Match result; if (path.edges.empty()) return result;
    const auto upper = std::upper_bound(path.cumulative.begin(), path.cumulative.end(), distance);
    const int index = (std::min)(static_cast<int>(path.edges.size()) - 1, (std::max)(0, static_cast<int>(upper - path.cumulative.begin()) - 1));
    const auto& edge = edges[path.edges[index]]; const auto a = nodes[path.nodes[index]].p, b = nodes[path.nodes[index + 1]].p;
    result.valid = true; result.edge = path.edges[index]; result.from = path.nodes[index]; result.to = path.nodes[index + 1]; result.routeIndex = index; result.routeDistance = Clamp(distance, 0.0, path.distance);
    result.progress = Clamp((distance - path.cumulative[index]) / edge.length, 0.0, 1.0); result.p = { a.x + (b.x - a.x) * result.progress, a.y + (b.y - a.y) * result.progress }; result.heading = std::atan2(b.y - a.y, b.x - a.x); return result;
}
double Data::NextStop(const Path& path, const Context& context, double observedDistance) const
{
    for (std::size_t i = 0; i < path.nodes.size(); ++i) {
        if (path.cumulative[i] < observedDistance - 6.0) continue;
        const auto found = checkpoints.find(path.nodes[i]); if (found == checkpoints.end()) continue;
        const int incoming = i ? path.edges[i - 1] : -1, outgoing = i < path.edges.size() ? path.edges[i] : -1;
        for (const auto& checkpoint : found->second) if (RuleActive(checkpoint.rule, context) && (checkpoint.incoming < 0 || checkpoint.incoming == incoming) && (checkpoint.outgoing < 0 || checkpoint.outgoing == outgoing)) return (std::max)(observedDistance, path.cumulative[i]);
    }
    return path.distance;
}
}
