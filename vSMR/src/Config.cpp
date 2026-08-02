#include "stdafx.h"
#include "Config.hpp"
#include <algorithm>
#include <mutex>

namespace
{
	const char* kMetadataWrapperKey = "_vsmr";
	const char* kMetadataSchemaVersionKey = "schema_version";
	const char* kLastActiveProfileKey = "last_active_profile";
	const char* kVacdmKey = "vacdm";
	const char* kVacdmServerUrlKey = "server_url";
	const char* kBackupSuffix = ".bak";
	const char* kAvisoPresetsKey = "aviso_presets";
	const char* kAirportPresetStoresKey = "airports";
	const char* kPresetItemsKey = "items";
	const char* kDefaultPresetKey = "default";
	volatile LONG gTemporaryFileSequence = 0;
	// Every CConfig instance points at the same persisted profiles file. Both
	// ordinary saves and narrowly-scoped preset transactions participate in this
	// lock so a stale radar screen cannot race a newer preset write.
	std::mutex gConfigSaveMutex;
	std::vector<CConfig*> gLiveConfigs;

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

	std::string TrimAsciiWhitespace(std::string value)
	{
		size_t start = 0;
		while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
			++start;

		size_t end = value.size();
		while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
			--end;
		return value.substr(start, end - start);
	}

	std::string NormalizeAirportKey(std::string value)
	{
		value = TrimAsciiWhitespace(value);
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::toupper(character));
		});
		return value;
	}

	const rapidjson::Value* FindMetadataValue(const rapidjson::Document& profilesDocument)
	{
		if (!profilesDocument.IsArray())
			return nullptr;

		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			const rapidjson::Value& entry = profilesDocument[index];
			if (IsMetadataEntry(entry))
				return &entry[kMetadataWrapperKey];
		}
		return nullptr;
	}

	rapidjson::Value* FindMetadataValue(rapidjson::Document& profilesDocument)
	{
		return const_cast<rapidjson::Value*>(
			FindMetadataValue(static_cast<const rapidjson::Document&>(profilesDocument)));
	}

	rapidjson::Value& EnsureMetadataValue(rapidjson::Document& profilesDocument)
	{
		if (!profilesDocument.IsArray())
			profilesDocument.SetArray();

		if (rapidjson::Value* metadata = FindMetadataValue(profilesDocument))
			return *metadata;

		rapidjson::Value wrapper(rapidjson::kObjectType);
		rapidjson::Value metadata(rapidjson::kObjectType);
		metadata.AddMember(kMetadataSchemaVersionKey, 1, profilesDocument.GetAllocator());
		rapidjson::Value wrapperKey;
		wrapperKey.SetString(kMetadataWrapperKey, profilesDocument.GetAllocator());
		wrapper.AddMember(wrapperKey, metadata, profilesDocument.GetAllocator());
		profilesDocument.PushBack(wrapper, profilesDocument.GetAllocator());
		return profilesDocument[profilesDocument.Size() - 1][kMetadataWrapperKey];
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

	bool EqualsNoCaseAscii(const std::string& left, const std::string& right)
	{
		if (left.size() != right.size())
			return false;
		for (size_t index = 0; index < left.size(); ++index)
		{
			if (std::tolower(static_cast<unsigned char>(left[index])) !=
				std::tolower(static_cast<unsigned char>(right[index])))
			{
				return false;
			}
		}
		return true;
	}

	rapidjson::Value* FindProfileByName(
		rapidjson::Document& profilesDocument,
		const std::string& profileName)
	{
		if (!profilesDocument.IsArray() || profileName.empty())
			return nullptr;

		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			rapidjson::Value& profile = profilesDocument[index];
			if (IsProfileEntry(profile) &&
				EqualsNoCaseAscii(profile["name"].GetString(), profileName))
			{
				return &profile;
			}
		}
		return nullptr;
	}

	const rapidjson::Value* FindProfileByName(
		const rapidjson::Document& profilesDocument,
		const std::string& profileName)
	{
		if (!profilesDocument.IsArray() || profileName.empty())
			return nullptr;

		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			const rapidjson::Value& profile = profilesDocument[index];
			if (IsProfileEntry(profile) &&
				EqualsNoCaseAscii(profile["name"].GetString(), profileName))
			{
				return &profile;
			}
		}
		return nullptr;
	}

	bool CloneJsonValue(
		const rapidjson::Value& source,
		rapidjson::Document::AllocatorType& allocator,
		rapidjson::Value& destination)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		source.Accept(writer);

		rapidjson::Document clone(&allocator);
		clone.Parse<0>(buffer.GetString());
		if (clone.HasParseError())
			return false;

		destination = static_cast<rapidjson::Value&>(clone);
		return true;
	}

	rapidjson::Value& EnsureArrayMember(
		rapidjson::Value& object,
		const char* key,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!object.IsObject())
			object.SetObject();

		if (!object.HasMember(key) || !object[key].IsArray())
		{
			rapidjson::Value replacement(rapidjson::kArrayType);
			if (object.HasMember(key))
				object[key] = replacement;
			else
			{
				rapidjson::Value keyValue;
				keyValue.SetString(key, allocator);
				object.AddMember(keyValue, replacement, allocator);
			}
		}
		return object[key];
	}

	std::string ReadPresetName(const rapidjson::Value& preset)
	{
		if (!preset.IsObject() || !preset.HasMember("name") || !preset["name"].IsString())
			return "";
		return TrimAsciiWhitespace(preset["name"].GetString());
	}

	rapidjson::SizeType FindPresetIndexNoCase(
		const rapidjson::Value& items,
		const std::string& name)
	{
		if (!items.IsArray() || name.empty())
			return static_cast<rapidjson::SizeType>(-1);

		for (rapidjson::SizeType index = 0; index < items.Size(); ++index)
		{
			const std::string candidate = ReadPresetName(items[index]);
			if (!candidate.empty() && EqualsNoCaseAscii(candidate, name))
				return index;
		}
		return static_cast<rapidjson::SizeType>(-1);
	}

	bool JsonValuesEqual(const rapidjson::Value& left, const rapidjson::Value& right)
	{
		rapidjson::StringBuffer leftBuffer;
		rapidjson::Writer<rapidjson::StringBuffer> leftWriter(leftBuffer);
		left.Accept(leftWriter);
		rapidjson::StringBuffer rightBuffer;
		rapidjson::Writer<rapidjson::StringBuffer> rightWriter(rightBuffer);
		right.Accept(rightWriter);
		return std::strcmp(leftBuffer.GetString(), rightBuffer.GetString()) == 0;
	}

	std::string MakeUniqueMigratedPresetName(
		const rapidjson::Value& items,
		const std::string& originalName,
		const std::string& sourceProfile)
	{
		const std::string suffix = TrimAsciiWhitespace(sourceProfile).empty()
			? "Legacy profile"
			: TrimAsciiWhitespace(sourceProfile);
		const std::string base = originalName + " (" + suffix + ")";
		std::string candidate = base;
		int sequence = 2;
		while (FindPresetIndexNoCase(items, candidate) != static_cast<rapidjson::SizeType>(-1))
			candidate = base + " " + std::to_string(sequence++);
		return candidate;
	}

	bool PresetSectionDefaultIsValid(const rapidjson::Value& section)
	{
		if (!section.IsObject() || !section.HasMember(kDefaultPresetKey) ||
			!section[kDefaultPresetKey].IsString() || !section.HasMember(kPresetItemsKey) ||
			!section[kPresetItemsKey].IsArray())
		{
			return false;
		}

		const std::string defaultName =
			TrimAsciiWhitespace(section[kDefaultPresetKey].GetString());
		return !defaultName.empty() &&
			FindPresetIndexNoCase(section[kPresetItemsKey], defaultName) !=
				static_cast<rapidjson::SizeType>(-1);
	}

	bool PresetSectionCanBeMigrated(const rapidjson::Value& section)
	{
		if (!section.IsObject())
			return false;
		if (section.HasMember(kPresetItemsKey))
		{
			if (!section[kPresetItemsKey].IsArray())
				return false;
			const rapidjson::Value& items = section[kPresetItemsKey];
			for (rapidjson::SizeType index = 0; index < items.Size(); ++index)
			{
				if (ReadPresetName(items[index]).empty())
					return false;
			}
		}
		return !section.HasMember(kDefaultPresetKey) ||
			section[kDefaultPresetKey].IsString();
	}

	bool ProfilePresetRootCanBeMigrated(const rapidjson::Value& root)
	{
		if (!PresetSectionCanBeMigrated(root))
			return false;
		if (!root.HasMember(kAirportPresetStoresKey))
			return true;
		if (!root[kAirportPresetStoresKey].IsObject())
			return false;

		const rapidjson::Value& airports = root[kAirportPresetStoresKey];
		for (auto member = airports.MemberBegin(); member != airports.MemberEnd(); ++member)
		{
			if (NormalizeAirportKey(member->name.GetString()).empty() ||
				!PresetSectionCanBeMigrated(member->value))
			{
				return false;
			}
		}
		return true;
	}

	bool MergePresetSection(
		rapidjson::Value& destinationSection,
		const rapidjson::Value& sourceSection,
		const std::string& sourceProfile,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!sourceSection.IsObject())
			return false;
		if (sourceSection.HasMember(kPresetItemsKey) &&
			!sourceSection[kPresetItemsKey].IsArray())
		{
			return false;
		}
		if (sourceSection.HasMember(kDefaultPresetKey) &&
			!sourceSection[kDefaultPresetKey].IsString())
		{
			return false;
		}

		if (!destinationSection.IsObject())
			destinationSection.SetObject();
		rapidjson::Value& destinationItems =
			EnsureArrayMember(destinationSection, kPresetItemsKey, allocator);
		const std::string sourceDefault = sourceSection.HasMember(kDefaultPresetKey)
			? TrimAsciiWhitespace(sourceSection[kDefaultPresetKey].GetString())
			: "";
		std::string mappedDefault;

		if (sourceSection.HasMember(kPresetItemsKey))
		{
			const rapidjson::Value& sourceItems = sourceSection[kPresetItemsKey];
			for (rapidjson::SizeType index = 0; index < sourceItems.Size(); ++index)
			{
				const rapidjson::Value& sourcePreset = sourceItems[index];
				const std::string sourceName = ReadPresetName(sourcePreset);
				if (sourceName.empty())
					return false;

				std::string destinationName = sourceName;
				const rapidjson::SizeType existingIndex =
					FindPresetIndexNoCase(destinationItems, sourceName);
				if (existingIndex == static_cast<rapidjson::SizeType>(-1))
				{
					rapidjson::Value clonedPreset;
					if (!CloneJsonValue(sourcePreset, allocator, clonedPreset))
						return false;
					destinationItems.PushBack(clonedPreset, allocator);
				}
				else if (JsonValuesEqual(destinationItems[existingIndex], sourcePreset))
				{
					destinationName = ReadPresetName(destinationItems[existingIndex]);
				}
				else
				{
					destinationName = MakeUniqueMigratedPresetName(
						destinationItems,
						sourceName,
						sourceProfile);
					rapidjson::Value clonedPreset;
					if (!CloneJsonValue(sourcePreset, allocator, clonedPreset))
						return false;
					SetStringMember(clonedPreset, "name", destinationName, allocator);
					destinationItems.PushBack(clonedPreset, allocator);
				}

				if (!sourceDefault.empty() && EqualsNoCaseAscii(sourceDefault, sourceName))
					mappedDefault = destinationName;
			}
		}

		if (!PresetSectionDefaultIsValid(destinationSection))
		{
			if (destinationSection.HasMember(kDefaultPresetKey))
				destinationSection.RemoveMember(kDefaultPresetKey);
			if (!mappedDefault.empty())
				SetStringMember(destinationSection, kDefaultPresetKey, mappedDefault, allocator);
		}
		return true;
	}

	bool MergeProfilePresetRootIntoMetadata(
		rapidjson::Value& metadata,
		const rapidjson::Value& profile,
		const std::string& activeAirport,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!profile.IsObject() || !profile.HasMember(kAvisoPresetsKey) ||
			!profile[kAvisoPresetsKey].IsObject() ||
			!ProfilePresetRootCanBeMigrated(profile[kAvisoPresetsKey]))
		{
			return false;
		}

		const rapidjson::Value& sourceRoot = profile[kAvisoPresetsKey];
		rapidjson::Value& destinationRoot =
			EnsureObjectMember(metadata, kAvisoPresetsKey, allocator);
		rapidjson::Value& destinationAirports =
			EnsureObjectMember(destinationRoot, kAirportPresetStoresKey, allocator);
		const std::string sourceProfile = ReadStringMember(profile, "name");

		if (sourceRoot.HasMember(kAirportPresetStoresKey))
		{
			if (!sourceRoot[kAirportPresetStoresKey].IsObject())
				return false;

			const rapidjson::Value& sourceAirports = sourceRoot[kAirportPresetStoresKey];
			for (auto member = sourceAirports.MemberBegin(); member != sourceAirports.MemberEnd(); ++member)
			{
				const std::string airportKey = NormalizeAirportKey(member->name.GetString());
				if (airportKey.empty() || !member->value.IsObject())
					return false;
				rapidjson::Value& destinationSection =
					EnsureObjectMember(destinationAirports, airportKey.c_str(), allocator);
				if (!MergePresetSection(
					destinationSection,
					member->value,
					sourceProfile,
					allocator))
				{
					return false;
				}
			}
		}

		const bool hasFlatItems = sourceRoot.HasMember(kPresetItemsKey);
		const bool hasFlatDefault = sourceRoot.HasMember(kDefaultPresetKey);
		if (hasFlatItems || hasFlatDefault)
		{
			const std::string airportKey = NormalizeAirportKey(activeAirport);
			if (airportKey.empty())
				return false;
			rapidjson::Value& destinationSection =
				EnsureObjectMember(destinationAirports, airportKey.c_str(), allocator);
			if (!MergePresetSection(
				destinationSection,
				sourceRoot,
				sourceProfile,
				allocator))
			{
				return false;
			}
		}

		return true;
	}

	bool MigrateProfileAvisoPresetRoots(
		rapidjson::Document& profilesDocument,
		const std::string& preferredProfileName,
		const std::string& activeAirport)
	{
		if (!profilesDocument.IsArray())
			return false;

		std::vector<rapidjson::SizeType> profileOrder;
		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			const rapidjson::Value& profile = profilesDocument[index];
			if (IsProfileEntry(profile) &&
				EqualsNoCaseAscii(profile["name"].GetString(), preferredProfileName))
			{
				profileOrder.push_back(index);
				break;
			}
		}
		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			if (!IsProfileEntry(profilesDocument[index]) ||
				(!profileOrder.empty() && profileOrder.front() == index))
			{
				continue;
			}
			profileOrder.push_back(index);
		}

		bool migrated = false;
		for (rapidjson::SizeType profileIndex : profileOrder)
		{
			rapidjson::Value& profile = profilesDocument[profileIndex];
			if (!profile.HasMember(kAvisoPresetsKey))
				continue;

			rapidjson::Value& metadata = EnsureMetadataValue(profilesDocument);
			// EnsureMetadataValue may append to the array, but existing element
			// storage remains valid only after reacquiring the indexed profile.
			rapidjson::Value& currentProfile = profilesDocument[profileIndex];
			if (!MergeProfilePresetRootIntoMetadata(
				metadata,
				currentProfile,
				activeAirport,
				profilesDocument.GetAllocator()))
			{
				continue;
			}

			currentProfile.RemoveMember(kAvisoPresetsKey);
			migrated = true;
		}
		return migrated;
	}

	bool MergeAvisoPresetRoot(
		rapidjson::Value& destinationProfile,
		const rapidjson::Value& authoritativeProfile,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (!destinationProfile.IsObject() || !authoritativeProfile.IsObject())
			return false;

		if (!authoritativeProfile.HasMember(kAvisoPresetsKey))
		{
			if (destinationProfile.HasMember(kAvisoPresetsKey))
				destinationProfile.RemoveMember(kAvisoPresetsKey);
			return true;
		}

		rapidjson::Value clonedRoot;
		if (!CloneJsonValue(authoritativeProfile[kAvisoPresetsKey], allocator, clonedRoot))
			return false;

		if (destinationProfile.HasMember(kAvisoPresetsKey))
			destinationProfile[kAvisoPresetsKey] = clonedRoot;
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(kAvisoPresetsKey, allocator);
			destinationProfile.AddMember(keyValue, clonedRoot, allocator);
		}
		return true;
	}

	bool MergeLatestAvisoPresetRoots(
		rapidjson::Document& destination,
		const rapidjson::Document& authoritative,
		const std::vector<CConfig::ProfileSaveIdentity>& profileIdentities)
	{
		if (!destination.IsArray() || !authoritative.IsArray())
			return false;

		// Airport preset state is file-global. Once the canonical metadata root
		// exists, it is authoritative and legacy profile copies must not be
		// resurrected by a stale Control Center or radar-screen save.
		const rapidjson::Value* authoritativeMetadata = FindMetadataValue(authoritative);
		if (authoritativeMetadata != nullptr &&
			authoritativeMetadata->HasMember(kAvisoPresetsKey))
		{
			rapidjson::Value& destinationMetadata = EnsureMetadataValue(destination);
			if (!MergeAvisoPresetRoot(
				destinationMetadata,
				*authoritativeMetadata,
				destination.GetAllocator()))
			{
				return false;
			}

			for (rapidjson::SizeType index = 0; index < destination.Size(); ++index)
			{
				rapidjson::Value& destinationProfile = destination[index];
				if (!IsProfileEntry(destinationProfile))
					continue;

				const std::string currentName = destinationProfile["name"].GetString();
				const CConfig::ProfileSaveIdentity* identity = nullptr;
				for (const CConfig::ProfileSaveIdentity& candidate : profileIdentities)
				{
					if (EqualsNoCaseAscii(candidate.currentName, currentName))
					{
						identity = &candidate;
						break;
					}
				}

				const rapidjson::Value* authoritativeProfile = nullptr;
				if (identity == nullptr)
					authoritativeProfile = FindProfileByName(authoritative, currentName);
				else if (!identity->persistedName.empty())
					authoritativeProfile = FindProfileByName(
						authoritative,
						identity->persistedName);

				if (authoritativeProfile != nullptr &&
					authoritativeProfile->HasMember(kAvisoPresetsKey))
				{
					if (!MergeAvisoPresetRoot(
						destinationProfile,
						*authoritativeProfile,
						destination.GetAllocator()))
					{
						return false;
					}
				}
				else if (destinationProfile.HasMember(kAvisoPresetsKey))
				{
					destinationProfile.RemoveMember(kAvisoPresetsKey);
				}
			}
			return true;
		}

		for (rapidjson::SizeType index = 0; index < destination.Size(); ++index)
		{
			rapidjson::Value& destinationProfile = destination[index];
			if (!IsProfileEntry(destinationProfile))
				continue;

			const std::string currentName = destinationProfile["name"].GetString();
			const CConfig::ProfileSaveIdentity* identity = nullptr;
			for (const CConfig::ProfileSaveIdentity& candidate : profileIdentities)
			{
				if (EqualsNoCaseAscii(candidate.currentName, currentName))
				{
					identity = &candidate;
					break;
				}
			}

			// A supplied empty persisted name explicitly marks a newly-created
			// profile. Otherwise use the stable pre-edit name so a rename cannot
			// detach this profile from a newer preset root on disk.
			const rapidjson::Value* authoritativeProfile = nullptr;
			if (identity == nullptr)
				authoritativeProfile = FindProfileByName(authoritative, currentName);
			else if (!identity->persistedName.empty())
				authoritativeProfile = FindProfileByName(authoritative, identity->persistedName);
			if (authoritativeProfile != nullptr &&
				!MergeAvisoPresetRoot(
					destinationProfile,
					*authoritativeProfile,
					destination.GetAllocator()))
			{
				return false;
			}
		}
		return true;
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

	bool PersistConfigDocument(
		const std::string& destination,
		const rapidjson::Document& source)
	{
		if (!source.IsArray())
			return false;

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		source.Accept(writer);

		const std::string serializedJson(buffer.GetString(), buffer.Size());
		rapidjson::Document validationDocument;
		if (!ParseValidatedArray(serializedJson, validationDocument))
			return false;

		std::string temporaryPath;
		if (!WriteTemporaryFile(destination, serializedJson, temporaryPath))
			return false;

		std::string persistedJson;
		rapidjson::Document persistedValidationDocument;
		if (!ReadFileContents(temporaryPath, persistedJson) ||
			persistedJson != serializedJson ||
			!ParseValidatedArray(persistedJson, persistedValidationDocument))
		{
			::DeleteFileA(temporaryPath.c_str());
			return false;
		}

		if (!BackupDestinationIfPresent(destination))
		{
			::DeleteFileA(temporaryPath.c_str());
			return false;
		}

		if (!::MoveFileExA(
			temporaryPath.c_str(),
			destination.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileA(temporaryPath.c_str());
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

	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	gLiveConfigs.push_back(this);
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

bool CConfig::saveConfig(const vector<ProfileSaveIdentity>& profileIdentities)
{
	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	if (!document.IsArray())
		return false;

	// Preset stores are immediate, shared state. An ordinary save may originate
	// from a screen whose full profile snapshot predates another screen's preset
	// transaction, so refresh only these roots before persisting everything else.
	std::string latestJson;
	Document latestDocument;
	if (ReadFileContents(config_path, latestJson) &&
		ParseValidatedArray(latestJson, latestDocument) &&
		!MergeLatestAvisoPresetRoots(document, latestDocument, profileIdentities))
	{
		return false;
	}

	return PersistConfigDocument(config_path, document);
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

	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);

	// Deliberately load only the profiles document. A malformed or unavailable
	// maps file must not make an otherwise valid preset transaction fail.
	std::string latestJson;
	Document latestDocument;
	if (!ReadFileContents(config_path, latestJson) ||
		!ParseValidatedArray(latestJson, latestDocument))
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

	if ((migrated || action == AvisoPresetTransactionAction::Save) &&
		!PersistConfigDocument(config_path, latestDocument))
	{
		return false;
	}

	bool ownerMerged = false;
	for (CConfig* liveConfig : gLiveConfigs)
	{
		if (liveConfig == nullptr ||
			!EqualsNoCaseAscii(liveConfig->config_path, config_path))
			continue;

		if (!MergeLatestAvisoPresetRoots(
			liveConfig->document,
			latestDocument,
			{}))
		{
			return false;
		}
		if (liveConfig == this)
			ownerMerged = true;
	}

	return ownerMerged;
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
	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	gLiveConfigs.erase(
		std::remove(gLiveConfigs.begin(), gLiveConfigs.end(), this),
		gLiveConfigs.end());
}
