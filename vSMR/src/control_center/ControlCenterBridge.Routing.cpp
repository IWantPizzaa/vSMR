#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/ControlCenterPerformance.hpp"
#include "shared/logging/Logger.hpp"

#include "rapidjson/document.h"

#include <cstdint>
#include <filesystem>

using VsmrControlCenterProtocol::DecodedEnvelope;
using VsmrControlCenterProtocol::MakeEnvelope;
using namespace VsmrControlCenterBridgeInternal;
bool VsmrControlCenterBridgeImpl::Dispatch(
	const DecodedEnvelope& envelope,
	std::string& error)
{
	error.clear();
	switch (envelope.action)
	{
	case VsmrBridgeAction::UiReady:
		SendAuthoritativeState("initial", envelope.id);
		return true;
	case VsmrBridgeAction::WindowClose:
		if (Callbacks.closeWindow)
			Callbacks.closeWindow();
		return true;
	case VsmrBridgeAction::WindowDragStart:
		if (Callbacks.beginWindowDrag)
			Callbacks.beginWindowDrag();
		return true;
	case VsmrBridgeAction::StateSave:
		if (!SaveAll(envelope.payload, envelope.id, error))
			return false;
		{
			rapidjson::Document saved;
			MakeEnvelope(saved, "state.saved", envelope.id);
			Allocator& allocator = saved.GetAllocator();
			rapidjson::Value payload(rapidjson::kObjectType);
			AddString(payload, "message", "Saved", allocator);
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
			rapidjson::Value settings;
			BuildSettings(settings, allocator);
			payload.AddMember("settings", settings, allocator);
			saved.AddMember("payload", payload, allocator);
			Send(saved);
		}
		// The browser owns the exact immutable snapshot that was accepted above.
		// A compact durable-write acknowledgement is sufficient; echoing profiles
		// back through an authoritative-state render can overwrite edits made while
		// this save was in flight. Initial load, explicit reload and external saves
		// remain authoritative synchronization boundaries.
		return true;
	case VsmrBridgeAction::StateReload:
	{
		if (Owner == nullptr)
		{
			error = "vSMR radar state is not available.";
			return false;
		}
		if (Callbacks.cancelPendingResources)
			Callbacks.cancelPendingResources();
		const bool configReloaded = Owner->ReloadConfig();
		const bool avisoReloaded = Owner->ForceReloadAvisoGeoJson();
		if (!configReloaded || !avisoReloaded)
		{
			// ReloadConfig may intentionally retain the last usable state or activate
			// a read-only migrated document while returning false. Publish that exact
			// recovery state before reporting the warning so the Web UI and native
			// revision/health state cannot diverge. Keep the last rendered AVISO in
			// the UI when its replacement failed validation.
			SendAuthoritativeState("reload", envelope.id, avisoReloaded);
			error =
				!configReloaded && !avisoReloaded
					? "Profiles did not reload normally and AVISO validation failed. The safest available profiles state and previous rendered overlay remain active; review Settings."
					: !configReloaded
						? "Profiles did not reload normally. The safest available in-memory state is active; review the recovery status in Settings."
						: "AVISO validation failed; the previous rendered overlay remains active.";
			return false;
		}
		SendAuthoritativeState("reload", envelope.id);
		SendAck(envelope.id, "state.reload", "Configuration reloaded");
		return true;
	}
	case VsmrBridgeAction::StateReset:
		if (Callbacks.cancelPendingResources)
			Callbacks.cancelPendingResources();
		if (!Callbacks.requestResetDefaults)
		{
			error = "Bundled defaults are not available in this host.";
			return false;
		}
		Callbacks.requestResetDefaults(envelope.id);
		return true;
	case VsmrBridgeAction::RuntimeProfileChange:
		if (!HandleProfileChange(envelope.payload, error))
			return false;
		SendAuthoritativeState("profile", envelope.id);
		return true;
	case VsmrBridgeAction::RuntimeModeChange:
		if (!HandleModeChange(envelope.payload, error))
			return false;
		SendAuthoritativeState("mode", envelope.id);
		return true;
	case VsmrBridgeAction::RuntimeInsetToggle:
		if (!HandleInsetToggle(envelope.payload, false, error))
			return false;
		SendAuthoritativeState("inset", envelope.id);
		return true;
	case VsmrBridgeAction::RuntimeSrwToggle:
		if (!HandleInsetToggle(envelope.payload, true, error))
			return false;
		SendAuthoritativeState("inset", envelope.id);
		return true;
	case VsmrBridgeAction::InsetPresetLoad:
	case VsmrBridgeAction::InsetPresetCapture:
	case VsmrBridgeAction::InsetPresetUpdate:
	case VsmrBridgeAction::InsetPresetRename:
	case VsmrBridgeAction::InsetPresetDuplicate:
	case VsmrBridgeAction::InsetPresetDefault:
	case VsmrBridgeAction::InsetPresetReset:
	case VsmrBridgeAction::InsetPresetDelete:
	case VsmrBridgeAction::InsetPresetLinked:
		if (!HandlePreset(envelope.action, envelope.payload, error))
			return false;
		SendAuthoritativeState("preset", envelope.id);
		return true;
	case VsmrBridgeAction::InsetPresetLegacyAssign:
		if (!HandleLegacyPresetAssignment(envelope.payload, error))
			return false;
		SendAuthoritativeState("legacy-preset-assigned", envelope.id);
		return true;
	case VsmrBridgeAction::AlertsUpdate:
		if (!HandleAlerts(envelope.payload, error))
			return false;
		SendAck(envelope.id, envelope.type, "Alert settings applied");
		return true;
	case VsmrBridgeAction::SettingsUpdate:
		if (!HandleSettings(envelope.payload, error))
			return false;
		SendAck(envelope.id, envelope.type, "Settings applied");
		return true;
	case VsmrBridgeAction::PerformanceStateRequest:
	{
		const int requestedWindow = envelope.payload != nullptr
			? ReadInt(*envelope.payload, "windowSeconds", 120)
			: 120;
		const int requestedPoints = envelope.payload != nullptr
			? ReadInt(*envelope.payload, "maxSeriesPoints", 120)
			: 120;
		SendPerformanceState(
			envelope.id,
			VsmrControlCenterPerformance::NormalizeWindowSeconds(requestedWindow),
			VsmrControlCenterPerformance::NormalizeSeriesPoints(requestedPoints));
		return true;
	}
	case VsmrBridgeAction::PerformanceReset:
		if (Owner == nullptr)
		{
			error = "vSMR performance diagnostics are not available.";
			return false;
		}
		Owner->ResetPerformanceDiagnostics();
		PerformancePeaks.Reset();
		SendAck(envelope.id, envelope.type, "Performance sample reset");
		return true;
	case VsmrBridgeAction::PerformanceReportExport:
	{
		if (Owner == nullptr)
		{
			error = "vSMR performance diagnostics are not available.";
			return false;
		}
		const std::string format = envelope.payload != nullptr
			? LowerAscii(ReadString(*envelope.payload, "format"))
			: "json";
		if (!format.empty() && format != "json")
		{
			error = "Only JSON performance reports are supported.";
			return false;
		}
		const std::uint32_t windowSeconds =
			VsmrControlCenterPerformance::NormalizeWindowSeconds(
			envelope.payload != nullptr
				? ReadInt(*envelope.payload, "windowSeconds", 120)
				: 120);
		const std::string nativeReport = Owner->BuildPerformanceReportJson(
			windowSeconds,
			VsmrPerformance::MaximumFrameSamples);
		const VsmrControlCenterPerformanceContext performanceContext =
			BuildPerformanceContext();
		std::string report;
		if (!VsmrControlCenterPerformance::AddWorkerQueuesToReport(
				nativeReport,
				performanceContext.workerQueues,
				report,
				error))
			return false;
		std::string reportPath;
		if (!VsmrControlCenterPerformance::WriteReportAtomically(
			report,
			std::filesystem::u8path(Owner->DataPath),
			reportPath,
			error))
		{
			return false;
		}
		SendPerformanceExportAck(envelope.id, reportPath);
		Logger::info("Performance diagnostics report written path=" + reportPath);
		return true;
	}
	case VsmrBridgeAction::UpdateStateRequest:
		SendUpdateState(envelope.id);
		return true;
	case VsmrBridgeAction::UpdateSettingsUpdate:
		if (!HandleUpdateSettings(envelope.payload, error))
			return false;
		SendUpdateState(envelope.id);
		SendAck(envelope.id, envelope.type, "Update settings saved");
		return true;
	case VsmrBridgeAction::UpdateActionRequest:
	{
		std::string requestedAction;
		if (!HandleUpdateAction(envelope.payload, envelope.id, requestedAction, error))
			return false;
		SendAck(
			envelope.id,
			envelope.type,
			requestedAction == "retry_update"
				? "Update retry queued for the next startup"
				: "AVISO reload queued for the next startup");
		SendUpdateState(envelope.id);
		return true;
	}
	case VsmrBridgeAction::UpdateReleaseOpen:
		if (!OpenUpdateRelease(envelope.payload, error))
			return false;
		SendAck(envelope.id, envelope.type, "Opened release notes");
		return true;
	case VsmrBridgeAction::ResourceComputerLoad:
	{
		const std::string resource =
			RuntimeResourceFromType(envelope.type, envelope.payload);
		if (Callbacks.requestComputerLoad)
			Callbacks.requestComputerLoad(resource, envelope.id);
		return true;
	}
	case VsmrBridgeAction::ResourceGithubLoad:
	{
		const std::string resource =
			RuntimeResourceFromType(envelope.type, envelope.payload);
		const std::string url = envelope.payload != nullptr
			? ReadString(*envelope.payload, "url")
			: "";
		if (!IsAllowedGithubUrl(url))
		{
			error = "Only github.com and raw.githubusercontent.com file URLs are allowed.";
			return false;
		}
		const std::string normalizedUrl = NormalizeGithubRawUrl(url);
		if (normalizedUrl.empty())
		{
			error = "The GitHub URL must point to a file.";
			return false;
		}
		if (Callbacks.requestGithubLoad)
			Callbacks.requestGithubLoad(
				resource,
				normalizedUrl,
				envelope.id);
		return true;
	}
	case VsmrBridgeAction::RuntimeGroupVisibility:
	case VsmrBridgeAction::RuntimeGroupsVisibility:
	case VsmrBridgeAction::RuntimeGroupsUpdate:
		if (!HandleAvisoGroups(envelope.action, envelope.payload, error))
			return false;
		if (envelope.payload != nullptr &&
			envelope.payload->IsObject() &&
			envelope.payload->HasMember("aviso") &&
			(*envelope.payload)["aviso"].IsObject())
		{
			SendStagedAuthoritativeState(
				*envelope.payload,
				"group",
				envelope.id);
		}
		else
		{
			SendAuthoritativeState("group", envelope.id);
		}
		return true;
	default:
		error = "Unsupported bridge action: " + envelope.type;
		return false;
	}
}
