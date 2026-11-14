#pragma once
#include "radar/RadarScreenTypes.hpp"

namespace VsmrAviso
{
    inline Gdiplus::Color StandLabelTextColor(const VsmrRadarTypes::AvisoLabel& label,
        const VsmrRadarTypes::AvisoRasterRenderRequest& request)
    {
        if (!label.standKey.empty() && label.standKey == request.highlightedStand)
            return request.colorPalette == "light" ? Gdiplus::Color(255, 0, 92, 180) : Gdiplus::Color(255, 255, 210, 60);
        return request.colorPalette == "real" ? label.realTextColor :
            request.colorPalette == "light" ? label.lightTextColor : label.textColor;
    }
}
