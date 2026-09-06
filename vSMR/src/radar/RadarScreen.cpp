#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "bootstrap/RuntimeContext.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.AvisoRuntimeState.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "radar/RadarScreenSupport.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "insets/InsetWindow.hpp"
#include <cctype>
#include <commctrl.h>
#include "rapidjson/document.h"
#include "tags/TagColorRules.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "tags/CdmTagHelpers.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "plugin/Plugin.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/RapidJsonUtils.hpp"
#include "shared/WindowsPathEncoding.hpp"

namespace TagColorRules = VsmrTagColorRules;

#pragma comment(lib, "comctl32.lib")

namespace
{
	class RadarConstructionGuard final
	{
	public:
		RadarConstructionGuard(ULONG_PTR& gdiplusToken, std::uintptr_t screenToken) noexcept :
			GdiplusToken(gdiplusToken),
			ScreenToken(screenToken)
		{
		}

		~RadarConstructionGuard() noexcept
		{
			if (ConstructionComplete)
				return;

			VsmrCrashReporter::ClearRadarState(ScreenToken);
			if (GdiplusToken != 0)
			{
				GdiplusShutdown(GdiplusToken);
				GdiplusToken = 0;
			}
		}

		void Complete() noexcept
		{
			ConstructionComplete = true;
		}

	private:
		ULONG_PTR& GdiplusToken;
		std::uintptr_t ScreenToken = 0;
		bool ConstructionComplete = false;
	};
}

CPoint mouseLocation(0, 0);
string TagBeingDragged;
int LeaderLineDefaultlenght = 50;

// Cursor state shared by radar screen instances (managed on the UI thread).

bool initCursor = true;
HCURSOR smrCursor = NULL;
bool standardCursor; // True when the default arrow cursor is active.
bool customCursor; // True when the plugin-specific cursor theme is enabled.
constexpr UINT_PTR kInsetWindowSubclassId = 0x56534D52u; // "VSMR"
std::map<HWND, std::vector<CSMRRadar*>> gInsetWindowRadarScreens;
UINT AvisoWorkerRefreshMessage()
{
	static const UINT message =
		::RegisterWindowMessageA("vSMR.2.AvisoWorkerRefresh");
	return message;
}
void RestoreInsetWindowProcHooks();
void RemoveInsetWindowProcHooksForRadar(CSMRRadar* radarScreen);
HHOOK gThreadMouseHook = nullptr;
DWORD gThreadMouseHookThreadId = 0;
DWORD gLastThreadHookError = 0xFFFFFFFF;
HHOOK gThreadKeyboardHook = nullptr;
DWORD gThreadKeyboardHookThreadId = 0;
DWORD gLastThreadKeyboardHookError = 0xFFFFFFFF;
LRESULT CALLBACK InsetWindowSubclassProc(
	HWND hwnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam,
	UINT_PTR subclassId,
	DWORD_PTR referenceData);
LRESULT CALLBACK MouseMessageHookProc(int code, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK KeyboardMessageHookProc(int code, WPARAM wParam, LPARAM lParam);
void UnhookAvisoThreadHooks();
bool TryHandleAvisoWheel(POINT screenPoint, int wheelDelta, HWND sourceHwnd);

map<string, string> CSMRRadar::vStripsStands;

// ReSharper disable CppMsExtAddressOfClassRValue

CSMRRadar::CSMRRadar()
{

	Logger::info("CSMRRadar::CSMRRadar()");
	LOGFONT runtimeOverlayFont = {};
	runtimeOverlayFont.lfHeight = -11;
	runtimeOverlayFont.lfWeight = FW_BOLD;
	runtimeOverlayFont.lfCharSet = DEFAULT_CHARSET;
	runtimeOverlayFont.lfQuality = CLEARTYPE_QUALITY;
	runtimeOverlayFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
	strcpy_s(runtimeOverlayFont.lfFaceName, LF_FACESIZE, "Tahoma");
	RuntimeOverlayFont.CreateFontIndirect(&runtimeOverlayFont);
	LOGFONT runtimeMenuActionFont = runtimeOverlayFont;
	runtimeMenuActionFont.lfHeight = -10;
	runtimeMenuActionFont.lfWeight = FW_NORMAL;
	RuntimeMenuActionFont.CreateFontIndirect(&runtimeMenuActionFont);

	// Initializing randomizer
	srand(static_cast<unsigned>(time(nullptr)));

	// Initialize GDI+
	GdiplusStartupInput gdiplusStartupInput;
	const Gdiplus::Status gdiplusStatus = GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);
	if (gdiplusStatus != Gdiplus::Ok)
	{
		m_gdiplusToken = 0;
		Logger::info("CSMRRadar::CSMRRadar() GdiplusStartup failed status=" + std::to_string(static_cast<int>(gdiplusStatus)));
	}
	RadarConstructionGuard constructionGuard(
		m_gdiplusToken,
		reinterpret_cast<std::uintptr_t>(this));

	// Getting the DLL file folder
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
			DllPath = fs::path(modulePath).parent_path().u8string();
		}
	}

	DataPath = VsmrRadarSupport::ResolvePluginDataDirectoryPath(DllPath);
	ConfigPath = VsmrRadarSupport::ResolvePluginFilePath(DllPath, "vSMR_Profiles.json");
	{
		const std::string sessionProfilesPath =
			CSMRPlugin::GetActiveProfilesConfigPath();
		std::error_code sessionPathError;
		if (!sessionProfilesPath.empty() &&
			fs::is_regular_file(fs::u8path(sessionProfilesPath), sessionPathError) &&
			!sessionPathError)
		{
			ConfigPath = fs::absolute(
				fs::u8path(sessionProfilesPath),
				sessionPathError).lexically_normal().u8string();
			if (sessionPathError)
				ConfigPath = sessionProfilesPath;
		}
	}
	mapsPath = VsmrRadarSupport::ResolvePluginFilePath(DllPath, "vSMR_Maps.json");
	IconsPath = VsmrRadarSupport::ResolvePluginDirectoryPath(DllPath, "aircraft_icons");
	LoadAircraftSpecs();

	Logger::info("Loading callsigns");

	// Creating the RIMCAS instance
	if (Callsigns == nullptr)
		Callsigns = std::make_unique<CCallsignLookup>();

	// We can look in three places for this file:
	// 1. Within the plugin directory
	// 2. In the ICAO folder of a GNG package
	// 3. In the working directory of EuroScope
	std::vector<fs::path> possible_paths;
	possible_paths.push_back(fs::u8path(DllPath) / "ICAO_Airlines.txt");
	possible_paths.push_back(fs::u8path(DllPath).parent_path().parent_path() / "ICAO" / "ICAO_Airlines.txt");
	possible_paths.push_back(fs::u8path(DllPath).parent_path().parent_path().parent_path() / "ICAO" / "ICAO_Airlines.txt");

	for (const auto& p : possible_paths) {
		Logger::info("Trying to read callsigns from: " + p.u8string());
		std::error_code callsignPathError;
		if (fs::is_regular_file(p, callsignPathError) && !callsignPathError) {
			Logger::info("Found callsign file!");
			Callsigns->readFile(p);

			break;
		}
	};

	Logger::info("Loading RIMCAS & Config");
	// Creating the RIMCAS instance
	if (RimcasInstance == nullptr)
		RimcasInstance = std::make_unique<CRimcas>();

	// Loading up the config file
	if (CurrentConfig == nullptr)
		CurrentConfig = std::make_unique<CConfig>(ConfigPath, mapsPath);

	standardCursor = true;
	ActiveAirport = "EGKK";
	const std::string avisoDefaultAirport = DetectDefaultAirportFromAviso();
	if (!avisoDefaultAirport.empty())
	{
		ActiveAirport = avisoDefaultAirport;
		Logger::info("CSMRRadar::CSMRRadar() default active airport from AVISO file=" + ActiveAirport);
	}
	// Set up the native inset windows.
	const int srwWindowId = APPWINDOW_ONE - APPWINDOW_BASE;
	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	appWindowDisplays[srwWindowId] = false;
	appWindowDisplays[avisoWindowId] = false;
	appWindowDisplays[weatherWindowId] = false;
	appWindowDisplays[timerWindowId] = false;
	appWindows[srwWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_ONE);
	appWindows[avisoWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_AVISO);
	appWindows[avisoWindowId]->m_Mode = CInsetWindow::Mode::AvisoViewport;
	appWindows[avisoWindowId]->m_Area = { 260, 260, 760, 560 };
	appWindows[weatherWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_WEATHER);
	appWindows[weatherWindowId]->m_Mode = CInsetWindow::Mode::Weather;
	appWindows[weatherWindowId]->m_Area = { 300, 200, 606, 375 };
	appWindows[timerWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_TIMER);
	appWindows[timerWindowId]->m_Mode = CInsetWindow::Mode::Timer;
	appWindows[timerWindowId]->m_Area = { 100, 180, 184, 236 };

	Logger::info("Loading profile");

	this->CSMRRadar::LoadProfile("Default", false);

	PublishCrashRadarState("main");
	constructionGuard.Complete();

}

POINT CSMRRadar::ConvertCoordFromPositionToPixel(CPosition position)
{
	// EuroScope owns the display transform. Using it directly keeps drawing,
	// hit-testing, zooming and native panning in one coordinate system.
	return EuroScopePlugIn::CRadarScreen::ConvertCoordFromPositionToPixel(position);
}

CPosition CSMRRadar::ConvertCoordFromPixelToPosition(POINT point)
{
	return EuroScopePlugIn::CRadarScreen::ConvertCoordFromPixelToPosition(point);
}

CSMRRadar::~CSMRRadar()
{
	PublishCrashRadarState("closing", "none");
	Logger::info(string(__FUNCSIG__));
	BeginShutdown();
	CloseVsmrControlCenterWindow();
	DestroyVsmrControlCenterWindow();
	RadarScreensOpened.erase(std::remove(RadarScreensOpened.begin(), RadarScreensOpened.end(), this), RadarScreensOpened.end());
	RemoveInsetWindowProcHooksForRadar(this);
	if (RadarScreensOpened.empty())
	{
		RestoreInsetWindowProcHooks();
		UnhookAvisoThreadHooks();
	}
	customFonts.clear();
	appWindows.clear();
	AircraftIcons.clear();
	RealisticIconBitmapCache.clear();
	ClearAvisoGeoJsonRasterCache();

	// Shutting down GDI+
	if (m_gdiplusToken != 0)
	{
		GdiplusShutdown(m_gdiplusToken);
		m_gdiplusToken = 0;
	}
	VsmrCrashReporter::ClearRadarState(
		reinterpret_cast<std::uintptr_t>(this));
}

void CSMRRadar::SamplePerformanceResourcesIfDue(bool force)
{
	const std::uint64_t now = VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
	if (!force && PerformanceLastResourceSampleMilliseconds != 0 &&
		now - PerformanceLastResourceSampleMilliseconds < 1000)
	{
		return;
	}
	PerformanceLastResourceSampleMilliseconds = now;

	VsmrPerformance::ResourceSample sample;
	sample.timestampMilliseconds = now;
	sample.processGdiObjects = static_cast<std::uint32_t>(
		::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	auto addBitmap = [&](Gdiplus::Bitmap* bitmap, std::size_t& categoryCount)
	{
		if (bitmap == nullptr)
			return;
		++categoryCount;
		++sample.ownedBitmapCount;
		sample.estimatedBitmapBytes += static_cast<std::uint64_t>(bitmap->GetWidth()) *
			static_cast<std::uint64_t>(bitmap->GetHeight()) * 4ULL;
	};
	for (const auto& aircraftIcon : AircraftIcons)
		addBitmap(aircraftIcon.second.get(), sample.aircraftBitmapCount);
	for (const auto& realisticIcon : RealisticIconBitmapCache)
		addBitmap(realisticIcon.second.bitmap.get(), sample.realisticIconBitmapCount);
	if (AvisoGeoJsonRasterCache != nullptr)
	{
		sample.mainAvisoBitmapCount = 1;
		++sample.ownedBitmapCount;
		if (AvisoGeoJsonRasterWidth > 0 && AvisoGeoJsonRasterHeight > 0)
		{
			sample.estimatedBitmapBytes += static_cast<std::uint64_t>(AvisoGeoJsonRasterWidth) *
				static_cast<std::uint64_t>(AvisoGeoJsonRasterHeight) * 4ULL;
		}
	}
	for (const auto& appWindow : appWindows)
	{
		if (appWindow.second == nullptr)
			continue;
		std::uint64_t estimatedBytes = 0;
		const std::size_t bitmapCount = appWindow.second->GetAvisoPerformanceBitmapCount(&estimatedBytes);
		sample.insetAvisoBitmapCount += bitmapCount;
		sample.ownedBitmapCount += bitmapCount;
		sample.estimatedBitmapBytes += estimatedBytes;
	}
	PerformanceDiagnostics.RecordResources(sample);
}

VsmrPerformance::Snapshot CSMRRadar::GetPerformanceSnapshot(
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints)
{
	SamplePerformanceResourcesIfDue();
	const VsmrPerformance::AvisoQueueDepth mainQueue =
		AvisoGeoJsonRenderPipeline != nullptr
		? AvisoGeoJsonRenderPipeline->QueueDepth()
		: VsmrPerformance::AvisoQueueDepth{};
	VsmrPerformance::AvisoQueueDepth insetQueue;
	for (const auto& appWindow : appWindows)
	{
		if (appWindow.second == nullptr)
			continue;
		const VsmrPerformance::AvisoQueueDepth windowQueue =
			appWindow.second->GetAvisoPerformanceQueueDepth();
		insetQueue.pending += windowQueue.pending;
		insetQueue.inFlight += windowQueue.inFlight;
		insetQueue.completed += windowQueue.completed;
		insetQueue.workers += windowQueue.workers;
	}

	std::size_t realisticScaledEntries = 0;
	std::size_t realisticRotatedEntries = 0;
	for (const auto& item : RealisticIconBitmapCache)
	{
		if (item.first.rfind("s|", 0) == 0)
			++realisticScaledEntries;
		else if (item.first.rfind("r|", 0) == 0)
			++realisticRotatedEntries;
	}
	return PerformanceDiagnostics.GetSnapshot(
		windowSeconds,
		maximumSeriesPoints,
		mainQueue,
		insetQueue,
		AircraftIcons.size(),
		realisticScaledEntries,
		realisticRotatedEntries);
}

void CSMRRadar::ResetPerformanceDiagnostics()
{
	PerformanceDiagnostics.Reset();
	PerformanceLastResourceSampleMilliseconds = 0;
	SamplePerformanceResourcesIfDue(true);
}

std::string CSMRRadar::BuildPerformanceReportJson(
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints)
{
	const VsmrPerformance::Snapshot snapshot = GetPerformanceSnapshot(
		windowSeconds,
		maximumSeriesPoints);
	return VsmrPerformance::BuildJsonReport(
		snapshot,
		VsmrPluginVersion,
		getActiveAirport(),
		CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : std::string());
}

void CSMRRadar::PublishCrashRadarState(
	const char* radar,
	const char* inset) const noexcept
{
	char visibleInsets[96]{};
	if (inset == nullptr)
	{
		auto appendInset = [&](const char* name)
		{
			if (name == nullptr || name[0] == '\0')
				return;
			if (visibleInsets[0] != '\0')
				strcat_s(visibleInsets, ",");
			strcat_s(visibleInsets, name);
		};

		for (const auto& display : appWindowDisplays)
		{
			if (!display.second)
				continue;
			switch (display.first)
			{
			case APPWINDOW_ONE - APPWINDOW_BASE:
				appendInset("SRW1");
				break;
			case APPWINDOW_AVISO - APPWINDOW_BASE:
				appendInset("AVISO");
				break;
			case APPWINDOW_WEATHER - APPWINDOW_BASE:
				appendInset("WEATHER");
				break;
			case APPWINDOW_TIMER - APPWINDOW_BASE:
				appendInset("TIMER");
				break;
			default:
				appendInset("OTHER");
				break;
			}
		}
		if (visibleInsets[0] == '\0')
			strcpy_s(visibleInsets, "none");
		inset = visibleInsets;
	}

	const char* const airportValue = ActiveAirport.c_str();
	const char* const profileValue = CrashActiveProfile;
	const char* const radarValue = radar != nullptr ? radar : "unknown";
	const char* const insetValue = inset != nullptr ? inset : "unknown";
	if (strcmp(CrashLastAirport, airportValue) == 0 &&
		strcmp(CrashLastProfile, profileValue) == 0 &&
		strcmp(CrashLastRadar, radarValue) == 0 &&
		strcmp(CrashLastInset, insetValue) == 0)
	{
		return;
	}

	VsmrCrashReporter::RecordRadarState(
		reinterpret_cast<std::uintptr_t>(this),
		airportValue,
		profileValue,
		radarValue,
		insetValue);
	strncpy_s(CrashLastAirport, sizeof(CrashLastAirport), airportValue, _TRUNCATE);
	strncpy_s(CrashLastProfile, sizeof(CrashLastProfile), profileValue, _TRUNCATE);
	strncpy_s(CrashLastRadar, sizeof(CrashLastRadar), radarValue, _TRUNCATE);
	strncpy_s(CrashLastInset, sizeof(CrashLastInset), insetValue, _TRUNCATE);
}

void CSMRRadar::LoadCustomFont() {
	Logger::info(string(__FUNCSIG__));
	// Loading the custom font if there is one in use
	customFonts.clear();

	std::string fontName = GetActiveTagFontName();
	if (fontName.empty())
		fontName = "EuroScope";

	const Value& profile = CurrentConfig->getActiveProfile();
	const Value* sizeConfig = nullptr;
	const Value* weightConfig = nullptr;
	if (profile.IsObject() && profile.HasMember("font") && profile["font"].IsObject())
	{
		const Value& font = profile["font"];
		if (font.HasMember("sizes") && font["sizes"].IsObject())
			sizeConfig = &font["sizes"];
		if (font.HasMember("weight") && font["weight"].IsString())
			weightConfig = &font["weight"];
	}

	auto getFontSize = [&](const char* key, int fallback) -> int
	{
		if (sizeConfig && sizeConfig->HasMember(key) && (*sizeConfig)[key].IsInt())
		{
			int configured = (*sizeConfig)[key].GetInt();
			return (configured < 6) ? 6 : configured;
		}
		return fallback;
	};

	const int sizeOne = getFontSize("one", 10);
	const int sizeTwo = getFontSize("two", 11);
	const int sizeThree = getFontSize("three", 12);
	const int sizeFour = getFontSize("four", 13);
	const int sizeFive = getFontSize("five", 14);

	std::wstring buffer = std::wstring(fontName.begin(), fontName.end());
	Gdiplus::FontStyle fontStyle = Gdiplus::FontStyleRegular;
	if (weightConfig && strcmp(weightConfig->GetString(), "Bold") == 0)
		fontStyle = Gdiplus::FontStyleBold;
	if (weightConfig && strcmp(weightConfig->GetString(), "Italic") == 0)
		fontStyle = Gdiplus::FontStyleItalic;

	auto createFont = [&](int size) -> std::unique_ptr<Gdiplus::Font>
	{
		std::unique_ptr<Gdiplus::Font> font = std::make_unique<Gdiplus::Font>(buffer.c_str(), Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		if (font->GetLastStatus() != Gdiplus::Ok)
		{
			font = std::make_unique<Gdiplus::Font>(L"Arial", Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		}
		return font;
	};

	customFonts[1] = createFont(sizeOne);
	customFonts[2] = createFont(sizeTwo);
	customFonts[3] = createFont(sizeThree);
	customFonts[4] = createFont(sizeFour);
	customFonts[5] = createFont(sizeFive);
}

bool CSMRRadar::SetProfilesConfigPath(
	const std::string& path,
	std::string* errorText,
	bool persistToAsr)
{
	if (errorText != nullptr)
		errorText->clear();

	auto normalizeExistingPath = [](const std::string& value, std::string& result) -> bool
	{
		result.clear();
		fs::path normalized;
		if (!VsmrWindowsPath::TryResolveExistingFile(value, normalized))
			return false;
		result = normalized.u8string();
		return true;
	};

	bool sessionSelectionClaimed = false;
	const std::string sessionPath =
		CSMRPlugin::GetActiveProfilesConfigPath(&sessionSelectionClaimed);
	std::string normalizedSessionPath;
	const bool sessionPathAvailable =
		normalizeExistingPath(sessionPath, normalizedSessionPath);

	// Explicit Control Center selection always replaces the session source.
	// ASR restoration is first-screen-wins: once one valid path has claimed the
	// session, a later screen adopts it instead of switching every existing view
	// back to a stale per-screen value.
	const bool explicitSelection = persistToAsr;
	std::string normalizedRequestedPath;
	const bool requestedPathAvailable =
		normalizeExistingPath(path, normalizedRequestedPath);
	std::string normalizedPath;
	if (!explicitSelection && sessionSelectionClaimed && sessionPathAvailable)
	{
		normalizedPath = normalizedSessionPath;
	}
	else if (requestedPathAvailable)
	{
		normalizedPath = normalizedRequestedPath;
	}
	else
	{
		if (!explicitSelection && sessionPathAvailable)
		{
			normalizedPath = normalizedSessionPath;
		}
		else
		{
			if (errorText != nullptr)
				*errorText = "The selected profiles file is no longer available.";
			return false;
		}
	}
	const bool publishSessionSelection =
		explicitSelection ||
		(requestedPathAvailable &&
			(!sessionSelectionClaimed || !sessionPathAvailable));

	std::vector<CSMRRadar*> targets;
	if (publishSessionSelection)
	{
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar != nullptr &&
				std::find(targets.begin(), targets.end(), radar) == targets.end())
			{
				targets.push_back(radar);
			}
		}
	}
	if (std::find(targets.begin(), targets.end(), this) == targets.end())
		targets.insert(targets.begin(), this);

	struct Replacement
	{
		CSMRRadar* radar = nullptr;
		std::unique_ptr<CConfig> config;
	};
	std::vector<Replacement> replacements;
	replacements.reserve(targets.size());
	try
	{
		for (CSMRRadar* radar : targets)
		{
			auto config = std::make_unique<CConfig>(normalizedPath, radar->mapsPath);
			if (config->getProfileCount() == 0)
			{
				if (errorText != nullptr)
					*errorText = "The selected profiles file contains no usable profiles.";
				return false;
			}
			replacements.push_back({ radar, std::move(config) });
		}
	}
	catch (const std::exception& error)
	{
		if (errorText != nullptr)
			*errorText = "Unable to load the selected profiles file: " + std::string(error.what());
		return false;
	}
	catch (...)
	{
		if (errorText != nullptr)
			*errorText = "Unable to load the selected profiles file.";
		return false;
	}

	CConfig* primaryConfig = nullptr;
	for (const Replacement& replacement : replacements)
	{
		if (replacement.radar == this)
		{
			primaryConfig = replacement.config.get();
			break;
		}
	}
	if (primaryConfig == nullptr)
		return false;

	const std::vector<std::string> profileNames = primaryConfig->getAllProfiles();
	std::string activeProfile = primaryConfig->getLastActiveProfileName();
	if (activeProfile.empty() ||
		std::find_if(profileNames.begin(), profileNames.end(), [&](const std::string& candidate) {
			return _stricmp(candidate.c_str(), activeProfile.c_str()) == 0;
		}) == profileNames.end())
	{
		const std::string currentProfile = CurrentConfig != nullptr
			? CurrentConfig->getActiveProfileName()
			: std::string();
		const auto currentMatch = std::find_if(profileNames.begin(), profileNames.end(), [&](const std::string& candidate) {
			return _stricmp(candidate.c_str(), currentProfile.c_str()) == 0;
		});
		activeProfile = currentMatch != profileNames.end()
			? *currentMatch
			: profileNames.front();
	}

	for (Replacement& replacement : replacements)
	{
		CSMRRadar* radar = replacement.radar;
		radar->ConfigPath = normalizedPath;
		radar->CurrentConfig = std::move(replacement.config);
		radar->LoadProfile(activeProfile, false);
		radar->InvalidateAirportPositionCache();
		radar->InvalidateRunwayGeometryCache();
		radar->RadarViewZoomLevel = -1;
		radar->LastMapRunwayStatuses.clear();
		radar->LastMapActiveAirport.clear();
		if (publishSessionSelection || radar == this)
		{
			radar->SaveDataToAsr(
				"ProfilesFile",
				"Active vSMR profiles file",
				radar->ConfigPath.c_str());
		}
		radar->RequestRefresh();
	}
	if (publishSessionSelection)
		CSMRPlugin::PublishActiveProfilesConfigPath(normalizedPath, true);
	for (CSMRRadar* radar : targets)
	{
		if (radar != this &&
			radar->VsmrControlCenterDialog != nullptr)
		{
			radar->VsmrControlCenterDialog->SyncFromRadar(
				publishSessionSelection ? "resource-source" : "runtime");
		}
	}
	RememberSessionActiveProfile(activeProfile);
	return true;
}

bool CSMRRadar::ReloadConfig() {
	Logger::info("CSMRRadar::ReloadConfig()");
	std::string activeProfile = CurrentConfig ? CurrentConfig->getActiveProfileName() : "Default";
	bool reloadSucceeded = true;
	if (!CurrentConfig)
	{
		CurrentConfig = std::make_unique<CConfig>(ConfigPath, mapsPath);
		reloadSucceeded = CurrentConfig->getProfileCount() > 0;
	}
	else {
		reloadSucceeded = CurrentConfig->reload();
	}
	if (activeProfile.empty())
		activeProfile = "Default";
	if (CurrentConfig->isItActiveProfile(activeProfile) == 0 && !CurrentConfig->getAllProfiles().empty()) {
		activeProfile = CurrentConfig->getAllProfiles().front();
	}
	// A reload adopts disk as the authority. Recording the outgoing runtime
	// alerts into the freshly loaded document would give each radar a divergent
	// in-memory copy carrying the new revision token, allowing a later unrelated
	// save to overwrite the authoritative alert state.
	this->LoadProfile(activeProfile, false);
	// Force map visibility recomputation on next frame even when zoom level is unchanged.
	InvalidateAirportPositionCache();
	InvalidateRunwayGeometryCache();
	RadarViewZoomLevel = -1;
	LastMapRunwayStatuses.clear();
	LastMapActiveAirport.clear();
	AvisoGeoJsonResolvedAirport.clear();
	AvisoGeoJsonResolvedDllPath.clear();
	AvisoGeoJsonResolvedPath.clear();
	AvisoGeoJsonLastStatTick = 0;
	RequestRefresh();
	return reloadSucceeded;
}

void CSMRRadar::LoadProfile(
	string profileName,
	bool saveOutgoingState,
	bool persistNormalization) {
	Logger::info(string(__FUNCSIG__));
	// Record runtime changes only when switching within the same source. A new
	// source must never inherit state from the file it is replacing.
	if (saveOutgoingState)
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());

	// Loading the new profile
	CurrentConfig->setActiveProfile(profileName);
	const std::string effectiveProfileName = CurrentConfig->getActiveProfileName();
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::ProfileUpdate);
	strncpy_s(
		CrashActiveProfile,
		sizeof(CrashActiveProfile),
		effectiveProfileName.empty() ? "unavailable" : effectiveProfileName.c_str(),
		_TRUNCATE);
	PublishCrashRadarState("main");
	InvalidateRunwayGeometryCache();
	InvalidateStructuredTagRuleCache();
	EnsureTargetGroundStatusColorEntries(persistNormalization);

	// Loading all the new data
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	const Value* rimcasConfig = nullptr;
	if (activeProfile.IsObject() && activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		rimcasConfig = &activeProfile["rimcas"];

	RimcasRunwaysExplicitlyConfigured = false;
	if (rimcasConfig != nullptr)
	{
		if (rimcasConfig->HasMember("visibility") && (*rimcasConfig)["visibility"].IsString())
		{
			std::string visibility = (*rimcasConfig)["visibility"].GetString();
			std::transform(
				visibility.begin(),
				visibility.end(),
				visibility.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (visibility == "lvp" || visibility == "low")
				isLVP = true;
			else if (visibility == "normal")
				isLVP = false;
		}

		if (rimcasConfig->HasMember("runways") &&
			(*rimcasConfig)["runways"].IsArray() &&
			!(*rimcasConfig)["runways"].Empty())
		{
			RimcasRunwaysExplicitlyConfigured = true;
			RimcasInstance->MonitoredRunwayArr.clear();
			RimcasInstance->MonitoredRunwayDep.clear();
			RimcasInstance->ClosedRunway.clear();

			auto trimRunwayPart = [](const std::string& value) -> std::string
			{
				size_t first = 0;
				while (first < value.size() &&
					std::isspace(static_cast<unsigned char>(value[first])) != 0)
				{
					++first;
				}
				size_t last = value.size();
				while (last > first &&
					std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
				{
					--last;
				}
				return value.substr(first, last - first);
			};
			auto normalizeRunwayPair = [&](const std::string& rawName) -> std::string
			{
				std::string name = trimRunwayPart(rawName);
				const size_t slash = name.find('/');
				if (slash != std::string::npos)
				{
					const std::string first = trimRunwayPart(name.substr(0, slash));
					const std::string second = trimRunwayPart(name.substr(slash + 1));
					if (!first.empty() && !second.empty())
						name = first + " / " + second;
				}
				std::transform(
					name.begin(),
					name.end(),
					name.begin(),
					[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				return name;
			};

			const Value& runwayRows = (*rimcasConfig)["runways"];
			for (SizeType index = 0; index < runwayRows.Size(); ++index)
			{
				const Value& runway = runwayRows[index];
				if (!runway.IsObject() ||
					!runway.HasMember("id") ||
					!runway["id"].IsString())
				{
					continue;
				}

				const std::string runwayId = normalizeRunwayPair(runway["id"].GetString());
				if (runwayId.empty())
					continue;
				RimcasInstance->MonitoredRunwayArr[runwayId] =
					runway.HasMember("arrival") &&
					runway["arrival"].IsBool() &&
					runway["arrival"].GetBool();
				RimcasInstance->MonitoredRunwayDep[runwayId] =
					runway.HasMember("departure") &&
					runway["departure"].IsBool() &&
					runway["departure"].GetBool();
				RimcasInstance->ClosedRunway[runwayId] =
					runway.HasMember("closed") &&
					runway["closed"].IsBool() &&
					runway["closed"].GetBool();
			}
		}
	}

	// Inactive alerts
	unordered_set inactiveAlerts = CurrentConfig->getInactiveAlert();
	RimcasInstance->setInactiveAlerts(inactiveAlerts);
	auto readCountdownDefinition = [&](const Value* arrayValue, const std::vector<int>& fallback) -> std::vector<int>
	{
		std::vector<int> values;
		if (arrayValue != nullptr && arrayValue->IsArray())
		{
			for (SizeType i = 0; i < arrayValue->Size(); ++i)
			{
				if ((*arrayValue)[i].IsInt())
					values.push_back((*arrayValue)[i].GetInt());
			}
		}
		if (values.empty())
			values = fallback;
		return values;
	};

	const std::vector<int> defaultRimcasTimer = { 60, 45, 30, 15, 0 };
	const std::vector<int> defaultRimcasTimerLvp = { 120, 90, 60, 30, 0 };
	const Value* rimcasTimer = (rimcasConfig != nullptr && rimcasConfig->HasMember("timer")) ? &(*rimcasConfig)["timer"] : nullptr;
	const Value* rimcasTimerLvp = (rimcasConfig != nullptr && rimcasConfig->HasMember("timer_lvp")) ? &(*rimcasConfig)["timer_lvp"] : nullptr;
	const std::vector<int> RimcasNorm = readCountdownDefinition(rimcasTimer, defaultRimcasTimer);
	const std::vector<int> RimcasLVP = readCountdownDefinition(rimcasTimerLvp, defaultRimcasTimerLvp);
	RimcasInstance->setCountdownDefinition(RimcasNorm, RimcasLVP);

	customCursor = CurrentConfig->isCustomCursorUsed();
	currentFontSize = GetActiveLabelFontSize();

	// Reloading the fonts
	this->LoadCustomFont();

	TagDefinitionEditorType = "departure";
	TagDefinitionEditorDetailed = !GetTagDefinitionDetailedSameAsDefinition();
	TagDefinitionEditorDepartureStatus = "default";
	TagDefinitionEditorSelectedLine = 0;
	if (!RimcasRunwaysExplicitlyConfigured)
		RefreshLegacyRimcasRunwayMonitoring();

}

bool CSMRRadar::IsAppWindowDisplayed(int appWindowId) const
{
	const auto display = appWindowDisplays.find(appWindowId);
	return display != appWindowDisplays.end() && display->second;
}

bool CSMRRadar::UpdateTimerInsetCountdowns()
{
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	const auto timerWindow = appWindows.find(timerWindowId);
	return timerWindow != appWindows.end() &&
		timerWindow->second != nullptr &&
		timerWindow->second->UpdateTimerCountdowns();
}

void CSMRRadar::InvalidateStructuredTagRuleCache()
{
	StructuredTagRulesCache.clear();
	StructuredTagRulesCacheValid = false;
}

void UnhookAvisoMouseHook()
{
	if (gThreadMouseHook == nullptr)
		return;

	if (!::UnhookWindowsHookEx(gThreadMouseHook))
	{
		Logger::info("AVISO viewport thread wheel hook removal failed error=" +
			std::to_string(::GetLastError()));
		return;
	}
	gThreadMouseHook = nullptr;
	gThreadMouseHookThreadId = 0;
}

void UnhookAvisoKeyboardHook()
{
	if (gThreadKeyboardHook == nullptr)
		return;

	if (!::UnhookWindowsHookEx(gThreadKeyboardHook))
	{
		Logger::info("AVISO viewport thread keyboard hook removal failed error=" +
			std::to_string(::GetLastError()));
		return;
	}
	gThreadKeyboardHook = nullptr;
	gThreadKeyboardHookThreadId = 0;
}

void UnhookAvisoThreadHooks()
{
	UnhookAvisoMouseHook();
	UnhookAvisoKeyboardHook();
}

void ClearAvisoWheelRoutingState(bool cancelWindowInteractions = false)
{
	for (CSMRRadar* radarScreen : RadarScreensOpened)
	{
		if (radarScreen == nullptr)
			continue;

		radarScreen->AvisoGeoJsonScrollSelected = false;
		if (cancelWindowInteractions)
			radarScreen->CancelInsetWindowInteractions();
		for (auto& appWindow : radarScreen->appWindows)
		{
			CInsetWindow* insetWindow = appWindow.second.get();
			if (insetWindow == nullptr)
				continue;

			if (!cancelWindowInteractions && insetWindow->m_AvisoRightPanning)
				insetWindow->EndAvisoPan();
			if (cancelWindowInteractions)
			{
				insetWindow->m_AvisoScrollSelected = false;
				insetWindow->m_AvisoScreenArea = { 0, 0, 0, 0 };
				insetWindow->m_AvisoScreenAreaValid = false;
				insetWindow->m_AvisoRenderWindow = nullptr;
			}
		}
	}
}

bool IsEuroScopeViewSwitchKey(WPARAM key)
{
	return key >= VK_F1 && key <= VK_F12;
}

bool IsMouseButtonDownMessage(WPARAM message)
{
	switch (message)
	{
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_NCLBUTTONDOWN:
	case WM_NCRBUTTONDOWN:
	case WM_NCMBUTTONDOWN:
	case WM_NCXBUTTONDOWN:
		return true;
	default:
		return false;
	}
}

LRESULT CALLBACK InsetWindowSubclassProc(
	HWND hwnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam,
	UINT_PTR subclassId,
	DWORD_PTR referenceData)
{
	UNREFERENCED_PARAMETER(referenceData);

	if (subclassId != kInsetWindowSubclassId)
		return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);

	if (uMsg == WM_NCDESTROY)
	{
		::RemoveWindowSubclass(hwnd, InsetWindowSubclassProc, kInsetWindowSubclassId);
		gInsetWindowRadarScreens.erase(hwnd);
		return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
	}

	const UINT workerRefreshMessage = AvisoWorkerRefreshMessage();
	if (workerRefreshMessage != 0 && uMsg == workerRefreshMessage)
	{
		const auto radarIt = gInsetWindowRadarScreens.find(hwnd);
		CSMRRadar* requestedRadar = reinterpret_cast<CSMRRadar*>(wParam);
		const bool registered =
			requestedRadar != nullptr &&
			radarIt != gInsetWindowRadarScreens.end() &&
			std::find(radarIt->second.begin(), radarIt->second.end(), requestedRadar) != radarIt->second.end();
		if (registered && !requestedRadar->IsShutdownRequested())
		{
			try
			{
				requestedRadar->RequestRefresh();
			}
			catch (...)
			{
				Logger::info("AVISO UI refresh request failed");
			}
		}
		return 0;
	}

	switch (uMsg)
	{
	case WM_MOUSEWHEEL:
	{
		const int wheelDelta = static_cast<short>(HIWORD(wParam));
		const POINTS wheelPoint = MAKEPOINTS(lParam);
		const POINT screenPoint = {
			static_cast<LONG>(wheelPoint.x),
			static_cast<LONG>(wheelPoint.y) };
		if (TryHandleAvisoWheel(screenPoint, wheelDelta, hwnd))
			return 0;
		break;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (IsEuroScopeViewSwitchKey(wParam))
			ClearAvisoWheelRoutingState(true);
		break;
	case WM_SETCURSOR:
	{
		const auto radarIt = gInsetWindowRadarScreens.find(hwnd);
		if (radarIt != gInsetWindowRadarScreens.end())
		{
			for (CSMRRadar* radarScreen : radarIt->second)
			{
				if (radarScreen != nullptr &&
					!radarScreen->IsShutdownRequested() &&
					radarScreen->HandleInsetSetCursor(hwnd))
				{
					return TRUE;
				}
			}
		}
		// SetCursor(nullptr) explicitly hides the pointer. If cursor setup has not
		// completed (or a custom resource failed), let EuroScope choose its cursor.
		if (smrCursor != nullptr)
		{
			::SetCursor(smrCursor);
			return TRUE;
		}
		break;
	}
	default:
		break;
	}

	return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void EnsureInsetWindowProcHook(HWND hwnd, CSMRRadar* radarScreen)
{
	if (hwnd == nullptr || !::IsWindow(hwnd) || radarScreen == nullptr)
		return;

	auto existing = gInsetWindowRadarScreens.find(hwnd);
	if (existing == gInsetWindowRadarScreens.end())
	{
		if (!::SetWindowSubclass(hwnd, InsetWindowSubclassProc, kInsetWindowSubclassId, 0))
		{
			Logger::info("Inset window subclass installation failed error=" +
				std::to_string(::GetLastError()));
			return;
		}
		existing = gInsetWindowRadarScreens.emplace(
			hwnd,
			std::vector<CSMRRadar*>{}).first;
	}

	auto& radarScreens = existing->second;
	if (std::find(radarScreens.begin(), radarScreens.end(), radarScreen) == radarScreens.end())
		radarScreens.push_back(radarScreen);
}

void RemoveInsetWindowProcHooksForRadar(CSMRRadar* radarScreen)
{
	if (radarScreen == nullptr)
		return;

	for (auto it = gInsetWindowRadarScreens.begin(); it != gInsetWindowRadarScreens.end();)
	{
		auto& radarScreens = it->second;
		radarScreens.erase(
			std::remove(radarScreens.begin(), radarScreens.end(), radarScreen),
			radarScreens.end());
		if (!radarScreens.empty())
		{
			++it;
			continue;
		}

		const HWND hwnd = it->first;
		if (hwnd == nullptr || !::IsWindow(hwnd) ||
			::RemoveWindowSubclass(hwnd, InsetWindowSubclassProc, kInsetWindowSubclassId))
		{
			it = gInsetWindowRadarScreens.erase(it);
			continue;
		}

		// Keep the empty registry entry so runtime unload remains blocked while
		// Windows may still call this DLL through a live subclass callback.
		Logger::info("Inset window subclass removal failed error=" +
			std::to_string(::GetLastError()));
		++it;
	}
}

void RestoreInsetWindowProcHooks()
{
	for (auto it = gInsetWindowRadarScreens.begin(); it != gInsetWindowRadarScreens.end();)
	{
		const HWND hwnd = it->first;
		if (hwnd == nullptr || !::IsWindow(hwnd) ||
			::RemoveWindowSubclass(hwnd, InsetWindowSubclassProc, kInsetWindowSubclassId))
		{
			it = gInsetWindowRadarScreens.erase(it);
			continue;
		}

		Logger::info("Inset window subclass removal failed error=" +
			std::to_string(::GetLastError()));
		++it;
	}
}

bool TryHandleAvisoWheel(POINT screenPoint, int wheelDelta, HWND sourceHwnd)
{
	if (wheelDelta == 0)
		return false;

	for (CSMRRadar* radarScreen : RadarScreensOpened)
	{
		if (radarScreen == nullptr || radarScreen->IsShutdownRequested())
			continue;
		if (radarScreen->HandleAvisoMouseWheelAtScreenPoint(screenPoint, wheelDelta, sourceHwnd))
			return true;
	}
	return false;
}

LRESULT CALLBACK MouseMessageHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0 && IsMouseButtonDownMessage(wParam))
		ClearAvisoWheelRoutingState();

	if (code >= 0 && wParam == WM_MOUSEWHEEL && lParam != 0)
	{
		MOUSEHOOKSTRUCTEX* mouseData = reinterpret_cast<MOUSEHOOKSTRUCTEX*>(lParam);
		const int wheelDelta = static_cast<short>(HIWORD(mouseData->mouseData));
		HWND sourceHwnd = mouseData->hwnd;
		if (sourceHwnd == nullptr || !::IsWindow(sourceHwnd))
			sourceHwnd = ::WindowFromPoint(mouseData->pt);
		if (TryHandleAvisoWheel(mouseData->pt, wheelDelta, sourceHwnd))
			return 1;
	}

	return ::CallNextHookEx(gThreadMouseHook, code, wParam, lParam);
}

LRESULT CALLBACK KeyboardMessageHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	const bool keyReleased = (static_cast<ULONG_PTR>(lParam) & 0x80000000ULL) != 0;
	if (code >= 0 && !keyReleased && IsEuroScopeViewSwitchKey(wParam))
		ClearAvisoWheelRoutingState(true);

	return ::CallNextHookEx(gThreadKeyboardHook, code, wParam, lParam);
}

void EnsureAvisoWheelHooks(CSMRRadar* radarScreen)
{
	if (radarScreen == nullptr || radarScreen->IsShutdownRequested())
		return;

	const DWORD currentThreadId = ::GetCurrentThreadId();
	if (gThreadMouseHook != nullptr && gThreadMouseHookThreadId != currentThreadId)
		UnhookAvisoMouseHook();
	if (gThreadKeyboardHook != nullptr && gThreadKeyboardHookThreadId != currentThreadId)
		UnhookAvisoKeyboardHook();

	if (gThreadMouseHook == nullptr)
	{
		gThreadMouseHook = ::SetWindowsHookEx(WH_MOUSE, MouseMessageHookProc, nullptr, currentThreadId);
		if (gThreadMouseHook == nullptr)
		{
			const DWORD error = ::GetLastError();
			if (gLastThreadHookError != error)
			{
				Logger::info("AVISO viewport thread wheel hook install failed error=" + std::to_string(error));
				gLastThreadHookError = error;
			}
		}
		else
		{
			gThreadMouseHookThreadId = currentThreadId;
			gLastThreadHookError = ERROR_SUCCESS;
			Logger::info("AVISO viewport thread wheel hook installed thread=" + std::to_string(currentThreadId));
		}
	}

	if (gThreadKeyboardHook == nullptr)
	{
		gThreadKeyboardHook = ::SetWindowsHookEx(WH_KEYBOARD, KeyboardMessageHookProc, nullptr, currentThreadId);
		if (gThreadKeyboardHook == nullptr)
		{
			const DWORD error = ::GetLastError();
			if (gLastThreadKeyboardHookError != error)
			{
				Logger::info("AVISO viewport thread keyboard hook install failed error=" + std::to_string(error));
				gLastThreadKeyboardHookError = error;
			}
		}
		else
		{
			gThreadKeyboardHookThreadId = currentThreadId;
			gLastThreadKeyboardHookError = ERROR_SUCCESS;
			Logger::info("AVISO viewport thread keyboard hook installed thread=" + std::to_string(currentThreadId));
		}
	}

}


// ReSharper restore CppMsExtAddressOfClassRValue

//---EuroScopePlugInExitCustom-----------------------------------------------

void CSMRRadar::EuroScopePlugInExitCustom()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

		BeginShutdown();
		CloseVsmrControlCenterWindow();
		DestroyVsmrControlCenterWindow();

		RestoreInsetWindowProcHooks();
		UnhookAvisoThreadHooks();
}
