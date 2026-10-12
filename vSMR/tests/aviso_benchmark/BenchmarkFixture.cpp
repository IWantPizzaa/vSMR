// Benchmark-only loader conversion extracted from dev e774ac5.
// Uses the shipped document directly, without local overrides/editor validation.
#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>
#include "BenchmarkFixture.hpp"
#include "aviso/AvisoFeatureMetadata.hpp"
#include "aviso/AvisoPolygonOutline.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"
#include "rapidjson/document.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>
using namespace rapidjson;
using namespace VsmrRadarTypes;
using VsmrAvisoSupport::AvisoMin;
using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::ToUpperAscii;
using VsmrAvisoFeatureMetadata::ReadFeatureIdentity;
using VsmrAvisoFeatureMetadata::ReadGroupIds;
using VsmrAvisoFeatureMetadata::TrimAirportCode;
namespace {
	bool TryParseHexByte(const char* text, unsigned int& value)
	{
		if (text == nullptr)
			return false;

		value = 0;
		for (int i = 0; i < 2; ++i)
		{
			const char c = text[i];
			unsigned int digit = 0;
			if (c >= '0' && c <= '9')
				digit = static_cast<unsigned int>(c - '0');
			else if (c >= 'a' && c <= 'f')
				digit = static_cast<unsigned int>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				digit = static_cast<unsigned int>(c - 'A' + 10);
			else
				return false;
			value = (value << 4) | digit;
		}
		return true;
	}

	bool AvisoHasStringProperty(const Value* properties, const char* key)
	{
		return properties != nullptr &&
			properties->IsObject() &&
			key != nullptr &&
			properties->HasMember(key) &&
			(*properties)[key].IsString() &&
			(*properties)[key].GetString()[0] != '\0';
	}

	bool AvisoHasNumberProperty(const Value* properties, const char* key)
	{
		return properties != nullptr &&
			properties->IsObject() &&
			key != nullptr &&
			properties->HasMember(key) &&
			(*properties)[key].IsNumber();
	}

	COLORREF ParseAvisoOpaqueColor(const Value* value, COLORREF fallback)
	{
		if (value == nullptr || !value->IsString())
			return fallback;
		const char* hex = value->GetString();
		if (hex == nullptr || hex[0] != '#' || std::strlen(hex) != 7)
			return fallback;

		unsigned int red = 0;
		unsigned int green = 0;
		unsigned int blue = 0;
		if (!TryParseHexByte(hex + 1, red) ||
			!TryParseHexByte(hex + 3, green) ||
			!TryParseHexByte(hex + 5, blue))
		{
			return fallback;
		}
		return RGB(red, green, blue);
	}

	Gdiplus::Color ParseAvisoColorResolved(const Value* sharedPaint, const Value* inlineProperties, const char* colorProperty, const char* opacityProperty, const Gdiplus::Color& fallback)
	{
		// A catalog style provides defaults for every feature that references it.
		// Feature properties are the persisted per-object overrides used by both
		// AVISO editors, so they must win when both locations define a paint key.
		const Value* colorSource = AvisoHasStringProperty(inlineProperties, colorProperty)
			? inlineProperties
			: sharedPaint;
		if (!AvisoHasStringProperty(colorSource, colorProperty))
			return fallback;

		const char* hex = (*colorSource)[colorProperty].GetString();
		if (hex == nullptr || hex[0] != '#' || std::strlen(hex) != 7)
			return fallback;

		unsigned int red = 0;
		unsigned int green = 0;
		unsigned int blue = 0;
		if (!TryParseHexByte(hex + 1, red) ||
			!TryParseHexByte(hex + 3, green) ||
			!TryParseHexByte(hex + 5, blue))
		{
			return fallback;
		}

		double opacity = static_cast<double>(fallback.GetAlpha()) / 255.0;
		const Value* opacitySource = nullptr;
		if (AvisoHasNumberProperty(inlineProperties, opacityProperty))
			opacitySource = inlineProperties;
		else if (AvisoHasNumberProperty(sharedPaint, opacityProperty))
			opacitySource = sharedPaint;
		if (opacitySource != nullptr)
			opacity = (*opacitySource)[opacityProperty].GetDouble();

		opacity = std::clamp(opacity, 0.0, 1.0);
		const BYTE alpha = static_cast<BYTE>(std::lround(opacity * 255.0));
		return Gdiplus::Color(alpha, static_cast<BYTE>(red), static_cast<BYTE>(green), static_cast<BYTE>(blue));
	}

	const Value* GetAvisoPalettePaint(const Value* paint, const char* palette)
	{
		if (paint == nullptr ||
			!paint->IsObject() ||
			palette == nullptr ||
			!paint->HasMember("palette-overrides") ||
			!(*paint)["palette-overrides"].IsObject())
		{
			return nullptr;
		}

		const Value& palettes = (*paint)["palette-overrides"];
		if (palettes.HasMember(palette) && palettes[palette].IsObject())
			return &palettes[palette];

		// Schema-2 files used "day" before Light and Real became separate
		// palettes. Light inherits that value, while Real prefers Light and then
		// the legacy Day value when an airport has not yet been migrated.
		if (std::strcmp(palette, "real") == 0 && palettes.HasMember("light") && palettes["light"].IsObject())
			return &palettes["light"];
		if ((std::strcmp(palette, "light") == 0 || std::strcmp(palette, "real") == 0) &&
			palettes.HasMember("day") && palettes["day"].IsObject())
		{
			return &palettes["day"];
		}
		return nullptr;
	}

	Gdiplus::Color ParseAvisoPaletteColorResolved(
		const Value* sharedPaint,
		const Value* inlineProperties,
		const char* palette,
		const char* colorProperty,
		const Gdiplus::Color& fallback)
	{
		// Palette-specific feature overrides win over palette-specific catalog
		// defaults. An intentional feature-level base color remains authoritative
		// in every palette unless that feature supplies its own override. All other
		// missing overrides inherit the effective base (Dark) color, keeping older
		// and custom schema-2 AVISO documents compatible.
		const Value* inlinePalette = GetAvisoPalettePaint(inlineProperties, palette);
		const Value* sharedPalette = GetAvisoPalettePaint(sharedPaint, palette);
		const Value* colorSource = nullptr;
		if (AvisoHasStringProperty(inlinePalette, colorProperty))
			colorSource = inlinePalette;
		else if (AvisoHasStringProperty(inlineProperties, colorProperty))
			return fallback;
		else
			colorSource = sharedPalette;
		if (!AvisoHasStringProperty(colorSource, colorProperty))
			return fallback;

		const char* hex = (*colorSource)[colorProperty].GetString();
		if (hex == nullptr || hex[0] != '#' || std::strlen(hex) != 7)
			return fallback;

		unsigned int red = 0;
		unsigned int green = 0;
		unsigned int blue = 0;
		if (!TryParseHexByte(hex + 1, red) ||
			!TryParseHexByte(hex + 3, green) ||
			!TryParseHexByte(hex + 5, blue))
		{
			return fallback;
		}

		return Gdiplus::Color(
			fallback.GetAlpha(),
			static_cast<BYTE>(red),
			static_cast<BYTE>(green),
			static_cast<BYTE>(blue));
	}

	const char* GetAvisoStringProperty(const Value* properties, std::initializer_list<const char*> keys)
	{
		if (properties == nullptr || !properties->IsObject())
			return nullptr;

		for (const char* key : keys)
		{
			if (key == nullptr ||
				!properties->HasMember(key) ||
				!(*properties)[key].IsString())
			{
				continue;
			}

			const char* value = (*properties)[key].GetString();
			if (value != nullptr && value[0] != '\0')
				return value;
		}

		return nullptr;
	}

	const char* GetAvisoStringPropertyResolved(const Value* sharedPaint, const Value* inlineProperties, std::initializer_list<const char*> keys)
	{
		if (const char* value = GetAvisoStringProperty(inlineProperties, keys))
			return value;
		return GetAvisoStringProperty(sharedPaint, keys);
	}

	bool IsAvisoFeatureVisible(const Value* properties)
	{
		if (properties == nullptr || !properties->IsObject())
			return true;

		if (properties->HasMember("visible"))
		{
			const Value& visible = (*properties)["visible"];
			if (visible.IsBool())
				return visible.GetBool();
			if (visible.IsString())
			{
				const std::string value = ToUpperAscii(TrimAirportCode(visible.GetString()));
				if (value == "FALSE" || value == "0" || value == "NO" || value == "OFF" || value == "HIDDEN" || value == "NONE")
					return false;
			}
		}

		if (const char* visibility = GetAvisoStringProperty(properties, { "visibility" }))
		{
			const std::string value = ToUpperAscii(TrimAirportCode(visibility));
			if (value == "NONE" || value == "HIDDEN" || value == "FALSE" || value == "OFF" || value == "0")
				return false;
		}

		return true;
	}

	std::wstring AvisoUtf8ToWide(const char* text)
	{
		if (text == nullptr || text[0] == '\0')
			return L"";

		const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
		if (requiredLength > 1)
		{
			std::wstring wide(static_cast<size_t>(requiredLength), L'\0');
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), requiredLength);
			wide.resize(static_cast<size_t>(requiredLength - 1));
			return wide;
		}

		std::wstring fallback;
		while (*text != '\0')
			fallback.push_back(static_cast<unsigned char>(*text++));
		return fallback;
	}

	float ParseAvisoFloatProperty(const Value* properties, const char* key, float fallback, float minValue, float maxValue)
	{
		if (properties == nullptr || !properties->IsObject() || key == nullptr ||
			!properties->HasMember(key) ||
			!(*properties)[key].IsNumber())
		{
			return fallback;
		}

		const double value = (*properties)[key].GetDouble();
		if (!std::isfinite(value))
			return fallback;
		return static_cast<float>(std::clamp(value, static_cast<double>(minValue), static_cast<double>(maxValue)));
	}

	float ParseAvisoFloatPropertyResolved(const Value* sharedPaint, const Value* inlineProperties, const char* key, float fallback, float minValue, float maxValue)
	{
		if (AvisoHasNumberProperty(inlineProperties, key))
			return ParseAvisoFloatProperty(inlineProperties, key, fallback, minValue, maxValue);
		return ParseAvisoFloatProperty(sharedPaint, key, fallback, minValue, maxValue);
	}

	int ParseAvisoMinimumZoomLevel(const Value* sharedPaint, const Value* inlineProperties)
	{
		const Value* zoomValue = nullptr;
		const char* aliases[] = { "zoomLevel", "zoom_level" };
		auto findZoomValue = [&](const Value* properties) -> const Value*
		{
			for (const char* alias : aliases)
			{
				if (AvisoHasNumberProperty(properties, alias))
					return &(*properties)[alias];
			}
			return nullptr;
		};
		zoomValue = findZoomValue(inlineProperties);
		if (zoomValue == nullptr)
			zoomValue = findZoomValue(sharedPaint);

		if (zoomValue == nullptr)
			return 0;

		const int level = static_cast<int>(std::lround(std::clamp(zoomValue->GetDouble(), 0.0, 14.0)));
		return level;
	}

	float ParseAvisoStrokeWidth(const Value* properties, float fallback)
	{
		if (properties == nullptr || !properties->IsObject() ||
			!properties->HasMember("stroke-width") ||
			!(*properties)["stroke-width"].IsNumber())
		{
			return fallback;
		}

		const double width = (*properties)["stroke-width"].GetDouble();
		if (!std::isfinite(width))
			return fallback;
		return static_cast<float>(std::clamp(width, 0.25, 8.0));
	}

	float ParseAvisoStrokeWidthResolved(const Value* sharedPaint, const Value* inlineProperties, float fallback)
	{
		if (AvisoHasNumberProperty(inlineProperties, "stroke-width"))
			return ParseAvisoStrokeWidth(inlineProperties, fallback);
		return ParseAvisoStrokeWidth(sharedPaint, fallback);
	}

	const Value* ResolveAvisoStylePaint(const Value* properties, const std::unordered_map<std::string, const Value*>& stylePaintById)
	{
		if (properties == nullptr || !properties->IsObject())
			return nullptr;
		const char* styleId = GetAvisoStringProperty(properties, { "style_id" });
		if (styleId == nullptr)
			return nullptr;
		const auto found = stylePaintById.find(styleId);
		return found != stylePaintById.end() ? found->second : nullptr;
	}

}
namespace VsmrAvisoBenchmark {
Fixture LoadFixture(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open fixture: " + path.u8string());
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Document document;
    document.Parse(json.data(), json.size());
    if (document.HasParseError() || !document.IsObject() || !document.HasMember("features") || !document["features"].IsArray())
        throw std::runtime_error("Invalid fixture: " + path.u8string());
const Value& features = document["features"];
	COLORREF parsedDarkBackgroundColor = RGB(67, 74, 79);
	COLORREF parsedLightBackgroundColor = RGB(67, 74, 79);
	COLORREF parsedRealBackgroundColor = RGB(67, 74, 79);
	if (document.HasMember("metadata") && document["metadata"].IsObject())
	{
		const Value& metadata = document["metadata"];
		if (metadata.HasMember("background_colors") && metadata["background_colors"].IsObject())
		{
			const Value& backgroundColors = metadata["background_colors"];
			if (backgroundColors.HasMember("dark"))
				parsedDarkBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["dark"], parsedDarkBackgroundColor);
			else if (backgroundColors.HasMember("night"))
				parsedDarkBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["night"], parsedDarkBackgroundColor);

			if (backgroundColors.HasMember("light"))
				parsedLightBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["light"], parsedDarkBackgroundColor);
			else if (backgroundColors.HasMember("day"))
				parsedLightBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["day"], parsedDarkBackgroundColor);
			else
				parsedLightBackgroundColor = parsedDarkBackgroundColor;

			if (backgroundColors.HasMember("real"))
				parsedRealBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["real"], parsedLightBackgroundColor);
			else
				parsedRealBackgroundColor = parsedLightBackgroundColor;
		}
	}
	std::vector<AvisoFeature> parsedFeatures;
	std::vector<AvisoLabel> parsedLabels;
	bool parsedHasBounds = false;
	double parsedMinLongitude = 0.0;
	double parsedMinLatitude = 0.0;
	double parsedMaxLongitude = 0.0;
	double parsedMaxLatitude = 0.0;
	std::unordered_map<std::string, const Value*> stylePaintById;
	if (document.HasMember("styles") && document["styles"].IsObject())
	{
		const Value& styles = document["styles"];
		for (Value::ConstMemberIterator it = styles.MemberBegin(); it != styles.MemberEnd(); ++it)
		{
			if (!it->name.IsString() || !it->value.IsObject())
				continue;
			if (it->value.HasMember("paint") && it->value["paint"].IsObject())
				stylePaintById.emplace(it->name.GetString(), &it->value["paint"]);
		}
	}
	std::vector<AvisoGroup> parsedGroups;
	std::unordered_set<std::string> parsedGroupIds;
	auto addParsedGroup = [&](const std::string& rawId, const std::string& rawName, bool visible)
	{
		const std::string& id = rawId;
		if (id.empty() || !parsedGroupIds.insert(id).second)
			return;

		AvisoGroup group;
		group.id = id;
		group.name = TrimAirportCode(rawName);
		if (group.name.empty())
			group.name = id;
		group.visible = visible;
		parsedGroups.push_back(std::move(group));
	};
	if (document.HasMember("vsmr_groups") && document["vsmr_groups"].IsArray())
	{
		const Value& groupDefinitions = document["vsmr_groups"];
		for (SizeType i = 0; i < groupDefinitions.Size(); ++i)
		{
			const Value& groupValue = groupDefinitions[i];
			if (!groupValue.IsObject())
				continue;
			const char* id = GetAvisoStringProperty(&groupValue, { "id", "group_id" });
			if (id == nullptr)
				continue;
			const char* name = GetAvisoStringProperty(&groupValue, { "name", "label" });
			addParsedGroup(id, name != nullptr ? name : id, IsAvisoFeatureVisible(&groupValue));
		}
	}
	size_t polygonCount = 0;
	size_t multiLineCount = 0;
	size_t labelCount = 0;
	size_t missingStyleCount = 0;

	auto addPoint = [](const Value& coordinate, AvisoFeature& feature, std::vector<AvisoPoint>& pathPoints) {
		if (!coordinate.IsArray() || coordinate.Size() < 2 ||
			!coordinate[static_cast<SizeType>(0)].IsNumber() ||
			!coordinate[static_cast<SizeType>(1)].IsNumber())
		{
			return;
		}

		const double longitude = coordinate[static_cast<SizeType>(0)].GetDouble();
		const double latitude = coordinate[static_cast<SizeType>(1)].GetDouble();
		if (!std::isfinite(longitude) || !std::isfinite(latitude))
			return;

		pathPoints.push_back({ longitude, latitude });
		feature.minLongitude = AvisoMin(feature.minLongitude, longitude);
		feature.maxLongitude = AvisoMax(feature.maxLongitude, longitude);
		feature.minLatitude = AvisoMin(feature.minLatitude, latitude);
		feature.maxLatitude = AvisoMax(feature.maxLatitude, latitude);
	};

	for (SizeType i = 0; i < features.Size(); ++i)
	{
		const Value& featureValue = features[i];
		if (!featureValue.IsObject() ||
			!featureValue.HasMember("geometry") ||
			!featureValue["geometry"].IsObject())
		{
			continue;
		}

		const Value& geometry = featureValue["geometry"];
		if (!geometry.HasMember("type") ||
			!geometry["type"].IsString() ||
			!geometry.HasMember("coordinates") ||
			!geometry["coordinates"].IsArray())
		{
			continue;
		}

		const Value* properties = nullptr;
		if (featureValue.HasMember("properties") && featureValue["properties"].IsObject())
			properties = &featureValue["properties"];
		const std::string sourceFeatureId = ReadFeatureIdentity(featureValue);
		const std::vector<std::string> groupIds = ReadGroupIds(properties);
		for (const std::string& groupId : groupIds)
			addParsedGroup(groupId, groupId, true);
		if (!IsAvisoFeatureVisible(properties))
			continue;
		const Value* sharedPaint = ResolveAvisoStylePaint(properties, stylePaintById);
		if (sharedPaint == nullptr && !stylePaintById.empty() && GetAvisoStringProperty(properties, { "style_id" }) != nullptr)
			++missingStyleCount;

		const std::string geometryType = geometry["type"].GetString();
		const Value& coordinates = geometry["coordinates"];

		if (geometryType == "Point")
		{
			const char* geometryRole = GetAvisoStringProperty(properties, { "geometry_role" });
			const char* objectType = GetAvisoStringProperty(properties, { "object_type", "type" });
			const bool textRole = geometryRole != nullptr && ToUpperAscii(geometryRole) == "TEXT_LABEL";
			const bool labelObject = objectType != nullptr && ToUpperAscii(objectType) == "LABEL";
			if (!textRole && !labelObject)
				continue;

			if (!coordinates.IsArray() ||
				coordinates.Size() < 2 ||
				!coordinates[static_cast<SizeType>(0)].IsNumber() ||
				!coordinates[static_cast<SizeType>(1)].IsNumber())
			{
				continue;
			}

			const char* rawText = GetAvisoStringProperty(properties, { "text-field", "display_frequency", "text", "label", "name" });
			if (rawText == nullptr)
				continue;

			const double longitude = coordinates[static_cast<SizeType>(0)].GetDouble();
			const double latitude = coordinates[static_cast<SizeType>(1)].GetDouble();
			if (!std::isfinite(longitude) || !std::isfinite(latitude))
				continue;

			AvisoLabel parsedLabel;
			parsedLabel.position = { longitude, latitude };
			parsedLabel.sourceFeatureIndex = static_cast<int>(i);
			parsedLabel.sourceFeatureId = sourceFeatureId;
			parsedLabel.groupIds = groupIds;
			parsedLabel.text = AvisoUtf8ToWide(rawText);
			if (parsedLabel.text.empty())
				continue;

			const char* labelClass = GetAvisoStringProperty(properties, { "label_class", "category", "section" });
			if (labelClass != nullptr)
				parsedLabel.labelClass = labelClass;
			const char* textAnchor = GetAvisoStringPropertyResolved(sharedPaint, properties, { "text-anchor" });
			if (textAnchor != nullptr)
				parsedLabel.textAnchor = textAnchor;
			const char* textFont = GetAvisoStringPropertyResolved(sharedPaint, properties, { "text-font", "font", "font-family" });
			if (textFont != nullptr)
				parsedLabel.fontFamily = AvisoUtf8ToWide(textFont);

			parsedLabel.textColor = ParseAvisoColorResolved(sharedPaint, properties, "text-color", nullptr, Gdiplus::Color(255, 128, 128, 128));
			parsedLabel.haloColor = ParseAvisoColorResolved(sharedPaint, properties, "text-halo-color", nullptr, Gdiplus::Color(255, 0, 0, 0));
			parsedLabel.lightTextColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "light", "text-color", parsedLabel.textColor);
			parsedLabel.lightHaloColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "light", "text-halo-color", parsedLabel.haloColor);
			parsedLabel.realTextColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "real", "text-color", parsedLabel.textColor);
			parsedLabel.realHaloColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "real", "text-halo-color", parsedLabel.haloColor);
			parsedLabel.textSize = ParseAvisoFloatPropertyResolved(sharedPaint, properties, "text-size", 12.0f, 6.0f, 32.0f);
			parsedLabel.haloWidth = ParseAvisoFloatPropertyResolved(sharedPaint, properties, "text-halo-width", 1.0f, 0.0f, 6.0f);
			parsedLabel.minimumZoomLevel = ParseAvisoMinimumZoomLevel(sharedPaint, properties);
			parsedLabels.push_back(std::move(parsedLabel));
			++labelCount;
			continue;
		}

		AvisoFeature parsedFeature;
		parsedFeature.sourceFeatureIndex = static_cast<int>(i);
		parsedFeature.sourceFeatureId = sourceFeatureId;
		parsedFeature.groupIds = groupIds;
		parsedFeature.fillColor = ParseAvisoColorResolved(sharedPaint, properties, "fill", "fill-opacity", Gdiplus::Color(217, 53, 66, 82));
		parsedFeature.strokeColor = ParseAvisoColorResolved(sharedPaint, properties, "stroke", "stroke-opacity", Gdiplus::Color(191, 140, 152, 170));
		parsedFeature.lightFillColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "light", "fill", parsedFeature.fillColor);
		parsedFeature.lightStrokeColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "light", "stroke", parsedFeature.strokeColor);
		parsedFeature.realFillColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "real", "fill", parsedFeature.fillColor);
		parsedFeature.realStrokeColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "real", "stroke", parsedFeature.strokeColor);
		parsedFeature.polygonOutline = VsmrAviso::ResolvePolygonOutline(sharedPaint, properties, nullptr, nullptr);
		parsedFeature.lightPolygonOutline = VsmrAviso::ResolvePolygonOutline(sharedPaint, properties,
			GetAvisoPalettePaint(sharedPaint, "light"), GetAvisoPalettePaint(properties, "light"));
		parsedFeature.realPolygonOutline = VsmrAviso::ResolvePolygonOutline(sharedPaint, properties,
			GetAvisoPalettePaint(sharedPaint, "real"), GetAvisoPalettePaint(properties, "real"));
		parsedFeature.strokeWidth = ParseAvisoStrokeWidthResolved(sharedPaint, properties, 1.0f);
		parsedFeature.minimumZoomLevel = ParseAvisoMinimumZoomLevel(sharedPaint, properties);
		parsedFeature.minLongitude = (std::numeric_limits<double>::max)();
		parsedFeature.minLatitude = (std::numeric_limits<double>::max)();
		parsedFeature.maxLongitude = std::numeric_limits<double>::lowest();
		parsedFeature.maxLatitude = std::numeric_limits<double>::lowest();

		if (geometryType == "Polygon")
		{
			parsedFeature.polygon = true;
			for (SizeType ringIndex = 0; ringIndex < coordinates.Size(); ++ringIndex)
			{
				const Value& ring = coordinates[ringIndex];
				if (!ring.IsArray())
					continue;

				std::vector<AvisoPoint> ringPoints;
				ringPoints.reserve(ring.Size());
				for (SizeType pointIndex = 0; pointIndex < ring.Size(); ++pointIndex)
					addPoint(ring[pointIndex], parsedFeature, ringPoints);

				if (ringPoints.size() >= 3)
					parsedFeature.paths.push_back(std::move(ringPoints));
			}
			if (!parsedFeature.paths.empty())
				++polygonCount;
		}
		else if (geometryType == "MultiLineString")
		{
			parsedFeature.polygon = false;
			for (SizeType lineIndex = 0; lineIndex < coordinates.Size(); ++lineIndex)
			{
				const Value& line = coordinates[lineIndex];
				if (!line.IsArray())
					continue;

				std::vector<AvisoPoint> linePoints;
				linePoints.reserve(line.Size());
				for (SizeType pointIndex = 0; pointIndex < line.Size(); ++pointIndex)
					addPoint(line[pointIndex], parsedFeature, linePoints);

				if (linePoints.size() >= 2)
					parsedFeature.paths.push_back(std::move(linePoints));
			}
			if (!parsedFeature.paths.empty())
				++multiLineCount;
		}
		else if (geometryType == "LineString")
		{
			parsedFeature.polygon = false;
			std::vector<AvisoPoint> linePoints;
			linePoints.reserve(coordinates.Size());
			for (SizeType pointIndex = 0; pointIndex < coordinates.Size(); ++pointIndex)
				addPoint(coordinates[pointIndex], parsedFeature, linePoints);

			if (linePoints.size() >= 2)
			{
				parsedFeature.paths.push_back(std::move(linePoints));
				++multiLineCount;
			}
		}

		if (!parsedFeature.paths.empty() &&
			parsedFeature.minLongitude <= parsedFeature.maxLongitude &&
			parsedFeature.minLatitude <= parsedFeature.maxLatitude)
		{
			if (!parsedHasBounds)
			{
				parsedMinLongitude = parsedFeature.minLongitude;
				parsedMaxLongitude = parsedFeature.maxLongitude;
				parsedMinLatitude = parsedFeature.minLatitude;
				parsedMaxLatitude = parsedFeature.maxLatitude;
				parsedHasBounds = true;
			}
			else
			{
				parsedMinLongitude = AvisoMin(parsedMinLongitude, parsedFeature.minLongitude);
				parsedMaxLongitude = AvisoMax(parsedMaxLongitude, parsedFeature.maxLongitude);
				parsedMinLatitude = AvisoMin(parsedMinLatitude, parsedFeature.minLatitude);
				parsedMaxLatitude = AvisoMax(parsedMaxLatitude, parsedFeature.maxLatitude);
			}
			parsedFeatures.push_back(std::move(parsedFeature));
		}
	}
    if (!parsedHasBounds) throw std::runtime_error("Fixture has no geometry bounds");
    Fixture result;
    result.features = std::make_shared<const std::vector<AvisoFeature>>(std::move(parsedFeatures));
    result.labels = std::make_shared<const std::vector<AvisoLabel>>(std::move(parsedLabels));
    auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
    for (const auto& group : parsedGroups) visibility->emplace(group.id, group.visible);
    result.visibility = std::move(visibility);
    result.minLongitude = parsedMinLongitude; result.maxLongitude = parsedMaxLongitude;
    result.minLatitude = parsedMinLatitude; result.maxLatitude = parsedMaxLatitude;
    const COLORREF colors[] = { parsedDarkBackgroundColor, parsedLightBackgroundColor, parsedRealBackgroundColor };
    for (size_t i = 0; i < 3; ++i) result.backgrounds[i] = Gdiplus::Color(255, GetRValue(colors[i]), GetGValue(colors[i]), GetBValue(colors[i]));
    return result;
}
}
