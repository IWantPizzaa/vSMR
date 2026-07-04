#pragma once
#include <EuroScopePlugIn.h>
#include <iostream>
#include <vector>
#include <map>
#include <unordered_set>
#include <string>
#include <algorithm>
#define _USE_MATH_DEFINES
#include <math.h>
#include "Constant.hpp"
#include <functional>
#include "Logger.h"

class CSMRRadar;
using namespace std;
using namespace Gdiplus;
using namespace EuroScopePlugIn;

class CRimcas {
public:
	CRimcas();
	virtual ~CRimcas();

	const string string_false = "!NO";

	struct RunwayAreaType {
		string Name = "";
		vector<CPosition> Definition;
		bool set = false;
	};

	COLORREF WarningColor = RGB(160, 90, 30); //RGB(180, 100, 50)
	COLORREF AlertColor = RGB(150, 0, 0);

	enum RimcasAlertTypes { NoAlert, StageOne, StageTwo };
	enum RimcasAlerts {NONE, XPDRSTDBY, NOPUSH, NOTAXI, NOTKOF, STATRPA, RWYINC, HIGHSPD, RWYCLSD, RWYTYPE, EMERG};
	enum RimcasAlertSeverity { WARNING, CAUTION };
	enum RunwayStatus { DEP, ARR, BOTH, CLSD};

	map<string, RunwayAreaType> RunwayAreas;
	map<string, RunwayStatus> RunwayStatuses;
	multimap<string, string> AcOnRunway;
	vector<int> CountdownDefinition;
	vector<int> CountdownDefinitionLVP;
	multimap<string, string> ApproachingAircrafts;
	map<string, map<int, string>> TimeTable;
	unordered_set<string> inactiveAlerts;
	map<string, RimcasAlerts> movementAlerts;
	map<string, bool> MonitoredRunwayDep;
	map<string, bool> MonitoredRunwayArr;
	map<string, RimcasAlertTypes> AcColor;

	bool IsLVP = false;

	int Is_Left(const POINT &p0, const POINT &p1, const POINT &point)
	{
		return ((p1.x - p0.x) * (point.y - p0.y) -
			(point.x - p0.x) * (p1.y - p0.y));
	}

	bool Is_Inside(const POINT &point, const std::vector<POINT> &points_list)
	{
		// The winding number counter.
		int winding_number = 0;

		// Loop through all edges of the polygon.
		typedef std::vector<POINT>::size_type size_type;

		size_type size = points_list.size();

		for (size_type i = 0; i < size; ++i)             // Edge from point1 to points_list[i+1]
		{
			POINT point1(points_list[i]);
			POINT point2;

			// Wrap?
			if (i == (size - 1))
			{
				point2 = points_list[0];
			}
			else
			{
				point2 = points_list[i + 1];
			}

			if (point1.y <= point.y)                                    // start y <= point.y
			{
				if (point2.y > point.y)                                 // An upward crossing
				{
					if (Is_Left(point1, point2, point) > 0)             // Point left of edge
					{
						++winding_number;                               // Have a valid up intersect
					}
				}
			}
			else
			{
				// start y > point.y (no test needed)
				if (point2.y <= point.y)                                // A downward crossing
				{
					if (Is_Left(point1, point2, point) < 0)             // Point right of edge
					{
						--winding_number;                               // Have a valid down intersect
					}
				}
			}
		}

		return (winding_number != 0);
	}

	string GetAcInRunwayArea(CRadarTarget Ac, CRadarScreen *instance);
	string GetAcInRunwayAreaSoon(CRadarTarget Ac, CRadarScreen *instance, bool isCorrelated);
	void AddRunwayArea(CRadarScreen *instance, string runway_name1, string runway_name2, vector<CPosition> Definition);
	void SetRunwayStatus(string runway, RunwayStatus status) { RunwayStatuses[runway] = status; }
	const map<string, RunwayStatus>& GetRunwayStatuses() const { return RunwayStatuses; }
	Color GetAircraftColor(string AcCallsign, Color StandardColor, Color OnRunwayColor, Color RimcasStageOne, Color RimcasStageTwo);
	Color GetAircraftColor(string AcCallsign, Color StandardColor, Color OnRunwayColor);
	const unordered_set<string>& GetInactiveAlerts() const { return inactiveAlerts; }

	bool isAcOnRunway(string callsign);
	string AcOnRunwayFunc(CRadarTarget Rt, CRadarScreen* instance);
	void CheckForMovementAlert(CRadarTarget Rt, CRadarScreen* instance, bool isLVP);

	vector<CPosition> GetRunwayArea(CPosition Left, CPosition Right, float hwidth = 92.5f);

	void OnRefreshBegin(bool isLVP);
	void OnRefresh(CRadarTarget Rt, CRadarScreen *instance, bool isCorrelated, bool isLVP);
	void OnRefreshEnd(CRadarScreen *instance, int threshold);
	void Reset();

	RimcasAlertTypes getAlert(string callsign);
	RimcasAlerts getMovementAlert(string callsign);
	RimcasAlertSeverity getAlertSeverity(RimcasAlerts alert);

	void setInactiveAlerts(const unordered_set<string>& alerts) {
		inactiveAlerts = alerts;
	}

	void setCountdownDefinition(vector<int> data, vector<int> dataLVP)
	{
		CountdownDefinition = data;
		std::sort(CountdownDefinition.begin(), CountdownDefinition.end(), std::greater<int>());

		CountdownDefinitionLVP = dataLVP;
		std::sort(CountdownDefinitionLVP.begin(), CountdownDefinitionLVP.end(), std::greater<int>());
	}

	void toggleClosedRunway(string runway) {
		if (ClosedRunway.find(runway) == ClosedRunway.end())
			ClosedRunway[runway] = true;
		else
			ClosedRunway[runway] = !ClosedRunway[runway];
	}

	void toggleActiveAlert(string alert) {
		if (inactiveAlerts.find(alert) == inactiveAlerts.end())
			inactiveAlerts.insert(alert);
		else
			inactiveAlerts.erase(alert);
	}

	void toggleMonitoredRunwayDep(string runway) {
		if (MonitoredRunwayDep.find(runway) == MonitoredRunwayDep.end())
			MonitoredRunwayDep[runway] = true;
		else
			MonitoredRunwayDep[runway] = !MonitoredRunwayDep[runway];
	}

	void toggleMonitoredRunwayArr(string runway) {
		if (MonitoredRunwayArr.find(runway) == MonitoredRunwayArr.end())
			MonitoredRunwayArr[runway] = true;
		else
			MonitoredRunwayArr[runway] = !MonitoredRunwayArr[runway];
	}

	map<string, bool> ClosedRunway;
};
