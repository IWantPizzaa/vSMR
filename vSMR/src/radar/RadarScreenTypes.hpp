#pragma once

#include <Windows.h>

#include <EuroScopePlugIn.h>
#include <GdiPlus.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace VsmrRadarTypes
{
	struct Point2
	{
		double x;
		double y;
	};

	struct PatatoidePoints
	{
		std::map<int, Point2> points;
		std::map<int, Point2> historyOnePoints;
		std::map<int, Point2> historyTwoPoints;
		std::map<int, Point2> historyThreePoints;
	};

	struct AircraftSpec
	{
		double length = 0.0;
		double wingspan = 0.0;
	};

	struct RealisticIconCacheEntry
	{
		std::unique_ptr<Gdiplus::Bitmap> bitmap;
		int centerX = 0;
		int centerY = 0;
		unsigned long long lastUsedFrame = 0;
	};

	struct AvisoPoint
	{
		double longitude = 0.0;
		double latitude = 0.0;
	};

	struct AvisoGroup
	{
		std::string id;
		std::string name;
		std::vector<std::string> colorPalettes;
		bool visible = true;
	};

	struct AvisoFeature
	{
		bool polygon = false;
		int sourceFeatureIndex = -1;
		std::string sourceFeatureId;
		std::vector<std::string> groupIds;
		std::vector<std::string> colorPalettes;
		std::vector<std::vector<AvisoPoint>> paths;
		Gdiplus::Color fillColor = Gdiplus::Color(217, 53, 66, 82);
		Gdiplus::Color strokeColor = Gdiplus::Color(191, 140, 152, 170);
		Gdiplus::Color lightFillColor = Gdiplus::Color(217, 53, 66, 82);
		Gdiplus::Color lightStrokeColor = Gdiplus::Color(191, 140, 152, 170);
		Gdiplus::Color realFillColor = Gdiplus::Color(217, 53, 66, 82);
		Gdiplus::Color realStrokeColor = Gdiplus::Color(191, 140, 152, 170);
		float strokeWidth = 1.0f;
		int minimumZoomLevel = 0;
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
		std::vector<std::string> colorPalettes;
		std::wstring text;
		std::wstring fontFamily = L"Arial";
		std::string labelClass;
		std::string textAnchor = "center";
		Gdiplus::Color textColor = Gdiplus::Color(255, 128, 128, 128);
		Gdiplus::Color haloColor = Gdiplus::Color(255, 0, 0, 0);
		Gdiplus::Color lightTextColor = Gdiplus::Color(255, 128, 128, 128);
		Gdiplus::Color lightHaloColor = Gdiplus::Color(255, 0, 0, 0);
		Gdiplus::Color realTextColor = Gdiplus::Color(255, 128, 128, 128);
		Gdiplus::Color realHaloColor = Gdiplus::Color(255, 0, 0, 0);
		float textSize = 12.0f;
		float haloWidth = 1.0f;
		double maxMetersPerPixel = 0.0;
		int minimumZoomLevel = 0;
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

	// Render requests own immutable snapshots because the raster worker can
	// outlive the frame that queued it. The generation and cancellation token
	// prevent an older result from replacing newer group or viewport state.
	struct AvisoRasterRenderRequest
	{
		unsigned long long requestId = 0;
		std::uint64_t performanceQueuedAtMilliseconds = 0;
		std::uint32_t debounceMilliseconds = 0;
		std::shared_ptr<std::atomic<std::uint64_t>> cancellationToken;
		unsigned long long groupGeneration = 0;
		std::string path;
		std::shared_ptr<const std::vector<AvisoFeature>> features;
		std::shared_ptr<const std::vector<AvisoLabel>> labels;
		std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
		std::string colorPalette = "dark";
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
		int viewportZoomLevel = 0;
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
		std::string colorPalette = "dark";
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

	struct AvisoLoadPerformance
	{
		std::string path;
		double readMilliseconds = 0.0;
		double parseMilliseconds = 0.0;
		double validateMilliseconds = 0.0;
		double convertCommitMilliseconds = 0.0;
		double totalMilliseconds = 0.0;
		bool success = false;
	};

	enum class RuntimeMenuPopup
	{
		None,
		Mode,
		Groups,
		Insets,
		Profile,
		Vsid,
		Datalink
	};

	struct CachedRunwayGeometry
	{
		std::string runwayNameA;
		std::string runwayNameB;
		std::string displayName;
		double trueHeadingA = 0.0;
		double trueHeadingB = 0.0;
		bool trueHeadingAValid = false;
		bool trueHeadingBValid = false;
		std::vector<EuroScopePlugIn::CPosition> rimcasDefinition;
		std::vector<EuroScopePlugIn::CPosition> closedDefinition;
	};

	struct CorrelationSettings
	{
		bool proModeEnabled = false;
		bool acceptPilotSquawk = true;
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
		bool requireReady = false;
		bool towerFilter = false;
		bool structuredRulesEnabled = true;
		int maximumAirborneAltitudeFt = 5500;
		int maximumAirborneSpeedKt = 250;
		DisplayModeStatusVisibility statuses;
	};
}
