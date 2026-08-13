#pragma once
#include "EuroScopePlugIn.h"
#include "HttpHelper.hpp"
#include "CPDLCSettingsDialog.hpp"
#include "DataLinkDialog.hpp"
#include <string>
#include <algorithm>
#include "Constant.hpp"
#include "Mmsystem.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "SMRRadar.hpp"
#include "Logger.h"

#define MY_PLUGIN_NAME      "vSMR"
#define MY_PLUGIN_VERSION   "v2.0.0-beta.2"
#define MY_PLUGIN_DEVELOPER "Mathias Derelle, Alexis Balzano, Pierre Ferran, Even Rognlien, Lionel Bischof, Daniel Lange, Juha Holopainen, Keanu Czirjak"
#define MY_PLUGIN_COPYRIGHT "GPL v3"
#define MY_PLUGIN_VIEW_AVISO  "SMR radar display"

using namespace std;
using namespace EuroScopePlugIn;

struct DatalinkControlState
{
	bool connected = false;
	bool connecting = false;
	bool pollInProgress = false;
	bool controllerConnected = false;
	std::string logonCallsign;
	bool hasPassword = false;
	bool playSound = false;
	bool cdmAutoEnabled = false;
	int cdmDelayMinutes = 5;
	int cdmCooldownMinutes = 60;
	bool vacdmConfigured = false;
	std::string activeAirport;
	std::string cdmAliasPath;
	bool cdmAliasReady = false;
	std::string statusMessage;
};

class CSMRPlugin :
	public EuroScopePlugIn::CPlugIn
{
public:
	CSMRPlugin();
	virtual ~CSMRPlugin();

	DatalinkControlState GetDatalinkControlState() const;
	bool UpdateDatalinkControlSettings(
		const std::string& callsign,
		const std::string& password,
		bool replacePassword,
		bool playSound,
		bool cdmAutoEnabled,
		int delayMinutes,
		int cooldownMinutes,
		std::string& error);
	bool ConnectDatalink(std::string& error);
	bool DisconnectDatalink(std::string& error);
	bool PollDatalink(std::string& error);
	bool RunCdmReminderScan(std::string& result, std::string& error);
	static std::string GetActiveProfilesConfigPath(
		bool* selectionClaimed = nullptr);
	static void PublishActiveProfilesConfigPath(
		const std::string& path,
		bool claimSelection);
	bool QueueNetworkJob(std::function<void()> job);
	void StopNetworkWorkers();
	void StopWeatherFetchWorker();

	//---OnCompileCommand------------------------------------------

	virtual bool OnCompileCommand(const char * sCommandLine);

	//---OnFunctionCall------------------------------------------

	virtual void OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area);

	//---OnGetTagItem------------------------------------------

	virtual void OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int * pColorCode, COLORREF * pRGB, double * pFontSize);

	//---OnFlightPlanDisconnect------------------------------------------

	virtual void OnFlightPlanDisconnect(CFlightPlan FlightPlan);

	//---OnTimer------------------------------------------

	virtual void OnTimer(int Counter);

	//---OnNewMetarReceived------------------------------------------

	virtual void OnNewMetarReceived(const char* sStation, const char* sFullMetar);

	//---OnAirportRunwayActivityChanged-----------------------------

	void OnAirportRunwayActivityChanged() override;

	//---OnRadarScreenCreated------------------------------------------

	virtual CRadarScreen * OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated);

private:
	bool WriteDiagnosticsReport(
		std::string& reportPath,
		std::string& error);
	void NetworkWorkerMain();
	void QueueWeatherFetch(const std::string& station);
	void WeatherFetchThreadMain();

	std::mutex NetworkWorkerMutex;
	std::condition_variable NetworkWorkerCondition;
	std::vector<std::thread> NetworkWorkers;
	std::deque<std::function<void()>> NetworkJobs;
	std::atomic<bool> NetworkCancellationRequested{ false };
	bool NetworkWorkersStopping = false;

	std::mutex WeatherFetchMutex;
	std::condition_variable WeatherFetchCondition;
	std::thread WeatherFetchThread;
	std::atomic<bool> WeatherFetchCancellationRequested{ false };
	bool WeatherFetchStop = false;
	std::deque<std::string> WeatherFetchQueue;
	std::set<std::string> WeatherFetchQueued;
	std::map<std::string, std::time_t> WeatherLastAttemptUtc;
};

