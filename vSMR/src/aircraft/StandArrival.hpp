#pragma once

#include "aircraft/GroundState.hpp"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace VsmrStand
{
    std::string NormalizeAirport(std::string_view airport);
    std::string NormalizeName(std::string_view stand);
    std::string Key(std::string_view airport, std::string_view stand);
    bool IsStandLabel(std::string_view labelClass);
    bool IsArrival(std::string_view airport, std::string_view origin, std::string_view destination);
    bool CanResetStatus(std::string_view status);

    struct Zone
    {
        std::string name;
        double latitude = 0.0;
        double longitude = 0.0;
        double radiusMeters = 0.0;
    };
    inline constexpr std::size_t MaximumZoneResponseBytes = 2U * 1024U * 1024U;
    // RampAgent's /api/airports/{ICAO}/stands: Coordinates = lat:lon:radius(m).
    bool ParseZones(std::string_view json, std::vector<Zone>& zones);
    std::string ContainingZone(const std::vector<Zone>& zones, double latitude,
        double longitude, std::string_view assignedStand);

    struct Observation
    {
        std::string flightIdentity;
        std::string zone;
        std::uint64_t report = 0; // Advances only on a real EuroScope position callback.
        std::uint64_t nowMilliseconds = 0;
        int ageSeconds = 0;
        int speedKnots = 0;
        bool eligible = false;
    };
    class ArrivalTracker
    {
    public:
        bool Update(const std::string& callsign, const Observation& observation);
        void Complete(const std::string& callsign);
        void Forget(const std::string& callsign) { states_.erase(callsign); }
        void Clear() { states_.clear(); }
        void Retain(const std::set<std::string>& callsigns);
    private:
        struct State
        {
            std::string identity, zone;
            std::uint64_t report = 0, first = 0, last = 0;
            unsigned samples = 0;
            bool completed = false;
        };
        std::map<std::string, State> states_;
    };

    // EuroScope's ground-status setter is the scratchpad setter. Preserve the
    // controller's text and real speed assignments, including on a rejected write.
    template<class AssignedData>
    bool ResetToNoStatus(AssignedData& data, const std::string& previousStatus)
    {
        const int previousSpeed = data.GetAssignedSpeed();
        const bool clearShared = VsmrGroundStateSync::IsReservedAssignedSpeed(previousSpeed);
        const char* raw = data.GetScratchPadString();
        const std::string scratchpad = raw != nullptr ? raw : "";
        if (clearShared && (!data.SetAssignedSpeed(0) || data.GetAssignedSpeed() != 0))
            return false;
        if (!data.SetScratchPadString("NSTS"))
        {
            if (clearShared) (void)data.SetAssignedSpeed(previousSpeed);
            return false;
        }
        raw = data.GetScratchPadString();
        if (scratchpad != (raw != nullptr ? raw : "") && !data.SetScratchPadString(scratchpad.c_str()))
        {
            if (!previousStatus.empty()) (void)data.SetScratchPadString(previousStatus.c_str());
            (void)data.SetScratchPadString(scratchpad.c_str());
            if (clearShared) (void)data.SetAssignedSpeed(previousSpeed);
            return false;
        }
        return true;
    }
}
