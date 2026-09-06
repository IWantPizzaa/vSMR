#include "platform/windows/PrecompiledHeader.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/RuntimeConfig.Internal.hpp"
#include "shared/JsonInputLimits.hpp"
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <mutex>

using namespace VsmrRuntimeConfigInternal;

bool CConfig::validateSerializedInputLimits(
	const string& serializedJson,
	string& error)
{
	error.clear();
	return ValidateJsonInputLimits(serializedJson, &error);
}

bool CConfig::validateAndMigrateProfilesDocument(
	rapidjson::Document& profilesDocument,
	string& error,
	bool& migrated)
{
	error.clear();
	migrated = false;
	// Public DOM callers may not have passed through serialized input validation.
	if (!ValidateJsonDocumentLimits(profilesDocument, &error))
		return false;
	return validateAndMigratePrevalidatedProfilesDocument(
		profilesDocument,
		error,
		migrated);
}

bool CConfig::validateAndMigratePrevalidatedProfilesDocument(
	rapidjson::Document& profilesDocument,
	string& error,
	bool& migrated)
{
	error.clear();
	migrated = false;
	if (!profilesDocument.IsArray())
	{
		error = "vSMR_Profiles.json must contain a JSON array.";
		return false;
	}
	if (profilesDocument.Size() > kMaximumProfiles + 1U)
	{
		error = "vSMR_Profiles.json exceeds the 256-profile limit.";
		return false;
	}

	std::unordered_set<std::string> normalizedNames;
	std::vector<std::string> profileNames;
	bool metadataSeen = false;
	rapidjson::Value* metadata = nullptr;
	auto& allocator = profilesDocument.GetAllocator();

	// Validating profile entries and locating the single metadata record
	for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
	{
		rapidjson::Value& item = profilesDocument[index];
		if (!item.IsObject())
		{
			error = "Every vSMR_Profiles.json entry must be an object.";
			return false;
		}

		if (IsMetadataEntry(item))
		{
			if (metadataSeen)
			{
				error = "vSMR_Profiles.json contains more than one _vsmr metadata entry.";
				return false;
			}
			metadataSeen = true;
			metadata = &item[kMetadataWrapperKey];
			continue;
		}

		if (!item.HasMember("name") || !item["name"].IsString())
		{
			error = "Every non-metadata entry requires a string profile name.";
			return false;
		}
		const std::string name = TrimAsciiWhitespace(item["name"].GetString());
		if (name.empty())
		{
			error = "Profile names cannot be empty.";
			return false;
		}
		std::string normalizedName = name;
		std::transform(normalizedName.begin(), normalizedName.end(), normalizedName.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (!normalizedNames.insert(normalizedName).second)
		{
			error = "Profile names must be unique (case-insensitive).";
			return false;
		}
		profileNames.push_back(name);
		if (profileNames.size() > kMaximumProfiles)
		{
			error = "vSMR_Profiles.json exceeds the 256-profile limit.";
			return false;
		}
		if (name != item["name"].GetString())
		{
			SetStringMember(item, "name", name, allocator);
			migrated = true;
		}

		int schemaVersion = 1;
		if (item.HasMember(kMetadataSchemaVersionKey))
		{
			if (!item[kMetadataSchemaVersionKey].IsInt())
			{
				error = "Profile '" + name + "' schema_version must be an integer.";
				return false;
			}
			schemaVersion = item[kMetadataSchemaVersionKey].GetInt();
		}
		if (schemaVersion < 1)
		{
			error = "Profile '" + name + "' has an invalid schema_version.";
			return false;
		}
		if (schemaVersion > kCurrentProfileSchemaVersion)
		{
			error = "Profile '" + name + "' uses unsupported future schema_version " +
				std::to_string(schemaVersion) + ". This build supports up to " +
				std::to_string(kCurrentProfileSchemaVersion) + ".";
			return false;
		}

		const std::string context = "Profile '" + name + "'";
		for (const char* key : { "approach_insets", "filters", "font", "labels", "maps", "rimcas", "rules", "targets" })
		{
			if (!ValidateOptionalMemberType(item, key, rapidjson::kObjectType, context, error))
				return false;
		}
		if (!ValidateOptionalMemberType(item, "sid_text_colors", rapidjson::kArrayType, context, error))
			return false;

		if (schemaVersion < kCurrentProfileSchemaVersion)
		{
			// Version 1 allowed these core sections to be absent. Empty objects are a
			// lossless migration because every reader already applies field defaults.
			EnsureEmptyObjectMember(item, "labels", allocator);
			EnsureEmptyObjectMember(item, "targets", allocator);
			SetIntegerMember(item, kMetadataSchemaVersionKey, kCurrentProfileSchemaVersion, allocator);
			migrated = true;
		}
		if (!RequireObjectMember(item, "labels", context, error) ||
			!RequireObjectMember(item, "targets", context, error))
			return false;

		if (item.HasMember("filters") && item["filters"].HasMember("display_modes"))
		{
			const rapidjson::Value& displayModes = item["filters"]["display_modes"];
			if (!displayModes.IsObject() ||
				(displayModes.HasMember("active") && !displayModes["active"].IsString()) ||
				(displayModes.HasMember("items") && !displayModes["items"].IsArray()))
			{
				error = context + " filters.display_modes has an invalid shape.";
				return false;
			}
			if (displayModes.HasMember("items"))
			{
				std::unordered_set<std::string> modeNames;
				for (rapidjson::SizeType modeIndex = 0; modeIndex < displayModes["items"].Size(); ++modeIndex)
				{
					const rapidjson::Value& mode = displayModes["items"][modeIndex];
					const std::string modeName = TrimAsciiWhitespace(ReadStringMember(mode, "name"));
					if (!mode.IsObject() || modeName.empty())
					{
						error = context + " contains a display mode without a valid name.";
						return false;
					}
					std::string normalizedMode = modeName;
					std::transform(normalizedMode.begin(), normalizedMode.end(), normalizedMode.begin(), [](unsigned char c) {
						return static_cast<char>(std::tolower(c));
					});
					if (!modeNames.insert(normalizedMode).second)
					{
						error = context + " contains duplicate display mode names.";
						return false;
					}
					if (mode.HasMember("statuses") && !mode["statuses"].IsObject())
					{
						error = context + " display-mode statuses must be an object.";
						return false;
					}
				}
			}
		}

		if (item.HasMember("rules") && item["rules"].HasMember("items") &&
			!item["rules"]["items"].IsArray())
		{
			error = context + " rules.items must be an array.";
			return false;
		}
		if (item.HasMember("rimcas"))
		{
			const rapidjson::Value& rimcas = item["rimcas"];
			for (const char* key : { "inactive_alerts", "runways", "timer", "timer_lvp" })
			{
				if (!ValidateOptionalMemberType(rimcas, key, rapidjson::kArrayType, context + " rimcas", error))
					return false;
			}
		}

		if (item.HasMember(kAvisoPresetsKey) && !item[kAvisoPresetsKey].IsObject())
		{
			error = context + " aviso_presets must be an object.";
			return false;
		}
	}

	if (profileNames.empty())
	{
		error = "vSMR_Profiles.json contains no usable profiles.";
		return false;
	}

	// Validating file-wide settings after all profile names are known
	if (!metadataSeen)
	{
		rapidjson::Value wrapper(rapidjson::kObjectType);
		rapidjson::Value metadataValue(rapidjson::kObjectType);
		metadataValue.AddMember(kMetadataSchemaVersionKey, kCurrentMetadataSchemaVersion, allocator);
		rapidjson::Value wrapperKey;
		wrapperKey.SetString(kMetadataWrapperKey, allocator);
		wrapper.AddMember(wrapperKey, metadataValue, allocator);
		profilesDocument.PushBack(wrapper, allocator);
		metadata = &profilesDocument[profilesDocument.Size() - 1][kMetadataWrapperKey];
		migrated = true;
	}

	if (metadata == nullptr || !metadata->IsObject())
	{
		error = "The _vsmr metadata entry is invalid.";
		return false;
	}
	int metadataSchema = 1;
	if (metadata->HasMember(kMetadataSchemaVersionKey))
	{
		if (!(*metadata)[kMetadataSchemaVersionKey].IsInt())
		{
			error = "_vsmr.schema_version must be an integer.";
			return false;
		}
		metadataSchema = (*metadata)[kMetadataSchemaVersionKey].GetInt();
	}
	if (metadataSchema < 1 || metadataSchema > kCurrentMetadataSchemaVersion)
	{
		error = metadataSchema > kCurrentMetadataSchemaVersion
			? "The profiles file uses an unsupported future _vsmr schema_version."
			: "The profiles file has an invalid _vsmr schema_version.";
		return false;
	}
	if (!metadata->HasMember(kMetadataSchemaVersionKey))
	{
		SetIntegerMember(*metadata, kMetadataSchemaVersionKey, kCurrentMetadataSchemaVersion, allocator);
		migrated = true;
	}
	if (metadata->HasMember(kLastActiveProfileKey) && !(*metadata)[kLastActiveProfileKey].IsString())
	{
		error = "_vsmr.last_active_profile must be a string.";
		return false;
	}
	if (metadata->HasMember(kAvisoPresetsKey))
	{
		const rapidjson::Value& presetRoot = (*metadata)[kAvisoPresetsKey];
		if (!presetRoot.IsObject())
		{
			error = "_vsmr.aviso_presets must be an object.";
			return false;
		}
		if (presetRoot.HasMember(kAirportPresetStoresKey))
		{
			if (!presetRoot[kAirportPresetStoresKey].IsObject())
			{
				error = "_vsmr.aviso_presets.airports must be an object.";
				return false;
			}
			for (auto airport = presetRoot[kAirportPresetStoresKey].MemberBegin();
				airport != presetRoot[kAirportPresetStoresKey].MemberEnd(); ++airport)
			{
				const std::string airportName = NormalizeAirportKey(airport->name.GetString());
				if (airportName.empty() ||
					!ValidatePresetStore(airport->value, "Inset presets for " + airportName, error))
					return false;
			}
		}
		// Flat stores are deliberately retained. They do not contain an airport
		// identity, so assigning them to whichever airport happens to be active
		// would silently corrupt their scope.
		if (!ValidatePresetStore(presetRoot, "Legacy inset presets", error))
			return false;
	}

	const std::string lastActive = TrimAsciiWhitespace(ReadStringMember(*metadata, kLastActiveProfileKey));
	if (!lastActive.empty())
	{
		const bool exists = std::any_of(profileNames.begin(), profileNames.end(), [&](const std::string& name) {
			return EqualsNoCaseAscii(name, lastActive);
		});
		if (!exists)
		{
			SetStringMember(*metadata, kLastActiveProfileKey, profileNames.front(), allocator);
			migrated = true;
		}
	}
	return true;
}

CConfig::CConfig(string configPath, string mapPath)
{
	config_path = configPath;
	map_path = mapPath;
	invalid_profile.SetObject();
	loadConfig();
	loadMap();

	setActiveProfile("Default");

	std::lock_guard<std::mutex> writeGuard(ConfigSaveMutex());
	LiveConfigs().push_back(this);
}

bool CConfig::reload()
{
	string activeName = getActiveProfileName();
	const bool configLoaded = loadConfig();
	// Legacy map data is optional and independent from the profiles source.
	// A bad obsolete map file must not turn a successful profile reload or
	// backup recovery into a false failure.
	loadMap();
	if (!profiles.empty())
		setActiveProfile(activeName.empty() ? profiles.begin()->first : activeName);
	return configLoaded;
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
	const std::string serializedJson(buffer.GetString(), buffer.Size());
	if (!ParseValidatedArray(serializedJson, parsed, &error))
	{
		if (error.empty())
			error = "Profiles state could not be parsed.";
		return false;
	}
	bool migrated = false;
	if (!validateAndMigratePrevalidatedProfilesDocument(parsed, error, migrated))
		return false;

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
	const bool hadUsableConfiguration = !profiles.empty() && document.IsArray();
	config_revision = FileRevision(config_path);
	config_healthy = false;
	last_load_message.clear();

	auto parseSizeBoundedCandidate = [&](
		const std::string& serializedJson,
		Document& candidate,
		map<string, rapidjson::SizeType>& candidateProfiles,
		bool& migrated,
		std::string& error) -> bool
	{
		if (!ParseSizeBoundedArray(serializedJson, candidate, &error))
		{
			if (error.empty())
				error = "The profiles file is not a valid JSON array.";
			return false;
		}
		if (!validateAndMigratePrevalidatedProfilesDocument(candidate, error, migrated))
			return false;
		for (SizeType index = 0; index < candidate.Size(); ++index)
		{
			const Value& profile = candidate[index];
			if (IsProfileEntry(profile))
				candidateProfiles[trimProfileName(profile["name"].GetString())] = index;
		}
		return !candidateProfiles.empty();
	};

	// Loading the primary profiles source
	std::string mainJson;
	std::string mainError;
	std::string mainReadError;
	Document mainCandidate(&document.GetAllocator());
	map<string, rapidjson::SizeType> mainProfiles;
	bool mainMigrated = false;
	const bool mainRead = ReadFileContents(config_path, mainJson, &mainReadError);
	const bool mainValid = mainRead && parseSizeBoundedCandidate(
		mainJson,
		mainCandidate,
		mainProfiles,
		mainMigrated,
		mainError);

	if (mainValid)
	{
		if (mainMigrated && !PersistConfigDocument(config_path, mainCandidate))
		{
			// The source is semantically valid and the migration succeeded.  Keep
			// that safe migrated view available in memory, but mark it unhealthy so
			// the UI blocks writes until the user fixes permissions or restores a
			// writable source.
			AdoptDocument(document, mainCandidate);
			profiles.swap(mainProfiles);
			active_profile = profiles.begin()->second;
			config_revision = ContentRevision(mainJson);
			config_healthy = false;
			last_load_message =
				"The profiles file is valid but its schema migration could not be saved. "
				"The migrated profiles are active read-only. Check folder permissions, then reload.";
			ReportLoadFailure("configuration");
			return false;
		}

		AdoptDocument(document, mainCandidate);
		profiles.swap(mainProfiles);
		active_profile = profiles.begin()->second;
		config_revision = mainMigrated
			? FileRevision(config_path)
			: ContentRevision(mainJson);
		config_healthy = true;
		if (mainMigrated)
			last_load_message = "Profiles were migrated transactionally to schema version 2.";
		return true;
	}

	const std::string failure = mainRead
		? (mainError.empty() ? "The profiles file is invalid." : mainError)
		: (mainReadError.empty()
			? "The configured profiles file is missing or cannot be read."
			: mainReadError);

	last_load_message = failure + " Restore bundled defaults from Settings.";
	if (!hadUsableConfiguration)
	{
		document.SetArray();
		profiles.clear();
		active_profile = 0;
		invalid_profile.SetObject();
	}
	ReportLoadFailure("configuration");
	return false;
}

bool CConfig::loadMap()
{
	std::string serializedJson;
	if (!ReadFileContents(map_path, serializedJson))
	{
		if (mapDocument.IsNull())
			mapDocument.SetArray();
		// Missing legacy map data is a valid empty configuration
		return true;
	}

	Document validationDocument;
	std::map<int, std::vector<mapData>> loadedMaps;
	if (!ParseSizeBoundedArray(serializedJson, validationDocument) ||
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

const Value& CConfig::getActiveProfile() const {
	if (document.IsArray())
	{
		if (active_profile < document.Size() && IsProfileEntry(document[active_profile]))
			return document[active_profile];

		if (!profiles.empty())
		{
			const rapidjson::SizeType fallbackProfile = profiles.begin()->second;
			if (fallbackProfile < document.Size() &&
				IsProfileEntry(document[fallbackProfile]))
			{
				return document[fallbackProfile];
			}
		}
	}

	return invalid_profile;
}

Value& CConfig::getMutableActiveProfile() {
	if (document.IsArray())
	{
		if (active_profile < document.Size() && IsProfileEntry(document[active_profile]))
			return document[active_profile];

		if (!profiles.empty())
		{
			const rapidjson::SizeType fallbackProfile = profiles.begin()->second;
			if (fallbackProfile < document.Size() &&
				IsProfileEntry(document[fallbackProfile]))
			{
				return document[fallbackProfile];
			}
		}
	}

	return invalid_profile;
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
			if (VsmrRadarUiSupport::startsWith(sid.c_str(), currentSid.c_str()))
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

Gdiplus::Color CConfig::getConfigColor(const Value& colorConfig) {
	if (!colorConfig.IsObject())
		return Gdiplus::Color(255, 0, 0, 0);

	const int r = ReadColorComponent(colorConfig, "r");
	const int g = ReadColorComponent(colorConfig, "g");
	const int b = ReadColorComponent(colorConfig, "b");
	const int a = ReadColorComponent(colorConfig, "a", 255);

	Gdiplus::Color Color(
		static_cast<BYTE>(a),
		static_cast<BYTE>(r),
		static_cast<BYTE>(g),
		static_cast<BYTE>(b));
	return Color;
}

COLORREF CConfig::getConfigColorRef(const Value& colorConfig) {
	if (!colorConfig.IsObject())
		return RGB(0, 0, 0);

	const int r = ReadColorComponent(colorConfig, "r");
	const int g = ReadColorComponent(colorConfig, "g");
	const int b = ReadColorComponent(colorConfig, "b");

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
	// Older profiles did not store this choice and used the custom cursor
	return true;
}

bool CConfig::isCustomRunwayAvail(string airport, string name1, string name2) {
	const Value& activeProfile = getActiveProfile();
	const Value* mapDefinitions = GetObjectMemberIfPresent(activeProfile, "maps");
	if (mapDefinitions == nullptr)
		return false;
	const Value* airportMap = GetObjectMemberIfPresent(*mapDefinitions, airport.c_str());
	if (airportMap == nullptr || !airportMap->HasMember("runways") || !(*airportMap)["runways"].IsArray())
		return false;

	const Value& runways = (*airportMap)["runways"];
	for (SizeType i = 0; i < runways.Size(); i++) {
		const Value& runway = runways[i];
		if (!runway.IsObject() || !runway.HasMember("runway_name") || !runway["runway_name"].IsString())
			continue;
		const char* runwayName = runway["runway_name"].GetString();
		if (VsmrRadarUiSupport::startsWith(name1.c_str(), runwayName) ||
			VsmrRadarUiSupport::startsWith(name2.c_str(), runwayName))
			return true;
	}
	return false;
}

vector<string> CConfig::getAllProfiles() const {
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
		for (std::map<string, rapidjson::SizeType>::const_iterator it = profiles.begin(); it != profiles.end(); ++it)
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

string CConfig::getConfigRevision() const
{
	return config_revision;
}

string CConfig::getPersistedConfigRevision() const
{
	return FileRevision(config_path);
}

string CConfig::getLastLoadMessage() const
{
	return last_load_message;
}

bool CConfig::isConfigHealthy() const
{
	return config_healthy && !profiles.empty();
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

bool CConfig::saveConfig(
	const vector<ProfileSaveIdentity>& profileIdentities,
	const string& expectedRevision,
	string* error,
	bool allowRecoveryReplacement)
{
	std::lock_guard<std::mutex> writeGuard(ConfigSaveMutex());
	if (error != nullptr)
		error->clear();
	if (!document.IsArray())
	{
		if (error != nullptr)
			*error = "The in-memory profiles state is not an array.";
		return false;
	}

	std::string currentJson;
	const bool currentFileReadable = ReadFileContents(config_path, currentJson);
	const std::string currentRevision = currentFileReadable
		? ContentRevision(currentJson)
		: "missing";
	const std::string requiredRevision = expectedRevision.empty()
		? config_revision
		: expectedRevision;
	if (!requiredRevision.empty() && requiredRevision != currentRevision)
	{
		if (error != nullptr)
			*error =
				"The profiles file changed in another vSMR window. Reload before saving so those changes are not overwritten.";
		return false;
	}
	if (!config_healthy && !allowRecoveryReplacement)
	{
		if (error != nullptr)
			*error = "The profiles source is invalid. Restore bundled defaults from Settings before saving.";
		return false;
	}

	Document validated;
	rapidjson::StringBuffer candidateBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> candidateWriter(candidateBuffer);
	document.Accept(candidateWriter);
	const std::string candidateJson(candidateBuffer.GetString(), candidateBuffer.Size());
	bool migrated = false;
	std::string validationError;
	if (!ParseValidatedArray(candidateJson, validated, &validationError) ||
		!validateAndMigratePrevalidatedProfilesDocument(validated, validationError, migrated))
	{
		if (error != nullptr)
			*error = validationError.empty()
				? "The profiles state failed validation. Nothing was saved."
				: validationError;
		return false;
	}
	if (migrated)
	{
		Document replacement(&document.GetAllocator());
		rapidjson::StringBuffer migratedBuffer;
		rapidjson::Writer<rapidjson::StringBuffer> migratedWriter(migratedBuffer);
		validated.Accept(migratedWriter);
		replacement.Parse<0>(migratedBuffer.GetString());
		if (replacement.HasParseError())
		{
			if (error != nullptr)
				*error = "The migrated profiles state could not be prepared for saving.";
			return false;
		}
		AdoptDocument(document, replacement);
	}

	// Preset stores are immediate, shared state. An ordinary save may originate
	// from a screen whose full profile snapshot predates another screen's preset
	// transaction, so refresh only these roots before persisting everything else.
	std::string latestJson;
	Document latestDocument;
	if (currentFileReadable)
	{
		bool latestMigrated = false;
		std::string latestError;
		if (!ParseSizeBoundedArray(currentJson, latestDocument, &latestError) ||
			!validateAndMigratePrevalidatedProfilesDocument(latestDocument, latestError, latestMigrated))
		{
			// An explicitly confirmed recovery save may replace a damaged primary
			// file while ordinary saves still fail closed on external corruption
			if (config_healthy)
			{
				if (error != nullptr)
					*error = "The profiles file became invalid on disk. Reload or restore bundled defaults before saving.";
				return false;
			}
		}
		else
		{
			if (!MergeLatestAvisoPresetRoots(document, latestDocument, profileIdentities))
			{
				if (error != nullptr)
					*error = "Unable to merge shared inset presets from the latest profiles file.";
				return false;
			}
		}
	}

	if (!PersistConfigDocument(config_path, document))
	{
		if (error != nullptr)
			*error = "Unable to save vSMR_Profiles.json atomically.";
		return false;
	}
	config_revision = FileRevision(config_path);
	config_healthy = true;
	last_load_message.clear();
	return true;
}

bool CConfig::sharesConfigFileWith(const CConfig& other) const
{
	return EqualsNoCaseAscii(config_path, other.config_path);
}

const Value* CConfig::getSharedAvisoPresetContainer() const
{
	return findMetadata();
}

bool CConfig::transactAvisoPresetStore(
	const string& preferredProfileName,
	const string& activeAirport,
	const AvisoPresetTransaction& transaction)
{
	if (NormalizeAirportKey(activeAirport).empty() || !transaction)
		return false;

	std::lock_guard<std::mutex> writeGuard(ConfigSaveMutex());

	// Deliberately load only the profiles document. A malformed or unavailable
	// maps file must not make an otherwise valid preset transaction fail.
	std::string latestJson;
	Document latestDocument;
	if (!ReadFileContents(config_path, latestJson) ||
		!ParseSizeBoundedArray(latestJson, latestDocument))
	{
		return false;
	}
	const std::string previousRevision = ContentRevision(latestJson);
	bool schemaMigrated = false;
	std::string validationError;
	if (!validateAndMigratePrevalidatedProfilesDocument(
		latestDocument,
		validationError,
		schemaMigrated))
	{
		return false;
	}
	const bool migrated = MigrateProfileAvisoPresetRoots(
		latestDocument,
		// Profile identity is used only to choose deterministic precedence while
		// importing legacy data; the resulting store itself is profile-independent.
		trimProfileName(preferredProfileName),
		activeAirport);
	Value& sharedMetadata = EnsureMetadataValue(latestDocument);
	const AvisoPresetTransactionAction action = transaction(
		sharedMetadata,
		latestDocument.GetAllocator());
	if (action == AvisoPresetTransactionAction::Abort)
		return false;

	if ((schemaMigrated || migrated || action == AvisoPresetTransactionAction::Save) &&
		!PersistConfigDocument(config_path, latestDocument))
	{
		return false;
	}
	const std::string persistedRevision = FileRevision(config_path);

	bool ownerMerged = false;
	for (CConfig* liveConfig : LiveConfigs())
	{
		if (liveConfig == nullptr ||
			!EqualsNoCaseAscii(liveConfig->config_path, config_path))
			continue;

		const bool revisionWasCurrent =
			liveConfig->config_revision == previousRevision;
		if (!MergeLatestAvisoPresetRoots(
			liveConfig->document,
			latestDocument,
			{}))
		{
			return false;
		}
		if (revisionWasCurrent)
			liveConfig->config_revision = persistedRevision;
		if (liveConfig == this)
			ownerMerged = true;
	}

	return ownerMerged;
}

bool CConfig::assignUnscopedAvisoPresetsToAirport(
	const string& preferredProfileName,
	const string& airport,
	size_t& assignedPresetCount,
	string& error)
{
	assignedPresetCount = 0;
	error.clear();
	const std::string airportKey = NormalizeAirportKey(airport);
	if (airportKey.size() != 4 ||
		!std::all_of(airportKey.begin(), airportKey.end(), [](unsigned char c) {
			return std::isalnum(c) != 0;
		}))
	{
		error = "A valid four-character airport is required.";
		return false;
	}

	std::lock_guard<std::mutex> writeGuard(ConfigSaveMutex());
	std::string latestJson;
	Document latestDocument;
	if (!ReadFileContents(config_path, latestJson) ||
		!ParseSizeBoundedArray(latestJson, latestDocument))
	{
		error = "The current profiles file is unavailable or invalid.";
		return false;
	}
	const std::string previousRevision = ContentRevision(latestJson);
	bool schemaMigrated = false;
	if (!validateAndMigratePrevalidatedProfilesDocument(
		latestDocument,
		error,
		schemaMigrated))
	{
		return false;
	}

	auto& allocator = latestDocument.GetAllocator();
	Value& metadata = EnsureMetadataValue(latestDocument);
	Value& globalRoot = EnsureObjectMember(metadata, kAvisoPresetsKey, allocator);
	Value& airportStores = EnsureObjectMember(globalRoot, kAirportPresetStoresKey, allocator);
	Value& destination = EnsureObjectMember(airportStores, airportKey.c_str(), allocator);
	bool assigned = false;

	auto assignFlatStore = [&](
		Value& sourceRoot,
		const Value* parentProfile,
		const std::string& sourceLabel) -> bool
	{
		const bool hasItems = sourceRoot.HasMember(kPresetItemsKey);
		const bool hasDefault = sourceRoot.HasMember(kDefaultPresetKey);
		if (!hasItems && !hasDefault)
			return true;

		std::string explicitAirport = NormalizeAirportKey(
			ReadStringMember(sourceRoot, "airport"));
		if (explicitAirport.empty() && parentProfile != nullptr)
			explicitAirport = NormalizeAirportKey(
				ReadStringMember(*parentProfile, "airport"));
		const bool hasExplicitAirport = explicitAirport.size() == 4 &&
			std::all_of(explicitAirport.begin(), explicitAirport.end(), [](unsigned char c) {
				return std::isalnum(c) != 0;
			});
		if (hasExplicitAirport)
			return true;

		if (!PresetSectionCanBeMigrated(sourceRoot) ||
			!MergePresetSection(destination, sourceRoot, sourceLabel, allocator))
		{
			error = "A legacy inset preset store could not be migrated safely.";
			return false;
		}
		if (hasItems)
			assignedPresetCount += sourceRoot[kPresetItemsKey].Size();
		sourceRoot.RemoveMember(kPresetItemsKey);
		if (hasDefault)
			sourceRoot.RemoveMember(kDefaultPresetKey);
		if (sourceRoot.HasMember("airport"))
			sourceRoot.RemoveMember("airport");
		assigned = true;
		return true;
	};

	if (!assignFlatStore(globalRoot, nullptr, "Legacy"))
		return false;
	for (rapidjson::SizeType index = 0; index < latestDocument.Size(); ++index)
	{
		Value& profile = latestDocument[index];
		if (!IsProfileEntry(profile) ||
			!profile.HasMember(kAvisoPresetsKey) ||
			!profile[kAvisoPresetsKey].IsObject())
		{
			continue;
		}
		Value& profileRoot = profile[kAvisoPresetsKey];
		const std::string label = ReadStringMember(profile, "name");
		if (!assignFlatStore(profileRoot, &profile, label))
			return false;
	}

	if (!assigned)
	{
		error = "No unscoped legacy inset presets remain to assign.";
		return false;
	}

	// Consolidate any already-scoped profile stores at the same time. After the
	// explicit assignment, the canonical metadata root is the sole authority and
	// ordinary stale-writer protection can safely remain strict.
	MigrateProfileAvisoPresetRoots(
		latestDocument,
		trimProfileName(preferredProfileName),
		airportKey);
	if (!PersistConfigDocument(config_path, latestDocument))
	{
		error = "Unable to save the airport assignment atomically.";
		return false;
	}
	const std::string persistedRevision = FileRevision(config_path);
	for (CConfig* liveConfig : LiveConfigs())
	{
		if (liveConfig == nullptr ||
			!EqualsNoCaseAscii(liveConfig->config_path, config_path))
		{
			continue;
		}
		const bool revisionWasCurrent =
			liveConfig->config_revision == previousRevision;
		if (!MergeLatestAvisoPresetRoots(
			liveConfig->document,
			latestDocument,
			{}))
		{
			continue;
		}
		if (revisionWasCurrent)
			liveConfig->config_revision = persistedRevision;
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
	std::lock_guard<std::mutex> writeGuard(ConfigSaveMutex());
	std::vector<CConfig*>& liveConfigs = LiveConfigs();
	liveConfigs.erase(
		std::remove(liveConfigs.begin(), liveConfigs.end(), this),
		liveConfigs.end());
}
