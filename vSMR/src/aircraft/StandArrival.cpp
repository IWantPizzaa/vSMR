#include "aircraft/StandArrival.hpp"
#include "shared/JsonDocument.hpp"
#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>

namespace VsmrStand
{
namespace
{
    std::string Compact(std::string_view input)
    {
        std::string result;
        for (unsigned char c : input)
        {
            if (c == ' ' || c == '\t' || c == '-' || c == '_') continue;
            if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return {};
            result.push_back(static_cast<char>(c));
        }
        return result;
    }
    bool Number(const std::string& value, double& result)
    {
        std::istringstream stream(value);
        stream.imbue(std::locale::classic());
        stream >> result;
        return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(result);
    }
    double Distance(double lat1, double lon1, double lat2, double lon2)
    {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        const double a = std::sin((lat2-lat1)*radians/2.0);
        const double b = std::sin((lon2-lon1)*radians/2.0);
        const double h = a*a + std::cos(lat1*radians)*std::cos(lat2*radians)*b*b;
        return 12742000.0 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
    }
}
std::string NormalizeAirport(std::string_view airport)
{
    const auto normalized = Compact(airport);
    return normalized.size() == 4U ? normalized : std::string{};
}
std::string NormalizeName(std::string_view stand)
{
    if (stand.size() > 64U) return {};
    const auto compact = Compact(stand);
    std::string result;
    for (std::size_t i = 0; i < compact.size(); ++i)
    {
        if (compact[i] == '0' && (i == 0 || compact[i-1] < '0' || compact[i-1] > '9'))
            while (i+1U < compact.size() && compact[i] == '0' && compact[i+1U] >= '0' && compact[i+1U] <= '9') ++i;
        result.push_back(compact[i]);
    }
    return result;
}
std::string Key(std::string_view airport, std::string_view stand)
{
    const auto code = NormalizeAirport(airport), name = NormalizeName(stand);
    return code.empty() || name.empty() ? std::string{} : code + ':' + name;
}
bool IsStandLabel(std::string_view labelClass)
{
    const auto value = Compact(labelClass);
    return value == "GATES" || value == "GATE" || value == "STANDS" || value == "STAND" || value == "PARKING";
}
bool IsArrival(std::string_view airport, std::string_view origin, std::string_view destination)
{
    const auto code = NormalizeAirport(airport);
    return !code.empty() && NormalizeAirport(destination) == code && NormalizeAirport(origin) != code;
}
bool CanResetStatus(std::string_view status)
{
    const auto value = Compact(status);
    // DEPA may still be carried from the departure airport after landing.
    // The caller first requires an arrival flight plan at the active airport.
    return value == "ARR" || value == "TXIN" || value == "TAXIIN" || value == "TAXI" || value == "DEPA" ||
        value == "PARK" || value == "PARKED";
}
bool ParseZones(std::string_view json, std::vector<Zone>& zones)
{
    zones.clear();
    if (json.empty() || json.size() > MaximumZoneResponseBytes) return false;
    rapidjson::Document document;
    VsmrJson::ParseDocument(document, json);
    if (document.HasParseError() || !document.IsObject() || document.MemberCount() > 10000U) return false;
    std::set<std::string> seen;
    for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member)
    {
        const auto name = NormalizeName({member->name.GetString(), member->name.GetStringLength()});
        const auto& item = member->value;
        if (name.empty() || !item.IsObject() || !item.HasMember("Coordinates") || !item["Coordinates"].IsString()) continue;
        const std::string coordinates(item["Coordinates"].GetString(), item["Coordinates"].GetStringLength());
        const auto a = coordinates.find(':'), b = coordinates.find(':', a == std::string::npos ? 0U : a+1U);
        if (a == std::string::npos || b == std::string::npos) continue;
        Zone zone; zone.name = name;
        if (!Number(coordinates.substr(0, a), zone.latitude) ||
            !Number(coordinates.substr(a+1U, b-a-1U), zone.longitude) ||
            !Number(coordinates.substr(b+1U), zone.radiusMeters) ||
            std::abs(zone.latitude) > 90.0 || std::abs(zone.longitude) > 180.0 ||
            zone.radiusMeters < 1.0 || zone.radiusMeters > 200.0) continue;
        if (!seen.insert(name).second) { zones.clear(); return false; }
        zones.push_back(std::move(zone));
    }
    return !zones.empty();
}
std::string ContainingZone(const std::vector<Zone>& zones, double latitude,
    double longitude, std::string_view assignedStand)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude) > 90.0 || std::abs(longitude) > 180.0) return {};
    const auto assigned = NormalizeName(assignedStand);
    if (!assignedStand.empty() && assigned.empty()) return {};
    const Zone* closest = nullptr;
    double closestDistance = 1e20;
    for (const auto& zone : zones)
    {
        if (!assigned.empty() && zone.name != assigned) continue;
        const double distance = Distance(latitude, longitude, zone.latitude, zone.longitude);
        if (distance <= zone.radiusMeters && distance < closestDistance)
        {
            closest = &zone;
            closestDistance = distance;
        }
    }
    return closest != nullptr ? closest->name : std::string{};
}
bool ArrivalTracker::Update(const std::string& callsign, const Observation& observation)
{
    auto& state = states_[callsign];
    if (state.identity != observation.flightIdentity || state.zone != observation.zone)
    {
        state = {};
        state.identity = observation.flightIdentity;
        state.zone = observation.zone;
    }
    if (state.completed) return false;
    if (!observation.eligible || observation.zone.empty() || observation.report == 0 ||
        observation.ageSeconds < 0 || observation.ageSeconds > 10 ||
        observation.speedKnots < 0 || observation.speedKnots > 1)
    {
        state.samples = 0;
        state.report = observation.report;
        return false;
    }
    if (observation.report == state.report) return false;
    state.report = observation.report;
    const auto age = static_cast<std::uint64_t>(observation.ageSeconds)*1000U;
    const auto now = observation.nowMilliseconds;
    const auto sampleTime = now >= age ? now-age : 0U;
    if (state.samples == 0 || sampleTime < state.last || sampleTime-state.last > 15000U)
    {
        state.samples = 0;
        state.first = sampleTime;
    }
    state.last = sampleTime;
    ++state.samples;
    return state.samples >= 3U && sampleTime-state.first >= 10000U;
}
void ArrivalTracker::Complete(const std::string& callsign) { states_[callsign].completed = true; }
void ArrivalTracker::Retain(const std::set<std::string>& callsigns)
{
    for (auto it = states_.begin(); it != states_.end();)
        if (!callsigns.count(it->first)) it = states_.erase(it); else ++it;
}
}
