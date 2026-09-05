#pragma once
#include <EuroScopePlugIn.h>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <GdiPlus.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "radar/RadarScreenTypes.hpp"
#include "radar/RadarUiSupport.hpp"
#include "aircraft/CallsignLookup.hpp"
#include "config/RuntimeConfig.hpp"
#include "diagnostics/PerformanceDiagnostics.hpp"
#include "safety/Rimcas.hpp"
#include "radar/RadarGeometry.hpp"
#include "scene/RadarScene.hpp"
#include <memory>
#include <mutex>
#include <atomic>
#include "shared/logging/Logger.hpp"
#include "tags/TagDataTypes.hpp"
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>

using namespace std;
using namespace Gdiplus;
using namespace EuroScopePlugIn;
namespace fs = std::filesystem;

class CVsmrControlCenterDialog;
class CInsetWindow;
class VsmrControlCenterBridge;
class VsmrControlCenterBridgeImpl;
struct AvisoViewportState;
struct VsmrRadarInteractionAccess;
struct VsmrRadarPresetAccess;
namespace VsmrTargetRendering
{
	struct IconCacheCallbacks;
}
namespace VsmrAviso
{
	class AvisoRasterPipeline;
	class AvisoRasterBlitter;
}

class CSMRRadar :
	public EuroScopePlugIn::CRadarScreen
{
public:
	using POINT2 = VsmrRadarTypes::Point2;
	using Patatoide_Points = VsmrRadarTypes::PatatoidePoints;
	using AircraftSpec = VsmrRadarTypes::AircraftSpec;
	using RealisticIconCacheEntry = VsmrRadarTypes::RealisticIconCacheEntry;
	using AvisoPoint = VsmrRadarTypes::AvisoPoint;
	using AvisoGroup = VsmrRadarTypes::AvisoGroup;
	using AvisoFeature = VsmrRadarTypes::AvisoFeature;
	using AvisoLabel = VsmrRadarTypes::AvisoLabel;
	using AvisoMainViewPreset = VsmrRadarTypes::AvisoMainViewPreset;
	using AvisoPreset = VsmrRadarTypes::AvisoPreset;
	using AvisoRasterRenderRequest = VsmrRadarTypes::AvisoRasterRenderRequest;
	using AvisoRasterRenderResult = VsmrRadarTypes::AvisoRasterRenderResult;
	using AvisoLoadPerformance = VsmrRadarTypes::AvisoLoadPerformance;
	using RuntimeMenuPopup = VsmrRadarTypes::RuntimeMenuPopup;
	using CachedRunwayGeometry = VsmrRadarTypes::CachedRunwayGeometry;
	using CorrelationSettings = VsmrRadarTypes::CorrelationSettings;
	using DisplayModeStatusVisibility = VsmrRadarTypes::DisplayModeStatusVisibility;
	using DisplayModeSettings = VsmrRadarTypes::DisplayModeSettings;

	CSMRRadar();
	virtual ~CSMRRadar();
	static bool CanUnloadRuntimeCallbacks() noexcept;
	POINT ConvertCoordFromPositionToPixel(CPosition position);
	CPosition ConvertCoordFromPixelToPosition(POINT point);
	VsmrPerformance::Snapshot GetPerformanceSnapshot(
		std::uint32_t windowSeconds = 60,
		std::size_t maximumSeriesPoints = 240);
	void ResetPerformanceDiagnostics();
	std::string BuildPerformanceReportJson(
		std::uint32_t windowSeconds = 0,
		std::size_t maximumSeriesPoints = VsmrPerformance::MaximumFrameSamples);
	void MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason reason) noexcept;
	void SamplePerformanceResourcesIfDue(bool force = false);
	void PublishCrashRadarState(
		const char* radar = "main",
		const char* inset = nullptr) const noexcept;

	bool ReloadConfig();
	bool SetProfilesConfigPath(
		const std::string& path,
		std::string* errorText = nullptr,
		bool persistToAsr = true);
	void RefreshAfterAirportRunwayActivityChange(bool activeAirportChanged);
	bool IsAppWindowDisplayed(int appWindowId) const;
	bool UpdateTimerInsetCountdowns();

private:
	// These collaborators implement tightly coupled rendering or same-screen
	// coordination. Named friends keep mutable radar state private from every
	// other consumer while avoiding public container accessors.
	friend class CInsetWindow;
	friend class VsmrControlCenterBridge;
	friend class VsmrControlCenterBridgeImpl;
	friend struct AvisoViewportState;
	friend struct VsmrRadarInteractionAccess;
	friend struct VsmrRadarPresetAccess;
	friend void ClearAvisoWheelRoutingState(bool cancelWindowInteractions);

	static map<string, string> vStripsStands;

	map<string, POINT> TagsOffsets;

	map<string, Patatoide_Points> Patatoides;

	int RadarViewZoomLevel = 0;
	std::map<std::string, CRimcas::RunwayStatus> LastMapRunwayStatuses;
	std::string LastMapActiveAirport;

	string DllPath;
	string DataPath;
	string ConfigPath;
	string mapsPath;
	std::unique_ptr<CCallsignLookup> Callsigns;
	std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> AircraftIcons;
	std::string IconsPath;
	std::map<std::string, AircraftSpec> AircraftSpecs;
	std::map<std::string, RealisticIconCacheEntry> RealisticIconBitmapCache;
	unsigned long long RealisticIconCacheFrame = 0;
	mutable bool StructuredTagRulesCacheValid = false;
	mutable std::vector<StructuredTagColorRule> StructuredTagRulesCache;
	std::vector<AvisoFeature> AvisoGeoJsonFeatures;
	std::vector<AvisoLabel> AvisoGeoJsonLabels;
	std::shared_ptr<const std::vector<AvisoFeature>> AvisoGeoJsonFeatureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> AvisoGeoJsonLabelSnapshot;
	size_t AvisoGeoJsonSourceFeatureCount = 0;
	std::vector<AvisoGroup> AvisoRuntimeGroups;
	std::shared_ptr<const std::unordered_map<std::string, bool>> AvisoGroupVisibilitySnapshot;
	mutable std::mutex AvisoGroupMutex;
	std::atomic<unsigned long long> AvisoGroupGeneration{ 0 };
	mutable std::string AvisoGeoJsonResolvedAirport;
	mutable std::string AvisoGeoJsonResolvedDllPath;
	mutable std::string AvisoGeoJsonResolvedPath;
	std::unordered_map<std::string, std::string> AvisoGeoJsonOverridePaths;
	// A newer vSMR may own these airport-scoped ASR records.  Keep them
	// untouched instead of silently replacing them with this build's schema.
	std::unordered_set<std::string> UnsupportedInsetAsrStateAirports;
	std::string AvisoGeoJsonLoadedPath;
	std::string AvisoGeoJsonViewInitializedPath;
	fs::file_time_type AvisoGeoJsonLoadedWriteTime;
	unsigned long AvisoGeoJsonLastStatTick = 0;
	std::string AvisoGeoJsonLastFailedPath;
	fs::file_time_type AvisoGeoJsonLastFailedWriteTime;
	unsigned long AvisoGeoJsonLastFailedTick = 0;
	bool AvisoGeoJsonLastFailedWriteTimeValid = false;
	HBITMAP AvisoGeoJsonRasterCache = nullptr;
	std::unique_ptr<VsmrAviso::AvisoRasterBlitter> AvisoRasterBlitterInstance;
	std::string AvisoGeoJsonRasterCachePath;
	unsigned long long AvisoGeoJsonRasterGroupGeneration = 0;
	std::string AvisoGeoJsonRasterColorPalette = "dark";
	double AvisoGeoJsonRasterMinLongitude = 0.0;
	double AvisoGeoJsonRasterMinLatitude = 0.0;
	double AvisoGeoJsonRasterMaxLongitude = 0.0;
	double AvisoGeoJsonRasterMaxLatitude = 0.0;
	int AvisoGeoJsonRasterWidth = 0;
	int AvisoGeoJsonRasterHeight = 0;
	double AvisoGeoJsonRasterAnchorLongitude = 0.0;
	double AvisoGeoJsonRasterAnchorLatitude = 0.0;
	double AvisoGeoJsonRasterBottomRightLongitude = 0.0;
	double AvisoGeoJsonRasterBottomRightLatitude = 0.0;
	Gdiplus::PointF AvisoGeoJsonRasterProjectedTopLeft;
	Gdiplus::PointF AvisoGeoJsonRasterProjectedTopRight;
	Gdiplus::PointF AvisoGeoJsonRasterProjectedBottomLeft;
	Gdiplus::PointF AvisoGeoJsonRasterProjectedBottomRight;
	bool AvisoGeoJsonRasterAnchorValid = false;
	bool AvisoGeoJsonLastViewValid = false;
	std::string AvisoGeoJsonLastViewPath;
	double AvisoGeoJsonLastViewMinLongitude = 0.0;
	double AvisoGeoJsonLastViewMinLatitude = 0.0;
	double AvisoGeoJsonLastViewMaxLongitude = 0.0;
	double AvisoGeoJsonLastViewMaxLatitude = 0.0;
	unsigned long AvisoGeoJsonLastViewChangeTick = 0;
	bool AvisoGeoJsonLoadAttempted = false;
	bool AvisoGeoJsonLoaded = false;
	bool AvisoGeoJsonRenderDisabled = false;
	std::string AvisoGeoJsonRenderDisabledPath;
	bool AvisoGeoJsonHasBounds = false;
	double AvisoGeoJsonMinLongitude = 0.0;
	double AvisoGeoJsonMinLatitude = 0.0;
	double AvisoGeoJsonMaxLongitude = 0.0;
	double AvisoGeoJsonMaxLatitude = 0.0;
	std::unique_ptr<VsmrAviso::AvisoRasterPipeline> AvisoGeoJsonRenderPipeline;
	std::atomic<HWND> AvisoRefreshHostWindow{ nullptr };
	std::atomic<bool> ShutdownRequested{ false };
	std::atomic<bool> AvisoGeoJsonRenderStop{ false };
	bool AvisoGeoJsonScrollSelected = false;
	bool AvisoViewsLinked = false;
	std::string ActiveAvisoPresetName;
	double PerfLastFrameMs = 0.0;
	double PerfLastAvisoMs = 0.0;
	double PerfLastTargetsMs = 0.0;
	double PerfLastRimcasMs = 0.0;
	double PerfLastTagsMs = 0.0;
	double PerfLastSrwMs = 0.0;
	double PerfLastAvisoInsetMs = 0.0;
	double PerfLastRdfMs = 0.0;
	double PerfLastInsetChromeMs = 0.0;
	AvisoLoadPerformance LastAvisoLoadPerformance;
	unsigned long PerfLastLogTick = 0;
	std::shared_ptr<const VsmrScene::RadarScene> CurrentRadarScene;
	std::array<std::shared_ptr<VsmrScene::RadarScene>, 2> RadarSceneBuffers;
	std::size_t RadarSceneBuildBufferIndex = 0;
	std::uint64_t RadarSceneFrameId = 0;
	double PerfLastSceneBuildMs = 0.0;
	VsmrPerformance::PerformanceDiagnostics PerformanceDiagnostics;
	std::uint64_t PerformanceLastResourceSampleMilliseconds = 0;
	std::atomic<std::uint32_t> PendingPerformanceRefreshReasonMask{
		VsmrPerformance::RefreshReasonMask(VsmrPerformance::FrameRefreshReason::Initial) };
	bool PerformanceLastMainViewValid = false;
	double PerformanceLastMainViewMinLongitude = 0.0;
	double PerformanceLastMainViewMinLatitude = 0.0;
	double PerformanceLastMainViewMaxLongitude = 0.0;
	double PerformanceLastMainViewMaxLatitude = 0.0;

	map<int, bool> appWindowDisplays;

	map<string, CRect> tagAreas;
	map<string, CRect> tagCollisionAreas;
	map<string, CRect> targetAreas;
	map<string, double> TagAngles;
	map<string, int> TagLeaderLineLength;
	map<string, CRect> previousTagSize;
	map<std::string, POINT> TagDragOffsetFromCenter;

	std::unique_ptr<CVsmrControlCenterDialog> VsmrControlCenterDialog;
	std::string TagDefinitionEditorType = "departure";
	bool TagDefinitionEditorDetailed = false;
	std::string TagDefinitionEditorDepartureStatus = "default";
	int TagDefinitionEditorSelectedLine = 0;
	static const int TagDefinitionEditorMaxLines = 4;

	bool isLVP = false;
	bool RimcasRunwaysExplicitlyConfigured = false;

	map<string, RECT> TimePopupAreas;

	map<string, RECT> MenuPositions;
	RuntimeMenuPopup ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	std::string PendingGroundStatusCallsign;
	POINT RuntimeMenuPosition = { 14, 100 };
	bool RuntimeMenuPositionInitialized = false;
	bool RuntimeMenuMinimized = false;
	bool InitialInsetStateRestorePending = false;
	CRect InitialInsetStateRestoreBounds = { 0, 0, 0, 0 };
	int InitialInsetStateRestoreStableFrames = 0;
	unsigned long InitialInsetStateRestoreBoundsChangedTick = 0;
	CRect RuntimeMenuArea = { 0, 0, 0, 0 };
	CRect RuntimeMenuPopupArea = { 0, 0, 0, 0 };
	int RuntimeMenuPopupScrollOffset = 0;
	unsigned long FpsLastSampleTick = 0;
	int FpsFrameCount = 0;
	int FpsDisplayValue = 0;
	bool ShowFps = true;
	bool UiUseDayColorTheme = false;
	std::string AvisoColorPalette = "dark";
	COLORREF AvisoDarkBackgroundColor = RGB(67, 74, 79);
	COLORREF AvisoLightBackgroundColor = RGB(67, 74, 79);
	COLORREF AvisoRealBackgroundColor = RGB(67, 74, 79);
	CFont RuntimeOverlayFont;
	CFont RuntimeMenuActionFont;
	bool AirportPositionsCacheValid = false;
	std::vector<CachedRunwayGeometry> CachedRunwayGeometries;
	bool CachedRunwayGeometryValid = false;
	std::string CachedRunwayAirport;
	std::string CachedRunwayProfile;
	bool CachedRunwayIsLvp = false;
	unsigned long RunwayStatusLastRefreshTick = 0;
	std::string RunwayStatusLastAirport;

	map<string, std::chrono::steady_clock::time_point> RecentlyAutoMovedTags;

	std::unique_ptr<CRimcas> RimcasInstance;
	std::unique_ptr<CConfig> CurrentConfig;

	std::map<int, std::unique_ptr<Gdiplus::Font>> customFonts;
	std::map<int, std::unique_ptr<CInsetWindow>> appWindows;
	ULONG_PTR m_gdiplusToken = 0;
	int currentFontSize = 1;

	map<string, CPosition> AirportPositions;

	//----
	// Tag types
	//---

	enum TagTypes { Departure, Arrival, Airborne, Uncorrelated };


	string ActiveAirport = "EGKK";
	char CrashActiveProfile[96] = "unavailable";
	mutable char CrashLastAirport[16]{};
	mutable char CrashLastProfile[96]{};
	mutable char CrashLastRadar[96]{};
	mutable char CrashLastInset[96]{};

	void InvalidateAirportPositionCache();
	void InvalidateRunwayGeometryCache();
	void EnsureAirportPositionCache();
	void EnsureRunwayGeometryCache();
	void RefreshRunwayStatuses(bool force);
	void RefreshLegacyRimcasRunwayMonitoring();
	VsmrTargetRendering::IconCacheCallbacks CreateTargetIconCacheCallbacks();
	struct RefreshPerformance;
	using RefreshStageCallback = std::function<void(const char*)>;
	bool PrepareRefreshPhase(HDC hDC, int phase);
	void RefreshSectorMap(RefreshPerformance& performance, const RefreshStageCallback& setRefreshStage);
	void PrepareRefreshInsetLayout(CRect& insetLayoutBounds);
	std::shared_ptr<const VsmrScene::RadarScene> BuildRefreshSceneAndRenderAviso(
		HDC hDC, Gdiplus::Graphics& graphics, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void RenderClosedRunwayOverlays(
		Gdiplus::Graphics& graphics, const RefreshStageCallback& setRefreshStage);
	void RenderRefreshTargets(
		Gdiplus::Graphics& graphics, CDC& dc, const RECT& radarArea,
		const VsmrScene::RadarScene* frameScene, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void RenderRefreshRimcasPanels(
		CDC& dc, const VsmrScene::RadarScene* frameScene, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void DeconflictRefreshTags(
		const VsmrScene::RadarScene* frameScene, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void RenderRefreshInsets(
		HDC hDC, Gdiplus::Graphics& graphics, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void RenderRefreshFpsOverlay(
		CDC& dc, const RECT& radarArea, const RefreshStageCallback& setRefreshStage);
	void RenderRefreshTagsAndRdf(
		HDC hDC, Gdiplus::Graphics& graphics, CDC& dc, RefreshPerformance& performance,
		const RefreshStageCallback& setRefreshStage);
	void RecordRefreshPerformance(
		const VsmrScene::RadarScene* frameScene, RefreshPerformance& performance);


public:
	inline string getActiveAirport() const {
		return ActiveAirport;
	}
	const std::string& GetDllPath() const noexcept { return DllPath; }
	const std::string& GetDataPath() const noexcept { return DataPath; }
	const std::string& GetIconsPath() const noexcept { return IconsPath; }
	std::string LookupCallsignName(const std::string& code) const
	{
		return Callsigns != nullptr ? Callsigns->getCallsign(code) : std::string();
	}

	string setActiveAirport(
		string value,
		bool switchInsetContext = true,
		bool syncControlCenter = true);

	inline bool TryGetActiveAirportPosition(CPosition& outPosition) const
	{
		auto airportIt = AirportPositions.find(ActiveAirport);
		if (airportIt == AirportPositions.end())
			return false;
		outPosition = airportIt->second;
		return true;
	}

	//---GenerateTagData--------------------------------------------

	static map<string, string> GenerateTagData(CRadarTarget Rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, string ActiveAirport, const std::string& stableCallsign = "", const VacdmPilotData* capturedVacdmData = nullptr, const int* capturedPreviousFlightLevel = nullptr);
	using TagReplacingMap = std::map<std::string, std::string>;

	//---IsCorrelatedFuncs---------------------------------------------

	CorrelationSettings BuildCorrelationSettings() const;
	bool IsCorrelatedWithSettings(CFlightPlan fp, CRadarTarget rt, const CorrelationSettings& settings) const;
	virtual bool IsCorrelated(CFlightPlan fp, CRadarTarget rt);
	DisplayModeSettings GetActiveDisplayModeSettings() const;
	bool IsWithinAirborneDisplayLimits(int reportedGs, int pressureAltitudeFt, const DisplayModeSettings& settings) const;
	bool ShouldDisplayTargetForDisplayMode(CFlightPlan fp, bool acIsCorrelated, int reportedGs, int pressureAltitudeFt, bool targetOnRunway, const DisplayModeSettings& settings, const VacdmPilotData* capturedVacdmData) const;

	//---LoadCustomFont--------------------------------------------

	virtual void LoadCustomFont();

	//---LoadProfile--------------------------------------------

	virtual void LoadProfile(
		string profileName,
		bool saveOutgoingState = true,
		bool persistNormalization = true);
	void EnsureTargetGroundStatusColorEntries(bool persistChanges = true);
	bool EnsureVsmrControlCenterWindowCreated();
	void OpenVsmrControlCenterWindow();
	void OpenVsmrControlCenterWindow(const std::string& pageName);
	void CloseVsmrControlCenterWindow();
	void DestroyVsmrControlCenterWindow();
	void OnVsmrControlCenterWindowClosed();
	std::vector<std::string> GetOrderedProfileNamesForUi() const;
	std::string GetActiveProfileNameForEditor() const;
	bool SetActiveProfileForEditor(const std::string& name, bool persistToDisk);
	std::string ReadLastActiveProfileFromConfig() const;
	void WriteLastActiveProfileToConfig(const std::string& profileName) const;
	static void RememberSessionActiveProfile(const std::string& profileName);
	static std::string GetSessionActiveProfile(const std::string& fallbackProfile);
	std::vector<DisplayModeSettings> GetProfileDisplayModesForEditor(const std::string& profileName) const;
	std::string GetActiveProfileDisplayModeForEditor(const std::string& profileName) const;
	bool SetProfileDisplayModeActiveForEditor(const std::string& profileName, const std::string& modeName);
	std::vector<std::string> GetTagDefinitionTokens() const;
	std::string NormalizeTagDefinitionType(const std::string& type) const;
	std::string TagDefinitionTypeLabel(const std::string& type) const;
	std::string NormalizeTagDefinitionDepartureStatus(const std::string& status) const;
	std::string TagDefinitionDepartureStatusLabel(const std::string& status) const;
	std::vector<std::string> GetTagDefinitionStatusesForType(const std::string& type) const;
	bool IsTagDefinitionStatusAllowedForType(const std::string& type, const std::string& status) const;
	bool GetTagRoundedCornersEnabledForEditor() const;
	bool GetTagDefinitionDetailedSameAsDefinition() const;
	bool SetTagDefinitionDetailedSameAsDefinition(bool sameAsDefinition, bool persistToDisk);
	bool GetTagDefinitionDetailedSameAsDefinition(const std::string& type, const std::string& status) const;
	bool SetTagDefinitionDetailedSameAsDefinition(const std::string& type, const std::string& status, bool sameAsDefinition, bool persistToDisk);
	void GetTagDefinitionEditorContext(std::string& type, bool& detailed, std::string& status) const;
	void SetTagDefinitionEditorContext(const std::string& type, bool detailed, const std::string& status);
	std::string GetTagEditorTargetColorPath() const;
	std::string GetTagEditorLabelColorPath() const;
	bool GetTagDefinitionArray(std::string type, bool detailed, rapidjson::Value*& outArray, bool createIfMissing, const std::string& departureStatus = "default");
	std::vector<std::string> GetTagDefinitionLineStrings(std::string type, bool detailed, int maxLines, bool createIfMissing, const std::string& departureStatus = "default");
	void SetTagDefinitionLineString(std::string type, bool detailed, int lineIndex, const std::string& lineText, const std::string& departureStatus = "default");
	void InsertTagDefinitionTokenIntoLine(const std::string& token, bool makeBold = false);
	std::map<std::string, std::string> BuildTagDefinitionPreviewMap(const std::string& type);
	std::vector<std::string> BuildTagDefinitionPreviewLines();
	std::vector<std::string> BuildTagDefinitionPreviewLinesForContext(const std::string& type, bool detailed, const std::string& departureStatus);
	void SaveTagDefinitionConfig();
	std::string NormalizeTargetIconStyle(const std::string& style) const;
	std::string GetActiveTargetIconStyle() const;
	bool SetActiveTargetIconStyle(const std::string& style, bool persistToDisk);
	std::string NormalizeSmallTargetIconBoostResolutionPreset(const std::string& preset) const;
	std::string GetSmallTargetIconBoostResolutionPreset() const;
	bool SetSmallTargetIconBoostResolutionPreset(const std::string& preset, bool persistToDisk);
	double GetSmallTargetIconBoostResolutionScale() const;
	std::vector<std::string> GetAvailableTagFonts() const;
	int GetActiveLabelFontSize() const;
	bool SetActiveLabelFontSize(int size, bool persistToDisk);
	std::string GetActiveTagFontName() const;
	bool SetActiveTagFontName(const std::string& fontName, bool persistToDisk);
	std::string NormalizeStructuredRuleSource(const std::string& source) const;
	std::string NormalizeStructuredRuleToken(const std::string& source, const std::string& token) const;
	std::string NormalizeStructuredRuleCondition(const std::string& source, const std::string& condition) const;
	std::string NormalizeStructuredRuleTagType(const std::string& tagType) const;
	std::string NormalizeStructuredRuleStatus(const std::string& status) const;
	std::string NormalizeStructuredRuleDetail(const std::string& detail) const;
	const std::vector<StructuredTagColorRule>& GetStructuredTagColorRules() const;
	bool SetStructuredTagColorRules(const std::vector<StructuredTagColorRule>& rules, bool persistToDisk);
	Gdiplus::Bitmap* GetAircraftIcon(const std::string& acType);
	void TrimRealisticIconBitmapCache(const std::string& protectedCacheKey, unsigned long long cacheFrame);
	Gdiplus::Bitmap* GetCachedRealisticIconBitmap(
		const std::string& iconType,
		Gdiplus::Bitmap* sourceBitmap,
		UINT sourceWidth,
		UINT sourceHeight,
		bool applyTint,
		const Gdiplus::Color& tintColor,
		double drawW,
		double drawH,
		unsigned long long cacheFrame,
		int& outDrawW,
		int& outDrawH,
		std::string& outCacheKey);
	RealisticIconCacheEntry* GetCachedRotatedRealisticIconBitmap(
		const std::string& scaledCacheKey,
		Gdiplus::Bitmap* scaledBitmap,
		int scaledWidth,
		int scaledHeight,
		double rotationDeg,
		unsigned long long cacheFrame);
	void LoadAircraftSpecs();
	void InvalidateStructuredTagRuleCache();

	//---OnAsrContentLoaded--------------------------------------------

	virtual void OnAsrContentLoaded(bool Loaded);
	bool LoadInsetStateFromAsrForAirport(const std::string& airport, bool allowLegacyFallback);
	void SaveInsetStateToAsrForAirport(const std::string& airport);
	void ResetInsetWindowState(int appWindowId, bool preserveVisibility = true);
	void ResetAllInsetWindowStates(bool preserveVisibility = true);

	//---OnAsrContentToBeSaved------------------------------------------

	virtual void OnAsrContentToBeSaved();

	//---OnRefresh------------------------------------------------------

	virtual void OnRefresh(HDC hDC, int Phase);
	std::shared_ptr<const VsmrScene::RadarScene> BuildRadarScene(
		bool lowVisibilityProcedures,
		double* outRimcasMilliseconds = nullptr);
	const VsmrScene::RadarScene* GetCurrentRadarScene() const noexcept;
	void RenderRuntimeMenu(HDC hDC, Gdiplus::Graphics& graphics);
	bool HandleRuntimeMenuClick(int objectType, const char* objectId, POINT point, RECT area, int button);
	bool HandleRuntimeMenuMove(int objectType, const char* objectId, POINT point, RECT area, bool released);
	void CloseRuntimeMenuPopup();
	void LoadRuntimeMenuPositionFromAsr();
	void SaveRuntimeMenuPositionToAsr();
	std::string DetectDefaultAirportFromAviso() const;
	std::string ResolveAvisoGeoJsonPathForAirport(const std::string& airport) const;
	std::string GetAvisoGeoJsonEditorPathForAirport(const std::string& airport) const;
	void SetAvisoGeoJsonOverrideForAirport(const std::string& airport, const std::string& path);
	bool EnsureAvisoGeoJsonLoaded(
		const std::string& path,
		bool retainPreviousOnFailure = true);
	bool ForceReloadAvisoGeoJson();
	std::vector<AvisoGroup> GetAvisoGroups() const;
	std::shared_ptr<const std::unordered_map<std::string, bool>> GetAvisoGroupVisibilitySnapshot(
		unsigned long long* outGeneration = nullptr) const;
	bool GetAvisoRenderSnapshots(
		std::shared_ptr<const std::vector<AvisoFeature>>& outFeatures,
		std::shared_ptr<const std::vector<AvisoLabel>>& outLabels,
		std::shared_ptr<const std::unordered_map<std::string, bool>>& outGroupVisibility,
		unsigned long long& outGeneration) const;
	bool ApplyAvisoGroupMembershipSnapshot(
		const rapidjson::Value& aviso,
		std::string* outError = nullptr);
	bool SetAvisoGroupVisibility(const std::string& groupId, bool visible);
	bool ToggleAvisoGroupVisibility(const std::string& groupId, bool* outVisible = nullptr);
	bool SetAvisoGroupVisibilities(const std::vector<std::pair<std::string, bool>>& visibility);
	bool UpdateAvisoGroups(const std::vector<AvisoGroup>& groups);
	std::string GetAvisoColorPalette() const;
	bool SetAvisoColorPalette(const std::string& palette, bool persistToAsr = true);
	std::string GetUiColorTheme() const;
	bool SetUiColorTheme(const std::string& theme, bool persistToAsr = true);
	void InvalidateAvisoGroupRendering();
	void ClearAvisoGeoJsonRasterCache();
	CRect ResolveMainAvisoRenderArea();
	COLORREF GetAvisoBackgroundColor() const noexcept;
	void RenderAvisoGeoJson(HDC hDC, Gdiplus::Graphics& graphics);
	void BeginShutdown();
	bool IsShutdownRequested() const;
	void EnsureAvisoGeoJsonRenderPipeline();
	void StopAvisoGeoJsonRenderPipeline();
	bool IsAvisoGeoJsonRenderStopRequested() const;
	void RequestRefreshFromWorker();
	void QueueAvisoGeoJsonRasterRender(AvisoRasterRenderRequest request);
	void ApplyCompletedAvisoGeoJsonRaster();
	bool IsAvisoRasterRenderRequestCancelled(const AvisoRasterRenderRequest& request) const noexcept;
	std::unique_ptr<AvisoRasterRenderResult> RenderAvisoGeoJsonRaster(const AvisoRasterRenderRequest& request) const;
	bool PrewarmAvisoForActiveAirport();
	std::vector<AvisoPreset> GetAvisoPresets() const;
	std::string GetDefaultAvisoPresetName() const;
	std::string GetActiveAvisoPresetName() const;
	bool SaveAvisoPreset(
		const std::string& requestedName,
		bool overwriteExisting,
		std::string* outSavedName = nullptr,
		std::optional<bool> linkedMovementOverride = std::nullopt);
	bool LoadAvisoPreset(const std::string& name);
	bool RenameAvisoPreset(
		const std::string& oldName,
		const std::string& newName,
		std::optional<bool> linkedMovementOverride = std::nullopt);
	bool DuplicateAvisoPreset(const std::string& sourceName, const std::string& requestedName, std::string* outSavedName = nullptr);
	bool DeleteAvisoPreset(const std::string& name);
	bool SetDefaultAvisoPreset(const std::string& name);
	bool ClearDefaultAvisoPreset();
	bool ApplyDefaultAvisoPresetIfConfigured();
	void ResetAvisoPresetStateForActiveAirport(bool applyDefaultPreset = true);
	void ActivateNoAvisoPreset();
	bool UpdateActiveAvisoPreset();
	bool ResetActiveAvisoPreset();
	bool SetActiveAvisoPresetLinkedMovement(bool linked);
	bool IsAvisoPresetLinkedMovementEnabled() const;
	void SyncLinkedAvisoSecondaryToMainView();
	void RenderTags(Graphics& graphics, CDC& dc);

	//---OnClickScreenObject-----------------------------------------

	virtual void OnClickScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button);

	//---OnButtonDownScreenObject-------------------------------------

	virtual void OnButtonDownScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button);

	//---OnButtonUpScreenObject---------------------------------------

	virtual void OnButtonUpScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button);

	//---OnMoveScreenObject---------------------------------------------

	virtual void OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released);

	//---OnOverScreenObject---------------------------------------------

	virtual void OnOverScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area);
	bool HandleInsetSetCursor(HWND hwnd);
	bool HandleAvisoMouseWheel(HWND hwnd, WPARAM wParam, LPARAM lParam);
	bool HandleAvisoMouseWheelAtScreenPoint(POINT screenPoint, int wheelDelta, HWND sourceHwnd);
	void CancelInsetWindowInteractions();

	//---OnCompileCommand-----------------------------------------

	virtual bool OnCompileCommand(const char * sCommandLine);

	//---OnRadarTargetPositionUpdate---------------------------------------------

	virtual void OnRadarTargetPositionUpdate(CRadarTarget RadarTarget);

	//---OnFlightPlanDisconnect---------------------------------------------

	virtual void OnFlightPlanDisconnect(CFlightPlan FlightPlan);

	//---Haversine---------------------------------------------
	// Heading in deg, distance in m
	const double PI = SMRGeometry::Pi;

	virtual CPosition Haversine(CPosition origin, double heading, double distance);
	virtual double Haversine(CPosition origin, CPosition dest);

	//---GetZoomLevelFromCrossDistance-----------------------------
	int maxZoomLevel = 14;
	virtual int getZoomLevelFromCrossDistance(double crossDistance);

	//---getIntFromCategory-------------------------------------------
	virtual int getIntFromCategory(string category);

	//---GetBottomLine---------------------------------------------

	virtual string GetBottomLine(const char * Callsign);

	//---OnFunctionCall-------------------------------------------------

	virtual void OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area);

	//---OnAsrContentToBeClosed-----------------------------------------

	void EuroScopePlugInExitCustom();

	virtual void OnAsrContentToBeClosed(void);
};
