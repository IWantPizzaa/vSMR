#pragma once
#include "rapidjson/document.h"
#include <string>
namespace VsmrAviso
{
    inline unsigned GeometryPaletteMask(const rapidjson::Value* properties)
    {
        if (!properties || !properties->HasMember("geometry_palettes")) return 7U;
        const auto& palettes = (*properties)["geometry_palettes"];
        if (!palettes.IsArray()) return 0U;
        unsigned mask = 0;
        for (const auto& p : palettes.GetArray())
        {
            if (!p.IsString()) continue;
            const std::string name = p.GetString();
            if (name == "dark") mask |= 1U;
            if (name == "light") mask |= 2U;
            if (name == "real") mask |= 4U;
        }
        return mask;
    }
    inline bool GeometryVisible(unsigned mask, const std::string& palette)
    {
        return (mask & (palette == "real" ? 4U : palette == "light" ? 2U : 1U)) != 0U;
    }
}
