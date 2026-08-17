#include "platform/windows/PrecompiledHeader.hpp"
#include "safety/Rimcas.hpp"
#include "aircraft/GroundState.hpp"
#include "scene/RadarScene.hpp"
#include "shared/logging/Logger.hpp"

namespace
{
	CPosition ToEuroScopePosition(const VsmrScene::GeoPoint& source)
	{
		CPosition position;
		position.m_Latitude = source.latitude;
		position.m_Longitude = source.longitude;
		return position;
	}
}

CRimcas::CRimcas()
{
}

CRimcas::~CRimcas()
{
	Reset();
}

void CRimcas::Reset() {
	Logger::info(string(__FUNCSIG__));
	RunwayAreas.clear();
	RunwayStatuses.clear();
	InvalidateRunwayAreaScreenCache();
	AcColor.clear();
	AcOnRunway.clear();
	AircraftOnRunway.clear();
	TimeTable.clear();
	inactiveAlerts.clear();
	MonitoredRunwayArr.clear();
	MonitoredRunwayDep.clear();
	ApproachingAircrafts.clear();
}

void CRimcas::OnRefreshBegin(bool isLVP, int transitionAltitude) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	InvalidateRunwayAreaScreenCache();
	AcColor.clear();
	AcOnRunway.clear();
	AircraftOnRunway.clear();
	TimeTable.clear();
	ApproachingAircrafts.clear();
	this->IsLVP = isLVP;
	this->TransitionAltitude = transitionAltitude;
	movementAlerts.clear();
}

void CRimcas::OnRefresh(const VsmrScene::Target& Rt, CRadarScreen* instance) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (Rt.callsign.empty())
		return;
	GetAcInRunwayArea(Rt, instance);
	GetAcInRunwayAreaSoon(Rt, instance);
	CheckForMovementAlert(Rt, instance);
}

void CRimcas::AddRunwayArea(CRadarScreen* instance, string runway_name1, string runway_name2, vector<CPosition> Definition) {
	(void)instance;
	Logger::info(string(__FUNCSIG__));
	string Name = runway_name1 + " / " + runway_name2;

	RunwayAreaType Runway;
	Runway.Name = Name;
	Runway.Definition = std::move(Definition);

	RunwayAreas[Name] = std::move(Runway);
	InvalidateRunwayAreaScreenCache();
}

void CRimcas::InvalidateRunwayAreaScreenCache()
{
	RunwayAreasScreenCache.clear();
	RunwayAreasScreenCacheValid = false;
	RunwayAreasScreenCacheInstance = nullptr;
}

const vector<POINT>* CRimcas::GetRunwayAreaScreenPoints(const string& runway, CRadarScreen* instance)
{
	if (instance == nullptr)
		return nullptr;

	if (!RunwayAreasScreenCacheValid || RunwayAreasScreenCacheInstance != instance)
	{
		RunwayAreasScreenCache.clear();
		for (const auto& runwayArea : RunwayAreas)
		{
			vector<POINT> runwayOnScreen;
			runwayOnScreen.reserve(runwayArea.second.Definition.size());
			for (const auto& point : runwayArea.second.Definition)
				runwayOnScreen.push_back(instance->ConvertCoordFromPositionToPixel(point));

			if (runwayOnScreen.size() >= 3)
				RunwayAreasScreenCache[runwayArea.first] = std::move(runwayOnScreen);
		}

		RunwayAreasScreenCacheInstance = instance;
		RunwayAreasScreenCacheValid = true;
	}

	const auto cacheIt = RunwayAreasScreenCache.find(runway);
	if (cacheIt == RunwayAreasScreenCache.end())
		return nullptr;
	return &cacheIt->second;
}

string CRimcas::GetAcInRunwayArea(const VsmrScene::Target& Ac, CRadarScreen* instance) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (instance == nullptr || Ac.callsign.empty() || !Ac.position.valid)
		return string_false;

	int AltitudeDif = 0;
	if (Ac.transponderModeC && Ac.previousPosition.valid)
		AltitudeDif = Ac.flightLevel - Ac.previousFlightLevel;

	if (Ac.groundSpeed > 160 || AltitudeDif > 200)
		return string_false;

	POINT AcPosPix = instance->ConvertCoordFromPositionToPixel(ToEuroScopePosition(Ac.position));

	for (std::map<string, RunwayAreaType>::iterator it = RunwayAreas.begin(); it != RunwayAreas.end(); ++it)
	{
		const auto monitoredDepIt = MonitoredRunwayDep.find(it->first);
		if (monitoredDepIt == MonitoredRunwayDep.end() || !monitoredDepIt->second)
			continue;

		const vector<POINT>* RunwayOnScreen = GetRunwayAreaScreenPoints(it->first, instance);
		if (RunwayOnScreen == nullptr)
			continue;

		if (Is_Inside(AcPosPix, *RunwayOnScreen)) {
			AcOnRunway.insert(std::pair<string, string>(it->first, Ac.callsign));
			AircraftOnRunway.insert(Ac.callsign);
			return string(it->first);
		}
	}

	return string_false;
}

string CRimcas::GetAcInRunwayAreaSoon(const VsmrScene::Target& Ac, CRadarScreen* instance) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (instance == nullptr || Ac.callsign.empty() || !Ac.position.valid)
		return string_false;

	int AltitudeDif = 0;
	if (Ac.transponderModeC && Ac.previousPosition.valid)
		AltitudeDif = Ac.flightLevel - Ac.previousFlightLevel;

	// Making sure the AC is airborne and not climbing, but below transition
	if (Ac.groundSpeed < 50 ||
		AltitudeDif > 50 ||
		Ac.pressureAltitude > TransitionAltitude)
		return string_false;

	// If the AC is already on the runway, then there is no point in this step
	if (isAcOnRunway(Ac.callsign))
		return string_false;

	for (std::map<string, RunwayAreaType>::iterator it = RunwayAreas.begin(); it != RunwayAreas.end(); ++it)
	{
		const auto monitoredArrIt = MonitoredRunwayArr.find(it->first);
		if (monitoredArrIt == MonitoredRunwayArr.end() || !monitoredArrIt->second)
			continue;

		// We need to know when and if the AC is going to enter the runway within 5 minutes (by steps of 10 seconds

		const vector<POINT>* RunwayOnScreen = GetRunwayAreaScreenPoints(it->first, instance);
		if (RunwayOnScreen == nullptr)
			continue;

		for (int t = 5; t <= 300; t += 5)
		{
			double distance = Ac.reportedGroundSpeed * 0.514444 * t;

			// We tolerate up 2 degree variations to the runway at long range (> 120 s)
			// And 3 degrees after (<= 120 t)

			bool isGoingToLand = false;
			int AngleMin = -2;
			int AngleMax = 2;
			if (t <= 120)
			{
				AngleMin = -3;
				AngleMax = 3;
			}

			for (int a = AngleMin; a <= AngleMax; a++)
			{
				POINT PredictedPosition = instance->ConvertCoordFromPositionToPixel(
					BetterHarversine(ToEuroScopePosition(Ac.position), fmod(Ac.trackHeadingDegrees + a, 360), distance));
				isGoingToLand = Is_Inside(PredictedPosition, *RunwayOnScreen);

				if (isGoingToLand)
					break;
			}

			if (isGoingToLand)
			{
				// The aircraft is going to be on the runway, we need to decide where it needs to be shown on the AIW
				bool first = true;
				vector<int> Definiton = CountdownDefinition;
				if (IsLVP)
					Definiton = CountdownDefinitionLVP;
				for (size_t k = 0; k < Definiton.size(); k++)
				{
					int Time = Definiton.at(k);

					int PreviousTime = 0;
					if (first)
					{
						PreviousTime = Time + 15;
						first = false;
					}
					else
					{
						PreviousTime = Definiton.at(k - 1);
					}
					if (t < PreviousTime && t >= Time)
					{
						TimeTable[it->first][Time] = Ac.callsign;
						break;
					}
				}

				// If the AC is xx seconds away from the runway, we consider him on it

				int StageTwoTrigger = 20;
				if (IsLVP)
					StageTwoTrigger = 30;

				if (t <= StageTwoTrigger)
				{
					AcOnRunway.insert(std::pair<string, string>(it->first, Ac.callsign));
					AircraftOnRunway.insert(Ac.callsign);
				}

				// If the AC is 45 seconds away from the runway, we consider him approaching

				if (t > StageTwoTrigger && t <= 45)
					ApproachingAircrafts.insert(std::pair<string, string>(it->first, Ac.callsign));

				return Ac.callsign;
			}
		}
	}

	return CRimcas::string_false;
}

vector<CPosition> CRimcas::GetRunwayArea(CPosition Left, CPosition Right, float hwidth) {
	Logger::info(string(__FUNCSIG__));
	vector<CPosition> out;

	double RunwayBearing = RadToDeg(TrueBearing(Left, Right));
	float padding = hwidth * 4;
	CPosition leftPadded = BetterHarversine(Left, fmod(RunwayBearing + 180, 360), padding);
	CPosition rightPadded = BetterHarversine(Right, fmod(RunwayBearing, 360), padding);
	out.push_back(BetterHarversine(leftPadded, fmod(RunwayBearing + 90, 360), hwidth)); // Bottom Left
	out.push_back(BetterHarversine(rightPadded, fmod(RunwayBearing + 90, 360), hwidth)); // Bottom Right
	out.push_back(BetterHarversine(rightPadded, fmod(RunwayBearing - 90, 360), hwidth)); // Top Right
	out.push_back(BetterHarversine(leftPadded, fmod(RunwayBearing - 90, 360), hwidth)); // Top Left

	return out;
}

void CRimcas::OnRefreshEnd(const VsmrScene::RadarScene& scene, int threshold) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));

	for (map<string, RunwayAreaType>::iterator it = RunwayAreas.begin(); it != RunwayAreas.end(); ++it)
	{
		const auto monitoredArrIt = MonitoredRunwayArr.find(it->first);
		const bool monitoredArr = (monitoredArrIt != MonitoredRunwayArr.end()) && monitoredArrIt->second;
		const auto monitoredDepIt = MonitoredRunwayDep.find(it->first);
		const bool monitoredDep = (monitoredDepIt != MonitoredRunwayDep.end()) && monitoredDepIt->second;
		if (!monitoredArr && !monitoredDep)
			continue;

		bool isOnClosedRunway = false;
		if (ClosedRunway.find(it->first) != ClosedRunway.end()) {
			if (ClosedRunway[it->first])
				isOnClosedRunway = true;
		}

		bool isAnotherAcApproaching = ApproachingAircrafts.count(it->first) > 0;

		if (AcOnRunway.count(it->first) > 1 || isOnClosedRunway || isAnotherAcApproaching) {

			auto AcOnRunwayRange = AcOnRunway.equal_range(it->first);

			for (map<string, string>::iterator it2 = AcOnRunwayRange.first; it2 != AcOnRunwayRange.second; ++it2)
			{
				if (it2->second.empty())
					continue;

				if (isOnClosedRunway) {
					AcColor[it2->second] = StageTwo;
				}
				else
				{
					const VsmrScene::Target* rd1 = scene.FindTarget(it2->second);
					if (rd1 == nullptr)
						continue;

					if (rd1->groundSpeed > threshold)
					{
						// If the aircraft is on the runway and stage two, we check if 
						// the aircraft is going towards any aircraft thats on the runway
						// if not, we don't display the warning
						bool triggerStageTwo = false;
						for (map<string, string>::iterator it3 = AcOnRunwayRange.first; it3 != AcOnRunwayRange.second; ++it3)
						{
							if (it3->second.empty())
								continue;

							const VsmrScene::Target* rd2 = scene.FindTarget(it3->second);
							if (rd2 == nullptr)
								continue;
							if (!rd1->position.valid || !rd2->position.valid ||
								!rd1->previousPosition.valid || !rd2->previousPosition.valid)
								continue;

							CPosition currentRd1 = ToEuroScopePosition(rd1->position);
							CPosition currentRd2 = ToEuroScopePosition(rd2->position);
							CPosition previousRd1 = ToEuroScopePosition(rd1->previousPosition);
							CPosition previousRd2 = ToEuroScopePosition(rd2->previousPosition);
							double currentDist = currentRd1.DistanceTo(currentRd2);
							double oldDist = previousRd1.DistanceTo(previousRd2);

							if (currentDist < oldDist)
							{
								triggerStageTwo = true;
								break;
							}
						}

						if (triggerStageTwo)
							AcColor[it2->second] = StageTwo;
					}
					else
					{
						AcColor[it2->second] = StageOne;
					}
				}
			}

			for (auto& ac : ApproachingAircrafts)
			{
				if (ac.first == it->first && AcOnRunway.count(it->first) > 1)
					AcColor[ac.second] = StageOne;

				if (ac.first == it->first && isOnClosedRunway)
					AcColor[ac.second] = StageTwo;
			}
		}

	}

}

bool CRimcas::isAcOnRunway(const string& callsign) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	return AircraftOnRunway.find(callsign) != AircraftOnRunway.end();
}

string CRimcas::AcOnRunwayFunc(const VsmrScene::Target& Rt, CRadarScreen* instance)
{
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (instance == nullptr || !Rt.position.valid)
		return string();
	POINT acPosPix = instance->ConvertCoordFromPositionToPixel(ToEuroScopePosition(Rt.position));
	for (const auto& rwy : RunwayAreas) {
		const vector<POINT>* runwayOnScreen = GetRunwayAreaScreenPoints(rwy.first, instance);
		if (runwayOnScreen != nullptr && Is_Inside(acPosPix, *runwayOnScreen)) {
			return rwy.first;
		}
	}
	return string();
}

void CRimcas::CheckForMovementAlert(const VsmrScene::Target& Rt, CRadarScreen* instance)
{
	if (Rt.callsign.empty())
		return;

	if (!Rt.hasCorrelatedFlightPlan || !Rt.position.valid) {
		movementAlerts[Rt.callsign] = CRimcas::RimcasAlerts::NONE;
		return;
	}
	string rwyOn = AcOnRunwayFunc(Rt, instance);
	int groundspeed = Rt.reportedGroundSpeed;
	const GroundStateCategory groundStateCategory = classifyGroundStateForCallsign(
		Rt.callsign.c_str(), Rt.towerModeGroundStateText.c_str(), groundspeed, !rwyOn.empty());
	const bool departureAuthorized = groundStateCategory == GroundStateCategory::Depa;
	const bool lineupAuthorized = groundStateCategory == GroundStateCategory::Lnup;
	const bool taxiAuthorized = groundStateCategory == GroundStateCategory::Taxi || departureAuthorized || lineupAuthorized;
	const bool pushAuthorized = groundStateCategory == GroundStateCategory::Push || groundStateCategory == GroundStateCategory::Taxi || lineupAuthorized;

	// RWY CLSD
	if (inactiveAlerts.find("RWY CLSD") == inactiveAlerts.end()) {
		if (rwyOn != "") {
			string rwy1 = rwyOn.substr(0, rwyOn.find(" / "));
			string rwy2 = rwyOn.substr(rwyOn.find(" / ") + 4);
			const auto rwy1StatusIt = RunwayStatuses.find(rwy1);
			const auto rwy2StatusIt = RunwayStatuses.find(rwy2);
			if (rwy1StatusIt != RunwayStatuses.end() &&
				rwy2StatusIt != RunwayStatuses.end() &&
				rwy1StatusIt->second == CLSD &&
				rwy2StatusIt->second == CLSD &&
				3 < groundspeed) {
				movementAlerts[Rt.callsign] = RWYCLSD;
				return;
			}
		}
	}
	
	// RWY TYPE
	if (inactiveAlerts.find("RWY TYPE") == inactiveAlerts.end()) {
		if (rwyOn != "") {
			string rwy1 = rwyOn.substr(0, rwyOn.find(" / "));
			string rwy2 = rwyOn.substr(rwyOn.find(" / ") + 4);
			const auto rwy1StatusIt = RunwayStatuses.find(rwy1);
			const auto rwy2StatusIt = RunwayStatuses.find(rwy2);
			const bool rwyOneIsArrival = (rwy1StatusIt != RunwayStatuses.end() && rwy1StatusIt->second == ARR);
			const bool rwyTwoIsArrival = (rwy2StatusIt != RunwayStatuses.end() && rwy2StatusIt->second == ARR);
			if ((rwyOneIsArrival || rwyTwoIsArrival) && 3 < groundspeed) {
				movementAlerts[Rt.callsign] = RWYTYPE;
				return;
			}
		}
	}

	// RWY INCURSION
	if (inactiveAlerts.find("RWY INC") == inactiveAlerts.end()) {
		if (!departureAuthorized && !lineupAuthorized) {
			if (rwyOn != "") {
				movementAlerts[Rt.callsign] = RWYINC;
				return;
			}
		}
	}

	// STAT RPA
	if (inactiveAlerts.find("STAT RPA") == inactiveAlerts.end()) {
		if (departureAuthorized && 0 == groundspeed) {
			movementAlerts[Rt.callsign] = STATRPA;
			return;
		}
	}

	int headingDiffRaw = std::abs(static_cast<int>(Rt.trackHeadingDegrees) - Rt.reportedHeadingDegrees);
	int headingDiff = headingDiffRaw % 360;
	if (headingDiff > 180) headingDiff = 360 - headingDiff;
	bool isReversing = headingDiff >= 100;
	// NO PUSH
	if (inactiveAlerts.find("NO PUSH") == inactiveAlerts.end()) {
		if (!pushAuthorized && 2 < groundspeed && isReversing) {
			movementAlerts[Rt.callsign] = NOPUSH;
			return;
		}
	}

	// HIGHS SPD
	if (inactiveAlerts.find("HIGH SPD") == inactiveAlerts.end()) {
		int speedThreashold = IsLVP ? 25 : 35;
		if (!departureAuthorized && speedThreashold < groundspeed && rwyOn == "") {
			movementAlerts[Rt.callsign] = HIGHSPD;
			return;
		}
	}

	// NO TKOF
	if (inactiveAlerts.find("NO TKOF") == inactiveAlerts.end()) {
		if (!departureAuthorized && 35 < groundspeed && rwyOn != "") {
			movementAlerts[Rt.callsign] = NOTKOF;
			return;
		}
	}

	// NO TAXI
	if (inactiveAlerts.find("NO TAXI") == inactiveAlerts.end()) {
		if (!taxiAuthorized && 5 < groundspeed && !isReversing) {
			movementAlerts[Rt.callsign] = NOTAXI;
			return;
		}
	}

	// EMERG
	if (inactiveAlerts.find("EMERG") == inactiveAlerts.end()) {
		if (Rt.reportedSquawk == "7700") {
			movementAlerts[Rt.callsign] = EMERG;
			return;
		}
	}

	movementAlerts[Rt.callsign] = CRimcas::RimcasAlerts::NONE;
}

CRimcas::RimcasAlertTypes CRimcas::getAlert(const string& callsign)
{
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	const auto alertIt = AcColor.find(callsign);
	if (alertIt == AcColor.end())
		return NoAlert;

	return alertIt->second;
}

CRimcas::RimcasAlerts CRimcas::getMovementAlert(const string& callsign)
{
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	const auto alertIt = movementAlerts.find(callsign);
	if (alertIt == movementAlerts.end())
		return CRimcas::RimcasAlerts::NONE;

	return alertIt->second;
}

CRimcas::RimcasAlertSeverity CRimcas::getAlertSeverity(RimcasAlerts alert)
{
	switch (alert)
	{
	case CRimcas::XPDRSTDBY:
		return RimcasAlertSeverity::WARNING;
		break;
	case CRimcas::NOPUSH:
		return RimcasAlertSeverity::CAUTION;
		break;
	case CRimcas::NOTAXI:
		return RimcasAlertSeverity::CAUTION;
		break;
	case CRimcas::NOTKOF:
		return RimcasAlertSeverity::WARNING;
		break;
	case CRimcas::STATRPA:
		return RimcasAlertSeverity::WARNING;
		break;
	case CRimcas::RWYINC:
		return RimcasAlertSeverity::WARNING;
		break;
	case CRimcas::HIGHSPD:
		return RimcasAlertSeverity::CAUTION;
		break;
	case CRimcas::RWYTYPE:
		return RimcasAlertSeverity::CAUTION;
		break;
	case CRimcas::RWYCLSD:
		return RimcasAlertSeverity::WARNING;
		break;
	case CRimcas::EMERG:
		return RimcasAlertSeverity::WARNING;
		break;
	default:
		return RimcasAlertSeverity::CAUTION;
		break;
	}
}

Color CRimcas::GetAircraftColor(const string& AcCallsign, Color StandardColor, Color OnRunwayColor, Color RimcasStageOne, Color RimcasStageTwo) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	const auto colorIt = AcColor.find(AcCallsign);
	if (colorIt == AcColor.end()) {
		if (isAcOnRunway(AcCallsign)) {
			return OnRunwayColor;
		}
		else {
			return StandardColor;
		}
	}
	else {
		if (colorIt->second == StageOne) {
			return RimcasStageOne;
		}
		else {
			return RimcasStageTwo;
		}
	}
}

Color CRimcas::GetAircraftColor(const string& AcCallsign, Color StandardColor, Color OnRunwayColor) {
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (isAcOnRunway(AcCallsign)) {
		return OnRunwayColor;
	}
	else {
		return StandardColor;
	}
}
