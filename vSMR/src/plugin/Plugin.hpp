#pragma once
#include "EuroScopePlugIn.h"
#include "platform/windows/network/HttpHelper.hpp"
#include "datalink/CPDLCSettingsDialog.hpp"
#include "datalink/DataLinkDialog.hpp"
#include <string>
#include <algorithm>
#include "radar/RadarUiSupport.hpp"
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
#include "radar/RadarScreen.hpp"
#include "shared/logging/Logger.hpp"

#define MY_PLUGIN_NAME      "vSMR"
#define MY_PLUGIN_VERSION   "v2.0.0-beta.3"
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
	bool vacdmReady = false;
	std::string activeAirport;
	std::string cdmAliasPath;
	bool cdmAliasReady = false;
	std::string statusMessage;
};

struct WorkerQueueSnapshot
{
	std::size_t networkWorkers = 0;
	std::size_t networkQueued = 0;
	std::size_t networkInFlight = 0;
	bool weatherWorkerRunning = false;
	std::size_t weatherQueued = 0;
	std::size_t weatherInFlight = 0;
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
		std::string& error,
		bool updateConnectionSettings = true);
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
	WorkerQueueSnapshot GetWorkerQueueSnapshot();

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

	//---OnControllerPositionUpdate/Disconnect----------------------

	void OnControllerPositionUpdate(CController Controller) override;
	void OnControllerDisconnect(CController Controller) override;

	//---OnRadarScreenCreated------------------------------------------

	virtual CRadarScreen * OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated);

private:
	bool WriteDiagnosticsReport(
		std::string& reportPath,
		std::string& error);
	void NetworkWorkerMain();
	void QueueWeatherFetch(const std::string& station);
	void WeatherFetchThreadMain();
	void RefreshAvisoFrequencyOwnershipOverlays();

	std::mutex NetworkWorkerMutex;
	std::condition_variable NetworkWorkerCondition;
	std::vector<std::thread> NetworkWorkers;
	std::deque<std::function<void()>> NetworkJobs;
	std::size_t NetworkWorkerThreadsRunning = 0;
	std::size_t NetworkJobsInFlight = 0;
	std::atomic<bool> NetworkCancellationRequested{ false };
	bool NetworkWorkersStopping = false;

	std::mutex WeatherFetchMutex;
	std::condition_variable WeatherFetchCondition;
	std::thread WeatherFetchThread;
	std::atomic<bool> WeatherFetchCancellationRequested{ false };
	bool WeatherFetchStop = false;
	bool WeatherWorkerRunning = false;
	std::deque<std::string> WeatherFetchQueue;
	std::size_t WeatherFetchesInFlight = 0;
	std::set<std::string> WeatherFetchQueued;
	std::map<std::string, std::time_t> WeatherLastAttemptUtc;
};

// Called by the runtime ABI when EuroScope unloads the plug-in. Returns true
// only after every radar screen has already released itself and the plug-in
// instance has been deleted, as required by the EuroScope SDK contract.
bool VsmrShutdownPlugin();

