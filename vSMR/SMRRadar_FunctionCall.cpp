#include "stdafx.h"
#include "SMRRadar.hpp"

namespace
{
	double ClampDouble(double value, double minValue, double maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	int ClampInt(int value, int minValue, int maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	void RgbToHsv(int r, int g, int b, double& h, double& s, double& v)
	{
		double rf = ClampDouble(r / 255.0, 0.0, 1.0);
		double gf = ClampDouble(g / 255.0, 0.0, 1.0);
		double bf = ClampDouble(b / 255.0, 0.0, 1.0);

		double cmax = max(rf, max(gf, bf));
		double cmin = min(rf, min(gf, bf));
		double delta = cmax - cmin;

		h = 0.0;
		if (delta > 1e-9)
		{
			if (cmax == rf)
				h = 60.0 * fmod(((gf - bf) / delta), 6.0);
			else if (cmax == gf)
				h = 60.0 * (((bf - rf) / delta) + 2.0);
			else
				h = 60.0 * (((rf - gf) / delta) + 4.0);
		}
		if (h < 0.0)
			h += 360.0;

		s = (cmax <= 1e-9) ? 0.0 : (delta / cmax);
		v = cmax;
	}

	std::vector<int> ExtractIntegers(const std::string& text)
	{
		std::vector<int> values;
		int sign = 1;
		int number = 0;
		bool inNumber = false;

		for (size_t i = 0; i < text.size(); ++i)
		{
			const char c = text[i];
			if (c == '-' && !inNumber)
			{
				sign = -1;
				continue;
			}
			if (std::isdigit(static_cast<unsigned char>(c)))
			{
				inNumber = true;
				number = number * 10 + (c - '0');
				continue;
			}

			if (inNumber)
			{
				values.push_back(sign * number);
				number = 0;
				sign = 1;
				inNumber = false;
			}
			else
			{
				sign = 1;
			}
		}

		if (inNumber)
			values.push_back(sign * number);

		return values;
	}

	bool TryParseHexByte(const std::string& text, size_t offset, int& outValue)
	{
		if (offset + 1 >= text.size())
			return false;

		auto hexValue = [](char ch) -> int
		{
			unsigned char c = static_cast<unsigned char>(ch);
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'A' && c <= 'F')
				return 10 + (c - 'A');
			if (c >= 'a' && c <= 'f')
				return 10 + (c - 'a');
			return -1;
		};

		int hi = hexValue(text[offset]);
		int lo = hexValue(text[offset + 1]);
		if (hi < 0 || lo < 0)
			return false;

		outValue = (hi << 4) | lo;
		return true;
	}

	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (char ch : text)
		{
			if (!std::isspace(static_cast<unsigned char>(ch)))
				normalized.push_back(ch);
		}

		if (normalized.empty())
			return false;

		if (normalized[0] == '#')
			normalized.erase(0, 1);

		if (normalized.size() != 6 && normalized.size() != 8)
			return false;

		int rr = 0, gg = 0, bb = 0, aa = 255;
		if (!TryParseHexByte(normalized, 0, rr) ||
			!TryParseHexByte(normalized, 2, gg) ||
			!TryParseHexByte(normalized, 4, bb))
		{
			return false;
		}

		hasAlpha = false;
		if (normalized.size() == 8)
		{
			if (!TryParseHexByte(normalized, 6, aa))
				return false;
			hasAlpha = true;
		}

		r = rr;
		g = gg;
		b = bb;
		a = aa;
		return true;
	}
}

extern CPoint mouseLocation;
extern map<int, CInsetWindow *> appWindows;

void CSMRRadar::OnFunctionCall(int FunctionId, const char * sItemString, POINT Pt, RECT Area) {
	Logger::info(string(__FUNCSIG__));
	mouseLocation = Pt;
	if (FunctionId == APPWINDOW_ONE || FunctionId == APPWINDOW_TWO) {
		int id = FunctionId - APPWINDOW_BASE;
		appWindowDisplays[id] = !appWindowDisplays[id];
	}

	if (FunctionId == RIMCAS_ACTIVE_AIRPORT_FUNC) {
		setActiveAirport(sItemString);
		SaveDataToAsr("Airport", "Active airport", getActiveAirport().c_str());
	}

	if (FunctionId == RIMCAS_UPDATE_FONTS) {
		if (strcmp(sItemString, "Size 1") == 0)
			currentFontSize = 1;
		if (strcmp(sItemString, "Size 2") == 0)
			currentFontSize = 2;
		if (strcmp(sItemString, "Size 3") == 0)
			currentFontSize = 3;
		if (strcmp(sItemString, "Size 4") == 0)
			currentFontSize = 4;
		if (strcmp(sItemString, "Size 5") == 0)
			currentFontSize = 5;

		// Persist profile label font size even when selecting the currently active size.
		if (!SetActiveLabelFontSize(currentFontSize, false))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save label font size to vSMR_Profiles.json", true, true, false, false, false);
		}
		else if (!CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save label font size to vSMR_Profiles.json", true, true, false, false, false);
		}

		ShowLists["Label Font Size"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_TAG_FONT)
	{
		if (sItemString != nullptr)
		{
			// Keep font selection persisted in profile JSON even when selecting the current value again.
			if (!SetActiveTagFontName(sItemString, false))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save tag font to vSMR_Profiles.json", true, true, false, false, false);
			}
			else if (!CurrentConfig->saveConfig())
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save tag font to vSMR_Profiles.json", true, true, false, false, false);
			}
			else
			{
				LoadCustomFont();
			}
		}

		ShowLists["Tag Font"] = true;
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_QDM_TOGGLE) {
		QDMenabled = !QDMenabled;
		QDMSelectEnabled = false;
	}

	if (FunctionId == RIMCAS_QDM_SELECT_TOGGLE)
	{
		if (!QDMSelectEnabled)
		{
			QDMSelectPt = ConvertCoordFromPositionToPixel(AirportPositions[getActiveAirport()]);
		}
		QDMSelectEnabled = !QDMSelectEnabled;
		QDMenabled = false;
	}

	if (FunctionId == RIMCAS_UPDATE_PROFILE) {
		this->CSMRRadar::LoadProfile(sItemString);
		LoadCustomFont();
		SaveDataToAsr("ActiveProfile", "vSMR active profile", sItemString);

		ShowLists["Profiles"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_ICON_STYLE)
	{
		if (sItemString != nullptr)
		{
			if (!SetActiveTargetIconStyle(sItemString, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon style to vSMR_Profiles.json", true, true, false, false, false);
			}
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TOGGLE_FIXED_PIXEL_ICON_SIZE)
	{
		const bool nextEnabled = !GetFixedPixelTargetIconSizeEnabled();
		if (!SetFixedPixelTargetIconSizeEnabled(nextEnabled, true))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save fixed pixel icon size to vSMR_Profiles.json", true, true, false, false, false);
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_FIXED_PIXEL_TRIANGLE_SCALE)
	{
		if (sItemString != nullptr)
		{
			double scale = atof(sItemString);
			scale = std::clamp(scale, 0.1, 3.0);
			if (!SetFixedPixelTriangleIconScale(scale, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save fixed size to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_TOGGLE_SMALL_ICON_BOOST)
	{
		const bool nextEnabled = !GetSmallTargetIconBoostEnabled();
		if (!SetSmallTargetIconBoostEnabled(nextEnabled, true))
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save small icon boost to vSMR_Profiles.json", true, true, false, false, false);
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_SMALL_ICON_BOOST_FACTOR)
	{
		if (sItemString != nullptr)
		{
			double factor = atof(sItemString);
			factor = std::clamp(factor, 0.5, 4.0);
			if (!SetSmallTargetIconBoostFactor(factor, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon boost factor to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_SMALL_ICON_BOOST_RESOLUTION)
	{
		if (sItemString != nullptr)
		{
			if (!SetSmallTargetIconBoostResolutionPreset(sItemString, true))
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save icon boost resolution to vSMR_Profiles.json", true, true, false, false, false);
			}
		}
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATEFILTER1 || FunctionId == RIMCAS_UPDATEFILTER2) {
		int id = FunctionId - RIMCAS_UPDATEFILTER;
		if (startsWith("UNL", sItemString))
			sItemString = "66000";
		appWindows[id]->m_Filter = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATERANGE1 || FunctionId == RIMCAS_UPDATERANGE2) {
		int id = FunctionId - RIMCAS_UPDATERANGE;
		appWindows[id]->m_Scale = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATEROTATE1 || FunctionId == RIMCAS_UPDATEROTATE2) {
		int id = FunctionId - RIMCAS_UPDATEROTATE;
		appWindows[id]->m_Rotation = atoi(sItemString);
	}

	if (FunctionId == RIMCAS_UPDATE_BRIGHNESS) {
		if (strcmp(sItemString, "Day") == 0)
			ColorSettingsDay = true;
		else
			ColorSettingsDay = false;

		ShowLists["Colour Settings"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_ALERTS_TOGGLE_FUNC) {
		RimcasInstance->toggleActiveAlert(string(sItemString));
		CurrentConfig->setInactiveAlert(RimcasInstance->GetInactiveAlerts());
		if (!CurrentConfig->saveConfig())
		{
			GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save active alerts to vSMR_Profiles.json", true, true, false, false, false);
		}
		ShowLists["Active Alerts"] = true;
		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CA_ARRIVAL_FUNC) {
		RimcasInstance->toggleMonitoredRunwayArr(string(sItemString));

		ShowLists["Conflict Alert ARR"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CA_MONITOR_FUNC) {
		RimcasInstance->toggleMonitoredRunwayDep(string(sItemString));

		ShowLists["Conflict Alert DEP"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_CLOSED_RUNWAYS_FUNC) {
		RimcasInstance->toggleClosedRunway(string(sItemString));

		ShowLists["Runway closed"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_OPEN_LIST) {
		if (sItemString != nullptr &&
			(strcmp(sItemString, "Tag Definitions") == 0 || strcmp(sItemString, "Profile Editor") == 0))
		{
			OpenProfileEditorWindow();
			return;
		}

		ShowLists[string(sItemString)] = true;
		ListAreas[string(sItemString)] = Area;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_PROFILE_COLOR_SELECT)
	{
		if (sItemString != nullptr && IsProfileColorPathValid(sItemString))
		{
			OpenProfileColorPicker(sItemString, ShowTagDefinitionEditor);
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_PROFILE_COLOR_EDIT_RGB)
	{
		if (sItemString != nullptr)
		{
			const std::vector<int> values = ExtractIntegers(sItemString);
			if (values.size() >= 3)
			{
				int r = ClampInt(values[0], 0, 255);
				int g = ClampInt(values[1], 0, 255);
				int b = ClampInt(values[2], 0, 255);
				RgbToHsv(r, g, b, ProfileColorPickerHue, ProfileColorPickerSaturation, ProfileColorPickerValue);
				ApplyProfileColorPicker(true);
			}
			else
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Color", "Use RGB format like 73,87,126", true, true, false, false, false);
			}
		}
	}

	if (FunctionId == RIMCAS_PROFILE_COLOR_EDIT_ALPHA)
	{
		if (sItemString != nullptr)
		{
			const std::vector<int> values = ExtractIntegers(sItemString);
			if (!values.empty())
			{
				ProfileColorPickerAlpha = ClampInt(values[0], 0, 255);
				ProfileColorPickerHasAlpha = true;
				ApplyProfileColorPicker(true);
			}
			else
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Color", "Use opacity value 0-255", true, true, false, false, false);
			}
		}
	}

	if (FunctionId == RIMCAS_PROFILE_COLOR_EDIT_HEX)
	{
		if (sItemString != nullptr)
		{
			int r = 0, g = 0, b = 0, a = 255;
			bool hasAlpha = false;
			if (TryParseHexColor(sItemString, r, g, b, a, hasAlpha))
			{
				RgbToHsv(r, g, b, ProfileColorPickerHue, ProfileColorPickerSaturation, ProfileColorPickerValue);
				if (hasAlpha)
				{
					ProfileColorPickerAlpha = ClampInt(a, 0, 255);
					ProfileColorPickerHasAlpha = true;
				}
				ApplyProfileColorPicker(true);
			}
			else
			{
				GetPlugIn()->DisplayUserMessage("vSMR", "Color", "Use HEX format like #49577E or #49577EFF", true, true, false, false, false);
			}
		}
	}

	if (FunctionId == RIMCAS_TAGDEF_SELECT_TYPE)
	{
		if (sItemString != nullptr)
		{
			TagDefinitionEditorType = NormalizeTagDefinitionType(sItemString);
			if (TagDefinitionEditorType != "departure" && TagDefinitionEditorType != "arrival" && TagDefinitionEditorType != "airborne")
				TagDefinitionEditorType = "departure";

			if (!IsTagDefinitionStatusAllowedForType(TagDefinitionEditorType, TagDefinitionEditorDepartureStatus))
				TagDefinitionEditorDepartureStatus = "default";

			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TAGDEF_SELECT_MODE)
	{
		if (sItemString != nullptr)
		{
			std::string mode = sItemString;
			std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			TagDefinitionEditorDetailed = (mode.find("detail") != std::string::npos);
			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TAGDEF_SELECT_STATUS)
	{
		if (sItemString != nullptr)
		{
			TagDefinitionEditorDepartureStatus = NormalizeTagDefinitionDepartureStatus(sItemString);
			if (!IsTagDefinitionStatusAllowedForType(TagDefinitionEditorType, TagDefinitionEditorDepartureStatus))
				TagDefinitionEditorDepartureStatus = "default";

			RequestRefresh();
		}
	}

	if (FunctionId == RIMCAS_TAGDEF_INSERT_TOKEN)
	{
		if (sItemString != nullptr && strlen(sItemString) > 0)
		{
			InsertTagDefinitionTokenIntoLine(sItemString);
		}
	}

	if (FunctionId == RIMCAS_TAGDEF_INSERT_TOKEN_BOLD)
	{
		if (sItemString != nullptr && strlen(sItemString) > 0)
		{
			InsertTagDefinitionTokenIntoLine(sItemString, true);
		}
	}

	if (FunctionId >= RIMCAS_TAGDEF_EDIT_LINE_BASE && FunctionId < RIMCAS_TAGDEF_EDIT_LINE_BASE + TagDefinitionEditorMaxLines)
	{
		int lineIndex = FunctionId - RIMCAS_TAGDEF_EDIT_LINE_BASE;
		TagDefinitionEditorSelectedLine = lineIndex;
		SetTagDefinitionLineString(TagDefinitionEditorType, TagDefinitionEditorDetailed, lineIndex, sItemString ? sItemString : "", TagDefinitionEditorDepartureStatus);
	}

	if (FunctionId == RIMCAS_UPDATE_LVP) {
		if (strcmp(sItemString, "Normal") == 0)
			isLVP = false;
		if (strcmp(sItemString, "Low") == 0)
			isLVP = true;

		ShowLists["Visibility"] = true;

		RequestRefresh();
	}

	if (FunctionId == RIMCAS_UPDATE_AFTERGLOW)
	{
		Afterglow = !Afterglow;
	}

	if (FunctionId == RIMCAS_UPDATE_GND_TRAIL)
	{
		Trail_Gnd = atoi(sItemString);

		ShowLists["GRND Trail Dots"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_APP_TRAIL)
	{
		Trail_App = atoi(sItemString);

		ShowLists["APPR Trail Dots"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_PTL)
	{
		PredictedLength = atoi(sItemString);

		ShowLists["Predicted Track Line"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_LABEL)
	{
		ColorManager->update_brightness("label", std::atoi(sItemString));
		ShowLists["Label"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_AFTERGLOW)
	{
		ColorManager->update_brightness("afterglow", std::atoi(sItemString));
		ShowLists["Afterglow"] = true;
	}

	if (FunctionId == RIMCAS_BRIGHTNESS_SYMBOL)
	{
		ColorManager->update_brightness("symbol", std::atoi(sItemString));
		ShowLists["Symbol"] = true;
	}

	if (FunctionId == RIMCAS_UPDATE_RELEASE)
	{
		ReleaseInProgress = !ReleaseInProgress;
		if (ReleaseInProgress)
			AcquireInProgress = false;
		NeedCorrelateCursor = ReleaseInProgress;

		CorrelateCursor();
	}

	if (FunctionId == RIMCAS_UPDATE_ACQUIRE)
	{
		AcquireInProgress = !AcquireInProgress;
		if (AcquireInProgress)
			ReleaseInProgress = false;
		NeedCorrelateCursor = AcquireInProgress;

		CorrelateCursor();
	}
}
