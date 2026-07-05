#include "stdafx.h"
#include "Config.hpp"
#include <algorithm>

namespace
{
	const char* kMetadataWrapperKey = "_vsmr";
	const char* kMetadataSchemaVersionKey = "schema_version";
	const char* kLastActiveProfileKey = "last_active_profile";
	const char* kVacdmKey = "vacdm";
	const char* kVacdmServerUrlKey = "server_url";

	const rapidjson::Value* GetObjectMemberIfPresent(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsObject())
			return nullptr;
		return &object[key];
	}

	bool IsProfileEntry(const rapidjson::Value& value)
	{
		return value.IsObject() && value.HasMember("name") && value["name"].IsString();
	}

	bool IsMetadataEntry(const rapidjson::Value& value)
	{
		return value.IsObject() &&
			value.HasMember(kMetadataWrapperKey) &&
			value[kMetadataWrapperKey].IsObject() &&
			!value.HasMember("name");
	}

	std::string ReadStringMember(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsString())
			return "";
		return object[key].GetString();
	}

	void SetStringMember(rapidjson::Value& object, const char* key, const std::string& value, rapidjson::Document::AllocatorType& allocator)
	{
		if (!object.IsObject() || key == nullptr)
			return;

		rapidjson::Value stringValue;
		stringValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
		if (object.HasMember(key))
			object[key] = stringValue;
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			object.AddMember(keyValue, stringValue, allocator);
		}
	}

	rapidjson::Value& EnsureObjectMember(rapidjson::Value& object, const char* key, rapidjson::Document::AllocatorType& allocator)
	{
		if (!object.IsObject())
			object.SetObject();

		if (!object.HasMember(key) || !object[key].IsObject())
		{
			object.RemoveMember(key);
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value newObject(rapidjson::kObjectType);
			object.AddMember(keyValue, newObject, allocator);
		}

		return object[key];
	}

	int ReadColorComponent(const rapidjson::Value& colorValue, const char* key, int fallback = 0)
	{
		if (!colorValue.IsObject() || key == nullptr || !colorValue.HasMember(key) || !colorValue[key].IsInt())
			return fallback;
		return std::clamp(colorValue[key].GetInt(), 0, 255);
	}
}

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
		if (!IsProfileEntry(profile))
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
	if (document.IsArray())
	{
		if (active_profile < document.Size() && IsProfileEntry(document[active_profile]))
			return document[active_profile];

		if (!profiles.empty())
			return document[profiles.begin()->second];
	}

	static const Value emptyProfile(kObjectType);
	return emptyProfile;
}

const Value* CConfig::findSidDefinition(const string& sid, const string& airport)
{
	const Value& activeProfile = getActiveProfile();
	const Value* mapsObject = GetObjectMemberIfPresent(activeProfile, "maps");
	if (mapsObject == nullptr)
		return nullptr;

	const Value* airportMap = GetObjectMemberIfPresent(*mapsObject, airport.c_str());
	if (airportMap == nullptr || !airportMap->HasMember("sids") || !(*airportMap)["sids"].IsArray())
		return nullptr;

	const Value& sidDefinitions = (*airportMap)["sids"];
	for (SizeType i = 0; i < sidDefinitions.Size(); i++)
	{
		const Value& sidDefinition = sidDefinitions[i];
		if (!sidDefinition.IsObject() || !sidDefinition.HasMember("names") || !sidDefinition["names"].IsArray())
			continue;

		const Value& sidNames = sidDefinition["names"];
		for (SizeType s = 0; s < sidNames.Size(); s++) {
			if (!sidNames[s].IsString())
				continue;

			string currentSid = sidNames[s].GetString();
			std::transform(currentSid.begin(), currentSid.end(), currentSid.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			if (startsWith(sid.c_str(), currentSid.c_str()))
				return &sidDefinition;
		}
	}

	return nullptr;
}

bool CConfig::isSidColorAvail(string sid, string airport) {
	return findSidDefinition(sid, airport) != nullptr;
}

Gdiplus::Color CConfig::getSidColor(string sid, string airport)
{
	const Value* sidDefinition = findSidDefinition(sid, airport);
	if (sidDefinition != nullptr && sidDefinition->HasMember("color") && (*sidDefinition)["color"].IsObject())
		return getConfigColor((*sidDefinition)["color"]);
	return Gdiplus::Color(0, 0, 0);
}

Gdiplus::Color CConfig::getConfigColor(const Value& config_path) {
	if (!config_path.IsObject())
		return Gdiplus::Color(255, 0, 0, 0);

	const int r = ReadColorComponent(config_path, "r");
	const int g = ReadColorComponent(config_path, "g");
	const int b = ReadColorComponent(config_path, "b");
	const int a = ReadColorComponent(config_path, "a", 255);

	Gdiplus::Color Color(a, r, g, b);
	return Color;
}

COLORREF CConfig::getConfigColorRef(const Value& config_path) {
	if (!config_path.IsObject())
		return RGB(0, 0, 0);

	const int r = ReadColorComponent(config_path, "r");
	const int g = ReadColorComponent(config_path, "g");
	const int b = ReadColorComponent(config_path, "b");

	COLORREF Color(RGB(r, g, b));
	return Color;
}

const Value& CConfig::getAirportMapIfAny(string airport) {
	const Value& activeProfile = getActiveProfile();
	const Value* mapData = GetObjectMemberIfPresent(activeProfile, "maps");
	if (mapData == nullptr)
		return activeProfile;

	const Value* airportMap = GetObjectMemberIfPresent(*mapData, airport.c_str());
	if (airportMap != nullptr)
		return *airportMap;

	return activeProfile;
}

bool CConfig::isAirportMapAvail(string airport) {
	const Value& activeProfile = getActiveProfile();
	const Value* mapData = GetObjectMemberIfPresent(activeProfile, "maps");
	return mapData != nullptr && GetObjectMemberIfPresent(*mapData, airport.c_str()) != nullptr;
}

bool CConfig::isCustomCursorUsed() {
	const Value& activeProfile = getActiveProfile();
	if (activeProfile.IsObject() && activeProfile.HasMember("cursor") && activeProfile["cursor"].IsString())
		return strcmp(activeProfile["cursor"].GetString(), "Default") != 0;
	return true; // by default use custom one so we don't break compatibility for old json settings that don't have the entry
}

bool CConfig::isCustomRunwayAvail(string airport, string name1, string name2) {
	const Value& activeProfile = getActiveProfile();
	const Value* maps = GetObjectMemberIfPresent(activeProfile, "maps");
	if (maps == nullptr)
		return false;
	const Value* airportMap = GetObjectMemberIfPresent(*maps, airport.c_str());
	if (airportMap == nullptr || !airportMap->HasMember("runways") || !(*airportMap)["runways"].IsArray())
		return false;

	const Value& runways = (*airportMap)["runways"];
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
			if (IsProfileEntry(profile)) {
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

size_t CConfig::getProfileCount() const
{
	return profiles.size();
}

const Value* CConfig::findMetadata() const
{
	if (!document.IsArray())
		return nullptr;

	for (SizeType i = 0; i < document.Size(); i++)
	{
		const Value& item = document[i];
		if (IsMetadataEntry(item))
			return &item[kMetadataWrapperKey];
	}

	return nullptr;
}

Value& CConfig::ensureMetadata()
{
	if (!document.IsArray())
		document.SetArray();

	for (SizeType i = 0; i < document.Size(); i++)
	{
		Value& item = document[i];
		if (IsMetadataEntry(item))
			return item[kMetadataWrapperKey];
	}

	Value wrapper(kObjectType);
	Value metadata(kObjectType);
	metadata.AddMember(kMetadataSchemaVersionKey, 1, document.GetAllocator());

	Value wrapperKey;
	wrapperKey.SetString(kMetadataWrapperKey, document.GetAllocator());
	wrapper.AddMember(wrapperKey, metadata, document.GetAllocator());
	document.PushBack(wrapper, document.GetAllocator());
	return document[document.Size() - 1][kMetadataWrapperKey];
}

string CConfig::getLastActiveProfileName() const
{
	const Value* metadata = findMetadata();
	if (metadata == nullptr)
		return "";
	return trimProfileName(ReadStringMember(*metadata, kLastActiveProfileKey));
}

bool CConfig::setLastActiveProfileName(const string& profileName)
{
	const string trimmedProfile = trimProfileName(profileName);
	if (trimmedProfile.empty())
		return false;

	Value& metadata = ensureMetadata();
	SetStringMember(metadata, kLastActiveProfileKey, trimmedProfile, document.GetAllocator());
	return true;
}

string CConfig::getVacdmServerUrl() const
{
	const Value* metadata = findMetadata();
	if (metadata == nullptr)
		return "";

	const Value* vacdm = GetObjectMemberIfPresent(*metadata, kVacdmKey);
	if (vacdm == nullptr)
		return "";
	return ReadStringMember(*vacdm, kVacdmServerUrlKey);
}

bool CConfig::setVacdmServerUrl(const string& serverUrl)
{
	Value& metadata = ensureMetadata();
	Value& vacdm = EnsureObjectMember(metadata, kVacdmKey, document.GetAllocator());
	SetStringMember(vacdm, kVacdmServerUrlKey, serverUrl, document.GetAllocator());
	return true;
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

bool CConfig::setInactiveAlert(const unordered_set<string>& inactiveAlerts)
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
