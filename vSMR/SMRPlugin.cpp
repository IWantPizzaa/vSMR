#include "stdafx.h"
#include "SMRPlugin.hpp"
#include <atomic>
#include <mutex>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <fstream>
#include "rapidjson/document.h"

bool Logger::ENABLED;
string Logger::DLL_PATH;
Logger::Mode Logger::CURRENT_MODE = Logger::Mode::Normal;

// CPDLC/Hoppie connection state shared between timer and worker threads.
std::atomic<bool> HoppieConnected(false);
std::atomic<bool> ConnectionMessage(false);
std::atomic<bool> FailedToConnectMessage(false);

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
// Guards all mutable CPDLC message state used by worker threads.
std::mutex DatalinkStateMutex;

string tmessage;
string tdest;
string ttype;

std::atomic<int> messageId(0);

clock_t timer;

string myfrequency;

map<string, string> vStrips_Stands;

bool startThreadvStrips = true;

using namespace SMRPluginSharedData;
char recv_buf[1024];

vector<CSMRRadar*> RadarScreensOpened;

// Snapshot cache of the latest vACDM pilot data keyed by normalized callsign.
std::mutex VacdmPilotsMutex;
std::map<std::string, VacdmPilotData> VacdmPilots;
std::atomic<bool> VacdmFetchInProgress(false);
std::atomic<clock_t> VacdmLastFetchClock(0);
const int VacdmFetchIntervalSeconds = 15;
const std::string VacdmPilotsUrlDefault = "https://app.vacdm.net/api/v1/pilots";
std::atomic<unsigned long> VacdmFetchCounter(0);
std::mutex VacdmDebugStateMutex;
std::string VacdmDebugAselCallsign;

namespace
{
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

	std::string BuildHoppieQueryPrefix(const std::string& destination, const std::string& type)
	{
		std::string url = baseUrlDatalink;
		url += "?logon=";
		url += logonCode;
		url += "&from=";
		url += logonCallsign;
		url += "&to=";
		url += destination;
		url += "&type=";
		url += type;
		return url;
	}

	void EncodeSpacesAsPercent20(std::string& text)
	{
		size_t startPos = 0;
		while ((startPos = text.find(' ', startPos)) != std::string::npos)
		{
			text.replace(startPos, 1, "%20");
			startPos += 3;
		}
	}

	bool TryReadVacdmServerUrl(std::string& outServerUrl)
	{
		outServerUrl.clear();
		if (Logger::DLL_PATH.empty())
			return false;

		std::ifstream input(Logger::DLL_PATH + "\\vacdm.txt");
		if (!input.is_open())
			return false;

		std::string line;
		while (std::getline(input, line))
		{
			if (line.size() >= 3 &&
				static_cast<unsigned char>(line[0]) == 0xEF &&
				static_cast<unsigned char>(line[1]) == 0xBB &&
				static_cast<unsigned char>(line[2]) == 0xBF)
			{
				line = line.substr(3);
			}

			const std::string trimmed = TrimAsciiWhitespaceCopy(line);
			if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
				continue;

			const size_t separator = trimmed.find('=');
			if (separator == std::string::npos)
				continue;

			const std::string key = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(trimmed.substr(0, separator)));
			if (key != "SERVER_URL")
				continue;

			std::string value = TrimAsciiWhitespaceCopy(trimmed.substr(separator + 1));
			if (value.size() >= 2 &&
				((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
			{
				value = value.substr(1, value.size() - 2);
			}

			while (!value.empty() && value.back() == '/')
				value.pop_back();
			if (value.empty())
				continue;

			outServerUrl = value;
			return true;
		}

		return false;
	}

	std::string ResolveVacdmPilotsUrl()
	{
		std::string serverUrl;
		if (!TryReadVacdmServerUrl(serverUrl))
			return VacdmPilotsUrlDefault;
		return serverUrl + "/api/v1/pilots";
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

void refreshVacdmData(void* arg)
{
	(void)arg;

	struct ResetFetchFlag
	{
		~ResetFetchFlag()
		{
			VacdmLastFetchClock = clock();
			VacdmFetchInProgress.store(false);
		}
	} reset;

	try
	{
		if (httpHelper == NULL)
			return;

		const std::string pilotsUrl = ResolveVacdmPilotsUrl();
		std::string raw = httpHelper->downloadStringFromURL(pilotsUrl);
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

		std::string aselCallsign;
		{
			std::lock_guard<std::mutex> stateGuard(VacdmDebugStateMutex);
			aselCallsign = VacdmDebugAselCallsign;
		}
		const size_t parsedPilotCount = parsedData.size();
		const bool aselFound = !aselCallsign.empty() && parsedData.find(aselCallsign) != parsedData.end();

		{
			std::lock_guard<std::mutex> guard(VacdmPilotsMutex);
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

void datalinkLogin(void * arg) {
	(void)arg;
	string raw;
	string url = BuildHoppieQueryPrefix("SERVER", "PING");
	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		HoppieConnected.store(true);
		ConnectionMessage.store(true);
	}
	else {
		FailedToConnectMessage.store(true);
	}
};

void sendDatalinkMessage(void * arg) {
	(void)arg;
	std::string localDest;
	std::string localType;
	std::string localMessage;
	std::string localCallsign;

	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		localDest = tdest;
		localType = ttype;
		localMessage = tmessage;
		localCallsign = DatalinkToSend.callsign;
	}

	string raw;
	string url = BuildHoppieQueryPrefix(localDest, localType);
	url += "&packet=";
	url += localMessage;

	EncodeSpacesAsPercent20(url);

	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		PendingMessages.erase(localCallsign);
		RemoveCallsignUnlocked(AircraftMessage, localCallsign);
		AddCallsignUniqueUnlocked(AircraftMessageSent, localCallsign);
	}
};

void pollMessages(void * arg) {
	(void)arg;
	string raw = "";
	string url = BuildHoppieQueryPrefix("SERVER", "POLL");
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
					{
						std::lock_guard<std::mutex> guard(DatalinkStateMutex);
						tmessage = "UNABLE";
						ttype = "CPDLC";
						tdest = message.from;
					}
					_beginthread(sendDatalinkMessage, 0, NULL);
				} else {
					if (PlaySoundClr) {
						AFX_MANAGE_STATE(AfxGetStaticModuleState());
						PlaySound(MAKEINTRESOURCE(IDR_WAVE1), AfxGetInstanceHandle(), SND_RESOURCE | SND_ASYNC);
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


};

void sendDatalinkClearance(void * arg) {
	(void)arg;
	DatalinkPacket packet;
	std::string localFrequency;
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		packet = DatalinkToSend;
		localFrequency = myfrequency;
	}

	string raw;
	string url = BuildHoppieQueryPrefix(packet.callsign, "CPDLC");
	url += "&packet=/data2/";
	const int messageSequence = messageId.fetch_add(1) + 1;
	url += std::to_string(messageSequence);
	url += "//R/";
	url += "CLR TO @";
	url += packet.destination;
	url += "@ RWY @";
	url += packet.rwy;
	url += "@ DEP @";
	url += packet.sid;
	url += "@ INIT CLB @";
	url += packet.climb;
	url += "@ SQUAWK @";
	url += packet.squawk;
	url += "@ ";
	if (packet.ctot != "no" && packet.ctot.size() > 3) {
		url += "CTOT @";
		url += packet.ctot;
		url += "@ ";
	}
	if (packet.asat != "no" && packet.asat.size() > 3) {
		url += "TSAT @";
		url += packet.asat;
		url += "@ ";
	}
	if (packet.freq != "no" && packet.freq.size() > 5) {
		url += "WHEN RDY CALL FREQ @";
		url += packet.freq;
		url += "@";
	}
	else {
		url += "WHEN RDY CALL @";
		url += localFrequency;
		url += "@";
	}
	url += " IF UNABLE CALL VOICE ";
	if (packet.message != "no" && packet.message.size() > 1)
		url += packet.message;

	EncodeSpacesAsPercent20(url);

	raw.assign(httpHelper->downloadStringFromURL(url));

	if (startsWith("ok", raw.c_str())) {
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		RemoveCallsignUnlocked(AircraftDemandingClearance, packet.callsign);
		RemoveCallsignUnlocked(AircraftStandby, packet.callsign);
		PendingMessages.erase(packet.callsign);
		AddCallsignUniqueUnlocked(AircraftMessageSent, packet.callsign);
	}
};

CSMRPlugin::CSMRPlugin(void) :CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, MY_PLUGIN_NAME, MY_PLUGIN_VERSION, MY_PLUGIN_DEVELOPER, MY_PLUGIN_COPYRIGHT)
{

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

	if (httpHelper == NULL)
		httpHelper = new HttpHelper();

	const char * p_value;

	if ((p_value = GetDataFromSettings("cpdlc_logon")) != NULL)
		logonCallsign = p_value;
	if ((p_value = GetDataFromSettings("cpdlc_password")) != NULL)
		logonCode = p_value;
	if ((p_value = GetDataFromSettings("cpdlc_sound")) != NULL)
		PlaySoundClr = bool(!!atoi(p_value));

	char DllPathFile[_MAX_PATH];
	string DllPath;

	GetModuleFileNameA(HINSTANCE(&__ImageBase), DllPathFile, sizeof(DllPathFile));
	DllPath = DllPathFile;
	DllPath.resize(DllPath.size() - strlen("vSMR.dll"));
	Logger::DLL_PATH = DllPath;
}

CSMRPlugin::~CSMRPlugin()
{
	// Persist CPDLC settings via EuroScope's plugin settings storage.
	SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", logonCallsign.c_str());
	SaveDataToSettings("cpdlc_password", "The CPDLC logon password", logonCode.c_str());
	int temp = 0;
	if (PlaySoundClr)
		temp = 1;
	SaveDataToSettings("cpdlc_sound", "Play sound on clearance request", std::to_string(temp).c_str());

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
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
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

	if (startsWithCommand(".smr connect"))
	{
		if (ControllerMyself().IsController()) {
			if (!HoppieConnected.load()) {
				_beginthread(datalinkLogin, 0, NULL);
			}
			else {
				HoppieConnected.store(false);
				DisplayUserMessage("CPDLC", "Server", "Logged off!", true, true, false, true, false);
			}
		}
		else {
			DisplayUserMessage("CPDLC", "Error", "You are not logged in as a controller!", true, true, false, true, false);
		}

		return true;
	}
	else if (startsWithCommand(".smr poll"))
	{
		if (HoppieConnected.load()) {
			_beginthread(pollMessages, 0, NULL);
		}
		return true;
	}
	else if (commandLower == ".smr reload") {
		for (auto rd : RadarScreensOpened) {
			if (rd != nullptr)
				rd->ReloadConfig();
		}
		DisplayUserMessage("vSMR", "Config", "Reloaded vSMR_Profiles.json", true, true, false, true, false);
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
	else if (commandLower == ".smr profile" || commandLower == ".smr editor" || commandLower == ".smr config")
	{
		bool opened = false;
		for (auto* rd : RadarScreensOpened)
		{
			if (rd == nullptr)
				continue;
			rd->OpenProfileEditorWindow();
			opened = true;
			break;
		}

		if (!opened)
		{
			DisplayUserMessage("vSMR", "Config", "No active SMR radar screen found to open the config window.", true, true, false, true, false);
		}
		return true;
	}
	else if (startsWithCommand(".smr"))
	{
		auto applyCpdlcDialogValues = [&](const CCPDLCSettingsDialog& dialog)
		{
			logonCallsign = dialog.m_Logon;
			logonCode = dialog.m_Password;
			PlaySoundClr = (dialog.m_Sound != 0);
		};

		CCPDLCSettingsDialog dia(AfxGetMainWnd());
		dia.m_Logon = logonCallsign.c_str();
		dia.m_Password = logonCode.c_str();
		dia.m_Sound = int(PlaySoundClr);

		INT_PTR dialogResult = dia.DoModal();
		if (dialogResult == -1)
		{
			CCPDLCSettingsDialog diaNoParent(nullptr);
			diaNoParent.m_Logon = logonCallsign.c_str();
			diaNoParent.m_Password = logonCode.c_str();
			diaNoParent.m_Sound = int(PlaySoundClr);
			dialogResult = diaNoParent.DoModal();
			if (dialogResult == IDOK)
			{
				applyCpdlcDialogValues(diaNoParent);
			}
		}
		else if (dialogResult == IDOK)
		{
			applyCpdlcDialogValues(dia);
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

		SaveDataToSettings("cpdlc_logon", "The CPDLC logon callsign", logonCallsign.c_str());
		SaveDataToSettings("cpdlc_password", "The CPDLC logon password", logonCode.c_str());
		SaveDataToSettings("cpdlc_sound", "Play sound on clearance request", std::to_string(PlaySoundClr ? 1 : 0).c_str());

		return true;
	}
	return false;
}

void CSMRPlugin::OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int * pColorCode, COLORREF * pRGB, double * pFontSize) {
	Logger::info(string(__FUNCSIG__));
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
	Logger::info(string(__FUNCSIG__));
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
				DatalinkToSend.callsign = fpCallsign;
				tmessage = "STANDBY";
				ttype = "CPDLC";
				tdest = fpCallsign;
			}
			_beginthread(sendDatalinkMessage, 0, NULL);
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

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				DatalinkToSend.callsign = fpCallsign;
				tmessage = dia.m_Message;
				ttype = "TELEX";
				tdest = fpCallsign;
			}
			_beginthread(sendDatalinkMessage, 0, NULL);
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
				DatalinkToSend.callsign = fpCallsign;
				tmessage = "UNABLE CALL ON FREQ";
				ttype = "CPDLC";
				tdest = fpCallsign;

				RemoveCallsignUnlocked(AircraftDemandingClearance, fpCallsign);
				RemoveCallsignUnlocked(AircraftStandby, fpCallsign);
				PendingMessages.erase(fpCallsign);
			}

			_beginthread(sendDatalinkMessage, 0, NULL);
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

			{
				std::lock_guard<std::mutex> guard(DatalinkStateMutex);
				DatalinkToSend.callsign = fpCallsign;
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
			}

			_beginthread(sendDatalinkClearance, 0, NULL);

		}

	}
}

void CSMRPlugin::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	Logger::info(string(__FUNCSIG__));
	if (!FlightPlan.IsValid())
		return;

	const char* callsign = FlightPlan.GetCallsign();
	if (callsign == nullptr || callsign[0] == '\0')
		return;

	CRadarTarget rt = RadarTargetSelect(callsign);
	const char* systemId = rt.IsValid() ? rt.GetSystemID() : nullptr;
	if (systemId == nullptr || systemId[0] == '\0')
		return;

	auto releasedIt = std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId);
	if (releasedIt != ReleasedTracks.end())
		ReleasedTracks.erase(releasedIt);

	auto correlatedIt = std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), systemId);
	if (correlatedIt != ManuallyCorrelated.end())
		ManuallyCorrelated.erase(correlatedIt);
}

void CSMRPlugin::OnTimer(int Counter)
{
	(void)Counter;
	Logger::info(string(__FUNCSIG__));
	BLINK = !BLINK;
	static int lastConnectionType = -999;
	static clock_t lastConnectionTypeChangeClock = 0;
	const int currentConnectionType = GetConnectionType();
	if (currentConnectionType != lastConnectionType)
	{
		Logger::info("EuroScope connection_type=" + std::to_string(currentConnectionType));
		lastConnectionType = currentConnectionType;
		lastConnectionTypeChangeClock = clock();
	}

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
		DisplayUserMessage("CPDLC", "Server", "Could not login! Callsign probably in use.", true, true, false, true, false);
		FailedToConnectMessage.store(false);
	}

	if (HoppieConnected.load() && GetConnectionType() == CONNECTION_TYPE_NO) {
		DisplayUserMessage("CPDLC", "Server", "Automatically logged off!", true, true, false, true, false);
		HoppieConnected.store(false);
	}

	const int cpdlcPollIntervalSeconds = 10;
	if (((clock() - timer) / CLOCKS_PER_SEC) > cpdlcPollIntervalSeconds && HoppieConnected.load()) {
		_beginthread(pollMessages, 0, NULL);
		timer = clock();
	}

	// Avoid scheduling fetches while the connection state is still settling.
	const int vacdmConnectionSettleSeconds = 20;
	const bool networkConnectionActive = (currentConnectionType != CONNECTION_TYPE_NO);
	const bool connectionStableForVacdm = networkConnectionActive &&
		(lastConnectionTypeChangeClock == 0 || ((clock() - lastConnectionTypeChangeClock) / CLOCKS_PER_SEC) >= vacdmConnectionSettleSeconds);

	const clock_t lastVacdmFetchClock = VacdmLastFetchClock.load();
	if (connectionStableForVacdm &&
		(lastVacdmFetchClock == 0 || ((clock() - lastVacdmFetchClock) / CLOCKS_PER_SEC) >= VacdmFetchIntervalSeconds) &&
		!VacdmFetchInProgress.load())
	{
		bool expected = false;
		if (VacdmFetchInProgress.compare_exchange_strong(expected, true))
			_beginthread(refreshVacdmData, 0, NULL);
	}

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
};

CRadarScreen * CSMRPlugin::OnRadarScreenCreated(const char * sDisplayName, bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated)
{
	Logger::info(string(__FUNCSIG__));
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
	const std::vector<CSMRRadar*> radarScreens = RadarScreensOpened;
	RadarScreensOpened.clear();
	for (auto* var : radarScreens)
	{
		if (var != nullptr)
			var->EuroScopePlugInExitCustom();
	}
}
