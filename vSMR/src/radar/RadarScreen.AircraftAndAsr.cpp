#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "insets/InsetWindow.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "crash/CrashRuntime.hpp"
#include "shared/WindowsPathEncoding.hpp"

#include <ShlObj.h>
#include <array>
#include <cerrno>
#include <cstdlib>

namespace
{
	std::string NormalizeInsetAirport(std::string airport)
	{
		const auto first = std::find_if_not(airport.begin(), airport.end(), [](unsigned char value) {
			return std::isspace(value) != 0;
		});
		const auto last = std::find_if_not(airport.rbegin(), airport.rend(), [](unsigned char value) {
			return std::isspace(value) != 0;
		}).base();
		if (first >= last)
			return "";

		airport = std::string(first, last);
		std::transform(airport.begin(), airport.end(), airport.begin(), [](unsigned char value) {
			return static_cast<char>(std::toupper(value));
		});
		return airport;
	}

	std::string AirportInsetAsrPrefix(const std::string& airport)
	{
		const std::string normalized = NormalizeInsetAirport(airport);
		return normalized.empty() ? "" : "Insets." + normalized + ".";
	}

	bool ParseAsrInt(const char* text, int minimum, int maximum, int& value)
	{
		if (text == nullptr || *text == '\0')
			return false;
		errno = 0;
		char* end = nullptr;
		const long parsed = std::strtol(text, &end, 10);
		if (errno == ERANGE || end == text || *end != '\0' ||
			parsed < minimum || parsed > maximum)
			return false;
		value = static_cast<int>(parsed);
		return true;
	}

	bool ParseAsrDouble(const char* text, double minimum, double maximum, double& value)
	{
		if (text == nullptr || *text == '\0')
			return false;
		errno = 0;
		char* end = nullptr;
		const double parsed = std::strtod(text, &end);
		if (errno == ERANGE || end == text || *end != '\0' ||
			!std::isfinite(parsed) || parsed < minimum || parsed > maximum)
			return false;
		value = parsed;
		return true;
	}
}

VsmrTargetRendering::IconCacheCallbacks CSMRRadar::CreateTargetIconCacheCallbacks()
{
	VsmrTargetRendering::IconCacheCallbacks callbacks;
	callbacks.beginFrame = [this]() -> std::uint64_t
	{
		return ++RealisticIconCacheFrame;
	};
	callbacks.getSourceBitmap = [this](const std::string& iconType) -> Gdiplus::Bitmap*
	{
		return GetAircraftIcon(iconType);
	};
	callbacks.getScaledBitmap = [this](
		const std::string& iconType,
		Gdiplus::Bitmap* sourceBitmap,
		UINT sourceWidth,
		UINT sourceHeight,
		const Gdiplus::Color& tintColor,
		double drawWidth,
		double drawHeight,
		std::uint64_t cacheFrame,
		int& pixelWidth,
		int& pixelHeight,
		std::string& cacheKey) -> Gdiplus::Bitmap*
	{
		return GetCachedRealisticIconBitmap(
			iconType,
			sourceBitmap,
			sourceWidth,
			sourceHeight,
			true,
			tintColor,
			drawWidth,
			drawHeight,
			cacheFrame,
			pixelWidth,
			pixelHeight,
			cacheKey);
	};
	callbacks.getRotatedBitmap = [this](
		const std::string& cacheKey,
		Gdiplus::Bitmap* scaledBitmap,
		int width,
		int height,
		double rotationDegrees,
		std::uint64_t cacheFrame) -> VsmrTargetRendering::CachedBitmap
	{
		RealisticIconCacheEntry* cached = GetCachedRotatedRealisticIconBitmap(
			cacheKey,
			scaledBitmap,
			width,
			height,
			rotationDegrees,
			cacheFrame);
		if (cached == nullptr)
			return {};
		return { cached->bitmap.get(), cached->centerX, cached->centerY };
	};
	return callbacks;
}

Gdiplus::Bitmap* CSMRRadar::GetAircraftIcon(const std::string& acTypeRaw)
{
	std::string ac = acTypeRaw;
	if (ac.empty())
		return nullptr;
	std::transform(ac.begin(), ac.end(), ac.begin(), ::tolower);

	auto it = AircraftIcons.find(ac);
	if (it != AircraftIcons.end())
	{
		PerformanceDiagnostics.RecordAircraftSourceCache(true);
		return it->second.get();
	}
	PerformanceDiagnostics.RecordAircraftSourceCache(false);

	const fs::path candidate = fs::u8path(IconsPath) / (ac + ".png");
	if (!fs::exists(candidate))
	{
		AircraftIcons[ac] = nullptr;
		return nullptr;
	}

	auto bmp = std::make_unique<Gdiplus::Bitmap>(candidate.wstring().c_str());
	if (bmp->GetLastStatus() != Gdiplus::Ok)
	{
		AircraftIcons[ac] = nullptr;
		return nullptr;
	}

	AircraftIcons[ac] = std::move(bmp);
	return AircraftIcons[ac].get();
}

void CSMRRadar::TrimRealisticIconBitmapCache(const std::string& protectedCacheKey, unsigned long long cacheFrame)
{
	const size_t softLimit = 384;
	const size_t hardLimit = 512;
	if (RealisticIconBitmapCache.size() <= hardLimit)
		return;

	const unsigned long long staleBefore = (cacheFrame > 120) ? (cacheFrame - 120) : 0;
	for (auto it = RealisticIconBitmapCache.begin();
		it != RealisticIconBitmapCache.end() && RealisticIconBitmapCache.size() > softLimit;)
	{
		if (it->first != protectedCacheKey && it->second.lastUsedFrame < staleBefore)
			it = RealisticIconBitmapCache.erase(it);
		else
			++it;
	}

	if (RealisticIconBitmapCache.size() <= hardLimit)
		return;

	std::vector<std::pair<unsigned long long, std::string>> cacheAge;
	cacheAge.reserve(RealisticIconBitmapCache.size());
	for (const auto& cacheItem : RealisticIconBitmapCache)
		cacheAge.emplace_back(cacheItem.second.lastUsedFrame, cacheItem.first);
	std::sort(cacheAge.begin(), cacheAge.end());

	for (const auto& cacheItem : cacheAge)
	{
		if (RealisticIconBitmapCache.size() <= softLimit)
			break;
		if (cacheItem.second == protectedCacheKey)
			continue;
		RealisticIconBitmapCache.erase(cacheItem.second);
	}
}

Gdiplus::Bitmap* CSMRRadar::GetCachedRealisticIconBitmap(
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
	std::string& outCacheKey)
{
	outDrawW = std::clamp(static_cast<int>(std::lround(drawW)), 1, 2048);
	outDrawH = std::clamp(static_cast<int>(std::lround(drawH)), 1, 2048);
	outCacheKey.clear();
	if (sourceBitmap == nullptr || sourceWidth == 0 || sourceHeight == 0 || iconType.empty())
		return nullptr;

	const unsigned int tintKey = applyTint
		? ((static_cast<unsigned int>(tintColor.GetAlpha()) << 24) |
			(static_cast<unsigned int>(tintColor.GetR()) << 16) |
			(static_cast<unsigned int>(tintColor.GetG()) << 8) |
			static_cast<unsigned int>(tintColor.GetB()))
		: 0xffffffffu;
	outCacheKey =
		"s|" + iconType + "|" +
		std::to_string(outDrawW) + "x" + std::to_string(outDrawH) + "|" +
		std::to_string(tintKey);

	auto cachedBitmap = RealisticIconBitmapCache.find(outCacheKey);
	if (cachedBitmap != RealisticIconBitmapCache.end())
	{
		PerformanceDiagnostics.RecordRealisticScaledCache(true);
		cachedBitmap->second.lastUsedFrame = cacheFrame;
		return cachedBitmap->second.bitmap.get();
	}
	PerformanceDiagnostics.RecordRealisticScaledCache(false);

	TrimRealisticIconBitmapCache(std::string(), cacheFrame);

	auto scaledBitmap = std::make_unique<Gdiplus::Bitmap>(
		outDrawW,
		outDrawH,
		PixelFormat32bppPARGB);
	if (scaledBitmap == nullptr || scaledBitmap->GetLastStatus() != Gdiplus::Ok)
		return nullptr;

	Gdiplus::Graphics scaledGraphics(scaledBitmap.get());
	if (scaledGraphics.GetLastStatus() != Gdiplus::Ok)
		return nullptr;

	scaledGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
	scaledGraphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
	scaledGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
	scaledGraphics.Clear(Gdiplus::Color(0, 0, 0, 0));

	Gdiplus::Rect destinationRect(0, 0, outDrawW, outDrawH);
	if (applyTint)
	{
		const Gdiplus::REAL tintAlpha = static_cast<Gdiplus::REAL>(tintColor.GetAlpha()) / 255.0f;
		Gdiplus::ColorMatrix colorMatrix = {
			{
				{ static_cast<Gdiplus::REAL>(tintColor.GetR()) / 255.0f, 0.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, static_cast<Gdiplus::REAL>(tintColor.GetG()) / 255.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, static_cast<Gdiplus::REAL>(tintColor.GetB()) / 255.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 0.0f, tintAlpha, 0.0f },
				{ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }
			}
		};
		Gdiplus::ImageAttributes tintAttributes;
		tintAttributes.SetColorMatrix(&colorMatrix, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
		scaledGraphics.DrawImage(
			sourceBitmap,
			destinationRect,
			0,
			0,
			static_cast<INT>(sourceWidth),
			static_cast<INT>(sourceHeight),
			UnitPixel,
			&tintAttributes);
	}
	else
	{
		scaledGraphics.DrawImage(
			sourceBitmap,
			destinationRect,
			0,
			0,
			static_cast<INT>(sourceWidth),
			static_cast<INT>(sourceHeight),
			UnitPixel);
	}

	RealisticIconCacheEntry cacheEntry;
	cacheEntry.bitmap = std::move(scaledBitmap);
	cacheEntry.centerX = outDrawW / 2;
	cacheEntry.centerY = outDrawH / 2;
	cacheEntry.lastUsedFrame = cacheFrame;
	auto inserted = RealisticIconBitmapCache.emplace(outCacheKey, std::move(cacheEntry));
	return inserted.first->second.bitmap.get();
}

CSMRRadar::RealisticIconCacheEntry* CSMRRadar::GetCachedRotatedRealisticIconBitmap(
	const std::string& scaledCacheKey,
	Gdiplus::Bitmap* scaledBitmap,
	int scaledWidth,
	int scaledHeight,
	double rotationDeg,
	unsigned long long cacheFrame)
{
	if (scaledCacheKey.empty() || scaledBitmap == nullptr || scaledWidth <= 0 || scaledHeight <= 0)
		return nullptr;

	double normalizedRotation = std::fmod(rotationDeg, 360.0);
	if (normalizedRotation < 0.0)
		normalizedRotation += 360.0;
	const int rotationBucket = static_cast<int>(std::lround(normalizedRotation / 2.0)) % 180;
	const double cachedRotationDeg = static_cast<double>(rotationBucket) * 2.0;
	const std::string cacheKey = "r|" + scaledCacheKey + "|" + std::to_string(rotationBucket);

	auto cachedBitmap = RealisticIconBitmapCache.find(cacheKey);
	if (cachedBitmap != RealisticIconBitmapCache.end())
	{
		PerformanceDiagnostics.RecordRealisticRotatedCache(true);
		cachedBitmap->second.lastUsedFrame = cacheFrame;
		return &cachedBitmap->second;
	}
	PerformanceDiagnostics.RecordRealisticRotatedCache(false);

	TrimRealisticIconBitmapCache(scaledCacheKey, cacheFrame);

	const double rotationRadians = cachedRotationDeg * M_PI / 180.0;
	const double absCos = std::abs(std::cos(rotationRadians));
	const double absSin = std::abs(std::sin(rotationRadians));
	const int rotatedWidth = std::clamp(
		static_cast<int>(std::ceil(static_cast<double>(scaledWidth) * absCos + static_cast<double>(scaledHeight) * absSin)),
		1,
		3072);
	const int rotatedHeight = std::clamp(
		static_cast<int>(std::ceil(static_cast<double>(scaledWidth) * absSin + static_cast<double>(scaledHeight) * absCos)),
		1,
		3072);
	const int centerX = rotatedWidth / 2;
	const int centerY = rotatedHeight / 2;

	auto rotatedBitmap = std::make_unique<Gdiplus::Bitmap>(
		rotatedWidth,
		rotatedHeight,
		PixelFormat32bppPARGB);
	if (rotatedBitmap == nullptr || rotatedBitmap->GetLastStatus() != Gdiplus::Ok)
		return nullptr;

	Gdiplus::Graphics rotatedGraphics(rotatedBitmap.get());
	if (rotatedGraphics.GetLastStatus() != Gdiplus::Ok)
		return nullptr;

	rotatedGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
	rotatedGraphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
	rotatedGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
	rotatedGraphics.Clear(Gdiplus::Color(0, 0, 0, 0));

	Gdiplus::Matrix rotationTransform;
	rotationTransform.Translate(
		static_cast<Gdiplus::REAL>(centerX),
		static_cast<Gdiplus::REAL>(centerY));
	rotationTransform.Rotate(static_cast<Gdiplus::REAL>(cachedRotationDeg));
	rotationTransform.Translate(
		static_cast<Gdiplus::REAL>(-scaledWidth / 2.0),
		static_cast<Gdiplus::REAL>(-scaledHeight / 2.0));
	rotatedGraphics.SetTransform(&rotationTransform);
	rotatedGraphics.DrawImage(scaledBitmap, 0, 0);

	RealisticIconCacheEntry cacheEntry;
	cacheEntry.bitmap = std::move(rotatedBitmap);
	cacheEntry.centerX = centerX;
	cacheEntry.centerY = centerY;
	cacheEntry.lastUsedFrame = cacheFrame;
	auto inserted = RealisticIconBitmapCache.emplace(cacheKey, std::move(cacheEntry));
	return &inserted.first->second;
}

void CSMRRadar::LoadAircraftSpecs() {
	AircraftSpecs.clear();
	std::vector<fs::path> candidates;

	auto pushUniqueCandidate = [&](const fs::path& candidate) {
		if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			candidates.push_back(candidate);
		};

	if (!DataPath.empty())
		pushUniqueCandidate(fs::u8path(DataPath) / "ICAO_Aircraft.json");
	pushUniqueCandidate(fs::u8path(DllPath) / "vSMR_Data" / "ICAO_Aircraft.json");

	// Legacy flat AppData\Roaming\EuroScope\LFXX\Plugins fallback
	std::array<wchar_t, MAX_PATH> appData{};
	if (SUCCEEDED(::SHGetFolderPathW(
		nullptr,
		CSIDL_APPDATA,
		nullptr,
		SHGFP_TYPE_CURRENT,
		appData.data()))) {
		fs::path roaming(appData.data());
		pushUniqueCandidate(roaming / "EuroScope" / "LFXX" / "Plugins" / "ICAO_Aircraft.json");
	}

	// Plugin folder and parent
	pushUniqueCandidate(fs::u8path(DllPath) / "ICAO_Aircraft.json");
	pushUniqueCandidate(fs::u8path(DllPath).parent_path() / "ICAO_Aircraft.json");

	auto getStringMember = [](const rapidjson::Value& obj, std::initializer_list<const char*> keys, std::string& out) -> bool {
		for (auto k : keys) {
			if (obj.HasMember(k) && obj[k].IsString()) {
				out = obj[k].GetString();
				return true;
			}
		}
		return false;
	};

	auto getNumberMember = [](const rapidjson::Value& obj, std::initializer_list<const char*> keys, double& out) -> bool {
		for (auto k : keys) {
			if (obj.HasMember(k) && obj[k].IsNumber()) {
				out = obj[k].GetDouble();
				return true;
			}
		}
		return false;
	};

	int totalLoaded = 0;

	for (auto& p : candidates) {
		Logger::info("Trying to read aircraft specs from: " + p.u8string());
		if (!fs::exists(p)) {
			Logger::info("Specs file not found at: " + p.u8string());
			continue;
		}

		std::ifstream ifs(p, std::ios::binary);
		std::stringstream ss;
		ss << ifs.rdbuf();
		ifs.close();

		// Sanitize JSON to handle BOM, comments, and trailing commas
		auto sanitizeJson = [](const std::string& in) {
			std::string out;
			out.reserve(in.size());

			// Remove UTF-8 BOM if present
			size_t start = 0;
			if (in.size() >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF) {
				start = 3;
			}

			bool inString = false;
			bool escape = false;
			bool lineComment = false;
			bool blockComment = false;

			for (size_t i = start; i < in.size(); ++i) {
				char c = in[i];

				if (lineComment) {
					if (c == '\n') {
						lineComment = false;
						out.push_back(c);
					}
					continue;
				}
				if (blockComment) {
					if (c == '*' && i + 1 < in.size() && in[i + 1] == '/') {
						blockComment = false;
						++i;
					}
					continue;
				}

				if (!inString && c == '/' && i + 1 < in.size()) {
					if (in[i + 1] == '/') {
						lineComment = true;
						++i;
						continue;
					}
					if (in[i + 1] == '*') {
						blockComment = true;
						++i;
						continue;
					}
				}

				if (!inString && c == '\r') {
					continue; // drop CR
				}

				out.push_back(c);

				if (c == '\\' && !escape) {
					escape = true;
					continue;
				}
				if (c == '"' && !escape) {
					inString = !inString;
				}
				escape = false;
			}

			// Remove trailing commas before ']' or '}'
			std::string finalOut;
			finalOut.reserve(out.size());
			for (size_t i = 0; i < out.size(); ++i) {
				if (out[i] == ',') {
					size_t j = i + 1;
					while (j < out.size() && isspace(static_cast<unsigned char>(out[j]))) {
						++j;
					}
					if (j < out.size() && (out[j] == ']' || out[j] == '}')) {
						continue; // skip this comma
					}
				}
				finalOut.push_back(out[i]);
			}

			return finalOut;
		};

		std::string rawJson = ss.str();
		std::string sanitized = sanitizeJson(rawJson);

		rapidjson::Document doc;
		if (doc.Parse<0>(sanitized.c_str()).HasParseError()) {
			Logger::info("Parse error in ICAO_Aircraft.json at: " + p.u8string());
			continue;
		}
		int loaded = 0;

		auto loadEntry = [&](const rapidjson::Value& entry, const std::string& implicitCode = "") {
			std::string code = implicitCode;
			double length = 0.0;
			double wingspan = 0.0;

			// Support both our native schema and the ICAO_Aircraft.json schema from GNG
			bool okCode = !code.empty() || getStringMember(entry, { "icao_code", "ICAO" }, code);
			bool okLen = getNumberMember(entry, { "length", "Length" }, length);
			bool okSpan = getNumberMember(entry, { "wingspan", "Wingspan" }, wingspan);
			if (!okCode || !okLen || !okSpan)
				return;

			std::transform(code.begin(), code.end(), code.begin(), ::tolower);
			AircraftSpec spec;
			spec.length = length;
			spec.wingspan = wingspan;
			AircraftSpecs[code] = spec;
			loaded++;
		};

		if (doc.IsArray()) {
			for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
				const auto& entry = doc[i];
				loadEntry(entry);
			}
		} else if (doc.IsObject()) {
			for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
				const auto& entry = it->value;
				loadEntry(entry, it->name.GetString());
			}
		} else {
			Logger::info("ICAO_Aircraft.json has unexpected root type at: " + p.u8string());
		}

		Logger::info("Loaded " + std::to_string(loaded) + " aircraft specs from " + p.u8string());
		totalLoaded += loaded;
		if (loaded > 0)
			break;
	}

	Logger::info("Total aircraft specs loaded: " + std::to_string(totalLoaded));
}
void CSMRRadar::ResetInsetWindowState(int appWindowId, bool preserveVisibility)
{
	const auto windowIt = appWindows.find(appWindowId);
	if (windowIt == appWindows.end() || windowIt->second == nullptr)
		return;

	const bool wasVisible = appWindowDisplays.find(appWindowId) != appWindowDisplays.end() &&
		appWindowDisplays[appWindowId];
	CInsetWindow* window = windowIt->second.get();
	window->m_Offset = { 0, 0 };
	window->m_OffsetInit = { 0, 0 };
	window->m_OffsetDrag = { 0, 0 };
	window->m_Grip = false;
	window->m_AvisoLayoutMode = CInsetWindow::AvisoLayoutMode::Floating;
	window->ResetAvisoInteractionState();

	if (window->IsAvisoViewport())
	{
		window->m_Area = { 260, 260, 760, 560 };
		window->m_AvisoScale = 350;
		window->m_AvisoCenterLatitude = 0.0;
		window->m_AvisoCenterLongitude = 0.0;
		window->m_AvisoDragStartLatitude = 0.0;
		window->m_AvisoDragStartLongitude = 0.0;
		window->m_AvisoViewInitialized = false;
		window->ClearAvisoViewportCache();
	}
	else if (window->IsWeather())
	{
		window->m_Area = { 300, 200, 606, 375 };
	}
	else if (window->IsTimer())
	{
		window->m_Area = { 100, 180, 184, 236 };
		window->m_AvisoLayoutMode = CInsetWindow::AvisoLayoutMode::Floating;
	}
	else
	{
		window->m_Area = { 200, 200, 600, 500 };
		window->m_Scale = 15;
		window->m_Filter = 5500;
		window->m_Rotation = 0.0;
	}

	appWindowDisplays[appWindowId] = preserveVisibility ? wasVisible : false;
}

void CSMRRadar::ResetAllInsetWindowStates(bool preserveVisibility)
{
	ResetInsetWindowState(1, preserveVisibility);
	ResetInsetWindowState(APPWINDOW_AVISO - APPWINDOW_BASE, preserveVisibility);
	ResetInsetWindowState(APPWINDOW_WEATHER - APPWINDOW_BASE, preserveVisibility);
	ResetInsetWindowState(APPWINDOW_TIMER - APPWINDOW_BASE, preserveVisibility);
}

void CSMRRadar::SaveInsetStateToAsrForAirport(const std::string& airport)
{
	const std::string normalizedAirport = NormalizeInsetAirport(airport);
	if (normalizedAirport.empty() ||
		UnsupportedInsetAsrStateAirports.find(normalizedAirport) != UnsupportedInsetAsrStateAirports.end())
	{
		return;
	}

	const std::string prefix = AirportInsetAsrPrefix(airport);
	if (prefix.empty())
		return;

	auto save = [&](const std::string& suffix, const char* description, const std::string& value)
	{
		const std::string key = prefix + suffix;
		SaveDataToAsr(key.c_str(), description, value.c_str());
	};

	save("Version", "Airport-specific inset state version", "3");
	const auto avisoPath = AvisoGeoJsonOverridePaths.find(normalizedAirport);
	save(
		"AvisoFile",
		"Airport-specific AVISO source file",
		avisoPath != AvisoGeoJsonOverridePaths.end() ? avisoPath->second : std::string());
	for (const int id : { 1 })
	{
		const auto windowIt = appWindows.find(id);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		CInsetWindow* window = windowIt->second.get();
		const std::string windowPrefix = "SRW" + std::to_string(id);
		save(windowPrefix + "TopLeftX", "SRW position", std::to_string(window->m_Area.left));
		save(windowPrefix + "TopLeftY", "SRW position", std::to_string(window->m_Area.top));
		save(windowPrefix + "BottomRightX", "SRW position", std::to_string(window->m_Area.right));
		save(windowPrefix + "BottomRightY", "SRW position", std::to_string(window->m_Area.bottom));
		save(windowPrefix + "OffsetX", "SRW offset", std::to_string(window->m_Offset.x));
		save(windowPrefix + "OffsetY", "SRW offset", std::to_string(window->m_Offset.y));
		save(windowPrefix + "Filter", "SRW filter", std::to_string(window->m_Filter));
		save(windowPrefix + "Scale", "SRW zoom", std::to_string(window->m_Scale));
		save(windowPrefix + "Rotation", "SRW legacy rotation", std::to_string(window->m_Rotation));
		save(windowPrefix + "LayoutMode", "SRW layout mode", std::to_string(static_cast<int>(window->m_AvisoLayoutMode)));
		const auto displayIt = appWindowDisplays.find(id);
		save(windowPrefix + "Display", "Display Secondary Radar Window",
			displayIt != appWindowDisplays.end() && displayIt->second ? "1" : "0");
	}

	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const auto avisoWindowIt = appWindows.find(avisoWindowId);
	if (avisoWindowIt != appWindows.end() && avisoWindowIt->second != nullptr)
	{
		CInsetWindow* window = avisoWindowIt->second.get();
		const std::string windowPrefix = "AVISO1";
		save(windowPrefix + "TopLeftX", "AVISO viewport position", std::to_string(window->m_Area.left));
		save(windowPrefix + "TopLeftY", "AVISO viewport position", std::to_string(window->m_Area.top));
		save(windowPrefix + "BottomRightX", "AVISO viewport position", std::to_string(window->m_Area.right));
		save(windowPrefix + "BottomRightY", "AVISO viewport position", std::to_string(window->m_Area.bottom));
		save(windowPrefix + "CenterLat", "AVISO viewport center", std::to_string(window->m_AvisoCenterLatitude));
		save(windowPrefix + "CenterLon", "AVISO viewport center", std::to_string(window->m_AvisoCenterLongitude));
		save(windowPrefix + "Scale", "AVISO viewport zoom", std::to_string(window->m_AvisoScale));
		save(windowPrefix + "LayoutMode", "AVISO viewport layout mode", std::to_string(static_cast<int>(window->m_AvisoLayoutMode)));
		const auto displayIt = appWindowDisplays.find(avisoWindowId);
		save(windowPrefix + "Display", "Display AVISO viewport",
			displayIt != appWindowDisplays.end() && displayIt->second ? "1" : "0");
	}

	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const auto weatherWindowIt = appWindows.find(weatherWindowId);
	if (weatherWindowIt != appWindows.end() && weatherWindowIt->second != nullptr)
	{
		CInsetWindow* window = weatherWindowIt->second.get();
		const std::string windowPrefix = "WEATHER1";
		save(windowPrefix + "TopLeftX", "Weather inset position", std::to_string(window->m_Area.left));
		save(windowPrefix + "TopLeftY", "Weather inset position", std::to_string(window->m_Area.top));
		save(windowPrefix + "BottomRightX", "Weather inset position", std::to_string(window->m_Area.right));
		save(windowPrefix + "BottomRightY", "Weather inset position", std::to_string(window->m_Area.bottom));
		save(windowPrefix + "LayoutMode", "Weather inset layout mode", std::to_string(static_cast<int>(window->m_AvisoLayoutMode)));
		const auto displayIt = appWindowDisplays.find(weatherWindowId);
		save(windowPrefix + "Display", "Display Weather inset",
			displayIt != appWindowDisplays.end() && displayIt->second ? "1" : "0");
	}

	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	const auto timerWindowIt = appWindows.find(timerWindowId);
	if (timerWindowIt != appWindows.end() && timerWindowIt->second != nullptr)
	{
		CInsetWindow* window = timerWindowIt->second.get();
		const std::string windowPrefix = "TIMER1";
		save(windowPrefix + "TopLeftX", "Timer inset position", std::to_string(window->m_Area.left));
		save(windowPrefix + "TopLeftY", "Timer inset position", std::to_string(window->m_Area.top));
		save(windowPrefix + "BottomRightX", "Timer inset position", std::to_string(window->m_Area.right));
		save(windowPrefix + "BottomRightY", "Timer inset position", std::to_string(window->m_Area.bottom));
		save(windowPrefix + "LayoutMode", "Timer inset layout mode", std::to_string(static_cast<int>(window->m_AvisoLayoutMode)));
		const auto displayIt = appWindowDisplays.find(timerWindowId);
		save(windowPrefix + "Display", "Display Timer inset",
			displayIt != appWindowDisplays.end() && displayIt->second ? "1" : "0");
	}

	save("ActivePreset", "Active airport inset preset", ActiveAvisoPresetName);
	save("Linked", "Link AVISO views", AvisoViewsLinked ? "1" : "0");
}

bool CSMRRadar::LoadInsetStateFromAsrForAirport(const std::string& airport, bool allowLegacyFallback)
{
	const std::string scopedPrefix = AirportInsetAsrPrefix(airport);
	if (scopedPrefix.empty())
		return false;
	const std::string normalizedAirport = NormalizeInsetAirport(airport);

	// An AVISO source selected earlier in this EuroScope session is
	// authoritative for later-created radar screens. Their independently saved
	// ASR values must not split the same airport across different source files.
	std::string sessionAvisoPath;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar == this)
			continue;
		const auto selectedPath = radar->AvisoGeoJsonOverridePaths.find(normalizedAirport);
		if (selectedPath == radar->AvisoGeoJsonOverridePaths.end() || selectedPath->second.empty())
			continue;

		std::error_code pathError;
		const std::filesystem::path candidate = std::filesystem::u8path(selectedPath->second);
		if (!std::filesystem::is_regular_file(candidate, pathError) || pathError)
			continue;
		sessionAvisoPath = std::filesystem::absolute(candidate, pathError).lexically_normal().u8string();
		if (pathError)
			sessionAvisoPath = selectedPath->second;
		break;
	}
	const bool hasSessionAvisoPath = !sessionAvisoPath.empty();
	auto adoptSessionAvisoPath = [&]()
	{
		if (!hasSessionAvisoPath)
			return;
		AvisoGeoJsonOverridePaths[normalizedAirport] = sessionAvisoPath;
		AvisoGeoJsonResolvedAirport.clear();
		AvisoGeoJsonResolvedDllPath.clear();
		AvisoGeoJsonResolvedPath.clear();
		AvisoGeoJsonLastStatTick = 0;
		const std::string key = scopedPrefix + "AvisoFile";
		SaveDataToAsr(
			key.c_str(),
			"Airport-specific AVISO source file",
			sessionAvisoPath.c_str());
	};

	auto readWithPrefix = [&](const std::string& prefix, const std::string& suffix) -> const char*
	{
		const std::string key = prefix + suffix;
		return GetDataFromAsr(key.c_str());
	};

	// ----- Locating airport-scoped or legacy state -----
	bool hasScopedState = readWithPrefix(scopedPrefix, "Version") != nullptr;
	if (!hasScopedState)
	{
		for (const char* probe : { "AvisoFile", "SRW1Display", "AVISO1Display", "WEATHER1Display", "TIMER1Display", "SRW1TopLeftX", "AVISO1TopLeftX", "WEATHER1TopLeftX", "TIMER1TopLeftX" })
		{
			if (readWithPrefix(scopedPrefix, probe) != nullptr)
			{
				hasScopedState = true;
				break;
			}
		}
	}

	std::string readPrefix;
	if (hasScopedState)
		readPrefix = scopedPrefix;
	else if (allowLegacyFallback)
	{
		for (const char* probe : { "AvisoFile", "SRW1Display", "AVISO1Display", "WEATHER1Display", "TIMER1Display", "SRW1TopLeftX", "AVISO1TopLeftX", "WEATHER1TopLeftX", "TIMER1TopLeftX" })
		{
			if (GetDataFromAsr(probe) != nullptr)
			{
				readPrefix.clear();
				hasScopedState = true;
				break;
			}
		}
	}

	if (!hasScopedState)
	{
		adoptSessionAvisoPath();
		return false;
	}

	auto read = [&](const std::string& suffix) -> const char*
	{
		return readWithPrefix(readPrefix, suffix);
	};
	auto readInt = [&](const std::string& suffix, int minimum, int maximum, auto& target)
	{
		if (const char* value = read(suffix))
		{
			int parsed = 0;
			if (ParseAsrInt(value, minimum, maximum, parsed))
				target = parsed;
			else
				Logger::info("Ignored invalid inset ASR integer: " + readPrefix + suffix);
		}
	};
	auto readDouble = [&](const std::string& suffix, double minimum, double maximum, double& target) -> bool
	{
		if (const char* value = read(suffix))
		{
			double parsed = 0.0;
			if (ParseAsrDouble(value, minimum, maximum, parsed))
			{
				target = parsed;
				return true;
			}
			Logger::info("Ignored invalid inset ASR number: " + readPrefix + suffix);
		}
		return false;
	};
	auto readDisplay = [&](const std::string& suffix, bool& target)
	{
		if (const char* value = read(suffix))
		{
			int parsed = 0;
			if (ParseAsrInt(value, 0, 1, parsed))
				target = parsed != 0;
			else
				Logger::info("Ignored invalid inset ASR visibility: " + readPrefix + suffix);
		}
	};
	auto readRect = [&](const std::string& windowPrefix, RECT& target)
	{
		const char* raw[4] = {
			read(windowPrefix + "TopLeftX"),
			read(windowPrefix + "TopLeftY"),
			read(windowPrefix + "BottomRightX"),
			read(windowPrefix + "BottomRightY")
		};
		const bool any = std::any_of(std::begin(raw), std::end(raw), [](const char* value) {
			return value != nullptr;
		});
		if (!any)
			return;
		int values[4] = {};
		for (int index = 0; index < 4; ++index)
		{
			if (!ParseAsrInt(raw[index], -100000, 100000, values[index]))
			{
				Logger::info("Ignored incomplete or invalid inset ASR rectangle: " + readPrefix + windowPrefix);
				return;
			}
		}
		if (values[2] <= values[0] || values[3] <= values[1] ||
			values[2] - values[0] > 20000 || values[3] - values[1] > 20000)
		{
			Logger::info("Ignored invalid inset ASR rectangle dimensions: " + readPrefix + windowPrefix);
			return;
		}
		target.left = values[0];
		target.top = values[1];
		target.right = values[2];
		target.bottom = values[3];
	};

	// ----- Validating the stored schema version -----
	if (!readPrefix.empty())
	{
		int version = 0;
		const char* rawVersion = read("Version");
		if (rawVersion != nullptr && !ParseAsrInt(rawVersion, 1, 3, version))
		{
			errno = 0;
			char* versionEnd = nullptr;
			const unsigned long long numericVersion = std::strtoull(rawVersion, &versionEnd, 10);
			const bool isFutureVersion =
				rawVersion[0] != '-' &&
				versionEnd != rawVersion &&
				*versionEnd == '\0' &&
				(errno == ERANGE || numericVersion > 3);
			if (isFutureVersion)
			{
				UnsupportedInsetAsrStateAirports.insert(normalizedAirport);
				Logger::info(
					"Preserved unsupported future airport inset ASR state version for " +
					normalizedAirport);
				return false;
			}

			UnsupportedInsetAsrStateAirports.erase(normalizedAirport);
			Logger::info("Ignored invalid airport inset ASR state version for " + normalizedAirport);
			return false;
		}
		UnsupportedInsetAsrStateAirports.erase(normalizedAirport);
	}
	else
	{
		UnsupportedInsetAsrStateAirports.erase(normalizedAirport);
	}

	if (hasSessionAvisoPath)
	{
		adoptSessionAvisoPath();
	}
	else if (const char* selectedAvisoPath = read("AvisoFile"))
	{
		std::filesystem::path sourcePath;
		if (selectedAvisoPath[0] != '\0' &&
			VsmrWindowsPath::TryResolveExistingFile(selectedAvisoPath, sourcePath))
		{
			AvisoGeoJsonOverridePaths[normalizedAirport] =
				sourcePath.u8string();
		}
		else
		{
			AvisoGeoJsonOverridePaths.erase(normalizedAirport);
			GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"AVISO source",
				("The saved AVISO source for " + normalizedAirport +
					" is unavailable. vSMR will use the packaged airport AVISO when available.").c_str(),
				true, true, false, false, false);
		}
	}

	// ----- Restoring inset windows -----
	for (const int id : { 1 })
	{
		const auto windowIt = appWindows.find(id);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		CInsetWindow* window = windowIt->second.get();
		const std::string windowPrefix = "SRW" + std::to_string(id);
		readRect(windowPrefix, window->m_Area);
		readInt(windowPrefix + "OffsetX", -100000, 100000, window->m_Offset.x);
		readInt(windowPrefix + "OffsetY", -100000, 100000, window->m_Offset.y);
		readInt(windowPrefix + "Filter", 0, 66000, window->m_Filter);
		readInt(windowPrefix + "Scale", 1, 2400, window->m_Scale);
		readDouble(windowPrefix + "Rotation", -36000.0, 36000.0, window->m_Rotation);
		int layoutMode = static_cast<int>(window->m_AvisoLayoutMode);
		readInt(windowPrefix + "LayoutMode", 0, 8, layoutMode);
		window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(layoutMode);
		bool visible = appWindowDisplays[id];
		readDisplay(windowPrefix + "Display", visible);
		appWindowDisplays[id] = visible;
		window->ResetAvisoInteractionState();
	}

	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const auto avisoWindowIt = appWindows.find(avisoWindowId);
	if (avisoWindowIt != appWindows.end() && avisoWindowIt->second != nullptr)
	{
		CInsetWindow* window = avisoWindowIt->second.get();
		const std::string windowPrefix = "AVISO1";
		bool centerLoaded = false;
		readRect(windowPrefix, window->m_Area);
		const bool latitudeLoaded = readDouble(
			windowPrefix + "CenterLat", -85.0, 85.0, window->m_AvisoCenterLatitude);
		const bool longitudeLoaded = readDouble(
			windowPrefix + "CenterLon", -180.0, 180.0, window->m_AvisoCenterLongitude);
		centerLoaded = latitudeLoaded && longitudeLoaded;
		window->m_AvisoViewInitialized = centerLoaded;
		readInt(windowPrefix + "Scale", 1, 2400, window->m_AvisoScale);
		int layoutMode = static_cast<int>(window->m_AvisoLayoutMode);
		readInt(windowPrefix + "LayoutMode", 0, 8, layoutMode);
		window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(layoutMode);
		bool visible = appWindowDisplays[avisoWindowId];
		readDisplay(windowPrefix + "Display", visible);
		appWindowDisplays[avisoWindowId] = visible;
		window->ResetAvisoInteractionState();
		window->ClearAvisoViewportCache();
	}

	const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
	const auto weatherWindowIt = appWindows.find(weatherWindowId);
	if (weatherWindowIt != appWindows.end() && weatherWindowIt->second != nullptr)
	{
		CInsetWindow* window = weatherWindowIt->second.get();
		const std::string windowPrefix = "WEATHER1";
		readRect(windowPrefix, window->m_Area);
		int layoutMode = static_cast<int>(window->m_AvisoLayoutMode);
		readInt(windowPrefix + "LayoutMode", 0, 8, layoutMode);
		window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(layoutMode);
		bool visible = appWindowDisplays[weatherWindowId];
		readDisplay(windowPrefix + "Display", visible);
		appWindowDisplays[weatherWindowId] = visible;
		window->ResetAvisoInteractionState();
	}

	const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
	const auto timerWindowIt = appWindows.find(timerWindowId);
	if (timerWindowIt != appWindows.end() && timerWindowIt->second != nullptr)
	{
		const std::string windowPrefix = "TIMER1";
		const bool hasTimerState =
			read(windowPrefix + "Display") != nullptr ||
			read(windowPrefix + "TopLeftX") != nullptr;
		if (!hasTimerState)
		{
			// Older airport-scoped state predates the Timer. Reset its window
			// instead of carrying another airport's position or visibility over.
			ResetInsetWindowState(timerWindowId, false);
		}
		else
		{
			CInsetWindow* window = timerWindowIt->second.get();
			readRect(windowPrefix, window->m_Area);
			int layoutMode = static_cast<int>(CInsetWindow::AvisoLayoutMode::Floating);
			readInt(windowPrefix + "LayoutMode", 0, 8, layoutMode);
			window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(layoutMode);
			bool visible = appWindowDisplays[timerWindowId];
			readDisplay(windowPrefix + "Display", visible);
			appWindowDisplays[timerWindowId] = visible;
			window->ResetAvisoInteractionState();
		}
	}

	// ----- Restoring preset state -----
	ActiveAvisoPresetName.clear();
	AvisoViewsLinked = false;
	if (!readPrefix.empty())
	{
		if (const char* activePreset = read("ActivePreset"))
		{
			for (const AvisoPreset& preset : GetAvisoPresets())
			{
				if (_stricmp(preset.name.c_str(), activePreset) == 0)
				{
					ActiveAvisoPresetName = preset.name;
					break;
				}
			}
		}
		if (const char* linked = read("Linked"))
		{
			int parsed = 0;
			AvisoViewsLinked =
				!ActiveAvisoPresetName.empty() &&
				ParseAsrInt(linked, 0, 1, parsed) && parsed != 0;
		}
	}

	return true;
}

string CSMRRadar::setActiveAirport(
	string value,
	bool switchInsetContext,
	bool syncControlCenter)
{
	const std::string airport = NormalizeInsetAirport(value);
	if (airport.empty() || _stricmp(ActiveAirport.c_str(), airport.c_str()) == 0)
		return ActiveAirport;

	if (switchInsetContext && !ActiveAirport.empty())
	{
		SaveInsetStateToAsrForAirport(ActiveAirport);
	}

	ActiveAirport = airport;
	MarkPerformanceRefreshReason(
		VsmrPerformance::FrameRefreshReason::AirportUpdate);
	PublishCrashRadarState("main");
	InvalidateRunwayGeometryCache();
	LastMapActiveAirport.clear();
	RunwayStatusLastRefreshTick = 0;
	RunwayStatusLastAirport.clear();
	ClearAvisoGeoJsonRasterCache();
	AvisoGeoJsonLastViewValid = false;
	RefreshLegacyRimcasRunwayMonitoring();

	if (switchInsetContext)
	{
		ResetAllInsetWindowStates(false);
		ResetAvisoPresetStateForActiveAirport(false);
		if (!LoadInsetStateFromAsrForAirport(ActiveAirport, false))
			ApplyDefaultAvisoPresetIfConfigured();
		SaveDataToAsr("Airport", "Active airport", ActiveAirport.c_str());
		if (syncControlCenter && VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar();
	}
	// ASR loading restores its airport-scoped AVISO override later in the same
	// callback, so defer that prewarm to OnAsrContentLoaded's final state.
	if (switchInsetContext)
		PrewarmAvisoForActiveAirport();

	return ActiveAirport;
}

void CSMRRadar::OnAsrContentLoaded(bool Loaded)
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnAsrContentLoaded",
		reinterpret_cast<std::uintptr_t>(this));
	(void)Loaded;
	Logger::info(string(__FUNCSIG__));
	const char * p_value;

	// ReSharper disable CppZeroConstantCanBeReplacedWithNullptr
	if ((p_value = GetDataFromAsr("Airport")) != NULL)
	{
		setActiveAirport(p_value, false);
		Logger::info("OnAsrContentLoaded: active airport from ASR=" + getActiveAirport());
	}
	else
	{
		Logger::info("OnAsrContentLoaded: no ASR Airport value; active airport=" + getActiveAirport());
	}

	const char* savedProfilesPath = GetDataFromAsr("ProfilesFile");
	if (savedProfilesPath != NULL && savedProfilesPath[0] != '\0')
	{
		const std::string requestedProfilesPath(savedProfilesPath);
		std::string profilePathError;
		if (!SetProfilesConfigPath(requestedProfilesPath, &profilePathError, false))
		{
			Logger::info(
				"OnAsrContentLoaded: profiles source is unavailable path=" +
				requestedProfilesPath + " error=" + profilePathError);
			GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"Profiles source",
				("The saved profiles source is unavailable. vSMR kept the current safe source. " +
					profilePathError).c_str(),
				true, true, false, false, false);
		}
	}
	else if (!ConfigPath.empty())
	{
		// Missing legacy keys do not claim the session source. The constructor
		// has already adopted any claimed session path, so merely persist the
		// effective value for this ASR's next load.
		SaveDataToAsr(
			"ProfilesFile",
			"Active vSMR profiles file",
			ConfigPath.c_str());
	}

	std::string loadedProfileName;
	const std::string persistedProfile = ReadLastActiveProfileFromConfig();
	if (!persistedProfile.empty())
	{
		this->LoadProfile(persistedProfile);
		LoadCustomFont();
		loadedProfileName = CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : persistedProfile;
	}
	else if ((p_value = GetDataFromAsr("ActiveProfile")) != NULL)
	{
		this->LoadProfile(string(p_value));
		LoadCustomFont();
		loadedProfileName = CurrentConfig != nullptr ? CurrentConfig->getActiveProfileName() : std::string(p_value);
	}
	else if (CurrentConfig != nullptr)
	{
		loadedProfileName = CurrentConfig->getActiveProfileName();
	}

	if (!loadedProfileName.empty())
	{
		RememberSessionActiveProfile(loadedProfileName);
		WriteLastActiveProfileToConfig(loadedProfileName);
		SaveDataToAsr("ActiveProfile", "vSMR active profile", loadedProfileName.c_str());
	}

	// Label font size is persisted per profile in vSMR_Profiles.json.
	// Keep ASR value untouched to avoid overriding the active profile setting.

	ShowFps = true;
	if ((p_value = GetDataFromAsr("ShowFps")) != NULL)
		ShowFps = atoi(p_value) != 0;

	AvisoUseDayColorPalette = false;
	if ((p_value = GetDataFromAsr("AvisoColorPalette")) != NULL)
		SetAvisoColorPalette(p_value, false);

	LoadRuntimeMenuPositionFromAsr();

	ResetAllInsetWindowStates(false);
	ResetAvisoPresetStateForActiveAirport(false);
	if (!LoadInsetStateFromAsrForAirport(getActiveAirport(), true))
		ApplyDefaultAvisoPresetIfConfigured();
	SaveInsetStateToAsrForAirport(getActiveAirport());
	InitialInsetStateRestorePending = true;
	InitialInsetStateRestoreBounds.SetRectEmpty();
	InitialInsetStateRestoreStableFrames = 0;
	InitialInsetStateRestoreBoundsChangedTick = ::GetTickCount();
	AvisoGeoJsonScrollSelected = false;
	for (auto& appWindow : appWindows)
	{
		CInsetWindow* insetWindow = appWindow.second.get();
		if (insetWindow == nullptr)
			continue;

		insetWindow->ResetAvisoInteractionState();
	}

	// Auto-detect active sector runways when no runway rows are configured.
	RefreshLegacyRimcasRunwayMonitoring();
	PrewarmAvisoForActiveAirport();
	PublishCrashRadarState("main");

	// Create the hidden Control Center as soon as the radar/ASR state is ready.
	// WebView2 and the initial authoritative payload then load in the background,
	// so the first user-open only has to reveal the already initialized window.
	if (!EnsureVsmrControlCenterWindowCreated())
		Logger::info("Control Center preload deferred until its first open.");

	// ReSharper restore CppZeroConstantCanBeReplacedWithNullptr
}

void CSMRRadar::OnAsrContentToBeSaved()
{
	VsmrCrashRuntime::RecordEuroScopeCallback(
		"CSMRRadar::OnAsrContentToBeSaved",
		reinterpret_cast<std::uintptr_t>(this));
	Logger::info(string(__FUNCSIG__));

	SaveDataToAsr("Airport", "Active airport for RIMCAS", getActiveAirport().c_str());

	const std::string activeProfileFallback = (CurrentConfig != nullptr) ? CurrentConfig->getActiveProfileName() : "Default";
	const std::string activeProfileToPersist = GetSessionActiveProfile(activeProfileFallback);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfileToPersist.c_str());
	WriteLastActiveProfileToConfig(activeProfileToPersist);
	SaveDataToAsr("ProfilesFile", "Active vSMR profiles file", ConfigPath.c_str());

	SaveDataToAsr("FontSize", "vSMR font size", std::to_string(currentFontSize).c_str());

	SaveDataToAsr("ShowFps", "Show FPS counter", ShowFps ? "1" : "0");
	SaveDataToAsr(
		"AvisoColorPalette",
		"AVISO day/night color palette",
		GetAvisoColorPalette().c_str());

	SaveRuntimeMenuPositionToAsr();
	if (!InitialInsetStateRestorePending)
		SaveInsetStateToAsrForAirport(getActiveAirport());
}



