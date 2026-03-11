#pragma once
#include <EuroScopePlugIn.h>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <time.h>
#include <GdiPlus.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "Constant.hpp"
#include "CallsignLookup.hpp"
#include "Config.hpp"
#include "Rimcas.hpp"
#include "InsetWindow.h"
#include <memory>
#include <asio/io_service.hpp>
#include <thread>
#include <mutex>
#include <ctime>
#include "ColorManager.h"
#include "Logger.h"
#include <filesystem>
#include <iostream>

using namespace std;
using namespace Gdiplus;
using namespace EuroScopePlugIn;
namespace fs = std::filesystem;

namespace SMRSharedData
{
	static vector<string> ReleasedTracks;
	static vector<string> ManuallyCorrelated;
};


namespace SMRPluginSharedData
{
	static asio::io_service io_service;
}

using namespace SMRSharedData;

struct VacdmPilotData
{
	std::string callsign;
	std::string tobtState;
	std::time_t tobtUtc = 0;
	std::time_t tsatUtc = 0;
	std::time_t ttotUtc = 0;
	std::time_t asatUtc = 0;
	std::time_t aobtUtc = 0;
	std::time_t atotUtc = 0;
	std::time_t asrtUtc = 0;
	std::time_t aortUtc = 0;
	std::time_t ctotUtc = 0;
	bool hasTobt = false;
	bool hasTsat = false;
	bool hasTtot = false;
	bool hasAsat = false;
	bool hasAobt = false;
	bool hasAtot = false;
	bool hasAsrt = false;
	bool hasAort = false;
	bool hasCtot = false;
	bool hasBooking = false;
};

struct FrameVacdmLookupResult
{
	bool hasData = false;
	VacdmPilotData data;
};

struct StructuredTagColorRule
{
	struct Criterion
	{
		std::string source = "vacdm";
		std::string token;
		std::string condition;
	};

	std::string source = "vacdm";
	std::string token;
	std::string condition;
	std::vector<Criterion> criteria;
	std::string name;
	std::string tagType = "any";
	std::string status = "any";
	std::string detail = "any";
	bool applyTarget = false;
	int targetR = 255;
	int targetG = 255;
	int targetB = 255;
	int targetA = 255;
	bool applyTag = false;
	int tagR = 255;
	int tagG = 255;
	int tagB = 255;
	int tagA = 255;
	bool applyText = false;
	int textR = 255;
	int textG = 255;
	int textB = 255;
	int textA = 255;
};

bool TryGetVacdmPilotData(const std::string& callsign, VacdmPilotData& outData);
class CProfileEditorDialog;

class CSMRRadar :
	public EuroScopePlugIn::CRadarScreen
{
public:
	CSMRRadar();
	virtual ~CSMRRadar();

	void ReloadConfig();

	static map<string, string> vStripsStands;

	bool BLINK = false;
	bool drawRunways = false;
	map<string, POINT> TagsOffsets;

	vector<string> Active_Arrivals;

	clock_t clock_init, clock_final;

	COLORREF SMR_TARGET_COLOR = RGB(255, 242, 73);
	COLORREF SMR_H1_COLOR = RGB(0, 255, 255);
	COLORREF SMR_H2_COLOR = RGB(0, 219, 219);
	COLORREF SMR_H3_COLOR = RGB(0, 183, 183);

	typedef struct tagPOINT2 {
		double x;
		double y;
	} POINT2;

	struct Patatoide_Points {
		map<int, POINT2> points;
		map<int, POINT2> History_one_points;
		map<int, POINT2> History_two_points;
		map<int, POINT2> History_three_points;
	};

	map<string, Patatoide_Points> Patatoides;

	int RadarViewZoomLevel = 0;

	map<string, bool> ClosedRunway;

	char DllPathFile[_MAX_PATH];
	string DllPath;
	string ConfigPath;
	string mapsPath;
	CCallsignLookup * Callsigns = nullptr;
	CColorManager * ColorManager;
	std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> AircraftIcons;
	std::string IconsPath;
	struct AircraftSpec { double length = 0.0; double wingspan = 0.0; };
	std::map<std::string, AircraftSpec> AircraftSpecs;
	mutable bool StructuredTagRulesCacheValid = false;
	mutable std::vector<StructuredTagColorRule> StructuredTagRulesCache;

	map<string, bool> ShowLists;
	map<string, RECT> ListAreas;

	map<int, bool> appWindowDisplays;

	map<string, CRect> tagAreas;
	map<string, CRect> tagCollisionAreas;
	map<string, double> TagAngles;
	map<string, int> TagLeaderLineLength;
	map<string, CRect> previousTagSize;
	map<std::string, POINT> TagDragOffsetFromCenter;

	bool QDMenabled = false;
	bool QDMSelectEnabled = false;
	POINT QDMSelectPt;
	POINT QDMmousePt;

	bool ColorSettingsDay = true;
	vector<string> ProfileColorPaths;
	map<string, bool> ProfileColorPathHasAlpha;
	string SelectedProfileColorPath;
	std::unique_ptr<CProfileEditorDialog> ProfileEditorDialog;
	std::string TagDefinitionEditorType = "departure";
	bool TagDefinitionEditorDetailed = false;
	std::string TagDefinitionEditorDepartureStatus = "default";
	int TagDefinitionEditorSelectedLine = 0;
	static const int TagDefinitionEditorMaxLines = 4;

	bool isLVP = false;

	map<string, RECT> TimePopupAreas;

	map<int, string> TimePopupData;
	multimap<string, string> AcOnRunway;
	map<string, bool> ColorAC;

	map<string, CRimcas::RunwayAreaType> RunwayAreas;

	map<string, RECT> MenuPositions;
	map<string, bool> DisplayMenu;

	map<string, clock_t> RecentlyAutoMovedTags;

	CRimcas * RimcasInstance = nullptr;
	CConfig * CurrentConfig = nullptr;

	map<int, Gdiplus::Font *> customFonts;
	int currentFontSize = 1;

	map<string, CPosition> AirportPositions;

	bool Afterglow = true;

	int Trail_Gnd = 4;
	int Trail_App = 4;
	int PredictedLength = 0;

	bool NeedCorrelateCursor = false;
	bool ReleaseInProgress = false;
	bool AcquireInProgress = false;

	multimap<string, string> DistanceTools;
	bool DistanceToolActive = false;
	pair<string, string> ActiveDistance;

	//----
	// Tag types
	//---

	enum TagTypes { Departure, Arrival, Airborne, Uncorrelated };


	string ActiveAirport = "EGKK";

	inline string getActiveAirport() {
		return ActiveAirport;
	}

	inline string setActiveAirport(string value) {
		return ActiveAirport = value;
	}

	//---GenerateTagData--------------------------------------------

	static map<string, string> GenerateTagData(CRadarTarget Rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, bool useSpeedForGates, string ActiveAirport);
	using TagReplacingMap = std::map<std::string, std::string>;
	using FrameTagDataCache = std::unordered_map<std::string, TagReplacingMap>;
	using FrameVacdmLookupCache = std::unordered_map<std::string, FrameVacdmLookupResult>;

	//---IsCorrelatedFuncs---------------------------------------------

	inline virtual bool IsCorrelated(CFlightPlan fp, CRadarTarget rt)
	{
		auto hasText = [](const char* text) -> bool
		{
			return text != nullptr && text[0] != '\0';
		};

		if (CurrentConfig->getActiveProfile()["filters"]["pro_mode"]["enable"].GetBool())
		{
			if (fp.IsValid())
			{
				bool isCorr = false;
				const char* assignedSquawk = fp.GetControllerAssignedData().GetSquawk();
				const char* reportedSquawk = (rt.IsValid() && rt.GetPosition().IsValid()) ? rt.GetPosition().GetSquawk() : nullptr;
				if (hasText(assignedSquawk) && hasText(reportedSquawk) && strcmp(assignedSquawk, reportedSquawk) == 0)
				{
					isCorr = true;
				}

				if (CurrentConfig->getActiveProfile()["filters"]["pro_mode"]["accept_pilot_squawk"].GetBool())
				{
					isCorr = true;
				}

				if (isCorr)
				{
					const Value& sqs = CurrentConfig->getActiveProfile()["filters"]["pro_mode"]["do_not_autocorrelate_squawks"];
					for (SizeType i = 0; i < sqs.Size(); i++) {
						if (hasText(reportedSquawk) && sqs[i].IsString() && strcmp(reportedSquawk, sqs[i].GetString()) == 0)
						{
							isCorr = false;
							break;
						}
					}
				}

				const char* systemId = rt.IsValid() ? rt.GetSystemID() : nullptr;
				if (hasText(systemId) && std::find(ManuallyCorrelated.begin(), ManuallyCorrelated.end(), systemId) != ManuallyCorrelated.end())
				{
					isCorr = true;
				}

				if (hasText(systemId) && std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId) != ReleasedTracks.end())
				{
					isCorr = false;
				}

				return isCorr;
			}

			return false;
		} else
		{
			// If the pro mode is not used, then the AC is always correlated
			return true;
		}
	};

	//---CorrelateCursor--------------------------------------------

	virtual void CorrelateCursor();

	//---LoadCustomFont--------------------------------------------

	virtual void LoadCustomFont();

	//---LoadProfile--------------------------------------------

	virtual void LoadProfile(string profileName);
	void EnsureTargetGroundStatusColorEntries();
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
	bool PersistProfileEditorWindowLayout(const CRect& windowRect, bool visible, bool persistToDisk);
	CRect GetProfileEditorWindowRectFromConfig() const;
	std::vector<std::string> GetProfileColorPathsForEditor();
	std::string GetSelectedProfileColorPathForEditor() const;
	bool SelectProfileColorPathForEditor(const std::string& path);
	bool GetSelectedProfileColorForEditor(int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool SetSelectedProfileColorForEditor(int r, int g, int b, int a, bool useAlpha, bool persistToDisk);
	std::vector<std::string> GetProfileNamesForEditor() const;
	std::string GetActiveProfileNameForEditor() const;
	bool SetActiveProfileForEditor(const std::string& name, bool persistToDisk);
	bool GetProfileProModeEnabledForEditor(const std::string& name, bool& outEnabled) const;
	bool SetProfileProModeEnabledForEditor(const std::string& name, bool enabled);
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
	void LoadAircraftSpecs();
	void InvalidateStructuredTagRuleCache();

	//---OnAsrContentLoaded--------------------------------------------

	virtual void OnAsrContentLoaded(bool Loaded);

	//---OnAsrContentToBeSaved------------------------------------------

	virtual void OnAsrContentToBeSaved();

	//---OnRefresh------------------------------------------------------

	virtual void OnRefresh(HDC hDC, int Phase);
	void RenderTags(Graphics& graphics, CDC& dc, bool frameProModeEnabled, const FrameTagDataCache& frameTagDataCache, const FrameVacdmLookupCache& frameVacdmLookupCache);

	//---OnClickScreenObject-----------------------------------------

	virtual void OnClickScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, int Button);

	//---OnMoveScreenObject---------------------------------------------

	virtual void OnMoveScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area, bool Released);

	//---OnOverScreenObject---------------------------------------------

	virtual void OnOverScreenObject(int ObjectType, const char * sObjectId, POINT Pt, RECT Area);

	//---OnCompileCommand-----------------------------------------

	virtual bool OnCompileCommand(const char * sCommandLine);

	//---RefreshAirportActivity---------------------------------------------

	virtual void RefreshAirportActivity(void);

	//---OnRadarTargetPositionUpdate---------------------------------------------

	virtual void OnRadarTargetPositionUpdate(CRadarTarget RadarTarget);

	//---OnFlightPlanDisconnect---------------------------------------------

	virtual void OnFlightPlanDisconnect(CFlightPlan FlightPlan);

	virtual bool isVisible(CRadarTarget rt)
	{
		if (!rt.IsValid())
			return false;

		CRadarTargetPositionData RtPos = rt.GetPosition();
		if (!RtPos.IsValid())
			return false;

		auto airportIt = AirportPositions.find(getActiveAirport());
		if (airportIt == AirportPositions.end())
			return false;

		int radarRange = CurrentConfig->getActiveProfile()["filters"]["radar_range_nm"].GetInt();
		int altitudeFilter = CurrentConfig->getActiveProfile()["filters"]["hide_above_alt"].GetInt();
		int speedFilter = CurrentConfig->getActiveProfile()["filters"]["hide_above_spd"].GetInt();
		bool isAcDisplayed = true;

		if (airportIt->second.DistanceTo(RtPos.GetPosition()) > radarRange)
			isAcDisplayed = false;

		if (altitudeFilter != 0) {
			if (RtPos.GetPressureAltitude() > altitudeFilter)
				isAcDisplayed = false;
		}

		if (speedFilter != 0) {
			if (RtPos.GetReportedGS() > speedFilter)
				isAcDisplayed = false;
		}

		return isAcDisplayed;
	}

	//---Haversine---------------------------------------------
	// Heading in deg, distance in m
	const double PI = (double)M_PI;

	inline virtual CPosition Haversine(CPosition origin, double heading, double distance) {

		CPosition newPos;

		double d = (distance*0.00053996) / 60 * PI / 180;
		double trk = DegToRad(heading);
		double lat0 = DegToRad(origin.m_Latitude);
		double lon0 = DegToRad(origin.m_Longitude);

		double lat = asin(sin(lat0) * cos(d) + cos(lat0) * sin(d) * cos(trk));
		double lon = cos(lat) == 0 ? lon0 : fmod(lon0 + asin(sin(trk) * sin(d) / cos(lat)) + PI, 2 * PI) - PI;

		newPos.m_Latitude = RadToDeg(lat);
		newPos.m_Longitude = RadToDeg(lon);

		return newPos;
	}

	inline virtual double Haversine(CPosition origin, CPosition dest) {
		double haversine;
		double temp;

		double earthRadius = 6372797.56085;

		origin.m_Latitude = DegToRad(origin.m_Latitude);
		origin.m_Longitude = DegToRad(origin.m_Longitude);
		dest.m_Latitude = DegToRad(dest.m_Latitude);
		dest.m_Longitude = DegToRad(dest.m_Longitude);

		haversine = (pow(sin((1.0 / 2) * (dest.m_Latitude - origin.m_Latitude)), 2)) + ((cos(origin.m_Latitude)) * (cos(dest.m_Latitude)) * (pow(sin((1.0 / 2) * (dest.m_Longitude - origin.m_Longitude)), 2)));
		temp = 2 * asin(min(1.0, sqrt(haversine)));
		return earthRadius * temp;
	}

	//---GetZoomLevelFromCrossDistance-----------------------------
	int maxZoomLevel = 14;
	inline virtual int getZoomLevelFromCrossDistance(double crossDistance) {
		int d = (int)crossDistance;

		if (d <= 2000)
			return 14;
		if (d <= 2500)
			return 13;
		if (d <= 3000)
			return 12;
		if (d <= 4000)
			return 11;
		if (d <= 5000)
			return 10;
		if (d <= 6000)
			return 9;
		if (d <= 8000)
			return 8;
		if (d <= 9500)
			return 7;
		if (d <= 12000)
			return 6;
		if (d <= 14000)
			return 5;
		if (d <= 18000)
			return 4;
		if (d <= 22000)
			return 3;
		if (d <= 28000)
			return 2;
		if (d <= 34000)
			return 1;
		return 0;
	}

	inline virtual float randomizeHeading(float originHead) {
		return float(fmod(originHead + float((rand() % 5) - 2), 360));
	}

	//---getIntFromCategory-------------------------------------------
	inline virtual int getIntFromCategory(string category) {
		if (category == "FREETEXT")
			return SECTOR_ELEMENT_FREE_TEXT;
		else if (category == "RUNWAY")
			return SECTOR_ELEMENT_RUNWAY;
		else if (category == "VOR")
			return SECTOR_ELEMENT_VOR;
		else if (category == "NDB")
			return SECTOR_ELEMENT_NDB;
		else if (category == "FIX")
			return SECTOR_ELEMENT_FIX;
		else if (category == "AIRPORT")
			return SECTOR_ELEMENT_AIRPORT;
		else if (category == "STAR")
			return  SECTOR_ELEMENT_STAR;
		else if (category == "SID")
			return SECTOR_ELEMENT_SID;
		else if (category == "ARTC")
			return SECTOR_ELEMENT_ARTC;
		else if (category == "GEO")
			return SECTOR_ELEMENT_GEO;
		else if (category == "AIRSPACE")
			return SECTOR_ELEMENT_AIRSPACE;
		else if (category == "REGIONS")
			return SECTOR_ELEMENT_REGIONS;
		else
			return -1;
	}

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

	//  This gets called before OnAsrContentToBeSaved()
	// -> we can't delete CurrentConfig just yet otherwise we can't save the active profile
	inline virtual void OnAsrContentToBeClosed(void)
	{
		CloseProfileEditorWindow(false);
		DestroyProfileEditorWindow();
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
		CurrentConfig->saveConfig();
		delete RimcasInstance;
		delete this;
	};
};
