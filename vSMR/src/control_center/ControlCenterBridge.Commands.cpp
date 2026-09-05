#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterDialog.hpp"

#include "rapidjson/document.h"

#include <unordered_set>
#include <utility>

using namespace VsmrControlCenterBridgeInternal;
bool VsmrControlCenterBridgeImpl::HandleProfileChange(
	const rapidjson::Value* payload,
	std::string& error)
{
	const std::string profile =
		payload != nullptr ? ReadString(*payload, "profile") : "";
	if (profile.empty())
	{
		error = "Profile name is required.";
		return false;
	}
	if (!Owner->SetActiveProfileForEditor(profile, true))
	{
		error = "The selected profile could not be activated.";
		return false;
	}
	return true;
}

bool VsmrControlCenterBridgeImpl::HandleModeChange(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Mode payload must be an object.";
		return false;
	}
	std::string profile = ReadString(*payload, "profile");
	if (profile.empty())
		profile = Owner->GetActiveProfileNameForEditor();
	const std::string mode = ReadString(*payload, "mode");
	if (profile.empty() || mode.empty())
	{
		error = "Profile and mode names are required.";
		return false;
	}
	if (!Owner->SetProfileDisplayModeActiveForEditor(profile, mode))
	{
		error = "The selected display mode could not be activated.";
		return false;
	}
	return true;
}

bool VsmrControlCenterBridgeImpl::HandleInsetToggle(
	const rapidjson::Value* payload,
	bool srwOnly,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Inset payload must be an object.";
		return false;
	}
	const std::string airport = TrimAscii(ReadString(*payload, "airport"));
	if (!airport.empty() && !EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
	{
		error = "Inset request belongs to a different airport.";
		return false;
	}
	const std::string profile = TrimAscii(ReadString(*payload, "profile"));
	if (!profile.empty() && !EqualsNoCase(profile, TrimAscii(Owner->GetActiveProfileNameForEditor())))
	{
		error = "Inset request belongs to a different profile.";
		return false;
	}
	const std::string window = LowerAscii(ReadString(*payload, "window"));
	int id = 0;
	if (window == "srw1") id = 1;
	else if (!srwOnly && window == "weather") id = APPWINDOW_WEATHER - APPWINDOW_BASE;
	else if (!srwOnly && window == "timer") id = APPWINDOW_TIMER - APPWINDOW_BASE;
	else if (!srwOnly && (window == "aviso" || window.empty())) id = 3;
	if (id == 0)
	{
		error = "Unknown inset window.";
		return false;
	}
	const bool visible = ReadBool(*payload, "visible", false);
	Owner->CancelInsetWindowInteractions();
	Owner->appWindowDisplays[id] = visible;
	if (!visible)
	{
		auto windowIt = Owner->appWindows.find(id);
		if (windowIt != Owner->appWindows.end() && windowIt->second != nullptr)
			windowIt->second->ResetAvisoInteractionState();
	}
	Owner->SaveInsetStateToAsrForAirport(Owner->getActiveAirport());
	Owner->RequestRefresh();
	return true;
}

bool VsmrControlCenterBridgeImpl::HandlePreset(
	VsmrBridgeAction action,
	const rapidjson::Value* payload,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Inset preset payload must be an object.";
		return false;
	}
	const std::string airport = TrimAscii(ReadString(*payload, "airport"));
	if (!airport.empty() && !EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
	{
		error = "Inset preset request belongs to a different airport.";
		return false;
	}
	std::string presetName = ReadString(*payload, "preset");
	if (payload->HasMember("preset") && (*payload)["preset"].IsObject())
		presetName = ReadString((*payload)["preset"], "name");
	const std::string oldName = ReadString(*payload, "oldName");
	const std::string sourceName = ReadString(*payload, "source");
	const bool linked = ReadBool(
		*payload,
		"linked_movement",
		payload->HasMember("preset") && (*payload)["preset"].IsObject()
			? ReadBool((*payload)["preset"], "linked_movement", false)
			: false);

	bool ok = false;
	switch (action)
	{
	case VsmrBridgeAction::InsetPresetLoad:
		ok = Owner->LoadAvisoPreset(presetName);
		break;
	case VsmrBridgeAction::InsetPresetCapture:
	{
		std::string savedName;
		ok = Owner->SaveAvisoPreset(presetName, false, &savedName, linked);
		break;
	}
	case VsmrBridgeAction::InsetPresetUpdate:
		ok = Owner->UpdateActiveAvisoPreset();
		break;
	case VsmrBridgeAction::InsetPresetRename:
		ok = Owner->RenameAvisoPreset(
			oldName.empty() ? Owner->GetActiveAvisoPresetName() : oldName,
			presetName,
			linked);
		break;
	case VsmrBridgeAction::InsetPresetDuplicate:
	{
		std::string savedName;
		ok = Owner->DuplicateAvisoPreset(
			sourceName.empty() ? Owner->GetActiveAvisoPresetName() : sourceName,
			presetName,
			&savedName);
		break;
	}
	case VsmrBridgeAction::InsetPresetDefault:
		ok = Owner->SetDefaultAvisoPreset(presetName);
		break;
	case VsmrBridgeAction::InsetPresetReset:
		ok = Owner->ResetActiveAvisoPreset();
		break;
	case VsmrBridgeAction::InsetPresetDelete:
		ok = Owner->DeleteAvisoPreset(presetName);
		break;
	case VsmrBridgeAction::InsetPresetLinked:
		ok = Owner->SetActiveAvisoPresetLinkedMovement(linked);
		break;
	default:
		break;
	}

	if (!ok)
		error = "Inset preset operation failed.";
	Owner->RequestRefresh();
	return ok;
}

bool VsmrControlCenterBridgeImpl::HandleLegacyPresetAssignment(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (Owner == nullptr || Owner->CurrentConfig == nullptr ||
		payload == nullptr || !payload->IsObject())
	{
		error = "Legacy inset preset assignment is not available.";
		return false;
	}
	const std::string airport = TrimAscii(ReadString(*payload, "airport"));
	if (airport.empty() ||
		!EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
	{
		error = "The active airport changed before the legacy presets were assigned.";
		return false;
	}

	size_t assignedPresetCount = 0;
	if (!Owner->CurrentConfig->assignUnscopedAvisoPresetsToAirport(
		Owner->GetActiveProfileNameForEditor(),
		airport,
		assignedPresetCount,
		error))
	{
		return false;
	}

	bool reloadFailed = false;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->CurrentConfig == nullptr ||
			!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
		{
			continue;
		}
		if (!radar->ReloadConfig())
			reloadFailed = true;
		radar->RequestRefresh();
		if (radar != Owner && radar->VsmrControlCenterDialog != nullptr)
			radar->VsmrControlCenterDialog->SyncFromRadar("external-save");
	}
	if (reloadFailed)
	{
		Owner->GetPlugIn()->DisplayUserMessage(
			"vSMR", "Inset preset migration",
			"The assignment was saved, but one or more radar windows must be reloaded.",
			true, true, false, false, false);
	}
	return true;
}

bool VsmrControlCenterBridgeImpl::HandleAlerts(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (Owner->CurrentConfig == nullptr ||
		Owner->RimcasInstance == nullptr ||
		payload == nullptr ||
		!payload->IsObject())
	{
		error = "Alert state is not available.";
		return false;
	}

	rapidjson::Value& activeProfile = Owner->CurrentConfig->getMutableActiveProfile();
	if (!activeProfile.IsObject())
	{
		error = "The active profile is invalid.";
		return false;
	}

	Allocator& allocator = Owner->CurrentConfig->document.GetAllocator();
	rapidjson::Value& rimcas =
		EnsureObjectMember(activeProfile, "rimcas", allocator);
	if (payload->HasMember("rimcas") && (*payload)["rimcas"].IsObject())
		CloneJsonValue((*payload)["rimcas"], rimcas, allocator);
	SetBoolMember(
		rimcas,
		"enabled",
		ReadBool(*payload, "enabled", true),
		allocator);

	std::unordered_set<std::string> inactiveAlerts;
	if (rimcas.HasMember("inactive_alerts") &&
		rimcas["inactive_alerts"].IsArray())
	{
		const rapidjson::Value& alerts = rimcas["inactive_alerts"];
		for (rapidjson::SizeType index = 0; index < alerts.Size(); ++index)
		{
			const rapidjson::Value& alert = alerts[index];
			if (alert.IsString())
				inactiveAlerts.insert(alert.GetString());
		}
	}
	Owner->RimcasInstance->setInactiveAlerts(inactiveAlerts);

	const std::string visibility = LowerAscii(ReadString(*payload, "visibility"));
	Owner->isLVP = visibility == "lvp" || visibility == "low";

	if (payload->HasMember("runways") && (*payload)["runways"].IsArray())
	{
		Owner->RimcasInstance->MonitoredRunwayArr.clear();
		Owner->RimcasInstance->MonitoredRunwayDep.clear();
		Owner->RimcasInstance->ClosedRunway.clear();
		const rapidjson::Value& runways = (*payload)["runways"];
		for (rapidjson::SizeType index = 0; index < runways.Size(); ++index)
		{
			const rapidjson::Value& runway = runways[index];
			if (!runway.IsObject())
				continue;
			const std::string name = ReadString(runway, "id");
			if (name.empty())
				continue;
			Owner->RimcasInstance->MonitoredRunwayArr[name] =
				ReadBool(runway, "arrival", false);
			Owner->RimcasInstance->MonitoredRunwayDep[name] =
				ReadBool(runway, "departure", false);
			Owner->RimcasInstance->ClosedRunway[name] =
				ReadBool(runway, "closed", false);
		}
	}

	Owner->LoadProfile(Owner->GetActiveProfileNameForEditor());
	Owner->RequestRefresh();
	return true;
}

bool VsmrControlCenterBridgeImpl::HandleSettings(
	const rapidjson::Value* payload,
	std::string& error)
{
	if (payload == nullptr || !payload->IsObject())
	{
		error = "Settings payload must be an object.";
		return false;
	}

	const std::string resolution = ReadString(*payload, "resolutionPreset");
	if (!resolution.empty() &&
		!Owner->SetSmallTargetIconBoostResolutionPreset(resolution, false))
	{
		error = "The selected resolution preset is invalid.";
		return false;
	}

	if (payload->HasMember("showFps") && (*payload)["showFps"].IsBool())
	{
		Owner->ShowFps = (*payload)["showFps"].GetBool();
		Owner->SaveDataToAsr(
			"ShowFps",
			"Show FPS counter",
			Owner->ShowFps ? "1" : "0");
	}

	if (payload->HasMember("avisoColorPalette"))
	{
		if (!(*payload)["avisoColorPalette"].IsString() ||
			!Owner->SetAvisoColorPalette((*payload)["avisoColorPalette"].GetString(), true))
		{
			error = "AVISO color palette must be dark, light, or real.";
			return false;
		}
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar == Owner || radar->CurrentConfig == nullptr ||
				Owner->CurrentConfig == nullptr ||
				!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
			{
				continue;
			}
			if (!radar->SetAvisoColorPalette((*payload)["avisoColorPalette"].GetString(), false))
				radar->EnsureAvisoColorPaletteAvailable(false);
		}
	}

	if (payload->HasMember("uiColorTheme"))
	{
		if (!(*payload)["uiColorTheme"].IsString() ||
			!Owner->SetUiColorTheme((*payload)["uiColorTheme"].GetString(), true))
		{
			error = "UI theme must be day or night.";
			return false;
		}
	}

	if (Owner->CurrentConfig != nullptr &&
		payload->HasMember("rimcas") &&
		(*payload)["rimcas"].IsBool())
	{
		rapidjson::Value& activeProfile = Owner->CurrentConfig->getMutableActiveProfile();
		rapidjson::Value& rimcas = EnsureObjectMember(
			activeProfile,
			"rimcas",
			Owner->CurrentConfig->document.GetAllocator());
		SetBoolMember(
			rimcas,
			"enabled",
			(*payload)["rimcas"].GetBool(),
			Owner->CurrentConfig->document.GetAllocator());
	}
	Owner->RequestRefresh();
	return true;
}

bool VsmrControlCenterBridgeImpl::HandleAvisoGroups(
	VsmrBridgeAction action,
	const rapidjson::Value* payload,
	std::string& error)
{
	if (Owner == nullptr)
	{
		error = "vSMR radar state is not available.";
		return false;
	}
	if (payload == nullptr || !payload->IsObject())
	{
		error = "AVISO group payload must be an object.";
		return false;
	}

	const std::string avisoPath =
		Owner->ResolveAvisoGeoJsonRenderPathForAirport(Owner->getActiveAirport());
	if (!avisoPath.empty())
		Owner->EnsureAvisoGeoJsonLoaded(avisoPath);

	if (action == VsmrBridgeAction::RuntimeGroupVisibility)
	{
		std::string groupId = ReadString(*payload, "id");
		if (groupId.empty())
			groupId = ReadString(*payload, "group_id");
		if (groupId.empty())
		{
			error = "AVISO group id is required.";
			return false;
		}
		if (!payload->HasMember("visible") || !(*payload)["visible"].IsBool())
		{
			error = "AVISO group visibility must be a boolean.";
			return false;
		}
		if (!Owner->SetAvisoGroupVisibility(groupId, (*payload)["visible"].GetBool()))
		{
			error = "Unknown AVISO group id.";
			return false;
		}
		return true;
	}

	if (!payload->HasMember("groups") || !(*payload)["groups"].IsArray())
	{
		error = "AVISO groups must be an array.";
		return false;
	}
	const rapidjson::Value& groupValues = (*payload)["groups"];
	std::unordered_set<std::string> seenIds;

	if (action == VsmrBridgeAction::RuntimeGroupsVisibility)
	{
		std::vector<std::pair<std::string, bool>> visibility;
		visibility.reserve(groupValues.Size());
		for (rapidjson::SizeType index = 0; index < groupValues.Size(); ++index)
		{
			const rapidjson::Value& item = groupValues[index];
			if (!item.IsObject())
			{
				error = "Each AVISO group visibility entry must be an object.";
				return false;
			}
			std::string groupId = ReadString(item, "id");
			if (groupId.empty())
				groupId = ReadString(item, "group_id");
			if (groupId.empty())
			{
				error = "Each AVISO group visibility entry requires an id.";
				return false;
			}
			if (!seenIds.insert(groupId).second)
			{
				error = "AVISO group ids must be unique.";
				return false;
			}
			if (!item.HasMember("visible") || !item["visible"].IsBool())
			{
				error = "Each AVISO group visibility entry requires a boolean visible value.";
				return false;
			}
			visibility.push_back(std::make_pair(groupId, item["visible"].GetBool()));
		}

		if (!Owner->SetAvisoGroupVisibilities(visibility))
		{
			error = "One or more AVISO group ids are unknown.";
			return false;
		}
		return true;
	}

	if (action != VsmrBridgeAction::RuntimeGroupsUpdate)
	{
		error = "Unsupported AVISO group action.";
		return false;
	}

	std::unordered_map<std::string, bool> existingVisibility;
	for (const CSMRRadar::AvisoGroup& existing : Owner->GetAvisoGroups())
		existingVisibility[existing.id] = existing.visible;

	std::vector<CSMRRadar::AvisoGroup> groups;
	groups.reserve(groupValues.Size());
	for (rapidjson::SizeType index = 0; index < groupValues.Size(); ++index)
	{
		const rapidjson::Value& item = groupValues[index];
		if (!item.IsObject())
		{
			error = "Each AVISO group definition must be an object.";
			return false;
		}

		CSMRRadar::AvisoGroup group;
		group.id = ReadString(item, "id");
		if (group.id.empty())
			group.id = ReadString(item, "group_id");
		if (group.id.empty())
		{
			error = "Each AVISO group definition requires an id.";
			return false;
		}
		if (!seenIds.insert(group.id).second)
		{
			error = "AVISO group ids must be unique.";
			return false;
		}

		group.name = ReadString(item, "name");
		if (group.name.empty())
			group.name = group.id;
		const auto existing = existingVisibility.find(group.id);
		group.visible =
			existing != existingVisibility.end()
			? existing->second
			: true;
		if (item.HasMember("visible"))
		{
			if (!item["visible"].IsBool())
			{
				error = "AVISO group visible values must be boolean.";
				return false;
			}
			group.visible = item["visible"].GetBool();
		}
		groups.push_back(std::move(group));
	}

	if (payload->HasMember("aviso"))
	{
		const rapidjson::Value& stagedAviso = (*payload)["aviso"];
		if (!stagedAviso.IsObject() ||
			!stagedAviso.HasMember("features") ||
			!stagedAviso["features"].IsArray())
		{
			error = "Staged AVISO state must be a GeoJSON FeatureCollection.";
			return false;
		}
		if (!Owner->ApplyAvisoGroupMembershipSnapshot(stagedAviso, &error))
		{
			if (error.empty())
				error = "Unable to apply staged AVISO group membership.";
			return false;
		}
	}

	return Owner->UpdateAvisoGroups(groups);
}
