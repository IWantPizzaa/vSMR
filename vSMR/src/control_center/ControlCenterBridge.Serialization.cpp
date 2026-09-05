#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "aviso/AvisoDocumentModel.hpp"
#include "plugin/Plugin.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/ControlCenterPerformance.hpp"
#include "control_center/ControlCenterUpdater.hpp"
#include "shared/logging/Logger.hpp"

#include "rapidjson/document.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

using VsmrControlCenterProtocol::DecodedEnvelope;
using VsmrControlCenterProtocol::MakeEnvelope;
using namespace VsmrControlCenterBridgeInternal;
VsmrControlCenterBridgeImpl::VsmrControlCenterBridgeImpl(CSMRRadar* owner, VsmrBridgeHostCallbacks callbacks)
	: Owner(owner), Callbacks(std::move(callbacks))
{
}

std::string VsmrControlCenterBridgeImpl::NextNativeId()
{
	return "native-" + std::to_string(++NativeMessageSequence);
}

void VsmrControlCenterBridgeImpl::Send(rapidjson::Document& message)
{
	if (Callbacks.sendJson)
		Callbacks.sendJson(SerializeCompact(message));
}

void VsmrControlCenterBridgeImpl::SendAck(
	const std::string& requestId,
	const std::string& action,
	const std::string& messageText)
{
	rapidjson::Document message;
	MakeEnvelope(message, "state.ack", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "action", action, allocator);
	if (!messageText.empty())
		AddString(payload, "message", messageText, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
}

void VsmrControlCenterBridgeImpl::SendError(const std::string& requestId, const std::string& messageText)
{
	rapidjson::Document message;
	MakeEnvelope(message, "state.error", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "message", messageText, allocator);
	message.AddMember("payload", payload, allocator);
	AddString(message, "message", messageText, allocator);
	Send(message);
}

std::string VsmrControlCenterBridgeImpl::RadarIdentifier() const
{
	if (Owner == nullptr)
		return {};
	const auto found = std::find(RadarScreensOpened.begin(), RadarScreensOpened.end(), Owner);
	if (found == RadarScreensOpened.end())
		return "radar-current";
	return "radar-" + std::to_string(
		static_cast<std::size_t>(std::distance(RadarScreensOpened.begin(), found)) + 1U);
}

VsmrControlCenterPerformanceContext VsmrControlCenterBridgeImpl::BuildPerformanceContext() const
{
	VsmrControlCenterPerformanceContext context;
	context.radarId = RadarIdentifier();
	if (Owner != nullptr)
	{
		context.airport = Owner->getActiveAirport();
		context.profile = Owner->GetActiveProfileNameForEditor();
	}

	CSMRPlugin* const plugin = OwnerPlugin();
	if (plugin != nullptr)
	{
		const WorkerQueueSnapshot queues = plugin->GetWorkerQueueSnapshot();
		context.workerQueues.networkWorkers = queues.networkWorkers;
		context.workerQueues.networkQueued = queues.networkQueued;
		context.workerQueues.networkInFlight = queues.networkInFlight;
		context.workerQueues.weatherWorkerRunning = queues.weatherWorkerRunning;
		context.workerQueues.weatherQueued = queues.weatherQueued;
		context.workerQueues.weatherInFlight = queues.weatherInFlight;
	}
	return context;
}

void VsmrControlCenterBridgeImpl::SendPerformanceState(
	const std::string& requestId,
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints)
{
	rapidjson::Document message;
	MakeEnvelope(message, "performance.state", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	if (Owner == nullptr)
	{
		payload.AddMember("schemaVersion", 1, allocator);
		payload.AddMember("available", false, allocator);
	}
	else
	{
		const VsmrPerformance::Snapshot snapshot = Owner->GetPerformanceSnapshot(
			windowSeconds,
			VsmrPerformance::MaximumFrameSamples);
		VsmrControlCenterPerformance::BuildPayload(
			snapshot,
			BuildPerformanceContext(),
			maximumSeriesPoints,
			PerformancePeaks,
			payload,
			allocator);
	}
	message.AddMember("payload", payload, allocator);
	Send(message);
}

void VsmrControlCenterBridgeImpl::SendUpdateState(const std::string& requestId)
{
	rapidjson::Document message;
	MakeEnvelope(message, "update.state", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload;
	VsmrControlCenterUpdater::BuildStatePayload(payload, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
}

bool VsmrControlCenterBridgeImpl::HandleUpdateSettings(
	const rapidjson::Value* payload,
	std::string& error)
{
	return VsmrControlCenterUpdater::ApplySettings(payload, error);
}

bool VsmrControlCenterBridgeImpl::HandleUpdateAction(
	const rapidjson::Value* payload,
	const std::string& requestId,
	std::string& action,
	std::string& error)
{
	return VsmrControlCenterUpdater::QueueAction(
		payload,
		requestId,
		action,
		error);
}

bool VsmrControlCenterBridgeImpl::OpenUpdateRelease(
	const rapidjson::Value* payload,
	std::string& error)
{
	return VsmrControlCenterUpdater::OpenRelease(payload, error);
}

void VsmrControlCenterBridgeImpl::SendPerformanceExportAck(
	const std::string& requestId,
	const std::string& path)
{
	rapidjson::Document message;
	MakeEnvelope(message, "state.ack", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "action", "performance.report.export", allocator);
	AddString(payload, "message", "Performance report exported", allocator);
	AddString(payload, "path", path, allocator);
	payload.AddMember("cancelled", false, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
}

std::string VsmrControlCenterBridgeImpl::ContentRevision(const std::string& contents)
{
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

std::string VsmrControlCenterBridgeImpl::FileRevision(const std::string& path)
{
	std::string contents;
	return ReadFileText(
		path,
		contents,
		AvisoDocumentModel::MaximumSerializedInputBytes)
		? ContentRevision(contents)
		: "missing";
}

void VsmrControlCenterBridgeImpl::EvaluateAvisoHealth(
	const std::string& path,
	bool& healthy,
	std::string& message) const
{
	std::error_code fileError;
	const std::filesystem::path filePath = std::filesystem::u8path(path);
	const bool exists = !path.empty() &&
		std::filesystem::is_regular_file(filePath, fileError) &&
		!fileError;
	const std::uintmax_t size = exists
		? std::filesystem::file_size(filePath, fileError)
		: 0;
	const std::filesystem::file_time_type writeTime = exists && !fileError
		? std::filesystem::last_write_time(filePath, fileError)
		: std::filesystem::file_time_type{};
	const bool stampValid = !fileError;
	if (stampValid &&
		AvisoHealthCachePath == path &&
		AvisoHealthCacheExists == exists &&
		AvisoHealthCacheSize == size &&
		AvisoHealthCacheWriteTime == writeTime)
	{
		healthy = AvisoHealthCacheHealthy;
		message = AvisoHealthCacheMessage;
		return;
	}

	healthy = false;
	message = "The active airport AVISO source is missing or invalid; the previous overlay remains active when available.";
	std::string validatedDocumentJson;
	if (exists && stampValid &&
		size <= AvisoDocumentModel::MaximumSerializedInputBytes)
	{
		std::string avisoJson;
		rapidjson::Document parsed;
		std::string inputError;
		if (ReadFileText(
				path,
				avisoJson,
				AvisoDocumentModel::MaximumSerializedInputBytes) &&
			AvisoDocumentModel::ValidateSerializedInputLimits(
				avisoJson,
				inputError) &&
			!parsed.Parse<0>(avisoJson.c_str()).HasParseError() &&
			parsed.IsObject() &&
			parsed.HasMember("type") &&
			parsed["type"].IsString() &&
			std::strcmp(parsed["type"].GetString(), "FeatureCollection") == 0)
		{
			bool schemaSupported = true;
			if (parsed.HasMember("metadata"))
			{
				const rapidjson::Value& metadata = parsed["metadata"];
				schemaSupported = metadata.IsObject();
				if (schemaSupported && metadata.HasMember("schema_version"))
				{
					const rapidjson::Value& schemaVersion = metadata["schema_version"];
					schemaSupported = schemaVersion.IsInt() &&
						schemaVersion.GetInt() >= 1 &&
						schemaVersion.GetInt() <= 2;
				}
			}
			if (schemaSupported)
			{
				AvisoDocumentModel validationModel;
				CloneJsonValue(
					parsed,
					validationModel.MutableDocument(),
					validationModel.MutableDocument().GetAllocator());
				validationModel.MarkIndexesDirty();
				const AvisoValidationResult validation =
					validationModel.ValidateAndRecalculate();
				healthy = validation.ok;
				if (healthy)
					validatedDocumentJson = SerializeCompact(
						validationModel.GetDocument());
				if (!healthy && !validation.errorText.empty())
					message = validation.errorText;
			}
		}
	}
	else if (exists &&
		size > AvisoDocumentModel::MaximumSerializedInputBytes)
	{
		message = "The active airport AVISO source exceeds the supported 32 MB limit.";
	}

	AvisoHealthCachePath = path;
	AvisoHealthCacheExists = exists;
	AvisoHealthCacheSize = size;
	AvisoHealthCacheWriteTime = writeTime;
	AvisoHealthCacheHealthy = healthy;
	AvisoHealthCacheMessage = healthy ? std::string() : message;
	AvisoHealthCacheDocumentJson = healthy
		? std::move(validatedDocumentJson)
		: std::string();
	if (healthy)
		message.clear();
}

CSMRPlugin* VsmrControlCenterBridgeImpl::OwnerPlugin() const
{
	if (Owner == nullptr)
		return nullptr;
	return static_cast<CSMRPlugin*>(Owner->GetPlugIn());
}

void VsmrControlCenterBridgeImpl::BuildSettings(
	rapidjson::Value& settings,
	Allocator& allocator) const
{
	settings.SetObject();
	if (Owner == nullptr)
		return;

	AddString(settings, "profileFile", Owner->ConfigPath, allocator);
	AddString(
		settings,
		"avisoFile",
		Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport()),
		allocator);
	CSMRPlugin* plugin = OwnerPlugin();
	const std::string aliasPath = plugin != nullptr
		? plugin->GetDatalinkControlState().cdmAliasPath
		: std::string();
	AddString(settings, "aliasFile", aliasPath, allocator);
	AddString(
		settings,
		"resolutionPreset",
		Owner->GetSmallTargetIconBoostResolutionPreset(),
		allocator);
	settings.AddMember("showFps", Owner->ShowFps, allocator);
	AddString(settings, "uiColorTheme", Owner->GetUiColorTheme(), allocator);
	AddString(settings, "avisoColorPalette", Owner->GetAvisoColorPalette(), allocator);

	rapidjson::Value dataHealth(rapidjson::kObjectType);
	const bool configHealthy =
		Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isConfigHealthy();
	dataHealth.AddMember("profilesHealthy", configHealthy, allocator);
	dataHealth.AddMember(
		"profilesUsingBackup",
		Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isUsingBackup(),
		allocator);
	const bool profilesBackupAvailable =
		Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isBackupAvailable();
	dataHealth.AddMember(
		"profilesBackupAvailable",
		profilesBackupAvailable,
		allocator);
	AddInt64(
		dataHealth,
		"profilesBackupModifiedUnixSeconds",
		profilesBackupAvailable
			? Owner->CurrentConfig->getBackupModifiedUnixSeconds()
			: 0,
		allocator);
	AddString(
		dataHealth,
		"profilesMessage",
		Owner->CurrentConfig != nullptr
			? Owner->CurrentConfig->getLastLoadMessage()
			: "vSMR configuration is not available.",
		allocator);
	const std::string avisoPath =
		Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
	bool avisoHealthy = false;
	std::string avisoHealthMessage;
	EvaluateAvisoHealth(avisoPath, avisoHealthy, avisoHealthMessage);
	dataHealth.AddMember("avisoHealthy", avisoHealthy, allocator);
	AddString(
		dataHealth,
		"avisoMessage",
		avisoHealthy ? "" : avisoHealthMessage,
		allocator);
	settings.AddMember("dataHealth", dataHealth, allocator);
}

void VsmrControlCenterBridgeImpl::BuildRuntimeState(
	rapidjson::Value& runtime,
	Allocator& allocator) const
{
	runtime.SetObject();
	if (Owner == nullptr)
		return;

	rapidjson::Value insets(rapidjson::kObjectType);
	auto insetVisible = [&](int id) -> bool
	{
		const auto found = Owner->appWindowDisplays.find(id);
		return found != Owner->appWindowDisplays.end() && found->second;
	};
	insets.AddMember("aviso", insetVisible(3), allocator);
	insets.AddMember("srw1", insetVisible(1), allocator);
	insets.AddMember("weather", insetVisible(APPWINDOW_WEATHER - APPWINDOW_BASE), allocator);
	insets.AddMember("timer", insetVisible(APPWINDOW_TIMER - APPWINDOW_BASE), allocator);
	runtime.AddMember("insets", insets, allocator);
	runtime.AddMember("avisoInsetVisible", insetVisible(3), allocator);
	AddString(
		runtime,
		"activeAvisoPreset",
		Owner->GetActiveAvisoPresetName(),
		allocator);
	rapidjson::Value groups(rapidjson::kArrayType);
	for (const CSMRRadar::AvisoGroup& group : Owner->GetAvisoGroups())
	{
		rapidjson::Value item(rapidjson::kObjectType);
		AddString(item, "id", group.id, allocator);
		AddString(item, "name", group.name, allocator);
		item.AddMember("visible", group.visible, allocator);
		groups.PushBack(item, allocator);
	}
	runtime.AddMember("groups", groups, allocator);

	rapidjson::Value alerts(rapidjson::kObjectType);
	AddString(alerts, "visibility", Owner->isLVP ? "lvp" : "normal", allocator);
	rapidjson::Value runways(rapidjson::kArrayType);
	if (Owner->RimcasInstance != nullptr)
	{
		// Geometry can be temporarily invalidated while the active airport is
		// changing. Build the runtime list from the union of geometry and all
		// configured monitoring maps so the Control Center never receives a
		// false empty runway list during that transition.
		std::set<std::string> runwayNames;
		for (const auto& entry : Owner->RimcasInstance->RunwayAreas)
			runwayNames.insert(entry.first);
		for (const auto& entry : Owner->RimcasInstance->MonitoredRunwayArr)
			runwayNames.insert(entry.first);
		for (const auto& entry : Owner->RimcasInstance->MonitoredRunwayDep)
			runwayNames.insert(entry.first);
		for (const auto& entry : Owner->RimcasInstance->ClosedRunway)
			runwayNames.insert(entry.first);

		for (const std::string& runway : runwayNames)
		{
			rapidjson::Value item(rapidjson::kObjectType);
			AddString(item, "id", runway, allocator);
			const auto arr = Owner->RimcasInstance->MonitoredRunwayArr.find(runway);
			const auto dep = Owner->RimcasInstance->MonitoredRunwayDep.find(runway);
			const auto closed = Owner->RimcasInstance->ClosedRunway.find(runway);
			item.AddMember(
				"arrival",
				arr != Owner->RimcasInstance->MonitoredRunwayArr.end() && arr->second,
				allocator);
			item.AddMember(
				"departure",
				dep != Owner->RimcasInstance->MonitoredRunwayDep.end() && dep->second,
				allocator);
			item.AddMember(
				"closed",
				closed != Owner->RimcasInstance->ClosedRunway.end() && closed->second,
				allocator);
			runways.PushBack(item, allocator);
		}
	}
	alerts.AddMember("runways", runways, allocator);
	runtime.AddMember("alerts", alerts, allocator);
}

void VsmrControlCenterBridgeImpl::SendAvisoState(const std::string& requestId)
{
	if (Owner == nullptr)
		return;

	const std::string path =
		Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
	rapidjson::Document aviso;
	bool healthy = false;
	std::string validationError;
	EvaluateAvisoHealth(path, healthy, validationError);
	const bool valid = healthy &&
		!AvisoHealthCacheDocumentJson.empty() &&
		!aviso.Parse<0>(AvisoHealthCacheDocumentJson.c_str()).HasParseError() &&
		aviso.IsObject();
	if (!valid)
	{
		aviso.SetObject();
		Allocator& allocator = aviso.GetAllocator();
		AddString(aviso, "type", "FeatureCollection", allocator);
		rapidjson::Value features(rapidjson::kArrayType);
		aviso.AddMember("features", features, allocator);
		if (!validationError.empty())
		{
			Logger::info(
				"Control Center withheld invalid AVISO GeoJSON: " +
				validationError);
		}
	}
	const std::vector<CSMRRadar::AvisoGroup> runtimeGroups = Owner->GetAvisoGroups();
	if (!runtimeGroups.empty())
	{
		const rapidjson::Value* persistedGroups =
			aviso.HasMember("vsmr_groups") && aviso["vsmr_groups"].IsArray()
			? &aviso["vsmr_groups"]
			: nullptr;
		rapidjson::Value groups(rapidjson::kArrayType);
		for (const CSMRRadar::AvisoGroup& group : runtimeGroups)
		{
			rapidjson::Value item(rapidjson::kObjectType);
			if (persistedGroups != nullptr)
			{
				for (rapidjson::SizeType index = 0; index < persistedGroups->Size(); ++index)
				{
					const rapidjson::Value& candidate = (*persistedGroups)[index];
					if (!candidate.IsObject())
						continue;
					std::string candidateId = ReadString(candidate, "id");
					if (candidateId.empty())
						candidateId = ReadString(candidate, "group_id");
					if (candidateId == group.id)
					{
						CloneJsonValue(candidate, item, aviso.GetAllocator());
						break;
					}
				}
			}
			if (!item.HasMember("id") && !item.HasMember("group_id"))
				AddString(item, "id", group.id, aviso.GetAllocator());
			SetStringMember(item, "name", group.name, aviso.GetAllocator());
			SetBoolMember(item, "visible", group.visible, aviso.GetAllocator());
			groups.PushBack(item, aviso.GetAllocator());
		}
		CopyOrReplaceMember(aviso, "vsmr_groups", groups, aviso.GetAllocator());
	}

	rapidjson::Document message;
	MakeEnvelope(message, "state.aviso", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload;
	CloneJsonValue(aviso, payload, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
}

void VsmrControlCenterBridgeImpl::SendAuthoritativeState(
	const std::string& reason,
	const std::string& requestId,
	bool includeAviso)
{
	if (Owner == nullptr || Owner->CurrentConfig == nullptr)
	{
		SendError(requestId, "vSMR configuration is not available.");
		return;
	}
	if (includeAviso)
	{
		const std::string avisoPath =
			Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
		if (!avisoPath.empty())
			Owner->EnsureAvisoGeoJsonLoaded(avisoPath);
	}

	rapidjson::Document message;
	MakeEnvelope(message, "state.authoritative", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	rapidjson::Value profiles;
	CloneJsonValue(Owner->CurrentConfig->document, profiles, allocator);
	payload.AddMember("profiles", profiles, allocator);
	rapidjson::Value settings;
	BuildSettings(settings, allocator);
	payload.AddMember("settings", settings, allocator);
	rapidjson::Value runtime;
	BuildRuntimeState(runtime, allocator);
	payload.AddMember("runtime", runtime, allocator);
	AddString(
		payload,
		"activeProfile",
		Owner->GetActiveProfileNameForEditor(),
		allocator);
	AddString(payload, "airport", Owner->getActiveAirport(), allocator);
	AddString(
		payload,
		"configRevision",
		Owner->CurrentConfig->getConfigRevision(),
		allocator);
	AddString(
		payload,
		"avisoRevision",
		FileRevision(Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport())),
		allocator);
	payload.AddMember("avisoFollows", includeAviso, allocator);
	AddString(payload, "reason", reason, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
	if (includeAviso)
		SendAvisoState(requestId);
}

void VsmrControlCenterBridgeImpl::SendStagedAuthoritativeState(
	const rapidjson::Value& stagedState,
	const std::string& reason,
	const std::string& requestId)
{
	if (Owner == nullptr || Owner->CurrentConfig == nullptr)
	{
		SendError(requestId, "vSMR configuration is not available.");
		return;
	}

	rapidjson::Document message;
	MakeEnvelope(message, "state.authoritative", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);

	rapidjson::Value profiles;
	CloneJsonValue(Owner->CurrentConfig->document, profiles, allocator);
	payload.AddMember("profiles", profiles, allocator);

	if (stagedState.IsObject() &&
		stagedState.HasMember("aviso") &&
		stagedState["aviso"].IsObject())
	{
		rapidjson::Value aviso;
		CloneJsonValue(stagedState["aviso"], aviso, allocator);
		payload.AddMember("aviso", aviso, allocator);
	}

	rapidjson::Value settings;
	if (stagedState.IsObject() &&
		stagedState.HasMember("settings") &&
		stagedState["settings"].IsObject())
	{
		CloneJsonValue(stagedState["settings"], settings, allocator);
	}
	else
	{
		BuildSettings(settings, allocator);
	}
	payload.AddMember("settings", settings, allocator);

	rapidjson::Value runtime;
	BuildRuntimeState(runtime, allocator);
	payload.AddMember("runtime", runtime, allocator);
	AddString(
		payload,
		"activeProfile",
		Owner->GetActiveProfileNameForEditor(),
		allocator);
	const std::string stagedAirport = ReadString(stagedState, "airport");
	AddString(
		payload,
		"airport",
		stagedAirport.empty() ? Owner->getActiveAirport() : stagedAirport,
		allocator);
	// Staged editor content must never be paired with a revision observed
	// after that content was captured. Echo only the caller's exact tokens;
	// omitting absent tokens keeps unrelated group-only updates from blessing
	// stale editor data with a newer disk revision.
	const std::string stagedConfigRevision =
		TrimAscii(ReadString(stagedState, "configRevision"));
	const std::string stagedAvisoRevision =
		TrimAscii(ReadString(stagedState, "avisoRevision"));
	if (!stagedConfigRevision.empty())
		AddString(
			payload,
			"configRevision",
			stagedConfigRevision,
			allocator);
	if (!stagedAvisoRevision.empty())
		AddString(
			payload,
			"avisoRevision",
			stagedAvisoRevision,
			allocator);
	AddString(payload, "reason", reason, allocator);
	message.AddMember("payload", payload, allocator);
	Send(message);
}
