#include "stdafx.h"
#include "GroundMap.hpp"

#include "ColorManager.h"
#include "Logger.h"
#include "SMRRadar.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "rapidjson/document.h"

using namespace Gdiplus;
using namespace rapidjson;

namespace
{
	constexpr int kDefaultMinZoom = 0;
	constexpr int kDefaultMaxZoom = 14;

	BYTE ColorRed(Color color)
	{
		return GetRValue(color.ToCOLORREF());
	}

	BYTE ColorGreen(Color color)
	{
		return GetGValue(color.ToCOLORREF());
	}

	BYTE ColorBlue(Color color)
	{
		return GetBValue(color.ToCOLORREF());
	}

	std::string TrimAsciiWhitespace(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;

		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;

		return text.substr(start, end - start);
	}

	std::string ToLowerCopy(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return text;
	}

	std::string ToUpperCopy(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return text;
	}

	bool EqualsNoCase(const std::string& left, const char* right)
	{
		if (right == nullptr)
			return false;
		return ToLowerCopy(left) == ToLowerCopy(right);
	}

	const Value* FindProperty(const Value* properties, std::initializer_list<const char*> names)
	{
		if (properties == nullptr || !properties->IsObject())
			return nullptr;

		for (const char* name : names)
		{
			if (name != nullptr && properties->HasMember(name))
				return &(*properties)[name];
		}

		for (auto it = properties->MemberBegin(); it != properties->MemberEnd(); ++it)
		{
			if (!it->name.IsString())
				continue;

			const std::string memberName = it->name.GetString();
			for (const char* name : names)
			{
				if (EqualsNoCase(memberName, name))
					return &it->value;
			}
		}

		return nullptr;
	}

	std::string ReadStringProperty(const Value* properties, std::initializer_list<const char*> names, const std::string& fallback = "")
	{
		const Value* value = FindProperty(properties, names);
		if (value == nullptr)
			return fallback;
		if (value->IsString())
			return value->GetString();
		if (value->IsInt())
			return std::to_string(value->GetInt());
		if (value->IsUint())
			return std::to_string(value->GetUint());
		if (value->IsDouble())
		{
			std::ostringstream ss;
			ss << value->GetDouble();
			return ss.str();
		}
		return fallback;
	}

	bool ReadBoolProperty(const Value* properties, std::initializer_list<const char*> names, bool fallback)
	{
		const Value* value = FindProperty(properties, names);
		if (value == nullptr)
			return fallback;
		if (value->IsBool())
			return value->GetBool();
		if (value->IsInt())
			return value->GetInt() != 0;
		if (value->IsString())
		{
			const std::string text = ToLowerCopy(TrimAsciiWhitespace(value->GetString()));
			if (text == "true" || text == "yes" || text == "1" || text == "on")
				return true;
			if (text == "false" || text == "no" || text == "0" || text == "off")
				return false;
		}
		return fallback;
	}

	double ReadDoubleProperty(const Value* properties, std::initializer_list<const char*> names, double fallback)
	{
		const Value* value = FindProperty(properties, names);
		if (value == nullptr)
			return fallback;
		if (value->IsNumber())
			return value->GetDouble();
		if (value->IsString())
		{
			char* end = nullptr;
			const double parsed = std::strtod(value->GetString(), &end);
			if (end != value->GetString())
				return parsed;
		}
		return fallback;
	}

	int ReadIntProperty(const Value* properties, std::initializer_list<const char*> names, int fallback)
	{
		return static_cast<int>(std::round(ReadDoubleProperty(properties, names, static_cast<double>(fallback))));
	}

	bool ParseHexByte(const std::string& text, size_t offset, BYTE& out)
	{
		if (offset + 2 > text.size())
			return false;

		const std::string token = text.substr(offset, 2);
		char* end = nullptr;
		const long value = std::strtol(token.c_str(), &end, 16);
		if (end == token.c_str() || value < 0 || value > 255)
			return false;

		out = static_cast<BYTE>(value);
		return true;
	}

	bool TryParseColorString(const std::string& rawText, Color fallback, Color& outColor)
	{
		const std::string text = ToLowerCopy(TrimAsciiWhitespace(rawText));
		if (text.empty())
			return false;

		if (text[0] == '#')
		{
			if (text.size() == 4)
			{
				auto expand = [](char c) -> BYTE {
					char token[3] = { c, c, '\0' };
					return static_cast<BYTE>(std::strtol(token, nullptr, 16));
				};
				outColor = Color(fallback.GetAlpha(), expand(text[1]), expand(text[2]), expand(text[3]));
				return true;
			}

			if (text.size() == 7 || text.size() == 9)
			{
				BYTE r = 0;
				BYTE g = 0;
				BYTE b = 0;
				BYTE a = fallback.GetAlpha();
				if (!ParseHexByte(text, 1, r) || !ParseHexByte(text, 3, g) || !ParseHexByte(text, 5, b))
					return false;
				if (text.size() == 9 && !ParseHexByte(text, 7, a))
					return false;
				outColor = Color(a, r, g, b);
				return true;
			}
		}

		if (text == "white")
			outColor = Color(fallback.GetAlpha(), 255, 255, 255);
		else if (text == "black")
			outColor = Color(fallback.GetAlpha(), 0, 0, 0);
		else if (text == "red")
			outColor = Color(fallback.GetAlpha(), 220, 65, 65);
		else if (text == "orange")
			outColor = Color(fallback.GetAlpha(), 245, 155, 45);
		else if (text == "yellow")
			outColor = Color(fallback.GetAlpha(), 240, 210, 75);
		else if (text == "green")
			outColor = Color(fallback.GetAlpha(), 80, 210, 120);
		else if (text == "blue")
			outColor = Color(fallback.GetAlpha(), 85, 160, 240);
		else if (text == "cyan")
			outColor = Color(fallback.GetAlpha(), 80, 220, 230);
		else if (text == "magenta" || text == "purple")
			outColor = Color(fallback.GetAlpha(), 200, 100, 220);
		else if (text == "grey" || text == "gray")
			outColor = Color(fallback.GetAlpha(), 170, 170, 170);
		else
			return false;

		return true;
	}

	BYTE NormalizeAlpha(double value, BYTE fallback)
	{
		if (!std::isfinite(value))
			return fallback;
		if (value <= 1.0)
			return static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::round(value * 255.0)), 0, 255));
		return static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::round(value)), 0, 255));
	}

	Color WithAlpha(Color color, BYTE alpha)
	{
		return Color(alpha, ColorRed(color), ColorGreen(color), ColorBlue(color));
	}

	bool TryReadColorValue(const Value& value, Color fallback, Color& outColor)
	{
		if (value.IsString())
			return TryParseColorString(value.GetString(), fallback, outColor);

		if (value.IsArray() && value.Size() >= 3)
		{
			const int r = value[static_cast<SizeType>(0)].IsNumber() ? std::clamp<int>(static_cast<int>(std::round(value[static_cast<SizeType>(0)].GetDouble())), 0, 255) : ColorRed(fallback);
			const int g = value[static_cast<SizeType>(1)].IsNumber() ? std::clamp<int>(static_cast<int>(std::round(value[static_cast<SizeType>(1)].GetDouble())), 0, 255) : ColorGreen(fallback);
			const int b = value[static_cast<SizeType>(2)].IsNumber() ? std::clamp<int>(static_cast<int>(std::round(value[static_cast<SizeType>(2)].GetDouble())), 0, 255) : ColorBlue(fallback);
			BYTE a = fallback.GetAlpha();
			if (value.Size() >= 4 && value[static_cast<SizeType>(3)].IsNumber())
				a = NormalizeAlpha(value[static_cast<SizeType>(3)].GetDouble(), fallback.GetAlpha());

			outColor = Color(a, r, g, b);
			return true;
		}

		if (value.IsObject())
		{
			auto readComponent = [&](std::initializer_list<const char*> names, int fallbackValue) -> int {
				const Value* component = FindProperty(&value, names);
				if (component != nullptr && component->IsNumber())
					return std::clamp<int>(static_cast<int>(std::round(component->GetDouble())), 0, 255);
				return fallbackValue;
			};

			int r = readComponent({ "r", "red" }, ColorRed(fallback));
			int g = readComponent({ "g", "green" }, ColorGreen(fallback));
			int b = readComponent({ "b", "blue" }, ColorBlue(fallback));
			BYTE a = fallback.GetAlpha();
			const Value* alpha = FindProperty(&value, { "a", "alpha", "opacity" });
			if (alpha != nullptr && alpha->IsNumber())
				a = NormalizeAlpha(alpha->GetDouble(), fallback.GetAlpha());

			outColor = Color(a, r, g, b);
			return true;
		}

		return false;
	}

	Color ReadColorProperty(const Value* properties, std::initializer_list<const char*> names, Color fallback)
	{
		const Value* value = FindProperty(properties, names);
		Color parsed;
		if (value != nullptr && TryReadColorValue(*value, fallback, parsed))
			return parsed;
		return fallback;
	}

	void ApplyOpacityProperty(const Value* properties, std::initializer_list<const char*> names, Color& color)
	{
		const Value* value = FindProperty(properties, names);
		if (value == nullptr)
			return;
		if (value->IsNumber())
			color = WithAlpha(color, NormalizeAlpha(value->GetDouble(), color.GetAlpha()));
	}

	bool ReadCoordinate(const Value& coordinate, CGroundMapRenderer::Coordinate& outCoordinate)
	{
		if (!coordinate.IsArray() ||
			coordinate.Size() < 2 ||
			!coordinate[static_cast<SizeType>(0)].IsNumber() ||
			!coordinate[static_cast<SizeType>(1)].IsNumber())
			return false;

		const double longitude = coordinate[static_cast<SizeType>(0)].GetDouble();
		const double latitude = coordinate[static_cast<SizeType>(1)].GetDouble();
		if (!std::isfinite(latitude) || !std::isfinite(longitude))
			return false;
		if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0)
			return false;

		outCoordinate.latitude = latitude;
		outCoordinate.longitude = longitude;
		return true;
	}

	double CoordinateDistanceSquared(const CGroundMapRenderer::Coordinate& left, const CGroundMapRenderer::Coordinate& right)
	{
		const double latitudeDelta = left.latitude - right.latitude;
		const double longitudeDelta = left.longitude - right.longitude;
		return latitudeDelta * latitudeDelta + longitudeDelta * longitudeDelta;
	}

	std::vector<CGroundMapRenderer::Coordinate> SimplifyCoordinatePath(
		const std::vector<CGroundMapRenderer::Coordinate>& sourcePath,
		double toleranceDegrees,
		bool closed,
		size_t minimumPoints)
	{
		if (sourcePath.size() <= minimumPoints || toleranceDegrees <= 0.0)
			return sourcePath;

		const double toleranceSquared = toleranceDegrees * toleranceDegrees;
		size_t end = sourcePath.size();
		if (closed && sourcePath.size() > 1 && CoordinateDistanceSquared(sourcePath.front(), sourcePath.back()) < 0.000000000001)
			--end;

		std::vector<CGroundMapRenderer::Coordinate> simplified;
		simplified.reserve(sourcePath.size());
		simplified.push_back(sourcePath.front());

		for (size_t i = 1; i < end; ++i)
		{
			const CGroundMapRenderer::Coordinate& coordinate = sourcePath[i];
			if (CoordinateDistanceSquared(coordinate, simplified.back()) >= toleranceSquared)
				simplified.push_back(coordinate);
		}

		if (!closed && CoordinateDistanceSquared(sourcePath.back(), simplified.back()) > 0.000000000001)
			simplified.push_back(sourcePath.back());

		if (simplified.size() < minimumPoints)
			return sourcePath;
		return simplified;
	}

	std::vector<std::vector<CGroundMapRenderer::Coordinate>> BuildLodPaths(
		const std::vector<std::vector<CGroundMapRenderer::Coordinate>>& sourcePaths,
		CGroundMapRenderer::FeatureKind kind,
		double toleranceDegrees)
	{
		if (kind == CGroundMapRenderer::FeatureKind::Point)
			return sourcePaths;

		const bool closed = kind == CGroundMapRenderer::FeatureKind::Polygon;
		const size_t minimumPoints = closed ? 3 : 2;
		std::vector<std::vector<CGroundMapRenderer::Coordinate>> lodPaths;
		lodPaths.reserve(sourcePaths.size());
		for (const auto& path : sourcePaths)
		{
			std::vector<CGroundMapRenderer::Coordinate> simplified = SimplifyCoordinatePath(path, toleranceDegrees, closed, minimumPoints);
			if (simplified.size() >= minimumPoints)
				lodPaths.push_back(std::move(simplified));
		}
		return lodPaths;
	}

	std::unique_ptr<GraphicsPath> BuildGeoPath(const std::vector<std::vector<CGroundMapRenderer::Coordinate>>& paths)
	{
		std::unique_ptr<GraphicsPath> geoPath = std::make_unique<GraphicsPath>(FillModeAlternate);
		std::vector<PointF> points;

		for (const auto& path : paths)
		{
			if (path.size() < 3)
				continue;

			points.clear();
			points.reserve(path.size());
			for (const auto& coordinate : path)
			{
				points.emplace_back(
					static_cast<REAL>(coordinate.longitude),
					static_cast<REAL>(coordinate.latitude));
			}

			if (points.size() >= 3)
				geoPath->AddPolygon(points.data(), static_cast<INT>(points.size()));
		}

		return geoPath->GetPointCount() > 0 ? std::move(geoPath) : nullptr;
	}

	std::vector<CGroundMapRenderer::Coordinate> ReadCoordinatePath(const Value& coordinates)
	{
		std::vector<CGroundMapRenderer::Coordinate> path;
		if (!coordinates.IsArray())
			return path;

		path.reserve(coordinates.Size());
		for (SizeType i = 0; i < coordinates.Size(); ++i)
		{
			CGroundMapRenderer::Coordinate coordinate;
			if (ReadCoordinate(coordinates[i], coordinate))
				path.push_back(coordinate);
		}
		return path;
	}

	bool ContainsToken(const std::string& haystack, const char* needle)
	{
		if (needle == nullptr || needle[0] == '\0')
			return false;
		return haystack.find(needle) != std::string::npos;
	}

	CGroundMapRenderer::FeatureStyle BuildStyle(
		CGroundMapRenderer::FeatureKind kind,
		const Value* properties,
		const std::string& label,
		const std::string& layer)
	{
		CGroundMapRenderer::FeatureStyle style;
		const std::string markerSymbol = ReadStringProperty(properties, { "marker-symbol", "marker_symbol", "symbol", "icon", "shape" });
		const std::string styleTokens = ToLowerCopy(layer + " " + label + " " + markerSymbol + " " +
			ReadStringProperty(properties, { "type", "class", "category", "kind" }));

		if (kind == CGroundMapRenderer::FeatureKind::Polygon)
		{
			style.fillEnabled = true;
			style.markerEnabled = false;
			style.fill = Color(35, 180, 190, 195);
		}
		else if (kind == CGroundMapRenderer::FeatureKind::Line)
		{
			style.fillEnabled = false;
			style.markerEnabled = false;
			style.stroke = Color(210, 180, 190, 195);
		}
		else
		{
			style.fillEnabled = false;
			style.strokeEnabled = true;
			style.markerEnabled = true;
		}

		if (ContainsToken(styleTokens, "aviso"))
		{
			style.stroke = Color(235, 245, 210, 70);
			style.marker = Color(235, 245, 210, 70);
			style.text = Color(235, 245, 235, 160);
			style.markerSize = 7.0f;
		}
		else if (ContainsToken(styleTokens, "rrsm"))
		{
			style.stroke = Color(220, 75, 210, 125);
			style.marker = Color(220, 75, 210, 125);
			style.strokeWidth = 1.8f;
		}
		else if (ContainsToken(styleTokens, "sensitive"))
		{
			style.stroke = Color(225, 245, 85, 85);
			style.fill = Color(45, 245, 85, 85);
			style.marker = Color(235, 245, 85, 85);
			style.fillEnabled = kind == CGroundMapRenderer::FeatureKind::Polygon;
		}
		else if (ContainsToken(styleTokens, "hotspot") || ContainsToken(styleTokens, "hot spot"))
		{
			style.stroke = Color(235, 245, 155, 45);
			style.fill = Color(45, 245, 155, 45);
			style.marker = Color(240, 245, 155, 45);
			style.markerSize = 8.0f;
			style.fillEnabled = kind == CGroundMapRenderer::FeatureKind::Polygon;
		}
		else if (ContainsToken(styleTokens, "cat iii") || ContainsToken(styleTokens, "cat_iii") || ContainsToken(styleTokens, "catiii"))
		{
			style.stroke = Color(220, 90, 210, 235);
			style.marker = Color(220, 90, 210, 235);
			style.strokeWidth = 1.6f;
		}

		style.arrowEnabled = ReadBoolProperty(properties, { "arrow", "has_arrow", "draw_arrow" },
			ContainsToken(styleTokens, "arrow"));

		style.stroke = ReadColorProperty(properties,
			{ "stroke", "stroke-color", "stroke_color", "strokeColor", "line-color", "line_color", "lineColor", "color" },
			style.stroke);
		style.fill = ReadColorProperty(properties,
			{ "fill", "fill-color", "fill_color", "fillColor", "area-color", "area_color", "areaColor" },
			style.fill);
		style.marker = ReadColorProperty(properties,
			{ "marker-color", "marker_color", "markerColor", "marker", "point-color", "point_color", "pointColor", "color" },
			style.marker);
		style.text = ReadColorProperty(properties,
			{ "text-color", "text_color", "textColor", "label-color", "label_color", "labelColor" },
			style.text);

		ApplyOpacityProperty(properties, { "opacity" }, style.stroke);
		ApplyOpacityProperty(properties, { "stroke-opacity", "stroke_opacity", "strokeOpacity" }, style.stroke);
		ApplyOpacityProperty(properties, { "fill-opacity", "fill_opacity", "fillOpacity" }, style.fill);
		ApplyOpacityProperty(properties, { "marker-opacity", "marker_opacity", "markerOpacity" }, style.marker);
		ApplyOpacityProperty(properties, { "text-opacity", "text_opacity", "textOpacity", "label-opacity", "label_opacity", "labelOpacity" }, style.text);

		style.strokeWidth = static_cast<float>(std::clamp(ReadDoubleProperty(properties,
			{ "stroke-width", "stroke_width", "strokeWidth", "line-width", "line_width", "lineWidth", "width" },
			style.strokeWidth), 0.5, 12.0));

		const std::string sizeText = ToLowerCopy(ReadStringProperty(properties, { "marker-size", "marker_size", "markerSize", "size" }));
		if (sizeText == "small")
			style.markerSize = 5.0f;
		else if (sizeText == "large")
			style.markerSize = 10.0f;
		else if (!sizeText.empty())
			style.markerSize = static_cast<float>(std::clamp(ReadDoubleProperty(properties,
				{ "marker-size", "marker_size", "markerSize", "size" }, style.markerSize), 3.0, 16.0));

		style.strokeEnabled = ReadBoolProperty(properties, { "stroke-visible", "stroke_visible", "strokeVisible", "draw_stroke" }, style.strokeEnabled);
		style.fillEnabled = ReadBoolProperty(properties, { "fill-visible", "fill_visible", "fillVisible", "draw_fill" }, style.fillEnabled);
		style.markerEnabled = ReadBoolProperty(properties, { "marker-visible", "marker_visible", "markerVisible", "draw_marker" }, style.markerEnabled);
		style.textEnabled = ReadBoolProperty(properties, { "label-visible", "label_visible", "labelVisible", "text-visible", "text_visible", "textVisible" },
			!label.empty() && (kind == CGroundMapRenderer::FeatureKind::Point || ContainsToken(styleTokens, "text")));

		return style;
	}

	CGroundMapRenderer::Feature BuildFeature(
		CGroundMapRenderer::FeatureKind kind,
		const Value* properties,
		std::vector<std::vector<CGroundMapRenderer::Coordinate>> paths)
	{
		CGroundMapRenderer::Feature feature;
		feature.kind = kind;
		feature.paths = std::move(paths);
		feature.label = ReadStringProperty(properties, { "label", "text", "title", "name", "id" });
		feature.layer = ReadStringProperty(properties, { "layer", "category", "type", "class", "group", "kind" });
		feature.visible = ReadBoolProperty(properties, { "visible", "enabled", "show" }, true);
		feature.zIndex = ReadIntProperty(properties, { "z", "z_index", "zIndex", "order", "priority" }, 0);
		feature.minZoom = std::clamp(ReadIntProperty(properties, { "minzoom", "minZoom", "min_zoom" }, kDefaultMinZoom), kDefaultMinZoom, kDefaultMaxZoom);
		feature.maxZoom = std::clamp(ReadIntProperty(properties, { "maxzoom", "maxZoom", "max_zoom" }, kDefaultMaxZoom), kDefaultMinZoom, kDefaultMaxZoom);
		feature.hasHeading = FindProperty(properties, { "heading", "bearing", "direction" }) != nullptr;
		feature.heading = ReadDoubleProperty(properties, { "heading", "bearing", "direction" }, 0.0);
		feature.style = BuildStyle(kind, properties, feature.label, feature.layer);
		if (feature.hasHeading)
			feature.style.arrowEnabled = true;
		return feature;
	}

	void AddFeatureIfValid(std::vector<CGroundMapRenderer::Feature>& features, CGroundMapRenderer::Feature feature)
	{
		feature.paths.erase(
			std::remove_if(feature.paths.begin(), feature.paths.end(), [](const std::vector<CGroundMapRenderer::Coordinate>& path) {
				return path.empty();
			}),
			feature.paths.end());

		if (!feature.visible || feature.paths.empty())
			return;

		feature.minLatitude = (std::numeric_limits<double>::max)();
		feature.maxLatitude = std::numeric_limits<double>::lowest();
		feature.minLongitude = (std::numeric_limits<double>::max)();
		feature.maxLongitude = std::numeric_limits<double>::lowest();
		for (const auto& path : feature.paths)
		{
			for (const auto& coordinate : path)
			{
				feature.minLatitude = (std::min)(feature.minLatitude, coordinate.latitude);
				feature.maxLatitude = (std::max)(feature.maxLatitude, coordinate.latitude);
				feature.minLongitude = (std::min)(feature.minLongitude, coordinate.longitude);
				feature.maxLongitude = (std::max)(feature.maxLongitude, coordinate.longitude);
				feature.hasBounds = true;
			}
		}

		if (feature.hasBounds)
		{
			feature.labelAnchor.latitude = (feature.minLatitude + feature.maxLatitude) * 0.5;
			feature.labelAnchor.longitude = (feature.minLongitude + feature.maxLongitude) * 0.5;
			feature.hasLabelAnchor = true;
		}

		feature.lodPaths[0] = BuildLodPaths(feature.paths, feature.kind, 0.000055);
		feature.lodPaths[1] = BuildLodPaths(feature.paths, feature.kind, 0.000022);
		feature.lodPaths[2] = BuildLodPaths(feature.paths, feature.kind, 0.000008);
		if (feature.kind == CGroundMapRenderer::FeatureKind::Polygon)
		{
			feature.geoPaths[0] = BuildGeoPath(feature.lodPaths[0]);
			feature.geoPaths[1] = BuildGeoPath(feature.lodPaths[1]);
			feature.geoPaths[2] = BuildGeoPath(feature.lodPaths[2]);
			feature.geoPaths[3] = BuildGeoPath(feature.paths);
		}

		features.push_back(std::move(feature));
	}

	void AppendGeometry(const Value& geometry, const Value* properties, std::vector<CGroundMapRenderer::Feature>& features)
	{
		if (!geometry.IsObject() || !geometry.HasMember("type") || !geometry["type"].IsString())
			return;

		const std::string type = geometry["type"].GetString();
		if (type == "GeometryCollection")
		{
			if (!geometry.HasMember("geometries") || !geometry["geometries"].IsArray())
				return;

			const Value& geometries = geometry["geometries"];
			for (SizeType i = 0; i < geometries.Size(); ++i)
				AppendGeometry(geometries[i], properties, features);
			return;
		}

		if (!geometry.HasMember("coordinates"))
			return;
		const Value& coordinates = geometry["coordinates"];

		if (type == "Point")
		{
			CGroundMapRenderer::Coordinate coordinate;
			if (ReadCoordinate(coordinates, coordinate))
				AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Point, properties, { { coordinate } }));
		}
		else if (type == "MultiPoint")
		{
			std::vector<CGroundMapRenderer::Coordinate> path = ReadCoordinatePath(coordinates);
			AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Point, properties, { path }));
		}
		else if (type == "LineString")
		{
			std::vector<CGroundMapRenderer::Coordinate> path = ReadCoordinatePath(coordinates);
			if (path.size() >= 2)
				AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Line, properties, { path }));
		}
		else if (type == "MultiLineString")
		{
			std::vector<std::vector<CGroundMapRenderer::Coordinate>> paths;
			if (coordinates.IsArray())
			{
				for (SizeType i = 0; i < coordinates.Size(); ++i)
				{
					std::vector<CGroundMapRenderer::Coordinate> path = ReadCoordinatePath(coordinates[i]);
					if (path.size() >= 2)
						paths.push_back(std::move(path));
				}
			}
			AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Line, properties, std::move(paths)));
		}
		else if (type == "Polygon")
		{
			std::vector<std::vector<CGroundMapRenderer::Coordinate>> rings;
			if (coordinates.IsArray())
			{
				for (SizeType i = 0; i < coordinates.Size(); ++i)
				{
					std::vector<CGroundMapRenderer::Coordinate> ring = ReadCoordinatePath(coordinates[i]);
					if (ring.size() >= 3)
						rings.push_back(std::move(ring));
				}
			}
			AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Polygon, properties, std::move(rings)));
		}
		else if (type == "MultiPolygon")
		{
			if (!coordinates.IsArray())
				return;

			for (SizeType polygonIndex = 0; polygonIndex < coordinates.Size(); ++polygonIndex)
			{
				const Value& polygon = coordinates[polygonIndex];
				if (!polygon.IsArray())
					continue;

				std::vector<std::vector<CGroundMapRenderer::Coordinate>> rings;
				for (SizeType ringIndex = 0; ringIndex < polygon.Size(); ++ringIndex)
				{
					std::vector<CGroundMapRenderer::Coordinate> ring = ReadCoordinatePath(polygon[ringIndex]);
					if (ring.size() >= 3)
						rings.push_back(std::move(ring));
				}
				AddFeatureIfValid(features, BuildFeature(CGroundMapRenderer::FeatureKind::Polygon, properties, std::move(rings)));
			}
		}
	}

	bool LoadGeoJson(const std::string& path, std::vector<CGroundMapRenderer::Feature>& features, std::string& error)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			error = "file not found";
			return false;
		}

		std::stringstream buffer;
		buffer << input.rdbuf();
		const std::string content = buffer.str();

		Document document;
		if (document.Parse<0>(content.c_str()).HasParseError())
		{
			error = "parse error";
			return false;
		}

		features.clear();
		if (document.IsObject() && document.HasMember("type") && document["type"].IsString() &&
			std::string(document["type"].GetString()) == "FeatureCollection")
		{
			if (!document.HasMember("features") || !document["features"].IsArray())
			{
				error = "FeatureCollection has no features array";
				return false;
			}

			const Value& geoFeatures = document["features"];
			for (SizeType i = 0; i < geoFeatures.Size(); ++i)
			{
				const Value& geoFeature = geoFeatures[i];
				if (!geoFeature.IsObject() || !geoFeature.HasMember("geometry"))
					continue;

				const Value* properties = nullptr;
				if (geoFeature.HasMember("properties") && geoFeature["properties"].IsObject())
					properties = &geoFeature["properties"];
				AppendGeometry(geoFeature["geometry"], properties, features);
			}
		}
		else if (document.IsObject() && document.HasMember("type") && document["type"].IsString())
		{
			AppendGeometry(document, nullptr, features);
		}
		else
		{
			error = "unsupported GeoJSON root";
			return false;
		}

		std::stable_sort(features.begin(), features.end(), [](const CGroundMapRenderer::Feature& left, const CGroundMapRenderer::Feature& right) {
			if (left.zIndex != right.zIndex)
				return left.zIndex < right.zIndex;
			return static_cast<int>(left.kind) > static_cast<int>(right.kind);
		});

		return true;
	}

	void RebuildFeatureIndices(CGroundMapRenderer::CacheEntry& entry)
	{
		entry.polygonIndices.clear();
		entry.lineIndices.clear();
		entry.pointIndices.clear();
		entry.textIndices.clear();

		entry.polygonIndices.reserve(entry.features.size());
		entry.lineIndices.reserve(entry.features.size());
		entry.pointIndices.reserve(entry.features.size());
		entry.textIndices.reserve(entry.features.size());

		for (std::size_t i = 0; i < entry.features.size(); ++i)
		{
			const CGroundMapRenderer::Feature& feature = entry.features[i];
			if (feature.kind == CGroundMapRenderer::FeatureKind::Polygon)
				entry.polygonIndices.push_back(i);
			else if (feature.kind == CGroundMapRenderer::FeatureKind::Line)
				entry.lineIndices.push_back(i);
			else
				entry.pointIndices.push_back(i);

			if (feature.style.textEnabled && !feature.label.empty())
				entry.textIndices.push_back(i);
		}
	}

	bool IsAirportEnabled(const std::string& airport)
	{
		return airport == "LFPG";
	}

	std::vector<std::filesystem::path> BuildAirportGeoJsonPaths(const std::string& airport, const std::string& dllPath)
	{
		const std::string filename = "AVISO_" + airport + ".geojson";
		const std::filesystem::path dllDirectory(dllPath);
		const std::filesystem::path rootDirectory = dllDirectory.has_parent_path() ? dllDirectory.parent_path() : std::filesystem::path(".");

		return {
			dllDirectory / filename,
			dllDirectory / "GroundMaps" / filename,
			rootDirectory / filename,
			rootDirectory / "GroundMaps" / filename,
			rootDirectory / "vSMR" / filename,
			rootDirectory / "vSMR" / "GroundMaps" / filename
		};
	}

	bool EnsureAirportLoaded(
		std::map<std::string, CGroundMapRenderer::CacheEntry>& cache,
		const std::string& airport,
		const std::string& dllPath)
	{
		if (!IsAirportEnabled(airport))
			return false;

		CGroundMapRenderer::CacheEntry& entry = cache[airport];
		if (entry.attempted)
			return entry.available;

		entry.attempted = true;
		const std::vector<std::filesystem::path> geoJsonPaths = BuildAirportGeoJsonPaths(airport, dllPath);
		std::filesystem::path geoJsonPath;
		std::error_code existsError;
		for (const std::filesystem::path& candidatePath : geoJsonPaths)
		{
			if (std::filesystem::exists(candidatePath, existsError))
			{
				geoJsonPath = candidatePath;
				break;
			}
		}

		if (geoJsonPath.empty())
		{
			if (!geoJsonPaths.empty())
				entry.path = geoJsonPaths.front().string();
			Logger::info("GroundMap: " + entry.path + " not found; using sector-file map rendering");
			return false;
		}

		entry.path = geoJsonPath.string();

		std::string error;
		entry.available = LoadGeoJson(entry.path, entry.features, error);
		if (entry.available)
		{
			RebuildFeatureIndices(entry);
			Logger::info("GroundMap: loaded " + std::to_string(entry.features.size()) + " features from " + entry.path);
		}
		else
		{
			Logger::info("GroundMap: failed to load " + entry.path + " (" + error + ")");
			entry.features.clear();
			entry.polygonIndices.clear();
			entry.lineIndices.clear();
			entry.pointIndices.clear();
			entry.textIndices.clear();
		}

		return entry.available;
	}

	bool PointIsNearRadarArea(const POINT& point, const RECT& radarArea, int margin)
	{
		return point.x >= radarArea.left - margin &&
			point.x <= radarArea.right + margin &&
			point.y >= radarArea.top - margin &&
			point.y <= radarArea.bottom + margin;
	}

	double SquaredDistance(const PointF& left, const PointF& right)
	{
		const double dx = left.X - right.X;
		const double dy = left.Y - right.Y;
		return dx * dx + dy * dy;
	}

	double SimplificationStepPixels(int zoomLevel)
	{
		if (zoomLevel <= 4)
			return 2.0;
		if (zoomLevel <= 7)
			return 1.25;
		if (zoomLevel <= 10)
			return 0.7;
		if (zoomLevel <= 12)
			return 0.35;
		return 0.0;
	}

	double MinimumFeatureExtentPixels(int zoomLevel, CGroundMapRenderer::FeatureKind kind)
	{
		if (kind == CGroundMapRenderer::FeatureKind::Point)
			return 0.0;
		if (zoomLevel <= 5)
			return 2.5;
		if (zoomLevel <= 8)
			return 1.5;
		if (zoomLevel <= 10)
			return 0.75;
		return 0.0;
	}

	struct RenderContext
	{
		RECT radarArea = {};
		CPosition displaySW;
		CPosition displayNE;
		double minLatitude = 0.0;
		double maxLatitude = 0.0;
		double minLongitude = 0.0;
		double maxLongitude = 0.0;
		double xScale = 0.0;
		double yScale = 0.0;
		double latitudeMargin = 0.0;
		double longitudeMargin = 0.0;
		double simplificationStepSquared = 0.0;
		double minimumFeatureExtent = 0.0;
		int zoomLevel = 0;
		bool interactive = false;
		bool valid = false;
		std::vector<PointF> scratchPoints;

		RenderContext(const RECT& area, const CPosition& sw, const CPosition& ne, int zoom, bool moving)
		{
			radarArea = area;
			displaySW = sw;
			displayNE = ne;
			zoomLevel = zoom;
			interactive = moving;

			const double width = static_cast<double>(radarArea.right - radarArea.left);
			const double height = static_cast<double>(radarArea.bottom - radarArea.top);
			minLatitude = (std::min)(displaySW.m_Latitude, displayNE.m_Latitude);
			maxLatitude = (std::max)(displaySW.m_Latitude, displayNE.m_Latitude);
			minLongitude = (std::min)(displaySW.m_Longitude, displayNE.m_Longitude);
			maxLongitude = (std::max)(displaySW.m_Longitude, displayNE.m_Longitude);

			const double latitudeSpan = maxLatitude - minLatitude;
			const double longitudeSpan = maxLongitude - minLongitude;
			valid = width > 0.0 && height > 0.0 && latitudeSpan > 0.0000001 && longitudeSpan > 0.0000001;
			if (!valid)
				return;

			xScale = width / longitudeSpan;
			yScale = height / latitudeSpan;
			longitudeMargin = 24.0 / xScale;
			latitudeMargin = 24.0 / yScale;

			double simplificationStep = SimplificationStepPixels(zoomLevel);
			if (interactive)
				simplificationStep = (std::max)(simplificationStep, zoomLevel <= 10 ? 2.5 : 1.25);
			simplificationStepSquared = simplificationStep * simplificationStep;
			minimumFeatureExtent = MinimumFeatureExtentPixels(zoomLevel, CGroundMapRenderer::FeatureKind::Polygon);
		}

		PointF ToPointF(const CGroundMapRenderer::Coordinate& coordinate) const
		{
			const double x = static_cast<double>(radarArea.left) + ((coordinate.longitude - minLongitude) * xScale);
			const double y = static_cast<double>(radarArea.bottom) - ((coordinate.latitude - minLatitude) * yScale);
			return PointF(static_cast<REAL>(x), static_cast<REAL>(y));
		}

		POINT ToPoint(const CGroundMapRenderer::Coordinate& coordinate) const
		{
			const PointF point = ToPointF(coordinate);
			return POINT{ static_cast<LONG>(std::round(point.X)), static_cast<LONG>(std::round(point.Y)) };
		}

		bool IntersectsView(const CGroundMapRenderer::Feature& feature) const
		{
			if (!feature.hasBounds)
				return true;
			return feature.maxLatitude >= minLatitude - latitudeMargin &&
				feature.minLatitude <= maxLatitude + latitudeMargin &&
				feature.maxLongitude >= minLongitude - longitudeMargin &&
				feature.minLongitude <= maxLongitude + longitudeMargin;
		}

		bool IsLargeEnough(const CGroundMapRenderer::Feature& feature) const
		{
			const double threshold = MinimumFeatureExtentPixels(zoomLevel, feature.kind);
			if (threshold <= 0.0 || !feature.hasBounds)
				return true;

			const double pixelWidth = (feature.maxLongitude - feature.minLongitude) * xScale;
			const double pixelHeight = (feature.maxLatitude - feature.minLatitude) * yScale;
			const double effectiveThreshold = interactive ? threshold * 3.0 : threshold;
			return (std::max)(pixelWidth, pixelHeight) >= effectiveThreshold;
		}

		bool BuildProjectedPath(const std::vector<CGroundMapRenderer::Coordinate>& sourcePath, bool closed, size_t minimumPoints)
		{
			scratchPoints.clear();
			if (sourcePath.size() < minimumPoints)
				return false;

			scratchPoints.reserve(sourcePath.size());
			for (const CGroundMapRenderer::Coordinate& coordinate : sourcePath)
			{
				const PointF point = ToPointF(coordinate);
				if (scratchPoints.empty() ||
					simplificationStepSquared <= 0.0 ||
					SquaredDistance(point, scratchPoints.back()) >= simplificationStepSquared)
				{
					scratchPoints.push_back(point);
				}
			}

			const PointF lastPoint = ToPointF(sourcePath.back());
			if (scratchPoints.empty() || SquaredDistance(lastPoint, scratchPoints.back()) >= 0.25)
				scratchPoints.push_back(lastPoint);

			if (closed && scratchPoints.size() > 2 && SquaredDistance(scratchPoints.front(), scratchPoints.back()) < 0.25)
				scratchPoints.pop_back();

			return scratchPoints.size() >= minimumPoints;
		}
	};

	Color CorrectColor(CColorManager* colorManager, const std::string& channel, Color color)
	{
		if (colorManager == nullptr)
			return color;
		return colorManager->get_corrected_color(channel, color);
	}

	struct RenderPassResources
	{
		std::unordered_map<ARGB, std::unique_ptr<SolidBrush>> brushes;
		std::unordered_map<unsigned long long, std::unique_ptr<Pen>> pens;
	};

	SolidBrush& GetCachedBrush(RenderPassResources& resources, Color color)
	{
		const ARGB key = color.GetValue();
		auto it = resources.brushes.find(key);
		if (it == resources.brushes.end())
			it = resources.brushes.emplace(key, std::make_unique<SolidBrush>(color)).first;
		return *it->second;
	}

	unsigned long long BuildPenKey(Color color, float width)
	{
		const unsigned long long colorKey = static_cast<unsigned long long>(color.GetValue());
		const unsigned long long widthKey = static_cast<unsigned long long>(
			std::clamp<int>(static_cast<int>(std::round(width * 100.0f)), 0, 0xFFFF));
		return (colorKey << 16) ^ widthKey;
	}

	Pen& GetCachedPen(RenderPassResources& resources, Color color, float width)
	{
		const unsigned long long key = BuildPenKey(color, width);
		auto it = resources.pens.find(key);
		if (it == resources.pens.end())
			it = resources.pens.emplace(key, std::make_unique<Pen>(color, width)).first;
		return *it->second;
	}

	int SelectLodIndexForZoom(const CGroundMapRenderer::Feature& feature, int zoomLevel, bool interactive)
	{
		if (feature.kind == CGroundMapRenderer::FeatureKind::Point)
			return 3;
		if (interactive)
		{
			if (zoomLevel <= 11 && !feature.lodPaths[0].empty())
				return 0;
			if (!feature.lodPaths[1].empty())
				return 1;
		}
		if (zoomLevel <= 5 && !feature.lodPaths[0].empty())
			return 0;
		if (zoomLevel <= 8 && !feature.lodPaths[1].empty())
			return 1;
		if (zoomLevel <= 10 && !feature.lodPaths[2].empty())
			return 2;
		return 3;
	}

	const std::vector<std::vector<CGroundMapRenderer::Coordinate>>& SelectPathsForLod(
		const CGroundMapRenderer::Feature& feature,
		int lodIndex)
	{
		if (lodIndex >= 0 && lodIndex < 3 && !feature.lodPaths[lodIndex].empty())
			return feature.lodPaths[lodIndex];
		return feature.paths;
	}

	void DrawLabel(
		CDC& dc,
		CColorManager* colorManager,
		const std::string& text,
		const POINT& point,
		Color textColor,
		int xOffset,
		int yOffset)
	{
		if (text.empty())
			return;

		const Color corrected = CorrectColor(colorManager, "label", textColor);
		const COLORREF oldColor = dc.SetTextColor(RGB(ColorRed(corrected), ColorGreen(corrected), ColorBlue(corrected)));
		const int oldBkMode = dc.SetBkMode(TRANSPARENT);
		dc.TextOutA(point.x + xOffset, point.y + yOffset, text.c_str());
		dc.SetBkMode(oldBkMode);
		dc.SetTextColor(oldColor);
	}

	void DrawArrowHead(Graphics& graphics, const PointF& from, const PointF& to, Color color, float size)
	{
		const double dx = to.X - from.X;
		const double dy = to.Y - from.Y;
		const double length = std::sqrt(dx * dx + dy * dy);
		if (length < 0.1)
			return;

		const double unitX = dx / length;
		const double unitY = dy / length;
		const double leftX = -unitY;
		const double leftY = unitX;

		PointF arrow[3] = {
			to,
			PointF(static_cast<REAL>(to.X - unitX * size + leftX * size * 0.45), static_cast<REAL>(to.Y - unitY * size + leftY * size * 0.45)),
			PointF(static_cast<REAL>(to.X - unitX * size - leftX * size * 0.45), static_cast<REAL>(to.Y - unitY * size - leftY * size * 0.45))
		};

		SolidBrush brush(color);
		graphics.FillPolygon(&brush, arrow, 3);
	}

	void RenderPolygonFeature(
		RenderContext& context,
		Graphics& graphics,
		const CGroundMapRenderer::Feature& feature,
		int lodIndex,
		CColorManager* colorManager,
		RenderPassResources& resources)
	{
		if (lodIndex < 0 || lodIndex >= static_cast<int>(feature.geoPaths.size()) || feature.geoPaths[lodIndex] == nullptr)
			return;

		if (feature.style.fillEnabled && feature.style.fill.GetAlpha() > 0)
		{
			SolidBrush& fillBrush = GetCachedBrush(resources, CorrectColor(colorManager, "symbol", feature.style.fill));
			graphics.FillPath(&fillBrush, feature.geoPaths[lodIndex].get());
		}
	}

	void RenderLineFeature(
		RenderContext& context,
		Graphics& graphics,
		const CGroundMapRenderer::Feature& feature,
		const std::vector<std::vector<CGroundMapRenderer::Coordinate>>& paths,
		CColorManager* colorManager,
		RenderPassResources& resources)
	{
		if (!feature.style.strokeEnabled || feature.style.stroke.GetAlpha() == 0)
			return;

		const Color lineColor = CorrectColor(colorManager, "symbol", feature.style.stroke);
		Pen& strokePen = GetCachedPen(resources, lineColor, feature.style.strokeWidth);

		for (const auto& sourcePath : paths)
		{
			if (!context.BuildProjectedPath(sourcePath, false, 2))
				continue;

			graphics.DrawLines(&strokePen, context.scratchPoints.data(), static_cast<INT>(context.scratchPoints.size()));

			if (feature.style.arrowEnabled)
			{
				DrawArrowHead(graphics, context.scratchPoints[context.scratchPoints.size() - 2], context.scratchPoints[context.scratchPoints.size() - 1], lineColor,
					(std::max)(7.0f, feature.style.strokeWidth * 4.0f));
			}
		}
	}

	void RenderPointFeature(
		RenderContext& context,
		Graphics& graphics,
		CDC& dc,
		const CGroundMapRenderer::Feature& feature,
		const RECT& radarArea,
		CColorManager* colorManager,
		RenderPassResources& resources,
		bool drawText)
	{
		const Color markerColor = CorrectColor(colorManager, "symbol", feature.style.marker);
		const Color strokeColor = CorrectColor(colorManager, "symbol", feature.style.stroke);
		SolidBrush& markerBrush = GetCachedBrush(resources, markerColor);
		Pen& markerPen = GetCachedPen(resources, strokeColor, (std::max)(1.0f, feature.style.strokeWidth));

		for (const auto& path : feature.paths)
		{
			for (const auto& coordinate : path)
			{
				const POINT pixel = context.ToPoint(coordinate);
				if (!PointIsNearRadarArea(pixel, radarArea, 30))
					continue;

				const float radius = feature.style.markerSize;
				if (feature.style.arrowEnabled && feature.hasHeading)
				{
					const double headingRad = DegToRad(feature.heading);
					const PointF tail(
						static_cast<REAL>(pixel.x - std::sin(headingRad) * radius),
						static_cast<REAL>(pixel.y + std::cos(headingRad) * radius));
					const PointF tip(
						static_cast<REAL>(pixel.x + std::sin(headingRad) * radius * 1.8),
						static_cast<REAL>(pixel.y - std::cos(headingRad) * radius * 1.8));
					Pen& arrowPen = GetCachedPen(resources, markerColor, (std::max)(1.0f, feature.style.strokeWidth));
					graphics.DrawLine(&arrowPen, tail, tip);
					DrawArrowHead(graphics, tail, tip, markerColor, (std::max)(6.0f, radius));
				}
				else if (feature.style.markerEnabled)
				{
					RectF rect(
						static_cast<REAL>(pixel.x) - radius,
						static_cast<REAL>(pixel.y) - radius,
						radius * 2.0f,
						radius * 2.0f);
					graphics.FillEllipse(&markerBrush, rect);
					if (feature.style.strokeEnabled)
						graphics.DrawEllipse(&markerPen, rect);
				}

				if (drawText && feature.style.textEnabled)
					DrawLabel(dc, colorManager, feature.label, pixel, feature.style.text, static_cast<int>(radius + 4.0f), -7);
			}
		}
	}

	void RenderFeature(
		RenderContext& context,
		Graphics& graphics,
		CDC& dc,
		const CGroundMapRenderer::Feature& feature,
		const RECT& radarArea,
		CColorManager* colorManager,
		RenderPassResources& resources,
		bool drawText)
	{
		if (context.zoomLevel < feature.minZoom || context.zoomLevel > feature.maxZoom)
			return;
		if (!context.IntersectsView(feature) || !context.IsLargeEnough(feature))
			return;

		const int lodIndex = SelectLodIndexForZoom(feature, context.zoomLevel, context.interactive);
		const std::vector<std::vector<CGroundMapRenderer::Coordinate>>& selectedPaths = SelectPathsForLod(feature, lodIndex);
		if (selectedPaths.empty())
			return;

		if (feature.kind == CGroundMapRenderer::FeatureKind::Polygon)
			RenderPolygonFeature(context, graphics, feature, lodIndex, colorManager, resources);
		else if (feature.kind == CGroundMapRenderer::FeatureKind::Line)
			RenderLineFeature(context, graphics, feature, selectedPaths, colorManager, resources);
		else
			RenderPointFeature(context, graphics, dc, feature, radarArea, colorManager, resources, drawText);

		if (drawText && feature.kind != CGroundMapRenderer::FeatureKind::Point && feature.style.textEnabled && feature.hasLabelAnchor)
		{
			const POINT centerPoint = context.ToPoint(feature.labelAnchor);
			if (PointIsNearRadarArea(centerPoint, radarArea, 30))
				DrawLabel(dc, colorManager, feature.label, centerPoint, feature.style.text, 4, -7);
		}
	}

	double CacheOverscanRatio(int zoomLevel)
	{
		return zoomLevel <= 5 ? 0.35 : 0.25;
	}

	struct CacheDimensions
	{
		int width = 0;
		int height = 0;
		double horizontalOverscan = 0.0;
		double verticalOverscan = 0.0;
	};

	CacheDimensions CalculateCacheDimensions(const RenderContext& context)
	{
		constexpr int maximumBitmapDimension = 4096;
		const int radarWidth = context.radarArea.right - context.radarArea.left;
		const int radarHeight = context.radarArea.bottom - context.radarArea.top;
		const int baseWidth = (std::max)(1, radarWidth);
		const int baseHeight = (std::max)(1, radarHeight);
		const double requestedOverscan = CacheOverscanRatio(context.zoomLevel);

		const int requestedExtraX = static_cast<int>(std::round(static_cast<double>(baseWidth) * requestedOverscan));
		const int requestedExtraY = static_cast<int>(std::round(static_cast<double>(baseHeight) * requestedOverscan));
		const int availableExtraX = (std::max)(0, (maximumBitmapDimension - baseWidth) / 2);
		const int availableExtraY = (std::max)(0, (maximumBitmapDimension - baseHeight) / 2);
		const int actualExtraX = (std::min)(requestedExtraX, availableExtraX);
		const int actualExtraY = (std::min)(requestedExtraY, availableExtraY);

		CacheDimensions result;
		result.width = std::clamp(baseWidth + actualExtraX * 2, 64, maximumBitmapDimension);
		result.height = std::clamp(baseHeight + actualExtraY * 2, 64, maximumBitmapDimension);
		result.horizontalOverscan = static_cast<double>(actualExtraX) / static_cast<double>(baseWidth);
		result.verticalOverscan = static_cast<double>(actualExtraY) / static_cast<double>(baseHeight);
		return result;
	}

	void GetExpandedCacheBounds(
		const RenderContext& context,
		double& minLatitude,
		double& maxLatitude,
		double& minLongitude,
		double& maxLongitude)
	{
		const CacheDimensions dimensions = CalculateCacheDimensions(context);
		const double latitudeSpan = context.maxLatitude - context.minLatitude;
		const double longitudeSpan = context.maxLongitude - context.minLongitude;
		const double latitudeMargin = latitudeSpan * dimensions.verticalOverscan;
		const double longitudeMargin = longitudeSpan * dimensions.horizontalOverscan;

		minLatitude = context.minLatitude - latitudeMargin;
		maxLatitude = context.maxLatitude + latitudeMargin;
		minLongitude = context.minLongitude - longitudeMargin;
		maxLongitude = context.maxLongitude + longitudeMargin;
	}

	bool CachedLayerCoversView(const CGroundMapRenderer::CachedGroundLayer& cache, const std::string& airport, const RenderContext& context)
	{
		if (!cache.valid || cache.bitmap == nullptr || cache.airport != airport)
			return false;

		const double latitudeEpsilon = (context.maxLatitude - context.minLatitude) * 0.002;
		const double longitudeEpsilon = (context.maxLongitude - context.minLongitude) * 0.002;
		return cache.minLatitude <= context.minLatitude + latitudeEpsilon &&
			cache.maxLatitude >= context.maxLatitude - latitudeEpsilon &&
			cache.minLongitude <= context.minLongitude + longitudeEpsilon &&
			cache.maxLongitude >= context.maxLongitude - longitudeEpsilon;
	}

	bool CachedLayerNeedsRebuild(const CGroundMapRenderer::CachedGroundLayer& cache, const std::string& airport, const RenderContext& context)
	{
		if (!cache.valid ||
			cache.bitmap == nullptr ||
			cache.airport != airport ||
			cache.zoomLevel != context.zoomLevel ||
			cache.width <= 0 ||
			cache.height <= 0)
		{
			return true;
		}

		if (!CachedLayerCoversView(cache, airport, context))
			return true;

		const double cacheLatitudeSpan = cache.maxLatitude - cache.minLatitude;
		const double cacheLongitudeSpan = cache.maxLongitude - cache.minLongitude;
		if (cacheLatitudeSpan <= 0.0 || cacheLongitudeSpan <= 0.0)
			return true;

		const double latitudeGuard = cacheLatitudeSpan * 0.08;
		const double longitudeGuard = cacheLongitudeSpan * 0.08;
		const bool nearCacheEdge =
			context.minLatitude <= cache.minLatitude + latitudeGuard ||
			context.maxLatitude >= cache.maxLatitude - latitudeGuard ||
			context.minLongitude <= cache.minLongitude + longitudeGuard ||
			context.maxLongitude >= cache.maxLongitude - longitudeGuard;

		const int radarWidth = context.radarArea.right - context.radarArea.left;
		const int radarHeight = context.radarArea.bottom - context.radarArea.top;
		if (radarWidth <= 0 || radarHeight <= 0)
			return true;

		const double currentLongitudeSpan = context.maxLongitude - context.minLongitude;
		const double currentLatitudeSpan = context.maxLatitude - context.minLatitude;
		if (currentLongitudeSpan <= 0.0 || currentLatitudeSpan <= 0.0)
			return true;

		const double cachedLongitudePerPixel = cacheLongitudeSpan / static_cast<double>(cache.width);
		const double cachedLatitudePerPixel = cacheLatitudeSpan / static_cast<double>(cache.height);
		const double currentLongitudePerPixel = currentLongitudeSpan / static_cast<double>(radarWidth);
		const double currentLatitudePerPixel = currentLatitudeSpan / static_cast<double>(radarHeight);
		if (currentLongitudePerPixel <= 0.0 || currentLatitudePerPixel <= 0.0)
			return true;

		const double scaleErrorX = std::fabs(cachedLongitudePerPixel / currentLongitudePerPixel - 1.0);
		const double scaleErrorY = std::fabs(cachedLatitudePerPixel / currentLatitudePerPixel - 1.0);
		const bool scaleChanged = scaleErrorX > 0.04 || scaleErrorY > 0.04;

		return nearCacheEdge || scaleChanged;
	}

	bool DrawVisibleCachedRegion(Graphics& graphics, const CGroundMapRenderer::CachedGroundLayer& cache, const RenderContext& context)
	{
		if (!cache.valid || cache.bitmap == nullptr || cache.width <= 0 || cache.height <= 0)
			return false;

		const double longitudeSpan = cache.maxLongitude - cache.minLongitude;
		const double latitudeSpan = cache.maxLatitude - cache.minLatitude;
		if (longitudeSpan <= 0.0 || latitudeSpan <= 0.0)
			return false;

		float sourceX = static_cast<float>(
			(context.minLongitude - cache.minLongitude) / longitudeSpan * static_cast<double>(cache.width));
		float sourceY = static_cast<float>(
			(cache.maxLatitude - context.maxLatitude) / latitudeSpan * static_cast<double>(cache.height));
		float sourceWidth = static_cast<float>(
			(context.maxLongitude - context.minLongitude) / longitudeSpan * static_cast<double>(cache.width));
		float sourceHeight = static_cast<float>(
			(context.maxLatitude - context.minLatitude) / latitudeSpan * static_cast<double>(cache.height));

		sourceX = std::clamp(sourceX, 0.0f, static_cast<float>(cache.width));
		sourceY = std::clamp(sourceY, 0.0f, static_cast<float>(cache.height));
		const float maxSourceWidth = (std::max)(1.0f, static_cast<float>(cache.width) - sourceX);
		const float maxSourceHeight = (std::max)(1.0f, static_cast<float>(cache.height) - sourceY);
		sourceWidth = std::clamp(sourceWidth, 1.0f, maxSourceWidth);
		sourceHeight = std::clamp(sourceHeight, 1.0f, maxSourceHeight);

		const RectF destination(
			static_cast<REAL>(context.radarArea.left),
			static_cast<REAL>(context.radarArea.top),
			static_cast<REAL>(context.radarArea.right - context.radarArea.left),
			static_cast<REAL>(context.radarArea.bottom - context.radarArea.top));
		if (destination.Width <= 1.0f || destination.Height <= 1.0f)
			return false;

		graphics.SetInterpolationMode(context.interactive ? InterpolationModeBilinear : InterpolationModeNearestNeighbor);
		graphics.DrawImage(
			cache.bitmap.get(),
			destination,
			sourceX,
			sourceY,
			sourceWidth,
			sourceHeight,
			UnitPixel);
		return true;
	}

	bool DrawStaleCachedLayer(Graphics& graphics, const CGroundMapRenderer::CachedGroundLayer& cache, const RenderContext& context)
	{
		if (!cache.valid || cache.bitmap == nullptr || cache.width <= 0 || cache.height <= 0)
			return false;

		const double longitudeSpan = cache.maxLongitude - cache.minLongitude;
		const double latitudeSpan = cache.maxLatitude - cache.minLatitude;
		if (longitudeSpan <= 0.0 || latitudeSpan <= 0.0)
			return false;

		const RectF destination(
			static_cast<REAL>(context.radarArea.left + (cache.minLongitude - context.minLongitude) * context.xScale),
			static_cast<REAL>(context.radarArea.bottom - (cache.maxLatitude - context.minLatitude) * context.yScale),
			static_cast<REAL>(longitudeSpan * context.xScale),
			static_cast<REAL>(latitudeSpan * context.yScale));
		const RectF radarRectangle(
			static_cast<REAL>(context.radarArea.left),
			static_cast<REAL>(context.radarArea.top),
			static_cast<REAL>(context.radarArea.right - context.radarArea.left),
			static_cast<REAL>(context.radarArea.bottom - context.radarArea.top));

		const REAL intersectionLeft = (std::max)(destination.X, radarRectangle.X);
		const REAL intersectionTop = (std::max)(destination.Y, radarRectangle.Y);
		const REAL intersectionRight = (std::min)(destination.X + destination.Width, radarRectangle.X + radarRectangle.Width);
		const REAL intersectionBottom = (std::min)(destination.Y + destination.Height, radarRectangle.Y + radarRectangle.Height);
		if (intersectionRight <= intersectionLeft || intersectionBottom <= intersectionTop)
			return false;

		graphics.SetInterpolationMode(InterpolationModeBilinear);
		graphics.DrawImage(
			cache.bitmap.get(),
			destination,
			0.0f,
			0.0f,
			static_cast<REAL>(cache.width),
			static_cast<REAL>(cache.height),
			UnitPixel);
		return true;
	}

	void RenderVectorLayer(
		RenderContext& context,
		Graphics& graphics,
		CDC& dc,
		const CGroundMapRenderer::CacheEntry& entry,
		const RECT& radarArea,
		CColorManager* colorManager,
		bool drawNonPolygonFeatures,
		bool drawText)
	{
		RenderPassResources resources;
		Matrix geoTransform(
			static_cast<REAL>(context.xScale),
			0.0f,
			0.0f,
			static_cast<REAL>(-context.yScale),
			static_cast<REAL>(radarArea.left - (context.minLongitude * context.xScale)),
			static_cast<REAL>(radarArea.bottom + (context.minLatitude * context.yScale)));

		graphics.SetTransform(&geoTransform);
		for (std::size_t featureIndex : entry.polygonIndices)
		{
			if (featureIndex < entry.features.size())
				RenderFeature(context, graphics, dc, entry.features[featureIndex], radarArea, colorManager, resources, drawText);
		}

		graphics.ResetTransform();
		if (!drawNonPolygonFeatures)
			return;

		for (std::size_t featureIndex : entry.lineIndices)
		{
			if (featureIndex < entry.features.size())
				RenderFeature(context, graphics, dc, entry.features[featureIndex], radarArea, colorManager, resources, drawText);
		}

		for (std::size_t featureIndex : entry.pointIndices)
		{
			if (featureIndex < entry.features.size())
				RenderFeature(context, graphics, dc, entry.features[featureIndex], radarArea, colorManager, resources, drawText);
		}
	}

	void RenderTextOverlay(
		RenderContext& context,
		CDC& dc,
		const CGroundMapRenderer::CacheEntry& entry,
		const RECT& radarArea,
		CColorManager* colorManager)
	{
		for (std::size_t featureIndex : entry.textIndices)
		{
			if (featureIndex >= entry.features.size())
				continue;
			const CGroundMapRenderer::Feature& feature = entry.features[featureIndex];
			if (!feature.style.textEnabled ||
				context.zoomLevel < feature.minZoom ||
				context.zoomLevel > feature.maxZoom ||
				!context.IntersectsView(feature) ||
				!context.IsLargeEnough(feature))
			{
				continue;
			}

			if (feature.kind == CGroundMapRenderer::FeatureKind::Point)
			{
				for (const auto& path : feature.paths)
				{
					for (const auto& coordinate : path)
					{
						const POINT pixel = context.ToPoint(coordinate);
						if (PointIsNearRadarArea(pixel, radarArea, 30))
							DrawLabel(dc, colorManager, feature.label, pixel, feature.style.text, static_cast<int>(feature.style.markerSize + 4.0f), -7);
					}
				}
			}
			else
			{
				if (!feature.hasLabelAnchor)
					continue;
				const POINT centerPoint = context.ToPoint(feature.labelAnchor);
				if (PointIsNearRadarArea(centerPoint, radarArea, 30))
					DrawLabel(dc, colorManager, feature.label, centerPoint, feature.style.text, 4, -7);
			}
		}
	}

	bool RebuildCachedLayer(
		CGroundMapRenderer::CachedGroundLayer& cache,
		const std::string& airport,
		const RenderContext& current,
		const CGroundMapRenderer::CacheEntry& entry,
		CDC& dc,
		CColorManager* colorManager)
	{
		double cacheMinLatitude = 0.0;
		double cacheMaxLatitude = 0.0;
		double cacheMinLongitude = 0.0;
		double cacheMaxLongitude = 0.0;
		GetExpandedCacheBounds(current, cacheMinLatitude, cacheMaxLatitude, cacheMinLongitude, cacheMaxLongitude);

		const CacheDimensions dimensions = CalculateCacheDimensions(current);
		const int cacheWidth = dimensions.width;
		const int cacheHeight = dimensions.height;
		std::unique_ptr<Bitmap> bitmap = std::make_unique<Bitmap>(cacheWidth, cacheHeight, PixelFormat32bppPARGB);
		if (bitmap->GetLastStatus() != Ok)
			return false;

		Graphics bitmapGraphics(bitmap.get());
		if (bitmapGraphics.GetLastStatus() != Ok)
			return false;

		bitmapGraphics.SetPageUnit(UnitPixel);
		bitmapGraphics.Clear(Color(0, 0, 0, 0));
		bitmapGraphics.SetSmoothingMode(SmoothingModeHighSpeed);
		bitmapGraphics.SetCompositingQuality(CompositingQualityHighSpeed);
		bitmapGraphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
		bitmapGraphics.SetPixelOffsetMode(PixelOffsetModeHalf);

		RECT cacheArea = { 0, 0, cacheWidth, cacheHeight };
		CPosition cacheSW;
		cacheSW.m_Latitude = cacheMinLatitude;
		cacheSW.m_Longitude = cacheMinLongitude;
		CPosition cacheNE;
		cacheNE.m_Latitude = cacheMaxLatitude;
		cacheNE.m_Longitude = cacheMaxLongitude;
		RenderContext cacheContext(cacheArea, cacheSW, cacheNE, current.zoomLevel, false);
		if (!cacheContext.valid)
			return false;

		const GraphicsState bitmapState = bitmapGraphics.Save();
		bitmapGraphics.SetClip(Rect(0, 0, cacheWidth, cacheHeight), CombineModeReplace);
		RenderVectorLayer(cacheContext, bitmapGraphics, dc, entry, cacheArea, colorManager, true, false);
		bitmapGraphics.Restore(bitmapState);

		cache.bitmap = std::move(bitmap);
		cache.airport = airport;
		cache.width = cacheWidth;
		cache.height = cacheHeight;
		cache.zoomLevel = current.zoomLevel;
		cache.minLatitude = cacheMinLatitude;
		cache.maxLatitude = cacheMaxLatitude;
		cache.minLongitude = cacheMinLongitude;
		cache.maxLongitude = cacheMaxLongitude;
		cache.valid = true;
		return true;
	}
}

void CGroundMapRenderer::ClearCache()
{
	AirportMaps.clear();
	CachedLayer = CachedGroundLayer();
	HasLastView = false;
	LastPanChangeTick = 0;
	LastZoomChangeTick = 0;
}

bool CGroundMapRenderer::RenderAirportMap(
	const std::string& airport,
	const std::string& dllPath,
	CSMRRadar& radar,
	Graphics& graphics,
	CDC& dc,
	const RECT& radarArea,
	CColorManager* colorManager)
{
	const std::string normalizedAirport = ToUpperCopy(TrimAsciiWhitespace(airport));
	if (!EnsureAirportLoaded(AirportMaps, normalizedAirport, dllPath))
		return false;

	auto mapIt = AirportMaps.find(normalizedAirport);
	if (mapIt == AirportMaps.end() || !mapIt->second.available)
		return false;

	CPosition displaySW;
	CPosition displayNE;
	radar.GetDisplayArea(&displaySW, &displayNE);

	const double currentMinLatitude = (std::min)(displaySW.m_Latitude, displayNE.m_Latitude);
	const double currentMaxLatitude = (std::max)(displaySW.m_Latitude, displayNE.m_Latitude);
	const double currentMinLongitude = (std::min)(displaySW.m_Longitude, displayNE.m_Longitude);
	const double currentMaxLongitude = (std::max)(displaySW.m_Longitude, displayNE.m_Longitude);
	const ULONGLONG nowTick = GetTickCount64();
	if (!HasLastView)
	{
		HasLastView = true;
	}
	else
	{
		const double previousLatitudeSpan = LastMaxLatitude - LastMinLatitude;
		const double previousLongitudeSpan = LastMaxLongitude - LastMinLongitude;
		const double currentLatitudeSpan = currentMaxLatitude - currentMinLatitude;
		const double currentLongitudeSpan = currentMaxLongitude - currentMinLongitude;
		const double previousCenterLatitude = (LastMinLatitude + LastMaxLatitude) * 0.5;
		const double previousCenterLongitude = (LastMinLongitude + LastMaxLongitude) * 0.5;
		const double currentCenterLatitude = (currentMinLatitude + currentMaxLatitude) * 0.5;
		const double currentCenterLongitude = (currentMinLongitude + currentMaxLongitude) * 0.5;
		const bool radarSizeChanged =
			LastRadarArea.left != radarArea.left ||
			LastRadarArea.top != radarArea.top ||
			LastRadarArea.right != radarArea.right ||
			LastRadarArea.bottom != radarArea.bottom;
		const double latitudeSpanTolerance = (std::max)(1e-9, std::fabs(previousLatitudeSpan) * 0.001);
		const double longitudeSpanTolerance = (std::max)(1e-9, std::fabs(previousLongitudeSpan) * 0.001);
		const bool zoomChanged =
			radarSizeChanged ||
			std::fabs(currentLatitudeSpan - previousLatitudeSpan) > latitudeSpanTolerance ||
			std::fabs(currentLongitudeSpan - previousLongitudeSpan) > longitudeSpanTolerance;
		const bool panChanged =
			std::fabs(currentCenterLatitude - previousCenterLatitude) > 0.0000005 ||
			std::fabs(currentCenterLongitude - previousCenterLongitude) > 0.0000005;

		if (zoomChanged)
			LastZoomChangeTick = nowTick;
		if (panChanged)
			LastPanChangeTick = nowTick;
	}

	LastRadarArea = radarArea;
	LastMinLatitude = currentMinLatitude;
	LastMaxLatitude = currentMaxLatitude;
	LastMinLongitude = currentMinLongitude;
	LastMaxLongitude = currentMaxLongitude;

	constexpr ULONGLONG panSettleDelayMs = 220;
	constexpr ULONGLONG zoomSettleDelayMs = 550;
	const bool panInteractive = LastPanChangeTick != 0 && nowTick - LastPanChangeTick < panSettleDelayMs;
	const bool zoomInteractive = LastZoomChangeTick != 0 && nowTick - LastZoomChangeTick < zoomSettleDelayMs;
	const bool interactiveView = panInteractive || zoomInteractive;
	RenderContext context(radarArea, displaySW, displayNE, radar.RadarViewZoomLevel, interactiveView);
	if (!context.valid)
		return false;

	const GraphicsState graphicsState = graphics.Save();
	graphics.SetClip(Gdiplus::Rect(
		radarArea.left,
		radarArea.top,
		radarArea.right - radarArea.left,
		radarArea.bottom - radarArea.top),
		CombineModeReplace);
	if (context.interactive)
	{
		graphics.SetSmoothingMode(SmoothingModeNone);
		graphics.SetCompositingQuality(CompositingQualityHighSpeed);
		graphics.SetInterpolationMode(InterpolationModeBilinear);
		graphics.SetPixelOffsetMode(PixelOffsetModeHighSpeed);
	}
	else
	{
		graphics.SetSmoothingMode(SmoothingModeAntiAlias);
		graphics.SetCompositingQuality(CompositingQualityHighQuality);
		graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
		graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
	}

	if (!context.interactive && CachedLayerNeedsRebuild(CachedLayer, normalizedAirport, context))
		RebuildCachedLayer(CachedLayer, normalizedAirport, context, mapIt->second, dc, colorManager);

	const bool canDrawCachedLayer = CachedLayerCoversView(CachedLayer, normalizedAirport, context);
	if (context.interactive)
	{
		if (canDrawCachedLayer)
			DrawVisibleCachedRegion(graphics, CachedLayer, context);
		else if (CachedLayer.valid && CachedLayer.airport == normalizedAirport)
			DrawStaleCachedLayer(graphics, CachedLayer, context);

		graphics.Restore(graphicsState);
		return true;
	}

	const bool drawIdleCache = !context.interactive && canDrawCachedLayer;
	if (drawIdleCache)
	{
		const bool drewCache = DrawVisibleCachedRegion(graphics, CachedLayer, context);
		if (drewCache)
		{
			if (!context.interactive)
				RenderTextOverlay(context, dc, mapIt->second, radarArea, colorManager);
			graphics.Restore(graphicsState);
			return true;
		}
	}

	const bool drawNonPolygonFeatures = !context.interactive || !canDrawCachedLayer;
	const bool drawText = !context.interactive;
	RenderVectorLayer(context, graphics, dc, mapIt->second, radarArea, colorManager, drawNonPolygonFeatures, drawText);
	graphics.Restore(graphicsState);

	return true;
}
