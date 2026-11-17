#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/SmartGroundIntegration.hpp"
#include "smart_ground/SmartGroundEngine.hpp"
#include "smart_ground/SmartGroundActivation.hpp"
#include "bootstrap/RuntimeContext.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "scene/RadarScene.hpp"
#include "safety/Rimcas.hpp"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    struct SourceState {
        std::uint64_t generation = 0; std::uint64_t sequence = 0; double lastReceipt = 0.0;
        VsmrSmartGround::Context context; bool hasContext = false;
    };
    std::mutex StateMutex;
    std::unordered_map<std::string, SourceState> Sources;
    std::uint64_t NextGeneration = 1;
    VsmrSmartGround::Activation Activation;
    bool Attempted = false;
    bool Ready = false;
    double Now() { return static_cast<double>(::GetTickCount64()) / 1000.0; }
    std::string NormalizeRunway(const std::string& value)
    {
        const auto result = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(value));
        return result.size() >= 2 && result.size() <= 3 && result[0] >= '0' && result[0] <= '3' && result[1] >= '0' && result[1] <= '9'
            && (result.size() == 2 || result[2] == 'L' || result[2] == 'R' || result[2] == 'C') ? result : std::string();
    }
    std::string Configuration(const VsmrScene::RadarScene& scene)
    {
        bool east = false, west = false;
        for (const auto& item : scene.airport.runwayStatuses) {
            if (item.second == CRimcas::CLSD) continue;
            const auto runway = NormalizeRunway(item.first);
            east = east || runway.rfind("08", 0) == 0 || runway.rfind("09", 0) == 0;
            west = west || runway.rfind("26", 0) == 0 || runway.rfind("27", 0) == 0;
        }
        if (east != west) return east ? "east" : "west";
        const auto provider = VsmrVsid::GetInterfaceState("LFPG");
        if (provider.paris && provider.paris->pg == VsmrParis::Flow::East) return "east";
        if (provider.paris && provider.paris->pg == VsmrParis::Flow::West) return "west";
        return {};
    }
}

bool VsmrSmartGroundBridge::IsEnabled()
{
    return Activation.IsEnabled();
}

void VsmrSmartGroundBridge::SetEnabled(bool enabled)
{
    if (!Activation.SetEnabled(enabled)) return;
    if (enabled) Initialize();
    else Shutdown();
}

void VsmrSmartGroundBridge::Initialize()
{
    if (!IsEnabled()) return;
    std::lock_guard<std::mutex> lock(StateMutex);
    if (Attempted) return;
    Attempted = true;
    auto root = VsmrRuntimeContext::IsConfigured() ? VsmrRuntimeContext::DataRoot()
        : std::filesystem::u8path(Logger::DLL_PATH) / "vSMR_Data";
    std::string error;
    Ready = VsmrSmartGround::Service().Load((root / "SmartGround").u8string(), error);
    if (!Ready) Logger::info("Smart Ground unavailable: " + error);
}

void VsmrSmartGroundBridge::Observe(EuroScopePlugIn::CRadarTarget target, bool onlyIfMissing)
{
    if (!IsEnabled()) return;
    Initialize();
    if (!Ready || !target.IsValid()) return;
    const auto position = target.GetPosition();
    const char* raw = target.GetCallsign();
    if (!position.IsValid() || raw == nullptr || *raw == '\0') return;
    const auto callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(raw));
    if (onlyIfMissing) {
        std::lock_guard<std::mutex> lock(StateMutex);
        if (Sources.count(callsign)) return;
    }
    const auto geo = position.GetPosition();
    const double speed = static_cast<double>(position.GetReportedGS() == 0 ? target.GetGS() : position.GetReportedGS());
    const double altitude = static_cast<double>(position.GetPressureAltitude());
    const bool surface = std::isfinite(geo.m_Latitude) && std::isfinite(geo.m_Longitude)
        && geo.m_Latitude >= 48.965 && geo.m_Latitude <= 49.06 && geo.m_Longitude >= 2.43 && geo.m_Longitude <= 2.70
        && altitude >= 100.0 && altitude <= 950.0 && speed >= 0.0 && speed <= 80.0;
    if (!surface) { Forget(callsign); return; }
    const double now = Now();
    VsmrSmartGround::Sample sample;
    sample.callsign = callsign;
    {
        std::lock_guard<std::mutex> lock(StateMutex);
        if (onlyIfMissing && Sources.count(callsign)) return;
        auto& source = Sources[callsign];
        if (source.generation == 0 || now - source.lastReceipt > 120.0) {
            source.generation = NextGeneration++; source.sequence = 0;
        }
        source.lastReceipt = now;
        sample.sessionId = std::to_string(source.generation);
        sample.sequence = ++source.sequence;
    }
    sample.receivedAt = now;
    sample.reportedAgeSeconds = static_cast<double>((std::max)(0, position.GetReceivedTime()));
    sample.latitude = geo.m_Latitude; sample.longitude = geo.m_Longitude;
    sample.groundSpeedKnots = speed; sample.altitudeFeet = altitude; sample.surfaceTarget = surface;
    const double heading = static_cast<double>(position.GetReportedHeadingTrueNorth());
    sample.headingTrueDegrees = std::isfinite(heading) && heading >= 0.0 && heading < 360.0 ? heading : -1.0;
    VsmrSmartGround::Service().Observe(sample);
}

void VsmrSmartGroundBridge::Forget(const std::string& callsign)
{
    { std::lock_guard<std::mutex> lock(StateMutex); Sources.erase(callsign); }
    VsmrSmartGround::Service().Forget(callsign);
}

void VsmrSmartGroundBridge::UpdateHoldingPoint(const std::string& callsign, const std::string& holdingPoint)
{
    if (!IsEnabled()) return;
    const auto key = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
    VsmrSmartGround::Context context;
    {
        std::lock_guard<std::mutex> lock(StateMutex);
        const auto source = Sources.find(key);
        if (source == Sources.end() || !source->second.hasContext || source->second.context.movement != "departure") return;
        if (source->second.context.holdingPointId == holdingPoint) return;
        source->second.context.holdingPointId = holdingPoint;
        context = source->second.context;
    }
    // Changing controller intent is independent of receiving another position.
    // SetContext preserves report age and rebuilds from the observed position.
    VsmrSmartGround::Service().SetContext(key, context);
}

bool VsmrSmartGroundBridge::Advance()
{
    if (!IsEnabled() || !Ready) return false;
    const double now = Now();
    {
        std::lock_guard<std::mutex> lock(StateMutex);
        for (auto it = Sources.begin(); it != Sources.end();) {
            if (now - it->second.lastReceipt > 120.0) { VsmrSmartGround::Service().Forget(it->first); it = Sources.erase(it); }
            else ++it;
        }
        if (Sources.empty()) return false;
    }
    VsmrSmartGround::Service().Advance(now);
    return true;
}

void VsmrSmartGroundBridge::UpdateScene(VsmrScene::RadarScene& scene)
{
    for (auto& target : scene.targets) target.smartGround = {};
    if (!IsEnabled() || scene.airport.icao != "LFPG") return;
    Initialize(); if (!Ready) return;
    const std::string configuration = Configuration(scene);
    std::unordered_set<std::string> observed;
    { std::lock_guard<std::mutex> lock(StateMutex); for (const auto& item : Sources) observed.insert(item.first); }
    for (const auto& target : scene.targets) {
        if (observed.count(target.normalizedCallsign) == 0) continue;
        VsmrSmartGround::Context context;
        context.airport = "LFPG"; context.configuration = configuration;
        // Presentation roles require correlation. Replay flight plans can still
        // identify an arrival while uncorrelated: never infer a departure from it.
        context.movement = ToUpperAsciiCopy(target.origin) == "LFPG" ? "departure"
            : ToUpperAsciiCopy(target.destination) == "LFPG" ? "arrival" : "unknown";
        context.profile = { ToUpperAsciiCopy(target.aircraftType), target.style.wingspanMeters, target.style.lengthMeters };
        VsmrRampAgent::AircraftData ramp;
        if (VsmrRampAgent::TryGetAircraftData(target.callsign, ramp) && !ramp.stand.empty()) context.standId = "stand:" + ramp.stand;
        context.runway = NormalizeRunway(context.movement == "arrival" ? target.arrivalRunway : target.departureRunway);
        VsmrVsid::AircraftData vsid;
        if (context.movement == "departure" && VsmrVsid::TryGetAircraftData(target.callsign, vsid) && !NormalizeRunway(vsid.runway).empty()) context.runway = NormalizeRunway(vsid.runway);
        if (context.configuration.empty() && !context.runway.empty()) context.configuration = context.runway.rfind("08", 0) == 0 || context.runway.rfind("09", 0) == 0 ? "east" : "west";
        context.holdingPointId = target.holdingPoint;
        for (const auto& runway : scene.airport.runwayStatuses) {
            if (runway.second == CRimcas::DEP || runway.second == CRimcas::BOTH) context.activeDepartureRunways.push_back(runway.first);
            if (runway.second == CRimcas::ARR || runway.second == CRimcas::BOTH) context.activeArrivalRunways.push_back(runway.first);
        }
        {
            std::lock_guard<std::mutex> lock(StateMutex);
            const auto source = Sources.find(target.normalizedCallsign);
            if (source != Sources.end()) { source->second.context = context; source->second.hasContext = true; }
        }
        VsmrSmartGround::Service().SetContext(target.normalizedCallsign, context);
    }
    VsmrSmartGround::Service().Advance(Now());
    const double now = Now();
    for (auto& target : scene.targets) {
        const auto snapshot = VsmrSmartGround::Service().GetSnapshot(target.normalizedCallsign, now);
        target.smartGround.collision = snapshot.collisionAlert;
        target.smartGround.offTrack = snapshot.offTrackAlert;
        target.smartGround.critical = snapshot.collisionCritical;
        target.smartGround.conflictingCallsign = snapshot.collisionWith;
        target.smartGround.reason = snapshot.collisionMessage;
        if (!snapshot.offTrackMessage.empty()) {
            if (!target.smartGround.reason.empty()) target.smartGround.reason += " | ";
            target.smartGround.reason += snapshot.offTrackMessage;
        }
    }
}

void VsmrSmartGroundBridge::Shutdown()
{
    Activation.SetEnabled(false);
    { std::lock_guard<std::mutex> lock(StateMutex); Sources.clear(); Ready = false; Attempted = false; }
    VsmrSmartGround::Service().Clear();
}
