#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
#include "insets/InsetWindow.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <limits>
#include "rapidjson/document.h"
#include "aircraft/GroundState.hpp"
#include "profiles/ProfileColorPaths.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "tags/VacdmTagHelpers.hpp"
#include "profiles/ProfileEditorDialog.hpp"
#include "aviso/AvisoEditorDialog.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "plugin/Plugin.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "rdf/RdfOverlay.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"

extern std::vector<CSMRRadar*> RadarScreensOpened;

CPoint mouseLocation(0, 0);
string TagBeingDragged;
int LeaderLineDefaultlenght = 50;

// Cursor state shared by radar screen instances (managed on the UI thread).

bool initCursor = true;
HCURSOR smrCursor = NULL;
bool standardCursor; // True when the default arrow cursor is active.
bool customCursor; // True when the plugin-specific cursor theme is enabled.
std::map<HWND, WNDPROC> gInsetWindowSourceProcs;
std::map<HWND, CSMRRadar*> gInsetWindowRadarScreens;
CSMRRadar* gWindowProcRadarScreen = nullptr;
UINT AvisoWorkerRefreshMessage()
{
	static const UINT message =
		::RegisterWindowMessageA("vSMR.2.AvisoWorkerRefresh");
	return message;
}
void RestoreInsetWindowProcHooks();
HHOOK gThreadMouseHook = nullptr;
DWORD gThreadMouseHookThreadId = 0;
DWORD gLastThreadHookError = 0xFFFFFFFF;
HHOOK gThreadKeyboardHook = nullptr;
DWORD gThreadKeyboardHookThreadId = 0;
DWORD gLastThreadKeyboardHookError = 0xFFFFFFFF;
bool gAvisoWheelRoutingEnabled = false;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MouseMessageHookProc(int code, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK KeyboardMessageHookProc(int code, WPARAM wParam, LPARAM lParam);
void UnhookAvisoThreadHooks();

map<string, string> CSMRRadar::vStripsStands;

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
		return fs::path(dllPath) / "vSMR_Data";
	}

	std::string ResolvePluginDataDirectoryPath(const std::string& dllPath)
	{
		const fs::path dataDirectory = PluginDataDirectory(dllPath);
		if (IsDirectoryNoThrow(dataDirectory))
			return dataDirectory.string();
		return fs::path(dllPath).string();
	}

	std::string ResolvePluginFilePath(const std::string& dllPath, const char* fileName)
	{
		const fs::path dataPath = PluginDataDirectory(dllPath) / fileName;
		if (IsRegularFileNoThrow(dataPath))
			return dataPath.string();
		return (fs::path(dllPath) / fileName).string();
	}

	std::string ResolvePluginDirectoryPath(const std::string& dllPath, const char* directoryName)
	{
		const fs::path dataPath = PluginDataDirectory(dllPath) / directoryName;
		if (IsDirectoryNoThrow(dataPath))
			return dataPath.string();
		return (fs::path(dllPath) / directoryName).string();
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

	bool SupportsDynamicAvisoFrequencyOwnership(const std::string& airport)
	{
		// The renderer and ownership model are airport-agnostic. Enable the
		// feature only where its source data has been operationally reviewed.
		return ToUpperAscii(TrimAvisoAirportCode(airport)) == "LFPG";
	}

	void PushUniquePath(std::vector<fs::path>& paths, const fs::path& path)
	{
		if (path.empty())
			return;

		const std::string normalized = ToUpperAscii(path.lexically_normal().string());
		for (const fs::path& existing : paths)
		{
			if (ToUpperAscii(existing.lexically_normal().string()) == normalized)
				return;
		}

		paths.push_back(path);
	}

	std::vector<fs::path> BuildAvisoGeoJsonSearchDirectories(const std::string& dllPath, const std::string& dataPath)
	{
		std::vector<fs::path> directories;
		const fs::path pluginDirectory(dllPath);
		const fs::path resolvedDataDirectory = dataPath.empty() ? PluginDataDirectory(dllPath) : fs::path(dataPath);

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
			if (ToUpperAscii(path.extension().string()) != ".GEOJSON")
				continue;

			const std::string stem = ToUpperAscii(path.stem().string());
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

	CSMRRadar::AvisoFrequencyOwnershipMetadata ReadAvisoFrequencyOwnershipMetadata(
		const Value* properties)
	{
		CSMRRadar::AvisoFrequencyOwnershipMetadata metadata;
		if (properties == nullptr || !properties->IsObject())
			return metadata;

		const char* geometryRole = GetAvisoStringProperty(properties, { "geometry_role" });
		const char* dynamicRole = GetAvisoStringProperty(properties, { "dynamic_role" });
		const char* featureType = GetAvisoStringProperty(properties, { "feature_type" });
		const bool ownershipArea = geometryRole != nullptr &&
			ToUpperAscii(TrimAvisoAirportCode(geometryRole)) == "FREQUENCY_OWNERSHIP_AREA";
		const bool ownershipLabel =
			(featureType != nullptr &&
				ToUpperAscii(TrimAvisoAirportCode(featureType)) == "FREQUENCY_POINT") ||
			(dynamicRole != nullptr &&
				ToUpperAscii(TrimAvisoAirportCode(dynamicRole)).find("FREQUENCY") != std::string::npos);
		if (!ownershipArea && !ownershipLabel)
			return metadata;

		metadata.dynamicItem = true;
		if (const char* service = GetAvisoStringProperty(properties, { "service" }))
			metadata.service = ToUpperAscii(TrimAvisoAirportCode(service));
		if (const char* ownerKey = GetAvisoStringProperty(properties, { "owner_key" }))
			metadata.ownerKey = ToUpperAscii(TrimAvisoAirportCode(ownerKey));

		if (properties->HasMember("takeover_chain") && (*properties)["takeover_chain"].IsArray())
		{
			const Value& chain = (*properties)["takeover_chain"];
			for (SizeType index = 0; index < chain.Size(); ++index)
			{
				if (!chain[index].IsString())
					continue;
				const std::string key = ToUpperAscii(TrimAvisoAirportCode(chain[index].GetString()));
				if (!key.empty() &&
					std::find(metadata.takeoverChain.begin(), metadata.takeoverChain.end(), key) == metadata.takeoverChain.end())
				{
					metadata.takeoverChain.push_back(key);
				}
			}
		}

		if (metadata.service == "DEL" || metadata.takeoverChain.empty())
			return metadata;

		std::ostringstream key;
		key << metadata.service << '|' << metadata.ownerKey;
		for (const std::string& position : metadata.takeoverChain)
			key << '|' << position;
		metadata.ruleKey = key.str();
		return metadata;
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

	double ParseAvisoZoomRangeKm(const Value* sharedPaint, const Value* inlineProperties)
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
			return 0.0;

		const int level = static_cast<int>(std::lround(std::clamp(zoomValue->GetDouble(), 0.0, 14.0)));
		static const double maxViewRangeKmByLevel[] = {
			0.0, 34.0, 28.0, 22.0, 18.0,
			14.0, 12.0, 9.5, 8.0, 6.0,
			5.0, 4.0, 3.0, 2.5, 2.0
		};
		return maxViewRangeKmByLevel[level];
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

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

// Utility functions 
inline double closest(std::vector<double> const& vec, double value) {
	auto const it = std::lower_bound(vec.begin(), vec.end(), value);
	if (it == vec.end()) { return -1; }

	return *it;
};
inline bool IsTagBeingDragged(string c)
{
	return TagBeingDragged == c;
}
bool mouseWithin(CRect rect) {
	if (mouseLocation.x >= rect.left + 1 && mouseLocation.x <= rect.right - 1 && mouseLocation.y >= rect.top + 1 && mouseLocation.y <= rect.bottom - 1)
		return true;
	return false;
}

// ReSharper disable CppMsExtAddressOfClassRValue

CSMRRadar::CSMRRadar()
{

	Logger::info("CSMRRadar::CSMRRadar()");
	LOGFONT runtimeOverlayFont = {};
	runtimeOverlayFont.lfHeight = -11;
	runtimeOverlayFont.lfWeight = FW_BOLD;
	runtimeOverlayFont.lfCharSet = DEFAULT_CHARSET;
	runtimeOverlayFont.lfQuality = CLEARTYPE_QUALITY;
	runtimeOverlayFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
	strcpy_s(runtimeOverlayFont.lfFaceName, LF_FACESIZE, "Tahoma");
	RuntimeOverlayFont.CreateFontIndirect(&runtimeOverlayFont);
	LOGFONT runtimeMenuActionFont = runtimeOverlayFont;
	runtimeMenuActionFont.lfHeight = -10;
	runtimeMenuActionFont.lfWeight = FW_NORMAL;
	RuntimeMenuActionFont.CreateFontIndirect(&runtimeMenuActionFont);

	// Initializing randomizer
	srand(static_cast<unsigned>(time(nullptr)));

	// Initialize GDI+
	GdiplusStartupInput gdiplusStartupInput;
	const Gdiplus::Status gdiplusStatus = GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);
	if (gdiplusStatus != Gdiplus::Ok)
	{
		m_gdiplusToken = 0;
		Logger::info("CSMRRadar::CSMRRadar() GdiplusStartup failed status=" + std::to_string(static_cast<int>(gdiplusStatus)));
	}

	// Getting the DLL file folder
	GetModuleFileNameA(HINSTANCE(&__ImageBase), DllPathFile, sizeof(DllPathFile));
	DllPath = DllPathFile;
	DllPath.resize(DllPath.size() - strlen("vSMR.dll"));
	
	DataPath = ResolvePluginDataDirectoryPath(DllPath);
	ConfigPath = ResolvePluginFilePath(DllPath, "vSMR_Profiles.json");
	{
		const std::string sessionProfilesPath =
			CSMRPlugin::GetActiveProfilesConfigPath();
		std::error_code sessionPathError;
		if (!sessionProfilesPath.empty() &&
			fs::is_regular_file(fs::path(sessionProfilesPath), sessionPathError) &&
			!sessionPathError)
		{
			ConfigPath = fs::absolute(
				fs::path(sessionProfilesPath),
				sessionPathError).lexically_normal().string();
			if (sessionPathError)
				ConfigPath = sessionProfilesPath;
		}
	}
	mapsPath = ResolvePluginFilePath(DllPath, "vSMR_Maps.json");
	IconsPath = ResolvePluginDirectoryPath(DllPath, "aircraft_icons");
	LoadAircraftSpecs();

	Logger::info("Loading callsigns");

	// Creating the RIMCAS instance
	if (Callsigns == nullptr)
		Callsigns = std::make_unique<CCallsignLookup>();

	// We can look in three places for this file:
	// 1. Within the plugin directory
	// 2. In the ICAO folder of a GNG package
	// 3. In the working directory of EuroScope
	std::vector<fs::path> possible_paths;
	possible_paths.push_back(fs::path(DllPath) / "ICAO_Airlines.txt");
	possible_paths.push_back(fs::path(DllPath).parent_path().parent_path() / "ICAO" / "ICAO_Airlines.txt");
	possible_paths.push_back(fs::path(DllPath).parent_path().parent_path().parent_path() / "ICAO" / "ICAO_Airlines.txt");

	for (auto p : possible_paths) {
		Logger::info("Trying to read callsigns from: " + p.string());
		if (fs::exists(p)) {
			Logger::info("Found callsign file!");
			Callsigns->readFile(p.string());

			break;
		}
	};

	Logger::info("Loading RIMCAS & Config");
	// Creating the RIMCAS instance
	if (RimcasInstance == nullptr)
		RimcasInstance = std::make_unique<CRimcas>();

	// Loading up the config file
	if (CurrentConfig == nullptr)
		CurrentConfig = std::make_unique<CConfig>(ConfigPath, mapsPath);

	standardCursor = true;	
	ActiveAirport = "EGKK";
	const std::string avisoDefaultAirport = DetectDefaultAirportFromAviso();
	if (!avisoDefaultAirport.empty())
	{
		ActiveAirport = avisoDefaultAirport;
		Logger::info("CSMRRadar::CSMRRadar() default active airport from AVISO file=" + ActiveAirport);
	}

	// Set up the native inset windows.
	const int srwWindowId = APPWINDOW_ONE - APPWINDOW_BASE;
	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	appWindowDisplays[srwWindowId] = false;
	appWindowDisplays[avisoWindowId] = false;
	appWindowDisplays[weatherWindowId] = false;
	appWindowDisplays[timerWindowId] = false;
	appWindows[srwWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_ONE);
	appWindows[avisoWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_AVISO);
	appWindows[avisoWindowId]->m_Mode = CInsetWindow::Mode::AvisoViewport;
	appWindows[avisoWindowId]->m_Area = { 260, 260, 760, 560 };
	appWindows[weatherWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_WEATHER);
	appWindows[weatherWindowId]->m_Mode = CInsetWindow::Mode::Weather;
	appWindows[weatherWindowId]->m_Area = { 300, 200, 606, 375 };
	appWindows[timerWindowId] = std::make_unique<CInsetWindow>(APPWINDOW_TIMER);
	appWindows[timerWindowId]->m_Mode = CInsetWindow::Mode::Timer;
	appWindows[timerWindowId]->m_Area = { 100, 180, 184, 236 };

	Logger::info("Loading profile");

	this->CSMRRadar::LoadProfile("Default", false);

	this->CSMRRadar::LoadCustomFont();
	PublishCrashRadarState("main");

}

CSMRRadar::~CSMRRadar()
{
	PublishCrashRadarState("closing", "none");
	Logger::info(string(__FUNCSIG__));
	BeginShutdown();
	if (gWindowProcRadarScreen == this)
		gWindowProcRadarScreen = nullptr;
	UnhookAvisoThreadHooks();
	CloseVsmrControlCenterWindow();
	CloseAvisoEditorWindow();
	DestroyAvisoEditorWindow();
	CloseProfileEditorWindow(false);
	DestroyProfileEditorWindow();
	DestroyVsmrControlCenterWindow();
	try {
		//this->OnAsrContentToBeSaved();
		//this->EuroScopePlugInExitCustom();
	}
	catch (exception &e) {
		stringstream s;
		s << e.what() << endl;
		AfxMessageBox(string("Error occurred " + s.str()).c_str());
	}
	RadarScreensOpened.erase(std::remove(RadarScreensOpened.begin(), RadarScreensOpened.end(), this), RadarScreensOpened.end());
	for (auto radarIt = gInsetWindowRadarScreens.begin(); radarIt != gInsetWindowRadarScreens.end();)
	{
		if (radarIt->second == this)
			radarIt = gInsetWindowRadarScreens.erase(radarIt);
		else
			++radarIt;
	}
	if (RadarScreensOpened.empty())
		RestoreInsetWindowProcHooks();
	customFonts.clear();
	appWindows.clear();
	AircraftIcons.clear();
	RealisticIconBitmapCache.clear();
	ClearAvisoGeoJsonRasterCache();

	// Shutting down GDI+
	if (m_gdiplusToken != 0)
	{
		GdiplusShutdown(m_gdiplusToken);
		m_gdiplusToken = 0;
	}
	VsmrCrashReporter::ClearRadarState(
		reinterpret_cast<std::uintptr_t>(this));
}

void CSMRRadar::SamplePerformanceResourcesIfDue(bool force)
{
	const std::uint64_t now = VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
	if (!force && PerformanceLastResourceSampleMilliseconds != 0 &&
		now - PerformanceLastResourceSampleMilliseconds < 1000)
	{
		return;
	}
	PerformanceLastResourceSampleMilliseconds = now;

	VsmrPerformance::ResourceSample sample;
	sample.timestampMilliseconds = now;
	sample.processGdiObjects = static_cast<std::uint32_t>(
		::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	auto addBitmap = [&](Gdiplus::Bitmap* bitmap, std::size_t& categoryCount)
	{
		if (bitmap == nullptr)
			return;
		++categoryCount;
		++sample.ownedBitmapCount;
		sample.estimatedBitmapBytes += static_cast<std::uint64_t>(bitmap->GetWidth()) *
			static_cast<std::uint64_t>(bitmap->GetHeight()) * 4ULL;
	};
	for (const auto& aircraftIcon : AircraftIcons)
		addBitmap(aircraftIcon.second.get(), sample.aircraftBitmapCount);
	for (const auto& realisticIcon : RealisticIconBitmapCache)
		addBitmap(realisticIcon.second.bitmap.get(), sample.realisticIconBitmapCount);
	if (AvisoGeoJsonRasterCache != nullptr)
	{
		sample.mainAvisoBitmapCount = 1;
		++sample.ownedBitmapCount;
		if (AvisoGeoJsonRasterWidth > 0 && AvisoGeoJsonRasterHeight > 0)
		{
			sample.estimatedBitmapBytes += static_cast<std::uint64_t>(AvisoGeoJsonRasterWidth) *
				static_cast<std::uint64_t>(AvisoGeoJsonRasterHeight) * 4ULL;
		}
	}
	for (const auto& appWindow : appWindows)
	{
		if (appWindow.second == nullptr)
			continue;
		std::uint64_t estimatedBytes = 0;
		const std::size_t bitmapCount = appWindow.second->GetAvisoPerformanceBitmapCount(&estimatedBytes);
		sample.insetAvisoBitmapCount += bitmapCount;
		sample.ownedBitmapCount += bitmapCount;
		sample.estimatedBitmapBytes += estimatedBytes;
	}
	PerformanceDiagnostics.RecordResources(sample);
}

VsmrPerformance::Snapshot CSMRRadar::GetPerformanceSnapshot(
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints)
{
	SamplePerformanceResourcesIfDue();
	VsmrPerformance::AvisoQueueDepth mainQueue;
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		mainQueue.pending = AvisoGeoJsonPendingRenderRequest != nullptr ? 1U : 0U;
		mainQueue.inFlight = AvisoGeoJsonRenderInFlight ? 1U : 0U;
		mainQueue.completed = AvisoGeoJsonCompletedRenderResult != nullptr ? 1U : 0U;
		mainQueue.workers = AvisoGeoJsonRenderThreadStarted ? 1U : 0U;
	}
	VsmrPerformance::AvisoQueueDepth insetQueue;
	for (const auto& appWindow : appWindows)
	{
		if (appWindow.second == nullptr)
			continue;
		const VsmrPerformance::AvisoQueueDepth windowQueue =
			appWindow.second->GetAvisoPerformanceQueueDepth();
		insetQueue.pending += windowQueue.pending;
		insetQueue.inFlight += windowQueue.inFlight;
		insetQueue.completed += windowQueue.completed;
		insetQueue.workers += windowQueue.workers;
	}

	std::size_t realisticScaledEntries = 0;
	std::size_t realisticRotatedEntries = 0;
	for (const auto& item : RealisticIconBitmapCache)
	{
		if (item.first.rfind("s|", 0) == 0)
			++realisticScaledEntries;
		else if (item.first.rfind("r|", 0) == 0)
			++realisticRotatedEntries;
	}
	return PerformanceDiagnostics.GetSnapshot(
		windowSeconds,
		maximumSeriesPoints,
		mainQueue,
		insetQueue,
		AircraftIcons.size(),
		realisticScaledEntries,
		realisticRotatedEntries);
}

void CSMRRadar::ResetPerformanceDiagnostics()
{
	PerformanceDiagnostics.Reset();
	PerformanceLastResourceSampleMilliseconds = 0;
	SamplePerformanceResourcesIfDue(true);
}

std::string CSMRRadar::BuildPerformanceReportJson(
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints)
{
	const VsmrPerformance::Snapshot snapshot = GetPerformanceSnapshot(
		windowSeconds,
		maximumSeriesPoints);
	return VsmrPerformance::BuildJsonReport(
		snapshot,
		MY_PLUGIN_VERSION,
		getActiveAirport(),
		CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : std::string());
}

void CSMRRadar::PublishCrashRadarState(
	const char* radar,
	const char* inset) const noexcept
{
	char visibleInsets[96]{};
	if (inset == nullptr)
	{
		auto appendInset = [&](const char* name)
		{
			if (name == nullptr || name[0] == '\0')
				return;
			if (visibleInsets[0] != '\0')
				strcat_s(visibleInsets, ",");
			strcat_s(visibleInsets, name);
		};

		for (const auto& display : appWindowDisplays)
		{
			if (!display.second)
				continue;
			switch (display.first)
			{
			case APPWINDOW_ONE - APPWINDOW_BASE:
				appendInset("SRW1");
				break;
			case APPWINDOW_AVISO - APPWINDOW_BASE:
				appendInset("AVISO");
				break;
			case APPWINDOW_WEATHER - APPWINDOW_BASE:
				appendInset("WEATHER");
				break;
			case APPWINDOW_TIMER - APPWINDOW_BASE:
				appendInset("TIMER");
				break;
			default:
				appendInset("OTHER");
				break;
			}
		}
		if (visibleInsets[0] == '\0')
			strcpy_s(visibleInsets, "none");
		inset = visibleInsets;
	}

	const char* const airportValue = ActiveAirport.c_str();
	const char* const profileValue = CrashActiveProfile;
	const char* const radarValue = radar != nullptr ? radar : "unknown";
	const char* const insetValue = inset != nullptr ? inset : "unknown";
	if (strcmp(CrashLastAirport, airportValue) == 0 &&
		strcmp(CrashLastProfile, profileValue) == 0 &&
		strcmp(CrashLastRadar, radarValue) == 0 &&
		strcmp(CrashLastInset, insetValue) == 0)
	{
		return;
	}

	VsmrCrashReporter::RecordRadarState(
		reinterpret_cast<std::uintptr_t>(this),
		airportValue,
		profileValue,
		radarValue,
		insetValue);
	strncpy_s(CrashLastAirport, sizeof(CrashLastAirport), airportValue, _TRUNCATE);
	strncpy_s(CrashLastProfile, sizeof(CrashLastProfile), profileValue, _TRUNCATE);
	strncpy_s(CrashLastRadar, sizeof(CrashLastRadar), radarValue, _TRUNCATE);
	strncpy_s(CrashLastInset, sizeof(CrashLastInset), insetValue, _TRUNCATE);
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
		if (SupportsDynamicAvisoFrequencyOwnership(airportUpper))
		{
			for (const fs::path& searchDirectory : searchDirectories)
			{
				const fs::path dynamicPath = searchDirectory / (airportUpper + "_Dyna_fixed.geojson");
				if (IsRegularFileNoThrow(dynamicPath))
					return rememberResolvedPath(dynamicPath.string());
			}

			const std::string expectedDynamicStem = airportUpper + "_DYNA_FIXED";
			for (const fs::path& searchDirectory : searchDirectories)
			{
				if (!IsDirectoryNoThrow(searchDirectory))
					continue;
				for (const auto& entry : fs::directory_iterator(searchDirectory))
				{
					if (!entry.is_regular_file())
						continue;
					const fs::path path = entry.path();
					if (ToUpperAscii(path.extension().string()) == ".GEOJSON" &&
						ToUpperAscii(path.stem().string()) == expectedDynamicStem)
					{
						return rememberResolvedPath(path.string());
					}
				}
			}

			// Retain the beta.2 preview filename as a compatibility fallback for
			// manual installations that update only the DLL.
			for (const fs::path& searchDirectory : searchDirectories)
			{
				const fs::path legacyDynamicPath = searchDirectory / (airportUpper + "_Dyna.geojson");
				if (IsRegularFileNoThrow(legacyDynamicPath))
					return rememberResolvedPath(legacyDynamicPath.string());
			}
		}

		for (const fs::path& searchDirectory : searchDirectories)
		{
			const fs::path exactPath = searchDirectory / (airportUpper + ".geojson");
			if (IsRegularFileNoThrow(exactPath))
				return rememberResolvedPath(exactPath.string());
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
				if (ToUpperAscii(path.extension().string()) == ".GEOJSON" &&
					ToUpperAscii(path.stem().string()) == expectedStem)
				{
					return rememberResolvedPath(path.string());
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
				if (ToUpperAscii(path.extension().string()) == ".GEOJSON" &&
					ToUpperAscii(path.stem().string()) == legacyExpectedStem)
				{
					return rememberResolvedPath(path.string());
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
	bool retainPreviousOnFailure)
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
		writeTime = fs::last_write_time(path);
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
			if (owner == nullptr || sample == nullptr)
				return;
			sample->totalMilliseconds = AvisoMax(
				0.0,
				RefreshPerfNowMs() - startedMilliseconds);
			owner->LastAvisoLoadPerformance = *sample;
			try
			{
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
	std::ifstream input(path, std::ios::binary);
	if (!input.is_open())
	{
		loadPerformance.readMilliseconds = AvisoMax(
			0.0,
			RefreshPerfNowMs() - readStartMilliseconds);
		Logger::info("AVISO GeoJSON open failed path=" + path);
		return rememberFailedAttempt(&writeTime);
	}

	std::stringstream buffer;
	buffer << input.rdbuf();
	const std::string json = buffer.str();
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
			parsedLabel.frequencyOwnership = ReadAvisoFrequencyOwnershipMetadata(properties);
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
			parsedLabel.textSize = ParseAvisoFloatPropertyResolved(sharedPaint, properties, "text-size", 12.0f, 6.0f, 32.0f);
			parsedLabel.haloWidth = ParseAvisoFloatPropertyResolved(sharedPaint, properties, "text-halo-width", 1.0f, 0.0f, 6.0f);
			parsedLabel.maxViewRangeKm = ParseAvisoZoomRangeKm(sharedPaint, properties);
			parsedLabels.push_back(std::move(parsedLabel));
			++labelCount;
			continue;
		}

		AvisoFeature parsedFeature;
		parsedFeature.sourceFeatureIndex = static_cast<int>(i);
		parsedFeature.sourceFeatureId = sourceFeatureId;
		parsedFeature.groupIds = groupIds;
		parsedFeature.frequencyOwnership = ReadAvisoFrequencyOwnershipMetadata(properties);
		parsedFeature.fillColor = ParseAvisoColorResolved(sharedPaint, properties, "fill", "fill-opacity", Gdiplus::Color(217, 53, 66, 82));
		parsedFeature.strokeColor = ParseAvisoColorResolved(sharedPaint, properties, "stroke", "stroke-opacity", Gdiplus::Color(191, 140, 152, 170));
		parsedFeature.strokeWidth = ParseAvisoStrokeWidthResolved(sharedPaint, properties, 1.0f);
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
	auto frequencyOwnership =
		std::make_shared<AvisoFrequencyOwnershipSnapshot>();
	std::map<std::string, AvisoFrequencyOwnershipMetadata> frequencyOwnershipRules;
	std::set<std::string> frequencyOwnershipRelevantPositions;
	std::map<std::string, std::set<std::string>> frequencyOwnershipServicePositions;
	auto registerFrequencyOwnershipRule = [&](const AvisoFrequencyOwnershipMetadata& metadata)
	{
		if (!metadata.dynamicItem || metadata.ruleKey.empty())
			return;
		frequencyOwnershipRules.emplace(metadata.ruleKey, metadata);
		frequencyOwnershipRelevantPositions.insert(
			metadata.takeoverChain.begin(), metadata.takeoverChain.end());
	};
	for (const AvisoFeature& feature : parsedFeatures)
		registerFrequencyOwnershipRule(feature.frequencyOwnership);
	for (const AvisoLabel& label : parsedLabels)
		registerFrequencyOwnershipRule(label.frequencyOwnership);
	std::map<std::string, std::string> directOwnerServices;
	for (const auto& ruleEntry : frequencyOwnershipRules)
	{
		const AvisoFrequencyOwnershipMetadata& rule = ruleEntry.second;
		if (!rule.ownerKey.empty() && !rule.service.empty())
			directOwnerServices.emplace(rule.ownerKey, rule.service);
	}
	for (const auto& ruleEntry : frequencyOwnershipRules)
	{
		const AvisoFrequencyOwnershipMetadata& rule = ruleEntry.second;
		auto& servicePositions = frequencyOwnershipServicePositions[rule.service];
		for (const std::string& position : rule.takeoverChain)
		{
			const auto directOwner = directOwnerServices.find(position);
			if (directOwner != directOwnerServices.end() && directOwner->second != rule.service)
				break;
			servicePositions.insert(position);
		}
	}
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
		AvisoFrequencyOwnershipStateSnapshot = frequencyOwnership;
		AvisoFrequencyOwnershipRules = std::move(frequencyOwnershipRules);
		AvisoFrequencyOwnershipRelevantPositions = std::move(frequencyOwnershipRelevantPositions);
		AvisoFrequencyOwnershipServicePositions = std::move(frequencyOwnershipServicePositions);
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}
	AvisoFrequencyOwnershipLastRefreshTick = 0;
	AvisoFrequencyOwnershipLastSignature.clear();
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
	std::shared_ptr<const AvisoFrequencyOwnershipSnapshot>& outFrequencyOwnership,
	unsigned long long& outGeneration) const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	outFeatures = AvisoGeoJsonFeatureSnapshot;
	outLabels = AvisoGeoJsonLabelSnapshot;
	outGroupVisibility = AvisoGroupVisibilitySnapshot;
	outFrequencyOwnership = AvisoFrequencyOwnershipStateSnapshot;
	outGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	return outFeatures != nullptr && outLabels != nullptr &&
		outGroupVisibility != nullptr && outFrequencyOwnership != nullptr;
}

void CSMRRadar::RefreshAvisoFrequencyOwnership(
	bool force,
	const std::vector<VsmrScene::ControllerState>* capturedControllers)
{
	if (IsShutdownRequested() || !AvisoGeoJsonLoaded)
		return;

	const unsigned long nowTick = ::GetTickCount();
	const unsigned long refreshIntervalMs = 500;
	if (!force && AvisoFrequencyOwnershipLastRefreshTick != 0 &&
		(nowTick - AvisoFrequencyOwnershipLastRefreshTick) < refreshIntervalMs)
	{
		return;
	}
	AvisoFrequencyOwnershipLastRefreshTick = nowTick;

	try
	{
		const bool enabledForAirport =
			SupportsDynamicAvisoFrequencyOwnership(getActiveAirport());
		std::map<std::string, AvisoFrequencyOwnershipMetadata> rules;
		std::set<std::string> relevantPositions;
		std::map<std::string, std::set<std::string>> servicePositions;
		if (enabledForAirport)
		{
			std::lock_guard<std::mutex> guard(AvisoGroupMutex);
			rules = AvisoFrequencyOwnershipRules;
			relevantPositions = AvisoFrequencyOwnershipRelevantPositions;
			servicePositions = AvisoFrequencyOwnershipServicePositions;
		}

		struct ConnectedController
		{
			bool mine = false;
			double frequency = 0.0;
		};
		std::map<std::string, ConnectedController> connectedByPosition;
		auto registerControllerData = [&](
			const std::string& rawPosition,
			double primaryFrequency,
			bool mine)
		{
			if (rawPosition.empty())
				return;
			const std::string position = ToUpperAscii(TrimAvisoAirportCode(rawPosition));
			if (position.empty() || relevantPositions.find(position) == relevantPositions.end())
				return;
			const ConnectedController candidate{ mine, primaryFrequency };
			auto existing = connectedByPosition.find(position);
			if (existing == connectedByPosition.end() || mine)
				connectedByPosition[position] = candidate;
		};

		if (capturedControllers != nullptr)
		{
			for (const VsmrScene::ControllerState& controller : *capturedControllers)
			{
				registerControllerData(
					controller.positionId,
					controller.primaryFrequency,
					controller.mine);
			}
		}
		else
		{
			CPlugIn* plugin = GetPlugIn();
			if (plugin == nullptr)
				return;

			const CController myself = plugin->ControllerMyself();
			const std::string myCallsign = myself.IsValid() && myself.GetCallsign() != nullptr
				? ToUpperAscii(TrimAvisoAirportCode(myself.GetCallsign()))
				: std::string();
			const std::string myPosition = myself.IsValid() && myself.GetPositionId() != nullptr
				? ToUpperAscii(TrimAvisoAirportCode(myself.GetPositionId()))
				: std::string();
			auto registerController = [&](const CController& controller)
			{
				if (!controller.IsValid() || !controller.IsController())
					return;
				const std::string callsign = controller.GetCallsign() != nullptr ? controller.GetCallsign() : "";
				const std::string position = controller.GetPositionId() != nullptr ? controller.GetPositionId() : "";
				const std::string normalizedCallsign = ToUpperAscii(TrimAvisoAirportCode(callsign));
				const std::string normalizedPosition = ToUpperAscii(TrimAvisoAirportCode(position));
				const bool mine =
					(!myCallsign.empty() && normalizedCallsign == myCallsign) ||
					(!myPosition.empty() && normalizedPosition == myPosition);
				registerControllerData(position, controller.GetPrimaryFrequency(), mine);
			};

			CController controller = plugin->ControllerSelectFirst();
			for (size_t guard = 0; controller.IsValid() && guard < 4096; ++guard)
			{
				registerController(controller);
				controller = plugin->ControllerSelectNext(controller);
			}
			registerController(myself);
		}

		auto nextSnapshot = std::make_shared<AvisoFrequencyOwnershipSnapshot>();
		if (enabledForAirport)
		{
			const ConnectedController* connectedRmp = nullptr;
			std::string connectedRmpPosition;
			const auto rmpPositions = servicePositions.find("RMP");
			if (rmpPositions != servicePositions.end())
			{
				for (const std::string& position : rmpPositions->second)
				{
					const auto connected = connectedByPosition.find(position);
					if (connected == connectedByPosition.end())
						continue;
					if (connectedRmp == nullptr || connected->second.mine)
					{
						connectedRmp = &connected->second;
						connectedRmpPosition = position;
					}
					if (connected->second.mine)
						break;
				}
			}

			for (const auto& ruleEntry : rules)
			{
				const AvisoFrequencyOwnershipMetadata& rule = ruleEntry.second;
				if (rule.service == "RMP" && connectedRmp != nullptr)
				{
					AvisoFrequencyOwner owner;
					owner.connected = true;
					owner.ownedByMe = connectedRmp->mine;
					owner.useSourceFrequency = true;
					owner.positionId = connectedRmpPosition;
					nextSnapshot->ownersByRule.emplace(ruleEntry.first, std::move(owner));
					continue;
				}
				for (const std::string& position : rule.takeoverChain)
				{
					const auto connected = connectedByPosition.find(position);
					if (connected == connectedByPosition.end())
						continue;

					AvisoFrequencyOwner owner;
					owner.connected = true;
					owner.ownedByMe = connected->second.mine;
					owner.positionId = position;
					const double frequency = connected->second.frequency;
					if (std::isfinite(frequency) && frequency >= 100.0 && frequency < 190.0)
					{
						std::ostringstream formatted;
						formatted << std::fixed << std::setprecision(3) << frequency;
						const std::string text = formatted.str();
						owner.frequencyLabel.assign(text.begin(), text.end());
					}
					nextSnapshot->ownersByRule.emplace(ruleEntry.first, std::move(owner));
					break;
				}
			}
		}

		// Key the raster refresh to what is actually rendered. EuroScope can
		// publish frequent updates for unrelated controllers; including the
		// complete online list here caused the large AVISO bitmap to be cleared
		// and rebuilt even though no LFPG ownership had changed.
		std::ostringstream signature;
		signature << (enabledForAirport ? "LFPG|" : "DISABLED|") << AvisoGeoJsonLoadedPath;
		for (const auto& ruleEntry : rules)
		{
			signature << '|' << ruleEntry.first << '=';
			const auto ownership = nextSnapshot->ownersByRule.find(ruleEntry.first);
			if (ownership == nextSnapshot->ownersByRule.end() || !ownership->second.connected)
			{
				signature << "NONE";
				continue;
			}
			signature << ownership->second.positionId << ':'
				<< (ownership->second.ownedByMe ? 'M' : 'O') << ':'
				<< (ownership->second.useSourceFrequency ? 'S' : 'C') << ':';
			const std::wstring& frequency = ownership->second.frequencyLabel;
			for (const wchar_t character : frequency)
				signature << static_cast<char>(character);
		}
		const std::string nextSignature = signature.str();
		if (nextSignature == AvisoFrequencyOwnershipLastSignature)
			return;

		{
			std::lock_guard<std::mutex> guard(AvisoGroupMutex);
			AvisoFrequencyOwnershipStateSnapshot = nextSnapshot;
			AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		}
		AvisoFrequencyOwnershipLastSignature = nextSignature;
		InvalidateAvisoGroupRendering();
		Logger::info(
			"AVISO dynamic frequency ownership refreshed airport=" + getActiveAirport() +
			" controllers=" + std::to_string(connectedByPosition.size()) +
			" resolved_rules=" + std::to_string(nextSnapshot->ownersByRule.size()));
	}
	catch (const std::exception& ex)
	{
		Logger::info("AVISO dynamic frequency ownership refresh failed: " + std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("AVISO dynamic frequency ownership refresh failed: unknown exception");
	}
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

	struct DeferredOwnershipOutline
	{
		const AvisoFeature* feature = nullptr;
		Color strokeColor;
		bool ownedByMe = false;
	};
	std::vector<DeferredOwnershipOutline> deferredOwnershipOutlines;
	std::vector<PointF> rasterPoints;
	for (const AvisoFeature& feature : *request.features)
	{
		if (renderCancelled())
			return nullptr;
		if (!IsAvisoGroupedItemVisible(feature.groupIds, request.groupVisibility.get()))
			continue;

		Color featureFillColor = feature.fillColor;
		Color featureStrokeColor = feature.strokeColor;
		bool ownedByMe = false;
		if (feature.frequencyOwnership.dynamicItem)
		{
			if (feature.frequencyOwnership.ruleKey.empty() || request.frequencyOwnership == nullptr)
				continue;
			const auto ownership = request.frequencyOwnership->ownersByRule.find(
				feature.frequencyOwnership.ruleKey);
			if (ownership == request.frequencyOwnership->ownersByRule.end() ||
				!ownership->second.connected)
			{
				continue;
			}
			if (ownership->second.ownedByMe)
			{
				ownedByMe = true;
				// Keep inherited/self-owned territory visible but deliberately
				// quieter and blue, while foreign ownership retains the source
				// service color (yellow in the LFPG data set).
				featureStrokeColor = Color(featureStrokeColor.GetAlpha(), 79, 195, 247);
				featureFillColor = Color(featureFillColor.GetAlpha(), 79, 195, 247);
			}
		}

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
		const bool deferOwnershipOutline =
			feature.polygon && feature.frequencyOwnership.dynamicItem &&
			featureStrokeColor.GetAlpha() > 0 && feature.strokeWidth > 0.0f &&
			!AvisoColorsEqual(featureFillColor, featureStrokeColor);
		if (deferOwnershipOutline)
		{
			deferredOwnershipOutlines.push_back(
				DeferredOwnershipOutline{ &feature, featureStrokeColor, ownedByMe });
		}

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

				if (!deferOwnershipOutline && featureStrokeColor.GetAlpha() > 0 &&
					feature.strokeWidth > 0.0f &&
					!AvisoColorsEqual(featureFillColor, featureStrokeColor))
				{
					Pen outlinePen(featureStrokeColor, feature.strokeWidth * static_cast<float>(request.rasterScale));
					outlinePen.SetLineJoin(LineJoinRound);
					rasterGraphics.DrawPolygon(&outlinePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
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

	// Ownership polygons share many exact edges. Drawing each complete polygon
	// in source order lets a later cyan outline overwrite part of an external
	// yellow boundary (or vice versa). Paint every fill above, then draw all
	// self-owned outlines first and external outlines last. Shared boundaries
	// are consequently continuous and consistently identify the other owner.
	for (const bool drawSelfOwned : { true, false })
	{
		for (const DeferredOwnershipOutline& deferred : deferredOwnershipOutlines)
		{
			if (renderCancelled())
				return nullptr;
			if (deferred.feature == nullptr || deferred.ownedByMe != drawSelfOwned)
				continue;

			const AvisoFeature& feature = *deferred.feature;
			Pen outlinePen(
				deferred.strokeColor,
				feature.strokeWidth * static_cast<float>(request.rasterScale));
			outlinePen.SetLineJoin(LineJoinRound);
			for (const std::vector<AvisoPoint>& ring : feature.paths)
			{
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
					appendRasterPoint(
						rasterPoints,
						lastCoordinate,
						hasLastCoordinate,
						ring[pointIndex],
						pointIndex == 0);
				}
				if (rasterPoints.size() >= 3)
				{
					rasterGraphics.DrawPolygon(
						&outlinePen,
						rasterPoints.data(),
						static_cast<INT>(rasterPoints.size()));
				}
			}
		}
	}

	const double centerLatitudeRadians = ((displayMinLat + displayMaxLat) * 0.5) * 3.14159265358979323846 / 180.0;
	const double metersPerPixelLon = (lonSpan * 111320.0 * std::cos(centerLatitudeRadians)) / AvisoMax(request.scaleX * lonSpan, 1.0);
	const double metersPerPixelLat = (latSpan * 110540.0) / AvisoMax(request.scaleY * latSpan, 1.0);
	const double metersPerPixel = AvisoMax(metersPerPixelLon, metersPerPixelLat);
	const double horizontalViewKm = lonSpan * 111.320 * std::cos(centerLatitudeRadians);
	const double verticalViewKm = latSpan * 110.540;
	const double viewRangeKm = AvisoMax(horizontalViewKm, verticalViewKm) * 0.5;
	auto isDenseLabelVisible = [&](const AvisoLabel& label) -> bool
	{
		if (label.maxViewRangeKm > 0.0 && viewRangeKm > label.maxViewRangeKm)
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
			if (label.frequencyOwnership.dynamicItem)
			{
				if (label.frequencyOwnership.ruleKey.empty() || request.frequencyOwnership == nullptr)
					continue;
				const auto ownership = request.frequencyOwnership->ownersByRule.find(
					label.frequencyOwnership.ruleKey);
				if (ownership == request.frequencyOwnership->ownersByRule.end() ||
					!ownership->second.connected ||
					ownership->second.ownedByMe ||
					(!ownership->second.useSourceFrequency && ownership->second.frequencyLabel.empty()))
				{
					continue;
				}
				renderedText = ownership->second.useSourceFrequency
					? &label.text
					: &ownership->second.frequencyLabel;
			}

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

			if (label.haloWidth > 0.0f && label.haloColor.GetAlpha() > 0)
			{
				Pen haloPen(label.haloColor, static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * 2.0f), 1.0)));
				haloPen.SetLineJoin(LineJoinRound);
				rasterGraphics.DrawPath(&haloPen, &textPath);
			}

			if (label.textColor.GetAlpha() > 0)
			{
				SolidBrush textBrush(label.textColor);
				rasterGraphics.FillPath(&textBrush, &textPath);
			}
		}
	}

	if (renderCancelled())
		return nullptr;

	auto result = std::make_unique<AvisoRasterRenderResult>();
	result->requestId = request.requestId;
	result->groupGeneration = request.groupGeneration;
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
			sameFile = sameFile && AvisoGeoJsonLoadedPath == path && AvisoGeoJsonLoadedWriteTime == fs::last_write_time(path);
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

	if (!EnsureAvisoGeoJsonLoaded(path) ||
		(AvisoGeoJsonFeatures.empty() && AvisoGeoJsonLabels.empty()))
	{
		return;
	}
	std::shared_ptr<const std::vector<AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> labelSnapshot;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	std::shared_ptr<const AvisoFrequencyOwnershipSnapshot> frequencyOwnership;
	unsigned long long groupGeneration = 0;
	if (!GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		frequencyOwnership,
		groupGeneration))
	{
		return;
	}
	if (const VsmrScene::RadarScene* scene = GetCurrentRadarScene();
		scene != nullptr && scene->avisoGeneration == groupGeneration)
	{
		frequencyOwnership = scene->frequencyOwnership;
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
		const bool nearNativeScale =
			std::abs(static_cast<double>(destWidthInt - sourceWidthInt)) <= 1.0 &&
			std::abs(static_cast<double>(destHeightInt - sourceHeightInt)) <= 1.0;
		const int oldStretchMode = ::SetStretchBltMode(hDC, nearNativeScale ? COLORONCOLOR : HALFTONE);
		if (!nearNativeScale)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			destWidthInt,
			destHeightInt,
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
		const bool nearNativeScale =
			std::abs(static_cast<double>(destWidthInt - sourceWidthInt)) <= 1.0 &&
			std::abs(static_cast<double>(destHeightInt - sourceHeightInt)) <= 1.0;
		const int oldStretchMode = ::SetStretchBltMode(hDC, nearNativeScale ? COLORONCOLOR : HALFTONE);
		if (!nearNativeScale)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			destWidthInt,
			destHeightInt,
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
	request.frequencyOwnership = frequencyOwnership;
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

void CSMRRadar::LoadCustomFont() {
	Logger::info(string(__FUNCSIG__));
	// Loading the custom font if there is one in use
	customFonts.clear();

	std::string fontName = GetActiveTagFontName();
	if (fontName.empty())
		fontName = "EuroScope";

	const Value& profile = CurrentConfig->getActiveProfile();
	const Value* sizeConfig = nullptr;
	const Value* weightConfig = nullptr;
	if (profile.IsObject() && profile.HasMember("font") && profile["font"].IsObject())
	{
		const Value& font = profile["font"];
		if (font.HasMember("sizes") && font["sizes"].IsObject())
			sizeConfig = &font["sizes"];
		if (font.HasMember("weight") && font["weight"].IsString())
			weightConfig = &font["weight"];
	}

	auto getFontSize = [&](const char* key, int fallback) -> int
	{
		if (sizeConfig && sizeConfig->HasMember(key) && (*sizeConfig)[key].IsInt())
		{
			int configured = (*sizeConfig)[key].GetInt();
			return (configured < 6) ? 6 : configured;
		}
		return fallback;
	};

	const int sizeOne = getFontSize("one", 10);
	const int sizeTwo = getFontSize("two", 11);
	const int sizeThree = getFontSize("three", 12);
	const int sizeFour = getFontSize("four", 13);
	const int sizeFive = getFontSize("five", 14);

	std::wstring buffer = std::wstring(fontName.begin(), fontName.end());
	Gdiplus::FontStyle fontStyle = Gdiplus::FontStyleRegular;
	if (weightConfig && strcmp(weightConfig->GetString(), "Bold") == 0)
		fontStyle = Gdiplus::FontStyleBold;
	if (weightConfig && strcmp(weightConfig->GetString(), "Italic") == 0)
		fontStyle = Gdiplus::FontStyleItalic;

	auto createFont = [&](int size) -> std::unique_ptr<Gdiplus::Font>
	{
		std::unique_ptr<Gdiplus::Font> font = std::make_unique<Gdiplus::Font>(buffer.c_str(), Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		if (font->GetLastStatus() != Gdiplus::Ok)
		{
			font = std::make_unique<Gdiplus::Font>(L"Arial", Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		}
		return font;
	};

	customFonts[1] = createFont(sizeOne);
	customFonts[2] = createFont(sizeTwo);
	customFonts[3] = createFont(sizeThree);
	customFonts[4] = createFont(sizeFour);
	customFonts[5] = createFont(sizeFive);
}

bool CSMRRadar::SetProfilesConfigPath(
	const std::string& path,
	std::string* errorText,
	bool persistToAsr)
{
	if (errorText != nullptr)
		errorText->clear();

	auto normalizeExistingPath = [](const std::string& value, std::string& result) -> bool
	{
		result.clear();
		std::error_code pathError;
		const fs::path normalized = fs::absolute(fs::path(value), pathError).lexically_normal();
		if (pathError || normalized.empty() ||
			!fs::is_regular_file(normalized, pathError) || pathError)
		{
			return false;
		}
		result = normalized.string();
		return true;
	};

	bool sessionSelectionClaimed = false;
	const std::string sessionPath =
		CSMRPlugin::GetActiveProfilesConfigPath(&sessionSelectionClaimed);
	std::string normalizedSessionPath;
	const bool sessionPathAvailable =
		normalizeExistingPath(sessionPath, normalizedSessionPath);

	// Explicit Control Center selection always replaces the session source.
	// ASR restoration is first-screen-wins: once one valid path has claimed the
	// session, a later screen adopts it instead of switching every existing view
	// back to a stale per-screen value.
	const bool explicitSelection = persistToAsr;
	std::string normalizedRequestedPath;
	const bool requestedPathAvailable =
		normalizeExistingPath(path, normalizedRequestedPath);
	std::string normalizedPath;
	if (!explicitSelection && sessionSelectionClaimed && sessionPathAvailable)
	{
		normalizedPath = normalizedSessionPath;
	}
	else if (requestedPathAvailable)
	{
		normalizedPath = normalizedRequestedPath;
	}
	else
	{
		if (!explicitSelection && sessionPathAvailable)
		{
			normalizedPath = normalizedSessionPath;
		}
		else
		{
			if (errorText != nullptr)
				*errorText = "The selected profiles file is no longer available.";
			return false;
		}
	}
	const bool publishSessionSelection =
		explicitSelection ||
		(requestedPathAvailable &&
			(!sessionSelectionClaimed || !sessionPathAvailable));

	std::vector<CSMRRadar*> targets;
	if (publishSessionSelection)
	{
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar != nullptr &&
				std::find(targets.begin(), targets.end(), radar) == targets.end())
			{
				targets.push_back(radar);
			}
		}
	}
	if (std::find(targets.begin(), targets.end(), this) == targets.end())
		targets.insert(targets.begin(), this);

	struct Replacement
	{
		CSMRRadar* radar = nullptr;
		std::unique_ptr<CConfig> config;
	};
	std::vector<Replacement> replacements;
	replacements.reserve(targets.size());
	try
	{
		for (CSMRRadar* radar : targets)
		{
			auto config = std::make_unique<CConfig>(normalizedPath, radar->mapsPath);
			if (config->getProfileCount() == 0)
			{
				if (errorText != nullptr)
					*errorText = "The selected profiles file contains no usable profiles.";
				return false;
			}
			replacements.push_back({ radar, std::move(config) });
		}
	}
	catch (const std::exception& error)
	{
		if (errorText != nullptr)
			*errorText = "Unable to load the selected profiles file: " + std::string(error.what());
		return false;
	}
	catch (...)
	{
		if (errorText != nullptr)
			*errorText = "Unable to load the selected profiles file.";
		return false;
	}

	CConfig* primaryConfig = nullptr;
	for (const Replacement& replacement : replacements)
	{
		if (replacement.radar == this)
		{
			primaryConfig = replacement.config.get();
			break;
		}
	}
	if (primaryConfig == nullptr)
		return false;

	const std::vector<std::string> profileNames = primaryConfig->getAllProfiles();
	std::string activeProfile = primaryConfig->getLastActiveProfileName();
	if (activeProfile.empty() ||
		std::find_if(profileNames.begin(), profileNames.end(), [&](const std::string& candidate) {
			return _stricmp(candidate.c_str(), activeProfile.c_str()) == 0;
		}) == profileNames.end())
	{
		const std::string currentProfile = CurrentConfig != nullptr
			? CurrentConfig->getActiveProfileName()
			: std::string();
		const auto currentMatch = std::find_if(profileNames.begin(), profileNames.end(), [&](const std::string& candidate) {
			return _stricmp(candidate.c_str(), currentProfile.c_str()) == 0;
		});
		activeProfile = currentMatch != profileNames.end()
			? *currentMatch
			: profileNames.front();
	}

	for (Replacement& replacement : replacements)
	{
		CSMRRadar* radar = replacement.radar;
		radar->ConfigPath = normalizedPath;
		radar->CurrentConfig = std::move(replacement.config);
		radar->LoadProfile(activeProfile, false);
		radar->InvalidateAirportPositionCache();
		radar->InvalidateRunwayGeometryCache();
		radar->RadarViewZoomLevel = -1;
		radar->LastMapRunwayStatuses.clear();
		radar->LastMapActiveAirport.clear();
		if (publishSessionSelection || radar == this)
		{
			radar->SaveDataToAsr(
				"ProfilesFile",
				"Active vSMR profiles file",
				radar->ConfigPath.c_str());
		}
		radar->RequestRefresh();
	}
	if (publishSessionSelection)
		CSMRPlugin::PublishActiveProfilesConfigPath(normalizedPath, true);
	for (CSMRRadar* radar : targets)
	{
		if (radar != this &&
			radar->VsmrControlCenterDialog != nullptr)
		{
			radar->VsmrControlCenterDialog->SyncFromRadar(
				publishSessionSelection ? "resource-source" : "runtime");
		}
	}
	RememberSessionActiveProfile(activeProfile);
	return true;
}

bool CSMRRadar::ReloadConfig() {
	Logger::info("CSMRRadar::ReloadConfig()");
	std::string activeProfile = CurrentConfig ? CurrentConfig->getActiveProfileName() : "Default";
	bool reloadSucceeded = true;
	if (!CurrentConfig)
	{
		CurrentConfig = std::make_unique<CConfig>(ConfigPath, mapsPath);
		reloadSucceeded = CurrentConfig->getProfileCount() > 0;
	}
	else {
		reloadSucceeded = CurrentConfig->reload();
	}
	if (activeProfile.empty())
		activeProfile = "Default";
	if (CurrentConfig->isItActiveProfile(activeProfile) == 0 && !CurrentConfig->getAllProfiles().empty()) {
		activeProfile = CurrentConfig->getAllProfiles().front();
	}
	// A reload adopts disk as the authority. Recording the outgoing runtime
	// alerts into the freshly loaded document would give each radar a divergent
	// in-memory copy carrying the new revision token, allowing a later unrelated
	// save to overwrite the authoritative alert state.
	this->LoadProfile(activeProfile, false);
	// Force map visibility recomputation on next frame even when zoom level is unchanged.
	InvalidateAirportPositionCache();
	InvalidateRunwayGeometryCache();
	RadarViewZoomLevel = -1;
	LastMapRunwayStatuses.clear();
	LastMapActiveAirport.clear();
	AvisoGeoJsonResolvedAirport.clear();
	AvisoGeoJsonResolvedDllPath.clear();
	AvisoGeoJsonResolvedPath.clear();
	AvisoGeoJsonLastStatTick = 0;
	RequestRefresh();
	return reloadSucceeded;
}

void CSMRRadar::LoadProfile(
	string profileName,
	bool saveOutgoingState,
	bool persistNormalization) {
	Logger::info(string(__FUNCSIG__));
	// Record runtime changes only when switching within the same source. A new
	// source must never inherit state from the file it is replacing.
	if (saveOutgoingState)
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());

	// Loading the new profile
	CurrentConfig->setActiveProfile(profileName);
	const std::string effectiveProfileName = CurrentConfig->getActiveProfileName();
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::ProfileUpdate);
	strncpy_s(
		CrashActiveProfile,
		sizeof(CrashActiveProfile),
		effectiveProfileName.empty() ? "unavailable" : effectiveProfileName.c_str(),
		_TRUNCATE);
	PublishCrashRadarState("main");
	InvalidateRunwayGeometryCache();
	InvalidateStructuredTagRuleCache();
	EnsureTargetGroundStatusColorEntries(persistNormalization);

	// Loading all the new data
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	const Value* rimcasConfig = nullptr;
	if (activeProfile.IsObject() && activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		rimcasConfig = &activeProfile["rimcas"];

	RimcasEnabled = true;
	RimcasUseRedEmergencySymbols = true;
	RimcasRunwaysExplicitlyConfigured = false;
	if (rimcasConfig != nullptr)
	{
		if (rimcasConfig->HasMember("enabled") && (*rimcasConfig)["enabled"].IsBool())
			RimcasEnabled = (*rimcasConfig)["enabled"].GetBool();
		if (rimcasConfig->HasMember("use_red_symbol_for_emergencies") &&
			(*rimcasConfig)["use_red_symbol_for_emergencies"].IsBool())
		{
			RimcasUseRedEmergencySymbols = (*rimcasConfig)["use_red_symbol_for_emergencies"].GetBool();
		}
		if (rimcasConfig->HasMember("visibility") && (*rimcasConfig)["visibility"].IsString())
		{
			std::string visibility = (*rimcasConfig)["visibility"].GetString();
			std::transform(
				visibility.begin(),
				visibility.end(),
				visibility.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (visibility == "lvp" || visibility == "low")
				isLVP = true;
			else if (visibility == "normal")
				isLVP = false;
		}

		if (rimcasConfig->HasMember("runways") && (*rimcasConfig)["runways"].IsArray())
		{
			RimcasRunwaysExplicitlyConfigured = true;
			RimcasInstance->MonitoredRunwayArr.clear();
			RimcasInstance->MonitoredRunwayDep.clear();
			RimcasInstance->ClosedRunway.clear();

			auto trimRunwayPart = [](const std::string& value) -> std::string
			{
				size_t first = 0;
				while (first < value.size() &&
					std::isspace(static_cast<unsigned char>(value[first])) != 0)
				{
					++first;
				}
				size_t last = value.size();
				while (last > first &&
					std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
				{
					--last;
				}
				return value.substr(first, last - first);
			};
			auto normalizeRunwayPair = [&](const std::string& rawName) -> std::string
			{
				std::string name = trimRunwayPart(rawName);
				const size_t slash = name.find('/');
				if (slash != std::string::npos)
				{
					const std::string first = trimRunwayPart(name.substr(0, slash));
					const std::string second = trimRunwayPart(name.substr(slash + 1));
					if (!first.empty() && !second.empty())
						name = first + " / " + second;
				}
				std::transform(
					name.begin(),
					name.end(),
					name.begin(),
					[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				return name;
			};

			const Value& runwayRows = (*rimcasConfig)["runways"];
			for (SizeType index = 0; index < runwayRows.Size(); ++index)
			{
				const Value& runway = runwayRows[index];
				if (!runway.IsObject() ||
					!runway.HasMember("id") ||
					!runway["id"].IsString())
				{
					continue;
				}

				const std::string runwayId = normalizeRunwayPair(runway["id"].GetString());
				if (runwayId.empty())
					continue;
				RimcasInstance->MonitoredRunwayArr[runwayId] =
					runway.HasMember("arrival") &&
					runway["arrival"].IsBool() &&
					runway["arrival"].GetBool();
				RimcasInstance->MonitoredRunwayDep[runwayId] =
					runway.HasMember("departure") &&
					runway["departure"].IsBool() &&
					runway["departure"].GetBool();
				RimcasInstance->ClosedRunway[runwayId] =
					runway.HasMember("closed") &&
					runway["closed"].IsBool() &&
					runway["closed"].GetBool();
			}
		}
	}

	// Inactive alerts
	unordered_set inactiveAlerts = CurrentConfig->getInactiveAlert();
	RimcasInstance->setInactiveAlerts(inactiveAlerts);
	if (!RimcasEnabled)
		RimcasInstance->OnRefreshBegin(isLVP);

	auto readCountdownDefinition = [&](const Value* arrayValue, const std::vector<int>& fallback) -> std::vector<int>
	{
		std::vector<int> values;
		if (arrayValue != nullptr && arrayValue->IsArray())
		{
			for (SizeType i = 0; i < arrayValue->Size(); ++i)
			{
				if ((*arrayValue)[i].IsInt())
					values.push_back((*arrayValue)[i].GetInt());
			}
		}
		if (values.empty())
			values = fallback;
		return values;
	};

	const std::vector<int> defaultRimcasTimer = { 60, 45, 30, 15, 0 };
	const std::vector<int> defaultRimcasTimerLvp = { 120, 90, 60, 30, 0 };
	const Value* rimcasTimer = (rimcasConfig != nullptr && rimcasConfig->HasMember("timer")) ? &(*rimcasConfig)["timer"] : nullptr;
	const Value* rimcasTimerLvp = (rimcasConfig != nullptr && rimcasConfig->HasMember("timer_lvp")) ? &(*rimcasConfig)["timer_lvp"] : nullptr;
	const std::vector<int> RimcasNorm = readCountdownDefinition(rimcasTimer, defaultRimcasTimer);
	const std::vector<int> RimcasLVP = readCountdownDefinition(rimcasTimerLvp, defaultRimcasTimerLvp);
	RimcasInstance->setCountdownDefinition(RimcasNorm, RimcasLVP);

	int leaderLineLength = 50;
	if (activeProfile.IsObject() &&
		activeProfile.HasMember("labels") &&
		activeProfile["labels"].IsObject() &&
		activeProfile["labels"].HasMember("leader_line_length") &&
		activeProfile["labels"]["leader_line_length"].IsInt())
	{
		leaderLineLength = activeProfile["labels"]["leader_line_length"].GetInt();
	}
	LeaderLineDefaultlenght = std::clamp(leaderLineLength, 0, 500);

	customCursor = CurrentConfig->isCustomCursorUsed();
	currentFontSize = GetActiveLabelFontSize();

	// Reloading the fonts
	this->LoadCustomFont();

	ProfileColorPaths.clear();
	ProfileColorPathHasAlpha.clear();
	SelectedProfileColorPath.clear();
	TagDefinitionEditorType = "departure";
	TagDefinitionEditorDetailed = !GetTagDefinitionDetailedSameAsDefinition();
	TagDefinitionEditorDepartureStatus = "default";
	TagDefinitionEditorSelectedLine = 0;
	if (!RimcasRunwaysExplicitlyConfigured)
		RefreshLegacyRimcasRunwayMonitoring();

	if (ProfileEditorDialog && ::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
		ProfileEditorDialog->SyncFromRadar();
}

void CSMRRadar::InvalidateAirportPositionCache()
{
	AirportPositionsCacheValid = false;
}

void CSMRRadar::InvalidateRunwayGeometryCache()
{
	CachedRunwayGeometryValid = false;
	CachedRunwayAirport.clear();
	CachedRunwayProfile.clear();
	CachedRunwayGeometries.clear();
	RunwayStatusLastRefreshTick = 0;
	RunwayStatusLastAirport.clear();
	LastMapRunwayStatuses.clear();
	LastMapActiveAirport.clear();

	if (RimcasInstance != nullptr)
	{
		RimcasInstance->RunwayAreas.clear();
		RimcasInstance->RunwayStatuses.clear();
	}
}

void CSMRRadar::EnsureAirportPositionCache()
{
	if (AirportPositionsCacheValid)
		return;

	AirportPositions.clear();

	CSectorElement apt;
	for (apt = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
		apt.IsValid();
		apt = GetPlugIn()->SectorFileElementSelectNext(apt, SECTOR_ELEMENT_AIRPORT))
	{
		const char* airportName = apt.GetName();
		if (airportName == nullptr || airportName[0] == '\0')
			continue;

		CPosition position;
		apt.GetPosition(&position, 0);
		AirportPositions[string(airportName)] = position;
	}

	AirportPositionsCacheValid = true;
}

void CSMRRadar::EnsureRunwayGeometryCache()
{
	if (RimcasInstance == nullptr)
		return;

	const std::string activeAirport = getActiveAirport();
	const std::string activeProfile = CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : std::string();

	auto populateRimcasRunwayAreas = [&]()
	{
		RimcasInstance->RunwayAreas.clear();
		for (const auto& runway : CachedRunwayGeometries)
			RimcasInstance->AddRunwayArea(this, runway.runwayNameA, runway.runwayNameB, runway.rimcasDefinition);
	};

	if (CachedRunwayGeometryValid &&
		CachedRunwayAirport == activeAirport &&
		CachedRunwayProfile == activeProfile &&
		CachedRunwayIsLvp == isLVP)
	{
		if (RimcasInstance->RunwayAreas.empty() && !CachedRunwayGeometries.empty())
			populateRimcasRunwayAreas();
		return;
	}

	CachedRunwayGeometries.clear();
	CachedRunwayAirport = activeAirport;
	CachedRunwayProfile = activeProfile;
	CachedRunwayIsLvp = isLVP;

	const Value* configuredRunways = nullptr;
	if (CurrentConfig != nullptr)
	{
		const Value& customMap = CurrentConfig->getAirportMapIfAny(activeAirport);
		if (customMap.IsObject() &&
			customMap.HasMember("runways") &&
			customMap["runways"].IsArray())
		{
			configuredRunways = &customMap["runways"];
		}
	}

	auto loadClosedRunwayDefinition = [&](const std::string& runwayNameA, const std::string& runwayNameB) -> std::vector<CPosition>
	{
		std::vector<CPosition> definition;
		if (configuredRunways == nullptr)
			return definition;

		for (SizeType i = 0; i < configuredRunways->Size(); ++i)
		{
			const Value& runway = (*configuredRunways)[i];
			if (!runway.IsObject() ||
				!runway.HasMember("runway_name") ||
				!runway["runway_name"].IsString())
			{
				continue;
			}

			const char* configuredName = runway["runway_name"].GetString();
			if (configuredName == nullptr || configuredName[0] == '\0')
				continue;

			if (!startsWith(runwayNameA.c_str(), configuredName) &&
				!startsWith(runwayNameB.c_str(), configuredName))
			{
				continue;
			}

			const char* pathName = isLVP ? "path_lvp" : "path";
			const Value* path = nullptr;
			if (runway.HasMember(pathName) && runway[pathName].IsArray())
				path = &runway[pathName];
			else if (isLVP && runway.HasMember("path") && runway["path"].IsArray())
				path = &runway["path"];

			if (path == nullptr)
				continue;

			for (SizeType j = 0; j < path->Size(); ++j)
			{
				const Value& point = (*path)[j];
				if (!point.IsArray() ||
					point.Size() < 2 ||
					!point[static_cast<SizeType>(0)].IsString() ||
					!point[static_cast<SizeType>(1)].IsString())
				{
					continue;
				}

				CPosition position;
				position.LoadFromStrings(
					point[static_cast<SizeType>(1)].GetString(),
					point[static_cast<SizeType>(0)].GetString());
				definition.push_back(position);
			}

			if (!definition.empty())
				return definition;
		}

		return definition;
	};

	auto normalizeHeading = [](double heading) -> double
	{
		heading = std::fmod(heading, 360.0);
		return heading < 0.0 ? heading + 360.0 : heading;
	};
	auto angularDistance = [&](double first, double second) -> double
	{
		return std::abs(normalizeHeading(first - second + 180.0) - 180.0);
	};
	auto resolveTrueHeading = [&](const CPosition& from, const CPosition& to, int sectorHeading, double& heading, bool& valid)
	{
		if (sectorHeading < 0 || sectorHeading > 360)
			return;
		heading = normalizeHeading(RadToDeg(TrueBearing(from, to)));
		if (angularDistance(heading, static_cast<double>(sectorHeading)) > 90.0)
			heading = normalizeHeading(heading + 180.0);
		valid = true;
	};

	CSectorElement rwy;
	for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirportName = rwy.GetAirportName();
		if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
			continue;

		if (!startsWith(activeAirport.c_str(), runwayAirportName))
			continue;

		const char* runwayNameA = rwy.GetRunwayName(0);
		const char* runwayNameB = rwy.GetRunwayName(1);
		if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
			continue;

		CPosition left;
		const bool leftValid = rwy.GetPosition(&left, 1);
		CPosition right;
		const bool rightValid = rwy.GetPosition(&right, 0);
		if (!leftValid || !rightValid)
			continue;

		CachedRunwayGeometry runway;
		runway.runwayNameA = runwayNameA;
		runway.runwayNameB = runwayNameB;
		runway.displayName = runway.runwayNameA + " / " + runway.runwayNameB;
		resolveTrueHeading(right, left, rwy.GetRunwayHeading(0), runway.trueHeadingA, runway.trueHeadingAValid);
		resolveTrueHeading(left, right, rwy.GetRunwayHeading(1), runway.trueHeadingB, runway.trueHeadingBValid);
		runway.rimcasDefinition = RimcasInstance->GetRunwayArea(left, right);
		runway.closedDefinition = loadClosedRunwayDefinition(runway.runwayNameA, runway.runwayNameB);
		CachedRunwayGeometries.push_back(std::move(runway));
	}

	CachedRunwayGeometryValid = true;
	populateRimcasRunwayAreas();
}

void CSMRRadar::RefreshRunwayStatuses(bool force)
{
	if (RimcasInstance == nullptr)
		return;

	const std::string activeAirport = getActiveAirport();
	const unsigned long nowTick = ::GetTickCount();
	const unsigned long statusRefreshIntervalMs = 200;
	if (!force &&
		RunwayStatusLastRefreshTick != 0 &&
		RunwayStatusLastAirport == activeAirport &&
		(nowTick - RunwayStatusLastRefreshTick) < statusRefreshIntervalMs)
	{
		return;
	}

	auto getRunwayStatus = [](CSectorElement& runway, int index) -> CRimcas::RunwayStatus
	{
		const bool isDepartureRunway = runway.IsElementActive(true, index);
		const bool isArrivalRunway = runway.IsElementActive(false, index);
		if (isDepartureRunway && isArrivalRunway)
			return CRimcas::RunwayStatus::BOTH;
		if (isDepartureRunway)
			return CRimcas::RunwayStatus::DEP;
		if (isArrivalRunway)
			return CRimcas::RunwayStatus::ARR;
		return CRimcas::RunwayStatus::CLSD;
	};

	std::map<std::string, CRimcas::RunwayStatus> runwayStatuses;
	CSectorElement rwy;
	for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirportName = rwy.GetAirportName();
		if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
			continue;

		if (!startsWith(activeAirport.c_str(), runwayAirportName))
			continue;

		const char* runwayNameA = rwy.GetRunwayName(0);
		const char* runwayNameB = rwy.GetRunwayName(1);
		if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
			continue;

		runwayStatuses[runwayNameA] = getRunwayStatus(rwy, 0);
		runwayStatuses[runwayNameB] = getRunwayStatus(rwy, 1);
	}

	RunwayStatusLastRefreshTick = nowTick;
	RunwayStatusLastAirport = activeAirport;

	if (RimcasInstance->RunwayStatuses != runwayStatuses)
		RimcasInstance->RunwayStatuses = std::move(runwayStatuses);
}

void CSMRRadar::RefreshLegacyRimcasRunwayMonitoring()
{
	if (RimcasInstance == nullptr || RimcasRunwaysExplicitlyConfigured)
		return;
	// The constructor loads a profile before EuroScope has accepted this radar
	// screen. Defer sector-file access until OnRadarScreenCreated has returned.
	if (std::find(RadarScreensOpened.begin(), RadarScreensOpened.end(), this) ==
		RadarScreensOpened.end())
	{
		return;
	}

	// Legacy profiles without a `rimcas.runways` member follow EuroScope's
	// current runway activity. Explicit profile rows, including an empty array,
	// remain authoritative and are never overwritten here.
	CPlugIn* plugin = GetPlugIn();
	struct ActiveSectorSelectionGuard
	{
		CPlugIn* plugin = nullptr;
		~ActiveSectorSelectionGuard()
		{
			if (plugin != nullptr)
				plugin->SelectActiveSectorfile();
		}
	} selectionGuard{ plugin };
	plugin->SelectScreenSectorfile(this);
	RimcasInstance->MonitoredRunwayArr.clear();
	RimcasInstance->MonitoredRunwayDep.clear();
	RimcasInstance->ClosedRunway.clear();

	const std::string activeAirport = getActiveAirport();
	CSectorElement runway;
	for (runway = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		runway.IsValid();
		runway = GetPlugIn()->SectorFileElementSelectNext(runway, SECTOR_ELEMENT_RUNWAY))
	{
		const char* runwayAirport = runway.GetAirportName();
		const char* runwayNameA = runway.GetRunwayName(0);
		const char* runwayNameB = runway.GetRunwayName(1);
		if (runwayAirport == nullptr || runwayNameA == nullptr || runwayNameB == nullptr ||
			runwayAirport[0] == '\0' || runwayNameA[0] == '\0' || runwayNameB[0] == '\0' ||
			_stricmp(runwayAirport, activeAirport.c_str()) != 0)
		{
			continue;
		}

		const std::string name = std::string(runwayNameA) + " / " + runwayNameB;
		RimcasInstance->MonitoredRunwayDep[name] =
			runway.IsElementActive(true, 0) || runway.IsElementActive(true, 1);
		RimcasInstance->MonitoredRunwayArr[name] =
			runway.IsElementActive(false, 0) || runway.IsElementActive(false, 1);
		RimcasInstance->ClosedRunway[name] = false;
	}
}

void CSMRRadar::InvalidateStructuredTagRuleCache()
{
	StructuredTagRulesCache.clear();
	StructuredTagRulesCacheValid = false;
}

void CSMRRadar::EnsureTargetGroundStatusColorEntries(bool persistChanges)
{
	// Backward-compatible profile migration and normalization:
	// ensure required nested objects, color entries and editor settings exist.
	if (!CurrentConfig || CurrentConfig->getProfileCount() == 0)
		return;

	Value& profile = const_cast<Value&>(CurrentConfig->getActiveProfile());
	if (!profile.IsObject())
		return;

	auto& allocator = CurrentConfig->document.GetAllocator();
	bool changed = false;

	auto ensureObjectMember = [&](Value& parent, const char* key) -> Value&
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value newObject(kObjectType);
			parent.AddMember(keyValue, newObject, allocator);
			changed = true;
		}

		return parent[key];
	};

	auto ensureColorMember = [&](Value& parent, const char* key, int r, int g, int b, int a)
	{
		if (!parent.HasMember(key) || !parent[key].IsObject())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			colorObject.AddMember("a", a, allocator);
			parent.AddMember(keyValue, colorObject, allocator);
			changed = true;
			return;
		}

		Value& colorObject = parent[key];
		auto ensureComponent = [&](const char* component, int value)
		{
			if (!colorObject.HasMember(component))
			{
				Value componentKey;
				componentKey.SetString(component, allocator);
				Value componentValue;
				componentValue.SetInt(value);
				colorObject.AddMember(componentKey, componentValue, allocator);
				changed = true;
				return;
			}

			if (!colorObject[component].IsInt() || colorObject[component].GetInt() < 0 || colorObject[component].GetInt() > 255)
			{
				colorObject[component].SetInt(value);
				changed = true;
			}
		};

		ensureComponent("r", r);
		ensureComponent("g", g);
		ensureComponent("b", b);
		ensureComponent("a", a);
	};

	auto replaceColorMember = [&](Value& parent, const char* key, const Value& sourceColor)
	{
		if (!sourceColor.IsObject())
			return;

		auto readColorComponent = [&](const char* component, int fallback) -> int
		{
			if (sourceColor.HasMember(component) && sourceColor[component].IsInt())
				return min(255, max(0, sourceColor[component].GetInt()));
			return fallback;
		};

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value colorObject(kObjectType);
		colorObject.AddMember("r", readColorComponent("r", 0), allocator);
		colorObject.AddMember("g", readColorComponent("g", 0), allocator);
		colorObject.AddMember("b", readColorComponent("b", 0), allocator);
		colorObject.AddMember("a", readColorComponent("a", 255), allocator);
		parent.AddMember(keyValue, colorObject, allocator);
		changed = true;
	};

	auto ensureBoolMember = [&](Value& parent, const char* key, bool defaultValue)
	{
		if (parent.HasMember(key) && parent[key].IsBool())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value boolValue;
		boolValue.SetBool(defaultValue);
		parent.AddMember(keyValue, boolValue, allocator);
		changed = true;
	};

	auto ensureIntMember = [&](Value& parent, const char* key, int defaultValue, int minValue, int maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = std::clamp(current, minValue, maxValue);
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value intValue;
		intValue.SetInt(defaultValue);
		parent.AddMember(keyValue, intValue, allocator);
		changed = true;
	};

	auto ensureDoubleMember = [&](Value& parent, const char* key, double defaultValue, double minValue, double maxValue)
	{
		defaultValue = std::clamp(defaultValue, minValue, maxValue);
		if (parent.HasMember(key) && parent[key].IsNumber())
		{
			double currentValue = parent[key].GetDouble();
			double boundedValue = std::clamp(currentValue, minValue, maxValue);
			if (fabs(currentValue - boundedValue) > 0.0001)
			{
				parent[key].SetDouble(boundedValue);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value numberValue;
		numberValue.SetDouble(defaultValue);
		parent.AddMember(keyValue, numberValue, allocator);
		changed = true;
	};

	auto ensureResolutionPresetMember = [&](Value& parent, const char* key, const char* defaultValue)
	{
		auto normalizePreset = [](const std::string& raw) -> std::string
		{
			std::string lowered = raw;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered.find("4k") != std::string::npos || lowered.find("2160") != std::string::npos || lowered.find("uhd") != std::string::npos)
				return "4k";
			if (lowered.find("2k") != std::string::npos || lowered.find("1440") != std::string::npos || lowered.find("qhd") != std::string::npos)
				return "2k";
			return "1080p";
		};

		if (parent.HasMember(key) && parent[key].IsString())
		{
			const std::string normalized = normalizePreset(parent[key].GetString());
			if (normalized != parent[key].GetString())
			{
				parent[key].SetString(normalized.c_str(), static_cast<rapidjson::SizeType>(normalized.size()), allocator);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		const std::string normalizedDefault = normalizePreset(defaultValue ? defaultValue : "1080p");
		Value keyValue;
		keyValue.SetString(key, allocator);
		Value presetValue;
		presetValue.SetString(normalizedDefault.c_str(), static_cast<rapidjson::SizeType>(normalizedDefault.size()), allocator);
		parent.AddMember(keyValue, presetValue, allocator);
		changed = true;
	};

	auto appendCopiedDefinition = [&](Value& targetArray, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		for (rapidjson::SizeType i = 0; i < sourceArray.Size(); ++i)
		{
			const Value& sourceLine = sourceArray[i];
			Value copiedLine(kArrayType);
			if (sourceLine.IsArray())
			{
				for (rapidjson::SizeType j = 0; j < sourceLine.Size(); ++j)
				{
					if (sourceLine[j].IsString())
					{
						Value tokenValue;
						tokenValue.SetString(sourceLine[j].GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine[j].GetString())), allocator);
						copiedLine.PushBack(tokenValue, allocator);
					}
				}
			}
			else if (sourceLine.IsString())
			{
				Value tokenValue;
				tokenValue.SetString(sourceLine.GetString(), static_cast<rapidjson::SizeType>(strlen(sourceLine.GetString())), allocator);
				copiedLine.PushBack(tokenValue, allocator);
			}

			targetArray.PushBack(copiedLine, allocator);
		}
	};

	auto ensureDefinitionArrayMember = [&](Value& parent, const char* key, const Value* fallbackSource)
	{
		if (parent.HasMember(key) && parent[key].IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		if (fallbackSource != nullptr)
			appendCopiedDefinition(definitionArray, *fallbackSource);

		if (definitionArray.Size() == 0)
		{
			Value fallbackLine(kArrayType);
			Value fallbackToken;
			fallbackToken.SetString("callsign", allocator);
			fallbackLine.PushBack(fallbackToken, allocator);
			definitionArray.PushBack(fallbackLine, allocator);
		}

		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto replaceDefinitionArrayMember = [&](Value& parent, const char* key, const Value& sourceArray)
	{
		if (!sourceArray.IsArray())
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value definitionArray(kArrayType);
		appendCopiedDefinition(definitionArray, sourceArray);
		parent.AddMember(keyValue, definitionArray, allocator);
		changed = true;
	};

	auto cloneJsonValue = [&](Value& destination, const Value& source, const auto& cloneRef) -> void
	{
		if (source.IsObject())
		{
			destination.SetObject();
			for (auto member = source.MemberBegin(); member != source.MemberEnd(); ++member)
			{
				Value keyValue;
				keyValue.SetString(member->name.GetString(), static_cast<rapidjson::SizeType>(strlen(member->name.GetString())), allocator);
				Value childValue;
				cloneRef(childValue, member->value, cloneRef);
				destination.AddMember(keyValue, childValue, allocator);
			}
			return;
		}

		if (source.IsArray())
		{
			destination.SetArray();
			for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
			{
				Value childValue;
				cloneRef(childValue, source[i], cloneRef);
				destination.PushBack(childValue, allocator);
			}
			return;
		}

		if (source.IsString())
		{
			destination.SetString(source.GetString(), static_cast<rapidjson::SizeType>(strlen(source.GetString())), allocator);
			return;
		}

		if (source.IsBool())
		{
			destination.SetBool(source.GetBool());
			return;
		}
		if (source.IsInt())
		{
			destination.SetInt(source.GetInt());
			return;
		}
		if (source.IsUint())
		{
			destination.SetUint(source.GetUint());
			return;
		}
		if (source.IsInt64())
		{
			destination.SetInt64(source.GetInt64());
			return;
		}
		if (source.IsUint64())
		{
			destination.SetUint64(source.GetUint64());
			return;
		}
		if (source.IsDouble())
		{
			destination.SetDouble(source.GetDouble());
			return;
		}
		if (source.IsNull())
		{
			destination.SetNull();
			return;
		}

		destination.SetNull();
	};

	auto renameMemberIfPresent = [&](Value& parent, const char* oldKey, const char* newKey)
	{
		if (!parent.IsObject() || oldKey == nullptr || newKey == nullptr || strcmp(oldKey, newKey) == 0)
			return;
		if (!parent.HasMember(oldKey))
			return;

		if (parent.HasMember(newKey))
		{
			parent.RemoveMember(oldKey);
			changed = true;
			return;
		}

		Value keyValue;
		keyValue.SetString(newKey, allocator);
		Value copiedValue;
		cloneJsonValue(copiedValue, parent[oldKey], cloneJsonValue);
		parent.AddMember(keyValue, copiedValue, allocator);
		parent.RemoveMember(oldKey);
		changed = true;
	};

	auto copyBoolMemberIfPresent = [&](Value& parent, const char* key, const Value& sourceObject)
	{
		if (!sourceObject.IsObject() || !sourceObject.HasMember(key) || !sourceObject[key].IsBool())
			return;

		const bool sourceValue = sourceObject[key].GetBool();
		if (!parent.HasMember(key) || !parent[key].IsBool())
		{
			if (parent.HasMember(key))
				parent.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value boolValue(sourceValue);
			parent.AddMember(keyValue, boolValue, allocator);
			changed = true;
			return;
		}

		if (parent[key].GetBool() != sourceValue)
		{
			parent[key].SetBool(sourceValue);
			changed = true;
		}
	};

	auto ensureStringArrayMember = [&](Value& parent, const char* key, const std::vector<std::string>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (const std::string& item : defaults)
		{
			Value itemValue;
			itemValue.SetString(item.c_str(), static_cast<rapidjson::SizeType>(item.size()), allocator);
			outputArray.PushBack(itemValue, allocator);
		}
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	auto ensureIntArrayMember = [&](Value& parent, const char* key, const std::vector<int>& defaults)
	{
		bool rebuild = false;
		if (!parent.HasMember(key) || !parent[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			const Value& existingArray = parent[key];
			if (existingArray.Size() == 0)
			{
				rebuild = true;
			}
			else
			{
				for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
				{
					if (!existingArray[i].IsInt())
					{
						rebuild = true;
						break;
					}
				}
			}
		}

		if (!rebuild)
			return;

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value outputArray(kArrayType);
		for (int value : defaults)
			outputArray.PushBack(value, allocator);
		parent.AddMember(keyValue, outputArray, allocator);
		changed = true;
	};

	const std::vector<std::string> defaultDoNotAutocorrelateSquawks = {
		"2000", "2200", "1200", "7000"
	};

	ensureIntMember(profile, "schema_version", 2, 2, 9999);

	Value& filters = ensureObjectMember(profile, "filters");
	renameMemberIfPresent(filters, "hide_above_alt", "max_altitude_ft");
	renameMemberIfPresent(filters, "hide_above_spd", "max_speed_kt");
	changed = filters.RemoveMember("night_alpha_setting") || changed;
	changed = filters.RemoveMember("night_overlay_alpha") || changed;
	ensureIntMember(filters, "max_altitude_ft", 5500, 0, 80000);
	ensureIntMember(filters, "max_speed_kt", 250, 0, 2000);
	ensureIntMember(filters, "radar_range_nm", 999, 1, 9999);
	bool legacyProModeEnabled = false;
	bool legacyTowerModeEnabled = false;
	std::vector<std::string> legacyBlockedSquawks = defaultDoNotAutocorrelateSquawks;
	if (filters.HasMember("pro_mode") && filters["pro_mode"].IsObject())
	{
		Value& proMode = filters["pro_mode"];
		renameMemberIfPresent(proMode, "enable", "enabled");
		renameMemberIfPresent(proMode, "do_not_autocorrelate_squawks", "blocked_auto_correlate_squawks");
		if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
			legacyProModeEnabled = proMode["enabled"].GetBool();
		if (proMode.HasMember("blocked_auto_correlate_squawks") && proMode["blocked_auto_correlate_squawks"].IsArray())
		{
			std::vector<std::string> blockedSquawks;
			const Value& squawks = proMode["blocked_auto_correlate_squawks"];
			for (SizeType i = 0; i < squawks.Size(); ++i)
			{
				if (squawks[i].IsString() && squawks[i].GetStringLength() > 0)
					blockedSquawks.push_back(squawks[i].GetString());
			}
			if (!blockedSquawks.empty())
				legacyBlockedSquawks = blockedSquawks;
		}
	}
	if (filters.HasMember("tower_mode") && filters["tower_mode"].IsObject())
	{
		Value& towerMode = filters["tower_mode"];
		renameMemberIfPresent(towerMode, "enable", "enabled");
		if (towerMode.HasMember("enabled") && towerMode["enabled"].IsBool())
			legacyTowerModeEnabled = towerMode["enabled"].GetBool();
	}
	auto addDisplayMode = [&](Value& items, const char* name, bool requireAssignedSquawk)
	{
		Value mode(kObjectType);
		Value nameKey;
		nameKey.SetString("name", allocator);
		Value nameValue;
		nameValue.SetString(name, allocator);
		mode.AddMember(nameKey, nameValue, allocator);
		mode.AddMember("require_assigned_squawk", requireAssignedSquawk, allocator);
		mode.AddMember("require_clearance", false, allocator);
		mode.AddMember("require_valid_tsat", false, allocator);
		mode.AddMember("require_active_tobt", false, allocator);
		Value blockedSquawks(kArrayType);
		for (const std::string& squawk : legacyBlockedSquawks)
		{
			Value squawkValue;
			squawkValue.SetString(squawk.c_str(), static_cast<SizeType>(squawk.size()), allocator);
			blockedSquawks.PushBack(squawkValue, allocator);
		}
		mode.AddMember("blocked_auto_correlate_squawks", blockedSquawks, allocator);
		Value statuses(kObjectType);
		statuses.AddMember("no_status", true, allocator);
		statuses.AddMember("push", true, allocator);
		statuses.AddMember("startup", true, allocator);
		statuses.AddMember("taxi", true, allocator);
		statuses.AddMember("lineup", true, allocator);
		statuses.AddMember("departure", true, allocator);
		statuses.AddMember("on_runway", true, allocator);
		statuses.AddMember("airborne", true, allocator);
		statuses.AddMember("arrivals", true, allocator);
		statuses.AddMember("no_fpl", true, allocator);
		statuses.AddMember("uncorrelated", true, allocator);
		mode.AddMember("statuses", statuses, allocator);
		items.PushBack(mode, allocator);
	};
	Value& displayModes = ensureObjectMember(filters, "display_modes");
	if (!displayModes.HasMember("items") || !displayModes["items"].IsArray() || displayModes["items"].Empty())
	{
		if (displayModes.HasMember("items"))
			displayModes.RemoveMember("items");
		Value items(kArrayType);
		addDisplayMode(items, "Normal", false);
		addDisplayMode(items, "Pro", true);
		addDisplayMode(items, "Tower", false);
		addDisplayMode(items, "Pro + Tower", true);
		displayModes.AddMember("items", items, allocator);
		changed = true;
	}
	if (displayModes.HasMember("items") && displayModes["items"].IsArray())
	{
		Value& items = displayModes["items"];
		for (SizeType i = 0; i < items.Size(); ++i)
		{
			if (!items[i].IsObject())
				continue;
			Value& statuses = ensureObjectMember(items[i], "statuses");
			if (!statuses.HasMember("lineup") || !statuses["lineup"].IsBool())
			{
				const bool visible = statuses.HasMember("lnup") && statuses["lnup"].IsBool()
					? statuses["lnup"].GetBool()
					: (statuses.HasMember("taxi") && statuses["taxi"].IsBool() ? statuses["taxi"].GetBool() : true);
				ensureBoolMember(statuses, "lineup", visible);
			}
			if (statuses.HasMember("lnup"))
			{
				statuses.RemoveMember("lnup");
				changed = true;
			}
		}
	}
	std::string activeDisplayMode = "Normal";
	if (legacyProModeEnabled && legacyTowerModeEnabled)
		activeDisplayMode = "Pro + Tower";
	else if (legacyProModeEnabled)
		activeDisplayMode = "Pro";
	else if (legacyTowerModeEnabled)
		activeDisplayMode = "Tower";
	if (!displayModes.HasMember("active") || !displayModes["active"].IsString() || displayModes["active"].GetStringLength() == 0)
	{
		if (displayModes.HasMember("active"))
			displayModes.RemoveMember("active");
		Value activeValue;
		activeValue.SetString(activeDisplayMode.c_str(), static_cast<SizeType>(activeDisplayMode.size()), allocator);
		displayModes.AddMember("active", activeValue, allocator);
		changed = true;
	}
	if (filters.HasMember("pro_mode"))
	{
		filters.RemoveMember("pro_mode");
		changed = true;
	}
	if (filters.HasMember("tower_mode"))
	{
		filters.RemoveMember("tower_mode");
		changed = true;
	}

	Value& rimcas = ensureObjectMember(profile, "rimcas");
	renameMemberIfPresent(rimcas, "rimcas_stage_two_speed_threshold", "stage_two_speed_threshold_kt");
	ensureBoolMember(rimcas, "rimcas_label_only", true);
	ensureBoolMember(rimcas, "use_red_symbol_for_emergencies", true);
	ensureIntArrayMember(rimcas, "timer", { 60, 45, 30, 15, 0 });
	ensureIntArrayMember(rimcas, "timer_lvp", { 120, 90, 60, 30, 0 });
	ensureIntMember(rimcas, "stage_two_speed_threshold_kt", 25, 0, 250);
	ensureColorMember(rimcas, "background_color_stage_one", 160, 90, 30, 255);
	ensureColorMember(rimcas, "background_color_stage_two", 150, 0, 0, 255);
	ensureColorMember(rimcas, "caution_alert_text_color", 0, 0, 0, 255);
	ensureColorMember(rimcas, "warning_alert_text_color", 255, 255, 255, 255);
	ensureColorMember(rimcas, "caution_alert_background_color", 255, 255, 0, 255);
	ensureColorMember(rimcas, "warning_alert_background_color", 255, 0, 0, 255);
	ensureStringArrayMember(rimcas, "inactive_alerts", {});

	const std::vector<std::string> defaultTagFonts = {
		"EuroScope",
		"Consolas",
		"Lucida Console",
		"Courier New",
		"Segoe UI",
		"Tahoma",
		"Arial",
		"ods",
		"Deesse Medium"
	};

	Value& font = ensureObjectMember(profile, "font");
	if (!font.HasMember("font_name") || !font["font_name"].IsString() || strlen(font["font_name"].GetString()) == 0)
	{
		if (font.HasMember("font_name"))
			font.RemoveMember("font_name");

		Value keyValue;
		keyValue.SetString("font_name", allocator);
		Value fontNameValue;
		fontNameValue.SetString("EuroScope", allocator);
		font.AddMember(keyValue, fontNameValue, allocator);
		changed = true;
	}

	if (!font.HasMember("weight") || !font["weight"].IsString())
	{
		if (font.HasMember("weight"))
			font.RemoveMember("weight");

		Value keyValue;
		keyValue.SetString("weight", allocator);
		Value weightValue;
		weightValue.SetString("Regular", allocator);
		font.AddMember(keyValue, weightValue, allocator);
		changed = true;
	}

	Value& fontSizes = ensureObjectMember(font, "sizes");
	auto ensureFontSizeMember = [&](Value& parent, const char* key, int fallback)
	{
		if (parent.HasMember(key) && parent[key].IsInt())
		{
			const int current = parent[key].GetInt();
			const int bounded = (current < 6) ? 6 : current;
			if (current != bounded)
			{
				parent[key].SetInt(bounded);
				changed = true;
			}
			return;
		}

		if (parent.HasMember(key))
			parent.RemoveMember(key);

		Value keyValue;
		keyValue.SetString(key, allocator);
		Value value;
		value.SetInt((fallback < 6) ? 6 : fallback);
		parent.AddMember(keyValue, value, allocator);
		changed = true;
	};

	ensureFontSizeMember(fontSizes, "one", 10);
	ensureFontSizeMember(fontSizes, "two", 11);
	ensureFontSizeMember(fontSizes, "three", 12);
	ensureFontSizeMember(fontSizes, "four", 13);
	ensureFontSizeMember(fontSizes, "five", 14);

	auto ensureAvailableFontList = [&](Value& fontObject, const char* key)
	{
		bool rebuild = false;
		if (!fontObject.HasMember(key) || !fontObject[key].IsArray())
		{
			rebuild = true;
		}
		else
		{
			Value& existingArray = fontObject[key];
			for (rapidjson::SizeType i = 0; i < existingArray.Size(); ++i)
			{
				if (!existingArray[i].IsString())
				{
					rebuild = true;
					break;
				}
			}
		}

		if (rebuild)
		{
			if (fontObject.HasMember(key))
				fontObject.RemoveMember(key);

			Value keyValue;
			keyValue.SetString(key, allocator);
			Value fontArray(kArrayType);
			for (const std::string& fontName : defaultTagFonts)
			{
				Value fontValue;
				fontValue.SetString(fontName.c_str(), static_cast<rapidjson::SizeType>(fontName.size()), allocator);
				fontArray.PushBack(fontValue, allocator);
			}
			fontObject.AddMember(keyValue, fontArray, allocator);
			changed = true;
			return;
		}
	};

	ensureAvailableFontList(font, "available_fonts");
	ensureIntMember(font, "label_font_size", 1, 1, 5);

	Value& targets = ensureObjectMember(profile, "targets");
	renameMemberIfPresent(targets, "small_icon_boost_resolution", "small_icon_boost_resolution_preset");
	renameMemberIfPresent(targets, "fixed_pixel_triangle_scale", "fixed_pixel_icon_scale");
	if (!targets.HasMember("icon_style") || !targets["icon_style"].IsString())
	{
		if (targets.HasMember("icon_style"))
			targets.RemoveMember("icon_style");

		Value keyValue;
		keyValue.SetString("icon_style", allocator);
		Value value;
		value.SetString("realistic", allocator);
		targets.AddMember(keyValue, value, allocator);
		changed = true;
	}
	else
	{
		const std::string normalizedIconStyle = NormalizeTargetIconStyle(targets["icon_style"].GetString());
		if (normalizedIconStyle != targets["icon_style"].GetString())
		{
			targets["icon_style"].SetString(normalizedIconStyle.c_str(), static_cast<rapidjson::SizeType>(normalizedIconStyle.size()), allocator);
			changed = true;
		}
	}
	ensureBoolMember(targets, "small_icon_boost", false);
	ensureDoubleMember(targets, "small_icon_boost_factor", 1.0, 0.5, 4.0);
	ensureResolutionPresetMember(targets, "small_icon_boost_resolution_preset", "1080p");
	ensureBoolMember(targets, "fixed_pixel_icon_size", false);
	ensureDoubleMember(targets, "fixed_pixel_icon_scale", 1.0, 0.1, 3.0);
	ensureBoolMember(targets, "show_primary_target", true);
	ensureColorMember(targets, "target_color", 255, 242, 73, 255);
	changed = targets.RemoveMember("history_one_color") || changed;
	changed = targets.RemoveMember("history_two_color") || changed;
	changed = targets.RemoveMember("history_three_color") || changed;

	Value* legacyGroundIcons = nullptr;
	if (targets.HasMember("ground_icons") && targets["ground_icons"].IsObject())
		legacyGroundIcons = &targets["ground_icons"];

	Value& departureIcons = ensureObjectMember(targets, "departure");
	Value& arrivalIcons = ensureObjectMember(targets, "arrival");

	auto migrateTargetIconColor = [&](Value& destination, const char* destinationKey, int r, int g, int b, int a, const char* legacyPrimary, const char* legacySecondary = nullptr, const char* legacyTertiary = nullptr)
	{
		if ((!destination.HasMember(destinationKey) || !destination[destinationKey].IsObject()) && legacyGroundIcons != nullptr)
		{
			const Value* sourceColor = nullptr;
			auto pickLegacyColor = [&](const char* legacyKey) -> bool
			{
				if (legacyKey == nullptr || !legacyGroundIcons->HasMember(legacyKey))
					return false;
				const Value& candidate = (*legacyGroundIcons)[legacyKey];
				if (!candidate.IsObject())
					return false;
				sourceColor = &candidate;
				return true;
			};

			if (!pickLegacyColor(legacyPrimary))
				if (!pickLegacyColor(legacySecondary))
					pickLegacyColor(legacyTertiary);

			if (sourceColor != nullptr)
				replaceColorMember(destination, destinationKey, *sourceColor);
		}

		ensureColorMember(destination, destinationKey, r, g, b, a);
	};

	migrateTargetIconColor(departureIcons, "airborne", 240, 240, 240, 255, "airborne_departure");
	migrateTargetIconColor(departureIcons, "departure", 240, 240, 240, 255, "depa");
	migrateTargetIconColor(departureIcons, "gate", 165, 165, 165, 255, "departure_gate", "gate");
	migrateTargetIconColor(departureIcons, "no_fpl", 128, 128, 128, 255, "nofpl");
	migrateTargetIconColor(departureIcons, "no_status", 165, 165, 165, 255, "nsts");
	migrateTargetIconColor(departureIcons, "push", 253, 218, 13, 255, "push");
	migrateTargetIconColor(departureIcons, "startup", 253, 218, 13, 255, "stup");
	migrateTargetIconColor(departureIcons, "taxi", 240, 240, 240, 255, "taxi");
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) && legacyGroundIcons != nullptr)
	{
		const Value* legacyLineupColor = nullptr;
		if (legacyGroundIcons->HasMember("lnup") && (*legacyGroundIcons)["lnup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lnup"];
		else if (legacyGroundIcons->HasMember("lineup") && (*legacyGroundIcons)["lineup"].IsObject())
			legacyLineupColor = &(*legacyGroundIcons)["lineup"];
		if (legacyLineupColor != nullptr)
			replaceColorMember(departureIcons, "lineup", *legacyLineupColor);
	}
	if ((!departureIcons.HasMember("lineup") || !departureIcons["lineup"].IsObject()) &&
		departureIcons.HasMember("taxi") && departureIcons["taxi"].IsObject())
	{
		Value taxiColorCopy;
		cloneJsonValue(taxiColorCopy, departureIcons["taxi"], cloneJsonValue);
		replaceColorMember(departureIcons, "lineup", taxiColorCopy);
	}
	ensureColorMember(departureIcons, "lineup", 240, 240, 240, 255);

	migrateTargetIconColor(arrivalIcons, "airborne", 120, 190, 240, 255, "airborne_arrival");
	migrateTargetIconColor(arrivalIcons, "gate", 165, 165, 165, 255, "arrival_gate", "gate");
	migrateTargetIconColor(arrivalIcons, "on_ground", 165, 165, 165, 255, "arr", "arrival_taxi");

	if (targets.HasMember("ground_icons"))
	{
		targets.RemoveMember("ground_icons");
		changed = true;
	}

	if (!profile.HasMember("sid_text_colors") || !profile["sid_text_colors"].IsArray())
	{
		if (profile.HasMember("sid_text_colors"))
			profile.RemoveMember("sid_text_colors");

		Value sidTextColorsKey;
		sidTextColorsKey.SetString("sid_text_colors", allocator);
		Value sidTextColorsArray(kArrayType);
		profile.AddMember(sidTextColorsKey, sidTextColorsArray, allocator);
		changed = true;
	}

	Value& labels = ensureObjectMember(profile, "labels");
	renameMemberIfPresent(labels, "use_aspeed_for_gate", "use_speed_for_gate");
	renameMemberIfPresent(labels, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	ensureBoolMember(labels, "auto_deconfliction", true);
	ensureBoolMember(labels, "rounded_corners", true);
	ensureBoolMember(labels, "use_speed_for_gate", false);
	ensureIntMember(labels, "leader_line_length", 50, 0, 500);
	ensureBoolMember(labels, "definition_detailed_inherits_normal", false);
	if (labels.HasMember("sid_text_colors"))
	{
		labels.RemoveMember("sid_text_colors");
		changed = true;
	}

	Value& departureLabel = ensureObjectMember(labels, "departure");
	renameMemberIfPresent(departureLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(departureLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(departureLabel, "background_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "gate_color", "background_no_status_color");
	renameMemberIfPresent(departureLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "on_runway_color", "background_on_runway_color");
	renameMemberIfPresent(departureLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(departureLabel, "nofpl_color", "background_no_fpl_color");
	renameMemberIfPresent(departureLabel, "nosid_color", "background_no_sid_color");
	renameMemberIfPresent(departureLabel, "push_color", "background_push_color");
	renameMemberIfPresent(departureLabel, "startup_color", "background_startup_color");
	renameMemberIfPresent(departureLabel, "taxi_color", "background_taxi_color");
	renameMemberIfPresent(departureLabel, "lineup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "lnup_color", "background_lineup_color");
	renameMemberIfPresent(departureLabel, "departure_color", "background_departure_color");
	if (departureLabel.HasMember("status_background_colors") && departureLabel["status_background_colors"].IsObject())
	{
		Value& departureStatusColors = departureLabel["status_background_colors"];
		if (departureStatusColors.HasMember("nsts") && departureStatusColors["nsts"].IsObject())
			replaceColorMember(departureLabel, "background_no_status_color", departureStatusColors["nsts"]);
		if (departureStatusColors.HasMember("push") && departureStatusColors["push"].IsObject())
			replaceColorMember(departureLabel, "background_push_color", departureStatusColors["push"]);
		if (departureStatusColors.HasMember("stup") && departureStatusColors["stup"].IsObject())
			replaceColorMember(departureLabel, "background_startup_color", departureStatusColors["stup"]);
		if (departureStatusColors.HasMember("taxi") && departureStatusColors["taxi"].IsObject())
			replaceColorMember(departureLabel, "background_taxi_color", departureStatusColors["taxi"]);
		if (departureStatusColors.HasMember("lnup") && departureStatusColors["lnup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lnup"]);
		else if (departureStatusColors.HasMember("lineup") && departureStatusColors["lineup"].IsObject())
			replaceColorMember(departureLabel, "background_lineup_color", departureStatusColors["lineup"]);
		if (departureStatusColors.HasMember("depa") && departureStatusColors["depa"].IsObject())
			replaceColorMember(departureLabel, "background_departure_color", departureStatusColors["depa"]);
		departureLabel.RemoveMember("status_background_colors");
		changed = true;
	}
	ensureColorMember(departureLabel, "background_no_status_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "background_on_runway_color", 40, 50, 200, 255);
	ensureColorMember(departureLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(departureLabel, "background_push_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_startup_color", 253, 218, 13, 255);
	ensureColorMember(departureLabel, "background_taxi_color", 240, 240, 240, 255);
	if ((!departureLabel.HasMember("background_lineup_color") || !departureLabel["background_lineup_color"].IsObject()) &&
		departureLabel.HasMember("background_taxi_color") && departureLabel["background_taxi_color"].IsObject())
	{
		Value taxiColorCopy;
		cloneJsonValue(taxiColorCopy, departureLabel["background_taxi_color"], cloneJsonValue);
		replaceColorMember(departureLabel, "background_lineup_color", taxiColorCopy);
	}
	ensureColorMember(departureLabel, "background_lineup_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_departure_color", 240, 240, 240, 255);
	ensureColorMember(departureLabel, "background_no_fpl_color", 128, 128, 128, 255);
	ensureColorMember(departureLabel, "background_no_sid_color", 53, 126, 187, 255);

	Value& arrivalLabel = ensureObjectMember(labels, "arrival");
	renameMemberIfPresent(arrivalLabel, "definitionDetailled", "definition_detailed");
	renameMemberIfPresent(arrivalLabel, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
	renameMemberIfPresent(arrivalLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "background_color_on_runway", "background_on_runway_color");
	renameMemberIfPresent(arrivalLabel, "text_color", "text_on_ground_color");
	renameMemberIfPresent(arrivalLabel, "nofpl_color", "background_no_fpl_color");
	ensureColorMember(arrivalLabel, "background_on_ground_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "background_on_runway_color", 170, 50, 50, 255);
	ensureColorMember(arrivalLabel, "text_on_ground_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_no_fpl_color", 128, 128, 128, 255);
	if (arrivalLabel.HasMember("status_background_colors") && arrivalLabel["status_background_colors"].IsObject())
	{
		Value& arrivalStatusColors = arrivalLabel["status_background_colors"];
		if (arrivalStatusColors.HasMember("arr") && arrivalStatusColors["arr"].IsObject())
			replaceColorMember(arrivalLabel, "background_on_ground_color", arrivalStatusColors["arr"]);
		arrivalLabel.RemoveMember("status_background_colors");
		changed = true;
	}

	Value& airborneLabel = ensureObjectMember(labels, "airborne");
	if ((!labels.HasMember("use_departure_arrival_coloring") || !labels["use_departure_arrival_coloring"].IsBool()) &&
		airborneLabel.HasMember("use_departure_arrival_coloring") &&
		airborneLabel["use_departure_arrival_coloring"].IsBool())
	{
		if (labels.HasMember("use_departure_arrival_coloring"))
			labels.RemoveMember("use_departure_arrival_coloring");
		Value keyValue;
		keyValue.SetString("use_departure_arrival_coloring", allocator);
		Value boolValue(airborneLabel["use_departure_arrival_coloring"].GetBool());
		labels.AddMember(keyValue, boolValue, allocator);
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color") && airborneLabel["departure_background_color"].IsObject())
		replaceColorMember(departureLabel, "background_airborne_color", airborneLabel["departure_background_color"]);
	if (airborneLabel.HasMember("departure_text_color") && airborneLabel["departure_text_color"].IsObject())
		replaceColorMember(departureLabel, "text_airborne_color", airborneLabel["departure_text_color"]);
	if (airborneLabel.HasMember("arrival_background_color") && airborneLabel["arrival_background_color"].IsObject())
		replaceColorMember(arrivalLabel, "background_airborne_color", airborneLabel["arrival_background_color"]);
	if (airborneLabel.HasMember("arrival_text_color") && airborneLabel["arrival_text_color"].IsObject())
		replaceColorMember(arrivalLabel, "text_airborne_color", airborneLabel["arrival_text_color"]);
	ensureColorMember(departureLabel, "background_airborne_color", 53, 126, 187, 255);
	ensureColorMember(departureLabel, "text_airborne_color", 255, 255, 255, 255);
	ensureColorMember(arrivalLabel, "background_airborne_color", 191, 87, 91, 255);
	ensureColorMember(arrivalLabel, "text_airborne_color", 255, 255, 255, 255);
	if (airborneLabel.HasMember("background_color"))
	{
		airborneLabel.RemoveMember("background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("background_color_on_runway"))
	{
		airborneLabel.RemoveMember("background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("text_color"))
	{
		airborneLabel.RemoveMember("text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("departure_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color_on_runway"))
	{
		airborneLabel.RemoveMember("arrival_background_color_on_runway");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_background_color"))
	{
		airborneLabel.RemoveMember("departure_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_background_color"))
	{
		airborneLabel.RemoveMember("arrival_background_color");
		changed = true;
	}
	if (airborneLabel.HasMember("departure_text_color"))
	{
		airborneLabel.RemoveMember("departure_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("arrival_text_color"))
	{
		airborneLabel.RemoveMember("arrival_text_color");
		changed = true;
	}
	if (airborneLabel.HasMember("use_departure_arrival_coloring"))
	{
		airborneLabel.RemoveMember("use_departure_arrival_coloring");
		changed = true;
	}
	ensureBoolMember(labels, "use_departure_arrival_coloring", false);

	Value& uncorrelatedLabel = ensureObjectMember(labels, "uncorrelated");
	renameMemberIfPresent(uncorrelatedLabel, "background_color", "background_on_ground_color");
	renameMemberIfPresent(uncorrelatedLabel, "background_color_on_runway", "background_on_runway_color");
	ensureColorMember(uncorrelatedLabel, "background_on_ground_color", 150, 22, 135, 255);
	ensureColorMember(uncorrelatedLabel, "background_on_runway_color", 150, 22, 135, 50);

	ensureDefinitionArrayMember(departureLabel, "definition", nullptr);
	const Value* baseDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(departureLabel, "definition_detailed", baseDefinition);
	Value& departureStatusDefinitions = ensureObjectMember(departureLabel, "status_definitions");
	renameMemberIfPresent(departureStatusDefinitions, "lineup", "lnup");
	if (departureStatusDefinitions.HasMember("nsts") && departureStatusDefinitions["nsts"].IsObject())
	{
		Value& departureNstsSection = departureStatusDefinitions["nsts"];
		renameMemberIfPresent(departureNstsSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(departureNstsSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		if (departureNstsSection.HasMember("definition") && departureNstsSection["definition"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition", departureNstsSection["definition"]);
		if (departureNstsSection.HasMember("definition_detailed") && departureNstsSection["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(departureLabel, "definition_detailed", departureNstsSection["definition_detailed"]);
		copyBoolMemberIfPresent(departureLabel, "definition_detailed_inherits_normal", departureNstsSection);
		departureStatusDefinitions.RemoveMember("nsts");
		changed = true;
	}

	auto ensureStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(departureStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (departureLabel.HasMember("definition_detailed") && departureLabel["definition_detailed"].IsArray()) ? &departureLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	if ((!departureStatusDefinitions.HasMember("lnup") || !departureStatusDefinitions["lnup"].IsObject()) &&
		departureStatusDefinitions.HasMember("taxi") && departureStatusDefinitions["taxi"].IsObject())
	{
		if (departureStatusDefinitions.HasMember("lnup"))
			departureStatusDefinitions.RemoveMember("lnup");
		Value keyValue;
		keyValue.SetString("lnup", allocator);
		Value copiedValue;
		cloneJsonValue(copiedValue, departureStatusDefinitions["taxi"], cloneJsonValue);
		departureStatusDefinitions.AddMember(keyValue, copiedValue, allocator);
		changed = true;
	}
	ensureStatusDefinitionEntries("taxi");
	ensureStatusDefinitionEntries("lnup");
	ensureStatusDefinitionEntries("push");
	ensureStatusDefinitionEntries("stup");
	ensureStatusDefinitionEntries("depa");
	ensureStatusDefinitionEntries("nofpl");
	ensureStatusDefinitionEntries("airdep");
	ensureStatusDefinitionEntries("airdep_onrunway");

	ensureDefinitionArrayMember(arrivalLabel, "definition", nullptr);
	const Value* arrivalBaseDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(arrivalLabel, "definition_detailed", arrivalBaseDefinition);
	Value& arrivalStatusDefinitions = ensureObjectMember(arrivalLabel, "status_definitions");
	if (arrivalStatusDefinitions.HasMember("arr"))
	{
		arrivalStatusDefinitions.RemoveMember("arr");
		changed = true;
	}
	if (arrivalStatusDefinitions.HasMember("taxi"))
	{
		arrivalStatusDefinitions.RemoveMember("taxi");
		changed = true;
	}
	auto ensureArrivalStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(arrivalStatusDefinitions, statusKey);
		renameMemberIfPresent(statusSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(statusSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");
		const Value* defaultDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (arrivalLabel.HasMember("definition_detailed") && arrivalLabel["definition_detailed"].IsArray()) ? &arrivalLabel["definition_detailed"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definition_detailed", defaultDetailedDefinition);
	};

	ensureArrivalStatusDefinitionEntries("nofpl");
	ensureArrivalStatusDefinitionEntries("airarr");
	ensureArrivalStatusDefinitionEntries("airarr_onrunway");

	auto copyStatusDefinitionFromSource = [&](Value& destinationStatusDefinitions, const char* destinationStatusKey, const Value* sourceSection)
	{
		if (destinationStatusKey == nullptr || sourceSection == nullptr || !sourceSection->IsObject())
			return;

		Value& destinationSection = ensureObjectMember(destinationStatusDefinitions, destinationStatusKey);
		renameMemberIfPresent(destinationSection, "definitionDetailled", "definition_detailed");
		renameMemberIfPresent(destinationSection, "definition_detailed_same_as_definition", "definition_detailed_inherits_normal");

		if (sourceSection->HasMember("definition") && (*sourceSection)["definition"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition", (*sourceSection)["definition"]);
		if (sourceSection->HasMember("definition_detailed") && (*sourceSection)["definition_detailed"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definition_detailed"]);
		else if (sourceSection->HasMember("definitionDetailled") && (*sourceSection)["definitionDetailled"].IsArray())
			replaceDefinitionArrayMember(destinationSection, "definition_detailed", (*sourceSection)["definitionDetailled"]);

		copyBoolMemberIfPresent(destinationSection, "definition_detailed_inherits_normal", *sourceSection);
	};

	const Value* airborneDefinition = nullptr;
	if (airborneLabel.HasMember("definition") && airborneLabel["definition"].IsArray())
		airborneDefinition = &airborneLabel["definition"];

	auto findAirborneStatusSection = [&](const char* statusKey) -> const Value*
	{
		if (statusKey == nullptr)
			return nullptr;
		if (!airborneLabel.HasMember("status_definitions") || !airborneLabel["status_definitions"].IsObject())
			return nullptr;
		const Value& airborneStatusDefinitions = airborneLabel["status_definitions"];
		if (!airborneStatusDefinitions.HasMember(statusKey) || !airborneStatusDefinitions[statusKey].IsObject())
			return nullptr;
		return &airborneStatusDefinitions[statusKey];
	};

	const Value* sourceDepartureAirborne = findAirborneStatusSection("airdep");
	if (sourceDepartureAirborne == nullptr && airborneDefinition != nullptr)
		sourceDepartureAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep", sourceDepartureAirborne);

	const Value* sourceDepartureOnRunway = findAirborneStatusSection("airdep_onrunway");
	if (sourceDepartureOnRunway == nullptr)
		sourceDepartureOnRunway = sourceDepartureAirborne;
	copyStatusDefinitionFromSource(departureStatusDefinitions, "airdep_onrunway", sourceDepartureOnRunway);

	const Value* sourceArrivalAirborne = findAirborneStatusSection("airarr");
	if (sourceArrivalAirborne == nullptr && airborneDefinition != nullptr)
		sourceArrivalAirborne = &airborneLabel;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr", sourceArrivalAirborne);

	const Value* sourceArrivalOnRunway = findAirborneStatusSection("airarr_onrunway");
	if (sourceArrivalOnRunway == nullptr)
		sourceArrivalOnRunway = sourceArrivalAirborne;
	copyStatusDefinitionFromSource(arrivalStatusDefinitions, "airarr_onrunway", sourceArrivalOnRunway);

	Value legacyLabelRules(kObjectType);
	bool hasLegacyLabelRules = false;
	if (labels.HasMember("rules") && labels["rules"].IsObject())
	{
		cloneJsonValue(legacyLabelRules, labels["rules"], cloneJsonValue);
		hasLegacyLabelRules = true;
	}

	Value& structuredRules = ensureObjectMember(profile, "rules");
	const bool structuredRulesHasItems = structuredRules.HasMember("items") && structuredRules["items"].IsArray();
	if (!structuredRulesHasItems && hasLegacyLabelRules &&
		legacyLabelRules.HasMember("items") && legacyLabelRules["items"].IsArray())
	{
		cloneJsonValue(structuredRules, legacyLabelRules, cloneJsonValue);
		changed = true;
	}

	if (hasLegacyLabelRules)
	{
		Value& labelsForRulesCleanup = profile["labels"];
		labelsForRulesCleanup.RemoveMember("rules");
		changed = true;
	}

	ensureIntMember(structuredRules, "version", 1, 1, 1000);
	if (!structuredRules.HasMember("items") || !structuredRules["items"].IsArray())
	{
		if (structuredRules.HasMember("items"))
			structuredRules.RemoveMember("items");
		Value itemsKey;
		itemsKey.SetString("items", allocator);
		Value itemsArray(kArrayType);
		structuredRules.AddMember(itemsKey, itemsArray, allocator);
		changed = true;
	}
	Value& structuredRuleItems = structuredRules["items"];

	auto parseRuleColor = [&](const Value& item, const char* key, bool& outApply, int& outR, int& outG, int& outB)
	{
		outApply = false;
		outR = 255;
		outG = 255;
		outB = 255;
		if (!item.IsObject() || !item.HasMember(key) || !item[key].IsObject())
			return false;

		const Value& color = item[key];
		if (!color.HasMember("r") || !color["r"].IsInt() ||
			!color.HasMember("g") || !color["g"].IsInt() ||
			!color.HasMember("b") || !color["b"].IsInt())
		{
			return false;
		}

		outApply = true;
		outR = std::clamp(color["r"].GetInt(), 0, 255);
		outG = std::clamp(color["g"].GetInt(), 0, 255);
		outB = std::clamp(color["b"].GetInt(), 0, 255);
		return true;
	};

	auto buildRuleSignature = [&](const std::string& source, const std::string& token, const std::string& condition,
		const std::string& tagType, const std::string& status, const std::string& detail,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB) -> std::string
	{
		std::ostringstream signature;
		signature << source << "|"
			<< token << "|"
			<< condition << "|"
			<< tagType << "|"
			<< status << "|"
			<< detail << "|"
			<< (applyTarget ? 1 : 0) << ":" << targetR << "," << targetG << "," << targetB << "|"
			<< (applyTag ? 1 : 0) << ":" << tagR << "," << tagG << "," << tagB << "|"
			<< (applyText ? 1 : 0) << ":" << textR << "," << textG << "," << textB;
		return signature.str();
	};

	std::set<std::string> existingRuleSignatures;
	for (rapidjson::SizeType i = 0; i < structuredRuleItems.Size(); ++i)
	{
		if (!structuredRuleItems[i].IsObject())
			continue;

		const Value& item = structuredRuleItems[i];
		std::string source = "vacdm";
		if (item.HasMember("source") && item["source"].IsString())
			source = item["source"].GetString();
		else if (item.HasMember("kind") && item["kind"].IsString())
			source = item["kind"].GetString();
		source = NormalizeStructuredRuleSource(source);

		std::string token;
		if (item.HasMember("token") && item["token"].IsString())
			token = item["token"].GetString();
		token = NormalizeStructuredRuleToken(source, token);
		if (token.empty())
			continue;

		std::string condition = "any";
		if (item.HasMember("condition") && item["condition"].IsString())
			condition = item["condition"].GetString();
		else if (source == "runway" && item.HasMember("runway") && item["runway"].IsString())
			condition = item["runway"].GetString();
		else if (source != "runway" && item.HasMember("state") && item["state"].IsString())
			condition = item["state"].GetString();
		condition = NormalizeStructuredRuleCondition(source, condition);

		std::string tagType = "any";
		if (item.HasMember("tag_type") && item["tag_type"].IsString())
			tagType = item["tag_type"].GetString();
		tagType = NormalizeStructuredRuleTagType(tagType);

		std::string status = "any";
		if (item.HasMember("status") && item["status"].IsString())
			status = item["status"].GetString();
		status = NormalizeStructuredRuleStatus(status);

		std::string detail = "any";
		if (item.HasMember("detail") && item["detail"].IsString())
			detail = item["detail"].GetString();
		detail = NormalizeStructuredRuleDetail(detail);

		bool applyTarget = false;
		bool applyTag = false;
		bool applyText = false;
		int targetR = 255, targetG = 255, targetB = 255;
		int tagR = 255, tagG = 255, tagB = 255;
		int textR = 255, textG = 255, textB = 255;
		parseRuleColor(item, "target_color", applyTarget, targetR, targetG, targetB);
		parseRuleColor(item, "tag_color", applyTag, tagR, tagG, tagB);
		parseRuleColor(item, "text_color", applyText, textR, textG, textB);
		if (!applyTarget && !applyTag && !applyText)
			continue;

		existingRuleSignatures.insert(buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB));
	}

	auto appendStructuredRule = [&](const std::string& sourceRaw, const std::string& tokenRaw, const std::string& conditionRaw,
		const std::string& tagTypeRaw, const std::string& statusRaw, const std::string& detailRaw,
		bool applyTarget, int targetR, int targetG, int targetB,
		bool applyTag, int tagR, int tagG, int tagB,
		bool applyText, int textR, int textG, int textB)
	{
		const std::string source = NormalizeStructuredRuleSource(sourceRaw);
		const std::string token = NormalizeStructuredRuleToken(source, tokenRaw);
		if (token.empty())
			return;
		const std::string condition = NormalizeStructuredRuleCondition(source, conditionRaw);
		const std::string tagType = NormalizeStructuredRuleTagType(tagTypeRaw);
		const std::string status = NormalizeStructuredRuleStatus(statusRaw);
		const std::string detail = NormalizeStructuredRuleDetail(detailRaw);

		targetR = std::clamp(targetR, 0, 255);
		targetG = std::clamp(targetG, 0, 255);
		targetB = std::clamp(targetB, 0, 255);
		tagR = std::clamp(tagR, 0, 255);
		tagG = std::clamp(tagG, 0, 255);
		tagB = std::clamp(tagB, 0, 255);
		textR = std::clamp(textR, 0, 255);
		textG = std::clamp(textG, 0, 255);
		textB = std::clamp(textB, 0, 255);

		if (!applyTarget && !applyTag && !applyText)
			return;

		const std::string signature = buildRuleSignature(
			source, token, condition, tagType, status, detail,
			applyTarget, targetR, targetG, targetB,
			applyTag, tagR, tagG, tagB,
			applyText, textR, textG, textB);
		if (!existingRuleSignatures.insert(signature).second)
			return;

		Value ruleObject(kObjectType);
		Value sourceKey;
		sourceKey.SetString("source", allocator);
		Value sourceValue;
		sourceValue.SetString(source.c_str(), static_cast<rapidjson::SizeType>(source.size()), allocator);
		ruleObject.AddMember(sourceKey, sourceValue, allocator);

		Value tokenKey;
		tokenKey.SetString("token", allocator);
		Value tokenValue;
		tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
		ruleObject.AddMember(tokenKey, tokenValue, allocator);

		Value conditionKey;
		conditionKey.SetString("condition", allocator);
		Value conditionValue;
		conditionValue.SetString(condition.c_str(), static_cast<rapidjson::SizeType>(condition.size()), allocator);
		ruleObject.AddMember(conditionKey, conditionValue, allocator);

		Value tagTypeKey;
		tagTypeKey.SetString("tag_type", allocator);
		Value tagTypeValue;
		tagTypeValue.SetString(tagType.c_str(), static_cast<rapidjson::SizeType>(tagType.size()), allocator);
		ruleObject.AddMember(tagTypeKey, tagTypeValue, allocator);

		Value statusKey;
		statusKey.SetString("status", allocator);
		Value statusValue;
		statusValue.SetString(status.c_str(), static_cast<rapidjson::SizeType>(status.size()), allocator);
		ruleObject.AddMember(statusKey, statusValue, allocator);

		Value detailKey;
		detailKey.SetString("detail", allocator);
		Value detailValue;
		detailValue.SetString(detail.c_str(), static_cast<rapidjson::SizeType>(detail.size()), allocator);
		ruleObject.AddMember(detailKey, detailValue, allocator);

		auto appendRuleColor = [&](const char* key, bool apply, int r, int g, int b)
		{
			if (!apply)
				return;
			Value colorObject(kObjectType);
			colorObject.AddMember("r", r, allocator);
			colorObject.AddMember("g", g, allocator);
			colorObject.AddMember("b", b, allocator);
			Value colorKey;
			colorKey.SetString(key, allocator);
			ruleObject.AddMember(colorKey, colorObject, allocator);
		};

		appendRuleColor("target_color", applyTarget, targetR, targetG, targetB);
		appendRuleColor("tag_color", applyTag, tagR, tagG, tagB);
		appendRuleColor("text_color", applyText, textR, textG, textB);

		structuredRuleItems.PushBack(ruleObject, allocator);
		changed = true;
	};

	auto migrateDefinitionArray = [&](Value& definitionArray, const std::string& tagType, const std::string& status, const std::string& detail)
	{
		// Move legacy inline color-rule tokens into structured rules and keep display tokens intact.
		if (!definitionArray.IsArray())
			return;

		for (rapidjson::SizeType lineIndex = 0; lineIndex < definitionArray.Size(); ++lineIndex)
		{
			Value& lineValue = definitionArray[lineIndex];
			std::vector<std::string> sourceTokens;
			if (lineValue.IsArray())
			{
				for (rapidjson::SizeType tokenIndex = 0; tokenIndex < lineValue.Size(); ++tokenIndex)
				{
					if (lineValue[tokenIndex].IsString())
						sourceTokens.push_back(lineValue[tokenIndex].GetString());
				}
			}
			else if (lineValue.IsString())
			{
				sourceTokens = SplitDefinitionTokens(lineValue.GetString());
			}
			else
			{
				continue;
			}

			if (sourceTokens.empty())
				continue;

			bool removedRuleToken = false;
			std::vector<std::string> keptTokens;
			keptTokens.reserve(sourceTokens.size());

			for (const std::string& rawToken : sourceTokens)
			{
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawToken);
				const std::string baseToken = styledToken.token.empty() ? rawToken : styledToken.token;

				VacdmColorRuleDefinition vacdmRuleToken;
				if (TryParseVacdmColorRuleToken(baseToken, vacdmRuleToken))
				{
					appendStructuredRule("vacdm", vacdmRuleToken.token, vacdmRuleToken.expectedState, tagType, status, detail,
						vacdmRuleToken.hasTargetColor, vacdmRuleToken.targetR, vacdmRuleToken.targetG, vacdmRuleToken.targetB,
						vacdmRuleToken.hasTagColor, vacdmRuleToken.tagR, vacdmRuleToken.tagG, vacdmRuleToken.tagB,
						vacdmRuleToken.hasTextColor, vacdmRuleToken.textR, vacdmRuleToken.textG, vacdmRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				RunwayColorRuleDefinition runwayRuleToken;
				if (TryParseRunwayColorRuleToken(baseToken, runwayRuleToken))
				{
					appendStructuredRule("runway", runwayRuleToken.token, runwayRuleToken.expectedRunway, tagType, status, detail,
						runwayRuleToken.hasTargetColor, runwayRuleToken.targetR, runwayRuleToken.targetG, runwayRuleToken.targetB,
						runwayRuleToken.hasTagColor, runwayRuleToken.tagR, runwayRuleToken.tagG, runwayRuleToken.tagB,
						runwayRuleToken.hasTextColor, runwayRuleToken.textR, runwayRuleToken.textG, runwayRuleToken.textB);
					removedRuleToken = true;
					continue;
				}

				keptTokens.push_back(rawToken);
			}

			if (!removedRuleToken)
				continue;

			Value newLine(kArrayType);
			for (const std::string& token : keptTokens)
			{
				Value tokenValue;
				tokenValue.SetString(token.c_str(), static_cast<rapidjson::SizeType>(token.size()), allocator);
				newLine.PushBack(tokenValue, allocator);
			}

			lineValue = newLine;
			changed = true;
		}
	};

	auto migrateTypeDefinitions = [&](const char* typeKey)
	{
		Value& labelsForMigration = profile["labels"];
		if (!labelsForMigration.HasMember(typeKey) || !labelsForMigration[typeKey].IsObject())
			return;

		Value& section = labelsForMigration[typeKey];
		if (section.HasMember("definition") && section["definition"].IsArray())
			migrateDefinitionArray(section["definition"], typeKey, "default", "normal");
		if (section.HasMember("definition_detailed") && section["definition_detailed"].IsArray())
			migrateDefinitionArray(section["definition_detailed"], typeKey, "default", "detailed");

		if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
			return;

		Value& statusDefinitions = section["status_definitions"];
		for (auto statusIt = statusDefinitions.MemberBegin(); statusIt != statusDefinitions.MemberEnd(); ++statusIt)
		{
			if (!statusIt->name.IsString() || !statusIt->value.IsObject())
				continue;
			const std::string statusKey = statusIt->name.GetString();
			Value& statusSection = statusIt->value;
			if (statusSection.HasMember("definition") && statusSection["definition"].IsArray())
				migrateDefinitionArray(statusSection["definition"], typeKey, statusKey, "normal");
			if (statusSection.HasMember("definition_detailed") && statusSection["definition_detailed"].IsArray())
				migrateDefinitionArray(statusSection["definition_detailed"], typeKey, statusKey, "detailed");
		}
	};

	migrateTypeDefinitions("departure");
	migrateTypeDefinitions("arrival");
	migrateTypeDefinitions("airborne");
	migrateTypeDefinitions("uncorrelated");

	// A validated backup or a migrated read-only source may be active in memory.
	// Normalize that working copy for runtime use, but leave recovery to the
	// explicit Settings flow instead of showing a spurious startup save error.
	if (changed && persistChanges && CurrentConfig->isConfigHealthy() && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save status settings to vSMR_Profiles.json", true, true, false, false, false);
	}
}

void CSMRRadar::RebuildProfileColorEntries()
{
	ProfileColorPaths.clear();
	ProfileColorPathHasAlpha.clear();

	if (!CurrentConfig)
		return;

	const rapidjson::Value& activeProfile = CurrentConfig->getActiveProfile();
	CollectProfileColorPaths(activeProfile, "", ProfileColorPaths, ProfileColorPathHasAlpha);
	std::sort(ProfileColorPaths.begin(), ProfileColorPaths.end());
}

bool CSMRRadar::IsProfileColorPathValid(const std::string& path, bool* hasAlpha)
{
	if (hasAlpha)
		*hasAlpha = false;

	if (!CurrentConfig || path.empty())
		return false;

	std::vector<ProfileColorPathToken> tokens = ParseProfileColorPath(path);
	if (tokens.empty())
		return false;

	rapidjson::Value& activeProfile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	rapidjson::Value* colorValue = ResolveProfilePath(activeProfile, tokens);
	if (!colorValue)
		return false;

	return IsColorConfigObject(*colorValue, hasAlpha);
}

int CSMRRadar::GetProfileColorComponentValue(const std::string& path, char component, int fallback)
{
	if (!CurrentConfig || path.empty())
		return fallback;

	const char* componentKey = ColorComponentKey(component);
	if (!componentKey)
		return fallback;

	std::vector<ProfileColorPathToken> tokens = ParseProfileColorPath(path);
	if (tokens.empty())
		return fallback;

	rapidjson::Value& activeProfile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	rapidjson::Value* colorValue = ResolveProfilePath(activeProfile, tokens);
	if (!colorValue)
		return fallback;

	bool hasAlpha = false;
	if (!IsColorConfigObject(*colorValue, &hasAlpha))
		return fallback;

	const char normalized = NormalizeProfileColorComponent(component);
	if (normalized == 'a' && !hasAlpha)
		return fallback;

	if (!colorValue->HasMember(componentKey) || !(*colorValue)[componentKey].IsInt())
		return fallback;

	return (*colorValue)[componentKey].GetInt();
}

bool CSMRRadar::UpdateProfileColorComponent(const std::string& path, char component, int value)
{
	if (!CurrentConfig || path.empty())
		return false;

	const char* componentKey = ColorComponentKey(component);
	if (!componentKey)
		return false;

	std::vector<ProfileColorPathToken> tokens = ParseProfileColorPath(path);
	if (tokens.empty())
		return false;

	rapidjson::Value& activeProfile = const_cast<rapidjson::Value&>(CurrentConfig->getActiveProfile());
	rapidjson::Value* colorValue = ResolveProfilePath(activeProfile, tokens);
	if (!colorValue)
		return false;

	bool hasAlpha = false;
	if (!IsColorConfigObject(*colorValue, &hasAlpha))
		return false;

	const char normalized = NormalizeProfileColorComponent(component);
	const int clamped = (value < 0) ? 0 : ((value > 255) ? 255 : value);
	if (normalized == 'a' && !hasAlpha)
	{
		rapidjson::Value key;
		key.SetString("a", CurrentConfig->document.GetAllocator());
		rapidjson::Value alphaValue;
		alphaValue.SetInt(clamped);
		colorValue->AddMember(key, alphaValue, CurrentConfig->document.GetAllocator());
		return true;
	}

	if (!colorValue->HasMember(componentKey) || !(*colorValue)[componentKey].IsInt())
		return false;

	(*colorValue)[componentKey].SetInt(clamped);
	return true;
}

map<string, string> CSMRRadar::GenerateTagData(CRadarTarget rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, bool useSpeedForGates, string ActiveAirport, const std::string& stableCallsign, const VacdmPilotData* capturedVacdmData, const int* capturedPreviousFlightLevel)
{
	(void)isASEL;
	(void)ActiveAirport;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	auto verboseStep = [&](const std::string& step)
	{
		if (!Logger::is_verbose_mode())
			return;

		Logger::info("GenerateTagData: " + step);
	};
	verboseStep("begin stable_callsign=" + (stableCallsign.empty() ? std::string("<empty>") : stableCallsign));
	// ----
	// Tag items available
	// callsign: Callsign with freq state and comm *
	// actype: Aircraft type *
	// sctype: Aircraft type that changes for squawk error *
	// sqerror: Squawk error if there is one, or empty *
	// deprwy: Departure runway *
	// seprwy: Departure runway that changes to speed if speed > 25kts *
	// arvrwy: Arrival runway *
	// srvrwy: Speed that changes to arrival runway if speed < 25kts *
	// gate: Gate, from speed or scratchpad *
	// sate: Gate, from speed or scratchpad that changes to speed if speed > 25kts *
	// flightlevel: Flightlevel/Pressure altitude of the ac *
	// gs: Ground speed of the ac *
	// tobt: VACDM TOBT (HHMM)
	// tsat: VACDM TSAT (HHMM)
	// ttot: VACDM TTOT (HHMM)
	// asat: VACDM ASAT (HHMM)
	// aobt: VACDM AOBT (HHMM)
	// atot: VACDM ATOT (HHMM)
	// asrt: VACDM ASRT (HHMM)
	// aort: VACDM AORT (HHMM)
	// ctot: VACDM CTOT (HHMM)
	// event_booking: VACDM event booking flag ("B")
	// tendency: Climbing or descending symbol *
	// wake: Wake turbulance cat *
	// groundstatus: Current status *
	// ssr: the current squawk of the ac
	// asid: the assigned SID
	// ssid: a short version of the SID
	// origin: origin aerodrome
	// dest: destination aerodrome
	// clearance: departure/startup clearance flag ([ ] / [x]), clickable toggle
	// ----

	auto safeCString = [](const char* text) -> const char*
	{
		return text != nullptr ? text : "";
	};
	auto safeString = [&](const char* text) -> std::string
	{
		return text != nullptr ? std::string(text) : std::string();
	};
	const bool radarTargetValid = rt.IsValid();
	CRadarTargetPositionData rtPos;
	if (radarTargetValid)
		rtPos = rt.GetPosition();
	const bool hasRadarTarget = radarTargetValid && rtPos.IsValid();

	const bool hasFlightPlan = fp.IsValid();
	const bool hasReceivedFlightPlanData = hasFlightPlan && fp.GetFlightPlanData().IsReceived();
	const int reportedGs = hasRadarTarget ? rtPos.GetReportedGS() : 0;
	bool IsPrimary = hasRadarTarget ? !rtPos.GetTransponderC() : true;
	bool isAirborne = reportedGs > 50;
	verboseStep(
		"snapshot has_rt=" + std::string(hasRadarTarget ? "1" : "0") +
		" has_fp=" + std::string(hasFlightPlan ? "1" : "0") +
		" fp_received=" + std::string(hasReceivedFlightPlanData ? "1" : "0") +
		" reported_gs=" + std::to_string(reportedGs) +
		" corr=" + std::string(isAcCorrelated ? "1" : "0"));

	// ----- Callsign -------
	string callsign = stableCallsign;
	if (callsign.empty())
		callsign = safeString(radarTargetValid ? rt.GetCallsign() : nullptr);
	if (callsign.empty())
		callsign = safeString(hasFlightPlan ? fp.GetCallsign() : nullptr);
	if (hasReceivedFlightPlanData) {
		if (fp.GetControllerAssignedData().GetCommunicationType() == 't' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'T' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'r' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'R' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'v' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'V')
		{
			if (fp.GetControllerAssignedData().GetCommunicationType() != 'v' &&
				fp.GetControllerAssignedData().GetCommunicationType() != 'V') {
				callsign.append("/");
				callsign += fp.GetControllerAssignedData().GetCommunicationType();
			}
		}
		else if (fp.GetFlightPlanData().GetCommunicationType() == 't' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'r' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'T' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'R')
		{
			callsign.append("/");
			callsign += fp.GetFlightPlanData().GetCommunicationType();
		}

		switch (fp.GetState()) {

		case FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED:
			callsign = ">>" + callsign;
			break;

		case FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED:
			callsign = callsign + ">>";
			break;

		case FLIGHT_PLAN_STATE_ASSUMED:
			callsign = "[" + callsign + "]";
			break;

		}
	}

	// ----- Squawk error -------
	string sqerror = "";
	const char* assr = hasFlightPlan ? safeCString(fp.GetControllerAssignedData().GetSquawk()) : "";
	const char* ssr = hasRadarTarget ? safeCString(rtPos.GetSquawk()) : "";
	bool has_squawk_error = false;
	if (strlen(assr) != 0 && strlen(ssr) != 0 && !startsWith(ssr, assr)) {
		has_squawk_error = true;
		sqerror = "A";
		sqerror.append(assr);
	}

	verboseStep("callsign token prepared value=" + (callsign.empty() ? std::string("<empty>") : callsign));

	// ----- Aircraft type -------

	string actype = "NoFPL";
	if (hasReceivedFlightPlanData)
		actype = safeString(fp.GetFlightPlanData().GetAircraftFPType());
	if (actype.size() > 4 && actype != "NoFPL")
		actype = actype.substr(0, 4);

	// ----- Aircraft type that changes to squawk error -------
	string sctype = actype;
	if (has_squawk_error)
		sctype = sqerror;

	// ----- Groundspeed -------
	string speed = std::to_string(reportedGs);

	// ----- Departure runway -------
	string deprwy = hasReceivedFlightPlanData ? safeString(fp.GetFlightPlanData().GetDepartureRwy()) : "";
	if (deprwy.length() == 0)
		deprwy = "RWY";

	// ----- Departure runway that changes for overspeed -------
	string seprwy = deprwy;
	if (hasRadarTarget && reportedGs > 25)
		seprwy = std::to_string(reportedGs);

	// ----- Arrival runway -------
	string arvrwy = hasReceivedFlightPlanData ? safeString(fp.GetFlightPlanData().GetArrivalRwy()) : "";
	if (arvrwy.length() == 0)
		arvrwy = "RWY";

	// ----- Speed that changes to arrival runway -----
	string srvrwy = speed;
	if (hasRadarTarget && reportedGs < 25)
		srvrwy = arvrwy;

	// ----- Gate -------
	string gate;
	if (hasFlightPlan)
	{
		if (useSpeedForGates)
			gate = std::to_string(fp.GetControllerAssignedData().GetAssignedSpeed());
		else
			gate = safeString(fp.GetControllerAssignedData().GetScratchPadString());
	}

	replaceAll(gate, "STAND=", "");
	if (gate.size() > 4)
		gate = gate.substr(0, 4);

	if (gate.size() == 0 || gate == "0" || !isAcCorrelated)
		gate = "NoGate";

	// ----- Gate that changes to speed -------
	string sate = gate;
	if (hasRadarTarget && reportedGs > 25)
		sate = speed;

	// ----- Flightlevel -------
	int fl = hasRadarTarget ? rtPos.GetFlightLevel() : 0;
	int padding = 5;
	string pfls = "";
	if (fl <= TransitionAltitude) {
		fl = hasRadarTarget ? rtPos.GetPressureAltitude() : 0;
		pfls = "A";
		padding = 4;
	}
	string flightlevel = (pfls + padWithZeros(padding, fl)).substr(0, 3);

	// ----- Tendency -------
	string tendency = "-";
	int delta_fl = 0;
	if (hasRadarTarget && capturedPreviousFlightLevel != nullptr)
		delta_fl = rtPos.GetFlightLevel() - *capturedPreviousFlightLevel;
	if (abs(delta_fl) >= 50) {
		if (delta_fl < 0) {
			tendency = "|";
		}
		else {
			tendency = "^";
		}
	}

	// ----- Wake cat -------
	string wake = "?";
	if (hasReceivedFlightPlanData && isAcCorrelated) {
		wake = "";
		wake += fp.GetFlightPlanData().GetAircraftWtc();
	}

	// ----- SSR -------
	string tssr = hasRadarTarget ? safeCString(rtPos.GetSquawk()) : "";

	// ----- SID -------
	string dep = "SID";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		dep = safeString(fp.GetFlightPlanData().GetSidName());
	}

	// ----- Short SID -------
	string ssid = dep;
	if (hasFlightPlan && ssid.size() > 5 && isAcCorrelated)
	{
		ssid = dep.substr(0, 3);
		ssid += dep.substr(dep.size() - 2, dep.size());
	}

	// ------- Origin aerodrome -------
	string origin = "????";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		origin = safeString(fp.GetFlightPlanData().GetOrigin());
	}

	// ------- Destination aerodrome -------
	string dest = "????";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		dest = safeString(fp.GetFlightPlanData().GetDestination());
	}

	// ----- GSTAT -------
	string gstat = "STS";
	if (hasReceivedFlightPlanData && isAcCorrelated) {
		const char* groundState = safeCString(fp.GetGroundState());
		std::string stateCallsign = stableCallsign;
		if (stateCallsign.empty())
			stateCallsign = safeString(radarTargetValid ? rt.GetCallsign() : nullptr);
		if (stateCallsign.empty())
			stateCallsign = safeString(fp.GetCallsign());
		const GroundStateCategory observedState = classifyGroundState(groundState, reportedGs, false);
		if (VsmrGroundState::IsLineupOverrideActive(stateCallsign.c_str(), observedState))
			gstat = "LNUP";
		else if (strlen(groundState) != 0)
			gstat = groundState;
	}

	// ----- Clearance flag -------
	string clearance = "";
	if (hasFlightPlan && isAcCorrelated)
		clearance = fp.GetClearenceFlag() ? "[x]" : "[ ]";

	// ----- UK Controller Plugin / Assigned Stand -------
	string uk_stand;
	if (hasFlightPlan)
		uk_stand = safeString(fp.GetControllerAssignedData().GetFlightStripAnnotation(3));
	if (uk_stand.length() == 0)
		uk_stand = "";

	// ----- Ramp Agent Remark -------
	string remark;
	if (hasFlightPlan)
		remark = safeString(fp.GetControllerAssignedData().GetFlightStripAnnotation(4));
	if (remark.length() == 0)
		remark = "";
	
	// ----- Scratchpad -------
	string scratchpad;
	if (hasFlightPlan)
		scratchpad = safeString(fp.GetControllerAssignedData().GetScratchPadString());
	if (scratchpad.length() == 0)
		scratchpad = "...";

	// ----- VACDM fields -------
	string tobt = "";
	string tsat = "";
	string ttot = "";
	string asat = "";
	string aobt = "";
	string atot = "";
	string asrt = "";
	string aort = "";
	string ctot = "";
	string eventBooking = "";
	if (capturedVacdmData != nullptr)
	{
		const VacdmPilotData& vacdmPilot = *capturedVacdmData;
		if (vacdmPilot.hasTobt)
			tobt = FormatVacdmTimeToken(vacdmPilot.tobtUtc);
		if (vacdmPilot.hasTsat)
			tsat = FormatVacdmTimeToken(vacdmPilot.tsatUtc);
		if (vacdmPilot.hasTtot)
			ttot = FormatVacdmTimeToken(vacdmPilot.ttotUtc);
		if (vacdmPilot.hasAsat)
			asat = FormatVacdmTimeToken(vacdmPilot.asatUtc);
		if (vacdmPilot.hasAobt)
			aobt = FormatVacdmTimeToken(vacdmPilot.aobtUtc);
		if (vacdmPilot.hasAtot)
			atot = FormatVacdmTimeToken(vacdmPilot.atotUtc);
		if (vacdmPilot.hasAsrt)
			asrt = FormatVacdmTimeToken(vacdmPilot.asrtUtc);
		if (vacdmPilot.hasAort)
			aort = FormatVacdmTimeToken(vacdmPilot.aortUtc);
		if (vacdmPilot.hasCtot)
			ctot = FormatVacdmTimeToken(vacdmPilot.ctotUtc);
		eventBooking = vacdmPilot.hasBooking ? "B" : "";
	}

	// VACDM fallback: when backend has no entry, use FPL EOBT as TOBT baseline (matches VACDM plugin bootstrap behavior).
	if (tobt.empty() && hasReceivedFlightPlanData && isAcCorrelated)
		tobt = NormalizeHhmmToken(safeCString(fp.GetFlightPlanData().GetEstimatedDepartureTime()));


	// ----- Generating the replacing map -----
	map<string, string> TagReplacingMap;

	// System ID for uncorrelated
	TagReplacingMap["systemid"] = "T:";
	string tpss = callsign;
	if (tpss.empty())
		tpss = "000000";
	if (tpss.size() > 1)
		TagReplacingMap["systemid"].append(tpss.substr(1, min<size_t>(6, tpss.size() - 1)));
	else if (!tpss.empty())
		TagReplacingMap["systemid"].append(tpss.substr(0, min<size_t>(6, tpss.size())));
	else
		TagReplacingMap["systemid"].append("000000");

	// Display modes with the squawk rule enabled use SSR-centric fallback data.
	if (isProMode)
	{

		if (isAirborne && !isAcCorrelated)
		{
			callsign = tssr;
		}

		if (!isAcCorrelated)
		{
			actype = "NoFPL";
		}

		// Is a primary target

		if (isAirborne && !isAcCorrelated && IsPrimary)
		{
			flightlevel = "NoALT";
			tendency = "?";
			speed = std::to_string(reportedGs);
		}

		if (isAirborne && !isAcCorrelated && IsPrimary)
		{
			callsign = TagReplacingMap["systemid"];
		}
	}

	TagReplacingMap["callsign"] = callsign;
	TagReplacingMap["actype"] = actype;
	TagReplacingMap["sctype"] = sctype;
	TagReplacingMap["sqerror"] = sqerror;
	TagReplacingMap["deprwy"] = deprwy;
	TagReplacingMap["seprwy"] = seprwy;
	TagReplacingMap["arvrwy"] = arvrwy;
	TagReplacingMap["srvrwy"] = srvrwy;
	TagReplacingMap["gate"] = gate;
	TagReplacingMap["sate"] = sate;
	TagReplacingMap["flightlevel"] = flightlevel;
	TagReplacingMap["gs"] = speed;
	TagReplacingMap["tobt"] = tobt;
	TagReplacingMap["tsat"] = tsat;
	TagReplacingMap["ttot"] = ttot;
	TagReplacingMap["asat"] = asat;
	TagReplacingMap["aobt"] = aobt;
	TagReplacingMap["atot"] = atot;
	TagReplacingMap["asrt"] = asrt;
	TagReplacingMap["aort"] = aort;
	TagReplacingMap["ctot"] = ctot;
	TagReplacingMap["event_booking"] = eventBooking;
	TagReplacingMap["tendency"] = tendency;
	TagReplacingMap["wake"] = wake;
	TagReplacingMap["ssr"] = tssr;
	TagReplacingMap["asid"] = dep;
	TagReplacingMap["ssid"] = ssid;
	TagReplacingMap["origin"] = origin;
	TagReplacingMap["dest"] = dest;
	TagReplacingMap["groundstatus"] = gstat;
	TagReplacingMap["clearance"] = clearance;
	TagReplacingMap["uk_stand"] = uk_stand;
	TagReplacingMap["remark"] = remark;
	TagReplacingMap["scratchpad"] = scratchpad;
	verboseStep(
		"done callsign=" + TagReplacingMap["callsign"] +
		" actype=" + TagReplacingMap["actype"] +
		" gs=" + TagReplacingMap["gs"] +
		" sid=" + TagReplacingMap["asid"] +
		" corr=" + std::string(isAcCorrelated ? "1" : "0"));

	return TagReplacingMap;
}

void UnhookAvisoMouseHook()
{
	if (gThreadMouseHook == nullptr)
		return;

	::UnhookWindowsHookEx(gThreadMouseHook);
	gThreadMouseHook = nullptr;
	gThreadMouseHookThreadId = 0;
}

void UnhookAvisoKeyboardHook()
{
	if (gThreadKeyboardHook == nullptr)
		return;

	::UnhookWindowsHookEx(gThreadKeyboardHook);
	gThreadKeyboardHook = nullptr;
	gThreadKeyboardHookThreadId = 0;
}

void UnhookAvisoThreadHooks()
{
	UnhookAvisoMouseHook();
	UnhookAvisoKeyboardHook();
}

void ClearAvisoWheelRoutingState(bool cancelWindowInteractions = false)
{
	gAvisoWheelRoutingEnabled = false;

	for (CSMRRadar* radarScreen : RadarScreensOpened)
	{
		if (radarScreen == nullptr)
			continue;

		radarScreen->AvisoGeoJsonScrollSelected = false;
		if (cancelWindowInteractions)
			radarScreen->CancelInsetWindowInteractions();
		for (auto& appWindow : radarScreen->appWindows)
		{
			CInsetWindow* insetWindow = appWindow.second.get();
			if (insetWindow == nullptr)
				continue;

			if (!cancelWindowInteractions && insetWindow->m_AvisoRightPanning)
				insetWindow->EndAvisoPan();
			if (cancelWindowInteractions)
			{
				insetWindow->m_AvisoScrollSelected = false;
				insetWindow->m_AvisoScreenArea = { 0, 0, 0, 0 };
				insetWindow->m_AvisoScreenAreaValid = false;
				insetWindow->m_AvisoRenderWindow = nullptr;
			}
		}
	}
}

bool IsEuroScopeViewSwitchKey(WPARAM key)
{
	return key >= VK_F1 && key <= VK_F12;
}

bool IsMouseButtonDownMessage(WPARAM message)
{
	switch (message)
	{
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_NCLBUTTONDOWN:
	case WM_NCRBUTTONDOWN:
	case WM_NCMBUTTONDOWN:
	case WM_NCXBUTTONDOWN:
		return true;
	default:
		return false;
	}
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	const auto sourceProcIt = gInsetWindowSourceProcs.find(hwnd);
	const WNDPROC sourceProc = sourceProcIt != gInsetWindowSourceProcs.end()
		? sourceProcIt->second
		: nullptr;
	const auto forwardMessage = [&]() -> LRESULT
	{
		return sourceProc != nullptr
			? ::CallWindowProc(sourceProc, hwnd, uMsg, wParam, lParam)
			: ::DefWindowProc(hwnd, uMsg, wParam, lParam);
	};
	if (uMsg == WM_NCDESTROY)
	{
		const LRESULT result = forwardMessage();
		gInsetWindowSourceProcs.erase(hwnd);
		gInsetWindowRadarScreens.erase(hwnd);
		return result;
	}
	const UINT workerRefreshMessage = AvisoWorkerRefreshMessage();
	if (workerRefreshMessage != 0 && uMsg == workerRefreshMessage)
	{
		const auto radarIt = gInsetWindowRadarScreens.find(hwnd);
		CSMRRadar* requestedRadar = reinterpret_cast<CSMRRadar*>(wParam);
		if (requestedRadar != nullptr &&
			radarIt != gInsetWindowRadarScreens.end() &&
			radarIt->second == requestedRadar &&
			!requestedRadar->IsShutdownRequested())
		{
			try
			{
				requestedRadar->RequestRefresh();
			}
			catch (...)
			{
				Logger::info("AVISO UI refresh request failed");
			}
		}
		return 0;
	}

	switch (uMsg)
	{
	case WM_MOUSEWHEEL:
		if (gAvisoWheelRoutingEnabled && gWindowProcRadarScreen != nullptr && gWindowProcRadarScreen->HandleAvisoMouseWheel(hwnd, wParam, lParam))
			return 0;
		break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (IsEuroScopeViewSwitchKey(wParam))
			ClearAvisoWheelRoutingState(true);
		break;
	case WM_SETCURSOR:
		if (gWindowProcRadarScreen != nullptr && gWindowProcRadarScreen->HandleInsetSetCursor(hwnd))
			return true;
		// SetCursor(nullptr) explicitly hides the pointer. If cursor setup has not
		// completed (or a custom resource failed), let EuroScope choose its cursor.
		if (smrCursor != nullptr)
		{
			::SetCursor(smrCursor);
			return true;
		}
		break;
	default:
		return forwardMessage();
	}

	return forwardMessage();
}

void EnsureInsetWindowProcHook(HWND hwnd, CSMRRadar* radarScreen)
{
	if (hwnd == nullptr || !::IsWindow(hwnd) || radarScreen == nullptr)
		return;

	const WNDPROC currentProc = reinterpret_cast<WNDPROC>(
		::GetWindowLongPtr(hwnd, GWLP_WNDPROC));
	const auto existing = gInsetWindowSourceProcs.find(hwnd);
	if (existing != gInsetWindowSourceProcs.end())
	{
		// Another component may have subclassed above us. In that case our proc
		// remains in its forwarding chain; installing it again would create a loop.
		gWindowProcRadarScreen = radarScreen;
		gInsetWindowRadarScreens[hwnd] = radarScreen;
		return;
	}
	if (currentProc == nullptr || currentProc == WindowProc)
		return;

	::SetLastError(ERROR_SUCCESS);
	const WNDPROC previousProc = reinterpret_cast<WNDPROC>(
		::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
	if (previousProc == nullptr && ::GetLastError() != ERROR_SUCCESS)
	{
		Logger::info("Inset window procedure hook installation failed error=" + std::to_string(::GetLastError()));
		return;
	}
	const WNDPROC sourceProc = previousProc != nullptr ? previousProc : currentProc;
	if (sourceProc == WindowProc)
		return;

	gInsetWindowSourceProcs.emplace(hwnd, sourceProc);
	gInsetWindowRadarScreens[hwnd] = radarScreen;
	gWindowProcRadarScreen = radarScreen;
}

void RestoreInsetWindowProcHooks()
{
	for (const auto& entry : gInsetWindowSourceProcs)
	{
		HWND hwnd = entry.first;
		if (hwnd == nullptr || !::IsWindow(hwnd))
			continue;
		const WNDPROC currentProc = reinterpret_cast<WNDPROC>(
			::GetWindowLongPtr(hwnd, GWLP_WNDPROC));
		if (currentProc == WindowProc)
			::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(entry.second));
	}
	gInsetWindowSourceProcs.clear();
	gInsetWindowRadarScreens.clear();
}

bool TryHandleAvisoWheel(POINT screenPoint, int wheelDelta, HWND sourceHwnd)
{
	if (!gAvisoWheelRoutingEnabled || gWindowProcRadarScreen == nullptr || wheelDelta == 0)
		return false;

	return gWindowProcRadarScreen->HandleAvisoMouseWheelAtScreenPoint(screenPoint, wheelDelta, sourceHwnd);
}

LRESULT CALLBACK MouseMessageHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0 && IsMouseButtonDownMessage(wParam))
		ClearAvisoWheelRoutingState();

	if (code >= 0 && gWindowProcRadarScreen != nullptr && wParam == WM_MOUSEWHEEL && lParam != 0)
	{
		MOUSEHOOKSTRUCTEX* mouseData = reinterpret_cast<MOUSEHOOKSTRUCTEX*>(lParam);
		const int wheelDelta = static_cast<short>(HIWORD(mouseData->mouseData));
		HWND sourceHwnd = mouseData->hwnd;
		if (sourceHwnd == nullptr || !::IsWindow(sourceHwnd))
			sourceHwnd = ::WindowFromPoint(mouseData->pt);
		if (TryHandleAvisoWheel(mouseData->pt, wheelDelta, sourceHwnd))
			return 1;
	}

	return ::CallNextHookEx(gThreadMouseHook, code, wParam, lParam);
}

LRESULT CALLBACK KeyboardMessageHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	const bool keyReleased = (static_cast<ULONG_PTR>(lParam) & 0x80000000ULL) != 0;
	if (code >= 0 && !keyReleased && IsEuroScopeViewSwitchKey(wParam))
		ClearAvisoWheelRoutingState(true);

	return ::CallNextHookEx(gThreadKeyboardHook, code, wParam, lParam);
}

void EnsureAvisoWheelHooks(CSMRRadar* radarScreen)
{
	if (radarScreen == nullptr)
		return;

	gWindowProcRadarScreen = radarScreen;

	const DWORD currentThreadId = ::GetCurrentThreadId();
	if (gThreadMouseHook != nullptr && gThreadMouseHookThreadId != currentThreadId)
		UnhookAvisoMouseHook();
	if (gThreadKeyboardHook != nullptr && gThreadKeyboardHookThreadId != currentThreadId)
		UnhookAvisoKeyboardHook();

	if (gThreadMouseHook == nullptr)
	{
		gThreadMouseHook = ::SetWindowsHookEx(WH_MOUSE, MouseMessageHookProc, nullptr, currentThreadId);
		if (gThreadMouseHook == nullptr)
		{
			const DWORD error = ::GetLastError();
			if (gLastThreadHookError != error)
			{
				Logger::info("AVISO viewport thread wheel hook install failed error=" + std::to_string(error));
				gLastThreadHookError = error;
			}
		}
		else
		{
			gThreadMouseHookThreadId = currentThreadId;
			gLastThreadHookError = ERROR_SUCCESS;
			Logger::info("AVISO viewport thread wheel hook installed thread=" + std::to_string(currentThreadId));
		}
	}

	if (gThreadKeyboardHook == nullptr)
	{
		gThreadKeyboardHook = ::SetWindowsHookEx(WH_KEYBOARD, KeyboardMessageHookProc, nullptr, currentThreadId);
		if (gThreadKeyboardHook == nullptr)
		{
			const DWORD error = ::GetLastError();
			if (gLastThreadKeyboardHookError != error)
			{
				Logger::info("AVISO viewport thread keyboard hook install failed error=" + std::to_string(error));
				gLastThreadKeyboardHookError = error;
			}
		}
		else
		{
			gThreadKeyboardHookThreadId = currentThreadId;
			gLastThreadKeyboardHookError = ERROR_SUCCESS;
			Logger::info("AVISO viewport thread keyboard hook installed thread=" + std::to_string(currentThreadId));
		}
	}

}

void CSMRRadar::OnRefresh(HDC hDC, int Phase)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnRefresh",
		reinterpret_cast<std::uintptr_t>(this));
	if (IsShutdownRequested())
		return;
	PublishCrashRadarState("main");

	VSMR_REFRESH_LOG(string(__FUNCSIG__));
	const char* refreshStage = "entry";
	auto setRefreshStage = [&](const char* stage)
	{
		refreshStage = stage;
		VsmrCrashReporter::RecordBreadcrumb("render stage", stage);
	};
	VsmrCrashReporter::RecordBreadcrumb("render stage", refreshStage);
	auto logRefreshException = [&](const std::string& reason)
	{
		static DWORD lastLogTick = 0;
		static std::string lastMessage;
		const DWORD nowTick = ::GetTickCount();
		const std::string message = "OnRefresh caught exception stage=" + std::string(refreshStage) + " reason=" + reason;
		if (message != lastMessage || nowTick - lastLogTick > 2000)
		{
			OutputVsmrDebugLine(message);
			lastMessage = message;
			lastLogTick = nowTick;
		}
	};

	try
	{
	if (Logger::is_verbose_mode())
	{
		Logger::info(
			"OnRefresh begin phase=" + std::to_string(Phase) +
			" tag_collision_count=" + std::to_string(tagCollisionAreas.size()) +
			" tag_offset_count=" + std::to_string(TagsOffsets.size()) +
				" active_airport=" + getActiveAirport());
	}

	if (CurrentConfig == nullptr || RimcasInstance == nullptr)
	{
		static bool loggedMissingCoreObjects = false;
		if (!loggedMissingCoreObjects)
		{
			Logger::info("OnRefresh: skipped frame because core objects are not initialized");
			loggedMissingCoreObjects = true;
		}
		return;
	}

	EnsureAvisoWheelHooks(this);
	// Refresh pipeline is phase-driven by EuroScope. Cursor setup stays on the UI thread.
	if (initCursor)
	{
		if (customCursor) {
			HCURSOR loadedCursor = reinterpret_cast<HCURSOR>(::LoadImage(
				AfxGetInstanceHandle(),
				MAKEINTRESOURCE(IDC_SMRCURSOR),
				IMAGE_CURSOR,
				0,
				0,
				LR_SHARED));
			if (loadedCursor != nullptr)
				smrCursor = CopyCursor(loadedCursor);
			// EuroScope/MFC can still override the cursor occasionally; we therefore reapply it via window proc hook.

		}
		if (smrCursor == nullptr)
			smrCursor = ::LoadCursor(nullptr, IDC_ARROW);
		if (smrCursor != nullptr)
			::SetCursor(smrCursor);

		initCursor = false;
	}
	HWND insetHostWindow = ::WindowFromDC(hDC);
	if (insetHostWindow == nullptr || !::IsWindow(insetHostWindow))
		insetHostWindow = ::GetActiveWindow();
	EnsureInsetWindowProcHook(insetHostWindow, this);
	AvisoRefreshHostWindow.store(insetHostWindow, std::memory_order_release);

	if (Phase == REFRESH_PHASE_AFTER_LISTS) {
		VSMR_REFRESH_LOG("Phase == REFRESH_PHASE_AFTER_LISTS");
		VSMR_REFRESH_LOG("break Phase == REFRESH_PHASE_AFTER_LISTS");
		return;
	}

	if (Phase != REFRESH_PHASE_BEFORE_TAGS)
		return;

	VSMR_REFRESH_LOG("Phase != REFRESH_PHASE_BEFORE_TAGS");
	const unsigned long fpsNowTick = ::GetTickCount();
	if (FpsLastSampleTick == 0)
		FpsLastSampleTick = fpsNowTick;
	++FpsFrameCount;
	const unsigned long fpsElapsedMs = fpsNowTick - FpsLastSampleTick;
	if (fpsElapsedMs >= 500)
	{
		FpsDisplayValue = static_cast<int>((static_cast<double>(FpsFrameCount) * 1000.0 / static_cast<double>(fpsElapsedMs)) + 0.5);
		FpsFrameCount = 0;
		FpsLastSampleTick = fpsNowTick;
	}
	const double perfFrameStartMs = RefreshPerfNowMs();
	PerformanceDiagnostics.BeginFrame();
	std::uint32_t frameRefreshReasonMask =
		PendingPerformanceRefreshReasonMask.exchange(0, std::memory_order_acq_rel);
	double perfAvisoMs = 0.0;
	double perfTargetsMs = 0.0;
	double perfRimcasMs = 0.0;
	double perfTagsMs = 0.0;
	double perfSrwMs = 0.0;
	double perfAvisoInsetMs = 0.0;
	double perfRdfMs = 0.0;
	double perfInsetChromeMs = 0.0;

	struct Utils {
		static RECT GetAreaFromText(CDC * dc, string text, POINT Pos) {
			RECT Area = { Pos.x, Pos.y, Pos.x + dc->GetTextExtent(text.c_str()).cx, Pos.y + dc->GetTextExtent(text.c_str()).cy };
			return Area;
		}
		static string getEnumString(TagTypes type) {
			if (type == TagTypes::Departure)
				return "departure";
			if (type == TagTypes::Arrival)
				return "arrival";
			if (type == TagTypes::Uncorrelated)
				return "uncorrelated";
			return "airborne";
		}
		static vector<string> getVectorFromCommaList(const string& list) {
			vector<string> result;
			size_t start = 0;
			size_t end = list.find(',');
			while (end != string::npos) {
				result.push_back(list.substr(start, end - start));
				start = end + 1;
				end = list.find(',', start);
			}
			result.push_back(list.substr(start));
			return result;
		}
	};

	setRefreshStage("sector geometry cache");
	// Sector element enumeration is stateful in the EuroScope SDK. Always bind
	// it to this radar screen before reading airport geometry or ARR/DEP runway
	// activity; another callback may have selected a different sector source.
	CPlugIn* sectorPlugin = GetPlugIn();
	struct ActiveSectorSelectionGuard
	{
		CPlugIn* plugin = nullptr;
		~ActiveSectorSelectionGuard()
		{
			if (plugin != nullptr)
				plugin->SelectActiveSectorfile();
		}
	} sectorSelectionGuard{ sectorPlugin };
	sectorPlugin->SelectScreenSectorfile(this);
	EnsureAirportPositionCache();
	EnsureRunwayGeometryCache();
	RefreshRunwayStatuses(false);

	// Draw map elements based on zoom level
	CPosition radarDownLeft;
	CPosition radarUpRight;
	GetDisplayArea(&radarDownLeft, &radarUpRight);
	if (std::isfinite(radarDownLeft.m_Longitude) &&
		std::isfinite(radarDownLeft.m_Latitude) &&
		std::isfinite(radarUpRight.m_Longitude) &&
		std::isfinite(radarUpRight.m_Latitude))
	{
		const double minLongitude = radarDownLeft.m_Longitude;
		const double minLatitude = radarDownLeft.m_Latitude;
		const double maxLongitude = radarUpRight.m_Longitude;
		const double maxLatitude = radarUpRight.m_Latitude;
		constexpr double viewChangeEpsilon = 1.0e-8;
		if (PerformanceLastMainViewValid &&
			(std::abs(minLongitude - PerformanceLastMainViewMinLongitude) > viewChangeEpsilon ||
			 std::abs(minLatitude - PerformanceLastMainViewMinLatitude) > viewChangeEpsilon ||
			 std::abs(maxLongitude - PerformanceLastMainViewMaxLongitude) > viewChangeEpsilon ||
			 std::abs(maxLatitude - PerformanceLastMainViewMaxLatitude) > viewChangeEpsilon))
		{
			frameRefreshReasonMask |= VsmrPerformance::RefreshReasonMask(
				VsmrPerformance::FrameRefreshReason::MainViewChanged);
		}
		PerformanceLastMainViewValid = true;
		PerformanceLastMainViewMinLongitude = minLongitude;
		PerformanceLastMainViewMinLatitude = minLatitude;
		PerformanceLastMainViewMaxLongitude = maxLongitude;
		PerformanceLastMainViewMaxLatitude = maxLatitude;
	}
	double radarCrossDistance = Haversine(radarDownLeft, radarUpRight);
	int NewRadarViewZoomLevel = getZoomLevelFromCrossDistance(radarCrossDistance);
	const std::string currentMapAirport = getActiveAirport();
	const bool useAvisoGroundRenderer = !ResolveAvisoGeoJsonPathForAirport(currentMapAirport).empty();
	const auto& currentRunwayStatuses = RimcasInstance->GetRunwayStatuses();
	const bool needsMapRefresh =
		(NewRadarViewZoomLevel != RadarViewZoomLevel) ||
		(currentMapAirport != LastMapActiveAirport) ||
		(currentRunwayStatuses != LastMapRunwayStatuses);

	if (needsMapRefresh) {
		setRefreshStage("map refresh");
		RadarViewZoomLevel = NewRadarViewZoomLevel;
		if (!useAvisoGroundRenderer)
		{
			// Draw items based on asr config & zoom level
			vector<CConfig::mapData> allItems = CurrentConfig->getMapElementsForZoomLevel(maxZoomLevel);
			vector<CConfig::mapData> itemsToDraw = CurrentConfig->getMapElementsForZoomLevel(RadarViewZoomLevel);
			map<string, bool> drawItemMap;

			auto tokenDataStart = [](const string& s, const string& token) -> size_t {
				// Find token like "DEP" or "ARR" and return the index right after the token and the following separator (eg. "DEP:")
				size_t pos = s.find(token);
				if (pos == string::npos) return string::npos;
				// token length +1 for separator (':') -> matches previous code's +4 for "DEP" (3) + ':'
				return pos + token.length() + 1;
				};

			for (const auto& item : allItems) {
				// Consider element present if any map entry has the same element name (compare element only).
				bool present = std::any_of(itemsToDraw.begin(), itemsToDraw.end(),
					[&](const CConfig::mapData& m) { return m.element == item.element; });

				bool shouldDraw = present;

				// If the item has an "active" definition we need to evaluate DEP/ARR conditions
				if (present && item.active.size() > 4) {
					if (item.active.substr(0, 4) != currentMapAirport) {
						shouldDraw = false;
					}

					// airport prefix (first 4 chars) must match active airport
				
					if (shouldDraw) {
						size_t depPos = tokenDataStart(item.active, "DEP");
						size_t arrPos = tokenDataStart(item.active, "ARR");
						// If DEP present, extract substring between DEP: and ARR: (or end) and check runways
						if (depPos != string::npos) {
							size_t depEnd = (arrPos != string::npos) ? arrPos - 5 : item.active.size();
							string depList = item.active.substr(depPos, depEnd - depPos);
							vector<string> depRunways = Utils::getVectorFromCommaList(depList);
							for (const auto& rwy : depRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::DEP && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}

						// If ARR present, extract substring after ARR: and check runways
						if (arrPos != string::npos && shouldDraw) {
							string arrList = item.active.substr(arrPos);
							vector<string> arrRunways = Utils::getVectorFromCommaList(arrList);
							for (const auto& rwy : arrRunways) {
								auto it = currentRunwayStatuses.find(rwy);
								if (it == currentRunwayStatuses.end() || (it->second != CRimcas::RunwayStatus::ARR && it->second != CRimcas::RunwayStatus::BOTH)) {
									shouldDraw = false;
									break;
								}
							}
						}
					}
				}

				// Always set an entry for this element (avoids missing keys and an empty draw map).
				drawItemMap[item.element] = shouldDraw;
			}

			// Now apply the map
			setRefreshStage("sector element visibility");
			for (const auto& [elementName, toDraw] : drawItemMap) {
				size_t slashPos = elementName.find("/");
				if (slashPos == string::npos) continue;
				string category = elementName.substr(0, slashPos);
				string name = elementName.substr(slashPos + 1);

				int elementCategory = getIntFromCategory(category);
				if (elementCategory == -1) continue;
				CSectorElement element = GetPlugIn()->SectorFileElementSelectFirst(elementCategory);
				while (element.IsValid()) {
					const char* elementNameRaw = element.GetName();
					if (elementNameRaw == nullptr || elementNameRaw[0] == '\0')
					{
						element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
						continue;
					}

					if (strncmp(name.c_str(), elementNameRaw, strlen(name.c_str())) == 0) {
						const char* componentName = element.GetComponentName(0);
						if (componentName != nullptr)
							ShowSectorFileElement(element, componentName, toDraw);
					}
					element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
				}
			}

			setRefreshStage("RefreshMapContent");
			RefreshMapContent();
		}
		LastMapRunwayStatuses = currentRunwayStatuses;
		LastMapActiveAirport = currentMapAirport;
	}


	POINT p;
	if (GetCursorPos(&p)) {
		HWND activeWindow = GetActiveWindow();
		if (activeWindow != nullptr && ScreenToClient(activeWindow, &p)) {
			mouseLocation = p;
		}
	}

	VSMR_REFRESH_LOG("Graphics set up");
	setRefreshStage("graphics setup");
	CDC dc;
	dc.Attach(hDC);
	ScopedCdcDetach dcDetach(dc);

	// Creating the gdi+ graphics
	Graphics graphics(hDC);
	graphics.SetPageUnit(Gdiplus::UnitPixel);

	graphics.SetSmoothingMode(SmoothingModeAntiAlias);

	RECT RadarArea = GetRadarArea();
	RECT ChatArea = GetChatArea();
	CRect normalizedChatArea(ChatArea);
	normalizedChatArea.NormalizeRect();
	if (!normalizedChatArea.IsRectEmpty())
		RadarArea.bottom = normalizedChatArea.top;
	CRect insetLayoutBounds(RadarArea);
	insetLayoutBounds.NormalizeRect();
	if (InitialInsetStateRestorePending && !insetLayoutBounds.IsRectEmpty())
	{
		const unsigned long restoreNowTick = ::GetTickCount();
		const bool sameBounds =
			InitialInsetStateRestoreBounds.left == insetLayoutBounds.left &&
			InitialInsetStateRestoreBounds.top == insetLayoutBounds.top &&
			InitialInsetStateRestoreBounds.right == insetLayoutBounds.right &&
			InitialInsetStateRestoreBounds.bottom == insetLayoutBounds.bottom;
		if (sameBounds)
		{
			++InitialInsetStateRestoreStableFrames;
		}
		else
		{
			InitialInsetStateRestoreBounds = insetLayoutBounds;
			InitialInsetStateRestoreStableFrames = 1;
			InitialInsetStateRestoreBoundsChangedTick = restoreNowTick;
		}

		if (InitialInsetStateRestoreStableFrames >= 2 &&
			restoreNowTick - InitialInsetStateRestoreBoundsChangedTick >= 250)
		{
			ResetAllInsetWindowStates(false);
			ResetAvisoPresetStateForActiveAirport(false);
			if (!LoadInsetStateFromAsrForAirport(getActiveAirport(), true))
				ApplyDefaultAvisoPresetIfConfigured();
			else if (!ActiveAvisoPresetName.empty())
			{
				const std::string activePresetName = ActiveAvisoPresetName;
				LoadAvisoPreset(activePresetName);
			}
			InitialInsetStateRestorePending = false;
			InitialInsetStateRestoreStableFrames = 0;
			InitialInsetStateRestoreBoundsChangedTick = 0;
		}
	}
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr)
			appWindowIt->second->ApplyAvisoLayoutBounds(&insetLayoutBounds);
	}

	auto disableAvisoGeoJsonRender = [&](const std::string& reason)
	{
		std::string disabledPath = AvisoGeoJsonLoadedPath;
		if (disabledPath.empty())
			disabledPath = ResolveAvisoGeoJsonPathForAirport(getActiveAirport());

		AvisoGeoJsonRenderDisabled = true;
		AvisoGeoJsonRenderDisabledPath = disabledPath;
		ClearAvisoGeoJsonRasterCache();
		AvisoGeoJsonLastViewValid = false;
		Logger::info("AVISO GeoJSON render disabled path=" + disabledPath + " reason=" + reason);
	};

	// Capture all EuroScope-derived frame data once, finalize RIMCAS, and then
	// publish an immutable scene before any viewport renders traffic.  Main,
	// AVISO, and SRW projections consume this same snapshot later in the frame.
	bool frameRimcasEnabled = true;
	bool frameUseRedEmergencySymbols = true;
	int rimcasStageTwoSpeedThreshold = 25;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() && profile.HasMember("rimcas") && profile["rimcas"].IsObject())
		{
			const Value& rimcas = profile["rimcas"];
			if (rimcas.HasMember("enabled") && rimcas["enabled"].IsBool())
				frameRimcasEnabled = rimcas["enabled"].GetBool();
			if (rimcas.HasMember("use_red_symbol_for_emergencies") && rimcas["use_red_symbol_for_emergencies"].IsBool())
				frameUseRedEmergencySymbols = rimcas["use_red_symbol_for_emergencies"].GetBool();
			if (rimcas.HasMember("stage_two_speed_threshold_kt") && rimcas["stage_two_speed_threshold_kt"].IsInt())
				rimcasStageTwoSpeedThreshold = rimcas["stage_two_speed_threshold_kt"].GetInt();
			else if (rimcas.HasMember("rimcas_stage_two_speed_threshold") && rimcas["rimcas_stage_two_speed_threshold"].IsInt())
				rimcasStageTwoSpeedThreshold = rimcas["rimcas_stage_two_speed_threshold"].GetInt();
		}
	}
	rimcasStageTwoSpeedThreshold = std::clamp(rimcasStageTwoSpeedThreshold, 0, 250);
	RimcasEnabled = frameRimcasEnabled;
	RimcasUseRedEmergencySymbols = frameUseRedEmergencySymbols;
	setRefreshStage("shared radar scene build");
	double sceneRimcasMilliseconds = 0.0;
	const std::shared_ptr<const VsmrScene::RadarScene> frameSceneOwner = BuildRadarScene(
		frameRimcasEnabled,
		frameUseRedEmergencySymbols,
		isLVP,
		rimcasStageTwoSpeedThreshold,
		&sceneRimcasMilliseconds);
	const VsmrScene::RadarScene* frameScene = frameSceneOwner.get();
	perfRimcasMs += sceneRimcasMilliseconds;

	const double perfAvisoStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("AVISO render");
		RenderAvisoGeoJson(hDC, graphics);
	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		disableAvisoGeoJsonRender("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		disableAvisoGeoJsonRender("MFC exception");
	}
	catch (const std::exception& ex)
	{
		disableAvisoGeoJsonRender(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		disableAvisoGeoJsonRender("unknown exception");
	}
	perfAvisoMs += RefreshPerfNowMs() - perfAvisoStartMs;

	VSMR_REFRESH_LOG("Runway overlay loop");
	setRefreshStage("runway overlay draw");
	for (const auto& runway : CachedRunwayGeometries)
	{
		if (drawRunways && runway.rimcasDefinition.size() >= 3)
		{
			std::vector<PointF> points;
			points.reserve(runway.rimcasDefinition.size());
			for (const auto& point : runway.rimcasDefinition)
			{
				POINT toDraw = ConvertCoordFromPositionToPixel(point);
				points.push_back({ REAL(toDraw.x), REAL(toDraw.y) });
			}

			Pen runwayPen(Color(static_cast<Gdiplus::ARGB>(Color::White)));
			graphics.DrawPolygon(&runwayPen, points.data(), static_cast<INT>(points.size()));
		}

		if (!frameRimcasEnabled)
			continue;

		const auto closedRunwayIt = RimcasInstance->ClosedRunway.find(runway.displayName);
		if (closedRunwayIt == RimcasInstance->ClosedRunway.end() || !closedRunwayIt->second)
			continue;

		const std::vector<CPosition>& closedDefinition =
			runway.closedDefinition.empty() ? runway.rimcasDefinition : runway.closedDefinition;
		if (closedDefinition.size() < 3)
			continue;

		std::vector<PointF> points;
		points.reserve(closedDefinition.size());
		for (const auto& point : closedDefinition)
		{
			POINT toDraw = ConvertCoordFromPositionToPixel(point);
			points.push_back({ REAL(toDraw.x), REAL(toDraw.y) });
		}

		SolidBrush closedRunwayBrush(Color(150, 0, 0));
		graphics.FillPolygon(&closedRunwayBrush, points.data(), static_cast<INT>(points.size()));
	}

#pragma region symbols
	// Drawing the symbols
	VSMR_REFRESH_LOG("Symbols loop");
	setRefreshStage("target symbol rendering");

	// Cache current view scaling once per frame for configured icon sizing.
	double framePixPerMeter = 0.0;
	{
		RECT radarArea = GetRadarArea();
		RECT chatArea = GetChatArea();
		radarArea.bottom = chatArea.top;
		double pxW = (radarArea.right - radarArea.left > 0) ? double(radarArea.right - radarArea.left) : 1.0;
		double pxH = (radarArea.bottom - radarArea.top > 0) ? double(radarArea.bottom - radarArea.top) : 1.0;

		CPosition dispSW, dispNE;
		GetDisplayArea(&dispSW, &dispNE);
		double centerLat = (dispSW.m_Latitude + dispNE.m_Latitude) / 2.0;
		double centerLon = (dispSW.m_Longitude + dispNE.m_Longitude) / 2.0;

		CPosition leftMid; leftMid.m_Latitude = centerLat; leftMid.m_Longitude = dispSW.m_Longitude;
		CPosition rightMid; rightMid.m_Latitude = centerLat; rightMid.m_Longitude = dispNE.m_Longitude;
		CPosition bottomMid; bottomMid.m_Latitude = dispSW.m_Latitude; bottomMid.m_Longitude = centerLon;
		CPosition topMid; topMid.m_Latitude = dispNE.m_Latitude; topMid.m_Longitude = centerLon;

		double widthMeters = Haversine(leftMid, rightMid);
		double heightMeters = Haversine(bottomMid, topMid);

		double pixPerMeterX = (widthMeters > 1.0) ? (pxW / widthMeters) : 0.0;
		double pixPerMeterY = (heightMeters > 1.0) ? (pxH / heightMeters) : 0.0;

		if (pixPerMeterX > 0.0 && pixPerMeterY > 0.0)
			framePixPerMeter = (pixPerMeterX < pixPerMeterY) ? pixPerMeterX : pixPerMeterY;
		else if (pixPerMeterX > 0.0)
			framePixPerMeter = pixPerMeterX;
		else
			framePixPerMeter = pixPerMeterY;
	}

	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& frameTargetPresentation = frameScene != nullptr
		? frameScene->targetPresentation
		: defaultTargetPresentation;
	const bool frameUseNovaIconStyle = frameTargetPresentation.icon == VsmrScene::IconStyle::Nova;
	const bool frameUseDiamondIconStyle = frameTargetPresentation.icon == VsmrScene::IconStyle::Diamond;
	const bool frameUseRealisticIconStyle = frameTargetPresentation.icon == VsmrScene::IconStyle::Realistic;
	const bool frameUseFastRealisticBitmapRendering = frameUseRealisticIconStyle;
	const unsigned long long frameRealisticIconCacheFrame = frameUseRealisticIconStyle ? ++RealisticIconCacheFrame : RealisticIconCacheFrame;
	const bool frameSmallIconBoostEnabled = frameTargetPresentation.smallIconBoostEnabled;
	const bool frameFixedPixelIconSize = frameTargetPresentation.fixedPixelSize;
	const double frameSmallIconBoostFactor = frameTargetPresentation.smallIconBoostFactor;
	const double frameSmallIconBoostResolutionScale = frameTargetPresentation.resolutionScale;
	const double frameFixedTriangleScale = frameTargetPresentation.fixedTriangleScale;
	const Color frameSymbolWhiteColor(static_cast<Gdiplus::ARGB>(Gdiplus::Color::White));
	auto sanitizeFinitePositive = [](double value, double fallback, double minValue, double maxValue) -> double
	{
		if (!std::isfinite(value))
			return fallback;
		if (value < minValue)
			return minValue;
		if (value > maxValue)
			return maxValue;
		return value;
	};
	std::vector<PointF> framePatatoidePolygonPoints;
	auto drawPatatoidePolygon = [&](const std::vector<VsmrScene::GeoPoint>& sourcePoints, const Color& fillColor)
	{
		if (sourcePoints.empty())
			return;

		framePatatoidePolygonPoints.clear();
		framePatatoidePolygonPoints.reserve(sourcePoints.size());
		for (const VsmrScene::GeoPoint& sourcePoint : sourcePoints)
		{
			if (!sourcePoint.valid)
				continue;
			CPosition pos;
			pos.m_Latitude = sourcePoint.latitude;
			pos.m_Longitude = sourcePoint.longitude;
			POINT point = ConvertCoordFromPositionToPixel(pos);
			framePatatoidePolygonPoints.emplace_back(static_cast<REAL>(point.x), static_cast<REAL>(point.y));
		}

		if (framePatatoidePolygonPoints.size() < 3)
			return;

		SolidBrush polygonBrush(fillColor);
		graphics.FillPolygon(&polygonBrush, framePatatoidePolygonPoints.data(), static_cast<INT>(framePatatoidePolygonPoints.size()));
	};
	std::unordered_map<unsigned int, std::unique_ptr<Gdiplus::ImageAttributes>> frameTintAttributesCache;
	auto getCachedTintAttributes = [&](const Color& tintColor) -> Gdiplus::ImageAttributes*
	{
		const unsigned int tintKey =
			(static_cast<unsigned int>(tintColor.GetAlpha()) << 24) |
			(static_cast<unsigned int>(tintColor.GetR()) << 16) |
			(static_cast<unsigned int>(tintColor.GetG()) << 8) |
			static_cast<unsigned int>(tintColor.GetB());
		auto itTint = frameTintAttributesCache.find(tintKey);
		if (itTint != frameTintAttributesCache.end())
			return itTint->second.get();

		auto attrs = std::make_unique<Gdiplus::ImageAttributes>();
		const Gdiplus::REAL tintAlpha = static_cast<Gdiplus::REAL>(tintColor.GetAlpha()) / 255.0f;
		Gdiplus::ColorMatrix cm = {
			{
				{ static_cast<Gdiplus::REAL>(tintColor.GetR()) / 255.0f, 0.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, static_cast<Gdiplus::REAL>(tintColor.GetG()) / 255.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, static_cast<Gdiplus::REAL>(tintColor.GetB()) / 255.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 0.0f, tintAlpha, 0.0f },
				{ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }
			}
		};
		attrs->SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
		auto inserted = frameTintAttributesCache.emplace(tintKey, std::move(attrs));
		return inserted.first->second.get();
	};
	const Gdiplus::InterpolationMode frameSavedInterpolationMode = graphics.GetInterpolationMode();
	const Gdiplus::PixelOffsetMode frameSavedPixelOffsetMode = graphics.GetPixelOffsetMode();
	const Gdiplus::CompositingQuality frameSavedCompositingQuality = graphics.GetCompositingQuality();
	if (frameUseFastRealisticBitmapRendering)
	{
		graphics.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
		graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighSpeed);
		graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
	}
	setRefreshStage("radar target loop");
	const double perfRimcasBeforeTargetsMs = perfRimcasMs;
	const double perfTargetsStartMs = RefreshPerfNowMs();
	std::size_t frameVisibleTargetCount = 0;
	CRect frameVisibleRadarArea(RadarArea);
	frameVisibleRadarArea.NormalizeRect();
	static const std::vector<VsmrScene::Target> emptySceneTargets;
	const std::vector<VsmrScene::Target>& sceneTargets = frameScene != nullptr
		? frameScene->targets
		: emptySceneTargets;
	auto scenePosition = [](const VsmrScene::GeoPoint& source) -> CPosition
	{
		CPosition position;
		position.m_Latitude = source.latitude;
		position.m_Longitude = source.longitude;
		return position;
	};
	for (const VsmrScene::Target& sceneTarget : sceneTargets)
	{
		if (!sceneTarget.iconVisible || !sceneTarget.position.valid)
			continue;
		const std::string& rtCallsign = sceneTarget.callsign;
		auto iconVerboseStep = [&](const std::string& step)
		{
			if (!Logger::is_verbose_mode())
				return;
			Logger::info("IconRender: " + rtCallsign + " " + step);
		};

		const CPosition targetPosition = scenePosition(sceneTarget.position);
		iconVerboseStep("begin");

		const bool AcisCorrelated = sceneTarget.correlated;
		POINT acPosPix = ConvertCoordFromPositionToPixel(targetPosition);
		if (acPosPix.x >= frameVisibleRadarArea.left && acPosPix.x < frameVisibleRadarArea.right &&
			acPosPix.y >= frameVisibleRadarArea.top && acPosPix.y < frameVisibleRadarArea.bottom)
		{
			++frameVisibleTargetCount;
		}

		const bool drawLegacyPrimarySymbol = frameUseNovaIconStyle;
		if (drawLegacyPrimarySymbol && sceneTarget.style.showPrimaryReturn && !sceneTarget.primaryReturnPolygon.empty()) {
			const Color primaryTargetColor(
				sceneTarget.style.primaryReturnColor.alpha,
				sceneTarget.style.primaryReturnColor.red,
				sceneTarget.style.primaryReturnColor.green,
				sceneTarget.style.primaryReturnColor.blue);
			drawPatatoidePolygon(
				sceneTarget.primaryReturnPolygon,
				primaryTargetColor);
		}
		acPosPix = ConvertCoordFromPositionToPixel(targetPosition);

		// Prefer the aircraft-reported heading to keep icon orientation aligned with the nose (even when moving backwards)
		const double headingDeg = sceneTarget.headingTrueDegrees;

		// Icon sizing based on real dimensions and zoom
		int iconSize = 40;
		const bool useNovaIconStyle = frameUseNovaIconStyle;
		const bool useDiamondIconStyle = frameUseDiamondIconStyle;
		const bool useRealisticIconStyle = frameUseRealisticIconStyle;
		const std::string& iconType = sceneTarget.style.assetKey;
		Gdiplus::Bitmap* iconBmp = nullptr;
		if (useRealisticIconStyle)
			iconBmp = GetAircraftIcon(iconType);

		iconVerboseStep("after_scene_data");

		const bool smallIconBoostEnabled = frameSmallIconBoostEnabled;
		const bool fixedPixelIconSize = frameFixedPixelIconSize;
		UINT iconBmpWidth = 0;
		UINT iconBmpHeight = 0;
		bool canUseRealisticIcon = useRealisticIconStyle && iconBmp != nullptr;
		if (canUseRealisticIcon)
		{
			const Gdiplus::Status bmpStatus = iconBmp->GetLastStatus();
			iconBmpWidth = iconBmp->GetWidth();
			iconBmpHeight = iconBmp->GetHeight();
			if (bmpStatus != Gdiplus::Ok || iconBmpWidth == 0 || iconBmpHeight == 0)
			{
				iconVerboseStep(
					"realistic_icon_disabled status=" + std::to_string(static_cast<int>(bmpStatus)) +
					" w=" + std::to_string(static_cast<unsigned long long>(iconBmpWidth)) +
					" h=" + std::to_string(static_cast<unsigned long long>(iconBmpHeight)));
				canUseRealisticIcon = false;
			}
		}
		if (Logger::is_verbose_mode())
		{
			std::string iconDrawMode = useNovaIconStyle ? "nova" : (canUseRealisticIcon ? "realistic" : "symbol");
			Logger::info("IconRender: " + rtCallsign + " mode=" + iconDrawMode + " icon_type=" + iconType);
		}
		Color targetTintColor(
			sceneTarget.style.color.alpha,
			sceneTarget.style.color.red,
			sceneTarget.style.color.green,
			sceneTarget.style.color.blue);
		const bool applyTargetTintColor = true;

		if (useNovaIconStyle)
		{
			const Color novaSymbolColor = applyTargetTintColor ? targetTintColor : frameSymbolWhiteColor;
			Pen novaSymbolPen(novaSymbolColor, 1.0f);
			if (sceneTarget.transponderModeC) {
				PointF novaPoints[] = {
					PointF(static_cast<REAL>(acPosPix.x), static_cast<REAL>(acPosPix.y - 6)),
					PointF(static_cast<REAL>(acPosPix.x - 6), static_cast<REAL>(acPosPix.y)),
					PointF(static_cast<REAL>(acPosPix.x), static_cast<REAL>(acPosPix.y + 6)),
					PointF(static_cast<REAL>(acPosPix.x + 6), static_cast<REAL>(acPosPix.y)),
					PointF(static_cast<REAL>(acPosPix.x), static_cast<REAL>(acPosPix.y - 6))
				};
				graphics.DrawLines(&novaSymbolPen, novaPoints, static_cast<INT>(_countof(novaPoints)));
			}
			else {
				graphics.DrawLine(&novaSymbolPen, acPosPix.x, acPosPix.y, acPosPix.x - 4, acPosPix.y - 4);
				graphics.DrawLine(&novaSymbolPen, acPosPix.x, acPosPix.y, acPosPix.x + 4, acPosPix.y - 4);
				graphics.DrawLine(&novaSymbolPen, acPosPix.x, acPosPix.y, acPosPix.x - 4, acPosPix.y + 4);
				graphics.DrawLine(&novaSymbolPen, acPosPix.x, acPosPix.y, acPosPix.x + 4, acPosPix.y + 4);
			}
			iconSize = 12;
		}
		else if (canUseRealisticIcon) {

			// Compute on-screen size that scales with zoom (uniform for all aircraft)
			double drawW = iconSize;
			double drawH = iconSize;
			const double pixPerMeter = std::isfinite(framePixPerMeter) ? framePixPerMeter : 0.0;

			const double lengthMeters = sceneTarget.style.lengthMeters;
			const double spanMeters = sceneTarget.style.wingspanMeters;
			iconVerboseStep(
				"realistic_dims len=" + std::to_string(lengthMeters) +
				" span=" + std::to_string(spanMeters));

			if (fixedPixelIconSize)
			{
				const double configuredFactor = smallIconBoostEnabled ? frameSmallIconBoostFactor : 1.0;
				const double resolutionScale = frameSmallIconBoostResolutionScale;
				const double referenceAircraftMeters = 40.0; // medium-jet baseline
				const double referencePixels = 18.0 * resolutionScale;
				const double pxPerMeterFixed = referencePixels / referenceAircraftMeters;
				drawW = spanMeters * pxPerMeterFixed * configuredFactor;
				drawH = lengthMeters * pxPerMeterFixed * configuredFactor;
			}
			else
			{
				if (pixPerMeter > 0.0) {
					drawW = spanMeters * pixPerMeter;
					drawH = lengthMeters * pixPerMeter;
				}

				// Optional readability boost (realistic icons only):
				// apply one zoom-based factor for all aircraft so relative real-size differences stay intact.
				if (smallIconBoostEnabled && pixPerMeter > 0.0)
				{
					const double configuredFactor = frameSmallIconBoostFactor;
					const double resolutionScale = frameSmallIconBoostResolutionScale;
					const double referenceAircraftMeters = 40.0; // medium-jet baseline for zoom trigger
					const double referenceScreenSize = referenceAircraftMeters * pixPerMeter;
					const double boostStartSize = 14.0 * resolutionScale;
					const double boostedReferenceSize = 18.0 * configuredFactor * resolutionScale;
					if (referenceScreenSize < boostStartSize)
					{
						const double safeRefSize = max(0.01, referenceScreenSize);
						const double zoomBoostScale = std::clamp(boostedReferenceSize / safeRefSize, 1.0, 6.0 * configuredFactor * resolutionScale);
						drawW *= zoomBoostScale;
						drawH *= zoomBoostScale;
					}
				}
			}

			// Clamp sizes to keep visible but not giant
			double minSize = 4.0;
			double maxSize = 1200.0;
			drawW = sanitizeFinitePositive(drawW, 24.0, minSize, maxSize);
			drawH = sanitizeFinitePositive(drawH, 24.0, minSize, maxSize);
			iconVerboseStep(
				"realistic_size w=" + std::to_string(drawW) +
				" h=" + std::to_string(drawH));
			int drawPixelW = 0;
			int drawPixelH = 0;
			std::string scaledRealisticIconCacheKey;
			Gdiplus::Bitmap* cachedRealisticIcon = GetCachedRealisticIconBitmap(
				iconType,
				iconBmp,
				iconBmpWidth,
				iconBmpHeight,
				applyTargetTintColor,
				targetTintColor,
				drawW,
				drawH,
				frameRealisticIconCacheFrame,
				drawPixelW,
				drawPixelH,
				scaledRealisticIconCacheKey);
			if (cachedRealisticIcon == nullptr)
			{
				drawPixelW = std::clamp(static_cast<int>(std::lround(drawW)), 1, 2048);
				drawPixelH = std::clamp(static_cast<int>(std::lround(drawH)), 1, 2048);
			}

			// Screen-relative heading from pixel forward vector (handles rotated display)
			CPosition nosePosDraw = scenePosition(sceneTarget.headingProbe);
			POINT nosePixDraw = ConvertCoordFromPositionToPixel(nosePosDraw);
			double fx = double(nosePixDraw.x - acPosPix.x);
			double fy = double(nosePixDraw.y - acPosPix.y);
			double screenHeadingDeg = atan2(fy, fx) * 180.0 / M_PI;
			// Adjust because SVG nose is up; rotate so north = 0, east = 90, etc.
			// GDI+ uses screen coords (Y grows down); negate to align with screen vector and SVG nose-up.
			double rotationDeg = screenHeadingDeg + 90.0;
			if (!std::isfinite(rotationDeg))
				rotationDeg = 0.0;
			iconVerboseStep("realistic_before_transform rot=" + std::to_string(rotationDeg));

			RealisticIconCacheEntry* rotatedRealisticIcon = GetCachedRotatedRealisticIconBitmap(
				scaledRealisticIconCacheKey,
				cachedRealisticIcon,
				drawPixelW,
				drawPixelH,
				rotationDeg,
				frameRealisticIconCacheFrame);
			if (rotatedRealisticIcon != nullptr && rotatedRealisticIcon->bitmap != nullptr)
			{
				iconVerboseStep("before_realistic_draw_rotated_cached");
				graphics.DrawImage(
					rotatedRealisticIcon->bitmap.get(),
					acPosPix.x - rotatedRealisticIcon->centerX,
					acPosPix.y - rotatedRealisticIcon->centerY);
				iconVerboseStep("after_realistic_draw_rotated_cached");
				iconSize = max(
					static_cast<int>(rotatedRealisticIcon->bitmap->GetWidth()),
					static_cast<int>(rotatedRealisticIcon->bitmap->GetHeight()));
			}
			else
			{
				GraphicsState state = graphics.Save();
				Gdiplus::Matrix m;
				m.Translate(Gdiplus::REAL(acPosPix.x), Gdiplus::REAL(acPosPix.y));
				m.Rotate(Gdiplus::REAL(rotationDeg));
				m.Translate(Gdiplus::REAL(-drawPixelW / 2.0), Gdiplus::REAL(-drawPixelH / 2.0));
				graphics.SetTransform(&m);

				if (cachedRealisticIcon != nullptr)
				{
					iconVerboseStep("before_realistic_draw_scaled_cached_fallback");
					graphics.DrawImage(cachedRealisticIcon, 0, 0);
					iconVerboseStep("after_realistic_draw_scaled_cached_fallback");
				}
				else if (applyTargetTintColor) {
					iconVerboseStep("before_realistic_draw_tinted_fallback");
					Gdiplus::ImageAttributes* attrs = getCachedTintAttributes(targetTintColor);
					RectF dest(0.0f, 0.0f, static_cast<REAL>(drawPixelW), static_cast<REAL>(drawPixelH));
					graphics.DrawImage(
						iconBmp,
						dest,
						0.0f,
						0.0f,
						static_cast<Gdiplus::REAL>(iconBmpWidth),
						static_cast<Gdiplus::REAL>(iconBmpHeight),
						UnitPixel,
						attrs);
					iconVerboseStep("after_realistic_draw_tinted_fallback");
				}
				else {
					iconVerboseStep("before_realistic_draw_plain_fallback");
					graphics.DrawImage(iconBmp, Gdiplus::REAL(0), Gdiplus::REAL(0), Gdiplus::REAL(drawPixelW), Gdiplus::REAL(drawPixelH));
					iconVerboseStep("after_realistic_draw_plain_fallback");
				}
				graphics.Restore(state);

				const double rotationRadians = rotationDeg * M_PI / 180.0;
				const double absCos = std::abs(std::cos(rotationRadians));
				const double absSin = std::abs(std::sin(rotationRadians));
				const int rotatedWidth = static_cast<int>(std::ceil(drawPixelW * absCos + drawPixelH * absSin));
				const int rotatedHeight = static_cast<int>(std::ceil(drawPixelW * absSin + drawPixelH * absCos));
				iconSize = max(rotatedWidth, rotatedHeight);
			}
		}
		else
		{
			const double pixPerMeter = std::isfinite(framePixPerMeter) ? framePixPerMeter : 0.0;

			const double lenMetersBase = 20.0;
			const double halfWidthMetersBase = 12.0;
			const double symbolSizeScale = frameFixedTriangleScale;
			double lenPx = 20.0;
			double halfWidthPx = 12.0;
			double lenMetersUsed = lenMetersBase;
			double halfWidthMetersUsed = halfWidthMetersBase;

			if (fixedPixelIconSize)
			{
				const double configuredFactor = smallIconBoostEnabled ? frameSmallIconBoostFactor : 1.0;
				const double resolutionScale = frameSmallIconBoostResolutionScale;
				const double fixedScale = configuredFactor * resolutionScale;
				lenPx = std::clamp(lenPx * fixedScale, 6.0, 160.0);
				halfWidthPx = std::clamp(halfWidthPx * fixedScale, 3.0, 80.0);
				if (pixPerMeter > 0.0)
				{
					lenMetersUsed = lenPx / pixPerMeter;
					halfWidthMetersUsed = halfWidthPx / pixPerMeter;
				}
			}
			else
			{
				if (pixPerMeter > 0.0) {
					lenPx = std::clamp(pixPerMeter * lenMetersBase, 6.0, 120.0);
					halfWidthPx = std::clamp(pixPerMeter * halfWidthMetersBase, 3.0, 60.0);
					lenMetersUsed = lenPx / pixPerMeter;
					halfWidthMetersUsed = halfWidthPx / pixPerMeter;
				}

				// Optional readability boost for tiny triangle symbols when zoomed out.
				if (smallIconBoostEnabled)
				{
					const double configuredFactor = frameSmallIconBoostFactor;
					const double resolutionScale = frameSmallIconBoostResolutionScale;
					const double currentExtent = lenPx + halfWidthPx;
					if (currentExtent > 0.0)
					{
						const double targetMinExtent = 14.0 * configuredFactor * resolutionScale;
						const double boostScale = std::clamp(targetMinExtent / currentExtent, 1.0, 2.0 * configuredFactor * resolutionScale);
						lenPx *= boostScale;
						halfWidthPx *= boostScale;
						if (pixPerMeter > 0.0)
						{
							lenMetersUsed = lenPx / pixPerMeter;
							halfWidthMetersUsed = halfWidthPx / pixPerMeter;
						}
					}
				}
			}

			// Fixed Size scale always applies to arrow/diamond symbols, regardless of fixed-pixel mode.
			lenPx = sanitizeFinitePositive(lenPx * symbolSizeScale, 20.0, 1.0, 220.0);
			halfWidthPx = sanitizeFinitePositive(halfWidthPx * symbolSizeScale, 12.0, 1.0, 110.0);
			if (pixPerMeter > 0.0)
			{
				lenMetersUsed = lenPx / pixPerMeter;
				halfWidthMetersUsed = halfWidthPx / pixPerMeter;
			}

			auto wrap360 = [](double deg) {
				double wrapped = fmod(deg, 360.0);
				return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
			};

			const Color drawColor = applyTargetTintColor ? targetTintColor : frameSymbolWhiteColor;
			if (useDiamondIconStyle)
			{
				iconVerboseStep("before_symbol_diamond_draw");
				// Rounded square rendered as a 45-degree rotated diamond.
				const double diagonalPx = std::clamp(lenPx + halfWidthPx, 10.0, 220.0);
				const double sidePx = diagonalPx / std::sqrt(2.0);
				const double halfSide = sidePx / 2.0;
				const Gdiplus::REAL rectX = static_cast<Gdiplus::REAL>(acPosPix.x - halfSide);
				const Gdiplus::REAL rectY = static_cast<Gdiplus::REAL>(acPosPix.y - halfSide);
				const Gdiplus::REAL rectW = static_cast<Gdiplus::REAL>(sidePx);
				const Gdiplus::REAL rectH = static_cast<Gdiplus::REAL>(sidePx);
				Gdiplus::REAL radius = std::clamp(static_cast<Gdiplus::REAL>(sidePx * 0.22), 2.0f, static_cast<Gdiplus::REAL>(sidePx / 2.0));

				Gdiplus::GraphicsPath diamondPath;
				const Gdiplus::REAL d = radius * 2.0f;
				diamondPath.AddArc(rectX, rectY, d, d, 180, 90);
				diamondPath.AddArc(rectX + rectW - d, rectY, d, d, 270, 90);
				diamondPath.AddArc(rectX + rectW - d, rectY + rectH - d, d, d, 0, 90);
				diamondPath.AddArc(rectX, rectY + rectH - d, d, d, 90, 90);
				diamondPath.CloseFigure();

				CPosition nosePosDraw = scenePosition(sceneTarget.headingProbe);
				POINT nosePixDraw = ConvertCoordFromPositionToPixel(nosePosDraw);
				double fx = double(nosePixDraw.x - acPosPix.x);
				double fy = double(nosePixDraw.y - acPosPix.y);
				double screenHeadingDeg = atan2(fy, fx) * 180.0 / M_PI;
				double rotationDeg = screenHeadingDeg + 45.0;

				GraphicsState diamondState = graphics.Save();
				Gdiplus::Matrix diamondTransform;
				diamondTransform.RotateAt(static_cast<Gdiplus::REAL>(rotationDeg), PointF(static_cast<Gdiplus::REAL>(acPosPix.x), static_cast<Gdiplus::REAL>(acPosPix.y)));
				graphics.MultiplyTransform(&diamondTransform);
				SolidBrush diamondBrush(drawColor);
				graphics.FillPath(&diamondBrush, &diamondPath);
				graphics.Restore(diamondState);
				iconVerboseStep("after_symbol_diamond_draw");
				iconSize = int(max(12.0, diagonalPx));
			}
			else
			{
				iconVerboseStep("before_symbol_arrow_draw");
				auto move = [&](const CPosition& start, double bearingDeg, double distanceMeters) {
					return BetterHarversine(start, wrap360(bearingDeg), distanceMeters);
				};

				CPosition acPos = targetPosition;
				CPosition tipPos = move(acPos, headingDeg, lenMetersUsed);
				CPosition basePos = move(acPos, headingDeg + 180.0, lenMetersUsed * 0.33);
				CPosition notchPos = move(acPos, headingDeg + 180.0, lenMetersUsed * 0.05);
				CPosition rightPos = move(basePos, headingDeg + 90.0, halfWidthMetersUsed);
				CPosition leftPos = move(basePos, headingDeg - 90.0, halfWidthMetersUsed);

				POINT tip = ConvertCoordFromPositionToPixel(tipPos);
				POINT right = ConvertCoordFromPositionToPixel(rightPos);
				POINT notch = ConvertCoordFromPositionToPixel(notchPos);
				POINT left = ConvertCoordFromPositionToPixel(leftPos);

				PointF tri[4] = {
					PointF(Gdiplus::REAL(tip.x), Gdiplus::REAL(tip.y)),
					PointF(Gdiplus::REAL(right.x), Gdiplus::REAL(right.y)),
					PointF(Gdiplus::REAL(notch.x), Gdiplus::REAL(notch.y)),
					PointF(Gdiplus::REAL(left.x), Gdiplus::REAL(left.y))
				};

				SolidBrush arrowBrush(drawColor);
				graphics.FillPolygon(&arrowBrush, tri, 4);
				iconVerboseStep("after_symbol_arrow_draw");
				iconSize = int(max(12.0, lenPx + halfWidthPx));
			}
		}
		iconVerboseStep("after_icon_draw");

		if (mouseWithin({ acPosPix.x - 5, acPosPix.y - 5, acPosPix.x + 5, acPosPix.y + 5 })) {
			dc.MoveTo(acPosPix.x, acPosPix.y - 8);
			dc.LineTo(acPosPix.x - 6, acPosPix.y - 12);
			dc.MoveTo(acPosPix.x, acPosPix.y - 8);
			dc.LineTo(acPosPix.x + 6, acPosPix.y - 12);

			dc.MoveTo(acPosPix.x, acPosPix.y + 8);
			dc.LineTo(acPosPix.x - 6, acPosPix.y + 12);
			dc.MoveTo(acPosPix.x, acPosPix.y + 8);
			dc.LineTo(acPosPix.x + 6, acPosPix.y + 12);

			dc.MoveTo(acPosPix.x - 8, acPosPix.y );
			dc.LineTo(acPosPix.x - 12, acPosPix.y -6);
			dc.MoveTo(acPosPix.x - 8, acPosPix.y);
			dc.LineTo(acPosPix.x - 12 , acPosPix.y + 6);

			dc.MoveTo(acPosPix.x + 8, acPosPix.y);
			dc.LineTo(acPosPix.x + 12, acPosPix.y - 6);
			dc.MoveTo(acPosPix.x + 8, acPosPix.y);
			dc.LineTo(acPosPix.x + 12, acPosPix.y + 6);
		}

		int hitSize = max(iconSize, 12);
		std::string hoverTextStorage;
		const char* hoverText = "";
		if (AcisCorrelated)
		{
			hoverTextStorage = sceneTarget.bottomLine;
			hoverText = hoverTextStorage.c_str();
		}
		else
		{
			hoverText = sceneTarget.systemId.c_str();
		}
		iconVerboseStep("before_add_screen_object");
		AddScreenObject(DRAWING_AC_SYMBOL, rtCallsign.c_str(), { acPosPix.x - hitSize / 2, acPosPix.y - hitSize / 2, acPosPix.x + hitSize / 2, acPosPix.y + hitSize / 2 }, false, hoverText);
		iconVerboseStep("after_add_screen_object");
	}
	perfTargetsMs += AvisoMax(0.0, (RefreshPerfNowMs() - perfTargetsStartMs) - (perfRimcasMs - perfRimcasBeforeTargetsMs));
	if (frameUseFastRealisticBitmapRendering)
	{
		graphics.SetInterpolationMode(frameSavedInterpolationMode);
		graphics.SetPixelOffsetMode(frameSavedPixelOffsetMode);
		graphics.SetCompositingQuality(frameSavedCompositingQuality);
	}

#pragma endregion Drawing of the symbols

	tagAreas.clear();
	tagCollisionAreas.clear();

	graphics.SetSmoothingMode(SmoothingModeDefault);

	const double perfTagsStartMs = RefreshPerfNowMs();
	try
	{
		setRefreshStage("tag rendering");
		RenderTags(graphics, dc);
	}
	catch (const std::exception& ex)
	{
		Logger::info(std::string("RenderTags: std::exception caught: ") + ex.what());
	}
	catch (...)
	{
		Logger::info("RenderTags: unknown C++ exception caught");
	}
	perfTagsMs += RefreshPerfNowMs() - perfTagsStartMs;

	// RDF is resolved and projected on the EuroScope/UI thread.  Drawing it
	// here keeps the main-view marker above vSMR targets and labels, while the
	// inset renderers below can cover it with their own independently projected
	// RDF overlays.
	graphics.Flush(Gdiplus::FlushIntentionSync);
	const CRect rdfMainArea = ResolveMainAvisoRenderArea();
	if (!rdfMainArea.IsRectEmpty())
	{
		const double perfRdfStartMs = RefreshPerfNowMs();
		VsmrRdf::Draw(
			hDC,
			this,
			rdfMainArea,
			[this](const CPosition& position) -> POINT
			{
				return ConvertCoordFromPositionToPixel(position);
			});
		perfRdfMs += RefreshPerfNowMs() - perfRdfStartMs;
	}

	// Releasing the hDC after the drawing
	graphics.ReleaseHDC(hDC);

	CBrush BrushGrey(RGB(150, 150, 150));
	COLORREF oldColor = dc.SetTextColor(RGB(33, 33, 33));

	int TextHeight = dc.GetTextExtent("60").cy;
	VSMR_REFRESH_LOG("RIMCAS Loop");
	setRefreshStage("RIMCAS list rendering");
	const double perfRimcasListStartMs = RefreshPerfNowMs();
	for (std::map<string, bool>::iterator it = RimcasInstance->MonitoredRunwayArr.begin(); it != RimcasInstance->MonitoredRunwayArr.end(); ++it)
	{
		const auto timeTableIt = RimcasInstance->TimeTable.find(it->first);
		if (!it->second || timeTableIt == RimcasInstance->TimeTable.end() || timeTableIt->second.empty())
			continue;
		const auto& runwayTimeTable = timeTableIt->second;

		vector<int> TimeDefinition = RimcasInstance->CountdownDefinition;
		if (isLVP)
			TimeDefinition = RimcasInstance->CountdownDefinitionLVP;
		Color rimcasStageOneColor(255, 160, 90, 30);
		Color rimcasStageTwoColor(255, 150, 0, 0);
		const Value& activeProfile = CurrentConfig->getActiveProfile();
		if (activeProfile.IsObject() && activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		{
			const Value& rimcasConfig = activeProfile["rimcas"];
			if (rimcasConfig.HasMember("background_color_stage_one") && rimcasConfig["background_color_stage_one"].IsObject())
				rimcasStageOneColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_one"]);
			if (rimcasConfig.HasMember("background_color_stage_two") && rimcasConfig["background_color_stage_two"].IsObject())
				rimcasStageTwoColor = CurrentConfig->getConfigColor(rimcasConfig["background_color_stage_two"]);
		}

		auto timePopupAreaIt = TimePopupAreas.find(it->first);
		if (timePopupAreaIt == TimePopupAreas.end())
		{
			auto emplaceResult = TimePopupAreas.emplace(it->first, RECT{ 300, 300, 430, 300 + LONG(TextHeight * (TimeDefinition.size() + 1)) });
			timePopupAreaIt = emplaceResult.first;
		}

		CRect CRectTime = timePopupAreaIt->second;
		CRectTime.NormalizeRect();

		dc.FillRect(CRectTime, &BrushGrey);

		// Drawing the runway name
		string tempS = it->first;
		dc.TextOutA(CRectTime.left + CRectTime.Width() / 2 - dc.GetTextExtent(tempS.c_str()).cx / 2, CRectTime.top, tempS.c_str());

		int TopOffset = TextHeight;
		// Drawing the times
		for (auto &Time : TimeDefinition)
		{
			dc.SetTextColor(RGB(33, 33, 33));

			const auto timeEntryIt = runwayTimeTable.find(Time);
			const std::string acCallsign = (timeEntryIt != runwayTimeTable.end()) ? timeEntryIt->second : "";
			tempS = std::to_string(Time) + ": " + acCallsign;
			const VsmrScene::Target* sceneTarget = frameScene != nullptr
				? frameScene->FindTarget(acCallsign)
				: nullptr;
			if (sceneTarget != nullptr &&
				sceneTarget->rimcas.alertStage != static_cast<int>(CRimcas::NoAlert))
			{
				const Color alertColor = sceneTarget->rimcas.alertStage == static_cast<int>(CRimcas::StageOne)
					? rimcasStageOneColor
					: rimcasStageTwoColor;
				CBrush RimcasBrush(alertColor.ToCOLORREF());

				CRect TempRect = { CRectTime.left, CRectTime.top + TopOffset, CRectTime.right, CRectTime.top + TopOffset + TextHeight };
				TempRect.NormalizeRect();

				dc.FillRect(TempRect, &RimcasBrush);
				dc.SetTextColor(RGB(238, 238, 208));
			}

			dc.TextOutA(CRectTime.left, CRectTime.top + TopOffset, tempS.c_str());

			TopOffset += TextHeight;
		}

		AddScreenObject(RIMCAS_IAW, it->first.c_str(), CRectTime, true, "");

	}
	perfRimcasMs += RefreshPerfNowMs() - perfRimcasListStartMs;

	//
	// Tag deconflicting
	//

	VSMR_REFRESH_LOG("Tag deconfliction loop");
	setRefreshStage("tag deconfliction");
	const double perfTagDeconflictStartMs = RefreshPerfNowMs();
	bool autoDeconflictionEnabled = true;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() &&
			profile.HasMember("labels") &&
			profile["labels"].IsObject() &&
			profile["labels"].HasMember("auto_deconfliction") &&
			profile["labels"]["auto_deconfliction"].IsBool())
		{
			autoDeconflictionEnabled = profile["labels"]["auto_deconfliction"].GetBool();
		}
	}

	std::vector<std::string> staleTagCallsigns;
	auto isTagCoolingDown = [&](const std::string& callsign) -> bool
	{
		auto recentMoveIt = RecentlyAutoMovedTags.find(callsign);
		if (recentMoveIt == RecentlyAutoMovedTags.end())
			return false;

		const double elapsedSeconds = ((double)clock() - recentMoveIt->second) / ((double)CLOCKS_PER_SEC);
		if (elapsedSeconds >= 0.8)
		{
			RecentlyAutoMovedTags.erase(recentMoveIt);
			return false;
		}
		return true;
	};
	auto intersectsAnyOtherTag = [&](const std::string& callsign, const CRect& rect, bool* outRotateAntiClockwise) -> bool
	{
		for (const auto& otherArea : tagCollisionAreas)
		{
			if (callsign == otherArea.first)
				continue;
			if (IsTagBeingDragged(otherArea.first))
				continue;

			CRect intersection;
			if (!intersection.IntersectRect(rect, otherArea.second))
				continue;

			if (outRotateAntiClockwise != nullptr)
				*outRotateAntiClockwise = (rect.left <= otherArea.second.left);
			return true;
		}
		return false;
	};

	if (autoDeconflictionEnabled)
	{
		for (const auto& areaEntry : tagCollisionAreas)
		{
			const std::string& callsign = areaEntry.first;
			const CRect& currentRect = areaEntry.second;

			if (IsTagBeingDragged(callsign))
				continue;
			if (isTagCoolingDown(callsign))
				continue;

			// Decide the preferred rotation direction from current overlaps.
			bool rotateAntiClockwise = false;
			(void)intersectsAnyOtherTag(callsign, currentRect, &rotateAntiClockwise);

			const VsmrScene::Target* deconflictTarget = frameScene != nullptr
				? frameScene->FindTarget(callsign)
				: nullptr;
			if (deconflictTarget == nullptr || !deconflictTarget->position.valid)
			{
				staleTagCallsigns.push_back(callsign);
				if (Logger::is_verbose_mode())
				{
					Logger::info(
						"OnRefresh deconfliction: pruned stale tag state callsign=" + callsign +
						" target_valid=" + std::string(deconflictTarget != nullptr ? "1" : "0"));
				}
				continue;
			}

			auto angleIt = TagAngles.find(callsign);
			if (angleIt == TagAngles.end())
				angleIt = TagAngles.emplace(callsign, 270.0f).first;
			const double baseAngle = angleIt->second;

			int leaderLength = LeaderLineDefaultlenght;
			auto leaderLengthIt = TagLeaderLineLength.find(callsign);
			if (leaderLengthIt != TagLeaderLineLength.end())
				leaderLength = leaderLengthIt->second;

			CPosition deconflictPosition;
			deconflictPosition.m_Latitude = deconflictTarget->position.latitude;
			deconflictPosition.m_Longitude = deconflictTarget->position.longitude;
			const POINT acPosPix = ConvertCoordFromPositionToPixel(deconflictPosition);
			const int width = currentRect.Width();
			const int height = currentRect.Height();

			for (double rotated = 0.0; abs(rotated) <= 360.0;)
			{
				const double candidateAngle = fmod(baseAngle + rotated, 360.0f);
				POINT tagCenter;
				tagCenter.x = long(acPosPix.x + float(leaderLength * cos(DegToRad(candidateAngle))));
				tagCenter.y = long(acPosPix.y + float(leaderLength * sin(DegToRad(candidateAngle))));

				CRect newRectangle(
					tagCenter.x - (width / 2),
					tagCenter.y - (height / 2),
					tagCenter.x + (width / 2),
					tagCenter.y + (height / 2));
				newRectangle.NormalizeRect();

				const bool hasConflict = intersectsAnyOtherTag(callsign, newRectangle, nullptr);
				if (!hasConflict)
				{
					angleIt->second = fmod(baseAngle + rotated, 360.0f);

					const POINT newCenter = newRectangle.CenterPoint();
					const POINT newOffset = { newCenter.x - acPosPix.x, newCenter.y - acPosPix.y };
					TagsOffsets[callsign] = newOffset;

					tagCollisionAreas[callsign] = newRectangle;
					RecentlyAutoMovedTags[callsign] = clock();
					break;
				}

				rotated += rotateAntiClockwise ? -22.5f : 22.5f;
			}
		}
	}
	if (!staleTagCallsigns.empty())
	{
		for (const std::string& callsign : staleTagCallsigns)
		{
			TagsOffsets.erase(callsign);
			TagAngles.erase(callsign);
			TagLeaderLineLength.erase(callsign);
			tagAreas.erase(callsign);
			tagCollisionAreas.erase(callsign);
			previousTagSize.erase(callsign);
			TagDragOffsetFromCenter.erase(callsign);
			RecentlyAutoMovedTags.erase(callsign);
			Patatoides.erase(callsign);
		}

		if (Logger::is_verbose_mode())
		{
			Logger::info(
				"OnRefresh deconfliction: removed stale entries count=" +
				std::to_string(staleTagCallsigns.size()));
		}
	}
	perfTagsMs += RefreshPerfNowMs() - perfTagDeconflictStartMs;

	//
	// App windows
	//

	VSMR_REFRESH_LOG("App window rendering");
	setRefreshStage("app window rendering");

	SyncLinkedAvisoSecondaryToMainView();

	bool insetRenderedForWheel = false;
	gAvisoWheelRoutingEnabled = false;
	CInsetWindow* activeInsetWindow = nullptr;
	for (const auto& display : appWindowDisplays)
	{
		if (!display.second)
			continue;
		auto appWindowIt = appWindows.find(display.first);
		if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
			(appWindowIt->second->IsWindowMoveActive() || appWindowIt->second->IsWindowResizeActive()))
		{
			activeInsetWindow = appWindowIt->second.get();
			break;
		}
	}
	CInsetWindow* foregroundInsetWindow = activeInsetWindow;
	if (foregroundInsetWindow == nullptr)
	{
		for (const auto& display : appWindowDisplays)
		{
			if (!display.second)
				continue;
			auto appWindowIt = appWindows.find(display.first);
			if (appWindowIt != appWindows.end() && appWindowIt->second != nullptr &&
				appWindowIt->second->m_AvisoScrollSelected)
			{
				foregroundInsetWindow = appWindowIt->second.get();
				break;
			}
		}
	}
	auto renderInsetWindow = [&](CInsetWindow* appWindow)
	{
		if (appWindow == nullptr)
			return;
		const double insetStartMs = RefreshPerfNowMs();
		appWindow->render(hDC, this, &graphics, mouseLocation);
		const double insetElapsedMs = RefreshPerfNowMs() - insetStartMs;
		if (appWindow->IsSecondaryRadar())
			perfSrwMs += insetElapsedMs;
		else if (appWindow->IsAvisoViewport())
			perfAvisoInsetMs += insetElapsedMs;
		perfRdfMs += appWindow->GetLastRdfRenderMilliseconds();
		perfInsetChromeMs += appWindow->GetLastChromeRenderMilliseconds();
		if (appWindow->m_AvisoScreenAreaValid)
			insetRenderedForWheel = true;
	};
	for (std::map<int, bool>::iterator it = appWindowDisplays.begin(); it != appWindowDisplays.end(); ++it)
	{
		if (!it->second)
			continue;

		int appWindowId = it->first;
		auto appWindowIt = appWindows.find(appWindowId);
		if (appWindowIt == appWindows.end() || appWindowIt->second == nullptr)
			continue;

		CInsetWindow* appWindow = appWindowIt->second.get();
		if (appWindow != foregroundInsetWindow)
			renderInsetWindow(appWindow);
	}
	if (activeInsetWindow != nullptr)
		activeInsetWindow->RenderSnapPreview(graphics);
	if (foregroundInsetWindow != nullptr)
		renderInsetWindow(foregroundInsetWindow);
	gAvisoWheelRoutingEnabled = insetRenderedForWheel;

	setRefreshStage("fps overlay");
	if (ShowFps)
	{
		const std::string fpsText = "FPS " + std::to_string(FpsDisplayValue);
		CFont* previousFont = RuntimeOverlayFont.GetSafeHandle() != nullptr
			? dc.SelectObject(&RuntimeOverlayFont)
			: nullptr;
		const CSize fpsSize = dc.GetTextExtent(fpsText.c_str());
		CRect fpsArea = ResolveMainAvisoRenderArea();
		if (fpsArea.IsRectEmpty())
			fpsArea = CRect(RadarArea);
		fpsArea.NormalizeRect();
		auto overlapsVisibleInset = [&](const CRect& candidate, int& nextY) -> bool
		{
			bool overlaps = false;
			for (const auto& display : appWindowDisplays)
			{
				if (!display.second)
					continue;
				auto windowIt = appWindows.find(display.first);
				if (windowIt == appWindows.end() || windowIt->second == nullptr)
					continue;
				CRect overlap;
				const CRect frame = windowIt->second->GetWindowFrameRect();
				if (overlap.IntersectRect(candidate, frame) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, frame.bottom + 4);
				}
				CRect preview;
				if (windowIt->second->GetSnapPreviewRect(preview) &&
					overlap.IntersectRect(candidate, preview) && !overlap.IsRectEmpty())
				{
					overlaps = true;
					nextY = max(nextY, preview.bottom + 4);
				}
			}
			return overlaps;
		};
		auto findFpsPosition = [&](bool alignRight, POINT& position) -> bool
		{
			int y = fpsArea.top + 4;
			for (size_t attempt = 0; attempt <= appWindows.size(); ++attempt)
			{
				const int x = alignRight
					? max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6)
					: fpsArea.left + 6;
				CRect candidate(x, y, x + fpsSize.cx, y + fpsSize.cy);
				if (candidate.right > fpsArea.right || candidate.bottom > fpsArea.bottom)
					return false;
				int nextY = y;
				if (!overlapsVisibleInset(candidate, nextY))
				{
					position = { x, y };
					return true;
				}
				if (nextY <= y)
					return false;
				y = nextY;
			}
			return false;
		};
		POINT fpsPosition = {
			max(fpsArea.left + 4, fpsArea.right - fpsSize.cx - 6),
			fpsArea.top + 4
		};
		bool fpsPositionFound = findFpsPosition(true, fpsPosition);
		if (!fpsPositionFound)
			fpsPositionFound = findFpsPosition(false, fpsPosition);
		const int previousBackgroundMode = dc.SetBkMode(TRANSPARENT);
		const COLORREF previousTextColor = dc.SetTextColor(RGB(235, 245, 247));
		if (fpsPositionFound)
			dc.TextOutA(fpsPosition.x, fpsPosition.y, fpsText.c_str());
		dc.SetTextColor(previousTextColor);
		dc.SetBkMode(previousBackgroundMode);
		if (previousFont != nullptr)
			dc.SelectObject(previousFont);
	}

	setRefreshStage("runtime menu");
	RenderRuntimeMenu(hDC, graphics);

	PerfLastFrameMs = RefreshPerfNowMs() - perfFrameStartMs;
	PerfLastAvisoMs = perfAvisoMs;
	PerfLastTargetsMs = perfTargetsMs;
	PerfLastRimcasMs = perfRimcasMs;
	PerfLastTagsMs = perfTagsMs;
	PerfLastSrwMs = perfSrwMs;
	PerfLastAvisoInsetMs = perfAvisoInsetMs;
	PerfLastRdfMs = perfRdfMs;
	PerfLastInsetChromeMs = perfInsetChromeMs;
	VsmrPerformance::FrameSample performanceFrame;
	performanceFrame.frameId = frameScene != nullptr ? frameScene->frameId : RadarSceneFrameId;
	performanceFrame.timestampMilliseconds =
		VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds();
	performanceFrame.frameMilliseconds = PerfLastFrameMs;
	performanceFrame.sceneMilliseconds = PerfLastSceneBuildMs;
	performanceFrame.avisoMilliseconds = PerfLastAvisoMs;
	performanceFrame.targetsMilliseconds = PerfLastTargetsMs;
	performanceFrame.rimcasMilliseconds = PerfLastRimcasMs;
	performanceFrame.tagsMilliseconds = PerfLastTagsMs;
	performanceFrame.srwMilliseconds = PerfLastSrwMs;
	performanceFrame.avisoInsetMilliseconds = PerfLastAvisoInsetMs;
	performanceFrame.rdfMilliseconds = PerfLastRdfMs;
	performanceFrame.insetChromeMilliseconds = PerfLastInsetChromeMs;
	performanceFrame.visibleTargets = frameVisibleTargetCount;
	performanceFrame.visibleTags = tagAreas.size();
	if (frameScene != nullptr)
	{
		performanceFrame.sceneAvisoLoadMilliseconds = frameScene->stats.avisoLoadMilliseconds;
		performanceFrame.sceneControllerOwnershipMilliseconds =
			frameScene->stats.controllerOwnershipMilliseconds;
		performanceFrame.sceneTargetCaptureMilliseconds = frameScene->stats.targetCaptureMilliseconds;
		performanceFrame.sceneFinalizeMilliseconds = frameScene->stats.finalizeMilliseconds;
		performanceFrame.euroScopeLookupMilliseconds = frameScene->stats.sdkLookupMilliseconds;
		frameRefreshReasonMask |= frameScene->stats.refreshReasonMask;
		performanceFrame.processedTargets = frameScene->stats.sdkTargetEnumerations;
		performanceFrame.capturedTargets = frameScene->stats.targetCount;
		performanceFrame.radarFilteredTargets = frameScene->stats.radarFilteredTargetCount;
		performanceFrame.iconTargets = frameScene->stats.iconTargetCount;
		performanceFrame.tagTargets = frameScene->stats.tagTargetCount;
	}
	performanceFrame.refreshReasonMask = frameRefreshReasonMask;
	PerformanceDiagnostics.RecordFrame(performanceFrame);
	SamplePerformanceResourcesIfDue();
	const unsigned long perfNowTick = ::GetTickCount();
	if (PerfLastLogTick == 0 || perfNowTick - PerfLastLogTick >= 2000)
	{
		PerfLastLogTick = perfNowTick;
		Logger::info(
			"FramePerf ms frame=" + std::to_string(static_cast<int>(PerfLastFrameMs + 0.5)) +
			" scene=" + std::to_string(static_cast<int>(PerfLastSceneBuildMs + 0.5)) +
			" aviso=" + std::to_string(static_cast<int>(PerfLastAvisoMs + 0.5)) +
			" targets=" + std::to_string(static_cast<int>(PerfLastTargetsMs + 0.5)) +
			" rimcas=" + std::to_string(static_cast<int>(PerfLastRimcasMs + 0.5)) +
			" tags=" + std::to_string(static_cast<int>(PerfLastTagsMs + 0.5)) +
			" srw=" + std::to_string(static_cast<int>(PerfLastSrwMs + 0.5)) +
			" aviso_inset=" + std::to_string(static_cast<int>(PerfLastAvisoInsetMs + 0.5)) +
			" rdf=" + std::to_string(static_cast<int>(PerfLastRdfMs + 0.5)) +
			" inset_chrome=" + std::to_string(static_cast<int>(PerfLastInsetChromeMs + 0.5)) +
			(frameScene != nullptr
				? " scene_targets=" + std::to_string(frameScene->stats.targetCount) +
				  " scene_tags=" + std::to_string(frameScene->stats.tagElementCount) +
				  " scene_controllers=" + std::to_string(frameScene->stats.controllerCount) +
				  " scene_bytes_lower=" + std::to_string(frameScene->stats.lowerBoundOwnedBytes) +
				  " scene_es_targets=" + std::to_string(frameScene->stats.sdkTargetEnumerations) +
				  " scene_es_fp=" + std::to_string(frameScene->stats.sdkFlightPlanLookups) +
				  " scene_es_correlated_fp=" + std::to_string(frameScene->stats.sdkCorrelatedFlightPlanLookups) +
				  " scene_vacdm=" + std::to_string(frameScene->stats.vacdmLookups)
				: " scene_unavailable=1"));
	}

	dcDetach.Detach();
	setRefreshStage("complete");

	VSMR_REFRESH_LOG("END "+ string(__FUNCSIG__));

	}
	catch (COleException* ex)
	{
		long scode = 0;
		if (ex != nullptr)
		{
			scode = static_cast<long>(ex->m_sc);
			ex->Delete();
		}
		logRefreshException("OLE exception scode=" + std::to_string(scode));
	}
	catch (CException* ex)
	{
		if (ex != nullptr)
			ex->Delete();
		logRefreshException("MFC exception");
	}
	catch (const std::exception& ex)
	{
		logRefreshException(std::string("std exception: ") + ex.what());
	}
	catch (...)
	{
		logRefreshException("unknown exception");
	}
}

// ReSharper restore CppMsExtAddressOfClassRValue

//---EuroScopePlugInExitCustom-----------------------------------------------

void CSMRRadar::EuroScopePlugInExitCustom()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

		BeginShutdown();
		CloseVsmrControlCenterWindow();
		CloseAvisoEditorWindow();
		DestroyAvisoEditorWindow();
		CloseProfileEditorWindow(false);
		DestroyProfileEditorWindow();
		DestroyVsmrControlCenterWindow();

		RestoreInsetWindowProcHooks();
		UnhookAvisoThreadHooks();
		gWindowProcRadarScreen = nullptr;
}
