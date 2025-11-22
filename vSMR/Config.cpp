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

vector<string> CConfig::getMapElementsForZoomLevel(int zoomLevel)
{
	vector<string> out;
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
	ss << ifs.rdbuf();
	ifs.close();

	if (document.Parse<0>(ss.str().c_str()).HasParseError()) {
		AfxMessageBox("An error parsing vSMR configuration occurred.\nOnce fixed, reload the config by typing '.smr reload'", MB_OK);
	
		ASSERT(AfxGetMainWnd() != NULL);
		AfxGetMainWnd()->SendMessage(WM_CLOSE);
	}
	
	profiles.clear();

	assert(document.IsArray());

	for (SizeType i = 0; i < document.Size(); i++) {
		const Value& profile = document[i];
		string profile_name = profile["name"].GetString();

		profiles.insert(pair<string, rapidjson::SizeType>(profile_name, i));
	}
}

void CConfig::loadMap()
{
	stringstream ss;
	ifstream ifs(map_path.c_str(), std::ios::binary);
	if (!ifs) {
		return; // no map defined
	}
	ss << ifs.rdbuf();
	ifs.close();

	if (mapDocument.Parse<0>(ss.str().c_str()).HasParseError()) {
		AfxMessageBox("An error parsing vSMR maps occurred.\nOnce fixed, reload the config by typing '.smr reload'", MB_OK);
	
		ASSERT(AfxGetMainWnd() != NULL);
		AfxGetMainWnd()->SendMessage(WM_CLOSE);
	}

	assert(mapDocument.IsArray());
	for (SizeType i = 0; i < mapDocument.Size(); i++) {
		const Value& map = mapDocument[i];
		int mapZoomLevel = map["zoomLevel"].GetInt();
		string element = map["element"].GetString();
		maps[mapZoomLevel].push_back(element);
	}
}

const Value& CConfig::getActiveProfile() {
	return document[active_profile];
}

bool CConfig::isSidColorAvail(string sid, string airport) {
	if (getActiveProfile().HasMember("maps"))
	{
		if (getActiveProfile()["maps"].HasMember(airport.c_str()))
		{
			if (getActiveProfile()["maps"][airport.c_str()].HasMember("sids") && getActiveProfile()["maps"][airport.c_str()]["sids"].IsArray())
			{
				const Value& SIDs = getActiveProfile()["maps"][airport.c_str()]["sids"];
				for (SizeType i = 0; i < SIDs.Size(); i++)
				{
					const Value& SIDNames = SIDs[i]["names"];
					for (SizeType s = 0; s < SIDNames.Size(); s++) {
						string currentsid = SIDNames[s].GetString();
						std::transform(currentsid.begin(), currentsid.end(), currentsid.begin(), ::toupper);
						if (startsWith(sid.c_str(), currentsid.c_str()))
						{
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

Gdiplus::Color CConfig::getSidColor(string sid, string airport)
{
	if (getActiveProfile().HasMember("maps"))
	{
		if (getActiveProfile()["maps"].HasMember(airport.c_str()))
		{
			if (getActiveProfile()["maps"][airport.c_str()].HasMember("sids") && getActiveProfile()["maps"][airport.c_str()]["sids"].IsArray())
			{
				const Value& SIDs = getActiveProfile()["maps"][airport.c_str()]["sids"];
				for (SizeType i = 0; i < SIDs.Size(); i++)
				{
					const Value& SIDNames = SIDs[i]["names"];
					for (SizeType s = 0; s < SIDNames.Size(); s++) {
						string currentsid = SIDNames[s].GetString();
						std::transform(currentsid.begin(), currentsid.end(), currentsid.begin(), ::toupper);
						if (startsWith(sid.c_str(), currentsid.c_str()))
						{
							return getConfigColor(SIDs[i]["color"]);
						}
					}
				}
			}
		}
	}
	return Gdiplus::Color(0, 0, 0);
}

Gdiplus::Color CConfig::getConfigColor(const Value& config_path) {
	int r = config_path["r"].GetInt();
	int g = config_path["g"].GetInt();
	int b = config_path["b"].GetInt();
	int a = 255;
	if (config_path.HasMember("a"))
		a = config_path["a"].GetInt();

	Gdiplus::Color Color(a, r, g, b);
	return Color;
}

COLORREF CConfig::getConfigColorRef(const Value& config_path) {
	int r = config_path["r"].GetInt();
	int g = config_path["g"].GetInt();
	int b = config_path["b"].GetInt();

	COLORREF Color(RGB(r, g, b));
	return Color;
}

const Value& CConfig::getAirportMapIfAny(string airport) {
	if (getActiveProfile().HasMember("maps")) {
		const Value& map_data = getActiveProfile()["maps"];
		if (map_data.HasMember(airport.c_str())) {
			const Value& airport_map = map_data[airport.c_str()];
			return airport_map;
		}
	}
	return getActiveProfile();
}

bool CConfig::isAirportMapAvail(string airport) {
	if (getActiveProfile().HasMember("maps")) {
		if (getActiveProfile()["maps"].HasMember(airport.c_str())) {
			return true;
		}
	}
	return false;
}

bool CConfig::isCustomCursorUsed() {
	if (getActiveProfile().HasMember("cursor")) {		
		if (strcmp(getActiveProfile()["cursor"].GetString(), "Default") == 0) {
			return false;
		}
	}
	return true; // by default use custom one so we don't break compatibility for old json settings that don't have the entry
}

bool CConfig::isCustomRunwayAvail(string airport, string name1, string name2) {
	if (getActiveProfile().HasMember("maps")) {
		if (getActiveProfile()["maps"].HasMember(airport.c_str())) {
			if (getActiveProfile()["maps"][airport.c_str()].HasMember("runways") 
				&& getActiveProfile()["maps"][airport.c_str()]["runways"].IsArray()) {
				const Value& Runways = getActiveProfile()["maps"][airport.c_str()]["runways"];
				for (SizeType i = 0; i < Runways.Size(); i++) {
					if (startsWith(name1.c_str(), Runways[i]["runway_name"].GetString()) ||
						startsWith(name2.c_str(), Runways[i]["runway_name"].GetString())) {
						return true;
					}
				}
			}
		}
	}
	return false;
}

vector<string> CConfig::getAllProfiles() {
	vector<string> toR;
	for (std::map<string, rapidjson::SizeType>::iterator it = profiles.begin(); it != profiles.end(); ++it)
	{
		toR.push_back(it->first);
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
	if (getActiveProfile()["rimcas"].HasMember("inactive_alerts")) {
		unordered_set<string> toR;
		const Value& inactiveAlerts = getActiveProfile()["rimcas"]["inactive_alerts"];
		for (SizeType i = 0; i < inactiveAlerts.Size(); i++) {
			toR.insert(inactiveAlerts[i].GetString());
		}
		return toR;
	}
	return unordered_set<string>();
}

bool CConfig::setInactiveAlert(unordered_set<string> inactiveAlerts)
{
	// Modify the document in memory
	Value& rimcas = const_cast<Value&>(getActiveProfile()["rimcas"]);
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