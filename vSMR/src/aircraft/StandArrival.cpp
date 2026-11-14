#include "aircraft/StandArrival.hpp"

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
}
