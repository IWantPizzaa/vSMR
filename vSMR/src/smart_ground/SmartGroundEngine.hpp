#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// SDK-independent owned values. Time is monotonic seconds, distances metres,
// speed knots at the ingestion boundary, heading true clockwise from north.
namespace VsmrSmartGround
{
struct Profile
{
    std::string typeCode;
    double wingspanMetres = 0.0;
    double lengthMetres = 0.0;
};
struct Context
{
    std::string airport = "LFPG";
    std::string configuration = "west";
    std::string movement = "departure";
    std::string standId;
    std::string runway;
    std::string holdingPointId;
    std::vector<std::string> activeArrivalRunways;
    std::vector<std::string> activeDepartureRunways;
    Profile profile;
    bool coloredDualAxis = false;
    bool southwestDeicingOccupied = false;
};
struct Sample
{
    std::string callsign;
    std::string sessionId;
    std::uint64_t sequence = 0;
    double receivedAt = 0.0;
    double reportedAgeSeconds = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double headingTrueDegrees = -1.0;
    double groundSpeedKnots = 0.0;
    double altitudeFeet = 0.0;
    bool surfaceTarget = true;
};
struct RoutePoint
{
    double latitude = 0.0;
    double longitude = 0.0;
    std::string nodeId;
    bool editable = false;
    bool controllerSegment = false; // Segment arriving at this point.
};
struct Snapshot
{
    std::string callsign;
    std::uint64_t previewRevision = 0;
    bool routeAvailable = false;
    bool controllerRoute = false;
    bool provisionalRoute = true;
    bool recoveryProposed = false;
    bool collisionAlert = false;
    bool collisionCritical = false;
    bool offTrackAlert = false;
    bool offTrackDeviation = false;
    bool suspectedOffTrack = false;
    std::string collisionWith;
    std::string collisionMessage;
    std::string offTrackMessage;
    std::string dataQuality;
    std::string routeMessage;
    std::string taxiwayRoute;
    double reportAgeSeconds = 0.0;
    double uncertaintyMetres = 0.0;
    double distanceRemainingMetres = 0.0;
    std::vector<RoutePoint> remainingRoute;
};
struct RouteResult
{
    bool available = false;
    std::vector<std::string> nodeIds;
    std::vector<std::string> edgeIds;
    double distanceMetres = 0.0;
    std::string error;
};

class Engine
{
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    bool Load(const std::string& dataDirectory, std::string& error);
    bool IsReady() const;
    void SetContext(const std::string& callsign, const Context& context);
    bool Observe(const Sample& sample);
    void Advance(double nowSeconds);
    Snapshot GetSnapshot(const std::string& callsign, double nowSeconds) const;
    std::vector<Snapshot> GetSnapshots(double nowSeconds) const;
    bool EditRoute(const std::string& callsign, const std::string& anchorNodeId, const std::string& targetNodeId, std::string& error);
    bool PreviewEditRoute(const std::string& callsign, const std::string& anchorNodeId, const std::string& targetNodeId, Snapshot& preview, std::string& error) const;
    RoutePoint FindIntersection(double latitude, double longitude, double maximumDistanceMetres = 60.0) const;
    bool UndoRoute(const std::string& callsign, std::string& error);
    bool SetTaxiwayRoute(const std::string& callsign, const std::string& taxiways, std::string& error);
    bool ResetRoute(const std::string& callsign, std::string& error);
    void DisregardOffTrack(const std::string& callsign, bool permanent);
    std::string NearestIntersection(double latitude, double longitude, double maximumDistanceMetres = 60.0) const;
    RouteResult FindRoute(const std::string& originLocationId, const std::string& destinationLocationId, const Context& context) const;
    void Forget(const std::string& callsign);
    void Clear();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Engine& Service();
}
