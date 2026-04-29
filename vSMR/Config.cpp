#include "stdafx.h"
#include "Config.hpp"
#include <algorithm>

CConfig::CConfig(string configPath, string mapPath)
{
	config_path = configPath;
	map_path = mapPath;
	loadConfig();
	loadMap();

	setActiveProfile("Default");
}

void CConfig::reload()
{
	string activeName = getActiveProfileName();
	loadConfig();
	loadMap();
	if (!activeName.empty() && profiles.find(activeName) != profiles.end())
		setActiveProfile(activeName);
	else if (!profiles.empty())
		setActiveProfile(profiles.begin()->first);
}

vector<CConfig::mapData> CConfig::getMapElementsForZoomLevel(int zoomLevel)
{
	vector<CConfig::mapData> out;
	for (auto it = maps.begin(); it != maps.end(); ++it)
	{
		if (it->first <= zoomLevel)
		{
			out.insert(out.end(), it->second.begin(), it->second.end());
		}
	}
	return out;
}

void CConfig::loadConfig() {

	stringstream ss;
	ifstream ifs;
	ifs.open(config_path.c_str(), std::ios::binary);
	if (!ifs.is_open()) {
		document.SetArray();
		profiles.clear();
		active_profile = 0;
		return;
	}
	ss << ifs.rdbuf();
	ifs.close();

	if (document.Parse<0>(ss.str().c_str()).HasParseError()) {
		AfxMessageBox("An error parsing vSMR configuration occurred.\nOnce fixed, reload the config by typing '.smr reload'", MB_OK);
	
		ASSERT(AfxGetMainWnd() != NULL);
		AfxGetMainWnd()->SendMessage(WM_CLOSE);
		document.SetArray();
		profiles.clear();
		active_profile = 0;
		return;
	}
	
	profiles.clear();

	if (!document.IsArray()) {
		document.SetArray();
		active_profile = 0;
		return;
	}

	for (SizeType i = 0; i < document.Size(); i++) {
		const Value& profile = document[i];
		if (!profile.IsObject() || !profile.HasMember("name") || !profile["name"].IsString())
			continue;
		string profile_name = profile["name"].GetString();
		profiles.insert(pair<string, rapidjson::SizeType>(profile_name, i));
	}
}

void CConfig::loadMap()
{
	maps.clear();

	stringstream ss;
	ifstream ifs(map_path.c_str(), std::ios::binary);
	if (!ifs) {
		mapDocument.SetArray();
		return; // no map defined
	}
	ss << ifs.rdbuf();
	ifs.close();

	if (mapDocument.Parse<0>(ss.str().c_str()).HasParseError()) {
		AfxMessageBox("An error parsing vSMR maps occurred.\nOnce fixed, reload the config by typing '.smr reload'", MB_OK);
	
		ASSERT(AfxGetMainWnd() != NULL);
		AfxGetMainWnd()->SendMessage(WM_CLOSE);
		return;
	}

	if (!mapDocument.IsArray())
		return;
	for (SizeType i = 0; i < mapDocument.Size(); i++) {
		const Value& map = mapDocument[i];
		int mapZoomLevel = map["zoomLevel"].GetInt();
		string element = map["element"].GetString();
		string active;
		if (map.HasMember("active"))
			active = map["active"].GetString();

		mapData data = { element, active };
		maps[mapZoomLevel].push_back(data);
	}
}

const Value& CConfig::getActiveProfile() {
	if (document.IsArray() && !document.Empty())
	{
		if (active_profile < document.Size())
			return document[active_profile];
		return document[static_cast<SizeType>(0)];
	}

	static const Value emptyProfile(kObjectType);
	return emptyProfile;
}

bool CConfig::isSidColorAvail(string sid, string airport) {
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("maps") || !activeProfile["maps"].IsObject())
		return false;

	const Value& maps = activeProfile["maps"];
	if (!maps.HasMember(airport.c_str()) || !maps[airport.c_str()].IsObject())
		return false;

	const Value& airportMap = maps[airport.c_str()];
	if (!airportMap.HasMember("sids") || !airportMap["sids"].IsArray())
		return false;

	const Value& sidDefinitions = airportMap["sids"];
	for (SizeType i = 0; i < sidDefinitions.Size(); i++)
	{
		const Value& sidDefinition = sidDefinitions[i];
		if (!sidDefinition.IsObject() || !sidDefinition.HasMember("names") || !sidDefinition["names"].IsArray())
			continue;

		const Value& sidNames = sidDefinition["names"];
		for (SizeType s = 0; s < sidNames.Size(); s++) {
			if (!sidNames[s].IsString())
				continue;
			string currentsid = sidNames[s].GetString();
			std::transform(currentsid.begin(), currentsid.end(), currentsid.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			if (startsWith(sid.c_str(), currentsid.c_str()))
				return true;
		}
	}
	return false;
}

Gdiplus::Color CConfig::getSidColor(string sid, string airport)
{
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("maps") || !activeProfile["maps"].IsObject())
		return Gdiplus::Color(0, 0, 0);

	const Value& maps = activeProfile["maps"];
	if (!maps.HasMember(airport.c_str()) || !maps[airport.c_str()].IsObject())
		return Gdiplus::Color(0, 0, 0);

	const Value& airportMap = maps[airport.c_str()];
	if (!airportMap.HasMember("sids") || !airportMap["sids"].IsArray())
		return Gdiplus::Color(0, 0, 0);

	const Value& sidDefinitions = airportMap["sids"];
	for (SizeType i = 0; i < sidDefinitions.Size(); i++)
	{
		const Value& sidDefinition = sidDefinitions[i];
		if (!sidDefinition.IsObject() || !sidDefinition.HasMember("names") || !sidDefinition["names"].IsArray())
			continue;

		const Value& sidNames = sidDefinition["names"];
		for (SizeType s = 0; s < sidNames.Size(); s++) {
			if (!sidNames[s].IsString())
				continue;
			string currentsid = sidNames[s].GetString();
			std::transform(currentsid.begin(), currentsid.end(), currentsid.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			if (startsWith(sid.c_str(), currentsid.c_str()))
			{
				if (sidDefinition.HasMember("color") && sidDefinition["color"].IsObject())
					return getConfigColor(sidDefinition["color"]);
				return Gdiplus::Color(0, 0, 0);
			}
		}
	}
	return Gdiplus::Color(0, 0, 0);
}

Gdiplus::Color CConfig::getConfigColor(const Value& config_path) {
	if (!config_path.IsObject())
		return Gdiplus::Color(255, 0, 0, 0);

	int r = (config_path.HasMember("r") && config_path["r"].IsInt()) ? config_path["r"].GetInt() : 0;
	int g = (config_path.HasMember("g") && config_path["g"].IsInt()) ? config_path["g"].GetInt() : 0;
	int b = (config_path.HasMember("b") && config_path["b"].IsInt()) ? config_path["b"].GetInt() : 0;
	int a = 255;
	if (config_path.HasMember("a") && config_path["a"].IsInt())
		a = config_path["a"].GetInt();

	r = std::clamp(r, 0, 255);
	g = std::clamp(g, 0, 255);
	b = std::clamp(b, 0, 255);
	a = std::clamp(a, 0, 255);

	Gdiplus::Color Color(a, r, g, b);
	return Color;
}

COLORREF CConfig::getConfigColorRef(const Value& config_path) {
	if (!config_path.IsObject())
		return RGB(0, 0, 0);

	int r = (config_path.HasMember("r") && config_path["r"].IsInt()) ? config_path["r"].GetInt() : 0;
	int g = (config_path.HasMember("g") && config_path["g"].IsInt()) ? config_path["g"].GetInt() : 0;
	int b = (config_path.HasMember("b") && config_path["b"].IsInt()) ? config_path["b"].GetInt() : 0;

	r = std::clamp(r, 0, 255);
	g = std::clamp(g, 0, 255);
	b = std::clamp(b, 0, 255);

	COLORREF Color(RGB(r, g, b));
	return Color;
}

const Value& CConfig::getAirportMapIfAny(string airport) {
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("maps") || !activeProfile["maps"].IsObject())
		return activeProfile;

	const Value& mapData = activeProfile["maps"];
	if (mapData.HasMember(airport.c_str()) && mapData[airport.c_str()].IsObject())
		return mapData[airport.c_str()];

	return activeProfile;
}

bool CConfig::isAirportMapAvail(string airport) {
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("maps") || !activeProfile["maps"].IsObject())
		return false;
	if (activeProfile["maps"].HasMember(airport.c_str()) && activeProfile["maps"][airport.c_str()].IsObject())
		return true;
	return false;
}

bool CConfig::isCustomCursorUsed() {
	const Value& activeProfile = getActiveProfile();
	if (activeProfile.IsObject() && activeProfile.HasMember("cursor") && activeProfile["cursor"].IsString())
		return strcmp(activeProfile["cursor"].GetString(), "Default") != 0;
	return true; // by default use custom one so we don't break compatibility for old json settings that don't have the entry
}

bool CConfig::isCustomRunwayAvail(string airport, string name1, string name2) {
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("maps") || !activeProfile["maps"].IsObject())
		return false;
	const Value& maps = activeProfile["maps"];
	if (!maps.HasMember(airport.c_str()) || !maps[airport.c_str()].IsObject())
		return false;
	const Value& airportMap = maps[airport.c_str()];
	if (!airportMap.HasMember("runways") || !airportMap["runways"].IsArray())
		return false;

	const Value& runways = airportMap["runways"];
	for (SizeType i = 0; i < runways.Size(); i++) {
		const Value& runway = runways[i];
		if (!runway.IsObject() || !runway.HasMember("runway_name") || !runway["runway_name"].IsString())
			continue;
		const char* runwayName = runway["runway_name"].GetString();
		if (startsWith(name1.c_str(), runwayName) || startsWith(name2.c_str(), runwayName))
			return true;
	}
	return false;
}

vector<string> CConfig::getAllProfiles() {
	vector<string> toR;

	if (document.IsArray()) {
		for (SizeType i = 0; i < document.Size(); i++) {
			const Value& profile = document[i];
			if (profile.IsObject() && profile.HasMember("name") && profile["name"].IsString()) {
				toR.push_back(profile["name"].GetString());
			}
		}
	}

	if (toR.empty()) {
		for (std::map<string, rapidjson::SizeType>::iterator it = profiles.begin(); it != profiles.end(); ++it)
		{
			toR.push_back(it->first);
		}
	}

	return toR;
}

bool CConfig::saveConfig()
{
	FILE* fp = nullptr;
	if (fopen_s(&fp, config_path.c_str(), "wb") != 0 || !fp)
		return false;

	rapidjson::FileStream os(fp);
	rapidjson::PrettyWriter<rapidjson::FileStream> writer(os);

	document.Accept(writer);

	fclose(fp);
	return true;
}

unordered_set<string> CConfig::getInactiveAlert()
{
	const Value& activeProfile = getActiveProfile();
	if (!activeProfile.IsObject() || !activeProfile.HasMember("rimcas") || !activeProfile["rimcas"].IsObject())
		return unordered_set<string>();

	const Value& rimcas = activeProfile["rimcas"];
	if (rimcas.HasMember("inactive_alerts") && rimcas["inactive_alerts"].IsArray()) {
		unordered_set<string> toR;
		const Value& inactiveAlerts = rimcas["inactive_alerts"];
		for (SizeType i = 0; i < inactiveAlerts.Size(); i++) {
			toR.insert(inactiveAlerts[i].GetString());
		}
		return toR;
	}
	return unordered_set<string>();
}

bool CConfig::setInactiveAlert(unordered_set<string> inactiveAlerts)
{
	if (!document.IsArray() || document.Empty() || active_profile >= document.Size() || !document[active_profile].IsObject())
		return false;

	Value& activeProfile = document[active_profile];
	if (!activeProfile.HasMember("rimcas") || !activeProfile["rimcas"].IsObject())
	{
		Value rimcasObject(kObjectType);
		activeProfile.RemoveMember("rimcas");
		activeProfile.AddMember("rimcas", rimcasObject, document.GetAllocator());
	}

	// Modify the document in memory
	Value& rimcas = activeProfile["rimcas"];
	Value inactiveAlertArray(rapidjson::kArrayType);
	for (const string& alert : inactiveAlerts) {
		Value alertValue;
		alertValue.SetString(alert.c_str(), static_cast<SizeType>(alert.length()), document.GetAllocator());
		inactiveAlertArray.PushBack(alertValue, document.GetAllocator());
	}
	rimcas.RemoveMember("inactive_alerts");
	rimcas.AddMember("inactive_alerts", inactiveAlertArray, document.GetAllocator());
	return true;
}

CConfig::~CConfig()
{
}
