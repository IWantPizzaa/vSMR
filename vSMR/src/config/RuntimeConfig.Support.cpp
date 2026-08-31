#include "platform/windows/PrecompiledHeader.hpp"
#include "config/RuntimeConfig.Internal.hpp"
#include "shared/JsonInputLimits.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>

namespace VsmrRuntimeConfigInternal
{
	volatile LONG gTemporaryFileSequence = 0;

	std::mutex& ConfigSaveMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	std::vector<CConfig*>& LiveConfigs()
	{
		static std::vector<CConfig*> configs;
		return configs;
	}

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

	int ReadColorComponent(const rapidjson::Value& colorValue, const char* key, int fallback)
	{
		if (!colorValue.IsObject() || key == nullptr || !colorValue.HasMember(key) || !colorValue[key].IsInt())
			return fallback;
		return std::clamp(colorValue[key].GetInt(), 0, 255);
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

	static bool ValidateJsonStructureLimits(
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

	static bool ValidateJsonInputSize(
		std::uint64_t byteCount,
		std::string* error)
	{
		if (byteCount <= kMaximumConfigFileBytes)
			return true;
		if (error != nullptr)
			*error = "The JSON file exceeds the 16 MB configuration limit.";
		return false;
	}

	bool ValidateJsonInputLimits(
		const std::string& contents,
		std::string* error)
	{
		if (!ValidateJsonInputSize(contents.size(), error))
			return false;

		return ValidateJsonStructureLimits(contents, error);
	}

	bool ReadFileContents(
		const std::string& path,
		std::string& contents,
		std::string* error)
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
		if (!ValidateJsonInputSize(static_cast<std::uint64_t>(length), error))
			return false;
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

	static bool ParseArrayAfterInputSizeValidation(
		const std::string& serializedJson,
		rapidjson::Document& validationDocument,
		std::string* error)
	{
		if (!ValidateJsonStructureLimits(serializedJson, error))
			return false;
		validationDocument.Parse<0>(serializedJson.c_str());
		if (validationDocument.HasParseError() || !validationDocument.IsArray())
		{
			if (error != nullptr)
				*error = "The JSON document must be a valid array.";
			return false;
		}
		// Resource limits were enforced by the streaming pass before this DOM was
		// allocated; profile callers perform only schema and migration checks next.
		return true;
	}

	bool ParseValidatedArray(
		const std::string& serializedJson,
		rapidjson::Document& validationDocument,
		std::string* error)
	{
		if (!ValidateJsonInputSize(serializedJson.size(), error))
			return false;
		return ParseArrayAfterInputSizeValidation(
			serializedJson,
			validationDocument,
			error);
	}

	bool ParseSizeBoundedArray(
		const std::string& serializedJson,
		rapidjson::Document& validationDocument,
		std::string* error)
	{
		// ReadFileContents already rejected oversized or concurrently growing files.
		// Keep that byte-boundary check single while retaining the structural pass.
		return ParseArrayAfterInputSizeValidation(
			serializedJson,
			validationDocument,
			error);
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
					CloneJsonValue(sourcePreset, clonedPreset, allocator);
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
					CloneJsonValue(sourcePreset, clonedPreset, allocator);
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
		CloneJsonValue(authoritativeProfile[kAvisoPresetsKey], clonedRoot, allocator);

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
			!ParseSizeBoundedArray(persistedJson, persistedValidationDocument))
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
