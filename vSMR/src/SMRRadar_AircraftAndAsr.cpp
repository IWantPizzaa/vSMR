#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"
#include "VsmrControlCenterDialog.hpp"

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
}

Gdiplus::Bitmap* CSMRRadar::GetAircraftIcon(const std::string& acTypeRaw)
{
	std::string ac = acTypeRaw;
	if (ac.empty())
		return nullptr;
	std::transform(ac.begin(), ac.end(), ac.begin(), ::tolower);

	auto it = AircraftIcons.find(ac);
	if (it != AircraftIcons.end())
		return it->second.get();

	const fs::path candidate = fs::path(IconsPath) / (ac + ".png");
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
		cachedBitmap->second.lastUsedFrame = cacheFrame;
		return cachedBitmap->second.bitmap.get();
	}

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
		cachedBitmap->second.lastUsedFrame = cacheFrame;
		return &cachedBitmap->second;
	}

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
		pushUniqueCandidate(fs::path(DataPath) / "ICAO_Aircraft.json");
	pushUniqueCandidate(fs::path(DllPath) / "vSMR_Data" / "ICAO_Aircraft.json");

	// Legacy flat AppData\Roaming\EuroScope\LFXX\Plugins fallback
	char* appdata = nullptr;
	size_t appdata_len = 0;
	if (_dupenv_s(&appdata, &appdata_len, "APPDATA") == 0 && appdata != nullptr) {
		fs::path roaming(appdata);
		pushUniqueCandidate(roaming / "EuroScope" / "LFXX" / "Plugins" / "ICAO_Aircraft.json");
		free(appdata);
	}

	// Plugin folder and parent
	pushUniqueCandidate(fs::path(DllPath) / "ICAO_Aircraft.json");
	pushUniqueCandidate(fs::path(DllPath).parent_path() / "ICAO_Aircraft.json");

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
		Logger::info("Trying to read aircraft specs from: " + p.string());
		if (!fs::exists(p)) {
			Logger::info("Specs file not found at: " + p.string());
			continue;
		}

		std::ifstream ifs(p.string(), std::ios::binary);
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
			Logger::info("Parse error in ICAO_Aircraft.json at: " + p.string());
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
			Logger::info("ICAO_Aircraft.json has unexpected root type at: " + p.string());
		}

		Logger::info("Loaded " + std::to_string(loaded) + " aircraft specs from " + p.string());
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
	else
	{
		window->m_Area = appWindowId == 2
			? RECT{ 240, 240, 640, 540 }
			: RECT{ 200, 200, 600, 500 };
		window->m_Scale = 15;
		window->m_Filter = 5500;
		window->m_Rotation = 0.0;
	}

	appWindowDisplays[appWindowId] = preserveVisibility ? wasVisible : false;
}

void CSMRRadar::ResetAllInsetWindowStates(bool preserveVisibility)
{
	ResetInsetWindowState(1, preserveVisibility);
	ResetInsetWindowState(2, preserveVisibility);
	ResetInsetWindowState(APPWINDOW_AVISO - APPWINDOW_BASE, preserveVisibility);
}

void CSMRRadar::SaveInsetStateToAsrForAirport(const std::string& airport)
{
	const std::string prefix = AirportInsetAsrPrefix(airport);
	if (prefix.empty())
		return;

	auto save = [&](const std::string& suffix, const char* description, const std::string& value)
	{
		const std::string key = prefix + suffix;
		SaveDataToAsr(key.c_str(), description, value.c_str());
	};

	save("Version", "Airport-specific inset state version", "1");
	for (int id = 1; id <= 2; ++id)
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

	save("ActivePreset", "Active airport inset preset", ActiveAvisoPresetName);
	save("Linked", "Link AVISO views", AvisoViewsLinked ? "1" : "0");
}

bool CSMRRadar::LoadInsetStateFromAsrForAirport(const std::string& airport, bool allowLegacyFallback)
{
	const std::string scopedPrefix = AirportInsetAsrPrefix(airport);
	if (scopedPrefix.empty())
		return false;

	auto readWithPrefix = [&](const std::string& prefix, const std::string& suffix) -> const char*
	{
		const std::string key = prefix + suffix;
		return GetDataFromAsr(key.c_str());
	};

	bool hasScopedState = readWithPrefix(scopedPrefix, "Version") != nullptr;
	if (!hasScopedState)
	{
		for (const char* probe : { "SRW1Display", "SRW2Display", "AVISO1Display", "SRW1TopLeftX", "AVISO1TopLeftX" })
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
		for (const char* probe : { "SRW1Display", "SRW2Display", "AVISO1Display", "SRW1TopLeftX", "AVISO1TopLeftX" })
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
		return false;

	auto read = [&](const std::string& suffix) -> const char*
	{
		return readWithPrefix(readPrefix, suffix);
	};

	for (int id = 1; id <= 2; ++id)
	{
		const auto windowIt = appWindows.find(id);
		if (windowIt == appWindows.end() || windowIt->second == nullptr)
			continue;

		CInsetWindow* window = windowIt->second.get();
		const std::string windowPrefix = "SRW" + std::to_string(id);
		const char* value = nullptr;
		if ((value = read(windowPrefix + "TopLeftX")) != nullptr) window->m_Area.left = atoi(value);
		if ((value = read(windowPrefix + "TopLeftY")) != nullptr) window->m_Area.top = atoi(value);
		if ((value = read(windowPrefix + "BottomRightX")) != nullptr) window->m_Area.right = atoi(value);
		if ((value = read(windowPrefix + "BottomRightY")) != nullptr) window->m_Area.bottom = atoi(value);
		if ((value = read(windowPrefix + "OffsetX")) != nullptr) window->m_Offset.x = atoi(value);
		if ((value = read(windowPrefix + "OffsetY")) != nullptr) window->m_Offset.y = atoi(value);
		if ((value = read(windowPrefix + "Filter")) != nullptr) window->m_Filter = std::clamp(atoi(value), 0, 66000);
		if ((value = read(windowPrefix + "Scale")) != nullptr) window->m_Scale = std::clamp(atoi(value), 1, 2400);
		if ((value = read(windowPrefix + "Rotation")) != nullptr) window->m_Rotation = atof(value);
		if ((value = read(windowPrefix + "LayoutMode")) != nullptr)
			window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(std::clamp(atoi(value), 0, 8));
		if ((value = read(windowPrefix + "Display")) != nullptr) appWindowDisplays[id] = atoi(value) != 0;
		window->ResetAvisoInteractionState();
	}

	const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
	const auto avisoWindowIt = appWindows.find(avisoWindowId);
	if (avisoWindowIt != appWindows.end() && avisoWindowIt->second != nullptr)
	{
		CInsetWindow* window = avisoWindowIt->second.get();
		const std::string windowPrefix = "AVISO1";
		const char* value = nullptr;
		bool centerLoaded = false;
		if ((value = read(windowPrefix + "TopLeftX")) != nullptr) window->m_Area.left = atoi(value);
		if ((value = read(windowPrefix + "TopLeftY")) != nullptr) window->m_Area.top = atoi(value);
		if ((value = read(windowPrefix + "BottomRightX")) != nullptr) window->m_Area.right = atoi(value);
		if ((value = read(windowPrefix + "BottomRightY")) != nullptr) window->m_Area.bottom = atoi(value);
		if ((value = read(windowPrefix + "CenterLat")) != nullptr)
		{
			window->m_AvisoCenterLatitude = std::clamp(atof(value), -85.0, 85.0);
			centerLoaded = true;
		}
		if ((value = read(windowPrefix + "CenterLon")) != nullptr)
		{
			window->m_AvisoCenterLongitude = atof(value);
			centerLoaded = true;
		}
		window->m_AvisoViewInitialized = centerLoaded;
		if ((value = read(windowPrefix + "Scale")) != nullptr) window->m_AvisoScale = std::clamp(atoi(value), 1, 2400);
		if ((value = read(windowPrefix + "LayoutMode")) != nullptr)
			window->m_AvisoLayoutMode = static_cast<CInsetWindow::AvisoLayoutMode>(std::clamp(atoi(value), 0, 8));
		if ((value = read(windowPrefix + "Display")) != nullptr) appWindowDisplays[avisoWindowId] = atoi(value) != 0;
		window->ResetAvisoInteractionState();
		window->ClearAvisoViewportCache();
	}

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
			AvisoViewsLinked = !ActiveAvisoPresetName.empty() && atoi(linked) != 0;
	}

	return true;
}

string CSMRRadar::setActiveAirport(string value, bool switchInsetContext)
{
	const std::string airport = NormalizeInsetAirport(value);
	if (airport.empty() || _stricmp(ActiveAirport.c_str(), airport.c_str()) == 0)
		return ActiveAirport;

	if (switchInsetContext && !ActiveAirport.empty())
		SaveInsetStateToAsrForAirport(ActiveAirport);

	ActiveAirport = airport;
	InvalidateRunwayGeometryCache();
	LastMapActiveAirport.clear();
	RunwayStatusLastRefreshTick = 0;
	RunwayStatusLastAirport.clear();
	ClearAvisoGeoJsonRasterCache();
	AvisoGeoJsonLastViewValid = false;

	if (switchInsetContext)
	{
		ResetAllInsetWindowStates(false);
		ResetAvisoPresetStateForActiveProfile(false);
		if (!LoadInsetStateFromAsrForAirport(ActiveAirport, false))
			ApplyDefaultAvisoPresetIfConfigured();
		SaveDataToAsr("Airport", "Active airport", ActiveAirport.c_str());
		if (VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar();
	}

	return ActiveAirport;
}

void CSMRRadar::OnAsrContentLoaded(bool Loaded)
{
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

	LoadRuntimeMenuPositionFromAsr();

	ResetAllInsetWindowStates(false);
	ResetAvisoPresetStateForActiveProfile(false);
	if (!LoadInsetStateFromAsrForAirport(getActiveAirport(), true))
		ApplyDefaultAvisoPresetIfConfigured();
	SaveInsetStateToAsrForAirport(getActiveAirport());
	AvisoGeoJsonScrollSelected = false;
	for (auto& appWindow : appWindows)
	{
		CInsetWindow* insetWindow = appWindow.second.get();
		if (insetWindow == nullptr)
			continue;

		insetWindow->ResetAvisoInteractionState();
	}

	// Auto-detect active sector runways only for legacy profiles. An explicit
	// profile runway array is authoritative, including when it is empty.
	if (!RimcasRunwaysExplicitlyConfigured)
	{
		CSectorElement rwy;
		for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
			rwy.IsValid();
			rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
		{
			const char* runwayAirportName = rwy.GetAirportName();
			if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
				continue;

			const char* runwayNameA = rwy.GetRunwayName(0);
			const char* runwayNameB = rwy.GetRunwayName(1);
			if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
				continue;

			if (startsWith(getActiveAirport().c_str(), runwayAirportName)) {
				string name = string(runwayNameA) + " / " + string(runwayNameB);

				if (rwy.IsElementActive(true, 0) || rwy.IsElementActive(true, 1) || rwy.IsElementActive(false, 0) || rwy.IsElementActive(false, 1)) {
					RimcasInstance->toggleMonitoredRunwayDep(name);
					if (rwy.IsElementActive(false, 0) || rwy.IsElementActive(false, 1)) {
						RimcasInstance->toggleMonitoredRunwayArr(name);
					}
				}
			}
		}
	}

	// ReSharper restore CppZeroConstantCanBeReplacedWithNullptr
}

void CSMRRadar::OnAsrContentToBeSaved()
{
	Logger::info(string(__FUNCSIG__));

	SaveDataToAsr("Airport", "Active airport for RIMCAS", getActiveAirport().c_str());

	const std::string activeProfileFallback = (CurrentConfig != nullptr) ? CurrentConfig->getActiveProfileName() : "Default";
	const std::string activeProfileToPersist = GetSessionActiveProfile(activeProfileFallback);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfileToPersist.c_str());
	WriteLastActiveProfileToConfig(activeProfileToPersist);

	SaveDataToAsr("FontSize", "vSMR font size", std::to_string(currentFontSize).c_str());

	SaveDataToAsr("ShowFps", "Show FPS counter", ShowFps ? "1" : "0");

	SaveRuntimeMenuPositionToAsr();
	SaveInsetStateToAsrForAirport(getActiveAirport());
}



