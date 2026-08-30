#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "bootstrap/RuntimeContext.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreenSupport.hpp"
#include "radar/TargetTrailRenderer.hpp"
#include "insets/InsetWindow.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <limits>
#include <commctrl.h>
#include "rapidjson/document.h"
#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
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

extern std::map<HWND, std::vector<CSMRRadar*>> gInsetWindowRadarScreens;
extern HHOOK gThreadMouseHook;
extern HHOOK gThreadKeyboardHook;
UINT AvisoWorkerRefreshMessage();
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

	double AvisoMin(double left, double right)
	{
		return left < right ? left : right;
	}

	double AvisoMax(double left, double right)
	{
		return left > right ? left : right;
	}

	std::string ToUpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
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

	bool IsAvisoGroupedItemVisible(
		const std::vector<std::string>& groupIds,
		const std::unordered_map<std::string, bool>* visibilityById)
	{
		if (groupIds.empty() || visibilityById == nullptr)
			return true;

		bool foundKnownGroup = false;
		for (const std::string& groupId : groupIds)
		{
			const auto found = visibilityById->find(groupId);
			if (found == visibilityById->end())
			{
				// Legacy/foreign group references remain visible until a
				// definition explicitly controls them.
				return true;
			}

			foundKnownGroup = true;
			if (found->second)
				return true;
		}

		// Multiple membership uses union semantics: an item is visible when
		// at least one of its configured groups is visible.
		return !foundKnownGroup;
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

	bool AvisoWithinTolerance(double left, double right, double tolerance)
	{
		const double delta = left - right;
		return delta >= -tolerance && delta <= tolerance;
	}

	bool AvisoPointWithinTolerance(const PointF& left, const PointF& right, double tolerance)
	{
		return AvisoWithinTolerance(left.X, right.X, tolerance) &&
			AvisoWithinTolerance(left.Y, right.Y, tolerance);
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

	class ScopedHBitmap
	{
	public:
		~ScopedHBitmap()
		{
			Reset();
		}

		void Reset(HBITMAP bitmap = nullptr)
		{
			if (bitmap_ != nullptr)
				::DeleteObject(bitmap_);
			bitmap_ = bitmap;
		}

		HBITMAP Get() const
		{
			return bitmap_;
		}

		HBITMAP Release()
		{
			HBITMAP bitmap = bitmap_;
			bitmap_ = nullptr;
			return bitmap;
		}

	private:
		HBITMAP bitmap_ = nullptr;
	};

	void OutputVsmrDebugLine(const std::string& message)
	{
		const std::string line = "[vSMR] " + message + "\n";
		::OutputDebugStringA(line.c_str());
		Logger::info(message);
	}

	class ScopedCdcDetach
	{
	public:
		explicit ScopedCdcDetach(CDC& dc) : dc_(dc)
		{
		}

		~ScopedCdcDetach()
		{
			Detach();
		}

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

void CSMRRadar::EnsureAvisoGeoJsonRenderThread()
{
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;

	std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
	if (IsShutdownRequested() ||
		AvisoGeoJsonRenderStop.load(std::memory_order_relaxed) ||
		AvisoGeoJsonRenderThreadStarted)
	{
		return;
	}

	try
	{
		AvisoGeoJsonRenderStop.store(false, std::memory_order_relaxed);
		AvisoGeoJsonRenderThread = std::thread(&CSMRRadar::AvisoGeoJsonRenderThreadMain, this);
		AvisoGeoJsonRenderThreadStarted = true;
	}
	catch (const std::exception& ex)
	{
		AvisoGeoJsonRenderThreadStarted = false;
		Logger::info("AVISO render worker start failed: " + std::string(ex.what()));
	}
	catch (...)
	{
		AvisoGeoJsonRenderThreadStarted = false;
		Logger::info("AVISO render worker start failed: unknown exception");
	}
}

void CSMRRadar::StopAvisoGeoJsonRenderThread()
{
	bool shouldJoin = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		AvisoGeoJsonRenderStop.store(true, std::memory_order_relaxed);
		AvisoGeoJsonRenderCancellationToken->fetch_add(1, std::memory_order_release);
		AvisoGeoJsonPendingRenderRequest.reset();
		AvisoGeoJsonCompletedRenderResult.reset();
		AvisoGeoJsonRenderLastRequestValid = false;
		shouldJoin = AvisoGeoJsonRenderThreadStarted;
	}

	if (!shouldJoin)
	{
		if (AvisoGeoJsonRenderThread.joinable())
			AvisoGeoJsonRenderThread.join();
		return;
	}

	AvisoGeoJsonRenderCondition.notify_all();
	if (AvisoGeoJsonRenderThread.joinable())
		AvisoGeoJsonRenderThread.join();

	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		AvisoGeoJsonRenderThreadStarted = false;
		AvisoGeoJsonRenderInFlight = false;
		AvisoGeoJsonRenderLastRequestValid = false;
	}
}

bool CSMRRadar::IsAvisoGeoJsonRenderStopRequested() const
{
	return IsShutdownRequested() || AvisoGeoJsonRenderStop.load(std::memory_order_relaxed);
}

bool CSMRRadar::IsShutdownRequested() const
{
	return ShutdownRequested.load(std::memory_order_relaxed);
}

bool CSMRRadar::CanUnloadRuntimeCallbacks() noexcept
{
	// A failed Win32 callback removal must retain the DLL. Otherwise Windows may
	// dispatch into unmapped code after EuroScope completes plug-in shutdown.
	return gInsetWindowRadarScreens.empty() &&
		gThreadMouseHook == nullptr &&
		gThreadKeyboardHook == nullptr;
}

void CSMRRadar::BeginShutdown()
{
	ShutdownRequested.store(true, std::memory_order_relaxed);
	AvisoRefreshHostWindow.store(nullptr, std::memory_order_release);
	AvisoGeoJsonRenderStop.store(true, std::memory_order_relaxed);
	StopAvisoGeoJsonRenderThread();

	for (auto& appWindow : appWindows)
	{
		if (appWindow.second != nullptr)
		{
			appWindow.second->EndAvisoPan();
			appWindow.second->CancelWindowInteraction();
			appWindow.second->CancelAvisoViewportRender();
		}
	}
}

void CSMRRadar::QueueAvisoGeoJsonRasterRender(AvisoRasterRenderRequest request)
{
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;

	if (request.path.empty() ||
		request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0)
	{
		return;
	}

	EnsureAvisoGeoJsonRenderThread();
	if (IsShutdownRequested() || IsAvisoGeoJsonRenderStopRequested())
		return;

	bool shouldNotify = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		if (IsShutdownRequested() ||
			AvisoGeoJsonRenderStop.load(std::memory_order_relaxed) ||
			!AvisoGeoJsonRenderThreadStarted)
			return;

		const double longitudeTolerance = AvisoMax(
			std::abs(request.displayMaxLongitude - request.displayMinLongitude) /
				static_cast<double>((std::max)(request.rasterWidth, 1)) * 0.5,
			1e-10);
		const double latitudeTolerance = AvisoMax(
			std::abs(request.displayMaxLatitude - request.displayMinLatitude) /
				static_cast<double>((std::max)(request.rasterHeight, 1)) * 0.5,
			1e-10);
		const bool sameRequest =
			AvisoGeoJsonRenderLastRequestValid &&
			AvisoGeoJsonRenderLastRequestPath == request.path &&
			AvisoGeoJsonRenderLastRequestUseDayPalette == request.useDayPalette &&
			AvisoGeoJsonRenderLastRequestGroupGeneration == request.groupGeneration &&
			std::abs(AvisoGeoJsonRenderLastRequestRasterWidth - request.rasterWidth) <= 2 &&
			std::abs(AvisoGeoJsonRenderLastRequestRasterHeight - request.rasterHeight) <= 2 &&
			AvisoWithinTolerance(AvisoGeoJsonRenderLastRequestMinLongitude, request.displayMinLongitude, longitudeTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRenderLastRequestMinLatitude, request.displayMinLatitude, latitudeTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRenderLastRequestMaxLongitude, request.displayMaxLongitude, longitudeTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRenderLastRequestMaxLatitude, request.displayMaxLatitude, latitudeTolerance) &&
			AvisoPointWithinTolerance(AvisoGeoJsonRenderLastRequestProjectedTopLeft, request.projectedTopLeft, 0.75) &&
			AvisoPointWithinTolerance(AvisoGeoJsonRenderLastRequestProjectedTopRight, request.projectedTopRight, 0.75) &&
			AvisoPointWithinTolerance(AvisoGeoJsonRenderLastRequestProjectedBottomLeft, request.projectedBottomLeft, 0.75) &&
			AvisoPointWithinTolerance(AvisoGeoJsonRenderLastRequestProjectedBottomRight, request.projectedBottomRight, 0.75);
		if (sameRequest)
		{
			PerformanceDiagnostics.RecordAvisoRequestCoalesced(
				VsmrPerformance::AvisoViewport::Main);
			return;
		}

		request.requestId = ++AvisoGeoJsonRenderNextRequestId;
		request.performanceQueuedAtMilliseconds =
			VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
		if (request.debounceMilliseconds == 0 && AvisoGeoJsonRasterCache != nullptr)
			request.debounceMilliseconds = 24;
		request.cancellationToken = AvisoGeoJsonRenderCancellationToken;
		AvisoGeoJsonRenderLatestRequestId = request.requestId;
		AvisoGeoJsonRenderCancellationToken->store(request.requestId, std::memory_order_release);
		AvisoGeoJsonRenderLastRequestValid = true;
		AvisoGeoJsonRenderLastRequestPath = request.path;
		AvisoGeoJsonRenderLastRequestUseDayPalette = request.useDayPalette;
		AvisoGeoJsonRenderLastRequestMinLongitude = request.displayMinLongitude;
		AvisoGeoJsonRenderLastRequestMinLatitude = request.displayMinLatitude;
		AvisoGeoJsonRenderLastRequestMaxLongitude = request.displayMaxLongitude;
		AvisoGeoJsonRenderLastRequestMaxLatitude = request.displayMaxLatitude;
		AvisoGeoJsonRenderLastRequestRasterWidth = request.rasterWidth;
		AvisoGeoJsonRenderLastRequestRasterHeight = request.rasterHeight;
		AvisoGeoJsonRenderLastRequestGroupGeneration = request.groupGeneration;
		AvisoGeoJsonRenderLastRequestProjectedTopLeft = request.projectedTopLeft;
		AvisoGeoJsonRenderLastRequestProjectedTopRight = request.projectedTopRight;
		AvisoGeoJsonRenderLastRequestProjectedBottomLeft = request.projectedBottomLeft;
		AvisoGeoJsonRenderLastRequestProjectedBottomRight = request.projectedBottomRight;
		const bool supersededPendingRequest =
			AvisoGeoJsonPendingRenderRequest != nullptr || AvisoGeoJsonRenderInFlight;
		AvisoGeoJsonPendingRenderRequest = std::make_unique<AvisoRasterRenderRequest>(std::move(request));
		PerformanceDiagnostics.RecordAvisoRequestQueued(
			VsmrPerformance::AvisoViewport::Main,
			supersededPendingRequest);
		shouldNotify = true;
	}

	if (shouldNotify)
		AvisoGeoJsonRenderCondition.notify_one();
}

void CSMRRadar::ClearAvisoGeoJsonRasterCache()
{
	if (AvisoGeoJsonRasterCache != nullptr)
	{
		::DeleteObject(AvisoGeoJsonRasterCache);
		AvisoGeoJsonRasterCache = nullptr;
	}

	AvisoGeoJsonRasterCachePath.clear();
	AvisoGeoJsonRasterGroupGeneration = 0;
	AvisoGeoJsonRasterUseDayPalette = false;
	AvisoGeoJsonRasterMinLongitude = 0.0;
	AvisoGeoJsonRasterMinLatitude = 0.0;
	AvisoGeoJsonRasterMaxLongitude = 0.0;
	AvisoGeoJsonRasterMaxLatitude = 0.0;
	AvisoGeoJsonRasterWidth = 0;
	AvisoGeoJsonRasterHeight = 0;
	AvisoGeoJsonRasterAnchorLongitude = 0.0;
	AvisoGeoJsonRasterAnchorLatitude = 0.0;
	AvisoGeoJsonRasterBottomRightLongitude = 0.0;
	AvisoGeoJsonRasterBottomRightLatitude = 0.0;
	AvisoGeoJsonRasterProjectedTopLeft = PointF();
	AvisoGeoJsonRasterProjectedTopRight = PointF();
	AvisoGeoJsonRasterProjectedBottomLeft = PointF();
	AvisoGeoJsonRasterProjectedBottomRight = PointF();
	AvisoGeoJsonRasterAnchorValid = false;
}

void CSMRRadar::ApplyCompletedAvisoGeoJsonRaster()
{
	if (IsShutdownRequested())
		return;

	std::unique_ptr<AvisoRasterRenderResult> result;
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		result = std::move(AvisoGeoJsonCompletedRenderResult);
	}

	if (result == nullptr || result->bitmap == nullptr)
		return;

	bool resultApplied = false;
	{
		std::lock_guard<std::mutex> groupGuard(AvisoGroupMutex);
		if (result->groupGeneration == AvisoGroupGeneration.load(std::memory_order_relaxed))
		{
			ClearAvisoGeoJsonRasterCache();
			AvisoGeoJsonRasterCache = result->bitmap;
			result->bitmap = nullptr;
			AvisoGeoJsonRasterCachePath = result->path;
			AvisoGeoJsonRasterGroupGeneration = result->groupGeneration;
			AvisoGeoJsonRasterUseDayPalette = result->useDayPalette;
			AvisoGeoJsonRasterMinLongitude = result->displayMinLongitude;
			AvisoGeoJsonRasterMinLatitude = result->displayMinLatitude;
			AvisoGeoJsonRasterMaxLongitude = result->displayMaxLongitude;
			AvisoGeoJsonRasterMaxLatitude = result->displayMaxLatitude;
			AvisoGeoJsonRasterWidth = result->rasterWidth;
			AvisoGeoJsonRasterHeight = result->rasterHeight;
			AvisoGeoJsonRasterAnchorLongitude = result->renderMinLongitude;
			AvisoGeoJsonRasterAnchorLatitude = result->renderMaxLatitude;
			AvisoGeoJsonRasterBottomRightLongitude = result->renderMaxLongitude;
			AvisoGeoJsonRasterBottomRightLatitude = result->renderMinLatitude;
			AvisoGeoJsonRasterProjectedTopLeft = result->projectedTopLeft;
			AvisoGeoJsonRasterProjectedTopRight = result->projectedTopRight;
			AvisoGeoJsonRasterProjectedBottomLeft = result->projectedBottomLeft;
			AvisoGeoJsonRasterProjectedBottomRight = result->projectedBottomRight;
			AvisoGeoJsonRasterAnchorValid = true;
			resultApplied = true;
		}
	}
	if (resultApplied)
		PerformanceDiagnostics.RecordAvisoResultApplied(VsmrPerformance::AvisoViewport::Main);
	else
		PerformanceDiagnostics.RecordAvisoResultDiscarded(VsmrPerformance::AvisoViewport::Main);
}

void CSMRRadar::AvisoGeoJsonRenderThreadMain()
{
	VsmrCrashRuntime::OwnedThreadRole crashThreadRole("main AVISO render worker");
	try
	{
		for (;;)
		{
			std::unique_ptr<AvisoRasterRenderRequest> request;
			{
				std::unique_lock<std::mutex> lock(AvisoGeoJsonRenderMutex);
				AvisoGeoJsonRenderCondition.wait(lock, [&]() {
					return IsAvisoGeoJsonRenderStopRequested() || AvisoGeoJsonPendingRenderRequest != nullptr;
				});

				if (IsAvisoGeoJsonRenderStopRequested())
					return;
				while (AvisoGeoJsonPendingRenderRequest != nullptr &&
					AvisoGeoJsonPendingRenderRequest->debounceMilliseconds > 0)
				{
					const std::uint64_t observedRequestId =
						AvisoGeoJsonPendingRenderRequest->requestId;
					const std::uint64_t readyAt =
						AvisoGeoJsonPendingRenderRequest->performanceQueuedAtMilliseconds +
						AvisoGeoJsonPendingRenderRequest->debounceMilliseconds;
					const std::uint64_t now =
						VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
					if (now >= readyAt)
						break;
					AvisoGeoJsonRenderCondition.wait_for(
						lock,
						std::chrono::milliseconds(
							static_cast<long long>(readyAt - now)),
						[&]() {
							return IsAvisoGeoJsonRenderStopRequested() ||
								AvisoGeoJsonPendingRenderRequest == nullptr ||
								AvisoGeoJsonPendingRenderRequest->requestId != observedRequestId;
						});
					if (IsAvisoGeoJsonRenderStopRequested())
						return;
					if (AvisoGeoJsonPendingRenderRequest != nullptr &&
						AvisoGeoJsonPendingRenderRequest->requestId != observedRequestId)
					{
						PerformanceDiagnostics.RecordAvisoRequestDebounced(
							VsmrPerformance::AvisoViewport::Main);
						continue;
					}
					break;
				}

				request = std::move(AvisoGeoJsonPendingRenderRequest);
				AvisoGeoJsonRenderInFlight = request != nullptr;
			}

			if (request == nullptr)
				continue;
			VsmrCrashRuntime::RecordCurrentThreadCallback(
				"CSMRRadar::AvisoGeoJsonRenderThreadMain",
				reinterpret_cast<std::uintptr_t>(this));

			std::unique_ptr<AvisoRasterRenderResult> result;
			const std::uint64_t renderStartMilliseconds =
				VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
			const double queueWaitMilliseconds = request->performanceQueuedAtMilliseconds == 0
				? 0.0
				: static_cast<double>(renderStartMilliseconds - request->performanceQueuedAtMilliseconds);
			const auto renderStart = std::chrono::steady_clock::now();
			try
			{
				result = RenderAvisoGeoJsonRaster(*request);
			}
			catch (CException* ex)
			{
				if (ex != nullptr)
					ex->Delete();
				Logger::info("AVISO render worker caught MFC exception");
			}
			catch (const std::exception& ex)
			{
				Logger::info("AVISO render worker caught exception: " + std::string(ex.what()));
			}
			catch (...)
			{
				Logger::info("AVISO render worker caught unknown exception");
			}
			const double rebuildMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - renderStart).count();
			const bool renderCancelled =
				result == nullptr && IsAvisoRasterRenderRequestCancelled(*request);
			if (renderCancelled)
			{
				PerformanceDiagnostics.RecordAvisoRasterBuildCancelled(
					VsmrPerformance::AvisoViewport::Main);
			}
			else
			{
				PerformanceDiagnostics.RecordAvisoRasterBuild(
					VsmrPerformance::AvisoViewport::Main,
					rebuildMilliseconds,
					queueWaitMilliseconds,
					result != nullptr);
			}

			bool shouldRefresh = false;
			bool discardedResult = false;
			bool stopRequested = false;
			{
				std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
				if (result == nullptr &&
					!renderCancelled &&
					request->requestId == AvisoGeoJsonRenderLatestRequestId)
				{
					// A transient allocation/GDI failure must be retryable. Leaving the
					// request marked valid would coalesce every identical future frame.
					AvisoGeoJsonRenderLastRequestValid = false;
				}
				if (IsAvisoGeoJsonRenderStopRequested())
				{
					stopRequested = true;
					discardedResult = result != nullptr;
				}
				else if (result != nullptr && result->requestId == AvisoGeoJsonRenderLatestRequestId)
				{
					if (AvisoGeoJsonCompletedRenderResult != nullptr)
						discardedResult = true;
					AvisoGeoJsonCompletedRenderResult = std::move(result);
					shouldRefresh = true;
				}
				else if (result != nullptr)
				{
					discardedResult = true;
				}
				AvisoGeoJsonRenderInFlight = false;
			}
			if (discardedResult)
				PerformanceDiagnostics.RecordAvisoResultDiscarded(VsmrPerformance::AvisoViewport::Main);
			if (stopRequested)
				return;

			if (shouldRefresh)
				RequestRefreshFromWorker();
		}
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		Logger::info("AVISO render worker stopped after MFC exception");
	}
	catch (const std::exception& ex)
	{
		Logger::info("AVISO render worker stopped after exception: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("AVISO render worker stopped after unknown exception");
	}
	std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
	AvisoGeoJsonRenderInFlight = false;
}

void CSMRRadar::RequestRefreshFromWorker()
{
	if (IsShutdownRequested())
		return;

	const HWND hostWindow = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	const UINT refreshMessage = AvisoWorkerRefreshMessage();
	if (refreshMessage == 0 ||
		hostWindow == nullptr ||
		!::IsWindow(hostWindow))
		return;

	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::AvisoWorkerUpdate);
	if (!::PostMessage(
		hostWindow,
		refreshMessage,
		reinterpret_cast<WPARAM>(this),
		0))
	{
		Logger::info("AVISO render worker could not post a UI refresh request");
	}
}

bool CSMRRadar::IsAvisoRasterRenderRequestCancelled(
	const AvisoRasterRenderRequest& request) const noexcept
{
	return IsAvisoGeoJsonRenderStopRequested() ||
		request.groupGeneration != AvisoGroupGeneration.load(std::memory_order_relaxed) ||
		(request.cancellationToken != nullptr &&
			request.cancellationToken->load(std::memory_order_acquire) != request.requestId);
}

void CSMRRadar::MarkPerformanceRefreshReason(
	VsmrPerformance::FrameRefreshReason reason) noexcept
{
	const std::uint32_t reasonMask = VsmrPerformance::RefreshReasonMask(reason);
	if (reasonMask != 0)
		PendingPerformanceRefreshReasonMask.fetch_or(reasonMask, std::memory_order_relaxed);
}

std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> CSMRRadar::RenderAvisoGeoJsonRaster(const AvisoRasterRenderRequest& request) const
{
	if (request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0 ||
		request.rasterWidth > 6400 ||
		request.rasterHeight > 6400 ||
		static_cast<std::uint64_t>(request.rasterWidth) *
			static_cast<std::uint64_t>(request.rasterHeight) > 32000000ULL)
	{
		return nullptr;
	}

	auto renderCancelled = [&]() -> bool
	{
		return IsAvisoRasterRenderRequestCancelled(request);
	};
	if (renderCancelled())
		return nullptr;

	BITMAPINFO bitmapInfo = {};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = request.rasterWidth;
	bitmapInfo.bmiHeader.biHeight = -request.rasterHeight;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;

	void* dibBits = nullptr;
	ScopedHBitmap dibBitmap;
	dibBitmap.Reset(::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &dibBits, nullptr, 0));
	if (dibBitmap.Get() == nullptr || dibBits == nullptr)
		return nullptr;

	const int rasterStride = request.rasterWidth * 4;
	auto raster = std::make_unique<Bitmap>(
		request.rasterWidth,
		request.rasterHeight,
		rasterStride,
		PixelFormat32bppPARGB,
		static_cast<BYTE*>(dibBits));
	if (raster == nullptr || raster->GetLastStatus() != Ok)
		return nullptr;

	Graphics rasterGraphics(raster.get());
	if (rasterGraphics.GetLastStatus() != Ok)
		return nullptr;

	rasterGraphics.SetPageUnit(UnitPixel);
	rasterGraphics.Clear(Color(0, 0, 0, 0));
	rasterGraphics.SetSmoothingMode(SmoothingModeAntiAlias);
	rasterGraphics.SetPixelOffsetMode(PixelOffsetModeHalf);
	rasterGraphics.SetCompositingQuality(CompositingQualityHighSpeed);

	const double displayMinLon = request.displayMinLongitude;
	const double displayMaxLon = request.displayMaxLongitude;
	const double displayMinLat = request.displayMinLatitude;
	const double displayMaxLat = request.displayMaxLatitude;
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
		return nullptr;
	if (renderCancelled())
		return nullptr;
	const double centerLatitudeRadians = ((displayMinLat + displayMaxLat) * 0.5) * 3.14159265358979323846 / 180.0;
	const double metersPerPixelLon = (lonSpan * 111320.0 * std::cos(centerLatitudeRadians)) /
		AvisoMax(request.scaleX * lonSpan, 1.0);
	const double metersPerPixelLat = (latSpan * 110540.0) /
		AvisoMax(request.scaleY * latSpan, 1.0);
	const double metersPerPixel = AvisoMax(metersPerPixelLon, metersPerPixelLat);

	auto projectScreenPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(request.projectedTopLeft.X) + static_cast<double>(request.projectedTopRight.X - request.projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(request.projectedBottomLeft.X) + static_cast<double>(request.projectedBottomRight.X - request.projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(request.projectedTopLeft.Y) + static_cast<double>(request.projectedTopRight.Y - request.projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(request.projectedBottomLeft.Y) + static_cast<double>(request.projectedBottomRight.Y - request.projectedBottomLeft.Y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};

	const PointF rasterRenderTopLeft = projectScreenPoint(request.renderMinLongitude, request.renderMaxLatitude);
	const PointF rasterRenderTopRight = projectScreenPoint(request.renderMaxLongitude, request.renderMaxLatitude);
	const PointF rasterRenderBottomLeft = projectScreenPoint(request.renderMinLongitude, request.renderMinLatitude);
	const PointF rasterRenderBottomRight = projectScreenPoint(request.renderMaxLongitude, request.renderMinLatitude);
	const double rasterRenderLeft = AvisoMin(AvisoMin(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMin(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderTop = AvisoMin(AvisoMin(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMin(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderRight = AvisoMax(AvisoMax(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMax(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderBottom = AvisoMax(AvisoMax(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMax(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderWidth = rasterRenderRight - rasterRenderLeft;
	const double rasterRenderHeight = rasterRenderBottom - rasterRenderTop;
	if (rasterRenderWidth <= 0.0 || rasterRenderHeight <= 0.0)
		return nullptr;
	const double rasterCoordinateScaleX = static_cast<double>(request.rasterWidth) / rasterRenderWidth;
	const double rasterCoordinateScaleY = static_cast<double>(request.rasterHeight) / rasterRenderHeight;

	auto projectRasterPoint = [&](const AvisoPoint& coordinate) -> PointF
	{
		const PointF screenPoint = projectScreenPoint(coordinate.longitude, coordinate.latitude);
		const double x = (static_cast<double>(screenPoint.X) - rasterRenderLeft) * rasterCoordinateScaleX;
		const double y = (static_cast<double>(screenPoint.Y) - rasterRenderTop) * rasterCoordinateScaleY;
		return PointF(static_cast<REAL>(x), static_cast<REAL>(y));
	};

	const double minRasterPointDistance = AvisoMax(0.35 * request.rasterScale, 0.5);
	const double minRasterPointDistanceSquared = minRasterPointDistance * minRasterPointDistance;
	auto appendRasterPoint = [&](std::vector<PointF>& points, AvisoPoint& lastCoordinate, bool& hasLastCoordinate, const AvisoPoint& coordinate, bool force)
	{
		if (!force && hasLastCoordinate)
		{
			const double approxDx = (coordinate.longitude - lastCoordinate.longitude) * request.scaleX * rasterCoordinateScaleX;
			const double approxDy = (coordinate.latitude - lastCoordinate.latitude) * request.scaleY * rasterCoordinateScaleY;
			if ((approxDx * approxDx + approxDy * approxDy) < minRasterPointDistanceSquared)
				return;
		}

		const PointF point = projectRasterPoint(coordinate);
		if (!force && !points.empty())
		{
			const PointF& lastPoint = points.back();
			const double dx = static_cast<double>(point.X - lastPoint.X);
			const double dy = static_cast<double>(point.Y - lastPoint.Y);
			if ((dx * dx + dy * dy) < minRasterPointDistanceSquared)
				return;
		}

		points.push_back(point);
		lastCoordinate = coordinate;
		hasLastCoordinate = true;
	};

	std::vector<PointF> rasterPoints;
	for (const AvisoFeature& feature : *request.features)
	{
		if (renderCancelled())
			return nullptr;
		if (!IsAvisoGroupedItemVisible(feature.groupIds, request.groupVisibility.get()))
			continue;
		if (feature.minimumZoomLevel > 0 && request.viewportZoomLevel < feature.minimumZoomLevel)
			continue;

		Color featureFillColor = request.useDayPalette ? feature.dayFillColor : feature.fillColor;
		Color featureStrokeColor = request.useDayPalette ? feature.dayStrokeColor : feature.strokeColor;

		if (feature.maxLatitude < request.renderMinLatitude ||
			feature.minLatitude > request.renderMaxLatitude ||
			feature.maxLongitude < request.renderMinLongitude ||
			feature.minLongitude > request.renderMaxLongitude)
		{
			continue;
		}

		const double featurePixelWidth = (feature.maxLongitude - feature.minLongitude) * request.scaleX * rasterCoordinateScaleX;
		const double featurePixelHeight = (feature.maxLatitude - feature.minLatitude) * request.scaleY * rasterCoordinateScaleY;
		if (featurePixelWidth < 0.5 && featurePixelHeight < 0.5)
			continue;
		if (feature.polygon)
		{
			for (const std::vector<AvisoPoint>& ring : feature.paths)
			{
				if (renderCancelled())
					return nullptr;
				if (ring.size() < 3)
					continue;

				rasterPoints.clear();
				rasterPoints.reserve(ring.size());
				AvisoPoint lastCoordinate{};
				bool hasLastCoordinate = false;
				for (size_t pointIndex = 0; pointIndex < ring.size(); ++pointIndex)
				{
					if ((pointIndex & 0xff) == 0 && renderCancelled())
						return nullptr;
					appendRasterPoint(rasterPoints, lastCoordinate, hasLastCoordinate, ring[pointIndex], pointIndex == 0);
				}

				if (rasterPoints.size() < 3)
					continue;
				if (renderCancelled())
					return nullptr;

				if (featureFillColor.GetAlpha() > 0)
				{
					SolidBrush fillBrush(featureFillColor);
					rasterGraphics.FillPolygon(&fillBrush, rasterPoints.data(), static_cast<INT>(rasterPoints.size()), FillModeAlternate);
				}
			}
			continue;
		}

		if (featureStrokeColor.GetAlpha() == 0 || feature.strokeWidth <= 0.0f)
			continue;

		Pen linePen(featureStrokeColor, feature.strokeWidth * static_cast<float>(request.rasterScale));
		linePen.SetLineJoin(LineJoinRound);
		linePen.SetStartCap(LineCapRound);
		linePen.SetEndCap(LineCapRound);
		for (const std::vector<AvisoPoint>& line : feature.paths)
		{
			if (renderCancelled())
				return nullptr;
			if (line.size() < 2)
				continue;

			rasterPoints.clear();
			rasterPoints.reserve(line.size());
			AvisoPoint lastCoordinate{};
			bool hasLastCoordinate = false;
			for (size_t pointIndex = 0; pointIndex < line.size(); ++pointIndex)
			{
				if ((pointIndex & 0xff) == 0 && renderCancelled())
					return nullptr;
				appendRasterPoint(rasterPoints, lastCoordinate, hasLastCoordinate, line[pointIndex], pointIndex == 0 || pointIndex + 1 == line.size());
			}

			if (rasterPoints.size() >= 2)
			{
				if (renderCancelled())
					return nullptr;
				rasterGraphics.DrawLines(&linePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
			}
		}
	}

	auto isDenseLabelVisible = [&](const AvisoLabel& label) -> bool
	{
		if (label.minimumZoomLevel > 0 && request.viewportZoomLevel < label.minimumZoomLevel)
			return false;
		if (label.maxMetersPerPixel > 0.0 && metersPerPixel > label.maxMetersPerPixel)
			return false;
		return true;
	};
	auto labelRectForAnchor = [](const PointF& point, REAL widthPx, REAL heightPx, const std::string& anchor) -> RectF
	{
		const std::string normalizedAnchor = ToUpperAscii(anchor);
		REAL x = point.X - (widthPx * 0.5f);
		REAL y = point.Y - (heightPx * 0.5f);
		if (normalizedAnchor.find("LEFT") != std::string::npos)
			x = point.X;
		else if (normalizedAnchor.find("RIGHT") != std::string::npos)
			x = point.X - widthPx;
		if (normalizedAnchor.find("TOP") != std::string::npos)
			y = point.Y;
		else if (normalizedAnchor.find("BOTTOM") != std::string::npos)
			y = point.Y - heightPx;
		return RectF(x, y, widthPx, heightPx);
	};

	if (!request.labels->empty())
	{
		rasterGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		FontFamily fallbackLabelFontFamily(L"Arial");
		StringFormat labelFormat;
		labelFormat.SetAlignment(StringAlignmentCenter);
		labelFormat.SetLineAlignment(StringAlignmentCenter);
		labelFormat.SetFormatFlags(StringFormatFlagsNoWrap);
		auto getLabelEmSize = [&](float textSize) -> REAL
		{
			const float scaledSize = static_cast<float>(std::clamp(static_cast<double>(textSize * static_cast<float>(request.rasterScale)), 6.0, 40.0));
			const int fontKey = static_cast<int>(std::lround(static_cast<double>(scaledSize) * 10.0));
			return static_cast<REAL>(fontKey) / 10.0f;
		};

		for (const AvisoLabel& label : *request.labels)
		{
			if (renderCancelled())
				return nullptr;
			if (!IsAvisoGroupedItemVisible(label.groupIds, request.groupVisibility.get()))
				continue;

			const std::wstring* renderedText = &label.text;

			if (label.position.latitude < request.renderMinLatitude ||
				label.position.latitude > request.renderMaxLatitude ||
				label.position.longitude < request.renderMinLongitude ||
				label.position.longitude > request.renderMaxLongitude ||
				renderedText->empty() ||
				!isDenseLabelVisible(label))
			{
				continue;
			}

			const REAL labelEmSize = getLabelEmSize(label.textSize);
			const PointF labelPoint = projectRasterPoint(label.position);
			const REAL textLength = static_cast<REAL>(renderedText->length());
			const REAL scaledTextSize = static_cast<REAL>(label.textSize * static_cast<float>(request.rasterScale));
			const REAL haloPadding = static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale), 0.0) * 3.0);
			const REAL layoutWidth = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * AvisoMax(static_cast<double>(textLength), 1.0) * 0.9f + haloPadding * 2.0f), 14.0));
			const REAL layoutHeight = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * 1.65f + haloPadding * 2.0f), 10.0));
			const RectF layoutRect = labelRectForAnchor(labelPoint, layoutWidth, layoutHeight, label.textAnchor);
			FontFamily labelFontFamily(label.fontFamily.empty() ? L"Arial" : label.fontFamily.c_str());
			const FontFamily* fontFamily = labelFontFamily.GetLastStatus() == Ok ? &labelFontFamily : &fallbackLabelFontFamily;

			GraphicsPath textPath;
			textPath.AddString(
				renderedText->c_str(),
				static_cast<INT>(renderedText->length()),
				fontFamily,
				FontStyleRegular,
				labelEmSize,
				layoutRect,
				&labelFormat);

			if (textPath.GetPointCount() <= 0)
				continue;

			const Color labelHaloColor = request.useDayPalette ? label.dayHaloColor : label.haloColor;
			const Color labelTextColor = request.useDayPalette ? label.dayTextColor : label.textColor;
			if (label.haloWidth > 0.0f && labelHaloColor.GetAlpha() > 0)
			{
				Pen haloPen(labelHaloColor, static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * 2.0f), 1.0)));
				haloPen.SetLineJoin(LineJoinRound);
				rasterGraphics.DrawPath(&haloPen, &textPath);
			}

			if (labelTextColor.GetAlpha() > 0)
			{
				SolidBrush textBrush(labelTextColor);
				rasterGraphics.FillPath(&textBrush, &textPath);
			}
		}
	}

	if (renderCancelled())
		return nullptr;

	auto result = std::make_unique<AvisoRasterRenderResult>();
	result->requestId = request.requestId;
	result->groupGeneration = request.groupGeneration;
	result->useDayPalette = request.useDayPalette;
	result->bitmap = dibBitmap.Release();
	result->path = request.path;
	result->rasterWidth = request.rasterWidth;
	result->rasterHeight = request.rasterHeight;
	result->displayMinLongitude = request.displayMinLongitude;
	result->displayMinLatitude = request.displayMinLatitude;
	result->displayMaxLongitude = request.displayMaxLongitude;
	result->displayMaxLatitude = request.displayMaxLatitude;
	result->renderMinLongitude = request.renderMinLongitude;
	result->renderMinLatitude = request.renderMinLatitude;
	result->renderMaxLongitude = request.renderMaxLongitude;
	result->renderMaxLatitude = request.renderMaxLatitude;
	result->projectedTopLeft = request.projectedTopLeft;
	result->projectedTopRight = request.projectedTopRight;
	result->projectedBottomLeft = request.projectedBottomLeft;
	result->projectedBottomRight = request.projectedBottomRight;
	return result;
}

CRect CSMRRadar::ResolveMainAvisoRenderArea()
{
	CRect mainArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	mainArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		mainArea.bottom = chatArea.top;
	mainArea.NormalizeRect();
	if (mainArea.IsRectEmpty())
		return mainArea;

	LONG availableLeft = mainArea.left;
	LONG availableTop = mainArea.top;
	LONG availableRight = mainArea.right;
	LONG availableBottom = mainArea.bottom;
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		const auto windowIt = appWindows.find(display.first);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		const CInsetWindow* inset = windowIt->second.get();
		if (inset->IsTimer())
			continue;
		CRect insetArea(inset->m_Area);
		insetArea.NormalizeRect();
		switch (inset->m_AvisoLayoutMode)
		{
		case CInsetWindow::AvisoLayoutMode::SplitLeft:
			availableLeft = max(availableLeft, insetArea.right);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitRight:
			availableRight = min(availableRight, insetArea.left);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitTop:
			availableTop = max(availableTop, insetArea.bottom);
			break;
		case CInsetWindow::AvisoLayoutMode::SplitBottom:
			availableBottom = min(availableBottom, insetArea.top);
			break;
		default:
			continue;
		}
	}
	if (availableRight <= availableLeft || availableBottom <= availableTop)
		return CRect(0, 0, 0, 0);
	return CRect(availableLeft, availableTop, availableRight, availableBottom);
}

COLORREF CSMRRadar::GetAvisoBackgroundColor() const noexcept
{
	return AvisoUseDayColorPalette
		? AvisoDayBackgroundColor
		: AvisoNightBackgroundColor;
}

void CSMRRadar::RenderAvisoGeoJson(HDC hDC, Gdiplus::Graphics& graphics)
{
	if (IsShutdownRequested())
		return;

	const std::string path = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());
	if (path.empty())
		return;

	if (AvisoGeoJsonRenderDisabled)
	{
		bool sameFile = AvisoGeoJsonRenderDisabledPath.empty() || AvisoGeoJsonRenderDisabledPath == path;
		try
		{
			sameFile = sameFile && AvisoGeoJsonLoadedPath == path && AvisoGeoJsonLoadedWriteTime == fs::last_write_time(fs::u8path(path));
		}
		catch (...)
		{
			sameFile = true;
		}

		if (sameFile)
			return;

		AvisoGeoJsonRenderDisabled = false;
		AvisoGeoJsonRenderDisabledPath.clear();
	}

	if (!EnsureAvisoGeoJsonLoaded(path))
	{
		return;
	}
	CRect backgroundArea = ResolveMainAvisoRenderArea();
	backgroundArea.NormalizeRect();
	if (!backgroundArea.IsRectEmpty())
		CDC::FromHandle(hDC)->FillSolidRect(backgroundArea, GetAvisoBackgroundColor());
	if (AvisoGeoJsonFeatures.empty() && AvisoGeoJsonLabels.empty())
		return;
	std::shared_ptr<const std::vector<AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> labelSnapshot;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	unsigned long long groupGeneration = 0;
	if (!GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		groupGeneration))
	{
		return;
	}

	if (AvisoGeoJsonHasBounds && AvisoGeoJsonViewInitializedPath != path)
	{
		CPosition currentDisplayA;
		CPosition currentDisplayB;
		GetDisplayArea(&currentDisplayA, &currentDisplayB);

		const double displayMinLat = AvisoMin(currentDisplayA.m_Latitude, currentDisplayB.m_Latitude);
		const double displayMaxLat = AvisoMax(currentDisplayA.m_Latitude, currentDisplayB.m_Latitude);
		const double displayMinLon = AvisoMin(currentDisplayA.m_Longitude, currentDisplayB.m_Longitude);
		const double displayMaxLon = AvisoMax(currentDisplayA.m_Longitude, currentDisplayB.m_Longitude);
		const bool displayOverlapsAviso =
			AvisoGeoJsonMaxLatitude >= displayMinLat &&
			AvisoGeoJsonMinLatitude <= displayMaxLat &&
			AvisoGeoJsonMaxLongitude >= displayMinLon &&
			AvisoGeoJsonMinLongitude <= displayMaxLon;

		if (!displayOverlapsAviso)
		{
			const double latSpan = AvisoGeoJsonMaxLatitude - AvisoGeoJsonMinLatitude;
			const double lonSpan = AvisoGeoJsonMaxLongitude - AvisoGeoJsonMinLongitude;
			const double latPadding = AvisoMax(latSpan * 0.08, 0.001);
			const double lonPadding = AvisoMax(lonSpan * 0.08, 0.001);

			CPosition leftDown;
			leftDown.m_Latitude = AvisoGeoJsonMinLatitude - latPadding;
			leftDown.m_Longitude = AvisoGeoJsonMinLongitude - lonPadding;

			CPosition rightUp;
			rightUp.m_Latitude = AvisoGeoJsonMaxLatitude + latPadding;
			rightUp.m_Longitude = AvisoGeoJsonMaxLongitude + lonPadding;

			SetDisplayArea(leftDown, rightUp);
			Logger::info("AVISO GeoJSON fitted display path=" + path);
		}

		AvisoGeoJsonViewInitializedPath = path;
	}

	CPosition displayA;
	CPosition displayB;
	GetDisplayArea(&displayA, &displayB);

	const double fullDisplayMinLat = AvisoMin(displayA.m_Latitude, displayB.m_Latitude);
	const double fullDisplayMaxLat = AvisoMax(displayA.m_Latitude, displayB.m_Latitude);
	const double fullDisplayMinLon = AvisoMin(displayA.m_Longitude, displayB.m_Longitude);
	const double fullDisplayMaxLon = AvisoMax(displayA.m_Longitude, displayB.m_Longitude);
	const double fullDisplayLatSpan = fullDisplayMaxLat - fullDisplayMinLat;
	const double fullDisplayLonSpan = fullDisplayMaxLon - fullDisplayMinLon;
	if (fullDisplayLatSpan <= 0.0 || fullDisplayLonSpan <= 0.0)
		return;

	auto makeDisplayPosition = [](double latitude, double longitude) -> CPosition
	{
		CPosition position;
		position.m_Latitude = latitude;
		position.m_Longitude = longitude;
		return position;
	};

	// Keep one projection basis for the complete EuroScope view. The visible
	// main area may be cropped by a snapped inset, but rebuilding the basis from
	// pixel-to-coordinate-to-integer-pixel round trips makes the map twitch by a
	// pixel as the divider moves.
	const POINT fullProjectedTopLeft = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMaxLat, fullDisplayMinLon));
	const POINT fullProjectedTopRight = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon));
	const POINT fullProjectedBottomLeft = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon));
	const POINT fullProjectedBottomRight = ConvertCoordFromPositionToPixel(
		makeDisplayPosition(fullDisplayMinLat, fullDisplayMaxLon));
	auto projectFullDisplayPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - fullDisplayMinLon) / fullDisplayLonSpan;
		const double v = (fullDisplayMaxLat - latitude) / fullDisplayLatSpan;
		const double topX = static_cast<double>(fullProjectedTopLeft.x) +
			static_cast<double>(fullProjectedTopRight.x - fullProjectedTopLeft.x) * u;
		const double bottomX = static_cast<double>(fullProjectedBottomLeft.x) +
			static_cast<double>(fullProjectedBottomRight.x - fullProjectedBottomLeft.x) * u;
		const double topY = static_cast<double>(fullProjectedTopLeft.y) +
			static_cast<double>(fullProjectedTopRight.y - fullProjectedTopLeft.y) * u;
		const double bottomY = static_cast<double>(fullProjectedBottomLeft.y) +
			static_cast<double>(fullProjectedBottomRight.y - fullProjectedBottomLeft.y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};
	double displayMinLat = fullDisplayMinLat;
	double displayMaxLat = fullDisplayMaxLat;
	double displayMinLon = fullDisplayMinLon;
	double displayMaxLon = fullDisplayMaxLon;

	CRect fullRadarArea(GetRadarArea());
	CRect chatArea(GetChatArea());
	fullRadarArea.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		fullRadarArea.bottom = chatArea.top;
	fullRadarArea.NormalizeRect();
	CRect radarArea = ResolveMainAvisoRenderArea();
	if (radarArea.IsRectEmpty())
		return;

	if (radarArea != fullRadarArea)
	{
		const POINT visibleCorners[] = {
			{ radarArea.left, radarArea.top },
			{ radarArea.right, radarArea.top },
			{ radarArea.left, radarArea.bottom },
			{ radarArea.right, radarArea.bottom }
		};
		double visibleMinLat = DBL_MAX;
		double visibleMaxLat = -DBL_MAX;
		double visibleMinLon = DBL_MAX;
		double visibleMaxLon = -DBL_MAX;
		bool visibleBoundsValid = true;
		for (const POINT& corner : visibleCorners)
		{
			const CPosition position = ConvertCoordFromPixelToPosition(corner);
			if (!std::isfinite(position.m_Latitude) || !std::isfinite(position.m_Longitude))
			{
				visibleBoundsValid = false;
				break;
			}
			visibleMinLat = AvisoMin(visibleMinLat, position.m_Latitude);
			visibleMaxLat = AvisoMax(visibleMaxLat, position.m_Latitude);
			visibleMinLon = AvisoMin(visibleMinLon, position.m_Longitude);
			visibleMaxLon = AvisoMax(visibleMaxLon, position.m_Longitude);
		}
		if (visibleBoundsValid)
		{
			displayMinLat = AvisoMax(fullDisplayMinLat, visibleMinLat);
			displayMaxLat = AvisoMin(fullDisplayMaxLat, visibleMaxLat);
			displayMinLon = AvisoMax(fullDisplayMinLon, visibleMinLon);
			displayMaxLon = AvisoMin(fullDisplayMaxLon, visibleMaxLon);
		}
	}
	const double latSpan = displayMaxLat - displayMinLat;
	const double lonSpan = displayMaxLon - displayMinLon;
	if (latSpan <= 0.0 || lonSpan <= 0.0)
		return;

	const double width = static_cast<double>(radarArea.right - radarArea.left);
	const double height = static_cast<double>(radarArea.bottom - radarArea.top);
	if (width <= 0.0 || height <= 0.0)
		return;

	const double fallbackScaleX = width / lonSpan;
	const double fallbackScaleY = height / latSpan;

	const PointF projectedTopLeft = projectFullDisplayPoint(displayMinLon, displayMaxLat);
	const PointF projectedTopRight = projectFullDisplayPoint(displayMaxLon, displayMaxLat);
	const PointF projectedBottomLeft = projectFullDisplayPoint(displayMinLon, displayMinLat);
	const PointF projectedBottomRight = projectFullDisplayPoint(displayMaxLon, displayMinLat);

	const double projectedWidthTop = std::abs(static_cast<double>(projectedTopRight.X - projectedTopLeft.X));
	const double projectedWidthBottom = std::abs(static_cast<double>(projectedBottomRight.X - projectedBottomLeft.X));
	const double projectedHeightLeft = std::abs(static_cast<double>(projectedBottomLeft.Y - projectedTopLeft.Y));
	const double projectedHeightRight = std::abs(static_cast<double>(projectedBottomRight.Y - projectedTopRight.Y));
	const double projectedWidth = AvisoMax(AvisoMax(projectedWidthTop, projectedWidthBottom), 1.0);
	const double projectedHeight = AvisoMax(AvisoMax(projectedHeightLeft, projectedHeightRight), 1.0);
	const double scaleX = projectedWidth > 1.0 ? projectedWidth / lonSpan : fallbackScaleX;
	const double scaleY = projectedHeight > 1.0 ? projectedHeight / latSpan : fallbackScaleY;

	auto projectScreenPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(projectedTopLeft.X) + static_cast<double>(projectedTopRight.X - projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(projectedBottomLeft.X) + static_cast<double>(projectedBottomRight.X - projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(projectedTopLeft.Y) + static_cast<double>(projectedTopRight.Y - projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(projectedBottomLeft.Y) + static_cast<double>(projectedBottomRight.Y - projectedBottomLeft.Y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};

	const double viewPixelTolerance = 1.15;
	const double lonPixelTolerance = (1.0 / scaleX) * viewPixelTolerance;
	const double latPixelTolerance = (1.0 / scaleY) * viewPixelTolerance;
	const double transformPixelTolerance = 4.0;

	auto rasterCacheTransformMatchesCurrentView = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr || !AvisoGeoJsonRasterAnchorValid)
			return false;
		const double cachedLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedLonSpan <= 0.0 || cachedLatSpan <= 0.0 || lonSpan <= 0.0 || latSpan <= 0.0)
			return false;

		const double cachedHorizontalX = AvisoGeoJsonRasterProjectedTopRight.X - AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedHorizontalY = AvisoGeoJsonRasterProjectedTopRight.Y - AvisoGeoJsonRasterProjectedTopLeft.Y;
		const double cachedVerticalX = AvisoGeoJsonRasterProjectedBottomLeft.X - AvisoGeoJsonRasterProjectedTopLeft.X;
		const double cachedVerticalY = AvisoGeoJsonRasterProjectedBottomLeft.Y - AvisoGeoJsonRasterProjectedTopLeft.Y;
		const double currentHorizontalX = static_cast<double>(projectedTopRight.X - projectedTopLeft.X);
		const double currentHorizontalY = static_cast<double>(projectedTopRight.Y - projectedTopLeft.Y);
		const double currentVerticalX = static_cast<double>(projectedBottomLeft.X - projectedTopLeft.X);
		const double currentVerticalY = static_cast<double>(projectedBottomLeft.Y - projectedTopLeft.Y);

		// Zooming changes the geographic span, not the viewport's projection
		// basis. The cached raster can therefore remain geo-anchored while the
		// worker produces the definitive bitmap for the new scale.
		const bool sameViewportBasis =
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY, transformPixelTolerance);
		if (sameViewportBasis)
			return true;

		// A snapped-divider resize keeps the geographic pixel scale while changing
		// the visible span, so retain the existing span-normalized compatibility.
		const double horizontalSpanRatio = cachedLonSpan / lonSpan;
		const double verticalSpanRatio = cachedLatSpan / latSpan;
		return
			AvisoWithinTolerance(cachedHorizontalX, currentHorizontalX * horizontalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedHorizontalY, currentHorizontalY * horizontalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalX, currentVerticalX * verticalSpanRatio, transformPixelTolerance) &&
			AvisoWithinTolerance(cachedVerticalY, currentVerticalY * verticalSpanRatio, transformPixelTolerance);
	};

	auto cacheMatchesCurrentView = [&]() -> bool
	{
		return AvisoGeoJsonRasterCache != nullptr &&
			AvisoGeoJsonRasterCachePath == path &&
			AvisoGeoJsonRasterGroupGeneration == groupGeneration &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMinLongitude, displayMinLon, lonPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMinLatitude, displayMinLat, latPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMaxLongitude, displayMaxLon, lonPixelTolerance) &&
			AvisoWithinTolerance(AvisoGeoJsonRasterMaxLatitude, displayMaxLat, latPixelTolerance) &&
			rasterCacheTransformMatchesCurrentView();
	};

	auto drawRasterCacheTransformed = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr || AvisoGeoJsonRasterWidth <= 0 || AvisoGeoJsonRasterHeight <= 0)
			return false;
		if (AvisoGeoJsonRasterCachePath != path)
			return false;
		if (AvisoGeoJsonRasterGroupGeneration != groupGeneration)
			return false;
		if (!AvisoGeoJsonRasterAnchorValid)
			return false;
		if (!rasterCacheTransformMatchesCurrentView())
			return false;

		const double cachedViewportLonSpan = std::abs(AvisoGeoJsonRasterBottomRightLongitude - AvisoGeoJsonRasterAnchorLongitude);
		const double cachedViewportLatSpan = std::abs(AvisoGeoJsonRasterAnchorLatitude - AvisoGeoJsonRasterBottomRightLatitude);
		if (cachedViewportLonSpan <= 0.0 || cachedViewportLatSpan <= 0.0)
			return false;

		const PointF destTopLeft = projectScreenPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF destTopRight = projectScreenPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF destBottomLeft = projectScreenPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const PointF destBottomRight = projectScreenPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double destX = AvisoMin(AvisoMin(destTopLeft.X, destTopRight.X), AvisoMin(destBottomLeft.X, destBottomRight.X));
		const double destY = AvisoMin(AvisoMin(destTopLeft.Y, destTopRight.Y), AvisoMin(destBottomLeft.Y, destBottomRight.Y));
		const double destRight = AvisoMax(AvisoMax(destTopLeft.X, destTopRight.X), AvisoMax(destBottomLeft.X, destBottomRight.X));
		const double destBottom = AvisoMax(AvisoMax(destTopLeft.Y, destTopRight.Y), AvisoMax(destBottomLeft.Y, destBottomRight.Y));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		if (destRight < static_cast<double>(radarArea.left) ||
			destX > static_cast<double>(radarArea.right) ||
			destBottom < static_cast<double>(radarArea.top) ||
			destY > static_cast<double>(radarArea.bottom))
		{
			return false;
		}

		const double visibleLeft = AvisoMax(destX, static_cast<double>(radarArea.left));
		const double visibleTop = AvisoMax(destY, static_cast<double>(radarArea.top));
		const double visibleRight = AvisoMin(destRight, static_cast<double>(radarArea.right));
		const double visibleBottom = AvisoMin(destBottom, static_cast<double>(radarArea.bottom));
		const double visibleWidth = visibleRight - visibleLeft;
		const double visibleHeight = visibleBottom - visibleTop;
		if (visibleWidth < 1.0 || visibleHeight < 1.0)
			return false;

		const double sourceScaleX = static_cast<double>(AvisoGeoJsonRasterWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(AvisoGeoJsonRasterHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, AvisoGeoJsonRasterHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		// Keep the expanded integer source crop on the same transform as the
		// floating-point destination. Mapping independently rounded rectangles
		// shifts the cached preview by one or more scaled source pixels.
		const double alignedDestLeft = destX + (static_cast<double>(sourceXInt) / sourceScaleX);
		const double alignedDestTop = destY + (static_cast<double>(sourceYInt) / sourceScaleY);
		const double alignedDestRight = destX + (static_cast<double>(sourceRightInt) / sourceScaleX);
		const double alignedDestBottom = destY + (static_cast<double>(sourceBottomInt) / sourceScaleY);
		const int destLeft = static_cast<int>(std::lround(alignedDestLeft));
		const int destTop = static_cast<int>(std::lround(alignedDestTop));
		const int destRightInt = static_cast<int>(std::lround(alignedDestRight));
		const int destBottomInt = static_cast<int>(std::lround(alignedDestBottom));
		const int destWidthInt = destRightInt - destLeft;
		const int destHeightInt = destBottomInt - destTop;
		if (destWidthInt <= 0 || destHeightInt <= 0)
			return false;

		HDC sourceDc = ::CreateCompatibleDC(hDC);
		if (sourceDc == nullptr)
			return false;

		HGDIOBJ oldBitmap = ::SelectObject(sourceDc, AvisoGeoJsonRasterCache);
		if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR)
		{
			::DeleteDC(sourceDc);
			return false;
		}

		graphics.Flush(FlushIntentionFlush);
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
		{
			::SelectObject(sourceDc, oldBitmap);
			::DeleteDC(sourceDc);
			return false;
		}

		::IntersectClipRect(hDC, radarArea.left, radarArea.top, radarArea.right, radarArea.bottom);
		// AlphaBlend can introduce a one-pixel seam through the centre of a
		// near-native bitmap when only one side of the destination is rounded.
		// Keep each native axis strictly 1:1 and stretch only real zoom previews.
		const int blendDestWidth =
			std::abs(destWidthInt - sourceWidthInt) <= 1 ? sourceWidthInt : destWidthInt;
		const int blendDestHeight =
			std::abs(destHeightInt - sourceHeightInt) <= 1 ? sourceHeightInt : destHeightInt;
		const bool scaled =
			blendDestWidth != sourceWidthInt || blendDestHeight != sourceHeightInt;
		const int oldStretchMode = ::SetStretchBltMode(hDC, scaled ? HALFTONE : COLORONCOLOR);
		if (scaled)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			blendDestWidth,
			blendDestHeight,
			sourceDc,
			sourceXInt,
			sourceYInt,
			sourceWidthInt,
			sourceHeightInt,
			blend);

		if (oldStretchMode != 0)
			::SetStretchBltMode(hDC, oldStretchMode);
		::RestoreDC(hDC, savedDc);
		::SelectObject(sourceDc, oldBitmap);
		::DeleteDC(sourceDc);
		return blended != FALSE;
	};

	auto drawRasterCacheViewportAligned = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr ||
			AvisoGeoJsonRasterCachePath != path ||
			AvisoGeoJsonRasterUseDayPalette != AvisoUseDayColorPalette ||
			AvisoGeoJsonRasterWidth <= 0 ||
			AvisoGeoJsonRasterHeight <= 0 ||
			!AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		auto projectCachedPoint = [&](double longitude, double latitude) -> PointF
		{
			const double u = (longitude - AvisoGeoJsonRasterMinLongitude) / cachedDisplayLonSpan;
			const double v = (AvisoGeoJsonRasterMaxLatitude - latitude) / cachedDisplayLatSpan;
			const double topX = static_cast<double>(AvisoGeoJsonRasterProjectedTopLeft.X) + static_cast<double>(AvisoGeoJsonRasterProjectedTopRight.X - AvisoGeoJsonRasterProjectedTopLeft.X) * u;
			const double bottomX = static_cast<double>(AvisoGeoJsonRasterProjectedBottomLeft.X) + static_cast<double>(AvisoGeoJsonRasterProjectedBottomRight.X - AvisoGeoJsonRasterProjectedBottomLeft.X) * u;
			const double topY = static_cast<double>(AvisoGeoJsonRasterProjectedTopLeft.Y) + static_cast<double>(AvisoGeoJsonRasterProjectedTopRight.Y - AvisoGeoJsonRasterProjectedTopLeft.Y) * u;
			const double bottomY = static_cast<double>(AvisoGeoJsonRasterProjectedBottomLeft.Y) + static_cast<double>(AvisoGeoJsonRasterProjectedBottomRight.Y - AvisoGeoJsonRasterProjectedBottomLeft.Y) * u;
			return PointF(
				static_cast<REAL>(topX + (bottomX - topX) * v),
				static_cast<REAL>(topY + (bottomY - topY) * v));
		};

		const PointF cachedRenderTopLeft = projectCachedPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderTopRight = projectCachedPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterAnchorLatitude);
		const PointF cachedRenderBottomLeft = projectCachedPoint(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const PointF cachedRenderBottomRight = projectCachedPoint(AvisoGeoJsonRasterBottomRightLongitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double cachedRenderLeft = AvisoMin(AvisoMin(cachedRenderTopLeft.X, cachedRenderTopRight.X), AvisoMin(cachedRenderBottomLeft.X, cachedRenderBottomRight.X));
		const double cachedRenderTop = AvisoMin(AvisoMin(cachedRenderTopLeft.Y, cachedRenderTopRight.Y), AvisoMin(cachedRenderBottomLeft.Y, cachedRenderBottomRight.Y));
		const double cachedRenderRight = AvisoMax(AvisoMax(cachedRenderTopLeft.X, cachedRenderTopRight.X), AvisoMax(cachedRenderBottomLeft.X, cachedRenderBottomRight.X));
		const double cachedRenderBottom = AvisoMax(AvisoMax(cachedRenderTopLeft.Y, cachedRenderTopRight.Y), AvisoMax(cachedRenderBottomLeft.Y, cachedRenderBottomRight.Y));
		const double cachedRenderWidth = cachedRenderRight - cachedRenderLeft;
		const double cachedRenderHeight = cachedRenderBottom - cachedRenderTop;
		if (cachedRenderWidth < 1.0 || cachedRenderHeight < 1.0)
			return false;

		const PointF sourceTopLeft = projectCachedPoint(displayMinLon, displayMaxLat);
		const PointF sourceTopRight = projectCachedPoint(displayMaxLon, displayMaxLat);
		const PointF sourceBottomLeft = projectCachedPoint(displayMinLon, displayMinLat);
		const PointF sourceBottomRight = projectCachedPoint(displayMaxLon, displayMinLat);
		const double sourceScreenLeft = AvisoMin(AvisoMin(sourceTopLeft.X, sourceTopRight.X), AvisoMin(sourceBottomLeft.X, sourceBottomRight.X));
		const double sourceScreenTop = AvisoMin(AvisoMin(sourceTopLeft.Y, sourceTopRight.Y), AvisoMin(sourceBottomLeft.Y, sourceBottomRight.Y));
		const double sourceScreenRight = AvisoMax(AvisoMax(sourceTopLeft.X, sourceTopRight.X), AvisoMax(sourceBottomLeft.X, sourceBottomRight.X));
		const double sourceScreenBottom = AvisoMax(AvisoMax(sourceTopLeft.Y, sourceTopRight.Y), AvisoMax(sourceBottomLeft.Y, sourceBottomRight.Y));

		const double sourceScaleX = static_cast<double>(AvisoGeoJsonRasterWidth) / cachedRenderWidth;
		const double sourceScaleY = static_cast<double>(AvisoGeoJsonRasterHeight) / cachedRenderHeight;
		const double sourceX = (sourceScreenLeft - cachedRenderLeft) * sourceScaleX;
		const double sourceY = (sourceScreenTop - cachedRenderTop) * sourceScaleY;
		const double sourceRight = (sourceScreenRight - cachedRenderLeft) * sourceScaleX;
		const double sourceBottom = (sourceScreenBottom - cachedRenderTop) * sourceScaleY;
		const double sourceWidth = sourceRight - sourceX;
		const double sourceHeight = sourceBottom - sourceY;
		if (sourceWidth < 1.0 || sourceHeight < 1.0)
			return false;

		// A zoom preview must have the whole current viewport in the cached
		// overscan. Partial clamping would stretch the wrong geographic region.
		const double coverageTolerance = 1e-6;
		if (sourceX < -coverageTolerance ||
			sourceY < -coverageTolerance ||
			sourceRight > static_cast<double>(AvisoGeoJsonRasterWidth) + coverageTolerance ||
			sourceBottom > static_cast<double>(AvisoGeoJsonRasterHeight) + coverageTolerance)
		{
			return false;
		}

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceRight));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceBottom));
		sourceXInt = std::clamp(sourceXInt, 0, AvisoGeoJsonRasterWidth);
		sourceYInt = std::clamp(sourceYInt, 0, AvisoGeoJsonRasterHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, AvisoGeoJsonRasterWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, AvisoGeoJsonRasterHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		const double destX = AvisoMin(AvisoMin(static_cast<double>(projectedTopLeft.X), static_cast<double>(projectedTopRight.X)), AvisoMin(static_cast<double>(projectedBottomLeft.X), static_cast<double>(projectedBottomRight.X)));
		const double destY = AvisoMin(AvisoMin(static_cast<double>(projectedTopLeft.Y), static_cast<double>(projectedTopRight.Y)), AvisoMin(static_cast<double>(projectedBottomLeft.Y), static_cast<double>(projectedBottomRight.Y)));
		const double destRight = AvisoMax(AvisoMax(static_cast<double>(projectedTopLeft.X), static_cast<double>(projectedTopRight.X)), AvisoMax(static_cast<double>(projectedBottomLeft.X), static_cast<double>(projectedBottomRight.X)));
		const double destBottom = AvisoMax(AvisoMax(static_cast<double>(projectedTopLeft.Y), static_cast<double>(projectedTopRight.Y)), AvisoMax(static_cast<double>(projectedBottomLeft.Y), static_cast<double>(projectedBottomRight.Y)));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		// Expand the destination by exactly the amount used to round the source
		// crop. This keeps the zoom center fixed when AlphaBlend receives integers.
		const double destPerSourceX = destWidth / sourceWidth;
		const double destPerSourceY = destHeight / sourceHeight;
		const double alignedDestLeft = destX + (static_cast<double>(sourceXInt) - sourceX) * destPerSourceX;
		const double alignedDestTop = destY + (static_cast<double>(sourceYInt) - sourceY) * destPerSourceY;
		const double alignedDestRight = destX + (static_cast<double>(sourceRightInt) - sourceX) * destPerSourceX;
		const double alignedDestBottom = destY + (static_cast<double>(sourceBottomInt) - sourceY) * destPerSourceY;
		const int destLeft = static_cast<int>(std::lround(alignedDestLeft));
		const int destTop = static_cast<int>(std::lround(alignedDestTop));
		const int destRightInt = static_cast<int>(std::lround(alignedDestRight));
		const int destBottomInt = static_cast<int>(std::lround(alignedDestBottom));
		const int destWidthInt = destRightInt - destLeft;
		const int destHeightInt = destBottomInt - destTop;
		if (destWidthInt <= 0 || destHeightInt <= 0)
			return false;

		HDC sourceDc = ::CreateCompatibleDC(hDC);
		if (sourceDc == nullptr)
			return false;
		HGDIOBJ oldBitmap = ::SelectObject(sourceDc, AvisoGeoJsonRasterCache);
		if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR)
		{
			::DeleteDC(sourceDc);
			return false;
		}

		graphics.Flush(FlushIntentionFlush);
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
		{
			::SelectObject(sourceDc, oldBitmap);
			::DeleteDC(sourceDc);
			return false;
		}

		::IntersectClipRect(hDC, radarArea.left, radarArea.top, radarArea.right, radarArea.bottom);
		const int blendDestWidth =
			std::abs(destWidthInt - sourceWidthInt) <= 1 ? sourceWidthInt : destWidthInt;
		const int blendDestHeight =
			std::abs(destHeightInt - sourceHeightInt) <= 1 ? sourceHeightInt : destHeightInt;
		const bool scaled =
			blendDestWidth != sourceWidthInt || blendDestHeight != sourceHeightInt;
		const int oldStretchMode = ::SetStretchBltMode(hDC, scaled ? HALFTONE : COLORONCOLOR);
		if (scaled)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			blendDestWidth,
			blendDestHeight,
			sourceDc,
			sourceXInt,
			sourceYInt,
			sourceWidthInt,
			sourceHeightInt,
			blend);

		if (oldStretchMode != 0)
			::SetStretchBltMode(hDC, oldStretchMode);
		::RestoreDC(hDC, savedDc);
		::SelectObject(sourceDc, oldBitmap);
		::DeleteDC(sourceDc);
		return blended != FALSE;
	};

	auto rasterCacheHasCompatibleZoom = [&]() -> bool
	{
		if (AvisoGeoJsonRasterCache == nullptr ||
			AvisoGeoJsonRasterCachePath != path ||
			AvisoGeoJsonRasterGroupGeneration != groupGeneration ||
			!AvisoGeoJsonRasterAnchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = AvisoGeoJsonRasterMaxLongitude - AvisoGeoJsonRasterMinLongitude;
		const double cachedDisplayLatSpan = AvisoGeoJsonRasterMaxLatitude - AvisoGeoJsonRasterMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		return
			lonScaleRatio >= 0.985 && lonScaleRatio <= 1.015 &&
			latScaleRatio >= 0.985 && latScaleRatio <= 1.015;
	};

	auto rasterCacheHasWorkingMargin = [&]() -> bool
	{
		if (!rasterCacheHasCompatibleZoom())
			return false;

		const double cachedRenderMinLon = AvisoMin(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMaxLon = AvisoMax(AvisoGeoJsonRasterAnchorLongitude, AvisoGeoJsonRasterBottomRightLongitude);
		const double cachedRenderMinLat = AvisoMin(AvisoGeoJsonRasterAnchorLatitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double cachedRenderMaxLat = AvisoMax(AvisoGeoJsonRasterAnchorLatitude, AvisoGeoJsonRasterBottomRightLatitude);
		const double requiredLonMargin = lonSpan * 0.25;
		const double requiredLatMargin = latSpan * 0.25;
		return
			cachedRenderMinLon <= displayMinLon - requiredLonMargin &&
			cachedRenderMaxLon >= displayMaxLon + requiredLonMargin &&
			cachedRenderMinLat <= displayMinLat - requiredLatMargin &&
			cachedRenderMaxLat >= displayMaxLat + requiredLatMargin;
	};

	ApplyCompletedAvisoGeoJsonRaster();
	if (cacheMatchesCurrentView() && drawRasterCacheTransformed())
	{
		PerformanceDiagnostics.RecordAvisoCacheOutcome(
			VsmrPerformance::AvisoViewport::Main,
			VsmrPerformance::AvisoCacheOutcome::Exact,
			false,
			false);
		return;
	}
	auto avisoRasterUpdatePending = [&]() -> bool
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		return AvisoGeoJsonPendingRenderRequest != nullptr ||
			AvisoGeoJsonRenderInFlight ||
			AvisoGeoJsonCompletedRenderResult != nullptr;
	};

	// Half a viewport of overscan still doubles each raster dimension and
	// comfortably exceeds the 25% refresh margin, while bounding allocation.
	const double overscanRatio = 0.50;
	const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
	const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
	const double renderMinLat = displayMinLat - (latSpan * overscanRatio);
	const double renderMaxLat = displayMaxLat + (latSpan * overscanRatio);
	const PointF renderTopLeft = projectScreenPoint(renderMinLon, renderMaxLat);
	const PointF renderTopRight = projectScreenPoint(renderMaxLon, renderMaxLat);
	const PointF renderBottomLeft = projectScreenPoint(renderMinLon, renderMinLat);
	const PointF renderBottomRight = projectScreenPoint(renderMaxLon, renderMinLat);
	const double renderScreenLeft = AvisoMin(AvisoMin(renderTopLeft.X, renderTopRight.X), AvisoMin(renderBottomLeft.X, renderBottomRight.X));
	const double renderScreenTop = AvisoMin(AvisoMin(renderTopLeft.Y, renderTopRight.Y), AvisoMin(renderBottomLeft.Y, renderBottomRight.Y));
	const double renderScreenRight = AvisoMax(AvisoMax(renderTopLeft.X, renderTopRight.X), AvisoMax(renderBottomLeft.X, renderBottomRight.X));
	const double renderScreenBottom = AvisoMax(AvisoMax(renderTopLeft.Y, renderTopRight.Y), AvisoMax(renderBottomLeft.Y, renderBottomRight.Y));
	const double renderPixelWidth = renderScreenRight - renderScreenLeft;
	const double renderPixelHeight = renderScreenBottom - renderScreenTop;
	if (renderPixelWidth <= 0.0 || renderPixelHeight <= 0.0)
		return;

	const double maxDimension = renderPixelWidth > renderPixelHeight ? renderPixelWidth : renderPixelHeight;
	const double targetRasterScale = 1.0;
	const double maxRasterSide = 6400.0;
	const double maxRasterPixels = 32000000.0;
	double rasterScale = targetRasterScale;
	const double sideLimitedScale = maxRasterSide / maxDimension;
	if (sideLimitedScale > 0.0 && sideLimitedScale < rasterScale)
		rasterScale = sideLimitedScale;
	const double pixelLimitedScale = std::sqrt(maxRasterPixels / (renderPixelWidth * renderPixelHeight));
	if (pixelLimitedScale > 0.0 && pixelLimitedScale < rasterScale)
		rasterScale = pixelLimitedScale;
	// The side/pixel caps are hard bounds. Only very large viewports fall below
	// half resolution; allowing that is safer than defeating the allocation cap.
	rasterScale = AvisoMin(rasterScale, targetRasterScale);
	int rasterWidth = static_cast<int>(std::floor(renderPixelWidth * rasterScale));
	int rasterHeight = static_cast<int>(std::floor(renderPixelHeight * rasterScale));
	if (rasterWidth < 1)
		rasterWidth = 1;
	if (rasterHeight < 1)
		rasterHeight = 1;

	AvisoRasterRenderRequest request;
	request.groupGeneration = groupGeneration;
	request.path = path;
	request.features = featureSnapshot;
	request.labels = labelSnapshot;
	request.groupVisibility = groupVisibility;
	request.useDayPalette = AvisoUseDayColorPalette;
	request.rasterWidth = rasterWidth;
	request.rasterHeight = rasterHeight;
	request.rasterScale = rasterScale;
	request.displayMinLongitude = displayMinLon;
	request.displayMinLatitude = displayMinLat;
	request.displayMaxLongitude = displayMaxLon;
	request.displayMaxLatitude = displayMaxLat;
	request.renderMinLongitude = renderMinLon;
	request.renderMinLatitude = renderMinLat;
	request.renderMaxLongitude = renderMaxLon;
	request.renderMaxLatitude = renderMaxLat;
	request.renderScreenLeft = renderScreenLeft;
	request.renderScreenTop = renderScreenTop;
	request.scaleX = scaleX;
	request.scaleY = scaleY;
	request.viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(
		SMRGeometry::DistanceMeters(
			makeDisplayPosition(fullDisplayMinLat, fullDisplayMinLon),
			makeDisplayPosition(fullDisplayMaxLat, fullDisplayMaxLon)));
	request.projectedTopLeft = projectedTopLeft;
	request.projectedTopRight = projectedTopRight;
	request.projectedBottomLeft = projectedBottomLeft;
	request.projectedBottomRight = projectedBottomRight;

	// A divider resize changes the visible geographic span, but not the map's
	// pixel scale. Prefer the geo-anchored cache whenever its transform matches;
	// the compatibility/margin check still decides whether to refresh it.
	if (drawRasterCacheTransformed())
	{
		bool updateRequested = false;
		if (!rasterCacheHasWorkingMargin())
		{
			updateRequested = true;
			QueueAvisoGeoJsonRasterRender(std::move(request));
		}
		const bool delayedByAvisoUpdate = updateRequested && avisoRasterUpdatePending();
		PerformanceDiagnostics.RecordAvisoCacheOutcome(
			VsmrPerformance::AvisoViewport::Main,
			delayedByAvisoUpdate
				? VsmrPerformance::AvisoCacheOutcome::Preview
				: VsmrPerformance::AvisoCacheOutcome::Exact,
			delayedByAvisoUpdate,
			false);
		return;
	}

	QueueAvisoGeoJsonRasterRender(std::move(request));
	const bool fallbackCacheDrawn = drawRasterCacheViewportAligned();
	const bool delayedByAvisoUpdate = avisoRasterUpdatePending();
	PerformanceDiagnostics.RecordAvisoCacheOutcome(
		VsmrPerformance::AvisoViewport::Main,
		fallbackCacheDrawn
			? VsmrPerformance::AvisoCacheOutcome::Preview
			: VsmrPerformance::AvisoCacheOutcome::Miss,
		delayedByAvisoUpdate,
		!fallbackCacheDrawn);
	if (fallbackCacheDrawn)
		return;
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
