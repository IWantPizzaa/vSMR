#include "platform/windows/PrecompiledHeader.hpp"
#include "config/RuntimeConfig.hpp"
#include "shared/JsonInputLimits.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
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
	constexpr int kCurrentProfileSchemaVersion = 2;
	constexpr int kCurrentMetadataSchemaVersion = 1;
	constexpr size_t kMaximumConfigFileBytes =
		CConfig::MaximumSerializedInputBytes;
	constexpr size_t kMaximumConfigStringBytes = 64U * 1024U;
	constexpr size_t kMaximumConfigJsonDepth = 64U;
	constexpr size_t kMaximumConfigJsonValues = 500000U;
	constexpr size_t kMaximumConfigContainerEntries = 100000U;
	constexpr size_t kMaximumProfiles = 256U;
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
		if (!profilesDocument.IsArray())
			return nullptr;

		for (rapidjson::SizeType index = 0; index < profilesDocument.Size(); ++index)
		{
			rapidjson::Value& entry = profilesDocument[index];
			if (IsMetadataEntry(entry))
				return &entry[kMetadataWrapperKey];
		}
		return nullptr;
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

	bool ValidateJsonTextLimits(const std::string& contents, std::string* error)
	{
		auto fail = [&](const char* message)
		{
			if (error != nullptr)
				*error = message;
			return false;
		};
		if (contents.size() > kMaximumConfigFileBytes)
			return fail("The JSON file exceeds the 16 MB configuration limit.");

		size_t depth = 0;
		size_t stringBytes = 0;
		bool inString = false;
		bool escaped = false;
		for (const char character : contents)
		{
			if (inString)
			{
				if (escaped)
					escaped = false;
				else if (character == '\\')
					escaped = true;
				else if (character == '"')
				{
					inString = false;
					continue;
				}
				if (++stringBytes > kMaximumConfigStringBytes)
					return fail("The JSON file contains a string longer than 64 KB.");
				continue;
			}

			if (character == '"')
			{
				inString = true;
				stringBytes = 0;
			}
			else if (character == '{' || character == '[')
			{
				if (++depth > kMaximumConfigJsonDepth)
					return fail("The JSON file exceeds the maximum nesting depth of 64.");
			}
			else if ((character == '}' || character == ']') && depth > 0)
			{
				--depth;
			}
		}
		return true;
	}

	bool ValidateJsonValueLimits(
		const rapidjson::Value& value,
		size_t depth,
		size_t& valueCount,
		std::string* error)
	{
		auto fail = [&](const char* message)
		{
			if (error != nullptr)
				*error = message;
			return false;
		};
		if (depth > kMaximumConfigJsonDepth)
			return fail("The JSON document exceeds the maximum nesting depth of 64.");
		if (++valueCount > kMaximumConfigJsonValues)
			return fail("The JSON document contains too many values.");
		if (value.IsString() && value.GetStringLength() > kMaximumConfigStringBytes)
			return fail("The JSON document contains a string longer than 64 KB.");
		if (value.IsArray())
		{
			if (value.Size() > kMaximumConfigContainerEntries)
				return fail("The JSON document contains an oversized array.");
			for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
			{
				if (!ValidateJsonValueLimits(value[index], depth + 1, valueCount, error))
					return false;
			}
		}
		else if (value.IsObject())
		{
			size_t memberCount = 0;
			for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
			{
				if (++memberCount > kMaximumConfigContainerEntries)
					return fail("The JSON document contains an oversized object.");
				if (member->name.GetStringLength() > kMaximumConfigStringBytes)
					return fail("The JSON document contains a property name longer than 64 KB.");
				if (!ValidateJsonValueLimits(member->value, depth + 1, valueCount, error))
					return false;
			}
		}
		return true;
	}

	bool ValidateJsonDocumentLimits(const rapidjson::Value& value, std::string* error)
	{
		size_t valueCount = 0;
		return ValidateJsonValueLimits(value, 0, valueCount, error);
	}

	bool ValidateJsonStreamingLimits(
		const std::string& contents,
		std::string* error)
	{
		VsmrJsonInputLimits::Limits limits;
		limits.maximumDepth = kMaximumConfigJsonDepth;
		limits.maximumValues = kMaximumConfigJsonValues;
		limits.maximumContainerEntries = kMaximumConfigContainerEntries;
		limits.maximumStringBytes = kMaximumConfigStringBytes;
		std::string validationError;
		if (VsmrJsonInputLimits::Validate(contents, limits, validationError))
			return true;
		if (error != nullptr)
			*error = validationError;
		return false;
	}

	bool ReadFileContents(
		const std::string& path,
		std::string& contents,
		std::string* error = nullptr)
	{
		contents.clear();
		if (error != nullptr)
			error->clear();
		std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
		if (!input.is_open())
		{
			if (error != nullptr)
				*error = "The file is missing or cannot be opened.";
			return false;
		}

		input.seekg(0, std::ios::end);
		const std::streamoff length = input.tellg();
		if (length < 0)
		{
			if (error != nullptr)
				*error = "The file size could not be determined.";
			return false;
		}
		if (static_cast<unsigned long long>(length) > kMaximumConfigFileBytes)
		{
			if (error != nullptr)
				*error = "The file exceeds the 16 MB configuration limit.";
			return false;
		}
		input.seekg(0, std::ios::beg);
		if (!input.good())
		{
			if (error != nullptr)
				*error = "The file could not be read.";
			return false;
		}

		contents.resize(static_cast<size_t>(length));
		if (length > 0)
		{
			input.read(contents.data(), static_cast<std::streamsize>(length));
			if (input.gcount() != length)
			{
				contents.clear();
				if (error != nullptr)
					*error = "The file changed or became unreadable while loading.";
				return false;
			}
		}
		if (input.peek() != std::char_traits<char>::eof())
		{
			contents.clear();
			if (error != nullptr)
				*error = "The file changed while loading.";
			return false;
		}
		if (!ValidateJsonTextLimits(contents, error))
		{
			contents.clear();
			return false;
		}
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
		rapidjson::Document& validationDocument,
		std::string* error = nullptr)
	{
		if (!ValidateJsonTextLimits(serializedJson, error))
			return false;
		if (!ValidateJsonStreamingLimits(serializedJson, error))
			return false;
		validationDocument.Parse<0>(serializedJson.c_str());
		if (validationDocument.HasParseError() || !validationDocument.IsArray())
		{
			if (error != nullptr)
				*error = "The JSON document must be a valid array.";
			return false;
		}
		return ValidateJsonDocumentLimits(validationDocument, error);
	}

	std::string ContentRevision(const std::string& contents)
	{
		// A revision token only has to detect a stale editor snapshot; it is not a
		// security primitive. FNV-1a is deterministic, fast, and avoids adding a
		// crypto dependency to the 32-bit EuroScope plugin.
		std::uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char byte : contents)
		{
			hash ^= static_cast<std::uint64_t>(byte);
			hash *= 1099511628211ULL;
		}
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16) << hash;
		return output.str();
	}

	std::string FileRevision(const std::string& path)
	{
		std::string contents;
		return ReadFileContents(path, contents)
			? ContentRevision(contents)
			: "missing";
	}

	bool HasFile(const std::string& path)
	{
		const std::filesystem::path nativePath = std::filesystem::u8path(path);
		const DWORD attributes = ::GetFileAttributesW(nativePath.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	bool RequireObjectMember(
		const rapidjson::Value& object,
		const char* key,
		const std::string& context,
		std::string& error)
	{
		if (!object.HasMember(key) || !object[key].IsObject())
		{
			error = context + " requires an object named '" + key + "'.";
			return false;
		}
		return true;
	}

	bool ValidateOptionalMemberType(
		const rapidjson::Value& object,
		const char* key,
		rapidjson::Type expectedType,
		const std::string& context,
		std::string& error)
	{
		if (!object.HasMember(key))
			return true;
		if (object[key].GetType() == expectedType)
			return true;
		error = context + " member '" + key + "' has the wrong JSON type.";
		return false;
	}

	void EnsureEmptyObjectMember(
		rapidjson::Value& object,
		const char* key,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key))
			return;
		rapidjson::Value memberKey;
		memberKey.SetString(key, allocator);
		rapidjson::Value member(rapidjson::kObjectType);
		object.AddMember(memberKey, member, allocator);
	}

	void SetIntegerMember(
		rapidjson::Value& object,
		const char* key,
		int value,
		rapidjson::Document::AllocatorType& allocator)
	{
		if (object.HasMember(key))
			object[key].SetInt(value);
		else
		{
			rapidjson::Value memberKey;
			memberKey.SetString(key, allocator);
			rapidjson::Value memberValue(value);
			object.AddMember(memberKey, memberValue, allocator);
		}
	}

	bool ValidatePresetRect(
		const rapidjson::Value& object,
		const std::string& context,
		std::string& error)
	{
		if (!object.IsObject())
		{
			error = context + " must be an object.";
			return false;
		}
		const char* coordinates[] = { "left", "top", "right", "bottom" };
		bool anyCoordinate = false;
		for (const char* key : coordinates)
		{
			if (!object.HasMember(key))
				continue;
			anyCoordinate = true;
			if (!object[key].IsInt())
			{
				error = context + " rectangle member '" + key + "' must be an integer.";
				return false;
			}
		}
		if (!anyCoordinate)
			return true;
		for (const char* key : coordinates)
		{
			if (!object.HasMember(key))
			{
				error = context + " contains an incomplete rectangle.";
				return false;
			}
		}
		if (object["right"].GetInt() <= object["left"].GetInt() ||
			object["bottom"].GetInt() <= object["top"].GetInt())
		{
			error = context + " rectangle must have positive width and height.";
			return false;
		}
		return true;
	}

	bool ValidatePresetStore(
		const rapidjson::Value& store,
		const std::string& context,
		std::string& error)
	{
		if (!store.IsObject())
		{
			error = context + " must be an object.";
			return false;
		}
		if (store.HasMember(kDefaultPresetKey) && !store[kDefaultPresetKey].IsString())
		{
			error = context + " default preset must be a string.";
			return false;
		}
		if (!store.HasMember(kPresetItemsKey))
			return true;
		if (!store[kPresetItemsKey].IsArray())
		{
			error = context + " items must be an array.";
			return false;
		}
		std::unordered_set<std::string> names;
		const rapidjson::Value& items = store[kPresetItemsKey];
		for (rapidjson::SizeType index = 0; index < items.Size(); ++index)
		{
			const rapidjson::Value& preset = items[index];
			const std::string presetContext = context + " preset #" + std::to_string(index + 1);
			const std::string name = ReadStringMember(preset, "name");
			if (!preset.IsObject() || TrimAsciiWhitespace(name).empty())
			{
				error = presetContext + " requires a non-empty name.";
				return false;
			}
			std::string normalizedName = TrimAsciiWhitespace(name);
			std::transform(normalizedName.begin(), normalizedName.end(), normalizedName.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			if (!names.insert(normalizedName).second)
			{
				error = context + " contains duplicate preset names.";
				return false;
			}
			for (const char* windowKey : { "secondary", "weather", "timer" })
			{
				if (preset.HasMember(windowKey) &&
					!ValidatePresetRect(preset[windowKey], presetContext + " " + windowKey, error))
					return false;
			}
			if (preset.HasMember("srw"))
			{
				if (!preset["srw"].IsArray())
				{
					error = presetContext + " SRW state must be an array.";
					return false;
				}
				for (rapidjson::SizeType windowIndex = 0; windowIndex < preset["srw"].Size(); ++windowIndex)
				{
					if (!ValidatePresetRect(
						preset["srw"][windowIndex],
						presetContext + " SRW #" + std::to_string(windowIndex + 1),
						error))
						return false;
				}
			}
		}
		return true;
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
		const bool hasFlatItems = sourceRoot.HasMember(kPresetItemsKey);
		const bool hasFlatDefault = sourceRoot.HasMember(kDefaultPresetKey);
		std::string flatAirportKey;
		if (hasFlatItems || hasFlatDefault)
		{
			// Resolve ownership before mutating the canonical destination. A mixed
			// scoped/unscoped legacy root with no explicit owner must be an atomic
			// no-op, not a partially repeated migration.
			flatAirportKey = NormalizeAirportKey(
				ReadStringMember(sourceRoot, "airport"));
			if (flatAirportKey.empty())
				flatAirportKey = NormalizeAirportKey(
					ReadStringMember(profile, "airport"));
			if (flatAirportKey.size() != 4 ||
				!std::all_of(
					flatAirportKey.begin(),
					flatAirportKey.end(),
					[](unsigned char c) { return std::isalnum(c) != 0; }))
			{
				return false;
			}
		}
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

		if (hasFlatItems || hasFlatDefault)
		{
			// Flat legacy stores have no inherent airport scope. Only migrate one
			// when the old data explicitly names its airport; the currently selected
			// EuroScope airport is not evidence of ownership.
			rapidjson::Value& destinationSection =
				EnsureObjectMember(
					destinationAirports,
					flatAirportKey.c_str(),
					allocator);
			if (!MergePresetSection(
				destinationSection,
				sourceRoot,
				sourceProfile,
				allocator))
			{
				return false;
			}
		}
		(void)activeAirport;

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
			const std::filesystem::path nativeTemporaryPath =
				std::filesystem::u8path(temporaryPath);
			output = ::CreateFileW(
				nativeTemporaryPath.c_str(),
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
			::DeleteFileW(std::filesystem::u8path(temporaryPath).c_str());
			temporaryPath.clear();
		}

		return succeeded;
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

		// Writing and verifying the replacement before touching the current file
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
			::DeleteFileW(std::filesystem::u8path(temporaryPath).c_str());
			return false;
		}

		if (!::MoveFileExW(
			std::filesystem::u8path(temporaryPath).c_str(),
			std::filesystem::u8path(destination).c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileW(std::filesystem::u8path(temporaryPath).c_str());
			return false;
		}

		return true;
	}
}

bool CConfig::validateSerializedInputLimits(
	const string& serializedJson,
	string& error)
{
	error.clear();
	return ValidateJsonTextLimits(serializedJson, &error) &&
		ValidateJsonStreamingLimits(serializedJson, &error);
}

bool CConfig::validateAndMigrateProfilesDocument(
	rapidjson::Document& profilesDocument,
	string& error,
	bool& migrated)
{
	error.clear();
	migrated = false;
	if (!ValidateJsonDocumentLimits(profilesDocument, &error))
		return false;
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
	if (metadata->HasMember(kVacdmKey))
	{
		if (!(*metadata)[kVacdmKey].IsObject() ||
			((*metadata)[kVacdmKey].HasMember(kVacdmServerUrlKey) &&
				!(*metadata)[kVacdmKey][kVacdmServerUrlKey].IsString()))
		{
			error = "_vsmr.vacdm.server_url must be a string.";
			return false;
		}
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

	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	gLiveConfigs.push_back(this);
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
	if (!validateAndMigrateProfilesDocument(parsed, error, migrated))
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
	using_backup = false;
	last_load_message.clear();

	auto parseCandidate = [&](
		const std::string& serializedJson,
		Document& candidate,
		map<string, rapidjson::SizeType>& candidateProfiles,
		bool& migrated,
		std::string& error) -> bool
	{
		if (!ParseValidatedArray(serializedJson, candidate, &error))
		{
			if (error.empty())
				error = "The profiles file is not a valid JSON array.";
			return false;
		}
		if (!validateAndMigrateProfilesDocument(candidate, error, migrated))
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
	const bool mainValid = mainRead && parseCandidate(
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
			using_backup = false;
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
		using_backup = false;
		if (mainMigrated)
			last_load_message = "Profiles were migrated transactionally to schema version 2.";
		return true;
	}

	const std::string failure = mainRead
		? (mainError.empty() ? "The profiles file is invalid." : mainError)
		: (mainReadError.empty()
			? "The configured profiles file is missing or cannot be read."
			: mainReadError);

	// Falling back to the last validated backup without replacing the primary
	std::string backupJson;
	std::string backupError;
	Document backupCandidate(&document.GetAllocator());
	map<string, rapidjson::SizeType> backupProfiles;
	bool backupMigrated = false;
	if (ReadFileContents(config_path + kBackupSuffix, backupJson) &&
		parseCandidate(
			backupJson,
			backupCandidate,
			backupProfiles,
			backupMigrated,
			backupError))
	{
		AdoptDocument(document, backupCandidate);
		profiles.swap(backupProfiles);
		active_profile = profiles.begin()->second;
		using_backup = true;
		last_load_message = failure +
			" A validated .bak copy is active in memory. Restore that backup or bundled defaults from Settings before saving other changes.";
		ReportLoadFailure("configuration (recovered from backup)");
		return false;
	}

	last_load_message = failure +
		" No validated backup is available. Restore bundled defaults from Settings.";
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
		if (startsWith(name1.c_str(), runwayName) || startsWith(name2.c_str(), runwayName))
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

bool CConfig::isUsingBackup() const
{
	return using_backup;
}

bool CConfig::isBackupAvailable() const
{
	std::string backupJson;
	if (!ReadFileContents(config_path + kBackupSuffix, backupJson))
		return false;
	Document candidate;
	if (!ParseValidatedArray(backupJson, candidate))
		return false;
	std::string error;
	bool migrated = false;
	return validateAndMigrateProfilesDocument(candidate, error, migrated);
}

std::int64_t CConfig::getBackupModifiedUnixSeconds() const
{
	std::error_code error;
	const std::filesystem::file_time_type modified =
		std::filesystem::last_write_time(
			std::filesystem::u8path(config_path + kBackupSuffix),
			error);
	if (error)
		return 0;

	const std::chrono::system_clock::time_point systemModified =
		std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			modified - std::filesystem::file_time_type::clock::now() +
			std::chrono::system_clock::now());
	const std::int64_t seconds =
		std::chrono::duration_cast<std::chrono::seconds>(
			systemModified.time_since_epoch()).count();
	return seconds > 0 ? seconds : 0;
}

bool CConfig::restoreBackup(string& error)
{
	error.clear();
	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	const std::string activeBefore = getActiveProfileName();
	const std::string backupPath = config_path + kBackupSuffix;
	std::string backupJson;
	if (!ReadFileContents(backupPath, backupJson))
	{
		error = "No readable profiles backup is available.";
		return false;
	}

	Document candidate;
	bool migrated = false;
	if (!ParseValidatedArray(backupJson, candidate, &error) ||
		!validateAndMigrateProfilesDocument(candidate, error, migrated))
	{
		if (error.empty())
			error = "The profiles backup contains invalid JSON.";
		return false;
	}

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	candidate.Accept(writer);
	const std::string restoredJson(buffer.GetString(), buffer.Size());
	std::string temporaryPath;
	if (!WriteTemporaryFile(config_path, restoredJson, temporaryPath) ||
		!::MoveFileExW(
			std::filesystem::u8path(temporaryPath).c_str(),
			std::filesystem::u8path(config_path).c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		if (!temporaryPath.empty())
			::DeleteFileW(std::filesystem::u8path(temporaryPath).c_str());
		error = "Unable to restore the profiles backup. Check folder permissions.";
		return false;
	}

	map<string, rapidjson::SizeType> restoredProfiles;
	for (rapidjson::SizeType index = 0; index < candidate.Size(); ++index)
	{
		if (IsProfileEntry(candidate[index]))
			restoredProfiles[trimProfileName(candidate[index]["name"].GetString())] = index;
	}
	Document replacement(&document.GetAllocator());
	replacement.Parse<0>(restoredJson.c_str());
	if (replacement.HasParseError() || restoredProfiles.empty())
	{
		error = "The profiles backup was restored but could not be activated. Reload vSMR.";
		return false;
	}
	AdoptDocument(document, replacement);
	profiles.swap(restoredProfiles);
	active_profile = profiles.begin()->second;
	if (!activeBefore.empty())
		setActiveProfile(activeBefore);
	config_revision = ContentRevision(restoredJson);
	config_healthy = true;
	using_backup = false;
	last_load_message = "Profiles backup restored.";
	return true;
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

bool CConfig::saveConfig(
	const vector<ProfileSaveIdentity>& profileIdentities,
	const string& expectedRevision,
	string* error,
	bool allowRecoveryReplacement)
{
	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
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
			*error = using_backup
				? "A backup is active in memory. Restore it from Settings before saving other changes."
				: "The profiles source is invalid. Restore a backup or bundled defaults from Settings before saving.";
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
		!validateAndMigrateProfilesDocument(validated, validationError, migrated))
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
		if (!ParseValidatedArray(currentJson, latestDocument, &latestError) ||
			!validateAndMigrateProfilesDocument(latestDocument, latestError, latestMigrated))
		{
			// An explicitly confirmed recovery save may replace a damaged primary
			// file while ordinary saves still fail closed on external corruption
			if (config_healthy)
			{
				if (error != nullptr)
					*error = "The profiles file became invalid on disk. Reload or restore a backup before saving.";
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
	using_backup = false;
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
	const std::string previousRevision = ContentRevision(latestJson);
	bool schemaMigrated = false;
	std::string validationError;
	if (!validateAndMigrateProfilesDocument(
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
	for (CConfig* liveConfig : gLiveConfigs)
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

	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	std::string latestJson;
	Document latestDocument;
	if (!ReadFileContents(config_path, latestJson) ||
		!ParseValidatedArray(latestJson, latestDocument))
	{
		error = "The current profiles file is unavailable or invalid.";
		return false;
	}
	const std::string previousRevision = ContentRevision(latestJson);
	bool schemaMigrated = false;
	if (!validateAndMigrateProfilesDocument(
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
	for (CConfig* liveConfig : gLiveConfigs)
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
	std::lock_guard<std::mutex> writeGuard(gConfigSaveMutex);
	gLiveConfigs.erase(
		std::remove(gLiveConfigs.begin(), gLiveConfigs.end(), this),
		gLiveConfigs.end());
}
