#pragma once

#include "shared/RapidJsonUtils.hpp"

#include "rapidjson/document.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace VsmrControlCenterBridgeInternal
{
	inline constexpr std::size_t kMaximumBridgeMessageBytes = 32u * 1024u * 1024u;

	using Allocator = rapidjson::Document::AllocatorType;

	std::mutex& BridgeSaveTransactionMutex();

	std::string TrimAscii(std::string value);
	std::string LowerAscii(std::string value);
	std::string UpperAscii(std::string value);
	bool EqualsNoCase(const std::string& left, const std::string& right);
	std::string NormalizeAirportCandidate(std::string value);
	std::string DetectAvisoAirport(const rapidjson::Value& document, std::string sourceHint);
	std::string ReadString(const rapidjson::Value& object, const char* key);
	bool ReadBool(const rapidjson::Value& object, const char* key, bool fallback);
	int ReadInt(const rapidjson::Value& object, const char* key, int fallback);
	using VsmrRapidJson::AddString;
	void AddInt64(rapidjson::Value& object, const char* key, std::int64_t value, Allocator& allocator);
	using VsmrRapidJson::SetStringMember;
	using VsmrRapidJson::SetBoolMember;
	using VsmrRapidJson::CloneJsonValue;
	rapidjson::Value& EnsureObjectMember(rapidjson::Value& object, const char* key, Allocator& allocator);
	void CopyOrReplaceMember(rapidjson::Value& destination, const char* key, const rapidjson::Value& source, Allocator& allocator);
	std::string SerializeCompact(const rapidjson::Value& value);
	bool IsProfileEntry(const rapidjson::Value& value);
	bool IsMetadataEntry(const rapidjson::Value& value);
	bool ValidateProfileArray(const rapidjson::Value& profiles, std::string& error);
	bool CreateRollbackSnapshot(const std::string& source, std::string& snapshotPath);
	bool RestoreRollbackSnapshotAtomically(const std::string& snapshotPath, const std::string& destination);
	bool DeleteRollbackSnapshot(std::string& snapshotPath);
	void MergeProfileArrayPreservingTopLevelUnknowns(const rapidjson::Value& original, const rapidjson::Value& incoming, rapidjson::Document& output);
	bool SamePersistedFeatureIdentity(const rapidjson::Value& left, const rapidjson::Value& right);
	void MergeAvisoPreservingCoordinates(rapidjson::Document& destination, const rapidjson::Value& incoming);
	bool ReadFileText(const std::string& path, std::string& text, std::size_t maximumBytes);
	std::string RuntimeResourceFromType(const std::string& requestedType, const rapidjson::Value* payload);
	bool IsAllowedGithubUrl(const std::string& value);
	std::string NormalizeGithubRawUrl(const std::string& value);
}
