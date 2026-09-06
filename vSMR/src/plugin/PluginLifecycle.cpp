#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "bootstrap/RuntimeContext.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "rdf/RdfOverlay.hpp"
#include "weather/WeatherStore.hpp"

#include <atomic>
#include <filesystem>
#include <vector>

CSMRPlugin::CSMRPlugin(void) :CPlugIn(
	EuroScopePlugIn::COMPATIBILITY_CODE,
	VsmrPluginName,
	VsmrPluginVersion,
	VsmrPluginDeveloper,
	VsmrPluginCopyright)
{
	// Resetting process-wide session state
	PluginShutdownRequested.store(false, std::memory_order_relaxed);
	FlightDataRefreshPending.store(false, std::memory_order_relaxed);
	VsmrHoldingPoint::ClearPending();
	VsmrCdm::Shutdown();
	VsmrVsid::Shutdown();
	NetworkCancellationRequested.store(false, std::memory_order_relaxed);
	ResetDatalinkRuntime();

	Logger::DLL_PATH = "";
	Logger::ENABLED = false;
	Logger::set_mode(Logger::Mode::Normal);

	// Registering the EuroScope display and tag items
	RegisterDisplayType(VsmrPluginAvisoDisplayName, false, true, true, true);

	RegisterTagItemType("Datalink clearance", TAG_ITEM_DATALINK_STS);
	RegisterTagItemFunction("Datalink menu", TAG_FUNC_DATALINK_MENU);
	RegisterTagItemType("Holding Point", TAG_ITEM_HOLDING_POINT);
	RegisterTagItemFunction("Holding Point", TAG_FUNC_HOLDING_POINT_EDIT);

	LoadDatalinkSettings();

	string DllPath;

	// Resolving runtime data sources
	if (VsmrRuntimeContext::IsConfigured())
	{
		DllPath = VsmrRuntimeContext::InstallRootUtf8();
	}
	else
	{
		std::wstring modulePath(32768, L'\0');
		const DWORD length = ::GetModuleFileNameW(
			HINSTANCE(&__ImageBase),
			modulePath.data(),
			static_cast<DWORD>(modulePath.size()));
		if (length > 0 && length < modulePath.size())
		{
			modulePath.resize(length);
			DllPath = std::filesystem::path(modulePath).parent_path().u8string();
		}
	}
	Logger::DLL_PATH = DllPath;
	{
		const std::filesystem::path installRoot = std::filesystem::u8path(DllPath);
		std::filesystem::path catalogPath = installRoot / "vSMR_Data" / "airports_hp.json";
		std::error_code catalogError;
		if (!std::filesystem::exists(catalogPath, catalogError))
			catalogPath = installRoot / "airports_hp.json";
		LoadHoldingPointCatalog(catalogPath.u8string());
	}
	ResetDatalinkProfileSource();

	bool rdfEnabled = true;
	const char* p_value = nullptr;
	if ((p_value = GetDataFromSettings("rdf_enabled")) != NULL)
		rdfEnabled = atoi(p_value) != 0;
	VsmrRdf::Start(this, rdfEnabled);

	// Publish only after every potentially throwing initialization step has
	// completed. A failed constructor must never leave a freed instance visible
	// to runtime callbacks or shutdown recovery.
	ActivePluginInstance.store(this, std::memory_order_release);
}

CSMRPlugin::~CSMRPlugin()
{
	// Stopping callbacks and workers before releasing shared state
	PluginShutdownRequested.store(true, std::memory_order_relaxed);
	BeginDatalinkShutdown();
	VsmrGroundState::ClearAllLineupOverrides();
	VsmrCdm::Shutdown();
	VsmrVsid::Shutdown();
	VsmrRdf::Stop();
	PrepareDatalinkRuntimeForExit();
	StopWeatherFetchWorker();
	StopNetworkWorkers();
	CSMRPlugin* expectedActivePlugin = this;
	ActivePluginInstance.compare_exchange_strong(
		expectedActivePlugin,
		nullptr,
		std::memory_order_acq_rel);
	VsmrWeather::Clear();

	PersistDatalinkSettings();
}

bool VsmrShutdownPlugin()
{
	VsmrCrashRuntime::RecordEuroScopeCallback("EuroScopePlugInExit");
	CSMRPlugin* const pluginInstance = ActivePluginInstance.load(
		std::memory_order_acquire);
	PluginShutdownRequested.store(true, std::memory_order_relaxed);
	VsmrGroundState::ClearAllLineupOverrides();
	VsmrCdm::Shutdown();
	VsmrVsid::Shutdown();
	CSMRPlugin::PrepareDatalinkRuntimeForExit();
	VsmrRdf::Stop();
	if (pluginInstance != nullptr)
	{
		pluginInstance->StopWeatherFetchWorker();
		pluginInstance->StopNetworkWorkers();
	}

	const std::vector<CSMRRadar*> radarScreens = RadarScreensOpened;
	for (auto* var : radarScreens)
	{
		if (var != nullptr)
			var->EuroScopePlugInExitCustom();
	}
	VsmrWeather::Clear();
	if (!RadarScreensOpened.empty())
	{
		// EuroScope normally closes every CRadarScreen before unloading the
		// plug-in. If that ordering is violated, keep both the plug-in object and
		// its DLL alive so a late radar callback cannot dereference freed code or
		// state. The loader treats false as a process-lifetime retained generation.
		return false;
	}
	if (!CSMRRadar::CanUnloadRuntimeCallbacks())
	{
		Logger::info(
			"vSMR runtime unload retained: a Win32 subclass or thread hook is still active");
		return false;
	}

	delete pluginInstance;
	VsmrCrashReporter::Remove();
	return true;
}
