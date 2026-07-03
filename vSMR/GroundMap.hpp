#pragma once

#include <GdiPlus.h>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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
		bool hasLocalProjection = false;
		double originLatitude = 0.0;
		double originLongitude = 0.0;
		double latitudeMetersPerDegree = 111320.0;
		double longitudeMetersPerDegree = 111320.0;
		double minX = 0.0;
		double minY = 0.0;
		double maxX = 0.0;
		double maxY = 0.0;
	};

	struct CachedGroundLayer
	{
		std::unique_ptr<Gdiplus::Bitmap> bitmap;
		std::string airport;
		int width = 0;
		int height = 0;
		int zoomLevel = -1;
		int styleRevision = 100;
		double minLatitude = 0.0;
		double maxLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLongitude = 0.0;
		bool valid = false;
	};

	struct GroundTileKey
	{
		std::string airport;
		int zoom = 0;
		int x = 0;
		int y = 0;
		int styleRevision = 100;

		bool operator==(const GroundTileKey& other) const
		{
			return airport == other.airport &&
				zoom == other.zoom &&
				x == other.x &&
				y == other.y &&
				styleRevision == other.styleRevision;
		}

		bool operator<(const GroundTileKey& other) const
		{
			if (airport != other.airport)
				return airport < other.airport;
			if (zoom != other.zoom)
				return zoom < other.zoom;
			if (x != other.x)
				return x < other.x;
			if (y != other.y)
				return y < other.y;
			return styleRevision < other.styleRevision;
		}
	};

	struct GroundTile
	{
		GroundTileKey key;
		std::unique_ptr<Gdiplus::Bitmap> bitmap;
		double minX = 0.0;
		double minY = 0.0;
		double maxX = 0.0;
		double maxY = 0.0;
		double minLatitude = 0.0;
		double maxLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLongitude = 0.0;
		bool ready = false;
		ULONGLONG lastUsed = 0;
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
	std::map<GroundTileKey, GroundTile> GroundTiles;
	std::deque<GroundTileKey> PendingGroundTiles;
	std::mutex StateMutex;
	bool HasLastView = false;
	RECT LastRadarArea = {};
	double LastMinLatitude = 0.0;
	double LastMaxLatitude = 0.0;
	double LastMinLongitude = 0.0;
	double LastMaxLongitude = 0.0;
	ULONGLONG LastPanChangeTick = 0;
	ULONGLONG LastZoomChangeTick = 0;
};
