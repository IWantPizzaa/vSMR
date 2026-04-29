#include "stdafx.h"
#include "Rimcas.hpp"

namespace
{
	std::string SafeString(const char* text)
	{
		return text != nullptr ? std::string(text) : std::string();
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
	AcColor.clear();
	AcOnRunway.clear();
	TimeTable.clear();
	inactiveAlerts.clear();
	MonitoredRunwayArr.clear();
	MonitoredRunwayDep.clear();
	ApproachingAircrafts.clear();
}

void CRimcas::OnRefreshBegin(bool isLVP) {
	Logger::info(string(__FUNCSIG__));
	AcColor.clear();
	AcOnRunway.clear();
	TimeTable.clear();
	ApproachingAircrafts.clear();
	this->IsLVP = isLVP;
	movementAlerts.clear();
}

void CRimcas::OnRefresh(CRadarTarget Rt, CRadarScreen* instance, bool isCorrelated, bool isLVP) {
	Logger::info(string(__FUNCSIG__));
	GetAcInRunwayArea(Rt, instance);
	GetAcInRunwayAreaSoon(Rt, instance, isCorrelated);
	CheckForMovementAlert(Rt, instance, isLVP);
}

void CRimcas::AddRunwayArea(CRadarScreen* instance, string runway_name1, string runway_name2, vector<CPosition> Definition) {
	Logger::info(string(__FUNCSIG__));
	string Name = runway_name1 + " / " + runway_name2;

	RunwayAreaType Runway;
	Runway.Name = Name;
	Runway.Definition = Definition;

	RunwayAreas[Name] = Runway;
}

string CRimcas::GetAcInRunwayArea(CRadarTarget Ac, CRadarScreen* instance) {
	Logger::info(string(__FUNCSIG__));
	const char* acCallsign = Ac.GetCallsign();
	if (acCallsign == nullptr || acCallsign[0] == '\0')
		return string_false;

	CRadarTargetPositionData currentPos = Ac.GetPosition();
	if (!currentPos.IsValid())
		return string_false;

	int AltitudeDif = 0;
	if (currentPos.GetTransponderC())
	{
		CRadarTargetPositionData previousPos = Ac.GetPreviousPosition(currentPos);
		if (previousPos.IsValid())
			AltitudeDif = currentPos.GetFlightLevel() - previousPos.GetFlightLevel();
	}

	if (Ac.GetGS() > 160 || AltitudeDif > 200)
		return string_false;

	POINT AcPosPix = instance->ConvertCoordFromPositionToPixel(currentPos.GetPosition());

	for (std::map<string, RunwayAreaType>::iterator it = RunwayAreas.begin(); it != RunwayAreas.end(); ++it)
	{
		const auto monitoredDepIt = MonitoredRunwayDep.find(it->first);
		if (monitoredDepIt == MonitoredRunwayDep.end() || !monitoredDepIt->second)
			continue;

		vector<POINT> RunwayOnScreen;

		for (auto& Point : it->second.Definition)
		{
			RunwayOnScreen.push_back(instance->ConvertCoordFromPositionToPixel(Point));
		}

		if (Is_Inside(AcPosPix, RunwayOnScreen)) {
			AcOnRunway.insert(std::pair<string, string>(it->first, acCallsign));
			return string(it->first);
		}
	}

	return string_false;
}

string CRimcas::GetAcInRunwayAreaSoon(CRadarTarget Ac, CRadarScreen* instance, bool isCorrelated) {
	Logger::info(string(__FUNCSIG__));
	const char* acCallsign = Ac.GetCallsign();
	if (acCallsign == nullptr || acCallsign[0] == '\0')
		return string_false;

	CRadarTargetPositionData currentPos = Ac.GetPosition();
	if (!currentPos.IsValid())
		return string_false;

	int AltitudeDif = 0;
	if (currentPos.GetTransponderC())
	{
		CRadarTargetPositionData previousPos = Ac.GetPreviousPosition(currentPos);
		if (previousPos.IsValid())
			AltitudeDif = currentPos.GetFlightLevel() - previousPos.GetFlightLevel();
	}

	// Making sure the AC is airborne and not climbing, but below transition
	if (Ac.GetGS() < 50 ||
		AltitudeDif > 50 ||
		currentPos.GetPressureAltitude() > instance->GetPlugIn()->GetTransitionAltitude())
		return string_false;

	// If the AC is already on the runway, then there is no point in this step
	if (isAcOnRunway(acCallsign))
		return string_false;

	POINT AcPosPix = instance->ConvertCoordFromPositionToPixel(currentPos.GetPosition());

	for (std::map<string, RunwayAreaType>::iterator it = RunwayAreas.begin(); it != RunwayAreas.end(); ++it)
	{
		const auto monitoredArrIt = MonitoredRunwayArr.find(it->first);
		if (monitoredArrIt == MonitoredRunwayArr.end() || !monitoredArrIt->second)
			continue;

		// We need to know when and if the AC is going to enter the runway within 5 minutes (by steps of 10 seconds

		vector<POINT> RunwayOnScreen;

		for (auto& Point : it->second.Definition)
		{
			RunwayOnScreen.push_back(instance->ConvertCoordFromPositionToPixel(Point));
		}

		for (int t = 5; t <= 300; t += 5)
		{
			double distance = currentPos.GetReportedGS() * 0.514444 * t;

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
					BetterHarversine(currentPos.GetPosition(), fmod(Ac.GetTrackHeading() + a, 360), distance));
				isGoingToLand = Is_Inside(PredictedPosition, RunwayOnScreen);

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
						TimeTable[it->first][Time] = acCallsign;
						break;
					}
				}

				// If the AC is xx seconds away from the runway, we consider him on it

				int StageTwoTrigger = 20;
				if (IsLVP)
					StageTwoTrigger = 30;

				if (t <= StageTwoTrigger)
					AcOnRunway.insert(std::pair<string, string>(it->first, acCallsign));

				// If the AC is 45 seconds away from the runway, we consider him approaching

				if (t > StageTwoTrigger && t <= 45)
					ApproachingAircrafts.insert(std::pair<string, string>(it->first, acCallsign));

				return acCallsign;
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

void CRimcas::OnRefreshEnd(CRadarScreen* instance, int threshold) {
	Logger::info(string(__FUNCSIG__));
	if (instance == nullptr)
		return;

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
					CRadarTarget rd1 = instance->GetPlugIn()->RadarTargetSelect(it2->second.c_str());
					if (!rd1.IsValid())
						continue;

					if (rd1.GetGS() > threshold)
					{
						// If the aircraft is on the runway and stage two, we check if 
						// the aircraft is going towards any aircraft thats on the runway
						// if not, we don't display the warning
						bool triggerStageTwo = false;
						CRadarTargetPositionData currentRd1 = rd1.GetPosition();
						for (map<string, string>::iterator it3 = AcOnRunwayRange.first; it3 != AcOnRunwayRange.second; ++it3)
						{
							if (it3->second.empty())
								continue;

							CRadarTarget rd2 = instance->GetPlugIn()->RadarTargetSelect(it3->second.c_str());
							if (!rd2.IsValid())
								continue;
							CRadarTargetPositionData currentRd2 = rd2.GetPosition();
							CRadarTargetPositionData previousRd1 = rd1.GetPreviousPosition(currentRd1);
							CRadarTargetPositionData previousRd2 = rd2.GetPreviousPosition(currentRd2);
							if (!currentRd1.IsValid() || !currentRd2.IsValid() || !previousRd1.IsValid() || !previousRd2.IsValid())
								continue;

							double currentDist = currentRd1.GetPosition().DistanceTo(currentRd2.GetPosition());
							double oldDist = previousRd1.GetPosition().DistanceTo(previousRd2.GetPosition());

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

bool CRimcas::isAcOnRunway(string callsign) {
	Logger::info(string(__FUNCSIG__));
	for (std::map<string, string>::iterator it = AcOnRunway.begin(); it != AcOnRunway.end(); ++it)
	{
		if (it->second == callsign)
			return true;
	}

	return false;
}

string CRimcas::AcOnRunwayFunc(CRadarTarget Rt, CRadarScreen* instance)
{
	Logger::info(string(__FUNCSIG__));
	for (const auto& rwy : RunwayAreas) {
		POINT acPosPix = instance->ConvertCoordFromPositionToPixel(Rt.GetPosition().GetPosition());
		vector<POINT> runwayOnScreen;
		for (const auto& point : rwy.second.Definition) {
			runwayOnScreen.push_back(instance->ConvertCoordFromPositionToPixel(point));
		}
		if (Is_Inside(acPosPix, runwayOnScreen)) {
			return rwy.first;
		}
	}
	return string();
}

void CRimcas::CheckForMovementAlert(CRadarTarget Rt, CRadarScreen* instance, bool isLVP)
{
	const char* rtCallsign = Rt.GetCallsign();
	if (rtCallsign == nullptr || rtCallsign[0] == '\0')
		return;

	CFlightPlan fp = Rt.GetCorrelatedFlightPlan();
	CRadarTargetPositionData pos = Rt.GetPosition();
	if (false == fp.IsValid() || false == pos.IsValid()) {
		movementAlerts[rtCallsign] = CRimcas::RimcasAlerts::NONE;
		return;
	}
	std::string groundstate = SafeString(fp.GetGroundState());
	string rwyOn = AcOnRunwayFunc(Rt, instance);
	int groundspeed = pos.GetReportedGS();

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
				movementAlerts[rtCallsign] = RWYCLSD;
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
				movementAlerts[rtCallsign] = RWYTYPE;
				return;
			}
		}
	}

	// RWY INCURSION
	if (inactiveAlerts.find("RWY INC") == inactiveAlerts.end()) {
		if ("DEPA" != groundstate) {
			if (rwyOn != "") {
				movementAlerts[rtCallsign] = RWYINC;
				return;
			}
		}
	}

	// STAT RPA
	if (inactiveAlerts.find("STAT RPA") == inactiveAlerts.end()) {
		if ("DEPA" == groundstate && 0 == groundspeed) {
			movementAlerts[rtCallsign] = STATRPA;
			return;
		}
	}

	int headingDiffRaw = std::abs(static_cast<int>(Rt.GetTrackHeading()) - pos.GetReportedHeading());
	int headingDiff = headingDiffRaw % 360;
	if (headingDiff > 180) headingDiff = 360 - headingDiff;
	bool isReversing = headingDiff >= 100;
	// NO PUSH
	if (inactiveAlerts.find("NO PUSH") == inactiveAlerts.end()) {
		if ("PUSH" != groundstate && "TAXI" != groundstate && 2 < groundspeed && isReversing) {
			movementAlerts[rtCallsign] = NOPUSH;
			return;
		}
	}

	// HIGHS SPD
	if (inactiveAlerts.find("HIGH SPD") == inactiveAlerts.end()) {
		int speedThreashold = isLVP ? 25 : 35;
		if ("DEPA" != groundstate && speedThreashold < groundspeed && rwyOn == "") {
			movementAlerts[rtCallsign] = HIGHSPD;
			return;
		}
	}

	// NO TKOF
	if (inactiveAlerts.find("NO TKOF") == inactiveAlerts.end()) {
		if ("DEPA" != groundstate && 35 < groundspeed && rwyOn != "") {
			movementAlerts[rtCallsign] = NOTKOF;
			return;
		}
	}

	// NO TAXI
	if (inactiveAlerts.find("NO TAXI") == inactiveAlerts.end()) {
		if ("TAXI" != groundstate && "DEPA" != groundstate && 5 < groundspeed && !isReversing) {
			movementAlerts[rtCallsign] = NOTAXI;
			return;
		}
	}

	// EMERG
	if (inactiveAlerts.find("EMERG") == inactiveAlerts.end()) {
		const char* squawk = pos.GetSquawk();
		if (squawk != nullptr && strcmp(squawk, "7700") == 0) {
			movementAlerts[rtCallsign] = EMERG;
			return;
		}
	}

	movementAlerts[rtCallsign] = CRimcas::RimcasAlerts::NONE;
}

CRimcas::RimcasAlertTypes CRimcas::getAlert(string callsign)
{
	Logger::info(string(__FUNCSIG__));
	const auto alertIt = AcColor.find(callsign);
	if (alertIt == AcColor.end())
		return NoAlert;

	return alertIt->second;
}

CRimcas::RimcasAlerts CRimcas::getMovementAlert(string callsign)
{
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

Color CRimcas::GetAircraftColor(string AcCallsign, Color StandardColor, Color OnRunwayColor, Color RimcasStageOne, Color RimcasStageTwo) {
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

Color CRimcas::GetAircraftColor(string AcCallsign, Color StandardColor, Color OnRunwayColor) {
	Logger::info(string(__FUNCSIG__));
	if (isAcOnRunway(AcCallsign)) {
		return OnRunwayColor;
	}
	else {
		return StandardColor;
	}
}
