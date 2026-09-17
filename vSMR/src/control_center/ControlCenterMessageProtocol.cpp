#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"

#include "shared/TextUtils.hpp"
#include "shared/RapidJsonUtils.hpp"

namespace
{
	using VsmrRapidJson::AddString;

	std::string ReadString(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) ||
			!object[key].IsString())
		{
			return {};
		}
		return object[key].GetString();
	}

}

VsmrBridgeAction VsmrControlCenterProtocol::ActionFromType(
	const std::string& requestedType)
{
	const std::string type = ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(requestedType));
	if (type == "ui.ready") return VsmrBridgeAction::UiReady;
	if (type == "window.close") return VsmrBridgeAction::WindowClose;
	if (type == "window.drag" || type == "window.drag.start") return VsmrBridgeAction::WindowDragStart;
	if (type == "state.save" || type == "save.all") return VsmrBridgeAction::StateSave;
	if (type == "state.reload" || type == "reload.all") return VsmrBridgeAction::StateReload;
	if (type == "state.reset") return VsmrBridgeAction::StateReset;
	if (type == "runtime.profile.change") return VsmrBridgeAction::RuntimeProfileChange;
	if (type == "runtime.mode.change") return VsmrBridgeAction::RuntimeModeChange;
	if (type == "aviso.group.visibility") return VsmrBridgeAction::RuntimeGroupVisibility;
	if (type == "aviso.groups.visibility") return VsmrBridgeAction::RuntimeGroupsVisibility;
	if (type == "aviso.groups.update") return VsmrBridgeAction::RuntimeGroupsUpdate;
	if (type == "aviso.inset.toggle") return VsmrBridgeAction::RuntimeInsetToggle;
	if (type == "display.srw.toggle") return VsmrBridgeAction::RuntimeSrwToggle;
	if (type == "aviso.inset.preset.load") return VsmrBridgeAction::InsetPresetLoad;
	if (type == "aviso.inset.preset.capture") return VsmrBridgeAction::InsetPresetCapture;
	if (type == "aviso.inset.preset.update") return VsmrBridgeAction::InsetPresetUpdate;
	if (type == "aviso.inset.preset.rename") return VsmrBridgeAction::InsetPresetRename;
	if (type == "aviso.inset.preset.duplicate") return VsmrBridgeAction::InsetPresetDuplicate;
	if (type == "aviso.inset.preset.default") return VsmrBridgeAction::InsetPresetDefault;
	if (type == "aviso.inset.preset.reset") return VsmrBridgeAction::InsetPresetReset;
	if (type == "aviso.inset.preset.delete") return VsmrBridgeAction::InsetPresetDelete;
	if (type == "aviso.inset.preset.linked") return VsmrBridgeAction::InsetPresetLinked;
	if (type == "aviso.inset.preset.legacy.assign") return VsmrBridgeAction::InsetPresetLegacyAssign;
	if (type == "alerts.update") return VsmrBridgeAction::AlertsUpdate;
	if (type == "settings.update") return VsmrBridgeAction::SettingsUpdate;
	if (type == "performance.state.request") return VsmrBridgeAction::PerformanceStateRequest;
	if (type == "performance.reset") return VsmrBridgeAction::PerformanceReset;
	if (type == "performance.report.export") return VsmrBridgeAction::PerformanceReportExport;
	if (type == "update.state.request") return VsmrBridgeAction::UpdateStateRequest;
	if (type == "update.settings.update") return VsmrBridgeAction::UpdateSettingsUpdate;
	if (type == "update.action.request") return VsmrBridgeAction::UpdateActionRequest;
	if (type == "update.release.open") return VsmrBridgeAction::UpdateReleaseOpen;
	if (type == "resource.computer.load" || type == "profiles.load.computer" ||
		type == "aviso.load.computer" || type == "browse.profiles" ||
		type == "browse.aviso")
	{
		return VsmrBridgeAction::ResourceComputerLoad;
	}
	if (type == "resource.github.load" || type == "profiles.load.github" ||
		type == "aviso.load.github")
	{
		return VsmrBridgeAction::ResourceGithubLoad;
	}
	return VsmrBridgeAction::Unknown;
}

bool VsmrControlCenterProtocol::DecodeEnvelope(
	const rapidjson::Document& document,
	DecodedEnvelope& envelope,
	std::string& error)
{
	error.clear();
	if (!document.IsObject())
	{
		error = "Bridge message must be a JSON object.";
		return false;
	}

	envelope = {};
	envelope.version = Version;
	if (document.HasMember("version"))
	{
		if (!document["version"].IsInt())
		{
			error = "Bridge message version must be an integer.";
			return false;
		}
		envelope.version = document["version"].GetInt();
	}
	if (envelope.version != Version)
	{
		error = "Unsupported bridge protocol version.";
		return false;
	}

	envelope.id = ReadString(document, "id");
	envelope.type = ReadString(document, "type");
	if (envelope.type.empty())
		envelope.type = ReadString(document, "action");
	if (envelope.type.empty())
	{
		error = "Bridge message type is required.";
		return false;
	}

	envelope.action = ActionFromType(envelope.type);
	if (document.HasMember("payload"))
		envelope.payload = &document["payload"];
	return true;
}

void VsmrControlCenterProtocol::MakeEnvelope(
	rapidjson::Document& document,
	const std::string& type,
	const std::string& requestId)
{
	document.SetObject();
	auto& allocator = document.GetAllocator();
	document.AddMember("version", Version, allocator);
	if (!requestId.empty())
		AddString(document, "id", requestId, allocator);
	AddString(document, "type", type, allocator);
}
