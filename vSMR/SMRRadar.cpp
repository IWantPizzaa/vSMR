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

// Cursor state shared by radar screen instances (managed on the UI thread).

bool initCursor = true;
HCURSOR smrCursor = NULL;
bool standardCursor; // True when the default arrow cursor is active.
bool customCursor; // True when the plugin-specific cursor theme is enabled.
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

	// Initializing randomizer
	srand(static_cast<unsigned>(time(nullptr)));
	clock_init = clock();
	clock_final = clock_init;

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
	for (auto& fontEntry : customFonts)
	{
		delete fontEntry.second;
	}
	customFonts.clear();

	delete ColorManager;
	ColorManager = nullptr;

	delete RimcasInstance;
	RimcasInstance = nullptr;

	delete Callsigns;
	Callsigns = nullptr;

	// Shutting down GDI+
	GdiplusShutdown(m_gdiplusToken);
	delete CurrentConfig;
	CurrentConfig = nullptr;
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
	const Value& activeProfile = CurrentConfig->getActiveProfile();
	const Value* rimcasConfig = nullptr;
	if (activeProfile.IsObject() && activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		rimcasConfig = &activeProfile["rimcas"];

	// Inactive alerts
	unordered_set inactiveAlerts = CurrentConfig->getInactiveAlert();
	RimcasInstance->setInactiveAlerts(inactiveAlerts);

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
	// Backward-compatible profile migration and normalization:
	// ensure required nested objects, color entries and editor settings exist.
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
	renameMemberIfPresent(filters, "night_alpha_setting", "night_overlay_alpha");
	ensureIntMember(filters, "max_altitude_ft", 5500, 0, 80000);
	ensureIntMember(filters, "max_speed_kt", 250, 0, 2000);
	ensureIntMember(filters, "radar_range_nm", 999, 1, 9999);
	ensureIntMember(filters, "night_overlay_alpha", 110, 0, 255);
	Value& proMode = ensureObjectMember(filters, "pro_mode");
	renameMemberIfPresent(proMode, "enable", "enabled");
	renameMemberIfPresent(proMode, "do_not_autocorrelate_squawks", "blocked_auto_correlate_squawks");
	ensureBoolMember(proMode, "enabled", false);
	ensureBoolMember(proMode, "accept_pilot_squawk", true);
	ensureStringArrayMember(proMode, "blocked_auto_correlate_squawks", defaultDoNotAutocorrelateSquawks);

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

	ensureStatusDefinitionEntries("taxi");
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

map<string, string> CSMRRadar::GenerateTagData(CRadarTarget rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, bool useSpeedForGates, string ActiveAirport, const std::string& stableCallsign)
{
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
	{
		if (hasRadarTarget)
		{
			CRadarTargetPositionData currentPos = rtPos;
			CRadarTargetPositionData previousPos = rt.GetPreviousPosition(currentPos);
			if (currentPos.IsValid() && previousPos.IsValid())
				delta_fl = currentPos.GetFlightLevel() - previousPos.GetFlightLevel();
		}
	}
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
		if (strlen(groundState) != 0)
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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_SETCURSOR:
		SetCursor(smrCursor);
		return true;
	default:
		if (gSourceProc != nullptr)
			return CallWindowProc(gSourceProc, hwnd, uMsg, wParam, lParam);
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
}

void CSMRRadar::OnRefresh(HDC hDC, int Phase)
{
	VSMR_REFRESH_LOG(string(__FUNCSIG__));
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

	if (ColorManager == nullptr)
	{
		Logger::info("OnRefresh: ColorManager was null; recreating");
		ColorManager = new CColorManager();
		if (ColorManager == nullptr)
			return;
	}
	// Refresh pipeline is phase-driven by EuroScope. Cursor/theme work is kept here on the UI thread.
	if (initCursor)
	{
		if (customCursor) {
			smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_SMRCURSOR), IMAGE_CURSOR, 0, 0, LR_SHARED));
			// EuroScope/MFC can still override the cursor occasionally; we therefore reapply it via window proc hook.

		}
		else {
			smrCursor = (HCURSOR)::LoadCursor(NULL, IDC_ARROW);
		}

		if (smrCursor != nullptr)
		{
			pluginWindow = GetActiveWindow();
			if (pluginWindow != nullptr && ::IsWindow(pluginWindow))
			{
				WNDPROC previousProc = reinterpret_cast<WNDPROC>(
					::SetWindowLongPtr(pluginWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
				if (previousProc != nullptr)
					gSourceProc = previousProc;
				else
					pluginWindow = nullptr;
			}
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

			int nightAlpha = 110;
			if (CurrentConfig != nullptr)
			{
				const Value& profile = CurrentConfig->getActiveProfile();
				if (profile.IsObject() &&
					profile.HasMember("filters") &&
					profile["filters"].IsObject())
				{
					const Value& filters = profile["filters"];
					if (filters.HasMember("night_overlay_alpha") && filters["night_overlay_alpha"].IsInt())
					{
						nightAlpha = filters["night_overlay_alpha"].GetInt();
					}
					else if (filters.HasMember("night_alpha_setting") && filters["night_alpha_setting"].IsInt())
					{
						nightAlpha = filters["night_alpha_setting"].GetInt();
					}
				}
			}
			nightAlpha = std::clamp(nightAlpha, 0, 255);
			SolidBrush AlphaBrush(Color(nightAlpha, 0, 0, 0));

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

		RefreshMapContent();
	}


	if (!QDMenabled && !QDMSelectEnabled)
	{
		POINT p;
		if (GetCursorPos(&p)) {
			HWND activeWindow = GetActiveWindow();
			if (activeWindow != nullptr && ScreenToClient(activeWindow, &p)) {
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
		const char* airportName = apt.GetName();
		if (airportName == nullptr || airportName[0] == '\0')
			continue;

		CPosition Pos;
		apt.GetPosition(&Pos, 0);
		AirportPositions[string(airportName)] = Pos;
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
		const char* runwayAirportName = rwy.GetAirportName();
		if (runwayAirportName == nullptr || runwayAirportName[0] == '\0')
			continue;

		if (startsWith(getActiveAirport().c_str(), runwayAirportName)) {

			const char* runwayNameA = rwy.GetRunwayName(0);
			const char* runwayNameB = rwy.GetRunwayName(1);
			if (runwayNameA == nullptr || runwayNameB == nullptr || runwayNameA[0] == '\0' || runwayNameB[0] == '\0')
				continue;

			CPosition Left;
			rwy.GetPosition(&Left, 1);
			CPosition Right;
			rwy.GetPosition(&Right, 0);

			string runway_name = runwayNameA;
			string runway_name2 = runwayNameB;

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
	bool frameProModeEnabled = false;
	bool frameUseAspeedForGate = false;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject())
		{
			if (profile.HasMember("filters") &&
				profile["filters"].IsObject() &&
				profile["filters"].HasMember("pro_mode") &&
				profile["filters"]["pro_mode"].IsObject())
			{
				const Value& proMode = profile["filters"]["pro_mode"];
				if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
				{
					frameProModeEnabled = proMode["enabled"].GetBool();
				}
				else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
				{
					frameProModeEnabled = proMode["enable"].GetBool();
				}
			}

			if (profile.HasMember("labels") &&
				profile["labels"].IsObject() &&
				profile["labels"].HasMember("use_speed_for_gate") &&
				profile["labels"]["use_speed_for_gate"].IsBool())
			{
				frameUseAspeedForGate = profile["labels"]["use_speed_for_gate"].GetBool();
			}
			else if (profile.HasMember("labels") &&
				profile["labels"].IsObject() &&
				profile["labels"].HasMember("use_aspeed_for_gate") &&
				profile["labels"]["use_aspeed_for_gate"].IsBool())
			{
				frameUseAspeedForGate = profile["labels"]["use_aspeed_for_gate"].GetBool();
			}
		}
	}
	const int frameTransitionAltitude = GetPlugIn()->GetTransitionAltitude();
	const std::string frameActiveAirport = getActiveAirport();
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
				overrides.targetA = rule.targetA;
			}
			if (rule.applyTag)
			{
				overrides.hasTagColor = true;
				overrides.tagR = rule.tagR;
				overrides.tagG = rule.tagG;
				overrides.tagB = rule.tagB;
				overrides.tagA = rule.tagA;
			}
			if (rule.applyText)
			{
				overrides.hasTextColor = true;
				overrides.textR = rule.textR;
				overrides.textG = rule.textG;
				overrides.textB = rule.textB;
				overrides.textA = rule.textA;
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
		const char* rtCallsignRaw = rt.GetCallsign();
		if (rtCallsignRaw == nullptr || rtCallsignRaw[0] == '\0')
		{
			static bool loggedMissingRefreshCallsign = false;
			if (!loggedMissingRefreshCallsign)
			{
				Logger::info("OnRefresh: skipped target with missing callsign");
				loggedMissingRefreshCallsign = true;
			}
			continue;
		}
		const std::string rtCallsign = rtCallsignRaw;
		auto iconVerboseStep = [&](const std::string& step)
		{
			if (!Logger::is_verbose_mode())
				return;
			Logger::info("IconRender: " + rtCallsign + " " + step);
		};

		CRadarTargetPositionData RtPos = rt.GetPosition();
		if (!RtPos.IsValid())
			continue;

		int reportedGs = RtPos.GetReportedGS();
		bool isAcDisplayed = isVisible(rt);

		if (!isAcDisplayed)
			continue;
		iconVerboseStep("begin");

		CFlightPlan iconFp = GetPlugIn()->FlightPlanSelect(rtCallsign.c_str());
		bool AcisCorrelated = IsCorrelated(iconFp, rt);
		RimcasInstance->OnRefresh(rt, this, AcisCorrelated, isLVP);

		POINT acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());

		if (rt.GetGS() > 5) {
			CRadarTargetPositionData pAcPos = rt.GetPosition();

			for (int i = 1; i <= 2; i++) {
				CRadarTargetPositionData previousTrailPos = rt.GetPreviousPosition(pAcPos);
				if (!previousTrailPos.IsValid())
					break;

				pAcPos = previousTrailPos;
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
				if (!previousPos.IsValid())
					break;

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
			for (unsigned int i = 0; i < Patatoides[rtCallsign].points.size(); i++)
			{
				CPosition pos;
				pos.m_Latitude = Patatoides[rtCallsign].points[i].x;
				pos.m_Longitude = Patatoides[rtCallsign].points[i].y;

				lpPoints[i] = { REAL(ConvertCoordFromPositionToPixel(pos).x), REAL(ConvertCoordFromPositionToPixel(pos).y) };
			}

			graphics.FillPolygon(&H_Brush, lpPoints, Patatoides[rtCallsign].points.size());
		}
		acPosPix = ConvertCoordFromPositionToPixel(RtPos.GetPosition());

		const bool proModeEnabled = frameProModeEnabled;
		const char* systemId = rt.GetSystemID();
		const bool hasSystemId = (systemId != nullptr && systemId[0] != '\0');
		const bool isReleasedTrack = hasSystemId &&
			(std::find(ReleasedTracks.begin(), ReleasedTracks.end(), systemId) != ReleasedTracks.end());
		const char* assignedSquawk = iconFp.IsValid() ? iconFp.GetControllerAssignedData().GetSquawk() : nullptr;
		const char* reportedSquawk = RtPos.GetSquawk();
		const bool hasAssignedSquawk = (assignedSquawk != nullptr && assignedSquawk[0] != '\0');
		const bool hasReportedSquawk = (reportedSquawk != nullptr && reportedSquawk[0] != '\0');
		const bool isWrongSquawk = hasAssignedSquawk && hasReportedSquawk &&
			strcmp(assignedSquawk, reportedSquawk) != 0;
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
		std::string acType;
		if (iconFp.IsValid())
		{
			const char* acTypeRaw = iconFp.GetFlightPlanData().GetAircraftFPType();
			acType = (acTypeRaw != nullptr) ? acTypeRaw : "";
		}
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

		bool isOnRunway = RimcasInstance->isAcOnRunway(rtCallsign);
		GroundStateCategory groundStateCat = GroundStateCategory::Unknown;
		if (iconFp.IsValid()) {
			groundStateCat = classifyGroundState(iconFp.GetGroundState(), reportedGs, isOnRunway);
		}

		bool isDepartureTarget = false;
		if (iconFp.IsValid() && AcisCorrelated)
		{
			const char* originRaw = iconFp.GetFlightPlanData().GetOrigin();
			std::string originAirport = (originRaw != nullptr) ? originRaw : "";
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
		else if (!isDepartureTarget)
			vacdmRuleType = "arrival";

		std::string vacdmRuleStatus = "default";
		if (vacdmRuleType == "departure" || vacdmRuleType == "arrival")
		{
			if (isAirborneTarget)
			{
				if (vacdmRuleType == "departure")
					vacdmRuleStatus = isOnRunway ? "airdep_onrunway" : "airdep";
				else
					vacdmRuleStatus = isOnRunway ? "airarr_onrunway" : "airarr";
			}
			else if (hasNoFlightPlan)
			{
				vacdmRuleStatus = "nofpl";
			}
			else
			{
				switch (groundStateCat)
				{
				case GroundStateCategory::Taxi:
					if (vacdmRuleType == "departure")
						vacdmRuleStatus = "taxi";
					else if (vacdmRuleType == "arrival")
						vacdmRuleStatus = "default";
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
						vacdmRuleStatus = "default";
					break;
				case GroundStateCategory::Depa:
					if (vacdmRuleType == "departure")
						vacdmRuleStatus = "depa";
					break;
				case GroundStateCategory::Arr:
					if (vacdmRuleType == "arrival")
						vacdmRuleStatus = "default";
					break;
				default:
					break;
				}
			}
		}

		VacdmPilotData vacdmRulePilotData;
		const bool hasVacdmRulePilotData = getCachedVacdmLookup(rt, iconFp, vacdmRulePilotData);
		const std::vector<VacdmColorRuleDefinition>& vacdmColorRules =
			getCachedIconVacdmColorRules(vacdmRuleType, vacdmRuleStatus);
		VacdmColorRuleOverrides vacdmColorRuleOverrides =
			EvaluateVacdmColorRules(vacdmColorRules, hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		const char* frameAselCallsign = frameAselTarget.IsValid() ? frameAselTarget.GetCallsign() : nullptr;
		const bool iconIsAseL = (frameAselCallsign != nullptr && strcmp(frameAselCallsign, rtCallsign.c_str()) == 0);
		const std::string targetCallsign = ToUpperAsciiCopy(rtCallsign);
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
				frameActiveAirport,
				rtCallsign);
			tagDataIt = frameTagDataCache.emplace(targetCallsign, std::move(generatedTagData)).first;
		}
		iconVerboseStep("after_tag_data");
		const TagReplacingMap& iconReplacingMap = tagDataIt->second;
		const VacdmColorRuleOverrides structuredIconColorRuleOverrides = evaluateStructuredColorRules(
			vacdmRuleType,
			vacdmRuleStatus,
			"normal",
			iconReplacingMap,
			hasVacdmRulePilotData ? &vacdmRulePilotData : nullptr);
		iconVerboseStep("after_structured_rules");
		if (structuredIconColorRuleOverrides.hasTargetColor)
		{
			vacdmColorRuleOverrides.hasTargetColor = true;
			vacdmColorRuleOverrides.targetR = structuredIconColorRuleOverrides.targetR;
			vacdmColorRuleOverrides.targetG = structuredIconColorRuleOverrides.targetG;
			vacdmColorRuleOverrides.targetB = structuredIconColorRuleOverrides.targetB;
			vacdmColorRuleOverrides.targetA = structuredIconColorRuleOverrides.targetA;
		}

		const bool smallIconBoostEnabled = frameSmallIconBoostEnabled;
		const bool fixedPixelIconSize = frameFixedPixelIconSize;
		auto isValidColorObject = [](const Value& colorValue) -> bool
		{
			if (!colorValue.IsObject())
				return false;
			if (!colorValue.HasMember("r") || !colorValue["r"].IsInt())
				return false;
			if (!colorValue.HasMember("g") || !colorValue["g"].IsInt())
				return false;
			if (!colorValue.HasMember("b") || !colorValue["b"].IsInt())
				return false;
			if (colorValue.HasMember("a") && !colorValue["a"].IsInt())
				return false;
			return true;
		};
		const Value* targetsConfig = nullptr;
		if (CurrentConfig != nullptr)
		{
			const Value& profile = CurrentConfig->getActiveProfile();
			if (profile.IsObject() && profile.HasMember("targets") && profile["targets"].IsObject())
				targetsConfig = &profile["targets"];
		}
		auto tryReadColorFromObject = [&](const Value& parentObject, const char* key, Color& outColor) -> bool
		{
			if (key == nullptr || key[0] == '\0')
				return false;
			if (!parentObject.HasMember(key))
				return false;
			const Value& iconColor = parentObject[key];
			if (!isValidColorObject(iconColor))
				return false;
			outColor = CurrentConfig->getConfigColor(iconColor);
			return true;
		};
		auto getGroundIconColor = [&](const char* key, Color fallback) -> Color
		{
			if (targetsConfig == nullptr || key == nullptr || key[0] == '\0')
				return fallback;

			const Value& targets = *targetsConfig;
			Color resolvedColor;
			auto tryGetFromSection = [&](const char* sectionKey, const char* sectionColorKey) -> bool
			{
				if (sectionKey == nullptr || sectionColorKey == nullptr)
					return false;
				if (!targets.HasMember(sectionKey) || !targets[sectionKey].IsObject())
					return false;
				return tryReadColorFromObject(targets[sectionKey], sectionColorKey, resolvedColor);
			};

			if (_stricmp(key, "airborne_departure") == 0 || _stricmp(key, "departure_airborne") == 0)
			{
				if (tryGetFromSection("departure", "airborne"))
					return resolvedColor;
			}
			else if (_stricmp(key, "depa") == 0 || _stricmp(key, "departure") == 0)
			{
				if (tryGetFromSection("departure", "departure"))
					return resolvedColor;
			}
			else if (_stricmp(key, "departure_gate") == 0)
			{
				if (tryGetFromSection("departure", "gate"))
					return resolvedColor;
			}
			else if (_stricmp(key, "nofpl") == 0 || _stricmp(key, "no_fpl") == 0)
			{
				if (tryGetFromSection("departure", "no_fpl"))
					return resolvedColor;
			}
			else if (_stricmp(key, "nsts") == 0 || _stricmp(key, "no_status") == 0)
			{
				if (tryGetFromSection("departure", "no_status"))
					return resolvedColor;
			}
			else if (_stricmp(key, "push") == 0)
			{
				if (tryGetFromSection("departure", "push"))
					return resolvedColor;
			}
			else if (_stricmp(key, "stup") == 0 || _stricmp(key, "startup") == 0)
			{
				if (tryGetFromSection("departure", "startup"))
					return resolvedColor;
			}
			else if (_stricmp(key, "taxi") == 0)
			{
				if (tryGetFromSection("departure", "taxi"))
					return resolvedColor;
			}
			else if (_stricmp(key, "airborne_arrival") == 0 || _stricmp(key, "arrival_airborne") == 0)
			{
				if (tryGetFromSection("arrival", "airborne"))
					return resolvedColor;
			}
			else if (_stricmp(key, "arrival_gate") == 0)
			{
				if (tryGetFromSection("arrival", "gate"))
					return resolvedColor;
			}
			else if (_stricmp(key, "arr") == 0 || _stricmp(key, "arrival_taxi") == 0 || _stricmp(key, "on_ground") == 0)
			{
				if (tryGetFromSection("arrival", "on_ground"))
					return resolvedColor;
			}
			else if (_stricmp(key, "gate") == 0)
			{
				if (tryGetFromSection("departure", "gate"))
					return resolvedColor;
				if (tryGetFromSection("arrival", "gate"))
					return resolvedColor;
			}
			else
			{
				// Also support direct access to already-migrated section keys when used internally.
				if (tryGetFromSection("departure", key))
					return resolvedColor;
				if (tryGetFromSection("arrival", key))
					return resolvedColor;
			}

			if (targets.HasMember("ground_icons") && targets["ground_icons"].IsObject() &&
				tryReadColorFromObject(targets["ground_icons"], key, resolvedColor))
			{
				return resolvedColor;
			}

			return fallback;
		};
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
		bool canUseRealisticIcon = useRealisticIconStyle && iconBmp != nullptr;
		if (canUseRealisticIcon)
		{
			const Gdiplus::Status bmpStatus = iconBmp->GetLastStatus();
			const UINT bmpWidth = iconBmp->GetWidth();
			const UINT bmpHeight = iconBmp->GetHeight();
			if (bmpStatus != Gdiplus::Ok || bmpWidth == 0 || bmpHeight == 0)
			{
				iconVerboseStep(
					"realistic_icon_disabled status=" + std::to_string(static_cast<int>(bmpStatus)) +
					" w=" + std::to_string(static_cast<unsigned long long>(bmpWidth)) +
					" h=" + std::to_string(static_cast<unsigned long long>(bmpHeight)));
				canUseRealisticIcon = false;
			}
		}
		if (Logger::is_verbose_mode())
		{
			std::string iconDrawMode = canUseRealisticIcon ? "realistic" : "symbol";
			Logger::info("IconRender: " + rtCallsign + " mode=" + iconDrawMode + " icon_type=" + iconType);
		}

		if (canUseRealisticIcon) {

			// Compute on-screen size that scales with zoom (uniform for all aircraft)
			double drawW = iconSize;
			double drawH = iconSize;
			const double pixPerMeter = std::isfinite(framePixPerMeter) ? framePixPerMeter : 0.0;

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

			// Small fallback table to avoid heavy dynamic initialization in the hot render loop.
			if (lengthMeters <= 0 || spanMeters <= 0) {
				static const std::unordered_map<std::string, std::pair<double, double>> lightFallbackDims = {
					{ "a320", {37.6, 35.8} },
					{ "a321", {44.5, 35.8} },
					{ "b738", {39.5, 35.8} },
					{ "b77w", {73.9, 64.8} },
					{ "a388", {73.0, 79.8} },
					{ "c172", {8.2, 10.9} }
				};
				auto itHard = lightFallbackDims.find(acTypeLower);
				if (itHard == lightFallbackDims.end())
					itHard = lightFallbackDims.find(iconType);
				if (itHard != lightFallbackDims.end()) {
					lengthMeters = itHard->second.first;
					spanMeters = itHard->second.second;
				}
			}

			// If still missing, fill from WTC defaults
			if (lengthMeters <= 0 || spanMeters <= 0) {
				wtcFallbackDims(wtc, lengthMeters, spanMeters);
			}
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
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arr", getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 90, 150, 235)));
					applyTint = true;
					break;
				default:
					break;
				}
			}

			if (vacdmColorRuleOverrides.hasTargetColor)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					Color(vacdmColorRuleOverrides.targetA, vacdmColorRuleOverrides.targetR, vacdmColorRuleOverrides.targetG, vacdmColorRuleOverrides.targetB));
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
			if (!std::isfinite(rotationDeg))
				rotationDeg = 0.0;
			iconVerboseStep("realistic_before_transform rot=" + std::to_string(rotationDeg));

			GraphicsState state = graphics.Save();
			Gdiplus::Matrix m;
			m.Translate(Gdiplus::REAL(acPosPix.x), Gdiplus::REAL(acPosPix.y));
			m.Rotate(Gdiplus::REAL(rotationDeg));
			m.Translate(Gdiplus::REAL(-drawW / 2.0), Gdiplus::REAL(-drawH / 2.0));
			graphics.SetTransform(&m);

			if (applyTint) {
				iconVerboseStep("before_realistic_draw_tinted");
				const Gdiplus::REAL tintAlpha = static_cast<Gdiplus::REAL>(tintColor.GetAlpha()) / 255.0f;
				Gdiplus::ColorMatrix cm = {
					{
						{ static_cast<REAL>(tintColor.GetR()) / 255.0f, 0.0f, 0.0f, 0.0f, 0.0f },
						{ 0.0f, static_cast<REAL>(tintColor.GetG()) / 255.0f, 0.0f, 0.0f, 0.0f },
						{ 0.0f, 0.0f, static_cast<REAL>(tintColor.GetB()) / 255.0f, 0.0f, 0.0f },
						{ 0.0f, 0.0f, 0.0f, tintAlpha, 0.0f },
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
				iconVerboseStep("after_realistic_draw_tinted");
			}
			else {
				iconVerboseStep("before_realistic_draw_plain");
				graphics.DrawImage(iconBmp, Gdiplus::REAL(0), Gdiplus::REAL(0), Gdiplus::REAL(drawW), Gdiplus::REAL(drawH));
				iconVerboseStep("after_realistic_draw_plain");
			}
			graphics.Restore(state);
			iconSize = int(max(drawW, drawH));
		}
		else
		{
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
				case GroundStateCategory::Taxi:
					tintColor = ColorManager->get_corrected_color("symbol",
						getGroundIconColor("arr", getGroundIconColor("arrival_gate", getGroundIconColor("gate", Color(255, 165, 165, 165)))));
					applyTint = true;
					break;
				case GroundStateCategory::Push:
				case GroundStateCategory::Stup:
					tintColor = ColorManager->get_corrected_color("symbol", getGroundIconColor("push", Color(255, 90, 150, 235)));
					applyTint = true;
					break;
				default:
					break;
				}
			}

			if (vacdmColorRuleOverrides.hasTargetColor)
			{
				tintColor = ColorManager->get_corrected_color("symbol",
					Color(vacdmColorRuleOverrides.targetA, vacdmColorRuleOverrides.targetR, vacdmColorRuleOverrides.targetG, vacdmColorRuleOverrides.targetB));
				applyTint = true;
			}

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

			const Color drawColor = applyTint ? tintColor : ColorManager->get_corrected_color("symbol", Gdiplus::Color::White);
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
				iconVerboseStep("after_symbol_diamond_draw");
				iconSize = int(max(12.0, diagonalPx));
			}
			else
			{
				iconVerboseStep("before_symbol_arrow_draw");
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
				iconVerboseStep("after_symbol_arrow_draw");
				iconSize = int(max(12.0, lenPx + halfWidthPx));
			}
		}
		iconVerboseStep("after_icon_draw");

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
		std::string hoverTextStorage;
		const char* hoverText = "";
		if (AcisCorrelated)
		{
			hoverTextStorage = GetBottomLine(rtCallsign.c_str());
			hoverText = hoverTextStorage.c_str();
		}
		else
		{
			const char* systemIdText = rt.GetSystemID();
			hoverText = (systemIdText != nullptr) ? systemIdText : "";
		}
		iconVerboseStep("before_add_screen_object");
		AddScreenObject(DRAWING_AC_SYMBOL, rtCallsign.c_str(), { acPosPix.x - hitSize / 2, acPosPix.y - hitSize / 2, acPosPix.x + hitSize / 2, acPosPix.y + hitSize / 2 }, false, hoverText);
		iconVerboseStep("after_add_screen_object");
	}

#pragma endregion Drawing of the symbols

	TimePopupData.clear();
	AcOnRunway.clear();
	ColorAC.clear();
	tagAreas.clear();
	tagCollisionAreas.clear();

	int rimcasStageTwoSpeedThreshold = 25;
	if (CurrentConfig != nullptr)
	{
		const Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() &&
			profile.HasMember("rimcas") &&
			profile["rimcas"].IsObject())
		{
			const Value& rimcas = profile["rimcas"];
			if (rimcas.HasMember("stage_two_speed_threshold_kt") && rimcas["stage_two_speed_threshold_kt"].IsInt())
			{
				rimcasStageTwoSpeedThreshold = rimcas["stage_two_speed_threshold_kt"].GetInt();
			}
			else if (rimcas.HasMember("rimcas_stage_two_speed_threshold") && rimcas["rimcas_stage_two_speed_threshold"].IsInt())
			{
				rimcasStageTwoSpeedThreshold = rimcas["rimcas_stage_two_speed_threshold"].GetInt();
			}
		}
	}
	rimcasStageTwoSpeedThreshold = std::clamp(rimcasStageTwoSpeedThreshold, 0, 250);
	RimcasInstance->OnRefreshEnd(this, rimcasStageTwoSpeedThreshold);

	graphics.SetSmoothingMode(SmoothingModeDefault);

	try
	{
		RenderTags(graphics, dc, frameProModeEnabled, frameTagDataCache, frameVacdmLookupCache);
	}
	catch (const std::exception& ex)
	{
		Logger::info(std::string("RenderTags: std::exception caught: ") + ex.what());
	}
	catch (...)
	{
		Logger::info("RenderTags: unknown C++ exception caught");
	}

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
		vector<string> allProfiles = GetOrderedProfileNamesForUi();
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
			CRadarTarget activeDistanceTarget = GetPlugIn()->RadarTargetSelect(ActiveDistance.first.c_str());
			if (!activeDistanceTarget.IsValid() || !activeDistanceTarget.GetPosition().IsValid())
			{
				DistanceToolActive = false;
				ActiveDistance = pair<string, string>("", "");
				dc.SelectObject(oldPen);
				RequestRefresh();
				return;
			}
			CPosition r = activeDistanceTarget.GetPosition().GetPosition();
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
	std::string activeProfileName = GetActiveProfileNameForEditor();
	if (activeProfileName.empty())
		activeProfileName = "Default";
	dc.TextOutA(ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, activeProfileName.c_str());
	AddScreenObject(RIMCAS_ACTIVE_PROFILE, "ActiveProfile", { ToolBarAreaTop.left + offset, ToolBarAreaTop.top + 4, ToolBarAreaTop.left + offset + dc.GetTextExtent(activeProfileName.c_str()).cx, ToolBarAreaTop.top + 4 + dc.GetTextExtent(activeProfileName.c_str()).cy }, false, "Active profile");

	offset += dc.GetTextExtent(activeProfileName.c_str()).cx + 10;
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
	for (const auto& areas : tagCollisionAreas)
	{
		if (!autoDeconflictionEnabled)
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

		CRadarTarget deconflictTarget = GetPlugIn()->RadarTargetSelect(areas.first.c_str());
		if (!deconflictTarget.IsValid() || !deconflictTarget.GetPosition().IsValid())
		{
			staleTagCallsigns.push_back(areas.first);
			if (Logger::is_verbose_mode())
			{
				Logger::info(
					"OnRefresh deconfliction: pruned stale tag state callsign=" + areas.first +
					" target_valid=" + std::string(deconflictTarget.IsValid() ? "1" : "0"));
			}
			continue;
		}

		if (TagAngles.find(areas.first) == TagAngles.end())
			TagAngles[areas.first] = 270.0f;

		POINT acPosPix = ConvertCoordFromPositionToPixel(deconflictTarget.GetPosition().GetPosition());
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

		if (pluginWindow != nullptr && gSourceProc != nullptr && ::IsWindow(pluginWindow))
			::SetWindowLongPtr(pluginWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gSourceProc));

		pluginWindow = nullptr;
		gSourceProc = nullptr;
}
