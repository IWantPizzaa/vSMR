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
#include <time.h>
#include <GdiPlus.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "Constant.hpp"
#include "CallsignLookup.hpp"
#include "Config.hpp"
#include "Rimcas.hpp"
#include "SMRGeometry.hpp"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <ctime>
#include "Logger.h"
#include "SMRDataTypes.hpp"
#include <filesystem>
#include <iostream>
#include <optional>

using namespace std;
using namespace Gdiplus;
using namespace EuroScopePlugIn;
namespace fs = std::filesystem;

class CProfileEditorDialog;
class CAvisoEditorDialog;
class CVsmrControlCenterDialog;
class CInsetWindow;

class CSMRRadar :
	public EuroScopePlugIn::CRadarScreen
{
public:
	CSMRRadar();
	virtual ~CSMRRadar();

	bool ReloadConfig();
	bool SetProfilesConfigPath(
		const std::string& path,
		std::string* errorText = nullptr,
		bool persistToAsr = true);

	static map<string, string> vStripsStands;

	bool drawRunways = false;
	map<string, POINT> TagsOffsets;

	typedef struct tagPOINT2 {
		double x;
		double y;
	} POINT2;

	struct Patatoide_Points {
		map<int, POINT2> points;
	};

	map<string, Patatoide_Points> Patatoides;

	int RadarViewZoomLevel = 0;
	std::map<std::string, CRimcas::RunwayStatus> LastMapRunwayStatuses;
	std::string LastMapActiveAirport;

	char DllPathFile[_MAX_PATH];
	string DllPath;
	string DataPath;
	string ConfigPath;
	string mapsPath;
	std::unique_ptr<CCallsignLookup> Callsigns;
	std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> AircraftIcons;
	std::string IconsPath;
	struct AircraftSpec { double length = 0.0; double wingspan = 0.0; };
	std::map<std::string, AircraftSpec> AircraftSpecs;
	struct RealisticIconCacheEntry
	{
		std::unique_ptr<Gdiplus::Bitmap> bitmap;
		int centerX = 0;
		int centerY = 0;
		unsigned long long lastUsedFrame = 0;
	};
	std::map<std::string, RealisticIconCacheEntry> RealisticIconBitmapCache;
	unsigned long long RealisticIconCacheFrame = 0;
	mutable bool StructuredTagRulesCacheValid = false;
	mutable std::vector<StructuredTagColorRule> StructuredTagRulesCache;
	struct AvisoPoint
	{
		double longitude = 0.0;
		double latitude = 0.0;
	};
	struct AvisoGroup
	{
		std::string id;
		std::string name;
		bool visible = true;
	};
	struct AvisoFeature
	{
		bool polygon = false;
		int sourceFeatureIndex = -1;
		std::string sourceFeatureId;
		std::vector<std::string> groupIds;
		std::vector<std::vector<AvisoPoint>> paths;
		Gdiplus::Color fillColor = Gdiplus::Color(217, 53, 66, 82);
		Gdiplus::Color strokeColor = Gdiplus::Color(191, 140, 152, 170);
		float strokeWidth = 1.0f;
		double minLongitude = 0.0;
		double minLatitude = 0.0;
		double maxLongitude = 0.0;
		double maxLatitude = 0.0;
	};
	struct AvisoLabel
	{
		AvisoPoint position;
		int sourceFeatureIndex = -1;
		std::string sourceFeatureId;
		std::vector<std::string> groupIds;
		std::wstring text;
		std::wstring fontFamily = L"Arial";
		std::string labelClass;
		std::string textAnchor = "center";
		Gdiplus::Color textColor = Gdiplus::Color(255, 128, 128, 128);
		Gdiplus::Color haloColor = Gdiplus::Color(255, 0, 0, 0);
		float textSize = 12.0f;
		float haloWidth = 1.0f;
		double maxMetersPerPixel = 0.0;
		double maxViewRangeKm = 0.0;
	};
	struct AvisoMainViewPreset
	{
		bool valid = false;
		double minLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLatitude = 0.0;
		double maxLongitude = 0.0;
		int zoomLevel = 0;
	};
	struct AvisoPreset
	{
		struct InsetWindowState
		{
			bool valid = false;
			RECT area = { 300, 200, 606, 375 };
			int layoutMode = 0;
			bool visible = false;
		};

		struct SecondaryRadarWindow
		{
			bool valid = false;
			RECT area = { 200, 200, 600, 500 };
			POINT offset = { 0, 0 };
			int scale = 15;
			int filter = 5500;
			double rotation = 0.0;
			int layoutMode = 0;
			bool visible = false;
		};

		std::string name;
		AvisoMainViewPreset mainView;
		RECT secondaryArea = { 260, 260, 760, 560 };
		int secondaryScale = 350;
		double secondaryCenterLatitude = 0.0;
		double secondaryCenterLongitude = 0.0;
		int secondaryLayoutMode = 0;
		bool secondaryVisible = true;
		bool linkedMovement = false;
		std::array<SecondaryRadarWindow, 1> srw;
		InsetWindowState weather;
		InsetWindowState timer;
	};
	struct AvisoRasterRenderRequest
	{
		unsigned long long requestId = 0;
		unsigned long long groupGeneration = 0;
		std::string path;
		std::shared_ptr<const std::vector<AvisoFeature>> features;
		std::shared_ptr<const std::vector<AvisoLabel>> labels;
		std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
		int rasterWidth = 0;
		int rasterHeight = 0;
		double rasterScale = 1.0;
		double displayMinLongitude = 0.0;
		double displayMinLatitude = 0.0;
		double displayMaxLongitude = 0.0;
		double displayMaxLatitude = 0.0;
		double renderMinLongitude = 0.0;
		double renderMinLatitude = 0.0;
		double renderMaxLongitude = 0.0;
		double renderMaxLatitude = 0.0;
		double renderScreenLeft = 0.0;
		double renderScreenTop = 0.0;
		double scaleX = 1.0;
		double scaleY = 1.0;
		Gdiplus::PointF projectedTopLeft;
		Gdiplus::PointF projectedTopRight;
		Gdiplus::PointF projectedBottomLeft;
		Gdiplus::PointF projectedBottomRight;
	};
	struct AvisoRasterRenderResult
	{
		~AvisoRasterRenderResult()
		{
			if (bitmap != nullptr)
				::DeleteObject(bitmap);
		}

		unsigned long long requestId = 0;
		unsigned long long groupGeneration = 0;
		HBITMAP bitmap = nullptr;
		std::string path;
		int rasterWidth = 0;
		int rasterHeight = 0;
		double displayMinLongitude = 0.0;
		double displayMinLatitude = 0.0;
		double displayMaxLongitude = 0.0;
		double displayMaxLatitude = 0.0;
		double renderMinLongitude = 0.0;
		double renderMinLatitude = 0.0;
		double renderMaxLongitude = 0.0;
		double renderMaxLatitude = 0.0;
		Gdiplus::PointF projectedTopLeft;
		Gdiplus::PointF projectedTopRight;
		Gdiplus::PointF projectedBottomLeft;
		Gdiplus::PointF projectedBottomRight;
	};
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
	std::string AvisoGeoJsonRasterCachePath;
	unsigned long long AvisoGeoJsonRasterGroupGeneration = 0;
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
	std::mutex AvisoGeoJsonRenderMutex;
	std::condition_variable AvisoGeoJsonRenderCondition;
	std::thread AvisoGeoJsonRenderThread;
	std::atomic<HWND> AvisoRefreshHostWindow{ nullptr };
	std::atomic<bool> ShutdownRequested{ false };
	bool AvisoGeoJsonRenderThreadStarted = false;
	std::atomic<bool> AvisoGeoJsonRenderStop{ false };
	std::unique_ptr<AvisoRasterRenderRequest> AvisoGeoJsonPendingRenderRequest;
	std::unique_ptr<AvisoRasterRenderResult> AvisoGeoJsonCompletedRenderResult;
	unsigned long long AvisoGeoJsonRenderNextRequestId = 0;
	unsigned long long AvisoGeoJsonRenderLatestRequestId = 0;
	bool AvisoGeoJsonRenderLastRequestValid = false;
	std::string AvisoGeoJsonRenderLastRequestPath;
	double AvisoGeoJsonRenderLastRequestMinLongitude = 0.0;
	double AvisoGeoJsonRenderLastRequestMinLatitude = 0.0;
	double AvisoGeoJsonRenderLastRequestMaxLongitude = 0.0;
	double AvisoGeoJsonRenderLastRequestMaxLatitude = 0.0;
	int AvisoGeoJsonRenderLastRequestRasterWidth = 0;
	int AvisoGeoJsonRenderLastRequestRasterHeight = 0;
	unsigned long long AvisoGeoJsonRenderLastRequestGroupGeneration = 0;
	Gdiplus::PointF AvisoGeoJsonRenderLastRequestProjectedTopLeft;
	Gdiplus::PointF AvisoGeoJsonRenderLastRequestProjectedTopRight;
	Gdiplus::PointF AvisoGeoJsonRenderLastRequestProjectedBottomLeft;
	Gdiplus::PointF AvisoGeoJsonRenderLastRequestProjectedBottomRight;
	bool AvisoGeoJsonScrollSelected = false;
	bool AvisoViewsLinked = false;
	std::string ActiveAvisoPresetName;
	double PerfLastFrameMs = 0.0;
	double PerfLastAvisoMs = 0.0;
	double PerfLastTargetsMs = 0.0;
	double PerfLastRimcasMs = 0.0;
	double PerfLastTagsMs = 0.0;
	double PerfLastSrwMs = 0.0;
	unsigned long PerfLastLogTick = 0;

	map<int, bool> appWindowDisplays;

	map<string, CRect> tagAreas;
	map<string, CRect> tagCollisionAreas;
	map<string, double> TagAngles;
	map<string, int> TagLeaderLineLength;
	map<string, CRect> previousTagSize;
	map<std::string, POINT> TagDragOffsetFromCenter;

	vector<string> ProfileColorPaths;
	map<string, bool> ProfileColorPathHasAlpha;
	string SelectedProfileColorPath;
	std::unique_ptr<CProfileEditorDialog> ProfileEditorDialog;
	std::unique_ptr<CAvisoEditorDialog> AvisoEditorDialog;
	std::unique_ptr<CVsmrControlCenterDialog> VsmrControlCenterDialog;
	std::string TagDefinitionEditorType = "departure";
	bool TagDefinitionEditorDetailed = false;
	std::string TagDefinitionEditorDepartureStatus = "default";
	int TagDefinitionEditorSelectedLine = 0;
	static const int TagDefinitionEditorMaxLines = 4;

	bool isLVP = false;
	bool RimcasEnabled = true;
	bool RimcasUseRedEmergencySymbols = true;
	bool RimcasRunwaysExplicitlyConfigured = false;

	map<string, RECT> TimePopupAreas;

	map<string, RECT> MenuPositions;
	enum class RuntimeMenuPopup
	{
		None,
		Mode,
		Groups,
		Insets,
		Profile
	};
	RuntimeMenuPopup ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	std::string PendingGroundStatusCallsign;
	POINT RuntimeMenuPosition = { 14, 100 };
	bool RuntimeMenuPositionInitialized = false;
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
	CFont RuntimeOverlayFont;
	CFont RuntimeMenuActionFont;
	bool AirportPositionsCacheValid = false;
	struct CachedRunwayGeometry
	{
		std::string runwayNameA;
		std::string runwayNameB;
		std::string displayName;
		double trueHeadingA = 0.0;
		double trueHeadingB = 0.0;
		bool trueHeadingAValid = false;
		bool trueHeadingBValid = false;
		std::vector<CPosition> rimcasDefinition;
		std::vector<CPosition> closedDefinition;
	};
	std::vector<CachedRunwayGeometry> CachedRunwayGeometries;
	bool CachedRunwayGeometryValid = false;
	std::string CachedRunwayAirport;
	std::string CachedRunwayProfile;
	bool CachedRunwayIsLvp = false;
	unsigned long RunwayStatusLastRefreshTick = 0;
	std::string RunwayStatusLastAirport;

	map<string, clock_t> RecentlyAutoMovedTags;

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

	void InvalidateAirportPositionCache();
	void InvalidateRunwayGeometryCache();
	void EnsureAirportPositionCache();
	void EnsureRunwayGeometryCache();
	void RefreshRunwayStatuses(bool force);

	inline string getActiveAirport() const {
		return ActiveAirport;
	}

	string setActiveAirport(string value, bool switchInsetContext = true);

	inline bool TryGetActiveAirportPosition(CPosition& outPosition) const
	{
		auto airportIt = AirportPositions.find(ActiveAirport);
		if (airportIt == AirportPositions.end())
			return false;
		outPosition = airportIt->second;
		return true;
	}

	//---GenerateTagData--------------------------------------------

	static map<string, string> GenerateTagData(CRadarTarget Rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, bool useSpeedForGates, string ActiveAirport, const std::string& stableCallsign = "");
	using TagReplacingMap = std::map<std::string, std::string>;
	using FrameTagDataCache = std::unordered_map<std::string, TagReplacingMap>;
	using FrameVacdmLookupCache = std::unordered_map<std::string, FrameVacdmLookupResult>;

	//---IsCorrelatedFuncs---------------------------------------------

	struct CorrelationSettings
	{
		bool proModeEnabled = false;
		bool acceptPilotSquawk = true;
		std::vector<std::string> blockedAutoCorrelateSquawks;
	};

	struct DisplayModeStatusVisibility
	{
		bool noStatus = true;
		bool push = true;
		bool startup = true;
		bool taxi = true;
		bool lineup = true;
		bool departure = true;
		bool onRunway = true;
		bool airborne = true;
		bool arrivals = true;
		bool noFlightPlan = true;
		bool uncorrelated = true;
	};

	struct DisplayModeSettings
	{
		std::string name = "Normal";
		bool requireAssignedSquawk = false;
		bool acceptPilotSquawk = true;
		bool requireClearance = false;
		bool requireValidTsat = false;
		bool requireActiveTobt = false;
		std::vector<std::string> blockedAutoCorrelateSquawks;
		bool towerFilter = false;
		bool structuredRulesEnabled = true;
		DisplayModeStatusVisibility statuses;
	};

	CorrelationSettings BuildCorrelationSettings() const;
	bool IsCorrelatedWithSettings(CFlightPlan fp, CRadarTarget rt, const CorrelationSettings& settings) const;
	virtual bool IsCorrelated(CFlightPlan fp, CRadarTarget rt);
	DisplayModeSettings GetActiveDisplayModeSettings() const;
	bool ShouldDisplayTargetForDisplayMode(CFlightPlan fp, CRadarTarget rt, bool acIsCorrelated, int reportedGs, bool targetOnRunway, const DisplayModeSettings& settings) const;

	//---LoadCustomFont--------------------------------------------

	virtual void LoadCustomFont();

	//---LoadProfile--------------------------------------------

	virtual void LoadProfile(
		string profileName,
		bool saveOutgoingState = true,
		bool persistNormalization = true);
	void EnsureTargetGroundStatusColorEntries(bool persistChanges = true);
	void RebuildProfileColorEntries();
	bool IsProfileColorPathValid(const std::string& path, bool* hasAlpha = nullptr);
	int GetProfileColorComponentValue(const std::string& path, char component, int fallback = 0);
	bool UpdateProfileColorComponent(const std::string& path, char component, int value);
	void OpenProfileEditorWindow();
	void CloseProfileEditorWindow(bool persistVisibility);
	void DestroyProfileEditorWindow();
	void OnProfileEditorWindowClosed();
	void OnProfileEditorWindowLayoutChanged(const CRect& windowRect);
	bool IsProfileEditorWindowVisible() const;
	bool EnsureProfileEditorWindowCreated();
	bool EnsureVsmrControlCenterWindowCreated();
	void OpenVsmrControlCenterWindow();
	void OpenVsmrControlCenterWindow(const std::string& pageName);
	void CloseVsmrControlCenterWindow();
	void DestroyVsmrControlCenterWindow();
	void OnVsmrControlCenterWindowClosed();
	bool PersistProfileEditorWindowLayout(const CRect& windowRect, bool visible, bool persistToDisk);
	CRect GetProfileEditorWindowRectFromConfig() const;
	std::vector<std::string> GetProfileColorPathsForEditor();
	std::string GetSelectedProfileColorPathForEditor() const;
	bool SelectProfileColorPathForEditor(const std::string& path);
	bool GetSelectedProfileColorForEditor(int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool SetSelectedProfileColorForEditor(int r, int g, int b, int a, bool useAlpha, bool persistToDisk);
	std::vector<std::string> GetOrderedProfileNamesForUi() const;
	std::vector<std::string> GetProfileNamesForEditor() const;
	std::string GetActiveProfileNameForEditor() const;
	bool SetActiveProfileForEditor(const std::string& name, bool persistToDisk);
	std::string ReadLastActiveProfileFromConfig() const;
	void WriteLastActiveProfileToConfig(const std::string& profileName) const;
	static void RememberSessionActiveProfile(const std::string& profileName);
	static std::string GetSessionActiveProfile(const std::string& fallbackProfile);
	std::vector<DisplayModeSettings> GetProfileDisplayModesForEditor(const std::string& profileName) const;
	std::string GetActiveProfileDisplayModeForEditor(const std::string& profileName) const;
	bool SetProfileDisplayModeActiveForEditor(const std::string& profileName, const std::string& modeName);
	bool AddProfileDisplayModeForEditor(const std::string& profileName, const std::string& requestedName, bool duplicateSelectedMode, const std::string& selectedModeName, std::string* outCreatedName = nullptr);
	bool RenameProfileDisplayModeForEditor(const std::string& profileName, const std::string& oldName, const std::string& newName);
	bool DeleteProfileDisplayModeForEditor(const std::string& profileName, const std::string& modeName);
	bool UpdateProfileDisplayModeForEditor(const std::string& profileName, const DisplayModeSettings& settings);
	bool AddProfileForEditor(const std::string& requestedName, bool duplicateActiveProfile, std::string* outCreatedName = nullptr);
	bool RenameProfileForEditor(const std::string& oldName, const std::string& newName);
	bool DeleteProfileForEditor(const std::string& name);
	std::vector<std::string> GetTagDefinitionTokens() const;
	std::string NormalizeTagDefinitionType(const std::string& type) const;
	std::string TagDefinitionTypeLabel(const std::string& type) const;
	std::string NormalizeTagDefinitionDepartureStatus(const std::string& status) const;
	std::string TagDefinitionDepartureStatusLabel(const std::string& status) const;
	std::vector<std::string> GetTagDefinitionStatusesForType(const std::string& type) const;
	bool IsTagDefinitionStatusAllowedForType(const std::string& type, const std::string& status) const;
	bool GetTagAutoDeconflictionEnabledForEditor() const;
	bool SetTagAutoDeconflictionEnabledForEditor(bool enabled, bool persistToDisk);
	bool GetTagRoundedCornersEnabledForEditor() const;
	bool SetTagRoundedCornersEnabledForEditor(bool enabled, bool persistToDisk);
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
	bool GetFixedPixelTargetIconSizeEnabled() const;
	bool SetFixedPixelTargetIconSizeEnabled(bool enabled, bool persistToDisk);
	double GetFixedPixelTriangleIconScale() const;
	bool SetFixedPixelTriangleIconScale(double scale, bool persistToDisk);
	bool GetSmallTargetIconBoostEnabled() const;
	bool SetSmallTargetIconBoostEnabled(bool enabled, bool persistToDisk);
	double GetSmallTargetIconBoostFactor() const;
	bool SetSmallTargetIconBoostFactor(double factor, bool persistToDisk);
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
	void InvalidateAvisoGroupRendering();
	void ClearAvisoGeoJsonRasterCache();
	CRect ResolveMainAvisoRenderArea();
	void RenderAvisoGeoJson(HDC hDC, Gdiplus::Graphics& graphics);
	void BeginShutdown();
	bool IsShutdownRequested() const;
	void EnsureAvisoGeoJsonRenderThread();
	void StopAvisoGeoJsonRenderThread();
	bool IsAvisoGeoJsonRenderStopRequested() const;
	void RequestRefreshFromWorker();
	void AvisoGeoJsonRenderThreadMain();
	void QueueAvisoGeoJsonRasterRender(AvisoRasterRenderRequest request);
	void ApplyCompletedAvisoGeoJsonRaster();
	std::unique_ptr<AvisoRasterRenderResult> RenderAvisoGeoJsonRaster(const AvisoRasterRenderRequest& request) const;
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
	bool UpdateActiveAvisoPreset();
	bool ResetActiveAvisoPreset();
	bool SetActiveAvisoPresetLinkedMovement(bool linked);
	bool IsAvisoPresetLinkedMovementEnabled() const;
	void SyncLinkedAvisoSecondaryToMainView();
	bool EnsureAvisoEditorWindowCreated();
	void OpenAvisoEditorWindow();
	void CloseAvisoEditorWindow();
	void DestroyAvisoEditorWindow();
	void OnAvisoEditorWindowClosed();
	void RenderTags(Graphics& graphics, CDC& dc, bool frameProModeEnabled, bool frameTowerModeEnabled, const FrameTagDataCache& frameTagDataCache, const FrameVacdmLookupCache& frameVacdmLookupCache);

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

	virtual bool isVisible(CRadarTarget rt);

	//---Haversine---------------------------------------------
	// Heading in deg, distance in m
	const double PI = SMRGeometry::Pi;

	virtual CPosition Haversine(CPosition origin, double heading, double distance);
	virtual double Haversine(CPosition origin, CPosition dest);

	//---GetZoomLevelFromCrossDistance-----------------------------
	int maxZoomLevel = 14;
	virtual int getZoomLevelFromCrossDistance(double crossDistance);
	virtual float randomizeHeading(float originHead);

	//---getIntFromCategory-------------------------------------------
	virtual int getIntFromCategory(string category);

	//---GetBottomLine---------------------------------------------

	virtual string GetBottomLine(const char * Callsign);

	//---LineIntersect---------------------------------------------

	/*inline virtual POINT getIntersectionPoint(POINT lineA, POINT lineB, POINT lineC, POINT lineD) {

		double x1 = lineA.x;
		double y1 = lineA.y;
		double x2 = lineB.x;
		double y2 = lineB.y;

		double x3 = lineC.x;
		double y3 = lineC.y;
		double x4 = lineD.x;
		double y4 = lineD.y;

		POINT p = { 0, 0 };

		double d = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
		if (d != 0) {
			double xi = ((x3 - x4) * (x1 * y2 - y1 * x2) - (x1 - x2) * (x3 * y4 - y3 * x4)) / d;
			double yi = ((y3 - y4) * (x1 * y2 - y1 * x2) - (y1 - y2) * (x3 * y4 - y3 * x4)) / d;

			p = { (int)xi, (int)yi };

		}
		return p;
	}*/

	//---OnFunctionCall-------------------------------------------------

	virtual void OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area);

	//---OnAsrContentToBeClosed-----------------------------------------

	void CSMRRadar::EuroScopePlugInExitCustom();

	virtual void OnAsrContentToBeClosed(void);
};
