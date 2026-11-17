#pragma once

#include <charconv>
#include <string>
#include <system_error>

namespace VsmrSmartGroundOverlay
{
inline constexpr int RoutePointObjectType = 8896;
inline constexpr int OffTrackAlertObjectType = 8897;
inline constexpr int OffTrackAlertPopupFunction = 8898;

struct HandleIdentity
{
    std::string callsign;
    std::string nodeId;
    int viewportId = 0;
};

inline std::string EncodeHandle(const HandleIdentity& identity)
{
    if (identity.callsign.empty() || identity.nodeId.empty() || identity.viewportId < 0 ||
        identity.callsign.find('|') != std::string::npos || identity.nodeId.find('|') != std::string::npos)
        return {};
    return "sgroute|" + std::to_string(identity.viewportId) + "|" + identity.callsign + "|" + identity.nodeId;
}

inline bool DecodeHandle(const char* objectId, HandleIdentity& identity)
{
    if (objectId == nullptr) return false;
    const std::string text(objectId);
    if (text.size() > 512 || text.compare(0, 8, "sgroute|") != 0) return false;
    const auto first = text.find('|', 8), second = first == std::string::npos ? first : text.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos || first == 8 ||
        second == first + 1 || second + 1 >= text.size() || text.find('|', second + 1) != std::string::npos)
        return false;
    int viewport = 0;
    const char* begin = text.data() + 8;
    const char* end = text.data() + first;
    const auto parsed = std::from_chars(begin, end, viewport);
    if (parsed.ec != std::errc{} || parsed.ptr != end || viewport < 0) return false;
    identity = { text.substr(first + 1, second - first - 1), text.substr(second + 1), viewport };
    return true;
}
}
