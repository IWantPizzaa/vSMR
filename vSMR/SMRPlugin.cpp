#include "stdafx.h"
#include "SMRPlugin.hpp"
#include <atomic>
#include <mutex>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <set>
#include <fstream>

bool Logger::ENABLED;
string Logger::DLL_PATH;

bool HoppieConnected = false;
bool ConnectionMessage = false;
bool FailedToConnectMessage = false;

string logonCode = "";
string logonCallsign = "EGKK";

HttpHelper * httpHelper = NULL;

bool BLINK = false;

bool PlaySoundClr = false;

struct DatalinkPacket {
	string callsign;
	string destination;
	string sid;
	string rwy;
	string freq;
	string ctot;
	string asat;
	string squawk;
	string message;
	string climb;
};

DatalinkPacket DatalinkToSend;

string baseUrlDatalink = "http://www.hoppie.nl/acars/system/connect.html";

struct AcarsMessage {
	string from;
	string type;
	string message;
};

vector<string> AircraftDemandingClearance;
vector<string> AircraftMessageSent;
vector<string> AircraftMessage;
vector<string> AircraftWilco;
vector<string> AircraftStandby;
map<string, AcarsMessage> PendingMessages;
std::set<std::string> AircraftDatalinkClearedCallsigns;
std::map<std::string, std::time_t> AircraftCdmReminderSentUtc;
std::map<std::string, std::time_t> AircraftCdmAutoTrackedUtc;
struct QueuedCdmReminderMessage {
	std::string callsign;
	std::string message;
	int sendAttempts = 0;
};
std::deque<QueuedCdmReminderMessage> CdmReminderMessageQueue;
std::atomic<bool> CdmAutoModeEnabled(false);
std::atomic<int> CdmAutoDelayMinutes(5);
std::atomic<int> CdmReminderCooldownMinutes(60);
std::mutex CdmAutoStateMutex;

string tmessage;
string tdest;
string ttype;

int messageId = 0;

clock_t timer;

string myfrequency;

map<string, string> vStrips_Stands;

bool startThreadvStrips = true;

using namespace SMRPluginSharedData;
char recv_buf[1024];

vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	const std::time_t CdmWarningCooldownSeconds = 60;
	const int CdmReminderQueueMaxSendAttempts = 20;

	enum class CdmQueueReminderOutcome
	{
		Queued = 0,
		AlreadyNotified,
		AlreadyQueued,
		AlreadyCleared,
		Failed
	};

	struct CdmReminderQueueBatchStats
	{
		int queued = 0;
		int alreadyNotified = 0;
		int alreadyQueued = 0;
		int alreadyCleared = 0;
		int failed = 0;
	};

	int ClampNonNegativeMinutes(int minutes)
	{
		return minutes < 0 ? 0 : minutes;
	}

	int LoadNonNegativeAtomicMinutes(const std::atomic<int>& source)
	{
		return ClampNonNegativeMinutes(source.load(std::memory_order_relaxed));
	}

	std::string FormatMinutesLabel(int minutes)
	{
		const int clampedMinutes = ClampNonNegativeMinutes(minutes);
		std::string label = std::to_string(clampedMinutes) + " minute";
		if (clampedMinutes != 1)
			label += "s";
		return label;
	}

	void SetCdmAutoModeState(bool enabled, bool updateDelayMinutes, int delayMinutes)
	{
		if (updateDelayMinutes)
			CdmAutoDelayMinutes.store(ClampNonNegativeMinutes(delayMinutes), std::memory_order_relaxed);

		CdmAutoModeEnabled.store(enabled, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			AircraftCdmAutoTrackedUtc.clear();
		}
	}

	std::string BuildCdmCooldownStatusMessage(const char* action)
	{
		return std::string(action) + " CDM reminder cooldown: " + FormatMinutesLabel(LoadNonNegativeAtomicMinutes(CdmReminderCooldownMinutes));
	}

	std::string BuildCdmAutoStatusMessage(const char* action)
	{
		const bool enabled = CdmAutoModeEnabled.load(std::memory_order_relaxed);
		const int delayMinutes = LoadNonNegativeAtomicMinutes(CdmAutoDelayMinutes);

		std::string message = std::string(action) + " CDM auto mode: ";
		message += enabled ? "enabled" : "disabled";
		message += ", delay=" + FormatMinutesLabel(delayMinutes);
		if (enabled && delayMinutes == 0)
			message += " (immediate)";
		return message;
	}

	std::string BuildCdmQueueSummaryMessage(int checkedCount, const CdmReminderQueueBatchStats& stats)
	{
		std::string summary = "CDM check: ";
		summary += std::to_string(checkedCount) + " checked, ";
		summary += std::to_string(stats.queued) + " queued, ";
		summary += std::to_string(stats.alreadyNotified) + " already notified, ";
		summary += std::to_string(stats.alreadyQueued) + " already queued, ";
		summary += std::to_string(stats.alreadyCleared) + " already cleared, ";
		summary += std::to_string(stats.failed) + " failed.";
		return summary;
	}

	std::string ToUpperAsciiCopy(const std::string& text)
	{
		std::string normalized = text;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return normalized;
	}

	std::string ToLowerAsciiCopy(const std::string& text)
	{
		std::string normalized = text;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return normalized;
	}

	std::string TrimAsciiWhitespaceCopy(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;
		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	}

	std::string ExtractCommandArgument(const std::string& commandLower, const std::string& prefixLower)
	{
		if (commandLower.size() <= prefixLower.size())
			return "";
		return TrimAsciiWhitespaceCopy(commandLower.substr(prefixLower.size()));
	}

	std::string NormalizeCallsign(const std::string& callsign)
	{
		return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	}

	bool StartsWithCommand(const std::string& commandLower, const std::string& prefixLower)
	{
		if (commandLower.rfind(prefixLower, 0) != 0)
			return false;
		if (commandLower.size() == prefixLower.size())
			return true;
		return std::isspace(static_cast<unsigned char>(commandLower[prefixLower.size()])) != 0;
	}

	bool StartsWithTokenCaseInsensitive(const std::string& text, const std::string& token)
	{
		if (text.size() < token.size())
			return false;
		for (size_t i = 0; i < token.size(); ++i)
		{
			const unsigned char lhs = static_cast<unsigned char>(text[i]);
			const unsigned char rhs = static_cast<unsigned char>(token[i]);
			if (std::tolower(lhs) != std::tolower(rhs))
				return false;
		}
		if (text.size() == token.size())
			return true;
		return std::isspace(static_cast<unsigned char>(text[token.size()])) != 0;
	}

	bool TryParseNonNegativeInt(const std::string& text, int& outValue)
	{
		outValue = 0;
		const std::string trimmed = TrimAsciiWhitespaceCopy(text);
		if (trimmed.empty())
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(trimmed.c_str(), &end, 10);
		if (end == trimmed.c_str() || parsed < 0 || parsed > 24 * 60)
			return false;

		const std::string trailing = TrimAsciiWhitespaceCopy(end != nullptr ? std::string(end) : std::string());
		if (!trailing.empty())
			return false;

		outValue = static_cast<int>(parsed);
		return true;
	}

	bool IsNoStatusGroundState(const char* rawGroundState)
	{
		const std::string raw = rawGroundState != nullptr ? rawGroundState : "";
		const std::string trimmed = TrimAsciiWhitespaceCopy(raw);
		if (trimmed.empty())
			return true;

		std::string normalized;
		normalized.reserve(trimmed.size());
		for (char c : trimmed)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}

		return normalized.empty() || normalized == "NSTS" || normalized == "NOSTATUS";
	}

	bool IsGroundTargetForCdm(const CFlightPlan& fp)
	{
		CRadarTarget correlatedTarget = fp.GetCorrelatedRadarTarget();
		if (!correlatedTarget.IsValid())
			return false;
		return correlatedTarget.GetGS() <= 60;
	}

	std::string ResolveActiveAirportFilterUpper()
	{
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr)
				continue;
			std::string airport = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rd->getActiveAirport()));
			if (airport.size() > 4)
				airport = airport.substr(0, 4);
			return airport;
		}
		return "";
	}

	std::vector<std::string> CollectCdmReminderCandidatesForActiveAirport(EuroScopePlugIn::CPlugIn* plugIn)
	{
		std::vector<std::string> candidateCallsigns;
		if (plugIn == nullptr)
			return candidateCallsigns;

		const std::string activeAirportFilter = ResolveActiveAirportFilterUpper();
		candidateCallsigns.reserve(128);

		for (CFlightPlan fp = plugIn->FlightPlanSelectFirst(); fp.IsValid(); fp = plugIn->FlightPlanSelectNext(fp))
		{
			const char* fpCallsignRaw = fp.GetCallsign();
			if (fpCallsignRaw == nullptr || fpCallsignRaw[0] == '\0')
				continue;

			const char* originRaw = fp.GetFlightPlanData().GetOrigin();
			const std::string origin = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(originRaw != nullptr ? originRaw : ""));
			if (!activeAirportFilter.empty() && origin != activeAirportFilter)
				continue;
			if (!IsGroundTargetForCdm(fp))
				continue;
			if (!IsNoStatusGroundState(fp.GetGroundState()))
				continue;

			const std::string callsign = NormalizeCallsign(fpCallsignRaw);
			if (callsign.empty())
				continue;
			if (std::find(candidateCallsigns.begin(), candidateCallsigns.end(), callsign) == candidateCallsigns.end())
				candidateCallsigns.push_back(callsign);
		}

		return candidateCallsigns;
	}

	void PruneReminderHistoryUnlocked(std::time_t nowUtc)
	{
		const int cooldownMinutes = LoadNonNegativeAtomicMinutes(CdmReminderCooldownMinutes);
		const std::time_t cooldownSeconds = static_cast<std::time_t>(cooldownMinutes) * 60;

		for (auto it = AircraftCdmReminderSentUtc.begin(); it != AircraftCdmReminderSentUtc.end();)
		{
			if (std::difftime(nowUtc, it->second) >= static_cast<double>(cooldownSeconds))
				it = AircraftCdmReminderSentUtc.erase(it);
			else
				++it;
		}
	}

	bool HasRecentReminderUnlocked(const std::string& callsign, std::time_t nowUtc)
	{
		const auto it = AircraftCdmReminderSentUtc.find(callsign);
		if (it == AircraftCdmReminderSentUtc.end())
			return false;

		const int cooldownMinutes = LoadNonNegativeAtomicMinutes(CdmReminderCooldownMinutes);
		const std::time_t cooldownSeconds = static_cast<std::time_t>(cooldownMinutes) * 60;
		return std::difftime(nowUtc, it->second) < static_cast<double>(cooldownSeconds);
	}

	void MarkReminderSentUnlocked(const std::string& callsign, std::time_t nowUtc)
	{
		AircraftCdmReminderSentUtc[callsign] = nowUtc;
	}

	bool IsCdmReminderQueuedUnlocked(const std::string& callsign)
	{
		return std::any_of(
			CdmReminderMessageQueue.begin(),
			CdmReminderMessageQueue.end(),
			[&](const QueuedCdmReminderMessage& queued)
			{
				return queued.callsign == callsign;
			});
	}

	bool QueueCdmReminderUnlocked(const std::string& callsign, const std::string& message)
	{
		if (callsign.empty() || message.empty() || IsCdmReminderQueuedUnlocked(callsign))
			return false;

		QueuedCdmReminderMessage queued;
		queued.callsign = callsign;
		queued.message = message;
		queued.sendAttempts = 0;
		CdmReminderMessageQueue.push_back(queued);
		return true;
	}

	void RemoveQueuedCdmReminderUnlocked(const std::string& callsign)
	{
		CdmReminderMessageQueue.erase(
			std::remove_if(
				CdmReminderMessageQueue.begin(),
				CdmReminderMessageQueue.end(),
				[&](const QueuedCdmReminderMessage& queued)
				{
					return queued.callsign == callsign;
				}),
			CdmReminderMessageQueue.end());
	}

	bool HasDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		return AircraftDatalinkClearedCallsigns.find(callsign) != AircraftDatalinkClearedCallsigns.end();
	}

	void MarkDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		if (callsign.empty())
			return;
		AircraftDatalinkClearedCallsigns.insert(callsign);
		RemoveQueuedCdmReminderUnlocked(callsign);
	}

	void ClearDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		if (callsign.empty())
			return;
		AircraftDatalinkClearedCallsigns.erase(callsign);
	}

	void ClearCdmTrackingStateForCallsignUnlocked(const std::string& callsign)
	{
		if (callsign.empty())
			return;
		AircraftCdmReminderSentUtc.erase(callsign);
		AircraftCdmAutoTrackedUtc.erase(callsign);
		ClearDatalinkClearanceSentUnlocked(callsign);
		RemoveQueuedCdmReminderUnlocked(callsign);
	}

	CdmQueueReminderOutcome TryQueueCdmReminderForCallsign(
		const std::string& callsign,
		const std::string& reminderMessage,
		std::time_t nowUtc)
	{
		const std::string normalizedCallsign = NormalizeCallsign(callsign);
		const std::string normalizedMessage = TrimAsciiWhitespaceCopy(reminderMessage);
		if (normalizedCallsign.empty() || normalizedMessage.empty() || nowUtc <= 0)
			return CdmQueueReminderOutcome::Failed;

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			if (HasRecentReminderUnlocked(normalizedCallsign, nowUtc))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
			if (!QueueCdmReminderUnlocked(normalizedCallsign, normalizedMessage))
				return CdmQueueReminderOutcome::Failed;
		}

		return CdmQueueReminderOutcome::Queued;
	}

	CdmReminderQueueBatchStats QueueCdmRemindersForCallsigns(
		const std::vector<std::string>& callsigns,
		const std::string& reminderMessage,
		std::time_t nowUtc)
	{
		CdmReminderQueueBatchStats stats;
		for (const std::string& callsign : callsigns)
		{
			const CdmQueueReminderOutcome outcome = TryQueueCdmReminderForCallsign(callsign, reminderMessage, nowUtc);
			switch (outcome)
			{
			case CdmQueueReminderOutcome::Queued:
				++stats.queued;
				break;
			case CdmQueueReminderOutcome::AlreadyNotified:
				++stats.alreadyNotified;
				break;
			case CdmQueueReminderOutcome::AlreadyQueued:
				++stats.alreadyQueued;
				break;
			case CdmQueueReminderOutcome::AlreadyCleared:
				++stats.alreadyCleared;
				break;
			case CdmQueueReminderOutcome::Failed:
			default:
				++stats.failed;
				break;
			}
		}

		return stats;
	}

	bool TryReadCdmReminderMessageFromAlias(std::string& outMessage, std::string& outAliasPath)
	{
		outMessage.clear();
		outAliasPath = Logger::DLL_PATH.empty() ? "alias.txt" : (Logger::DLL_PATH + "\\..\\Alias\\alias.txt");

		std::ifstream input(outAliasPath);
		if (!input.is_open())
			return false;

		std::string line;
		while (std::getline(input, line))
		{
			std::string working = TrimAsciiWhitespaceCopy(line);
			if (working.empty())
				continue;
			if (working[0] == ';' || working[0] == '#')
				continue;
			if (!StartsWithTokenCaseInsensitive(working, ".cdm"))
				continue;

			working = TrimAsciiWhitespaceCopy(working.substr(4));
			if (StartsWithTokenCaseInsensitive(working, ".msg"))
				working = TrimAsciiWhitespaceCopy(working.substr(4));
			if (StartsWithTokenCaseInsensitive(working, "$aircraft"))
				working = TrimAsciiWhitespaceCopy(working.substr(9));
			if (working.empty())
				continue;

			outMessage = working;
			return true;
		}

		return false;
	}

	void NotifyMissingCdmAliasMessage(EuroScopePlugIn::CPlugIn* plugIn, const std::string& aliasPath)
	{
		static std::time_t lastWarningUtc = 0;
		const std::time_t nowUtc = std::time(nullptr);
		if (plugIn == nullptr || nowUtc <= 0)
			return;
		if (lastWarningUtc != 0 && std::difftime(nowUtc, lastWarningUtc) < static_cast<double>(CdmWarningCooldownSeconds))
			return;

		lastWarningUtc = nowUtc;
		const std::string detail = "Missing/invalid .cdm alias in " + aliasPath;
		plugIn->DisplayUserMessage("vSMR", "CDM", detail.c_str(), true, true, false, true, false);
	}

	bool TryLoadCdmReminderMessage(EuroScopePlugIn::CPlugIn* plugIn, std::string& outMessage)
	{
		std::string aliasPath;
		if (TryReadCdmReminderMessageFromAlias(outMessage, aliasPath))
			return true;

		NotifyMissingCdmAliasMessage(plugIn, aliasPath);
		return false;
	}

	bool IsLikelyCommandEditControl(HWND hwnd)
	{
		if (hwnd == nullptr || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd) || !::IsWindowEnabled(hwnd))
			return false;

		char className[64] = {};
		if (::GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0)
			return false;
		const std::string classUpper = ToUpperAsciiCopy(className);
		if (!(classUpper == "EDIT" || classUpper.find("RICHEDIT") != std::string::npos))
			return false;

		const LONG style = ::GetWindowLong(hwnd, GWL_STYLE);
		if ((style & ES_READONLY) != 0 || (style & ES_MULTILINE) != 0)
			return false;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return false;
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;
		if (width < 120 || height < 12)
			return false;

		return true;
	}

	struct MainWindowSearchContext
	{
		DWORD processId = 0;
		HWND bestWindow = nullptr;
		LONG bestArea = 0;
	};

	BOOL CALLBACK EnumMainWindowsForCurrentProcess(HWND hwnd, LPARAM lParam)
	{
		MainWindowSearchContext* context = reinterpret_cast<MainWindowSearchContext*>(lParam);
		if (context == nullptr)
			return TRUE;

		DWORD windowProcessId = 0;
		::GetWindowThreadProcessId(hwnd, &windowProcessId);
		if (windowProcessId != context->processId)
			return TRUE;
		if (!::IsWindowVisible(hwnd))
			return TRUE;
		if (::GetWindow(hwnd, GW_OWNER) != nullptr)
			return TRUE;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return TRUE;
		LONG width = rect.right - rect.left;
		LONG height = rect.bottom - rect.top;
		if (width < 0)
			width = 0;
		if (height < 0)
			height = 0;
		const LONG area = width * height;
		if (area > context->bestArea)
		{
			context->bestArea = area;
			context->bestWindow = hwnd;
		}

		return TRUE;
	}

	struct CommandEditSearchContext
	{
		RECT mainRect = {};
		HWND bestEdit = nullptr;
		int bestScore = -1000000000;
	};

	BOOL CALLBACK EnumCommandEditControls(HWND hwnd, LPARAM lParam)
	{
		CommandEditSearchContext* context = reinterpret_cast<CommandEditSearchContext*>(lParam);
		if (context == nullptr)
			return TRUE;
		if (!IsLikelyCommandEditControl(hwnd))
			return TRUE;

		RECT rect = {};
		if (!::GetWindowRect(hwnd, &rect))
			return TRUE;
		const LONG style = ::GetWindowLong(hwnd, GWL_STYLE);
		const int width = rect.right - rect.left;

		int score = rect.top + (width / 4);
		if ((style & WS_TABSTOP) != 0)
			score += 1000;
		if ((style & ES_AUTOHSCROLL) != 0)
			score += 500;
		if (rect.bottom >= context->mainRect.bottom - 80)
			score += 2000;

		if (score > context->bestScore)
		{
			context->bestScore = score;
			context->bestEdit = hwnd;
		}

		return TRUE;
	}

	HWND FindEuroScopeCommandEditControl()
	{
		HWND focusedWindow = ::GetFocus();
		if (IsLikelyCommandEditControl(focusedWindow))
			return focusedWindow;

		MainWindowSearchContext mainContext;
		mainContext.processId = ::GetCurrentProcessId();
		::EnumWindows(EnumMainWindowsForCurrentProcess, reinterpret_cast<LPARAM>(&mainContext));
		if (mainContext.bestWindow == nullptr)
			return nullptr;

		CommandEditSearchContext editContext;
		::GetWindowRect(mainContext.bestWindow, &editContext.mainRect);
		::EnumChildWindows(mainContext.bestWindow, EnumCommandEditControls, reinterpret_cast<LPARAM>(&editContext));
		return editContext.bestEdit;
	}

	bool ExecuteEuroScopeCommandViaUi(const std::string& command)
	{
		const std::string trimmed = TrimAsciiWhitespaceCopy(command);
		if (trimmed.empty())
			return false;

		HWND editControl = FindEuroScopeCommandEditControl();
		if (editControl == nullptr)
			return false;

		DWORD_PTR messageResult = 0;
		if (::SendMessageTimeoutA(
			editControl,
			WM_SETTEXT,
			0,
			reinterpret_cast<LPARAM>(trimmed.c_str()),
			SMTO_ABORTIFHUNG,
			250,
			&messageResult) == 0)
		{
			return false;
		}

		const bool keyDownPosted = (::PostMessage(editControl, WM_KEYDOWN, VK_RETURN, 0) != 0);
		const bool keyUpPosted = (::PostMessage(editControl, WM_KEYUP, VK_RETURN, 0) != 0);
		return keyDownPosted && keyUpPosted;
	}

	bool SendPrivateChatMessageLikeDotMsg(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign, const std::string& message)
	{
		if (plugIn == nullptr)
			return false;

		const std::string normalizedCallsign = NormalizeCallsign(callsign);
		const std::string normalizedMessage = TrimAsciiWhitespaceCopy(message);
		if (normalizedCallsign.empty() || normalizedMessage.empty())
			return false;

		const std::string command = ".msg " + normalizedCallsign + " " + normalizedMessage;
		if (ExecuteEuroScopeCommandViaUi(command))
			return true;

		static std::time_t lastInjectionWarningUtc = 0;
		const std::time_t nowUtc = std::time(nullptr);
		if (nowUtc > 0 && (lastInjectionWarningUtc == 0 || std::difftime(nowUtc, lastInjectionWarningUtc) >= static_cast<double>(CdmWarningCooldownSeconds)))
		{
			lastInjectionWarningUtc = nowUtc;
			plugIn->DisplayUserMessage("vSMR", "CDM", "Failed to inject .msg command into EuroScope command line.", true, true, false, true, false);
		}
		Logger::info("CDM .msg inject failed callsign=" + normalizedCallsign);
		return false;
	}

	bool IsCallsignEligibleForCdmReminderNow(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign)
	{
		if (plugIn == nullptr || callsign.empty())
			return false;

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			if (HasDatalinkClearanceSentUnlocked(callsign))
				return false;
		}

		const std::vector<std::string> candidates = CollectCdmReminderCandidatesForActiveAirport(plugIn);
		return std::find(candidates.begin(), candidates.end(), callsign) != candidates.end();
	}

	void ProcessAutomaticCdmReminder(CSMRPlugin* plugIn)
	{
		if (plugIn == nullptr || !CdmAutoModeEnabled.load(std::memory_order_relaxed))
			return;

		const std::time_t nowUtc = std::time(nullptr);
		if (nowUtc <= 0)
			return;

		const int delayMinutes = LoadNonNegativeAtomicMinutes(CdmAutoDelayMinutes);

		const std::vector<std::string> connectedCallsigns = CollectCdmReminderCandidatesForActiveAirport(plugIn);
		const std::set<std::string> connectedCallsignSet(connectedCallsigns.begin(), connectedCallsigns.end());
		std::vector<std::string> callsignsToQueue;
		callsignsToQueue.reserve(connectedCallsigns.size());

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			PruneReminderHistoryUnlocked(nowUtc);

			for (auto it = AircraftCdmAutoTrackedUtc.begin(); it != AircraftCdmAutoTrackedUtc.end();)
			{
				if (connectedCallsignSet.find(it->first) == connectedCallsignSet.end())
					it = AircraftCdmAutoTrackedUtc.erase(it);
				else
					++it;
			}

			for (const std::string& callsign : connectedCallsigns)
			{
				if (AircraftCdmAutoTrackedUtc.find(callsign) == AircraftCdmAutoTrackedUtc.end())
					AircraftCdmAutoTrackedUtc[callsign] = nowUtc;
			}

			for (const std::string& callsign : connectedCallsigns)
			{
				if (HasRecentReminderUnlocked(callsign, nowUtc))
					continue;
				if (IsCdmReminderQueuedUnlocked(callsign))
					continue;
				if (HasDatalinkClearanceSentUnlocked(callsign))
					continue;

				auto trackedIt = AircraftCdmAutoTrackedUtc.find(callsign);
				if (trackedIt == AircraftCdmAutoTrackedUtc.end())
					continue;
				const std::time_t dueAtUtc = trackedIt->second + static_cast<std::time_t>(delayMinutes) * 60;
				if (nowUtc >= dueAtUtc)
					callsignsToQueue.push_back(callsign);
			}
		}

		if (callsignsToQueue.empty())
			return;

		std::string reminderMessage;
		if (!TryLoadCdmReminderMessage(plugIn, reminderMessage))
			return;

		const CdmReminderQueueBatchStats queueStats =
			QueueCdmRemindersForCallsigns(callsignsToQueue, reminderMessage, nowUtc);

		if (queueStats.queued > 0)
			Logger::info("CDM auto reminder queued count=" + std::to_string(queueStats.queued) + " delay_min=" + std::to_string(delayMinutes));
	}

	void ProcessQueuedCdmReminderMessages(CSMRPlugin* plugIn)
	{
		if (plugIn == nullptr)
			return;

		const std::time_t nowUtc = std::time(nullptr);
		if (nowUtc <= 0)
			return;

		QueuedCdmReminderMessage queuedReminder;
		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			PruneReminderHistoryUnlocked(nowUtc);
			if (CdmReminderMessageQueue.empty())
				return;

			queuedReminder = CdmReminderMessageQueue.front();
			CdmReminderMessageQueue.pop_front();
		}

		const std::string callsign = NormalizeCallsign(queuedReminder.callsign);
		const std::string message = TrimAsciiWhitespaceCopy(queuedReminder.message);
		if (callsign.empty() || message.empty())
			return;

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			if (HasRecentReminderUnlocked(callsign, nowUtc))
				return;
		}

		if (!IsCallsignEligibleForCdmReminderNow(plugIn, callsign))
			return;

		if (SendPrivateChatMessageLikeDotMsg(plugIn, callsign, message))
		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			MarkReminderSentUnlocked(callsign, nowUtc);
			return;
		}

		queuedReminder.sendAttempts += 1;
		if (queuedReminder.sendAttempts >= CdmReminderQueueMaxSendAttempts)
		{
			Logger::info("CDM queued reminder dropped callsign=" + callsign);
			return;
		}

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			if (!HasRecentReminderUnlocked(callsign, nowUtc) && !IsCdmReminderQueuedUnlocked(callsign))
				CdmReminderMessageQueue.push_back(queuedReminder);
		}
	}
}

void datalinkLogin(void * arg) {
	string raw;
	string url = baseUrlDatalink;
	url += "?logon=";
	url += logonCode;
	url += "&from=";
	url += logonCallsign;
	url += "&to=SERVER&type=PING";
	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		HoppieConnected = true;
		ConnectionMessage = true;
	}
	else {
		FailedToConnectMessage = true;
	}
};

void sendDatalinkMessage(void * arg) {

	string raw;
	string url = baseUrlDatalink;
	url += "?logon=";
	url += logonCode;
	url += "&from=";
	url += logonCallsign;
	url += "&to=";
	url += tdest;
	url += "&type=";
	url += ttype;
	url += "&packet=";
	url += tmessage;

	size_t start_pos = 0;
	while ((start_pos = url.find(" ", start_pos)) != std::string::npos) {
		url.replace(start_pos, string(" ").length(), "%20");
		start_pos += string("%20").length();
	}

	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		if (PendingMessages.find(DatalinkToSend.callsign) != PendingMessages.end())
			PendingMessages.erase(DatalinkToSend.callsign);
		if (std::find(AircraftMessage.begin(), AircraftMessage.end(), DatalinkToSend.callsign.c_str()) != AircraftMessage.end()) {
			AircraftMessage.erase(std::remove(AircraftMessage.begin(), AircraftMessage.end(), DatalinkToSend.callsign.c_str()), AircraftMessage.end());
		}
		AircraftMessageSent.push_back(DatalinkToSend.callsign.c_str());
	}
};

void pollMessages(void * arg) {
	string raw = "";
	string url = baseUrlDatalink;
	url += "?logon=";
	url += logonCode;
	url += "&from=";
	url += logonCallsign;
	url += "&to=SERVER&type=POLL";
	raw.assign(httpHelper->downloadStringFromURL(url));

	if (!startsWith("ok", raw.c_str()) || raw.size() <= 3)
		return;

	raw = raw + " ";
	raw = raw.substr(3, raw.size() - 3);

	string delimiter = "}} ";
	size_t pos = 0;
	std::string token;
	while ((pos = raw.find(delimiter)) != std::string::npos) {
		token = raw.substr(1, pos);

		string parsed;
		stringstream input_stringstream(token);
		struct AcarsMessage message;
		int i = 1;
		while (getline(input_stringstream, parsed, ' '))
		{
			if (i == 1)
				message.from = parsed;
			if (i == 2)
				message.type = parsed;
			if (i > 2)
			{
				message.message.append(" ");
				message.message.append(parsed);
			}

			i++;
		}
		if (message.type.find("telex") != std::string::npos || message.type.find("cpdlc") != std::string::npos) {
			if (message.message.find("REQ") != std::string::npos || message.message.find("CLR") != std::string::npos || message.message.find("PDC") != std::string::npos || message.message.find("PREDEP") != std::string::npos || message.message.find("REQUEST") != std::string::npos) {
				if (message.message.find("LOGON") != std::string::npos) {
					tmessage = "UNABLE";
					ttype = "CPDLC";
					tdest = DatalinkToSend.callsign;
					_beginthread(sendDatalinkMessage, 0, NULL);
				} else {
					if (PlaySoundClr) {
						AFX_MANAGE_STATE(AfxGetStaticModuleState());
						PlaySound(MAKEINTRESOURCE(IDR_WAVE1), AfxGetInstanceHandle(), SND_RESOURCE | SND_ASYNC);
					}
					AircraftDemandingClearance.push_back(message.from);
				}
			}
			else if (message.message.find("WILCO") != std::string::npos || message.message.find("ROGER") != std::string::npos || message.message.find("RGR") != std::string::npos) {
				if (std::find(AircraftMessageSent.begin(), AircraftMessageSent.end(), message.from) != AircraftMessageSent.end()) {
					AircraftWilco.push_back(message.from);
				}
			}
			else if (message.message.length() != 0 ){
				AircraftMessage.push_back(message.from);
			}
			PendingMessages[message.from] = message;
		}

		raw.erase(0, pos + delimiter.length());
	}


};

void sendDatalinkClearance(void * arg) {
	string raw;
	string url = baseUrlDatalink;
	url += "?logon=";
	url += logonCode;
	url += "&from=";
	url += logonCallsign;
	url += "&to=";
	url += DatalinkToSend.callsign;
	url += "&type=CPDLC&packet=/data2/";
	messageId++;
	url += std::to_string(messageId);
	url += "//R/";
	url += "CLR TO @";
	url += DatalinkToSend.destination;
	url += "@ RWY @";
	url += DatalinkToSend.rwy;
	url += "@ DEP @";
	url += DatalinkToSend.sid;
	url += "@ INIT CLB @";
	url += DatalinkToSend.climb;
	url += "@ SQUAWK @";
	url += DatalinkToSend.squawk;
	url += "@ ";
	if (DatalinkToSend.ctot != "no" && DatalinkToSend.ctot.size() > 3) {
		url += "CTOT @";
		url += DatalinkToSend.ctot;
		url += "@ ";
	}
	if (DatalinkToSend.asat != "no" && DatalinkToSend.asat.size() > 3) {
		url += "TSAT @";
		url += DatalinkToSend.asat;
		url += "@ ";
	}
	if (DatalinkToSend.freq != "no" && DatalinkToSend.freq.size() > 5) {
		url += "WHEN RDY CALL FREQ @";
		url += DatalinkToSend.freq;
		url += "@";
	}
	else {
		url += "WHEN RDY CALL @";
		url += myfrequency;
		url += "@";
	}
	url += " IF UNABLE CALL VOICE ";
	if (DatalinkToSend.message != "no" && DatalinkToSend.message.size() > 1)
		url += DatalinkToSend.message;

	size_t start_pos = 0;
	while ((start_pos = url.find(" ", start_pos)) != std::string::npos) {
		url.replace(start_pos, string(" ").length(), "%20");
		start_pos += string("%20").length();
	}

	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		if (std::find(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), DatalinkToSend.callsign.c_str()) != AircraftDemandingClearance.end()) {
			AircraftDemandingClearance.erase(std::remove(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), DatalinkToSend.callsign.c_str()), AircraftDemandingClearance.end());
		}
		if (std::find(AircraftStandby.begin(), AircraftStandby.end(), DatalinkToSend.callsign.c_str()) != AircraftStandby.end()) {
			AircraftStandby.erase(std::remove(AircraftStandby.begin(), AircraftStandby.end(), DatalinkToSend.callsign.c_str()), AircraftStandby.end());
		}
		if (PendingMessages.find(DatalinkToSend.callsign) != PendingMessages.end())
			PendingMessages.erase(DatalinkToSend.callsign);
		AircraftMessageSent.push_back(DatalinkToSend.callsign.c_str());
		{
			const std::string normalizedCallsign = NormalizeCallsign(DatalinkToSend.callsign);
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			MarkDatalinkClearanceSentUnlocked(normalizedCallsign);
		}
	}
};

CSMRPlugin::CSMRPlugin(void) :CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, MY_PLUGIN_NAME, MY_PLUGIN_VERSION, MY_PLUGIN_DEVELOPER, MY_PLUGIN_COPYRIGHT)
{

	Logger::DLL_PATH = "";
	Logger::ENABLED = false;

	//
	// Adding the SMR Display type
	//
	RegisterDisplayType(MY_PLUGIN_VIEW_AVISO, false, true, true, true);

	RegisterTagItemType("Datalink clearance", TAG_ITEM_DATALINK_STS);
	RegisterTagItemFunction("Datalink menu", TAG_FUNC_DATALINK_MENU);

	messageId = rand() % 10000 + 1789;

	timer = clock();

	if (httpHelper == NULL)
		httpHelper = new HttpHelper();

	const char * p_value;

	if ((p_value = GetDataFromSettings("cpdlc_logon")) != NULL)
		logonCallsign = p_value;
	if ((p_value = GetDataFromSettings("cpdlc_password")) != NULL)
		logonCode = p_value;
	if ((p_value = GetDataFromSettings("cpdlc_sound")) != NULL)
		PlaySoundClr = bool(!!atoi(p_value));
	if ((p_value = GetDataFromSettings("cdm_auto_enabled")) != NULL)
		CdmAutoModeEnabled.store(bool(!!atoi(p_value)), std::memory_order_relaxed);
	if ((p_value = GetDataFromSettings("cdm_auto_delay_min")) != NULL)
	{
		int parsedDelayMinutes = 0;
		if (TryParseNonNegativeInt(p_value, parsedDelayMinutes))
			CdmAutoDelayMinutes.store(parsedDelayMinutes, std::memory_order_relaxed);
	}
	if ((p_value = GetDataFromSettings("cdm_cooldown_min")) != NULL)
	{
		int parsedCooldownMinutes = 0;
		if (TryParseNonNegativeInt(p_value, parsedCooldownMinutes))
			CdmReminderCooldownMinutes.store(parsedCooldownMinutes, std::memory_order_relaxed);
	}

	char DllPathFile[_MAX_PATH];
	string DllPath;

	GetModuleFileNameA(HINSTANCE(&__ImageBase), DllPathFile, sizeof(DllPathFile));
	DllPath = DllPathFile;
	DllPath.resize(DllPath.size() - strlen("vSMR.dll"));
	Logger::DLL_PATH = DllPath;
}

CSMRPlugin::~CSMRPlugin()
{
	// NOTE: 'SaveDataToSettings()' doesn't actually write data anywhere in a file, contrary to what the name freaking suggests.
	SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", logonCallsign.c_str());
	SaveDataToSettings("cpdlc_password", "The CPDLC logon password", logonCode.c_str());
	int temp = 0;
	if (PlaySoundClr)
		temp = 1;
	SaveDataToSettings("cpdlc_sound", "Play sound on clearance request", std::to_string(temp).c_str());
	SaveDataToSettings("cdm_auto_enabled", "Enable automatic CDM reminder messaging", CdmAutoModeEnabled.load(std::memory_order_relaxed) ? "1" : "0");
	const int cdmAutoDelayToPersist = LoadNonNegativeAtomicMinutes(CdmAutoDelayMinutes);
	SaveDataToSettings("cdm_auto_delay_min", "CDM auto reminder delay in minutes", std::to_string(cdmAutoDelayToPersist).c_str());
	const int cdmCooldownToPersist = LoadNonNegativeAtomicMinutes(CdmReminderCooldownMinutes);
	SaveDataToSettings("cdm_cooldown_min", "CDM reminder resend cooldown in minutes", std::to_string(cdmCooldownToPersist).c_str());

	try
	{
		io_service.stop();
		//vStripsThread.join();
	}
	catch (std::exception& e)
	{
		std::cerr << e.what() << std::endl;
	}
}

bool CSMRPlugin::OnCompileCommand(const char * sCommandLine) {
	const char* rawCommand = sCommandLine != nullptr ? sCommandLine : "";
	const std::string commandLower = ToLowerAsciiCopy(rawCommand);

	if (startsWith(".smr connect", rawCommand))
	{
		if (ControllerMyself().IsController()) {
			if (!HoppieConnected) {
				_beginthread(datalinkLogin, 0, NULL);
			}
			else {
				HoppieConnected = false;
				DisplayUserMessage("CPDLC", "Server", "Logged off!", true, true, false, true, false);
			}
		}
		else {
			DisplayUserMessage("CPDLC", "Error", "You are not logged in as a controller!", true, true, false, true, false);
		}

		return true;
	}
	else if (startsWith(".smr poll", rawCommand))
	{
		if (HoppieConnected) {
			_beginthread(pollMessages, 0, NULL);
		}
		return true;
	}
	else if (strcmp(rawCommand, ".smr log") == 0) {
		Logger::ENABLED = !Logger::ENABLED;
		return true;
	}
	else if (StartsWithCommand(commandLower, ".smr cdm cooldown"))
	{
		const std::string prefix = ".smr cdm cooldown";
		const std::string argument = ExtractCommandArgument(commandLower, prefix);

		if (argument.empty() || argument == "status")
		{
			const std::string statusMessage = BuildCdmCooldownStatusMessage("Status");
			DisplayUserMessage("vSMR", "CDM", statusMessage.c_str(), true, true, false, true, false);
			return true;
		}

		int parsedCooldownMinutes = 0;
		if (!TryParseNonNegativeInt(argument, parsedCooldownMinutes))
		{
			DisplayUserMessage(
				"vSMR",
				"CDM",
				"Usage: .smr cdm cooldown <minutes>. Example: .smr cdm cooldown 60",
				true,
				true,
				false,
				true,
				false);
			return true;
		}

		CdmReminderCooldownMinutes.store(parsedCooldownMinutes, std::memory_order_relaxed);
		const std::string updatedMessage = BuildCdmCooldownStatusMessage("Updated");
		DisplayUserMessage("vSMR", "CDM", updatedMessage.c_str(), true, true, false, true, false);
		return true;
	}
	else if (StartsWithCommand(commandLower, ".smr cdm auto"))
	{
		const std::string prefix = ".smr cdm auto";
		const std::string argument = ExtractCommandArgument(commandLower, prefix);

		if (argument.empty() || argument == "status")
		{
			const std::string statusMessage = BuildCdmAutoStatusMessage("Status");
			DisplayUserMessage("vSMR", "CDM", statusMessage.c_str(), true, true, false, true, false);
			return true;
		}

		if (argument == "off" || argument == "disable")
		{
			SetCdmAutoModeState(false, false, 0);
			const std::string updatedMessage = BuildCdmAutoStatusMessage("Updated");
			DisplayUserMessage("vSMR", "CDM", updatedMessage.c_str(), true, true, false, true, false);
			return true;
		}

		if (argument == "on" || argument == "enable")
		{
			SetCdmAutoModeState(true, false, 0);
			const std::string updatedMessage = BuildCdmAutoStatusMessage("Updated");
			DisplayUserMessage("vSMR", "CDM", updatedMessage.c_str(), true, true, false, true, false);
			return true;
		}

		int parsedDelayMinutes = 0;
		if (!TryParseNonNegativeInt(argument, parsedDelayMinutes))
		{
			DisplayUserMessage(
				"vSMR",
				"CDM",
				"Usage: .smr cdm auto <minutes|on|off>. Example: .smr cdm auto 5",
				true,
				true,
				false,
				true,
				false);
			return true;
		}

		SetCdmAutoModeState(true, true, parsedDelayMinutes);
		const std::string updatedMessage = BuildCdmAutoStatusMessage("Updated");
		DisplayUserMessage("vSMR", "CDM", updatedMessage.c_str(), true, true, false, true, false);
		return true;
	}
	else if (TrimAsciiWhitespaceCopy(commandLower) == ".smr cdm")
	{
		const std::time_t nowUtc = std::time(nullptr);
		if (nowUtc <= 0)
		{
			DisplayUserMessage("vSMR", "CDM", "Unable to evaluate current UTC time for CDM command.", true, true, false, true, false);
			return true;
		}

		std::string reminderMessage;
		if (!TryLoadCdmReminderMessage(this, reminderMessage))
			return true;

		const std::vector<std::string> candidateCallsigns = CollectCdmReminderCandidatesForActiveAirport(this);

		{
			std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
			PruneReminderHistoryUnlocked(nowUtc);
		}

		const CdmReminderQueueBatchStats queueStats =
			QueueCdmRemindersForCallsigns(candidateCallsigns, reminderMessage, nowUtc);
		const int checkedCount = static_cast<int>(candidateCallsigns.size());
		const std::string summary = BuildCdmQueueSummaryMessage(checkedCount, queueStats);
		DisplayUserMessage("vSMR", "CDM", summary.c_str(), true, true, false, true, false);
		return true;
	}
	else if (startsWith(".smr", rawCommand))
	{
		CCPDLCSettingsDialog dia;
		dia.m_Logon = logonCallsign.c_str();
		dia.m_Password = logonCode.c_str();
		dia.m_Sound = int(PlaySoundClr);

		if (dia.DoModal() != IDOK)
			return true;

		logonCallsign = dia.m_Logon;
		logonCode = dia.m_Password;
		PlaySoundClr = bool(!!dia.m_Sound);
		SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", logonCallsign.c_str());
		SaveDataToSettings("cpdlc_password", "The CPDLC logon password", logonCode.c_str());
		int temp = 0;
		if (PlaySoundClr)
			temp = 1;
		SaveDataToSettings("cpdlc_sound", "Play sound on clearance request", std::to_string(temp).c_str());

		return true;
	}
	return false;
}

void CSMRPlugin::OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int * pColorCode, COLORREF * pRGB, double * pFontSize) {
	Logger::info(string(__FUNCSIG__));
	if (ItemCode == TAG_ITEM_DATALINK_STS) {
		if (FlightPlan.IsValid()) {
			if (std::find(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), FlightPlan.GetCallsign()) != AircraftDemandingClearance.end()) {
				*pColorCode = TAG_COLOR_RGB_DEFINED;
				if (BLINK)
					*pRGB = RGB(130, 130, 130);
				else
					*pRGB = RGB(255, 255, 0);

				if (std::find(AircraftStandby.begin(), AircraftStandby.end(), FlightPlan.GetCallsign()) != AircraftStandby.end())
					strcpy_s(sItemString, 16, "S");
				else
					strcpy_s(sItemString, 16, "R");
			}
			else if (std::find(AircraftMessage.begin(), AircraftMessage.end(), FlightPlan.GetCallsign()) != AircraftMessage.end()) {
				*pColorCode = TAG_COLOR_RGB_DEFINED;
				if (BLINK)
					*pRGB = RGB(130, 130, 130);
				else
					*pRGB = RGB(255, 255, 0);
				strcpy_s(sItemString, 16, "T");
			}
			else if (std::find(AircraftWilco.begin(), AircraftWilco.end(), FlightPlan.GetCallsign()) != AircraftWilco.end()) {
				*pColorCode = TAG_COLOR_RGB_DEFINED;
				*pRGB = RGB(0, 176, 0);
				strcpy_s(sItemString, 16, "V");
			}
			else if (std::find(AircraftMessageSent.begin(), AircraftMessageSent.end(), FlightPlan.GetCallsign()) != AircraftMessageSent.end()) {
				*pColorCode = TAG_COLOR_RGB_DEFINED;
				*pRGB = RGB(255, 255, 0);
				strcpy_s(sItemString, 16, "V");
			}
			else {
				*pColorCode = TAG_COLOR_RGB_DEFINED;
				*pRGB = RGB(130, 130, 130);

				strcpy_s(sItemString, 16, "-");
			}
		}
	}
}

void CSMRPlugin::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area)
{
	Logger::info(string(__FUNCSIG__));
	if (FunctionId == TAG_FUNC_DATALINK_MENU) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		bool menu_is_datalink = true;

		if (FlightPlan.IsValid()) {
			if (std::find(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), FlightPlan.GetCallsign()) != AircraftDemandingClearance.end()) {
				menu_is_datalink = false;
			}
		}

		OpenPopupList(Area, "Datalink menu", 1);
		AddPopupListElement("Confirm", "", TAG_FUNC_DATALINK_CONFIRM, false, 2, menu_is_datalink);
		AddPopupListElement("Message", "", TAG_FUNC_DATALINK_MESSAGE, false, 2, false, true);
		AddPopupListElement("Standby", "", TAG_FUNC_DATALINK_STBY, false, 2, menu_is_datalink);
		AddPopupListElement("Voice", "", TAG_FUNC_DATALINK_VOICE, false, 2, menu_is_datalink);
		AddPopupListElement("Reset", "", TAG_FUNC_DATALINK_RESET, false, 2, false, true);
		AddPopupListElement("Close", "", EuroScopePlugIn::TAG_ITEM_FUNCTION_NO, false, 2, false, true);
	}

	if (FunctionId == TAG_FUNC_DATALINK_RESET) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* resetCallsignRaw = FlightPlan.GetCallsign();
			const std::string normalizedCallsign = NormalizeCallsign(resetCallsignRaw != nullptr ? resetCallsignRaw : "");
			if (std::find(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), FlightPlan.GetCallsign()) != AircraftDemandingClearance.end()) {
				AircraftDemandingClearance.erase(std::remove(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), FlightPlan.GetCallsign()), AircraftDemandingClearance.end());
			}
			if (std::find(AircraftStandby.begin(), AircraftStandby.end(), FlightPlan.GetCallsign()) != AircraftStandby.end()) {
				AircraftStandby.erase(std::remove(AircraftStandby.begin(), AircraftStandby.end(), FlightPlan.GetCallsign()), AircraftStandby.end());
			}
			if (std::find(AircraftMessageSent.begin(), AircraftMessageSent.end(), FlightPlan.GetCallsign()) != AircraftMessageSent.end()) {
				AircraftMessageSent.erase(std::remove(AircraftMessageSent.begin(), AircraftMessageSent.end(), FlightPlan.GetCallsign()), AircraftMessageSent.end());
			}
			if (std::find(AircraftWilco.begin(), AircraftWilco.end(), FlightPlan.GetCallsign()) != AircraftWilco.end()) {
				AircraftWilco.erase(std::remove(AircraftWilco.begin(), AircraftWilco.end(), FlightPlan.GetCallsign()), AircraftWilco.end());
			}
			if (std::find(AircraftMessage.begin(), AircraftMessage.end(), FlightPlan.GetCallsign()) != AircraftMessage.end()) {
				AircraftMessage.erase(std::remove(AircraftMessage.begin(), AircraftMessage.end(), FlightPlan.GetCallsign()), AircraftMessage.end());
			}
			if (PendingMessages.find(FlightPlan.GetCallsign()) != PendingMessages.end()) {
				PendingMessages.erase(FlightPlan.GetCallsign());
			}
			{
				std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
				ClearCdmTrackingStateForCallsignUnlocked(normalizedCallsign);
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_STBY) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			AircraftStandby.push_back(FlightPlan.GetCallsign());
			tmessage = "STANDBY";
			ttype = "CPDLC";
			tdest = FlightPlan.GetCallsign();
			_beginthread(sendDatalinkMessage, 0, NULL);
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_MESSAGE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			AFX_MANAGE_STATE(AfxGetStaticModuleState());

			CDataLinkDialog dia;
			dia.m_Callsign = FlightPlan.GetCallsign();
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();

			AcarsMessage msg = PendingMessages[FlightPlan.GetCallsign()];
			dia.m_Req = msg.message.c_str();

			string toReturn = "";

			if (dia.DoModal() != IDOK)
				return;

			tmessage = dia.m_Message;
			ttype = "TELEX";
			tdest = FlightPlan.GetCallsign();
			_beginthread(sendDatalinkMessage, 0, NULL);
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_VOICE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			tmessage = "UNABLE CALL ON FREQ";
			ttype = "CPDLC";
			tdest = FlightPlan.GetCallsign();

			if (std::find(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), DatalinkToSend.callsign.c_str()) != AircraftDemandingClearance.end()) {
				AircraftDemandingClearance.erase(std::remove(AircraftDemandingClearance.begin(), AircraftDemandingClearance.end(), FlightPlan.GetCallsign()), AircraftDemandingClearance.end());
			}
			if (std::find(AircraftStandby.begin(), AircraftStandby.end(), DatalinkToSend.callsign.c_str()) != AircraftStandby.end()) {
				AircraftStandby.erase(std::remove(AircraftStandby.begin(), AircraftStandby.end(), FlightPlan.GetCallsign()), AircraftDemandingClearance.end());
			}
			PendingMessages.erase(DatalinkToSend.callsign);

			_beginthread(sendDatalinkMessage, 0, NULL);
		}

	}

	if (FunctionId == TAG_FUNC_DATALINK_CONFIRM) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {

			AFX_MANAGE_STATE(AfxGetStaticModuleState());

			CDataLinkDialog dia;
			dia.m_Callsign = FlightPlan.GetCallsign();
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();
			dia.m_Departure = FlightPlan.GetFlightPlanData().GetSidName();
			dia.m_Rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			dia.m_SSR = FlightPlan.GetControllerAssignedData().GetSquawk();
			string freq = std::to_string(ControllerMyself().GetPrimaryFrequency());
			if (ControllerSelect(FlightPlan.GetCoordinatedNextController()).GetPrimaryFrequency() != 0)
				string freq = std::to_string(ControllerSelect(FlightPlan.GetCoordinatedNextController()).GetPrimaryFrequency());
			freq = freq.substr(0, 7);
			dia.m_Freq = freq.c_str();
			AcarsMessage msg = PendingMessages[FlightPlan.GetCallsign()];
			dia.m_Req = msg.message.c_str();

			string toReturn = "";

			int ClearedAltitude = FlightPlan.GetControllerAssignedData().GetClearedAltitude();
			int Ta = GetTransitionAltitude();

			if (ClearedAltitude != 0) {
				if (ClearedAltitude > Ta && ClearedAltitude > 2) {
					string str = std::to_string(ClearedAltitude);
					for (size_t i = 0; i < 5 - str.length(); i++)
						str = "0" + str;
					if (str.size() > 3)
						str.erase(str.begin() + 3, str.end());
					toReturn = "FL";
					toReturn += str;
				}
				else if (ClearedAltitude <= Ta && ClearedAltitude > 2) {


					toReturn = std::to_string(ClearedAltitude);
					toReturn += "ft";
				}
			}
			dia.m_Climb = toReturn.c_str();

			if (dia.DoModal() != IDOK)
				return;

			DatalinkToSend.callsign = FlightPlan.GetCallsign();
			DatalinkToSend.destination = FlightPlan.GetFlightPlanData().GetDestination();
			DatalinkToSend.rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			DatalinkToSend.sid = FlightPlan.GetFlightPlanData().GetSidName();
			DatalinkToSend.asat = dia.m_TSAT;
			DatalinkToSend.ctot = dia.m_CTOT;
			DatalinkToSend.freq = dia.m_Freq;
			DatalinkToSend.message = dia.m_Message;
			DatalinkToSend.squawk = FlightPlan.GetControllerAssignedData().GetSquawk();
			DatalinkToSend.climb = toReturn;

			myfrequency = std::to_string(ControllerMyself().GetPrimaryFrequency()).substr(0, 7);

			_beginthread(sendDatalinkClearance, 0, NULL);

		}

	}
}

void CSMRPlugin::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	Logger::info(string(__FUNCSIG__));
	const char* callsignRaw = FlightPlan.GetCallsign();
	const std::string callsign = NormalizeCallsign(callsignRaw != nullptr ? callsignRaw : "");
	{
		std::lock_guard<std::mutex> guard(CdmAutoStateMutex);
		ClearCdmTrackingStateForCallsignUnlocked(callsign);
	}

	CRadarTarget rt;
	if (!callsign.empty())
		rt = RadarTargetSelect(callsign.c_str());
	else if (callsignRaw != nullptr)
		rt = RadarTargetSelect(callsignRaw);

	if (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()) != ReleasedTracks.end())
		ReleasedTracks.erase(std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()));

	if (std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()) != ManuallyCorrelated.end())
		ManuallyCorrelated.erase(std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()));
}

void CSMRPlugin::OnTimer(int Counter)
{
	Logger::info(string(__FUNCSIG__));
	BLINK = !BLINK;

	if (HoppieConnected && ConnectionMessage) {
		DisplayUserMessage("CPDLC", "Server", "Logged in!", true, true, false, true, false);
		ConnectionMessage = false;
	}

	if (FailedToConnectMessage) {
		DisplayUserMessage("CPDLC", "Server", "Could not login! Callsign probably in use.", true, true, false, true, false);
		FailedToConnectMessage = false;
	}

	if (HoppieConnected && GetConnectionType() == CONNECTION_TYPE_NO) {
		DisplayUserMessage("CPDLC", "Server", "Automatically logged off!", true, true, false, true, false);
		HoppieConnected = false;
	}

	if (((clock() - timer) / CLOCKS_PER_SEC) > 10 && HoppieConnected) {
		_beginthread(pollMessages, 0, NULL);
		timer = clock();
	}

	for (auto &ac : AircraftWilco)
	{
		CRadarTarget RadarTarget = RadarTargetSelect(ac.c_str());

		if (RadarTarget.IsValid()) {
			if (RadarTarget.GetGS() > 160) {
				AircraftWilco.erase(std::remove(AircraftWilco.begin(), AircraftWilco.end(), ac), AircraftWilco.end());
			}
		}
	}

	ProcessAutomaticCdmReminder(this);
	ProcessQueuedCdmReminderMessages(this);
};

CRadarScreen * CSMRPlugin::OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated)
{
	Logger::info(string(__FUNCSIG__));
	if (!strcmp(sDisplayName, MY_PLUGIN_VIEW_AVISO)) {
		CSMRRadar* rd = new CSMRRadar();
		RadarScreensOpened.push_back(rd);
		return rd;
	}

	return NULL;
}

//---EuroScopePlugInExit-----------------------------------------------

void __declspec (dllexport) EuroScopePlugInExit(void)
{
	for each (auto var in RadarScreensOpened)
	{
		var->EuroScopePlugInExitCustom();
	}
}
