#pragma once
#include <Windows.h>

namespace VsmrRendering
{
    // The mixed GDI/GDI+ menu renderer and its HRGNs use absolute coordinates.
    inline SIZE RuntimeMenuSurfaceSize(const RECT& area) noexcept
    {
        if (area.left < 0 || area.top < 0 || area.right <= area.left || area.bottom <= area.top)
            return { 0, 0 };
        return { area.right + 1, area.bottom + 1 };
    }
}
