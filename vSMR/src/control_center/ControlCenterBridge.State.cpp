#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "aviso/AvisoDocumentModel.hpp"
#include "config/RuntimeConfig.hpp"
#include "platform/windows/network/HttpHelper.hpp"

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace VsmrControlCenterBridgeInternal
{
	std::mutex& BridgeSaveTransactionMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	std::string TrimAscii(std::string value)
	{
		auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
			return !isSpace(static_cast<unsigned char>(c));
		}));
		value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
			return !isSpace(static_cast<unsigned char>(c));
		}).base(), value.end());
		return value;
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	std::string UpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	bool EqualsNoCase(const std::string& left, const std::string& right)
	{
		return LowerAscii(left) == LowerAscii(right);
	}

	std::string NormalizeAirportCandidate(std::string value)
	{
		value = UpperAscii(TrimAscii(value));
		if (value.size() != 4 || value == "JSON" || value == "AVIS" || value == "GEOJ")
			return "";
		for (const unsigned char character : value)
		{
			if (std::isalnum(character) == 0)
				return "";
		}
		return value;
	}

	std::string DetectAvisoAirport(
		const rapidjson::Value& document,
		std::string sourceHint)
	{
		const char* keys[] = {
			"icao", "icao_code", "airport_icao", "airport_code", "airport", "active_airport"
		};
		auto findInObject = [&](const rapidjson::Value& object) -> std::string
		{
			if (!object.IsObject())
				return "";
			for (const char* key : keys)
			{
				if (!object.HasMember(key) || !object[key].IsString())
					continue;
				const std::string candidate = NormalizeAirportCandidate(object[key].GetString());
				if (!candidate.empty())
					return candidate;
			}
			return "";
		};

		std::string airport = findInObject(document);
		if (!airport.empty())
			return airport;
		if (document.IsObject() && document.HasMember("metadata"))
		{
			airport = findInObject(document["metadata"]);
			if (!airport.empty())
				return airport;
		}
		if (document.IsObject() && document.HasMember("properties"))
		{
			airport = findInObject(document["properties"]);
			if (!airport.empty())
				return airport;
		}

		std::replace(sourceHint.begin(), sourceHint.end(), '\\', '/');
		const size_t suffix = sourceHint.find_first_of("?#");
		if (suffix != std::string::npos)
			sourceHint.resize(suffix);
		const size_t slash = sourceHint.find_last_of('/');
		if (slash != std::string::npos)
			sourceHint = sourceHint.substr(slash + 1);

		// Work only with the basename stem so URL path segments and extensions
		// such as blob/json/geojson can never be mistaken for an airport. Accept
		// either a bare ICAO filename (LFPO.geojson) or the ICAO token directly
		// beside AVISO (LFPO_AVISO.geojson / AVISO_LFPO.geojson).
		const size_t extension = sourceHint.find_last_of('.');
		if (extension != std::string::npos)
			sourceHint.resize(extension);
		sourceHint = UpperAscii(sourceHint);

		std::vector<std::string> tokens;
		std::string token;
		for (const unsigned char character : sourceHint)
		{
			if (std::isalnum(character) != 0)
			{
				token.push_back(static_cast<char>(character));
				continue;
			}
			if (!token.empty())
			{
				tokens.push_back(std::move(token));
				token.clear();
			}
		}
		if (!token.empty())
			tokens.push_back(std::move(token));

		if (tokens.size() == 1)
			return NormalizeAirportCandidate(tokens.front());
		for (size_t index = 0; index < tokens.size(); ++index)
		{
			if (tokens[index] != "AVISO")
				continue;
			if (index > 0)
			{
				airport = NormalizeAirportCandidate(tokens[index - 1]);
				if (!airport.empty())
					return airport;
			}
			if (index + 1 < tokens.size())
			{
				airport = NormalizeAirportCandidate(tokens[index + 1]);
				if (!airport.empty())
					return airport;
			}
		}
		return "";
	}

	std::string ReadString(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsString())
			return "";
		return object[key].GetString();
	}

	bool ReadBool(const rapidjson::Value& object, const char* key, bool fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsBool())
			return fallback;
		return object[key].GetBool();
	}

	int ReadInt(const rapidjson::Value& object, const char* key, int fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsInt())
			return fallback;
		return object[key].GetInt();
	}

	void AddString(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator);
	void SetStringMember(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator);
	void SetBoolMember(
		rapidjson::Value& object,
		const char* key,
		bool value,
		Allocator& allocator);

	void AddString(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator)
	{
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value stringValue;
		stringValue.SetString(
			value.c_str(),
			static_cast<rapidjson::SizeType>(value.size()),
			allocator);
		object.AddMember(keyValue, stringValue, allocator);
	}

	void AddInt64(
		rapidjson::Value& object,
		const char* key,
		std::int64_t value,
		Allocator& allocator)
	{
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value number;
		number.SetInt64(value);
		object.AddMember(keyValue, number, allocator);
	}

	void SetStringMember(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator)
	{
		rapidjson::Value stringValue;
		stringValue.SetString(
			value.c_str(),
			static_cast<rapidjson::SizeType>(value.size()),
			allocator);
		if (object.HasMember(key))
			object[key] = stringValue;
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			object.AddMember(keyValue, stringValue, allocator);
		}
	}

	void SetBoolMember(
		rapidjson::Value& object,
		const char* key,
		bool value,
		Allocator& allocator)
	{
		if (object.HasMember(key))
			object[key].SetBool(value);
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value boolValue;
			boolValue.SetBool(value);
			object.AddMember(keyValue, boolValue, allocator);
		}
	}

	void CloneJsonValue(
		const rapidjson::Value& source,
		rapidjson::Value& destination,
		Allocator& allocator)
	{
		if (source.IsObject())
		{
			destination.SetObject();
			for (rapidjson::Value::ConstMemberIterator member = source.MemberBegin();
				member != source.MemberEnd();
				++member)
			{
				rapidjson::Value key;
				key.SetString(
					member->name.GetString(),
					member->name.GetStringLength(),
					allocator);
				rapidjson::Value value;
				CloneJsonValue(member->value, value, allocator);
				destination.AddMember(key, value, allocator);
			}
			return;
		}
		if (source.IsArray())
		{
			destination.SetArray();
			for (rapidjson::SizeType index = 0; index < source.Size(); ++index)
			{
				rapidjson::Value value;
				CloneJsonValue(source[index], value, allocator);
				destination.PushBack(value, allocator);
			}
			return;
		}
		if (source.IsString())
		{
			destination.SetString(
				source.GetString(),
				source.GetStringLength(),
				allocator);
			return;
		}
		if (source.IsBool()) { destination.SetBool(source.GetBool()); return; }
		if (source.IsInt()) { destination.SetInt(source.GetInt()); return; }
		if (source.IsUint()) { destination.SetUint(source.GetUint()); return; }
		if (source.IsInt64()) { destination.SetInt64(source.GetInt64()); return; }
		if (source.IsUint64()) { destination.SetUint64(source.GetUint64()); return; }
		if (source.IsDouble()) { destination.SetDouble(source.GetDouble()); return; }
		destination.SetNull();
	}

	rapidjson::Value& EnsureObjectMember(
		rapidjson::Value& object,
		const char* key,
		Allocator& allocator)
	{
		if (!object.IsObject())
			object.SetObject();
		if (!object.HasMember(key) || !object[key].IsObject())
		{
			if (object.HasMember(key))
				object.RemoveMember(key);
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value member(rapidjson::kObjectType);
			object.AddMember(keyValue, member, allocator);
		}
		return object[key];
	}

	void CopyOrReplaceMember(
		rapidjson::Value& destination,
		const char* key,
		const rapidjson::Value& source,
		Allocator& allocator)
	{
		if (destination.HasMember(key))
			destination.RemoveMember(key);
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value copy;
		CloneJsonValue(source, copy, allocator);
		destination.AddMember(keyValue, copy, allocator);
	}

	std::string SerializeCompact(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		value.Accept(writer);
		return std::string(buffer.GetString(), buffer.Size());
	}


	bool IsProfileEntry(const rapidjson::Value& value)
	{
		return value.IsObject() &&
			value.HasMember("name") &&
			value["name"].IsString() &&
			!TrimAscii(value["name"].GetString()).empty();
	}

	bool IsMetadataEntry(const rapidjson::Value& value)
	{
		return value.IsObject() &&
			value.HasMember("_vsmr") &&
			value["_vsmr"].IsObject() &&
			!value.HasMember("name");
	}

	bool ValidateProfileArray(const rapidjson::Value& profiles, std::string& error)
	{
		rapidjson::Document candidate;
		candidate.Parse<0>(SerializeCompact(profiles).c_str());
		if (candidate.HasParseError())
		{
			error = "Profiles state could not be parsed.";
			return false;
		}
		bool migrated = false;
		return CConfig::validateAndMigrateProfilesDocument(candidate, error, migrated);
	}

	bool CreateRollbackSnapshot(
		const std::string& source,
		std::string& snapshotPath)
	{
		snapshotPath.clear();
		if (source.empty())
			return false;
		const std::filesystem::path nativeSource =
			std::filesystem::u8path(source);

		for (int attempt = 0; attempt < 128; ++attempt)
		{
			std::ostringstream candidate;
			candidate << source
				<< ".transaction-rollback."
				<< ::GetCurrentProcessId()
				<< "."
				<< ::GetTickCount()
				<< "."
				<< attempt;
			const std::string candidatePath = candidate.str();
			if (::CopyFileW(
				nativeSource.c_str(),
				std::filesystem::u8path(candidatePath).c_str(),
				TRUE))
			{
				snapshotPath = candidatePath;
				break;
			}

			const DWORD copyError = ::GetLastError();
			if (copyError != ERROR_FILE_EXISTS &&
				copyError != ERROR_ALREADY_EXISTS)
			{
				return false;
			}
		}
		if (snapshotPath.empty())
			return false;

		HANDLE snapshotFile = ::CreateFileW(
			std::filesystem::u8path(snapshotPath).c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		const bool flushed =
			snapshotFile != INVALID_HANDLE_VALUE &&
			::FlushFileBuffers(snapshotFile) != FALSE;
		if (snapshotFile != INVALID_HANDLE_VALUE)
			::CloseHandle(snapshotFile);
		if (!flushed)
		{
			::DeleteFileW(std::filesystem::u8path(snapshotPath).c_str());
			snapshotPath.clear();
			return false;
		}
		return true;
	}

	bool RestoreRollbackSnapshotAtomically(
		const std::string& snapshotPath,
		const std::string& destination)
	{
		return !snapshotPath.empty() &&
			!destination.empty() &&
			::MoveFileExW(
				std::filesystem::u8path(snapshotPath).c_str(),
				std::filesystem::u8path(destination).c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	bool DeleteRollbackSnapshot(std::string& snapshotPath)
	{
		if (snapshotPath.empty())
			return true;
		const bool deleted =
			::DeleteFileW(std::filesystem::u8path(snapshotPath).c_str()) != FALSE ||
			::GetLastError() == ERROR_FILE_NOT_FOUND;
		if (deleted)
			snapshotPath.clear();
		return deleted;
	}

	void MergeProfileArrayPreservingTopLevelUnknowns(
		const rapidjson::Value& original,
		const rapidjson::Value& incoming,
		rapidjson::Document& output)
	{
		output.SetArray();
		Allocator& allocator = output.GetAllocator();

		const rapidjson::Value* incomingMetadata = nullptr;
		bool hasIncomingUnknownEntries = false;
		for (rapidjson::SizeType i = 0; i < incoming.Size(); ++i)
		{
			const rapidjson::Value& item = incoming[i];
			if (IsMetadataEntry(item))
			{
				incomingMetadata = &item;
				continue;
			}

			hasIncomingUnknownEntries =
				hasIncomingUnknownEntries || !IsProfileEntry(item);
			rapidjson::Value copy;
			CloneJsonValue(item, copy, allocator);
			output.PushBack(copy, allocator);
		}

		const rapidjson::Value* originalMetadata = nullptr;
		if (original.IsArray())
		{
			for (rapidjson::SizeType i = 0; i < original.Size(); ++i)
			{
				const rapidjson::Value& item = original[i];
				if (IsMetadataEntry(item))
				{
					originalMetadata = &item;
					continue;
				}
				if (IsProfileEntry(item))
					continue;
				if (hasIncomingUnknownEntries)
					continue;

				rapidjson::Value copy;
				CloneJsonValue(item, copy, allocator);
				output.PushBack(copy, allocator);
			}
		}

		const rapidjson::Value* metadata = incomingMetadata != nullptr
			? incomingMetadata
			: originalMetadata;
		if (metadata != nullptr)
		{
			rapidjson::Value copy;
			CloneJsonValue(*metadata, copy, allocator);
			output.PushBack(copy, allocator);
		}
	}

	bool SamePersistedFeatureIdentity(
		const rapidjson::Value& left,
		const rapidjson::Value& right)
	{
		auto readId = [](const rapidjson::Value& feature) -> std::string
		{
			if (feature.IsObject() && feature.HasMember("id") && feature["id"].IsString())
				return feature["id"].GetString();
			if (feature.IsObject() &&
				feature.HasMember("properties") &&
				feature["properties"].IsObject() &&
				feature["properties"].HasMember("id") &&
				feature["properties"]["id"].IsString())
				return feature["properties"]["id"].GetString();
			return "";
		};

		const std::string leftId = readId(left);
		const std::string rightId = readId(right);
		if (leftId.empty() || rightId.empty())
			return true;
		return leftId == rightId;
	}

	void MergeAvisoPreservingCoordinates(
		rapidjson::Document& destination,
		const rapidjson::Value& incoming)
	{
		if (!destination.IsObject() || !incoming.IsObject())
		{
			CloneJsonValue(incoming, destination, destination.GetAllocator());
			return;
		}

		Allocator& allocator = destination.GetAllocator();
		for (auto member = incoming.MemberBegin(); member != incoming.MemberEnd(); ++member)
		{
			if (std::string(member->name.GetString()) == "features")
				continue;
			CopyOrReplaceMember(destination, member->name.GetString(), member->value, allocator);
		}

		if (!incoming.HasMember("features") || !incoming["features"].IsArray())
			return;
		if (!destination.HasMember("features") || !destination["features"].IsArray())
		{
			CopyOrReplaceMember(destination, "features", incoming["features"], allocator);
			return;
		}

		rapidjson::Value& currentFeatures = destination["features"];
		const rapidjson::Value& newFeatures = incoming["features"];
		if (currentFeatures.Size() != newFeatures.Size())
		{
			CopyOrReplaceMember(destination, "features", newFeatures, allocator);
			return;
		}

		for (rapidjson::SizeType index = 0; index < newFeatures.Size(); ++index)
		{
			rapidjson::Value& current = currentFeatures[index];
			const rapidjson::Value& updated = newFeatures[index];
			if (!current.IsObject() || !updated.IsObject() ||
				!SamePersistedFeatureIdentity(current, updated))
			{
				rapidjson::Value replacement;
				CloneJsonValue(updated, replacement, allocator);
				current = replacement;
				continue;
			}

			std::vector<std::string> keysToRemove;
			for (auto member = current.MemberBegin(); member != current.MemberEnd(); ++member)
			{
				const std::string key = member->name.GetString();
				if (key != "geometry" && !updated.HasMember(member->name.GetString()))
					keysToRemove.push_back(key);
			}
			for (const std::string& key : keysToRemove)
				current.RemoveMember(key.c_str());

			for (auto member = updated.MemberBegin(); member != updated.MemberEnd(); ++member)
			{
				const std::string key = member->name.GetString();
				if (key == "geometry")
					continue;
				CopyOrReplaceMember(current, key.c_str(), member->value, allocator);
			}
		}
	}

	bool ReadFileText(
		const std::string& path,
		std::string& text,
		size_t maximumBytes)
	{
		text.clear();
		try
		{
			std::ifstream input(
				std::filesystem::u8path(path),
				std::ios::binary);
			if (!input.is_open())
				return false;

			input.seekg(0, std::ios::end);
			const std::streamoff length = input.tellg();
			if (length < 0 ||
				static_cast<unsigned long long>(length) > maximumBytes)
			{
				return false;
			}
			input.seekg(0, std::ios::beg);
			if (!input.good())
				return false;

			text.resize(static_cast<size_t>(length));
			if (length > 0)
			{
				input.read(text.data(), static_cast<std::streamsize>(length));
				if (input.gcount() != length)
				{
					text.clear();
					return false;
				}
			}
			if (input.peek() != std::char_traits<char>::eof())
			{
				text.clear();
				return false;
			}
			return true;
		}
		catch (...)
		{
			text.clear();
			return false;
		}
	}

	std::string RuntimeResourceFromType(
		const std::string& requestedType,
		const rapidjson::Value* payload)
	{
		if (payload != nullptr && payload->IsObject())
		{
			std::string resource = LowerAscii(ReadString(*payload, "resource"));
			if (resource.empty())
				resource = LowerAscii(ReadString(*payload, "kind"));
			if (resource == "profiles" || resource == "aviso")
				return resource;
		}
		const std::string type = LowerAscii(requestedType);
		return type.find("profile") != std::string::npos ? "profiles" : "aviso";
	}

	bool IsAllowedGithubUrl(const std::string& value)
	{
		const std::string url = TrimAscii(value);
		return HttpHelper::IsHttpsUrlForHost(url, "github.com") ||
			HttpHelper::IsHttpsUrlForHost(url, "www.github.com") ||
			HttpHelper::IsHttpsUrlForHost(url, "raw.githubusercontent.com");
	}

	std::string NormalizeGithubRawUrl(const std::string& value)
	{
		std::string url = TrimAscii(value);
		const std::string rawPrefix = "https://raw.githubusercontent.com/";
		if (HttpHelper::IsHttpsUrlForHost(url, "raw.githubusercontent.com"))
			return url;
		if (!HttpHelper::IsHttpsUrlForHost(url, "github.com") &&
			!HttpHelper::IsHttpsUrlForHost(url, "www.github.com"))
			return "";

		const size_t authorityEnd = url.find('/', url.find("://") + 3);
		if (authorityEnd == std::string::npos)
			return "";
		std::string path = url.substr(authorityEnd + 1);
		const size_t suffix = path.find_first_of("?#");
		if (suffix != std::string::npos)
			path.resize(suffix);
		const size_t blob = LowerAscii(path).find("/blob/");
		if (blob == std::string::npos)
			return "";
		const std::string repository = path.substr(0, blob);
		const std::string file = path.substr(blob + 6);
		if (repository.empty() || repository.find('/') == std::string::npos || file.empty())
			return "";
		return rawPrefix + repository + "/" + file;
	}
}
