#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "aviso/AvisoDocumentModel.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/RuntimeResourceFiles.hpp"

#include "rapidjson/document.h"

#include <cstring>

using VsmrControlCenterProtocol::MakeEnvelope;
using namespace VsmrControlCenterBridgeInternal;
bool VsmrControlCenterBridge::ValidateLoadedResource(
	const std::string& resource,
	const std::string& jsonText,
	std::string& error) const
{
	error.clear();
	if (jsonText.empty())
	{
		error = "The selected resource is empty.";
		return false;
	}

	const std::string normalizedResource = LowerAscii(resource);
	if (normalizedResource == "profiles")
	{
		if (!CConfig::validateSerializedInputLimits(jsonText, error))
			return false;
	}
	else if (normalizedResource == "aviso")
	{
		if (!AvisoDocumentModel::ValidateSerializedInputLimits(jsonText, error))
			return false;
	}
	else
	{
		error = "Unknown resource type.";
		return false;
	}

	rapidjson::Document parsed;
	parsed.Parse<0>(jsonText.c_str());
	if (parsed.HasParseError())
	{
		error = "The selected resource contains invalid JSON.";
		return false;
	}

	if (normalizedResource == "profiles")
		return ValidateProfileArray(parsed, error);
	if (normalizedResource == "aviso")
	{
		if (!parsed.IsObject() ||
			!parsed.HasMember("type") ||
			!parsed["type"].IsString() ||
			std::strcmp(parsed["type"].GetString(), "FeatureCollection") != 0 ||
			!parsed.HasMember("features") ||
			!parsed["features"].IsArray())
		{
			error = "The selected AVISO file is not a GeoJSON FeatureCollection.";
			return false;
		}
		if (parsed.HasMember("metadata") && !parsed["metadata"].IsObject())
		{
			error = "AVISO metadata must be an object.";
			return false;
		}
		if (parsed.HasMember("metadata") && parsed["metadata"].IsObject() &&
			parsed["metadata"].HasMember("schema_version"))
		{
			const rapidjson::Value& schemaVersion = parsed["metadata"]["schema_version"];
			if (!schemaVersion.IsInt() || schemaVersion.GetInt() < 1)
			{
				error = "AVISO metadata.schema_version must be a positive integer.";
				return false;
			}
			if (schemaVersion.GetInt() > 2)
			{
				error = "The AVISO file uses a future schema version that this build does not support.";
				return false;
			}
		}

		AvisoDocumentModel validationModel;
		CloneJsonValue(
			parsed,
			validationModel.MutableDocument(),
			validationModel.MutableDocument().GetAllocator());
		validationModel.MarkIndexesDirty();
		const AvisoValidationResult result =
			validationModel.ValidateAndRecalculate();
		if (!result.ok)
		{
			error = result.errorText.empty()
				? "The selected AVISO file failed validation."
				: result.errorText;
			return false;
		}
		return true;
	}

	return false;
}

bool VsmrControlCenterBridge::HandleLoadedResource(
	const std::string& resource,
	const std::string& source,
	const std::string& requestId,
	const std::string& jsonText,
	const std::string& effectivePath)
{
	std::string validationError;
	if (!ValidateLoadedResource(resource, jsonText, validationError))
	{
		State->SendError(requestId, validationError);
		return false;
	}

	rapidjson::Document parsed;
	parsed.Parse<0>(jsonText.c_str());
	const std::string normalizedResource = LowerAscii(resource);
	if (normalizedResource == "profiles")
	{
		bool migrated = false;
		std::string migrationError;
		if (!CConfig::validateAndMigrateProfilesDocument(parsed, migrationError, migrated))
		{
			State->SendError(requestId, migrationError);
			return false;
		}
	}
	std::string normalizedEffectivePath;
	std::string activatedAvisoRevision;
	if (!effectivePath.empty())
	{
		std::string pathError;
		if (!VsmrResourceFiles::NormalizeExistingFilePath(
			effectivePath,
			normalizedEffectivePath,
			pathError))
		{
			State->SendError(
				requestId,
				pathError.empty() ? "The selected resource path is unavailable." : pathError);
			return false;
		}

		// The file picker/download result is validated before this method runs.
		// Reject a source that changed between that read and activation instead of
		// pairing stale JSON with a newer revision token.
		std::string activationJson;
		const size_t maximumResourceBytes = normalizedResource == "profiles"
			? CConfig::MaximumSerializedInputBytes
			: AvisoDocumentModel::MaximumSerializedInputBytes;
		if (!ReadFileText(
				normalizedEffectivePath,
				activationJson,
				maximumResourceBytes) ||
			State->ContentRevision(activationJson) !=
				State->ContentRevision(jsonText))
		{
			State->SendError(
				requestId,
				"The selected resource changed while it was loading. Select it again.");
			return false;
		}

		if (State->Owner == nullptr)
		{
			State->SendError(requestId, "vSMR radar state is not available.");
			return false;
		}

		std::string activationError;
		if (normalizedResource == "profiles")
		{
			if (!State->Owner->SetProfilesConfigPath(
				normalizedEffectivePath,
				&activationError,
				true))
			{
				State->SendError(
					requestId,
					activationError.empty()
						? "Unable to activate the selected profiles file."
						: activationError);
				return false;
			}
			// Return exactly what the native runtime activated, not the earlier
			// picker buffer. Its revision now describes this same document.
			CloneJsonValue(
				State->Owner->CurrentConfig->document,
				parsed,
				parsed.GetAllocator());
		}
		else if (normalizedResource == "aviso")
		{
			const std::string activeAirport = NormalizeAirportCandidate(
				State->Owner->getActiveAirport());
			if (activeAirport.empty())
			{
				State->SendError(requestId, "Select an active airport before loading AVISO GeoJSON.");
				return false;
			}

			const std::string detectedAirport = DetectAvisoAirport(parsed, source);
			if (detectedAirport.empty())
			{
				State->SendError(
					requestId,
					"Could not determine the AVISO airport. Add metadata.icao or use a filename such as LFPO.geojson, LFPO_AVISO.geojson, or AVISO_LFPO.geojson.");
				return false;
			}
			if (detectedAirport != activeAirport)
			{
				State->SendError(
					requestId,
					"This AVISO file is for " + detectedAirport +
					". Select that airport before loading it; the active airport is " +
					activeAirport + ".");
				return false;
			}

			const auto previousOverride =
				State->Owner->AvisoGeoJsonOverridePaths.find(activeAirport);
			const bool hadPreviousOverride =
				previousOverride != State->Owner->AvisoGeoJsonOverridePaths.end();
			const std::string previousOverridePath = hadPreviousOverride
				? previousOverride->second
				: std::string();
			State->Owner->SetAvisoGeoJsonOverrideForAirport(
				activeAirport,
				normalizedEffectivePath);
			const std::string activatedPath =
				State->Owner->ResolveAvisoGeoJsonPathForAirport(activeAirport);
			if (!EqualsNoCase(activatedPath, normalizedEffectivePath) ||
				!State->Owner->ForceReloadAvisoGeoJson())
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(requestId, "Unable to activate the selected AVISO GeoJSON file.");
				return false;
			}

			std::string activatedJson;
			std::string activatedValidationError;
			if (!ReadFileText(
					normalizedEffectivePath,
					activatedJson,
					AvisoDocumentModel::MaximumSerializedInputBytes) ||
				!ValidateLoadedResource(
					"aviso",
					activatedJson,
					activatedValidationError) ||
				State->ContentRevision(activatedJson) !=
					State->FileRevision(normalizedEffectivePath))
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(
					requestId,
					"The selected AVISO file changed during activation. Select it again.");
				return false;
			}
			activatedAvisoRevision =
				State->ContentRevision(activatedJson);
			parsed.Parse<0>(activatedJson.c_str());
			if (parsed.HasParseError() ||
				DetectAvisoAirport(parsed, source) != activeAirport)
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(
					requestId,
					"The activated AVISO no longer matches the active airport.");
				return false;
			}

			// The override is process-wide across radar screens. Do not publish it
			// to other Control Centers until validation and renderer activation have
			// both succeeded, otherwise a failed load briefly exposes the path that
			// is about to be rolled back.
			for (CSMRRadar* radar : RadarScreensOpened)
			{
				if (radar == nullptr || radar == State->Owner ||
					radar->VsmrControlCenterDialog == nullptr)
				{
					continue;
				}
				radar->VsmrControlCenterDialog->SyncFromRadar("resource-source");
			}
		}
	}

	rapidjson::Document message;
	MakeEnvelope(message, "resource.loaded", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "resource", normalizedResource, allocator);
	AddString(payload, "source", source, allocator);
	if (State->Owner != nullptr && State->Owner->CurrentConfig != nullptr)
	{
		AddString(
			payload,
			"configRevision",
			State->Owner->CurrentConfig->getConfigRevision(),
			allocator);
		AddString(
			payload,
			"avisoRevision",
			activatedAvisoRevision.empty()
				? State->FileRevision(State->Owner->GetAvisoGeoJsonEditorPathForAirport(State->Owner->getActiveAirport()))
				: activatedAvisoRevision,
			allocator);
		rapidjson::Value settings;
		State->BuildSettings(settings, allocator);
		payload.AddMember("settings", settings, allocator);
	}
	if (!normalizedEffectivePath.empty())
		AddString(payload, "path", normalizedEffectivePath, allocator);
	rapidjson::Value data;
	CloneJsonValue(parsed, data, allocator);
	payload.AddMember("data", data, allocator);
	message.AddMember("payload", payload, allocator);
	State->Send(message);
	return true;
}
