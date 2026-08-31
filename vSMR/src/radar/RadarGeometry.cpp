#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace
{
	double DegToRadLocal(double degrees)
	{
		return degrees / 180.0 * SMRGeometry::Pi;
	}

	double RadToDegLocal(double radians)
	{
		return radians / SMRGeometry::Pi * 180.0;
	}
}

namespace SMRGeometry
{
	EuroScopePlugIn::CPosition ProjectPosition(EuroScopePlugIn::CPosition origin, double headingDeg, double distanceMeters)
	{
		EuroScopePlugIn::CPosition newPos;

		const double distanceDegrees = (distanceMeters * 0.00053996) / 60.0 * Pi / 180.0;
		const double trackRadians = DegToRadLocal(headingDeg);
		const double originLat = DegToRadLocal(origin.m_Latitude);
		const double originLon = DegToRadLocal(origin.m_Longitude);

		const double lat = std::asin(std::sin(originLat) * std::cos(distanceDegrees) +
			std::cos(originLat) * std::sin(distanceDegrees) * std::cos(trackRadians));
		const double lon = std::cos(lat) == 0
			? originLon
			: std::fmod(originLon + std::asin(std::sin(trackRadians) * std::sin(distanceDegrees) / std::cos(lat)) + Pi, 2.0 * Pi) - Pi;

		newPos.m_Latitude = RadToDegLocal(lat);
		newPos.m_Longitude = RadToDegLocal(lon);

		return newPos;
	}

	double DistanceMeters(EuroScopePlugIn::CPosition origin, EuroScopePlugIn::CPosition destination)
	{
		constexpr double earthRadiusMeters = 6372797.56085;

		origin.m_Latitude = DegToRadLocal(origin.m_Latitude);
		origin.m_Longitude = DegToRadLocal(origin.m_Longitude);
		destination.m_Latitude = DegToRadLocal(destination.m_Latitude);
		destination.m_Longitude = DegToRadLocal(destination.m_Longitude);

		const double haversine =
			std::pow(std::sin(0.5 * (destination.m_Latitude - origin.m_Latitude)), 2.0) +
			(std::cos(origin.m_Latitude) * std::cos(destination.m_Latitude) *
				std::pow(std::sin(0.5 * (destination.m_Longitude - origin.m_Longitude)), 2.0));
		const double angularDistance = 2.0 * std::asin((std::min)(1.0, std::sqrt(haversine)));
		return earthRadiusMeters * angularDistance;
	}

	int ZoomLevelFromCrossDistance(double crossDistanceMeters)
	{
		const int distance = static_cast<int>(crossDistanceMeters);

		if (distance <= 2000)
			return 14;
		if (distance <= 2500)
			return 13;
		if (distance <= 3000)
			return 12;
		if (distance <= 4000)
			return 11;
		if (distance <= 5000)
			return 10;
		if (distance <= 6000)
			return 9;
		if (distance <= 8000)
			return 8;
		if (distance <= 9500)
			return 7;
		if (distance <= 12000)
			return 6;
		if (distance <= 14000)
			return 5;
		if (distance <= 18000)
			return 4;
		if (distance <= 22000)
			return 3;
		if (distance <= 28000)
			return 2;
		if (distance <= 34000)
			return 1;
		return 0;
	}

	int SectorElementCategoryFromName(const std::string& category)
	{
		if (category == "FREETEXT")
			return EuroScopePlugIn::SECTOR_ELEMENT_FREE_TEXT;
		if (category == "RUNWAY")
			return EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY;
		if (category == "VOR")
			return EuroScopePlugIn::SECTOR_ELEMENT_VOR;
		if (category == "NDB")
			return EuroScopePlugIn::SECTOR_ELEMENT_NDB;
		if (category == "FIX")
			return EuroScopePlugIn::SECTOR_ELEMENT_FIX;
		if (category == "AIRPORT")
			return EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT;
		if (category == "STAR")
			return EuroScopePlugIn::SECTOR_ELEMENT_STAR;
		if (category == "SID")
			return EuroScopePlugIn::SECTOR_ELEMENT_SID;
		if (category == "ARTC")
			return EuroScopePlugIn::SECTOR_ELEMENT_ARTC;
		if (category == "GEO")
			return EuroScopePlugIn::SECTOR_ELEMENT_GEO;
		if (category == "AIRSPACE")
			return EuroScopePlugIn::SECTOR_ELEMENT_AIRSPACE;
		if (category == "REGIONS")
			return EuroScopePlugIn::SECTOR_ELEMENT_REGIONS;
		return -1;
	}
}
