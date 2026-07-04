#include "stdafx.h"
#include "SMRRadar.hpp"

#include <algorithm>

CSMRRadar::CorrelationSettings CSMRRadar::BuildCorrelationSettings() const
{
	CorrelationSettings settings;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() && profile.HasMember("filters") && profile["filters"].IsObject())
		{
			const Value& filters = profile["filters"];
			if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
			{
				const Value& proMode = filters["pro_mode"];
				if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
					settings.proModeEnabled = proMode["enabled"].GetBool();
				else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
					settings.proModeEnabled = proMode["enable"].GetBool();
				if (proMode.HasMember("accept_pilot_squawk") && proMode["accept_pilot_squawk"].IsBool())
					settings.acceptPilotSquawk = proMode["accept_pilot_squawk"].GetBool();
				if (proMode.HasMember("blocked_auto_correlate_squawks"))
					settings.blockedAutoCorrelateSquawks = &proMode["blocked_auto_correlate_squawks"];
				else if (proMode.HasMember("do_not_autocorrelate_squawks"))
					settings.blockedAutoCorrelateSquawks = &proMode["do_not_autocorrelate_squawks"];
			}
		}
	}
	return settings;
}

bool CSMRRadar::IsCorrelatedWithSettings(CFlightPlan fp, CRadarTarget rt, const CorrelationSettings& settings) const
{
	auto hasText = [](const char* text) -> bool
	{
		return text != nullptr && text[0] != '\0';
	};

	if (!settings.proModeEnabled)
	{
		// If pro mode is disabled, all targets are considered correlated.
		return true;
	}

	if (!fp.IsValid())
		return false;

	bool isCorr = false;
	const char* assignedSquawk = fp.GetControllerAssignedData().GetSquawk();
	const char* reportedSquawk = (rt.IsValid() && rt.GetPosition().IsValid()) ? rt.GetPosition().GetSquawk() : nullptr;
	if (hasText(assignedSquawk) && hasText(reportedSquawk) && strcmp(assignedSquawk, reportedSquawk) == 0)
		isCorr = true;

	if (settings.acceptPilotSquawk)
		isCorr = true;

	if (isCorr && settings.blockedAutoCorrelateSquawks != nullptr && settings.blockedAutoCorrelateSquawks->IsArray())
	{
		for (SizeType i = 0; i < settings.blockedAutoCorrelateSquawks->Size(); i++)
		{
			const Value& blockedSquawk = (*settings.blockedAutoCorrelateSquawks)[i];
			if (hasText(reportedSquawk) && blockedSquawk.IsString() && strcmp(reportedSquawk, blockedSquawk.GetString()) == 0)
			{
				isCorr = false;
				break;
			}
		}
	}

	const char* systemId = rt.IsValid() ? rt.GetSystemID() : nullptr;
	if (hasText(systemId) && ManuallyCorrelated.find(systemId) != ManuallyCorrelated.end())
		isCorr = true;

	if (hasText(systemId) && ReleasedTracks.find(systemId) != ReleasedTracks.end())
		isCorr = false;

	return isCorr;
}

bool CSMRRadar::IsCorrelated(CFlightPlan fp, CRadarTarget rt)
{
	return IsCorrelatedWithSettings(fp, rt, BuildCorrelationSettings());
}

bool CSMRRadar::isVisible(CRadarTarget rt)
{
	if (!rt.IsValid())
		return false;

	CRadarTargetPositionData rtPos = rt.GetPosition();
	if (!rtPos.IsValid())
		return false;

	auto airportIt = AirportPositions.find(getActiveAirport());
	if (airportIt == AirportPositions.end())
		return false;

	int radarRange = 999;
	int altitudeFilter = 5500;
	int speedFilter = 250;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() && profile.HasMember("filters") && profile["filters"].IsObject())
		{
			const Value& filters = profile["filters"];
			if (filters.HasMember("radar_range_nm") && filters["radar_range_nm"].IsInt())
				radarRange = filters["radar_range_nm"].GetInt();
			if (filters.HasMember("max_altitude_ft") && filters["max_altitude_ft"].IsInt())
				altitudeFilter = filters["max_altitude_ft"].GetInt();
			else if (filters.HasMember("hide_above_alt") && filters["hide_above_alt"].IsInt())
				altitudeFilter = filters["hide_above_alt"].GetInt();
			if (filters.HasMember("max_speed_kt") && filters["max_speed_kt"].IsInt())
				speedFilter = filters["max_speed_kt"].GetInt();
			else if (filters.HasMember("hide_above_spd") && filters["hide_above_spd"].IsInt())
				speedFilter = filters["hide_above_spd"].GetInt();
		}
	}
	radarRange = (std::max)(1, radarRange);
	bool isAcDisplayed = true;

	if (airportIt->second.DistanceTo(rtPos.GetPosition()) > radarRange)
		isAcDisplayed = false;

	if (altitudeFilter != 0)
	{
		if (rtPos.GetPressureAltitude() > altitudeFilter)
			isAcDisplayed = false;
	}

	if (speedFilter != 0)
	{
		if (rtPos.GetReportedGS() > speedFilter)
			isAcDisplayed = false;
	}

	return isAcDisplayed;
}

CPosition CSMRRadar::Haversine(CPosition origin, double heading, double distance)
{
	return SMRGeometry::ProjectPosition(origin, heading, distance);
}

double CSMRRadar::Haversine(CPosition origin, CPosition dest)
{
	return SMRGeometry::DistanceMeters(origin, dest);
}

int CSMRRadar::getZoomLevelFromCrossDistance(double crossDistance)
{
	return SMRGeometry::ZoomLevelFromCrossDistance(crossDistance);
}

float CSMRRadar::randomizeHeading(float originHead)
{
	return SMRGeometry::RandomizeHeading(originHead);
}

int CSMRRadar::getIntFromCategory(string category)
{
	return SMRGeometry::SectorElementCategoryFromName(category);
}

void CSMRRadar::OnAsrContentToBeClosed(void)
{
	CloseProfileEditorWindow(false);
	DestroyProfileEditorWindow();

	const std::string fallbackProfile = (CurrentConfig != nullptr) ? CurrentConfig->getActiveProfileName() : "Default";
	const std::string profileToPersist = GetSessionActiveProfile(fallbackProfile);
	WriteLastActiveProfileToDisk(profileToPersist);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", profileToPersist.c_str());

	if (CurrentConfig != nullptr)
	{
		// Reload before writing shutdown state so stale radar instances do not overwrite
		// edits already saved by another screen during the session.
		CurrentConfig->reload();
		if (RimcasInstance != nullptr)
			CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
		CurrentConfig->saveConfig();
	}

	delete this;
}
