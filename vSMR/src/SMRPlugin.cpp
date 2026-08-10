#include "stdafx.h"
#include "SMRPlugin.hpp"
#include "InsetWindow.h"
#include <atomic>
#include <mutex>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <deque>
#include <set>
#include <memory>
#include <new>
#include <limits>
#include <wincrypt.h>
#include "rapidjson/document.h"
#include "WeatherData.hpp"
#include "RdfOverlay.hpp"
#include "SMRGroundState.hpp"

#pragma comment(lib, "crypt32.lib")

std::atomic<bool> Logger::ENABLED{ false };
string Logger::DLL_PATH;
std::atomic<Logger::Mode> Logger::CURRENT_MODE{ Logger::Mode::Normal };

// CPDLC/Hoppie connection state shared between timer and worker threads.
std::atomic<bool> HoppieConnected(false);
std::atomic<bool> HoppieConnecting(false);
std::atomic<bool> HoppiePollInProgress(false);
std::atomic<unsigned long long> HoppieConnectionGeneration(0);
std::atomic<unsigned long long> HoppiePollGeneration(0);
std::atomic<bool> ConnectionMessage(false);
std::atomic<bool> FailedToConnectMessage(false);
std::atomic<bool> PluginShutdownRequested(false);
std::atomic<CSMRPlugin*> ActivePluginInstance{ nullptr };

string logonCode = "";
string logonCallsign = "EGKK";

bool BLINK = false;

bool PlaySoundClr = false;
std::string DatalinkStatusMessage = "Disconnected.";
std::mutex DatalinkControlMutex;

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

const string baseUrlDatalink = "https://www.hoppie.nl/acars/system/connect.html";

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
std::set<std::string> AircraftDatalinkClearedCallsigns;
map<string, std::time_t> AircraftCdmTobtReminderSentUtc;

struct QueuedCdmReminderMessage {
	string callsign;
	string message;
	int sendAttempts = 0;
	bool automatic = false;
};

std::deque<QueuedCdmReminderMessage> CdmReminderMessageQueue;

std::atomic<bool> CdmAutoModeEnabled(false);
std::atomic<int> CdmAutoDelayMinutes(5);

enum class CdmAutoEligibility
{
	Pending = 0,
	SuppressValidAtConnect,
	NotifyAfterDelay,
	NotifyImmediateExpiredAtConnect
};

struct CdmAutoTrackedAircraftState
{
	std::time_t connectedAtUtc = 0;
	std::time_t dueAtUtc = 0;
	CdmAutoEligibility eligibility = CdmAutoEligibility::Pending;
};

map<string, CdmAutoTrackedAircraftState> AircraftCdmAutoTracked;
map<string, AcarsMessage> PendingMessages;
// Guards all mutable CPDLC message state used by worker threads.
std::mutex DatalinkStateMutex;
std::atomic<int> CdmReminderCooldownMinutes(60);

std::atomic<int> messageId(0);

clock_t timer;

map<string, string> vStrips_Stands;

bool startThreadvStrips = true;

char recv_buf[1024];

vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	std::mutex LineupOverrideMutex;
	std::map<std::string, std::chrono::steady_clock::time_point> LineupOverrides;

	std::string NormalizeLineupCallsign(const char* callsign)
	{
		std::string normalized = callsign != nullptr ? callsign : "";
		normalized.erase(
			std::remove_if(normalized.begin(), normalized.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
			normalized.end());
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return normalized;
	}
}

bool VsmrGroundState::SetLineupOverride(const char* callsign)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return false;
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides[normalized] = std::chrono::steady_clock::now();
	return true;
}

void VsmrGroundState::ClearLineupOverride(const char* callsign)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return;
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides.erase(normalized);
}

void VsmrGroundState::ClearAllLineupOverrides()
{
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides.clear();
}

bool VsmrGroundState::IsLineupOverrideActive(const char* callsign, GroundStateCategory observedCategory)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return false;

	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	const auto overrideIt = LineupOverrides.find(normalized);
	if (overrideIt == LineupOverrides.end())
		return false;

	if (observedCategory == GroundStateCategory::Taxi || observedCategory == GroundStateCategory::Lnup)
		return true;

	// SetScratchPadString("TAXI") updates EuroScope asynchronously on some
	// installations. Keep the local state briefly, then fail safely if the
	// host never reports TAXI or another controller changes the status.
	if (std::chrono::steady_clock::now() - overrideIt->second < std::chrono::seconds(2))
		return true;

	LineupOverrides.erase(overrideIt);
	return false;
}

// Snapshot cache of the latest vACDM pilot data keyed by normalized callsign.
std::mutex VacdmPilotsMutex;
std::map<std::string, VacdmPilotData> VacdmPilots;
std::atomic<bool> VacdmFetchInProgress(false);
std::atomic<clock_t> VacdmLastFetchClock(0);
const int VacdmFetchIntervalSeconds = 15;
const std::string VacdmPilotsUrlDefault = "https://app.vacdm.net/api/v1/pilots";
std::mutex ProfilesSourceMutex;
std::string ActiveProfilesConfigPath;
bool ActiveProfilesConfigPathClaimed = false;
unsigned long long ProfilesSourceGeneration = 0;
// Guarded by ProfilesSourceMutex. Workers consume only a copied snapshot.
std::string VacdmConfiguredServerUrl;
std::atomic<bool> VacdmPollingEnabled(false);
std::atomic<unsigned long> VacdmFetchCounter(0);
std::atomic<unsigned long> VacdmLastSehCode(0);
std::mutex VacdmDebugStateMutex;
std::string VacdmDebugAselCallsign;

namespace
{
	const std::time_t CdmWarningCooldownSeconds = 60;
	const int CdmReminderQueueMaxSendAttempts = 20;
	const int CdmMaximumMinutes = 24 * 60;
	const size_t HoppieResponseLimitBytes = 1024U * 1024U;
	const size_t VacdmResponseLimitBytes = 16U * 1024U * 1024U;
	const size_t WeatherResponseLimitBytes = 4096U;
	const char* ProtectedCredentialPrefix = "dpapi:";

	std::filesystem::path ResolveRuntimeAudioPath(const wchar_t* fileName)
	{
		std::wstring modulePathBuffer(32768, L'\0');
		const DWORD modulePathLength = ::GetModuleFileNameW(
			HINSTANCE(&__ImageBase),
			modulePathBuffer.data(),
			static_cast<DWORD>(modulePathBuffer.size()));

		std::filesystem::path pluginDirectory;
		if (modulePathLength > 0 && modulePathLength < modulePathBuffer.size())
		{
			modulePathBuffer.resize(modulePathLength);
			pluginDirectory = std::filesystem::path(modulePathBuffer).parent_path();
		}
		else if (!Logger::DLL_PATH.empty())
		{
			pluginDirectory = std::filesystem::path(Logger::DLL_PATH);
		}

		if (pluginDirectory.empty())
			return {};

		return pluginDirectory / L"vSMR_Data" / L"Audio" / fileName;
	}

	bool PlayRuntimeAudio(const wchar_t* fileName, const char* description)
	{
		const std::filesystem::path audioPath = ResolveRuntimeAudioPath(fileName);
		if (audioPath.empty())
		{
			Logger::info(std::string("Unable to resolve ") + description + " audio path");
			return false;
		}

		std::error_code ec;
		if (!std::filesystem::is_regular_file(audioPath, ec))
		{
			Logger::info(std::string(description) + " audio file is missing: " + audioPath.u8string());
			return false;
		}

		if (!::PlaySoundW(audioPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
		{
			Logger::info(std::string(description) + " audio playback failed: " + audioPath.u8string());
			return false;
		}

		return true;
	}

	struct DatalinkCredentialsSnapshot
	{
		std::string callsign;
		std::string password;
		bool playSound = false;
	};

	struct DatalinkLoginRequest
	{
		DatalinkCredentialsSnapshot credentials;
		unsigned long long generation = 0;
	};

	struct DatalinkPollRequest
	{
		CSMRPlugin* plugin = nullptr;
		DatalinkCredentialsSnapshot credentials;
		unsigned long long generation = 0;
		unsigned long long pollGeneration = 0;
		bool reportStatus = false;
	};

	struct DatalinkMessageRequest
	{
		DatalinkCredentialsSnapshot credentials;
		unsigned long long generation = 0;
		std::string destination;
		std::string type;
		std::string packet;
		std::string callsign;
	};

	struct DatalinkClearanceRequest
	{
		DatalinkCredentialsSnapshot credentials;
		unsigned long long generation = 0;
		DatalinkPacket packet;
		std::string fallbackFrequency;
		int messageSequence = 0;
	};

	DatalinkCredentialsSnapshot SnapshotDatalinkCredentials()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		DatalinkCredentialsSnapshot snapshot;
		snapshot.callsign = logonCallsign;
		snapshot.password = logonCode;
		snapshot.playSound = PlaySoundClr;
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

	HttpHelper& GetHttpHelper()
	{
		static HttpHelper helper;
		return helper;
	}

	enum class CdmQueueReminderOutcome
	{
		Queued = 0,
		AlreadyNotified,
		AlreadyQueued,
		AlreadyCleared,
		HasSubmittedTobt,
		Failed
	};

	bool HasSubmittedTobtState(const VacdmPilotData& pilotData);

	std::string ToUpperAsciiCopy(const std::string& text)
	{
		std::string normalized = text;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
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

	DATA_BLOB HoppieCredentialEntropy()
	{
		static char entropy[] = "vSMR CPDLC credential v1";
		DATA_BLOB blob = {};
		blob.pbData = reinterpret_cast<BYTE*>(entropy);
		blob.cbData = static_cast<DWORD>(strlen(entropy));
		return blob;
	}

	bool ProtectHoppieCredential(
		const std::string& plaintext,
		std::string& protectedValue)
	{
		protectedValue.clear();
		if (plaintext.empty())
			return true;
		if (plaintext.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)()))
			return false;

		DATA_BLOB input = {};
		input.pbData = reinterpret_cast<BYTE*>(
			const_cast<char*>(plaintext.data()));
		input.cbData = static_cast<DWORD>(plaintext.size());
		DATA_BLOB entropy = HoppieCredentialEntropy();
		DATA_BLOB encrypted = {};
		if (!::CryptProtectData(
			&input,
			L"vSMR Hoppie code",
			&entropy,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&encrypted))
		{
			return false;
		}

		DWORD encodedCharacters = 0;
		const DWORD base64Flags =
			CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
		bool succeeded = ::CryptBinaryToStringA(
			encrypted.pbData,
			encrypted.cbData,
			base64Flags,
			nullptr,
			&encodedCharacters) != FALSE;
		std::string encoded;
		if (succeeded && encodedCharacters > 0)
		{
			encoded.resize(encodedCharacters, '\0');
			succeeded = ::CryptBinaryToStringA(
				encrypted.pbData,
				encrypted.cbData,
				base64Flags,
				encoded.data(),
				&encodedCharacters) != FALSE;
			if (succeeded)
			{
				while (!encoded.empty() && encoded.back() == '\0')
					encoded.pop_back();
			}
		}
		if (encrypted.pbData != nullptr)
		{
			::SecureZeroMemory(encrypted.pbData, encrypted.cbData);
			::LocalFree(encrypted.pbData);
		}
		if (!succeeded || encoded.empty())
			return false;
		protectedValue = std::string(ProtectedCredentialPrefix) + encoded;
		return true;
	}

	bool UnprotectHoppieCredential(
		const std::string& storedValue,
		std::string& plaintext,
		bool& wasPlaintext)
	{
		plaintext.clear();
		wasPlaintext = false;
		if (storedValue.empty())
			return true;

		const size_t prefixLength = strlen(ProtectedCredentialPrefix);
		if (storedValue.compare(0, prefixLength, ProtectedCredentialPrefix) != 0)
		{
			plaintext = TrimAsciiWhitespaceCopy(storedValue);
			wasPlaintext = !plaintext.empty();
			return true;
		}

		const std::string encoded = storedValue.substr(prefixLength);
		if (encoded.empty())
			return false;
		DWORD decodedBytes = 0;
		if (!::CryptStringToBinaryA(
			encoded.c_str(),
			static_cast<DWORD>(encoded.size()),
			CRYPT_STRING_BASE64,
			nullptr,
			&decodedBytes,
			nullptr,
			nullptr) || decodedBytes == 0)
		{
			return false;
		}

		std::vector<BYTE> decoded(decodedBytes);
		if (!::CryptStringToBinaryA(
			encoded.c_str(),
			static_cast<DWORD>(encoded.size()),
			CRYPT_STRING_BASE64,
			decoded.data(),
			&decodedBytes,
			nullptr,
			nullptr))
		{
			::SecureZeroMemory(decoded.data(), decoded.size());
			return false;
		}

		DATA_BLOB encrypted = {};
		encrypted.pbData = decoded.data();
		encrypted.cbData = decodedBytes;
		DATA_BLOB entropy = HoppieCredentialEntropy();
		DATA_BLOB output = {};
		LPWSTR description = nullptr;
		const bool succeeded = ::CryptUnprotectData(
			&encrypted,
			&description,
			&entropy,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&output) != FALSE;
		::SecureZeroMemory(decoded.data(), decoded.size());
		if (description != nullptr)
			::LocalFree(description);
		if (!succeeded)
			return false;

		plaintext.assign(
			reinterpret_cast<const char*>(output.pbData),
			output.cbData);
		if (output.pbData != nullptr)
		{
			::SecureZeroMemory(output.pbData, output.cbData);
			::LocalFree(output.pbData);
		}
		return true;
	}

	std::string EncodeUrlQueryComponent(const std::string& text)
	{
		static const char hex[] = "0123456789ABCDEF";
		std::string encoded;
		encoded.reserve(text.size());
		for (unsigned char c : text)
		{
			const bool isAsciiAlphaNumeric =
				(c >= 'A' && c <= 'Z') ||
				(c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9');
			if (isAsciiAlphaNumeric || c == '-' || c == '_' || c == '.' || c == '~')
			{
				encoded.push_back(static_cast<char>(c));
				continue;
			}
			encoded.push_back('%');
			encoded.push_back(hex[(c >> 4) & 0x0F]);
			encoded.push_back(hex[c & 0x0F]);
		}
		return encoded;
	}

	std::string NormalizeHoppieResponse(const std::string& raw)
	{
		std::string normalized = raw;
		if (normalized.size() >= 3 &&
			static_cast<unsigned char>(normalized[0]) == 0xEF &&
			static_cast<unsigned char>(normalized[1]) == 0xBB &&
			static_cast<unsigned char>(normalized[2]) == 0xBF)
		{
			normalized.erase(0, 3);
		}
		normalized = TrimAsciiWhitespaceCopy(normalized);
		for (char& c : normalized)
		{
			if (c == '\r' || c == '\n' || c == '\t')
				c = ' ';
		}
		return normalized;
	}

	bool IsHoppieOkResponse(const std::string& raw)
	{
		const std::string normalized = NormalizeHoppieResponse(raw);
		if (normalized.size() < 2 ||
			std::tolower(static_cast<unsigned char>(normalized[0])) != 'o' ||
			std::tolower(static_cast<unsigned char>(normalized[1])) != 'k')
		{
			return false;
		}
		return normalized.size() == 2 ||
			std::isspace(static_cast<unsigned char>(normalized[2])) != 0 ||
			normalized[2] == '{';
	}

	std::string RedactSensitiveValue(std::string text, const std::string& secret)
	{
		if (secret.empty())
			return text;
		size_t position = 0;
		while ((position = text.find(secret, position)) != std::string::npos)
		{
			text.replace(position, secret.size(), "<redacted>");
			position += strlen("<redacted>");
		}
		return text;
	}

	std::string BuildHoppieLoginFailureMessage(
		const std::string& raw,
		const std::string& password)
	{
		std::string response = NormalizeHoppieResponse(raw);
		response = RedactSensitiveValue(response, password);
		response = RedactSensitiveValue(response, EncodeUrlQueryComponent(password));
		if (response.empty())
		{
			return "Connection failed: Hoppie returned no response. Check the network or proxy and try again.";
		}
		const size_t maximumResponseLength = 160;
		if (response.size() > maximumResponseLength)
			response = response.substr(0, maximumResponseLength) + "...";
		return "Hoppie rejected the connection: " + response;
	}

	std::string KeepAsciiAlnumCopy(const std::string& text)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (char c : text)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}
		return normalized;
	}

	std::string StripAtFirstCallsignDelimiter(const std::string& text)
	{
		const size_t pos = text.find_first_of("/\\ _.-");
		if (pos == std::string::npos)
			return text;
		return text.substr(0, pos);
	}

	std::vector<std::string> BuildVacdmLookupCandidates(const std::string& callsign)
	{
		std::vector<std::string> candidates;
		auto pushUnique = [&](const std::string& value) {
			const std::string candidate = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(value));
			if (candidate.empty())
				return;
			if (std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
				return;
			candidates.push_back(candidate);
			};

		const std::string trimmed = TrimAsciiWhitespaceCopy(callsign);
		pushUnique(trimmed);
		pushUnique(StripAtFirstCallsignDelimiter(trimmed));
		pushUnique(KeepAsciiAlnumCopy(trimmed));

		const size_t slashPos = trimmed.find('/');
		if (slashPos != std::string::npos)
			pushUnique(trimmed.substr(0, slashPos));

		return candidates;
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

	std::vector<std::string> CollectFlightPlanCandidateCallsignsForActiveAirport(EuroScopePlugIn::CPlugIn* plugIn)
	{
		std::vector<std::string> candidateCallsigns;
		if (plugIn == nullptr)
			return candidateCallsigns;

		const std::string activeAirportFilter = ResolveActiveAirportFilterUpper();
		candidateCallsigns.reserve(256);

		auto addUniqueCallsign = [&](const std::string& rawCallsign)
		{
			const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawCallsign));
			if (callsign.empty())
				return;
			if (std::find(candidateCallsigns.begin(), candidateCallsigns.end(), callsign) == candidateCallsigns.end())
				candidateCallsigns.push_back(callsign);
		};

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

			addUniqueCallsign(fpCallsignRaw);
		}

		return candidateCallsigns;
	}

	void PruneCdmReminderHistoryUnlocked(std::time_t nowUtc)
	{
		int cooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
		if (cooldownMinutes < 0)
			cooldownMinutes = 0;
		const std::time_t cooldownSeconds = static_cast<std::time_t>(cooldownMinutes) * 60;
		for (auto it = AircraftCdmTobtReminderSentUtc.begin(); it != AircraftCdmTobtReminderSentUtc.end();)
		{
			if (std::difftime(nowUtc, it->second) >= static_cast<double>(cooldownSeconds))
				it = AircraftCdmTobtReminderSentUtc.erase(it);
			else
				++it;
		}
	}

	bool HasRecentCdmReminderUnlocked(const std::string& callsign, std::time_t nowUtc)
	{
		auto it = AircraftCdmTobtReminderSentUtc.find(callsign);
		if (it == AircraftCdmTobtReminderSentUtc.end())
			return false;

		int cooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
		if (cooldownMinutes < 0)
			cooldownMinutes = 0;
		const std::time_t cooldownSeconds = static_cast<std::time_t>(cooldownMinutes) * 60;
		if (std::difftime(nowUtc, it->second) >= static_cast<double>(cooldownSeconds))
		{
			AircraftCdmTobtReminderSentUtc.erase(it);
			return false;
		}

		return true;
	}

	void MarkCdmReminderSentUnlocked(const std::string& callsign, std::time_t nowUtc)
	{
		AircraftCdmTobtReminderSentUtc[callsign] = nowUtc;
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

	bool QueueCdmReminderUnlocked(
		const std::string& callsign,
		const std::string& message,
		bool automatic)
	{
		if (callsign.empty() || message.empty())
			return false;
		if (IsCdmReminderQueuedUnlocked(callsign))
			return false;

		QueuedCdmReminderMessage queued;
		queued.callsign = callsign;
		queued.message = message;
		queued.sendAttempts = 0;
		queued.automatic = automatic;
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

	void MarkDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.insert(normalizedCallsign);
		RemoveQueuedCdmReminderUnlocked(normalizedCallsign);
	}

	void ClearDatalinkClearanceSentUnlocked(const std::string& callsign)
	{
		const std::string normalizedCallsign = NormalizeCallsignForState(callsign);
		if (normalizedCallsign.empty())
			return;

		AircraftDatalinkClearedCallsigns.erase(normalizedCallsign);
	}

	CdmQueueReminderOutcome TryQueueCdmReminderForCallsign(
		const std::string& callsign,
		const std::string& reminderMessage,
		std::time_t nowUtc,
		bool* outVacdmEvaluated = nullptr,
		bool* outHasVacdmData = nullptr,
		bool automatic = false)
	{
		if (outVacdmEvaluated != nullptr)
			*outVacdmEvaluated = false;
		if (outHasVacdmData != nullptr)
			*outHasVacdmData = false;

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		if (normalizedCallsign.empty() || reminderMessage.empty() || nowUtc <= 0)
			return CdmQueueReminderOutcome::Failed;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, nowUtc))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
		}

		VacdmPilotData pilotData;
		const bool hasVacdmData = TryGetVacdmPilotData(normalizedCallsign, pilotData);
		if (outVacdmEvaluated != nullptr)
			*outVacdmEvaluated = true;
		if (outHasVacdmData != nullptr)
			*outHasVacdmData = hasVacdmData;

		if (hasVacdmData && HasSubmittedTobtState(pilotData))
			return CdmQueueReminderOutcome::HasSubmittedTobt;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasRecentCdmReminderUnlocked(normalizedCallsign, nowUtc))
				return CdmQueueReminderOutcome::AlreadyNotified;
			if (IsCdmReminderQueuedUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyQueued;
			if (HasDatalinkClearanceSentUnlocked(normalizedCallsign))
				return CdmQueueReminderOutcome::AlreadyCleared;
			if (!QueueCdmReminderUnlocked(normalizedCallsign, reminderMessage, automatic))
				return CdmQueueReminderOutcome::Failed;
		}

		return CdmQueueReminderOutcome::Queued;
	}

	void ClearCdmAutoTrackingState(bool clearQueuedAutomaticReminders = false)
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftCdmAutoTracked.clear();
		if (clearQueuedAutomaticReminders)
		{
			CdmReminderMessageQueue.erase(
				std::remove_if(
					CdmReminderMessageQueue.begin(),
					CdmReminderMessageQueue.end(),
					[](const QueuedCdmReminderMessage& reminder)
					{
						return reminder.automatic;
					}),
				CdmReminderMessageQueue.end());
		}
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

	std::string NormalizeVacdmServerUrl(std::string value)
	{
		value = TrimAsciiWhitespaceCopy(value);
		while (!value.empty() && value.back() == '/')
			value.pop_back();
		std::string host;
		if (value.find('?') != std::string::npos ||
			!HttpHelper::IsValidHttpsUrl(value, &host))
		{
			return "";
		}
		const bool numericHost = !host.empty() &&
			std::all_of(host.begin(), host.end(), [](unsigned char character) {
				return std::isdigit(character) != 0 || character == '.';
			});
		const bool localHost = host == "localhost" ||
			(host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0) ||
			(host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0);
		if (numericHost || localHost || host.find('.') == std::string::npos)
			return "";
		return value;
	}

	std::filesystem::path ResolveDefaultProfilesConfigPath()
	{
		const std::filesystem::path pluginDirectory(Logger::DLL_PATH);
		const std::filesystem::path dataConfigPath = pluginDirectory / "vSMR_Data" / "vSMR_Profiles.json";

		std::error_code ec;
		if (std::filesystem::exists(dataConfigPath, ec))
			return dataConfigPath;
		return pluginDirectory / "vSMR_Profiles.json";
	}

	bool TryReadVacdmServerUrl(
		const std::filesystem::path& configPath,
		std::string& outServerUrl)
	{
		outServerUrl.clear();
		if (configPath.empty())
			return false;
		std::ifstream input(configPath, std::ios::binary);

		if (!input.is_open())
			return false;

		std::stringstream buffer;
		buffer << input.rdbuf();
		std::string json = buffer.str();
		if (json.size() >= 3 &&
			static_cast<unsigned char>(json[0]) == 0xEF &&
			static_cast<unsigned char>(json[1]) == 0xBB &&
			static_cast<unsigned char>(json[2]) == 0xBF)
		{
			json = json.substr(3);
		}

		rapidjson::Document document;
		if (document.Parse<0>(json.c_str()).HasParseError() || !document.IsArray())
			return false;

		for (rapidjson::SizeType i = 0; i < document.Size(); ++i)
		{
			const rapidjson::Value& entry = document[i];
			if (!entry.IsObject() ||
				!entry.HasMember("_vsmr") ||
				!entry["_vsmr"].IsObject())
			{
				continue;
			}

			const rapidjson::Value& metadata = entry["_vsmr"];
			if (!metadata.HasMember("vacdm") || !metadata["vacdm"].IsObject())
				continue;

			const rapidjson::Value& vacdm = metadata["vacdm"];
			if (!vacdm.HasMember("server_url") || !vacdm["server_url"].IsString())
				continue;

			const std::string value = NormalizeVacdmServerUrl(vacdm["server_url"].GetString());
			if (value.empty())
				continue;

			outServerUrl = value;
			return true;
		}

		return false;
	}

	std::string ResolveVacdmPilotsUrl(unsigned long long* sourceGeneration = nullptr)
	{
		std::string serverUrl;
		{
			std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
			serverUrl = VacdmConfiguredServerUrl;
			if (sourceGeneration != nullptr)
				*sourceGeneration = ProfilesSourceGeneration;
		}
		if (!serverUrl.empty())
			return serverUrl + "/api/v1/pilots";
		return VacdmPilotsUrlDefault;
	}

	bool TryParseIsoUtcTimestamp(const std::string& iso, std::time_t& outUtc)
	{
		outUtc = 0;
		if (iso.size() < 19)
			return false;

		int year = 0;
		int month = 0;
		int day = 0;
		int hour = 0;
		int minute = 0;
		int second = 0;
		if (::sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
			return false;

		std::tm tmUtc = {};
		tmUtc.tm_year = year - 1900;
		tmUtc.tm_mon = month - 1;
		tmUtc.tm_mday = day;
		tmUtc.tm_hour = hour;
		tmUtc.tm_min = minute;
		tmUtc.tm_sec = second;
		tmUtc.tm_isdst = 0;
		std::time_t parsed = _mkgmtime(&tmUtc);
		if (parsed <= 0)
			return false;

		outUtc = parsed;
		return true;
	}

	std::string FormatSehCode(unsigned long code)
	{
		char buffer[16] = {};
		sprintf_s(buffer, "0x%08lX", code);
		return std::string(buffer);
	}

	int CaptureVacdmSehCode(unsigned long sehCode)
	{
		VacdmLastSehCode.store(sehCode, std::memory_order_relaxed);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool HasConfirmedTobtState(const VacdmPilotData& pilotData)
	{
		const std::string state = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(pilotData.tobtState));
		return state == "CONFIRMED";
	}

	bool HasSubmittedTobtState(const VacdmPilotData& pilotData)
	{
		if (!pilotData.hasTobt)
			return false;

		const std::string state = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(pilotData.tobtState));
		if (state == "FLIGHTPLAN" || state == "GUESS" || state == "MISSING")
			return false;
		return true;
	}

	bool IsExpiredTobtAtConnectTime(const VacdmPilotData& pilotData, std::time_t connectedAtUtc)
	{
		if (!pilotData.hasTobt)
			return false;
		return std::difftime(connectedAtUtc, pilotData.tobtUtc) >= 0.0;
	}

	bool IsValidConfirmedTobtAtConnectTime(const VacdmPilotData& pilotData, std::time_t connectedAtUtc)
	{
		if (!HasConfirmedTobtState(pilotData) || !pilotData.hasTobt)
			return false;
		return std::difftime(pilotData.tobtUtc, connectedAtUtc) > 0.0;
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

	std::filesystem::path ResolveCdmAliasPath(EuroScopePlugIn::CPlugIn* plugIn)
	{
		std::vector<std::filesystem::path> candidates;

		std::filesystem::path processDirectory;
		char modulePath[MAX_PATH] = {};
		const DWORD modulePathLength = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
		if (modulePathLength > 0 && modulePathLength < MAX_PATH)
			processDirectory = std::filesystem::path(std::string(modulePath, modulePathLength)).parent_path();

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
			const std::filesystem::path pluginDir(Logger::DLL_PATH);
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

	bool TryReadCdmReminderMessageFromAlias(EuroScopePlugIn::CPlugIn* plugIn, std::string& outMessage, std::string& outAliasPath)
	{
		outMessage.clear();
		const std::filesystem::path aliasPath = ResolveCdmAliasPath(plugIn);
		outAliasPath = aliasPath.string();

		std::ifstream input(aliasPath);
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
		Logger::info("CDM alias load failed path=" + aliasPath);
	}

	bool TryLoadCdmReminderMessage(EuroScopePlugIn::CPlugIn* plugIn, std::string& outMessage)
	{
		std::string aliasPath;
		if (TryReadCdmReminderMessageFromAlias(plugIn, outMessage, aliasPath))
			return true;

		NotifyMissingCdmAliasMessage(plugIn, aliasPath);
		return false;
	}

	bool IsCallsignEligibleForCdmReminderNow(EuroScopePlugIn::CPlugIn* plugIn, const std::string& callsign)
	{
		if (plugIn == nullptr || callsign.empty())
			return false;

		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			if (HasDatalinkClearanceSentUnlocked(callsign))
				return false;
		}

		const std::vector<std::string> eligibleCallsigns = CollectFlightPlanCandidateCallsignsForActiveAirport(plugIn);
		if (std::find(eligibleCallsigns.begin(), eligibleCallsigns.end(), callsign) == eligibleCallsigns.end())
			return false;

		VacdmPilotData pilotData;
		if (TryGetVacdmPilotData(callsign, pilotData) && HasSubmittedTobtState(pilotData))
			return false;

		return true;
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
		LONG bestScore = LONG_MIN;
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

		LONG score = rect.top;
		score += width / 4;
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

		const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
		if (normalizedCallsign.empty() || message.empty())
			return false;

		const std::string command = ".msg " + normalizedCallsign + " " + message;
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

	bool SendDatalinkPacketMessage(const DatalinkMessageRequest& request)
	{
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(
				std::memory_order_acquire))
			return false;

		string raw;
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=";
		url += EncodeUrlQueryComponent(request.destination);
		url += "&type=";
		url += EncodeUrlQueryComponent(request.type);
		url += "&packet=";
		url += EncodeUrlQueryComponent(request.packet);

		raw.assign(GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes));
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(
				std::memory_order_acquire))
			return false;

		if (!startsWith("ok", raw.c_str()))
			return false;

		if (!request.callsign.empty())
		{
			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			PendingMessages.erase(request.callsign);
			RemoveCallsignUnlocked(AircraftMessage, request.callsign);
			AddCallsignUniqueUnlocked(AircraftMessageSent, request.callsign);
		}

		return true;
	}

	bool QueueDatalinkMessage(
		CSMRPlugin* plugin,
		const std::string& destination,
		const std::string& type,
		const std::string& packet,
		const std::string& callsign)
	{
		if (plugin == nullptr ||
			PluginShutdownRequested.load(std::memory_order_acquire))
		{
			return false;
		}
		DatalinkMessageRequest request;
		request.credentials = SnapshotDatalinkCredentials();
		request.generation = HoppieConnectionGeneration.load(
			std::memory_order_acquire);
		request.destination = destination;
		request.type = type;
		request.packet = packet;
		request.callsign = callsign;
		return plugin->QueueNetworkJob([request]() {
			(void)SendDatalinkPacketMessage(request);
		});
	}
}

bool TryGetVacdmPilotData(const std::string& callsign, VacdmPilotData& outData)
{
	std::lock_guard<std::mutex> guard(VacdmPilotsMutex);
	// Match with the same normalization strategy used during ingest.
	const std::vector<std::string> candidates = BuildVacdmLookupCandidates(callsign);
	for (const auto& candidate : candidates)
	{
		auto it = VacdmPilots.find(candidate);
		if (it != VacdmPilots.end())
		{
			outData = it->second;
			return true;
		}
	}
	return false;
}

void ProcessCdmAutoMode(CSMRPlugin* plugIn)
{
	if (plugIn == nullptr || !CdmAutoModeEnabled.load(std::memory_order_relaxed))
		return;

	int delayMinutes = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	if (delayMinutes < 0)
		delayMinutes = 0;
	const std::time_t nowUtc = std::time(nullptr);
	if (nowUtc <= 0)
		return;

	const std::vector<std::string> connectedCallsigns = CollectFlightPlanCandidateCallsignsForActiveAirport(plugIn);
	std::vector<std::string> callsignsToQueue;
	callsignsToQueue.reserve(connectedCallsigns.size());

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(nowUtc);

		for (auto it = AircraftCdmAutoTracked.begin(); it != AircraftCdmAutoTracked.end();)
		{
			if (std::find(connectedCallsigns.begin(), connectedCallsigns.end(), it->first) == connectedCallsigns.end())
				it = AircraftCdmAutoTracked.erase(it);
			else
				++it;
		}

		for (const std::string& callsign : connectedCallsigns)
		{
			auto trackedIt = AircraftCdmAutoTracked.find(callsign);
			if (trackedIt == AircraftCdmAutoTracked.end())
			{
				CdmAutoTrackedAircraftState state;
				state.connectedAtUtc = nowUtc;
				state.dueAtUtc = nowUtc + static_cast<std::time_t>(delayMinutes) * 60;
				state.eligibility = CdmAutoEligibility::Pending;
				AircraftCdmAutoTracked.emplace(callsign, state);
			}
		}

		for (const std::string& callsign : connectedCallsigns)
		{
			if (HasRecentCdmReminderUnlocked(callsign, nowUtc))
				continue;
			if (IsCdmReminderQueuedUnlocked(callsign))
				continue;

			auto trackedIt = AircraftCdmAutoTracked.find(callsign);
			if (trackedIt == AircraftCdmAutoTracked.end())
				continue;

			CdmAutoTrackedAircraftState& tracked = trackedIt->second;
			if (tracked.eligibility == CdmAutoEligibility::SuppressValidAtConnect)
				continue;

			// During the delay window, cancel the reminder if TOBT gets submitted.
			if (tracked.eligibility == CdmAutoEligibility::NotifyAfterDelay)
			{
				VacdmPilotData updatedPilotData;
				if (TryGetVacdmPilotData(callsign, updatedPilotData) && HasSubmittedTobtState(updatedPilotData))
				{
					tracked.eligibility = CdmAutoEligibility::SuppressValidAtConnect;
					continue;
				}
			}

			if (tracked.eligibility == CdmAutoEligibility::Pending)
			{
				VacdmPilotData pilotData;
				const bool hasVacdmData = TryGetVacdmPilotData(callsign, pilotData);
				if (hasVacdmData)
				{
					if (IsExpiredTobtAtConnectTime(pilotData, tracked.connectedAtUtc))
					{
						tracked.eligibility = CdmAutoEligibility::NotifyImmediateExpiredAtConnect;
						tracked.dueAtUtc = nowUtc;
					}
					else if (IsValidConfirmedTobtAtConnectTime(pilotData, tracked.connectedAtUtc))
					{
						tracked.eligibility = CdmAutoEligibility::SuppressValidAtConnect;
					}
					else
					{
						tracked.eligibility = CdmAutoEligibility::NotifyAfterDelay;
						tracked.dueAtUtc = tracked.connectedAtUtc + static_cast<std::time_t>(delayMinutes) * 60;
					}
				}
				else if (nowUtc >= tracked.connectedAtUtc + static_cast<std::time_t>(delayMinutes) * 60)
				{
					tracked.eligibility = CdmAutoEligibility::NotifyAfterDelay;
					tracked.dueAtUtc = tracked.connectedAtUtc + static_cast<std::time_t>(delayMinutes) * 60;
				}
			}

			if ((tracked.eligibility == CdmAutoEligibility::NotifyAfterDelay ||
				tracked.eligibility == CdmAutoEligibility::NotifyImmediateExpiredAtConnect) &&
				nowUtc >= tracked.dueAtUtc)
			{
				callsignsToQueue.push_back(callsign);
			}
		}
	}

	int queuedCount = 0;
	std::string reminderMessage;
	if (!callsignsToQueue.empty() && !TryLoadCdmReminderMessage(plugIn, reminderMessage))
	{
		return;
	}

	for (const std::string& callsign : callsignsToQueue)
	{
		const CdmQueueReminderOutcome outcome =
			TryQueueCdmReminderForCallsign(
				callsign,
				reminderMessage,
				nowUtc,
				nullptr,
				nullptr,
				true);
		if (outcome == CdmQueueReminderOutcome::Queued)
			++queuedCount;
	}

	if (queuedCount > 0)
		Logger::info("CDM auto reminder queued count=" + std::to_string(queuedCount) + " delay_min=" + std::to_string(delayMinutes));
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
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(nowUtc);
		if (CdmReminderMessageQueue.empty())
			return;

		queuedReminder = CdmReminderMessageQueue.front();
		CdmReminderMessageQueue.pop_front();
	}

	const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(queuedReminder.callsign));
	const std::string message = TrimAsciiWhitespaceCopy(queuedReminder.message);
	if (callsign.empty() || message.empty())
		return;

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		if (HasRecentCdmReminderUnlocked(callsign, nowUtc))
			return;
	}

	if (!IsCallsignEligibleForCdmReminderNow(plugIn, callsign))
		return;

	if (SendPrivateChatMessageLikeDotMsg(plugIn, callsign, message))
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		MarkCdmReminderSentUnlocked(callsign, nowUtc);
		return;
	}

	queuedReminder.sendAttempts += 1;
	if (queuedReminder.sendAttempts >= CdmReminderQueueMaxSendAttempts)
	{
		Logger::info("CDM queued reminder dropped callsign=" + callsign);
		return;
	}

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		if (!HasRecentCdmReminderUnlocked(callsign, nowUtc) && !IsCdmReminderQueuedUnlocked(callsign))
			CdmReminderMessageQueue.push_back(queuedReminder);
	}
}

void refreshVacdmDataImpl()
{
	unsigned long long sourceGeneration = 0;
	const std::string pilotsUrl = ResolveVacdmPilotsUrl(&sourceGeneration);

	struct ResetFetchFlag
	{
		unsigned long long sourceGeneration = 0;
		explicit ResetFetchFlag(unsigned long long generation)
			: sourceGeneration(generation) {}

		~ResetFetchFlag()
		{
			bool sourceStillCurrent = false;
			{
				std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
				sourceStillCurrent =
					ProfilesSourceGeneration == sourceGeneration;
			}
			if (sourceStillCurrent)
				VacdmLastFetchClock = clock();
			VacdmFetchInProgress.store(false);
		}
	} reset{ sourceGeneration };

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		!VacdmPollingEnabled.load(std::memory_order_acquire))
		return;

	try
	{
		std::string raw = GetHttpHelper().downloadStringFromURL(
			pilotsUrl,
			6000,
			&PluginShutdownRequested,
			VacdmResponseLimitBytes);
		if (PluginShutdownRequested.load(std::memory_order_relaxed))
			return;

		if (raw.empty())
		{
			Logger::info("VACDM refresh failed: empty response url=" + pilotsUrl);
			return;
		}

		rapidjson::Document doc;
		if (doc.Parse<0>(raw.c_str()).HasParseError() || !doc.IsArray())
		{
			Logger::info("VACDM refresh failed: invalid JSON array url=" + pilotsUrl);
			return;
		}

		// Parse into a temporary map so readers never observe a partially refreshed cache.
		std::map<std::string, VacdmPilotData> parsedData;

		for (rapidjson::SizeType i = 0; i < doc.Size(); ++i)
		{
			const rapidjson::Value& pilot = doc[i];
			if (!pilot.IsObject() || !pilot.HasMember("callsign") || !pilot["callsign"].IsString())
				continue;

			VacdmPilotData data;
			data.callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(pilot["callsign"].GetString()));

			const rapidjson::Value* vacdm = nullptr;
			if (pilot.HasMember("vacdm") && pilot["vacdm"].IsObject())
				vacdm = &pilot["vacdm"];

			auto readTime = [&](const char* key, std::time_t& outTime, bool& outHas) {
				outTime = 0;
				outHas = false;
				if (vacdm == nullptr || !vacdm->HasMember(key) || !(*vacdm)[key].IsString())
					return;
				std::time_t parsed = 0;
				if (TryParseIsoUtcTimestamp((*vacdm)[key].GetString(), parsed))
				{
					outTime = parsed;
					outHas = true;
				}
				};

			readTime("tobt", data.tobtUtc, data.hasTobt);
			readTime("tsat", data.tsatUtc, data.hasTsat);
			readTime("ttot", data.ttotUtc, data.hasTtot);
			readTime("asat", data.asatUtc, data.hasAsat);
			readTime("aobt", data.aobtUtc, data.hasAobt);
			readTime("atot", data.atotUtc, data.hasAtot);
			readTime("asrt", data.asrtUtc, data.hasAsrt);
			readTime("aort", data.aortUtc, data.hasAort);
			readTime("ctot", data.ctotUtc, data.hasCtot);

			if (vacdm != nullptr && vacdm->HasMember("tobt_state") && (*vacdm)["tobt_state"].IsString())
				data.tobtState = (*vacdm)["tobt_state"].GetString();

			if (pilot.HasMember("hasBooking") && pilot["hasBooking"].IsBool())
				data.hasBooking = pilot["hasBooking"].GetBool();

			parsedData[data.callsign] = data;
		}

		if (PluginShutdownRequested.load(std::memory_order_relaxed))
			return;

		std::string aselCallsign;
		{
			std::lock_guard<std::mutex> stateGuard(VacdmDebugStateMutex);
			aselCallsign = VacdmDebugAselCallsign;
		}
		const size_t parsedPilotCount = parsedData.size();
		const bool aselFound = !aselCallsign.empty() && parsedData.find(aselCallsign) != parsedData.end();

		{
			std::lock_guard<std::mutex> sourceGuard(ProfilesSourceMutex);
			if (ProfilesSourceGeneration != sourceGeneration)
				return;
			std::lock_guard<std::mutex> pilotsGuard(VacdmPilotsMutex);
			VacdmPilots.swap(parsedData);
		}

		const unsigned long fetchIndex = ++VacdmFetchCounter;
		Logger::info(
			"VACDM refresh #" + std::to_string(fetchIndex) +
			" pilots=" + std::to_string(parsedPilotCount) +
			" asel=" + (aselCallsign.empty() ? std::string("<none>") : aselCallsign) +
			" asel_present=" + std::string(aselFound ? "1" : "0") +
			" url=" + pilotsUrl
		);
	}
	catch (const std::exception& ex)
	{
		Logger::info("VACDM refresh exception: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("VACDM refresh exception: unknown");
	}
}

void refreshVacdmData()
{
#if defined(_MSC_VER)
	__try
	{
		refreshVacdmDataImpl();
	}
	__except (CaptureVacdmSehCode(static_cast<unsigned long>(GetExceptionCode())))
	{
		VacdmLastFetchClock = clock();
		VacdmFetchInProgress.store(false);
	}
#else
	refreshVacdmDataImpl();
#endif
}

void datalinkLogin(DatalinkLoginRequest request) {
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;
	struct ResetConnectingFlag
	{
		unsigned long long generation = 0;
		~ResetConnectingFlag()
		{
			if (generation == HoppieConnectionGeneration.load(std::memory_order_acquire))
				HoppieConnecting.store(false, std::memory_order_release);
		}
	} resetConnecting{ request.generation };

	bool connected = false;
	std::string failureMessage;
	try
	{
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=SERVER&type=PING";
		const string raw = GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes);
		connected = IsHoppieOkResponse(raw);
		if (!connected)
			failureMessage = BuildHoppieLoginFailureMessage(raw, request.credentials.password);
	}
	catch (const std::exception& exception)
	{
		connected = false;
		failureMessage = "Connection failed before Hoppie replied.";
		std::string exceptionDetail = RedactSensitiveValue(
			exception.what(),
			request.credentials.password);
		exceptionDetail = RedactSensitiveValue(
			exceptionDetail,
			EncodeUrlQueryComponent(request.credentials.password));
		Logger::info("CPDLC login exception: " + exceptionDetail);
	}
	catch (...)
	{
		connected = false;
		failureMessage = "Connection failed before Hoppie replied.";
		Logger::info("CPDLC login exception: unknown");
	}

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
	{
		return;
	}

	if (connected)
	{
		SetDatalinkStatusMessage("Connected.");
		HoppieConnected.store(true, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		ConnectionMessage.store(true, std::memory_order_release);
	}
	else
	{
		if (failureMessage.empty())
			failureMessage = "Connection failed: Hoppie rejected the login.";
		ConnectionMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage(failureMessage);
		HoppieConnected.store(false, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		Logger::info(failureMessage);
		FailedToConnectMessage.store(true, std::memory_order_release);
	}
};

void pollMessages(DatalinkPollRequest request) {
	struct ResetPollFlag
	{
		unsigned long long pollGeneration = 0;
		~ResetPollFlag()
		{
			if (pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
		}
	} resetPoll{ request.pollGeneration };
	const auto completePoll = [&](bool succeeded)
	{
		if (request.pollGeneration ==
			HoppiePollGeneration.load(std::memory_order_acquire))
		{
			HoppiePollInProgress.store(false, std::memory_order_release);
		}
		if (!request.reportStatus ||
			PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			return;
		}
		SetDatalinkStatusMessage(succeeded ? "Poll complete." : "Poll failed.");
	};

	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		completePoll(false);
		return;
	}

	string raw;
	try
	{
		string url = baseUrlDatalink;
		url += "?logon=";
		url += EncodeUrlQueryComponent(request.credentials.password);
		url += "&from=";
		url += EncodeUrlQueryComponent(request.credentials.callsign);
		url += "&to=SERVER&type=POLL";
		raw.assign(GetHttpHelper().downloadStringFromURL(
			url,
			6000,
			&PluginShutdownRequested,
			HoppieResponseLimitBytes));
	}
	catch (...)
	{
		completePoll(false);
		return;
	}

	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
	{
		completePoll(false);
		return;
	}

	if (!startsWith("ok", raw.c_str()))
	{
		completePoll(false);
		return;
	}
	if (raw.size() <= 3)
	{
		completePoll(true);
		return;
	}

	raw = raw + " ";
	raw = raw.substr(3, raw.size() - 3);

	string delimiter = "}} ";
	size_t pos = 0;
	std::string token;
	while ((pos = raw.find(delimiter)) != std::string::npos) {
		if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			completePoll(false);
			return;
		}

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
					QueueDatalinkMessage(
						request.plugin,
						message.from,
						"CPDLC",
						"UNABLE",
						"");
				} else {
					if (request.credentials.playSound) {
						PlayRuntimeAudio(L"Ding.wav", "CPDLC notification");
					}
					std::lock_guard<std::mutex> guard(DatalinkStateMutex);
					AddCallsignUniqueUnlocked(AircraftDemandingClearance, message.from);
				}
			}
			else if (message.message.find("WILCO") != std::string::npos || message.message.find("ROGER") != std::string::npos || message.message.find("RGR") != std::string::npos) {
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				if (ContainsCallsignUnlocked(AircraftMessageSent, message.from)) {
					AddCallsignUniqueUnlocked(AircraftWilco, message.from);
				}
			}
			else if (message.message.length() != 0 ){
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				AddCallsignUniqueUnlocked(AircraftMessage, message.from);
			}
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				PendingMessages[message.from] = message;
			}
		}

		raw.erase(0, pos + delimiter.length());
	}

	completePoll(true);
};

namespace
{
	bool StartDatalinkPoll(bool reportStatus, std::string& error)
	{
		error.clear();
		if (PluginShutdownRequested.load(std::memory_order_relaxed))
		{
			error = "The CPDLC service is shutting down.";
			return false;
		}
		if (!HoppieConnected.load(std::memory_order_acquire))
		{
			error = "CPDLC is not connected.";
			return false;
		}

		bool expected = false;
		if (!HoppiePollInProgress.compare_exchange_strong(
			expected,
			true,
			std::memory_order_acq_rel))
		{
			error = "A CPDLC poll is already in progress.";
			return false;
		}

		CSMRPlugin* plugin = ActivePluginInstance.load(std::memory_order_acquire);
		if (plugin == nullptr)
		{
			HoppiePollInProgress.store(false, std::memory_order_release);
			error = "The CPDLC service is unavailable.";
			return false;
		}
		DatalinkPollRequest request;
		request.plugin = plugin;
		request.credentials = SnapshotDatalinkCredentials();
		request.generation = HoppieConnectionGeneration.load(std::memory_order_acquire);
		request.pollGeneration =
			HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		request.reportStatus = reportStatus;
		if (!HoppieConnected.load(std::memory_order_acquire) ||
			request.generation != HoppieConnectionGeneration.load(std::memory_order_acquire))
		{
			if (request.pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
			error = "CPDLC disconnected before the poll could start.";
			return false;
		}

		if (reportStatus)
			SetDatalinkStatusMessage("Polling...");

		if (!plugin->QueueNetworkJob([request]() { pollMessages(request); }))
		{
			if (request.pollGeneration == HoppiePollGeneration.load(std::memory_order_acquire))
				HoppiePollInProgress.store(false, std::memory_order_release);
			error = "Unable to queue the CPDLC poll request.";
			if (reportStatus)
				SetDatalinkStatusMessage(error);
			return false;
		}
		return true;
	}
}

void sendDatalinkClearance(DatalinkClearanceRequest request) {
	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(
			std::memory_order_acquire))
		return;
	const DatalinkPacket& packet = request.packet;

	string payload = "/data2/";
	payload += std::to_string(request.messageSequence);
	payload += "//R/";
	payload += "CLR TO @";
	payload += packet.destination;
	payload += "@ RWY @";
	payload += packet.rwy;
	payload += "@ DEP @";
	payload += packet.sid;
	payload += "@ INIT CLB @";
	payload += packet.climb;
	payload += "@ SQUAWK @";
	payload += packet.squawk;
	payload += "@ ";
	if (packet.ctot != "no" && packet.ctot.size() > 3) {
		payload += "CTOT @";
		payload += packet.ctot;
		payload += "@ ";
	}
	if (packet.asat != "no" && packet.asat.size() > 3) {
		payload += "TSAT @";
		payload += packet.asat;
		payload += "@ ";
	}
	if (packet.freq != "no" && packet.freq.size() > 5) {
		payload += "WHEN RDY CALL FREQ @";
		payload += packet.freq;
		payload += "@";
	}
	else {
		payload += "WHEN RDY CALL @";
		payload += request.fallbackFrequency;
		payload += "@";
	}
	payload += " IF UNABLE CALL VOICE ";
	if (packet.message != "no" && packet.message.size() > 1)
		payload += packet.message;

	string url = baseUrlDatalink;
	url += "?logon=";
	url += EncodeUrlQueryComponent(request.credentials.password);
	url += "&from=";
	url += EncodeUrlQueryComponent(request.credentials.callsign);
	url += "&to=";
	url += EncodeUrlQueryComponent(packet.callsign);
	url += "&type=CPDLC&packet=";
	url += EncodeUrlQueryComponent(payload);

	const string raw = GetHttpHelper().downloadStringFromURL(
		url,
		6000,
		&PluginShutdownRequested,
		HoppieResponseLimitBytes);
	if (PluginShutdownRequested.load(std::memory_order_relaxed) ||
		request.generation != HoppieConnectionGeneration.load(
			std::memory_order_acquire))
		return;

	if (startsWith("ok", raw.c_str())) {
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		RemoveCallsignUnlocked(AircraftDemandingClearance, packet.callsign);
		RemoveCallsignUnlocked(AircraftStandby, packet.callsign);
		PendingMessages.erase(packet.callsign);
		AddCallsignUniqueUnlocked(AircraftMessageSent, packet.callsign);
		MarkDatalinkClearanceSentUnlocked(packet.callsign);
	}
};

std::string CSMRPlugin::GetActiveProfilesConfigPath(
	bool* selectionClaimed)
{
	std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
	if (selectionClaimed != nullptr)
		*selectionClaimed = ActiveProfilesConfigPathClaimed;
	return ActiveProfilesConfigPath;
}

void CSMRPlugin::PublishActiveProfilesConfigPath(
	const std::string& path,
	bool claimSelection)
{
	std::string configuredVacdmServerUrl;
	const bool vacdmConfigured = TryReadVacdmServerUrl(
		std::filesystem::path(path),
		configuredVacdmServerUrl);
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		ActiveProfilesConfigPath = path;
		ActiveProfilesConfigPathClaimed = claimSelection;
		VacdmConfiguredServerUrl = vacdmConfigured
			? configuredVacdmServerUrl
			: std::string();
		++ProfilesSourceGeneration;
		// Publish the enable state before workers can observe the new source
		// generation. This prevents an old `true` value from starting a fetch
		// against the fallback URL after switching to a profile without VACDM.
		VacdmPollingEnabled.store(vacdmConfigured, std::memory_order_release);
		std::lock_guard<std::mutex> pilotsGuard(VacdmPilotsMutex);
		VacdmPilots.clear();
	}

	VacdmLastFetchClock.store(0, std::memory_order_relaxed);
	if (vacdmConfigured)
	{
		Logger::info(
			"VACDM polling enabled profiles=" + path +
			" server_url=" + configuredVacdmServerUrl);
	}
	else
	{
		Logger::info(
			"VACDM polling disabled profiles=" + path +
			" (no _vsmr.vacdm.server_url)");
	}
}

bool CSMRPlugin::QueueNetworkJob(std::function<void()> job)
{
	if (!job || PluginShutdownRequested.load(std::memory_order_acquire) ||
		NetworkCancellationRequested.load(std::memory_order_acquire))
	{
		return false;
	}

	std::unique_lock<std::mutex> lock(NetworkWorkerMutex);
	if (NetworkWorkersStopping)
		return false;
	if (NetworkWorkers.empty())
	{
		try
		{
			NetworkWorkers.emplace_back(&CSMRPlugin::NetworkWorkerMain, this);
			try
			{
				NetworkWorkers.emplace_back(&CSMRPlugin::NetworkWorkerMain, this);
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Network worker pool running with one worker: " +
					std::string(ex.what()));
			}
		}
		catch (const std::exception& ex)
		{
			Logger::info(
				"Unable to start network worker: " +
				std::string(ex.what()));
			return false;
		}
		catch (...)
		{
			Logger::info("Unable to start network worker: unknown error");
			return false;
		}
	}

	try
	{
		NetworkJobs.emplace_back(std::move(job));
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Unable to queue network request: " +
			std::string(ex.what()));
		return false;
	}
	lock.unlock();
	NetworkWorkerCondition.notify_one();
	return true;
}

void CSMRPlugin::NetworkWorkerMain()
{
	try
	{
		for (;;)
		{
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(NetworkWorkerMutex);
				NetworkWorkerCondition.wait(lock, [this]() {
					return NetworkWorkersStopping || !NetworkJobs.empty();
				});
				if (NetworkWorkersStopping)
					return;
				job = std::move(NetworkJobs.front());
				NetworkJobs.pop_front();
			}

			try
			{
				job();
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Network job exception: " +
						std::string(ex.what()));
			}
			catch (...)
			{
				Logger::info("Network job exception: unknown");
			}
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Network worker terminated by exception: " +
			std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("Network worker terminated by unknown exception");
	}
}

void CSMRPlugin::StopNetworkWorkers()
{
	NetworkCancellationRequested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
		NetworkWorkersStopping = true;
		NetworkJobs.clear();
	}
	NetworkWorkerCondition.notify_all();
	for (std::thread& worker : NetworkWorkers)
	{
		if (worker.joinable())
			::CancelSynchronousIo(worker.native_handle());
	}
	for (std::thread& worker : NetworkWorkers)
	{
		if (worker.joinable())
			worker.join();
	}
	NetworkWorkers.clear();
}

bool CSMRPlugin::WriteDiagnosticsReport(
	std::string& reportPath,
	std::string& error)
{
	reportPath.clear();
	error.clear();
	try
	{
		const std::filesystem::path pluginDirectory(Logger::DLL_PATH);
		const std::filesystem::path dataDirectory =
			pluginDirectory / "vSMR_Data";
		const std::filesystem::path webUiDirectory =
			dataDirectory / "vSMR_webUI";
		std::filesystem::path diagnosticsDirectory =
			dataDirectory / "Diagnostics";
		std::error_code pathError;
		std::filesystem::create_directories(diagnosticsDirectory, pathError);
		if (pathError)
		{
			wchar_t temporaryPath[32768] = {};
			const DWORD length = ::GetTempPathW(
				static_cast<DWORD>(std::size(temporaryPath)),
				temporaryPath);
			if (length == 0 || length >= std::size(temporaryPath))
			{
				error = "Unable to resolve a writable diagnostics folder.";
				return false;
			}
			diagnosticsDirectory =
				std::filesystem::path(temporaryPath) / "vSMR_Diagnostics";
			pathError.clear();
			std::filesystem::create_directories(
				diagnosticsDirectory,
				pathError);
			if (pathError)
			{
				error = "Unable to create a diagnostics folder.";
				return false;
			}
		}

		SYSTEMTIME utc = {};
		::GetSystemTime(&utc);
		char fileName[96] = {};
		_snprintf_s(
			fileName,
			_TRUNCATE,
			"vSMR_diagnostics_%04u%02u%02u_%02u%02u%02u_%lu.txt",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			static_cast<unsigned long>(::GetCurrentProcessId()));
		const std::filesystem::path target = diagnosticsDirectory / fileName;
		std::filesystem::path temporary = target;
		temporary += ".tmp";

		const std::filesystem::path logPath =
			pluginDirectory / "vsmr.log";
		bool logWritable = false;
		{
			std::ofstream logProbe(logPath, std::ios::binary | std::ios::app);
			logWritable = logProbe.good();
		}

		const std::string profilesPath = GetActiveProfilesConfigPath();
		const DatalinkControlState datalink = GetDatalinkControlState();
		const DatalinkCredentialsSnapshot credentials =
			SnapshotDatalinkCredentials();
		const std::vector<std::string> recentLogMessages =
			Logger::recent_messages();
		size_t queuedNetworkJobs = 0;
		size_t networkWorkerCount = 0;
		{
			std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
			queuedNetworkJobs = NetworkJobs.size();
			networkWorkerCount = NetworkWorkers.size();
		}
		size_t queuedWeatherRequests = 0;
		bool weatherWorkerRunning = false;
		{
			std::lock_guard<std::mutex> lock(WeatherFetchMutex);
			queuedWeatherRequests = WeatherFetchQueue.size();
			weatherWorkerRunning = WeatherFetchThread.joinable();
		}

		auto singleLine = [](std::string value) {
			for (char& character : value)
			{
				if (character == '\r' || character == '\n' || character == '\t')
					character = ' ';
			}
			if (value.size() > 512)
				value.resize(512);
			return value;
		};
		auto yesNo = [](bool value) { return value ? "yes" : "no"; };
		std::error_code existsError;
		std::ostringstream report;
		report << "vSMR support diagnostics\n";
		report << "version=" << MY_PLUGIN_VERSION << "\n";
#if defined(_M_IX86)
		report << "architecture=x86\n";
#elif defined(_M_X64)
		report << "architecture=x64\n";
#else
		report << "architecture=unknown\n";
#endif
		report << "process_id=" << ::GetCurrentProcessId() << "\n";
		report << "generated_utc="
			<< utc.wYear << '-'
			<< std::setfill('0') << std::setw(2) << utc.wMonth << '-'
			<< std::setw(2) << utc.wDay << 'T'
			<< std::setw(2) << utc.wHour << ':'
			<< std::setw(2) << utc.wMinute << ':'
			<< std::setw(2) << utc.wSecond << "Z\n";
		report << "plugin_directory=" << pluginDirectory.u8string() << "\n";
		report << "data_directory=" << dataDirectory.u8string() << "\n";
		report << "data_directory_exists="
			<< yesNo(std::filesystem::is_directory(dataDirectory, existsError)) << "\n";
		existsError.clear();
		report << "webui_directory=" << webUiDirectory.u8string() << "\n";
		report << "webui_directory_exists="
			<< yesNo(std::filesystem::is_directory(webUiDirectory, existsError)) << "\n";
		report << "profiles_path=" << profilesPath << "\n";
		existsError.clear();
		report << "profiles_exists="
			<< yesNo(!profilesPath.empty() && std::filesystem::is_regular_file(
				std::filesystem::path(profilesPath), existsError)) << "\n";
		report << "logging_enabled=" << yesNo(Logger::ENABLED) << "\n";
		report << "logging_mode=" << Logger::mode_name(Logger::get_mode()) << "\n";
		report << "log_path=" << logPath.u8string() << "\n";
		report << "log_writable=" << yesNo(logWritable) << "\n";
		report << "active_airport=" << singleLine(datalink.activeAirport) << "\n";
		report << "cpdlc_connected=" << yesNo(datalink.connected) << "\n";
		report << "cpdlc_connecting=" << yesNo(datalink.connecting) << "\n";
		report << "cpdlc_polling=" << yesNo(datalink.pollInProgress) << "\n";
		report << "cpdlc_has_protected_code=" << yesNo(datalink.hasPassword) << "\n";
		report << "cpdlc_status=" << singleLine(datalink.statusMessage) << "\n";
		report << "vacdm_configured=" << yesNo(datalink.vacdmConfigured) << "\n";
		report << "cdm_auto_enabled=" << yesNo(datalink.cdmAutoEnabled) << "\n";
		report << "network_workers=" << networkWorkerCount << "\n";
		report << "network_jobs_queued=" << queuedNetworkJobs << "\n";
		report << "weather_worker_running=" << yesNo(weatherWorkerRunning) << "\n";
		report << "weather_requests_queued=" << queuedWeatherRequests << "\n";
		report << "radar_screens=" << RadarScreensOpened.size() << "\n";
		report << "shutdown_requested="
			<< yesNo(PluginShutdownRequested.load(std::memory_order_acquire)) << "\n";
		report << "recent_log_entries="
			<< (std::min)(recentLogMessages.size(), static_cast<size_t>(64))
			<< "\n";
		report << "Recent entries can contain operational callsigns; credentials are redacted.\n";
		const size_t firstRecentEntry = recentLogMessages.size() > 64
			? recentLogMessages.size() - 64
			: 0;
		for (size_t index = firstRecentEntry;
			index < recentLogMessages.size();
			++index)
		{
			std::string redacted = RedactSensitiveValue(
				recentLogMessages[index],
				credentials.password);
			redacted = RedactSensitiveValue(
				redacted,
				EncodeUrlQueryComponent(credentials.password));
			report << "recent_log=" << singleLine(redacted) << "\n";
		}
		report << "Secrets, message payloads, and endpoint query strings are intentionally omitted.\n";

		{
			std::ofstream output(
				temporary,
				std::ios::binary | std::ios::trunc);
			if (!output)
			{
				error = "Unable to create the diagnostics report.";
				return false;
			}
			output << report.str();
			output.flush();
			if (!output.good())
			{
				output.close();
				std::filesystem::remove(temporary, pathError);
				error = "Unable to write the diagnostics report.";
				return false;
			}
		}
		pathError.clear();
		std::filesystem::rename(temporary, target, pathError);
		if (pathError)
		{
			std::filesystem::remove(temporary, existsError);
			error = "Unable to finalize the diagnostics report.";
			return false;
		}
		reportPath = target.u8string();
		return true;
	}
	catch (const std::exception& ex)
	{
		error = std::string("Diagnostics failed: ") + ex.what();
		return false;
	}
	catch (...)
	{
		error = "Diagnostics failed unexpectedly.";
		return false;
	}
}

CSMRPlugin::CSMRPlugin(void) :CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, MY_PLUGIN_NAME, MY_PLUGIN_VERSION, MY_PLUGIN_DEVELOPER, MY_PLUGIN_COPYRIGHT)
{
	ActivePluginInstance.store(this, std::memory_order_release);
	PluginShutdownRequested.store(false, std::memory_order_relaxed);
	NetworkCancellationRequested.store(false, std::memory_order_relaxed);
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		logonCallsign = "EGKK";
		logonCode.clear();
		PlaySoundClr = false;
		DatalinkStatusMessage = "Disconnected.";
	}
	CdmAutoModeEnabled.store(false, std::memory_order_relaxed);
	CdmAutoDelayMinutes.store(5, std::memory_order_relaxed);
	CdmReminderCooldownMinutes.store(60, std::memory_order_relaxed);

	Logger::DLL_PATH = "";
	Logger::ENABLED = false;
	Logger::set_mode(Logger::Mode::Normal);

	// Register the plugin radar screen type.
	RegisterDisplayType(MY_PLUGIN_VIEW_AVISO, false, true, true, true);

	RegisterTagItemType("Datalink clearance", TAG_ITEM_DATALINK_STS);
	RegisterTagItemFunction("Datalink menu", TAG_FUNC_DATALINK_MENU);

	messageId.store(rand() % 10000 + 1789);

	timer = clock();
	VacdmLastFetchClock = 0;

	const char * p_value;
	bool migratePlaintextCredential = false;
	std::string migratedProtectedCredential;

	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		if ((p_value = GetDataFromSettings("cpdlc_logon")) != NULL)
			logonCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(p_value));
		if ((p_value = GetDataFromSettings("cpdlc_password")) != NULL)
		{
			bool wasPlaintext = false;
			std::string unprotectedCredential;
			if (UnprotectHoppieCredential(
				p_value,
				unprotectedCredential,
				wasPlaintext))
			{
				logonCode = std::move(unprotectedCredential);
				migratePlaintextCredential = wasPlaintext;
			}
			else
			{
				logonCode.clear();
				DatalinkStatusMessage =
					"The saved Hoppie code could not be unlocked. Enter it again.";
				Logger::info("CPDLC saved credential could not be decrypted");
			}
		}
		if ((p_value = GetDataFromSettings("cpdlc_sound")) != NULL)
			PlaySoundClr = bool(!!atoi(p_value));
	}
	if (migratePlaintextCredential)
	{
		const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
		if (ProtectHoppieCredential(
			credentials.password,
			migratedProtectedCredential))
		{
			SaveDataToSettings(
				"cpdlc_password",
				"The protected CPDLC Hoppie code",
				migratedProtectedCredential.c_str());
			Logger::info("CPDLC saved credential migrated to Windows DPAPI protection");
		}
		else
		{
			SaveDataToSettings(
				"cpdlc_password",
				"The protected CPDLC Hoppie code",
				"");
			Logger::info("CPDLC plaintext credential migration failed; persistent copy removed");
		}
	}
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
	{
		std::lock_guard<std::mutex> guard(ProfilesSourceMutex);
		ActiveProfilesConfigPath.clear();
		ActiveProfilesConfigPathClaimed = false;
		ProfilesSourceGeneration = 0;
		VacdmConfiguredServerUrl.clear();
	}
	PublishActiveProfilesConfigPath(
		ResolveDefaultProfilesConfigPath().string(),
		false);

	bool rdfEnabled = true;
	if ((p_value = GetDataFromSettings("rdf_enabled")) != NULL)
		rdfEnabled = atoi(p_value) != 0;
	VsmrRdf::Start(this, rdfEnabled);
}

CSMRPlugin::~CSMRPlugin()
{
	PluginShutdownRequested.store(true, std::memory_order_relaxed);
	VsmrGroundState::ClearAllLineupOverrides();
	VsmrRdf::Stop();
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	VacdmPollingEnabled.store(false, std::memory_order_relaxed);
	StopWeatherFetchWorker();
	StopNetworkWorkers();
	CSMRPlugin* expectedActivePlugin = this;
	ActivePluginInstance.compare_exchange_strong(
		expectedActivePlugin,
		nullptr,
		std::memory_order_acq_rel);
	VsmrWeather::Clear();

	// Persist CPDLC settings via EuroScope's plugin settings storage.
	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", credentials.callsign.c_str());
	std::string protectedCredential;
	if (ProtectHoppieCredential(credentials.password, protectedCredential))
	{
		SaveDataToSettings(
			"cpdlc_password",
			"The protected CPDLC Hoppie code",
			protectedCredential.c_str());
	}
	else
	{
		Logger::info("CPDLC credential was not persisted because DPAPI protection failed");
	}
	SaveDataToSettings("cpdlc_sound", "Play sound on clearance request", credentials.playSound ? "1" : "0");
	SaveDataToSettings("cdm_auto_enabled", "Enable automatic CDM reminder messaging", CdmAutoModeEnabled.load(std::memory_order_relaxed) ? "1" : "0");
	int cdmAutoDelayToPersist = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	if (cdmAutoDelayToPersist < 0)
		cdmAutoDelayToPersist = 0;
	SaveDataToSettings("cdm_auto_delay_min", "CDM auto reminder delay in minutes", std::to_string(cdmAutoDelayToPersist).c_str());
	int cdmCooldownToPersist = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
	if (cdmCooldownToPersist < 0)
		cdmCooldownToPersist = 0;
	SaveDataToSettings("cdm_cooldown_min", "CDM reminder resend cooldown in minutes", std::to_string(cdmCooldownToPersist).c_str());
}

void CSMRPlugin::StopWeatherFetchWorker()
{
	WeatherFetchCancellationRequested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> guard(WeatherFetchMutex);
		WeatherFetchStop = true;
		WeatherFetchQueue.clear();
		WeatherFetchQueued.clear();
	}
	WeatherFetchCondition.notify_all();
	if (WeatherFetchThread.joinable())
	{
		::CancelSynchronousIo(WeatherFetchThread.native_handle());
		WeatherFetchThread.join();
	}
}

DatalinkControlState CSMRPlugin::GetDatalinkControlState() const
{
	DatalinkControlState state;
	state.connected = HoppieConnected.load(std::memory_order_acquire);
	state.connecting = HoppieConnecting.load(std::memory_order_acquire);
	state.pollInProgress = HoppiePollInProgress.load(std::memory_order_acquire);
	state.controllerConnected = ControllerMyself().IsController();
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		state.logonCallsign = logonCallsign;
		state.hasPassword = !TrimAsciiWhitespaceCopy(logonCode).empty();
		state.playSound = PlaySoundClr;
		state.statusMessage = DatalinkStatusMessage;
	}
	state.cdmAutoEnabled = CdmAutoModeEnabled.load(std::memory_order_relaxed);
	state.cdmDelayMinutes = CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	state.cdmCooldownMinutes = CdmReminderCooldownMinutes.load(std::memory_order_relaxed);
	state.vacdmConfigured = VacdmPollingEnabled.load(std::memory_order_relaxed);
	state.activeAirport = ResolveActiveAirportFilterUpper();

	std::string aliasMessage;
	state.cdmAliasReady = TryReadCdmReminderMessageFromAlias(
		const_cast<CSMRPlugin*>(this),
		aliasMessage,
		state.cdmAliasPath);
	return state;
}

bool CSMRPlugin::UpdateDatalinkControlSettings(
	const std::string& callsign,
	const std::string& password,
	bool replacePassword,
	bool playSound,
	bool cdmAutoEnabled,
	int delayMinutes,
	int cooldownMinutes,
	std::string& error)
{
	error.clear();
	const std::string normalizedCallsign =
		ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	const std::string normalizedPassword =
		replacePassword ? TrimAsciiWhitespaceCopy(password) : std::string();
	if (normalizedCallsign.empty())
	{
		error = "The CPDLC logon callsign is required.";
		return false;
	}
	if (replacePassword && normalizedPassword.empty())
	{
		error = "Enter a Hoppie code before replacing the saved code.";
		return false;
	}
	if (delayMinutes < 0 || delayMinutes > CdmMaximumMinutes)
	{
		error = "The CDM auto delay must be between 0 and 1440 minutes.";
		return false;
	}
	if (cooldownMinutes < 0 || cooldownMinutes > CdmMaximumMinutes)
	{
		error = "The CDM reminder cooldown must be between 0 and 1440 minutes.";
		return false;
	}

	const bool previousAutoEnabled =
		CdmAutoModeEnabled.load(std::memory_order_relaxed);
	const int previousDelayMinutes =
		CdmAutoDelayMinutes.load(std::memory_order_relaxed);
	std::string effectivePassword;
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		effectivePassword = replacePassword ? normalizedPassword : logonCode;
	}
	std::string protectedPasswordToPersist;
	if (!ProtectHoppieCredential(
		effectivePassword,
		protectedPasswordToPersist))
	{
		error = "Windows could not protect the Hoppie code. Settings were not changed.";
		Logger::info("CPDLC settings update rejected because DPAPI protection failed");
		return false;
	}
	bool credentialsChanged = false;
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		credentialsChanged =
			ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(logonCallsign)) != normalizedCallsign ||
			(replacePassword && logonCode != normalizedPassword);
		logonCallsign = normalizedCallsign;
		if (replacePassword)
			logonCode = normalizedPassword;
		PlaySoundClr = playSound;
	}
	if (credentialsChanged &&
		(HoppieConnected.load(std::memory_order_acquire) ||
			HoppieConnecting.load(std::memory_order_acquire)))
	{
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppieConnected.store(false, std::memory_order_release);
		HoppieConnecting.store(false, std::memory_order_release);
		HoppiePollInProgress.store(false, std::memory_order_release);
		ConnectionMessage.store(false, std::memory_order_relaxed);
		FailedToConnectMessage.store(false, std::memory_order_relaxed);
		SetDatalinkStatusMessage("Credentials changed. Reconnect CPDLC to apply them.");
	}
	else if (credentialsChanged)
	{
		SetDatalinkStatusMessage("Credentials updated. Ready to connect.");
	}
	CdmAutoModeEnabled.store(cdmAutoEnabled, std::memory_order_relaxed);
	CdmAutoDelayMinutes.store(delayMinutes, std::memory_order_relaxed);
	CdmReminderCooldownMinutes.store(cooldownMinutes, std::memory_order_relaxed);

	if (previousAutoEnabled != cdmAutoEnabled ||
		previousDelayMinutes != delayMinutes)
	{
		ClearCdmAutoTrackingState(true);
	}

	SaveDataToSettings(
		"cpdlc_logon",
		"The CPDLC logon callsign",
		normalizedCallsign.c_str());
	SaveDataToSettings(
		"cpdlc_password",
		"The protected CPDLC Hoppie code",
		protectedPasswordToPersist.c_str());
	SaveDataToSettings(
		"cpdlc_sound",
		"Play sound on clearance request",
		playSound ? "1" : "0");
	SaveDataToSettings(
		"cdm_auto_enabled",
		"Enable automatic CDM reminder messaging",
		cdmAutoEnabled ? "1" : "0");
	SaveDataToSettings(
		"cdm_auto_delay_min",
		"CDM auto reminder delay in minutes",
		std::to_string(delayMinutes).c_str());
	SaveDataToSettings(
		"cdm_cooldown_min",
		"CDM reminder resend cooldown in minutes",
		std::to_string(cooldownMinutes).c_str());
	return true;
}

bool CSMRPlugin::ConnectDatalink(std::string& error)
{
	error.clear();
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		error = "The CPDLC service is shutting down.";
		return false;
	}
	if (!ControllerMyself().IsController())
	{
		error = "You are not logged in as a controller.";
		return false;
	}
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		error = "CPDLC is already connected.";
		return false;
	}

	const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
	if (TrimAsciiWhitespaceCopy(credentials.callsign).empty() ||
		TrimAsciiWhitespaceCopy(credentials.password).empty())
	{
		error = "A CPDLC logon callsign and Hoppie code are required.";
		return false;
	}

	bool expected = false;
	if (!HoppieConnecting.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel))
	{
		error = "A CPDLC connection attempt is already in progress.";
		return false;
	}
	if (HoppieConnected.load(std::memory_order_acquire))
	{
		HoppieConnecting.store(false, std::memory_order_release);
		error = "CPDLC is already connected.";
		return false;
	}

	const unsigned long long generation =
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Connecting...");

	DatalinkLoginRequest request;
	request.credentials = credentials;
	request.generation = generation;
	if (!QueueNetworkJob([request]() { datalinkLogin(request); }))
	{
		HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
		HoppieConnecting.store(false, std::memory_order_release);
		error = "Unable to queue the CPDLC connection request.";
		SetDatalinkStatusMessage(error);
		return false;
	}
	return true;
}

bool CSMRPlugin::DisconnectDatalink(std::string& error)
{
	error.clear();
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_release);
	HoppieConnecting.store(false, std::memory_order_release);
	HoppiePollInProgress.store(false, std::memory_order_release);
	ConnectionMessage.store(false, std::memory_order_relaxed);
	FailedToConnectMessage.store(false, std::memory_order_relaxed);
	SetDatalinkStatusMessage("Disconnected.");
	return true;
}

bool CSMRPlugin::PollDatalink(std::string& error)
{
	return StartDatalinkPoll(true, error);
}

bool CSMRPlugin::RunCdmReminderScan(std::string& result, std::string& error)
{
	result.clear();
	error.clear();
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		error = "The CDM reminder service is shutting down.";
		return false;
	}

	const std::vector<std::string> candidateCallsigns =
		CollectFlightPlanCandidateCallsignsForActiveAirport(this);
	const std::time_t nowUtc = std::time(nullptr);
	std::string reminderMessage;
	std::string aliasPath;
	if (!TryReadCdmReminderMessageFromAlias(this, reminderMessage, aliasPath))
	{
		error = "Missing or invalid .cdm alias";
		if (!aliasPath.empty())
			error += " in " + aliasPath;
		error += ".";
		return false;
	}

	int alreadyNotifiedCount = 0;
	int alreadyQueuedCount = 0;
	int alreadyClearedCount = 0;
	int hasTobtCount = 0;
	int queuedCount = 0;
	int failedCount = 0;
	int missingVacdmCount = 0;

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PruneCdmReminderHistoryUnlocked(nowUtc);
	}

	for (const std::string& callsign : candidateCallsigns)
	{
		bool vacdmEvaluated = false;
		bool hasVacdmData = false;
		const CdmQueueReminderOutcome outcome =
			TryQueueCdmReminderForCallsign(
				callsign,
				reminderMessage,
				nowUtc,
				&vacdmEvaluated,
				&hasVacdmData);
		if (vacdmEvaluated && !hasVacdmData)
			++missingVacdmCount;

		switch (outcome)
		{
		case CdmQueueReminderOutcome::Queued:
			++queuedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyNotified:
			++alreadyNotifiedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyQueued:
			++alreadyQueuedCount;
			break;
		case CdmQueueReminderOutcome::AlreadyCleared:
			++alreadyClearedCount;
			break;
		case CdmQueueReminderOutcome::HasSubmittedTobt:
			++hasTobtCount;
			break;
		case CdmQueueReminderOutcome::Failed:
		default:
			++failedCount;
			break;
		}
	}

	const int checkedCount = static_cast<int>(candidateCallsigns.size());
	result = "CDM check: ";
	result += std::to_string(checkedCount) + " checked, ";
	result += std::to_string(queuedCount) + " queued, ";
	result += std::to_string(hasTobtCount) + " already has TOBT, ";
	result += std::to_string(alreadyNotifiedCount) + " already notified, ";
	result += std::to_string(alreadyQueuedCount) + " already queued, ";
	result += std::to_string(alreadyClearedCount) + " already cleared, ";
	result += std::to_string(missingVacdmCount) + " missing VACDM, ";
	result += std::to_string(failedCount) + " failed.";
	return true;
}

bool CSMRPlugin::OnCompileCommand(const char * sCommandLine) {
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return false;

	const std::string command = TrimAsciiWhitespaceCopy(sCommandLine == nullptr ? "" : std::string(sCommandLine));
	std::string commandLower = command;
	std::transform(commandLower.begin(), commandLower.end(), commandLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const auto startsWithCommand = [&](const char* prefix) -> bool
	{
		if (prefix == nullptr)
			return false;
		const std::string p(prefix);
		return commandLower.rfind(p, 0) == 0;
	};

	if (commandLower == ".smr diagnostics" || commandLower == ".smr diag")
	{
		std::string reportPath;
		std::string error;
		if (WriteDiagnosticsReport(reportPath, error))
		{
			const std::string message =
				"Redacted diagnostics written to " + reportPath;
			DisplayUserMessage(
				"vSMR",
				"Diagnostics",
				message.c_str(),
				true,
				true,
				false,
				true,
				false);
			Logger::info("Diagnostics report written path=" + reportPath);
		}
		else
		{
			DisplayUserMessage(
				"vSMR",
				"Diagnostics",
				error.c_str(),
				true,
				true,
				false,
				true,
				false);
		}
		return true;
	}
	if (startsWithCommand(".smr connect"))
	{
		const DatalinkControlState state = GetDatalinkControlState();
		std::string error;
		if (state.connected || state.connecting)
		{
			DisconnectDatalink(error);
			DisplayUserMessage(
				"CPDLC",
				"Server",
				state.connected ? "Logged off!" : "Connection attempt cancelled.",
				true,
				true,
				false,
				true,
				false);
		}
		else if (!ConnectDatalink(error))
		{
			DisplayUserMessage("CPDLC", "Error", error.c_str(), true, true, false, true, false);
		}

		return true;
	}
	else if (startsWithCommand(".smr poll"))
	{
		std::string error;
		if (!PollDatalink(error) && !error.empty())
			DisplayUserMessage("CPDLC", "Error", error.c_str(), true, true, false, true, false);
		return true;
	}
	else if (commandLower == ".smr reload") {
		for (auto rd : RadarScreensOpened) {
			if (rd != nullptr)
				rd->ReloadConfig();
		}
		DisplayUserMessage("vSMR", "Config", "Reloaded vSMR runtime data", true, true, false, true, false);
		return true;
	}
	else if (commandLower == ".smr rdf" || startsWithCommand(".smr rdf "))
	{
		const std::string prefix = ".smr rdf";
		const std::string argument = commandLower.size() > prefix.size()
			? TrimAsciiWhitespaceCopy(commandLower.substr(prefix.size()))
			: std::string();

		if (argument == "on" || argument == "enable" || argument == "1")
		{
			VsmrRdf::SetEnabled(true);
			SaveDataToSettings("rdf_enabled", "Enable the native vSMR RDF overlay", "1");
		}
		else if (argument == "off" || argument == "disable" || argument == "0")
		{
			VsmrRdf::SetEnabled(false);
			SaveDataToSettings("rdf_enabled", "Enable the native vSMR RDF overlay", "0");
		}
		else if (!argument.empty() && argument != "status")
		{
			DisplayUserMessage(
				"vSMR",
				"RDF",
				"Usage: .smr rdf [on|off|status]",
				true, true, false, true, false);
			return true;
		}

		const VsmrRdf::Status status = VsmrRdf::GetStatus();
		std::string message = "Native RDF ";
		message += status.enabled ? "enabled" : "disabled";
		message += ", TrackAudio ";
		message += status.trackAudioConnected ? "connected" : "waiting";
		message += ", active transmissions=" + std::to_string(status.activeTransmissionCount);
		DisplayUserMessage("vSMR", "RDF", message.c_str(), true, true, false, true, false);
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar != nullptr && !radar->IsShutdownRequested())
				radar->RequestRefresh();
		}
		return true;
	}
	else if (startsWithCommand(".smr cdm cooldown"))
	{
		const std::string prefix = ".smr cdm cooldown";
		std::string argument = "";
		if (commandLower.size() > prefix.size())
			argument = TrimAsciiWhitespaceCopy(commandLower.substr(prefix.size()));

		const auto publishCooldownStatus = [&](const char* action)
		{
			int cooldownMinutes = GetDatalinkControlState().cdmCooldownMinutes;
			if (cooldownMinutes < 0)
				cooldownMinutes = 0;
			std::string message = std::string(action) + " CDM reminder cooldown: " + std::to_string(cooldownMinutes) + " minute";
			if (cooldownMinutes != 1)
				message += "s";
			DisplayUserMessage("vSMR", "CDM", message.c_str(), true, true, false, true, false);
		};

		if (argument.empty() || argument == "status")
		{
			publishCooldownStatus("Status");
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

		const DatalinkControlState state = GetDatalinkControlState();
		std::string updateError;
		if (!UpdateDatalinkControlSettings(
			state.logonCallsign,
			"",
			false,
			state.playSound,
			state.cdmAutoEnabled,
			state.cdmDelayMinutes,
			parsedCooldownMinutes,
			updateError))
		{
			DisplayUserMessage("vSMR", "CDM", updateError.c_str(), true, true, false, true, false);
			return true;
		}
		publishCooldownStatus("Updated");
		return true;
	}
	else if (startsWithCommand(".smr cdm auto"))
	{
		const std::string prefix = ".smr cdm auto";
		std::string argument = "";
		if (commandLower.size() > prefix.size())
			argument = TrimAsciiWhitespaceCopy(commandLower.substr(prefix.size()));

		const auto publishCdmAutoStatus = [&](const char* action)
		{
			const DatalinkControlState state = GetDatalinkControlState();
			const bool enabled = state.cdmAutoEnabled;
			int delayMinutes = state.cdmDelayMinutes;
			if (delayMinutes < 0)
				delayMinutes = 0;

			std::string message = std::string(action) + " CDM auto mode: ";
			message += enabled ? "enabled" : "disabled";
			message += ", delay=" + std::to_string(delayMinutes) + " minute";
			if (delayMinutes != 1)
				message += "s";
			if (enabled && delayMinutes == 0)
				message += " (immediate)";

			DisplayUserMessage("vSMR", "CDM", message.c_str(), true, true, false, true, false);
		};

		if (argument.empty() || argument == "status")
		{
			publishCdmAutoStatus("Status");
			return true;
		}

		if (argument == "off" || argument == "disable")
		{
			const DatalinkControlState state = GetDatalinkControlState();
			std::string updateError;
			if (!UpdateDatalinkControlSettings(
				state.logonCallsign,
				"",
				false,
				state.playSound,
				false,
				state.cdmDelayMinutes,
				state.cdmCooldownMinutes,
				updateError))
			{
				DisplayUserMessage("vSMR", "CDM", updateError.c_str(), true, true, false, true, false);
				return true;
			}
			publishCdmAutoStatus("Updated");
			return true;
		}

		if (argument == "on" || argument == "enable")
		{
			const DatalinkControlState state = GetDatalinkControlState();
			std::string updateError;
			if (!UpdateDatalinkControlSettings(
				state.logonCallsign,
				"",
				false,
				state.playSound,
				true,
				state.cdmDelayMinutes,
				state.cdmCooldownMinutes,
				updateError))
			{
				DisplayUserMessage("vSMR", "CDM", updateError.c_str(), true, true, false, true, false);
				return true;
			}
			publishCdmAutoStatus("Updated");
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

		const DatalinkControlState state = GetDatalinkControlState();
		std::string updateError;
		if (!UpdateDatalinkControlSettings(
			state.logonCallsign,
			"",
			false,
			state.playSound,
			true,
			parsedDelayMinutes,
			state.cdmCooldownMinutes,
			updateError))
		{
			DisplayUserMessage("vSMR", "CDM", updateError.c_str(), true, true, false, true, false);
			return true;
		}
		publishCdmAutoStatus("Updated");
		return true;
	}
	else if (commandLower == ".smr cdm")
	{
		std::string result;
		std::string error;
		if (RunCdmReminderScan(result, error))
			DisplayUserMessage("vSMR", "CDM", result.c_str(), true, true, false, true, false);
		else if (!error.empty())
			DisplayUserMessage("vSMR", "CDM", error.c_str(), true, true, false, true, false);
		return true;
	}
	else if (startsWithCommand(".smr log")) {
		const std::string prefix = ".smr log";
		std::string argument = "";
		if (commandLower.size() > prefix.size())
			argument = TrimAsciiWhitespaceCopy(commandLower.substr(prefix.size()));

		auto publishLogStatus = [&](const std::string& action)
		{
			std::string detail = action + " - vsmr.log ";
			detail += Logger::ENABLED ? "enabled" : "disabled";
			if (Logger::ENABLED)
			{
				detail += " (";
				detail += Logger::mode_name(Logger::get_mode());
				detail += ")";
			}
			detail += " at ";
			detail += Logger::DLL_PATH;
			detail += "\\vsmr.log";
			DisplayUserMessage("vSMR", "Log", detail.c_str(), true, true, false, true, false);
			if (Logger::ENABLED)
			{
				Logger::info("Logging active mode=" + std::string(Logger::mode_name(Logger::get_mode())));
			}
		};

		if (argument.empty())
		{
			if (Logger::ENABLED)
			{
				Logger::ENABLED = false;
			}
			else
			{
				Logger::ENABLED = true;
				Logger::set_mode(Logger::Mode::Normal);
			}
			publishLogStatus("Updated");
			return true;
		}

		if (argument == "status")
		{
			publishLogStatus("Status");
			return true;
		}

		if (argument == "off" || argument == "disable" || argument == "0")
		{
			Logger::ENABLED = false;
			publishLogStatus("Updated");
			return true;
		}

		if (argument == "on" || argument == "enable" || argument == "1" ||
			argument == "normal" || argument == "n")
		{
			Logger::ENABLED = true;
			Logger::set_mode(Logger::Mode::Normal);
			publishLogStatus("Updated");
			return true;
		}

		if (argument == "verbose" || argument == "v")
		{
			Logger::ENABLED = true;
			Logger::set_mode(Logger::Mode::Verbose);
			publishLogStatus("Updated");
			return true;
		}

		DisplayUserMessage(
			"vSMR",
			"Log",
			"Usage: .smr log [normal|verbose|off|status]",
			true,
			true,
			false,
			true,
			false);
		return true;
	}
	else if (commandLower == ".smr editor" || commandLower == ".smr vsmr" || commandLower == ".smr config" || commandLower == ".smr profile")
	{
		const std::string pageName =
			commandLower == ".smr profile" ? "profiles" :
			commandLower == ".smr config" ? "settings" :
			"overview";
		bool opened = false;
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr)
				continue;
			rd->OpenVsmrControlCenterWindow(pageName);
			opened = true;
			break;
		}

		if (!opened)
		{
			DisplayUserMessage("vSMR", "Config", "No active SMR radar screen found to open the vSMR window.", true, true, false, true, false);
		}
		return true;
	}
	else if (commandLower == ".smr")
	{
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr)
				continue;
			rd->OpenVsmrControlCenterWindow("settings");
			return true;
		}

		const DatalinkCredentialsSnapshot credentials = SnapshotDatalinkCredentials();
		auto applyCpdlcDialogValues = [&](const CCPDLCSettingsDialog& dialog) -> bool
		{
			const DatalinkControlState state = GetDatalinkControlState();
			std::string updateError;
			if (UpdateDatalinkControlSettings(
				static_cast<const char*>(CStringA(dialog.m_Logon)),
				static_cast<const char*>(CStringA(dialog.m_Password)),
				true,
				dialog.m_Sound != 0,
				state.cdmAutoEnabled,
				state.cdmDelayMinutes,
				state.cdmCooldownMinutes,
				updateError))
			{
				return true;
			}
			DisplayUserMessage("CPDLC", "Error", updateError.c_str(), true, true, false, true, false);
			return false;
		};

		CCPDLCSettingsDialog dia(AfxGetMainWnd());
		dia.m_Logon = credentials.callsign.c_str();
		dia.m_Password = credentials.password.c_str();
		dia.m_Sound = int(credentials.playSound);

		INT_PTR dialogResult = dia.DoModal();
		if (dialogResult == -1)
		{
			CCPDLCSettingsDialog diaNoParent(nullptr);
			diaNoParent.m_Logon = credentials.callsign.c_str();
			diaNoParent.m_Password = credentials.password.c_str();
			diaNoParent.m_Sound = int(credentials.playSound);
			dialogResult = diaNoParent.DoModal();
			if (dialogResult == IDOK)
			{
				if (!applyCpdlcDialogValues(diaNoParent))
					return true;
			}
		}
		else if (dialogResult == IDOK)
		{
			if (!applyCpdlcDialogValues(dia))
				return true;
		}

		if (dialogResult == -1)
		{
			const DWORD lastError = ::GetLastError();
			const HRSRC dlgResource = ::FindResource(AfxGetResourceHandle(), MAKEINTRESOURCE(CCPDLCSettingsDialog::IDD), RT_DIALOG);
			std::string detail = "Failed to open CPDLC settings window";
			detail += " (GetLastError=" + std::to_string(static_cast<unsigned long>(lastError));
			detail += ", resource=" + std::string(dlgResource != nullptr ? "ok" : "missing") + ")";
			DisplayUserMessage("CPDLC", "Error", detail.c_str(), true, true, false, true, false);
			return true;
		}
		if (dialogResult != IDOK)
			return true;

		return true;
	}
	return false;
}

void CSMRPlugin::OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int * pColorCode, COLORREF * pRGB, double * pFontSize) {
	(void)RadarTarget;
	(void)TagData;
	(void)pFontSize;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		strcpy_s(sItemString, 16, "");
		return;
	}

	if (ItemCode != TAG_ITEM_DATALINK_STS)
		return;

	*pColorCode = TAG_COLOR_RGB_DEFINED;
	*pRGB = RGB(130, 130, 130);
	strcpy_s(sItemString, 16, "-");

	if (!FlightPlan.IsValid())
		return;

	const char* fpCallsign = FlightPlan.GetCallsign();
	if (fpCallsign == nullptr || fpCallsign[0] == '\0')
		return;

	const std::string callsign = fpCallsign;
	bool isDemanding = false;
	bool isStandby = false;
	bool hasMessage = false;
	bool isWilco = false;
	bool isMessageSent = false;
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		isDemanding = ContainsCallsignUnlocked(AircraftDemandingClearance, callsign);
		isStandby = ContainsCallsignUnlocked(AircraftStandby, callsign);
		hasMessage = ContainsCallsignUnlocked(AircraftMessage, callsign);
		isWilco = ContainsCallsignUnlocked(AircraftWilco, callsign);
		isMessageSent = ContainsCallsignUnlocked(AircraftMessageSent, callsign);
	}

	if (isDemanding) {
		if (!BLINK)
			*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, isStandby ? "S" : "R");
		return;
	}

	if (hasMessage) {
		if (!BLINK)
			*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, "T");
		return;
	}

	if (isWilco) {
		*pRGB = RGB(0, 176, 0);
		strcpy_s(sItemString, 16, "V");
		return;
	}

	if (isMessageSent) {
		*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, "V");
		return;
	}
}

void CSMRPlugin::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area)
{
	(void)sItemString;
	(void)Pt;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (FunctionId == TAG_FUNC_DATALINK_MENU) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		bool menu_is_datalink = true;

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign != nullptr && fpCallsign[0] != '\0')
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				if (ContainsCallsignUnlocked(AircraftDemandingClearance, fpCallsign))
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
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			std::lock_guard<std::mutex> guard(DatalinkStateMutex);
			RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
			RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
			RemoveCallsignUnlocked(AircraftMessageSent, fpCallsign);
			RemoveCallsignUnlocked(AircraftWilco, fpCallsign);
			RemoveCallsignUnlocked(AircraftMessage, fpCallsign);
			ClearDatalinkClearanceSentUnlocked(fpCallsign);
			PendingMessages.erase(fpCallsign);
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_STBY) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				AddCallsignUniqueUnlocked(AircraftStandby, fpCallsign);
			}
			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"CPDLC",
				"STANDBY",
				fpCallsign))
			{
				Logger::info("CPDLC STANDBY request could not be queued");
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_MESSAGE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			AFX_MANAGE_STATE(AfxGetStaticModuleState());

			CDataLinkDialog dia;
			dia.SetDialogMode(CDataLinkDialog::DialogMode::Message);
			dia.m_Callsign = fpCallsign;
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();

			AcarsMessage msg;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				auto msgIt = PendingMessages.find(fpCallsign);
				if (msgIt != PendingMessages.end())
					msg = msgIt->second;
			}
			dia.m_Req = msg.message.c_str();

			string toReturn = "";

			if (dia.DoModal() != IDOK)
				return;

			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"TELEX",
				static_cast<const char*>(dia.m_Message),
				fpCallsign))
			{
				Logger::info("CPDLC free-text message could not be queued");
			}
		}
	}

	if (FunctionId == TAG_FUNC_DATALINK_VOICE) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
				RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
				PendingMessages.erase(fpCallsign);
			}

			if (!QueueDatalinkMessage(
				this,
				fpCallsign,
				"CPDLC",
				"UNABLE CALL ON FREQ",
				fpCallsign))
			{
				Logger::info("CPDLC voice fallback message could not be queued");
			}
		}

	}

	if (FunctionId == TAG_FUNC_DATALINK_CONFIRM) {
		CFlightPlan FlightPlan = FlightPlanSelectASEL();

		if (FlightPlan.IsValid()) {
			const char* fpCallsign = FlightPlan.GetCallsign();
			if (fpCallsign == nullptr || fpCallsign[0] == '\0')
				return;

			AFX_MANAGE_STATE(AfxGetStaticModuleState());

			CDataLinkDialog dia;
			dia.SetDialogMode(CDataLinkDialog::DialogMode::Pdc);
			dia.m_Callsign = fpCallsign;
			dia.m_Aircraft = FlightPlan.GetFlightPlanData().GetAircraftFPType();
			dia.m_Dest = FlightPlan.GetFlightPlanData().GetDestination();
			dia.m_From = FlightPlan.GetFlightPlanData().GetOrigin();
			dia.m_Departure = FlightPlan.GetFlightPlanData().GetSidName();
			dia.m_Rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			dia.m_SSR = FlightPlan.GetControllerAssignedData().GetSquawk();
			string freq = std::to_string(ControllerMyself().GetPrimaryFrequency());
			if (ControllerSelect(FlightPlan.GetCoordinatedNextController()).GetPrimaryFrequency() != 0)
				freq = std::to_string(ControllerSelect(FlightPlan.GetCoordinatedNextController()).GetPrimaryFrequency());
			freq = freq.substr(0, 7);
			dia.m_Freq = freq.c_str();
			AcarsMessage msg;
			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				auto msgIt = PendingMessages.find(fpCallsign);
				if (msgIt != PendingMessages.end())
					msg = msgIt->second;
			}
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

			DatalinkClearanceRequest request;
			request.credentials = SnapshotDatalinkCredentials();
			request.generation = HoppieConnectionGeneration.load(
				std::memory_order_acquire);
			request.packet.callsign = fpCallsign;
			request.packet.destination = FlightPlan.GetFlightPlanData().GetDestination();
			request.packet.rwy = FlightPlan.GetFlightPlanData().GetDepartureRwy();
			request.packet.sid = FlightPlan.GetFlightPlanData().GetSidName();
			request.packet.asat = static_cast<const char*>(dia.m_TSAT);
			request.packet.ctot = static_cast<const char*>(dia.m_CTOT);
			request.packet.freq = static_cast<const char*>(dia.m_Freq);
			request.packet.message = static_cast<const char*>(dia.m_Message);
			request.packet.squawk = FlightPlan.GetControllerAssignedData().GetSquawk();
			request.packet.climb = toReturn;
			request.fallbackFrequency =
				std::to_string(ControllerMyself().GetPrimaryFrequency()).substr(0, 7);
			request.messageSequence = messageId.fetch_add(1) + 1;
			if (!QueueNetworkJob([request]() { sendDatalinkClearance(request); }))
				Logger::info("CPDLC clearance could not be queued");

		}

	}
}

void CSMRPlugin::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (!FlightPlan.IsValid())
		return;

	const char* callsign = FlightPlan.GetCallsign();
	if (callsign == nullptr || callsign[0] == '\0')
		return;
	const std::string normalizedCallsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
	if (normalizedCallsign.empty())
		return;
	VsmrGroundState::ClearLineupOverride(normalizedCallsign.c_str());

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftCdmAutoTracked.erase(normalizedCallsign);
		RemoveQueuedCdmReminderUnlocked(normalizedCallsign);
		ClearDatalinkClearanceSentUnlocked(normalizedCallsign);
	}
}

void CSMRPlugin::OnNewMetarReceived(const char* sStation, const char* sFullMetar)
{
	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
		VsmrWeather::Update(sStation, sFullMetar);
}

void CSMRPlugin::QueueWeatherFetch(const std::string& rawStation)
{
	const std::string station = VsmrWeather::NormalizeIcao(rawStation);
	if (station.empty() || PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	VsmrWeather::Snapshot snapshot;
	const bool hasSnapshot = VsmrWeather::TryGet(station, snapshot);
	const std::time_t now = std::time(nullptr);
	const std::time_t refreshInterval = hasSnapshot ? 5 * 60 : 60;

	std::lock_guard<std::mutex> guard(WeatherFetchMutex);
	if (WeatherFetchStop || WeatherFetchQueued.find(station) != WeatherFetchQueued.end())
		return;

	const auto lastAttempt = WeatherLastAttemptUtc.find(station);
	if (lastAttempt != WeatherLastAttemptUtc.end() &&
		now >= lastAttempt->second && now - lastAttempt->second < refreshInterval)
	{
		return;
	}

	if (!WeatherFetchThread.joinable())
	{
		try
		{
			WeatherFetchThread = std::thread(&CSMRPlugin::WeatherFetchThreadMain, this);
		}
		catch (const std::system_error&)
		{
			WeatherLastAttemptUtc[station] = now;
			return;
		}
	}

	WeatherLastAttemptUtc[station] = now;
	WeatherFetchQueued.insert(station);
	WeatherFetchQueue.push_back(station);
	WeatherFetchCondition.notify_one();
}

void CSMRPlugin::WeatherFetchThreadMain()
{
	try
	{
		for (;;)
		{
			std::string station;
			{
				std::unique_lock<std::mutex> lock(WeatherFetchMutex);
				WeatherFetchCondition.wait(lock, [this]() {
					return WeatherFetchStop || !WeatherFetchQueue.empty();
					});
				if (WeatherFetchStop)
					return;

				station = std::move(WeatherFetchQueue.front());
				WeatherFetchQueue.pop_front();
				WeatherFetchQueued.erase(station);
			}

			try
			{
				const std::time_t requestStartedUtc = std::time(nullptr);
				const std::string url =
					"https://metar.vatsim.net/metar.php?id=" + station;
				const std::string report = GetHttpHelper().downloadStringFromURL(
					url,
					3500,
					&WeatherFetchCancellationRequested,
					WeatherResponseLimitBytes);
				if (!PluginShutdownRequested.load(std::memory_order_relaxed) &&
					!report.empty() && report.size() <= WeatherResponseLimitBytes)
				{
					VsmrWeather::Update(station, report, requestStartedUtc, true);
				}
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Weather fetch exception: " +
					std::string(ex.what()));
			}
			catch (...)
			{
				Logger::info("Weather fetch exception: unknown");
			}
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Weather worker terminated by exception: " +
			std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("Weather worker terminated by unknown exception");
	}
}

void CSMRPlugin::OnTimer(int Counter)
{
	(void)Counter;
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	BLINK = !BLINK;
	VsmrRdf::OnTimer();
	static int lastConnectionType = -999;
	static clock_t lastConnectionTypeChangeClock = 0;
	const int currentConnectionType = GetConnectionType();
	if (currentConnectionType != lastConnectionType)
	{
		Logger::info("EuroScope connection_type=" + std::to_string(currentConnectionType));
		lastConnectionType = currentConnectionType;
		lastConnectionTypeChangeClock = clock();
	}

	const unsigned long pendingVacdmSehCode = VacdmLastSehCode.exchange(0, std::memory_order_relaxed);
	if (pendingVacdmSehCode != 0)
		Logger::info("VACDM refresh SEH exception code=" + FormatSehCode(pendingVacdmSehCode));

	{
		std::string aselCallsign;
		const CFlightPlan aselFlightPlan = FlightPlanSelectASEL();
		if (aselFlightPlan.IsValid())
		{
			const char* callsign = aselFlightPlan.GetCallsign();
			if (callsign != NULL)
				aselCallsign = ToUpperAsciiCopy(callsign);
		}

		std::lock_guard<std::mutex> stateGuard(VacdmDebugStateMutex);
		VacdmDebugAselCallsign = aselCallsign;
	}

	if (HoppieConnected.load() && ConnectionMessage.load()) {
		DisplayUserMessage("CPDLC", "Server", "Logged in!", true, true, false, true, false);
		ConnectionMessage.store(false);
	}

	if (FailedToConnectMessage.load()) {
		std::string failureMessage = GetDatalinkStatusMessageCopy();
		if (failureMessage.empty())
			failureMessage = "Could not connect to Hoppie.";
		DisplayUserMessage("CPDLC", "Server", failureMessage.c_str(), true, true, false, true, false);
		FailedToConnectMessage.store(false);
	}

	if ((HoppieConnected.load(std::memory_order_acquire) ||
		HoppieConnecting.load(std::memory_order_acquire)) &&
		GetConnectionType() == CONNECTION_TYPE_NO) {
		std::string disconnectError;
		DisconnectDatalink(disconnectError);
		SetDatalinkStatusMessage("Automatically disconnected because EuroScope is offline.");
		DisplayUserMessage("CPDLC", "Server", "Automatically logged off!", true, true, false, true, false);
	}

	if (!PluginShutdownRequested.load(std::memory_order_relaxed) &&
		((clock() - timer) / CLOCKS_PER_SEC) > 10 &&
		HoppieConnected.load()) {
		std::string pollError;
		StartDatalinkPoll(false, pollError);
		timer = clock();
	}

	const bool networkConnectionActive = (currentConnectionType != CONNECTION_TYPE_NO);
	const bool vacdmPollingEnabled = VacdmPollingEnabled.load(std::memory_order_relaxed);
	const bool connectionStableForVacdm = networkConnectionActive &&
		(lastConnectionTypeChangeClock == 0 || ((clock() - lastConnectionTypeChangeClock) / CLOCKS_PER_SEC) >= 20);

	const clock_t lastVacdmFetchClock = VacdmLastFetchClock.load();
	if (vacdmPollingEnabled &&
		!PluginShutdownRequested.load(std::memory_order_relaxed) &&
		connectionStableForVacdm &&
		(lastVacdmFetchClock == 0 || ((clock() - lastVacdmFetchClock) / CLOCKS_PER_SEC) >= VacdmFetchIntervalSeconds) &&
		!VacdmFetchInProgress.load())
	{
		bool expected = false;
		if (VacdmFetchInProgress.compare_exchange_strong(expected, true))
		{
			if (PluginShutdownRequested.load(std::memory_order_relaxed))
			{
				VacdmFetchInProgress.store(false);
				VacdmLastFetchClock = clock();
			}
			else
			{
				if (!QueueNetworkJob([]() { refreshVacdmData(); }))
				{
					VacdmFetchInProgress.store(false);
					VacdmLastFetchClock = clock();
					Logger::info("VACDM refresh could not be queued");
				}
			}
		}
	}

	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		ProcessCdmAutoMode(this);
		ProcessQueuedCdmReminderMessages(this);
	}

	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		AircraftWilco.erase(
			std::remove_if(
				AircraftWilco.begin(),
				AircraftWilco.end(),
				[&](const std::string& callsign)
				{
					CRadarTarget radarTarget = RadarTargetSelect(callsign.c_str());
					return radarTarget.IsValid() && radarTarget.GetGS() > 160;
				}),
			AircraftWilco.end());
	}

	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	bool timerAlarmDue = false;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->IsShutdownRequested())
			continue;
		bool refresh = false;
		const auto weatherDisplay = radar->appWindowDisplays.find(weatherWindowId);
		if (weatherDisplay != radar->appWindowDisplays.end() && weatherDisplay->second)
		{
			QueueWeatherFetch(radar->getActiveAirport());
			refresh = true;
		}
		const auto timerDisplay = radar->appWindowDisplays.find(timerWindowId);
		const auto timerWindow = radar->appWindows.find(timerWindowId);
		if (timerWindow != radar->appWindows.end() && timerWindow->second != nullptr &&
			timerWindow->second->UpdateTimerCountdowns())
		{
			timerAlarmDue = true;
		}
		if (timerDisplay != radar->appWindowDisplays.end() && timerDisplay->second)
			refresh = true;
		if (refresh)
			radar->RequestRefresh();
	}
	if (timerAlarmDue && !PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		PlayRuntimeAudio(L"Alarm.wav", "Timer alarm");
	}
};

CRadarScreen * CSMRPlugin::OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated)
{
	(void)NeedRadarContent;
	(void)GeoReferenced;
	(void)CanBeSaved;
	(void)CanBeCreated;
	Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
		return NULL;

	if (sDisplayName != nullptr && !strcmp(sDisplayName, MY_PLUGIN_VIEW_AVISO)) {
		CSMRRadar* rd = new CSMRRadar();
		RadarScreensOpened.push_back(rd);
		return rd;
	}

	return NULL;
}

//---EuroScopePlugInExit-----------------------------------------------

void __declspec (dllexport) EuroScopePlugInExit(void)
{
	CSMRPlugin* pluginInstance = ActivePluginInstance.exchange(
		nullptr,
		std::memory_order_acq_rel);
	PluginShutdownRequested.store(true, std::memory_order_relaxed);
	VsmrGroundState::ClearAllLineupOverrides();
	HoppieConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppiePollGeneration.fetch_add(1, std::memory_order_acq_rel);
	HoppieConnected.store(false, std::memory_order_relaxed);
	HoppieConnecting.store(false, std::memory_order_relaxed);
	HoppiePollInProgress.store(false, std::memory_order_relaxed);
	VacdmPollingEnabled.store(false, std::memory_order_relaxed);
	VsmrRdf::Stop();
	if (pluginInstance != nullptr)
	{
		pluginInstance->StopWeatherFetchWorker();
		pluginInstance->StopNetworkWorkers();
	}

	const std::vector<CSMRRadar*> radarScreens = RadarScreensOpened;
	for (auto* var : radarScreens)
	{
		if (var != nullptr)
			var->EuroScopePlugInExitCustom();
	}
	VsmrWeather::Clear();
}
