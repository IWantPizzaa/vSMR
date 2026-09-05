#pragma once

#include <functional>
#include <memory>
#include <string>

class CSMRRadar;
class VsmrControlCenterBridgeImpl;

enum class VsmrBridgeAction
{
	Unknown,
	UiReady,
	WindowClose,
	WindowDragStart,
	StateSave,
	StateReload,
	StateReset,
	RuntimeProfileChange,
	RuntimeModeChange,
	RuntimeGroupVisibility,
	RuntimeGroupsVisibility,
	RuntimeGroupsUpdate,
	RuntimeInsetToggle,
	RuntimeSrwToggle,
	InsetPresetLoad,
	InsetPresetCapture,
	InsetPresetUpdate,
	InsetPresetRename,
	InsetPresetDuplicate,
	InsetPresetDefault,
	InsetPresetReset,
	InsetPresetDelete,
	InsetPresetLinked,
	InsetPresetLegacyAssign,
	AlertsUpdate,
	SettingsUpdate,
	PerformanceStateRequest,
	PerformanceReset,
	PerformanceReportExport,
	UpdateStateRequest,
	UpdateSettingsUpdate,
	UpdateActionRequest,
	UpdateReleaseOpen,
	ResourceComputerLoad,
	ResourceGithubLoad
};

struct VsmrBridgeHostCallbacks
{
	std::function<void(const std::string&)> sendJson;
	std::function<void()> closeWindow;
	std::function<void()> beginWindowDrag;
	std::function<void(const std::string& resource, const std::string& requestId)> requestComputerLoad;
	std::function<void(const std::string& requestId)> requestResetDefaults;
	std::function<void()> cancelPendingResources;
	std::function<void(
		const std::string& resource,
		const std::string& url,
		const std::string& requestId)> requestGithubLoad;
};

class VsmrControlCenterBridge
{
public:
	VsmrControlCenterBridge(CSMRRadar* owner, VsmrBridgeHostCallbacks callbacks);
	~VsmrControlCenterBridge();

	void SetOwner(CSMRRadar* owner);
	bool HandleWebMessage(const std::string& messageJson);
	void PushAuthoritativeState(const std::string& reason = "native");
	void PushError(const std::string& requestId, const std::string& message);
	bool ValidateLoadedResource(
		const std::string& resource,
		const std::string& jsonText,
		std::string& error) const;
	bool HandleLoadedResource(
		const std::string& resource,
		const std::string& source,
		const std::string& requestId,
		const std::string& jsonText,
		const std::string& effectivePath = "");

private:
	std::unique_ptr<VsmrControlCenterBridgeImpl> State;
};
