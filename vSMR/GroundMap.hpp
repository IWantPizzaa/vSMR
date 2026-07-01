#pragma once

#include <GdiPlus.h>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

class CColorManager;
class CSMRRadar;
class CDC;

class CGroundMapRenderer
{
public:
	struct Coordinate
	{
		double latitude = 0.0;
		double longitude = 0.0;
	};

	enum class FeatureKind
	{
		Point,
		Line,
		Polygon
	};

	struct FeatureStyle
	{
		Gdiplus::Color stroke = Gdiplus::Color(210, 180, 190, 195);
		Gdiplus::Color fill = Gdiplus::Color(35, 180, 190, 195);
		Gdiplus::Color marker = Gdiplus::Color(230, 240, 210, 75);
		Gdiplus::Color text = Gdiplus::Color(230, 240, 240, 240);
		float strokeWidth = 1.4f;
		float markerSize = 6.0f;
		bool strokeEnabled = true;
		bool fillEnabled = false;
		bool markerEnabled = true;
		bool textEnabled = false;
		bool arrowEnabled = false;
	};

	struct Feature
	{
		FeatureKind kind = FeatureKind::Line;
		std::vector<std::vector<Coordinate>> paths;
		std::array<std::vector<std::vector<Coordinate>>, 3> lodPaths;
		std::array<std::unique_ptr<Gdiplus::GraphicsPath>, 4> geoPaths;
		std::string label;
		std::string layer;
		FeatureStyle style;
		int zIndex = 0;
		int minZoom = 0;
		int maxZoom = 14;
		bool visible = true;
		bool hasHeading = false;
		double heading = 0.0;
		bool hasBounds = false;
		double minLatitude = 0.0;
		double maxLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLongitude = 0.0;
		bool hasLabelAnchor = false;
		Coordinate labelAnchor;
	};

	struct CacheEntry
	{
		bool attempted = false;
		bool available = false;
		std::string path;
		std::vector<Feature> features;
		std::vector<std::size_t> polygonIndices;
		std::vector<std::size_t> lineIndices;
		std::vector<std::size_t> pointIndices;
		std::vector<std::size_t> textIndices;
	};

	struct CachedGroundLayer
	{
		std::unique_ptr<Gdiplus::Bitmap> bitmap;
		std::string airport;
		int width = 0;
		int height = 0;
		int zoomLevel = -1;
		double minLatitude = 0.0;
		double maxLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLongitude = 0.0;
		bool valid = false;
	};

	void ClearCache();
	bool RenderAirportMap(
		const std::string& airport,
		const std::string& dllPath,
		CSMRRadar& radar,
		Gdiplus::Graphics& graphics,
		CDC& dc,
		const RECT& radarArea,
		CColorManager* colorManager);

private:
	std::map<std::string, CacheEntry> AirportMaps;
	CachedGroundLayer CachedLayer;
	bool HasLastView = false;
	RECT LastRadarArea = {};
	double LastMinLatitude = 0.0;
	double LastMaxLatitude = 0.0;
	double LastMinLongitude = 0.0;
	double LastMaxLongitude = 0.0;
	ULONGLONG LastViewChangeTick = 0;
};
