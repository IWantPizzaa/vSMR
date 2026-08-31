#pragma once

#include "EuroScopePlugIn.h"
#include <string>

namespace SMRGeometry
{
	inline constexpr double Pi = 3.14159265358979323846264338327950288;

	EuroScopePlugIn::CPosition ProjectPosition(EuroScopePlugIn::CPosition origin, double headingDeg, double distanceMeters);
	double DistanceMeters(EuroScopePlugIn::CPosition origin, EuroScopePlugIn::CPosition destination);
	int ZoomLevelFromCrossDistance(double crossDistanceMeters);
	int SectorElementCategoryFromName(const std::string& category);
}
