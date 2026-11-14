#include "smart_ground/SmartGroundInternal.hpp"
#include "shared/JsonDocument.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace VsmrSmartGround
{
namespace
{
using Json = rapidjson::Value;
std::string Text(const Json& v, const char* key, const std::string& fallback = {}) { return v.IsObject() && v.HasMember(key) && v[key].IsString() ? v[key].GetString() : fallback; }
double Number(const Json& v, const char* key, double fallback = 0.0) { return v.IsObject() && v.HasMember(key) && v[key].IsNumber() ? v[key].GetDouble() : fallback; }
bool Boolean(const Json& v, const char* key, bool fallback = false) { return v.IsObject() && v.HasMember(key) && v[key].IsBool() ? v[key].GetBool() : fallback; }
const Json& Array(const Json& v, const char* key) { static const Json empty(rapidjson::kArrayType); if (!v.IsObject()) throw std::runtime_error("Expected Smart Ground object"); if (v.HasMember(key) && !v[key].IsArray()) throw std::runtime_error("Expected Smart Ground array"); return v.HasMember(key) ? v[key] : empty; }
void CheckArrays(const Json& value)
{
    if (value.IsObject()) for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
        const std::string key = member->name.GetString();
        const bool strings = key == "edgeIds" || key == "standIds" || key == "portalEdgeIds" || key == "originPortalEdgeIds" || key == "destinationPortalEdgeIds" || key == "originZoneIds" || key == "destinationZoneIds" || key == "destinationNodeIds" || key == "runways" || key == "preferredAxisIds" || key == "firstMainAxisIds" || key == "lastMainAxisIds" || key == "sourceIds" || key == "typeCodes" || key == "excludedTypeCodes";
        if (strings) { if (!member->value.IsArray()) throw std::runtime_error("Invalid identifier array"); for (const auto& item : member->value.GetArray()) if (!item.IsString()) throw std::runtime_error("Invalid identifier array value"); }
        if (key == "requiredAxisGroups") { if (!member->value.IsArray()) throw std::runtime_error("Invalid axis groups"); for (const auto& group : member->value.GetArray()) { if (!group.IsArray()) throw std::runtime_error("Invalid axis group"); for (const auto& item : group.GetArray()) if (!item.IsString()) throw std::runtime_error("Invalid axis identifier"); } }
        if (key == "airportFlags" && member->value.IsArray()) for (const auto& item : member->value.GetArray()) if (!item.IsString() && !item.IsObject()) throw std::runtime_error("Invalid airport flag");
        CheckArrays(member->value);
    } else if (value.IsArray()) for (const auto& item : value.GetArray()) CheckArrays(item);
}
rapidjson::Document Read(const std::filesystem::path& directory, const char* name)
{
    std::ifstream input(directory / name, std::ios::binary); if (!input) throw std::runtime_error(std::string("Missing Smart Ground data: ") + name);
    input.seekg(0, std::ios::end); const std::streamoff length = input.tellg();
    if (length < 0 || length > 64 * 1024 * 1024) throw std::runtime_error("Smart Ground JSON exceeds input limit");
    input.seekg(0); std::string bytes(static_cast<std::size_t>(length), '\0'); input.read(bytes.data(), length);
    if (!input && length) throw std::runtime_error("Could not read complete Smart Ground JSON");
    rapidjson::Document document; VsmrJson::ParseDocument(document, bytes);
    if (document.HasParseError() || !document.IsObject()) throw std::runtime_error(std::string("Invalid Smart Ground JSON: ") + name);
    CheckArrays(document);
    return document;
}
template<class Map> int Lookup(const Map& map, const std::string& id) { const auto found = map.find(id); if (found == map.end()) throw std::runtime_error("Unknown Smart Ground geometry reference: " + id); return found->second; }
std::uint64_t Cell(int x, int y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y); }
std::uint32_t ZoneMask(const Json& values, const std::unordered_map<std::string, int>& ids) { std::uint32_t result = 0; for (const auto& value : values.GetArray()) result |= std::uint32_t(1) << Lookup(ids, value.GetString()); return result; }
std::uint64_t AxisMask(const Json& values, const std::unordered_map<std::string, int>& ids) { std::uint64_t result = 0; for (const auto& value : values.GetArray()) result |= std::uint64_t(1) << Lookup(ids, value.GetString()); return result; }
}

void Data::Load(const std::string& directory)
{
    *this = Data();
    const auto path = std::filesystem::u8path(directory);
    const auto graph = Read(path, "lfpg.json"), operations = Read(path, "lfpg_operations.json"), corridors = Read(path, "lfpg_corridors.json"), aircraft = Read(path, "lfpg_aircraft_restrictions.json");
    for (const auto* document : { &operations, &corridors, &aircraft }) if (Text(*document, "airport") != "LFPG") throw std::runtime_error("Smart Ground airport manifest mismatch");
    for (const auto& value : Array(graph, "nodes").GetArray()) {
        Node node; node.id = Text(value, "id"); node.p = { Number(value, "x"), Number(value, "y") }; node.latitude = Number(value, "lat"); node.longitude = Number(value, "lon");
        if (node.id.empty() || nodeById.count(node.id) || !std::isfinite(node.p.x) || !std::isfinite(node.p.y)) throw std::runtime_error("Invalid Smart Ground node");
        nodeById[node.id] = static_cast<int>(nodes.size()); nodes.push_back(std::move(node));
    }
    for (const auto& value : Array(graph, "edges").GetArray()) {
        Edge edge; edge.id = Text(value, "id"); edge.name = Text(value, "name"); edge.from = Lookup(nodeById, Text(value, "from")); edge.to = Lookup(nodeById, Text(value, "to")); edge.length = Number(value, "length"); edge.sourceSegment = Text(value, "sourceSegmentId");
        const std::string kind = Text(value, "kind"); edge.kind = kind == "runway" ? 2 : kind == "parking_position" ? 1 : 0;
        if (edge.id.empty() || edgeById.count(edge.id) || !std::isfinite(edge.length) || edge.length <= 0.0) throw std::runtime_error("Invalid Smart Ground edge");
        const int index = static_cast<int>(edges.size()); edgeById[edge.id] = index; nodes[edge.from].edges.push_back(index); nodes[edge.to].edges.push_back(index); edges.push_back(std::move(edge));
    }
    if (nodes.empty() || edges.empty()) throw std::runtime_error("Empty Smart Ground graph");
    auto locationsFrom = [&](const Json& document, const char* key) {
        for (const auto& value : Array(document, key).GetArray()) {
            Location location; location.id = Text(value, "id"); location.name = Text(value, "name");
            if (!value.HasMember("nodeId") || !value["nodeId"].IsString()) continue;
            location.node = Lookup(nodeById, value["nodeId"].GetString()); location.kind = Text(value, "kind") == "stand" ? 1 : 0;
            locations.emplace(location.id, std::move(location));
        }
    };
    locationsFrom(graph, "locations"); locationsFrom(operations, "operationalLocations");
    for (const auto* document : { &operations, &corridors }) if (document->HasMember("edgeNameOverrides") && (*document)["edgeNameOverrides"].IsObject())
        for (auto member = (*document)["edgeNameOverrides"].MemberBegin(); member != (*document)["edgeNameOverrides"].MemberEnd(); ++member) {
            const auto found = edgeById.find(member->name.GetString()); if (found != edgeById.end() && member->value.IsString()) edges[found->second].name = member->value.GetString();
        }
    for (const auto& value : Array(corridors, "axes").GetArray()) {
        const int index = static_cast<int>(axisIds.size()); if (index >= 63) throw std::runtime_error("Too many Smart Ground axes");
        axisIds[Text(value, "id")] = index; axisEdges.emplace_back();
        const std::string role = Text(value, "role");
        for (const auto& id : Array(value, "edgeIds").GetArray()) {
            const int e = Lookup(edgeById, id.GetString()); axisEdges.back().insert(e);
            if (role == "main") edges[e].mainAxes |= std::uint64_t(1) << index;
            if (role == "connector") edges[e].connector = true;
            if (role == "apron") edges[e].apron = true;
        }
    }
    for (const auto& value : Array(corridors, "zones").GetArray()) {
        const int index = static_cast<int>(zoneIds.size()); if (index >= 31) throw std::runtime_error("Too many Smart Ground zones");
        zoneIds[Text(value, "id")] = index; zonePortals.emplace_back();
        for (const auto& id : Array(value, "standIds").GetArray()) { auto found = locations.find(id.GetString()); if (found != locations.end()) found->second.zones |= std::uint32_t(1) << index; }
        for (const auto& id : Array(value, "edgeIds").GetArray()) edges[Lookup(edgeById, id.GetString())].zones |= std::uint32_t(1) << index;
        for (const auto& id : Array(value, "portalEdgeIds").GetArray()) zonePortals.back().insert(Lookup(edgeById, id.GetString()));
    }
    for (const auto& value : Array(corridors, "specialAreas").GetArray()) for (const auto& id : Array(value, "edgeIds").GetArray()) edges[Lookup(edgeById, id.GetString())].excluded = true;
    auto readRule = [&](const Json& value, bool conditional) {
        Rule rule; rule.configuration = ConfigurationMask(Text(value, "configuration", "both")); rule.movement = MovementMask(Text(value, "movement", "both"));
        const auto direction = Text(value, "direction"); rule.direction = direction == "forward" ? 1 : direction == "reverse" ? -1 : 0; rule.conditional = conditional;
        if (conditional && value.HasMember("condition") && value["condition"].IsObject()) {
            const auto& condition = value["condition"];
            for (const auto& type : Array(condition, "typeCodes").GetArray()) rule.condition.types.emplace_back(type.GetString());
            for (const auto& type : Array(condition, "excludedTypeCodes").GetArray()) rule.condition.excludedTypes.emplace_back(type.GetString());
            if (condition.HasMember("wingspanMetres") && condition["wingspanMetres"].IsObject()) {
                const auto& bounds = condition["wingspanMetres"]; rule.condition.gt = Number(bounds, "gt", -Infinity); rule.condition.gte = Number(bounds, "gte", -Infinity); rule.condition.lt = Number(bounds, "lt", Infinity); rule.condition.lte = Number(bounds, "lte", Infinity);
            }
            for (const auto& flag : Array(condition, "airportFlags").GetArray()) {
                const std::string name = flag.GetString(); if (name == "colored-dual-axis") rule.condition.flags |= 1; else if (name == "southwest-deicing-occupied") rule.condition.flags |= 2; else throw std::runtime_error("Unsupported Smart Ground conditional flag");
            }
        }
        return rule;
    };
    for (const auto* document : { &operations, &corridors, &aircraft }) for (const auto& value : Array(*document, "edgeRules").GetArray()) {
        if (document == &operations && !Boolean(value, "authoritative", true)) continue;
        const int index = static_cast<int>(rules.size()); rules.push_back(readRule(value, document == &aircraft));
        for (const auto& id : Array(value, "edgeIds").GetArray()) edges[Lookup(edgeById, id.GetString())].rules.push_back(index);
    }
    for (const auto* document : { &operations, &aircraft }) for (const auto& value : Array(*document, "turnRules").GetArray()) {
        Turn turn; turn.incoming = Lookup(edgeById, Text(value, "fromEdgeId")); turn.outgoing = Lookup(edgeById, Text(value, "toEdgeId")); turn.rule = readRule(value, document == &aircraft);
        turns[Lookup(nodeById, Text(value, "viaNodeId"))].push_back(std::move(turn));
    }
    for (const auto& value : Array(operations, "clearanceRules").GetArray()) {
        Checkpoint checkpoint; const auto incoming = Text(value, "fromEdgeId"), outgoing = Text(value, "toEdgeId");
        if (!incoming.empty()) checkpoint.incoming = Lookup(edgeById, incoming); if (!outgoing.empty()) checkpoint.outgoing = Lookup(edgeById, outgoing);
        checkpoint.rule = readRule(value, false); checkpoints[Lookup(nodeById, Text(value, "nodeId"))].push_back(checkpoint);
    }
    for (const auto& value : Array(operations, "runwayAccess").GetArray()) {
        Access access; access.locationId = Text(value, "locationId"); access.runway = Text(value, "runway"); access.role = Text(value, "role"); access.ppr = Boolean(value, "pprRequired");
        const auto location = locations.find(access.locationId); if (location == locations.end()) throw std::runtime_error("Unknown Smart Ground runway access"); access.node = location->second.node;
        access.configuration = access.runway.rfind("08", 0) == 0 || access.runway.rfind("09", 0) == 0 ? 1 : 2; accesses.push_back(std::move(access));
    }
    for (const auto& value : Array(corridors, "flowPolicies").GetArray()) {
        Policy policy; policy.id = Text(value, "id"); policy.configuration = ConfigurationMask(Text(value, "configuration", "both")); policy.movement = MovementMask(Text(value, "movement")); policy.priority = static_cast<int>(Number(value, "priority"));
        policy.originZones = ZoneMask(Array(value, "originZoneIds"), zoneIds); policy.destinationZones = ZoneMask(Array(value, "destinationZoneIds"), zoneIds);
        for (const auto& runway : Array(value, "runways").GetArray()) policy.runways.insert(runway.GetString());
        for (const auto& id : Array(value, "destinationNodeIds").GetArray()) policy.destinationNodes.insert(Lookup(nodeById, id.GetString()));
        for (const auto& id : Array(value, "originPortalEdgeIds").GetArray()) policy.originPortals.insert(Lookup(edgeById, id.GetString()));
        for (const auto& id : Array(value, "destinationPortalEdgeIds").GetArray()) policy.destinationPortals.insert(Lookup(edgeById, id.GetString()));
        for (const auto& group : Array(value, "requiredAxisGroups").GetArray()) policy.requiredAxes.push_back(AxisMask(group, axisIds));
        policy.preferred = AxisMask(Array(value, "preferredAxisIds"), axisIds); policy.first = AxisMask(Array(value, "firstMainAxisIds"), axisIds); policy.last = AxisMask(Array(value, "lastMainAxisIds"), axisIds);
        policy.mainRequired = Boolean(value, "requireMainAxis"); policy.differentZones = Boolean(value, "differentZonesOnly"); policies.push_back(std::move(policy));
    }
    std::sort(policies.begin(), policies.end(), [](const Policy& a, const Policy& b) { return a.priority != b.priority ? a.priority > b.priority : a.id < b.id; });
    std::vector<int> parent(nodes.size()); std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](int n) { while (parent[n] != n) n = parent[n]; return n; };
    for (const auto& edge : edges) if (edge.kind == 1) parent[root(edge.from)] = root(edge.to);
    for (const auto& edge : edges) if (edge.kind == 1) { nodes[edge.from].parkingComponent = root(edge.from); nodes[edge.to].parkingComponent = root(edge.to); }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto& edge = edges[i]; const auto a = nodes[edge.from].p, b = nodes[edge.to].p;
        for (int x = static_cast<int>(std::floor((std::min)(a.x, b.x) / 100.0)); x <= static_cast<int>(std::floor((std::max)(a.x, b.x) / 100.0)); ++x)
            for (int y = static_cast<int>(std::floor((std::min)(a.y, b.y) / 100.0)); y <= static_cast<int>(std::floor((std::max)(a.y, b.y) / 100.0)); ++y) cells[Cell(x, y)].push_back(static_cast<int>(i));
    }
    ready = true;
}

Profile Data::EffectiveProfile(const Profile& input) const
{
    Profile profile = input; if (profile.typeCode == "A380") profile.typeCode = "A388";
    const double span = profile.typeCode == "A320" ? 35.8 : profile.typeCode == "A388" ? 79.75 : profile.typeCode == "B744" ? 64.92 : profile.typeCode == "B748" ? 68.4 : 80.0;
    const double length = profile.typeCode == "A320" ? 37.57 : profile.typeCode == "A388" ? 72.73 : profile.typeCode == "B744" ? 70.67 : profile.typeCode == "B748" ? 76.25 : 0.0;
    if (!std::isfinite(profile.wingspanMetres) || profile.wingspanMetres <= 0.0) profile.wingspanMetres = span;
    const double minimumSpan = profile.typeCode == "A320" ? 34.1 : profile.typeCode == "A388" ? 79.75 : profile.typeCode == "B744" ? 64.44 : profile.typeCode == "B748" ? 68.4 : 0.0;
    profile.wingspanMetres = (std::max)(profile.wingspanMetres, minimumSpan);
    if (!std::isfinite(profile.lengthMetres) || profile.lengthMetres <= 0.0) profile.lengthMetres = length > 0.0 ? length : (std::max)(20.0, profile.wingspanMetres * 1.5);
    if (length > 0.0) profile.lengthMetres = (std::max)(profile.lengthMetres, length);
    return profile;
}

bool Data::RuleActive(const Rule& rule, const Context& context) const
{
    if (!(rule.configuration & ConfigurationMask(context.configuration)) || !(rule.movement & MovementMask(context.movement))) return false;
    if (!rule.conditional) return true;
    const auto profile = EffectiveProfile(context.profile); const auto& c = rule.condition;
    const bool unknown = profile.typeCode.empty() || profile.typeCode == "UNKNOWN" || profile.typeCode == "ZZZZ" || profile.typeCode == "CUSTOM";
    auto matchesType = [&](const std::string& type) { return type == profile.typeCode || type == "C5" && profile.typeCode.rfind("C5", 0) == 0; };
    if (!unknown && !c.types.empty() && std::none_of(c.types.begin(), c.types.end(), matchesType)) return false;
    if (!unknown && std::any_of(c.excludedTypes.begin(), c.excludedTypes.end(), matchesType)) return false;
    const double s = profile.wingspanMetres;
    if (s <= c.gt || s < c.gte || s >= c.lt || s > c.lte) return false;
    const unsigned flags = (context.coloredDualAxis ? 1U : 0U) | (context.southwestDeicingOccupied ? 2U : 0U);
    return (flags & c.flags) == c.flags;
}
bool Data::AllowArc(int edgeIndex, int from, const Context& context) const
{
    const auto& edge = edges[edgeIndex]; if (edge.kind == 2 || edge.excluded) return false;
    for (int index : edge.rules) { const auto& rule = rules[index]; if (RuleActive(rule, context) && (rule.direction == 0 || rule.direction == 1 && edge.from != from || rule.direction == -1 && edge.to != from)) return false; }
    return true;
}
bool Data::AllowTurn(int incoming, int node, int outgoing, const Context& context) const
{
    if (incoming < 0) return true;
    if (incoming == outgoing) return false;
    const auto& first = edges[incoming]; const auto& second = edges[outgoing];
    const auto found = turns.find(node); if (found != turns.end()) for (const auto& turn : found->second) if (turn.incoming == incoming && turn.outgoing == outgoing && RuleActive(turn.rule, context)) return false;
    if (first.kind == 0 && second.kind == 0) {
        const auto before = nodes[first.from == node ? first.to : first.from].p, via = nodes[node].p, after = nodes[second.from == node ? second.to : second.from].p;
        const double ux = via.x - before.x, uy = via.y - before.y, vx = after.x - via.x, vy = after.y - via.y, denominator = std::hypot(ux, uy) * std::hypot(vx, vy);
        if (denominator > 1e-10 && (ux * vx + uy * vy) / denominator <= -std::sqrt(3.0) / 2.0 + 1e-10) return false;
    }
    if (nodes[node].id.rfind("intersection:", 0) == 0 && (first.sourceSegment.empty() || first.sourceSegment != second.sourceSegment)) return false;
    return true;
}
int Data::ParkingPhase(int edgeIndex, int phase, int origin, int destination) const
{
    const auto& edge = edges[edgeIndex]; if (edge.kind != 1) return phase == 2 ? -1 : 1;
    const int component = nodes[edge.from].parkingComponent;
    if (phase == 0 && component >= 0 && component == nodes[origin].parkingComponent) return 0;
    if (component >= 0 && component == nodes[destination].parkingComponent) return 2;
    return -1;
}
bool Data::IsJunction(int node) const
{
    int taxi = 0; std::unordered_set<std::string> names;
    for (int e : nodes[node].edges) if (edges[e].kind == 0) { taxi++; if (!edges[e].name.empty()) names.insert(edges[e].name); }
    return taxi >= 3 || taxi == 2 && names.size() >= 2;
}
std::vector<int> Data::NearbyEdges(Point point, double radius) const
{
    std::unordered_set<int> seen;
    for (int x = static_cast<int>(std::floor((point.x - radius) / 100.0)); x <= static_cast<int>(std::floor((point.x + radius) / 100.0)); ++x)
        for (int y = static_cast<int>(std::floor((point.y - radius) / 100.0)); y <= static_cast<int>(std::floor((point.y + radius) / 100.0)); ++y) { const auto found = cells.find(Cell(x, y)); if (found != cells.end()) seen.insert(found->second.begin(), found->second.end()); }
    return { seen.begin(), seen.end() };
}
}
