#pragma once
#include "smart_ground/SmartGroundEngine.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace VsmrSmartGround
{
constexpr double Pi = 3.14159265358979323846;
constexpr double KnotsToMetres = 0.514444;
constexpr double Infinity = std::numeric_limits<double>::infinity();
struct Point { double x = 0.0, y = 0.0; };
inline double Distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline double Wrap(double x) { return std::atan2(std::sin(x), std::cos(x)); }
inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
inline Point Project(double latitude, double longitude) { return { (longitude - 2.5479) * 111320.0 * std::cos(49.0097 * Pi / 180.0), (latitude - 49.0097) * 111320.0 }; }
inline RoutePoint Geographic(Point point) { RoutePoint p; p.latitude = 49.0097 + point.y / 111320.0; p.longitude = 2.5479 + point.x / (111320.0 * std::cos(49.0097 * Pi / 180.0)); return p; }
inline int ConfigurationMask(const std::string& s) { return s == "east" ? 1 : s == "west" ? 2 : 3; }
inline int MovementMask(const std::string& s) { return s == "departure" ? 1 : s == "arrival" ? 2 : s == "transfer" ? 4 : 7; }
struct Node { std::string id; Point p; double latitude = 0.0, longitude = 0.0; int parkingComponent = -1; std::vector<int> edges; };
struct Edge { std::string id, name, sourceSegment; int from = -1, to = -1, kind = 0; double length = 0.0; std::uint64_t mainAxes = 0; std::uint32_t zones = 0; bool connector = false, apron = false, excluded = false; std::vector<int> rules; };
std::string TaxiwayName(const Edge& edge);
struct Condition
{
    std::vector<std::string> types, excludedTypes;
    double gt = -Infinity, gte = -Infinity, lt = Infinity, lte = Infinity;
    unsigned flags = 0;
};
struct Rule { int configuration = 3, movement = 7, direction = 0; bool conditional = false; Condition condition; };
struct Turn { int incoming = -1, outgoing = -1; Rule rule; };
struct Checkpoint { int incoming = -1, outgoing = -1; Rule rule; };
struct Location { std::string id, name; int node = -1, kind = 0; std::uint32_t zones = 0; };
struct Access { std::string locationId, runway, role; int node = -1, configuration = 3; bool ppr = false; };
struct Policy
{
    std::string id; int configuration = 3, movement = 7, priority = 0;
    std::uint32_t originZones = 0, destinationZones = 0;
    std::unordered_set<std::string> runways;
    std::unordered_set<int> destinationNodes, originPortals, destinationPortals;
    std::vector<std::uint64_t> requiredAxes;
    std::uint64_t preferred = 0, first = 0, last = 0;
    bool mainRequired = false, differentZones = false;
};
struct FlowContext { std::uint32_t originZones = 0, destinationZones = 0; std::vector<std::unordered_set<int>> stages; std::unordered_set<int> forbidden; std::uint64_t first = 0, last = 0; };
struct FlowState
{
    int stage = 0, mainPhase = 0; bool originExited = false, destinationEntered = false, lastMatches = false; std::uint64_t lastAxes = 0;
    bool operator==(const FlowState& s) const { return stage == s.stage && mainPhase == s.mainPhase && originExited == s.originExited && destinationEntered == s.destinationEntered && lastMatches == s.lastMatches && lastAxes == s.lastAxes; }
};
struct Path
{
    std::vector<int> nodes, edges; std::vector<double> cumulative; std::vector<int> waypoints;
    std::vector<bool> controllerSegments;
    std::vector<std::string> requestedTaxiways;
    double distance = 0.0; bool controller = false; unsigned revision = 0;
    bool empty() const { return edges.empty(); }
};
void MarkControllerSegments(Path& candidate, const Path& standard);
struct Match { bool valid = false, ambiguous = false, opposite = false; int edge = -1, from = -1, to = -1, routeIndex = -1; double progress = 0.0, routeDistance = 0.0, distance = Infinity, score = Infinity, heading = 0.0; Point p; };
struct PhysicalMotion { double start = 0.0, end = 0.0; Point p, velocity; double body = 0.0, padding = 0.0; };
double SweptEnvelopeProximity(const Profile& first, const std::vector<PhysicalMotion>& a, const Profile& second, const std::vector<PhysicalMotion>& b, double marginMetres);
struct Data
{
    bool ready = false;
    std::vector<Node> nodes; std::vector<Edge> edges; std::vector<Rule> rules;
    std::unordered_map<std::string, int> nodeById, edgeById;
    std::unordered_map<std::string, Location> locations;
    std::unordered_map<int, std::vector<Turn>> turns;
    std::unordered_map<int, std::vector<Checkpoint>> checkpoints;
    std::vector<Access> accesses; std::vector<Policy> policies;
    std::unordered_map<std::string, int> axisIds, zoneIds;
    std::vector<std::unordered_set<int>> axisEdges, zonePortals;
    std::unordered_map<std::uint64_t, std::vector<int>> cells;
    void Load(const std::string& directory);
    bool RuleActive(const Rule& rule, const Context& context) const;
    bool AllowArc(int edge, int from, const Context& context) const;
    bool AllowTurn(int incoming, int node, int outgoing, const Context& context) const;
    int ParkingPhase(int edge, int phase, int origin, int destination) const;
    bool IsJunction(int node) const;
    Profile EffectiveProfile(const Profile& profile) const;
    FlowContext Compile(const Context& context, const std::string& origin, const std::string& destination, int from, int to, bool controller) const;
    bool Step(const FlowContext& context, const FlowState& previous, int edge, FlowState& next, double& apron, int& reentries, int& axisChanges) const;
    Path Search(int from, int to, const Context& context, const std::string& originId, const std::string& destinationId, bool controller = false, const Path* fixedPrefix = nullptr, const std::vector<int>& waypoints = {}, const std::vector<std::string>& taxiways = {}) const;
    bool Validate(const Path& path, const Context& context) const;
    void Rebuild(Path& path) const;
    Match Associate(Point p, double heading, double speed, const Path& path, double previousDistance, double expectedAdvance, bool useContinuity) const;
    Match PointAt(const Path& path, double distance) const;
    double NextStop(const Path& path, const Context& context, double observedDistance) const;
    std::vector<int> NearbyEdges(Point point, double radius) const;
};
struct Track
{
    std::uint64_t previewRevision = 0;
    Context context; Sample sample; bool hasSample = false, hasContext = false;
    Point raw, previousRaw; double speed = 0.0, movementHeading = 0.0, bodyHeading = 0.0, progress = 0.0;
    bool movementHeadingMeasured = false;
    Match match; Path path, standard, recovery; std::vector<Path> editHistory;
    std::string originId, destinationId, routeError;
    bool offTrack = false, suspect = false; int evidence = 0, normalEvidence = 0;
    bool disregardEpisode = false, disregardPermanent = false, reanchorRoute = false;
    double evidenceStart = 0.0, lastEvidence = -Infinity; int deviationKind = 0;
    bool collision = false, critical = false; std::string collisionWith, collisionMessage;
    double routeRetryAfter = 0.0;
};
struct CollisionState
{
    std::string first, second; double firstEvidence = 0.0, lastPositive = 0.0, clearStart = 0.0, timeToConflict = Infinity;
    std::uint64_t sequenceA = 0, sequenceB = 0; int positives = 0, negatives = 0; bool active = false, critical = false, hasReports = false;
};
struct Engine::Impl
{
    mutable std::mutex mutex;
    Data data; std::unordered_map<std::string, Track> tracks; std::unordered_map<std::string, CollisionState> pairs;
    double lastAdvance = -Infinity;
    std::uint64_t nextPreviewRevision = 1;
    void EnsureRoute(Track& track, double now);
    Snapshot SnapshotFor(const std::string& callsign, const Track& track, double now) const;
    void EvaluateCollisions(double now);
    bool CurrentPrefix(const Track& track, Path& prefix, std::string& error) const;
    bool BuildRouteEdit(const Track& track, const std::string& anchorId, const std::string& targetId, Path& candidate, std::string& error) const;
    void ApplyRoute(Track& track, Path candidate, bool controller) const;
    void CommitRoute(Track& track, Path candidate, bool controller);
};
}
