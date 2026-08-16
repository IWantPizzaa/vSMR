#include "stdafx.h"
#include "SMRRadar.hpp"
#include "SMRGroundState.hpp"
#include "SMRTagColorRules.hpp"
#include "SMRVacdmTagHelpers.hpp"
#include "CrashRuntime.hpp"

#include <algorithm>

CSMRRadar::CorrelationSettings CSMRRadar::BuildCorrelationSettings() const
{
	CorrelationSettings settings;
	const DisplayModeSettings displayMode = GetActiveDisplayModeSettings();
	settings.proModeEnabled = displayMode.requireAssignedSquawk;
	settings.acceptPilotSquawk = displayMode.acceptPilotSquawk;
	settings.blockedAutoCorrelateSquawks = displayMode.blockedAutoCorrelateSquawks;
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

	if (isCorr)
	{
		for (const std::string& blockedSquawk : settings.blockedAutoCorrelateSquawks)
		{
			if (hasText(reportedSquawk) && strcmp(reportedSquawk, blockedSquawk.c_str()) == 0)
			{
				isCorr = false;
				break;
			}
		}
	}

	return isCorr;
}

bool CSMRRadar::IsCorrelated(CFlightPlan fp, CRadarTarget rt)
{
	return IsCorrelatedWithSettings(fp, rt, BuildCorrelationSettings());
}

bool CSMRRadar::ShouldDisplayTargetForDisplayMode(CFlightPlan fp, CRadarTarget rt, bool acIsCorrelated, int reportedGs, bool targetOnRunway, const DisplayModeSettings& settings) const
{
	if (settings.requireClearance && (!fp.IsValid() || !fp.GetClearenceFlag()))
		return false;

	if (settings.requireValidTsat || settings.requireActiveTobt)
	{
		VacdmPilotData pilotData;
		if (!TryGetVacdmPilotDataForTarget(rt, fp, pilotData))
			return false;

		if (settings.requireValidTsat)
		{
			const std::string tsatState = ResolveVacdmRuleStateName("tsat", &pilotData);
			if (tsatState != "valid" && tsatState != "valid_ctot")
				return false;
		}

		if (settings.requireActiveTobt)
		{
			const std::string tobtState = ResolveVacdmRuleStateName("tobt", &pilotData);
			if (tobtState != "confirmed" &&
				tobtState != "unconfirmed" &&
				tobtState != "confirmed_delay" &&
				tobtState != "unconfirmed_delay")
			{
				return false;
			}
		}
	}

	if (!fp.IsValid())
		return settings.statuses.noFlightPlan;

	if (!acIsCorrelated && reportedGs >= 3)
		return settings.statuses.uncorrelated;

	const std::string activeAirport = getActiveAirport();
	const char* destination = fp.GetFlightPlanData().GetDestination();
	const char* origin = fp.GetFlightPlanData().GetOrigin();
	const bool isArrival =
		destination != nullptr &&
		destination[0] != '\0' &&
		!activeAirport.empty() &&
		_stricmp(destination, activeAirport.c_str()) == 0 &&
		(origin == nullptr || _stricmp(origin, activeAirport.c_str()) != 0);
	const bool isDeparture =
		origin != nullptr &&
		origin[0] != '\0' &&
		!activeAirport.empty() &&
		_stricmp(origin, activeAirport.c_str()) == 0;

	if (isArrival && !settings.statuses.arrivals)
		return false;
	if (reportedGs > 50 && !settings.statuses.airborne)
		return false;
	if (targetOnRunway && !settings.statuses.onRunway)
		return false;

	if (isArrival)
		return true;

	if (!isDeparture && destination != nullptr && destination[0] != '\0')
		return settings.statuses.arrivals;

	const GroundStateCategory targetStatus = classifyGroundStateForCallsign(fp.GetCallsign(), fp.GetGroundState(), reportedGs, targetOnRunway);
	switch (targetStatus)
	{
	case GroundStateCategory::Push:
		return settings.statuses.push;
	case GroundStateCategory::Stup:
		return settings.statuses.startup;
	case GroundStateCategory::Taxi:
		return settings.statuses.taxi;
	case GroundStateCategory::Lnup:
		return settings.statuses.lineup;
	case GroundStateCategory::Depa:
		return settings.statuses.departure;
	case GroundStateCategory::Nsts:
	case GroundStateCategory::Gate:
	case GroundStateCategory::Unknown:
		return settings.statuses.noStatus;
	case GroundStateCategory::Arr:
		return settings.statuses.arrivals;
	default:
		return true;
	}
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
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnAsrContentToBeClosed",
		reinterpret_cast<std::uintptr_t>(this));
	PublishCrashRadarState("closing", "none");
	BeginShutdown();
	CloseVsmrControlCenterWindow();
	CloseProfileEditorWindow(false);
	DestroyProfileEditorWindow();
	DestroyVsmrControlCenterWindow();

	const std::string fallbackProfile = (CurrentConfig != nullptr) ? CurrentConfig->getActiveProfileName() : "Default";
	const std::string profileToPersist = GetSessionActiveProfile(fallbackProfile);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", profileToPersist.c_str());

	if (CurrentConfig != nullptr)
	{
		// Reload before writing shutdown state so stale radar instances do not overwrite
		// edits already saved by another screen during the session.
		CurrentConfig->reload();
		WriteLastActiveProfileToConfig(profileToPersist);
		if (RimcasInstance != nullptr)
			CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
		CurrentConfig->saveConfig();
	}

	delete this;
}
