#pragma once

#include <functional>
#include <memory>
#include <string>

class CSMRRadar;

enum class VsmrBridgeAction
{
	Unknown,
	UiReady,
	WindowClose,
	WindowDragStart,
	StateSave,
	StateReload,
	StateReset,
	StateUndo,
	StateRedo,
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
	AlertsUpdate,
	SettingsUpdate,
	DatalinkStateRequest,
	DatalinkSettingsUpdate,
	DatalinkConnect,
	DatalinkDisconnect,
	DatalinkPoll,
	CdmScan,
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
		const std::string& jsonText);

private:
	struct Impl;
	std::unique_ptr<Impl> State;
};
