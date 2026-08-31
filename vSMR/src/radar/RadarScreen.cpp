#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "bootstrap/RuntimeContext.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.AvisoRuntimeState.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "radar/RadarScreenSupport.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "insets/InsetWindow.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <limits>
#include <commctrl.h>
#include "rapidjson/document.h"
#include "tags/TagColorRules.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "tags/VacdmTagHelpers.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "plugin/Plugin.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/WindowsPathEncoding.hpp"

#pragma comment(lib, "comctl32.lib")

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

namespace
{
	double AvisoMax(double left, double right)
	{
		return left > right ? left : right;
	}

	double RefreshPerfNowMs()
	{
		static LARGE_INTEGER frequency = {};
		if (frequency.QuadPart == 0)
			::QueryPerformanceFrequency(&frequency);

		LARGE_INTEGER counter = {};
		::QueryPerformanceCounter(&counter);
		return (static_cast<double>(counter.QuadPart) * 1000.0) /
			static_cast<double>(frequency.QuadPart);
	}

	void OutputVsmrDebugLine(const std::string& message)
	{
		const std::string line = "[vSMR] " + message + "\n";
		::OutputDebugStringA(line.c_str());
		Logger::info(message);
	}

	class ScopedCdcDetach
	{
	public:
		explicit ScopedCdcDetach(CDC& dc) : dc_(dc) {}
		~ScopedCdcDetach() { Detach(); }

		void Detach()
		{
			if (attached_ && dc_.GetSafeHdc() != nullptr)
				dc_.Detach();
			attached_ = false;
		}

	private:
		CDC& dc_;
		bool attached_ = true;
	};
}

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

// Utility functions
inline double closest(std::vector<double> const& vec, double value) {
	auto const it = std::lower_bound(vec.begin(), vec.end(), value);
	if (it == vec.end()) { return -1; }

	return *it;
};
inline bool IsTagBeingDragged(string c)
{
	return TagBeingDragged == c;
}
bool mouseWithin(CRect rect) {
	if (mouseLocation.x >= rect.left + 1 && mouseLocation.x <= rect.right - 1 && mouseLocation.y >= rect.top + 1 && mouseLocation.y <= rect.bottom - 1)
		return true;
	return false;
}

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

	for (auto p : possible_paths) {
		Logger::info("Trying to read callsigns from: " + p.u8string());
		if (fs::exists(p)) {
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

	this->CSMRRadar::LoadCustomFont();
	PublishCrashRadarState("main");

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
	try {
		//this->OnAsrContentToBeSaved();
		//this->EuroScopePlugInExitCustom();
	}
	catch (exception &e) {
		stringstream s;
		s << e.what() << endl;
		AfxMessageBox(string("Error occurred " + s.str()).c_str());
	}
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

void CSMRRadar::EnsureTargetGroundStatusColorEntries(bool persistChanges)
{
	// Backward-compatible profile migration and normalization:
	// ensure required nested objects, color entries and editor settings exist.
	if (!CurrentConfig || CurrentConfig->getProfileCount() == 0)
		return;

	Value& profile = CurrentConfig->getMutableActiveProfile();
	if (!profile.IsObject())
		return;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	auto ensureObjectMember = [&](Value& parent, const char* key) -> Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value newObject(kObjectType);
			parent.AddMember(keyValue, newObject, allocator);
			changed = true;
		}

		return parent[key];
	};

	auto ensureColorMember = [&](Value& parent, const char* key, int r, int g, int b, int a)
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			colorObject.AddMember("a", a, allocator);
			parent.AddMember(keyValue, colorObject, allocator);
			changed = true;
			return;
		}

		Value& colorObject = parent[key];
		auto ensureComponent = [&](const char* component, int value)
		{
			if (!colorObject.HasMember(component))
			{
				Value componentKey;
				componentKey.SetString(component, allocator);
				Value componentValue;
				componentValue.SetInt(value);
				colorObject.AddMember(componentKey, componentValue, allocator);
				changed = true;
				return;
			}

			if (!colorObject[component].IsInt() || colorObject[component].GetInt() < 0 || colorObject[component].GetInt() > 255)
			{
				colorObject[component].SetInt(value);
				changed = true;
			}
		};

		ensureComponent("r", r);
		ensureComponent("g", g);
		ensureComponent("b", b);
		ensureComponent("a", a);
	};

	auto replaceColorMember = [&](Value& parent, const char* key, const Value& sourceColor)
	{
		if (!sourceColor.IsObject())
			return;

		auto readColorComponent = [&](const char* component, int fallback) -> int
		{
			if (sourceColor.HasMember(component) && sourceColor[component].IsInt())
				return min(255, max(0, sourceColor[component].GetInt()));
			return fallback;
		};

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value colorObject(kObjectType);
		colorObject.AddMember("r", readColorComponent("r", 0), allocator);
		colorObject.AddMember("g", readColorComponent("g", 0), allocator);
		colorObject.AddMember("b", readColorComponent("b", 0), allocator);
		colorObject.AddMember("a", readColorComponent("a", 255), allocator);
		parent.AddMember(keyValue, colorObject, allocator);
		changed = true;
	};

	auto ensureBoolMember = [&](Value& parent, const char* key, bool defaultValue)
	{
		if (parent.HasMember(key) && parent[key].IsBool())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value boolValue;
		boolValue.SetBool(defaultValue);
		parent.AddMember(keyValue, boolValue, allocator);
		changed = true;
	};

	auto ensureIntMember = [&](Value& parent, const char* key, int defaultValue, int minValue, int maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = std::clamp(current, minValue, maxValue);
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value intValue;
		intValue.SetInt(defaultValue);
		parent.AddMember(keyValue, intValue, allocator);
		changed = true;
	};

	auto ensureDoubleMember = [&](Value& parent, const char* key, double defaultValue, double minValue, double maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsNumber())
		{
			double currentValue = parent[key].GetDouble();
			double boundedValue = std::clamp(currentValue, minValue, maxValue);
			if (fabs(currentValue - boundedValue) > 0.0001)
			{
				parent[key].SetDouble(boundedValue);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value numberValue;
		numberValue.SetDouble(defaultValue);
		parent.AddMember(keyValue, numberValue, allocator);
		changed = true;
	};

	auto ensureResolutionPresetMember = [&](Value& parent, const char* key, const char* defaultValue)
	{
		auto normalizePreset = [](const std::string& raw) -> std::string
		{
			std::string lowered = raw;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered.find("4k") != std::string::npos || lowered.find("2160") != std::string::npos || lowered.find("uhd") != std::string::npos)
				return "4k";
			if (lowered.find("2k") != std::string::npos || lowered.find("1440") != std::string::npos || lowered.find("qhd") != std::string::npos)
				return "2k";
			return "1080p";
		};

		if (parent.HasMember(key) && parent[key].IsString())
		{
			const std::string normalized = normalizePreset(parent[key].GetString());
			if (normalized != parent[key].GetString())
			{
				parent[key].SetString(normalized.c_str(), static_cast<rapidjson::SizeType>(normalized.size()), allocator);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		const std::string normalizedDefault = normalizePreset(defaultValue ? defaultValue : "1080p");
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value presetValue;
		presetValue.SetString(normalizedDefault.c_str(), static_cast<rapidjson::SizeType>(normalizedDefault.size()), allocator);
		parent.AddMember(keyValue, presetValue, allocator);
		changed = true;
	};

	auto appendCopiedDefinition = [&](Value& targetArray, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		for (rapidjson::SizeType i = 0; i < sourceArray.Size(); ++i)
		{
			const Value& sourceLine = sourceArray[i];
			Value copiedLine(kArrayType);
			if (sourceLine.IsArray())
			{
				for (rapidjson::SizeType j = 0; j < sourceLine.Size(); ++j)
				{
					if (sourceLine[j].IsString())
					{
						Value tokenValue;
						tokenValue.SetString(sourceLine[j].GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine[j].GetString())), allocator);
						copiedLine.PushBack(tokenValue, allocator);
					}
				}
			}
			else if (sourceLine.IsString())
			{
				Value tokenValue;
				tokenValue.SetString(sourceLine.GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine.GetString())), allocator);
				copiedLine.PushBack(tokenValue, allocator);
			}

			targetArray.PushBack(copiedLine, allocator);
		}
	};

	auto ensureDefinitionArrayMember = [&](Value& parent, const char* key, const Value* fallbackSource)
	{
		if (parent.HasMember(key) && parent[key].IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		if (fallbackSource != nullptr)
			appendCopiedDefinition(definitionArray, *fallbackSource);

		if (definitionArray.Size() == 0)
		{
			Value fallbackLine(kArrayType);
			Value fallbackToken;
			fallbackToken.SetString("callsign", allocator);
			fallbackLine.PushBack(fallbackToken, allocator);
			definitionArray.PushBack(fallbackLine, allocator);
		}

		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto replaceDefinitionArrayMember = [&](Value& parent, const char* key, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		appendCopiedDefinition(definitionArray, sourceArray);
		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto cloneJsonValue = [&](Value& destination, const Value& source, const auto& cloneRef) -> void
	{
		if (source.IsObject())
		{
			destination.SetObject();
			for (auto member = source.MemberBegin(); member != source.MemberEnd(); ++member)
			{
				Value keyValue;
				keyValue.SetString(member->name.GetString(), static_cast<rapidjson::SizeType>(strlen(member->name.GetString())), allocator);
				Value childValue;
				cloneRef(childValue, member->value, cloneRef);
				destination.AddMember(keyValue, childValue, allocator);
			}
			return;
		}

		if (source.IsArray())
		{
			destination.SetArray();
			for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
			{
				Value childValue;
				cloneRef(childValue, source[i], cloneRef);
				destination.PushBack(childValue, allocator);
			}
			return;
		}

		if (source.IsString())
		{
			destination.SetString(source.GetString(), static_cast<rapidjson::SizeType>(strlen(source.GetString())), allocator);
			return;
		}

		if (source.IsBool())
		{
			destination.SetBool(source.GetBool());
			return;
		}
		if (source.IsInt())
		{
			destination.SetInt(source.GetInt());
			return;
		}
		if (source.IsUint())
		{
			destination.SetUint(source.GetUint());
			return;
		}
		if (source.IsInt64())
		{
			destination.SetInt64(source.GetInt64());
			return;
		}
		if (source.IsUint64())
		{
			destination.SetUint64(source.GetUint64());
			return;
		}
		if (source.IsDouble())
		{
			destination.SetDouble(source.GetDouble());
			return;
		}
		if (source.IsNull())
		{
			destination.SetNull();
			return;
		}

		destination.SetNull();
	};

	auto renameMemberIfPresent = [&](Value& parent, const char* oldKey, const char* newKey)
	{
		if (!parent.IsObject() || oldKey == nullptr || newKey == nullptr || strcmp(oldKey, newKey) == 0)
			return;
		if (!parent.HasMember(oldKey))
			return;

		if (parent.HasMember(newKey))
		{
			parent.RemoveMember(oldKey);
			changed = true;
			return;
		}

		Value keyValue;
		keyValue.SetString(newKey, allocator);
		Value copiedValue;
		cloneJsonValue(copiedValue, parent[oldKey], cloneJsonValue);
		parent.AddMember(keyValue, copiedValue, allocator);
		parent.RemoveMember(oldKey);
		changed = true;
	};

	auto copyBoolMemberIfPresent = [&](Value& parent, const char* key, const Value& sourceObject)
	{
		if (!sourceObject.IsObject() || !sourceObject.HasMember(key) || !sourceObject[key].IsBool())
			return;

		const bool sourceValue = sourceObject[key].GetBool();
		if (!parent.HasMember(key) || !parent[key].IsBool())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value boolValue(sourceValue);
			parent.AddMember(keyValue, boolValue, allocator);
			changed = true;
			return;
		}

		if (parent[key].GetBool() != sourceValue)
		{
			parent[key].SetBool(sourceValue);
			changed = true;
		}
	};

	auto ensureStringArrayMember = [&](Value& parent, const char* key, const std::vector<std::string>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (const std::string& item : defaults)
		{
			Value itemValue;
			itemValue.SetString(item.c_str(), static_cast<rapidjson::SizeType>(item.size()), allocator);
			outputArray.PushBack(itemValue, allocator);
		}
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	auto ensureIntArrayMember = [&](Value& parent, const char* key, const std::vector<int>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			if (existingArray.Size() == 0)
			{
				rebuild = true;
			}
			else
			{
				for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
				{
					if (!existingArray[i].IsInt())
					{
						rebuild = true;
						break;
					}
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (int value : defaults)
			outputArray.PushBack(value, allocator);
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	ensureIntMember(profile, "schema_version", 2, 2, 9999);

	Value& filters = ensureObjectMember(profile, "filters");
	int legacyMaximumAirborneAltitudeFt = 5500;
	int legacyMaximumAirborneSpeedKt = 250;
	if (filters.HasMember("max_altitude_ft") && filters["max_altitude_ft"].IsInt())
		legacyMaximumAirborneAltitudeFt = std::clamp(filters["max_altitude_ft"].GetInt(), 0, 60000);
	if (filters.HasMember("max_speed_kt") && filters["max_speed_kt"].IsInt())
		legacyMaximumAirborneSpeedKt = std::clamp(filters["max_speed_kt"].GetInt(), 0, 1000);
	changed = filters.RemoveMember("hide_above_alt") || changed;
	changed = filters.RemoveMember("hide_above_spd") || changed;
	changed = filters.RemoveMember("max_altitude_ft") || changed;
	changed = filters.RemoveMember("max_speed_kt") || changed;
	changed = filters.RemoveMember("radar_range_nm") || changed;
	changed = filters.RemoveMember("night_alpha_setting") || changed;
	changed = filters.RemoveMember("night_overlay_alpha") || changed;
	bool legacyProModeEnabled = false;
	bool legacyTowerModeEnabled = false;
	if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
	{
		Value& proMode = filters["pro_mode"];
		renameMemberIfPresent(proMode, "enable", "enabled");
		changed = proMode.RemoveMember("do_not_autocorrelate_squawks") || changed;
		changed = proMode.RemoveMember("blocked_auto_correlate_squawks") || changed;
		if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
			legacyProModeEnabled = proMode["enabled"].GetBool();
	}
	if (filters.HasMember("tower_mode") && filters["tower_mode"].IsObject())
	{
		Value& towerMode = filters["tower_mode"];
		renameMemberIfPresent(towerMode, "enable", "enabled");
		if (towerMode.HasMember("enabled") && towerMode["enabled"].IsBool())
			legacyTowerModeEnabled = towerMode["enabled"].GetBool();
	}
	auto addDisplayMode = [&](Value& items, const char* name, bool requireAssignedSquawk)
	{
		Value mode(kObjectType);
		Value nameKey;
		nameKey.SetString("name", allocator);
		Value nameValue;
		nameValue.SetString(name, allocator);
		mode.AddMember(nameKey, nameValue, allocator);
		mode.AddMember("require_assigned_squawk", requireAssignedSquawk, allocator);
		mode.AddMember("require_clearance", false, allocator);
		mode.AddMember("require_valid_tsat", false, allocator);
		mode.AddMember("require_active_tobt", false, allocator);
		mode.AddMember("max_airborne_altitude_ft", legacyMaximumAirborneAltitudeFt, allocator);
		mode.AddMember("max_airborne_speed_kt", legacyMaximumAirborneSpeedKt, allocator);
		Value statuses(kObjectType);
		statuses.AddMember("no_status", true, allocator);
		statuses.AddMember("push", true, allocator);
		statuses.AddMember("startup", true, allocator);
		statuses.AddMember("taxi", true, allocator);
		statuses.AddMember("lineup", true, allocator);
		statuses.AddMember("departure", true, allocator);
		statuses.AddMember("on_runway", true, allocator);
		statuses.AddMember("airborne", true, allocator);
		statuses.AddMember("arrivals", true, allocator);
		statuses.AddMember("no_fpl", true, allocator);
		statuses.AddMember("uncorrelated", true, allocator);
		mode.AddMember("statuses", statuses, allocator);
		items.PushBack(mode, allocator);
	};
	Value& displayModes = ensureObjectMember(filters, "display_modes");
	if (!displayModes.HasMember("items") || !displayModes["items"].IsArray() || displayModes["items"].Empty())
	{
		if (displayModes.HasMember("items"))
			displayModes.RemoveMember("items");
		Value items(kArrayType);
		addDisplayMode(items, "Normal", false);
		addDisplayMode(items, "Pro", true);
		addDisplayMode(items, "Tower", false);
		addDisplayMode(items, "Pro + Tower", true);
		displayModes.AddMember("items", items, allocator);
		changed = true;
	}
	if (displayModes.HasMember("items") && displayModes["items"].IsArray())
	{
		Value& items = displayModes["items"];
		for (SizeType i = 0; i < items.Size(); ++i)
		{
			if (!items[i].IsObject())
				continue;
			changed = items[i].RemoveMember("do_not_autocorrelate_squawks") || changed;
			changed = items[i].RemoveMember("blocked_auto_correlate_squawks") || changed;
			ensureIntMember(items[i], "max_airborne_altitude_ft", legacyMaximumAirborneAltitudeFt, 0, 60000);
			ensureIntMember(items[i], "max_airborne_speed_kt", legacyMaximumAirborneSpeedKt, 0, 1000);
			Value& statuses = ensureObjectMember(items[i], "statuses");
			if (!statuses.HasMember("lineup") || !statuses["lineup"].IsBool())
			{
				const bool visible = statuses.HasMember("lnup") && statuses["lnup"].IsBool()
					? statuses["lnup"].GetBool()
					: (statuses.HasMember("taxi") && statuses["taxi"].IsBool() ? statuses["taxi"].GetBool() : true);
				ensureBoolMember(statuses, "lineup", visible);
			}
			if (statuses.HasMember("lnup"))
			{
				statuses.RemoveMember("lnup");
				changed = true;
			}
		}
	}
	std::string activeDisplayMode = "Normal";
	if (legacyProModeEnabled && legacyTowerModeEnabled)
		activeDisplayMode = "Pro + Tower";
	else if (legacyProModeEnabled)
		activeDisplayMode = "Pro";
	else if (legacyTowerModeEnabled)
		activeDisplayMode = "Tower";
	if (!displayModes.HasMember("active") || !displayModes["active"].IsString() || displayModes["active"].GetStringLength() == 0)
	{
		if (displayModes.HasMember("active"))
			displayModes.RemoveMember("active");
		Value activeValue;
		activeValue.SetString(activeDisplayMode.c_str(), static_cast<SizeType>(activeDisplayMode.size()), allocator);
		displayModes.AddMember("active", activeValue, allocator);
		changed = true;
	}
	if (filters.HasMember("pro_mode"))
	{
		filters.RemoveMember("pro_mode");
		changed = true;
	}
	if (filters.HasMember("tower_mode"))
	{
		filters.RemoveMember("tower_mode");
		changed = true;
	}

	Value& rimcas = ensureObjectMember(profile, "rimcas");
	changed = rimcas.RemoveMember("rimcas_stage_two_speed_threshold") || changed;
	changed = rimcas.RemoveMember("stage_two_speed_threshold_kt") || changed;
	changed = rimcas.RemoveMember("enabled") || changed;
	changed = rimcas.RemoveMember("rimcas_label_only") || changed;
	changed = rimcas.RemoveMember("use_red_symbol_for_emergencies") || changed;
	ensureIntArrayMember(rimcas, "timer", { 60, 45, 30, 15, 0 });
	ensureIntArrayMember(rimcas, "timer_lvp", { 120, 90, 60, 30, 0 });
	ensureColorMember(rimcas, "background_color_stage_one", 160, 90, 30, 255);
	ensureColorMember(rimcas, "background_color_stage_two", 150, 0, 0, 255);
	ensureColorMember(rimcas, "caution_alert_text_color", 0, 0, 0, 255);
	ensureColorMember(rimcas, "warning_alert_text_color", 255, 255, 255, 255);
	ensureColorMember(rimcas, "caution_alert_background_color", 255, 255, 0, 255);
	ensureColorMember(rimcas, "warning_alert_background_color", 255, 0, 0, 255);
	ensureStringArrayMember(rimcas, "inactive_alerts", {});

	const std::vector<std::string> defaultTagFonts = {
		"EuroScope",
		"Consolas",
		"Lucida Console",
		"Courier New",
		"Segoe UI",
		"Tahoma",
		"Arial",
		"ods",
		"Deesse Medium"
	};

	Value& font = ensureObjectMember(profile, "font");
	if (!font.HasMember("font_name") || !font["font_name"].IsString() || strlen(font["font_name"].GetString()) == 0)
	{
		if (font.HasMember("font_name"))
			font.RemoveMember("font_name");

		Value keyValue;
		keyValue.SetString("font_name", allocator);
		Value fontNameValue;
		fontNameValue.SetString("EuroScope", allocator);
		font.AddMember(keyValue, fontNameValue, allocator);
		changed = true;
	}

	if (!font.HasMember("weight") || !font["weight"].IsString())
	{
		if (font.HasMember("weight"))
			font.RemoveMember("weight");

		Value keyValue;
		keyValue.SetString("weight", allocator);
		Value weightValue;
		weightValue.SetString("Regular", allocator);
		font.AddMember(keyValue, weightValue, allocator);
		changed = true;
	}

	Value& fontSizes = ensureObjectMember(font, "sizes");
	auto ensureFontSizeMember = [&](Value& parent, const char* key, int fallback)
	{
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = (current < 6) ? 6 : current;
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value value;
		value.SetInt((fallback < 6) ? 6 : fallback);
		parent.AddMember(keyValue, value, allocator);
		changed = true;
	};

	ensureFontSizeMember(fontSizes, "one", 10);
	ensureFontSizeMember(fontSizes, "two", 11);
	ensureFontSizeMember(fontSizes, "three", 12);
	ensureFontSizeMember(fontSizes, "four", 13);
	ensureFontSizeMember(fontSizes, "five", 14);

	auto ensureAvailableFontList = [&](Value& fontObject, const char* key)
	{
		bool rebuild = false;
		if (!fontObject.HasMember(key) || !fontObject[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			Value& existingArray = fontObject[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (rebuild)
		{
			if (fontObject.HasMember(key))
				fontObject.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value fontArray(kArrayType);
			for (const std::string& fontName : defaultTagFonts)
			{
				Value fontValue;
				fontValue.SetString(fontName.c_str(), static_cast<rapidjson::SizeType>(fontName.size()), allocator);
				fontArray.PushBack(fontValue, allocator);
			}
			fontObject.AddMember(keyValue, fontArray, allocator);
			changed = true;
			return;
		}
	};

	ensureAvailableFontList(font, "available_fonts");
	ensureIntMember(font, "label_font_size", 1, 1, 5);

	Value& targets = ensureObjectMember(profile, "targets");
	renameMemberIfPresent(targets, "small_icon_boost_resolution", "small_icon_boost_resolution_preset");
	if (!targets.HasMember("icon_style") || !targets["icon_style"].IsString())
	{
		if (targets.HasMember("icon_style"))
			targets.RemoveMember("icon_style");

		Value keyValue;
		keyValue.SetString("icon_style", allocator);
		Value value;
		value.SetString("realistic", allocator);
		targets.AddMember(keyValue, value, allocator);
		changed = true;
	}
	else
	{
		const std::string normalizedIconStyle = NormalizeTargetIconStyle(targets["icon_style"].GetString());
		if (normalizedIconStyle != targets["icon_style"].GetString())
		{
			targets["icon_style"].SetString(normalizedIconStyle.c_str(), static_cast<rapidjson::SizeType>(normalizedIconStyle.size()), allocator);
			changed = true;
		}
	}
	ensureResolutionPresetMember(targets, "small_icon_boost_resolution_preset", "1080p");
	ensureBoolMember(targets, "trail_enabled", true);
	ensureIntMember(targets, "trail_ground_points", 4, 0, 16);
	ensureIntMember(targets, "trail_airborne_points", 8, 0, 16);
	ensureDoubleMember(targets, "symbol_scale", 1.0, 0.5, 1.5);
	changed = targets.RemoveMember("small_icon_boost") || changed;
	changed = targets.RemoveMember("small_icon_boost_factor") || changed;
	changed = targets.RemoveMember("fixed_pixel_icon_size") || changed;
	changed = targets.RemoveMember("fixed_pixel_icon_scale") || changed;
	changed = targets.RemoveMember("fixed_pixel_triangle_scale") || changed;
	changed = targets.RemoveMember("show_primary_target") || changed;
	ensureColorMember(targets, "target_color", 255, 242, 73, 255);
	changed = targets.RemoveMember("history_one_color") || changed;
	changed = targets.RemoveMember("history_two_color") || changed;
	changed = targets.RemoveMember("history_three_color") || changed;

	Value* legacyGroundIcons = nullptr;
	if (targets.HasMember("ground_icons") && targets["ground_icons"].IsObject())
		legacyGroundIcons = &targets["ground_icons"];

	Value& departureIcons = ensureObjectMember(targets, "departure");
	Value& arrivalIcons = ensureObjectMember(targets, "arrival");

	auto migrateTargetIconColor = [&](Value& destination, const char* destinationKey, int r, int g, int b, int a, const char* legacyPrimary, const char* legacySecondary = nullptr, const char* legacyTertiary = nullptr)
	{
		if ((!destination.HasMember(destinationKey) || !destination[destinationKey].IsObject()) && legacyGroundIcons != nullptr)
		{
			const Value* sourceColor = nullptr;
			auto pickLegacyColor = [&](const char* legacyKey) -> bool
			{
				if (legacyKey == nullptr || !legacyGroundIcons->HasMember(legacyKey))
					return false;
				const Value& candidate = (*legacyGroundIcons)[legacyKey];
				if (!candidate.IsObject())
					return false;
				sourceColor = &candidate;
				return true;
			};

			if (!pickLegacyColor(legacyPrimary))
				if (!pickLegacyColor(legacySecondary))
					pickLegacyColor(legacyTertiary);

			if (sourceColor != nullptr)
				replaceColorMember(destination, destinationKey, *sourceColor);
		}

		ensureColorMember(destination, destinationKey, r, g, b, a);
	};

	migrateTargetIconColor(departureIcons, "airborne", 240, 240, 240, 255, "airborne_departure");
	migrateTargetIconColor(departureIcons, "departure", 240, 240, 240, 255, "depa");
	migrateTargetIconColor(departureIcons, "gate", 165, 165, 165, 255, "departure_gate", "gate");
	migrateTargetIconColor(departureIcons, "no_fpl", 128, 128, 128, 255, "nofpl");
	migrateTargetIconColor(departureIcons, "no_status", 165, 165, 165, 255, "nsts");
	migrateTargetIconColor(departureIcons, "push", 253, 218, 13, 255, "push");
	migrateTargetIconColor(departureIcons, "startup", 253, 218, 13, 255, "stup");
	migrateTargetIconColor(departureIcons, "taxi", 240, 240, 240, 255, "taxi");
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) && legacyGroundIcons != nullptr)
	{
		const Value* legacyLineupColor = nullptr;
		if (legacyGroundIcons->HasMember("lnup") && (*legacyGroundIcons)["lnup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lnup"];
		else if (legacyGroundIcons->HasMember("lineup") && (*legacyGroundIcons)["lineup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lineup"];
		if (legacyLineupColor != nullptr)
			replaceColorMember(departureIcons, "lineup", *legacyLineupColor);
	}
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) &&
		departureIcons.HasMember("taxi") && departureIcons["taxi"].IsObject())
	{
		Value taxiColorCopy;
		cloneJsonValue(taxiColorCopy, departureIcons["taxi"], cloneJsonValue);
		replaceColorMember(departureIcons, "lineup", taxiColorCopy);
	}
	ensureColorMember(departureIcons, "lineup", 240, 240, 240, 255);

	migrateTargetIconColor(arrivalIcons, "airborne", 120, 190, 240, 255, "airborne_arrival");
	migrateTargetIconColor(arrivalIcons, "gate", 165, 165, 165, 255, "arrival_gate", "gate");
	migrateTargetIconColor(arrivalIcons, "on_ground", 165, 165, 165, 255, "arr", "arrival_taxi");

	if (targets.HasMember("ground_icons"))
	{
		targets.RemoveMember("ground_icons");
		changed = true;
	}

	if (!profile.HasMember("sid_text_colors") || !profile["sid_text_colors"].IsArray())
	{
		if (profile.HasMember("sid_text_colors"))
			profile.RemoveMember("sid_text_colors");

		Value sidTextColorsKey;
		sidTextColorsKey.SetString("sid_text_colors", allocator);
		Value sidTextColorsArray(kArrayType);
		profile.AddMember(sidTextColorsKey, sidTextColorsArray, allocator);
		changed = true;
	}

	Value& labels = ensureObjectMember(profile, "labels");
	changed = labels.RemoveMember("use_aspeed_for_gate") || changed;
	changed = labels.RemoveMember("use_speed_for_gate") || changed;
	changed = labels.RemoveMember("leader_line_length") || changed;
	changed = labels.RemoveMember("use_departure_arrival_coloring") || changed;
	renameMemberIfPresent(labels, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	ensureBoolMember(labels, "auto_deconfliction", true);
	ensureBoolMember(labels, "rounded_corners", true);
	ensureBoolMember(labels, "definition_detailed_inherits_normal", false);
	if (labels.HasMember("sid_text_colors"))
	{
		labels.RemoveMember("sid_text_colors");
		changed = true;
	}

	Value& departureLabel = ensureObjectMember(labels, "departure");
	renameMemberIfPresent(departureLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(departureLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(departureLabel, "background_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "gate_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "on_runway_color", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(departureLabel, "nofpl_color", "background_no_fpl_color");
	renameMemberIfPresent(departureLabel, "nosid_color", "background_no_sid_color");
	renameMemberIfPresent(departureLabel, "push_color", "background_push_color");
	renameMemberIfPresent(departureLabel, "startup_color", "background_startup_color");
	renameMemberIfPresent(departureLabel, "taxi_color", "background_taxi_color");
	renameMemberIfPresent(departureLabel, "lineup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "lnup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "departure_color", "background_departure_color");
	if (departureLabel.HasMember("status_background_colors") && departureLabel["status_background_colors"].IsObject())
	{
		Value& departureStatusColors = departureLabel["status_background_colors"];
		if (departureStatusColors.HasMember("nsts") && departureStatusColors["nsts"].IsObject())
			replaceColorMember(departureLabel, "background_no_status_color", departureStatusColors["nsts"]);
		if (departureStatusColors.HasMember("push") && departureStatusColors["push"].IsObject())
			replaceColorMember(departureLabel, "background_push_color", departureStatusColors["push"]);
		if (departureStatusColors.HasMember("stup") && departureStatusColors["stup"].IsObject())
			replaceColorMember(departureLabel, "background_startup_color", departureStatusColors["stup"]);
		if (departureStatusColors.HasMember("taxi") && departureStatusColors["taxi"].IsObject())
			replaceColorMember(departureLabel, "background_taxi_color", departureStatusColors["taxi"]);
		if (departureStatusColors.HasMember("lnup") && departureStatusColors["lnup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lnup"]);
		else if (departureStatusColors.HasMember("lineup") && departureStatusColors["lineup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lineup"]);
		if (departureStatusColors.HasMember("depa") && departureStatusColors["depa"].IsObject())
			replaceColorMember(departureLabel, "background_departure_color", departureStatusColors["depa"]);
		departureLabel.RemoveMember("status_background_colors");
		changed = true;
	}
	ensureColorMember(departureLabel, "background_no_status_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "background_on_runway_color", 40, 50, 200, 255);
	ensureColorMember(departureLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(departureLabel, "background_push_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_startup_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_taxi_color", 240, 240, 240, 255);
	if ((!departureLabel.HasMember("background_lineup_color") || !departureLabel["background_lineup_color"].IsObject()) &&
		departureLabel.HasMember("background_taxi_color") && departureLabel["background_taxi_color"].IsObject())
	{
		Value taxiColorCopy;
		cloneJsonValue(taxiColorCopy, departureLabel["background_taxi_color"], cloneJsonValue);
		replaceColorMember(departureLabel, "background_lineup_color", taxiColorCopy);
	}
	ensureColorMember(departureLabel, "background_lineup_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_departure_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_no_fpl_color", 128, 128, 128, 255);
	ensureColorMember(departureLabel, "background_no_sid_color", 53, 126, 187, 255);

	Value& arrivalLabel = ensureObjectMember(labels, "arrival");
	renameMemberIfPresent(arrivalLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(arrivalLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(arrivalLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(arrivalLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "nofpl_color", "background_no_fpl_color");
	ensureColorMember(arrivalLabel, "background_on_ground_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "background_on_runway_color", 170, 50, 50, 255);
	ensureColorMember(arrivalLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_no_fpl_color", 128, 128, 128, 255);
	if (arrivalLabel.HasMember("status_background_colors") && arrivalLabel["status_background_colors"].IsObject())
	{
		Value& arrivalStatusColors = arrivalLabel["status_background_colors"];
		if (arrivalStatusColors.HasMember("arr") && arrivalStatusColors["arr"].IsObject())
			replaceColorMember(arrivalLabel, "background_on_ground_color", arrivalStatusColors["arr"]);
		arrivalLabel.RemoveMember("status_background_colors");
		changed = true;
	}

	Value& airborneLabel = ensureObjectMember(labels, "airborne");
	if (airborneLabel.HasMember("departure_background_color") && airborneLabel["departure_background_color"].IsObject())
		replaceColorMember(departureLabel, "background_airborne_color", airborneLabel["departure_background_color"]);
	if (airborneLabel.HasMember("departure_text_color") && airborneLabel["departure_text_color"].IsObject())
		replaceColorMember(departureLabel, "text_airborne_color", airborneLabel["departure_text_color"]);
	if (airborneLabel.HasMember("arrival_background_color") && airborneLabel["arrival_background_color"].IsObject())
		replaceColorMember(arrivalLabel, "background_airborne_color", airborneLabel["arrival_background_color"]);
	if (airborneLabel.HasMember("arrival_text_color") && airborneLabel["arrival_text_color"].IsObject())
		replaceColorMember(arrivalLabel, "text_airborne_color", airborneLabel["arrival_text_color"]);
	ensureColorMember(departureLabel, "background_airborne_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "text_airborne_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_airborne_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "text_airborne_color", 255, 255, 255, 255);
	if (airborneLabel.HasMember("background_color"))
	{
		airborneLabel.RemoveMember("background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("background_color_on_runway"))
	{
		airborneLabel.RemoveMember("background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("text_color"))
	{
		airborneLabel.RemoveMember("text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("departure_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("arrival_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color"))
	{
		airborneLabel.RemoveMember("departure_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color"))
	{
		airborneLabel.RemoveMember("arrival_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_text_color"))
	{
		airborneLabel.RemoveMember("departure_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_text_color"))
	{
		airborneLabel.RemoveMember("arrival_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("use_departure_arrival_coloring"))
	{
		airborneLabel.RemoveMember("use_departure_arrival_coloring");
		changed = true;
	}

	Value& uncorrelatedLabel = ensureObjectMember(labels, "uncorrelated");
	renameMemberIfPresent(uncorrelatedLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(uncorrelatedLabel, "background_color_on_runway", "background_on_runway_color");
	ensureColorMember(uncorrelatedLabel, "background_on_ground_color", 150, 22, 135, 255);
	ensureColorMember(uncorrelatedLabel, "background_on_runway_color", 150, 22, 135, 50);

	ensureDefinitionArrayMember(departureLabel, "definition", nullptr);
	const Value* baseDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(departureLabel, "definition_detailed", baseDefinition);
	Value& departureStatusDefinitions = ensureObjectMember(departureLabel, "status_definitions");
	renameMemberIfPresent(departureStatusDefinitions, "lineup", "lnup");
	if (departureStatusDefinitions.HasMember("nsts") && departureStatusDefinitions["nsts"].IsObject())
	{
		Value& departureNstsSection = departureStatusDefinitions["nsts"];
		renameMemberIfPresent(departureNstsSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(departureNstsSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		if (departureNstsSection.HasMember("definition") && departureNstsSection["definition"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition", departureNstsSection["definition"]);
		if (departureNstsSection.HasMember("definition_detailed") && departureNstsSection["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition_detailed", departureNstsSection["definition_detailed"]);
		copyBoolMemberIfPresent(departureLabel, "definition_detailed_inherits_normal", departureNstsSection);
		departureStatusDefinitions.RemoveMember("nsts");
		changed = true;
	}

	auto ensureStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(departureStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (departureLabel.HasMember("definition_detailed") && departureLabel["definition_detailed"].IsArray()) ? &departureLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	if ((!departureStatusDefinitions.HasMember("lnup") || !departureStatusDefinitions["lnup"].IsObject()) &&
		departureStatusDefinitions.HasMember("taxi") && departureStatusDefinitions["taxi"].IsObject())
	{
		if (departureStatusDefinitions.HasMember("lnup"))
			departureStatusDefinitions.RemoveMember("lnup");
		Value keyValue;
		keyValue.SetString("lnup", allocator);
		Value copiedValue;
		cloneJsonValue(copiedValue, departureStatusDefinitions["taxi"], cloneJsonValue);
		departureStatusDefinitions.AddMember(keyValue, copiedValue, allocator);
		changed = true;
	}
	ensureStatusDefinitionEntries("taxi");
	ensureStatusDefinitionEntries("lnup");
	ensureStatusDefinitionEntries("push");
	ensureStatusDefinitionEntries("stup");
	ensureStatusDefinitionEntries("depa");
	ensureStatusDefinitionEntries("nofpl");
	ensureStatusDefinitionEntries("airdep");
	ensureStatusDefinitionEntries("airdep_onrunway");

	ensureDefinitionArrayMember(arrivalLabel, "definition", nullptr);
	const Value* arrivalBaseDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(arrivalLabel, "definition_detailed", arrivalBaseDefinition);
	Value& arrivalStatusDefinitions = ensureObjectMember(arrivalLabel, "status_definitions");
	if (arrivalStatusDefinitions.HasMember("arr"))
	{
		arrivalStatusDefinitions.RemoveMember("arr");
		changed = true;
	}
	if (arrivalStatusDefinitions.HasMember("taxi"))
	{
		arrivalStatusDefinitions.RemoveMember("taxi");
		changed = true;
	}
	auto ensureArrivalStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(arrivalStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (arrivalLabel.HasMember("definition_detailed") && arrivalLabel["definition_detailed"].IsArray()) ? &arrivalLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	ensureArrivalStatusDefinitionEntries("nofpl");
	ensureArrivalStatusDefinitionEntries("airarr");
	ensureArrivalStatusDefinitionEntries("airarr_onrunway");

	auto copyStatusDefinitionFromSource = [&](Value& destinationStatusDefinitions, const char* destinationStatusKey, const Value* sourceSection)
	{
		if (destinationStatusKey == nullptr || sourceSection == nullptr || !sourceSection->IsObject())
			return;

		Value& destinationSection = ensureObjectMember(destinationStatusDefinitions, destinationStatusKey);
		renameMemberIfPresent(destinationSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(destinationSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");

		if (sourceSection->HasMember("definition") && (*sourceSection)["definition"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition", (*sourceSection)["definition"]);
		if (sourceSection->HasMember("definition_detailed") && (*sourceSection)["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definition_detailed"]);
		else if (sourceSection->HasMember("definitionDetailled") && (*sourceSection)["definitionDetailled"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definitionDetailled"]);

		copyBoolMemberIfPresent(destinationSection, "definition_detailed_inherits_normal", *sourceSection);
	};

	const Value* airborneDefinition = nullptr;
	if (airborneLabel.HasMember("definition") && airborneLabel["definition"].IsArray())
		airborneDefinition = &airborneLabel["definition"];

	auto findAirborneStatusSection = [&](const char* statusKey) -> const Value*
	{
		if (statusKey == nullptr)
			return nullptr;
		if (!airborneLabel.HasMember("status_definitions") || !airborneLabel["status_definitions"].IsObject())
			return nullptr;
		const Value& airborneStatusDefinitions = airborneLabel["status_definitions"];
		if (!airborneStatusDefinitions.HasMember(statusKey) || !airborneStatusDefinitions[statusKey].IsObject())
			return nullptr;
		return &airborneStatusDefinitions[statusKey];
	};

	const Value* sourceDepartureAirborne = findAirborneStatusSection("airdep");
	if (sourceDepartureAirborne == nullptr && airborneDefinition != nullptr)
		sourceDepartureAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep", sourceDepartureAirborne);

	const Value* sourceDepartureOnRunway = findAirborneStatusSection("airdep_onrunway");
	if (sourceDepartureOnRunway == nullptr)
		sourceDepartureOnRunway = sourceDepartureAirborne;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep_onrunway", sourceDepartureOnRunway);

	const Value* sourceArrivalAirborne = findAirborneStatusSection("airarr");
	if (sourceArrivalAirborne == nullptr && airborneDefinition != nullptr)
		sourceArrivalAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr", sourceArrivalAirborne);

	const Value* sourceArrivalOnRunway = findAirborneStatusSection("airarr_onrunway");
	if (sourceArrivalOnRunway == nullptr)
		sourceArrivalOnRunway = sourceArrivalAirborne;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr_onrunway", sourceArrivalOnRunway);

	Value legacyLabelRules(kObjectType);
	bool hasLegacyLabelRules = false;
	if (labels.HasMember("rules") && labels["rules"].IsObject())
	{
		cloneJsonValue(legacyLabelRules, labels["rules"], cloneJsonValue);
		hasLegacyLabelRules = true;
	}

	Value& structuredRules = ensureObjectMember(profile, "rules");
	const bool structuredRulesHasItems = structuredRules.HasMember("items") && structuredRules["items"].IsArray();
	if (!structuredRulesHasItems && hasLegacyLabelRules &&
		legacyLabelRules.HasMember("items") && legacyLabelRules["items"].IsArray())
	{
		cloneJsonValue(structuredRules, legacyLabelRules, cloneJsonValue);
		changed = true;
	}

	if (hasLegacyLabelRules)
	{
		Value& labelsForRulesCleanup = profile["labels"];
		labelsForRulesCleanup.RemoveMember("rules");
		changed = true;
	}

	ensureIntMember(structuredRules, "version", 1, 1, 1000);
	if (!structuredRules.HasMember("items") || !structuredRules["items"].IsArray())
	{
		if (structuredRules.HasMember("items"))
			structuredRules.RemoveMember("items");
		Value itemsKey;
		itemsKey.SetString("items", allocator);
		Value itemsArray(kArrayType);
		structuredRules.AddMember(itemsKey, itemsArray, allocator);
		changed = true;
	}
	Value& structuredRuleItems = structuredRules["items"];

	auto parseRuleColor = [&](const Value& item, const char* key, bool& outApply, int& outR, int& outG, int& outB)
	{
		outApply = false;
		outR = 255;
		outG = 255;
		outB = 255;
		if (!item.IsObject() || !item.HasMember(key) || !item[key].IsObject())
			return false;

		const Value& color = item[key];
		if (!color.HasMember("r") || !color["r"].IsInt() ||
			!color.HasMember("g") || !color["g"].IsInt() ||
			!color.HasMember("b") || !color["b"].IsInt())
		{
			return false;
		}

		outApply = true;
		outR = std::clamp(color["r"].GetInt(), 0, 255);
		outG = std::clamp(color["g"].GetInt(), 0, 255);
		outB = std::clamp(color["b"].GetInt(), 0, 255);
		return true;
	};

	auto buildRuleSignature = [&](const std::string& source, const std::string& token, const std::string& condition,
		const std::string& tagType, const std::string& status, const std::string& detail,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB) -> std::string
	{
		std::ostringstream signature;
		signature << source << "|"
			<< token << "|"
			<< condition << "|"
			<< tagType << "|"
			<< status << "|"
			<< detail << "|"
			<< (applyTarget ? 1 : 0) << ":" << targetR << "," << targetG << "," << targetB << "|"
			<< (applyTag ? 1 : 0) << ":" << tagR << "," << tagG << "," << tagB << "|"
			<< (applyText ? 1 : 0) << ":" << textR << "," << textG << "," << textB;
		return signature.str();
	};

	std::set<std::string> existingRuleSignatures;
	for (rapidjson::SizeType i = 0; i < structuredRuleItems.Size(); ++i)
	{
		if (!structuredRuleItems[i].IsObject())
			continue;

		const Value& item = structuredRuleItems[i];
		std::string source = "vacdm";
		if (item.HasMember("source") && item["source"].IsString())
			source = item["source"].GetString();
		else if (item.HasMember("kind") && item["kind"].IsString())
			source = item["kind"].GetString();
		source = NormalizeStructuredRuleSource(source);

		std::string token;
		if (item.HasMember("token") && item["token"].IsString())
			token = item["token"].GetString();
		token = NormalizeStructuredRuleToken(source, token);
		if (token.empty())
			continue;

		std::string condition = "any";
		if (item.HasMember("condition") && item["condition"].IsString())
			condition = item["condition"].GetString();
		else if (source == "runway" && item.HasMember("runway") && item["runway"].IsString())
			condition = item["runway"].GetString();
		else if (source != "runway" && item.HasMember("state") && item["state"].IsString())
			condition = item["state"].GetString();
		condition = NormalizeStructuredRuleCondition(source, condition);

		std::string tagType = "any";
		if (item.HasMember("tag_type") && item["tag_type"].IsString())
			tagType = item["tag_type"].GetString();
		tagType = NormalizeStructuredRuleTagType(tagType);

		std::string status = "any";
		if (item.HasMember("status") && item["status"].IsString())
			status = item["status"].GetString();
		status = NormalizeStructuredRuleStatus(status);

		std::string detail = "any";
		if (item.HasMember("detail") && item["detail"].IsString())
			detail = item["detail"].GetString();
		detail = NormalizeStructuredRuleDetail(detail);

		bool applyTarget = false;
		bool applyTag = false;
		bool applyText = false;
		int targetR = 255, targetG = 255, targetB = 255;
		int tagR = 255, tagG = 255, tagB = 255;
		int textR = 255, textG = 255, textB = 255;
		parseRuleColor(item, "target_color", applyTarget, targetR, targetG, targetB);
		parseRuleColor(item, "tag_color", applyTag, tagR, tagG, tagB);
		parseRuleColor(item, "text_color", applyText, textR, textG, textB);
		if (!applyTarget && !applyTag && !applyText)
			continue;

		existingRuleSignatures.insert(buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB));
	}

	auto appendStructuredRule = [&](const std::string& sourceRaw, const std::string& tokenRaw, const std::string& conditionRaw,
		const std::string& tagTypeRaw, const std::string& statusRaw, const std::string& detailRaw,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB)
	{
		const std::string source = NormalizeStructuredRuleSource(sourceRaw);
		const std::string token = NormalizeStructuredRuleToken(source, tokenRaw);
		if (token.empty())
			return;
		const std::string condition = NormalizeStructuredRuleCondition(source, conditionRaw);
		const std::string tagType = NormalizeStructuredRuleTagType(tagTypeRaw);
		const std::string status = NormalizeStructuredRuleStatus(statusRaw);
		const std::string detail = NormalizeStructuredRuleDetail(detailRaw);

		targetR = std::clamp(targetR, 0, 255);
		targetG = std::clamp(targetG, 0, 255);
		targetB = std::clamp(targetB, 0, 255);
		tagR = std::clamp(tagR, 0, 255);
		tagG = std::clamp(tagG, 0, 255);
		tagB = std::clamp(tagB, 0, 255);
		textR = std::clamp(textR, 0, 255);
		textG = std::clamp(textG, 0, 255);
		textB = std::clamp(textB, 0, 255);

		if (!applyTarget && !applyTag && !applyText)
			return;

		const std::string signature = buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB);
		if (!existingRuleSignatures.insert(signature).second)
			return;

		Value ruleObject(kObjectType);
		Value sourceKey;
		sourceKey.SetString("source", allocator);
		Value sourceValue;
		sourceValue.SetString(source.c_str(), static_cast<rapidjson::SizeType>(source.size()), allocator);
		ruleObject.AddMember(sourceKey, sourceValue, allocator);

		Value tokenKey;
		tokenKey.SetString("token", allocator);
		Value tokenValue;
		tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
		ruleObject.AddMember(tokenKey, tokenValue, allocator);

		Value conditionKey;
		conditionKey.SetString("condition", allocator);
		Value conditionValue;
		conditionValue.SetString(condition.c_str(), static_cast<rapidjson::SizeType>(condition.size()), allocator);
		ruleObject.AddMember(conditionKey, conditionValue, allocator);

		Value tagTypeKey;
		tagTypeKey.SetString("tag_type", allocator);
		Value tagTypeValue;
		tagTypeValue.SetString(tagType.c_str(), static_cast<rapidjson::SizeType>(tagType.size()), allocator);
		ruleObject.AddMember(tagTypeKey, tagTypeValue, allocator);

		Value statusKey;
		statusKey.SetString("status", allocator);
		Value statusValue;
		statusValue.SetString(status.c_str(), static_cast<rapidjson::SizeType>(status.size()), allocator);
		ruleObject.AddMember(statusKey, statusValue, allocator);

		Value detailKey;
		detailKey.SetString("detail", allocator);
		Value detailValue;
		detailValue.SetString(detail.c_str(), static_cast<rapidjson::SizeType>(detail.size()), allocator);
		ruleObject.AddMember(detailKey, detailValue, allocator);

		auto appendRuleColor = [&](const char* key, bool apply, int r, int g, int b)
		{
			if (!apply)
				return;
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			Value colorKey;
			colorKey.SetString(key, allocator);
			ruleObject.AddMember(colorKey, colorObject, allocator);
		};

		appendRuleColor("target_color", applyTarget, targetR, targetG, targetB);
		appendRuleColor("tag_color", applyTag, tagR, tagG, tagB);
		appendRuleColor("text_color", applyText, textR, textG, textB);

		structuredRuleItems.PushBack(ruleObject, allocator);
		changed = true;
	};

	auto migrateDefinitionArray = [&](Value& definitionArray, const std::string& tagType, const std::string& status, const std::string& detail)
	{
		// Move legacy inline color-rule tokens into structured rules and keep display tokens intact.
		if (!definitionArray.IsArray())
			return;

		for (rapidjson::SizeType lineIndex = 0; lineIndex < definitionArray.Size(); ++lineIndex)
		{
			Value& lineValue = definitionArray[lineIndex];
			std::vector<std::string> sourceTokens;
			if (lineValue.IsArray())
			{
				for (rapidjson::SizeType tokenIndex = 0; tokenIndex < lineValue.Size(); ++tokenIndex)
				{
					if (lineValue[tokenIndex].IsString())
						sourceTokens.push_back(lineValue[tokenIndex].GetString());
				}
			}
			else if (lineValue.IsString())
			{
				sourceTokens = SplitDefinitionTokens(lineValue.GetString());
			}
			else
			{
				continue;
			}

			if (sourceTokens.empty())
				continue;

			bool removedRuleToken = false;
			std::vector<std::string> keptTokens;
			keptTokens.reserve(sourceTokens.size());

			for (const std::string& rawToken : sourceTokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;

				VacdmColorRuleDefinition vacdmRuleToken;
				if (TryParseVacdmColorRuleToken(baseToken, vacdmRuleToken))
				{
					appendStructuredRule("vacdm", vacdmRuleToken.token, vacdmRuleToken.expectedState, tagType, status, detail,
						vacdmRuleToken.hasTargetColor, vacdmRuleToken.targetR, vacdmRuleToken.targetG, vacdmRuleToken.targetB,
						vacdmRuleToken.hasTagColor, vacdmRuleToken.tagR, vacdmRuleToken.tagG, vacdmRuleToken.tagB,
						vacdmRuleToken.hasTextColor, vacdmRuleToken.textR, vacdmRuleToken.textG, vacdmRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				RunwayColorRuleDefinition runwayRuleToken;
				if (TryParseRunwayColorRuleToken(baseToken, runwayRuleToken))
				{
					appendStructuredRule("runway", runwayRuleToken.token, runwayRuleToken.expectedRunway, tagType, status, detail,
						runwayRuleToken.hasTargetColor, runwayRuleToken.targetR, runwayRuleToken.targetG, runwayRuleToken.targetB,
						runwayRuleToken.hasTagColor, runwayRuleToken.tagR, runwayRuleToken.tagG, runwayRuleToken.tagB,
						runwayRuleToken.hasTextColor, runwayRuleToken.textR, runwayRuleToken.textG, runwayRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				keptTokens.push_back(rawToken);
			}

			if (!removedRuleToken)
				continue;

			Value newLine(kArrayType);
			for (const std::string& token : keptTokens)
			{
				Value tokenValue;
				tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
				newLine.PushBack(tokenValue, allocator);
			}

			lineValue = newLine;
			changed = true;
		}
	};

	auto migrateTypeDefinitions = [&](const char* typeKey)
	{
		Value& labelsForMigration = profile["labels"];
		if (!labelsForMigration.HasMember(typeKey) || !labelsForMigration[typeKey].IsObject())
			return;

		Value& section = labelsForMigration[typeKey];
		if (section.HasMember("definition") && section["definition"].IsArray())
			migrateDefinitionArray(section["definition"], typeKey, "default", "normal");
		if (section.HasMember("definition_detailed") && section["definition_detailed"].IsArray())
			migrateDefinitionArray(section["definition_detailed"], typeKey, "default", "detailed");

		if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
			return;

		Value& statusDefinitions = section["status_definitions"];
		for (auto statusIt = statusDefinitions.MemberBegin(); statusIt != statusDefinitions.MemberEnd(); ++statusIt)
		{
			if (!statusIt->name.IsString() || !statusIt->value.IsObject())
				continue;
			const std::string statusKey = statusIt->name.GetString();
			Value& statusSection = statusIt->value;
			if (statusSection.HasMember("definition") && statusSection["definition"].IsArray())
				migrateDefinitionArray(statusSection["definition"], typeKey, statusKey, "normal");
			if (statusSection.HasMember("definition_detailed") && statusSection["definition_detailed"].IsArray())
				migrateDefinitionArray(statusSection["definition_detailed"], typeKey, statusKey, "detailed");
		}
	};

	migrateTypeDefinitions("departure");
	migrateTypeDefinitions("arrival");
	migrateTypeDefinitions("airborne");
	migrateTypeDefinitions("uncorrelated");

	// A validated backup or a migrated read-only source may be active in memory.
	// Normalize that working copy for runtime use, but leave recovery to the
	// explicit Settings flow instead of showing a spurious startup save error.
	if (changed && persistChanges && CurrentConfig->isConfigHealthy() && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save status settings to vSMR_Profiles.json", true, true, false, false, false);
	}
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

void CSMRRadar::OnRefresh(HDC hDC, int Phase)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnRefresh",
		reinterpret_cast<std::uintptr_t>(this));
	if (IsShutdownRequested())
		return;
	PublishCrashRadarState("main");

	VSMR_REFRESH_LOG(string(__FUNCSIG__));
	const char* refreshStage = "entry";
	auto setRefreshStage = [&](const char* stage)
	{
		refreshStage = stage;
		VsmrCrashReporter::RecordBreadcrumb("render stage", stage);
	};
	VsmrCrashReporter::RecordBreadcrumb("render stage", refreshStage);
	auto logRefreshException = [&](const std::string& reason)
	{
		static DWORD lastLogTick = 0;
		static std::string lastMessage;
		const DWORD nowTick = ::GetTickCount();
		const std::string message = "OnRefresh caught exception stage=" + std::string(refreshStage) + " reason=" + reason;
		if (message != lastMessage || nowTick - lastLogTick > 2000)
		{
			OutputVsmrDebugLine(message);
			lastMessage = message;
			lastLogTick = nowTick;
		}
	};

	try
	{
	if (Logger::is_verbose_mode())
	{
		Logger::info(
			"OnRefresh begin phase=" + std::to_string(Phase) +
			" tag_collision_count=" + std::to_string(tagCollisionAreas.size()) +
			" tag_offset_count=" + std::to_string(TagsOffsets.size()) +
				" active_airport=" + getActiveAirport());
	}

	if (CurrentConfig == nullptr || RimcasInstance == nullptr)
	{
		static bool loggedMissingCoreObjects = false;
		if (!loggedMissingCoreObjects)
		{
			Logger::info("OnRefresh: skipped frame because core objects are not initialized");
			loggedMissingCoreObjects = true;
		}
		return;
	}

	EnsureAvisoWheelHooks(this);
	// Refresh pipeline is phase-driven by EuroScope. Cursor setup stays on the UI thread.
	if (initCursor)
	{
		if (customCursor) {
			HCURSOR loadedCursor = reinterpret_cast<HCURSOR>(::LoadImage(
				AfxGetInstanceHandle(),
				MAKEINTRESOURCE(IDC_SMRCURSOR),
				IMAGE_CURSOR,
				0,
				0,
				LR_SHARED));
			if (loadedCursor != nullptr)
				smrCursor = loadedCursor;
			// EuroScope/MFC can still override the cursor occasionally; we therefore reapply it via window proc hook.

		}
		if (smrCursor == nullptr)
			smrCursor = ::LoadCursor(nullptr, IDC_ARROW);
		if (smrCursor != nullptr)
			::SetCursor(smrCursor);

		initCursor = false;
	}
	HWND insetHostWindow = ::WindowFromDC(hDC);
	if (insetHostWindow == nullptr || !::IsWindow(insetHostWindow))
		insetHostWindow = ::GetActiveWindow();
	EnsureInsetWindowProcHook(insetHostWindow, this);
	AvisoRefreshHostWindow.store(insetHostWindow, std::memory_order_release);

	if (Phase == REFRESH_PHASE_AFTER_LISTS) {
		VSMR_REFRESH_LOG("Phase == REFRESH_PHASE_AFTER_LISTS");
		VSMR_REFRESH_LOG("break Phase == REFRESH_PHASE_AFTER_LISTS");
		return;
	}

	if (Phase != REFRESH_PHASE_BEFORE_TAGS)
		return;

	VSMR_REFRESH_LOG("Phase != REFRESH_PHASE_BEFORE_TAGS");
	const unsigned long fpsNowTick = ::GetTickCount();
	if (FpsLastSampleTick == 0)
		FpsLastSampleTick = fpsNowTick;
	++FpsFrameCount;
	const unsigned long fpsElapsedMs = fpsNowTick - FpsLastSampleTick;
	if (fpsElapsedMs >= 500)
	{
		FpsDisplayValue = static_cast<int>((static_cast<double>(FpsFrameCount) * 1000.0 / static_cast<double>(fpsElapsedMs)) + 0.5);
		FpsFrameCount = 0;
		FpsLastSampleTick = fpsNowTick;
	}
	const double perfFrameStartMs = RefreshPerfNowMs();
	PerformanceDiagnostics.BeginFrame();
	std::uint32_t frameRefreshReasonMask =
		PendingPerformanceRefreshReasonMask.exchange(0, std::memory_order_acq_rel);
	double perfAvisoMs = 0.0;
	double perfTargetsMs = 0.0;
	double perfRimcasMs = 0.0;
	double perfTagsMs = 0.0;
	double perfSrwMs = 0.0;
	double perfAvisoInsetMs = 0.0;
	double perfRdfMs = 0.0;
	double perfInsetChromeMs = 0.0;

	struct Utils {
		static RECT GetAreaFromText(CDC * dc, string text, POINT Pos) {
			RECT Area = { Pos.x, Pos.y, Pos.x + dc->GetTextExtent(text.c_str()).cx, Pos.y + dc->GetTextExtent(text.c_str()).cy };
			return Area;
		}
		static string getEnumString(TagTypes type) {
			if (type == TagTypes::Departure)
				return "departure";
			if (type == TagTypes::Arrival)
				return "arrival";
			if (type == TagTypes::Uncorrelated)
				return "uncorrelated";
			return "airborne";
		}
		static vector<string> getVectorFromCommaList(const string& list) {
			vector<string> result;
			size_t start = 0;
			size_t end = list.find(',');
			while (end != string::npos) {
				result.push_back(list.substr(start, end - start));
				start = end + 1;
				end = list.find(',', start);
			}
			result.push_back(list.substr(start));
			return result;
		}
	};

	setRefreshStage("sector geometry cache");
	// Sector element enumeration is stateful in the EuroScope SDK. Always bind
	// it to this radar screen before reading airport geometry or ARR/DEP runway
	// activity; another callback may have selected a different sector source.
	CPlugIn* sectorPlugin = GetPlugIn();
	struct ActiveSectorSelectionGuard
	{
		CPlugIn* plugin = nullptr;
		~ActiveSectorSelectionGuard()
		{
			if (plugin != nullptr)
				plugin->SelectActiveSectorfile();
		}
	} sectorSelectionGuard{ sectorPlugin };
	sectorPlugin->SelectScreenSectorfile(this);
	EnsureAirportPositionCache();
	EnsureRunwayGeometryCache();
	RefreshRunwayStatuses(false);

	// Draw map elements based on zoom level
	CPosition radarDownLeft;
	CPosition radarUpRight;
	GetDisplayArea(&radarDownLeft, &radarUpRight);
	if (std::isfinite(radarDownLeft.m_Longitude) &&
		std::isfinite(radarDownLeft.m_Latitude) &&
		std::isfinite(radarUpRight.m_Longitude) &&
		std::isfinite(radarUpRight.m_Latitude))
	{
		const double minLongitude = radarDownLeft.m_Longitude;
		const double minLatitude = radarDownLeft.m_Latitude;
		const double maxLongitude = radarUpRight.m_Longitude;
		const double maxLatitude = radarUpRight.m_Latitude;
		constexpr double viewChangeEpsilon = 1.0e-8;
		if (PerformanceLastMainViewValid &&
			(std::abs(minLongitude - PerformanceLastMainViewMinLongitude) > viewChangeEpsilon ||
			 std::abs(minLatitude - PerformanceLastMainViewMinLatitude) > viewChangeEpsilon ||
			 std::abs(maxLongitude - PerformanceLastMainViewMaxLongitude) > viewChangeEpsilon ||
			 std::abs(maxLatitude - PerformanceLastMainViewMaxLatitude) > viewChangeEpsilon))
		{
			frameRefreshReasonMask |= VsmrPerformance::RefreshReasonMask(
				VsmrPerformance::FrameRefreshReason::MainViewChanged);
		}
		PerformanceLastMainViewValid = true;
		PerformanceLastMainViewMinLongitude = minLongitude;
		PerformanceLastMainViewMinLatitude = minLatitude;
		PerformanceLastMainViewMaxLongitude = maxLongitude;
		PerformanceLastMainViewMaxLatitude = maxLatitude;
	}
	double radarCrossDistance = Haversine(radarDownLeft, radarUpRight);
	int NewRadarViewZoomLevel = getZoomLevelFromCrossDistance(radarCrossDistance);
	const std::string currentMapAirport = getActiveAirport();
	const bool useAvisoGroundRenderer = !ResolveAvisoGeoJsonPathForAirport(currentMapAirport).empty();
	const auto& currentRunwayStatuses = RimcasInstance->GetRunwayStatuses();
	const bool needsMapRefresh =
		(NewRadarViewZoomLevel != RadarViewZoomLevel) ||
		(currentMapAirport != LastMapActiveAirport) ||
		(currentRunwayStatuses != LastMapRunwayStatuses);

	if (needsMapRefresh) {
		setRefreshStage("map refresh");
		RadarViewZoomLevel = NewRadarViewZoomLevel;
		if (!useAvisoGroundRenderer)
		{
			// Draw items based on asr config & zoom level
			vector<CConfig::mapData> allItems = CurrentConfig->getMapElementsForZoomLevel(maxZoomLevel);
			vector<CConfig::mapData> itemsToDraw = CurrentConfig->getMapElementsForZoomLevel(RadarViewZoomLevel);
			map<string, bool> drawItemMap;

			auto tokenDataStart = [](const string& s, const string& token) -> size_t {
				// Find token like "DEP" or "ARR" and return the index right after the token and the following separator (eg. "DEP:")
				size_t pos = s.find(token);
				if (pos == string::npos) return string::npos;
				// token length +1 for separator (':') -> matches previous code's +4 for "DEP" (3) + ':'
				return pos + token.length() + 1;
				};

			for (const auto& item : allItems) {
				// Consider element present if any map entry has the same element name (compare element only).
				bool present = std::any_of(itemsToDraw.begin(), itemsToDraw.end(),
					[&](const CConfig::mapData& m) { return m.element == item.element; });

				bool shouldDraw = present;

				// If the item has an "active" definition we need to evaluate DEP/ARR conditions
				if (present && item.active.size() > 4) {
					if (item.active.substr(0, 4) != currentMapAirport) {
						shouldDraw = false;
					}

					// airport prefix (first 4 chars) must match active airport
				
					if (shouldDraw) {
						size_t depPos = tokenDataStart(item.active, "DEP");
						size_t arrPos = tokenDataStart(item.active, "ARR");
						// If DEP present, extract substring between DEP: and ARR: (or end) and check runways
						if (depPos != string::npos) {
							size_t depEnd = (arrPos != string::npos) ? arrPos - 5 : item.active.size();
							string depList = item.active.substr(depPos, depEnd - depPos);
							vector<string> depRunways = Utils::getVectorFromCommaList(depList);
							for (const auto& rwy : depRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::DEP && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}

						// If ARR present, extract substring after ARR: and check runways
						if (arrPos != string::npos && shouldDraw) {
							string arrList = item.active.substr(arrPos);
							vector<string> arrRunways = Utils::getVectorFromCommaList(arrList);
							for (const auto& rwy : arrRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::ARR && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}
					}
				}

				// Always set an entry for this element (avoids missing keys and an empty draw map).
				drawItemMap[item.element] = shouldDraw;
			}

			// Now apply the map
			setRefreshStage("sector element visibility");
			for (const auto& [elementName, toDraw] : drawItemMap) {
				size_t slashPos = elementName.find("/");
				if (slashPos == string::npos) continue;
				string category = elementName.substr(0, slashPos);
				string name = elementName.substr(slashPos + 1);

				int elementCategory = getIntFromCategory(category);
				if (elementCategory == -1) continue;
				CSectorElement element = GetPlugIn()->SectorFileElementSelectFirst(elementCategory);
				while (element.IsValid()) {
					const char* elementNameRaw = element.GetName();
					if (elementNameRaw == nullptr || elementNameRaw[0] == '\0')
					{
						element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
						continue;
					}

					if (strncmp(name.c_str(), elementNameRaw, strlen(name.c_str())) == 0) {
						const char* componentName = element.GetComponentName(0);
						if (componentName != nullptr)
							ShowSectorFileElement(element, componentName, toDraw);
					}
					element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
				}
			}

			setRefreshStage("RefreshMapContent");
			RefreshMapContent();
		}
		LastMapRunwayStatuses = currentRunwayStatuses;
		LastMapActiveAirport = currentMapAirport;
	}


	POINT p;
	if (GetCursorPos(&p)) {
		HWND activeWindow = GetActiveWindow();
		if (activeWindow != nullptr && ScreenToClient(activeWindow, &p)) {
			mouseLocation = p;
		}
	}

	VSMR_REFRESH_LOG("Graphics set up");
	setRefreshStage("graphics setup");
	CDC dc;
	dc.Attach(hDC);
	ScopedCdcDetach dcDetach(dc);

	// Creating the gdi+ graphics
	Graphics graphics(hDC);
	graphics.SetPageUnit(Gdiplus::UnitPixel);

	graphics.SetSmoothingMode(SmoothingModeAntiAlias);

	RECT RadarArea = GetRadarArea();
	RECT ChatArea = GetChatArea();
	CRect normalizedChatArea(ChatArea);
	normalizedChatArea.NormalizeRect();
	if (!normalizedChatArea.IsRectEmpty())
		RadarArea.bottom = normalizedChatArea.top;
	CRect insetLayoutBounds(RadarArea);
	insetLayoutBounds.NormalizeRect();
	if (InitialInsetStateRestorePending && !insetLayoutBounds.IsRectEmpty())
	{
		const unsigned long restoreNowTick = ::GetTickCount();
		const bool sameBounds =
			InitialInsetStateRestoreBounds.left == insetLayoutBounds.left &&
			InitialInsetStateRestoreBounds.top == insetLayoutBounds.top &&
			InitialInsetStateRestoreBounds.right == insetLayoutBounds.right &&
			InitialInsetStateRestoreBounds.bottom == insetLayoutBounds.bottom;
		if (sameBounds)
		{
			++InitialInsetStateRestoreStableFrames;
		}
		else
		{
			InitialInsetStateRestoreBounds = insetLayoutBounds;
			InitialInsetStateRestoreStableFrames = 1;
			InitialInsetStateRestoreBoundsChangedTick = restoreNowTick;
		}

		if (InitialInsetStateRestoreStableFrames >= 2 &&
			restoreNowTick - InitialInsetStateRestoreBoundsChangedTick >= 250)
		{
			ResetAllInsetWindowStates(false);
			ResetAvisoPresetStateForActiveAirport(false);
			if (!LoadInsetStateFromAsrForAirport(getActiveAirport(), true))
				ApplyDefaultAvisoPresetIfConfigured();
			else if (!ActiveAvisoPresetName.empty())
			{
				const std::string activePresetName = ActiveAvisoPresetName;
				LoadAvisoPreset(activePresetName);
			}
			InitialInsetStateRestorePending = false;
			InitialInsetStateRestoreStableFrames = 0;
			InitialInsetStateRestoreBoundsChangedTick = 0;
		}
	}
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr)
			appWindowIt->second->ApplyAvisoLayoutBounds(&insetLayoutBounds);
	}

	auto disableAvisoGeoJsonRender = [&](const std::string& reason)
	{
		std::string disabledPath = AvisoGeoJsonLoadedPath;
		if (disabledPath.empty())
			disabledPath = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());

		AvisoGeoJsonRenderDisabled = true;
		AvisoGeoJsonRenderDisabledPath = disabledPath;
		ClearAvisoGeoJsonRasterCache();
		AvisoGeoJsonLastViewValid = false;
		Logger::info("AVISO GeoJSON render disabled path=" + disabledPath + " reason=" + reason);
	};

	// Capture all EuroScope-derived frame data once, finalize RIMCAS, and then
	// publish an immutable scene before any viewport renders traffic.  Main,
	// AVISO, and SRW projections consume this same snapshot later in the frame.
	setRefreshStage("shared radar scene build");
	double sceneRimcasMilliseconds = 0.0;
	const std::shared_ptr<const VsmrScene::RadarScene> frameSceneOwner = BuildRadarScene(
		isLVP,
		&sceneRimcasMilliseconds);
	const VsmrScene::RadarScene* frameScene = frameSceneOwner.get();
	perfRimcasMs += sceneRimcasMilliseconds;

	const double perfAvisoStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("AVISO render");
		RenderAvisoGeoJson(hDC, graphics);
	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		disableAvisoGeoJsonRender("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		disableAvisoGeoJsonRender("MFC exception");
	}
	catch (const std::exception& ex)
	{
		disableAvisoGeoJsonRender(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		disableAvisoGeoJsonRender("unknown exception");
	}
	perfAvisoMs += RefreshPerfNowMs() - perfAvisoStartMs;

	VSMR_REFRESH_LOG("Runway overlay loop");
	setRefreshStage("runway overlay draw");
	for (const auto& runway : CachedRunwayGeometries)
	{
		const auto closedRunwayIt = RimcasInstance->ClosedRunway.find(runway.displayName);
		if (closedRunwayIt == RimcasInstance->ClosedRunway.end() || !closedRunwayIt->second)
			continue;

		const std::vector<CPosition>& closedDefinition =
			runway.closedDefinition.empty() ? runway.rimcasDefinition : runway.closedDefinition;
		if (closedDefinition.size() < 3)
			continue;

		std::vector<PointF> points;
		points.reserve(closedDefinition.size());
		for (const auto& point : closedDefinition)
		{
			POINT toDraw = ConvertCoordFromPositionToPixel(point);
			points.push_back({ REAL(toDraw.x), REAL(toDraw.y) });
		}

		SolidBrush closedRunwayBrush(Color(150, 0, 0));
		graphics.FillPolygon(&closedRunwayBrush, points.data(), static_cast<INT>(points.size()));
	}

#pragma region symbols
	// Drawing the symbols
	VSMR_REFRESH_LOG("Symbols loop");
	setRefreshStage("target symbol rendering");

	// Cache current view scaling once per frame for configured icon sizing.
	double framePixPerMeter = 0.0;
	{
		RECT radarArea = GetRadarArea();
		RECT chatArea = GetChatArea();
		radarArea.bottom = chatArea.top;
		double pxW = (radarArea.right - radarArea.left > 0) ? double(radarArea.right - radarArea.left) : 1.0;
		double pxH = (radarArea.bottom - radarArea.top > 0) ? double(radarArea.bottom - radarArea.top) : 1.0;

		CPosition dispSW, dispNE;
		GetDisplayArea(&dispSW, &dispNE);
		double centerLat = (dispSW.m_Latitude + dispNE.m_Latitude) / 2.0;
		double centerLon = (dispSW.m_Longitude + dispNE.m_Longitude) / 2.0;

		CPosition leftMid; leftMid.m_Latitude = centerLat; leftMid.m_Longitude = dispSW.m_Longitude;
		CPosition rightMid; rightMid.m_Latitude = centerLat; rightMid.m_Longitude = dispNE.m_Longitude;
		CPosition bottomMid; bottomMid.m_Latitude = dispSW.m_Latitude; bottomMid.m_Longitude = centerLon;
		CPosition topMid; topMid.m_Latitude = dispNE.m_Latitude; topMid.m_Longitude = centerLon;

		double widthMeters = Haversine(leftMid, rightMid);
		double heightMeters = Haversine(bottomMid, topMid);

		double pixPerMeterX = (widthMeters > 1.0) ? (pxW / widthMeters) : 0.0;
		double pixPerMeterY = (heightMeters > 1.0) ? (pxH / heightMeters) : 0.0;

		if (pixPerMeterX > 0.0 && pixPerMeterY > 0.0)
			framePixPerMeter = (pixPerMeterX < pixPerMeterY) ? pixPerMeterX : pixPerMeterY;
		else if (pixPerMeterX > 0.0)
			framePixPerMeter = pixPerMeterX;
		else
			framePixPerMeter = pixPerMeterY;
	}

	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& frameTargetPresentation = frameScene != nullptr
		? frameScene->targetPresentation
		: defaultTargetPresentation;
	setRefreshStage("radar target loop");
	const double perfRimcasBeforeTargetsMs = perfRimcasMs;
	const double perfTargetsStartMs = RefreshPerfNowMs();
	std::size_t frameVisibleTargetCount = 0;
	targetAreas.clear();
	CRect frameVisibleRadarArea(RadarArea);
	frameVisibleRadarArea.NormalizeRect();
	static const std::vector<VsmrScene::Target> emptySceneTargets;
	const std::vector<VsmrScene::Target>& sceneTargets = frameScene != nullptr
		? frameScene->targets
		: emptySceneTargets;
	auto scenePosition = [](const VsmrScene::GeoPoint& source) -> CPosition
	{
		CPosition position;
		position.m_Latitude = source.latitude;
		position.m_Longitude = source.longitude;
		return position;
	};
	{
		VsmrTargetRendering::FrameSettings targetRenderSettings;
		targetRenderSettings.presentation = frameTargetPresentation;
		targetRenderSettings.pixelsPerMeter = framePixPerMeter;
		targetRenderSettings.projectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
		{
			return ConvertCoordFromPositionToPixel(scenePosition(point));
		};
		targetRenderSettings.pointVisible = [&](const POINT& point, int margin) -> bool
		{
			return point.x >= frameVisibleRadarArea.left - margin &&
				point.x <= frameVisibleRadarArea.right + margin &&
				point.y >= frameVisibleRadarArea.top - margin &&
				point.y <= frameVisibleRadarArea.bottom + margin;
		};
		targetRenderSettings.iconCache = CreateTargetIconCacheCallbacks();

		VsmrTargetRendering::Frame targetRenderer(graphics, std::move(targetRenderSettings));
		for (const VsmrScene::Target& sceneTarget : sceneTargets)
		{
			if (!sceneTarget.iconVisible || !sceneTarget.position.valid)
				continue;
			const std::string& rtCallsign = sceneTarget.callsign;
			auto iconVerboseStep = [&](const std::string& step)
			{
				if (!Logger::is_verbose_mode())
					return;
				Logger::info("IconRender: " + rtCallsign + " " + step);
			};

			const CPosition targetPosition = scenePosition(sceneTarget.position);
			iconVerboseStep("begin");

			const bool AcisCorrelated = sceneTarget.correlated;
			POINT acPosPix = ConvertCoordFromPositionToPixel(targetPosition);
			if (acPosPix.x >= frameVisibleRadarArea.left && acPosPix.x < frameVisibleRadarArea.right &&
				acPosPix.y >= frameVisibleRadarArea.top && acPosPix.y < frameVisibleRadarArea.bottom)
			{
				++frameVisibleTargetCount;
			}

			iconVerboseStep("after_scene_data");
			const VsmrTargetRendering::DrawResult drawResult =
				targetRenderer.DrawTarget(sceneTarget);
			acPosPix = drawResult.center;
			if (Logger::is_verbose_mode())
			{
				const char* iconDrawMode = sceneTarget.style.icon == VsmrScene::IconStyle::Nova
					? "nova"
					: (drawResult.realisticBitmapDrawn ? "realistic" : "symbol");
				Logger::info(
					"IconRender: " + rtCallsign +
					" mode=" + iconDrawMode +
					" icon_type=" + sceneTarget.style.assetKey);
			}
			iconVerboseStep("after_icon_draw");
			if (mouseWithin({ acPosPix.x - 5, acPosPix.y - 5, acPosPix.x + 5, acPosPix.y + 5 })) {
				dc.MoveTo(acPosPix.x, acPosPix.y - 8);
				dc.LineTo(acPosPix.x - 6, acPosPix.y - 12);
				dc.MoveTo(acPosPix.x, acPosPix.y - 8);
				dc.LineTo(acPosPix.x + 6, acPosPix.y - 12);

				dc.MoveTo(acPosPix.x, acPosPix.y + 8);
				dc.LineTo(acPosPix.x - 6, acPosPix.y + 12);
				dc.MoveTo(acPosPix.x, acPosPix.y + 8);
				dc.LineTo(acPosPix.x + 6, acPosPix.y + 12);

				dc.MoveTo(acPosPix.x - 8, acPosPix.y );
				dc.LineTo(acPosPix.x - 12, acPosPix.y -6);
				dc.MoveTo(acPosPix.x - 8, acPosPix.y);
				dc.LineTo(acPosPix.x - 12 , acPosPix.y + 6);

				dc.MoveTo(acPosPix.x + 8, acPosPix.y);
				dc.LineTo(acPosPix.x + 12, acPosPix.y - 6);
				dc.MoveTo(acPosPix.x + 8, acPosPix.y);
				dc.LineTo(acPosPix.x + 12, acPosPix.y + 6);
			}

			std::string hoverTextStorage;
			const char* hoverText = "";
			if (AcisCorrelated)
			{
				hoverTextStorage = sceneTarget.bottomLine;
				hoverText = hoverTextStorage.c_str();
			}
			else
			{
				hoverText = sceneTarget.systemId.c_str();
			}
			iconVerboseStep("before_add_screen_object");
			const CRect targetArea(
				drawResult.hitBounds.left,
				drawResult.hitBounds.top,
				drawResult.hitBounds.right,
				drawResult.hitBounds.bottom);
			targetAreas[rtCallsign] = targetArea;
			AddScreenObject(DRAWING_AC_SYMBOL, rtCallsign.c_str(), targetArea, false, hoverText);
			iconVerboseStep("after_add_screen_object");
		}
		perfTargetsMs += AvisoMax(0.0, (RefreshPerfNowMs() - perfTargetsStartMs) - (perfRimcasMs - perfRimcasBeforeTargetsMs));
	}

#pragma endregion Drawing of the symbols

	tagAreas.clear();
	tagCollisionAreas.clear();

	graphics.SetSmoothingMode(SmoothingModeDefault);

	const double perfTagsStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("tag rendering");
		RenderTags(graphics, dc);
	}
	catch (const std::exception& ex)
	{
		Logger::info(std::string("RenderTags: std::exception caught: ") + ex.what());
	}
	catch (...)
	{
		Logger::info("RenderTags: unknown C++ exception caught");
	}
	perfTagsMs += RefreshPerfNowMs() - perfTagsStartMs;

	// RDF is resolved and projected on the EuroScope/UI thread.  Drawing it
	// here keeps the main-view marker above vSMR targets and labels, while the
	// inset renderers below can cover it with their own independently projected
	// RDF overlays.
	graphics.Flush(Gdiplus::FlushIntentionSync);
	const CRect rdfMainArea = ResolveMainAvisoRenderArea();
	if (!rdfMainArea.IsRectEmpty())
	{
		const double perfRdfStartMs = RefreshPerfNowMs();
		VsmrRdf::Draw(
			hDC,
			this,
			rdfMainArea,
			[this](const CPosition& position) -> POINT
			{
				return ConvertCoordFromPositionToPixel(position);
			});
		perfRdfMs += RefreshPerfNowMs() - perfRdfStartMs;
	}

	// Releasing the hDC after the drawing
	graphics.ReleaseHDC(hDC);

	CBrush BrushGrey(RGB(150, 150, 150));
	COLORREF oldColor = dc.SetTextColor(RGB(33, 33, 33));

	int TextHeight = dc.GetTextExtent("60").cy;
	VSMR_REFRESH_LOG("RIMCAS Loop");
	setRefreshStage("RIMCAS list rendering");
	const double perfRimcasListStartMs = RefreshPerfNowMs();
	for (std::map<string, bool>::iterator it = RimcasInstance->MonitoredRunwayArr.begin(); it != RimcasInstance->MonitoredRunwayArr.end(); ++it)
	{
		const auto timeTableIt = RimcasInstance->TimeTable.find(it->first);
		if (!it->second || timeTableIt == RimcasInstance->TimeTable.end() || timeTableIt->second.empty())
			continue;
		const auto& runwayTimeTable = timeTableIt->second;

		vector<int> TimeDefinition = RimcasInstance->CountdownDefinition;
		if (isLVP)
			TimeDefinition = RimcasInstance->CountdownDefinitionLVP;
		Color rimcasStageOneColor(255, 160, 90, 30);
		Color rimcasStageTwoColor(255, 150, 0, 0);
		const Value& activeProfile = CurrentConfig->getActiveProfile();
		if (activeProfile.IsObject() && activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		{
			const Value& rimcasConfig = activeProfile["rimcas"];
			if (rimcasConfig.HasMember("background_color_stage_one") && rimcasConfig["background_color_stage_one"].IsObject())
				rimcasStageOneColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_one"]);
			if (rimcasConfig.HasMember("background_color_stage_two") && rimcasConfig["background_color_stage_two"].IsObject())
				rimcasStageTwoColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_two"]);
		}

		auto timePopupAreaIt = TimePopupAreas.find(it->first);
		if (timePopupAreaIt == TimePopupAreas.end())
		{
			auto emplaceResult = TimePopupAreas.emplace(it->first, RECT{ 300, 300, 430, 300 + LONG(TextHeight * (TimeDefinition.size() + 1)) });
			timePopupAreaIt = emplaceResult.first;
		}

		CRect CRectTime = timePopupAreaIt->second;
		CRectTime.NormalizeRect();

		dc.FillRect(CRectTime, &BrushGrey);

		// Drawing the runway name
		string tempS = it->first;
		dc.TextOutA(CRectTime.left + CRectTime.Width() / 2 - dc.GetTextExtent(tempS.c_str()).cx / 2, CRectTime.top, tempS.c_str());

		int TopOffset = TextHeight;
		// Drawing the times
		for (auto &Time : TimeDefinition)
		{
			dc.SetTextColor(RGB(33, 33, 33));

			const auto timeEntryIt = runwayTimeTable.find(Time);
			const std::string acCallsign = (timeEntryIt != runwayTimeTable.end()) ? timeEntryIt->second : "";
			tempS = std::to_string(Time) + ": " + acCallsign;
			const VsmrScene::Target* sceneTarget = frameScene != nullptr
				? frameScene->FindTarget(acCallsign)
				: nullptr;
			if (sceneTarget != nullptr &&
				sceneTarget->rimcas.alertStage != static_cast<int>(CRimcas::NoAlert))
			{
				const Color alertColor = sceneTarget->rimcas.alertStage == static_cast<int>(CRimcas::StageOne)
					? rimcasStageOneColor
					: rimcasStageTwoColor;
				CBrush RimcasBrush(alertColor.ToCOLORREF());

				CRect TempRect = { CRectTime.left, CRectTime.top + TopOffset, CRectTime.right, CRectTime.top + TopOffset + TextHeight };
				TempRect.NormalizeRect();

				dc.FillRect(TempRect, &RimcasBrush);
				dc.SetTextColor(RGB(238, 238, 208));
			}

			dc.TextOutA(CRectTime.left, CRectTime.top + TopOffset, tempS.c_str());

			TopOffset += TextHeight;
		}

		AddScreenObject(RIMCAS_IAW, it->first.c_str(), CRectTime, true, "");

	}
	perfRimcasMs += RefreshPerfNowMs() - perfRimcasListStartMs;

	//
	// Tag deconflicting
	//

	VSMR_REFRESH_LOG("Tag deconfliction loop");
	setRefreshStage("tag deconfliction");
	const double perfTagDeconflictStartMs = RefreshPerfNowMs();
	bool autoDeconflictionEnabled = true;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() &&
			profile.HasMember("labels") &&
			profile["labels"].IsObject() &&
			profile["labels"].HasMember("auto_deconfliction") &&
			profile["labels"]["auto_deconfliction"].IsBool())
		{
			autoDeconflictionEnabled = profile["labels"]["auto_deconfliction"].GetBool();
		}
	}

	std::vector<std::string> staleTagCallsigns;
	auto isTagCoolingDown = [&](const std::string& callsign) -> bool
	{
		auto recentMoveIt = RecentlyAutoMovedTags.find(callsign);
		if (recentMoveIt == RecentlyAutoMovedTags.end())
			return false;

		if (std::chrono::steady_clock::now() - recentMoveIt->second >=
			std::chrono::milliseconds(800))
		{
			RecentlyAutoMovedTags.erase(recentMoveIt);
			return false;
		}
		return true;
	};
	auto intersectsAnyOtherTag = [&](const std::string& callsign, const CRect& rect, bool* outRotateAntiClockwise) -> bool
	{
		for (const auto& otherArea : tagCollisionAreas)
		{
			if (callsign == otherArea.first)
				continue;
			if (IsTagBeingDragged(otherArea.first))
				continue;

			CRect intersection;
			if (!intersection.IntersectRect(rect, otherArea.second))
				continue;

			if (outRotateAntiClockwise != nullptr)
				*outRotateAntiClockwise = (rect.left <= otherArea.second.left);
			return true;
		}
		return false;
	};

	if (autoDeconflictionEnabled)
	{
		for (const auto& areaEntry : tagCollisionAreas)
		{
			const std::string& callsign = areaEntry.first;
			const CRect& currentRect = areaEntry.second;

			if (IsTagBeingDragged(callsign))
				continue;
			if (isTagCoolingDown(callsign))
				continue;

			// Decide the preferred rotation direction from current overlaps.
			bool rotateAntiClockwise = false;
			(void)intersectsAnyOtherTag(callsign, currentRect, &rotateAntiClockwise);

			const VsmrScene::Target* deconflictTarget = frameScene != nullptr
				? frameScene->FindTarget(callsign)
				: nullptr;
			if (deconflictTarget == nullptr || !deconflictTarget->position.valid)
			{
				staleTagCallsigns.push_back(callsign);
				if (Logger::is_verbose_mode())
				{
					Logger::info(
						"OnRefresh deconfliction: pruned stale tag state callsign=" + callsign +
						" target_valid=" + std::string(deconflictTarget != nullptr ? "1" : "0"));
				}
				continue;
			}

			auto angleIt = TagAngles.find(callsign);
			if (angleIt == TagAngles.end())
				angleIt = TagAngles.emplace(callsign, 270.0f).first;
			const double baseAngle = angleIt->second;

			int leaderLength = LeaderLineDefaultlenght;
			auto leaderLengthIt = TagLeaderLineLength.find(callsign);
			if (leaderLengthIt != TagLeaderLineLength.end())
				leaderLength = leaderLengthIt->second;

			CPosition deconflictPosition;
			deconflictPosition.m_Latitude = deconflictTarget->position.latitude;
			deconflictPosition.m_Longitude = deconflictTarget->position.longitude;
			const POINT acPosPix = ConvertCoordFromPositionToPixel(deconflictPosition);
			const int width = currentRect.Width();
			const int height = currentRect.Height();

			for (double rotated = 0.0; abs(rotated) <= 360.0;)
			{
				const double candidateAngle = fmod(baseAngle + rotated, 360.0f);
				POINT tagCenter;
				tagCenter.x = long(acPosPix.x + float(leaderLength * cos(DegToRad(candidateAngle))));
				tagCenter.y = long(acPosPix.y + float(leaderLength * sin(DegToRad(candidateAngle))));

				CRect newRectangle(
					tagCenter.x - (width / 2),
					tagCenter.y - (height / 2),
					tagCenter.x + (width / 2),
					tagCenter.y + (height / 2));
				newRectangle.NormalizeRect();

				const bool hasConflict = intersectsAnyOtherTag(callsign, newRectangle, nullptr);
				if (!hasConflict)
				{
					angleIt->second = fmod(baseAngle + rotated, 360.0f);

					const POINT newCenter = newRectangle.CenterPoint();
					const POINT newOffset = { newCenter.x - acPosPix.x, newCenter.y - acPosPix.y };
					TagsOffsets[callsign] = newOffset;

					tagCollisionAreas[callsign] = newRectangle;
					RecentlyAutoMovedTags[callsign] = std::chrono::steady_clock::now();
					break;
				}

				rotated += rotateAntiClockwise ? -22.5f : 22.5f;
			}
		}
	}
	if (!staleTagCallsigns.empty())
	{
		for (const std::string& callsign : staleTagCallsigns)
		{
			TagsOffsets.erase(callsign);
			TagAngles.erase(callsign);
			TagLeaderLineLength.erase(callsign);
			tagAreas.erase(callsign);
			tagCollisionAreas.erase(callsign);
			previousTagSize.erase(callsign);
			TagDragOffsetFromCenter.erase(callsign);
			RecentlyAutoMovedTags.erase(callsign);
			Patatoides.erase(callsign);
		}

		if (Logger::is_verbose_mode())
		{
			Logger::info(
				"OnRefresh deconfliction: removed stale entries count=" +
				std::to_string(staleTagCallsigns.size()));
		}
	}
	perfTagsMs += RefreshPerfNowMs() - perfTagDeconflictStartMs;

	//
	// App windows
	//

	VSMR_REFRESH_LOG("App window rendering");
	setRefreshStage("app window rendering");

	SyncLinkedAvisoSecondaryToMainView();

	CInsetWindow* activeInsetWindow = nullptr;
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
			(appWindowIt->second->IsWindowMoveActive() || appWindowIt->second->IsWindowResizeActive()))
		{
			activeInsetWindow = appWindowIt->second.get();
			break;
		}
	}
	CInsetWindow* foregroundInsetWindow = activeInsetWindow;
	if (foregroundInsetWindow == nullptr)
	{
		for (const auto& display : appWindowDisplays)
		{
			if (!display.second)
				continue;
			auto appWindowIt = appWindows.find(display.first);
			if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
				appWindowIt->second->m_AvisoScrollSelected)
			{
				foregroundInsetWindow = appWindowIt->second.get();
				break;
			}
		}
	}
	auto renderInsetWindow = [&](CInsetWindow* appWindow)
	{
		if (appWindow == nullptr)
			return;
		const double insetStartMs = RefreshPerfNowMs();
		appWindow->render(hDC, this, &graphics, mouseLocation);
		const double insetElapsedMs = RefreshPerfNowMs() - insetStartMs;
		if (appWindow->IsSecondaryRadar())
			perfSrwMs += insetElapsedMs;
		else if (appWindow->IsAvisoViewport())
			perfAvisoInsetMs += insetElapsedMs;
		perfRdfMs += appWindow->GetLastRdfRenderMilliseconds();
		perfInsetChromeMs += appWindow->GetLastChromeRenderMilliseconds();
	};
	for (std::map<int, bool>::iterator it = appWindowDisplays.begin(); it != appWindowDisplays.end(); ++it)
	{
		if (!it->second)
			continue;

		int appWindowId = it->first;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			continue;

		CInsetWindow* appWindow = appWindowIt->second.get();
		if (appWindow != foregroundInsetWindow)
			renderInsetWindow(appWindow);
	}
	if (activeInsetWindow != nullptr)
		activeInsetWindow->RenderSnapPreview(graphics);
	if (foregroundInsetWindow != nullptr)
		renderInsetWindow(foregroundInsetWindow);
	setRefreshStage("fps overlay");
	if (ShowFps)
	{
		const std::string fpsText = "FPS " + std::to_string(FpsDisplayValue);
		CFont* previousFont = RuntimeOverlayFont.GetSafeHandle() != nullptr
			? dc.SelectObject(&RuntimeOverlayFont)
			: nullptr;
		const CSize fpsSize = dc.GetTextExtent(fpsText.c_str());
		CRect fpsArea = ResolveMainAvisoRenderArea();
		if (fpsArea.IsRectEmpty())
			fpsArea = CRect(RadarArea);
		fpsArea.NormalizeRect();
		auto overlapsVisibleInset = [&](const CRect& candidate, int& nextY) -> bool
		{
			bool overlaps = false;
			for (const auto& display : appWindowDisplays)
			{
				if (!display.second)
					continue;
				auto windowIt = appWindows.find(display.first);
				if (windowIt == appWindows.end() || windowIt->second == nullptr)
					continue;
				CRect overlap;
				const CRect frame = windowIt->second->GetWindowFrameRect();
				if (overlap.IntersectRect(candidate, frame) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, frame.bottom + 4);
				}
				CRect preview;
				if (windowIt->second->GetSnapPreviewRect(preview) &&
					overlap.IntersectRect(candidate, preview) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, preview.bottom + 4);
				}
			}
			return overlaps;
		};
		auto findFpsPosition = [&](bool alignRight, POINT& position) -> bool
		{
			int y = fpsArea.top + 4;
			for (size_t attempt = 0; attempt <= appWindows.size(); ++attempt)
			{
				const int x = alignRight
					? max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6)
					: fpsArea.left + 6;
				CRect candidate(x, y, x + fpsSize.cx, y + fpsSize.cy);
				if (candidate.right > fpsArea.right || candidate.bottom > fpsArea.bottom)
					return false;
				int nextY = y;
				if (!overlapsVisibleInset(candidate, nextY))
				{
					position = { x, y };
					return true;
				}
				if (nextY <= y)
					return false;
				y = nextY;
			}
			return false;
		};
		POINT fpsPosition = {
			max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6),
			fpsArea.top + 4
		};
		bool fpsPositionFound = findFpsPosition(true, fpsPosition);
		if (!fpsPositionFound)
			fpsPositionFound = findFpsPosition(false, fpsPosition);
		const int previousBackgroundMode = dc.SetBkMode(TRANSPARENT);
		const COLORREF previousTextColor = dc.SetTextColor(RGB(235, 245, 247));
		if (fpsPositionFound)
			dc.TextOutA(fpsPosition.x, fpsPosition.y, fpsText.c_str());
		dc.SetTextColor(previousTextColor);
		dc.SetBkMode(previousBackgroundMode);
		if (previousFont != nullptr)
			dc.SelectObject(previousFont);
	}

	setRefreshStage("runtime menu");
	RenderRuntimeMenu(hDC, graphics);

	PerfLastFrameMs = RefreshPerfNowMs() - perfFrameStartMs;
	PerfLastAvisoMs = perfAvisoMs;
	PerfLastTargetsMs = perfTargetsMs;
	PerfLastRimcasMs = perfRimcasMs;
	PerfLastTagsMs = perfTagsMs;
	PerfLastSrwMs = perfSrwMs;
	PerfLastAvisoInsetMs = perfAvisoInsetMs;
	PerfLastRdfMs = perfRdfMs;
	PerfLastInsetChromeMs = perfInsetChromeMs;
	VsmrPerformance::FrameSample performanceFrame;
	performanceFrame.frameId = frameScene != nullptr ? frameScene->frameId : RadarSceneFrameId;
	performanceFrame.timestampMilliseconds =
		VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
	performanceFrame.frameMilliseconds = PerfLastFrameMs;
	performanceFrame.sceneMilliseconds = PerfLastSceneBuildMs;
	performanceFrame.avisoMilliseconds = PerfLastAvisoMs;
	performanceFrame.targetsMilliseconds = PerfLastTargetsMs;
	performanceFrame.rimcasMilliseconds = PerfLastRimcasMs;
	performanceFrame.tagsMilliseconds = PerfLastTagsMs;
	performanceFrame.srwMilliseconds = PerfLastSrwMs;
	performanceFrame.avisoInsetMilliseconds = PerfLastAvisoInsetMs;
	performanceFrame.rdfMilliseconds = PerfLastRdfMs;
	performanceFrame.insetChromeMilliseconds = PerfLastInsetChromeMs;
	performanceFrame.visibleTargets = frameVisibleTargetCount;
	performanceFrame.visibleTags = tagAreas.size();
	if (frameScene != nullptr)
	{
		performanceFrame.sceneAvisoLoadMilliseconds = frameScene->stats.avisoLoadMilliseconds;
		performanceFrame.sceneControllerCaptureMilliseconds =
			frameScene->stats.controllerCaptureMilliseconds;
		performanceFrame.sceneTargetCaptureMilliseconds = frameScene->stats.targetCaptureMilliseconds;
		performanceFrame.sceneFinalizeMilliseconds = frameScene->stats.finalizeMilliseconds;
		performanceFrame.euroScopeLookupMilliseconds = frameScene->stats.sdkLookupMilliseconds;
		frameRefreshReasonMask |= frameScene->stats.refreshReasonMask;
		performanceFrame.processedTargets = frameScene->stats.sdkTargetEnumerations;
		performanceFrame.capturedTargets = frameScene->stats.targetCount;
		performanceFrame.radarFilteredTargets = frameScene->stats.radarFilteredTargetCount;
		performanceFrame.iconTargets = frameScene->stats.iconTargetCount;
		performanceFrame.tagTargets = frameScene->stats.tagTargetCount;
	}
	performanceFrame.refreshReasonMask = frameRefreshReasonMask;
	PerformanceDiagnostics.RecordFrame(performanceFrame);
	SamplePerformanceResourcesIfDue();
	const unsigned long perfNowTick = ::GetTickCount();
	if (PerfLastLogTick == 0 || perfNowTick - PerfLastLogTick >= 2000)
	{
		PerfLastLogTick = perfNowTick;
		Logger::info(
			"FramePerf ms frame=" + std::to_string(static_cast<int>(PerfLastFrameMs + 0.5)) +
			" scene=" + std::to_string(static_cast<int>(PerfLastSceneBuildMs + 0.5)) +
			" aviso=" + std::to_string(static_cast<int>(PerfLastAvisoMs + 0.5)) +
			" targets=" + std::to_string(static_cast<int>(PerfLastTargetsMs + 0.5)) +
			" rimcas=" + std::to_string(static_cast<int>(PerfLastRimcasMs + 0.5)) +
			" tags=" + std::to_string(static_cast<int>(PerfLastTagsMs + 0.5)) +
			" srw=" + std::to_string(static_cast<int>(PerfLastSrwMs + 0.5)) +
			" aviso_inset=" + std::to_string(static_cast<int>(PerfLastAvisoInsetMs + 0.5)) +
			" rdf=" + std::to_string(static_cast<int>(PerfLastRdfMs + 0.5)) +
			" inset_chrome=" + std::to_string(static_cast<int>(PerfLastInsetChromeMs + 0.5)) +
			(frameScene != nullptr
				? " scene_targets=" + std::to_string(frameScene->stats.targetCount) +
				  " scene_tags=" + std::to_string(frameScene->stats.tagElementCount) +
				  " scene_controllers=" + std::to_string(frameScene->stats.controllerCount) +
				  " scene_bytes_lower=" + std::to_string(frameScene->stats.lowerBoundOwnedBytes) +
				  " scene_es_targets=" + std::to_string(frameScene->stats.sdkTargetEnumerations) +
				  " scene_es_fp=" + std::to_string(frameScene->stats.sdkFlightPlanLookups) +
				  " scene_es_correlated_fp=" + std::to_string(frameScene->stats.sdkCorrelatedFlightPlanLookups) +
				  " scene_vacdm=" + std::to_string(frameScene->stats.vacdmLookups)
				: " scene_unavailable=1"));
	}

	dcDetach.Detach();
	setRefreshStage("complete");

	VSMR_REFRESH_LOG("END "+ string(__FUNCSIG__));

	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		logRefreshException("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		logRefreshException("MFC exception");
	}
	catch (const std::exception& ex)
	{
		logRefreshException(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		logRefreshException("unknown exception");
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
