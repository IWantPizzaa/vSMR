#pragma once
#include <string>
#include <string_view>

namespace VsmrStand
{
    std::string NormalizeAirport(std::string_view airport);
    std::string NormalizeName(std::string_view stand);
    std::string Key(std::string_view airport, std::string_view stand);
    bool IsStandLabel(std::string_view labelClass);
    bool IsArrival(std::string_view airport, std::string_view origin, std::string_view destination);
}
