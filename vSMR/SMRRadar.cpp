#include "stdafx.h"
#include "Resource.h"
#include "SMRRadar.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include "rapidjson/document.h"
#include "SMRRadar_TagShared.hpp"
#include "ProfileEditorDialog.hpp"

extern std::vector<CSMRRadar*> RadarScreensOpened;

ULONG_PTR m_gdiplusToken;
CPoint mouseLocation(0, 0);
string TagBeingDragged;
int LeaderLineDefaultlenght = 50;

//
// Cursor Things
//

bool initCursor = true;
HCURSOR smrCursor = NULL;
bool standardCursor; // switches between mouse cursor and pointer cursor when moving tags
bool customCursor; // use SMR version or default windows mouse symbol
WNDPROC gSourceProc;
HWND pluginWindow;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

map<string, string> CSMRRadar::vStripsStands;

map<int, CInsetWindow *> appWindows;

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

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

	// Initializing randomizer
	srand(static_cast<unsigned>(time(nullptr)));

	// Initialize GDI+
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);

	// Getting the DLL file folder
	GetModuleFileNameA(HINSTANCE(&__ImageBase), DllPathFile, sizeof(DllPathFile));
	DllPath = DllPathFile;
	DllPath.resize(DllPath.size() - strlen("vSMR.dll"));
	
	ConfigPath = DllPath + "\\vSMR_Profiles.json";
	mapsPath = DllPath + "\\vSMR_Maps.json";
	IconsPath = DllPath + "\\aircraft_icons";
	LoadAircraftSpecs();

	Logger::info("Loading callsigns");

	// Creating the RIMCAS instance
	if (Callsigns == nullptr)
		Callsigns = new CCallsignLookup();

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
		RimcasInstance = new CRimcas();

	// Loading up the config file
	if (CurrentConfig == nullptr)
		CurrentConfig = new CConfig(ConfigPath, mapsPath);

	if (ColorManager == nullptr)
		ColorManager = new CColorManager();

	standardCursor = true;	
	ActiveAirport = "EGKK";

	// Setting up the data for the 2 approach windows
	appWindowDisplays[1] = false;
	appWindowDisplays[2] = false;
	appWindows[1] = new CInsetWindow(APPWINDOW_ONE);
	appWindows[2] = new CInsetWindow(APPWINDOW_TWO);

	Logger::info("Loading profile");

	this->CSMRRadar::LoadProfile("Default");

	this->CSMRRadar::LoadCustomFont();

	this->CSMRRadar::RefreshAirportActivity();
}

CSMRRadar::~CSMRRadar()
{
	Logger::info(string(__FUNCSIG__));
	CloseProfileEditorWindow(false);
	DestroyProfileEditorWindow();
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
	// Shutting down GDI+
	GdiplusShutdown(m_gdiplusToken);
	delete CurrentConfig;
}

void CSMRRadar::CorrelateCursor() {
	if (NeedCorrelateCursor)
	{
		if (standardCursor)
		{
			smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCORRELATE), IMAGE_CURSOR, 0, 0, LR_SHARED));

			AFX_MANAGE_STATE(AfxGetStaticModuleState());
			ASSERT(smrCursor);
			SetCursor(smrCursor);
			standardCursor = false;
		}
	}
	else
	{
	if (!standardCursor)
	{
		if (customCursor) {
			smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));
		}
			else {
				smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
			}

			AFX_MANAGE_STATE(AfxGetStaticModuleState());
			ASSERT(smrCursor);
			SetCursor(smrCursor);
			standardCursor = true;
		}
	}
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

	auto createFont = [&](int size) -> Gdiplus::Font*
	{
		Gdiplus::Font* font = new Gdiplus::Font(buffer.c_str(), Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		if (font->GetLastStatus() != Gdiplus::Ok)
		{
			delete font;
			font = new Gdiplus::Font(L"Arial", Gdiplus::REAL(size), fontStyle, Gdiplus::UnitPixel);
		}
		return font;
	};

	customFonts[1] = createFont(sizeOne);
	customFonts[2] = createFont(sizeTwo);
	customFonts[3] = createFont(sizeThree);
	customFonts[4] = createFont(sizeFour);
	customFonts[5] = createFont(sizeFive);
}

void CSMRRadar::ReloadConfig() {
	Logger::info("CSMRRadar::ReloadConfig()");
	std::string activeProfile = CurrentConfig ? CurrentConfig->getActiveProfileName() : "Default";
	if (!CurrentConfig)
		CurrentConfig = new CConfig(ConfigPath, mapsPath);
	else {
		CurrentConfig->reload();
	}
	if (activeProfile.empty())
		activeProfile = "Default";
	if (CurrentConfig->isItActiveProfile(activeProfile) == 0 && !CurrentConfig->getAllProfiles().empty()) {
		activeProfile = CurrentConfig->getAllProfiles().front();
	}
	this->LoadProfile(activeProfile);
	this->RefreshAirportActivity();
}

void CSMRRadar::LoadProfile(string profileName) {
	Logger::info(string(__FUNCSIG__));
	// Saving old profile data
	CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());

	// Loading the new profile
	CurrentConfig->setActiveProfile(profileName);
	InvalidateStructuredTagRuleCache();
	EnsureTargetGroundStatusColorEntries();

	// Loading all the new data
	const Value &RimcasTimer = CurrentConfig->getActiveProfile()["rimcas"]["timer"];
	const Value &RimcasTimerLVP = CurrentConfig->getActiveProfile()["rimcas"]["timer_lvp"];

	// Inactive alerts
	unordered_set inactiveAlerts = CurrentConfig->getInactiveAlert();
	RimcasInstance->setInactiveAlerts(inactiveAlerts);

	vector<int> RimcasNorm;
	for (SizeType i = 0; i < RimcasTimer.Size(); i++) {
		RimcasNorm.push_back(RimcasTimer[i].GetInt());
	}

	vector<int> RimcasLVP;
	for (SizeType i = 0; i < RimcasTimerLVP.Size(); i++) {
		RimcasLVP.push_back(RimcasTimerLVP[i].GetInt());
	}
	RimcasInstance->setCountdownDefinition(RimcasNorm, RimcasLVP);
	LeaderLineDefaultlenght = CurrentConfig->getActiveProfile()["labels"]["leader_line_length"].GetInt();

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

	if (ProfileEditorDialog && ::IsWindow(ProfileEditorDialog->GetSafeHwnd()))
		ProfileEditorDialog->SyncFromRadar();
}

void CSMRRadar::InvalidateStructuredTagRuleCache()
{
	StructuredTagRulesCache.clear();
	StructuredTagRulesCacheValid = false;
}

void CSMRRadar::EnsureTargetGroundStatusColorEntries()
{
	if (!CurrentConfig)
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
	ensureResolutionPresetMember(targets, "small_icon_boost_resolution", "1080p");
	ensureBoolMember(targets, "fixed_pixel_icon_size", false);
	ensureDoubleMember(targets, "fixed_pixel_triangle_scale", 1.0, 0.1, 3.0);

	Value& groundIcons = ensureObjectMember(targets, "ground_icons");

	ensureColorMember(groundIcons, "gate", 165, 165, 165, 255);
	ensureColorMember(groundIcons, "departure_gate", 165, 165, 165, 255);
	ensureColorMember(groundIcons, "arrival_gate", 165, 165, 165, 255);
	ensureColorMember(groundIcons, "push", 253, 218, 13, 255);
	ensureColorMember(groundIcons, "stup", 253, 218, 13, 255);
	ensureColorMember(groundIcons, "taxi", 240, 240, 240, 255);
	ensureColorMember(groundIcons, "depa", 240, 240, 240, 255);
	ensureColorMember(groundIcons, "arr", 165, 165, 165, 255);
	ensureColorMember(groundIcons, "airborne_departure", 240, 240, 240, 255);
	ensureColorMember(groundIcons, "airborne_arrival", 120, 190, 240, 255);
	ensureColorMember(groundIcons, "arrival_taxi", 70, 195, 120, 255);
	ensureColorMember(groundIcons, "nsts", 165, 165, 165, 255);
	ensureColorMember(groundIcons, "nofpl", 128, 128, 128, 255);

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
	ensureBoolMember(labels, "definition_detailed_same_as_definition", false);
	if (labels.HasMember("sid_text_colors"))
	{
		labels.RemoveMember("sid_text_colors");
		changed = true;
	}

	Value& departureLabel = ensureObjectMember(labels, "departure");
	ensureColorMember(departureLabel, "nofpl_color", 128, 128, 128, 255);
	Value& departureStatusColors = ensureObjectMember(departureLabel, "status_background_colors");
	ensureColorMember(departureStatusColors, "push", 253, 218, 13, 255);
	ensureColorMember(departureStatusColors, "stup", 253, 218, 13, 255);
	ensureColorMember(departureStatusColors, "taxi", 240, 240, 240, 255);
	ensureColorMember(departureStatusColors, "nsts", 165, 165, 165, 255);
	ensureColorMember(departureStatusColors, "depa", 240, 240, 240, 255);

	Value& arrivalLabel = ensureObjectMember(labels, "arrival");
	ensureColorMember(arrivalLabel, "nofpl_color", 128, 128, 128, 255);
	Value& arrivalStatusColors = ensureObjectMember(arrivalLabel, "status_background_colors");
	ensureColorMember(arrivalStatusColors, "arr", 165, 165, 165, 255);
	ensureColorMember(arrivalStatusColors, "taxi", 70, 195, 120, 255);

	Value& airborneLabel = ensureObjectMember(labels, "airborne");
	ensureColorMember(airborneLabel, "departure_background_color", 53, 126, 187, 255);
	ensureColorMember(airborneLabel, "arrival_background_color", 191, 87, 91, 255);
	ensureColorMember(airborneLabel, "departure_background_color_on_runway", 40, 50, 200, 255);
	ensureColorMember(airborneLabel, "arrival_background_color_on_runway", 170, 50, 50, 255);
	ensureColorMember(airborneLabel, "departure_text_color", 255, 255, 255, 255);
	ensureColorMember(airborneLabel, "arrival_text_color", 255, 255, 255, 255);

	ensureDefinitionArrayMember(departureLabel, "definition", nullptr);
	const Value* baseDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(departureLabel, "definitionDetailled", baseDefinition);
	Value& departureStatusDefinitions = ensureObjectMember(departureLabel, "status_definitions");

	auto ensureStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(departureStatusDefinitions, statusKey);
		const Value* defaultDefinition = (departureLabel.HasMember("definition") && departureLabel["definition"].IsArray()) ? &departureLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (departureLabel.HasMember("definitionDetailled") && departureLabel["definitionDetailled"].IsArray()) ? &departureLabel["definitionDetailled"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definitionDetailled", defaultDetailedDefinition);
	};

	ensureStatusDefinitionEntries("taxi");
	ensureStatusDefinitionEntries("push");
	ensureStatusDefinitionEntries("stup");
	ensureStatusDefinitionEntries("nsts");
	ensureStatusDefinitionEntries("depa");
	ensureStatusDefinitionEntries("nofpl");

	ensureDefinitionArrayMember(arrivalLabel, "definition", nullptr);
	const Value* arrivalBaseDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
	ensureDefinitionArrayMember(arrivalLabel, "definitionDetailled", arrivalBaseDefinition);
	Value& arrivalStatusDefinitions = ensureObjectMember(arrivalLabel, "status_definitions");
	auto ensureArrivalStatusDefinitionEntries = [&](const char* statusKey)
	{
		Value& statusSection = ensureObjectMember(arrivalStatusDefinitions, statusKey);
		const Value* defaultDefinition = (arrivalLabel.HasMember("definition") && arrivalLabel["definition"].IsArray()) ? &arrivalLabel["definition"] : nullptr;
		const Value* defaultDetailedDefinition = (arrivalLabel.HasMember("definitionDetailled") && arrivalLabel["definitionDetailled"].IsArray()) ? &arrivalLabel["definitionDetailled"] : defaultDefinition;
		ensureDefinitionArrayMember(statusSection, "definition", defaultDefinition);
		ensureDefinitionArrayMember(statusSection, "definitionDetailled", defaultDetailedDefinition);
	};

	ensureArrivalStatusDefinitionEntries("arr");
	ensureArrivalStatusDefinitionEntries("taxi");
	ensureArrivalStatusDefinitionEntries("nofpl");

	Value& labelRules = ensureObjectMember(labels, "rules");
	ensureIntMember(labelRules, "version", 1, 1, 1000);
	if (!labelRules.HasMember("items") || !labelRules["items"].IsArray())
	{
		if (labelRules.HasMember("items"))
			labelRules.RemoveMember("items");
		Value itemsKey;
		itemsKey.SetString("items", allocator);
		Value itemsArray(kArrayType);
		labelRules.AddMember(itemsKey, itemsArray, allocator);
		changed = true;
	}
	Value& labelRuleItems = labelRules["items"];

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
	for (rapidjson::SizeType i = 0; i < labelRuleItems.Size(); ++i)
	{
		if (!labelRuleItems[i].IsObject())
			continue;

		const Value& item = labelRuleItems[i];
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

		labelRuleItems.PushBack(ruleObject, allocator);
		changed = true;
	};

	auto migrateDefinitionArray = [&](Value& definitionArray, const std::string& tagType, const std::string& status, const std::string& detail)
	{
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
		if (!labels.HasMember(typeKey) || !labels[typeKey].IsObject())
			return;

		Value& section = labels[typeKey];
		if (section.HasMember("definition") && section["definition"].IsArray())
			migrateDefinitionArray(section["definition"], typeKey, "default", "normal");
		if (section.HasMember("definitionDetailled") && section["definitionDetailled"].IsArray())
			migrateDefinitionArray(section["definitionDetailled"], typeKey, "default", "detailed");

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
			if (statusSection.HasMember("definitionDetailled") && statusSection["definitionDetailled"].IsArray())
				migrateDefinitionArray(statusSection["definitionDetailled"], typeKey, statusKey, "detailed");
		}
	};

	migrateTypeDefinitions("departure");
	migrateTypeDefinitions("arrival");
	migrateTypeDefinitions("airborne");
	migrateTypeDefinitions("uncorrelated");

	Value& uiLayout = ensureObjectMember(profile, "ui_layout");
	Value& profileEditorWindow = ensureObjectMember(uiLayout, "profile_editor_window");
	ensureIntMember(profileEditorWindow, "x", 120, -32768, 32767);
	ensureIntMember(profileEditorWindow, "y", 120, -32768, 32767);
	ensureIntMember(profileEditorWindow, "width", 640, 320, 4096);
	ensureIntMember(profileEditorWindow, "height", 520, 220, 2160);
	ensureBoolMember(profileEditorWindow, "visible", false);

	if (changed && !CurrentConfig->saveConfig())
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

map<string, string> CSMRRadar::GenerateTagData(CRadarTarget rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, bool useSpeedForGates, string ActiveAirport)
{
	Logger::info(string(__FUNCSIG__));
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

	bool IsPrimary = !rt.GetPosition().GetTransponderC();
	bool isAirborne = rt.GetPosition().GetReportedGS() > 50;

	// ----- Callsign -------
	string callsign = rt.GetCallsign();
	if (fp.IsValid()) {
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
	const char * assr = fp.GetControllerAssignedData().GetSquawk();
	const char * ssr = rt.GetPosition().GetSquawk();
	bool has_squawk_error = false;
	if (strlen(assr) != 0 && !startsWith(ssr, assr)) {
		has_squawk_error = true;
		sqerror = "A";
		sqerror.append(assr);
	}

	// ----- Aircraft type -------

	string actype = "NoFPL";
	if (fp.IsValid() && fp.GetFlightPlanData().IsReceived())
		actype = fp.GetFlightPlanData().GetAircraftFPType();
	if (actype.size() > 4 && actype != "NoFPL")
		actype = actype.substr(0, 4);

	// ----- Aircraft type that changes to squawk error -------
	string sctype = actype;
	if (has_squawk_error)
		sctype = sqerror;

	// ----- Groundspeed -------
	string speed = std::to_string(rt.GetPosition().GetReportedGS());

	// ----- Departure runway -------
	string deprwy = fp.GetFlightPlanData().GetDepartureRwy();
	if (deprwy.length() == 0)
		deprwy = "RWY";

	// ----- Departure runway that changes for overspeed -------
	string seprwy = deprwy;
	if (rt.GetPosition().GetReportedGS() > 25)
		seprwy = std::to_string(rt.GetPosition().GetReportedGS());

	// ----- Arrival runway -------
	string arvrwy = fp.GetFlightPlanData().GetArrivalRwy();
	if (arvrwy.length() == 0)
		arvrwy = "RWY";

	// ----- Speed that changes to arrival runway -----
	string srvrwy = speed;
	if (rt.GetPosition().GetReportedGS() < 25)
		srvrwy = arvrwy;

	// ----- Gate -------
	string gate;
	if (useSpeedForGates)
		gate = std::to_string(fp.GetControllerAssignedData().GetAssignedSpeed());
	else
		gate = fp.GetControllerAssignedData().GetScratchPadString();

	replaceAll(gate, "STAND=", "");
	gate = gate.substr(0, 4);

	if (gate.size() == 0 || gate == "0" || !isAcCorrelated)
		gate = "NoGate";

	// ----- Gate that changes to speed -------
	string sate = gate;
	if (rt.GetPosition().GetReportedGS() > 25)
		sate = speed;

	// ----- Flightlevel -------
	int fl = rt.GetPosition().GetFlightLevel();
	int padding = 5;
	string pfls = "";
	if (fl <= TransitionAltitude) {
		fl = rt.GetPosition().GetPressureAltitude();
		pfls = "A";
		padding = 4;
	}
	string flightlevel = (pfls + padWithZeros(padding, fl)).substr(0, 3);

	// ----- Tendency -------
	string tendency = "-";
	int delta_fl = rt.GetPosition().GetFlightLevel() - rt.GetPreviousPosition(rt.GetPosition()).GetFlightLevel();
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
	if (fp.IsValid() && isAcCorrelated) {
		wake = "";
		wake += fp.GetFlightPlanData().GetAircraftWtc();
	}

	// ----- SSR -------
	string tssr = "";
	if (rt.IsValid())
	{
		tssr = rt.GetPosition().GetSquawk();
	}

	// ----- SID -------
	string dep = "SID";
	if (fp.IsValid() && isAcCorrelated)
	{
		dep = fp.GetFlightPlanData().GetSidName();
	}

	// ----- Short SID -------
	string ssid = dep;
	if (fp.IsValid() && ssid.size() > 5 && isAcCorrelated)
	{
		ssid = dep.substr(0, 3);
		ssid += dep.substr(dep.size() - 2, dep.size());
	}

	// ------- Origin aerodrome -------
	string origin = "????";
	if (isAcCorrelated)
	{
		origin = fp.GetFlightPlanData().GetOrigin();
	}

	// ------- Destination aerodrome -------
	string dest = "????";
	if (isAcCorrelated)
	{
		dest = fp.GetFlightPlanData().GetDestination();
	}

	// ----- GSTAT -------
	string gstat = "STS";
	if (fp.IsValid() && isAcCorrelated) {
		if (strlen(fp.GetGroundState()) != 0)
			gstat = fp.GetGroundState();
	}

	// ----- Clearance flag -------
	string clearance = "";
	if (fp.IsValid() && isAcCorrelated)
		clearance = fp.GetClearenceFlag() ? "[x]" : "[ ]";

	// ----- UK Controller Plugin / Assigned Stand -------
	string uk_stand;
	uk_stand = fp.GetControllerAssignedData().GetFlightStripAnnotation(3);
	if (uk_stand.length() == 0)
		uk_stand = "";

	// ----- Ramp Agent Remark -------
	string remark = fp.GetControllerAssignedData().GetFlightStripAnnotation(4);
	if (remark.length() == 0)
		remark = "";
	
	// ----- Scratchpad -------
	string scratchpad = fp.GetControllerAssignedData().GetScratchPadString();
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
	VacdmPilotData vacdmPilot;
	if (TryGetVacdmPilotDataForTarget(rt, fp, vacdmPilot))
	{
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
	if (tobt.empty() && fp.IsValid() && isAcCorrelated)
		tobt = NormalizeHhmmToken(fp.GetFlightPlanData().GetEstimatedDepartureTime());


	// ----- Generating the replacing map -----
	map<string, string> TagReplacingMap;

	// System ID for uncorrelated
	TagReplacingMap["systemid"] = "T:";
	string tpss = rt.GetSystemID();
	TagReplacingMap["systemid"].append(tpss.substr(1, 6));

	// Pro mode data here
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
			speed = std::to_string(rt.GetGS());
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

	return TagReplacingMap;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_SETCURSOR:
		SetCursor(smrCursor);
		return true;
	default:
		return CallWindowProc(gSourceProc, hwnd, uMsg, wParam, lParam);
	}
}

void CSMRRadar::OnRefresh(HDC hDC, int Phase)
{
	VSMR_REFRESH_LOG(string(__FUNCSIG__));
	// Changing the mouse cursor
	if (initCursor)
	{
		if (customCursor) {
			smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));
			// This got broken because of threading as far as I can tell
			// The cursor does change for some milliseconds but gets reset almost instantly by external MFC code

		}
		else {
			smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
		}

		if (smrCursor != nullptr)
		{		
			pluginWindow = GetActiveWindow();
			gSourceProc = (WNDPROC)SetWindowLong(pluginWindow, GWL_WNDPROC, (LONG)WindowProc);
		}
		initCursor = false;
	}

	if (Phase == REFRESH_PHASE_AFTER_LISTS) {
		VSMR_REFRESH_LOG("Phase == REFRESH_PHASE_AFTER_LISTS");
		if (!ColorSettingsDay) {
			// Creating the gdi+ graphics
			Graphics graphics(hDC);
			graphics.SetPageUnit(Gdiplus::UnitPixel);

			graphics.SetSmoothingMode(SmoothingModeAntiAlias);

			SolidBrush AlphaBrush(Color(CurrentConfig->getActiveProfile()["filters"]["night_alpha_setting"].GetInt(), 0, 0, 0));

			CRect RadarArea(GetRadarArea());
			RadarArea.top = RadarArea.top - 1;
			RadarArea.bottom = GetChatArea().bottom;

			graphics.FillRectangle(&AlphaBrush, CopyRect(CRect(RadarArea)));

			graphics.ReleaseHDC(hDC);
		}

		VSMR_REFRESH_LOG("break Phase == REFRESH_PHASE_AFTER_LISTS");
		return;
	}

	if (Phase != REFRESH_PHASE_BEFORE_TAGS)
		return;

	VSMR_REFRESH_LOG("Phase != REFRESH_PHASE_BEFORE_TAGS");

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

	// Timer each seconds
	clock_final = clock() - clock_init;
	double delta_t = (double)clock_final / ((double)CLOCKS_PER_SEC);
	if (delta_t >= 1) {
		clock_init = clock();
		BLINK = !BLINK;
		RefreshAirportActivity();
	}

	// Draw map elements based on zoom level
	CPosition radarDownLeft;
	CPosition radarUpRight;
	GetDisplayArea(&radarDownLeft, &radarUpRight);
	double radarCrossDistance = Haversine(radarDownLeft, radarUpRight);
	int NewRadarViewZoomLevel = getZoomLevelFromCrossDistance(radarCrossDistance);
	
	if (NewRadarViewZoomLevel != RadarViewZoomLevel) {
		RadarViewZoomLevel = NewRadarViewZoomLevel;
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
				if (item.active.substr(0, 4) != ActiveAirport) {
					shouldDraw = false;
				}

				auto runwayStatuses = RimcasInstance->GetRunwayStatuses();

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
							auto it = runwayStatuses.find(rwy);
							if (it == runwayStatuses.end() || (it->second != CRimcas::RunwayStatus::DEP && it->second != CRimcas::RunwayStatus::BOTH)) {
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
							auto it = runwayStatuses.find(rwy);
							if (it == runwayStatuses.end() || (it->second != CRimcas::RunwayStatus::ARR && it->second != CRimcas::RunwayStatus::BOTH)) {
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
		for (const auto& [elementName, toDraw] : drawItemMap) {
			size_t slashPos = elementName.find("/");
			if (slashPos == string::npos) continue;
			string category = elementName.substr(0, slashPos);
			string name = elementName.substr(slashPos + 1);

			int elementCategory = getIntFromCategory(category);
			if (elementCategory == -1) continue;
			CSectorElement element = GetPlugIn()->SectorFileElementSelectFirst(elementCategory);
			while (element.IsValid()) {
				if (strncmp(name.c_str(), element.GetName(), strlen(name.c_str())) == 0) {
					ShowSectorFileElement(element, element.GetComponentName(0), toDraw);
				}
				element = GetPlugIn()->SectorFileElementSelectNext(element, elementCategory);
			}
		}

		RefreshMapContent();
	}


	if (!QDMenabled && !QDMSelectEnabled)
	{
		POINT p;
		if (GetCursorPos(&p)) {
			if (ScreenToClient(GetActiveWindow(), &p)) {
				mouseLocation = p;
			}
		}
	}

	VSMR_REFRESH_LOG("Graphics set up");
	CDC dc;
	dc.Attach(hDC);

	// Creating the gdi+ graphics
	Graphics graphics(hDC);
	graphics.SetPageUnit(Gdiplus::UnitPixel);

	graphics.SetSmoothingMode(SmoothingModeAntiAlias);

	RECT RadarArea = GetRadarArea();
	RECT ChatArea = GetChatArea();
	RadarArea.bottom = ChatArea.top;

	AirportPositions.clear();


	CSectorElement apt;
	for (apt = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
		apt.IsValid();
		apt = GetPlugIn()->SectorFileElementSelectNext(apt, SECTOR_ELEMENT_AIRPORT))
	{
		CPosition Pos;
		apt.GetPosition(&Pos, 0);
		AirportPositions[string(apt.GetName())] = Pos;
	}

	RimcasInstance->RunwayAreas.clear();

	if (QDMSelectEnabled || QDMenabled)
	{
		CRect R(GetRadarArea());
		R.top += 20;
		R.bottom = GetChatArea().top;

		R.NormalizeRect();
		AddScreenObject(DRAWING_BACKGROUND_CLICK, "", R, false, "");
	}

	VSMR_REFRESH_LOG("Runway loop");
	CSectorElement rwy;
	for (rwy = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		if (startsWith(getActiveAirport().c_str(), rwy.GetAirportName())) {

			CPosition Left;
			rwy.GetPosition(&Left, 1);
			CPosition Right;
			rwy.GetPosition(&Right, 0);

			string runway_name = rwy.GetRunwayName(0);
			string runway_name2 = rwy.GetRunwayName(1);

			double bearing1 = TrueBearing(Left, Right);
			double bearing2 = TrueBearing(Right, Left);

			const Value& CustomMap = CurrentConfig->getAirportMapIfAny(getActiveAirport());

			vector<CPosition> def;
// Rimcas now ignores the defined runway polygon to ensure that the correct detection area is used, defined runway is now only used for closed runway
//			if (CurrentConfig->isCustomRunwayAvail(getActiveAirport(), runway_name, runway_name2)) {
			//	const Value& Runways = CustomMap["runways"];
			//
			//		if (Runways.IsArray()) {
			//		for (SizeType i = 0; i < Runways.Size(); i++) {
			//			if (startsWith(runway_name.c_str(), Runways[i]["runway_name"].GetString()) ||
			//				startsWith(runway_name2.c_str(), Runways[i]["runway_name"].GetString())) {
			//
			//				string path_name = "path";
			//
			//				if (isLVP)
			//					path_name = "path_lvp";
			//
			//				const Value& Path = Runways[i][path_name.c_str()];
			//				for (SizeType j = 0; j < Path.Size(); j++) {
			//					CPosition position;
			//					position.LoadFromStrings(Path[j][(SizeType)1].GetString(), Path[j][(SizeType)0].GetString());
			//
			//					def.push_back(position);
			//				}
			//	
			//			}
			//		}
			//	}
			//}
			//else {
				def = RimcasInstance->GetRunwayArea(Left, Right);
			//}

			RimcasInstance->AddRunwayArea(this, runway_name, runway_name2, def);

			// Check runway statuses
			bool isDepartureRwy = rwy.IsElementActive(true, 0);
			bool isArrivalRwy = rwy.IsElementActive(false, 0);
			if (isDepartureRwy) {
				if (isArrivalRwy) {
					RimcasInstance->SetRunwayStatus(runway_name, CRimcas::RunwayStatus::BOTH);
				}
				else {
					RimcasInstance->SetRunwayStatus(runway_name, CRimcas::RunwayStatus::DEP);
				}
			}
			else {
				if (isArrivalRwy) {
					RimcasInstance->SetRunwayStatus(runway_name, CRimcas::RunwayStatus::ARR);
				}
				else {
					RimcasInstance->SetRunwayStatus(runway_name, CRimcas::RunwayStatus::CLSD);
				}

			}
			isDepartureRwy = rwy.IsElementActive(true, 1);
			isArrivalRwy = rwy.IsElementActive(false, 1);
			if (isDepartureRwy) {
				if (isArrivalRwy) {
					RimcasInstance->SetRunwayStatus(runway_name2, CRimcas::RunwayStatus::BOTH);
				}
				else {
					RimcasInstance->SetRunwayStatus(runway_name2, CRimcas::RunwayStatus::DEP);
				}
			}
			else {
				if (isArrivalRwy) {
					RimcasInstance->SetRunwayStatus(runway_name2, CRimcas::RunwayStatus::ARR);
				}
				else {
					RimcasInstance->SetRunwayStatus(runway_name2, CRimcas::RunwayStatus::CLSD);
				}
			}

			string RwName = runway_name + " / " + runway_name2;

			if (drawRunways) {

				PointF lpPoints[5000];
				int w = 0;
				for (auto& Point : def)
				{
					POINT toDraw = ConvertCoordFromPositionToPixel(Point);

					lpPoints[w] = { REAL(toDraw.x), REAL(toDraw.y) };
					w++;
				}
				Pen pw(ColorManager->get_corrected_color("label", Color::White));
				graphics.DrawPolygon( &pw, lpPoints, w);
			}

			if (RimcasInstance->ClosedRunway.find(RwName) != RimcasInstance->ClosedRunway.end()) {
				if (RimcasInstance->ClosedRunway[RwName]) {

					CPen RedPen(PS_SOLID, 2, RGB(150, 0, 0));
					CPen * oldPen = dc.SelectObject(&RedPen);

					if (CurrentConfig->isCustomRunwayAvail(getActiveAirport(), runway_name, runway_name2)) {
						const Value& Runways = CustomMap["runways"];

						if (Runways.IsArray()) {
							for (SizeType i = 0; i < Runways.Size(); i++) {
								if (startsWith(runway_name.c_str(), Runways[i]["runway_name"].GetString()) ||
									startsWith(runway_name2.c_str(), Runways[i]["runway_name"].GetString())) {

									string path_name = "path";

									if (isLVP)
										path_name = "path_lvp";

									const Value& Path = Runways[i][path_name.c_str()];

									PointF lpPoints[5000];

									int k = 1;
									int l = 0;
									for (SizeType w = 0; w < Path.Size(); w++) {
										CPosition position;
										position.LoadFromStrings(Path[w][static_cast<SizeType>(1)].GetString(), Path[w][static_cast<SizeType>(0)].GetString());

										POINT cv = ConvertCoordFromPositionToPixel(position);
										lpPoints[l] = { REAL(cv.x), REAL(cv.y) };

										k++;
										l++;
									}

									graphics.FillPolygon(&SolidBrush(Color(150, 0, 0)), lpPoints, k - 1);

									break;
								}
							}
						}

					}
					else {
						PointF lpPoints[5000];
						int w = 0;
						for(auto &Point : def)
						{
							POINT toDraw = ConvertCoordFromPositionToPixel(Point);

							lpPoints[w] = { REAL(toDraw.x), REAL(toDraw.y) };
							w++;
						}

						graphics.FillPolygon(&SolidBrush(Color(150, 0, 0)), lpPoints, w);
					}

					dc.SelectObject(oldPen);
				}
			}
		}
	}

	RimcasInstance->OnRefreshBegin(isLVP);

#pragma region symbols
	// Drawing the symbols
	VSMR_REFRESH_LOG("Symbols loop");

	// Cache current view scaling once per frame; reused by trail and icon sizing.
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

	const std::string frameIconStyle = GetActiveTargetIconStyle();
	const bool frameUseDiamondIconStyle = (frameIconStyle == "diamond");
	const bool frameUseRealisticIconStyle = (frameIconStyle == "realistic");
	const bool frameSmallIconBoostEnabled = GetSmallTargetIconBoostEnabled();
	const bool frameFixedPixelIconSize = GetFixedPixelTargetIconSizeEnabled();
	const double frameSmallIconBoostFactor = std::clamp(GetSmallTargetIconBoostFactor(), 0.5, 4.0);
	const double frameSmallIconBoostResolutionScale = std::clamp(GetSmallTargetIconBoostResolutionScale(), 1.0, 2.0);
	const double frameFixedTriangleScale = std::clamp(GetFixedPixelTriangleIconScale(), 0.1, 3.0);
	const bool frameProModeEnabled = CurrentConfig->getActiveProfile()["filters"]["pro_mode"]["enable"].GetBool();
	const int frameTransitionAltitude = GetPlugIn()->GetTransitionAltitude();
	const std::string frameActiveAirport = getActiveAirport();
	const bool frameUseAspeedForGate = CurrentConfig->getActiveProfile()["labels"]["use_aspeed_for_gate"].GetBool();
	const std::vector<StructuredTagColorRule>& frameStructuredTagRules = GetStructuredTagColorRules();
	CRadarTarget frameAselTarget = GetPlugIn()->RadarTargetSelectASEL();
	FrameTagDataCache frameTagDataCache;
	FrameVacdmLookupCache frameVacdmLookupCache;
	std::unordered_map<std::string, std::vector<VacdmColorRuleDefinition>> frameIconVacdmRuleCache;
	auto structuredRuleContextMatches = [](const StructuredTagColorRule& rule, const std::string& type, const std::string& status, const std::string& detail) -> bool
	{
		auto normalize = [](const std::string& text) -> std::string
		{
			return ToLowerAsciiCopy(TrimAsciiWhitespaceCopy(text));
		};

		auto fieldMatches = [&](const std::string& expectedRaw, const std::string& currentRaw) -> bool
		{
			const std::string expected = normalize(expectedRaw);
			const std::string current = normalize(currentRaw);
			if (expected.empty() || expected == "any" || expected == "all" || expected == "*")
				return true;
			return expected == current;
		};

		return fieldMatches(rule.tagType, type) &&
			fieldMatches(rule.status, status) &&
			fieldMatches(rule.detail, detail);
	};
	auto evaluateStructuredColorRules = [&](const std::string& type, const std::string& status, const std::string& detail,
		const std::map<std::string, std::string>& replacingMap, const VacdmPilotData* pilotData) -> VacdmColorRuleOverrides
	{
		VacdmColorRuleOverrides overrides;
		for (const StructuredTagColorRule& rule : frameStructuredTagRules)
		{
			if (!structuredRuleContextMatches(rule, type, status, detail))
				continue;

			const std::string source = ToLowerAsciiCopy(rule.source);
			bool matches = false;
			if (source == "runway")
			{
				std::string actualRunway;
				auto itRunway = replacingMap.find(rule.token);
				if (itRunway != replacingMap.end())
					actualRunway = itRunway->second;
				matches = RunwayRuleConditionMatches(rule.condition, actualRunway);
			}
			else if (source == "custom")
			{
				std::string actualValue;
				auto itValue = replacingMap.find(rule.token);
				if (itValue != replacingMap.end())
					actualValue = itValue->second;
				matches = RunwayRuleConditionMatches(rule.condition, actualValue);
			}
			else
			{
				const std::string actualState = ResolveVacdmRuleStateName(rule.token, pilotData);
				matches = VacdmRuleStateMatches(rule.condition, actualState);
			}

			if (!matches)
				continue;

			if (rule.applyTarget)
			{
				overrides.hasTargetColor = true;
				overrides.targetR = rule.targetR;
				overrides.targetG = rule.targetG;
				overrides.targetB = rule.targetB;
			}
			if (rule.applyTag)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
			}
			if (rule.applyText)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
			}
		}
		return overrides;
	};
	auto getCachedVacdmLookup = [&](CRadarTarget radarTarget, CFlightPlan flightPlan, VacdmPilotData& outData) -> bool
	{
		const std::string callsign = radarTarget.IsValid() && radarTarget.GetCallsign() != nullptr
			? ToUpperAsciiCopy(radarTarget.GetCallsign())
			: std::string();
		if (callsign.empty())
			return false;

		auto it = frameVacdmLookupCache.find(callsign);
		if (it == frameVacdmLookupCache.end())
		{
			FrameVacdmLookupResult lookup;
			lookup.hasData = TryGetVacdmPilotDataForTarget(radarTarget, flightPlan, lookup.data);
			it = frameVacdmLookupCache.emplace(callsign, std::move(lookup)).first;
		}

		if (!it->second.hasData)
			return false;

		outData = it->second.data;
		return true;
	};
	auto getCachedIconVacdmColorRules = [&](const std::string& type, const std::string& status) -> const std::vector<VacdmColorRuleDefinition>&
	{
		const std::string cacheKey = type + "|" + status;
		auto it = frameIconVacdmRuleCache.find(cacheKey);
		if (it == frameIconVacdmRuleCache.end())
		{
			std::vector<VacdmColorRuleDefinition> rules;
			const std::vector<std::string> definitionLines =
				GetTagDefinitionLineStrings(type, false, TagDefinitionEditorMaxLines, false, status);
			CollectVacdmColorRulesFromLineTexts(definitionLines, rules);
			it = frameIconVacdmRuleCache.emplace(cacheKey, std::move(rules)).first;
		}
		return it->second;
	};
	EuroScopePlugIn::CRadarTarget rt;
	for (rt = GetPlugIn()->RadarTargetSelectFirst();
		rt.IsValid();
		rt = GetPlugIn()->RadarTargetSelectNext(rt))
	{
		if (!rt.IsValid() || !rt.GetPosition().IsValid())
			continue;

		int reportedGs = rt.GetPosition().GetReportedGS();
		bool isAcDisplayed = isVisible(rt);

		if (!isAcDisplayed)
			continue;

		CFlightPlan iconFp = GetPlugIn()->FlightPlanSelect(rt.GetCallsign());
		bool AcisCorrelated = IsCorrelated(iconFp, rt);
		RimcasInstance->OnRefresh(rt, this, AcisCorrelated, isLVP);

		CRadarTargetPositionData RtPos = rt.GetPosition();

		POINT acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());

		if (rt.GetGS() > 5) {
			POINT oldacPosPix;
			CRadarTargetPositionData pAcPos = rt.GetPosition();

			for (int i = 1; i <= 2; i++) {
				oldacPosPix = ConvertCoordFromPositionToPixel(pAcPos.GetPosition());
				pAcPos = rt.GetPreviousPosition(pAcPos);
				acPosPix = ConvertCoordFromPositionToPixel(pAcPos.GetPosition());

				// Afterglow polygons disabled (remove comet-like trail)
			}

			// Trails as shrinking bubbles
			int TrailNumber = Trail_Gnd;
			if (reportedGs > 50)
				TrailNumber = Trail_App;

			const double pixPerMeter = framePixPerMeter;

			CRadarTargetPositionData previousPos = rt.GetPreviousPosition(rt.GetPosition());
			for (int j = 1; j <= TrailNumber; j++) {
				POINT pCoord = ConvertCoordFromPositionToPixel(previousPos.GetPosition());

				// Bubble diameter is fixed in meters so it scales with zoom
				double metersPerBubble = 10.0; // base diameter in meters
				int diameterPx = 6;
				if (pixPerMeter > 0.0) {
					diameterPx = int(pixPerMeter * metersPerBubble + 0.5);
				}
				if (diameterPx < 2) diameterPx = 2;
				if (diameterPx > 50) diameterPx = 50;

				// Shrink size with age (more aggressive for visibility)
				double shrink = 1.0 - 0.15 * (j - 1);
				if (shrink < 0.2) shrink = 0.2;
				diameterPx = int(diameterPx * shrink + 0.5);
				if (diameterPx < 2) diameterPx = 2;
				int radius = diameterPx / 2;

				// Gradient from transparent white (new) to gray-blue (older) with fading alpha
				double t = (TrailNumber > 1) ? double(j - 1) / double(TrailNumber - 1) : 0.0;
				auto lerp = [](double a, double b, double tt) { return a + (b - a) * tt; };
				int r = int(lerp(255.0, 120.0, t) + 0.5);
				int g = int(lerp(255.0, 150.0, t) + 0.5);
				int b = int(lerp(255.0, 190.0, t) + 0.5);
				int a = int(lerp(200.0, 40.0, t) + 0.5); // fade alpha
				if (r < 0) r = 0; if (r > 255) r = 255;
				if (g < 0) g = 0; if (g > 255) g = 255;
				if (b < 0) b = 0; if (b > 255) b = 255;
				if (a < 0) a = 0; if (a > 255) a = 255;

				Color bubbleColor(static_cast<BYTE>(a), static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b));
				// Slightly thicker ring to balance visibility and hole size
				Gdiplus::Pen ringPen(bubbleColor, Gdiplus::REAL(1.5f));
				graphics.DrawEllipse(&ringPen, pCoord.x - radius, pCoord.y - radius, diameterPx, diameterPx);

				previousPos = rt.GetPreviousPosition(previousPos);
			}
		}


		// Disable legacy basic (yellow) aircraft symbol when using PNG icons
		const bool drawLegacyPrimarySymbol = false;
		if (drawLegacyPrimarySymbol && CurrentConfig->getActiveProfile()["targets"]["show_primary_target"].GetBool()) {

			SolidBrush H_Brush(ColorManager->get_corrected_color("afterglow",
				CurrentConfig->getConfigColor(CurrentConfig->getActiveProfile()["targets"]["target_color"])));

			PointF lpPoints[100];
			for (unsigned int i = 0; i < Patatoides[rt.GetCallsign()].points.size(); i++)
			{
				CPosition pos;
				pos.m_Latitude = Patatoides[rt.GetCallsign()].points[i].x;
				pos.m_Longitude = Patatoides[rt.GetCallsign()].points[i].y;

				lpPoints[i] = { REAL(ConvertCoordFromPositionToPixel(pos).x), REAL(ConvertCoordFromPositionToPixel(pos).y) };
			}

			graphics.FillPolygon(&H_Brush, lpPoints, Patatoides[rt.GetCallsign()].points.size());
		}
		acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());

		const bool proModeEnabled = frameProModeEnabled;
		const bool isReleasedTrack = (std::find(ReleasedTracks.begin(), ReleasedTracks.end(), rt.GetSystemID()) != ReleasedTracks.end());
		const bool hasAssignedSquawk = iconFp.IsValid() && strlen(iconFp.GetControllerAssignedData().GetSquawk()) != 0;
		const bool isWrongSquawk = hasAssignedSquawk &&
			strcmp(iconFp.GetControllerAssignedData().GetSquawk(), rt.GetPosition().GetSquawk()) != 0;
		if (proModeEnabled && !hasAssignedSquawk)
			AcisCorrelated = false;

		const bool keepIconForSquawkMismatch = proModeEnabled && (isWrongSquawk || !hasAssignedSquawk) && !isReleasedTrack;

		if (!AcisCorrelated && reportedGs < 1 && !ReleaseInProgress && !AcquireInProgress && !keepIconForSquawkMismatch)
			continue;

		// Prefer the aircraft-reported heading to keep icon orientation aligned with the nose (even when moving backwards)
		double headingDeg = double(RtPos.GetReportedHeadingTrueNorth());
		if (headingDeg < 0.0 || headingDeg >= 360.0)
			headingDeg = rt.GetTrackHeading();

		// Icon sizing based on real dimensions and zoom
		int iconSize = 40;
		const bool useDiamondIconStyle = frameUseDiamondIconStyle;
		const bool useRealisticIconStyle = frameUseRealisticIconStyle;
		char wtc = iconFp.IsValid() ? iconFp.GetFlightPlanData().GetAircraftWtc() : '\0';
		std::string acType = iconFp.IsValid() ? iconFp.GetFlightPlanData().GetAircraftFPType() : "";
		if (acType.size() > 4)
			acType = acType.substr(0, 4);
		std::string acTypeLower = acType;
		std::transform(acTypeLower.begin(), acTypeLower.end(), acTypeLower.begin(), ::tolower);

		auto fallbackTypeForWtc = [](char wtcChar) {
			switch (std::toupper(static_cast<unsigned char>(wtcChar))) {
			case 'L': return std::string("c172");
			case 'M': return std::string("a320");
			case 'H': return std::string("b77w");
			case 'J': return std::string("a388"); // super / heavy
			default: return std::string("a320");  // sensible large default
			}
		};

		// Pick an icon type, first the actual FP type, then WTC fallback if missing
		std::string iconType = acTypeLower;
		Bitmap* iconBmp = GetAircraftIcon(iconType);
		if (iconBmp == nullptr) {
			iconType = fallbackTypeForWtc(wtc);
			iconBmp = GetAircraftIcon(iconType);
		}

		// Pick specs for sizing: prefer actual type, else icon type, else WTC fallback
		auto specIt = AircraftSpecs.find(acTypeLower);
		if (specIt == AircraftSpecs.end()) {
			specIt = AircraftSpecs.find(iconType);
		}
		if (specIt == AircraftSpecs.end()) {
			std::string specFallback = fallbackTypeForWtc(wtc);
			specIt = AircraftSpecs.find(specFallback);
		}

		bool isOnRunway = RimcasInstance->isAcOnRunway(rt.GetCallsign());
		GroundStateCategory groundStateCat = GroundStateCategory::Unknown;
		if (iconFp.IsValid()) {
			groundStateCat = classifyGroundState(iconFp.GetGroundState(), reportedGs, isOnRunway);
		}

		bool isDepartureTarget = false;
		if (iconFp.IsValid() && AcisCorrelated)
		{
			std::string originAirport = iconFp.GetFlightPlanData().GetOrigin();
			if (!originAirport.empty() && !frameActiveAirport.empty())
			{
				std::transform(originAirport.begin(), originAirport.end(), originAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				std::string activeAirport = frameActiveAirport;
				std::transform(activeAirport.begin(), activeAirport.end(), activeAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				isDepartureTarget = (originAirport == activeAirport);
			}
		}
		const bool hasNoFlightPlan = !iconFp.IsValid();
		const bool isAirborneTarget = (reportedGs > 50);

		std::string vacdmRuleType = "departure";
		if (!AcisCorrelated && reportedGs >= 3)
			vacdmRuleType = "uncorrelated";
		else if (isAirborneTarget)
			vacdmRuleType = "airborne";
		else if (!isDepartureTarget)
			vacdmRuleType = "arrival";

		std::string vacdmRuleStatus = "default";
		if ((vacdmRuleType == "departure" || vacdmRuleType == "arrival") && hasNoFlightPlan)
		{
			vacdmRuleStatus = "nofpl";
		}
		else if (vacdmRuleType == "airborne")
		{
			if (isDepartureTarget)
				vacdmRuleStatus = isOnRunway ? "airdep_onrunway" : "airdep";
			else
				vacdmRuleStatus = isOnRunway ? "airarr_onrunway" : "airarr";
		}
		else if (vacdmRuleType == "departure" || vacdmRuleType == "arrival")
		{
			switch (groundStateCat)
			{
			case GroundStateCategory::Taxi:
				vacdmRuleStatus = "taxi";
				break;
			case GroundStateCategory::Push:
				if (vacdmRuleType == "departure")
					vacdmRuleStatus = "push";
				break;
			case GroundStateCategory::Stup:
				if (vacdmRuleType == "departure")
					vacdmRuleStatus = "stup";
				break;
			case GroundStateCategory::Nsts:
				if (vacdmRuleType == "departure")
					vacdmRuleStatus = "nsts";
				break;
			case GroundStateCategory::Depa:
				if (vacdmRuleType == "departure")
					vacdmRuleStatus = "depa";
				break;
			case GroundStateCategory::Arr:
				if (vacdmRuleType == "arrival")
					vacdmRuleStatus = "arr";
				break;
			default:
				break;
			}
		}

		VacdmPilotData vacdmRulePilotData;
		const bool hasVacdmRulePilotData = getCachedVacdmLookup(rt, iconFp, vacdmRulePilotData);
		const std::vector<VacdmColorRuleDefinition>& vacdmColorRules =
			getCachedIconVacdmColorRules(vacdmRuleType, vacdmRuleStatus);
		VacdmColorRuleOverrides vacdmColorRuleOverrides =
			EvaluateVacdmColorRules(vacdmColorRules, hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		const bool iconIsAseL = (frameAselTarget.IsValid() && strcmp(frameAselTarget.GetCallsign(), rt.GetCallsign()) == 0);
		const std::string targetCallsign = rt.GetCallsign() != nullptr ? ToUpperAsciiCopy(rt.GetCallsign()) : std::string();
		auto tagDataIt = frameTagDataCache.find(targetCallsign);
		if (tagDataIt == frameTagDataCache.end())
		{
			TagReplacingMap generatedTagData = GenerateTagData(
				rt,
				iconFp,
				iconIsAseL,
				AcisCorrelated,
				proModeEnabled,
				frameTransitionAltitude,
				frameUseAspeedForGate,
				frameActiveAirport);
			tagDataIt = frameTagDataCache.emplace(targetCallsign, std::move(generatedTagData)).first;
		}
		const TagReplacingMap& iconReplacingMap = tagDataIt->second;
		const VacdmColorRuleOverrides structuredIconColorRuleOverrides = evaluateStructuredColorRules(
			vacdmRuleType,
			vacdmRuleStatus,
			"normal",
			iconReplacingMap,
			hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		if (structuredIconColorRuleOverrides.hasTargetColor)
		{
			vacdmColorRuleOverrides.hasTargetColor = true;
			vacdmColorRuleOverrides.targetR = structuredIconColorRuleOverrides.targetR;
			vacdmColorRuleOverrides.targetG = structuredIconColorRuleOverrides.targetG;
			vacdmColorRuleOverrides.targetB = structuredIconColorRuleOverrides.targetB;
		}

		const bool smallIconBoostEnabled = frameSmallIconBoostEnabled;
		const bool fixedPixelIconSize = frameFixedPixelIconSize;

		if (useRealisticIconStyle && iconBmp != nullptr) {
			auto getGroundIconColor = [&](const char* key, Color fallback) -> Color {
				const Value& profile = CurrentConfig->getActiveProfile();
				if (profile.HasMember("targets")) {
					const Value& targets = profile["targets"];
					if (targets.HasMember("ground_icons") && targets["ground_icons"].HasMember(key)) {
						return CurrentConfig->getConfigColor(targets["ground_icons"][key]);
					}
				}
				return fallback;
			};

			// Compute on-screen size that scales with zoom (uniform for all aircraft)
			double drawW = iconSize;
			double drawH = iconSize;
			const double pixPerMeter = framePixPerMeter;

			// Use real-world dimensions when available; otherwise WTC defaults (no generic fallback)
			double lengthMeters = 0.0;
			double spanMeters = 0.0;

			auto wtcFallbackDims = [](char w, double& lenOut, double& spanOut) {
				switch (std::toupper(static_cast<unsigned char>(w))) {
				case 'L': lenOut = 28.0; spanOut = 28.0; break;
				case 'M': lenOut = 40.0; spanOut = 36.0; break;
				case 'H': lenOut = 60.0; spanOut = 60.0; break;
				case 'J': lenOut = 72.0; spanOut = 80.0; break;
				default: lenOut = 40.0; spanOut = 36.0; break;
				}
			};

			if (specIt != AircraftSpecs.end()) {
				if (specIt->second.length > 0)
					lengthMeters = specIt->second.length;
				if (specIt->second.wingspan > 0)
					spanMeters = specIt->second.wingspan;
			}

#if 0
#endif

			// Hardcoded overrides for specific types when JSON is not loaded or missing (compact)
			if (lengthMeters <= 0 || spanMeters <= 0) {
				static const std::unordered_map<std::string, std::pair<double, double>> hardcodedDims = {
					{ "a10", {16.3, 17.5} }, { "a124", {69.1, 73.3} }, { "a139", {13.5, 13.8} },
					{ "a20n", {37.6, 35.8} }, { "a225", {84.0, 88.4} }, { "a310", {46.7, 43.9} },
					{ "a318", {31.4, 34.1} }, { "a319", {33.8, 35.8} }, { "a320", {37.6, 35.8} },
					{ "a321", {44.5, 35.8} }, { "a332", {58.8, 60.3} }, { "a333", {63.6, 60.3} },
					{ "a338", {58.8, 64.0} }, { "a339", {63.7, 64.0} }, { "a342", {59.4, 60.3} },
					{ "a343", {63.6, 60.3} }, { "a359", {66.9, 64.8} }, { "a35k", {73.8, 64.8} },
					{ "a388", {73.0, 79.8} }, { "a400", {42.4, 45.1} }, { "a748", {20.4, 30.0} },
					{ "an2", {12.7, 18.2} }, { "an24", {23.5, 29.2} }, { "as32", {16.8, 16.2} },
					{ "as50", {12.9, 10.7} }, { "atp", {26.0, 30.6} }, { "b06", {12.1, 10.2} },
					{ "b1", {44.8, 41.7} }, { "b190", {17.6, 17.7} }, { "b350", {14.2, 17.7} },
					{ "b37m", {35.6, 35.9} }, { "b38m", {39.5, 35.9} }, { "b39m", {42.2, 35.9} },
					{ "b407", {12.7, 10.7} }, { "b461", {26.2, 26.2} }, { "b462", {28.6, 26.3} },
					{ "b463", {31.0, 26.3} }, { "b703", {46.6, 44.4} }, { "b712", {37.8, 28.5} },
					{ "b720", {41.3, 39.9} }, { "b721", {40.6, 32.9} }, { "b722", {46.7, 32.9} },
					{ "b731", {28.7, 28.3} }, { "b732", {30.5, 28.3} }, { "b733", {33.4, 31.1} },
					{ "b734", {36.4, 28.9} }, { "b735", {31.0, 31.1} }, { "b736", {31.2, 34.3} },
					{ "b737", {33.6, 35.8} }, { "b738", {39.5, 35.8} }, { "b739", {42.1, 35.8} },
					{ "b741", {70.6, 59.6} }, { "b744", {70.6, 64.4} }, { "b748", {76.3, 68.4} },
					{ "b74s", {56.3, 59.6} }, { "b752", {47.3, 41.1} }, { "b753", {54.5, 41.1} },
					{ "b762", {48.5, 47.6} }, { "b763", {54.9, 50.9} }, { "b764", {61.4, 51.9} },
					{ "b772", {63.7, 60.9} }, { "b773", {73.9, 60.9} }, { "b77l", {63.7, 64.8} },
					{ "b77w", {73.9, 64.8} }, { "b788", {56.7, 60.1} }, { "b789", {62.8, 60.1} },
					{ "b78x", {68.3, 60.1} }, { "bcs1", {35.0, 35.1} }, { "bcs3", {38.7, 35.1} },
					{ "be20", {13.4, 16.6} }, { "be35", {8.1, 8.4} }, { "be36", {8.1, 8.4} },
					{ "be58", {9.1, 11.5} }, { "be60", {10.3, 12.0} }, { "be9l", {10.8, 15.3} },
					{ "blcf", {71.7, 64.4} }, { "bn2p", {10.9, 14.9} }, { "bt7", {14.2, 10.0} },
					{ "c130", {29.8, 40.4} }, { "c152", {7.3, 10.2} }, { "c160", {32.4, 40.0} },
					{ "c17", {53.0, 51.7} }, { "c172", {8.2, 10.9} }, { "c2", {17.6, 24.6} },
					{ "c206", {8.6, 10.9} }, { "c208", {11.5, 15.9} }, { "c25b", {15.6, 16.3} },
					{ "c25c", {16.3, 15.5} }, { "c310", {9.7, 11.3} }, { "c402", {11.1, 13.5} },
					{ "c414", {10.3, 12.5} }, { "c510", {12.4, 13.2} }, { "c525", {13.0, 14.3} },
					{ "c5m", {75.3, 67.9} }, { "c68a", {19.0, 22.1} }, { "c700", {22.3, 21.0} },
					{ "c750", {22.0, 19.5} }, { "c919", {38.9, 35.8} }, { "cl30", {20.9, 19.5} },
					{ "cl60", {20.9, 19.6} }, { "conc", {61.7, 25.6} }, { "cp10", {7.2, 8.1} },
					{ "crj2", {26.8, 21.2} }, { "crj7", {32.5, 23.2} }, { "crj9", {36.2, 24.9} },
					{ "crjx", {39.1, 26.2} }, { "da40", {8.0, 11.9} }, { "da42", {8.6, 13.4} },
					{ "da62", {9.2, 14.6} }, { "dc10", {55.0, 50.4} }, { "dc3", {19.7, 29.0} },
					{ "dc6", {32.2, 35.8} }, { "dc86", {57.1, 43.4} }, { "dh8a", {22.3, 25.9} },
					{ "dh8c", {25.7, 27.4} }, { "dh8d", {32.8, 28.4} }, { "dhc2", {9.2, 14.6} },
					{ "dhc6", {15.1, 19.8} }, { "dhc7", {24.6, 28.4} }, { "dimo", {7.1, 16.5} },
					{ "dr40", {7.0, 8.7} }, { "dv20", {7.2, 10.9} }, { "e135", {26.3, 20.0} },
					{ "e145", {29.8, 20.0} }, { "e170", {29.9, 26.0} }, { "e190", {36.2, 28.7} },
					{ "e195", {38.6, 28.7} }, { "e290", {36.2, 33.7} }, { "e295", {41.5, 35.1} },
					{ "e3cf", {46.6, 44.4} }, { "e50p", {12.8, 12.3} }, { "e55p", {15.9, 16.2} },
					{ "e75s", {31.7, 26.0} }, { "eufi", {16.0, 10.9} }, { "evot", {9.1, 11.0} },
					{ "f100", {35.5, 28.1} }, { "f104", {16.7, 7.0} }, { "f14", {19.1, 19.5} },
					{ "f15", {19.4, 13.0} }, { "f16", {15.0, 10.0} }, { "f22", {18.9, 13.6} },
					{ "f27", {23.1, 29.0} }, { "f28", {27.4, 27.1} }, { "f2th", {20.2, 19.3} },
					{ "f35", {15.7, 11.0} }, { "f4", {17.8, 11.7} }, { "f70", {30.9, 29.1} },
					{ "f900", {20.2, 19.3} }, { "fa10", {13.9, 13.1} }, { "fa20", {17.2, 16.3} },
					{ "fa50", {18.5, 18.9} }, { "fa6x", {25.7, 25.9} }, { "fa7x", {23.2, 26.2} },
					{ "fa8x", {24.5, 26.3} }, { "g109", {8.1, 17.4} }, { "gl5t", {29.5, 28.6} },
					{ "gl7t", {33.9, 31.7} }, { "glex", {30.3, 28.6} }, { "glf5", {29.4, 28.5} },
					{ "glf6", {30.4, 30.4} }, { "h25b", {14.8, 15.7} }, { "h25c", {17.7, 15.7} },
					{ "k35e", {42.6, 40.4} }, { "k35r", {42.6, 40.4} }, { "md11", {61.2, 51.7} },
					{ "md80", {45.1, 32.9} }, { "md81", {40.0, 32.9} }, { "md82", {45.1, 32.9} },
					{ "md83", {45.1, 32.9} }, { "md87", {40.6, 32.9} }, { "md88", {45.1, 32.9} },
					{ "md90", {45.1, 35.1} }, { "mi38", {19.9, 21.3} }, { "n262", {25.4, 27.4} },
					{ "p06t", {10.9, 11.5} }, { "p28a", {7.2, 9.2} }, { "p28r", {7.2, 11.0} },
					{ "p46t", {11.5, 12.8} }, { "p68", {11.2, 12.0} }, { "pa31", {10.3, 12.8} },
					{ "pa34", {10.4, 11.8} }, { "pa44", {8.6, 11.9} }, { "pa46", {10.3, 13.1} },
					{ "pc12", {14.4, 16.3} }, { "pc6t", {10.2, 15.0} }, { "r44", {11.7, 10.1} },
					{ "s22t", {7.9, 11.7} }, { "s92", {20.8, 22.0} }, { "sb20", {27.3, 24.7} },
					{ "sf34", {24.7, 20.7} }, { "sr20", {8.1, 11.7} }, { "sr22", {7.9, 11.7} },
					{ "sw4", {19.8, 17.3} }, { "t134", {37.1, 29.0} }, { "t154", {47.0, 37.6} },
					{ "y12", {14.9, 17.3} }
				};
				auto itHard = hardcodedDims.find(acTypeLower);
				if (itHard != hardcodedDims.end()) {
					lengthMeters = itHard->second.first;
					spanMeters = itHard->second.second;
				}
			}

			// If still missing, fill from WTC defaults
			if (lengthMeters <= 0 || spanMeters <= 0) {
				wtcFallbackDims(wtc, lengthMeters, spanMeters);
			}

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
			drawW = std::clamp(drawW, minSize, maxSize);
			drawH = std::clamp(drawH, minSize, maxSize);

			Color tintColor;
			bool applyTint = false;
			if (hasNoFlightPlan)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					getGroundIconColor("nofpl", getGroundIconColor("gate", Color(255, 128, 128, 128))));
				applyTint = true;
			}
			else if (isAirborneTarget)
			{
				if (isDepartureTarget)
				{
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("airborne_departure", getGroundIconColor("depa", Color(255, 240, 240, 240))));
				}
				else
				{
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("airborne_arrival", getGroundIconColor("arr", Color(255, 120, 190, 240))));
				}
				applyTint = true;
			}
			else if (isDepartureTarget)
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("departure_gate", getGroundIconColor("gate", Color(255, 165, 165, 165))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 253, 218, 13)));
					applyTint = true;
					break;
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("stup", Color(255, 253, 218, 13)));
					applyTint = true;
					break;
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("taxi", Color(255, 240, 240, 240)));
					applyTint = true;
					break;
				case GroundStateCategory::Depa:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("depa", getGroundIconColor("taxi", Color(255, 240, 240, 240))));
					applyTint = true;
					break;
				case GroundStateCategory::Nsts:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("nsts", getGroundIconColor("departure_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				default:
					break;
				}
			}
			else
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
				case GroundStateCategory::Nsts:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165))));
					applyTint = true;
					break;
				case GroundStateCategory::Arr:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arr", getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 90, 150, 235)));
					applyTint = true;
					break;
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("arrival_taxi", getGroundIconColor("taxi", Color(255, 70, 195, 120))));
					applyTint = true;
					break;
				default:
					break;
				}
			}

			if (vacdmColorRuleOverrides.hasTargetColor)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					Color(255, vacdmColorRuleOverrides.targetR, vacdmColorRuleOverrides.targetG, vacdmColorRuleOverrides.targetB));
				applyTint = true;
			}

			// Screen-relative heading from pixel forward vector (handles rotated display)
			CPosition nosePosDraw = Haversine(RtPos.GetPosition(), headingDeg, 50.0);
			POINT nosePixDraw = ConvertCoordFromPositionToPixel(nosePosDraw);
			double fx = double(nosePixDraw.x - acPosPix.x);
			double fy = double(nosePixDraw.y - acPosPix.y);
			double screenHeadingDeg = atan2(fy, fx) * 180.0 / M_PI;
			// Adjust because SVG nose is up; rotate so north = 0, east = 90, etc.
			// GDI+ uses screen coords (Y grows down); negate to align with screen vector and SVG nose-up.
			double rotationDeg = screenHeadingDeg + 90.0;

			GraphicsState state = graphics.Save();
			Gdiplus::Matrix m;
			m.Translate(Gdiplus::REAL(acPosPix.x), Gdiplus::REAL(acPosPix.y));
			m.Rotate(Gdiplus::REAL(rotationDeg));
			m.Translate(Gdiplus::REAL(-drawW / 2.0), Gdiplus::REAL(-drawH / 2.0));
			graphics.SetTransform(&m);

			if (applyTint) {
				Gdiplus::ColorMatrix cm = {
					{
						{ static_cast<REAL>(tintColor.GetR()) / 255.0f, 0.0f, 0.0f, 0.0f, 0.0f },
						{ 0.0f, static_cast<REAL>(tintColor.GetG()) / 255.0f, 0.0f, 0.0f, 0.0f },
						{ 0.0f, 0.0f, static_cast<REAL>(tintColor.GetB()) / 255.0f, 0.0f, 0.0f },
						{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
						{ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }
					}
				};
				Gdiplus::ImageAttributes attrs;
				attrs.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
				RectF dest(0.0f, 0.0f, static_cast<REAL>(drawW), static_cast<REAL>(drawH));
				graphics.DrawImage(
					iconBmp,
					dest,
					0.0f,
					0.0f,
					static_cast<Gdiplus::REAL>(iconBmp->GetWidth()),
					static_cast<Gdiplus::REAL>(iconBmp->GetHeight()),
					UnitPixel,
					&attrs);
			}
			else {
				graphics.DrawImage(iconBmp, Gdiplus::REAL(0), Gdiplus::REAL(0), Gdiplus::REAL(drawW), Gdiplus::REAL(drawH));
			}
			graphics.Restore(state);
			iconSize = int(max(drawW, drawH));
		}
		else
		{
			auto getGroundIconColor = [&](const char* key, Color fallback) -> Color {
				const Value& profile = CurrentConfig->getActiveProfile();
				if (profile.HasMember("targets")) {
					const Value& targets = profile["targets"];
					if (targets.HasMember("ground_icons") && targets["ground_icons"].HasMember(key)) {
						return CurrentConfig->getConfigColor(targets["ground_icons"][key]);
					}
				}
				return fallback;
			};

			Color tintColor;
			bool applyTint = false;
			if (hasNoFlightPlan)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					getGroundIconColor("nofpl", getGroundIconColor("gate", Color(255, 128, 128, 128))));
				applyTint = true;
			}
			else if (isAirborneTarget)
			{
				if (isDepartureTarget)
				{
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("airborne_departure", getGroundIconColor("depa", Color(255, 240, 240, 240))));
				}
				else
				{
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("airborne_arrival", getGroundIconColor("arr", Color(255, 120, 190, 240))));
				}
				applyTint = true;
			}
			else if (isDepartureTarget)
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("departure_gate", getGroundIconColor("gate", Color(255, 165, 165, 165))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 253, 218, 13)));
					applyTint = true;
					break;
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("stup", Color(255, 253, 218, 13)));
					applyTint = true;
					break;
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("taxi", Color(255, 240, 240, 240)));
					applyTint = true;
					break;
				case GroundStateCategory::Depa:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("depa", getGroundIconColor("taxi", Color(255, 240, 240, 240))));
					applyTint = true;
					break;
				case GroundStateCategory::Nsts:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("nsts", getGroundIconColor("departure_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				default:
					break;
				}
			}
			else
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Gate:
				case GroundStateCategory::Nsts:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165))));
					applyTint = true;
					break;
				case GroundStateCategory::Arr:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arr", getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 90, 150, 235)));
					applyTint = true;
					break;
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("arrival_taxi", getGroundIconColor("taxi", Color(255, 70, 195, 120))));
					applyTint = true;
					break;
				default:
					break;
				}
			}

			if (vacdmColorRuleOverrides.hasTargetColor)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					Color(255, vacdmColorRuleOverrides.targetR, vacdmColorRuleOverrides.targetG, vacdmColorRuleOverrides.targetB));
				applyTint = true;
			}

			const double pixPerMeter = framePixPerMeter;

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
			lenPx = std::clamp(lenPx * symbolSizeScale, 1.0, 220.0);
			halfWidthPx = std::clamp(halfWidthPx * symbolSizeScale, 1.0, 110.0);
			if (pixPerMeter > 0.0)
			{
				lenMetersUsed = lenPx / pixPerMeter;
				halfWidthMetersUsed = halfWidthPx / pixPerMeter;
			}

			auto wrap360 = [](double deg) {
				double wrapped = fmod(deg, 360.0);
				return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
			};

			const Color drawColor = applyTint ? tintColor : ColorManager->get_corrected_color("symbol", Gdiplus::Color::White);
			if (useDiamondIconStyle)
			{
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

				CPosition nosePosDraw = Haversine(RtPos.GetPosition(), headingDeg, 50.0);
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
				iconSize = int(max(12.0, diagonalPx));
			}
			else
			{
				auto move = [&](const CPosition& start, double bearingDeg, double distanceMeters) {
					return BetterHarversine(start, wrap360(bearingDeg), distanceMeters);
				};

				CPosition acPos = RtPos.GetPosition();
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
				iconSize = int(max(12.0, lenPx + halfWidthPx));
			}
		}

		// Predicted Track Line
		// It starts 20 seconds away from the ac
		if (reportedGs > 50 && PredictedLength > 0)
		{
			double d = double(rt.GetPosition().GetReportedGS()*0.514444) * 10;
			CPosition AwayBase = BetterHarversine(rt.GetPosition().GetPosition(), rt.GetTrackHeading(), d);

			d = double(rt.GetPosition().GetReportedGS()*0.514444) * (PredictedLength * 60) - 10;
			CPosition PredictedEnd = BetterHarversine(AwayBase, rt.GetTrackHeading(), d);

			dc.MoveTo(ConvertCoordFromPositionToPixel(AwayBase));
			dc.LineTo(ConvertCoordFromPositionToPixel(PredictedEnd));
		}

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
		AddScreenObject(DRAWING_AC_SYMBOL, rt.GetCallsign(), { acPosPix.x - hitSize / 2, acPosPix.y - hitSize / 2, acPosPix.x + hitSize / 2, acPosPix.y + hitSize / 2 }, false, AcisCorrelated ? GetBottomLine(rt.GetCallsign()).c_str() : rt.GetSystemID());
	}

#pragma endregion Drawing of the symbols

	TimePopupData.clear();
	AcOnRunway.clear();
	ColorAC.clear();
	tagAreas.clear();
	tagCollisionAreas.clear();

	RimcasInstance->OnRefreshEnd(this, CurrentConfig->getActiveProfile()["rimcas"]["rimcas_stage_two_speed_threshold"].GetInt());

	graphics.SetSmoothingMode(SmoothingModeDefault);

	RenderTags(graphics, dc, frameProModeEnabled, frameTagDataCache, frameVacdmLookupCache);

	// Releasing the hDC after the drawing
	graphics.ReleaseHDC(hDC);

	CBrush BrushGrey(RGB(150, 150, 150));
	COLORREF oldColor = dc.SetTextColor(RGB(33, 33, 33));

	int TextHeight = dc.GetTextExtent("60").cy;
	VSMR_REFRESH_LOG("RIMCAS Loop");
	for (std::map<string, bool>::iterator it = RimcasInstance->MonitoredRunwayArr.begin(); it != RimcasInstance->MonitoredRunwayArr.end(); ++it)
	{
		if (!it->second || RimcasInstance->TimeTable[it->first].empty())
			continue;

		vector<int> TimeDefinition = RimcasInstance->CountdownDefinition;
		if (isLVP)
			TimeDefinition = RimcasInstance->CountdownDefinitionLVP;

		if (TimePopupAreas.find(it->first) == TimePopupAreas.end())
			TimePopupAreas[it->first] = { 300, 300, 430, 300+LONG(TextHeight*(TimeDefinition.size()+1)) };

		CRect CRectTime = TimePopupAreas[it->first];
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

			tempS = std::to_string(Time) + ": " + RimcasInstance->TimeTable[it->first][Time];
			if (RimcasInstance->AcColor.find(RimcasInstance->TimeTable[it->first][Time]) != RimcasInstance->AcColor.end())
			{
				CBrush RimcasBrush(RimcasInstance->GetAircraftColor(RimcasInstance->TimeTable[it->first][Time],
					Color::Black,
					Color::Black,
					CurrentConfig->getConfigColor(CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_one"]),
					CurrentConfig->getConfigColor(CurrentConfig->getActiveProfile()["rimcas"]["background_color_stage_two"])).ToCOLORREF()
					);

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

	VSMR_REFRESH_LOG("Menu bar lists");


	if (ShowLists["Conflict Alert ARR"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Conflict Alert ARR"], "CA Arrival", 1);
		for (std::map<string, CRimcas::RunwayAreaType>::iterator it = RimcasInstance->RunwayAreas.begin(); it != RimcasInstance->RunwayAreas.end(); ++it)
		{
			GetPlugIn()->AddPopupListElement(it->first.c_str(), "", RIMCAS_CA_ARRIVAL_FUNC, false, RimcasInstance->MonitoredRunwayArr[it->first.c_str()]);
		}
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Conflict Alert ARR"] = false;
	}

	if (ShowLists["Conflict Alert DEP"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Conflict Alert DEP"], "CA Departure", 1);
		for (std::map<string, CRimcas::RunwayAreaType>::iterator it = RimcasInstance->RunwayAreas.begin(); it != RimcasInstance->RunwayAreas.end(); ++it)
		{
			GetPlugIn()->AddPopupListElement(it->first.c_str(), "", RIMCAS_CA_MONITOR_FUNC, false, RimcasInstance->MonitoredRunwayDep[it->first.c_str()]);
		}
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Conflict Alert DEP"] = false;
	}

	if (ShowLists["Runway closed"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Runway closed"], "Runway Closed", 1);
		for (std::map<string, CRimcas::RunwayAreaType>::iterator it = RimcasInstance->RunwayAreas.begin(); it != RimcasInstance->RunwayAreas.end(); ++it)
		{
			GetPlugIn()->AddPopupListElement(it->first.c_str(), "", RIMCAS_CLOSED_RUNWAYS_FUNC, false, RimcasInstance->ClosedRunway[it->first.c_str()]);
		}
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Runway closed"] = false;
	}

	if (ShowLists["Visibility"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Visibility"], "Visibility", 1);
		GetPlugIn()->AddPopupListElement("Normal", "", RIMCAS_UPDATE_LVP, false, int(!isLVP));
		GetPlugIn()->AddPopupListElement("Low", "", RIMCAS_UPDATE_LVP, false, int(isLVP));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Visibility"] = false;
	}

	if (ShowLists["Active Alerts"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Active Alerts"], "Active Alerts", 1);
		GetPlugIn()->AddPopupListElement("NO PUSH", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("NO PUSH") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("NO TAXI", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("NO TAXI") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("NO TKOF", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("NO TKOF") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("STAT RPA", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("STAT RPA") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("RWY INC", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("RWY INC") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("RWY TYPE", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("RWY TYPE") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("RWY CLSD", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("RWY CLSD") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("HIGH SPD", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("HIGH SPD") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("EMERG", "", RIMCAS_ALERTS_TOGGLE_FUNC, false, RimcasInstance->inactiveAlerts.find("EMERG") == RimcasInstance->inactiveAlerts.end());
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Active Alerts"] = false;
	}

	if (ShowLists["Profiles"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Profiles"], "Profiles", 1);
		vector<string> allProfiles = CurrentConfig->getAllProfiles();
		for (std::vector<string>::iterator it = allProfiles.begin(); it != allProfiles.end(); ++it) {
			GetPlugIn()->AddPopupListElement(it->c_str(), "", RIMCAS_UPDATE_PROFILE, false, int(CurrentConfig->isItActiveProfile(it->c_str())));
		}
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Profiles"] = false;
	}

	if (ShowLists["Colour Settings"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Colour Settings"], "Colour Settings", 1);
		GetPlugIn()->AddPopupListElement("Day", "", RIMCAS_UPDATE_BRIGHNESS, false, int(ColorSettingsDay));
		GetPlugIn()->AddPopupListElement("Night", "", RIMCAS_UPDATE_BRIGHNESS, false, int(!ColorSettingsDay));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Colour Settings"] = false;
	}

	if (ShowLists["Label Font Size"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Label Font Size"], "Label Font Size", 1);
		GetPlugIn()->AddPopupListElement("Size 1", "", RIMCAS_UPDATE_FONTS, false, int(bool(currentFontSize == 1)));
		GetPlugIn()->AddPopupListElement("Size 2", "", RIMCAS_UPDATE_FONTS, false, int(bool(currentFontSize == 2)));
		GetPlugIn()->AddPopupListElement("Size 3", "", RIMCAS_UPDATE_FONTS, false, int(bool(currentFontSize == 3)));
		GetPlugIn()->AddPopupListElement("Size 4", "", RIMCAS_UPDATE_FONTS, false, int(bool(currentFontSize == 4)));
		GetPlugIn()->AddPopupListElement("Size 5", "", RIMCAS_UPDATE_FONTS, false, int(bool(currentFontSize == 5)));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Label Font Size"] = false;
	}

	if (ShowLists["Tag Font"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Tag Font"], "Tag Font", 1);

		auto toLowerCopy = [](std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		};

		const std::string currentFontName = GetActiveTagFontName();
		const std::string currentFontNameLower = toLowerCopy(currentFontName);
		std::vector<std::string> availableFonts = GetAvailableTagFonts();

		bool containsCurrentFont = false;
		for (const std::string& fontName : availableFonts)
		{
			if (toLowerCopy(fontName) == currentFontNameLower)
			{
				containsCurrentFont = true;
				break;
			}
		}

		if (!currentFontName.empty() && !containsCurrentFont)
			availableFonts.insert(availableFonts.begin(), currentFontName);

		for (const std::string& fontName : availableFonts)
		{
			GetPlugIn()->AddPopupListElement(fontName.c_str(), "", RIMCAS_UPDATE_TAG_FONT, false, int(bool(toLowerCopy(fontName) == currentFontNameLower)));
		}

		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Tag Font"] = false;
	}

	if (ShowLists["GRND Trail Dots"]) {
		GetPlugIn()->OpenPopupList(ListAreas["GRND Trail Dots"], "GRND Trail Dots", 1);
		GetPlugIn()->AddPopupListElement("0", "", RIMCAS_UPDATE_GND_TRAIL, false, int(bool(Trail_Gnd == 0)));
		GetPlugIn()->AddPopupListElement("2", "", RIMCAS_UPDATE_GND_TRAIL, false, int(bool(Trail_Gnd == 2)));
		GetPlugIn()->AddPopupListElement("4", "", RIMCAS_UPDATE_GND_TRAIL, false, int(bool(Trail_Gnd == 4)));
		GetPlugIn()->AddPopupListElement("8", "", RIMCAS_UPDATE_GND_TRAIL, false, int(bool(Trail_Gnd == 8)));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["GRND Trail Dots"] = false;
	}

	if (ShowLists["APPR Trail Dots"]) {
		GetPlugIn()->OpenPopupList(ListAreas["APPR Trail Dots"], "APPR Trail Dots", 1);
		GetPlugIn()->AddPopupListElement("0", "", RIMCAS_UPDATE_APP_TRAIL, false, int(bool(Trail_App == 0)));
		GetPlugIn()->AddPopupListElement("4", "", RIMCAS_UPDATE_APP_TRAIL, false, int(bool(Trail_App == 4)));
		GetPlugIn()->AddPopupListElement("8", "", RIMCAS_UPDATE_APP_TRAIL, false, int(bool(Trail_App == 8)));
		GetPlugIn()->AddPopupListElement("12", "", RIMCAS_UPDATE_APP_TRAIL, false, int(bool(Trail_App == 12)));
		GetPlugIn()->AddPopupListElement("16", "", RIMCAS_UPDATE_APP_TRAIL, false, int(bool(Trail_App == 16)));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["APPR Trail Dots"] = false;
	}

	if (ShowLists["Predicted Track Line"]) {
		GetPlugIn()->OpenPopupList(ListAreas["Predicted Track Line"], "Predicted Track Line", 1);
		GetPlugIn()->AddPopupListElement("0", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 0)));
		GetPlugIn()->AddPopupListElement("1", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 1)));
		GetPlugIn()->AddPopupListElement("2", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 2)));
		GetPlugIn()->AddPopupListElement("3", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 3)));
		GetPlugIn()->AddPopupListElement("4", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 4)));
		GetPlugIn()->AddPopupListElement("5", "", RIMCAS_UPDATE_PTL, false, int(bool(PredictedLength == 5)));
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Predicted Track Line"] = false;
	}

	if (ShowLists["Brightness"])
	{
		GetPlugIn()->OpenPopupList(ListAreas["Brightness"], "Brightness", 1);
		GetPlugIn()->AddPopupListElement("Label", "", RIMCAS_OPEN_LIST, false);
		GetPlugIn()->AddPopupListElement("Symbol", "", RIMCAS_OPEN_LIST, false);
		GetPlugIn()->AddPopupListElement("Afterglow", "", RIMCAS_OPEN_LIST, false);
		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Brightness"] = false;
	}

	if (ShowLists["Label"])
	{
		GetPlugIn()->OpenPopupList(ListAreas["Label"], "Label Brightness", 1);
		for(int i = CColorManager::bounds_low(); i <= CColorManager::bounds_high(); i +=10)
			GetPlugIn()->AddPopupListElement(std::to_string(i).c_str(), "", RIMCAS_BRIGHTNESS_LABEL, false, int(bool(i == ColorManager->get_brightness("label"))));

		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Label"] = false;
	}

	if (ShowLists["Symbol"])
	{
		GetPlugIn()->OpenPopupList(ListAreas["Symbol"], "Symbol Brightness", 1);
		for (int i = CColorManager::bounds_low(); i <= CColorManager::bounds_high(); i += 10)
			GetPlugIn()->AddPopupListElement(std::to_string(i).c_str(), "", RIMCAS_BRIGHTNESS_SYMBOL, false, int(bool(i == ColorManager->get_brightness("symbol"))));

		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Symbol"] = false;
	}

	if (ShowLists["Afterglow"])
	{
		GetPlugIn()->OpenPopupList(ListAreas["Afterglow"], "Afterglow Brightness", 1);
		for (int i = CColorManager::bounds_low(); i <= CColorManager::bounds_high(); i += 10)
			GetPlugIn()->AddPopupListElement(std::to_string(i).c_str(), "", RIMCAS_BRIGHTNESS_AFTERGLOW, false, int(bool(i == ColorManager->get_brightness("afterglow"))));

		GetPlugIn()->AddPopupListElement("Close", "", RIMCAS_CLOSE, false, 2, false, true);
		ShowLists["Afterglow"] = false;
	}

	VSMR_REFRESH_LOG("QRD");

	//---------------------------------
	// QRD
	//---------------------------------

	if (QDMenabled || QDMSelectEnabled || (DistanceToolActive && ActiveDistance.first != "")) {
		CPen Pen(PS_SOLID, 1, RGB(255, 255, 255));
		CPen *oldPen = dc.SelectObject(&Pen);

		POINT AirportPos = ConvertCoordFromPositionToPixel(AirportPositions[getActiveAirport()]);
		CPosition AirportCPos = AirportPositions[getActiveAirport()];
		if (QDMSelectEnabled)
		{
			AirportPos = QDMSelectPt;
			AirportCPos = ConvertCoordFromPixelToPosition(QDMSelectPt);
		}
		if (DistanceToolActive)
		{
			CPosition r = GetPlugIn()->RadarTargetSelect(ActiveDistance.first.c_str()).GetPosition().GetPosition();
			AirportPos = ConvertCoordFromPositionToPixel(r);
			AirportCPos = r;
		}
		dc.MoveTo(AirportPos);
		POINT point = mouseLocation;
		dc.LineTo(point);

		CPosition CursorPos = ConvertCoordFromPixelToPosition(point);
		double Distance = AirportCPos.DistanceTo(CursorPos);
		double Bearing = AirportCPos.DirectionTo(CursorPos);
	
		Gdiplus::Pen WhitePen(Color::White);
		graphics.DrawEllipse(&WhitePen, point.x - 5, point.y - 5, 10, 10);

		Distance = Distance / 0.00053996f;

		Distance = round(Distance * 10) / 10;

		Bearing = round(Bearing * 10) / 10;

		POINT TextPos = { point.x + 20, point.y };

		if (!DistanceToolActive)
		{
			string distances = std::to_string(Distance);
			size_t decimal_pos = distances.find(".");
			distances = distances.substr(0, decimal_pos + 2);

			string bearings = std::to_string(Bearing);
			decimal_pos = bearings.find(".");
			bearings = bearings.substr(0, decimal_pos + 2);

			string text = bearings;
			text += "Ãƒâ€šÃ‚Â° / ";
			text += distances;
			text += "m";
			COLORREF old_color = dc.SetTextColor(RGB(255, 255, 255));
			dc.TextOutA(TextPos.x, TextPos.y, text.c_str());
			dc.SetTextColor(old_color);
		}

		dc.SelectObject(oldPen);
		RequestRefresh();
	}

	// Distance tools here
	for (auto&& kv : DistanceTools)
	{
		CRadarTarget one = GetPlugIn()->RadarTargetSelect(kv.first.c_str());
		CRadarTarget two = GetPlugIn()->RadarTargetSelect(kv.second.c_str());

		if (!isVisible(one) || !isVisible(two))
			continue;

		CPen Pen(PS_SOLID, 1, RGB(255, 255, 255));
		CPen *oldPen = dc.SelectObject(&Pen);

		POINT onePoint = ConvertCoordFromPositionToPixel(one.GetPosition().GetPosition());
		POINT twoPoint = ConvertCoordFromPositionToPixel(two.GetPosition().GetPosition());

		dc.MoveTo(onePoint);
		dc.LineTo(twoPoint);

		POINT TextPos = { twoPoint.x + 20, twoPoint.y };

		double Distance = one.GetPosition().GetPosition().DistanceTo(two.GetPosition().GetPosition());
		double Bearing = one.GetPosition().GetPosition().DirectionTo(two.GetPosition().GetPosition());

		string distances = std::to_string(Distance);
		size_t decimal_pos = distances.find(".");
		distances = distances.substr(0, decimal_pos + 2);

		string bearings = std::to_string(Bearing);
		decimal_pos = bearings.find(".");
		bearings = bearings.substr(0, decimal_pos + 2);

		string text = bearings;
		text += "Ãƒâ€šÃ‚Â° / ";
		text += distances;
		text += "nm";
		COLORREF old_color = dc.SetTextColor(RGB(0, 0, 0));

		CRect ClickableRect = { TextPos.x - 2, TextPos.y, TextPos.x + dc.GetTextExtent(text.c_str()).cx + 2, TextPos.y + dc.GetTextExtent(text.c_str()).cy };
		graphics.FillRectangle(&SolidBrush(Color(127, 122, 122)), CopyRect(ClickableRect));
		dc.Draw3dRect(ClickableRect, RGB(75, 75, 75), RGB(45, 45, 45));
		dc.TextOutA(TextPos.x, TextPos.y, text.c_str());

		AddScreenObject(RIMCAS_DISTANCE_TOOL, string(kv.first+","+kv.second).c_str(), ClickableRect, false, "");

		dc.SetTextColor(old_color);

		dc.SelectObject(oldPen);
	}

	//---------------------------------
	// Drawing the toolbar
	//---------------------------------

	VSMR_REFRESH_LOG("Menu Bar");

	COLORREF qToolBarColor = RGB(127, 122, 122);

	// Drawing the toolbar on the top
	CRect ToolBarAreaTop(RadarArea.left, RadarArea.top, RadarArea.right, RadarArea.top + 20);
	dc.FillSolidRect(ToolBarAreaTop, qToolBarColor);

	COLORREF oldTextColor = dc.SetTextColor(RGB(0, 0, 0));

	int offset = 2;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, getActiveAirport().c_str());
	AddScreenObject(RIMCAS_ACTIVE_AIRPORT, "ActiveAirport", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent(getActiveAirport().c_str()).cx, ToolBarAreaTop.top + 4 + dc.GetTextExtent(getActiveAirport().c_str()).cy }, false, "Active Airport");

	offset += dc.GetTextExtent(getActiveAirport().c_str()).cx + 10;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, "Display");
	AddScreenObject(RIMCAS_MENU, "DisplayMenu", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent("Display").cx, ToolBarAreaTop.top + 4 + dc.GetTextExtent("Display").cy }, false, "Display menu");

	offset += dc.GetTextExtent("Display").cx + 10;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, "Target");
	AddScreenObject(RIMCAS_MENU, "TargetMenu", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent("Target").cx, ToolBarAreaTop.top + 4 + dc.GetTextExtent("Target").cy }, false, "Target menu");

	offset += dc.GetTextExtent("Target").cx + 10;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, "Colours");
	AddScreenObject(RIMCAS_MENU, "ColourMenu", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent("Colour").cx, ToolBarAreaTop.top + 4 + dc.GetTextExtent("Colour").cy }, false, "Colour menu");

	offset += dc.GetTextExtent("Colours").cx + 10;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, "Alerts");
	AddScreenObject(RIMCAS_MENU, "RIMCASMenu", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent("Alerts").cx, ToolBarAreaTop.top + 4 + +dc.GetTextExtent("Alerts").cy }, false, "RIMCAS menu");

	offset += dc.GetTextExtent("Alerts").cx + 10;
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, "/");
	CRect barDistanceRect = { ToolBarAreaTop.left + offset - 2, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent("/").cx, ToolBarAreaTop.top + 4 + +dc.GetTextExtent("/").cy };
	if (DistanceToolActive)
	{
		graphics.DrawRectangle(&Pen(Color::White), CopyRect(barDistanceRect));
	}
	AddScreenObject(RIMCAS_MENU, "/", barDistanceRect, false, "Distance tool");

	dc.SetTextColor(oldTextColor);

	//
	// Tag deconflicting
	//

	VSMR_REFRESH_LOG("Tag deconfliction loop");

	for (const auto areas : tagCollisionAreas)
	{
		if (!CurrentConfig->getActiveProfile()["labels"]["auto_deconfliction"].GetBool())
			break;

		if (IsTagBeingDragged(areas.first))
			continue;

		if (RecentlyAutoMovedTags.find(areas.first) != RecentlyAutoMovedTags.end())
		{
			double t = ((double)clock() - RecentlyAutoMovedTags[areas.first]) / ((double)CLOCKS_PER_SEC);
			if (t >= 0.8) {
				RecentlyAutoMovedTags.erase(areas.first);
			} else
			{
				continue;
			}
		}

		// We need to see wether the rotation will be clockwise or anti-clockwise

		bool isAntiClockwise = false;

		for (const auto area2 : tagCollisionAreas)
		{
			if (areas.first == area2.first)
				continue;

			if (IsTagBeingDragged(area2.first))
				continue;

			CRect h;

			if (h.IntersectRect(tagCollisionAreas[areas.first], area2.second))
			{
				if (areas.second.left <= area2.second.left)
				{
					isAntiClockwise = true;
				}

				break;
			}
		}

		// We then rotate the tags until we did a 360 or there is no more conflicts

		POINT acPosPix = ConvertCoordFromPositionToPixel(GetPlugIn()->RadarTargetSelect(areas.first.c_str()).GetPosition().GetPosition());
		int lenght = LeaderLineDefaultlenght;
		if (TagLeaderLineLength.find(areas.first) != TagLeaderLineLength.end())
			lenght = TagLeaderLineLength[areas.first];

		int width = areas.second.Width();
		int height = areas.second.Height();

		for (double rotated = 0.0; abs(rotated) <= 360.0;)
		{
			// We first rotate the tag
			double newangle = fmod(TagAngles[areas.first] + rotated, 360.0f);

			POINT TagCenter;
			TagCenter.x = long(acPosPix.x + float(lenght * cos(DegToRad(newangle))));
			TagCenter.y = long(acPosPix.y + float(lenght * sin(DegToRad(newangle))));

			CRect NewRectangle(TagCenter.x - (width / 2), TagCenter.y - (height / 2), TagCenter.x + (width / 2), TagCenter.y + (height / 2));
			NewRectangle.NormalizeRect();

			// Assume there is no conflict, then try again

			bool isTagConflicing = false;

			for (const auto area2 : tagCollisionAreas)
			{
				if (areas.first == area2.first)
					continue;

				if (IsTagBeingDragged(area2.first))
					continue;

				CRect h;

				if (h.IntersectRect(NewRectangle, area2.second))
				{
					isTagConflicing = true;
					break;
				}
			}

			if (!isTagConflicing)
			{
				double finalAngle = fmod(TagAngles[areas.first] + rotated, 360.0f);
				TagAngles[areas.first] = finalAngle;

				POINT newCenter = NewRectangle.CenterPoint();
				POINT newOffset = { newCenter.x - acPosPix.x, newCenter.y - acPosPix.y };
				TagsOffsets[areas.first] = newOffset;

				tagCollisionAreas[areas.first] = NewRectangle;
				RecentlyAutoMovedTags[areas.first] = clock();
				break;
			}

			if (isAntiClockwise)
				rotated -= 22.5f;
			else
				rotated += 22.5f;
		}
	}

	//
	// App windows
	//

	VSMR_REFRESH_LOG("App window rendering");

	for (std::map<int, bool>::iterator it = appWindowDisplays.begin(); it != appWindowDisplays.end(); ++it)
	{
		if (!it->second)
			continue;

		int appWindowId = it->first;
		appWindows[appWindowId]->render(hDC, this, &graphics, mouseLocation, DistanceTools);
	}

	dc.Detach();

	VSMR_REFRESH_LOG("END "+ string(__FUNCSIG__));

}

// ReSharper restore CppMsExtAddressOfClassRValue

//---EuroScopePlugInExitCustom-----------------------------------------------

void CSMRRadar::EuroScopePlugInExitCustom()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

		CloseProfileEditorWindow(false);
		DestroyProfileEditorWindow();

		if (smrCursor != nullptr && smrCursor != NULL)
		{
			SetWindowLong(pluginWindow, GWL_WNDPROC, (LONG)gSourceProc);
		}
}
