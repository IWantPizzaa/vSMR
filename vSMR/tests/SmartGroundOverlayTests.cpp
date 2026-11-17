#include "SmartGroundOverlayTests.hpp"
#include "radar/SmartGroundOverlayLogic.hpp"

std::vector<std::string> RunSmartGroundOverlayTests()
{
    using namespace VsmrSmartGroundOverlay;
    std::vector<std::string> failures;
    const auto check = [&](bool condition, const char* message) { if (!condition) failures.emplace_back(message); };
    HandleIdentity decoded;
    check(DecodeHandle(EncodeHandle({ "AFR123", "osm:2318941305", 0 }).c_str(), decoded),
        "Smart Ground main route handle must round-trip a real source node.");
    check(decoded.callsign == "AFR123" && decoded.nodeId == "osm:2318941305" && decoded.viewportId == 0,
        "Smart Ground handle must preserve aircraft, graph node and main-view identity.");
    check(DecodeHandle(EncodeHandle({ "BAW456", "intersection:27", 8890 }).c_str(), decoded),
        "Smart Ground inset route handles must retain their own projection identity.");
    check(decoded.viewportId == 8890 && decoded.callsign == "BAW456" && decoded.nodeId == "intersection:27",
        "Smart Ground inset coordinates must not be interpreted through the main radar transform.");
    for (const char* invalid : { "", "north.drag", "AFR123", "sgroute||AFR123|node", "sgroute|-1|AFR123|node",
        "sgroute|8890x|AFR123|node", "sgroute|999999999999999|AFR123|node", "sgroute|0||node",
        "sgroute|0|AFR123|", "sgroute|0|AFR123|node|extra" })
        check(!DecodeHandle(invalid, decoded), "Malformed or unrelated screen objects must never become a Smart Ground route edit.");
    check(!DecodeHandle(nullptr, decoded), "A missing route object must not dereference an SDK pointer.");
    check(EncodeHandle({ "AFR|123", "osm:1", 0 }).empty() && EncodeHandle({ "AFR123", "osm|1", 0 }).empty(),
        "Route handle delimiters must not ambiguously select another aircraft or source node.");
    check(EncodeHandle({ "AFR123", "osm:1", -1 }).empty(), "Invalid viewport IDs must not register draggable objects.");
    return failures;
}
