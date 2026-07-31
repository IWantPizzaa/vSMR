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
	const char* kBackupSuffix = ".bak";
	volatile LONG gTemporaryFileSequence = 0;

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
			rapidjson::Value newObject(rapidjson::kObjectType);
			if (object.HasMember(key))
				object[key] = newObject;
			else
			{
				rapidjson::Value keyValue;
				keyValue.SetString(key, allocator);
				object.AddMember(keyValue, newObject, allocator);
			}
		}

		return object[key];
	}

	int ReadColorComponent(const rapidjson::Value& colorValue, const char* key, int fallback = 0)
	{
		if (!colorValue.IsObject() || key == nullptr || !colorValue.HasMember(key) || !colorValue[key].IsInt())
			return fallback;
		return std::clamp(colorValue[key].GetInt(), 0, 255);
	}

	bool ReadFileContents(const std::string& path, std::string& contents)
	{
		std::ifstream input(path.c_str(), std::ios::binary);
		if (!input.is_open())
			return false;

		std::ostringstream stream;
		stream << input.rdbuf();
		if (input.bad())
			return false;

		input.close();
		if (input.fail())
			return false;

		contents = stream.str();
		return true;
	}

	void ReportLoadFailure(const char* fileDescription)
	{
		std::string message = "An error parsing vSMR ";
		message += fileDescription;
		message += " occurred.\nThe currently loaded data remains active.\nOnce fixed, reload the config by typing '.smr reload'";
		AfxMessageBox(message.c_str(), MB_OK | MB_ICONERROR);
	}

	void AdoptDocument(rapidjson::Document& destination, rapidjson::Document& source)
	{
		// Both documents use destination's allocator. Moving only the root value keeps
		// the public Document object stable while committing the validated replacement.
		static_cast<rapidjson::Value&>(destination) = static_cast<rapidjson::Value&>(source);
	}

	bool ParseValidatedArray(
		const std::string& serializedJson,
		rapidjson::Document& validationDocument)
	{
		validationDocument.Parse<0>(serializedJson.c_str());
		return !validationDocument.HasParseError() && validationDocument.IsArray();
	}

	bool BuildMapIndex(
		const rapidjson::Document& source,
		std::map<int, std::vector<CConfig::mapData>>& loadedMaps)
	{
		if (!source.IsArray())
			return false;

		for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
		{
			const rapidjson::Value& map = source[i];
			if (!map.IsObject() ||
				!map.HasMember("zoomLevel") || !map["zoomLevel"].IsInt() ||
				!map.HasMember("element") || !map["element"].IsString() ||
				(map.HasMember("active") && !map["active"].IsString()))
			{
				return false;
			}

			CConfig::mapData data;
			data.element = map["element"].GetString();
			if (map.HasMember("active"))
				data.active = map["active"].GetString();
			loadedMaps[map["zoomLevel"].GetInt()].push_back(data);
		}

		return true;
	}

	std::string BuildTemporaryPath(const std::string& destination, const char* purpose)
	{
		const LONG sequence = ::InterlockedIncrement(&gTemporaryFileSequence);
		std::ostringstream path;
		path << destination
			<< "." << purpose
			<< "." << ::GetCurrentProcessId()
			<< "." << ::GetTickCount()
			<< "." << sequence;
		return path.str();
	}

	bool WriteTemporaryFile(
		const std::string& destination,
		const std::string& contents,
		std::string& temporaryPath)
	{
		const int maximumAttempts = 128;
		HANDLE output = INVALID_HANDLE_VALUE;

		for (int attempt = 0; attempt < maximumAttempts; ++attempt)
		{
			temporaryPath = BuildTemporaryPath(destination, "tmp");
			output = ::CreateFileA(
				temporaryPath.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
				nullptr);
			if (output != INVALID_HANDLE_VALUE)
				break;

			const DWORD error = ::GetLastError();
			if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
			{
				temporaryPath.clear();
				return false;
			}
		}

		if (output == INVALID_HANDLE_VALUE)
		{
			temporaryPath.clear();
			return false;
		}

		bool succeeded = true;
		size_t offset = 0;
		while (offset < contents.size())
		{
			const size_t remaining = contents.size() - offset;
			const DWORD bytesToWrite = static_cast<DWORD>(
				remaining > 0x7fffffffULL ? 0x7fffffffULL : remaining);
			DWORD bytesWritten = 0;
			if (!::WriteFile(
				output,
				contents.data() + offset,
				bytesToWrite,
				&bytesWritten,
				nullptr) ||
				bytesWritten == 0)
			{
				succeeded = false;
				break;
			}
			offset += bytesWritten;
		}

		if (succeeded && !::FlushFileBuffers(output))
			succeeded = false;
		if (!::CloseHandle(output))
			succeeded = false;

		if (!succeeded)
		{
			::DeleteFileA(temporaryPath.c_str());
			temporaryPath.clear();
		}

		return succeeded;
	}

	bool BackupDestinationIfPresent(const std::string& destination)
	{
		const DWORD attributes = ::GetFileAttributesA(destination.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			const DWORD error = ::GetLastError();
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return false;

		const std::string backupPath = destination + kBackupSuffix;
		std::string temporaryBackupPath;
		const int maximumAttempts = 128;
		for (int attempt = 0; attempt < maximumAttempts; ++attempt)
		{
			temporaryBackupPath = BuildTemporaryPath(destination, "backup");
			if (::CopyFileA(destination.c_str(), temporaryBackupPath.c_str(), TRUE))
				break;

			const DWORD error = ::GetLastError();
			if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
				return false;
			temporaryBackupPath.clear();
		}

		if (temporaryBackupPath.empty())
			return false;

		if (!::MoveFileExA(
			temporaryBackupPath.c_str(),
			backupPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileA(temporaryBackupPath.c_str());
			return false;
		}

		return true;
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

bool CConfig::reload()
{
	string activeName = getActiveProfileName();
	const bool configLoaded = loadConfig();
	const bool mapLoaded = loadMap();
	if (!profiles.empty())
		setActiveProfile(activeName.empty() ? profiles.begin()->first : activeName);
	return configLoaded && mapLoaded;
}

bool CConfig::replaceInMemoryConfig(
	const Value& replacementDocument,
	const string& requestedActiveProfile,
	string& error)
{
	error.clear();
	if (!replacementDocument.IsArray())
	{
		error = "Profiles state must be an array.";
		return false;
	}

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	replacementDocument.Accept(writer);

	Document parsed(&document.GetAllocator());
	parsed.Parse<0>(buffer.GetString());
	if (parsed.HasParseError() || !parsed.IsArray())
	{
		error = "Profiles state could not be parsed.";
		return false;
	}

	map<string, rapidjson::SizeType> replacementProfiles;
	std::vector<string> normalizedNames;
	for (SizeType index = 0; index < parsed.Size(); ++index)
	{
		const Value& profile = parsed[index];
		if (!IsProfileEntry(profile))
			continue;

		const string name = trimProfileName(profile["name"].GetString());
		if (name.empty())
		{
			error = "Profile names cannot be empty.";
			return false;
		}

		string normalized = name;
		std::transform(
			normalized.begin(),
			normalized.end(),
			normalized.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		if (std::find(normalizedNames.begin(), normalizedNames.end(), normalized) != normalizedNames.end())
		{
			error = "Profile names must be unique.";
			return false;
		}
		normalizedNames.push_back(normalized);
		replacementProfiles[name] = index;
	}

	if (replacementProfiles.empty())
	{
		error = "At least one named profile is required.";
		return false;
	}

	AdoptDocument(document, parsed);
	profiles.swap(replacementProfiles);
	active_profile = profiles.begin()->second;
	setActiveProfile(
		trimProfileName(requestedActiveProfile).empty()
			? profiles.begin()->first
			: requestedActiveProfile);
	return true;
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

bool CConfig::loadConfig() {
	std::string serializedJson;
	if (!ReadFileContents(config_path, serializedJson))
	{
		if (document.IsNull())
			document.SetArray();
		return false;
	}

	Document validationDocument;
	if (!ParseValidatedArray(serializedJson, validationDocument))
	{
		ReportLoadFailure("configuration");
		if (document.IsNull())
			document.SetArray();
		return false;
	}

	map<string, rapidjson::SizeType> loadedProfiles;
	std::unordered_set<string> normalizedProfileNames;
	for (SizeType i = 0; i < validationDocument.Size(); i++) {
		const Value& profile = validationDocument[i];
		if (!IsProfileEntry(profile))
			continue;
		const string profileName = trimProfileName(profile["name"].GetString());
		string normalizedName = profileName;
		std::transform(
			normalizedName.begin(),
			normalizedName.end(),
			normalizedName.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		if (profileName.empty() ||
			!normalizedProfileNames.insert(normalizedName).second)
		{
			ReportLoadFailure("configuration");
			return false;
		}
		loadedProfiles.insert(pair<string, rapidjson::SizeType>(profileName, i));
	}
	if (loadedProfiles.empty())
	{
		ReportLoadFailure("configuration");
		return false;
	}

	Document replacement(&document.GetAllocator());
	replacement.Parse<0>(serializedJson.c_str());
	if (replacement.HasParseError() || !replacement.IsArray())
	{
		ReportLoadFailure("configuration");
		return false;
	}

	AdoptDocument(document, replacement);
	profiles.swap(loadedProfiles);
	active_profile = 0;
	return true;
}

bool CConfig::loadMap()
{
	std::string serializedJson;
	if (!ReadFileContents(map_path, serializedJson))
	{
		if (mapDocument.IsNull())
			mapDocument.SetArray();
		return true; // no map defined
	}

	Document validationDocument;
	std::map<int, std::vector<mapData>> loadedMaps;
	if (!ParseValidatedArray(serializedJson, validationDocument) ||
		!BuildMapIndex(validationDocument, loadedMaps))
	{
		ReportLoadFailure("maps");
		if (mapDocument.IsNull())
			mapDocument.SetArray();
		return false;
	}

	Document replacement(&mapDocument.GetAllocator());
	replacement.Parse<0>(serializedJson.c_str());
	if (replacement.HasParseError() || !replacement.IsArray())
	{
		ReportLoadFailure("maps");
		return false;
	}

	AdoptDocument(mapDocument, replacement);
	maps.swap(loadedMaps);
	return true;
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
	if (!document.IsArray())
		return false;

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	document.Accept(writer);

	const std::string serializedJson(buffer.GetString(), buffer.Size());
	Document validationDocument;
	if (!ParseValidatedArray(serializedJson, validationDocument))
		return false;

	std::string temporaryPath;
	if (!WriteTemporaryFile(config_path, serializedJson, temporaryPath))
		return false;

	std::string persistedJson;
	Document persistedValidationDocument;
	if (!ReadFileContents(temporaryPath, persistedJson) ||
		persistedJson != serializedJson ||
		!ParseValidatedArray(persistedJson, persistedValidationDocument))
	{
		::DeleteFileA(temporaryPath.c_str());
		return false;
	}

	if (!BackupDestinationIfPresent(config_path))
	{
		::DeleteFileA(temporaryPath.c_str());
		return false;
	}

	if (!::MoveFileExA(
		temporaryPath.c_str(),
		config_path.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		::DeleteFileA(temporaryPath.c_str());
		return false;
	}

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
			if (inactiveAlerts[i].IsString())
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
		if (activeProfile.HasMember("rimcas"))
			activeProfile["rimcas"] = rimcasObject;
		else
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
	if (rimcas.HasMember("inactive_alerts"))
		rimcas["inactive_alerts"] = inactiveAlertArray;
	else
		rimcas.AddMember("inactive_alerts", inactiveAlertArray, document.GetAllocator());
	return true;
}

CConfig::~CConfig()
{
}
