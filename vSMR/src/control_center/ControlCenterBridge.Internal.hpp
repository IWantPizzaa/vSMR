#pragma once

#include "control_center/ControlCenterBridge.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/ControlCenterPerformance.hpp"
#include "shared/RapidJsonUtils.hpp"

#include "rapidjson/document.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace VsmrControlCenterBridgeInternal
{
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

class CSMRPlugin;

// Per-dialog bridge state and operations. Definitions are divided by
// responsibility so protocol routing, persistence, and serialization can be
// reviewed independently without widening the public bridge API.
class VsmrControlCenterBridgeImpl
{
	friend class VsmrControlCenterBridge;

public:
	CSMRRadar* Owner = nullptr;
	VsmrBridgeHostCallbacks Callbacks;
	unsigned long long NativeMessageSequence = 0;
	mutable std::string AvisoHealthCachePath;
	mutable std::filesystem::file_time_type AvisoHealthCacheWriteTime{};
	mutable std::uintmax_t AvisoHealthCacheSize = 0;
	mutable bool AvisoHealthCacheExists = false;
	mutable bool AvisoHealthCacheHealthy = false;
	mutable std::string AvisoHealthCacheMessage;
	mutable std::string AvisoHealthCacheDocumentJson;
	VsmrControlCenterPerformancePeaks PerformancePeaks;

	explicit VsmrControlCenterBridgeImpl(
		CSMRRadar* owner,
		VsmrBridgeHostCallbacks callbacks);

	std::string NextNativeId();
	void Send(rapidjson::Document& message);
	void SendAck(
		const std::string& requestId,
		const std::string& action,
		const std::string& messageText = "");
	void SendError(const std::string& requestId, const std::string& messageText);
	std::string RadarIdentifier() const;
	VsmrControlCenterPerformanceContext BuildPerformanceContext() const;
	void SendPerformanceState(
		const std::string& requestId,
		std::uint32_t windowSeconds,
		std::size_t maximumSeriesPoints);
	void SendUpdateState(const std::string& requestId);
	bool HandleUpdateSettings(const rapidjson::Value* payload, std::string& error);
	bool HandleUpdateAction(
		const rapidjson::Value* payload,
		const std::string& requestId,
		std::string& action,
		std::string& error);
	bool OpenUpdateRelease(const rapidjson::Value* payload, std::string& error);
	void SendPerformanceExportAck(
		const std::string& requestId,
		const std::string& path);
	std::string ContentRevision(const std::string& contents);
	std::string FileRevision(const std::string& path);
	void EvaluateAvisoHealth(
		const std::string& path,
		bool& healthy,
		std::string& message) const;
	CSMRPlugin* OwnerPlugin() const;
	void BuildSettings(
		rapidjson::Value& settings,
		VsmrControlCenterBridgeInternal::Allocator& allocator) const;
	void BuildRuntimeState(
		rapidjson::Value& runtime,
		VsmrControlCenterBridgeInternal::Allocator& allocator) const;
	void SendAvisoState(const std::string& requestId);
	void SendAuthoritativeState(
		const std::string& reason,
		const std::string& requestId = "",
		bool includeAviso = true);
	void SendStagedAuthoritativeState(
		const rapidjson::Value& stagedState,
		const std::string& reason,
		const std::string& requestId);

	bool SaveAll(
		const rapidjson::Value* payload,
		const std::string& requestId,
		std::string& error);
	bool HandleProfileChange(const rapidjson::Value* payload, std::string& error);
	bool HandleModeChange(const rapidjson::Value* payload, std::string& error);
	bool HandleInsetToggle(
		const rapidjson::Value* payload,
		bool srwOnly,
		std::string& error);
	bool HandlePreset(
		VsmrBridgeAction action,
		const rapidjson::Value* payload,
		std::string& error);
	bool HandleLegacyPresetAssignment(
		const rapidjson::Value* payload,
		std::string& error);
	bool HandleAlerts(const rapidjson::Value* payload, std::string& error);
	bool HandleSettings(const rapidjson::Value* payload, std::string& error);
	bool HandleAvisoGroups(
		VsmrBridgeAction action,
		const rapidjson::Value* payload,
		std::string& error);
	bool Dispatch(
		const VsmrControlCenterProtocol::DecodedEnvelope& envelope,
		std::string& error);
};
