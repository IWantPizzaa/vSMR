#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterDialog.hpp"

void CSMRRadar::InvalidateAirportPositionCache()
{
	AirportPositionsCacheValid = false;
}

void CSMRRadar::InvalidateRunwayGeometryCache()
{
	CachedRunwayGeometryValid = false;
	CachedRunwayAirport.clear();
	CachedRunwayProfile.clear();
	CachedRunwayGeometries.clear();
	RunwayStatusLastRefreshTick = 0;
	RunwayStatusLastAirport.clear();
	LastMapRunwayStatuses.clear();
	LastMapActiveAirport.clear();

	if (RimcasInstance != nullptr)
	{
		RimcasInstance->RunwayAreas.clear();
		RimcasInstance->RunwayStatuses.clear();
	}
}

void CSMRRadar::EnsureAirportPositionCache()
{
	if (AirportPositionsCacheValid)
		return;

	AirportPositions.clear();

	CSectorElement apt;
	for (apt = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
		apt.IsValid();
		apt = GetPlugIn()->SectorFileElementSelectNext(apt, SECTOR_ELEMENT_AIRPORT))
	{
		const char* airportName = apt.GetName();
		if (airportName == nullptr || airportName[0] == '\0')
			continue;

		CPosition position;
		apt.GetPosition(&position, 0);
		AirportPositions[string(airportName)] = position;
	}

	AirportPositionsCacheValid = true;
}

void CSMRRadar::EnsureRunwayGeometryCache()
{
	if (RimcasInstance == nullptr)
		return;

	const std::string activeAirport = getActiveAirport();
	const std::string activeProfile = CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : std::string();

	auto populateRimcasRunwayAreas = [&]()
	{
		RimcasInstance->RunwayAreas.clear();
		for (const auto& runway : CachedRunwayGeometries)
			RimcasInstance->AddRunwayArea(this, runway.runwayNameA, runway.runwayNameB, runway.rimcasDefinition);
	};

	if (CachedRunwayGeometryValid &&
		CachedRunwayAirport == activeAirport &&
		CachedRunwayProfile == activeProfile &&
		CachedRunwayIsLvp == isLVP)
	{
		if (RimcasInstance->RunwayAreas.empty() && !CachedRunwayGeometries.empty())
			populateRimcasRunwayAreas();
		return;
	}

	CachedRunwayGeometries.clear();
	CachedRunwayAirport = activeAirport;
	CachedRunwayProfile = activeProfile;
	CachedRunwayIsLvp = isLVP;

	const Value* configuredRunways = nullptr;
	if (CurrentConfig != nullptr)
	{
		const Value& customMap = CurrentConfig->getAirportMapIfAny(activeAirport);
		if (customMap.IsObject() &&
			customMap.HasMember("runways") &&
			customMap["runways"].IsArray())
		{
			configuredRunways = &customMap["runways"];
		}
	}

	auto loadClosedRunwayDefinition = [&](const std::string& runwayNameA, const std::string& runwayNameB) -> std::vector<CPosition>
	{
		std::vector<CPosition> definition;
		if (configuredRunways == nullptr)
			return definition;

		for (SizeType i = 0; i < configuredRunways->Size(); ++i)
		{
			const Value& runway = (*configuredRunways)[i];
			if (!runway.IsObject() ||
				!runway.HasMember("runway_name") ||
				!runway["runway_name"].IsString())
			{
				continue;
			}

			const char* configuredName = runway["runway_name"].GetString();
			if (configuredName == nullptr || configuredName[0] == '\0')
				continue;

			if (!VsmrRadarUiSupport::startsWith(runwayNameA.c_str(), configuredName) &&
				!VsmrRadarUiSupport::startsWith(runwayNameB.c_str(), configuredName))
			{
				continue;
			}

			const char* pathName = isLVP ? "path_lvp" : "path";
			const Value* path = nullptr;
			if (runway.HasMember(pathName) && runway[pathName].IsArray())
				path = &runway[pathName];
			else if (isLVP && runway.HasMember("path") && runway["path"].IsArray())
				path = &runway["path"];

			if (path == nullptr)
				continue;

			for (SizeType j = 0; j < path->Size(); ++j)
			{
				const Value& point = (*path)[j];
				if (!point.IsArray() ||
					point.Size() < 2 ||
					!point[static_cast<SizeType>(0)].IsString() ||
					!point[static_cast<SizeType>(1)].IsString())
				{
					continue;
				}

				CPosition position;
				position.LoadFromStrings(
					point[static_cast<SizeType>(1)].GetString(),
					point[static_cast<SizeType>(0)].GetString());
				definition.push_back(position);
			}

			if (!definition.empty())
				return definition;
		}

		return definition;
	};

	auto normalizeHeading = [](double heading) -> double
	{
		heading = std::fmod(heading, 360.0);
		return heading < 0.0 ? heading + 360.0 : heading;
	};
	auto angularDistance = [&](double first, double second) -> double
	{
		return std::abs(normalizeHeading(first - second + 180.0) - 180.0);
	};
	auto resolveTrueHeading = [&](const CPosition& from, const CPosition& to, int sectorHeading, double& heading, bool& valid)
	{
		if (sectorHeading < 0 || sectorHeading > 360)
			return;
		heading = normalizeHeading(VsmrRadarUiSupport::RadToDeg(
			VsmrRadarUiSupport::TrueBearing(from, to)));
		if (angularDistance(heading, static_cast<double>(sectorHeading)) > 90.0)
			heading = normalizeHeading(heading + 180.0);
		valid = true;
	};

	CSectorElement rwy;
	for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirportName = rwy.GetAirportName();
		if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
			continue;

		if (!VsmrRadarUiSupport::startsWith(activeAirport.c_str(), runwayAirportName))
			continue;

		const char* runwayNameA = rwy.GetRunwayName(0);
		const char* runwayNameB = rwy.GetRunwayName(1);
		if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
			continue;

		CPosition left;
		const bool leftValid = rwy.GetPosition(&left, 1);
		CPosition right;
		const bool rightValid = rwy.GetPosition(&right, 0);
		if (!leftValid || !rightValid)
			continue;

		CachedRunwayGeometry runway;
		runway.runwayNameA = runwayNameA;
		runway.runwayNameB = runwayNameB;
		runway.displayName = runway.runwayNameA + " / " + runway.runwayNameB;
		resolveTrueHeading(right, left, rwy.GetRunwayHeading(0), runway.trueHeadingA, runway.trueHeadingAValid);
		resolveTrueHeading(left, right, rwy.GetRunwayHeading(1), runway.trueHeadingB, runway.trueHeadingBValid);
		runway.rimcasDefinition = RimcasInstance->GetRunwayArea(left, right);
		runway.closedDefinition = loadClosedRunwayDefinition(runway.runwayNameA, runway.runwayNameB);
		CachedRunwayGeometries.push_back(std::move(runway));
	}

	CachedRunwayGeometryValid = true;
	populateRimcasRunwayAreas();
}

void CSMRRadar::RefreshRunwayStatuses(bool force)
{
	if (RimcasInstance == nullptr)
		return;

	const std::string activeAirport = getActiveAirport();
	const unsigned long nowTick = ::GetTickCount();
	const unsigned long statusRefreshIntervalMs = 200;
	if (!force &&
		RunwayStatusLastRefreshTick != 0 &&
		RunwayStatusLastAirport == activeAirport &&
		(nowTick - RunwayStatusLastRefreshTick) < statusRefreshIntervalMs)
	{
		return;
	}

	auto getRunwayStatus = [](CSectorElement& runway, int index) -> CRimcas::RunwayStatus
	{
		const bool isDepartureRunway = runway.IsElementActive(true, index);
		const bool isArrivalRunway = runway.IsElementActive(false, index);
		if (isDepartureRunway && isArrivalRunway)
			return CRimcas::RunwayStatus::BOTH;
		if (isDepartureRunway)
			return CRimcas::RunwayStatus::DEP;
		if (isArrivalRunway)
			return CRimcas::RunwayStatus::ARR;
		return CRimcas::RunwayStatus::CLSD;
	};

	std::map<std::string, CRimcas::RunwayStatus> runwayStatuses;
	CSectorElement rwy;
	for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirportName = rwy.GetAirportName();
		if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
			continue;

		if (!VsmrRadarUiSupport::startsWith(activeAirport.c_str(), runwayAirportName))
			continue;

		const char* runwayNameA = rwy.GetRunwayName(0);
		const char* runwayNameB = rwy.GetRunwayName(1);
		if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
			continue;

		runwayStatuses[runwayNameA] = getRunwayStatus(rwy, 0);
		runwayStatuses[runwayNameB] = getRunwayStatus(rwy, 1);
	}

	RunwayStatusLastRefreshTick = nowTick;
	RunwayStatusLastAirport = activeAirport;

	if (RimcasInstance->RunwayStatuses != runwayStatuses)
		RimcasInstance->RunwayStatuses = std::move(runwayStatuses);
}

void CSMRRadar::RefreshLegacyRimcasRunwayMonitoring()
{
	if (RimcasInstance == nullptr || RimcasRunwaysExplicitlyConfigured)
		return;
	// The constructor loads a profile before EuroScope has accepted this radar
	// screen. Defer sector-file access until OnRadarScreenCreated has returned.
	if (std::find(RadarScreensOpened.begin(), RadarScreensOpened.end(), this) ==
		RadarScreensOpened.end())
	{
		return;
	}

	// Profiles without configured runway rows follow EuroScope's current runway
	// activity. Older editor versions commonly persisted an empty array, so it
	// must retain the same inherited behavior as a missing member.
	CPlugIn* plugin = GetPlugIn();
	struct ActiveSectorSelectionGuard
	{
		CPlugIn* plugin = nullptr;
		~ActiveSectorSelectionGuard()
		{
			if (plugin != nullptr)
				plugin->SelectActiveSectorfile();
		}
	} selectionGuard{ plugin };
	plugin->SelectScreenSectorfile(this);
	RimcasInstance->MonitoredRunwayArr.clear();
	RimcasInstance->MonitoredRunwayDep.clear();
	RimcasInstance->ClosedRunway.clear();

	const std::string activeAirport = getActiveAirport();
	CSectorElement runway;
	for (runway = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		runway.IsValid();
		runway = GetPlugIn()->SectorFileElementSelectNext(runway, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirport = runway.GetAirportName();
		const char* runwayNameA = runway.GetRunwayName(0);
		const char* runwayNameB = runway.GetRunwayName(1);
		if (runwayAirport == nullptr || runwayNameA == nullptr || runwayNameB == nullptr ||
			runwayAirport[0] == '\0' || runwayNameA[0] == '\0' || runwayNameB[0] == '\0' ||
			_stricmp(runwayAirport, activeAirport.c_str()) != 0)
		{
			continue;
		}

		const std::string name = std::string(runwayNameA) + " / " + runwayNameB;
		RimcasInstance->MonitoredRunwayDep[name] =
			runway.IsElementActive(true, 0) || runway.IsElementActive(true, 1);
		RimcasInstance->MonitoredRunwayArr[name] =
			runway.IsElementActive(false, 0) || runway.IsElementActive(false, 1);
		RimcasInstance->ClosedRunway[name] = false;
	}
}

void CSMRRadar::RefreshAfterAirportRunwayActivityChange(bool activeAirportChanged)
{
	RunwayStatusLastRefreshTick = 0;
	RunwayStatusLastAirport.clear();
	RefreshRunwayStatuses(true);
	RefreshLegacyRimcasRunwayMonitoring();
	LastMapRunwayStatuses.clear();
	LastMapActiveAirport.clear();
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::AirportUpdate);
	RequestRefresh();

	if ((activeAirportChanged || !RimcasRunwaysExplicitlyConfigured) &&
		VsmrControlCenterDialog != nullptr)
	{
		VsmrControlCenterDialog->SyncFromRadar("runtime");
	}
}
