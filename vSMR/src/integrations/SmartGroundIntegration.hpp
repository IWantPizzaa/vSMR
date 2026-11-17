#pragma once
#include <EuroScopePlugIn.h>
#include <string>

namespace VsmrScene { struct RadarScene; }

// A scene may seed a pre-existing SDK report once. Further observations come
// from position callbacks; additional views never count as fresh measurements.
namespace VsmrSmartGroundBridge
{
    bool IsEnabled();
    void SetEnabled(bool enabled);
    void Initialize();
    void Observe(EuroScopePlugIn::CRadarTarget target, bool onlyIfMissing = false);
    void Forget(const std::string& callsign);
    void UpdateHoldingPoint(const std::string& callsign, const std::string& holdingPoint);
    bool Advance();
    void UpdateScene(VsmrScene::RadarScene& scene);
    void Shutdown();
}
