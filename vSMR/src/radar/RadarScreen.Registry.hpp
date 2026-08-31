#pragma once

#include <vector>

class CSMRRadar;

// The plugin owns the registry; radar-related components only observe or update it.
extern std::vector<CSMRRadar*> RadarScreensOpened;
