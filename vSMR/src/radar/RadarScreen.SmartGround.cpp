#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/SmartGroundOverlayLogic.hpp"
#include "insets/InsetWindow.hpp"
#include "smart_ground/SmartGroundEngine.hpp"

#include <cmath>
#include <utility>

namespace
{
    double SmartGroundNow() { return static_cast<double>(::GetTickCount64()) / 1000.0; }

    class SavedSmartGroundGraphics
    {
    public:
        explicit SavedSmartGroundGraphics(Gdiplus::Graphics& graphics) : graphics_(graphics), state_(graphics.Save()) {}
        ~SavedSmartGroundGraphics() { graphics_.Restore(state_); }
    private:
        Gdiplus::Graphics& graphics_;
        Gdiplus::GraphicsState state_;
    };

    std::string SelectedSmartGroundCallsign(CSMRRadar& radar, const VsmrScene::RadarScene* scene)
    {
        // ASEL changes also originate in EuroScope lists, not only our painted
        // symbol. Query its current value rather than retaining a click cache.
        const auto selected = radar.GetPlugIn()->RadarTargetSelectASEL();
        if (selected.IsValid())
        {
            const char* callsign = selected.GetCallsign();
            if (callsign != nullptr && callsign[0] != '\0') return callsign;
        }
        const auto selectedPlan = radar.GetPlugIn()->FlightPlanSelectASEL();
        if (selectedPlan.IsValid())
        {
            const char* callsign = selectedPlan.GetCallsign();
            if (callsign != nullptr && callsign[0] != '\0') return callsign;
        }
        (void)scene; // A stale scene selection must not survive live deselection.
        return {};
    }

    CPosition RoutePosition(const VsmrSmartGround::RoutePoint& point)
    {
        CPosition result;
        result.m_Latitude = point.latitude;
        result.m_Longitude = point.longitude;
        return result;
    }
}

void CSMRRadar::ClearSmartGroundDrag()
{
    SmartGroundDragging = false;
    SmartGroundDragTarget.clear();
    SmartGroundDragPreview = {};
    SmartGroundDragPreviewRevision = 0;
    SmartGroundDragPreviewComputed = false;
    SmartGroundDragPreviewError.clear();
}

void CSMRRadar::UpdateSmartGroundDragPreview(const VsmrSmartGround::Snapshot& snapshot)
{
    const auto inverse = SmartGroundUnproject.find(SmartGroundDragViewport);
    const auto area = SmartGroundViewAreas.find(SmartGroundDragViewport);
    if (!SmartGroundDragging || !snapshot.routeAvailable || inverse == SmartGroundUnproject.end() || area == SmartGroundViewAreas.end())
    {
        ClearSmartGroundDrag(); return;
    }
    const auto anchor = std::find_if(snapshot.remainingRoute.begin(), snapshot.remainingRoute.end(),
        [this](const auto& value) { return value.editable && value.nodeId == SmartGroundDragAnchor; });
    if (anchor == snapshot.remainingRoute.end()) { ClearSmartGroundDrag(); return; }
    SmartGroundDragPosition = inverse->second(SmartGroundDragMousePoint);
    VsmrSmartGround::RoutePoint target;
    if (area->second.PtInRect(SmartGroundDragMousePoint))
        target = VsmrSmartGround::Service().FindIntersection(SmartGroundDragPosition.m_Latitude, SmartGroundDragPosition.m_Longitude, 60.0);
    if (!target.nodeId.empty()) SmartGroundDragPosition = RoutePosition(target);
    if (SmartGroundDragPreviewComputed && SmartGroundDragTarget == target.nodeId && SmartGroundDragPreviewRevision == snapshot.previewRevision) return;
    SmartGroundDragTarget = target.nodeId;
    SmartGroundDragPreviewRevision = snapshot.previewRevision;
    SmartGroundDragPreviewComputed = true;
    SmartGroundDragPreview = {};
    SmartGroundDragPreviewError.clear();
    if (!target.nodeId.empty() && target.nodeId != SmartGroundDragAnchor)
        VsmrSmartGround::Service().PreviewEditRoute(SmartGroundDragCallsign, SmartGroundDragAnchor,
            target.nodeId, SmartGroundDragPreview, SmartGroundDragPreviewError);
}

void CSMRRadar::RenderSmartGroundRoute(
    Gdiplus::Graphics& graphics,
    const VsmrScene::RadarScene* scene,
    const RECT& radarArea)
{
    RenderSmartGroundRouteProjected(graphics, scene, radarArea, 0,
        [this](const CPosition& position) { return ConvertCoordFromPositionToPixel(position); },
        [this](POINT point) { return ConvertCoordFromPixelToPosition(point); });
}

void CSMRRadar::RenderSmartGroundRouteProjected(
    Gdiplus::Graphics& graphics,
    const VsmrScene::RadarScene* scene,
    const RECT& radarArea,
    int viewportId,
    const std::function<POINT(const CPosition&)>& project,
    const std::function<CPosition(POINT)>& unproject)
{
    if (!project || !unproject || getActiveAirport() != "LFPG") { ClearSmartGroundDrag(); return; }
    const std::string callsign = SelectedSmartGroundCallsign(*this, scene);
    if (callsign.empty())
    {
        ClearSmartGroundDrag();
        return;
    }
    if (SmartGroundDragging && SmartGroundDragCallsign != callsign) ClearSmartGroundDrag();
    const auto snapshot = VsmrSmartGround::Service().GetSnapshot(callsign, SmartGroundNow());
    if (!snapshot.routeAvailable || snapshot.remainingRoute.size() < 2) { ClearSmartGroundDrag(); return; }

    CRect clip(radarArea); clip.NormalizeRect();
    if (clip.IsRectEmpty()) return;
    SmartGroundUnproject[viewportId] = unproject;
    SmartGroundViewAreas[viewportId] = clip;
    if (SmartGroundDragging && SmartGroundDragViewport == viewportId) UpdateSmartGroundDragPreview(snapshot);
    const SavedSmartGroundGraphics saved(graphics);
    graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, clip.Width(), clip.Height()), Gdiplus::CombineModeIntersect);
    const auto previousSmoothing = graphics.GetSmoothingMode();
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    std::vector<Gdiplus::PointF> points;
    points.reserve(snapshot.remainingRoute.size());
    for (const auto& point : snapshot.remainingRoute)
    {
        const auto pixel = project(RoutePosition(point));
        points.emplace_back(static_cast<Gdiplus::REAL>(pixel.x), static_cast<Gdiplus::REAL>(pixel.y));
    }
    const Gdiplus::Color standardColor(225, 196, 238, 123), assignedColor(225, 118, 200, 255);
    Gdiplus::Pen standardShadow(Gdiplus::Color(55, 196, 238, 123), 5.0f), assignedShadow(Gdiplus::Color(55, 118, 200, 255), 5.0f);
    Gdiplus::Pen standardPen(standardColor, 2.0f), assignedPen(assignedColor, 2.0f);
    // The engine has already clipped the travelled prefix and interpolated the
    // first remaining point on the current edge. Never draw its full history.
    // A controller route may retain long official sections. Colour each
    // directed source segment instead of turning the entire route blue.
    for (std::size_t i = 1; i < points.size(); ++i)
        graphics.DrawLine(snapshot.remainingRoute[i].controllerSegment ? &assignedShadow : &standardShadow, points[i - 1], points[i]);
    for (std::size_t i = 1; i < points.size(); ++i)
        graphics.DrawLine(snapshot.remainingRoute[i].controllerSegment ? &assignedPen : &standardPen, points[i - 1], points[i]);

    Gdiplus::Pen handlePen(standardColor, 1.3f);
    Gdiplus::SolidBrush handleFill(Gdiplus::Color(235, 20, 35, 44));
    POINT previousHandle{}; bool hasPreviousHandle = false;
    for (std::size_t i = 1; i + 1 < snapshot.remainingRoute.size(); ++i)
    {
        const auto& point = snapshot.remainingRoute[i];
        if (!point.editable || point.nodeId.empty()) continue;
        const auto pixel = project(RoutePosition(point));
        if (!clip.PtInRect(pixel)) continue;
        if (hasPreviousHandle && std::hypot(static_cast<double>(pixel.x - previousHandle.x),
            static_cast<double>(pixel.y - previousHandle.y)) < 18.0) continue;
        const auto objectId = VsmrSmartGroundOverlay::EncodeHandle({ callsign, point.nodeId, viewportId });
        if (objectId.empty()) continue;
        handlePen.SetColor(point.controllerSegment ? assignedColor : standardColor);
        graphics.FillEllipse(&handleFill, static_cast<Gdiplus::REAL>(pixel.x - 3), static_cast<Gdiplus::REAL>(pixel.y - 3), 6.0f, 6.0f);
        graphics.DrawEllipse(&handlePen, static_cast<Gdiplus::REAL>(pixel.x - 3), static_cast<Gdiplus::REAL>(pixel.y - 3), 6.0f, 6.0f);
        CRect hit(pixel.x - 5, pixel.y - 5, pixel.x + 6, pixel.y + 6), clipped;
        clipped.IntersectRect(hit, clip);
        AddScreenObject(VsmrSmartGroundOverlay::RoutePointObjectType, objectId.c_str(), clipped, true,
            "Drag to another taxiway intersection; right-click to undo route edit");
        previousHandle = pixel; hasPreviousHandle = true;
    }
    if (SmartGroundDragging && SmartGroundDragViewport == viewportId && SmartGroundDragCallsign == callsign)
    {
        // This is the actual validated candidate geometry, with a deliberately
        // faint stroke. Computing it never assigns the route or clears an alert.
        if (SmartGroundDragPreview.routeAvailable)
        {
            Gdiplus::Pen previewStandard(Gdiplus::Color(75, 196, 238, 123), 2.5f);
            Gdiplus::Pen previewAssigned(Gdiplus::Color(90, 118, 200, 255), 2.5f);
            const auto& route = SmartGroundDragPreview.remainingRoute;
            for (std::size_t i = 1; i < route.size(); ++i)
            {
                const auto from = project(RoutePosition(route[i - 1])), to = project(RoutePosition(route[i]));
                graphics.DrawLine(route[i].controllerSegment ? &previewAssigned : &previewStandard,
                    static_cast<Gdiplus::REAL>(from.x), static_cast<Gdiplus::REAL>(from.y),
                    static_cast<Gdiplus::REAL>(to.x), static_cast<Gdiplus::REAL>(to.y));
            }
        }
        const auto ghost = project(SmartGroundDragPosition);
        const auto marker = SmartGroundDragTarget.empty() ? Gdiplus::Color(170, 160, 160, 160)
            : SmartGroundDragPreviewError.empty() ? assignedColor : Gdiplus::Color(220, 240, 120, 100);
        Gdiplus::Pen snapPen(marker, 1.7f);
        Gdiplus::SolidBrush snapFill(Gdiplus::Color(40, marker.GetR(), marker.GetG(), marker.GetB()));
        graphics.FillEllipse(&snapFill, static_cast<Gdiplus::REAL>(ghost.x - 6), static_cast<Gdiplus::REAL>(ghost.y - 6), 12.0f, 12.0f);
        graphics.DrawEllipse(&snapPen, static_cast<Gdiplus::REAL>(ghost.x - 6), static_cast<Gdiplus::REAL>(ghost.y - 6), 12.0f, 12.0f);
    }
    graphics.SetSmoothingMode(previousSmoothing);
}

bool CSMRRadar::HandleSmartGroundMove(int objectType, const char* objectId, POINT point, RECT area, bool released)
{
    (void)area;
    if (objectType != VsmrSmartGroundOverlay::RoutePointObjectType) return false;
    if (getActiveAirport() != "LFPG") { ClearSmartGroundDrag(); return true; }
    VsmrSmartGroundOverlay::HandleIdentity handle;
    if (!VsmrSmartGroundOverlay::DecodeHandle(objectId, handle)) { ClearSmartGroundDrag(); return true; }
    const auto inverse = SmartGroundUnproject.find(handle.viewportId);
    const auto viewArea = SmartGroundViewAreas.find(handle.viewportId);
    if (inverse == SmartGroundUnproject.end() || viewArea == SmartGroundViewAreas.end() ||
        SelectedSmartGroundCallsign(*this, GetCurrentRadarScene()) != handle.callsign)
    {
        ClearSmartGroundDrag(); return true;
    }
    if (handle.viewportId != 0)
    {
        const auto window = appWindows.find(handle.viewportId - APPWINDOW_BASE);
        const auto visible = appWindowDisplays.find(handle.viewportId - APPWINDOW_BASE);
        if (window == appWindows.end() || !window->second || visible == appWindowDisplays.end() || !visible->second)
        {
            ClearSmartGroundDrag(); return true;
        }
    }
    if (!SmartGroundDragging || SmartGroundDragCallsign != handle.callsign ||
        SmartGroundDragAnchor != handle.nodeId || SmartGroundDragViewport != handle.viewportId) ClearSmartGroundDrag();
    SmartGroundDragCallsign = handle.callsign; SmartGroundDragAnchor = handle.nodeId;
    SmartGroundDragViewport = handle.viewportId;
    SmartGroundDragMousePoint = point;
    SmartGroundDragging = true;
    UpdateSmartGroundDragPreview(VsmrSmartGround::Service().GetSnapshot(handle.callsign, SmartGroundNow()));
    if (!SmartGroundDragging) { RequestRefresh(); return true; }
    if (released && viewArea->second.PtInRect(point))
    {
        // Revalidate the snapped intersection against the live track. A preview
        // must never become an assignment by copying its cached geometry.
        const std::string target = SmartGroundDragTarget;
        std::string error;
        if (target.empty()) error = "Drop the route point near an existing taxiway intersection.";
        else if (target != handle.nodeId && !VsmrSmartGround::Service().EditRoute(handle.callsign, handle.nodeId, target, error) && error.empty())
            error = "No authorized route through that intersection.";
        if (!error.empty()) GetPlugIn()->DisplayUserMessage("vSMR", "Smart Ground", error.c_str(), true, true, false, false, false);
    }
    if (released) ClearSmartGroundDrag();
    RequestRefresh();
    return true;
}

bool CSMRRadar::HandleSmartGroundClick(int objectType, const char* objectId, POINT point, RECT area, int button)
{
    (void)point; (void)area;
    if (objectType == VsmrSmartGroundOverlay::OffTrackAlertObjectType)
    {
        if (getActiveAirport() != "LFPG" || objectId == nullptr || *objectId == '\0') return true;
        const auto snapshot = VsmrSmartGround::Service().GetSnapshot(objectId, SmartGroundNow());
        if (!snapshot.offTrackAlert) return true;
        PendingSmartGroundAlertCallsign = objectId;
        const std::string title = "OFF TRACK - " + PendingSmartGroundAlertCallsign;
        GetPlugIn()->OpenPopupList(area, title.c_str(), 1);
        GetPlugIn()->AddPopupListElement("Disregard", "", VsmrSmartGroundOverlay::OffTrackAlertPopupFunction);
        GetPlugIn()->AddPopupListElement("Disregard permanently", "", VsmrSmartGroundOverlay::OffTrackAlertPopupFunction);
        GetPlugIn()->AddPopupListElement("Close", "", VsmrSmartGroundOverlay::OffTrackAlertPopupFunction, false, 2, false, true);
        return true;
    }
    if (objectType != VsmrSmartGroundOverlay::RoutePointObjectType) return false;
    if (getActiveAirport() != "LFPG") return true;
    if (button == BUTTON_RIGHT)
    {
        VsmrSmartGroundOverlay::HandleIdentity handle;
        if (VsmrSmartGroundOverlay::DecodeHandle(objectId, handle) &&
            SelectedSmartGroundCallsign(*this, GetCurrentRadarScene()) == handle.callsign)
        {
            std::string error;
            if (!VsmrSmartGround::Service().UndoRoute(handle.callsign, error) && !error.empty())
                GetPlugIn()->DisplayUserMessage("vSMR", "Smart Ground", error.c_str(), true, true, false, false, false);
        }
        RequestRefresh();
    }
    return true;
}

bool CSMRRadar::HandleSmartGroundFunction(int functionId, const char* itemString)
{
    if (functionId != VsmrSmartGroundOverlay::OffTrackAlertPopupFunction) return false;
    // Bind to the clicked aircraft, not whichever target is ASEL when the
    // controller finally chooses an item. Close consumes the same context.
    const std::string callsign = std::move(PendingSmartGroundAlertCallsign);
    PendingSmartGroundAlertCallsign.clear();
    if (callsign.empty() || itemString == nullptr) return true;
    if (strcmp(itemString, "Disregard") == 0)
        VsmrSmartGround::Service().DisregardOffTrack(callsign, false);
    else if (strcmp(itemString, "Disregard permanently") == 0)
        VsmrSmartGround::Service().DisregardOffTrack(callsign, true);
    RequestRefresh();
    return true;
}
