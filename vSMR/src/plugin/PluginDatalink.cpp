#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"
#include "plugin/PluginDatalink.Internal.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashReporter.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "shared/TextUtils.hpp"
#include "weather/WeatherStore.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>


// PDC connection state shared between timer and worker threads.
std::atomic<bool> DatalinkConnected(false);
std::atomic<bool> DatalinkConnecting(false);
std::atomic<bool> DatalinkPollInProgress(false);
std::atomic<unsigned long long> DatalinkConnectionGeneration(0);
std::atomic<unsigned long long> DatalinkPollGeneration(0);
std::atomic<bool> DatalinkYieldedToDman(false);
std::atomic<unsigned long long> DatalinkStateRevision(0);
std::atomic<bool> ConnectionMessage(false);
std::atomic<bool> FailedToConnectMessage(false);

// The PINEDE key, and the EuroScope callsign PINEDE knows this station by
std::string logonCode = "";
std::string logonCallsign;
std::string DatalinkBaseUrl = VsmrPinede::kDefaultBaseUrl;
std::string DatalinkStationFrequency;

std::string DatalinkStatusMessage = "Disconnected.";
std::mutex DatalinkControlMutex;

std::vector<std::string> AircraftDemandingClearance;
std::vector<std::string> AircraftMessageSent;
std::vector<std::string> AircraftMessage;
std::vector<std::string> AircraftWilco;
std::vector<std::string> AircraftStandby;
std::set<std::string> AircraftDatalinkClearedCallsigns;
std::set<std::string> AircraftDatalinkClearanceInFlightCallsigns;
std::set<std::string> AircraftWilcoDismissed;

std::map<std::string, AcarsMessage> PendingMessages;
// Guards all mutable CPDLC message state used by worker threads.
std::mutex DatalinkStateMutex;

PluginSteadyClock::time_point DatalinkLastPollAt;

std::mutex ProfilesSourceMutex;
std::string ActiveProfilesConfigPath;
bool ActiveProfilesConfigPathClaimed = false;

	const size_t DatalinkResponseLimitBytes = 1024U * 1024U;

	namespace
	{
		std::mutex DatalinkNoticesMutex;
		std::deque<std::string> DatalinkNotices;
	}

	void QueueDatalinkNotice(const std::string& message)
	{
		std::lock_guard<std::mutex> guard(DatalinkNoticesMutex);
		// a burst of failures must not flood the controller's message window
		if (DatalinkNotices.size() < 20)
			DatalinkNotices.push_back(message);
	}

	std::vector<std::string> TakeDatalinkNotices()
	{
		std::lock_guard<std::mutex> guard(DatalinkNoticesMutex);
		std::vector<std::string> notices(DatalinkNotices.begin(), DatalinkNotices.end());
		DatalinkNotices.clear();
		return notices;
	}

	DatalinkCredentialsSnapshot SnapshotDatalinkCredentials()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		DatalinkCredentialsSnapshot snapshot;
		snapshot.callsign = logonCallsign;
		snapshot.key = logonCode;
		snapshot.baseUrl = DatalinkBaseUrl;
		snapshot.frequency = DatalinkStationFrequency;
		return snapshot;
	}

	void SetDatalinkStatusMessage(const std::string& message)
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		DatalinkStatusMessage = message;
	}

	std::string GetDatalinkStatusMessageCopy()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		return DatalinkStatusMessage;
	}

	bool StartDatalinkPoll(bool reportStatus, std::string& error);





	std::string ResolveActiveAirportFilterUpper()
	{
		std::string resolvedAirport;
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr || rd->IsShutdownRequested())
				continue;
			std::string airport = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rd->getActiveAirport()));
			if (airport.size() > 4)
				airport = airport.substr(0, 4);
			if (airport.empty())
				continue;
			if (airport.size() != 4)
				return "";
			if (resolvedAirport.empty())
				resolvedAirport = airport;
			else if (resolvedAirport != airport)
				return "";
		}

		return resolvedAirport;
	}

	bool IsCdmBridgeReady()
	{
		return VsmrCdm::GetInterfaceState().providerReady;
	}

	std::string NormalizeCallsignForState(const std::string& callsign)
	{
		return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	}

	bool HasDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return false;
		return AircraftDatalinkClearedCallsigns.find(normalizedCallsign) != AircraftDatalinkClearedCallsigns.end();
	}

	bool HasDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		return !normalizedCallsign.empty() &&
			AircraftDatalinkClearanceInFlightCallsigns.find(normalizedCallsign) !=
			AircraftDatalinkClearanceInFlightCallsigns.end();
	}

	void MarkDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;
		AircraftDatalinkClearanceInFlightCallsigns.insert(normalizedCallsign);
	}

	void ClearDatalinkClearanceInFlightUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (!normalizedCallsign.empty())
			AircraftDatalinkClearanceInFlightCallsigns.erase(normalizedCallsign);
	}

	void MarkDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.insert(normalizedCallsign);
		AircraftDatalinkClearanceInFlightCallsigns.erase(normalizedCallsign);
	}

	void ClearDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.erase(normalizedCallsign);
	}

	void ResetDatalinkClearanceState()
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftDatalinkClearedCallsigns.clear();
		AircraftDatalinkClearanceInFlightCallsigns.clear();
		AircraftWilcoDismissed.clear();
	}

	bool ContainsCallsignUnlocked(const std::vector<std::string>& collection, const std::string& callsign)
	{
		return std::find(collection.begin(), collection.end(), callsign) != collection.end();
	}

	void AddCallsignUniqueUnlocked(std::vector<std::string>& collection, const std::string& callsign)
	{
		if (!ContainsCallsignUnlocked(collection, callsign))
			collection.push_back(callsign);
	}

	void RemoveCallsignUnlocked(std::vector<std::string>& collection, const std::string& callsign)
	{
		collection.erase(std::remove(collection.begin(), collection.end(), callsign), collection.end());
	}

	std::filesystem::path ResolveDefaultProfilesConfigPath()
	{
		const std::filesystem::path pluginDirectory =
			std::filesystem::u8path(Logger::DLL_PATH);
		const std::filesystem::path dataConfigPath = pluginDirectory / "vSMR_Data" / "vSMR_Profiles.json";

		std::error_code ec;
		const auto defaultsPath = pluginDirectory / "vSMR_Data" / "default.json";
		if (std::filesystem::is_regular_file(defaultsPath, ec) && !ec)
		{
			const auto userPath = pluginDirectory / "vSMR_Data" / "config.json";
			return std::filesystem::is_regular_file(userPath, ec) && !ec ? userPath : defaultsPath;
		}
		ec.clear();
		if (std::filesystem::exists(dataConfigPath, ec))
			return dataConfigPath;
		return pluginDirectory / "vSMR_Profiles.json";
	}

	std::string FormatUtcHhmm(std::time_t utcTime)
	{
		if (utcTime <= 0)
			return "";
		std::tm utc = {};
		if (::gmtime_s(&utc, &utcTime) != 0)
			return "";
		char value[5] = {};
		if (std::strftime(value, sizeof(value), "%H%M", &utc) != 4)
			return "";
		return value;
	}

	std::string StripEnclosingQuotesCopy(const std::string& text)
	{
		if (text.size() >= 2)
		{
			const char first = text.front();
			const char last = text.back();
			if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
				return text.substr(1, text.size() - 2);
		}
		return text;
	}

	std::filesystem::path NormalizeAliasPathForLookup(const std::filesystem::path& path)
	{
		if (path.empty())
			return path;

		std::error_code ec;
		const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
		if (!ec && !absolute.empty())
			return absolute.lexically_normal();

		return path.lexically_normal();
	}

	void AppendAliasPathCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& path)
	{
		if (path.empty())
			return;

		const std::filesystem::path normalizedPath = NormalizeAliasPathForLookup(path);
		if (std::find(candidates.begin(), candidates.end(), normalizedPath) != candidates.end())
			return;

		candidates.push_back(normalizedPath);
	}

	std::filesystem::path ResolveAliasFilePath(EuroScopePlugIn::CPlugIn* plugIn)
	{
		std::vector<std::filesystem::path> candidates;

		std::filesystem::path processDirectory;
		std::wstring modulePath(32768, L'\0');
		const DWORD modulePathLength = ::GetModuleFileNameW(
			nullptr,
			modulePath.data(),
			static_cast<DWORD>(modulePath.size()));
		if (modulePathLength > 0 && modulePathLength < modulePath.size())
		{
			modulePath.resize(modulePathLength);
			processDirectory = std::filesystem::path(modulePath).parent_path();
		}

		if (plugIn != nullptr)
		{
			const char* configuredAliasPathRaw = plugIn->GetDataFromSettings("alias");
			if (configuredAliasPathRaw != nullptr)
			{
				std::string configuredAliasPath = TrimAsciiWhitespaceCopy(configuredAliasPathRaw);
				configuredAliasPath = StripEnclosingQuotesCopy(configuredAliasPath);
				configuredAliasPath = TrimAsciiWhitespaceCopy(configuredAliasPath);
				if (!configuredAliasPath.empty())
				{
					const std::filesystem::path configuredPath(configuredAliasPath);
					AppendAliasPathCandidate(candidates, configuredPath);
					if (!configuredPath.is_absolute() && !processDirectory.empty())
						AppendAliasPathCandidate(candidates, processDirectory / configuredPath);
				}
			}
		}

		if (!processDirectory.empty())
			AppendAliasPathCandidate(candidates, processDirectory / "Alias" / "alias.txt");

		if (!Logger::DLL_PATH.empty())
		{
			const std::filesystem::path pluginDir =
				std::filesystem::u8path(Logger::DLL_PATH);
			AppendAliasPathCandidate(candidates, pluginDir / ".." / ".." / "Alias" / "alias.txt");
			AppendAliasPathCandidate(candidates, pluginDir / ".." / "Alias" / "alias.txt");
		}

		AppendAliasPathCandidate(candidates, std::filesystem::path("alias.txt"));

		std::error_code ec;
		for (const std::filesystem::path& candidate : candidates)
		{
			if (std::filesystem::exists(candidate, ec))
				return candidate;
			ec.clear();
		}

		if (!candidates.empty())
			return candidates.front();

		return std::filesystem::path("alias.txt");
	}
