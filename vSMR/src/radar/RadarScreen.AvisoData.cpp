#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"
#include "radar/RadarScreenSupport.hpp"
#include "insets/InsetWindow.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "shared/TextUtils.hpp"

#include <cctype>
#include <limits>
#include <set>
#include <unordered_map>

using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::AvisoMin;
using VsmrAvisoSupport::ToUpperAscii;

namespace
{
	std::mutex gSessionActiveProfileMutex;
	std::string gSessionActiveProfileName;

	BYTE ClampColorByte(int value)
	{
		return static_cast<BYTE>(std::clamp(value, 0, 255));
	}

	bool IsRegularFileNoThrow(const fs::path& path)
	{
		try
		{
			return fs::exists(path) && fs::is_regular_file(path);
		}
		catch (...)
		{
			return false;
		}
	}

	bool IsDirectoryNoThrow(const fs::path& path)
	{
		try
		{
			return fs::exists(path) && fs::is_directory(path);
		}
		catch (...)
		{
			return false;
		}
	}

	fs::path PluginDataDirectory(const std::string& dllPath)
	{
		return fs::u8path(dllPath) / "vSMR_Data";
	}

	std::string ResolvePluginDataDirectoryPath(const std::string& dllPath)
	{
		const fs::path dataDirectory = PluginDataDirectory(dllPath);
		if (IsDirectoryNoThrow(dataDirectory))
			return dataDirectory.u8string();
		return fs::u8path(dllPath).u8string();
	}

	std::string ResolvePluginFilePath(const std::string& dllPath, const char* fileName)
	{
		const fs::path dataPath = PluginDataDirectory(dllPath) / fileName;
		if (IsRegularFileNoThrow(dataPath))
			return dataPath.u8string();
		return (fs::u8path(dllPath) / fileName).u8string();
	}

	std::string ResolvePluginDirectoryPath(const std::string& dllPath, const char* directoryName)
	{
		const fs::path dataPath = PluginDataDirectory(dllPath) / directoryName;
		if (IsDirectoryNoThrow(dataPath))
			return dataPath.u8string();
		return (fs::u8path(dllPath) / directoryName).u8string();
	}

	std::string TrimAvisoAirportCode(const std::string& value)
	{
		size_t start = 0;
		while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
			++start;

		size_t end = value.size();
		while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
			--end;

		return value.substr(start, end - start);
	}

	void PushUniquePath(std::vector<fs::path>& paths, const fs::path& path)
	{
		if (path.empty())
			return;

		const std::string normalized = ToUpperAscii(path.lexically_normal().u8string());
		for (const fs::path& existing : paths)
		{
			if (ToUpperAscii(existing.lexically_normal().u8string()) == normalized)
				return;
		}

		paths.push_back(path);
	}

	std::vector<fs::path> BuildAvisoGeoJsonSearchDirectories(const std::string& dllPath, const std::string& dataPath)
	{
		std::vector<fs::path> directories;
		const fs::path pluginDirectory = fs::u8path(dllPath);
		const fs::path resolvedDataDirectory = dataPath.empty()
			? PluginDataDirectory(dllPath)
			: fs::u8path(dataPath);

		PushUniquePath(directories, resolvedDataDirectory / "AVISO");
		PushUniquePath(directories, resolvedDataDirectory);
		PushUniquePath(directories, pluginDirectory / "AVISO");
		PushUniquePath(directories, pluginDirectory);

		return directories;
	}

	std::set<std::string> CollectAvisoAirportsInDirectory(const fs::path& searchDirectory)
	{
		std::set<std::string> airports;
		if (!IsDirectoryNoThrow(searchDirectory))
			return airports;

		for (const auto& entry : fs::directory_iterator(searchDirectory))
		{
			if (!entry.is_regular_file())
				continue;

			const fs::path path = entry.path();
			if (ToUpperAscii(path.extension().u8string()) != ".GEOJSON")
				continue;

			const std::string stem = ToUpperAscii(path.stem().u8string());
			const auto isAirportCode = [](const std::string& value) {
				return value.size() == 4 &&
					std::all_of(value.begin(), value.end(), [](unsigned char character) {
						return std::isalnum(character) != 0;
					});
			};
			if (isAirportCode(stem))
			{
				airports.insert(stem);
				continue;
			}

			// Compatibility for installations that still contain the
			// pre-2.0 AVISO_<ICAO>.geojson filename.
			const std::string legacyPrefix = "AVISO_";
			if (stem.rfind(legacyPrefix, 0) == 0)
			{
				const std::string legacyAirport = stem.substr(legacyPrefix.size());
				if (isAirportCode(legacyAirport))
					airports.insert(legacyAirport);
			}
		}

		return airports;
	}

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

	Gdiplus::Color ParseAvisoColor(const Value* properties, const char* colorProperty, const char* opacityProperty, const Gdiplus::Color& fallback)
	{
		if (properties == nullptr || !properties->IsObject() || colorProperty == nullptr)
			return fallback;

		if (!properties->HasMember(colorProperty) || !(*properties)[colorProperty].IsString())
			return fallback;

		const char* hex = (*properties)[colorProperty].GetString();
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
		if (opacityProperty != nullptr &&
			properties->HasMember(opacityProperty) &&
			(*properties)[opacityProperty].IsNumber())
		{
			opacity = (*properties)[opacityProperty].GetDouble();
		}
		opacity = std::clamp(opacity, 0.0, 1.0);
		const BYTE alpha = static_cast<BYTE>(std::lround(opacity * 255.0));

		return Gdiplus::Color(alpha, static_cast<BYTE>(red), static_cast<BYTE>(green), static_cast<BYTE>(blue));
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
		if (!palettes.HasMember(palette) || !palettes[palette].IsObject())
			return nullptr;
		return &palettes[palette];
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
		// in both modes unless that feature also supplies a Day override. All other
		// missing overrides inherit the effective base (night) color, keeping older
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
				const std::string value = ToUpperAscii(TrimAvisoAirportCode(visible.GetString()));
				if (value == "FALSE" || value == "0" || value == "NO" || value == "OFF" || value == "HIDDEN" || value == "NONE")
					return false;
			}
		}

		if (const char* visibility = GetAvisoStringProperty(properties, { "visibility" }))
		{
			const std::string value = ToUpperAscii(TrimAvisoAirportCode(visibility));
			if (value == "NONE" || value == "HIDDEN" || value == "FALSE" || value == "OFF" || value == "0")
				return false;
		}

		return true;
	}

	void PushUniqueAvisoGroupId(std::vector<std::string>& groupIds, const std::string& rawGroupId)
	{
		const std::string& groupId = rawGroupId;
		if (groupId.empty() ||
			std::find(groupIds.begin(), groupIds.end(), groupId) != groupIds.end())
		{
			return;
		}
		groupIds.push_back(groupId);
	}

	bool TryReadAvisoFeatureGroupIds(
		const Value* properties,
		std::vector<std::string>& groupIds)
	{
		groupIds.clear();
		if (properties == nullptr || !properties->IsObject())
			return true;

		const char* arrayKeys[] = {
			"vsmr_group_ids",
			"vsmr_groups",
			"group_ids"
		};
		for (const char* key : arrayKeys)
		{
			if (!properties->HasMember(key))
				continue;

			const Value& value = (*properties)[key];
			if (value.IsArray())
			{
				for (SizeType i = 0; i < value.Size(); ++i)
				{
					if (!value[i].IsString())
						return false;
					PushUniqueAvisoGroupId(groupIds, value[i].GetString());
				}
			}
			else if (value.IsString())
			{
				PushUniqueAvisoGroupId(groupIds, value.GetString());
			}
			else
			{
				return false;
			}

			// The first present modern membership property is authoritative,
			// including an explicitly empty array used to remove legacy membership.
			return true;
		}

		const char* scalarKeys[] = {
			"group_id",
			"vsmr_group_id"
		};
		for (const char* key : scalarKeys)
		{
			if (!properties->HasMember(key))
				continue;
			const Value& value = (*properties)[key];
			if (!value.IsString())
				return false;
			PushUniqueAvisoGroupId(groupIds, value.GetString());
			return true;
		}

		return true;
	}

	std::vector<std::string> ReadAvisoFeatureGroupIds(const Value* properties)
	{
		std::vector<std::string> groupIds;
		if (!TryReadAvisoFeatureGroupIds(properties, groupIds))
			groupIds.clear();
		return groupIds;
	}

	std::string ReadAvisoFeatureIdentity(const Value& feature)
	{
		if (!feature.IsObject())
			return "";
		if (feature.HasMember("id") && feature["id"].IsString())
			return feature["id"].GetString();
		if (feature.HasMember("properties") &&
			feature["properties"].IsObject() &&
			feature["properties"].HasMember("id") &&
			feature["properties"]["id"].IsString())
		{
			return feature["properties"]["id"].GetString();
		}
		return "";
	}

	std::wstring AvisoUtf8ToWide(const char* text)
	{
		if (text == nullptr || text[0] == '\0')
			return L"";

		const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
		if (requiredLength > 1)
		{
			std::wstring wide(static_cast<size_t>(requiredLength - 1), L'\0');
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), requiredLength);
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

	bool AvisoColorsEqual(const Gdiplus::Color& left, const Gdiplus::Color& right)
	{
		return left.GetAlpha() == right.GetAlpha() &&
			left.GetR() == right.GetR() &&
			left.GetG() == right.GetG() &&
			left.GetB() == right.GetB();
	}

	double RefreshPerfNowMs()
	{
		static LARGE_INTEGER frequency = {};
		if (frequency.QuadPart == 0)
			::QueryPerformanceFrequency(&frequency);

		LARGE_INTEGER counter = {};
		::QueryPerformanceCounter(&counter);
		return (static_cast<double>(counter.QuadPart) * 1000.0) / static_cast<double>(frequency.QuadPart);
	}
}

namespace VsmrRadarSupport
{
	std::string ResolvePluginDataDirectoryPath(const std::string& dllPath)
	{
		return ::ResolvePluginDataDirectoryPath(dllPath);
	}

	std::string ResolvePluginFilePath(const std::string& dllPath, const char* fileName)
	{
		return ::ResolvePluginFilePath(dllPath, fileName);
	}

	std::string ResolvePluginDirectoryPath(const std::string& dllPath, const char* directoryName)
	{
		return ::ResolvePluginDirectoryPath(dllPath, directoryName);
	}
}

std::string CSMRRadar::DetectDefaultAirportFromAviso() const
{
	try
	{
		if (DllPath.empty())
			return "";

		for (const fs::path& searchDirectory : BuildAvisoGeoJsonSearchDirectories(DllPath, DataPath))
		{
			const std::set<std::string> airports = CollectAvisoAirportsInDirectory(searchDirectory);
			if (airports.empty())
				continue;

			if (airports.size() == 1)
				return *airports.begin();
			return "";
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info("DetectDefaultAirportFromAviso exception: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("DetectDefaultAirportFromAviso exception: unknown");
	}

	return "";
}

std::string CSMRRadar::ResolveAvisoGeoJsonPathForAirport(const std::string& airport) const
{
	const std::string airportUpper = ToUpperAscii(TrimAvisoAirportCode(airport));
	if (DllPath.empty() || airportUpper.size() != 4 ||
		!std::all_of(
			airportUpper.begin(),
			airportUpper.end(),
			[](unsigned char character) { return std::isalnum(character) != 0; }))
		return "";

	const auto overridePath = AvisoGeoJsonOverridePaths.find(airportUpper);
	if (overridePath != AvisoGeoJsonOverridePaths.end() &&
		IsRegularFileNoThrow(overridePath->second))
	{
		return overridePath->second;
	}

	const std::string resolutionKey = DllPath + "|" + DataPath;
	if (AvisoGeoJsonResolvedAirport == airportUpper &&
		AvisoGeoJsonResolvedDllPath == resolutionKey)
	{
		return AvisoGeoJsonResolvedPath;
	}

	auto rememberResolvedPath = [&](const std::string& path) -> std::string
	{
		AvisoGeoJsonResolvedAirport = airportUpper;
		AvisoGeoJsonResolvedDllPath = resolutionKey;
		AvisoGeoJsonResolvedPath = path;
		return path;
	};

	try
	{
		const std::vector<fs::path> searchDirectories = BuildAvisoGeoJsonSearchDirectories(DllPath, DataPath);
		for (const fs::path& searchDirectory : searchDirectories)
		{
			const fs::path exactPath = searchDirectory / (airportUpper + ".geojson");
			if (IsRegularFileNoThrow(exactPath))
				return rememberResolvedPath(exactPath.u8string());
		}

		const std::string expectedStem = airportUpper;
		for (const fs::path& searchDirectory : searchDirectories)
		{
			if (!IsDirectoryNoThrow(searchDirectory))
				continue;

			for (const auto& entry : fs::directory_iterator(searchDirectory))
			{
				if (!entry.is_regular_file())
					continue;

				const fs::path path = entry.path();
				if (ToUpperAscii(path.extension().u8string()) == ".GEOJSON" &&
					ToUpperAscii(path.stem().u8string()) == expectedStem)
				{
					return rememberResolvedPath(path.u8string());
				}
			}
		}

		// Older manual installations can continue to load until their data tree
		// is upgraded, but the canonical ICAO filename above always wins.
		const std::string legacyExpectedStem = "AVISO_" + airportUpper;
		for (const fs::path& searchDirectory : searchDirectories)
		{
			if (!IsDirectoryNoThrow(searchDirectory))
				continue;

			for (const auto& entry : fs::directory_iterator(searchDirectory))
			{
				if (!entry.is_regular_file())
					continue;

				const fs::path path = entry.path();
				if (ToUpperAscii(path.extension().u8string()) == ".GEOJSON" &&
					ToUpperAscii(path.stem().u8string()) == legacyExpectedStem)
				{
					return rememberResolvedPath(path.u8string());
				}
			}
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info("ResolveAvisoGeoJsonPathForAirport exception: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("ResolveAvisoGeoJsonPathForAirport exception: unknown");
	}

	return rememberResolvedPath("");
}

bool CSMRRadar::EnsureAvisoGeoJsonLoaded(
	const std::string& path,
	bool retainPreviousOnFailure) try
{
	if (IsShutdownRequested())
		return false;

	if (path.empty())
		return false;
	const unsigned long nowTick = ::GetTickCount();
	const unsigned long statRefreshIntervalMs = 500;

	const auto canRetainPrevious = [&]()
	{
		if (!retainPreviousOnFailure ||
			!AvisoGeoJsonLoaded ||
			AvisoGeoJsonLoadedPath != path)
		{
			return false;
		}
		std::lock_guard<std::mutex> groupGuard(AvisoGroupMutex);
		return AvisoGeoJsonFeatureSnapshot != nullptr &&
			AvisoGeoJsonLabelSnapshot != nullptr &&
			AvisoGroupVisibilitySnapshot != nullptr;
	};
	if (AvisoGeoJsonLastFailedPath == path &&
		AvisoGeoJsonLastFailedTick != 0 &&
		(nowTick - AvisoGeoJsonLastFailedTick) < statRefreshIntervalMs)
	{
		return canRetainPrevious();
	}
	const auto rememberFailedAttempt = [&](const fs::file_time_type* failedWriteTime)
	{
		AvisoGeoJsonLastFailedPath = path;
		AvisoGeoJsonLastFailedTick = nowTick;
		AvisoGeoJsonLastFailedWriteTimeValid = failedWriteTime != nullptr;
		if (failedWriteTime != nullptr)
			AvisoGeoJsonLastFailedWriteTime = *failedWriteTime;
		return canRetainPrevious();
	};

	if (AvisoGeoJsonLoadAttempted &&
		AvisoGeoJsonLoadedPath == path &&
		AvisoGeoJsonLastStatTick != 0 &&
		(nowTick - AvisoGeoJsonLastStatTick) < statRefreshIntervalMs)
	{
		return AvisoGeoJsonLoaded;
	}

	fs::file_time_type writeTime;
	try
	{
		writeTime = fs::last_write_time(fs::u8path(path));
		AvisoGeoJsonLastStatTick = nowTick;
	}
	catch (const std::exception& ex)
	{
		Logger::info("AVISO GeoJSON stat failed path=" + path + " error=" + ex.what());
		AvisoGeoJsonLastStatTick = nowTick;
		return rememberFailedAttempt(nullptr);
	}
	catch (...)
	{
		Logger::info("AVISO GeoJSON stat failed path=" + path + " error=unknown");
		AvisoGeoJsonLastStatTick = nowTick;
		return rememberFailedAttempt(nullptr);
	}

	if (AvisoGeoJsonLastFailedPath == path &&
		AvisoGeoJsonLastFailedWriteTimeValid &&
		AvisoGeoJsonLastFailedWriteTime == writeTime)
	{
		// Stat periodically, but do not repeatedly parse an unchanged,
		// multi-megabyte invalid source on EuroScope's UI thread.
		AvisoGeoJsonLastFailedTick = nowTick;
		return canRetainPrevious();
	}

	if (AvisoGeoJsonLoadAttempted &&
		AvisoGeoJsonLoadedPath == path &&
		AvisoGeoJsonLoadedWriteTime == writeTime)
	{
		return AvisoGeoJsonLoaded;
	}

	AvisoLoadPerformance loadPerformance;
	loadPerformance.path = path;
	const double loadStartMilliseconds = RefreshPerfNowMs();
	struct PublishAvisoLoadPerformance
	{
		CSMRRadar* owner = nullptr;
		CSMRRadar::AvisoLoadPerformance* sample = nullptr;
		double startedMilliseconds = 0.0;
		~PublishAvisoLoadPerformance() noexcept
		{
			try
			{
				if (owner == nullptr || sample == nullptr)
					return;
				sample->totalMilliseconds = AvisoMax(
					0.0,
					RefreshPerfNowMs() - startedMilliseconds);
				owner->LastAvisoLoadPerformance = *sample;
				Logger::info(
					"AVISO load perf ms path=" + sample->path +
					" success=" + std::string(sample->success ? "1" : "0") +
					" read=" + std::to_string(sample->readMilliseconds) +
					" parse=" + std::to_string(sample->parseMilliseconds) +
					" validate=" + std::to_string(sample->validateMilliseconds) +
					" convert_commit=" + std::to_string(sample->convertCommitMilliseconds) +
					" total=" + std::to_string(sample->totalMilliseconds));
			}
			catch (...)
			{
			}
		}
	} publishLoadPerformance{ this, &loadPerformance, loadStartMilliseconds };

	const double readStartMilliseconds = RefreshPerfNowMs();
	std::string json;
	std::string readError;
	if (!AvisoDocumentModel::ReadBoundedSourceFile(
		fs::u8path(path),
		json,
		readError))
	{
		loadPerformance.readMilliseconds = AvisoMax(
			0.0,
			RefreshPerfNowMs() - readStartMilliseconds);
		Logger::info(
			"AVISO GeoJSON read failed path=" + path +
			" error=" + (readError.empty() ? "unknown" : readError));
		return rememberFailedAttempt(&writeTime);
	}
	loadPerformance.readMilliseconds = AvisoMax(
		0.0,
		RefreshPerfNowMs() - readStartMilliseconds);

	const double parseStartMilliseconds = RefreshPerfNowMs();
	AvisoDocumentModel validationModel;
	Document& parsedDocument = validationModel.MutableDocument();
	if (parsedDocument.Parse<0>(json.c_str()).HasParseError())
	{
		loadPerformance.parseMilliseconds = AvisoMax(
			0.0,
			RefreshPerfNowMs() - parseStartMilliseconds);
		Logger::info(
			"AVISO GeoJSON parse failed path=" + path +
			" offset=" + std::to_string(parsedDocument.GetErrorOffset()) +
			" error=" + std::string(parsedDocument.GetParseError()));
		return rememberFailedAttempt(&writeTime);
	}
	loadPerformance.parseMilliseconds = AvisoMax(
		0.0,
		RefreshPerfNowMs() - parseStartMilliseconds);

	if (!parsedDocument.IsObject() ||
		!parsedDocument.HasMember("features") ||
		!parsedDocument["features"].IsArray())
	{
		Logger::info("AVISO GeoJSON has no feature array path=" + path);
		return rememberFailedAttempt(&writeTime);
	}

	const double validationStartMilliseconds = RefreshPerfNowMs();
	const AvisoValidationResult validation =
		validationModel.ValidateAndRecalculate();
	loadPerformance.validateMilliseconds = AvisoMax(
		0.0,
		RefreshPerfNowMs() - validationStartMilliseconds);
	if (!validation.ok)
	{
		Logger::info(
			"AVISO GeoJSON validation failed path=" + path +
			" error=" + validation.errorText);
		return rememberFailedAttempt(&writeTime);
	}
	const Document& document = validationModel.GetDocument();
	const double convertCommitStartMilliseconds = RefreshPerfNowMs();

	const Value& features = document["features"];
	COLORREF parsedNightBackgroundColor = RGB(67, 74, 79);
	COLORREF parsedDayBackgroundColor = RGB(67, 74, 79);
	if (document.HasMember("metadata") && document["metadata"].IsObject())
	{
		const Value& metadata = document["metadata"];
		if (metadata.HasMember("background_colors") && metadata["background_colors"].IsObject())
		{
			const Value& backgroundColors = metadata["background_colors"];
			if (backgroundColors.HasMember("night"))
				parsedNightBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["night"], parsedNightBackgroundColor);
			if (backgroundColors.HasMember("day"))
				parsedDayBackgroundColor = ParseAvisoOpaqueColor(&backgroundColors["day"], parsedNightBackgroundColor);
			else
				parsedDayBackgroundColor = parsedNightBackgroundColor;
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
		group.name = TrimAvisoAirportCode(rawName);
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
		const std::string sourceFeatureId = ReadAvisoFeatureIdentity(featureValue);
		const std::vector<std::string> groupIds = ReadAvisoFeatureGroupIds(properties);
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
			parsedLabel.dayTextColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "day", "text-color", parsedLabel.textColor);
			parsedLabel.dayHaloColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "day", "text-halo-color", parsedLabel.haloColor);
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
		parsedFeature.dayFillColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "day", "fill", parsedFeature.fillColor);
		parsedFeature.dayStrokeColor = ParseAvisoPaletteColorResolved(sharedPaint, properties, "day", "stroke", parsedFeature.strokeColor);
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

	auto featureSnapshot =
		std::make_shared<const std::vector<AvisoFeature>>(parsedFeatures);
	auto labelSnapshot =
		std::make_shared<const std::vector<AvisoLabel>>(parsedLabels);
	auto visibility =
		std::make_shared<std::unordered_map<std::string, bool>>();
	visibility->reserve(parsedGroups.size());
	for (const AvisoGroup& group : parsedGroups)
		(*visibility)[group.id] = group.visible;

	// Commit only after the replacement document has been fully read and parsed.
	// A malformed on-disk update must never clear the last known-good overlay.
	const bool loadedPathChanged = AvisoGeoJsonLoadedPath != path;
	AvisoGeoJsonFeatures = std::move(parsedFeatures);
	AvisoGeoJsonLabels = std::move(parsedLabels);
	AvisoGeoJsonLoadedPath = path;
	AvisoGeoJsonViewInitializedPath.clear();
	AvisoGeoJsonLoadedWriteTime = writeTime;
	AvisoGeoJsonLastViewValid = false;
	AvisoGeoJsonLastViewPath.clear();
	AvisoGeoJsonLastViewChangeTick = 0;
	AvisoGeoJsonLastStatTick = nowTick;
	AvisoGeoJsonLoadAttempted = true;
	AvisoGeoJsonLoaded = true;
	AvisoGeoJsonLastFailedPath.clear();
	AvisoGeoJsonLastFailedTick = 0;
	AvisoGeoJsonLastFailedWriteTimeValid = false;
	AvisoGeoJsonRenderDisabled = false;
	AvisoGeoJsonRenderDisabledPath.clear();
	AvisoGeoJsonHasBounds = parsedHasBounds;
	AvisoGeoJsonMinLongitude = parsedMinLongitude;
	AvisoGeoJsonMinLatitude = parsedMinLatitude;
	AvisoGeoJsonMaxLongitude = parsedMaxLongitude;
	AvisoGeoJsonMaxLatitude = parsedMaxLatitude;
	AvisoNightBackgroundColor = parsedNightBackgroundColor;
	AvisoDayBackgroundColor = parsedDayBackgroundColor;
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		AvisoGeoJsonRenderLatestRequestId = ++AvisoGeoJsonRenderNextRequestId;
		AvisoGeoJsonRenderCancellationToken->store(
			AvisoGeoJsonRenderLatestRequestId,
			std::memory_order_release);
		AvisoGeoJsonPendingRenderRequest.reset();
		AvisoGeoJsonCompletedRenderResult.reset();
		AvisoGeoJsonRenderLastRequestValid = false;
	}
	if (loadedPathChanged)
		ClearAvisoGeoJsonRasterCache();
	{
		std::lock_guard<std::mutex> groupGuard(AvisoGroupMutex);
		AvisoGeoJsonFeatureSnapshot = std::move(featureSnapshot);
		AvisoGeoJsonLabelSnapshot = std::move(labelSnapshot);
		AvisoGeoJsonSourceFeatureCount = static_cast<size_t>(features.Size());
		AvisoRuntimeGroups = parsedGroups;
		AvisoGroupVisibilitySnapshot = visibility;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}
	for (auto& appWindow : appWindows)
	{
		if (appWindow.second != nullptr && appWindow.second->IsAvisoViewport())
		{
			if (loadedPathChanged)
				appWindow.second->ClearAvisoViewportCache();
			else
				appWindow.second->InvalidateAvisoViewportRendering();
		}
	}
	loadPerformance.convertCommitMilliseconds = AvisoMax(
		0.0,
		RefreshPerfNowMs() - convertCommitStartMilliseconds);
	loadPerformance.success = true;
	Logger::info(
		"AVISO GeoJSON loaded path=" + path +
		" features=" + std::to_string(AvisoGeoJsonFeatures.size()) +
		" polygons=" + std::to_string(polygonCount) +
		" multilines=" + std::to_string(multiLineCount) +
		" labels=" + std::to_string(labelCount) +
		" groups=" + std::to_string(parsedGroups.size()) +
		" styles=" + std::to_string(stylePaintById.size()) +
		" missingStyles=" + std::to_string(missingStyleCount));
	return true;
}
catch (const std::exception& exception)
{
	try
	{
		Logger::info(
			"AVISO GeoJSON load stopped by exception path=" + path +
			" error=" + exception.what());
	}
	catch (...)
	{
	}
	return false;
}
catch (...)
{
	try
	{
		Logger::info("AVISO GeoJSON load stopped by unknown exception");
	}
	catch (...)
	{
	}
	return false;
}

bool CSMRRadar::PrewarmAvisoForActiveAirport()
{
	if (IsShutdownRequested())
		return false;
	const std::string path = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());
	return !path.empty() && EnsureAvisoGeoJsonLoaded(path, true);
}

std::vector<CSMRRadar::AvisoGroup> CSMRRadar::GetAvisoGroups() const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	return AvisoRuntimeGroups;
}

std::shared_ptr<const std::unordered_map<std::string, bool>> CSMRRadar::GetAvisoGroupVisibilitySnapshot(
	unsigned long long* outGeneration) const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	if (outGeneration != nullptr)
		*outGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	return AvisoGroupVisibilitySnapshot;
}

bool CSMRRadar::GetAvisoRenderSnapshots(
	std::shared_ptr<const std::vector<AvisoFeature>>& outFeatures,
	std::shared_ptr<const std::vector<AvisoLabel>>& outLabels,
	std::shared_ptr<const std::unordered_map<std::string, bool>>& outGroupVisibility,
	unsigned long long& outGeneration) const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	outFeatures = AvisoGeoJsonFeatureSnapshot;
	outLabels = AvisoGeoJsonLabelSnapshot;
	outGroupVisibility = AvisoGroupVisibilitySnapshot;
	outGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	return outFeatures != nullptr && outLabels != nullptr &&
		outGroupVisibility != nullptr;
}


bool CSMRRadar::ApplyAvisoGroupMembershipSnapshot(
	const rapidjson::Value& aviso,
	std::string* outError)
{
	auto fail = [&](const std::string& message) -> bool
	{
		if (outError != nullptr)
			*outError = message;
		return false;
	};
	if (outError != nullptr)
		outError->clear();

	if (!aviso.IsObject() ||
		!aviso.HasMember("features") ||
		!aviso["features"].IsArray())
	{
		return fail("Staged AVISO state must contain a features array.");
	}

	std::shared_ptr<const std::vector<AvisoFeature>> baseFeatures;
	std::shared_ptr<const std::vector<AvisoLabel>> baseLabels;
	size_t sourceFeatureCount = 0;
	unsigned long long baseGeneration = 0;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		baseFeatures = AvisoGeoJsonFeatureSnapshot;
		baseLabels = AvisoGeoJsonLabelSnapshot;
		sourceFeatureCount = AvisoGeoJsonSourceFeatureCount;
		baseGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	}
	if (baseFeatures == nullptr || baseLabels == nullptr)
		return fail("No loaded AVISO renderer snapshot is available.");

	const rapidjson::Value& stagedFeatures = aviso["features"];
	const size_t stagedFeatureCount = static_cast<size_t>(stagedFeatures.Size());
	std::vector<std::vector<std::string>> memberships(stagedFeatureCount);
	std::vector<std::string> featureIds(stagedFeatureCount);
	std::unordered_map<std::string, size_t> stagedIndexById;
	stagedIndexById.reserve(stagedFeatureCount);
	for (rapidjson::SizeType index = 0; index < stagedFeatures.Size(); ++index)
	{
		const rapidjson::Value& feature = stagedFeatures[index];
		if (!feature.IsObject() ||
			!feature.HasMember("properties") ||
			!feature["properties"].IsObject())
		{
			return fail(
				"Staged AVISO feature " + std::to_string(index + 1) +
				" must contain a properties object.");
		}

		if (!TryReadAvisoFeatureGroupIds(
			&feature["properties"],
			memberships[static_cast<size_t>(index)]))
		{
			return fail(
				"Staged AVISO feature " + std::to_string(index + 1) +
				" has an invalid group membership value.");
		}
		const size_t featureIndex = static_cast<size_t>(index);
		featureIds[featureIndex] = ReadAvisoFeatureIdentity(feature);
		if (!featureIds[featureIndex].empty() &&
			!stagedIndexById.emplace(featureIds[featureIndex], featureIndex).second)
		{
			return fail(
				"Staged AVISO feature ids must be unique when applying group membership.");
		}
	}

	auto featureSnapshot = std::make_shared<std::vector<AvisoFeature>>(*baseFeatures);
	auto labelSnapshot = std::make_shared<std::vector<AvisoLabel>>(*baseLabels);
	auto applyMembership = [&](auto& item) -> bool
	{
		if (item.sourceFeatureIndex < 0 ||
			static_cast<size_t>(item.sourceFeatureIndex) >= sourceFeatureCount)
		{
			return false;
		}

		size_t stagedIndex = 0;
		bool matched = false;
		if (!item.sourceFeatureId.empty())
		{
			const auto found = stagedIndexById.find(item.sourceFeatureId);
			if (found != stagedIndexById.end())
			{
				stagedIndex = found->second;
				matched = true;
			}
		}
		else
		{
			const size_t sourceIndex = static_cast<size_t>(item.sourceFeatureIndex);
			if (sourceIndex < stagedFeatureCount &&
				featureIds[sourceIndex].empty())
			{
				stagedIndex = sourceIndex;
				matched = true;
			}
		}

		// Geometry additions/deletions remain staged until Save. Leave any
		// loaded item without a safe staged identity match unchanged.
		if (matched)
			item.groupIds = memberships[stagedIndex];
		return true;
	};
	for (AvisoFeature& feature : *featureSnapshot)
	{
		if (!applyMembership(feature))
			return fail("Staged AVISO feature identities do not match the loaded renderer.");
	}
	for (AvisoLabel& label : *labelSnapshot)
	{
		if (!applyMembership(label))
			return fail("Staged AVISO feature identities do not match the loaded renderer.");
	}

	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		if (AvisoGroupGeneration.load(std::memory_order_relaxed) != baseGeneration ||
			AvisoGeoJsonFeatureSnapshot != baseFeatures ||
			AvisoGeoJsonLabelSnapshot != baseLabels ||
			AvisoGeoJsonSourceFeatureCount != sourceFeatureCount)
		{
			return fail("AVISO renderer state changed while applying staged membership.");
		}

		AvisoGeoJsonFeatureSnapshot = featureSnapshot;
		AvisoGeoJsonLabelSnapshot = labelSnapshot;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}

	InvalidateAvisoGroupRendering();
	return true;
}

void CSMRRadar::InvalidateAvisoGroupRendering()
{
	{
		std::lock_guard<std::mutex> renderGuard(AvisoGeoJsonRenderMutex);
		AvisoGeoJsonRenderLatestRequestId = ++AvisoGeoJsonRenderNextRequestId;
		AvisoGeoJsonRenderCancellationToken->store(
			AvisoGeoJsonRenderLatestRequestId,
			std::memory_order_release);
		AvisoGeoJsonPendingRenderRequest.reset();
		AvisoGeoJsonCompletedRenderResult.reset();
		AvisoGeoJsonRenderLastRequestValid = false;
	}

	// Keep the last same-path raster as a stale preview while the new group or
	// ownership generation is rebuilt. Exact-cache checks still reject it.
	AvisoGeoJsonLastViewValid = false;
	AvisoGeoJsonLastViewPath.clear();
	AvisoGeoJsonLastViewChangeTick = 0;

	for (auto& appWindow : appWindows)
	{
		if (appWindow.second != nullptr && appWindow.second->IsAvisoViewport())
			appWindow.second->InvalidateAvisoViewportRendering();
	}

	try
	{
		RequestRefresh();
	}
	catch (...)
	{
	}
}

std::string CSMRRadar::GetAvisoColorPalette() const
{
	return AvisoUseDayColorPalette ? "day" : "night";
}

bool CSMRRadar::SetAvisoColorPalette(const std::string& rawPalette, bool persistToAsr)
{
	std::string palette = rawPalette;
	palette.erase(
		palette.begin(),
		std::find_if(
			palette.begin(),
			palette.end(),
			[](unsigned char value) { return !std::isspace(value); }));
	palette.erase(
		std::find_if(
			palette.rbegin(),
			palette.rend(),
			[](unsigned char value) { return !std::isspace(value); }).base(),
		palette.end());
	std::transform(
		palette.begin(),
		palette.end(),
		palette.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (palette != "day" && palette != "night")
		return false;

	const bool useDayPalette = palette == "day";
	if (AvisoUseDayColorPalette != useDayPalette)
	{
		AvisoUseDayColorPalette = useDayPalette;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		InvalidateAvisoGroupRendering();
	}

	if (persistToAsr)
	{
		SaveDataToAsr(
			"AvisoColorPalette",
			"AVISO day/night color palette",
			GetAvisoColorPalette().c_str());
	}
	return true;
}

bool CSMRRadar::SetAvisoGroupVisibility(const std::string& rawGroupId, bool visible)
{
	const std::string& groupId = rawGroupId;
	if (groupId.empty())
		return false;

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		auto found = std::find_if(
			AvisoRuntimeGroups.begin(),
			AvisoRuntimeGroups.end(),
			[&](const AvisoGroup& group) { return group.id == groupId; });
		if (found == AvisoRuntimeGroups.end())
			return false;
		if (found->visible == visible)
			return true;

		found->visible = visible;
		auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
		visibility->reserve(AvisoRuntimeGroups.size());
		for (const AvisoGroup& group : AvisoRuntimeGroups)
			(*visibility)[group.id] = group.visible;
		AvisoGroupVisibilitySnapshot = visibility;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		changed = true;
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}

bool CSMRRadar::ToggleAvisoGroupVisibility(const std::string& rawGroupId, bool* outVisible)
{
	const std::string& groupId = rawGroupId;
	if (groupId.empty())
		return false;

	bool nextVisibility = true;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		auto found = std::find_if(
			AvisoRuntimeGroups.begin(),
			AvisoRuntimeGroups.end(),
			[&](const AvisoGroup& group) { return group.id == groupId; });
		if (found == AvisoRuntimeGroups.end())
			return false;

		found->visible = !found->visible;
		nextVisibility = found->visible;
		auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
		visibility->reserve(AvisoRuntimeGroups.size());
		for (const AvisoGroup& group : AvisoRuntimeGroups)
			(*visibility)[group.id] = group.visible;
		AvisoGroupVisibilitySnapshot = visibility;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}

	if (outVisible != nullptr)
		*outVisible = nextVisibility;
	InvalidateAvisoGroupRendering();
	return true;
}

bool CSMRRadar::SetAvisoGroupVisibilities(
	const std::vector<std::pair<std::string, bool>>& requestedVisibility)
{
	std::unordered_map<std::string, bool> visibilityById;
	for (const auto& entry : requestedVisibility)
	{
		const std::string& groupId = entry.first;
		if (!groupId.empty())
			visibilityById[groupId] = entry.second;
	}

	if (visibilityById.empty())
		return requestedVisibility.empty();

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		for (const auto& requested : visibilityById)
		{
			const auto found = std::find_if(
				AvisoRuntimeGroups.begin(),
				AvisoRuntimeGroups.end(),
				[&](const AvisoGroup& group) { return group.id == requested.first; });
			if (found == AvisoRuntimeGroups.end())
				return false;
		}

		for (AvisoGroup& group : AvisoRuntimeGroups)
		{
			const auto found = visibilityById.find(group.id);
			if (found == visibilityById.end())
				continue;
			if (group.visible != found->second)
			{
				group.visible = found->second;
				changed = true;
			}
		}

		if (changed)
		{
			auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
			visibility->reserve(AvisoRuntimeGroups.size());
			for (const AvisoGroup& group : AvisoRuntimeGroups)
				(*visibility)[group.id] = group.visible;
			AvisoGroupVisibilitySnapshot = visibility;
			AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}

bool CSMRRadar::UpdateAvisoGroups(const std::vector<AvisoGroup>& groups)
{
	std::vector<AvisoGroup> normalizedGroups;
	std::unordered_set<std::string> knownIds;
	normalizedGroups.reserve(groups.size());
	for (const AvisoGroup& source : groups)
	{
		AvisoGroup group = source;
		if (group.id.empty() || !knownIds.insert(group.id).second)
			continue;
		group.name = TrimAvisoAirportCode(group.name);
		if (group.name.empty())
			group.name = group.id;
		normalizedGroups.push_back(std::move(group));
	}

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		if (AvisoRuntimeGroups.size() != normalizedGroups.size())
		{
			changed = true;
		}
		else
		{
			for (size_t i = 0; i < normalizedGroups.size(); ++i)
			{
				if (AvisoRuntimeGroups[i].id != normalizedGroups[i].id ||
					AvisoRuntimeGroups[i].name != normalizedGroups[i].name ||
					AvisoRuntimeGroups[i].visible != normalizedGroups[i].visible)
				{
					changed = true;
					break;
				}
			}
		}

		if (changed)
		{
			AvisoRuntimeGroups = std::move(normalizedGroups);
			auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
			visibility->reserve(AvisoRuntimeGroups.size());
			for (const AvisoGroup& group : AvisoRuntimeGroups)
				(*visibility)[group.id] = group.visible;
			AvisoGroupVisibilitySnapshot = visibility;
			AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}

void CSMRRadar::RememberSessionActiveProfile(const std::string& profileName)
{
	const std::string trimmed = TrimAsciiWhitespaceCopy(profileName);
	if (trimmed.empty())
		return;

	std::lock_guard<std::mutex> guard(gSessionActiveProfileMutex);
	gSessionActiveProfileName = trimmed;
}

std::string CSMRRadar::GetSessionActiveProfile(const std::string& fallbackProfile)
{
	std::lock_guard<std::mutex> guard(gSessionActiveProfileMutex);
	if (!gSessionActiveProfileName.empty())
		return gSessionActiveProfileName;
	return fallbackProfile;
}

std::string CSMRRadar::ReadLastActiveProfileFromConfig() const
{
	if (CurrentConfig == nullptr)
		return "";
	return TrimAsciiWhitespaceCopy(CurrentConfig->getLastActiveProfileName());
}

void CSMRRadar::WriteLastActiveProfileToConfig(const std::string& profileName) const
{
	const std::string trimmedName = TrimAsciiWhitespaceCopy(profileName);
	if (trimmedName.empty() || CurrentConfig == nullptr)
		return;

	if (CurrentConfig->setLastActiveProfileName(trimmedName))
		CurrentConfig->saveConfig();
}
