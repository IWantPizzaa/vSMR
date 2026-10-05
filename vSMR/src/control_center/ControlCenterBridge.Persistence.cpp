#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoOverrides.hpp"
#include "config/LayeredConfig.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "rdf/RdfOverlay.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "shared/logging/Logger.hpp"

#include "rapidjson/document.h"

#include <filesystem>
#include <mutex>
#include <set>

using namespace VsmrControlCenterBridgeInternal;
bool VsmrControlCenterBridgeImpl::SaveAll(
	const rapidjson::Value* payload,
	const std::string& /*requestId*/,
	std::string& error)
{
	error.clear();
	if (Owner == nullptr || Owner->CurrentConfig == nullptr)
	{
		error = "vSMR configuration is not available.";
		return false;
	}
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Save payload must be an object.";
		return false;
	}
	if (!payload->HasMember("profiles"))
	{
		error = "Save payload is missing profiles.";
		return false;
	}

	bool hasStagedShowFps = false;
	bool stagedShowFps = Owner->ShowFps;
	bool hasStagedShowNorthIndicator = false;
	bool stagedShowNorthIndicator = Owner->ShowNorthIndicator;
	bool hasStagedRdfEnabled = false;
	bool stagedRdfEnabled = VsmrRdf::GetStatus().enabled;
	bool hasStagedAvisoColorPalette = false;
	std::string stagedAvisoColorPalette = Owner->GetAvisoColorPalette();
	bool hasStagedUiColorTheme = false;
	std::string stagedUiColorTheme = Owner->GetUiColorTheme();
	bool stagedWeatherAllAirports = Owner->WeatherAllAirports;
	if (payload->HasMember("settings"))
	{
		const rapidjson::Value& settings = (*payload)["settings"];
		if (!settings.IsObject())
		{
			error = "Save settings must be an object.";
			return false;
		}
		if (settings.HasMember("weatherAllAirports"))
		{
			if (!settings["weatherAllAirports"].IsBool())
			{
				error = "METAR airport grouping must be a boolean setting.";
				return false;
			}
			stagedWeatherAllAirports = settings["weatherAllAirports"].GetBool();
		}
		if (settings.HasMember("showFps"))
		{
			if (!settings["showFps"].IsBool())
			{
				error = "Show FPS must be a boolean setting.";
				return false;
			}
			hasStagedShowFps = true;
			stagedShowFps = settings["showFps"].GetBool();
		}
		if (settings.HasMember("showNorthIndicator"))
		{
			if (!settings["showNorthIndicator"].IsBool())
			{
				error = "North indicator must be a boolean setting.";
				return false;
			}
			hasStagedShowNorthIndicator = true;
			stagedShowNorthIndicator = settings["showNorthIndicator"].GetBool();
		}
		if (settings.HasMember("rdfEnabled"))
		{
			if (!settings["rdfEnabled"].IsBool())
			{
				error = "Native RDF must be a boolean setting.";
				return false;
			}
			hasStagedRdfEnabled = true;
			stagedRdfEnabled = settings["rdfEnabled"].GetBool();
		}
		if (settings.HasMember("avisoColorPalette"))
		{
			if (!settings["avisoColorPalette"].IsString())
			{
				error = "AVISO color palette must be dark, light, or real.";
				return false;
			}
			stagedAvisoColorPalette = TrimAscii(settings["avisoColorPalette"].GetString());
			if (!EqualsNoCase(stagedAvisoColorPalette, "dark") &&
				!EqualsNoCase(stagedAvisoColorPalette, "light") &&
				!EqualsNoCase(stagedAvisoColorPalette, "real") &&
				!EqualsNoCase(stagedAvisoColorPalette, "day") &&
				!EqualsNoCase(stagedAvisoColorPalette, "night"))
			{
				error = "AVISO color palette must be dark, light, or real.";
				return false;
			}
			hasStagedAvisoColorPalette = true;
		}
		if (settings.HasMember("uiColorTheme"))
		{
			if (!settings["uiColorTheme"].IsString())
			{
				error = "UI theme must be day or night.";
				return false;
			}
			stagedUiColorTheme = TrimAscii(settings["uiColorTheme"].GetString());
			if (!EqualsNoCase(stagedUiColorTheme, "day") &&
				!EqualsNoCase(stagedUiColorTheme, "night"))
			{
				error = "UI theme must be day or night.";
				return false;
			}
			hasStagedUiColorTheme = true;
		}
	}
	// Revision checks, both file writes, and rollback form one process-wide
	// transaction.  Without this lock two Control Centers can both pass the
	// AVISO revision check and then corrupt each other's transaction state.
	std::lock_guard<std::mutex> transactionLock(BridgeSaveTransactionMutex());
	const std::string stagedAirport = TrimAscii(ReadString(*payload, "airport"));
	if (payload->HasMember("aviso") &&
		(stagedAirport.empty() ||
			!EqualsNoCase(stagedAirport, TrimAscii(Owner->getActiveAirport()))))
	{
		error = "The active airport changed while these edits were staged. Reload the Control Center before saving.";
		return false;
	}

	const rapidjson::Value& incomingProfiles = (*payload)["profiles"];
	if (!ValidateProfileArray(incomingProfiles, error))
		return false;
	const std::string expectedConfigRevision =
		TrimAscii(ReadString(*payload, "configRevision"));
	const std::string expectedAvisoRevision =
		TrimAscii(ReadString(*payload, "avisoRevision"));
	if (expectedConfigRevision.empty())
	{
		error = "The Control Center has not received an authoritative profiles revision. Reload before saving.";
		return false;
	}
	if (payload->HasMember("aviso") && expectedAvisoRevision.empty())
	{
		error = "The Control Center has not received an authoritative AVISO revision. Reload before saving.";
		return false;
	}
	const bool recoveryConfirmed =
		ReadBool(*payload, "recoveryConfirmed", false);
	const bool avisoRecoveryConfirmed =
		ReadBool(*payload, "avisoRecoveryConfirmed", false);

	std::vector<CConfig::ProfileSaveIdentity> profileIdentities;
	if (payload->HasMember("profileIdentities"))
	{
		const rapidjson::Value& identities = (*payload)["profileIdentities"];
		if (!identities.IsArray())
		{
			error = "Profile identity state must be an array.";
			return false;
		}

		std::set<std::string> seenCurrentNames;
		for (rapidjson::SizeType index = 0; index < identities.Size(); ++index)
		{
			const rapidjson::Value& identity = identities[index];
			if (!identity.IsObject())
			{
				error = "Each profile identity must be an object.";
				return false;
			}
			const std::string currentName = TrimAscii(ReadString(identity, "currentName"));
			const std::string persistedName = TrimAscii(ReadString(identity, "persistedName"));
			if (currentName.empty())
			{
				error = "Each profile identity must name its current profile.";
				return false;
			}

			bool currentProfileExists = false;
			for (rapidjson::SizeType profileIndex = 0; profileIndex < incomingProfiles.Size(); ++profileIndex)
			{
				const rapidjson::Value& profile = incomingProfiles[profileIndex];
				if (IsProfileEntry(profile) &&
					EqualsNoCase(TrimAscii(profile["name"].GetString()), currentName))
				{
					currentProfileExists = true;
					break;
				}
			}
			if (!currentProfileExists ||
				!seenCurrentNames.insert(LowerAscii(currentName)).second)
			{
				error = "Profile identity state does not match the profiles being saved.";
				return false;
			}

			profileIdentities.push_back({ currentName, persistedName });
		}

		size_t incomingProfileCount = 0;
		for (rapidjson::SizeType index = 0; index < incomingProfiles.Size(); ++index)
			incomingProfileCount += IsProfileEntry(incomingProfiles[index]) ? 1u : 0u;
		if (profileIdentities.size() != incomingProfileCount)
		{
			error = "Profile identity state must contain exactly one entry per profile.";
			return false;
		}
	}

	rapidjson::Document mergedProfiles;
	MergeProfileArrayPreservingTopLevelUnknowns(
		Owner->CurrentConfig->document,
		incomingProfiles,
		mergedProfiles);
	if (!ValidateProfileArray(mergedProfiles, error))
		return false;

	std::unique_ptr<AvisoDocumentModel> avisoModel;
	std::string avisoPath;
	bool persistAvisoOverrides = false;
	bool activateCustomAviso = false;
	const bool resetAvisoOverrides = Owner->CurrentConfig->isLayeredConfig() &&
		ReadBool(*payload, "resetAvisoOverrides", false);
	const bool resetProfileOverrides = Owner->CurrentConfig->isLayeredConfig() &&
		ReadBool(*payload, "resetProfileOverrides", false);
	rapidjson::Document stagedUserSections(rapidjson::kObjectType);
	if (payload->HasMember("aviso"))
	{
		const rapidjson::Value& incomingAviso = (*payload)["aviso"];
		if (!incomingAviso.IsObject() ||
			!incomingAviso.HasMember("features") ||
			!incomingAviso["features"].IsArray())
		{
			error = "AVISO state must be a GeoJSON FeatureCollection.";
			return false;
		}

		avisoPath =
			Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
		if (!expectedAvisoRevision.empty() &&
			expectedAvisoRevision != AvisoRevision(avisoPath))
		{
			error =
				"The active AVISO file changed in another vSMR window. Reload before saving so those changes are not overwritten.";
			return false;
		}
		avisoModel = std::make_unique<AvisoDocumentModel>();
		std::string loadError;
		const std::filesystem::path dataRoot = std::filesystem::u8path(Owner->GetDataPath());
		const std::string sourcePath = resetAvisoOverrides
			? (dataRoot / "AVISO" / (stagedAirport + ".geojson")).u8string() : avisoPath;
		persistAvisoOverrides = Owner->CurrentConfig->isLayeredConfig() &&
			VsmrAvisoOverrides::IsManagedSource(dataRoot, std::filesystem::u8path(sourcePath));
		rapidjson::Document defaults;
		rapidjson::Document previousAviso;
		const std::string defaultSource = Owner->GetAvisoDefaultSource(sourcePath, loadError);
		if (defaultSource.empty() || !avisoModel->LoadFromFile(defaultSource, loadError))
		{
			// An invalid official baseline cannot safely produce sparse edits.
			if (persistAvisoOverrides)
			{
				error = "The official AVISO default is invalid. Repair it or select a custom map before editing.";
				return false;
			}
			if (!avisoRecoveryConfirmed)
			{
				error = loadError.empty() ? "Unable to load current AVISO data." : loadError;
				return false;
			}
			CloneJsonValue(
				incomingAviso,
				avisoModel->MutableDocument(),
				avisoModel->MutableDocument().GetAllocator());
		}
		else
		{
			defaults.CopyFrom(avisoModel->GetDocument(), defaults.GetAllocator());
			if (!resetAvisoOverrides &&
				!Owner->ApplyAvisoUserOverrides(sourcePath, avisoModel->MutableDocument(), error))
				return false;
			previousAviso.CopyFrom(avisoModel->GetDocument(), previousAviso.GetAllocator());
			MergeAvisoPreservingCoordinates(
				avisoModel->MutableDocument(),
				incomingAviso);
		}
		avisoModel->MarkIndexesDirty();
		if (!avisoModel->ValidateLoadedFeatureCollection(error))
			return false;
		if (persistAvisoOverrides)
		{
			rapidjson::Document airportOverrides;
			if (!VsmrAvisoOverrides::Build(defaults, previousAviso, avisoModel->GetDocument(),
				resetAvisoOverrides ? nullptr : VsmrAvisoOverrides::Find(*Owner->CurrentConfig, stagedAirport),
				airportOverrides, error)) return false;
			rapidjson::Value airports(rapidjson::kObjectType);
			if (const rapidjson::Value* current = Owner->CurrentConfig->getUserConfigSection("aviso"))
			{
				if (!current->IsObject()) { error = "Invalid AVISO override configuration."; return false; }
				airports.CopyFrom(*current, stagedUserSections.GetAllocator());
			}
			airports.RemoveMember(stagedAirport.c_str());
			if (!airportOverrides.ObjectEmpty())
				VsmrLayeredConfig::Put(airports, stagedAirport.c_str(), airportOverrides, stagedUserSections.GetAllocator());
			VsmrLayeredConfig::Put(stagedUserSections, "aviso", airports, stagedUserSections.GetAllocator());
		}
		else if (VsmrAvisoOverrides::IsManagedSource(dataRoot, std::filesystem::u8path(sourcePath)))
		{
			// External legacy profile documents have no layered store. Keep that
			// workflow usable, but edit a new user-owned map rather than official data.
			try
			{
				const auto directory = dataRoot / "UserData" / "Maps";
				std::filesystem::create_directories(directory);
				unsigned int suffix = 0;
				std::filesystem::path candidate;
				do
				{
					candidate = directory / (stagedAirport + "-custom-" +
						std::to_string(::GetCurrentProcessId()) + "-" +
						std::to_string(::GetTickCount64()) + "-" + std::to_string(suffix++) + ".geojson");
				} while (std::filesystem::exists(candidate));
				avisoPath = candidate.u8string();
				activateCustomAviso = true;
			}
			catch (const std::exception& exception)
			{
				error = "Cannot create a user-owned AVISO copy: " + std::string(exception.what());
				return false;
			}
		}
	}

	rapidjson::Document previousProfiles;
	CloneJsonValue(
		Owner->CurrentConfig->document,
		previousProfiles,
		previousProfiles.GetAllocator());
	const auto profileIndexesMatch = [](const rapidjson::Value& left, const rapidjson::Value& right)
	{
		if (!left.IsArray() || !right.IsArray() || left.Size() != right.Size())
			return false;
		for (rapidjson::SizeType index = 0; index < left.Size(); ++index)
		{
			const bool leftIsProfile = IsProfileEntry(left[index]);
			const bool rightIsProfile = IsProfileEntry(right[index]);
			if (leftIsProfile != rightIsProfile)
				return false;
			if (leftIsProfile && !EqualsNoCase(
				TrimAscii(left[index]["name"].GetString()),
				TrimAscii(right[index]["name"].GetString())))
			{
				return false;
			}
		}
		return true;
	};
	const bool ownerProfileIndexesRemainStable =
		profileIndexesMatch(previousProfiles, mergedProfiles);
	const std::string activeProfileBefore = Owner->GetActiveProfileNameForEditor();

	bool avisoExistedBeforeSave = false;
	std::string avisoRollbackSnapshotPath;
	if (avisoModel != nullptr && !persistAvisoOverrides)
	{
		const DWORD attributes = ::GetFileAttributesW(
			std::filesystem::u8path(avisoPath).c_str());
		avisoExistedBeforeSave =
			attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		if (avisoExistedBeforeSave &&
			!CreateRollbackSnapshot(avisoPath, avisoRollbackSnapshotPath))
		{
			error =
				"Unable to create an exact AVISO rollback snapshot; no files were changed.";
			return false;
		}
	}

	CloneJsonValue(
		mergedProfiles,
		Owner->CurrentConfig->document,
		Owner->CurrentConfig->document.GetAllocator());

	if (avisoModel != nullptr && !persistAvisoOverrides)
	{
		if (!avisoModel->SaveAtomically(
			avisoPath,
			error))
		{
			CloneJsonValue(
				previousProfiles,
				Owner->CurrentConfig->document,
				Owner->CurrentConfig->document.GetAllocator());
			bool primaryRestored = true;
			if (avisoExistedBeforeSave)
			{
				primaryRestored = RestoreRollbackSnapshotAtomically(
					avisoRollbackSnapshotPath,
					avisoPath);
				if (primaryRestored)
					avisoRollbackSnapshotPath.clear();
			}
			else
			{
				primaryRestored =
					::DeleteFileW(std::filesystem::u8path(avisoPath).c_str()) != FALSE ||
					::GetLastError() == ERROR_FILE_NOT_FOUND;
			}
			if (error.empty())
				error = "Unable to save AVISO GeoJSON atomically.";
			if (!primaryRestored)
				error += " The exact old primary remains at " +
					avisoRollbackSnapshotPath + ".";
			return false;
		}
	}

	std::string profileSaveError;
	if (!Owner->CurrentConfig->saveConfig(
		profileIdentities,
		expectedConfigRevision,
		&profileSaveError,
		recoveryConfirmed,
		persistAvisoOverrides ? &stagedUserSections : nullptr,
		resetProfileOverrides))
	{
		CloneJsonValue(
			previousProfiles,
			Owner->CurrentConfig->document,
			Owner->CurrentConfig->document.GetAllocator());
		bool avisoRollbackOk = true;
		if (avisoModel != nullptr && !persistAvisoOverrides)
		{
			if (avisoExistedBeforeSave)
			{
				avisoRollbackOk = RestoreRollbackSnapshotAtomically(
					avisoRollbackSnapshotPath,
					avisoPath);
				if (avisoRollbackOk)
					avisoRollbackSnapshotPath.clear();
			}
			else
				avisoRollbackOk =
					::DeleteFileW(std::filesystem::u8path(avisoPath).c_str()) != FALSE ||
					::GetLastError() == ERROR_FILE_NOT_FOUND;
		}
		error = profileSaveError.empty()
			? "Unable to save the user configuration atomically."
			: profileSaveError;
		if (!avisoRollbackOk)
		{
			error += " The AVISO rollback also failed.";
			if (!avisoRollbackSnapshotPath.empty())
					error += " The exact pre-save file remains at " +
						avisoRollbackSnapshotPath + ".";
		}
		return false;
	}

	if (!DeleteRollbackSnapshot(avisoRollbackSnapshotPath))
	{
		Logger::info(
			"Warning: unable to remove completed AVISO transaction snapshot " +
			avisoRollbackSnapshotPath);
	}
	if (persistAvisoOverrides && resetAvisoOverrides)
		Owner->SetAvisoGeoJsonOverrideForAirport(stagedAirport, "");
	else if (activateCustomAviso)
		Owner->SetAvisoGeoJsonOverrideForAirport(stagedAirport, avisoPath);
	// Display settings are ASR state rather than profile JSON. Commit them only
	// after the profiles/AVISO transaction succeeds so a failed Save changes no
	// live or persisted display state.
	if (hasStagedShowFps)
	{
		Owner->ShowFps = stagedShowFps;
		Owner->SaveDataToAsr(
			"ShowFps",
			"Show FPS counter",
			Owner->ShowFps ? "1" : "0");
	}
	if (hasStagedRdfEnabled)
		ApplyRdfEnabled(stagedRdfEnabled);
	if (hasStagedShowNorthIndicator)
	{
		Owner->ShowNorthIndicator = stagedShowNorthIndicator;
		Owner->SaveNorthIndicatorStateToAsr();
	}
	if (hasStagedAvisoColorPalette)
		Owner->SetAvisoColorPalette(stagedAvisoColorPalette, true);
	if (hasStagedUiColorTheme)
		Owner->SetUiColorTheme(stagedUiColorTheme, true);

	bool reloadFailed = false;
	Owner->WeatherAllAirports = stagedWeatherAllAirports;
	Owner->SaveDataToAsr("WeatherAllAirports", "Group open AVISO airports in METAR", stagedWeatherAllAirports ? "1" : "0");
	bool avisoReloadFailed = false;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->CurrentConfig == nullptr ||
			!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
		{
			continue;
		}

		// Keep ASR display settings synchronized across every radar screen that
		// shares this configuration file.
		if (hasStagedAvisoColorPalette && radar != Owner)
			radar->SetAvisoColorPalette(stagedAvisoColorPalette, false);
		if (hasStagedUiColorTheme && radar != Owner)
			radar->SetUiColorTheme(stagedUiColorTheme, false);

		// The owner's document is already the validated data written by
		// saveConfig. If profile indices did not change, reparsing that same file
		// only adds latency to frequent autosaves. Other radar screens still
		// reload so their independent CConfig instances receive the update.
		if (radar != Owner || !ownerProfileIndexesRemainStable)
		{
			if (!radar->ReloadConfig())
				reloadFailed = true;
		}
		if (avisoModel != nullptr &&
			((persistAvisoOverrides && EqualsNoCase(radar->getActiveAirport(), stagedAirport)) || EqualsNoCase(
				radar->GetAvisoGeoJsonEditorPathForAirport(radar->getActiveAirport()),
				avisoPath)))
		{
			if (!radar->ForceReloadAvisoGeoJson())
				avisoReloadFailed = true;
		}
		radar->RequestRefresh();
		if (radar != Owner && radar->VsmrControlCenterDialog != nullptr)
			radar->VsmrControlCenterDialog->SyncFromRadar("external-save");
	}
	if (!activeProfileBefore.empty() &&
		Owner->CurrentConfig->isItActiveProfile(activeProfileBefore) != 0)
	{
		// The staged profile is already authoritative. Saving the outgoing
		// runtime RIMCAS state here would overwrite a just-edited alert list
		// before the browser receives the post-save snapshot.
		Owner->LoadProfile(activeProfileBefore, false);
	}
	if (reloadFailed || avisoReloadFailed)
	{
		const std::string warning = reloadFailed && avisoReloadFailed
			? "The files were saved, but one or more radar windows could not reload their configuration or AVISO renderer. Reload vSMR before editing again."
			: avisoReloadFailed
				? "The files were saved, but one or more radar windows could not reload the AVISO renderer. Reload vSMR before editing again."
				: "The files were saved, but one or more radar windows could not reload them. Reload vSMR before editing again.";
		Logger::info(warning);
		Owner->GetPlugIn()->DisplayUserMessage(
			"vSMR",
			"Configuration reload",
			warning.c_str(),
			true, true, false, false, false);
	}
	return true;
}
