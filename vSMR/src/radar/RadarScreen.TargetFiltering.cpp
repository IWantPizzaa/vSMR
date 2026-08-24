#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "aircraft/GroundState.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/VacdmTagHelpers.hpp"
#include "crash/CrashRuntime.hpp"

CSMRRadar::CorrelationSettings CSMRRadar::BuildCorrelationSettings() const
{
	CorrelationSettings settings;
	const DisplayModeSettings displayMode = GetActiveDisplayModeSettings();
	settings.proModeEnabled = displayMode.requireAssignedSquawk;
	settings.acceptPilotSquawk = displayMode.acceptPilotSquawk;
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

	return isCorr;
}

bool CSMRRadar::IsCorrelated(CFlightPlan fp, CRadarTarget rt)
{
	return IsCorrelatedWithSettings(fp, rt, BuildCorrelationSettings());
}

bool CSMRRadar::ShouldDisplayTargetForDisplayMode(CFlightPlan fp, bool acIsCorrelated, int reportedGs, bool targetOnRunway, const DisplayModeSettings& settings, const VacdmPilotData* capturedVacdmData) const
{
	if (settings.requireClearance && (!fp.IsValid() || !fp.GetClearenceFlag()))
		return false;

	if (settings.requireValidTsat || settings.requireActiveTobt)
	{
		if (capturedVacdmData == nullptr)
			return false;

		if (settings.requireValidTsat)
		{
			const std::string tsatState = ResolveVacdmRuleStateName("tsat", capturedVacdmData);
			if (tsatState != "valid" && tsatState != "valid_ctot")
				return false;
		}

		if (settings.requireActiveTobt)
		{
			const std::string tobtState = ResolveVacdmRuleStateName("tobt", capturedVacdmData);
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
