#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include <filesystem>
#include <fstream>

namespace
{
    std::string Text(const char* value) { return value != nullptr ? value : ""; }
    std::string ReadCache(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > VsmrStand::MaximumZoneResponseBytes) return {};
        std::ifstream stream(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return stream && stream.peek() == std::char_traits<char>::eof() ? bytes : std::string{};
    }
    void WriteCache(const std::filesystem::path& path, const std::string& json)
    {
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += L"." + std::to_wstring(::GetCurrentProcessId()) + L"." +
            std::to_wstring(::GetCurrentThreadId()) + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(json.data(), static_cast<std::streamsize>(json.size()));
            stream.close();
            if (!stream) { ::DeleteFileW(temporary.c_str()); return; }
        }
        if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ::DeleteFileW(temporary.c_str());
    }
}

void CSMRPlugin::SetStandOptions(bool highlight, bool reset)
{
    HighlightArrivalStand = highlight;
    ResetStatusAtStand = reset;
    SaveDataToSettings("HighlightArrivalStand", "Highlight selected arrival stand on AVISO", highlight ? "1" : "0");
    SaveDataToSettings("ResetStatusAtStand", "Reset arrival status at stand", reset ? "1" : "0");
    if (!reset) { StandArrivals.Clear(); StandPositionReports.clear(); }
    for (auto* radar : RadarScreensOpened)
        if (radar != nullptr && !radar->IsShutdownRequested()) radar->RequestRefresh();
}

void CSMRPlugin::OnRadarTargetPositionUpdate(CRadarTarget target)
{
    // Counts fresh reports for stand detection only. Rendering always uses the
    // untouched SDK position; no interpolation, extrapolation or repaint timer.
    if (!ResetStatusAtStand || PluginShutdownRequested.load(std::memory_order_relaxed) || !target.IsValid()) return;
    try
    {
        const auto callsign = VsmrPluginBridge::NormalizeCallsign(Text(target.GetCallsign()));
        if (!callsign.empty()) StandPositionReports[callsign] = ++StandReportSerial;
    }
    catch (...) { Logger::info("Stand position report could not be recorded"); }
}

void CSMRPlugin::QueueStandCatalog(const std::string& airport, const std::string& dataPath)
{
    const auto now = ::GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(StandCatalogMutex);
        auto& entry = StandCatalogs[airport];
        if (entry.inFlight || now < entry.nextAttempt) return;
        entry.inFlight = true;
        entry.nextAttempt = now + 60000U;
    }
    const auto cachePath = std::filesystem::u8path(dataPath) / "UserData" / "StandZones" / (airport + ".json");
    if (QueueNetworkJob([this, airport, cachePath] {
        bool success = false;
        try
        {
            std::vector<VsmrStand::Zone> cached;
            if (VsmrStand::ParseZones(ReadCache(cachePath), cached))
            {
                std::lock_guard<std::mutex> lock(StandCatalogMutex);
                auto& entry = StandCatalogs[airport];
                if (!entry.zones) entry.zones = std::make_shared<const std::vector<VsmrStand::Zone>>(std::move(cached));
            }
            const auto json = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
                "https://rampagent.vatsim.fr/api/airports/" + airport + "/stands", 6000,
                &NetworkCancellationRequested, VsmrStand::MaximumZoneResponseBytes);
            std::vector<VsmrStand::Zone> zones;
            if (!NetworkCancellationRequested.load(std::memory_order_acquire) && VsmrStand::ParseZones(json, zones))
            {
                {
                    std::lock_guard<std::mutex> lock(StandCatalogMutex);
                    StandCatalogs[airport].zones = std::make_shared<const std::vector<VsmrStand::Zone>>(std::move(zones));
                }
                success = true;
                WriteCache(cachePath, json);
            }
        }
        catch (...) { Logger::info("Stand zone request failed for " + airport); }
        std::lock_guard<std::mutex> lock(StandCatalogMutex);
        auto& entry = StandCatalogs[airport];
        entry.inFlight = false;
        entry.nextAttempt = ::GetTickCount64() + (success ? 3600000U : 60000U);
    })) return;
    std::lock_guard<std::mutex> lock(StandCatalogMutex);
    StandCatalogs[airport].inFlight = false;
}

void CSMRPlugin::UpdateArrivalsAtStands()
{
    if (HighlightArrivalStand)
    {
        // EuroScope lists can change ASEL without a radar click or a new report.
        // Observe selection through the existing SDK timer; create no new timer.
        const auto selected = FlightPlanSelectASEL();
        const auto callsign = selected.IsValid() ? Text(selected.GetCallsign()) : std::string{};
        if (callsign != SelectedStandCallsign)
        {
            SelectedStandCallsign = callsign;
            FlightDataRefreshPending.store(true, std::memory_order_release);
        }
    }
    if (!ResetStatusAtStand) return;
    std::map<std::string, std::shared_ptr<const std::vector<VsmrStand::Zone>>> airports;
    for (auto* radar : RadarScreensOpened)
    {
        if (radar == nullptr || radar->IsShutdownRequested()) continue;
        const auto airport = VsmrStand::NormalizeAirport(radar->getActiveAirport());
        if (airport.empty() || airports.count(airport)) continue;
        QueueStandCatalog(airport, radar->GetDataPath());
        std::lock_guard<std::mutex> lock(StandCatalogMutex);
        airports.emplace(airport, StandCatalogs[airport].zones);
    }
    if (airports.empty()) { StandArrivals.Clear(); StandPositionReports.clear(); return; }
    std::set<std::string> seen, radarCallsigns;
    const auto now = ::GetTickCount64();
    for (auto flight = FlightPlanSelectFirst(); flight.IsValid(); flight = FlightPlanSelectNext(flight))
    {
        const auto callsign = VsmrPluginBridge::NormalizeCallsign(Text(flight.GetCallsign()));
        const auto plan = flight.GetFlightPlanData();
        const auto destination = VsmrStand::NormalizeAirport(Text(plan.GetDestination()));
        const auto airport = airports.find(destination);
        const auto origin = Text(plan.GetOrigin());
        const auto target = flight.GetCorrelatedRadarTarget();
        const auto radarCallsign = target.IsValid() ? VsmrPluginBridge::NormalizeCallsign(Text(target.GetCallsign())) : std::string{};
        if (!radarCallsign.empty()) radarCallsigns.insert(radarCallsign);
        if (callsign.empty() || airport == airports.end() || !airport->second ||
            !VsmrStand::IsArrival(destination, origin, destination) || !target.IsValid()) continue;
        const auto position = target.GetPosition();
        if (!position.IsValid()) continue;
        seen.insert(callsign);
        VsmrRampAgent::AircraftData assignment;
        (void)VsmrRampAgent::TryGetAircraftData(callsign, assignment);
        const auto point = position.GetPosition();
        VsmrStand::Observation observation;
        observation.flightIdentity = origin + '|' + destination + '|' + Text(plan.GetEstimatedDepartureTime());
        const auto report = StandPositionReports.find(radarCallsign);
        if (report != StandPositionReports.end()) observation.report = report->second;
        observation.nowMilliseconds = now;
        observation.ageSeconds = position.GetReceivedTime();
        observation.speedKnots = position.GetReportedGS();
        const auto status = Text(flight.GetGroundState());
        const auto controller = Text(flight.GetTrackingControllerCallsign());
        observation.eligible = VsmrStand::CanResetStatus(status) &&
            (controller.empty() || flight.GetTrackingControllerIsMe());
        if (observation.eligible && observation.speedKnots >= 0 && observation.speedKnots <= 1)
            observation.zone = VsmrStand::ContainingZone(*airport->second, point.m_Latitude, point.m_Longitude, assignment.stand);
        if (!StandArrivals.Update(callsign, observation)) continue;
        auto assigned = flight.GetControllerAssignedData();
        if (!VsmrStand::ResetToNoStatus(assigned, status)) continue;
        StandArrivals.Complete(callsign);
        VsmrGroundState::ForgetAircraft(callsign.c_str());
        FlightDataRefreshPending.store(true, std::memory_order_release);
        Logger::info("Arrival at stand: " + callsign + " " + destination + " " + observation.zone + " -> NSTS");
    }
    StandArrivals.Retain(seen);
    for (auto it = StandPositionReports.begin(); it != StandPositionReports.end();)
        if (!radarCallsigns.count(it->first)) it = StandPositionReports.erase(it); else ++it;
}
