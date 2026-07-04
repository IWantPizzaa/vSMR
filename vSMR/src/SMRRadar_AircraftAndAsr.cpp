#include "stdafx.h"
#include "SMRRadar.hpp"
#include "InsetWindow.h"

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

	// AppData\Roaming\EuroScope\LFXX\Plugins (priority)
	char* appdata = nullptr;
	size_t appdata_len = 0;
	if (_dupenv_s(&appdata, &appdata_len, "APPDATA") == 0 && appdata != nullptr) {
		fs::path roaming(appdata);
		candidates.push_back(roaming / "EuroScope" / "LFXX" / "Plugins" / "ICAO_Aircraft.json");
		free(appdata);
	}

	// Plugin folder and parent
	candidates.push_back(fs::path(DllPath) / "ICAO_Aircraft.json");
	candidates.push_back(fs::path(DllPath).parent_path() / "ICAO_Aircraft.json");

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


void CSMRRadar::OnAsrContentLoaded(bool Loaded)
{
	Logger::info(string(__FUNCSIG__));
	const char * p_value;

	// ReSharper disable CppZeroConstantCanBeReplacedWithNullptr
	if ((p_value = GetDataFromAsr("Airport")) != NULL)
	{
		setActiveAirport(p_value);
		Logger::info("OnAsrContentLoaded: active airport from ASR=" + getActiveAirport());
	}
	else
	{
		Logger::info("OnAsrContentLoaded: no ASR Airport value; active airport=" + getActiveAirport());
	}

	std::string loadedProfileName;
	const std::string persistedProfile = ReadLastActiveProfileFromDisk();
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
		WriteLastActiveProfileToDisk(loadedProfileName);
		SaveDataToAsr("ActiveProfile", "vSMR active profile", loadedProfileName.c_str());
	}

	// Label font size is persisted per profile in vSMR_Profiles.json.
	// Keep ASR value untouched to avoid overriding the active profile setting.

	if ((p_value = GetDataFromAsr("Afterglow")) != NULL)
		Afterglow = atoi(p_value) == 1 ? true : false;

	if ((p_value = GetDataFromAsr("AppTrailsDots")) != NULL)
		Trail_App = atoi(p_value);

	if ((p_value = GetDataFromAsr("GndTrailsDots")) != NULL)
		Trail_Gnd = atoi(p_value);

	if ((p_value = GetDataFromAsr("PredictedLine")) != NULL)
		PredictedLength = atoi(p_value);

	string temp;

	for (int i = 1; i < 3; i++)
	{
		string prefix = "SRW" + std::to_string(i);

		if ((p_value = GetDataFromAsr(string(prefix + "TopLeftX").c_str())) != NULL)
			appWindows[i]->m_Area.left = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "TopLeftY").c_str())) != NULL)
			appWindows[i]->m_Area.top = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "BottomRightX").c_str())) != NULL)
			appWindows[i]->m_Area.right = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "BottomRightY").c_str())) != NULL)
			appWindows[i]->m_Area.bottom = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "OffsetX").c_str())) != NULL)
			appWindows[i]->m_Offset.x = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "OffsetY").c_str())) != NULL)
			appWindows[i]->m_Offset.y = atoi(p_value);


		if ((p_value = GetDataFromAsr(string(prefix + "Filter").c_str())) != NULL)
			appWindows[i]->m_Filter = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "Scale").c_str())) != NULL)
			appWindows[i]->m_Scale = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "Rotation").c_str())) != NULL)
			appWindows[i]->m_Rotation = atoi(p_value);

		if ((p_value = GetDataFromAsr(string(prefix + "Display").c_str())) != NULL)
			appWindowDisplays[i] = atoi(p_value) == 1 ? true : false;
	}

	{
		const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
		auto avisoWindowIt = appWindows.find(avisoWindowId);
		if (avisoWindowIt != appWindows.end() && avisoWindowIt->second != nullptr)
		{
			CInsetWindow* avisoWindow = avisoWindowIt->second.get();
			const string prefix = "AVISO1";

			if ((p_value = GetDataFromAsr(string(prefix + "TopLeftX").c_str())) != NULL)
				avisoWindow->m_Area.left = atoi(p_value);

			if ((p_value = GetDataFromAsr(string(prefix + "TopLeftY").c_str())) != NULL)
				avisoWindow->m_Area.top = atoi(p_value);

			if ((p_value = GetDataFromAsr(string(prefix + "BottomRightX").c_str())) != NULL)
				avisoWindow->m_Area.right = atoi(p_value);

			if ((p_value = GetDataFromAsr(string(prefix + "BottomRightY").c_str())) != NULL)
				avisoWindow->m_Area.bottom = atoi(p_value);

			if ((p_value = GetDataFromAsr(string(prefix + "CenterLat").c_str())) != NULL)
			{
				avisoWindow->m_AvisoCenterLatitude = atof(p_value);
				avisoWindow->m_AvisoViewInitialized = true;
			}

			if ((p_value = GetDataFromAsr(string(prefix + "CenterLon").c_str())) != NULL)
			{
				avisoWindow->m_AvisoCenterLongitude = atof(p_value);
				avisoWindow->m_AvisoViewInitialized = true;
			}

			if ((p_value = GetDataFromAsr(string(prefix + "Scale").c_str())) != NULL)
				avisoWindow->m_AvisoScale = max(1, atoi(p_value));

			if ((p_value = GetDataFromAsr(string(prefix + "Display").c_str())) != NULL)
				appWindowDisplays[avisoWindowId] = atoi(p_value) == 1 ? true : false;
		}
	}

	// Auto load the airport config on ASR opened.
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

	// ReSharper restore CppZeroConstantCanBeReplacedWithNullptr
}

void CSMRRadar::OnAsrContentToBeSaved()
{
	Logger::info(string(__FUNCSIG__));

	SaveDataToAsr("Airport", "Active airport for RIMCAS", getActiveAirport().c_str());

	const std::string activeProfileFallback = (CurrentConfig != nullptr) ? CurrentConfig->getActiveProfileName() : "Default";
	const std::string activeProfileToPersist = GetSessionActiveProfile(activeProfileFallback);
	SaveDataToAsr("ActiveProfile", "vSMR active profile", activeProfileToPersist.c_str());
	WriteLastActiveProfileToDisk(activeProfileToPersist);

	SaveDataToAsr("FontSize", "vSMR font size", std::to_string(currentFontSize).c_str());

	SaveDataToAsr("Afterglow", "vSMR Afterglow enabled", std::to_string(int(Afterglow)).c_str());

	SaveDataToAsr("AppTrailsDots", "vSMR APPR Trail Dots", std::to_string(Trail_App).c_str());

	SaveDataToAsr("GndTrailsDots", "vSMR GRND Trail Dots", std::to_string(Trail_Gnd).c_str());

	SaveDataToAsr("PredictedLine", "vSMR Predicted Track Lines", std::to_string(PredictedLength).c_str());

	string temp = "";

	for (int i = 1; i < 3; i++)
	{
		string prefix = "SRW" + std::to_string(i);

		temp = std::to_string(appWindows[i]->m_Area.left);
		SaveDataToAsr(string(prefix + "TopLeftX").c_str(), "SRW position", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Area.top);
		SaveDataToAsr(string(prefix + "TopLeftY").c_str(), "SRW position", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Area.right);
		SaveDataToAsr(string(prefix + "BottomRightX").c_str(), "SRW position", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Area.bottom);
		SaveDataToAsr(string(prefix + "BottomRightY").c_str(), "SRW position", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Offset.x);
		SaveDataToAsr(string(prefix + "OffsetX").c_str(), "SRW offset", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Offset.y);
		SaveDataToAsr(string(prefix + "OffsetY").c_str(), "SRW offset", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Filter);
		SaveDataToAsr(string(prefix + "Filter").c_str(), "SRW filter", temp.c_str());

		temp = std::to_string(appWindows[i]->m_Scale);
		SaveDataToAsr(string(prefix + "Scale").c_str(), "SRW range", temp.c_str());

		temp = std::to_string((int)appWindows[i]->m_Rotation);
		SaveDataToAsr(string(prefix + "Rotation").c_str(), "SRW rotation", temp.c_str());

		string to_save = "0";
		if (appWindowDisplays[i])
			to_save = "1";
		SaveDataToAsr(string(prefix + "Display").c_str(), "Display Secondary Radar Window", to_save.c_str());
	}	

	{
		const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
		auto avisoWindowIt = appWindows.find(avisoWindowId);
		if (avisoWindowIt != appWindows.end() && avisoWindowIt->second != nullptr)
		{
			CInsetWindow* avisoWindow = avisoWindowIt->second.get();
			const string prefix = "AVISO1";

			temp = std::to_string(avisoWindow->m_Area.left);
			SaveDataToAsr(string(prefix + "TopLeftX").c_str(), "AVISO viewport position", temp.c_str());

			temp = std::to_string(avisoWindow->m_Area.top);
			SaveDataToAsr(string(prefix + "TopLeftY").c_str(), "AVISO viewport position", temp.c_str());

			temp = std::to_string(avisoWindow->m_Area.right);
			SaveDataToAsr(string(prefix + "BottomRightX").c_str(), "AVISO viewport position", temp.c_str());

			temp = std::to_string(avisoWindow->m_Area.bottom);
			SaveDataToAsr(string(prefix + "BottomRightY").c_str(), "AVISO viewport position", temp.c_str());

			temp = std::to_string(avisoWindow->m_AvisoCenterLatitude);
			SaveDataToAsr(string(prefix + "CenterLat").c_str(), "AVISO viewport center", temp.c_str());

			temp = std::to_string(avisoWindow->m_AvisoCenterLongitude);
			SaveDataToAsr(string(prefix + "CenterLon").c_str(), "AVISO viewport center", temp.c_str());

			temp = std::to_string(avisoWindow->m_AvisoScale);
			SaveDataToAsr(string(prefix + "Scale").c_str(), "AVISO viewport zoom", temp.c_str());

			string toSave = "0";
			if (appWindowDisplays[avisoWindowId])
				toSave = "1";
			SaveDataToAsr(string(prefix + "Display").c_str(), "Display AVISO viewport", toSave.c_str());
		}
	}
}



