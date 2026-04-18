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
#include "SMRRadar.hpp"
#include "Logger.h"
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#define MY_PLUGIN_NAME      "vSMR"
#define MY_PLUGIN_VERSION   "v1.0.8"
#define MY_PLUGIN_DEVELOPER "Pierre Ferran, Even Rognlien, Lionel Bischof, Daniel Lange, Juha Holopainen, Keanu Czirjak"
#define MY_PLUGIN_COPYRIGHT "GPL v3"
#define MY_PLUGIN_VIEW_AVISO  "SMR radar display"

using namespace std;
using namespace EuroScopePlugIn;

class CSMRPlugin :
	public EuroScopePlugIn::CPlugIn
{
public:
	CSMRPlugin();
	virtual ~CSMRPlugin();

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

	//---OnRadarScreenCreated------------------------------------------

	virtual CRadarScreen * OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated);

private:
	enum class CdmQueueReminderOutcome
	{
		Queued = 0,
		AlreadyNotified,
		AlreadyQueued,
		AlreadyCleared,
		Failed
	};

	struct QueuedCdmReminderMessage
	{
		std::string callsign;
		int sendAttempts = 0;
	};

	struct CdmReminderQueueBatchStats
	{
		int queued = 0;
		int alreadyNotified = 0;
		int alreadyQueued = 0;
		int alreadyCleared = 0;
		int failed = 0;
	};

	static constexpr std::time_t CdmWarningCooldownSeconds = 60;
	static constexpr int CdmReminderQueueMaxSendAttempts = 20;

	std::set<std::string> AircraftDatalinkClearedCallsigns;
	std::map<std::string, std::time_t> AircraftCdmReminderSentUtc;
	std::map<std::string, std::time_t> AircraftCdmAutoTrackedUtc;
	std::deque<QueuedCdmReminderMessage> CdmReminderMessageQueue;
	bool CdmAutoModeEnabled = false;
	int CdmAutoDelayMinutes = 5;
	int CdmReminderCooldownMinutes = 60;
	std::time_t CdmLastAliasWarningUtc = 0;
	std::time_t CdmLastInjectionWarningUtc = 0;
	std::string CdmCachedReminderAliasPath;
	std::string CdmCachedReminderMessage;
	unsigned long long CdmCachedReminderAliasWriteTicks = 0;
	bool CdmCachedReminderAliasHasWriteTicks = false;
	bool CdmCachedReminderMessageValid = false;
	std::mutex CdmAutoStateMutex;

	static int ClampNonNegativeMinutes(int minutes);
	static std::string FormatMinutesLabel(int minutes);
	void SetCdmAutoModeState(bool enabled, bool updateDelayMinutes, int delayMinutes);
	std::string BuildCdmCooldownStatusMessage(const char* action) const;
	std::string BuildCdmAutoStatusMessage(const char* action) const;
	std::string BuildCdmQueueSummaryMessage(int checkedCount, const CdmReminderQueueBatchStats& stats) const;

	bool IsGroundTargetForCdm(const CFlightPlan& fp) const;
	std::string ResolveActiveAirportFilterUpper() const;
	std::vector<std::string> CollectCdmReminderCandidatesForActiveAirport();

	void PruneReminderHistoryUnlocked(std::time_t nowUtc);
	bool HasRecentReminderUnlocked(const std::string& callsign, std::time_t nowUtc) const;
	void MarkReminderSentUnlocked(const std::string& callsign, std::time_t nowUtc);
	bool IsCdmReminderQueuedUnlocked(const std::string& callsign) const;
	bool QueueCdmReminderUnlocked(const std::string& callsign);
	void RemoveQueuedCdmReminderUnlocked(const std::string& callsign);
	bool HasDatalinkClearanceSentUnlocked(const std::string& callsign) const;
	void MarkDatalinkClearanceSentUnlocked(const std::string& callsign);
	void ClearDatalinkClearanceSentUnlocked(const std::string& callsign);
	void ClearCdmTrackingStateForCallsignUnlocked(const std::string& callsign);
	CdmQueueReminderOutcome TryQueueCdmReminderForCallsign(const std::string& callsign, std::time_t nowUtc);
	CdmReminderQueueBatchStats QueueCdmRemindersForCallsigns(const std::vector<std::string>& callsigns, std::time_t nowUtc);

	std::string ResolveCdmAliasPath() const;
	bool TryGetFileLastWriteTicks(const std::string& aliasPath, unsigned long long& outTicks) const;
	bool TryReadCdmReminderMessageFromAlias(const std::string& aliasPath, std::string& outMessage) const;
	void NotifyMissingCdmAliasMessage(const std::string& aliasPath);
	bool TryLoadCdmReminderMessage(std::string& outMessage);

	bool ExecuteEuroScopeCommandViaUi(const std::string& command);
	bool SendPrivateChatMessageLikeDotMsg(const std::string& callsign, const std::string& message);
	bool IsCallsignEligibleForCdmReminderNow(const std::string& callsign);
	void ProcessAutomaticCdmReminder();
	void ProcessQueuedCdmReminderMessages();

	static void __cdecl SendDatalinkClearanceThread(void* arg);
};

