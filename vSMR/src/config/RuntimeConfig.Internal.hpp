#pragma once

#include "config/RuntimeConfig.hpp"

#include <mutex>

namespace VsmrRuntimeConfigInternal
{
	inline constexpr const char* kMetadataWrapperKey = "_vsmr";
	inline constexpr const char* kMetadataSchemaVersionKey = "schema_version";
	inline constexpr const char* kLastActiveProfileKey = "last_active_profile";
	inline constexpr const char* kVacdmKey = "vacdm";
	inline constexpr const char* kVacdmServerUrlKey = "server_url";
	inline constexpr const char* kBackupSuffix = ".bak";
	inline constexpr const char* kAvisoPresetsKey = "aviso_presets";
	inline constexpr const char* kAirportPresetStoresKey = "airports";
	inline constexpr const char* kPresetItemsKey = "items";
	inline constexpr const char* kDefaultPresetKey = "default";
	inline constexpr int kCurrentProfileSchemaVersion = 2;
	inline constexpr int kCurrentMetadataSchemaVersion = 1;
	inline constexpr std::size_t kMaximumConfigFileBytes = CConfig::MaximumSerializedInputBytes;
	inline constexpr std::size_t kMaximumConfigStringBytes = 64U * 1024U;
	inline constexpr std::size_t kMaximumConfigJsonDepth = 64U;
	inline constexpr std::size_t kMaximumConfigJsonValues = 500000U;
	inline constexpr std::size_t kMaximumConfigContainerEntries = 100000U;
	inline constexpr std::size_t kMaximumProfiles = 256U;

	using Allocator = rapidjson::Document::AllocatorType;

	std::mutex& ConfigSaveMutex();
	std::vector<CConfig*>& LiveConfigs();
	const rapidjson::Value* GetObjectMemberIfPresent(const rapidjson::Value& object, const char* key);
	bool IsProfileEntry(const rapidjson::Value& value);
	bool IsMetadataEntry(const rapidjson::Value& value);
	std::string ReadStringMember(const rapidjson::Value& object, const char* key);
	void SetStringMember(rapidjson::Value& object, const char* key, const std::string& value, Allocator& allocator);
	rapidjson::Value& EnsureObjectMember(rapidjson::Value& object, const char* key, Allocator& allocator);
	std::string TrimAsciiWhitespace(std::string value);
	std::string NormalizeAirportKey(std::string value);
	const rapidjson::Value* FindMetadataValue(const rapidjson::Document& profilesDocument);
	rapidjson::Value* FindMetadataValue(rapidjson::Document& profilesDocument);
	rapidjson::Value& EnsureMetadataValue(rapidjson::Document& profilesDocument);
	int ReadColorComponent(const rapidjson::Value& colorValue, const char* key, int fallback = 0);
	bool ValidateJsonTextLimits(const std::string& contents, std::string* error);
	bool ValidateJsonDocumentLimits(const rapidjson::Value& value, std::string* error);
	bool ValidateJsonStreamingLimits(const std::string& contents, std::string* error);
	bool ReadFileContents(const std::string& path, std::string& contents, std::string* error = nullptr);
	void ReportLoadFailure(const char* fileDescription);
	void AdoptDocument(rapidjson::Document& destination, rapidjson::Document& source);
	bool ParseValidatedArray(const std::string& serializedJson, rapidjson::Document& validationDocument, std::string* error = nullptr);
	std::string ContentRevision(const std::string& contents);
	std::string FileRevision(const std::string& path);
	bool HasFile(const std::string& path);
	bool RequireObjectMember(const rapidjson::Value& object, const char* key, const std::string& context, std::string& error);
	bool ValidateOptionalMemberType(const rapidjson::Value& object, const char* key, rapidjson::Type expectedType, const std::string& context, std::string& error);
	void EnsureEmptyObjectMember(rapidjson::Value& object, const char* key, Allocator& allocator);
	void SetIntegerMember(rapidjson::Value& object, const char* key, int value, Allocator& allocator);
	bool ValidatePresetRect(const rapidjson::Value& object, const std::string& context, std::string& error);
	bool ValidatePresetStore(const rapidjson::Value& store, const std::string& context, std::string& error);
	bool EqualsNoCaseAscii(const std::string& left, const std::string& right);
	rapidjson::Value* FindProfileByName(rapidjson::Document& profilesDocument, const std::string& profileName);
	const rapidjson::Value* FindProfileByName(const rapidjson::Document& profilesDocument, const std::string& profileName);
	bool CloneJsonValue(const rapidjson::Value& source, Allocator& allocator, rapidjson::Value& destination);
	rapidjson::Value& EnsureArrayMember(rapidjson::Value& object, const char* key, Allocator& allocator);
	std::string ReadPresetName(const rapidjson::Value& preset);
	rapidjson::SizeType FindPresetIndexNoCase(const rapidjson::Value& items, const std::string& name);
	bool JsonValuesEqual(const rapidjson::Value& left, const rapidjson::Value& right);
	bool PresetSectionCanBeMigrated(const rapidjson::Value& section);
	bool MergePresetSection(rapidjson::Value& destinationSection, const rapidjson::Value& sourceSection, const std::string& sourceProfile, Allocator& allocator);
	bool MigrateProfileAvisoPresetRoots(rapidjson::Document& profilesDocument, const std::string& preferredProfileName, const std::string& activeAirport);
	bool MergeLatestAvisoPresetRoots(rapidjson::Document& destination, const rapidjson::Document& authoritative, const std::vector<CConfig::ProfileSaveIdentity>& profileIdentities);
	bool BuildMapIndex(const rapidjson::Document& source, std::map<int, std::vector<CConfig::mapData>>& loadedMaps);
	bool WriteTemporaryFile(const std::string& destination, const std::string& contents, std::string& temporaryPath);
	bool PersistConfigDocument(const std::string& destination, const rapidjson::Document& source);
}
