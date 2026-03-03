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

std::mutex VacdmPilotsMutex;
std::map<std::string, VacdmPilotData> VacdmPilots;
std::atomic<bool> VacdmFetchInProgress(false);
clock_t VacdmLastFetchClock = 0;
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
	// NOTE: 'SaveDataToSettings()' doesn't actually write data anywhere in a file, contrary to what the name freaking suggests.
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
	if (startsWith(".smr connect", sCommandLine))
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
	else if (startsWith(".smr poll", sCommandLine))
	{
		if (HoppieConnected) {
			_beginthread(pollMessages, 0, NULL);
		}
		return true;
	}
	else if (strcmp(sCommandLine, ".smr reload") == 0) {
		for (auto rd : RadarScreensOpened) {
			if (rd != nullptr)
				rd->ReloadConfig();
		}
		DisplayUserMessage("vSMR", "Config", "Reloaded vSMR_Profiles.json", true, true, false, true, false);
		return true;
	}
	else if (strcmp(sCommandLine, ".smr log") == 0) {
		Logger::ENABLED = !Logger::ENABLED;
		return true;
	}
	else if (startsWith(".smr", sCommandLine))
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
	CRadarTarget rt = RadarTargetSelect(FlightPlan.GetCallsign());

	if (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()) != ReleasedTracks.end())
		ReleasedTracks.erase(std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()));

	if (std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()) != ManuallyCorrelated.end())
		ManuallyCorrelated.erase(std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), rt.GetSystemID()));
}

void CSMRPlugin::OnTimer(int Counter)
{
	Logger::info(string(__FUNCSIG__));
	BLINK = !BLINK;

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

	if ((VacdmLastFetchClock == 0 || ((clock() - VacdmLastFetchClock) / CLOCKS_PER_SEC) >= VacdmFetchIntervalSeconds) &&
		!VacdmFetchInProgress.load())
	{
		bool expected = false;
		if (VacdmFetchInProgress.compare_exchange_strong(expected, true))
			_beginthread(refreshVacdmData, 0, NULL);
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
	const std::vector<CSMRRadar*> radarScreens = RadarScreensOpened;
	RadarScreensOpened.clear();
	for (auto* var : radarScreens)
	{
		if (var != nullptr)
			var->EuroScopePlugInExitCustom();
	}
}
