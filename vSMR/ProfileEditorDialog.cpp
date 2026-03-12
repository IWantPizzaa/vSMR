#include "stdafx.h"
#include "ProfileEditorDialog.hpp"
#include "SMRRadar.hpp"
#include "afxdialogex.h"
#include "Logger.h"
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>

IMPLEMENT_DYNAMIC(CProfileEditorDialog, CDialogEx)

namespace
{
	void LogProfileEditorInitStep(const char* step)
	{
		if (step == nullptr)
			return;
		Logger::info(std::string("ProfileEditor: ") + step);
	}

	const UINT WM_PE_COLOR_WHEEL_TRACK = WM_APP + 417;
	const UINT WM_PE_COLOR_VALUE_TRACK = WM_APP + 418;
	const UINT WM_PE_COLOR_OPACITY_TRACK = WM_APP + 419;
	const UINT WM_PE_RULE_COLOR_WHEEL_TRACK = WM_APP + 420;
	const UINT WM_PE_RULE_COLOR_VALUE_TRACK = WM_APP + 421;
	const UINT WM_PE_RULE_COLOR_OPACITY_TRACK = WM_APP + 422;
	const COLORREF kEditorBorderColor = RGB(198, 204, 214);
	const COLORREF kEditorFocusBorderColor = RGB(64, 132, 230);
	const COLORREF kEditorThemeBackgroundColor = RGB(240, 240, 240);
	const COLORREF kEditorSidebarBackgroundColor = RGB(250, 250, 251);
	const int kOffscreenPos = -5000;
	const int kTabColors = 0;
	const int kTabIcons = 1;
	const int kTabRules = 2;
	const int kTabProfile = 3;
	std::map<HWND, WNDPROC> gThemedEditOldProcs;
	std::map<HWND, WNDPROC> gThemedComboOldProcs;
	std::map<HWND, WNDPROC> gColorWheelOldProcs;
	std::map<HWND, HWND> gColorWheelOwnerWindows;
	std::map<HWND, WNDPROC> gColorValueSliderOldProcs;
	std::map<HWND, HWND> gColorValueSliderOwnerWindows;
	std::map<HWND, WNDPROC> gColorOpacitySliderOldProcs;
	std::map<HWND, HWND> gColorOpacitySliderOwnerWindows;
	std::map<HWND, WNDPROC> gRuleColorWheelOldProcs;
	std::map<HWND, HWND> gRuleColorWheelOwnerWindows;
	std::map<HWND, WNDPROC> gRuleColorValueSliderOldProcs;
	std::map<HWND, HWND> gRuleColorValueSliderOwnerWindows;
	std::map<HWND, WNDPROC> gRuleColorOpacitySliderOldProcs;
	std::map<HWND, HWND> gRuleColorOpacitySliderOwnerWindows;

	std::string TrimAsciiWhitespaceCopy(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;
		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	}

	std::vector<std::string> SplitRuleConditionClauses(const std::string& text)
	{
		std::vector<std::string> clauses;
		std::string current;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (i + 1 < text.size() && text[i] == '&' && text[i + 1] == '&')
			{
				const std::string trimmed = TrimAsciiWhitespaceCopy(current);
				if (!trimmed.empty())
					clauses.push_back(trimmed);
				current.clear();
				++i;
				continue;
			}
			current.push_back(text[i]);
		}

		const std::string trimmed = TrimAsciiWhitespaceCopy(current);
		if (!trimmed.empty())
			clauses.push_back(trimmed);
		return clauses;
	}

	std::string SerializeRuleConditionText(const StructuredTagColorRule& rule)
	{
		if (rule.criteria.empty())
			return rule.condition;

		std::string text = rule.criteria[0].condition;
		for (size_t i = 1; i < rule.criteria.size(); ++i)
		{
			const StructuredTagColorRule::Criterion& criterion = rule.criteria[i];
			text += " && ";
			text += criterion.source;
			text += ".";
			text += criterion.token;
			text += "=";
			text += criterion.condition;
		}
		return text;
	}

	bool TryParseExplicitRuleClause(
		const std::string& rawClause,
		CSMRRadar* owner,
		const std::string& defaultSource,
		StructuredTagColorRule::Criterion& outCriterion)
	{
		if (owner == nullptr)
			return false;

		const std::string clause = TrimAsciiWhitespaceCopy(rawClause);
		const size_t equalsPos = clause.find('=');
		if (equalsPos == std::string::npos)
			return false;

		const std::string selector = TrimAsciiWhitespaceCopy(clause.substr(0, equalsPos));
		const std::string conditionPart = TrimAsciiWhitespaceCopy(clause.substr(equalsPos + 1));
		if (selector.empty())
			return false;

		std::string sourcePart;
		std::string tokenPart = selector;
		const size_t dotPos = selector.find('.');
		if (dotPos != std::string::npos)
		{
			sourcePart = TrimAsciiWhitespaceCopy(selector.substr(0, dotPos));
			tokenPart = TrimAsciiWhitespaceCopy(selector.substr(dotPos + 1));
		}
		if (tokenPart.empty())
			return false;

		auto tryBuildFromSource = [&](const std::string& sourceCandidate) -> bool
		{
			const std::string normalizedSource = owner->NormalizeStructuredRuleSource(sourceCandidate);
			const std::string normalizedToken = owner->NormalizeStructuredRuleToken(normalizedSource, tokenPart);
			if (normalizedToken.empty())
				return false;
			outCriterion.source = normalizedSource;
			outCriterion.token = normalizedToken;
			outCriterion.condition = owner->NormalizeStructuredRuleCondition(normalizedSource, conditionPart.empty() ? "any" : conditionPart);
			return true;
			};

		if (!sourcePart.empty())
			return tryBuildFromSource(sourcePart);

		if (tryBuildFromSource(defaultSource))
			return true;
		if (tryBuildFromSource("runway"))
			return true;
		if (tryBuildFromSource("custom"))
			return true;
		if (tryBuildFromSource("vacdm"))
			return true;
		return false;
	}

	bool TryBuildRuleCriteriaFromConditionField(
		CSMRRadar* owner,
		const std::string& selectedSource,
		const std::string& selectedToken,
		const std::string& conditionText,
		std::vector<StructuredTagColorRule::Criterion>& outCriteria)
	{
		outCriteria.clear();
		if (owner == nullptr)
			return false;

		const std::string normalizedSource = owner->NormalizeStructuredRuleSource(selectedSource);
		const std::string normalizedToken = owner->NormalizeStructuredRuleToken(normalizedSource, selectedToken);
		if (normalizedToken.empty())
			return false;

		std::vector<std::string> clauses = SplitRuleConditionClauses(conditionText);
		if (clauses.empty())
			clauses.push_back("any");

		for (size_t i = 0; i < clauses.size(); ++i)
		{
			const std::string clause = TrimAsciiWhitespaceCopy(clauses[i]);
			if (clause.empty())
				continue;

			StructuredTagColorRule::Criterion criterion;
			if (TryParseExplicitRuleClause(clause, owner, normalizedSource, criterion))
			{
				outCriteria.push_back(criterion);
				continue;
			}

			criterion.source = normalizedSource;
			criterion.token = normalizedToken;
			criterion.condition = owner->NormalizeStructuredRuleCondition(normalizedSource, clause);
			outCriteria.push_back(criterion);
		}

		return !outCriteria.empty();
	}

	std::string CleanRuleDisplayName(const std::string& rawName, int fallbackIndex)
	{
		std::string text = TrimAsciiWhitespaceCopy(rawName);
		while (!text.empty())
		{
			const char c = text.front();
			if (c == '.' || c == '-' || c == '_' || c == ':' || c == ';' || std::isspace(static_cast<unsigned char>(c)) != 0)
			{
				text.erase(text.begin());
				continue;
			}
			break;
		}

		if (text.empty())
		{
			text = "Rule ";
			text += std::to_string(fallbackIndex);
		}
		return text;
	}

	std::string RuleSourceUiLabel(const std::string& source)
	{
		std::string lowered = TrimAsciiWhitespaceCopy(source);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowered == "runway")
			return "Runway";
		if (lowered == "custom" || lowered == "sid")
			return "SID";
		return "VACDM";
	}

	std::string NormalizeRuleTypeUiValue(CSMRRadar* owner, const std::string& type)
	{
		if (owner != nullptr)
			return owner->NormalizeStructuredRuleTagType(type);

		std::string lowered = TrimAsciiWhitespaceCopy(type);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowered.empty() || lowered == "all" || lowered == "*")
			return "any";
		if (lowered == "dep")
			return "departure";
		if (lowered == "arr")
			return "arrival";
		if (lowered == "air")
			return "airborne";
		if (lowered == "uncorr" || lowered == "uncor")
			return "uncorrelated";
		if (lowered == "departure" || lowered == "arrival" || lowered == "airborne" || lowered == "uncorrelated" || lowered == "any")
			return lowered;
		return "any";
	}

	std::string RuleTypeUiLabel(CSMRRadar* owner, const std::string& type)
	{
		const std::string normalizedType = NormalizeRuleTypeUiValue(owner, type);
		if (normalizedType == "any")
			return "Any";
		if (owner != nullptr)
			return owner->TagDefinitionTypeLabel(normalizedType);
		if (normalizedType == "departure")
			return "Departure";
		if (normalizedType == "arrival")
			return "Arrival";
		if (normalizedType == "airborne")
			return "Airborne";
		if (normalizedType == "uncorrelated")
			return "Uncorrelated";
		return "Any";
	}

	std::string NormalizeRuleStatusUiValue(CSMRRadar* owner, const std::string& type, const std::string& status)
	{
		const std::string normalizedType = NormalizeRuleTypeUiValue(owner, type);
		std::string lowered = TrimAsciiWhitespaceCopy(status);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::string compact;
		compact.reserve(lowered.size());
		for (char c : lowered)
		{
			if (c == ' ' || c == '_' || c == '-')
				continue;
			compact.push_back(c);
		}

		if (lowered.empty() || lowered == "all" || lowered == "*" || lowered == "any")
			return "any";

		if (normalizedType == "departure")
		{
			if (compact == "default" || compact == "nostatus" || compact == "nsts")
				return "default";
			if (compact == "nofpl" || compact == "noflightplan")
				return "nofpl";
			if (compact == "push")
				return "push";
			if (compact == "stup" || compact == "startup")
				return "stup";
			if (compact == "taxi")
				return "taxi";
			if (compact == "depa" || compact == "departure")
				return "depa";
			if (compact == "airdep" || compact == "airborne" || compact == "airbornedeparture")
				return "airdep";
			if (compact == "onrunway" || compact == "airdeponrunway" || compact == "airbornedepartureonrunway")
				return "airdep_onrunway";
		}
		else if (normalizedType == "arrival")
		{
			if (compact == "default" || compact == "onground" || compact == "arr" || compact == "arrival" || compact == "taxi")
				return "default";
			if (compact == "nofpl" || compact == "noflightplan")
				return "nofpl";
			if (compact == "airarr" || compact == "airborne" || compact == "airbornearrival")
				return "airarr";
			if (compact == "onrunway" || compact == "airarronrunway" || compact == "airbornearrivalonrunway")
				return "airarr_onrunway";
		}

		if (owner != nullptr)
			return owner->NormalizeStructuredRuleStatus(status);
		return lowered;
	}

	std::string RuleStatusUiLabel(CSMRRadar* owner, const std::string& type, const std::string& status)
	{
		const std::string normalizedType = NormalizeRuleTypeUiValue(owner, type);
		const std::string normalizedStatus = NormalizeRuleStatusUiValue(owner, type, status);

		if (normalizedStatus.empty() || normalizedStatus == "any")
			return "Any";
		if (normalizedStatus == "default")
		{
			if (normalizedType == "arrival")
				return "On Ground";
			if (normalizedType == "departure")
				return "No Status";
			return "Default";
		}
		if (normalizedStatus == "nofpl")
			return "No FPL";
		if (normalizedStatus == "push")
			return "Push";
		if (normalizedStatus == "stup")
			return "Startup";
		if (normalizedStatus == "taxi")
			return "Taxi";
		if (normalizedStatus == "depa")
			return "Departure";
		if (normalizedStatus == "airdep")
			return (normalizedType == "departure") ? "Airborne" : "Airborne Departure";
		if (normalizedStatus == "airarr")
			return (normalizedType == "arrival") ? "Airborne" : "Airborne Arrival";
		if (normalizedStatus == "airdep_onrunway")
			return (normalizedType == "departure") ? "On Runway" : "Airborne Departure On Runway";
		if (normalizedStatus == "airarr_onrunway")
			return (normalizedType == "arrival") ? "On Runway" : "Airborne Arrival On Runway";

		if (owner != nullptr)
			return owner->TagDefinitionDepartureStatusLabel(normalizedStatus);
		return status;
	}

	std::vector<std::string> RuleStatusUiOptions(CSMRRadar* owner, const std::string& type)
	{
		const std::string normalizedType = NormalizeRuleTypeUiValue(owner, type);
		if (normalizedType == "departure")
			return { "Any", "No Status", "No FPL", "Push", "Startup", "Taxi", "Departure", "Airborne", "On Runway" };
		if (normalizedType == "arrival")
			return { "Any", "On Ground", "No FPL", "Airborne", "On Runway" };
		return { "Any" };
	}

	std::string NormalizeTagTypeUiValue(CSMRRadar* owner, const std::string& type)
	{
		if (owner != nullptr)
			return owner->NormalizeTagDefinitionType(type);

		std::string lowered = TrimAsciiWhitespaceCopy(type);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowered == "dep")
			return "departure";
		if (lowered == "arr")
			return "arrival";
		if (lowered == "air")
			return "airborne";
		if (lowered == "departure" || lowered == "arrival" || lowered == "airborne" || lowered == "uncorrelated")
			return lowered;
		return "departure";
	}

	std::string TagTypeUiLabel(CSMRRadar* owner, const std::string& type)
	{
		const std::string normalizedType = NormalizeTagTypeUiValue(owner, type);
		if (owner != nullptr)
			return owner->TagDefinitionTypeLabel(normalizedType);
		if (normalizedType == "arrival")
			return "Arrival";
		if (normalizedType == "airborne")
			return "Airborne";
		if (normalizedType == "uncorrelated")
			return "Uncorrelated";
		return "Departure";
	}

	std::string TagStatusUiLabel(CSMRRadar* owner, const std::string& type, const std::string& status)
	{
		const std::string normalizedType = NormalizeTagTypeUiValue(owner, type);
		std::string normalizedStatus = status;
		if (owner != nullptr)
			normalizedStatus = owner->NormalizeTagDefinitionDepartureStatus(status);
		else
		{
			normalizedStatus = TrimAsciiWhitespaceCopy(status);
			std::transform(normalizedStatus.begin(), normalizedStatus.end(), normalizedStatus.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		}

		if (normalizedType == "departure")
		{
			if (normalizedStatus == "default" || normalizedStatus == "nsts")
				return "No Status";
			if (normalizedStatus == "nofpl")
				return "No FPL";
			if (normalizedStatus == "push")
				return "Push";
			if (normalizedStatus == "stup")
				return "Startup";
			if (normalizedStatus == "taxi")
				return "Taxi";
			if (normalizedStatus == "depa")
				return "Departure";
			if (normalizedStatus == "airdep")
				return "Airborne";
			if (normalizedStatus == "airdep_onrunway")
				return "On Runway";
		}
		else if (normalizedType == "arrival")
		{
			if (normalizedStatus == "default" || normalizedStatus == "arr" || normalizedStatus == "taxi")
				return "On Ground";
			if (normalizedStatus == "nofpl")
				return "No FPL";
			if (normalizedStatus == "airarr")
				return "Airborne";
			if (normalizedStatus == "airarr_onrunway")
				return "On Runway";
		}
		else if (normalizedType == "airborne")
		{
			if (normalizedStatus == "airdep" || normalizedStatus == "airarr")
				return "Airborne";
			if (normalizedStatus == "airdep_onrunway" || normalizedStatus == "airarr_onrunway")
				return "On Runway";
		}

		if (owner != nullptr)
			return owner->TagDefinitionDepartureStatusLabel(normalizedStatus);
		return status;
	}

	std::vector<std::string> SplitColorPathForDisplay(const std::string& path)
	{
		std::vector<std::string> segments;
		std::string current;
		current.reserve(path.size());
		for (char ch : path)
		{
			if (ch == '.')
			{
				if (!current.empty())
				{
					segments.push_back(current);
					current.clear();
				}
				continue;
			}
			current.push_back(ch);
		}
		if (!current.empty())
			segments.push_back(current);
		return segments;
	}

	std::string FormatColorPathSegmentForDisplay(const std::string& segment)
	{
		if (_stricmp(segment.c_str(), "targets") == 0)
			return "Icons";
		if (_stricmp(segment.c_str(), "labels") == 0)
			return "Tags";
		if (_stricmp(segment.c_str(), "rimcas") == 0)
			return "RIMCAS";
		if (_stricmp(segment.c_str(), "ui_layout") == 0)
			return "UI Layout";
		if (_stricmp(segment.c_str(), "pro_mode") == 0)
			return "Pro Mode";
		if (_stricmp(segment.c_str(), "ground_icons") == 0)
			return "Ground Icons";
		if (_stricmp(segment.c_str(), "status_background_colors") == 0)
			return "Status Background Colors";
		if (_stricmp(segment.c_str(), "sid_text_colors") == 0)
			return "SID Text Colors";
		if (_stricmp(segment.c_str(), "no_fpl") == 0)
			return "No FPL";
		if (_stricmp(segment.c_str(), "no_status") == 0)
			return "No Status";
		if (_stricmp(segment.c_str(), "on_ground") == 0)
			return "On Ground";
		if (_stricmp(segment.c_str(), "background_on_ground_color") == 0)
			return "Background On Ground";
		if (_stricmp(segment.c_str(), "background_on_runway_color") == 0 || _stricmp(segment.c_str(), "on_runway_color") == 0 || _stricmp(segment.c_str(), "background_color_on_runway") == 0)
			return "Background On Runway";
		if (_stricmp(segment.c_str(), "background_no_fpl_color") == 0 || _stricmp(segment.c_str(), "nofpl_color") == 0)
			return "Background No FPL";
		if (_stricmp(segment.c_str(), "text_on_ground_color") == 0 || _stricmp(segment.c_str(), "text_color") == 0)
			return "Text On Ground";
		if (_stricmp(segment.c_str(), "background_no_status_color") == 0 || _stricmp(segment.c_str(), "gate_color") == 0)
			return "Background No Status";
		if (_stricmp(segment.c_str(), "background_no_sid_color") == 0 || _stricmp(segment.c_str(), "nosid_color") == 0)
			return "Background No SID";
		if (_stricmp(segment.c_str(), "background_departure_color") == 0 || _stricmp(segment.c_str(), "departure_color") == 0)
			return "Background Departure";
		if (_stricmp(segment.c_str(), "background_push_color") == 0 || _stricmp(segment.c_str(), "push_color") == 0)
			return "Background Push";
		if (_stricmp(segment.c_str(), "background_startup_color") == 0 || _stricmp(segment.c_str(), "startup_color") == 0)
			return "Background Startup";
		if (_stricmp(segment.c_str(), "background_taxi_color") == 0 || _stricmp(segment.c_str(), "taxi_color") == 0)
			return "Background Taxi";
		if (_stricmp(segment.c_str(), "background_airborne_color") == 0)
			return "Background Airborne";
		if (_stricmp(segment.c_str(), "text_airborne_color") == 0)
			return "Text Airborne";

		std::string display = segment;
		bool capitalizeNext = true;
		for (char& ch : display)
		{
			if (ch == '_')
			{
				ch = ' ';
				capitalizeNext = true;
				continue;
			}
			if (capitalizeNext && std::isalpha(static_cast<unsigned char>(ch)) != 0)
			{
				ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
				capitalizeNext = false;
			}
			else
			{
				capitalizeNext = (ch == ' ');
			}
		}
		return display;
	}

	std::string FormatColorPathForDisplay(const std::string& path)
	{
		if (_stricmp(path.c_str(), "labels.departure.background_no_status_color") == 0 ||
			_stricmp(path.c_str(), "labels.departure.gate_color") == 0 ||
			_stricmp(path.c_str(), "labels.departure.background_color") == 0)
		{
			return "Tags > Departure > Background No Status";
		}
		if (_stricmp(path.c_str(), "labels.uncorrelated.background_on_runway_color") == 0)
		{
			return "Tags > Uncorrelated > Background On Runways";
		}

		const std::vector<std::string> segments = SplitColorPathForDisplay(path);
		std::string display;
		for (size_t i = 0; i < segments.size(); ++i)
		{
			if (!display.empty())
				display += " > ";
			display += FormatColorPathSegmentForDisplay(segments[i]);
		}
		return display;
	}

	COLORREF BlendColorOverBackground(COLORREF foreground, int alpha, COLORREF background)
	{
		const int clampedAlpha = min(255, max(0, alpha));
		const double a = static_cast<double>(clampedAlpha) / 255.0;
		const int r = static_cast<int>(round((GetRValue(foreground) * a) + (GetRValue(background) * (1.0 - a))));
		const int g = static_cast<int>(round((GetGValue(foreground) * a) + (GetGValue(background) * (1.0 - a))));
		const int b = static_cast<int>(round((GetBValue(foreground) * a) + (GetBValue(background) * (1.0 - a))));
		return RGB(r, g, b);
	}

	void MoveControlOffscreen(CWnd& control)
	{
		control.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	}

	void ApplyRoundedWindowRegion(HWND hwnd, int radius = 10)
	{
		if (!::IsWindow(hwnd))
			return;

		RECT bounds = {};
		::GetWindowRect(hwnd, &bounds);
		const int width = max(1, bounds.right - bounds.left);
		const int height = max(1, bounds.bottom - bounds.top);
		HRGN region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
		if (region == nullptr)
			return;

		if (::SetWindowRgn(hwnd, region, TRUE) == 0)
			::DeleteObject(region);
	}

	std::string JoinTagDefinitionLinesForEditor(const std::vector<std::string>& lines)
	{
		size_t lastLine = lines.size();
		while (lastLine > 0 && lines[lastLine - 1].empty())
			--lastLine;

		std::string text;
		for (size_t i = 0; i < lastLine; ++i)
		{
			text += lines[i];
			if (i + 1 < lastLine)
				text += "\r\n";
		}
		return text;
	}

	std::vector<std::string> SplitTagDefinitionEditorText(const std::string& text, size_t maxLines)
	{
		std::vector<std::string> lines;
		lines.reserve(maxLines);

		std::string currentLine;
		for (size_t i = 0; i < text.size(); ++i)
		{
			const char ch = text[i];
			if (ch == '\r' || ch == '\n')
			{
				lines.push_back(currentLine);
				currentLine.clear();
				if (lines.size() >= maxLines)
					break;
				if (ch == '\r' && (i + 1) < text.size() && text[i + 1] == '\n')
					++i;
				continue;
			}

			if (lines.size() < maxLines)
				currentLine.push_back(ch);
		}

		if (lines.size() < maxLines)
			lines.push_back(currentLine);

		while (lines.size() < maxLines)
			lines.push_back("");
		if (lines.size() > maxLines)
			lines.resize(maxLines);

		return lines;
	}

	void ShowControls(const std::initializer_list<CWnd*>& controls, int showMode)
	{
		for (CWnd* control : controls)
		{
			if (control != nullptr && ::IsWindow(control->GetSafeHwnd()))
				control->ShowWindow(showMode);
		}
	}

	int MeasureWrappedStaticHeight(CWnd& control, int width)
	{
		if (!::IsWindow(control.GetSafeHwnd()))
			return 0;

		CString text;
		control.GetWindowText(text);
		if (text.IsEmpty())
			return 0;

		CClientDC dc(&control);
		CFont* font = control.GetFont();
		CFont* oldFont = (font != nullptr) ? dc.SelectObject(font) : nullptr;
		CRect textRect(0, 0, max(1, width), 0);
		dc.DrawText(text, &textRect, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
		if (oldFont != nullptr)
			dc.SelectObject(oldFont);
		return textRect.Height();
	}



	void HsvToRgb(double hue, double saturation, double value, int& outR, int& outG, int& outB)
	{
		const double h = fmod(fmod(hue, 360.0) + 360.0, 360.0);
		const double s = min(1.0, max(0.0, saturation));
		const double v = min(1.0, max(0.0, value));

		const double c = v * s;
		const double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
		const double m = v - c;

		double rr = 0.0;
		double gg = 0.0;
		double bb = 0.0;
		if (h < 60.0) { rr = c; gg = x; bb = 0.0; }
		else if (h < 120.0) { rr = x; gg = c; bb = 0.0; }
		else if (h < 180.0) { rr = 0.0; gg = c; bb = x; }
		else if (h < 240.0) { rr = 0.0; gg = x; bb = c; }
		else if (h < 300.0) { rr = x; gg = 0.0; bb = c; }
		else { rr = c; gg = 0.0; bb = x; }

		outR = static_cast<int>(round((rr + m) * 255.0));
		outG = static_cast<int>(round((gg + m) * 255.0));
		outB = static_cast<int>(round((bb + m) * 255.0));
	}

	void RgbToHsv(int r, int g, int b, double& outHue, double& outSaturation, double& outValue)
	{
		const double rf = min(1.0, max(0.0, r / 255.0));
		const double gf = min(1.0, max(0.0, g / 255.0));
		const double bf = min(1.0, max(0.0, b / 255.0));

		const double maxValue = max(rf, max(gf, bf));
		const double minValue = min(rf, min(gf, bf));
		const double delta = maxValue - minValue;

		outValue = maxValue;
		outSaturation = (maxValue <= 0.0) ? 0.0 : (delta / maxValue);

		if (delta <= 0.0)
		{
			outHue = 0.0;
			return;
		}

		double hue = 0.0;
		if (maxValue == rf)
			hue = 60.0 * fmod(((gf - bf) / delta), 6.0);
		else if (maxValue == gf)
			hue = 60.0 * (((bf - rf) / delta) + 2.0);
		else
			hue = 60.0 * (((rf - gf) / delta) + 4.0);

		if (hue < 0.0)
			hue += 360.0;
		outHue = hue;
	}

	bool TryReadHueSaturationFromWheel(CStatic& wheelControl, const CPoint& screenPoint, double& outHue, double& outSaturation)
	{
		if (!::IsWindow(wheelControl.GetSafeHwnd()))
			return false;

		CRect wheelRectScreen;
		wheelControl.GetWindowRect(&wheelRectScreen);
		if (!wheelRectScreen.PtInRect(screenPoint))
			return false;

		const int localX = screenPoint.x - wheelRectScreen.left;
		const int localY = screenPoint.y - wheelRectScreen.top;
		const int width = wheelRectScreen.Width();
		const int height = wheelRectScreen.Height();
		const int innerLeft = 2;
		const int innerTop = 2;
		const int innerWidth = max(1, width - (innerLeft * 2));
		const int innerHeight = max(1, height - (innerTop * 2));
		const int diameter = min(innerWidth, innerHeight);
		const int radius = max(1, (diameter / 2) - 2);
		const double centerX = static_cast<double>(innerLeft + (innerWidth / 2));
		const double centerY = static_cast<double>(innerTop + (innerHeight / 2));
		const double dx = static_cast<double>(localX) - centerX;
		const double dy = centerY - static_cast<double>(localY);
		const double distance = sqrt((dx * dx) + (dy * dy));
		if (distance > static_cast<double>(radius))
			return false;

		double hue = atan2(dy, dx) * (180.0 / 3.14159265358979323846);
		if (hue < 0.0)
			hue += 360.0;
		outHue = hue;
		outSaturation = min(1.0, max(0.0, distance / static_cast<double>(radius)));
		return true;
	}

	bool TryReadVerticalSliderPosFromPoint(CSliderCtrl& sliderControl, const CPoint& screenPoint, int& outPos)
	{
		if (!::IsWindow(sliderControl.GetSafeHwnd()))
			return false;

		CRect sliderRectScreen;
		sliderControl.GetWindowRect(&sliderRectScreen);

		CRect clientRect;
		sliderControl.GetClientRect(&clientRect);
		if (clientRect.Height() <= 0)
			return false;

		const int channelTop = clientRect.top + 4;
		const int channelBottomExclusive = max(channelTop + 1, clientRect.bottom - 4);
		const int channelHeight = channelBottomExclusive - channelTop;
		const int localY = screenPoint.y - sliderRectScreen.top;
		const int clampedY = min(channelBottomExclusive - 1, max(channelTop, localY));
		const double t = 1.0 - (static_cast<double>(clampedY - channelTop) / static_cast<double>(max(1, channelHeight - 1)));
		outPos = min(100, max(0, static_cast<int>(round(t * 100.0))));
		return true;
	}

	void DrawVerticalValueSlider(CDC& dc, const CRect& clientRect, int sliderPos, int topR, int topG, int topB)
	{
		const int channelWidth = max(10, min(16, clientRect.Width() - 8));
		const int channelLeft = clientRect.left + ((clientRect.Width() - channelWidth) / 2);
		const int channelTop = clientRect.top + 4;
		const int channelBottom = clientRect.bottom - 4;
		CRect channelRect(channelLeft, channelTop, channelLeft + channelWidth, channelBottom);

		CRgn clipRegion;
		clipRegion.CreateRoundRectRgn(channelRect.left, channelRect.top, channelRect.right + 1, channelRect.bottom + 1, 8, 8);
		const int savedDc = dc.SaveDC();
		dc.SelectClipRgn(&clipRegion);
		const int gradientHeight = max(1, channelRect.Height());
		for (int y = 0; y < gradientHeight; ++y)
		{
			const double t = min(1.0, max(0.0, static_cast<double>(y) / static_cast<double>(max(1, gradientHeight - 1))));
			const int r = static_cast<int>(round(static_cast<double>(topR) * (1.0 - t)));
			const int g = static_cast<int>(round(static_cast<double>(topG) * (1.0 - t)));
			const int b = static_cast<int>(round(static_cast<double>(topB) * (1.0 - t)));
			dc.FillSolidRect(channelRect.left, channelRect.top + y, channelRect.Width(), 1, RGB(r, g, b));
		}
		dc.RestoreDC(savedDc);

		CPen channelBorder(PS_SOLID, 1, RGB(186, 186, 186));
		CPen* oldPen = dc.SelectObject(&channelBorder);
		CBrush* oldBrush = static_cast<CBrush*>(dc.SelectStockObject(HOLLOW_BRUSH));
		dc.RoundRect(&channelRect, CPoint(8, 8));

		const int clampedSliderPos = min(100, max(0, sliderPos));
		const double sliderT = static_cast<double>(clampedSliderPos) / 100.0;
		const int thumbHeight = 14;
		const int thumbHalf = thumbHeight / 2;
		const int centerY = channelRect.top + static_cast<int>(round((1.0 - sliderT) * static_cast<double>(max(1, channelRect.Height() - 1))));
		const int thumbLeft = max(clientRect.left + 1, channelRect.left - 3);
		const int thumbRight = min(clientRect.right - 1, channelRect.right + 3);
		const int thumbTop = max(clientRect.top + 1, centerY - thumbHalf);
		const int thumbBottom = min(clientRect.bottom - 1, centerY + thumbHalf + 1);
		CRect thumbRect(thumbLeft, thumbTop, thumbRight, thumbBottom);
		thumbRect.DeflateRect(1, 1);
		CBrush thumbOuterBrush(RGB(255, 255, 255));
		CPen thumbOuterPen(PS_SOLID, 1, RGB(212, 212, 212));
		dc.SelectObject(&thumbOuterPen);
		dc.SelectObject(&thumbOuterBrush);
		dc.RoundRect(&thumbRect, CPoint(10, 10));

		CRect thumbInner = thumbRect;
		thumbInner.DeflateRect(3, 3);
		CBrush thumbInnerBrush(RGB(64, 182, 227));
		CPen thumbInnerPen(PS_SOLID, 1, RGB(64, 182, 227));
		dc.SelectObject(&thumbInnerPen);
		dc.SelectObject(&thumbInnerBrush);
		dc.RoundRect(&thumbInner, CPoint(8, 8));

		dc.SelectObject(oldBrush);
		dc.SelectObject(oldPen);
	}

	void DrawHorizontalModernSlider(CDC& dc, const CRect& clientRect, CSliderCtrl& sliderControl)
	{
		dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

		CRect channelRect(
			clientRect.left + 10,
			clientRect.top + (clientRect.Height() / 2) - 3,
			clientRect.right - 10,
			clientRect.top + (clientRect.Height() / 2) + 3);
		if (channelRect.Width() < 10)
			return;

		const int rangeMin = sliderControl.GetRangeMin();
		const int rangeMax = sliderControl.GetRangeMax();
		const int rangeSpan = max(1, rangeMax - rangeMin);
		const int clampedPos = min(rangeMax, max(rangeMin, sliderControl.GetPos()));
		const double t = static_cast<double>(clampedPos - rangeMin) / static_cast<double>(rangeSpan);
		const int fillRight = channelRect.left + static_cast<int>(round(t * static_cast<double>(max(1, channelRect.Width() - 1))));

		const int tickTop = channelRect.bottom + 4;
		const int tickBottom = min(clientRect.bottom - 2, tickTop + 5);
		CPen tickPen(PS_SOLID, 1, RGB(172, 181, 193));
		CPen* oldTickPen = dc.SelectObject(&tickPen);
		for (int tickValue = rangeMin; tickValue <= rangeMax; ++tickValue)
		{
			const double tickT = static_cast<double>(tickValue - rangeMin) / static_cast<double>(rangeSpan);
			const int tickX = channelRect.left + static_cast<int>(round(tickT * static_cast<double>(max(1, channelRect.Width() - 1))));
			dc.MoveTo(tickX, tickTop);
			dc.LineTo(tickX, tickBottom);
		}
		dc.SelectObject(oldTickPen);

		CBrush baseBrush(RGB(204, 213, 224));
		CPen baseBorder(PS_SOLID, 1, RGB(184, 194, 206));
		CPen* oldPen = dc.SelectObject(&baseBorder);
		CBrush* oldBrush = dc.SelectObject(&baseBrush);
		dc.RoundRect(&channelRect, CPoint(6, 6));

		CRect fillRect(channelRect.left, channelRect.top, max(channelRect.left + 1, fillRight + 1), channelRect.bottom);
		CBrush fillBrush(RGB(63, 120, 208));
		CPen fillBorder(PS_SOLID, 1, RGB(63, 120, 208));
		dc.SelectObject(&fillBorder);
		dc.SelectObject(&fillBrush);
		dc.RoundRect(&fillRect, CPoint(6, 6));

		const int thumbCenterX = min(channelRect.right, max(channelRect.left, fillRight));
		const int thumbCenterY = channelRect.top + (channelRect.Height() / 2);
		CRect thumbOuter(thumbCenterX - 8, thumbCenterY - 8, thumbCenterX + 9, thumbCenterY + 9);
		CBrush thumbOuterBrush(RGB(255, 255, 255));
		CPen thumbOuterPen(PS_SOLID, 1, RGB(188, 198, 210));
		dc.SelectObject(&thumbOuterPen);
		dc.SelectObject(&thumbOuterBrush);
		dc.Ellipse(&thumbOuter);

		CRect thumbInner(thumbCenterX - 4, thumbCenterY - 4, thumbCenterX + 5, thumbCenterY + 5);
		CBrush thumbInnerBrush(RGB(63, 120, 208));
		CPen thumbInnerPen(PS_SOLID, 1, RGB(63, 120, 208));
		dc.SelectObject(&thumbInnerPen);
		dc.SelectObject(&thumbInnerBrush);
		dc.Ellipse(&thumbInner);

		dc.SelectObject(oldBrush);
		dc.SelectObject(oldPen);
	}

	LRESULT HandleTrackingControlWndProc(
		HWND hwnd,
		UINT message,
		WPARAM wParam,
		LPARAM lParam,
		std::map<HWND, WNDPROC>& oldProcMap,
		std::map<HWND, HWND>& ownerMap,
		UINT trackMessage)
	{
		auto oldIt = oldProcMap.find(hwnd);
		WNDPROC oldProc = (oldIt != oldProcMap.end()) ? oldIt->second : DefWindowProc;

		auto sendTrackMessage = [&](int x, int y)
		{
			auto ownerIt = ownerMap.find(hwnd);
			if (ownerIt == ownerMap.end() || !::IsWindow(ownerIt->second))
				return;

			POINT screenPoint = { x, y };
			::ClientToScreen(hwnd, &screenPoint);
			::SendMessage(ownerIt->second, trackMessage, static_cast<WPARAM>(screenPoint.x), static_cast<LPARAM>(screenPoint.y));
		};

	switch (message)
	{
	case WM_LBUTTONDOWN:
		::SetCapture(hwnd);
		sendTrackMessage(static_cast<int>(static_cast<short>(LOWORD(lParam))), static_cast<int>(static_cast<short>(HIWORD(lParam))));
		return 0;
	case WM_MOUSEWHEEL:
		if (trackMessage == WM_PE_COLOR_VALUE_TRACK ||
			trackMessage == WM_PE_COLOR_OPACITY_TRACK ||
			trackMessage == WM_PE_RULE_COLOR_VALUE_TRACK ||
			trackMessage == WM_PE_RULE_COLOR_OPACITY_TRACK)
		{
			auto ownerIt = ownerMap.find(hwnd);
			if (ownerIt != ownerMap.end() && ::IsWindow(ownerIt->second))
			{
				const int rangeMin = static_cast<int>(::SendMessage(hwnd, TBM_GETRANGEMIN, 0, 0));
				const int rangeMax = static_cast<int>(::SendMessage(hwnd, TBM_GETRANGEMAX, 0, 0));
				const int currentPos = static_cast<int>(::SendMessage(hwnd, TBM_GETPOS, 0, 0));
				const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
				int wheelSteps = wheelDelta / WHEEL_DELTA;
				if (wheelSteps == 0)
					wheelSteps = (wheelDelta > 0) ? 1 : -1;

				const int newPos = min(rangeMax, max(rangeMin, currentPos + wheelSteps));
				if (newPos != currentPos)
				{
					::SendMessage(hwnd, TBM_SETPOS, TRUE, static_cast<LPARAM>(newPos));
					::SendMessage(ownerIt->second, WM_VSCROLL, MAKEWPARAM(TB_THUMBPOSITION, newPos), reinterpret_cast<LPARAM>(hwnd));
				}
				return 0;
			}
		}
		break;
	case WM_MOUSEMOVE:
		if ((wParam & MK_LBUTTON) != 0 && ::GetCapture() == hwnd)
		{
			sendTrackMessage(static_cast<int>(static_cast<short>(LOWORD(lParam))), static_cast<int>(static_cast<short>(HIWORD(lParam))));
			return 0;
			}
			break;
		case WM_LBUTTONUP:
			if (::GetCapture() == hwnd)
				::ReleaseCapture();
			sendTrackMessage(static_cast<int>(static_cast<short>(LOWORD(lParam))), static_cast<int>(static_cast<short>(HIWORD(lParam))));
			return 0;
		case WM_NCDESTROY:
		{
			const LRESULT result = ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);
			oldProcMap.erase(hwnd);
			ownerMap.erase(hwnd);
			return result;
		}
		default:
			break;
		}

		return ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);
	}

	LRESULT CALLBACK ColorWheelWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gColorWheelOldProcs,
			gColorWheelOwnerWindows,
			WM_PE_COLOR_WHEEL_TRACK);
	}

	LRESULT CALLBACK ColorValueSliderWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gColorValueSliderOldProcs,
			gColorValueSliderOwnerWindows,
			WM_PE_COLOR_VALUE_TRACK);
	}

	LRESULT CALLBACK ColorOpacitySliderWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gColorOpacitySliderOldProcs,
			gColorOpacitySliderOwnerWindows,
			WM_PE_COLOR_OPACITY_TRACK);
	}

	LRESULT CALLBACK RuleColorWheelWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gRuleColorWheelOldProcs,
			gRuleColorWheelOwnerWindows,
			WM_PE_RULE_COLOR_WHEEL_TRACK);
	}

	LRESULT CALLBACK RuleColorValueSliderWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gRuleColorValueSliderOldProcs,
			gRuleColorValueSliderOwnerWindows,
			WM_PE_RULE_COLOR_VALUE_TRACK);
	}

	LRESULT CALLBACK RuleColorOpacitySliderWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return HandleTrackingControlWndProc(
			hwnd,
			message,
			wParam,
			lParam,
			gRuleColorOpacitySliderOldProcs,
			gRuleColorOpacitySliderOwnerWindows,
			WM_PE_RULE_COLOR_OPACITY_TRACK);
	}

	void DrawThemedBorder(HWND hwnd)
	{
		if (!::IsWindow(hwnd))
			return;

		HDC hdc = ::GetWindowDC(hwnd);
		if (hdc == nullptr)
			return;

		RECT bounds = {};
		::GetWindowRect(hwnd, &bounds);
		::OffsetRect(&bounds, -bounds.left, -bounds.top);
		::InflateRect(&bounds, -1, -1);

		HWND focusedWindow = ::GetFocus();
		const bool isFocused = (focusedWindow == hwnd) ||
			(focusedWindow != nullptr && (::IsChild(hwnd, focusedWindow) != FALSE));
		const COLORREF borderColor = isFocused ? kEditorFocusBorderColor : kEditorBorderColor;
		HPEN borderPen = ::CreatePen(PS_SOLID, 1, borderColor);
		HGDIOBJ oldPen = ::SelectObject(hdc, borderPen);
		HGDIOBJ oldBrush = ::SelectObject(hdc, ::GetStockObject(HOLLOW_BRUSH));
		::RoundRect(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom, 10, 10);
		::SelectObject(hdc, oldBrush);
		::SelectObject(hdc, oldPen);
		::DeleteObject(borderPen);
		::ReleaseDC(hwnd, hdc);
	}

	LRESULT CALLBACK ThemedEditWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto it = gThemedEditOldProcs.find(hwnd);
		WNDPROC oldProc = (it != gThemedEditOldProcs.end()) ? it->second : DefWindowProc;

		if (message == WM_NCDESTROY)
		{
			const LRESULT result = ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);
			gThemedEditOldProcs.erase(hwnd);
			return result;
		}

		const LRESULT result = ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);

		switch (message)
		{
		case WM_PAINT:
		case WM_NCPAINT:
		case WM_PRINTCLIENT:
		case WM_ENABLE:
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		case WM_SETTEXT:
		case WM_WINDOWPOSCHANGED:
			DrawThemedBorder(hwnd);
			break;
		default:
			break;
		}

		return result;
	}

	LRESULT CALLBACK ThemedComboWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto it = gThemedComboOldProcs.find(hwnd);
		WNDPROC oldProc = (it != gThemedComboOldProcs.end()) ? it->second : DefWindowProc;

		if (message == WM_NCDESTROY)
		{
			const LRESULT result = ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);
			gThemedComboOldProcs.erase(hwnd);
			return result;
		}

		const LRESULT result = ::CallWindowProc(oldProc, hwnd, message, wParam, lParam);

		switch (message)
		{
		case WM_PAINT:
		case WM_NCPAINT:
		case WM_PRINTCLIENT:
		case WM_ENABLE:
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		case WM_SIZE:
		case WM_WINDOWPOSCHANGED:
		DrawThemedBorder(hwnd);
			break;
		default:
			break;
		}

		return result;
	}
}

CProfileEditorDialog::CProfileEditorDialog(CSMRRadar* owner, CWnd* pParent /*=NULL*/)
	: CDialogEx(CProfileEditorDialog::IDD, pParent)
	, Owner(owner)
{
}

CProfileEditorDialog::~CProfileEditorDialog()
{
	UnsubclassEditorControls();
	Owner = nullptr;
}

void CProfileEditorDialog::SetOwner(CSMRRadar* owner)
{
	Owner = owner;
}

void CProfileEditorDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BOOL CProfileEditorDialog::OnInitDialog()
{
	LogProfileEditorInitStep("OnInitDialog: begin");
	CDialogEx::OnInitDialog();
	LogProfileEditorInitStep("OnInitDialog: after CDialogEx::OnInitDialog");

	CWnd* statusWnd = GetDlgItem(IDC_PROFILE_EDITOR_STATUS);
	if (statusWnd != nullptr && ::IsWindow(statusWnd->GetSafeHwnd()))
	{
		LogProfileEditorInitStep("OnInitDialog: hiding legacy status control");
		statusWnd->ShowWindow(SW_HIDE);
	}

	LogProfileEditorInitStep("OnInitDialog: before CreateEditorControls");
	CreateEditorControls();
	LogProfileEditorInitStep("OnInitDialog: after CreateEditorControls");
	Initialized = true;
	LogProfileEditorInitStep("OnInitDialog: Initialized=true");
	LogProfileEditorInitStep("OnInitDialog: before SyncFromRadar");
	SyncFromRadar();
	LogProfileEditorInitStep("OnInitDialog: after SyncFromRadar");
	LogProfileEditorInitStep("OnInitDialog: before ForceChildRepaint");
	ForceChildRepaint();
	LogProfileEditorInitStep("OnInitDialog: after ForceChildRepaint");
	LogProfileEditorInitStep("OnInitDialog: before NotifyWindowRectChanged");
	NotifyWindowRectChanged();
	LogProfileEditorInitStep("OnInitDialog: after NotifyWindowRectChanged");
	LogProfileEditorInitStep("OnInitDialog: end");
	return TRUE;
}

void CProfileEditorDialog::SyncFromRadar()
{
	LogProfileEditorInitStep("SyncFromRadar: begin");
	if (!ControlsCreated || Owner == nullptr)
	{
		LogProfileEditorInitStep("SyncFromRadar: early return (controls missing or owner null)");
		return;
	}

	LogProfileEditorInitStep("SyncFromRadar: before RebuildColorPathList");
	RebuildColorPathList();
	LogProfileEditorInitStep("SyncFromRadar: after RebuildColorPathList");
	LogProfileEditorInitStep("SyncFromRadar: before RefreshEditorFieldsFromSelection");
	RefreshEditorFieldsFromSelection();
	LogProfileEditorInitStep("SyncFromRadar: after RefreshEditorFieldsFromSelection");
	LogProfileEditorInitStep("SyncFromRadar: before SyncIconControlsFromRadar");
	SyncIconControlsFromRadar();
	LogProfileEditorInitStep("SyncFromRadar: after SyncIconControlsFromRadar");
	LogProfileEditorInitStep("SyncFromRadar: before RebuildRulesList");
	RebuildRulesList();
	LogProfileEditorInitStep("SyncFromRadar: after RebuildRulesList");
	LogProfileEditorInitStep("SyncFromRadar: before RefreshRuleControls");
	RefreshRuleControls();
	LogProfileEditorInitStep("SyncFromRadar: after RefreshRuleControls");
	LogProfileEditorInitStep("SyncFromRadar: before RebuildProfileList");
	RebuildProfileList();
	LogProfileEditorInitStep("SyncFromRadar: after RebuildProfileList");
	LogProfileEditorInitStep("SyncFromRadar: before RefreshProfileControls");
	RefreshProfileControls();
	LogProfileEditorInitStep("SyncFromRadar: after RefreshProfileControls");
	LogProfileEditorInitStep("SyncFromRadar: before SyncTagEditorControlsFromRadar");
	SyncTagEditorControlsFromRadar();
	LogProfileEditorInitStep("SyncFromRadar: after SyncTagEditorControlsFromRadar");
	LogProfileEditorInitStep("SyncFromRadar: before UpdatePageVisibility");
	UpdatePageVisibility();
	LogProfileEditorInitStep("SyncFromRadar: after UpdatePageVisibility");
	LogProfileEditorInitStep("SyncFromRadar: before ForceChildRepaint");
	ForceChildRepaint();
	LogProfileEditorInitStep("SyncFromRadar: after ForceChildRepaint");
	LogProfileEditorInitStep("SyncFromRadar: end");
}

void CProfileEditorDialog::HideAndNotifyOwner()
{
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnProfileEditorWindowClosed();
}

void CProfileEditorDialog::NotifyWindowRectChanged()
{
	if (!Initialized || Owner == nullptr || !::IsWindow(GetSafeHwnd()) || !IsWindowVisible() || IsIconic())
		return;

	CRect windowRect;
	GetWindowRect(&windowRect);
	Owner->OnProfileEditorWindowLayoutChanged(windowRect);
}

void CProfileEditorDialog::OnCancel()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnOK()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnClose()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnMove(int x, int y)
{
	CDialogEx::OnMove(x, y);
	NotifyWindowRectChanged();
}

void CProfileEditorDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	LayoutControls();
	ForceChildRepaint();
	NotifyWindowRectChanged();
}

void CProfileEditorDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI == nullptr)
		return;

	const int sidebarWidth = 128;
	const int mainPad = 18;
	const int innerPad = 16;
	const int splitGap = 16;
	const int minIconsColumnWidth = 280;
	lpMMI->ptMinTrackSize.x = sidebarWidth + (mainPad * 2) + (innerPad * 2) + splitGap + (minIconsColumnWidth * 2);
	lpMMI->ptMinTrackSize.y = 700;
}

bool CProfileEditorDialog::HandleColorSliderScroll(CScrollBar* pScrollBar, UINT nSBCode, UINT nPos)
{
	if (pScrollBar == nullptr)
		return false;

	const HWND sourceHwnd = pScrollBar->GetSafeHwnd();
	auto applyThumbPosition = [&](CSliderCtrl& slider)
	{
		if (nSBCode == TB_THUMBTRACK || nSBCode == TB_THUMBPOSITION)
			slider.SetPos(static_cast<int>(nPos));
	};

	if (sourceHwnd == ColorValueSlider.GetSafeHwnd())
	{
		applyThumbPosition(ColorValueSlider);
		ApplyDraftColorValueFromSlider();
		return true;
	}
	if (sourceHwnd == ColorOpacitySlider.GetSafeHwnd())
	{
		applyThumbPosition(ColorOpacitySlider);
		ApplyDraftColorOpacityFromSlider();
		return true;
	}
	if (sourceHwnd == RuleColorValueSlider.GetSafeHwnd())
	{
		applyThumbPosition(RuleColorValueSlider);
		ApplyRuleColorValueFromSlider();
		return true;
	}
	if (sourceHwnd == RuleColorOpacitySlider.GetSafeHwnd())
	{
		applyThumbPosition(RuleColorOpacitySlider);
		ApplyRuleColorOpacityFromSlider();
		return true;
	}

	return false;
}

void CProfileEditorDialog::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	if (pScrollBar != nullptr)
	{
		const HWND sourceHwnd = pScrollBar->GetSafeHwnd();
		if (sourceHwnd == FixedScaleSlider.GetSafeHwnd())
		{
			int snappedPos = FixedScaleSlider.GetPos();
			if (nSBCode == TB_THUMBTRACK || nSBCode == TB_THUMBPOSITION)
				snappedPos = static_cast<int>(nPos);
			snappedPos = min(FixedScaleSlider.GetRangeMax(), max(FixedScaleSlider.GetRangeMin(), snappedPos));
			FixedScaleSlider.SetPos(snappedPos);
			UpdateIconScaleValueLabels();
			if (!UpdatingControls && Owner != nullptr)
			{
				const double scale = SliderPosToScale(snappedPos);
				if (Owner->SetFixedPixelTriangleIconScale(scale, true))
					Owner->RequestRefresh();
			}
			FixedScaleSlider.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		}
		else if (sourceHwnd == BoostFactorSlider.GetSafeHwnd())
		{
			int snappedPos = BoostFactorSlider.GetPos();
			if (nSBCode == TB_THUMBTRACK || nSBCode == TB_THUMBPOSITION)
				snappedPos = static_cast<int>(nPos);
			snappedPos = min(BoostFactorSlider.GetRangeMax(), max(BoostFactorSlider.GetRangeMin(), snappedPos));
			BoostFactorSlider.SetPos(snappedPos);
			UpdateIconScaleValueLabels();
			if (!UpdatingControls && Owner != nullptr)
			{
				const double scale = SliderPosToScale(snappedPos);
				if (Owner->SetSmallTargetIconBoostFactor(scale, true))
					Owner->RequestRefresh();
			}
			BoostFactorSlider.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		}
		else
		{
			HandleColorSliderScroll(pScrollBar, nSBCode, nPos);
		}
	}

	CDialogEx::OnHScroll(nSBCode, nPos, pScrollBar);
}

void CProfileEditorDialog::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	HandleColorSliderScroll(pScrollBar, nSBCode, nPos);

	CDialogEx::OnVScroll(nSBCode, nPos, pScrollBar);
}

void CProfileEditorDialog::OnShowWindow(BOOL bShow, UINT nStatus)
{
	CDialogEx::OnShowWindow(bShow, nStatus);
	if (bShow && ControlsCreated)
	{
		LayoutControls();
		UpdatePageVisibility();
		ForceChildRepaint();
	}
}

void CProfileEditorDialog::OnDestroy()
{
	UnsubclassEditorControls();
	CDialogEx::OnDestroy();
}

void CProfileEditorDialog::ForceChildRepaint()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;

	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetNextWindow())
	{
		if (::IsWindow(child->GetSafeHwnd()))
		{
			child->Invalidate(FALSE);
			child->UpdateWindow();
		}
	}
}

void CProfileEditorDialog::UnsubclassEditorControls()
{
	auto restoreThemedEditProc = [](HWND hwnd)
	{
		if (hwnd == nullptr)
			return;

		auto it = gThemedEditOldProcs.find(hwnd);
		if (it == gThemedEditOldProcs.end())
			return;

		if (::IsWindow(hwnd) && it->second != nullptr)
			::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second));
		gThemedEditOldProcs.erase(it);
	};

	auto restoreThemedComboProc = [](HWND hwnd)
	{
		if (hwnd == nullptr)
			return;

		auto it = gThemedComboOldProcs.find(hwnd);
		if (it == gThemedComboOldProcs.end())
			return;

		if (::IsWindow(hwnd) && it->second != nullptr)
			::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second));
		gThemedComboOldProcs.erase(it);
	};

	auto restoreTrackingProcMap = [&](std::map<HWND, WNDPROC>& procMap, std::map<HWND, HWND>& ownerMap)
	{
		const HWND ownerHwnd = GetSafeHwnd();
		std::vector<HWND> trackedHwnds;
		trackedHwnds.reserve(ownerMap.size());
		for (const auto& entry : ownerMap)
		{
			if (entry.second == ownerHwnd)
				trackedHwnds.push_back(entry.first);
		}

		for (HWND trackedHwnd : trackedHwnds)
		{
			auto procIt = procMap.find(trackedHwnd);
			if (procIt != procMap.end())
			{
				if (::IsWindow(trackedHwnd) && procIt->second != nullptr)
					::SetWindowLongPtr(trackedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procIt->second));
				procMap.erase(procIt);
			}
			ownerMap.erase(trackedHwnd);
		}
	};

	restoreThemedEditProc(EditRgba.GetSafeHwnd());
	restoreThemedEditProc(EditHex.GetSafeHwnd());
	restoreThemedEditProc(RuleTargetEdit.GetSafeHwnd());
	restoreThemedEditProc(RuleTagEdit.GetSafeHwnd());
	restoreThemedEditProc(RuleTextEdit.GetSafeHwnd());
	restoreThemedEditProc(TagLine1Edit.GetSafeHwnd());
	restoreThemedEditProc(TagLine2Edit.GetSafeHwnd());
	restoreThemedEditProc(TagLine3Edit.GetSafeHwnd());
	restoreThemedEditProc(TagLine4Edit.GetSafeHwnd());
	restoreThemedEditProc(TagDetailedLine1Edit.GetSafeHwnd());
	restoreThemedEditProc(TagDetailedLine2Edit.GetSafeHwnd());
	restoreThemedEditProc(TagDetailedLine3Edit.GetSafeHwnd());
	restoreThemedEditProc(TagDetailedLine4Edit.GetSafeHwnd());
	restoreThemedComboProc(FixedScaleCombo.GetSafeHwnd());
	restoreThemedComboProc(BoostFactorCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleSourceCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleTokenCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleConditionCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleTypeCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleStatusCombo.GetSafeHwnd());
	restoreThemedComboProc(RuleDetailCombo.GetSafeHwnd());
	restoreThemedComboProc(TagTypeCombo.GetSafeHwnd());
	restoreThemedComboProc(TagStatusCombo.GetSafeHwnd());
	restoreThemedComboProc(TagTokenCombo.GetSafeHwnd());
	restoreThemedComboProc(BoostResolutionCombo.GetSafeHwnd());

	restoreTrackingProcMap(gColorWheelOldProcs, gColorWheelOwnerWindows);
	restoreTrackingProcMap(gColorValueSliderOldProcs, gColorValueSliderOwnerWindows);
	restoreTrackingProcMap(gColorOpacitySliderOldProcs, gColorOpacitySliderOwnerWindows);
	restoreTrackingProcMap(gRuleColorWheelOldProcs, gRuleColorWheelOwnerWindows);
	restoreTrackingProcMap(gRuleColorValueSliderOldProcs, gRuleColorValueSliderOwnerWindows);
	restoreTrackingProcMap(gRuleColorOpacitySliderOldProcs, gRuleColorOpacitySliderOwnerWindows);
}

HBRUSH CProfileEditorDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
	if (pWnd == nullptr)
		return hbr;
	const int controlId = pWnd->GetDlgCtrlID();
	// Let Windows/theming render button-class controls (radio/check/push) normally.
	// Custom CTLCOLOR brushes on CTLCOLOR_BTN can make controls disappear until interaction.
	if (nCtlColor == CTLCOLOR_BTN)
		return hbr;

	if (nCtlColor == CTLCOLOR_LISTBOX && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		pDC->SetTextColor(RGB(17, 24, 39));
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}
	if (nCtlColor == CTLCOLOR_EDIT && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		const bool isThemedEditControl = [&]()
		{
			switch (controlId)
			{
			case IDC_PE_EDIT_RGBA:
			case IDC_PE_EDIT_HEX:
			case IDC_PE_RULE_NAME_EDIT:
			case IDC_PE_RULE_TARGET_EDIT:
			case IDC_PE_RULE_TAG_EDIT:
			case IDC_PE_RULE_TEXT_EDIT:
			case IDC_PE_PROFILE_NAME_EDIT:
			case IDC_PE_TAG_DEFINITION_EDIT:
			case IDC_PE_TAG_LINE1_EDIT:
			case IDC_PE_TAG_LINE2_EDIT:
			case IDC_PE_TAG_LINE3_EDIT:
			case IDC_PE_TAG_LINE4_EDIT:
			case IDC_PE_TAG_DETAILED_EDIT:
			case IDC_PE_TAG_D_LINE1_EDIT:
			case IDC_PE_TAG_D_LINE2_EDIT:
			case IDC_PE_TAG_D_LINE3_EDIT:
			case IDC_PE_TAG_D_LINE4_EDIT:
				return true;
			default:
				return false;
			}
		}();
		if (isThemedEditControl)
		{
			pDC->SetBkColor(RGB(255, 255, 255));
			pDC->SetTextColor(RGB(17, 24, 39));
			return ::GetSysColorBrush(COLOR_WINDOW);
		}
	}
	if ((controlId == IDC_PE_SIDEBAR_PANEL || controlId == IDC_PE_SIDEBAR_TITLE) && SidebarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorSidebarBackgroundColor);
		pDC->SetTextColor(controlId == IDC_PE_SIDEBAR_TITLE ? RGB(92, 101, 116) : RGB(17, 24, 39));
		return static_cast<HBRUSH>(SidebarBrush.GetSafeHandle());
	}
	if ((controlId == IDC_PE_PROFILE_REPO_LINK || controlId == IDC_PE_PROFILE_COFFEE_LINK) && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		pDC->SetTextColor(RGB(47, 94, 182));
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}
	const bool useColorThemeBackground = [&]()
	{
		switch (controlId)
		{
		case IDC_PE_COLOR_LEFT_PANEL:
		case IDC_PE_SIDEBAR_PANEL:
		case IDC_PE_SIDEBAR_TITLE:
		case IDC_PE_PAGE_TITLE:
		case IDC_PE_PAGE_SUBTITLE:
		case IDC_PE_COLOR_RIGHT_PANEL:
		case IDC_PE_COLOR_TREE:
		case IDC_PE_COLOR_PATH_LABEL:
		case IDC_PE_SELECTED_PATH:
		case IDC_PE_COLOR_PICKER_LABEL:
		case IDC_PE_COLOR_PREVIEW_LABEL:
		case IDC_PE_COLOR_VALUE_LABEL:
		case IDC_PE_COLOR_OPACITY_LABEL:
		case IDC_PE_LABEL_RGBA:
		case IDC_PE_LABEL_HEX:
		case IDC_PE_ICON_PANEL:
		case IDC_PE_ICON_SHAPE_PANEL:
		case IDC_PE_ICON_SHAPE_HEADER:
		case IDC_PE_ICON_SIZE_HEADER:
		case IDC_PE_ICON_DISPLAY_PANEL:
		case IDC_PE_ICON_DISPLAY_HEADER:
		case IDC_PE_ICON_PREVIEW_PANEL:
		case IDC_PE_ICON_PREVIEW_HEADER:
		case IDC_PE_ICON_PREVIEW_SWATCH:
		case IDC_PE_ICON_PREVIEW_HINT:
		case IDC_PE_FIXED_SCALE_LABEL:
		case IDC_PE_FIXED_SCALE_VALUE:
		case IDC_PE_BOOST_FACTOR_LABEL:
		case IDC_PE_BOOST_FACTOR_VALUE:
		case IDC_PE_BOOST_RES_LABEL:
		case IDC_PE_FIXED_SCALE_TICK_MIN:
		case IDC_PE_FIXED_SCALE_TICK_MID:
		case IDC_PE_FIXED_SCALE_TICK_MAX:
		case IDC_PE_BOOST_FACTOR_TICK_MIN:
		case IDC_PE_BOOST_FACTOR_TICK_MID:
		case IDC_PE_BOOST_FACTOR_TICK_MAX:
		case IDC_PE_ICON_STYLE_ARROW:
		case IDC_PE_ICON_STYLE_DIAMOND:
		case IDC_PE_ICON_STYLE_REALISTIC:
		case IDC_PE_FIXED_PIXEL_CHECK:
		case IDC_PE_SMALL_BOOST_CHECK:
		case IDC_PE_RULE_LEFT_PANEL:
		case IDC_PE_RULE_RIGHT_PANEL:
		case IDC_PE_RULE_LEFT_HEADER:
		case IDC_PE_RULE_RIGHT_HEADER:
		case IDC_PE_RULE_SOURCE_LABEL:
		case IDC_PE_RULE_TOKEN_LABEL:
		case IDC_PE_RULE_CONDITION_LABEL:
		case IDC_PE_RULE_TYPE_LABEL:
		case IDC_PE_RULE_STATUS_LABEL:
		case IDC_PE_RULE_DETAIL_LABEL:
		case IDC_PE_RULE_TARGET_CHECK:
		case IDC_PE_RULE_TAG_CHECK:
		case IDC_PE_RULE_TEXT_CHECK:
		case IDC_PE_RULE_COLOR_WHEEL_LABEL:
		case IDC_PE_RULE_COLOR_VALUE_LABEL:
		case IDC_PE_RULE_COLOR_PREVIEW_LABEL:
		case IDC_PE_PROFILE_PANEL:
		case IDC_PE_PROFILE_HEADER:
		case IDC_PE_PROFILE_NAME_LABEL:
		case IDC_PE_PROFILE_INFO_PANEL:
		case IDC_PE_PROFILE_INFO_HEADER:
		case IDC_PE_PROFILE_INFO_BODY:
		case IDC_PE_TAG_PANEL:
		case IDC_PE_TAG_HEADER_PANEL:
		case IDC_PE_TAG_TYPE_LABEL:
		case IDC_PE_TAG_STATUS_LABEL:
		case IDC_PE_TAG_TOKEN_LABEL:
		case IDC_PE_TAG_DEF_HEADER:
		case IDC_PE_TAG_LINE1_LABEL:
		case IDC_PE_TAG_LINE2_LABEL:
		case IDC_PE_TAG_LINE3_LABEL:
		case IDC_PE_TAG_LINE4_LABEL:
		case IDC_PE_TAG_AUTO_DECONFLICTION:
		case IDC_PE_TAG_LINK_DETAILED:
		case IDC_PE_TAG_DETAILED_HEADER:
		case IDC_PE_TAG_D_LINE1_LABEL:
		case IDC_PE_TAG_D_LINE2_LABEL:
		case IDC_PE_TAG_D_LINE3_LABEL:
		case IDC_PE_TAG_D_LINE4_LABEL:
		case IDC_PE_TAG_PREVIEW_LABEL:
			return true;
		default:
			return false;
		}
	}();
	if (useColorThemeBackground && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		if (controlId == IDC_PE_SIDEBAR_TITLE || controlId == IDC_PE_PAGE_SUBTITLE || controlId == IDC_PE_ICON_PREVIEW_HINT)
			pDC->SetTextColor(RGB(107, 114, 128));
		else
			pDC->SetTextColor(RGB(17, 24, 39));
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}
	return hbr;
}

void CProfileEditorDialog::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (lpDrawItemStruct == nullptr)
	{
		CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
		return;
	}

	const UINT controlId = lpDrawItemStruct->CtlID;
	// Icon card panels are kept as standard static controls (not owner-drawn)
	// to avoid child overpaint/flicker issues on tab switches.
	if (controlId == IDC_PE_RULE_LIST || controlId == IDC_PE_PROFILE_LIST)
	{
		CListBox* listBox = (controlId == IDC_PE_PROFILE_LIST) ? &ProfileList : &RulesList;
		CDC dc;
		dc.Attach(lpDrawItemStruct->hDC);
		CRect outerRect(lpDrawItemStruct->rcItem);
		CRect localOuter(0, 0, outerRect.Width(), outerRect.Height());

		CDC memDc;
		memDc.CreateCompatibleDC(&dc);
		CBitmap frameBitmap;
		frameBitmap.CreateCompatibleBitmap(&dc, max(1, localOuter.Width()), max(1, localOuter.Height()));
		CBitmap* oldFrameBitmap = memDc.SelectObject(&frameBitmap);
		memDc.FillSolidRect(&localOuter, kEditorThemeBackgroundColor);

		if (lpDrawItemStruct->itemID != static_cast<UINT>(-1))
		{
			const bool isSelected = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
			CRect itemRect(localOuter);
			itemRect.DeflateRect(4, 2);

			if (isSelected)
			{
				CBrush selectedBrush(RGB(226, 238, 255));
				CPen selectedPen(PS_SOLID, 1, RGB(64, 132, 230));
				CPen* oldPen = memDc.SelectObject(&selectedPen);
				CBrush* oldBrush = memDc.SelectObject(&selectedBrush);
				memDc.RoundRect(&itemRect, CPoint(8, 8));
				memDc.SelectObject(oldBrush);
				memDc.SelectObject(oldPen);
			}

			CString itemText;
			listBox->GetText(static_cast<int>(lpDrawItemStruct->itemID), itemText);

			CFont* oldFont = nullptr;
			if (listBox->GetFont() != nullptr)
				oldFont = memDc.SelectObject(listBox->GetFont());

			memDc.SetBkMode(TRANSPARENT);
			memDc.SetTextColor(isSelected ? RGB(16, 66, 132) : RGB(17, 24, 39));
			CRect textRect(itemRect);
			textRect.DeflateRect(8, 0);
			memDc.DrawText(itemText, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0 && controlId != IDC_PE_PROFILE_LIST)
			{
				CRect focusRect(itemRect);
				focusRect.DeflateRect(3, 2);
				memDc.DrawFocusRect(&focusRect);
			}

			if (oldFont != nullptr)
				memDc.SelectObject(oldFont);
		}

		dc.BitBlt(outerRect.left, outerRect.top, localOuter.Width(), localOuter.Height(), &memDc, 0, 0, SRCCOPY);
		memDc.SelectObject(oldFrameBitmap);
		dc.Detach();
		return;
	}

	const bool isModernPushButton =
		(controlId == IDC_PE_APPLY_BUTTON) ||
		(controlId == IDC_PE_RESET_BUTTON) ||
		(controlId == IDC_PE_RULE_ADD_BUTTON) ||
		(controlId == IDC_PE_RULE_ADD_PARAM_BUTTON) ||
		(controlId == IDC_PE_RULE_REMOVE_BUTTON) ||
		(controlId == IDC_PE_RULE_COLOR_APPLY_BUTTON) ||
		(controlId == IDC_PE_RULE_COLOR_RESET_BUTTON) ||
		(controlId == IDC_PE_PROFILE_ADD_BUTTON) ||
		(controlId == IDC_PE_PROFILE_DUPLICATE_BUTTON) ||
		(controlId == IDC_PE_PROFILE_RENAME_BUTTON) ||
		(controlId == IDC_PE_PROFILE_DELETE_BUTTON) ||
		(controlId == IDC_PE_TAG_TOKEN_ADD_BUTTON) ||
		(controlId == IDC_PE_NAV_COLORS) ||
		(controlId == IDC_PE_NAV_ICON) ||
		(controlId == IDC_PE_NAV_RULES) ||
		(controlId == IDC_PE_NAV_PROFILE);
	if (isModernPushButton)
	{
		CDC dc;
		dc.Attach(lpDrawItemStruct->hDC);
		CRect outerRect(lpDrawItemStruct->rcItem);
		CRect localOuter(0, 0, outerRect.Width(), outerRect.Height());

		CDC memDc;
		memDc.CreateCompatibleDC(&dc);
		CBitmap frameBitmap;
		frameBitmap.CreateCompatibleBitmap(&dc, max(1, localOuter.Width()), max(1, localOuter.Height()));
		CBitmap* oldFrameBitmap = memDc.SelectObject(&frameBitmap);
		const bool isSidebarNavButton =
			(controlId == IDC_PE_NAV_COLORS) ||
			(controlId == IDC_PE_NAV_ICON) ||
			(controlId == IDC_PE_NAV_RULES) ||
			(controlId == IDC_PE_NAV_PROFILE);
		memDc.FillSolidRect(&localOuter, isSidebarNavButton ? kEditorSidebarBackgroundColor : kEditorThemeBackgroundColor);

		const bool isDisabled = (lpDrawItemStruct->itemState & ODS_DISABLED) != 0;
		const bool isPressed = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
		const bool isHot = (lpDrawItemStruct->itemState & ODS_HOTLIGHT) != 0;

		COLORREF fillColor = RGB(247, 248, 250);
		COLORREF borderColor = RGB(171, 180, 190);
		COLORREF textColor = RGB(17, 24, 39);
		if (isSidebarNavButton)
		{
			const int selectedTab = PageTabs.GetCurSel();
			bool isActive = false;
			if (controlId == IDC_PE_NAV_COLORS) isActive = (selectedTab == kTabColors);
			if (controlId == IDC_PE_NAV_ICON) isActive = (selectedTab == kTabIcons);
			if (controlId == IDC_PE_NAV_RULES) isActive = (selectedTab == kTabRules);
			if (controlId == IDC_PE_NAV_PROFILE) isActive = (selectedTab == kTabProfile);
			if (isActive)
			{
				fillColor = RGB(232, 240, 255);
				borderColor = RGB(181, 207, 252);
				textColor = RGB(29, 78, 216);
			}
			else
			{
				fillColor = kEditorSidebarBackgroundColor;
				borderColor = kEditorSidebarBackgroundColor;
				textColor = RGB(31, 41, 55);
			}
		}
		const bool isPrimaryButton =
			(controlId == IDC_PE_APPLY_BUTTON) ||
			(controlId == IDC_PE_RULE_COLOR_APPLY_BUTTON);
		if (isPrimaryButton && !isDisabled)
		{
			fillColor = isPressed ? RGB(47, 118, 234) : RGB(59, 130, 246);
			borderColor = RGB(37, 99, 235);
			textColor = RGB(255, 255, 255);
		}
		if (isDisabled)
		{
			fillColor = RGB(232, 232, 232);
			borderColor = RGB(194, 194, 194);
			textColor = RGB(138, 138, 138);
		}
		else if (isPressed)
		{
			fillColor = RGB(221, 236, 255);
			borderColor = RGB(63, 120, 208);
		}
		else if (isHot)
		{
			fillColor = RGB(236, 245, 255);
			borderColor = RGB(102, 153, 224);
		}

		CRect buttonRect(localOuter);
		buttonRect.DeflateRect(1, 1);
		CBrush fillBrush(fillColor);
		CPen borderPen(PS_SOLID, 1, borderColor);
		CBrush* oldBrush = memDc.SelectObject(&fillBrush);
		CPen* oldPen = memDc.SelectObject(&borderPen);
		memDc.RoundRect(&buttonRect, CPoint(10, 10));
		memDc.SelectObject(oldPen);
		memDc.SelectObject(oldBrush);

		CString buttonText;
		if (CWnd* button = GetDlgItem(controlId))
			button->GetWindowText(buttonText);
		CFont* oldFont = nullptr;
		if (CWnd* button = GetDlgItem(controlId))
		{
			CFont* font = button->GetFont();
			if (font != nullptr)
				oldFont = memDc.SelectObject(font);
		}
		memDc.SetBkMode(TRANSPARENT);
		memDc.SetTextColor(textColor);
		CRect textRect(buttonRect);
		memDc.DrawText(buttonText, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (oldFont != nullptr)
			memDc.SelectObject(oldFont);

		if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0 && !isSidebarNavButton)
		{
			CRect focusRect(buttonRect);
			focusRect.DeflateRect(4, 3);
			memDc.DrawFocusRect(&focusRect);
		}

		dc.BitBlt(outerRect.left, outerRect.top, localOuter.Width(), localOuter.Height(), &memDc, 0, 0, SRCCOPY);
		memDc.SelectObject(oldFrameBitmap);
		dc.Detach();
		return;
	}

	if (controlId == IDC_PE_COLOR_WHEEL || controlId == IDC_PE_RULE_COLOR_WHEEL)
	{
		CDC dc;
		dc.Attach(lpDrawItemStruct->hDC);
		CRect outerRect(lpDrawItemStruct->rcItem);
		CRect localOuter(0, 0, outerRect.Width(), outerRect.Height());

		CDC memDc;
		memDc.CreateCompatibleDC(&dc);
		CBitmap frameBitmap;
		frameBitmap.CreateCompatibleBitmap(&dc, max(1, localOuter.Width()), max(1, localOuter.Height()));
		CBitmap* oldFrameBitmap = memDc.SelectObject(&frameBitmap);

		memDc.FillSolidRect(&localOuter, kEditorThemeBackgroundColor);

		CRect wheelRect = localOuter;
		wheelRect.DeflateRect(2, 2);
		EnsureColorWheelBitmap(wheelRect);
		if (ColorWheelBitmap.GetSafeHandle() != nullptr)
		{
			CDC wheelDc;
			wheelDc.CreateCompatibleDC(&memDc);
			CBitmap* oldWheelBitmap = wheelDc.SelectObject(&ColorWheelBitmap);
			memDc.BitBlt(wheelRect.left, wheelRect.top, wheelRect.Width(), wheelRect.Height(), &wheelDc, 0, 0, SRCCOPY);
			wheelDc.SelectObject(oldWheelBitmap);
		}

		const int diameter = min(wheelRect.Width(), wheelRect.Height());
		const int radius = max(1, (diameter / 2) - 2);
		const int centerX = wheelRect.left + (wheelRect.Width() / 2);
		const int centerY = wheelRect.top + (wheelRect.Height() / 2);

		CPen borderPen(PS_SOLID, 1, RGB(186, 186, 186));
		CPen* oldPen = memDc.SelectObject(&borderPen);
		CBrush* oldBrush = static_cast<CBrush*>(memDc.SelectStockObject(HOLLOW_BRUSH));
		memDc.Ellipse(centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
		memDc.SelectObject(oldBrush);

		const bool isRuleWheel = (controlId == IDC_PE_RULE_COLOR_WHEEL);
		const bool hasColor = isRuleWheel ? RuleColorDraftValid : DraftColorValid;
		const int drawR = isRuleWheel ? RuleColorDraftR : DraftColorR;
		const int drawG = isRuleWheel ? RuleColorDraftG : DraftColorG;
		const int drawB = isRuleWheel ? RuleColorDraftB : DraftColorB;
		if (hasColor)
		{
			double hue = 0.0;
			double saturation = 0.0;
			if (isRuleWheel && RuleColorDraftHueSaturationValid)
			{
				hue = RuleColorDraftHue;
				saturation = RuleColorDraftSaturation;
			}
			else if (!isRuleWheel && DraftColorHueSaturationValid)
			{
				hue = DraftColorHue;
				saturation = DraftColorSaturation;
			}
			else
			{
				double value = 1.0;
				RgbToHsv(drawR, drawG, drawB, hue, saturation, value);
			}
			const double angleRad = hue * (3.14159265358979323846 / 180.0);
			const int markerX = centerX + static_cast<int>(round(cos(angleRad) * saturation * radius));
			const int markerY = centerY - static_cast<int>(round(sin(angleRad) * saturation * radius));
			CPen markerOuter(PS_SOLID, 2, RGB(255, 255, 255));
			CPen markerInner(PS_SOLID, 1, RGB(17, 24, 39));
			memDc.SelectObject(&markerOuter);
			memDc.Ellipse(markerX - 4, markerY - 4, markerX + 5, markerY + 5);
			memDc.SelectObject(&markerInner);
			memDc.Ellipse(markerX - 3, markerY - 3, markerX + 4, markerY + 4);
		}

		memDc.SelectObject(oldPen);
		dc.BitBlt(outerRect.left, outerRect.top, localOuter.Width(), localOuter.Height(), &memDc, 0, 0, SRCCOPY);
		memDc.SelectObject(oldFrameBitmap);
		dc.Detach();
		return;
	}

	const bool isColorTabSwatch = (controlId == IDC_PE_COLOR_PREVIEW_SWATCH);
	const bool isRuleSwatch =
		(controlId == IDC_PE_RULE_TARGET_SWATCH) ||
		(controlId == IDC_PE_RULE_TAG_SWATCH) ||
		(controlId == IDC_PE_RULE_TEXT_SWATCH);
	const bool isRulePreviewSwatch = (controlId == IDC_PE_RULE_COLOR_PREVIEW_SWATCH);
	if (!isColorTabSwatch && !isRuleSwatch && !isRulePreviewSwatch)
	{
		CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
		return;
	}

	CDC dc;
	dc.Attach(lpDrawItemStruct->hDC);
	CRect outerRect(lpDrawItemStruct->rcItem);
	CRect localOuter(0, 0, outerRect.Width(), outerRect.Height());

	CDC memDc;
	memDc.CreateCompatibleDC(&dc);
	CBitmap frameBitmap;
	frameBitmap.CreateCompatibleBitmap(&dc, max(1, localOuter.Width()), max(1, localOuter.Height()));
	CBitmap* oldFrameBitmap = memDc.SelectObject(&frameBitmap);

	memDc.FillSolidRect(&localOuter, kEditorThemeBackgroundColor);

	CRect roundedRect = localOuter;
	roundedRect.DeflateRect(1, 1);
	COLORREF fillColor = DraftColorValid ? RGB(DraftColorR, DraftColorG, DraftColorB) : RGB(255, 255, 255);
	int fillAlpha = (DraftColorValid ? DraftColorA : 255);
	bool swatchEnabled = true;

	auto tryReadRuleSwatchAlpha = [&](UINT swatchId, int& outAlpha) -> bool
	{
		outAlpha = 255;
		CButton* check = nullptr;
		CEdit* edit = nullptr;
		if (!GetRuleColorEditorTargetControls(swatchId, check, edit) || edit == nullptr)
			return false;

		CString rawText;
		edit->GetWindowText(rawText);
		const std::string text = std::string(rawText.GetString());
		int r = 0, g = 0, b = 0, a = 255;
		bool hasAlpha = false;
		if (TryParseRgbaQuad(text, r, g, b, a, hasAlpha))
		{
			outAlpha = hasAlpha ? a : 255;
			return true;
		}
		if (TryParseRgbTriplet(text, r, g, b))
		{
			outAlpha = 255;
			return true;
		}
		return false;
	};

	if (isRulePreviewSwatch)
	{
		if (RuleColorDraftValid)
		{
			fillColor = RGB(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB);
			fillAlpha = RuleColorDraftA;
			swatchEnabled = true;
		}
		else
		{
			COLORREF activeColor = RGB(240, 240, 240);
			bool activeEnabled = false;
			if (ResolveRuleSwatchColor(RuleColorActiveSwatchId, activeColor, activeEnabled))
			{
				fillColor = activeColor;
				if (!tryReadRuleSwatchAlpha(RuleColorActiveSwatchId, fillAlpha))
					fillAlpha = 255;
				swatchEnabled = true;
			}
			else
			{
				fillColor = RGB(240, 240, 240);
				fillAlpha = 255;
				swatchEnabled = false;
			}
		}
	}
	else if (isRuleSwatch)
	{
		COLORREF ruleColor = RGB(240, 240, 240);
		if (ResolveRuleSwatchColor(controlId, ruleColor, swatchEnabled))
		{
			fillColor = ruleColor;
			if (!tryReadRuleSwatchAlpha(controlId, fillAlpha))
				fillAlpha = 255;
		}
		else
		{
			fillAlpha = 255;
			swatchEnabled = false;
		}
	}

	const COLORREF previewColor = BlendColorOverBackground(fillColor, fillAlpha, kEditorThemeBackgroundColor);
	CBrush fillBrush(previewColor);
	CPen borderPen(PS_SOLID, 1, swatchEnabled ? RGB(186, 186, 186) : RGB(200, 200, 200));
	CBrush* oldBrush = memDc.SelectObject(&fillBrush);
	CPen* oldPen = memDc.SelectObject(&borderPen);
	memDc.RoundRect(&roundedRect, CPoint(10, 10));
	memDc.SelectObject(oldPen);
	memDc.SelectObject(oldBrush);

	if (isRuleSwatch && controlId == RuleColorActiveSwatchId)
	{
		// Use a double-ring indicator to make the selected swatch unambiguous.
		CRect activeOuterRect(localOuter);
		activeOuterRect.DeflateRect(1, 1);
		CPen activeOuterPen(PS_SOLID, 3, RGB(44, 116, 220));
		CPen* oldActiveOuterPen = memDc.SelectObject(&activeOuterPen);
		CBrush* oldActiveOuterBrush = static_cast<CBrush*>(memDc.SelectStockObject(HOLLOW_BRUSH));
		memDc.RoundRect(&activeOuterRect, CPoint(10, 10));
		memDc.SelectObject(oldActiveOuterBrush);
		memDc.SelectObject(oldActiveOuterPen);

		CRect activeInnerRect(activeOuterRect);
		activeInnerRect.DeflateRect(4, 4);
		CPen activeInnerPen(PS_SOLID, 1, RGB(255, 255, 255));
		CPen* oldActiveInnerPen = memDc.SelectObject(&activeInnerPen);
		CBrush* oldActiveInnerBrush = static_cast<CBrush*>(memDc.SelectStockObject(HOLLOW_BRUSH));
		memDc.RoundRect(&activeInnerRect, CPoint(8, 8));
		memDc.SelectObject(oldActiveInnerBrush);
		memDc.SelectObject(oldActiveInnerPen);
	}
	else if (isRulePreviewSwatch)
	{
		CRect previewRect(localOuter);
		previewRect.DeflateRect(1, 1);
		CPen previewPen(PS_SOLID, 1, RGB(64, 132, 230));
		CPen* oldPreviewPen = memDc.SelectObject(&previewPen);
		CBrush* oldPreviewBrush = static_cast<CBrush*>(memDc.SelectStockObject(HOLLOW_BRUSH));
		memDc.RoundRect(&previewRect, CPoint(10, 10));
		memDc.SelectObject(oldPreviewBrush);
		memDc.SelectObject(oldPreviewPen);
	}

	if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0 && isRuleSwatch)
	{
		CRect focusRect(localOuter);
		focusRect.DeflateRect(3, 3);
		memDc.DrawFocusRect(&focusRect);
	}

	dc.BitBlt(outerRect.left, outerRect.top, localOuter.Width(), localOuter.Height(), &memDc, 0, 0, SRCCOPY);
	memDc.SelectObject(oldFrameBitmap);
	dc.Detach();
}

void CProfileEditorDialog::OnPaint()
{
	CPaintDC dc(this);
	CRect clientRect;
	GetClientRect(&clientRect);
	dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

	const auto drawCardRect = [&](const CRect& sourceRect, bool filledPreview = false)
	{
		CRect rect(sourceRect);
		if (rect.Width() <= 2 || rect.Height() <= 2)
			return;

		CRect card(rect);
		card.DeflateRect(1, 1);
		CBrush fillBrush(filledPreview ? RGB(232, 235, 240) : kEditorThemeBackgroundColor);
		CPen borderPen(PS_SOLID, 1, RGB(198, 204, 214));
		CBrush* oldBrush = dc.SelectObject(&fillBrush);
		CPen* oldPen = dc.SelectObject(&borderPen);
		dc.RoundRect(&card, CPoint(12, 12));
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);
	};

	const auto drawCard = [&](CWnd& anchor, bool filledPreview = false)
	{
		if (!::IsWindow(anchor.GetSafeHwnd()))
			return;
		CRect rect;
		anchor.GetWindowRect(&rect);
		ScreenToClient(&rect);
		drawCardRect(rect, filledPreview);
	};

	const auto drawEditShell = [&](CWnd& edit)
	{
		if (!::IsWindow(edit.GetSafeHwnd()) || !edit.IsWindowVisible())
			return;

		CRect rect;
		edit.GetWindowRect(&rect);
		ScreenToClient(&rect);
		rect.InflateRect(2, 2);
		if (rect.Width() <= 2 || rect.Height() <= 2)
			return;

		CBrush fillBrush(RGB(255, 255, 255));
		CPen borderPen(PS_SOLID, 1, RGB(186, 192, 200));
		CBrush* oldBrush = dc.SelectObject(&fillBrush);
		CPen* oldPen = dc.SelectObject(&borderPen);
		dc.RoundRect(&rect, CPoint(10, 10));
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);
	};

	const auto maskHeaderTextBackground = [&](CWnd& header, COLORREF fillColor = kEditorThemeBackgroundColor)
	{
		if (!::IsWindow(header.GetSafeHwnd()))
			return;
		CRect rect;
		header.GetWindowRect(&rect);
		ScreenToClient(&rect);
		rect.InflateRect(8, 0);
		dc.FillSolidRect(&rect, fillColor);
	};

	const auto drawSectionDivider = [&](CWnd& header, CWnd& panel)
	{
		if (!::IsWindow(header.GetSafeHwnd()) || !header.IsWindowVisible() || !::IsWindow(panel.GetSafeHwnd()))
			return;

		CRect headerRect;
		header.GetWindowRect(&headerRect);
		ScreenToClient(&headerRect);

		CRect panelRect;
		panel.GetWindowRect(&panelRect);
		ScreenToClient(&panelRect);

		const int lineY = min(panelRect.bottom - 12, headerRect.bottom + 3);
		const int lineLeft = panelRect.left + 12;
		const int lineRight = panelRect.right - 12;
		if (lineRight <= lineLeft)
			return;

		CPen dividerPen(PS_SOLID, 1, RGB(210, 215, 223));
		CPen* oldPen = dc.SelectObject(&dividerPen);
		dc.MoveTo(lineLeft, lineY);
		dc.LineTo(lineRight, lineY);
		dc.SelectObject(oldPen);
	};

	CRect sidebarRect;
	if (::IsWindow(NavColorsButton.GetSafeHwnd()) && ::IsWindow(SidebarTitle.GetSafeHwnd()))
	{
		CRect titleRect;
		SidebarTitle.GetWindowRect(&titleRect);
		ScreenToClient(&titleRect);
		CRect navRect;
		NavColorsButton.GetWindowRect(&navRect);
		ScreenToClient(&navRect);
		sidebarRect = titleRect;
		sidebarRect.left = clientRect.left;
		sidebarRect.right = navRect.right + 16;
		sidebarRect.top = clientRect.top;
		sidebarRect.bottom = clientRect.bottom;
		dc.FillSolidRect(&sidebarRect, kEditorSidebarBackgroundColor);
		CPen dividerPen(PS_SOLID, 1, RGB(198, 204, 214));
		CPen* oldPen = dc.SelectObject(&dividerPen);
		dc.MoveTo(sidebarRect.right - 1, sidebarRect.top);
		dc.LineTo(sidebarRect.right - 1, sidebarRect.bottom);
		dc.SelectObject(oldPen);
	}

	const int selectedTab = PageTabs.GetCurSel();
	if (selectedTab == kTabIcons)
	{
		drawCard(IconShapePanel, false);
		drawCard(IconPanel, false);
		drawCard(IconDisplayPanel, false);
		drawCard(TagPanel, false);
		drawSectionDivider(IconShapeHeader, IconShapePanel);
		drawSectionDivider(IconSizeHeader, IconPanel);
		drawSectionDivider(IconDisplayHeader, IconDisplayPanel);
		drawSectionDivider(TagHeaderPanel, TagPanel);
	}
	else if (selectedTab == kTabColors)
	{
		drawCard(ColorLeftPanel, false);
		drawCard(ColorRightPanel, false);
		drawSectionDivider(ColorPathLabel, ColorLeftPanel);
		drawSectionDivider(SelectedPathText, ColorRightPanel);
		drawEditShell(EditRgba);
		drawEditShell(EditHex);
	}
	else if (selectedTab == kTabRules)
	{
		drawCard(RuleLeftPanel, false);
		drawCard(RuleRightPanel, false);
		drawSectionDivider(RuleLeftHeader, RuleLeftPanel);
		drawSectionDivider(RuleRightHeader, RuleRightPanel);
		drawEditShell(RuleNameEdit);
		drawEditShell(RuleTargetEdit);
		drawEditShell(RuleTagEdit);
		drawEditShell(RuleTextEdit);
	}
	else if (selectedTab == kTabProfile)
	{
		drawCard(ProfilePanel, false);
		drawCard(ProfileInfoPanel, false);
		drawSectionDivider(ProfileHeader, ProfilePanel);
		drawSectionDivider(ProfileInfoHeader, ProfileInfoPanel);
		maskHeaderTextBackground(ProfileHeader);
		maskHeaderTextBackground(ProfileInfoHeader);
		drawEditShell(ProfileNameEdit);
	}

	if (selectedTab == kTabIcons)
	{
		drawEditShell(TagDefinitionEdit);
		drawEditShell(TagDetailedDefinitionEdit);
	}
}

void CProfileEditorDialog::CreateEditorControls()
{
	if (ControlsCreated)
		return;

	const DWORD commonEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
	const DWORD commonMultilineEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;

	PageTabs.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, CRect(0, 0, 0, 0), this, IDC_PE_TAB);
	PageTabs.InsertItem(0, "Colors");
	PageTabs.InsertItem(1, "Icons && Tags");
	PageTabs.InsertItem(2, "Rules");
	PageTabs.InsertItem(3, "Profiles");
	SidebarPanel.Create("", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_SIDEBAR_PANEL);
	SidebarTitle.Create("SECTIONS", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_SIDEBAR_TITLE);
	SidebarDivider.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT, CRect(0, 0, 0, 0), this, IDC_PE_SIDEBAR_DIVIDER);
	NavColorsButton.Create("Colors", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_NAV_COLORS);
	NavIconButton.Create("Icons && Tags", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_NAV_ICON);
	NavRulesButton.Create("Rules", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_NAV_RULES);
	NavProfileButton.Create("Profiles", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_NAV_PROFILE);
	PageTitleLabel.Create("Colors", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_PAGE_TITLE);
	PageSubtitleLabel.Create("Select a color entry on the left and edit it on the right.", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_PAGE_SUBTITLE);

	ColorLeftPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_LEFT_PANEL);
	ColorRightPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_RIGHT_PANEL);
	ColorPathTree.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_TREE);
	ColorPathLabel.Create("Color List", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_PATH_LABEL);
	SelectedPathText.Create("Preview", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_SELECTED_PATH);
	ColorPickerLabel.Create("Color Wheel", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_PICKER_LABEL);
	ColorPreviewLabel.Create("Live Preview", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_PREVIEW_LABEL);
	ColorPreviewSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_PREVIEW_SWATCH);
	ColorWheel.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_WHEEL);
	ColorValueLabel.Create("Value", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_VALUE_LABEL);
	ColorValueSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_VALUE_SLIDER);
	ColorOpacityLabel.Create("Opacity", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_OPACITY_LABEL);
	ColorOpacitySlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_OPACITY_SLIDER);
	LabelRgba.Create("RGBA", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_RGBA);
	EditRgba.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_RGBA);
	LabelHex.Create("HEX", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_HEX);
	EditHex.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_HEX);
	ApplyColorButton.Create("Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_APPLY_BUTTON);
	ResetColorButton.Create("Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RESET_BUTTON);

	IconStyleArrow.Create("Arrow", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_ARROW);
	IconStyleDiamond.Create("Diamond", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_DIAMOND);
	IconStyleRealistic.Create("Realistic", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_REALISTIC);
	IconPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PANEL);
	IconShapePanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SHAPE_PANEL);
	IconShapeHeader.Create("Shape", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SHAPE_HEADER);
	IconSizeHeader.Create("Size", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SIZE_HEADER);
	IconDisplayPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_DISPLAY_PANEL);
	IconDisplayHeader.Create("Display", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_ICON_DISPLAY_HEADER);
	IconPreviewPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PREVIEW_PANEL);
	IconPreviewHeader.Create("Preview", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PREVIEW_HEADER);
	IconPreviewSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PREVIEW_SWATCH);
	IconPreviewHint.Create("Simple split layout: controls on the left, preview on the right.", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PREVIEW_HINT);
	IconShapePanel.ModifyStyleEx(0, WS_EX_TRANSPARENT);
	IconPanel.ModifyStyleEx(0, WS_EX_TRANSPARENT);
	IconDisplayPanel.ModifyStyleEx(0, WS_EX_TRANSPARENT);
	IconPreviewPanel.ModifyStyleEx(0, WS_EX_TRANSPARENT);
	IconSeparator1.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR1);
	IconSeparator2.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR2);
	IconSeparator3.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR3);
	FixedPixelCheck.Create("Fixed Pixel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_PIXEL_CHECK);
	FixedScaleLabel.Create("Fixed Size", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_LABEL);
	FixedScaleValueLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_VALUE);
	FixedScaleSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_SLIDER);
	FixedScaleTickMinLabel.Create("0.10x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MIN);
	FixedScaleTickMidLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MID);
	FixedScaleTickMaxLabel.Create("2.00x", WS_CHILD | WS_VISIBLE | SS_RIGHT, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MAX);
	FixedScaleCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_COMBO);
	SmallBoostCheck.Create("Small Icon Boost", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_SMALL_BOOST_CHECK);
	BoostFactorLabel.Create("Boost Factor", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_LABEL);
	BoostFactorValueLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_VALUE);
	BoostFactorSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_SLIDER);
	BoostFactorTickMinLabel.Create("0.10x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MIN);
	BoostFactorTickMidLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MID);
	BoostFactorTickMaxLabel.Create("2.00x", WS_CHILD | WS_VISIBLE | SS_RIGHT, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MAX);
	BoostFactorCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_COMBO);
	BoostResolutionLabel.Create("Resolution", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_LABEL);
	BoostResolutionCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_COMBO);
	BoostResolution1080Button.Create("1080p", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_RES_1080);
	BoostResolution2KButton.Create("2K", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_RES_2K);
	BoostResolution4KButton.Create("4K", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_RES_4K);
	IconPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	IconShapePanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	IconDisplayPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	IconPreviewPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	RuleLeftPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LEFT_PANEL);
	RuleRightPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_RULE_RIGHT_PANEL);
	RuleLeftHeader.Create("Rule List", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LEFT_HEADER);
	RuleRightHeader.Create("Selected Rule", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_RIGHT_HEADER);
	RulesList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LIST);
	RuleTree.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TREE);
	RuleAddButton.Create("Add Rule", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_ADD_BUTTON);
	RuleAddParameterButton.Create("+ Param", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_ADD_PARAM_BUTTON);
	RuleRemoveButton.Create("Remove", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_REMOVE_BUTTON);
	RuleNameLabel.Create("Rule Name", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_NAME_LABEL);
	RuleNameEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_NAME_EDIT);
	RuleSourceLabel.Create("Source", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_SOURCE_LABEL);
	RuleSourceCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_SOURCE_COMBO);
	RuleTokenLabel.Create("Token", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TOKEN_LABEL);
	RuleTokenCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TOKEN_COMBO);
	RuleConditionLabel.Create("Condition", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_CONDITION_LABEL);
	RuleConditionCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN, CRect(0, 0, 0, 0), this, IDC_PE_RULE_CONDITION_COMBO);
	RuleTypeLabel.Create("Tag Type", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TYPE_LABEL);
	RuleTypeCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TYPE_COMBO);
	RuleStatusLabel.Create("Status", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_STATUS_LABEL);
	RuleStatusCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_STATUS_COMBO);
	RuleDetailLabel.Create("Detail", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_DETAIL_LABEL);
	RuleDetailCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_DETAIL_COMBO);
	RuleTargetCheck.Create("Icons", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TARGET_CHECK);
	RuleTargetSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TARGET_SWATCH);
	RuleTargetEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TARGET_EDIT);
	RuleTagCheck.Create("Tag", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TAG_CHECK);
	RuleTagSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TAG_SWATCH);
	RuleTagEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TAG_EDIT);
	RuleTextCheck.Create("Text", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TEXT_CHECK);
	RuleTextSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TEXT_SWATCH);
	RuleTextEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TEXT_EDIT);
	RuleColorWheelLabel.Create("Color Wheel", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_WHEEL_LABEL);
	RuleColorWheel.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_WHEEL);
	RuleColorValueLabel.Create("Value", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_VALUE_LABEL);
	RuleColorValueSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_VALUE_SLIDER);
	RuleColorOpacityLabel.Create("Opacity", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_OPACITY_LABEL);
	RuleColorOpacitySlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_OPACITY_SLIDER);
	RuleColorPreviewLabel.Create("Live Preview", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_PREVIEW_LABEL);
	RuleColorPreviewSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_PREVIEW_SWATCH);
	RuleColorApplyButton.Create("Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_APPLY_BUTTON);
	RuleColorResetButton.Create("Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_RESET_BUTTON);
	ProfilePanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_PANEL);
	ProfileHeader.Create("Profiles", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_HEADER);
	ProfileList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_LIST);
	ProfileNameLabel.Create("Name", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_NAME_LABEL);
	ProfileNameEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_NAME_EDIT);
	ProfileProModeCheck.Create("Pro mode", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_PRO_MODE_CHECK);
	ProfileAddButton.Create("Add", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_ADD_BUTTON);
	ProfileDuplicateButton.Create("Duplicate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_DUPLICATE_BUTTON);
	ProfileRenameButton.Create("Rename", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_RENAME_BUTTON);
	ProfileDeleteButton.Create("Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_DELETE_BUTTON);
	ProfileInfoPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_INFO_PANEL);
	ProfileInfoHeader.Create("About", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_PROFILE_INFO_HEADER);
	ProfileInfoBody.Create(
		"Feedback, bug reports, or suggestions are always welcome! If you encounter an issue or have ideas to improve the plugin, feel free to open an issue or send feedback on the GitHub repository.\r\n\r\nThis plugin is completely free and always will be. Nothing is required from you to use it.\r\n\r\nIf you enjoy the project and would like to support its development, you can buy me a coffee using the link below.",
		WS_CHILD | WS_VISIBLE,
		CRect(0, 0, 0, 0),
		this,
		IDC_PE_PROFILE_INFO_BODY);
	ProfileRepoLink.Create(
		"https://github.com/IWantPizzaa/vSMR",
		WS_CHILD | WS_VISIBLE | SS_NOTIFY,
		CRect(0, 0, 0, 0),
		this,
		IDC_PE_PROFILE_REPO_LINK);
	ProfileCoffeeLink.Create(
		"https://buymeacoffee.com/i_want_pizzaa",
		WS_CHILD | WS_VISIBLE | SS_NOTIFY,
		CRect(0, 0, 0, 0),
		this,
		IDC_PE_PROFILE_COFFEE_LINK);
	RulesList.SetItemHeight(0, 28);
	ProfileList.SetItemHeight(0, 26);
	RuleTree.SetIndent(16);
	RuleTree.SendMessage(TVM_SETITEMHEIGHT, 32, 0);
	RuleTree.SetBkColor(kEditorThemeBackgroundColor);
	RuleTree.SetTextColor(RGB(17, 24, 39));

	TagPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_TAG_PANEL);
	TagHeaderPanel.Create("Options", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_HEADER_PANEL);
	TagTypeLabel.Create("Type", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TYPE_LABEL);
	TagTypeCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TYPE_COMBO);
	TagStatusLabel.Create("Status", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_STATUS_LABEL);
	TagStatusCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_STATUS_COMBO);
	TagTokenLabel.Create("Add", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_LABEL);
	TagTokenCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_COMBO);
	TagAddTokenButton.Create("Insert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_ADD_BUTTON);
	TagDefinitionHeader.Create("Definition", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DEF_HEADER);
	TagDefinitionEdit.Create(commonMultilineEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DEFINITION_EDIT);
	TagLine1Label.Create("L1", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE1_LABEL);
	TagLine1Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE1_EDIT);
	TagLine2Label.Create("L2", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE2_LABEL);
	TagLine2Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE2_EDIT);
	TagLine3Label.Create("L3", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE3_LABEL);
	TagLine3Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE3_EDIT);
	TagLine4Label.Create("L4", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE4_LABEL);
	TagLine4Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE4_EDIT);
	TagAutoDeconflictionToggle.Create("Auto Deconfliction", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_TAG_AUTO_DECONFLICTION);
	TagLinkDetailedToggle.Create("Custom Hover Details", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINK_DETAILED);
	TagDetailedHeader.Create("Definition Detailed", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DETAILED_HEADER);
	TagDetailedDefinitionEdit.Create(commonMultilineEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DETAILED_EDIT);
	TagDetailedLine1Label.Create("L1", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE1_LABEL);
	TagDetailedLine1Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE1_EDIT);
	TagDetailedLine2Label.Create("L2", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE2_LABEL);
	TagDetailedLine2Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE2_EDIT);
	TagDetailedLine3Label.Create("L3", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE3_LABEL);
	TagDetailedLine3Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE3_EDIT);
	TagDetailedLine4Label.Create("L4", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE4_LABEL);
	TagDetailedLine4Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_D_LINE4_EDIT);
	TagPreviewLabel.Create("Live Preview", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_PREVIEW_LABEL);
	TagPreviewEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, CRect(0, 0, 0, 0), this, IDC_PE_TAG_PREVIEW_EDIT);

	auto softenPanelBorder = [](CStatic& panel)
	{
		if (!::IsWindow(panel.GetSafeHwnd()))
			return;
		const LONG_PTR style = ::GetWindowLongPtr(panel.GetSafeHwnd(), GWL_STYLE);
		if ((style & SS_OWNERDRAW) != 0)
			return;
		panel.ModifyStyle(SS_ETCHEDFRAME, WS_BORDER);
	};
	softenPanelBorder(SidebarPanel);
	softenPanelBorder(ColorLeftPanel);
	softenPanelBorder(ColorRightPanel);
	softenPanelBorder(IconPanel);
	softenPanelBorder(IconShapePanel);
	softenPanelBorder(IconDisplayPanel);
	softenPanelBorder(IconPreviewPanel);
	softenPanelBorder(RuleLeftPanel);
	softenPanelBorder(RuleRightPanel);
	softenPanelBorder(ProfilePanel);
	softenPanelBorder(ProfileInfoPanel);
	softenPanelBorder(TagPanel);

	auto removeNativeBorder = [](CEdit& edit)
	{
		if (!::IsWindow(edit.GetSafeHwnd()))
			return;
		edit.ModifyStyle(WS_BORDER, 0);
		edit.ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
		edit.SetWindowPos(nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	};
	removeNativeBorder(EditRgba);
	removeNativeBorder(EditHex);
	removeNativeBorder(TagDefinitionEdit);
	removeNativeBorder(TagLine1Edit);
	removeNativeBorder(TagLine2Edit);
	removeNativeBorder(TagLine3Edit);
	removeNativeBorder(TagLine4Edit);
	removeNativeBorder(TagDetailedDefinitionEdit);
	removeNativeBorder(TagDetailedLine1Edit);
	removeNativeBorder(TagDetailedLine2Edit);
	removeNativeBorder(TagDetailedLine3Edit);
	removeNativeBorder(TagDetailedLine4Edit);

	TagTypeCombo.ResetContent();
	TagTypeCombo.AddString(TagTypeUiLabel(Owner, "departure").c_str());
	TagTypeCombo.AddString(TagTypeUiLabel(Owner, "arrival").c_str());
	TagTypeCombo.SetCurSel(0);
	TagLinkDetailedToggle.SetCheck(BST_UNCHECKED);
	TagAutoDeconflictionToggle.SetCheck(BST_CHECKED);
	PopulateTagTokenCombo();

	PopulateIconCombos();
	PopulateRuleCombos();
	HeaderBarBrush.CreateSolidBrush(kEditorThemeBackgroundColor);
	SidebarBrush.CreateSolidBrush(kEditorSidebarBackgroundColor);
	FixedScaleSlider.SetRange(1, 20, TRUE);
	FixedScaleSlider.SetTicFreq(1);
	FixedScaleSlider.SetLineSize(1);
	FixedScaleSlider.SetPageSize(1);
	BoostFactorSlider.SetRange(1, 20, TRUE);
	BoostFactorSlider.SetTicFreq(1);
	BoostFactorSlider.SetLineSize(1);
	BoostFactorSlider.SetPageSize(1);
	ColorValueSlider.SetRange(0, 100, TRUE);
	ColorValueSlider.SetTicFreq(10);
	ColorValueSlider.SetLineSize(1);
	ColorValueSlider.SetPageSize(10);
	ColorValueSlider.SetPos(100);
	ColorOpacitySlider.SetRange(0, 100, TRUE);
	ColorOpacitySlider.SetTicFreq(10);
	ColorOpacitySlider.SetLineSize(1);
	ColorOpacitySlider.SetPageSize(10);
	ColorOpacitySlider.SetPos(100);
	RuleColorValueSlider.SetRange(0, 100, TRUE);
	RuleColorValueSlider.SetTicFreq(10);
	RuleColorValueSlider.SetLineSize(1);
	RuleColorValueSlider.SetPageSize(10);
	RuleColorValueSlider.SetPos(100);
	RuleColorOpacitySlider.SetRange(0, 100, TRUE);
	RuleColorOpacitySlider.SetTicFreq(10);
	RuleColorOpacitySlider.SetLineSize(1);
	RuleColorOpacitySlider.SetPageSize(10);
	RuleColorOpacitySlider.SetPos(100);
	RuleColorActiveSwatchId = IDC_PE_RULE_TARGET_SWATCH;

	if (::IsWindow(ColorWheel.GetSafeHwnd()))
	{
		const HWND wheelHwnd = ColorWheel.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(wheelHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ColorWheelWndProc)));
		if (oldProc != nullptr)
		{
			gColorWheelOldProcs[wheelHwnd] = oldProc;
			gColorWheelOwnerWindows[wheelHwnd] = GetSafeHwnd();
		}
	}

	if (::IsWindow(ColorValueSlider.GetSafeHwnd()))
	{
		const HWND sliderHwnd = ColorValueSlider.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(sliderHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ColorValueSliderWndProc)));
		if (oldProc != nullptr)
		{
			gColorValueSliderOldProcs[sliderHwnd] = oldProc;
			gColorValueSliderOwnerWindows[sliderHwnd] = GetSafeHwnd();
		}
	}

	if (::IsWindow(ColorOpacitySlider.GetSafeHwnd()))
	{
		const HWND sliderHwnd = ColorOpacitySlider.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(sliderHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ColorOpacitySliderWndProc)));
		if (oldProc != nullptr)
		{
			gColorOpacitySliderOldProcs[sliderHwnd] = oldProc;
			gColorOpacitySliderOwnerWindows[sliderHwnd] = GetSafeHwnd();
		}
	}

	if (::IsWindow(RuleColorWheel.GetSafeHwnd()))
	{
		const HWND wheelHwnd = RuleColorWheel.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(wheelHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RuleColorWheelWndProc)));
		if (oldProc != nullptr)
		{
			gRuleColorWheelOldProcs[wheelHwnd] = oldProc;
			gRuleColorWheelOwnerWindows[wheelHwnd] = GetSafeHwnd();
		}
	}

	if (::IsWindow(RuleColorValueSlider.GetSafeHwnd()))
	{
		const HWND sliderHwnd = RuleColorValueSlider.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(sliderHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RuleColorValueSliderWndProc)));
		if (oldProc != nullptr)
		{
			gRuleColorValueSliderOldProcs[sliderHwnd] = oldProc;
			gRuleColorValueSliderOwnerWindows[sliderHwnd] = GetSafeHwnd();
		}
	}

	if (::IsWindow(RuleColorOpacitySlider.GetSafeHwnd()))
	{
		const HWND sliderHwnd = RuleColorOpacitySlider.GetSafeHwnd();
		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(sliderHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RuleColorOpacitySliderWndProc)));
		if (oldProc != nullptr)
		{
			gRuleColorOpacitySliderOldProcs[sliderHwnd] = oldProc;
			gRuleColorOpacitySliderOwnerWindows[sliderHwnd] = GetSafeHwnd();
		}
	}
	ColorPathTree.SetIndent(16);
	ColorPathTree.SetBkColor(kEditorThemeBackgroundColor);
	ColorPathTree.SetTextColor(RGB(17, 24, 39));
	if (GetFont() != nullptr)
	{
		LOGFONT lf = {};
		GetFont()->GetLogFont(&lf);
		LOGFONT titleLf = lf;
		titleLf.lfHeight = max(18, lf.lfHeight + 8);
		titleLf.lfWeight = FW_BOLD;
		TitleFont.CreateFontIndirect(&titleLf);
		LOGFONT sectionLf = lf;
		sectionLf.lfWeight = FW_BOLD;
		SectionHeaderFont.CreateFontIndirect(&sectionLf);
		LOGFONT uniformLf = lf;
		uniformLf.lfHeight = lf.lfHeight - 2;
		UniformUiFont.CreateFontIndirect(&uniformLf);
		LOGFONT linkLf = uniformLf;
		linkLf.lfUnderline = TRUE;
		LinkFont.CreateFontIndirect(&linkLf);
		LOGFONT monoLf = lf;
		strcpy_s(monoLf.lfFaceName, LF_FACESIZE, "Consolas");
		MonoFont.CreateFontIndirect(&monoLf);

		// Keep the OS default UI font for general controls, and use monospace only
		// for value-heavy fields where alignment/readability matters.
		ColorPathTree.SetFont(&MonoFont, TRUE);
		RuleTree.SetFont(&MonoFont, TRUE);
		ProfileList.SetFont(&MonoFont, TRUE);
		EditRgba.SetFont(GetFont(), TRUE);
		EditHex.SetFont(GetFont(), TRUE);
		RuleNameEdit.SetFont(GetFont(), TRUE);
		RuleTargetEdit.SetFont(GetFont(), TRUE);
		RuleTagEdit.SetFont(GetFont(), TRUE);
		RuleTextEdit.SetFont(GetFont(), TRUE);
		ProfileNameEdit.SetFont(GetFont(), TRUE);
		ProfileProModeCheck.SetFont(GetFont(), TRUE);
		TagAutoDeconflictionToggle.SetFont(GetFont(), TRUE);
		TagLine1Edit.SetFont(GetFont(), TRUE);
		TagLine2Edit.SetFont(GetFont(), TRUE);
		TagLine3Edit.SetFont(GetFont(), TRUE);
		TagLine4Edit.SetFont(GetFont(), TRUE);
		TagDetailedLine1Edit.SetFont(GetFont(), TRUE);
		TagDetailedLine2Edit.SetFont(GetFont(), TRUE);
		TagDetailedLine3Edit.SetFont(GetFont(), TRUE);
		TagDetailedLine4Edit.SetFont(GetFont(), TRUE);
		TagPreviewEdit.SetFont(&MonoFont, TRUE);
		PageTitleLabel.SetFont(&TitleFont, TRUE);
		PageSubtitleLabel.SetFont(GetFont(), TRUE);
		SidebarTitle.SetFont(&SectionHeaderFont, TRUE);
		ColorPathLabel.SetFont(&SectionHeaderFont, TRUE);
		SelectedPathText.SetFont(&SectionHeaderFont, TRUE);
		RuleLeftHeader.SetFont(&SectionHeaderFont, TRUE);
		RuleRightHeader.SetFont(&SectionHeaderFont, TRUE);
		ProfileHeader.SetFont(&SectionHeaderFont, TRUE);
		ProfileInfoHeader.SetFont(&SectionHeaderFont, TRUE);
		TagHeaderPanel.SetFont(&SectionHeaderFont, TRUE);
		IconShapeHeader.SetFont(&SectionHeaderFont, TRUE);
		IconSizeHeader.SetFont(&SectionHeaderFont, TRUE);
		IconDisplayHeader.SetFont(&SectionHeaderFont, TRUE);
		IconPreviewHeader.SetFont(&SectionHeaderFont, TRUE);

		// Use one consistent font everywhere in the profile editor:
		// match the font used by text editor fields.
		CFont* uniformFont = (UniformUiFont.GetSafeHandle() != nullptr) ? &UniformUiFont : GetFont();
		if (uniformFont != nullptr)
		{
			for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetNextWindow())
			{
				if (::IsWindow(child->GetSafeHwnd()))
					child->SetFont(uniformFont, TRUE);
			}
		}
		if (LinkFont.GetSafeHandle() != nullptr)
		{
			ProfileRepoLink.SetFont(&LinkFont, TRUE);
			ProfileCoffeeLink.SetFont(&LinkFont, TRUE);
		}
	}
	ApplyThemedEditBorders();
	ApplyThemedComboBorders();
	ControlsCreated = true;
	LayoutControls();
}

void CProfileEditorDialog::ApplyThemedEditBorders()
{
	auto attachBorder = [](CEdit& edit)
	{
		const HWND hwnd = edit.GetSafeHwnd();
		if (!::IsWindow(hwnd))
			return;

		auto it = gThemedEditOldProcs.find(hwnd);
		if (it != gThemedEditOldProcs.end())
		{
			if (it->second != nullptr)
				::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second));
			gThemedEditOldProcs.erase(it);
		}

		edit.ModifyStyle(WS_BORDER, 0);
		edit.ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
		edit.SetWindowPos(nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		edit.SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));

		CWnd* parent = edit.GetParent();
		if (parent != nullptr && ::IsWindow(parent->GetSafeHwnd()))
		{
			CRect rect;
			edit.GetWindowRect(&rect);
			parent->ScreenToClient(&rect);
			if (rect.Width() > 8 && rect.Height() > 8)
			{
				rect.DeflateRect(2, 2);
				edit.MoveWindow(rect, TRUE);
			}
		}
	};

	attachBorder(EditRgba);
	attachBorder(EditHex);
	attachBorder(RuleNameEdit);
	attachBorder(RuleTargetEdit);
	attachBorder(RuleTagEdit);
	attachBorder(RuleTextEdit);
	attachBorder(TagDefinitionEdit);
	attachBorder(TagLine1Edit);
	attachBorder(TagLine2Edit);
	attachBorder(TagLine3Edit);
	attachBorder(TagLine4Edit);
	attachBorder(TagDetailedDefinitionEdit);
	attachBorder(TagDetailedLine1Edit);
	attachBorder(TagDetailedLine2Edit);
	attachBorder(TagDetailedLine3Edit);
	attachBorder(TagDetailedLine4Edit);
	attachBorder(ProfileNameEdit);
}

void CProfileEditorDialog::ApplyThemedComboBorders()
{
	// Keep native combo rendering. The custom border subclass causes
	// paint artifacts in the closed state for dropdown controls.
}

void CProfileEditorDialog::SetEditTextPreserveCaret(CEdit& edit, const std::string& text)
{
	if (!::IsWindow(edit.GetSafeHwnd()))
		return;

	CString currentText;
	edit.GetWindowText(currentText);
	if (currentText.Compare(text.c_str()) == 0)
		return;

	const CWnd* focused = GetFocus();
	const bool keepCaret = (focused != nullptr && focused->GetSafeHwnd() == edit.GetSafeHwnd());
	int selStart = 0;
	int selEnd = 0;
	if (keepCaret)
		edit.GetSel(selStart, selEnd);

	edit.SetWindowTextA(text.c_str());

	if (keepCaret)
	{
		const int length = max(0, edit.GetWindowTextLength());
		const int newSelStart = min(selStart, length);
		const int newSelEnd = min(selEnd, length);
		edit.SetSel(newSelStart, newSelEnd);
	}
}

void CProfileEditorDialog::EnsureColorWheelBitmap(const CRect& wheelRect)
{
	const int width = max(1, wheelRect.Width());
	const int height = max(1, wheelRect.Height());
	if (ColorWheelBitmap.GetSafeHandle() != nullptr &&
		ColorWheelBitmapSize.cx == width &&
		ColorWheelBitmapSize.cy == height)
	{
		return;
	}

	if (ColorWheelBitmap.GetSafeHandle() != nullptr)
		ColorWheelBitmap.DeleteObject();

	CClientDC screenDc(this);
	CDC memDc;
	memDc.CreateCompatibleDC(&screenDc);
	if (!ColorWheelBitmap.CreateCompatibleBitmap(&screenDc, width, height))
		return;

	CBitmap* oldBitmap = memDc.SelectObject(&ColorWheelBitmap);
	memDc.FillSolidRect(0, 0, width, height, kEditorThemeBackgroundColor);

	const int diameter = min(width, height);
	const int radius = max(1, (diameter / 2) - 2);
	const int centerX = width / 2;
	const int centerY = height / 2;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const double dx = static_cast<double>(x - centerX);
			const double dy = static_cast<double>(centerY - y);
			const double distance = sqrt((dx * dx) + (dy * dy));
			if (distance > static_cast<double>(radius))
				continue;

			double hue = atan2(dy, dx) * (180.0 / 3.14159265358979323846);
			if (hue < 0.0)
				hue += 360.0;
			const double saturation = min(1.0, max(0.0, distance / static_cast<double>(radius)));
			int r = 255;
			int g = 255;
			int b = 255;
			HsvToRgb(hue, saturation, 1.0, r, g, b);
			memDc.SetPixelV(x, y, RGB(r, g, b));
		}
	}

	memDc.SelectObject(oldBitmap);
	ColorWheelBitmapSize = CSize(width, height);
}

void CProfileEditorDialog::PopulateIconCombos()
{
	FixedScaleCombo.ResetContent();
	const char* fixedScales[] = { "0.10x", "0.25x", "0.50x", "0.65x", "0.80x", "1.00x", "1.25x", "1.50x", "2.00x" };
	for (const char* value : fixedScales)
		FixedScaleCombo.AddString(value);

	BoostFactorCombo.ResetContent();
	const char* boostFactors[] = { "0.75x", "1.00x", "1.25x", "1.50x", "2.00x", "2.50x", "3.00x" };
	for (const char* value : boostFactors)
		BoostFactorCombo.AddString(value);

	BoostResolutionCombo.ResetContent();
	BoostResolutionCombo.AddString("1080p");
	BoostResolutionCombo.AddString("2K");
	BoostResolutionCombo.AddString("4K");
}

void CProfileEditorDialog::PopulateRuleTokenCombo(const std::string& source, const std::string& selectedToken)
{
	RuleTokenCombo.ResetContent();
	std::vector<std::string> tokens;
	std::string normalizedSource = source;
	std::transform(normalizedSource.begin(), normalizedSource.end(), normalizedSource.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (normalizedSource == "runway")
	{
		tokens = { "deprwy", "seprwy", "arvrwy", "srvrwy" };
	}
	else if (normalizedSource == "custom" || normalizedSource == "sid")
	{
		tokens = { "asid", "ssid" };
	}
	else
	{
		tokens = { "tobt", "tsat", "ttot", "asat", "aobt", "atot", "asrt", "aort", "ctot" };
	}

	int selectedIndex = 0;
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		RuleTokenCombo.AddString(tokens[i].c_str());
		if (!selectedToken.empty() && _stricmp(tokens[i].c_str(), selectedToken.c_str()) == 0)
			selectedIndex = static_cast<int>(i);
	}
	if (RuleTokenCombo.GetCount() > 0)
		RuleTokenCombo.SetCurSel(selectedIndex);
}

void CProfileEditorDialog::PopulateRuleCombos()
{
	RuleSourceCombo.ResetContent();
	RuleSourceCombo.AddString("VACDM");
	RuleSourceCombo.AddString("Runway");
	RuleSourceCombo.AddString("SID");
	RuleSourceCombo.SetCurSel(0);

	RuleTypeCombo.ResetContent();
	const std::vector<std::string> typeOptions = {
		"Any",
		RuleTypeUiLabel(Owner, "departure"),
		RuleTypeUiLabel(Owner, "arrival")
	};
	for (const std::string& value : typeOptions)
		RuleTypeCombo.AddString(value.c_str());
	SelectComboEntryByText(RuleTypeCombo, "Any");

	PopulateRuleStatusCombo("Any", "Any");

	RuleDetailCombo.ResetContent();
	RuleDetailCombo.AddString("any");
	RuleDetailCombo.AddString("normal");
	RuleDetailCombo.AddString("detailed");
	RuleDetailCombo.SetCurSel(0);

	PopulateRuleTokenCombo("vacdm", "tobt");
	PopulateRuleConditionCombo("vacdm", "tobt", "any");
}

void CProfileEditorDialog::PopulateRuleStatusCombo(const std::string& selectedType, const std::string& selectedStatus)
{
	RuleStatusCombo.ResetContent();
	const std::vector<std::string> statusOptions = RuleStatusUiOptions(Owner, selectedType);
	for (const std::string& value : statusOptions)
		RuleStatusCombo.AddString(value.c_str());

	const std::string desiredStatus = RuleStatusUiLabel(Owner, selectedType, selectedStatus.empty() ? "any" : selectedStatus);
	SelectComboEntryByText(RuleStatusCombo, desiredStatus);
	if (RuleStatusCombo.GetCurSel() == CB_ERR && RuleStatusCombo.GetCount() > 0)
		RuleStatusCombo.SetCurSel(0);
}

void CProfileEditorDialog::PopulateRuleConditionCombo(const std::string& source, const std::string& token, const std::string& selectedCondition)
{
	RuleConditionCombo.ResetContent();

	std::string normalizedSource = source;
	std::transform(normalizedSource.begin(), normalizedSource.end(), normalizedSource.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string normalizedToken = token;
	std::transform(normalizedToken.begin(), normalizedToken.end(), normalizedToken.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	std::vector<std::string> conditions;
	if (normalizedSource == "runway")
	{
		conditions = { "any", "set", "missing" };
	}
	else if (normalizedSource == "custom" || normalizedSource == "sid")
	{
		conditions = { "any", "set", "missing", "in: SID1X,SID2A", "not_in: SID1X,SID2A" };
	}
	else if (normalizedToken == "tobt")
	{
		conditions = { "any", "set", "missing", "inactive", "unconfirmed", "confirmed", "unconfirmed_delay", "confirmed_delay", "expired" };
	}
	else if (normalizedToken == "tsat")
	{
		conditions = { "any", "set", "missing", "inactive", "future", "valid", "expired", "future_ctot", "valid_ctot", "expired_ctot" };
	}
	else
	{
		conditions = { "any", "set", "missing", "future", "past" };
	}

	std::string normalizedSelectedCondition = selectedCondition;
	normalizedSelectedCondition.erase(normalizedSelectedCondition.begin(), std::find_if(normalizedSelectedCondition.begin(), normalizedSelectedCondition.end(), [](unsigned char c) { return std::isspace(c) == 0; }));
	normalizedSelectedCondition.erase(std::find_if(normalizedSelectedCondition.rbegin(), normalizedSelectedCondition.rend(), [](unsigned char c) { return std::isspace(c) == 0; }).base(), normalizedSelectedCondition.end());

	int selectedIndex = CB_ERR;
	for (size_t i = 0; i < conditions.size(); ++i)
	{
		RuleConditionCombo.AddString(conditions[i].c_str());
		if (!normalizedSelectedCondition.empty() && _stricmp(conditions[i].c_str(), normalizedSelectedCondition.c_str()) == 0)
			selectedIndex = static_cast<int>(i);
	}

	if (selectedIndex != CB_ERR)
		RuleConditionCombo.SetCurSel(selectedIndex);
	else if (!normalizedSelectedCondition.empty())
		RuleConditionCombo.SetWindowTextA(normalizedSelectedCondition.c_str());
	else if (RuleConditionCombo.GetCount() > 0)
		RuleConditionCombo.SetCurSel(0);
}

void CProfileEditorDialog::PopulateTagTokenCombo()
{
	if (Owner == nullptr)
		return;

	std::string previousSelection;
	const int previousIndex = TagTokenCombo.GetCurSel();
	if (previousIndex != CB_ERR)
	{
		CString selectedText;
		TagTokenCombo.GetLBText(previousIndex, selectedText);
		previousSelection = selectedText.GetString();
	}

	TagTokenCombo.ResetContent();
	const std::vector<std::string> tokens = Owner->GetTagDefinitionTokens();
	int selectedIndex = 0;
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		TagTokenCombo.AddString(tokens[i].c_str());
		if (!previousSelection.empty() && _stricmp(tokens[i].c_str(), previousSelection.c_str()) == 0)
			selectedIndex = static_cast<int>(i);
	}
	if (TagTokenCombo.GetCount() > 0)
		TagTokenCombo.SetCurSel(selectedIndex);
}

void CProfileEditorDialog::RebuildProfileList()
{
	if (Owner == nullptr)
		return;

	const std::string previousSelected = GetSelectedProfileName();
	const std::string activeProfile = Owner->GetActiveProfileNameForEditor();
	ProfileNames = Owner->GetProfileNamesForEditor();

	UpdatingControls = true;
	ProfileList.ResetContent();
	int selectedIndex = -1;
	for (size_t i = 0; i < ProfileNames.size(); ++i)
	{
		ProfileList.AddString(ProfileNames[i].c_str());
		if (!activeProfile.empty() && _stricmp(ProfileNames[i].c_str(), activeProfile.c_str()) == 0)
			selectedIndex = static_cast<int>(i);
	}

	if (selectedIndex < 0)
	{
		for (size_t i = 0; i < ProfileNames.size(); ++i)
		{
			if (!previousSelected.empty() && _stricmp(ProfileNames[i].c_str(), previousSelected.c_str()) == 0)
			{
				selectedIndex = static_cast<int>(i);
				break;
			}
		}
	}
	if (selectedIndex < 0 && !ProfileNames.empty())
		selectedIndex = 0;

	SelectedProfileListIndex = selectedIndex;
	if (selectedIndex >= 0)
	{
		ProfileList.SetCurSel(selectedIndex);
		SetEditTextPreserveCaret(ProfileNameEdit, ProfileNames[selectedIndex]);
	}
	else
	{
		SetEditTextPreserveCaret(ProfileNameEdit, "");
	}
	UpdatingControls = false;
}

std::string CProfileEditorDialog::GetSelectedProfileName() const
{
	const int selected = ProfileList.GetCurSel();
	if (selected == LB_ERR || selected < 0 || selected >= static_cast<int>(ProfileNames.size()))
		return "";
	return ProfileNames[selected];
}

void CProfileEditorDialog::RefreshProfileControls()
{
	bool hasSelection = (!GetSelectedProfileName().empty());
	if (!hasSelection && ProfileList.GetCount() > 0)
	{
		ProfileList.SetCurSel(0);
		if (!ProfileNames.empty())
			SetEditTextPreserveCaret(ProfileNameEdit, ProfileNames[0]);
		hasSelection = (!GetSelectedProfileName().empty());
	}

	bool proModeEnabled = false;
	const std::string selectedProfile = GetSelectedProfileName();
	if (hasSelection && Owner != nullptr)
		Owner->GetProfileProModeEnabledForEditor(selectedProfile, proModeEnabled);

	UpdatingControls = true;
	ProfileProModeCheck.SetCheck(proModeEnabled ? BST_CHECKED : BST_UNCHECKED);
	UpdatingControls = false;

	ProfileNameEdit.EnableWindow(hasSelection ? TRUE : FALSE);
	ProfileProModeCheck.EnableWindow(hasSelection ? TRUE : FALSE);
	ProfileDuplicateButton.EnableWindow(hasSelection ? TRUE : FALSE);
	ProfileRenameButton.EnableWindow(hasSelection ? TRUE : FALSE);
	ProfileDeleteButton.EnableWindow(hasSelection ? TRUE : FALSE);
}

void CProfileEditorDialog::LayoutControls()
{
	if (!ControlsCreated)
		return;

	CRect clientRect;
	GetClientRect(&clientRect);

	const int sidebarWidth = 128;
	const int sidebarPad = 16;
	const int navButtonHeight = 38;
	const int navGap = 8;
	const int mainPad = 18;
	const int topBarHeight = 10;

	PageTabs.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);

	SidebarPanel.MoveWindow(clientRect.left, clientRect.top, sidebarWidth, max(120, clientRect.Height()), TRUE);
	SidebarTitle.MoveWindow(clientRect.left + sidebarPad, clientRect.top + 18, sidebarWidth - (sidebarPad * 2), 20, TRUE);
	SidebarDivider.MoveWindow(clientRect.left + sidebarWidth - 1, clientRect.top, 2, max(120, clientRect.Height()), TRUE);
	int navY = clientRect.top + 46;
	const int navWidth = sidebarWidth - (sidebarPad * 2);
	NavColorsButton.MoveWindow(clientRect.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);
	navY += navButtonHeight + navGap;
	NavIconButton.MoveWindow(clientRect.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);
	navY += navButtonHeight + navGap;
	NavRulesButton.MoveWindow(clientRect.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);
	navY += navButtonHeight + navGap;
	NavProfileButton.MoveWindow(clientRect.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);

	const int mainLeft = clientRect.left + sidebarWidth + mainPad;
	const int mainTop = clientRect.top + mainPad;
	const int mainWidth = max(220, clientRect.Width() - sidebarWidth - (mainPad * 2));
	const int mainHeight = max(140, clientRect.Height() - (mainPad * 2));
	const int actionButtonWidth = 96;
	const int actionButtonGap = 10;
	MoveControlOffscreen(PageTitleLabel);
	MoveControlOffscreen(PageSubtitleLabel);
	ResetColorButton.MoveWindow(mainLeft + mainWidth - actionButtonWidth, mainTop, actionButtonWidth, 38, TRUE);
	ApplyColorButton.MoveWindow(mainLeft + mainWidth - (actionButtonWidth * 2) - actionButtonGap, mainTop, actionButtonWidth, 38, TRUE);

	CRect pageRect(mainLeft, mainTop + topBarHeight, mainLeft + mainWidth, mainTop + mainHeight);

	const int innerPad = 16;
	const int colorGap = 16;
	const int availableWidth = max(200, pageRect.Width() - (innerPad * 2) - colorGap);
	const int colorLeftWidth = availableWidth / 2;
	const int colorRightWidth = availableWidth - colorLeftWidth;
	const int colorTop = pageRect.top + innerPad;
	const int panelHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int colorLeft = pageRect.left + innerPad;
	const int rightLeft = colorLeft + colorLeftWidth + colorGap;

	const int rowHeight = 24;
	const int buttonHeight = 38;
	auto measureRadioWidth = [&](CButton& button, int minWidth)
	{
		CString text;
		button.GetWindowText(text);
		CClientDC dc(this);
		CFont* font = button.GetFont();
		CFont* oldFont = (font != nullptr) ? dc.SelectObject(font) : nullptr;
		const CSize textSize = dc.GetTextExtent(text);
		if (oldFont != nullptr)
			dc.SelectObject(oldFont);
		return max(minWidth, textSize.cx + 28);
	};

	// Colors: left tree panel + right editor panel
	ColorLeftPanel.MoveWindow(colorLeft, colorTop, colorLeftWidth, panelHeight, TRUE);
	ColorRightPanel.MoveWindow(rightLeft, colorTop, colorRightWidth, panelHeight, TRUE);
	ColorPathLabel.MoveWindow(colorLeft + 10, colorTop + 10, max(60, colorLeftWidth - 20), 24, TRUE);
	const int colorTreeTop = colorTop + 36;
	const int colorTreeHeight = max(80, panelHeight - 34);
	ColorPathTree.MoveWindow(colorLeft + 8, colorTreeTop + 4, max(80, colorLeftWidth - 16), max(60, colorTreeHeight - 12), TRUE);

	SelectedPathText.MoveWindow(rightLeft + 10, colorTop + 10, max(60, colorRightWidth - 20), 24, TRUE);

	int y = colorTop + 40;
	const int previewWidth = max(120, colorRightWidth - 28);
	ColorPreviewLabel.MoveWindow(rightLeft + 14, y, previewWidth, rowHeight, TRUE);
	y += rowHeight + 8;

	ColorPreviewSwatch.MoveWindow(rightLeft + 14, y, previewWidth, 52, TRUE);
	y += 52 + 16;

	const int sliderGap = 16;
	const int sliderWidth = 24;
	const int sliderColumnsWidth = (sliderWidth * 2) + sliderGap;
	const int wheelToSliderGap = 16;
	const int wheelAreaWidth = max(88, previewWidth - wheelToSliderGap - sliderColumnsWidth);
	const int wheelSize = min(170, wheelAreaWidth);
	const int wheelLeft = rightLeft + 14;
	const int sliderLabelTop = y;
	const int wheelTop = sliderLabelTop + rowHeight + 2;
	const int valueSliderLeft = wheelLeft + wheelSize + wheelToSliderGap;
	const int opacitySliderLeft = valueSliderLeft + sliderWidth + sliderGap;

	const int valueLabelWidth = 40;
	const int opacityLabelWidth = 56;
	int valueLabelLeft = valueSliderLeft - ((valueLabelWidth - sliderWidth) / 2);
	int opacityLabelLeft = opacitySliderLeft - ((opacityLabelWidth - sliderWidth) / 2);
	const int minLabelGap = 6;
	if ((valueLabelLeft + valueLabelWidth + minLabelGap) > opacityLabelLeft)
		opacityLabelLeft = valueLabelLeft + valueLabelWidth + minLabelGap;

	ColorPickerLabel.MoveWindow(wheelLeft, sliderLabelTop, wheelSize, rowHeight, TRUE);
	ColorWheel.MoveWindow(wheelLeft, wheelTop, wheelSize, wheelSize, TRUE);
	ColorValueLabel.MoveWindow(valueLabelLeft, sliderLabelTop, valueLabelWidth, rowHeight, TRUE);
	ColorValueSlider.MoveWindow(valueSliderLeft, wheelTop, sliderWidth, wheelSize, TRUE);
	ColorOpacityLabel.MoveWindow(opacityLabelLeft, sliderLabelTop, opacityLabelWidth, rowHeight, TRUE);
	ColorOpacitySlider.MoveWindow(opacitySliderLeft, wheelTop, sliderWidth, wheelSize, TRUE);
	y = wheelTop + wheelSize + 16;

	const int rgbaLabelWidth = 56;
	LabelRgba.MoveWindow(rightLeft + 14, y + 3, rgbaLabelWidth, rowHeight, TRUE);
	EditRgba.MoveWindow(rightLeft + 14 + rgbaLabelWidth + 6, y, max(120, previewWidth - rgbaLabelWidth - 6), rowHeight, TRUE);
	y += rowHeight + 8;

	LabelHex.MoveWindow(rightLeft + 14, y + 3, rgbaLabelWidth, rowHeight, TRUE);
	EditHex.MoveWindow(rightLeft + 14 + rgbaLabelWidth + 6, y, max(120, previewWidth - rgbaLabelWidth - 6), rowHeight, TRUE);
	y += rowHeight + 12;
	ApplyColorButton.MoveWindow(rightLeft + 14, y, 82, 34, TRUE);
	ResetColorButton.MoveWindow(rightLeft + 106, y, 82, 34, TRUE);

	const int iconTop = pageRect.top + innerPad;
	const int iconHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int iconGap = 16;
	const int iconAvailableWidth = max(560, pageRect.Width() - (innerPad * 2) - iconGap);
	const int iconLeftWidth = iconAvailableWidth / 2;
	const int iconRightWidth = iconAvailableWidth - iconLeftWidth;
	const int iconLeft = pageRect.left + innerPad;
	const int iconRightLeft = iconLeft + iconLeftWidth + iconGap;
	const int tagLeft = iconRightLeft;
	const int tagTop = iconTop;
	const int tagWidth = iconRightWidth;
	const int tagHeight = iconHeight;
	const int iconShapeCardHeight = 80;
	const int iconDisplayCardHeight = 96;
	const int iconSizeCardHeight = max(120, iconHeight - iconShapeCardHeight - iconDisplayCardHeight - (iconGap * 2));

	IconShapePanel.MoveWindow(iconLeft, iconTop, iconLeftWidth, iconShapeCardHeight, TRUE);
	IconPanel.MoveWindow(iconLeft, iconTop + iconShapeCardHeight + iconGap, iconLeftWidth, iconSizeCardHeight, TRUE);
	IconDisplayPanel.MoveWindow(iconLeft, iconTop + iconShapeCardHeight + iconGap + iconSizeCardHeight + iconGap, iconLeftWidth, iconDisplayCardHeight, TRUE);
	TagPanel.MoveWindow(tagLeft, tagTop, tagWidth, tagHeight, TRUE);

	const int iconPad = 16;
	const int iconContentLeft = iconLeft + iconPad;
	const int iconContentRight = iconLeft + iconLeftWidth - iconPad;
	const int iconContentWidth = max(120, iconContentRight - iconContentLeft);
	const int iconCheckboxHeight = 22;
	const int iconSliderHeight = 24;
	const int iconTickHeight = 16;
	const int iconTickWidth = 54;

	IconShapeHeader.MoveWindow(iconContentLeft, iconTop + 14, 100, rowHeight, TRUE);
	const int shapeRowY = iconTop + 44;
	const int iconRadioGap = 12;
	const int arrowWidth = measureRadioWidth(IconStyleArrow, 72);
	const int diamondWidth = measureRadioWidth(IconStyleDiamond, 84);
	const int realisticWidth = measureRadioWidth(IconStyleRealistic, 92);
	const int shapeRowWidth = arrowWidth + iconRadioGap + diamondWidth + iconRadioGap + realisticWidth;
	const int shapeRowLeft = iconContentLeft + max(0, (iconContentWidth - shapeRowWidth) / 2);
	IconStyleArrow.MoveWindow(shapeRowLeft, shapeRowY, arrowWidth, iconCheckboxHeight, TRUE);
	IconStyleDiamond.MoveWindow(shapeRowLeft + arrowWidth + iconRadioGap, shapeRowY, diamondWidth, iconCheckboxHeight, TRUE);
	IconStyleRealistic.MoveWindow(shapeRowLeft + arrowWidth + iconRadioGap + diamondWidth + iconRadioGap, shapeRowY, realisticWidth, iconCheckboxHeight, TRUE);

	int iconY = iconTop + iconShapeCardHeight + iconGap + 14;
	IconSizeHeader.MoveWindow(iconContentLeft, iconY, 120, rowHeight, TRUE);
	iconY += rowHeight + 8;

	FixedPixelCheck.MoveWindow(iconContentLeft, iconY, 140, iconCheckboxHeight, TRUE);
	iconY += iconCheckboxHeight + 12;

	FixedScaleLabel.MoveWindow(iconContentLeft, iconY + 2, 110, rowHeight, TRUE);
	FixedScaleValueLabel.MoveWindow(iconContentRight - 70, iconY + 2, 70, rowHeight, TRUE);
	iconY += rowHeight + 2;
	FixedScaleSlider.MoveWindow(iconContentLeft, iconY, iconContentWidth, iconSliderHeight, TRUE);
	iconY += iconSliderHeight + 2;
	FixedScaleTickMinLabel.MoveWindow(iconContentLeft, iconY, iconTickWidth, iconTickHeight, TRUE);
	FixedScaleTickMidLabel.MoveWindow(iconContentLeft + (iconContentWidth / 2) - (iconTickWidth / 2), iconY, iconTickWidth, iconTickHeight, TRUE);
	FixedScaleTickMaxLabel.MoveWindow(iconContentRight - iconTickWidth, iconY, iconTickWidth, iconTickHeight, TRUE);
	iconY += iconTickHeight + 14;

	SmallBoostCheck.MoveWindow(iconContentLeft, iconY, 160, iconCheckboxHeight, TRUE);
	iconY += iconCheckboxHeight + 12;

	BoostFactorLabel.MoveWindow(iconContentLeft, iconY + 2, 110, rowHeight, TRUE);
	BoostFactorValueLabel.MoveWindow(iconContentRight - 70, iconY + 2, 70, rowHeight, TRUE);
	iconY += rowHeight + 2;
	BoostFactorSlider.MoveWindow(iconContentLeft, iconY, iconContentWidth, iconSliderHeight, TRUE);
	iconY += iconSliderHeight + 2;
	BoostFactorTickMinLabel.MoveWindow(iconContentLeft, iconY, iconTickWidth, iconTickHeight, TRUE);
	BoostFactorTickMidLabel.MoveWindow(iconContentLeft + (iconContentWidth / 2) - (iconTickWidth / 2), iconY, iconTickWidth, iconTickHeight, TRUE);
	BoostFactorTickMaxLabel.MoveWindow(iconContentRight - iconTickWidth, iconY, iconTickWidth, iconTickHeight, TRUE);

	const int displayContentLeft = iconLeft + iconPad;
	IconDisplayHeader.MoveWindow(displayContentLeft, iconTop + iconShapeCardHeight + iconGap + iconSizeCardHeight + iconGap + 12, 100, rowHeight, TRUE);
	BoostResolutionLabel.MoveWindow(displayContentLeft, iconTop + iconShapeCardHeight + iconGap + iconSizeCardHeight + iconGap + 44, 90, rowHeight, TRUE);
	const int resButtonsTop = iconTop + iconShapeCardHeight + iconGap + iconSizeCardHeight + iconGap + 44;
	const int resolutionLabelWidth = 88;
	const int resolutionRadioGap = 10;
	const int resolution1080Width = measureRadioWidth(BoostResolution1080Button, 66);
	const int resolution2kWidth = measureRadioWidth(BoostResolution2KButton, 44);
	const int resolution4kWidth = measureRadioWidth(BoostResolution4KButton, 44);
	const int resolutionRowWidth = resolution1080Width + resolutionRadioGap + resolution2kWidth + resolutionRadioGap + resolution4kWidth;
	const int resolutionButtonsLeft = displayContentLeft + resolutionLabelWidth + max(8, (max(0, iconContentWidth - resolutionLabelWidth) - resolutionRowWidth) / 2);
	BoostResolution1080Button.MoveWindow(resolutionButtonsLeft, resButtonsTop, resolution1080Width, 22, TRUE);
	BoostResolution2KButton.MoveWindow(resolutionButtonsLeft + resolution1080Width + resolutionRadioGap, resButtonsTop, resolution2kWidth, 22, TRUE);
	BoostResolution4KButton.MoveWindow(resolutionButtonsLeft + resolution1080Width + resolutionRadioGap + resolution2kWidth + resolutionRadioGap, resButtonsTop, resolution4kWidth, 22, TRUE);
	MoveControlOffscreen(BoostResolutionCombo);
	MoveControlOffscreen(IconSeparator1);
	MoveControlOffscreen(IconSeparator2);
	MoveControlOffscreen(IconSeparator3);

	TagHeaderPanel.MoveWindow(tagLeft + 10, tagTop + 10, max(60, tagWidth - 20), 24, TRUE);

	const int tagPad = 12;
	const int tagLabelWidth = 52;
	const int tagContentLeft = tagLeft + tagPad;
	const int tagFieldLeft = tagContentLeft + tagLabelWidth + 10;
	const int tagRight = tagLeft + tagWidth - tagPad;
	const int baseFieldWidth = max(110, tagRight - tagFieldLeft);
	const int tokenButtonWidth = actionButtonWidth;
	const int tokenComboWidth = max(90, baseFieldWidth - tokenButtonWidth - 8);
	const int definitionEditWidth = max(140, tagWidth - (tagPad * 2));
	int definitionLineHeight = rowHeight - 6;
	{
		CClientDC dc(this);
		CFont* font = TagDefinitionEdit.GetFont();
		CFont* oldFont = (font != nullptr) ? dc.SelectObject(font) : nullptr;
		TEXTMETRIC tm = {};
		if (dc.GetTextMetrics(&tm))
			definitionLineHeight = max(14, tm.tmHeight + tm.tmExternalLeading);
		if (oldFont != nullptr)
			dc.SelectObject(oldFont);
	}
	const int definitionEditHeight = max(72, (definitionLineHeight * 4) + 8);

	int tagY = tagTop + 44;
	TagTypeLabel.MoveWindow(tagContentLeft, tagY + 4, tagLabelWidth, rowHeight, TRUE);
	TagTypeCombo.MoveWindow(tagFieldLeft, tagY, baseFieldWidth, rowHeight + 220, TRUE);
	tagY += rowHeight + 10;

	TagStatusLabel.MoveWindow(tagContentLeft, tagY + 4, tagLabelWidth, rowHeight, TRUE);
	TagStatusCombo.MoveWindow(tagFieldLeft, tagY, baseFieldWidth, rowHeight + 220, TRUE);
	tagY += rowHeight + 10;

	TagTokenLabel.MoveWindow(tagContentLeft, tagY + 4, tagLabelWidth, rowHeight, TRUE);
	TagTokenCombo.MoveWindow(tagFieldLeft, tagY, tokenComboWidth, rowHeight + 220, TRUE);
	TagAddTokenButton.MoveWindow(tagFieldLeft + tokenComboWidth + 8, tagY, tokenButtonWidth, rowHeight, TRUE);
	tagY += rowHeight + 12;

	TagDefinitionHeader.MoveWindow(tagContentLeft, tagY, max(100, tagWidth - (tagPad * 2)), rowHeight, TRUE);
	tagY += rowHeight + 8;
	TagDefinitionEdit.MoveWindow(tagContentLeft, tagY, definitionEditWidth, definitionEditHeight, TRUE);
	tagY += definitionEditHeight + 10;

	TagLinkDetailedToggle.MoveWindow(tagContentLeft, tagY, max(180, tagWidth - (tagPad * 2)), rowHeight, TRUE);
	tagY += rowHeight + 8;

	if (TagEditorSeparateDetailed)
	{
		TagDetailedHeader.MoveWindow(tagContentLeft, tagY, max(100, tagWidth - (tagPad * 2)), rowHeight, TRUE);
		tagY += rowHeight + 6;
		TagDetailedDefinitionEdit.MoveWindow(tagContentLeft, tagY, definitionEditWidth, definitionEditHeight, TRUE);
		tagY += definitionEditHeight + 10;
		TagAutoDeconflictionToggle.MoveWindow(tagContentLeft, tagY, max(180, tagWidth - (tagPad * 2)), rowHeight, TRUE);
	}
	else
	{
		TagAutoDeconflictionToggle.MoveWindow(tagContentLeft, tagY, max(180, tagWidth - (tagPad * 2)), rowHeight, TRUE);
		const int hiddenDetailedY = tagY + rowHeight + 8;
		TagDetailedHeader.MoveWindow(tagContentLeft, hiddenDetailedY, max(100, tagWidth - (tagPad * 2)), rowHeight, TRUE);
		TagDetailedDefinitionEdit.MoveWindow(tagContentLeft, hiddenDetailedY + rowHeight + 6, definitionEditWidth, definitionEditHeight, TRUE);
	}

	MoveControlOffscreen(TagLine1Label);
	MoveControlOffscreen(TagLine1Edit);
	MoveControlOffscreen(TagLine2Label);
	MoveControlOffscreen(TagLine2Edit);
	MoveControlOffscreen(TagLine3Label);
	MoveControlOffscreen(TagLine3Edit);
	MoveControlOffscreen(TagLine4Label);
	MoveControlOffscreen(TagLine4Edit);
	MoveControlOffscreen(TagDetailedLine1Label);
	MoveControlOffscreen(TagDetailedLine1Edit);
	MoveControlOffscreen(TagDetailedLine2Label);
	MoveControlOffscreen(TagDetailedLine2Edit);
	MoveControlOffscreen(TagDetailedLine3Label);
	MoveControlOffscreen(TagDetailedLine3Edit);
	MoveControlOffscreen(TagDetailedLine4Label);
	MoveControlOffscreen(TagDetailedLine4Edit);

	MoveControlOffscreen(IconPreviewPanel);
	MoveControlOffscreen(IconPreviewHeader);
	MoveControlOffscreen(IconPreviewSwatch);
	MoveControlOffscreen(IconPreviewHint);
	MoveControlOffscreen(FixedScaleCombo);
	MoveControlOffscreen(BoostFactorCombo);

	// Keep interactive Icon controls above decorative card panels.
	const UINT iconContentIds[] = {
		IDC_PE_ICON_STYLE_ARROW, IDC_PE_ICON_STYLE_DIAMOND, IDC_PE_ICON_STYLE_REALISTIC,
		IDC_PE_FIXED_PIXEL_CHECK, IDC_PE_FIXED_SCALE_LABEL, IDC_PE_FIXED_SCALE_VALUE, IDC_PE_FIXED_SCALE_SLIDER,
		IDC_PE_FIXED_SCALE_TICK_MIN, IDC_PE_FIXED_SCALE_TICK_MID, IDC_PE_FIXED_SCALE_TICK_MAX,
		IDC_PE_SMALL_BOOST_CHECK, IDC_PE_BOOST_FACTOR_LABEL, IDC_PE_BOOST_FACTOR_VALUE, IDC_PE_BOOST_FACTOR_SLIDER,
		IDC_PE_BOOST_FACTOR_TICK_MIN, IDC_PE_BOOST_FACTOR_TICK_MID, IDC_PE_BOOST_FACTOR_TICK_MAX,
		IDC_PE_BOOST_RES_LABEL, IDC_PE_BOOST_RES_COMBO, IDC_PE_ICON_RES_1080, IDC_PE_ICON_RES_2K, IDC_PE_ICON_RES_4K,
		IDC_PE_ICON_SHAPE_HEADER, IDC_PE_ICON_SIZE_HEADER, IDC_PE_ICON_DISPLAY_HEADER,
		IDC_PE_TAG_HEADER_PANEL, IDC_PE_TAG_TYPE_LABEL, IDC_PE_TAG_TYPE_COMBO, IDC_PE_TAG_STATUS_LABEL,
		IDC_PE_TAG_STATUS_COMBO, IDC_PE_TAG_TOKEN_LABEL, IDC_PE_TAG_TOKEN_COMBO, IDC_PE_TAG_TOKEN_ADD_BUTTON,
		IDC_PE_TAG_DEF_HEADER, IDC_PE_TAG_DEFINITION_EDIT, IDC_PE_TAG_AUTO_DECONFLICTION, IDC_PE_TAG_LINK_DETAILED, IDC_PE_TAG_DETAILED_HEADER,
		IDC_PE_TAG_DETAILED_EDIT
	};
	for (UINT controlId : iconContentIds)
	{
		if (CWnd* control = GetDlgItem(controlId))
		{
			if (::IsWindow(control->GetSafeHwnd()))
			{
				control->SetWindowPos(&CWnd::wndTop, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			}
		}
	}

	const int rulesTop = pageRect.top + innerPad;
	const int rulesPanelHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int rulesGap = 16;
	const int rulesAvailableWidth = max(220, pageRect.Width() - (innerPad * 2) - rulesGap);
	const int rulesLeftWidth = max(220, static_cast<int>(rulesAvailableWidth * 0.50));
	const int rulesRightWidth = max(220, rulesAvailableWidth - rulesLeftWidth);
	const int rulesLeft = pageRect.left + innerPad;
	const int rulesRightLeft = rulesLeft + rulesLeftWidth + rulesGap;

	RuleLeftPanel.MoveWindow(rulesLeft, rulesTop, rulesLeftWidth, rulesPanelHeight, TRUE);
	RuleRightPanel.MoveWindow(rulesRightLeft, rulesTop, rulesRightWidth, rulesPanelHeight, TRUE);
	RuleLeftHeader.MoveWindow(rulesLeft + 10, rulesTop + 10, max(60, rulesLeftWidth - 20), 24, TRUE);
	RuleRightHeader.MoveWindow(rulesRightLeft + 10, rulesTop + 10, max(60, rulesRightWidth - 20), 24, TRUE);

	const int ruleListTop = rulesTop + 38;
	const int ruleButtonAreaHeight = 44;
	const int ruleListHeight = max(80, rulesPanelHeight - 36 - ruleButtonAreaHeight - 8);
	const int ruleListWidth = max(90, rulesLeftWidth - 16);
	MoveControlOffscreen(RulesList);
	RuleTree.MoveWindow(rulesLeft + 8, ruleListTop, ruleListWidth, ruleListHeight, TRUE);
	const int ruleButtonsY = rulesTop + rulesPanelHeight - 46;
	const int ruleButtonsTotalWidth = (actionButtonWidth * 2) + actionButtonGap;
	const int ruleButtonsLeft = rulesLeft + max(12, ((ruleListWidth - ruleButtonsTotalWidth) / 2));
	RuleAddButton.MoveWindow(ruleButtonsLeft, ruleButtonsY, actionButtonWidth, buttonHeight, TRUE);
	RuleRemoveButton.MoveWindow(ruleButtonsLeft + actionButtonWidth + actionButtonGap, ruleButtonsY, actionButtonWidth, buttonHeight, TRUE);
	MoveControlOffscreen(RuleAddParameterButton);

	int rulesY = rulesTop + 44;
	const int rulesLabelWidth = 110;
	const int rulesContentLeft = rulesRightLeft + 14;
	const int rulesContentWidth = max(120, rulesRightWidth - 28);
	const int rulesFieldLeft = rulesContentLeft + rulesLabelWidth + 12;
	const int rulesLabelLeft = rulesContentLeft;
	const int rulesFieldWidth = max(90, rulesRightWidth - (rulesFieldLeft - rulesRightLeft) - 14);
	int paramY = rulesY;
	RuleSourceLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleSourceCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);
	paramY += rowHeight + 8;
	RuleTokenLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTokenCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);
	paramY += rowHeight + 8;
	RuleConditionLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleConditionCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);

	int effectY = rulesY;
	RuleNameLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleNameEdit.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight, TRUE);
	effectY += rowHeight + 8;
	RuleTypeLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTypeCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 8;
	RuleStatusLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleStatusCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 8;
	RuleDetailLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleDetailCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 16;

	const int ruleSwatchSize = rowHeight;
	const int ruleSwatchGap = 8;
	const int ruleEditLeft = rulesFieldLeft + ruleSwatchSize + ruleSwatchGap;
	const int ruleEditWidth = max(60, rulesFieldWidth - ruleSwatchSize - ruleSwatchGap);
	RuleTargetCheck.MoveWindow(rulesLabelLeft, effectY, 120, rowHeight, TRUE);
	RuleTargetSwatch.MoveWindow(rulesFieldLeft, effectY, ruleSwatchSize, ruleSwatchSize, TRUE);
	RuleTargetEdit.MoveWindow(ruleEditLeft, effectY, ruleEditWidth, rowHeight, TRUE);
	effectY += rowHeight + 8;
	RuleTagCheck.MoveWindow(rulesLabelLeft, effectY, 120, rowHeight, TRUE);
	RuleTagSwatch.MoveWindow(rulesFieldLeft, effectY, ruleSwatchSize, ruleSwatchSize, TRUE);
	RuleTagEdit.MoveWindow(ruleEditLeft, effectY, ruleEditWidth, rowHeight, TRUE);
	effectY += rowHeight + 8;
	RuleTextCheck.MoveWindow(rulesLabelLeft, effectY, 120, rowHeight, TRUE);
	RuleTextSwatch.MoveWindow(rulesFieldLeft, effectY, ruleSwatchSize, ruleSwatchSize, TRUE);
	RuleTextEdit.MoveWindow(ruleEditLeft, effectY, ruleEditWidth, rowHeight, TRUE);
	effectY += rowHeight + 10;

	const int ruleWheelLabelTop = effectY;
	const int ruleWheelSliderGap = 8;
	const int ruleWheelSliderWidth = 20;
	const int ruleWheelSize = max(72, min(140, rulesContentWidth - (ruleWheelSliderWidth * 2) - (ruleWheelSliderGap * 2)));
	const int ruleWheelGroupWidth = ruleWheelSize + ruleWheelSliderGap + ruleWheelSliderWidth + ruleWheelSliderGap + ruleWheelSliderWidth;
	const int ruleWheelLeft = rulesContentLeft + max(0, (rulesContentWidth - ruleWheelGroupWidth) / 2);
	const int ruleWheelTop = ruleWheelLabelTop + rowHeight + 2;
	const int ruleValueSliderLeft = ruleWheelLeft + ruleWheelSize + ruleWheelSliderGap;
	const int ruleOpacitySliderLeft = ruleValueSliderLeft + ruleWheelSliderWidth + ruleWheelSliderGap;
	const int ruleValueLabelWidth = 40;
	const int ruleValueLabelLeft = ruleValueSliderLeft - ((ruleValueLabelWidth - ruleWheelSliderWidth) / 2);
	const int ruleOpacityLabelWidth = 52;
	const int ruleOpacityLabelLeft = ruleOpacitySliderLeft - ((ruleOpacityLabelWidth - ruleWheelSliderWidth) / 2);
	RuleColorWheelLabel.MoveWindow(ruleWheelLeft, ruleWheelLabelTop, max(60, ruleWheelSize), rowHeight, TRUE);
	RuleColorWheel.MoveWindow(ruleWheelLeft, ruleWheelTop, ruleWheelSize, ruleWheelSize, TRUE);
	RuleColorValueLabel.MoveWindow(ruleValueLabelLeft, ruleWheelLabelTop, ruleValueLabelWidth, rowHeight, TRUE);
	RuleColorValueSlider.MoveWindow(ruleValueSliderLeft, ruleWheelTop, ruleWheelSliderWidth, ruleWheelSize, TRUE);
	RuleColorOpacityLabel.MoveWindow(ruleOpacityLabelLeft, ruleWheelLabelTop, ruleOpacityLabelWidth, rowHeight, TRUE);
	RuleColorOpacitySlider.MoveWindow(ruleOpacitySliderLeft, ruleWheelTop, ruleWheelSliderWidth, ruleWheelSize, TRUE);
	effectY = ruleWheelTop + ruleWheelSize + 10;

	const int rulePreviewWidth = max(130, min(rulesContentWidth, 220));
	const int rulePreviewLeft = rulesContentLeft + max(0, (rulesContentWidth - rulePreviewWidth) / 2);
	RuleColorPreviewLabel.MoveWindow(rulePreviewLeft, effectY, rulePreviewWidth, rowHeight, TRUE);
	effectY += rowHeight + 4;
	RuleColorPreviewSwatch.MoveWindow(rulePreviewLeft, effectY, rulePreviewWidth, 44, TRUE);
	effectY += 44 + 10;

	const int ruleActionButtonsWidth = (actionButtonWidth * 2) + actionButtonGap;
	const int ruleActionLeft = rulesContentLeft + max(0, (rulesContentWidth - ruleActionButtonsWidth) / 2);
	RuleColorApplyButton.MoveWindow(ruleActionLeft, effectY, actionButtonWidth, buttonHeight, TRUE);
	RuleColorResetButton.MoveWindow(ruleActionLeft + actionButtonWidth + actionButtonGap, effectY, actionButtonWidth, buttonHeight, TRUE);

	const int profilePageLeft = pageRect.left + innerPad;
	const int profileTop = pageRect.top + innerPad;
	const int profileGap = 18;
	const int profilePageWidth = max(240, pageRect.Width() - (innerPad * 2));
	const int profileHeight = max(180, pageRect.Height() - (innerPad * 2));
	const int profileLeftWidth = max(180, (profilePageWidth - profileGap) / 2);
	const int profileRightWidth = max(180, profilePageWidth - profileLeftWidth - profileGap);
	const int profileInfoLeft = profilePageLeft + profileLeftWidth + profileGap;
	ProfilePanel.MoveWindow(profilePageLeft, profileTop, profileLeftWidth, profileHeight, TRUE);
	ProfileInfoPanel.MoveWindow(profileInfoLeft, profileTop, profileRightWidth, profileHeight, TRUE);

	ProfileHeader.MoveWindow(profilePageLeft + 10, profileTop + 14, max(60, profileLeftWidth - 20), 24, TRUE);
	const int profileLeftContentLeft = profilePageLeft + 12;
	const int profileLeftContentTop = profileTop + 46;
	const int profileLeftContentWidth = max(180, profileLeftWidth - 24);
	const int profileButtonsRowGap = 12;
	const int profileToggleTopGap = 12;
	const int profileToggleHeight = rowHeight;
	const int profileButtonTopGap = 18;
	const int profileFooterHeight = (buttonHeight * 2) + rowHeight + profileToggleHeight + profileButtonsRowGap + profileToggleTopGap + profileButtonTopGap + 48;
	const int profileListHeight = max(100, profileHeight - 52 - profileFooterHeight);
	ProfileList.MoveWindow(profileLeftContentLeft, profileLeftContentTop, profileLeftContentWidth, profileListHeight, TRUE);

	const int profileDetailsTop = profileLeftContentTop + profileListHeight + 18;
	const int profileToggleTop = profileDetailsTop + 6;
	const int profileNameTop = profileToggleTop + profileToggleHeight + profileToggleTopGap;
	ProfileProModeCheck.MoveWindow(profileLeftContentLeft + 12, profileToggleTop, max(120, profileLeftContentWidth - 24), profileToggleHeight, TRUE);
	ProfileNameLabel.MoveWindow(profileLeftContentLeft + 12, profileNameTop + 4, 56, rowHeight, TRUE);
	ProfileNameEdit.MoveWindow(profileLeftContentLeft + 12 + 56 + 10, profileNameTop, max(140, profileLeftContentWidth - 90), rowHeight, TRUE);

	const int profileButtonWidth = max(78, min(actionButtonWidth, (profileLeftContentWidth - actionButtonGap) / 2));
	const int profileButtonsRowWidth = (profileButtonWidth * 2) + actionButtonGap;
	const int profileButtonsLeft = profileLeftContentLeft + max(0, (profileLeftContentWidth - profileButtonsRowWidth) / 2);
	const int profileButtonsTop = profileNameTop + rowHeight + profileButtonTopGap;
	ProfileAddButton.MoveWindow(profileButtonsLeft, profileButtonsTop, profileButtonWidth, buttonHeight, TRUE);
	ProfileDuplicateButton.MoveWindow(profileButtonsLeft + profileButtonWidth + actionButtonGap, profileButtonsTop, profileButtonWidth, buttonHeight, TRUE);
	ProfileRenameButton.MoveWindow(profileButtonsLeft, profileButtonsTop + buttonHeight + profileButtonsRowGap, profileButtonWidth, buttonHeight, TRUE);
	ProfileDeleteButton.MoveWindow(profileButtonsLeft + profileButtonWidth + actionButtonGap, profileButtonsTop + buttonHeight + profileButtonsRowGap, profileButtonWidth, buttonHeight, TRUE);

	const int profileInfoContentLeft = profileInfoLeft + 14;
	const int profileInfoContentWidth = max(140, profileRightWidth - 28);
	ProfileInfoHeader.MoveWindow(profileInfoContentLeft, profileTop + 14, profileInfoContentWidth, rowHeight, TRUE);
	const int profileInfoBodyHeight = max(rowHeight * 4, MeasureWrappedStaticHeight(ProfileInfoBody, profileInfoContentWidth));
	ProfileInfoBody.MoveWindow(profileInfoContentLeft, profileTop + 46, profileInfoContentWidth, profileInfoBodyHeight + 4, TRUE);
	const int profileLinksTop = profileTop + 46 + profileInfoBodyHeight + 12;
	ProfileRepoLink.MoveWindow(profileInfoContentLeft, profileLinksTop, profileInfoContentWidth, rowHeight, TRUE);
	ProfileCoffeeLink.MoveWindow(profileInfoContentLeft, profileLinksTop + 28, profileInfoContentWidth, rowHeight, TRUE);

	// Keep live preview generated, but hidden to match the compact editor layout.
	MoveControlOffscreen(TagPreviewLabel);
	MoveControlOffscreen(TagPreviewEdit);

	ApplyThemedEditBorders();
	UpdatePageVisibility();
}

void CProfileEditorDialog::UpdatePageVisibility()
{
	if (!ControlsCreated)
		return;

	const int selectedTab = PageTabs.GetCurSel();
	switch (selectedTab)
	{
	case kTabColors:
		PageTitleLabel.SetWindowTextA("Colors");
		PageSubtitleLabel.SetWindowTextA("Select a color entry on the left and edit it on the right.");
		break;
	case kTabIcons:
		PageTitleLabel.SetWindowTextA("Icons & Tags");
		PageSubtitleLabel.SetWindowTextA("Icon settings on the left, tag definitions on the right.");
		break;
	case kTabRules:
		PageTitleLabel.SetWindowTextA("Rules");
		PageSubtitleLabel.SetWindowTextA("Rule groups on the left, selected rule settings on the right.");
		break;
	case kTabProfile:
		PageTitleLabel.SetWindowTextA("Profiles");
		PageSubtitleLabel.SetWindowTextA("Manage profiles on the left and find support info on the right.");
		break;
	default:
		PageTitleLabel.SetWindowTextA("Profile Editor");
		PageSubtitleLabel.SetWindowTextA("");
		break;
	}
	const bool showColors = (selectedTab == kTabColors);
	const bool showIcons = (selectedTab == kTabIcons);
	const bool showRules = (selectedTab == kTabRules);
	const bool showProfile = (selectedTab == kTabProfile);
	const bool showTagEditor = showIcons;
	const bool showDetailedTag = showTagEditor && TagEditorSeparateDetailed;
	LastVisibilityTab = selectedTab;
	LastVisibilityDetailedTag = showDetailedTag;
	const int colorShowMode = showColors ? SW_SHOW : SW_HIDE;
	const int iconShowMode = showIcons ? SW_SHOW : SW_HIDE;
	const int ruleShowMode = showRules ? SW_SHOW : SW_HIDE;
	const bool hasRuleSelection = (SelectedRuleIndex >= 0 && SelectedRuleIndex < static_cast<int>(RuleBuffer.size()));
	const bool isParameterSelection = hasRuleSelection && SelectedRuleCriterionIndex >= 0;
	const int ruleParameterShowMode = (showRules && isParameterSelection) ? SW_SHOW : SW_HIDE;
	const int ruleEffectShowMode = (showRules && !isParameterSelection) ? SW_SHOW : SW_HIDE;
	const int tagShowMode = showTagEditor ? SW_SHOW : SW_HIDE;
	const int detailedTagShowMode = showDetailedTag ? SW_SHOW : SW_HIDE;

	SidebarPanel.ShowWindow(SW_HIDE);
	SidebarTitle.ShowWindow(SW_SHOW);
	SidebarDivider.ShowWindow(SW_HIDE);
	NavColorsButton.ShowWindow(SW_SHOW);
	NavIconButton.ShowWindow(SW_SHOW);
	NavRulesButton.ShowWindow(SW_SHOW);
	NavProfileButton.ShowWindow(SW_SHOW);
	PageTitleLabel.ShowWindow(SW_HIDE);
	PageSubtitleLabel.ShowWindow(SW_HIDE);
	NavColorsButton.Invalidate(FALSE);
	NavIconButton.Invalidate(FALSE);
	NavRulesButton.Invalidate(FALSE);
	NavProfileButton.Invalidate(FALSE);
	PageTabs.ShowWindow(SW_HIDE);

	ColorLeftPanel.ShowWindow(SW_HIDE);
	ColorRightPanel.ShowWindow(SW_HIDE);
	ColorPathTree.ShowWindow(colorShowMode);
	ColorPathLabel.ShowWindow(colorShowMode);
	SelectedPathText.ShowWindow(colorShowMode);
	ColorPickerLabel.ShowWindow(colorShowMode);
	ColorPreviewLabel.ShowWindow(colorShowMode);
	ColorPreviewSwatch.ShowWindow(colorShowMode);
	ColorWheel.ShowWindow(colorShowMode);
	ColorValueLabel.ShowWindow(colorShowMode);
	ColorValueSlider.ShowWindow(colorShowMode);
	ColorOpacityLabel.ShowWindow(colorShowMode);
	ColorOpacitySlider.ShowWindow(colorShowMode);
	LabelRgba.ShowWindow(colorShowMode);
	EditRgba.ShowWindow(colorShowMode);
	LabelHex.ShowWindow(colorShowMode);
	EditHex.ShowWindow(colorShowMode);
	ApplyColorButton.ShowWindow(colorShowMode);
	ResetColorButton.ShowWindow(colorShowMode);

	// Keep decorative icon card panels hidden to avoid overpainting child controls.
	IconPanel.ShowWindow(SW_HIDE);
	IconShapePanel.ShowWindow(SW_HIDE);
	IconDisplayPanel.ShowWindow(SW_HIDE);
	IconPreviewPanel.ShowWindow(SW_HIDE);
	IconStyleArrow.ShowWindow(iconShowMode);
	IconStyleDiamond.ShowWindow(iconShowMode);
	IconStyleRealistic.ShowWindow(iconShowMode);
	IconShapeHeader.ShowWindow(iconShowMode);
	IconSizeHeader.ShowWindow(iconShowMode);
	IconDisplayHeader.ShowWindow(iconShowMode);
	IconPreviewHeader.ShowWindow(SW_HIDE);
	IconPreviewSwatch.ShowWindow(SW_HIDE);
	IconPreviewHint.ShowWindow(SW_HIDE);
	IconSeparator1.ShowWindow(SW_HIDE);
	IconSeparator2.ShowWindow(SW_HIDE);
	IconSeparator3.ShowWindow(SW_HIDE);
	FixedPixelCheck.ShowWindow(iconShowMode);
	FixedScaleLabel.ShowWindow(iconShowMode);
	FixedScaleValueLabel.ShowWindow(iconShowMode);
	FixedScaleSlider.ShowWindow(iconShowMode);
	FixedScaleTickMinLabel.ShowWindow(iconShowMode);
	FixedScaleTickMidLabel.ShowWindow(iconShowMode);
	FixedScaleTickMaxLabel.ShowWindow(iconShowMode);
	FixedScaleCombo.ShowWindow(SW_HIDE);
	SmallBoostCheck.ShowWindow(iconShowMode);
	BoostFactorLabel.ShowWindow(iconShowMode);
	BoostFactorValueLabel.ShowWindow(iconShowMode);
	BoostFactorSlider.ShowWindow(iconShowMode);
	BoostFactorTickMinLabel.ShowWindow(iconShowMode);
	BoostFactorTickMidLabel.ShowWindow(iconShowMode);
	BoostFactorTickMaxLabel.ShowWindow(iconShowMode);
	BoostFactorCombo.ShowWindow(SW_HIDE);
	BoostResolutionLabel.ShowWindow(iconShowMode);
	BoostResolutionCombo.ShowWindow(SW_HIDE);
	BoostResolution1080Button.ShowWindow(iconShowMode);
	BoostResolution2KButton.ShowWindow(iconShowMode);
	BoostResolution4KButton.ShowWindow(iconShowMode);

	RulesList.ShowWindow(SW_HIDE);
	RuleTree.ShowWindow(ruleShowMode);
	RuleLeftPanel.ShowWindow(SW_HIDE);
	RuleRightPanel.ShowWindow(SW_HIDE);
	RuleLeftHeader.ShowWindow(ruleShowMode);
	RuleRightHeader.ShowWindow(ruleShowMode);
	RuleAddButton.ShowWindow(ruleShowMode);
	RuleAddParameterButton.ShowWindow(SW_HIDE);
	RuleRemoveButton.ShowWindow(ruleShowMode);
	RuleNameLabel.ShowWindow(ruleEffectShowMode);
	RuleNameEdit.ShowWindow(ruleEffectShowMode);
	RuleSourceLabel.ShowWindow(ruleParameterShowMode);
	RuleSourceCombo.ShowWindow(ruleParameterShowMode);
	RuleTokenLabel.ShowWindow(ruleParameterShowMode);
	RuleTokenCombo.ShowWindow(ruleParameterShowMode);
	RuleConditionLabel.ShowWindow(ruleParameterShowMode);
	RuleConditionCombo.ShowWindow(ruleParameterShowMode);
	RuleTypeLabel.ShowWindow(ruleEffectShowMode);
	RuleTypeCombo.ShowWindow(ruleEffectShowMode);
	RuleStatusLabel.ShowWindow(ruleEffectShowMode);
	RuleStatusCombo.ShowWindow(ruleEffectShowMode);
	RuleDetailLabel.ShowWindow(ruleEffectShowMode);
	RuleDetailCombo.ShowWindow(ruleEffectShowMode);
	RuleTargetCheck.ShowWindow(ruleEffectShowMode);
	RuleTargetSwatch.ShowWindow(ruleEffectShowMode);
	RuleTargetEdit.ShowWindow(ruleEffectShowMode);
	RuleTagCheck.ShowWindow(ruleEffectShowMode);
	RuleTagSwatch.ShowWindow(ruleEffectShowMode);
	RuleTagEdit.ShowWindow(ruleEffectShowMode);
	RuleTextCheck.ShowWindow(ruleEffectShowMode);
	RuleTextSwatch.ShowWindow(ruleEffectShowMode);
	RuleTextEdit.ShowWindow(ruleEffectShowMode);
	RuleColorWheelLabel.ShowWindow(ruleEffectShowMode);
	RuleColorWheel.ShowWindow(ruleEffectShowMode);
	RuleColorValueLabel.ShowWindow(ruleEffectShowMode);
	RuleColorValueSlider.ShowWindow(ruleEffectShowMode);
	RuleColorOpacityLabel.ShowWindow(ruleEffectShowMode);
	RuleColorOpacitySlider.ShowWindow(ruleEffectShowMode);
	RuleColorPreviewLabel.ShowWindow(ruleEffectShowMode);
	RuleColorPreviewSwatch.ShowWindow(ruleEffectShowMode);
	RuleColorApplyButton.ShowWindow(ruleEffectShowMode);
	RuleColorResetButton.ShowWindow(ruleEffectShowMode);
	ShowControls(
	{
		&ProfileHeader, &ProfileList, &ProfileNameLabel, &ProfileNameEdit, &ProfileProModeCheck,
		&ProfileAddButton, &ProfileDuplicateButton, &ProfileRenameButton, &ProfileDeleteButton,
		&ProfileInfoHeader, &ProfileInfoBody, &ProfileRepoLink, &ProfileCoffeeLink
	},
	showProfile ? SW_SHOW : SW_HIDE);
	ProfilePanel.ShowWindow(SW_HIDE);
	ProfileInfoPanel.ShowWindow(SW_HIDE);

	TagPanel.ShowWindow(SW_HIDE);
	TagHeaderPanel.ShowWindow(tagShowMode);
	TagTypeLabel.ShowWindow(tagShowMode);
	TagTypeCombo.ShowWindow(tagShowMode);
	TagStatusLabel.ShowWindow(tagShowMode);
	TagStatusCombo.ShowWindow(tagShowMode);
	TagTokenLabel.ShowWindow(tagShowMode);
	TagTokenCombo.ShowWindow(tagShowMode);
	TagAddTokenButton.ShowWindow(tagShowMode);
	TagDefinitionHeader.ShowWindow(tagShowMode);
	TagDefinitionEdit.ShowWindow(tagShowMode);
	TagLine1Label.ShowWindow(SW_HIDE);
	TagLine1Edit.ShowWindow(SW_HIDE);
	TagLine2Label.ShowWindow(SW_HIDE);
	TagLine2Edit.ShowWindow(SW_HIDE);
	TagLine3Label.ShowWindow(SW_HIDE);
	TagLine3Edit.ShowWindow(SW_HIDE);
	TagLine4Label.ShowWindow(SW_HIDE);
	TagLine4Edit.ShowWindow(SW_HIDE);
	TagAutoDeconflictionToggle.ShowWindow(tagShowMode);
	TagLinkDetailedToggle.ShowWindow(tagShowMode);
	TagDetailedHeader.ShowWindow(detailedTagShowMode);
	TagDetailedDefinitionEdit.ShowWindow(detailedTagShowMode);
	TagDetailedLine1Label.ShowWindow(SW_HIDE);
	TagDetailedLine1Edit.ShowWindow(SW_HIDE);
	TagDetailedLine2Label.ShowWindow(SW_HIDE);
	TagDetailedLine2Edit.ShowWindow(SW_HIDE);
	TagDetailedLine3Label.ShowWindow(SW_HIDE);
	TagDetailedLine3Edit.ShowWindow(SW_HIDE);
	TagDetailedLine4Label.ShowWindow(SW_HIDE);
	TagDetailedLine4Edit.ShowWindow(SW_HIDE);
	TagPreviewLabel.ShowWindow(SW_HIDE);
	TagPreviewEdit.ShowWindow(SW_HIDE);

	Invalidate(FALSE);
}

void CProfileEditorDialog::RebuildColorPathList()
{
	if (Owner == nullptr)
		return;

	std::string previousSelection = Owner->GetSelectedProfileColorPathForEditor();
	ColorPathEntries = Owner->GetProfileColorPathsForEditor();

	if (ColorPathEntries.empty())
	{
		UpdatingControls = true;
		ColorPathTree.DeleteAllItems();
		UpdatingControls = false;
		return;
	}

	auto findPath = [&](const std::string& path) -> bool
	{
		return std::find(ColorPathEntries.begin(), ColorPathEntries.end(), path) != ColorPathEntries.end();
	};

	if (previousSelection.empty() || !findPath(previousSelection))
		previousSelection = ColorPathEntries.front();

	RebuildColorPathTree(previousSelection);

	Owner->SelectProfileColorPathForEditor(previousSelection);
	LoadDraftColorFromSelection();
}

void CProfileEditorDialog::RebuildColorPathTree(const std::string& selectedPath)
{
	std::map<std::string, bool> expandedStateByPrefix;
	if (::IsWindow(ColorPathTree.GetSafeHwnd()))
	{
		std::function<void(HTREEITEM, const std::string&)> captureExpandedState;
		captureExpandedState = [&](HTREEITEM item, const std::string& parentPrefix)
		{
			HTREEITEM currentItem = item;
			while (currentItem != nullptr)
			{
				CString itemText = ColorPathTree.GetItemText(currentItem);
				const std::string segment = itemText.GetString();
				const std::string prefix = parentPrefix.empty() ? segment : (parentPrefix + "." + segment);
				expandedStateByPrefix[prefix] = (ColorPathTree.GetItemState(currentItem, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;

				HTREEITEM child = ColorPathTree.GetChildItem(currentItem);
				if (child != nullptr)
					captureExpandedState(child, prefix);

				currentItem = ColorPathTree.GetNextSiblingItem(currentItem);
			}
		};

		captureExpandedState(ColorPathTree.GetRootItem(), "");
	}

	UpdatingControls = true;
	ColorPathTree.DeleteAllItems();
	ColorTreeItemPaths.clear();

	std::map<std::string, HTREEITEM> nodeByPrefix;
	for (const std::string& path : ColorPathEntries)
	{
		const std::vector<std::string> segments = SplitPathSegments(path);
		if (segments.empty())
			continue;

		HTREEITEM parent = TVI_ROOT;
		std::string prefix;
		for (size_t i = 0; i < segments.size(); ++i)
		{
			if (!prefix.empty())
				prefix += ".";
			prefix += segments[i];

			auto itNode = nodeByPrefix.find(prefix);
			if (itNode == nodeByPrefix.end())
			{
				std::string displaySegment = FormatColorPathSegmentForDisplay(segments[i]);
				if (i + 1 == segments.size() &&
					(_stricmp(path.c_str(), "labels.departure.background_no_status_color") == 0 ||
					 _stricmp(path.c_str(), "labels.departure.gate_color") == 0 ||
					 _stricmp(path.c_str(), "labels.departure.background_color") == 0))
				{
					displaySegment = "Background No Status";
				}
				else if (i + 1 == segments.size() &&
					_stricmp(path.c_str(), "labels.uncorrelated.background_on_runway_color") == 0)
				{
					displaySegment = "Background On Runways";
				}

				HTREEITEM item = ColorPathTree.InsertItem(displaySegment.c_str(), parent);
				nodeByPrefix.insert(std::make_pair(prefix, item));
				itNode = nodeByPrefix.find(prefix);
			}

			parent = itNode->second;
			if (i + 1 == segments.size())
				ColorTreeItemPaths[parent] = path;
		}
	}

	for (const auto& node : nodeByPrefix)
	{
		auto itExpanded = expandedStateByPrefix.find(node.first);
		if (itExpanded != expandedStateByPrefix.end() && itExpanded->second)
			ColorPathTree.Expand(node.second, TVE_EXPAND);
	}

	SelectColorPathInTree(selectedPath);
	UpdatingControls = false;
}

bool CProfileEditorDialog::SelectColorPathInTree(const std::string& path)
{
	for (const auto& kv : ColorTreeItemPaths)
	{
		if (_stricmp(kv.second.c_str(), path.c_str()) == 0)
		{
			ColorPathTree.SelectItem(kv.first);
			HTREEITEM parent = ColorPathTree.GetParentItem(kv.first);
			while (parent != nullptr)
			{
				ColorPathTree.Expand(parent, TVE_EXPAND);
				parent = ColorPathTree.GetParentItem(parent);
			}
			return true;
		}
	}
	return false;
}

std::string CProfileEditorDialog::GetSelectedTreePath() const
{
	if (!::IsWindow(ColorPathTree.GetSafeHwnd()))
		return "";
	const HTREEITEM selected = ColorPathTree.GetSelectedItem();
	if (selected == nullptr)
		return "";

	auto it = ColorTreeItemPaths.find(selected);
	if (it == ColorTreeItemPaths.end())
		return "";
	return it->second;
}

void CProfileEditorDialog::LoadDraftColorFromSelection()
{
	if (Owner == nullptr)
		return;

	int r = 0, g = 0, b = 0, a = 255;
	bool hasAlpha = false;
	if (!Owner->GetSelectedProfileColorForEditor(r, g, b, a, hasAlpha))
		return;

	DraftColorR = r;
	DraftColorG = g;
	DraftColorB = b;
	DraftColorA = a;
	DraftColorHasAlpha = hasAlpha;
	DraftColorValid = true;
	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
	DraftColorHue = hue;
	DraftColorSaturation = saturation;
	DraftColorHueSaturationValid = true;
	UpdateDraftColorControls(false, true);
}

void CProfileEditorDialog::SyncColorValueSliderFromDraft()
{
	if (!DraftColorValid || !::IsWindow(ColorValueSlider.GetSafeHwnd()))
		return;

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
	const int position = min(100, max(0, static_cast<int>(round(value * 100.0))));

	UpdatingControls = true;
	ColorValueSlider.SetPos(position);
	UpdatingControls = false;
}

void CProfileEditorDialog::SyncColorOpacitySliderFromDraft()
{
	if (!DraftColorValid || !::IsWindow(ColorOpacitySlider.GetSafeHwnd()))
		return;

	const int alpha = min(255, max(0, DraftColorA));
	const int position = min(100, max(0, static_cast<int>(round((static_cast<double>(alpha) / 255.0) * 100.0))));

	UpdatingControls = true;
	ColorOpacitySlider.SetPos(position);
	UpdatingControls = false;
}

void CProfileEditorDialog::ApplyDraftColorValueFromSlider()
{
	if (UpdatingControls || !DraftColorValid)
		return;
	if (!::IsWindow(ColorValueSlider.GetSafeHwnd()))
		return;

	double hue = DraftColorHue;
	double saturation = DraftColorSaturation;
	if (!DraftColorHueSaturationValid)
	{
		double value = 1.0;
		RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
		DraftColorHue = hue;
		DraftColorSaturation = saturation;
		DraftColorHueSaturationValid = true;
	}

	const double sliderValue = min(1.0, max(0.0, static_cast<double>(ColorValueSlider.GetPos()) / 100.0));
	int r = 255;
	int g = 255;
	int b = 255;
	HsvToRgb(hue, saturation, sliderValue, r, g, b);

	DraftColorR = r;
	DraftColorG = g;
	DraftColorB = b;
	UpdateDraftColorControls(true, true, false);
}

void CProfileEditorDialog::ApplyDraftColorOpacityFromSlider()
{
	if (UpdatingControls || !DraftColorValid)
		return;
	if (!::IsWindow(ColorOpacitySlider.GetSafeHwnd()))
		return;

	const double sliderAlpha = min(1.0, max(0.0, static_cast<double>(ColorOpacitySlider.GetPos()) / 100.0));
	const int alpha = min(255, max(0, static_cast<int>(round(sliderAlpha * 255.0))));
	DraftColorA = alpha;
	if (alpha != 255)
		DraftColorHasAlpha = true;
	UpdateDraftColorControls(true, true, false);
}

void CProfileEditorDialog::UpdateDraftColorControls(bool updateRgba, bool updateHex, bool invalidateWheel)
{
	if (!DraftColorValid)
		return;

	char rgbaBuffer[48] = {};
	char hexBuffer[16] = {};
	sprintf_s(rgbaBuffer, sizeof(rgbaBuffer), "%d,%d,%d,%d", DraftColorR, DraftColorG, DraftColorB, DraftColorA);
	if (DraftColorHasAlpha || DraftColorA != 255)
		sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X%02X", DraftColorR, DraftColorG, DraftColorB, DraftColorA);
	else
		sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X", DraftColorR, DraftColorG, DraftColorB);

	UpdatingControls = true;
	if (updateRgba)
		SetEditTextPreserveCaret(EditRgba, rgbaBuffer);
	if (updateHex)
		SetEditTextPreserveCaret(EditHex, hexBuffer);
	UpdatingControls = false;

	SyncColorValueSliderFromDraft();
	SyncColorOpacitySliderFromDraft();
	ColorPreviewSwatch.Invalidate(FALSE);
	if (invalidateWheel)
		ColorWheel.Invalidate(FALSE);
	ColorValueSlider.Invalidate(FALSE);
	ColorOpacitySlider.Invalidate(FALSE);
}

std::vector<std::string> CProfileEditorDialog::SplitPathSegments(const std::string& path) const
{
	std::vector<std::string> segments;
	std::string current;
	current.reserve(path.size());

	for (char ch : path)
	{
		if (ch == '.')
		{
			if (!current.empty())
			{
				segments.push_back(current);
				current.clear();
			}
			continue;
		}
		current.push_back(ch);
	}
	if (!current.empty())
		segments.push_back(current);

	return segments;
}

void CProfileEditorDialog::RefreshEditorFieldsFromSelection()
{
	if (Owner == nullptr)
		return;

	const std::string selectedPath = Owner->GetSelectedProfileColorPathForEditor();
	if (!selectedPath.empty())
		SelectColorPathInTree(selectedPath);

	const std::string selectedDisplay = selectedPath.empty() ? "(none)" : FormatColorPathForDisplay(selectedPath);
	CString selectedLabel;
	selectedLabel.Format("Selected: %s", selectedDisplay.c_str());
	SelectedPathText.SetWindowTextA(selectedLabel);

	if (!DraftColorValid)
		LoadDraftColorFromSelection();
	UpdateDraftColorControls();
}

bool CProfileEditorDialog::TryParseRgbaQuad(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const
{
	std::vector<std::string> parts;
	parts.reserve(4);
	std::string current;
	bool sawComma = false;

	for (char ch : text)
	{
		if (std::isdigit(static_cast<unsigned char>(ch)))
		{
			current.push_back(ch);
			continue;
		}
		if (ch == ',')
		{
			parts.push_back(current);
			current.clear();
			sawComma = true;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(ch)))
			continue;
		return false;
	}

	if (!current.empty() || sawComma)
		parts.push_back(current);

	if (parts.size() != 3 && parts.size() != 4)
		return false;

	std::vector<int> values;
	values.reserve(parts.size());
	for (const std::string& part : parts)
	{
		if (part.empty())
			return false;
		int value = atoi(part.c_str());
		if (value < 0 || value > 255)
			return false;
		values.push_back(value);
	}

	r = values[0];
	g = values[1];
	b = values[2];
	a = values.size() >= 4 ? values[3] : 255;
	hasAlpha = (values.size() >= 4);
	return true;
}

bool CProfileEditorDialog::TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const
{
	std::string normalized;
	normalized.reserve(text.size());
	for (char c : text)
	{
		if (!std::isspace(static_cast<unsigned char>(c)))
			normalized.push_back(c);
	}

	if (normalized.empty())
		return false;
	if (normalized[0] == '#')
		normalized.erase(0, 1);
	if (normalized.size() != 6 && normalized.size() != 8)
		return false;

	auto parseHexByte = [&](size_t offset, int& outValue) -> bool
	{
		auto hexDigit = [](char ch) -> int
		{
			unsigned char c = static_cast<unsigned char>(ch);
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
			if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
			return -1;
		};

		if (offset + 1 >= normalized.size())
			return false;
		const int hi = hexDigit(normalized[offset]);
		const int lo = hexDigit(normalized[offset + 1]);
		if (hi < 0 || lo < 0)
			return false;
		outValue = (hi << 4) | lo;
		return true;
	};

	if (!parseHexByte(0, r) || !parseHexByte(2, g) || !parseHexByte(4, b))
		return false;

	a = 255;
	hasAlpha = false;
	if (normalized.size() == 8)
	{
		if (!parseHexByte(6, a))
			return false;
		hasAlpha = true;
	}
	return true;
}

double CProfileEditorDialog::ParseComboScaleSelection(CComboBox& combo, double fallback) const
{
	const int selected = combo.GetCurSel();
	if (selected == CB_ERR)
		return fallback;

	CString text;
	combo.GetLBText(selected, text);
	text.Trim();
	if (text.IsEmpty())
		return fallback;

	const char* raw = text.GetString();
	char* endPtr = nullptr;
	const double parsed = std::strtod(raw, &endPtr);
	if (endPtr == raw)
		return fallback;
	return parsed;
}

double CProfileEditorDialog::SliderPosToScale(int pos) const
{
	if (pos < 1)
		pos = 1;
	if (pos > 20)
		pos = 20;
	return static_cast<double>(pos) / 10.0;
}

int CProfileEditorDialog::ScaleToSliderPos(double scale) const
{
	int pos = static_cast<int>(std::lround(scale * 10.0));
	if (pos < 1)
		pos = 1;
	if (pos > 20)
		pos = 20;
	return pos;
}

void CProfileEditorDialog::UpdateIconScaleValueLabels()
{
	char textBuffer[24] = {};
	const double fixedScale = SliderPosToScale(FixedScaleSlider.GetPos());
	sprintf_s(textBuffer, sizeof(textBuffer), "%.2fx", fixedScale);
	FixedScaleValueLabel.SetWindowTextA(textBuffer);

	const double boostScale = SliderPosToScale(BoostFactorSlider.GetPos());
	sprintf_s(textBuffer, sizeof(textBuffer), "%.2fx", boostScale);
	BoostFactorValueLabel.SetWindowTextA(textBuffer);
}

void CProfileEditorDialog::SelectComboEntryByText(CComboBox& combo, const std::string& text)
{
	int index = combo.FindStringExact(-1, text.c_str());
	if (index == CB_ERR)
	{
		std::string loweredTarget = text;
		std::transform(loweredTarget.begin(), loweredTarget.end(), loweredTarget.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		for (int i = 0; i < combo.GetCount(); ++i)
		{
			CString itemText;
			combo.GetLBText(i, itemText);
			std::string loweredItem = itemText.GetString();
			std::transform(loweredItem.begin(), loweredItem.end(), loweredItem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (loweredItem == loweredTarget)
			{
				index = i;
				break;
			}
		}
	}
	if (index != CB_ERR)
		combo.SetCurSel(index);
}

void CProfileEditorDialog::SyncIconControlsFromRadar()
{
	if (Owner == nullptr)
		return;

	UpdatingControls = true;

	const std::string style = Owner->GetActiveTargetIconStyle();
	const bool triangleSelected = (style == "triangle");
	const bool diamondSelected = (style == "diamond");
	const bool realisticSelected = (!triangleSelected && !diamondSelected) || (style == "realistic");
	IconStyleArrow.SetCheck(triangleSelected ? BST_CHECKED : BST_UNCHECKED);
	IconStyleDiamond.SetCheck(diamondSelected ? BST_CHECKED : BST_UNCHECKED);
	IconStyleRealistic.SetCheck(realisticSelected ? BST_CHECKED : BST_UNCHECKED);

	FixedPixelCheck.SetCheck(Owner->GetFixedPixelTargetIconSizeEnabled() ? BST_CHECKED : BST_UNCHECKED);
	SmallBoostCheck.SetCheck(Owner->GetSmallTargetIconBoostEnabled() ? BST_CHECKED : BST_UNCHECKED);

	const double fixedScale = Owner->GetFixedPixelTriangleIconScale();
	int bestFixedIndex = 0;
	double bestFixedDiff = 1e9;
	for (int i = 0; i < FixedScaleCombo.GetCount(); ++i)
	{
		FixedScaleCombo.SetCurSel(i);
		const double value = ParseComboScaleSelection(FixedScaleCombo, fixedScale);
		const double diff = fabs(value - fixedScale);
		if (diff < bestFixedDiff)
		{
			bestFixedDiff = diff;
			bestFixedIndex = i;
		}
	}
	FixedScaleCombo.SetCurSel(bestFixedIndex);
	FixedScaleSlider.SetPos(ScaleToSliderPos(fixedScale));

	const double boostFactor = Owner->GetSmallTargetIconBoostFactor();
	int bestBoostIndex = 0;
	double bestBoostDiff = 1e9;
	for (int i = 0; i < BoostFactorCombo.GetCount(); ++i)
	{
		BoostFactorCombo.SetCurSel(i);
		const double value = ParseComboScaleSelection(BoostFactorCombo, boostFactor);
		const double diff = fabs(value - boostFactor);
		if (diff < bestBoostDiff)
		{
			bestBoostDiff = diff;
			bestBoostIndex = i;
		}
	}
	BoostFactorCombo.SetCurSel(bestBoostIndex);
	BoostFactorSlider.SetPos(ScaleToSliderPos(boostFactor));

	const std::string preset = Owner->GetSmallTargetIconBoostResolutionPreset();
	if (preset == "2k")
	{
		SelectComboEntryByText(BoostResolutionCombo, "2K");
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_CHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
	}
	else if (preset == "4k")
	{
		SelectComboEntryByText(BoostResolutionCombo, "4K");
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_CHECKED);
	}
	else
	{
		SelectComboEntryByText(BoostResolutionCombo, "1080p");
		BoostResolution1080Button.SetCheck(BST_CHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
	}

	UpdateIconScaleValueLabels();

	UpdatingControls = false;
}

void CProfileEditorDialog::RebuildRulesList()
{
	if (Owner == nullptr)
		return;

	RuleBuffer = Owner->GetStructuredTagColorRules();
	const int previousSelection = SelectedRuleIndex;
	const int previousCriterionSelection = SelectedRuleCriterionIndex;

	UpdatingControls = true;
	RuleTree.DeleteAllItems();
	RuleTreeSelectionMap.clear();
	for (size_t i = 0; i < RuleBuffer.size(); ++i)
	{
		const StructuredTagColorRule& rule = RuleBuffer[i];
		const std::string ruleName = CleanRuleDisplayName(rule.name, static_cast<int>(i + 1));
		CString ruleLabel;
		const int conditionCount = rule.criteria.empty() ? 1 : static_cast<int>(rule.criteria.size());
		ruleLabel.Format("  %s  (%d)", ruleName.c_str(), conditionCount);
		const HTREEITEM ruleItem = RuleTree.InsertItem(ruleLabel);
		RuleTreeSelectionMap[ruleItem] = std::make_pair(static_cast<int>(i), -1);

		std::vector<StructuredTagColorRule::Criterion> criteria = rule.criteria;
		if (criteria.empty())
			criteria.push_back(StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition });

		for (size_t c = 0; c < criteria.size(); ++c)
		{
			const StructuredTagColorRule::Criterion& criterion = criteria[c];
			CString criterionLabel;
			const std::string sourceLabel = RuleSourceUiLabel(criterion.source);
			criterionLabel.Format("-- %s.%s  =  %s", sourceLabel.c_str(), criterion.token.c_str(), criterion.condition.c_str());
			const HTREEITEM criterionItem = RuleTree.InsertItem(criterionLabel, ruleItem);
			RuleTreeSelectionMap[criterionItem] = std::make_pair(static_cast<int>(i), static_cast<int>(c));
		}
		RuleTree.Expand(ruleItem, TVE_EXPAND);
	}

	if (RuleBuffer.empty())
	{
		SelectedRuleIndex = -1;
		SelectedRuleCriterionIndex = -1;
	}
	else
	{
		SelectedRuleIndex = previousSelection;
		SelectedRuleCriterionIndex = previousCriterionSelection;
		if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
			SelectedRuleIndex = 0;
		if (SelectedRuleCriterionIndex >= static_cast<int>(RuleBuffer[SelectedRuleIndex].criteria.size()))
			SelectedRuleCriterionIndex = -1;
		SelectRuleNodeInTree(SelectedRuleIndex, SelectedRuleCriterionIndex);
	}
	UpdatingControls = false;
}

void CProfileEditorDialog::UpdateRulesListItemLabel(int index)
{
	(void)index;
	RebuildRulesList();
}

void CProfileEditorDialog::SelectRuleNodeInTree(int ruleIndex, int criterionIndex)
{
	if (!::IsWindow(RuleTree.GetSafeHwnd()))
		return;

	for (const auto& entry : RuleTreeSelectionMap)
	{
		if (entry.second.first == ruleIndex && entry.second.second == criterionIndex)
		{
			RuleTree.SelectItem(entry.first);
			return;
		}
	}

	for (const auto& entry : RuleTreeSelectionMap)
	{
		if (entry.second.first == ruleIndex && entry.second.second == -1)
		{
			RuleTree.SelectItem(entry.first);
			return;
		}
	}
}

bool CProfileEditorDialog::ResolveRuleSelectionFromTree(int& outRuleIndex, int& outCriterionIndex) const
{
	outRuleIndex = -1;
	outCriterionIndex = -1;
	if (!::IsWindow(RuleTree.GetSafeHwnd()))
		return false;

	const HTREEITEM selected = RuleTree.GetSelectedItem();
	if (selected == nullptr)
		return false;

	const auto it = RuleTreeSelectionMap.find(selected);
	if (it != RuleTreeSelectionMap.end())
	{
		outRuleIndex = it->second.first;
		outCriterionIndex = it->second.second;
		return true;
	}

	// Fallback: derive indices from current tree position when map lookup misses.
	auto siblingIndexOf = [&](HTREEITEM node) -> int
	{
		if (node == nullptr)
			return -1;
		int index = 0;
		for (HTREEITEM cur = node; cur != nullptr; cur = RuleTree.GetPrevSiblingItem(cur))
			++index;
		return index - 1;
	};

	const HTREEITEM parent = RuleTree.GetParentItem(selected);
	if (parent == nullptr)
	{
		outRuleIndex = siblingIndexOf(selected);
		outCriterionIndex = -1;
		return (outRuleIndex >= 0);
	}

	outRuleIndex = siblingIndexOf(parent);
	outCriterionIndex = siblingIndexOf(selected);
	return (outRuleIndex >= 0 && outCriterionIndex >= 0);
}

bool CProfileEditorDialog::GetRuleTreeActionRects(HTREEITEM item, CRect& addRect, CRect& deleteRect, bool& showAdd, bool& showDelete) const
{
	showAdd = false;
	showDelete = false;
	addRect.SetRectEmpty();
	deleteRect.SetRectEmpty();
	if (!::IsWindow(RuleTree.GetSafeHwnd()) || item == nullptr)
		return false;

	const auto it = RuleTreeSelectionMap.find(item);
	if (it == RuleTreeSelectionMap.end())
		return false;

	const bool isRuleNode = (it->second.second < 0);
	showAdd = isRuleNode;
	showDelete = false;

	CRect itemRect;
	if (!RuleTree.GetItemRect(item, &itemRect, TRUE))
		return false;

	CRect treeRect;
	RuleTree.GetClientRect(&treeRect);
	const int btnSize = 16;
	const int gap = 8;
	const int y = itemRect.top + max(0, ((itemRect.Height() - btnSize) / 2));
	int right = treeRect.right - 8;

	if (showAdd)
		addRect = CRect(right - btnSize, y, right, y + btnSize);
	right -= (btnSize + gap);
	deleteRect = CRect(right - btnSize, y, right, y + btnSize);

	return true;
}

void CProfileEditorDialog::InvalidateRuleColorSwatches()
{
	RuleTargetSwatch.Invalidate(FALSE);
	RuleTagSwatch.Invalidate(FALSE);
	RuleTextSwatch.Invalidate(FALSE);
	RuleColorPreviewSwatch.Invalidate(FALSE);
	RuleColorWheel.Invalidate(FALSE);
	RuleColorValueSlider.Invalidate(FALSE);
	RuleColorOpacitySlider.Invalidate(FALSE);
}

bool CProfileEditorDialog::ResolveRuleSwatchColor(UINT controlId, COLORREF& outColor, bool& outEnabled) const
{
	outColor = RGB(240, 240, 240);
	outEnabled = false;

	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return false;

	const StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
	switch (controlId)
	{
	case IDC_PE_RULE_TARGET_SWATCH:
		outEnabled = true;
		outColor = rule.applyTarget ? RGB(rule.targetR, rule.targetG, rule.targetB) : RGB(240, 240, 240);
		return true;
	case IDC_PE_RULE_TAG_SWATCH:
		outEnabled = true;
		outColor = rule.applyTag ? RGB(rule.tagR, rule.tagG, rule.tagB) : RGB(240, 240, 240);
		return true;
	case IDC_PE_RULE_TEXT_SWATCH:
		outEnabled = true;
		outColor = rule.applyText ? RGB(rule.textR, rule.textG, rule.textB) : RGB(240, 240, 240);
		return true;
	default:
		return false;
	}
}

bool CProfileEditorDialog::GetRuleColorEditorTargetControls(UINT swatchControlId, CButton*& outCheck, CEdit*& outEdit) const
{
	outCheck = nullptr;
	outEdit = nullptr;
	switch (swatchControlId)
	{
	case IDC_PE_RULE_TARGET_SWATCH:
		outCheck = const_cast<CButton*>(&RuleTargetCheck);
		outEdit = const_cast<CEdit*>(&RuleTargetEdit);
		return true;
	case IDC_PE_RULE_TAG_SWATCH:
		outCheck = const_cast<CButton*>(&RuleTagCheck);
		outEdit = const_cast<CEdit*>(&RuleTagEdit);
		return true;
	case IDC_PE_RULE_TEXT_SWATCH:
		outCheck = const_cast<CButton*>(&RuleTextCheck);
		outEdit = const_cast<CEdit*>(&RuleTextEdit);
		return true;
	default:
		return false;
	}
}

void CProfileEditorDialog::SyncRuleColorValueSliderFromDraft()
{
	if (!RuleColorDraftValid || !::IsWindow(RuleColorValueSlider.GetSafeHwnd()))
		return;

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
	const int position = min(100, max(0, static_cast<int>(round(value * 100.0))));

	UpdatingControls = true;
	RuleColorValueSlider.SetPos(position);
	UpdatingControls = false;
}

void CProfileEditorDialog::SyncRuleColorOpacitySliderFromDraft()
{
	if (!RuleColorDraftValid || !::IsWindow(RuleColorOpacitySlider.GetSafeHwnd()))
		return;

	const int alpha = min(255, max(0, RuleColorDraftA));
	const int position = min(100, max(0, static_cast<int>(round((static_cast<double>(alpha) / 255.0) * 100.0))));

	UpdatingControls = true;
	RuleColorOpacitySlider.SetPos(position);
	UpdatingControls = false;
}

void CProfileEditorDialog::SyncRuleColorEditorFromActiveControl()
{
	RuleColorDraftValid = false;
	RuleColorDraftDirty = false;
	RuleColorDraftHueSaturationValid = false;
	RuleColorApplyButton.EnableWindow(FALSE);
	RuleColorResetButton.EnableWindow((SelectedRuleIndex >= 0 && SelectedRuleIndex < static_cast<int>(RuleBuffer.size())) ? TRUE : FALSE);
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
	{
		InvalidateRuleColorSwatches();
		return;
	}

	if (RuleColorActiveSwatchId != IDC_PE_RULE_TARGET_SWATCH &&
		RuleColorActiveSwatchId != IDC_PE_RULE_TAG_SWATCH &&
		RuleColorActiveSwatchId != IDC_PE_RULE_TEXT_SWATCH)
	{
		RuleColorActiveSwatchId = IDC_PE_RULE_TARGET_SWATCH;
	}

	CButton* check = nullptr;
	CEdit* edit = nullptr;
	if (!GetRuleColorEditorTargetControls(RuleColorActiveSwatchId, check, edit) || check == nullptr || edit == nullptr)
		return;

	int r = 255;
	int g = 255;
	int b = 255;
	int a = 255;
	bool hasAlpha = false;
	CString colorText;
	edit->GetWindowText(colorText);
	if (!TryParseRgbaQuad(std::string(colorText.GetString()), r, g, b, a, hasAlpha))
	{
		const StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
		if (RuleColorActiveSwatchId == IDC_PE_RULE_TARGET_SWATCH)
		{
			r = rule.targetR; g = rule.targetG; b = rule.targetB; a = rule.targetA;
		}
		else if (RuleColorActiveSwatchId == IDC_PE_RULE_TAG_SWATCH)
		{
			r = rule.tagR; g = rule.tagG; b = rule.tagB; a = rule.tagA;
		}
		else
		{
			r = rule.textR; g = rule.textG; b = rule.textB; a = rule.textA;
		}
	}
	else if (!hasAlpha)
	{
		a = 255;
	}

	RuleColorDraftR = r;
	RuleColorDraftG = g;
	RuleColorDraftB = b;
	RuleColorDraftA = a;
	RuleColorDraftValid = true;
	RuleColorDraftDirty = false;
	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
	RuleColorDraftHue = hue;
	RuleColorDraftSaturation = saturation;
	RuleColorDraftHueSaturationValid = true;
	SyncRuleColorValueSliderFromDraft();
	SyncRuleColorOpacitySliderFromDraft();
	InvalidateRuleColorSwatches();
}

void CProfileEditorDialog::ApplyRuleColorValueFromSlider()
{
	if (UpdatingControls || !RuleColorDraftValid)
		return;
	if (!::IsWindow(RuleColorValueSlider.GetSafeHwnd()))
		return;

	double hue = RuleColorDraftHue;
	double saturation = RuleColorDraftSaturation;
	if (!RuleColorDraftHueSaturationValid)
	{
		double value = 1.0;
		RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
		RuleColorDraftHue = hue;
		RuleColorDraftSaturation = saturation;
		RuleColorDraftHueSaturationValid = true;
	}
	const double sliderValue = min(1.0, max(0.0, static_cast<double>(RuleColorValueSlider.GetPos()) / 100.0));

	int r = 255;
	int g = 255;
	int b = 255;
	HsvToRgb(hue, saturation, sliderValue, r, g, b);
	RuleColorDraftR = r;
	RuleColorDraftG = g;
	RuleColorDraftB = b;
	RuleColorDraftDirty = true;
	RuleColorApplyButton.EnableWindow(TRUE);
	InvalidateRuleColorSwatches();

	CButton* check = nullptr;
	CEdit* edit = nullptr;
	if (GetRuleColorEditorTargetControls(RuleColorActiveSwatchId, check, edit) && edit != nullptr)
	{
		UpdatingControls = true;
		SetEditTextPreserveCaret(*edit, FormatRgbaQuad(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, RuleColorDraftA));
		UpdatingControls = false;
	}
}

void CProfileEditorDialog::ApplyRuleColorOpacityFromSlider()
{
	if (UpdatingControls || !RuleColorDraftValid)
		return;
	if (!::IsWindow(RuleColorOpacitySlider.GetSafeHwnd()))
		return;

	const double sliderAlpha = min(1.0, max(0.0, static_cast<double>(RuleColorOpacitySlider.GetPos()) / 100.0));
	RuleColorDraftA = min(255, max(0, static_cast<int>(round(sliderAlpha * 255.0))));
	RuleColorDraftDirty = true;
	RuleColorApplyButton.EnableWindow(TRUE);
	InvalidateRuleColorSwatches();

	CButton* check = nullptr;
	CEdit* edit = nullptr;
	if (GetRuleColorEditorTargetControls(RuleColorActiveSwatchId, check, edit) && edit != nullptr)
	{
		UpdatingControls = true;
		SetEditTextPreserveCaret(*edit, FormatRgbaQuad(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, RuleColorDraftA));
		UpdatingControls = false;
	}
}

void CProfileEditorDialog::ApplyRuleColorDraftToActiveControl()
{
	if (UpdatingControls || !RuleColorDraftValid)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;

	CButton* check = nullptr;
	CEdit* edit = nullptr;
	if (!GetRuleColorEditorTargetControls(RuleColorActiveSwatchId, check, edit) || check == nullptr || edit == nullptr)
		return;

	UpdatingControls = true;
	check->SetCheck(BST_CHECKED);
	SetEditTextPreserveCaret(*edit, FormatRgbaQuad(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, RuleColorDraftA));
	UpdatingControls = false;

	OnRuleFieldChanged();
	RuleColorDraftDirty = false;
}

void CProfileEditorDialog::RefreshRuleControls()
{
	UpdatingControls = true;
	const bool hasSelection = (SelectedRuleIndex >= 0 && SelectedRuleIndex < static_cast<int>(RuleBuffer.size()));
	const bool isParameterSelection = hasSelection && SelectedRuleCriterionIndex >= 0;

	RuleAddParameterButton.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleRemoveButton.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleNameEdit.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleSourceCombo.EnableWindow(isParameterSelection ? TRUE : FALSE);
	RuleTokenCombo.EnableWindow(isParameterSelection ? TRUE : FALSE);
	RuleConditionCombo.EnableWindow(isParameterSelection ? TRUE : FALSE);
	RuleTypeCombo.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleStatusCombo.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleDetailCombo.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleTargetCheck.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleTagCheck.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleTextCheck.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleTargetSwatch.EnableWindow(FALSE);
	RuleTagSwatch.EnableWindow(FALSE);
	RuleTextSwatch.EnableWindow(FALSE);
	RuleTargetEdit.EnableWindow(FALSE);
	RuleTagEdit.EnableWindow(FALSE);
	RuleTextEdit.EnableWindow(FALSE);
	RuleColorWheel.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleColorValueSlider.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleColorOpacitySlider.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);
	RuleColorApplyButton.EnableWindow(FALSE);
	RuleColorResetButton.EnableWindow((hasSelection && !isParameterSelection) ? TRUE : FALSE);

	if (!hasSelection)
	{
		RuleRightHeader.SetWindowTextA("Rule Details");
		RuleRemoveButton.SetWindowTextA("Remove");
		SetEditTextPreserveCaret(RuleNameEdit, "");
		SelectComboEntryByText(RuleSourceCombo, "vacdm");
		PopulateRuleTokenCombo("vacdm", "tobt");
		PopulateRuleConditionCombo("vacdm", "tobt", "any");
		SelectComboEntryByText(RuleTypeCombo, RuleTypeUiLabel(Owner, "any"));
		PopulateRuleStatusCombo("any", "any");
		SelectComboEntryByText(RuleDetailCombo, "any");
		RuleTargetCheck.SetCheck(BST_UNCHECKED);
		RuleTagCheck.SetCheck(BST_UNCHECKED);
		RuleTextCheck.SetCheck(BST_UNCHECKED);
		SetEditTextPreserveCaret(RuleTargetEdit, "255,255,255,255");
		SetEditTextPreserveCaret(RuleTagEdit, "255,255,255,255");
		SetEditTextPreserveCaret(RuleTextEdit, "255,255,255,255");
		RuleColorDraftValid = false;
		RuleColorDraftDirty = false;
		RuleColorDraftHueSaturationValid = false;
		RuleColorDraftA = 255;
		RuleColorActiveSwatchId = IDC_PE_RULE_TARGET_SWATCH;
		RuleColorValueSlider.SetPos(100);
		RuleColorOpacitySlider.SetPos(100);
		RuleColorApplyButton.EnableWindow(FALSE);
		RuleColorResetButton.EnableWindow(FALSE);
		InvalidateRuleColorSwatches();
		UpdatingControls = false;
		return;
	}

	const StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
	RuleRemoveButton.SetWindowTextA(isParameterSelection ? "Del Param" : "Remove");
	if (isParameterSelection)
	{
		RuleRightHeader.SetWindowTextA("Parameter");
		const bool hasCriteria = !rule.criteria.empty();
		int criterionIndex = 0;
		if (hasCriteria)
		{
			criterionIndex = SelectedRuleCriterionIndex;
			const int maxIndex = static_cast<int>(rule.criteria.size()) - 1;
			if (criterionIndex > maxIndex)
				criterionIndex = maxIndex;
		}
		const StructuredTagColorRule::Criterion criterion =
			hasCriteria ? rule.criteria[criterionIndex] : StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition };

		SelectComboEntryByText(RuleSourceCombo, criterion.source);
		PopulateRuleTokenCombo(criterion.source, criterion.token);
		PopulateRuleConditionCombo(criterion.source, criterion.token, criterion.condition);

		RuleTargetCheck.SetCheck(BST_UNCHECKED);
		RuleTagCheck.SetCheck(BST_UNCHECKED);
		RuleTextCheck.SetCheck(BST_UNCHECKED);
		RuleTargetSwatch.EnableWindow(FALSE);
		RuleTagSwatch.EnableWindow(FALSE);
		RuleTextSwatch.EnableWindow(FALSE);
		RuleTargetEdit.EnableWindow(FALSE);
		RuleTagEdit.EnableWindow(FALSE);
		RuleTextEdit.EnableWindow(FALSE);
		RuleColorWheel.EnableWindow(FALSE);
		RuleColorValueSlider.EnableWindow(FALSE);
		RuleColorOpacitySlider.EnableWindow(FALSE);
		RuleColorApplyButton.EnableWindow(FALSE);
		RuleColorResetButton.EnableWindow(FALSE);
	}
	else
	{
		RuleRightHeader.SetWindowTextA("Rule Effects");
		SetEditTextPreserveCaret(RuleNameEdit, rule.name);
		const StructuredTagColorRule::Criterion primary =
			!rule.criteria.empty() ? rule.criteria.front() : StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition };
		SelectComboEntryByText(RuleSourceCombo, primary.source);
		PopulateRuleTokenCombo(primary.source, primary.token);
		PopulateRuleConditionCombo(primary.source, primary.token, SerializeRuleConditionText(rule));
		std::string ruleTypeForUi = rule.tagType;
		const std::string normalizedRuleType = NormalizeRuleTypeUiValue(Owner, rule.tagType);
		const std::string normalizedRuleStatus = NormalizeRuleStatusUiValue(Owner, rule.tagType, rule.status);
		if (normalizedRuleType == "airborne")
		{
			ruleTypeForUi = (normalizedRuleStatus == "airarr" || normalizedRuleStatus == "airarr_onrunway") ? "arrival" : "departure";
		}
		else if (normalizedRuleType == "uncorrelated")
		{
			ruleTypeForUi = "any";
		}
		SelectComboEntryByText(RuleTypeCombo, RuleTypeUiLabel(Owner, ruleTypeForUi));
		PopulateRuleStatusCombo(ruleTypeForUi, rule.status);
		SelectComboEntryByText(RuleDetailCombo, rule.detail);
		RuleTargetCheck.SetCheck(rule.applyTarget ? BST_CHECKED : BST_UNCHECKED);
		RuleTagCheck.SetCheck(rule.applyTag ? BST_CHECKED : BST_UNCHECKED);
		RuleTextCheck.SetCheck(rule.applyText ? BST_CHECKED : BST_UNCHECKED);
		SetEditTextPreserveCaret(RuleTargetEdit, FormatRgbaQuad(rule.targetR, rule.targetG, rule.targetB, rule.targetA));
		SetEditTextPreserveCaret(RuleTagEdit, FormatRgbaQuad(rule.tagR, rule.tagG, rule.tagB, rule.tagA));
		SetEditTextPreserveCaret(RuleTextEdit, FormatRgbaQuad(rule.textR, rule.textG, rule.textB, rule.textA));
		RuleTargetSwatch.EnableWindow(TRUE);
		RuleTagSwatch.EnableWindow(TRUE);
		RuleTextSwatch.EnableWindow(TRUE);
		RuleTargetEdit.EnableWindow(rule.applyTarget ? TRUE : FALSE);
		RuleTagEdit.EnableWindow(rule.applyTag ? TRUE : FALSE);
		RuleTextEdit.EnableWindow(rule.applyText ? TRUE : FALSE);
		RuleColorWheel.EnableWindow(TRUE);
		RuleColorValueSlider.EnableWindow(TRUE);
		RuleColorOpacitySlider.EnableWindow(TRUE);
		RuleColorApplyButton.EnableWindow(FALSE);
		RuleColorResetButton.EnableWindow(TRUE);
	}

	UpdatingControls = false;
	if (isParameterSelection)
	{
		RuleColorDraftValid = false;
		RuleColorDraftDirty = false;
		RuleColorDraftHueSaturationValid = false;
		RuleColorDraftA = 255;
	}
	else
	{
		SyncRuleColorEditorFromActiveControl();
	}
}

void CProfileEditorDialog::SyncTagEditorControlsFromRadar()
{
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: begin");
	if (Owner == nullptr)
	{
		LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: early return (owner null)");
		return;
	}
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before PopulateTagTokenCombo");
	PopulateTagTokenCombo();
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after PopulateTagTokenCombo");

	bool contextDetailed = false;
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before GetTagDefinitionEditorContext");
	Owner->GetTagDefinitionEditorContext(TagEditorType, contextDetailed, TagEditorStatus);
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after GetTagDefinitionEditorContext");
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before GetTagDefinitionDetailedSameAsDefinition(context)");
	TagEditorSeparateDetailed = !Owner->GetTagDefinitionDetailedSameAsDefinition(TagEditorType, TagEditorStatus);
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after GetTagDefinitionDetailedSameAsDefinition(context)");
	if (contextDetailed != TagEditorSeparateDetailed)
	{
		LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before SetTagDefinitionEditorContext(sync detailed)");
		Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
		LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after SetTagDefinitionEditorContext(sync detailed)");
	}

	UpdatingControls = true;
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before SelectComboEntryByText(TagTypeCombo)");
	SelectComboEntryByText(TagTypeCombo, TagTypeUiLabel(Owner, TagEditorType));
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after SelectComboEntryByText(TagTypeCombo)");
	TagAutoDeconflictionToggle.SetCheck(Owner->GetTagAutoDeconflictionEnabledForEditor() ? BST_CHECKED : BST_UNCHECKED);
	TagLinkDetailedToggle.SetCheck(TagEditorSeparateDetailed ? BST_CHECKED : BST_UNCHECKED);
	UpdatingControls = false;

	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before RefreshTagStatusOptions");
	RefreshTagStatusOptions();
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after RefreshTagStatusOptions");
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before RefreshTagDefinitionLines");
	RefreshTagDefinitionLines();
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after RefreshTagDefinitionLines");
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before RefreshTagPreview");
	RefreshTagPreview();
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after RefreshTagPreview");
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: before UpdatePageVisibility");
	UpdatePageVisibility();
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: after UpdatePageVisibility");
	LogProfileEditorInitStep("SyncTagEditorControlsFromRadar: end");
}

void CProfileEditorDialog::RefreshTagStatusOptions()
{
	if (Owner == nullptr)
		return;

	const std::string normalizedType = Owner->NormalizeTagDefinitionType(TagEditorType);
	const std::vector<std::string> statuses = Owner->GetTagDefinitionStatusesForType(TagEditorType);
	const std::string normalizedStatus = Owner->NormalizeTagDefinitionDepartureStatus(TagEditorStatus);

	UpdatingControls = true;
	TagStatusCombo.ResetContent();
	int selectedIndex = 0;
	for (size_t i = 0; i < statuses.size(); ++i)
	{
		const std::string label = TagStatusUiLabel(Owner, normalizedType, statuses[i]);
		TagStatusCombo.AddString(label.c_str());
		if (_stricmp(statuses[i].c_str(), normalizedStatus.c_str()) == 0 ||
			(_stricmp(statuses[i].c_str(), "default") == 0 &&
			 (normalizedStatus == "nsts" || normalizedStatus == "arr" || normalizedStatus == "taxi")))
		{
			selectedIndex = static_cast<int>(i);
		}
	}
	if (TagStatusCombo.GetCount() > 0)
		TagStatusCombo.SetCurSel(selectedIndex);
	UpdatingControls = false;

	if (TagStatusCombo.GetCount() > 0)
	{
		const int selected = TagStatusCombo.GetCurSel();
		if (selected != CB_ERR && selected >= 0 && selected < static_cast<int>(statuses.size()))
			TagEditorStatus = statuses[static_cast<size_t>(selected)];
		else
			TagEditorStatus = "default";
	}
	else
	{
		TagEditorStatus = "default";
	}
}

void CProfileEditorDialog::RefreshTagDefinitionLines()
{
	if (Owner == nullptr)
		return;

	const std::vector<std::string> lines = Owner->GetTagDefinitionLineStrings(
		TagEditorType,
		false,
		4,
		true,
		TagEditorStatus);
	const std::vector<std::string> detailedLines = Owner->GetTagDefinitionLineStrings(
		TagEditorType,
		true,
		4,
		true,
		TagEditorStatus);

	UpdatingControls = true;
	SetEditTextPreserveCaret(TagDefinitionEdit, JoinTagDefinitionLinesForEditor(lines));
	SetEditTextPreserveCaret(TagDetailedDefinitionEdit, JoinTagDefinitionLinesForEditor(detailedLines));
	UpdatingControls = false;
}

void CProfileEditorDialog::RefreshTagPreview()
{
	if (Owner == nullptr)
		return;

	LogProfileEditorInitStep("RefreshTagPreview: begin");
	LogProfileEditorInitStep("RefreshTagPreview: before SetTagDefinitionEditorContext");
	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	LogProfileEditorInitStep("RefreshTagPreview: after SetTagDefinitionEditorContext");
	LogProfileEditorInitStep("RefreshTagPreview: before BuildTagDefinitionPreviewLinesForContext(definition)");
	const std::vector<std::string> definitionPreviewLines = Owner->BuildTagDefinitionPreviewLinesForContext(
		TagEditorType,
		false,
		TagEditorStatus);
	LogProfileEditorInitStep("RefreshTagPreview: after BuildTagDefinitionPreviewLinesForContext(definition)");

	std::string previewText;
	previewText += "Definition";
	previewText += "\r\n";
	for (size_t i = 0; i < definitionPreviewLines.size(); ++i)
	{
		previewText += definitionPreviewLines[i];
		if (i + 1 < definitionPreviewLines.size())
			previewText += "\r\n";
	}

	if (TagEditorSeparateDetailed)
	{
		LogProfileEditorInitStep("RefreshTagPreview: before BuildTagDefinitionPreviewLinesForContext(detailed)");
		const std::vector<std::string> detailedPreviewLines = Owner->BuildTagDefinitionPreviewLinesForContext(
			TagEditorType,
			true,
			TagEditorStatus);
		LogProfileEditorInitStep("RefreshTagPreview: after BuildTagDefinitionPreviewLinesForContext(detailed)");
		previewText += "\r\n\r\nDefinition Detailed";
		previewText += "\r\n";
		for (size_t i = 0; i < detailedPreviewLines.size(); ++i)
		{
			previewText += detailedPreviewLines[i];
			if (i + 1 < detailedPreviewLines.size())
				previewText += "\r\n";
		}
	}

	LogProfileEditorInitStep("RefreshTagPreview: before SetWindowTextA");
	TagPreviewEdit.SetWindowTextA(previewText.c_str());
	LogProfileEditorInitStep("RefreshTagPreview: after SetWindowTextA");
	LogProfileEditorInitStep("RefreshTagPreview: end");
}

bool CProfileEditorDialog::TryParseRgbTriplet(const std::string& text, int& r, int& g, int& b) const
{
	std::vector<std::string> parts;
	parts.reserve(3);
	std::string current;
	bool sawComma = false;

	for (char ch : text)
	{
		if (std::isdigit(static_cast<unsigned char>(ch)))
		{
			current.push_back(ch);
			continue;
		}
		if (ch == ',')
		{
			parts.push_back(current);
			current.clear();
			sawComma = true;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(ch)))
			continue;
		return false;
	}

	if (!current.empty() || sawComma)
		parts.push_back(current);

	if (parts.size() != 3)
		return false;

	std::vector<int> values;
	values.reserve(3);
	for (const std::string& part : parts)
	{
		if (part.empty())
			return false;
		int value = atoi(part.c_str());
		if (value < 0 || value > 255)
			return false;
		values.push_back(value);
	}

	r = values[0];
	g = values[1];
	b = values[2];
	return true;
}

std::string CProfileEditorDialog::FormatRgbTriplet(int r, int g, int b) const
{
	char buffer[32] = {};
	sprintf_s(buffer, sizeof(buffer), "%d,%d,%d", r, g, b);
	return std::string(buffer);
}

std::string CProfileEditorDialog::FormatRgbaQuad(int r, int g, int b, int a) const
{
	char buffer[40] = {};
	sprintf_s(buffer, sizeof(buffer), "%d,%d,%d,%d", r, g, b, a);
	return std::string(buffer);
}

std::string CProfileEditorDialog::ReadComboText(CComboBox& combo) const
{
	const int index = combo.GetCurSel();
	if (index != CB_ERR)
	{
		CString selectedText;
		combo.GetLBText(index, selectedText);
		selectedText.Trim();
		if (!selectedText.IsEmpty())
			return std::string(selectedText.GetString());
	}

	CString text;
	combo.GetWindowText(text);
	text.Trim();
	if (!text.IsEmpty())
		return std::string(text.GetString());

	return "";
}

bool CProfileEditorDialog::ReadRuleFromControls(StructuredTagColorRule& outRule) const
{
	if (Owner == nullptr)
		return false;

	StructuredTagColorRule rule;
	if (SelectedRuleIndex >= 0 && SelectedRuleIndex < static_cast<int>(RuleBuffer.size()))
	{
		rule.criteria = RuleBuffer[SelectedRuleIndex].criteria;
	}
	if (rule.criteria.empty())
	{
		StructuredTagColorRule::Criterion defaultCriterion;
		defaultCriterion.source = Owner->NormalizeStructuredRuleSource(ReadComboText(const_cast<CComboBox&>(RuleSourceCombo)));
		defaultCriterion.token = Owner->NormalizeStructuredRuleToken(defaultCriterion.source, ReadComboText(const_cast<CComboBox&>(RuleTokenCombo)));
		if (defaultCriterion.token.empty())
			return false;
		defaultCriterion.condition = Owner->NormalizeStructuredRuleCondition(defaultCriterion.source, ReadComboText(const_cast<CComboBox&>(RuleConditionCombo)));
		rule.criteria.push_back(defaultCriterion);
	}
	rule.source = rule.criteria.front().source;
	rule.token = rule.criteria.front().token;
	rule.condition = rule.criteria.front().condition;
	CString ruleNameText;
	const_cast<CEdit&>(RuleNameEdit).GetWindowText(ruleNameText);
	rule.name = TrimAsciiWhitespaceCopy(std::string(ruleNameText.GetString()));
	const std::string selectedRuleTypeText = ReadComboText(const_cast<CComboBox&>(RuleTypeCombo));
	const std::string selectedRuleStatusText = ReadComboText(const_cast<CComboBox&>(RuleStatusCombo));
	rule.tagType = Owner->NormalizeStructuredRuleTagType(selectedRuleTypeText);
	rule.status = NormalizeRuleStatusUiValue(Owner, selectedRuleTypeText, selectedRuleStatusText);
	rule.detail = Owner->NormalizeStructuredRuleDetail(ReadComboText(const_cast<CComboBox&>(RuleDetailCombo)));

	rule.applyTarget = (RuleTargetCheck.GetCheck() == BST_CHECKED);
	rule.applyTag = (RuleTagCheck.GetCheck() == BST_CHECKED);
	rule.applyText = (RuleTextCheck.GetCheck() == BST_CHECKED);

	CString rgbaText;
	if (rule.applyTarget)
	{
		const_cast<CEdit&>(RuleTargetEdit).GetWindowText(rgbaText);
		bool hasAlpha = false;
		if (!TryParseRgbaQuad(std::string(rgbaText.GetString()), rule.targetR, rule.targetG, rule.targetB, rule.targetA, hasAlpha))
			return false;
	}
	if (rule.applyTag)
	{
		const_cast<CEdit&>(RuleTagEdit).GetWindowText(rgbaText);
		bool hasAlpha = false;
		if (!TryParseRgbaQuad(std::string(rgbaText.GetString()), rule.tagR, rule.tagG, rule.tagB, rule.tagA, hasAlpha))
			return false;
	}
	if (rule.applyText)
	{
		const_cast<CEdit&>(RuleTextEdit).GetWindowText(rgbaText);
		bool hasAlpha = false;
		if (!TryParseRgbaQuad(std::string(rgbaText.GetString()), rule.textR, rule.textG, rule.textB, rule.textA, hasAlpha))
			return false;
	}

	if (!rule.applyTarget && !rule.applyTag && !rule.applyText)
		return false;

	outRule = rule;
	return true;
}

bool CProfileEditorDialog::ReadRuleCriterionFromControls(StructuredTagColorRule::Criterion& outCriterion) const
{
	if (Owner == nullptr)
		return false;

	StructuredTagColorRule::Criterion criterion;
	criterion.source = Owner->NormalizeStructuredRuleSource(ReadComboText(const_cast<CComboBox&>(RuleSourceCombo)));
	criterion.token = Owner->NormalizeStructuredRuleToken(criterion.source, ReadComboText(const_cast<CComboBox&>(RuleTokenCombo)));
	if (criterion.token.empty())
		return false;
	criterion.condition = Owner->NormalizeStructuredRuleCondition(criterion.source, ReadComboText(const_cast<CComboBox&>(RuleConditionCombo)));
	outCriterion = criterion;
	return true;
}

void CProfileEditorDialog::ApplyRuleControlChanges(bool keepSelection)
{
	if (UpdatingControls || Owner == nullptr)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;
	if (SelectedRuleCriterionIndex >= 0)
		return;

	StructuredTagColorRule updatedRule;
	if (!ReadRuleFromControls(updatedRule))
		return;

	RuleBuffer[SelectedRuleIndex] = updatedRule;
	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	InvalidateRuleColorSwatches();
	if (keepSelection)
	{
		UpdateRulesListItemLabel(SelectedRuleIndex);
		SelectRuleNodeInTree(SelectedRuleIndex, -1);
		return;
	}

	const int preservedSelection = SelectedRuleIndex;
	SelectedRuleCriterionIndex = -1;
	RebuildRulesList();
	SelectedRuleIndex = preservedSelection;
	SelectRuleNodeInTree(SelectedRuleIndex, -1);
	RefreshRuleControls();
}

void CProfileEditorDialog::ApplyRuleCriterionControlChanges(bool keepSelection)
{
	if (UpdatingControls || Owner == nullptr)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;
	if (SelectedRuleCriterionIndex < 0)
		return;

	StructuredTagColorRule::Criterion updatedCriterion;
	if (!ReadRuleCriterionFromControls(updatedCriterion))
		return;

	StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
	if (rule.criteria.empty())
		rule.criteria.push_back(StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition });
	if (SelectedRuleCriterionIndex >= static_cast<int>(rule.criteria.size()))
		return;

	rule.criteria[SelectedRuleCriterionIndex] = updatedCriterion;
	rule.source = rule.criteria.front().source;
	rule.token = rule.criteria.front().token;
	rule.condition = rule.criteria.front().condition;

	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	if (keepSelection)
	{
		RebuildRulesList();
		SelectRuleNodeInTree(SelectedRuleIndex, SelectedRuleCriterionIndex);
		return;
	}

	RebuildRulesList();
	RefreshRuleControls();
}

void CProfileEditorDialog::OnRuleSelectionChanged()
{
	if (UpdatingControls)
		return;

	int selectedRule = -1;
	int selectedCriterion = -1;
	if (!ResolveRuleSelectionFromTree(selectedRule, selectedCriterion))
	{
		SelectedRuleIndex = -1;
		SelectedRuleCriterionIndex = -1;
	}
	else
	{
		SelectedRuleIndex = selectedRule;
		SelectedRuleCriterionIndex = selectedCriterion;
	}

	SetRedraw(FALSE);
	RefreshRuleControls();
	LayoutControls();
	UpdatePageVisibility();
	SetRedraw(TRUE);
	RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void CProfileEditorDialog::OnRuleTreeSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	OnRuleSelectionChanged();
	if (pResult != nullptr)
		*pResult = 0;
}

void CProfileEditorDialog::OnRuleTreeCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr || pNMHDR == nullptr)
		return;

	LPNMTVCUSTOMDRAW pTreeCd = reinterpret_cast<LPNMTVCUSTOMDRAW>(pNMHDR);
	if (pTreeCd == nullptr)
		return;

	switch (pTreeCd->nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		*pResult = CDRF_NOTIFYITEMDRAW;
		return;
	case CDDS_ITEMPREPAINT:
	{
		const HTREEITEM item = reinterpret_cast<HTREEITEM>(pTreeCd->nmcd.dwItemSpec);
		const auto selectionIt = RuleTreeSelectionMap.find(item);
		const bool isRuleNode = (selectionIt != RuleTreeSelectionMap.end() && selectionIt->second.second < 0);
		const bool selected = (::IsWindow(RuleTree.GetSafeHwnd()) && RuleTree.GetSelectedItem() == item);
		CRect itemRowRect;
		if (RuleTree.GetItemRect(item, &itemRowRect, FALSE))
		{
			CRect treeClientRect;
			RuleTree.GetClientRect(&treeClientRect);
			itemRowRect.left = max(2, itemRowRect.left - (isRuleNode ? 10 : 6));
			itemRowRect.right = max(itemRowRect.left + 8, treeClientRect.right - 2);
			itemRowRect.DeflateRect(0, 2);

			CDC* dc = CDC::FromHandle(pTreeCd->nmcd.hdc);
			if (dc != nullptr)
			{
				const COLORREF normalFill = isRuleNode ? RGB(235, 238, 242) : RGB(243, 245, 248);
				const COLORREF normalBorder = isRuleNode ? RGB(204, 210, 218) : RGB(214, 220, 228);
				CBrush rowBrush(selected ? RGB(226, 238, 255) : normalFill);
				CPen rowPen(PS_SOLID, 1, selected ? RGB(64, 132, 230) : normalBorder);
				CBrush* oldBrush = dc->SelectObject(&rowBrush);
				CPen* oldPen = dc->SelectObject(&rowPen);
				dc->RoundRect(&itemRowRect, CPoint(8, 8));
				dc->SelectObject(oldPen);
				dc->SelectObject(oldBrush);
			}
		}
		pTreeCd->clrText = selected ? RGB(16, 66, 132) : RGB(17, 24, 39);
		pTreeCd->clrTextBk = selected ? RGB(226, 238, 255) : (isRuleNode ? RGB(235, 238, 242) : RGB(243, 245, 248));
		*pResult = CDRF_NOTIFYPOSTPAINT;
		return;
	}
	case CDDS_ITEMPOSTPAINT:
	{
		CDC* dc = CDC::FromHandle(pTreeCd->nmcd.hdc);
		if (dc == nullptr)
		{
			*pResult = CDRF_DODEFAULT;
			return;
		}

		const HTREEITEM item = reinterpret_cast<HTREEITEM>(pTreeCd->nmcd.dwItemSpec);
		const auto selectionIt = RuleTreeSelectionMap.find(item);
		const bool isRuleNode = (selectionIt != RuleTreeSelectionMap.end() && selectionIt->second.second < 0);
		CRect itemRowRect;
		if (RuleTree.GetItemRect(item, &itemRowRect, FALSE))
		{
			CRect treeClientRect;
			RuleTree.GetClientRect(&treeClientRect);
			itemRowRect.left = max(2, itemRowRect.left - (isRuleNode ? 10 : 6));
			itemRowRect.right = max(itemRowRect.left + 8, treeClientRect.right - 2);
			itemRowRect.DeflateRect(0, 2);
			const bool selected = (RuleTree.GetSelectedItem() == item);
			CPen rowPen(PS_SOLID, 1, selected ? RGB(88, 137, 214) : (isRuleNode ? RGB(204, 210, 218) : RGB(214, 220, 228)));
			CBrush* oldBrush = static_cast<CBrush*>(dc->SelectStockObject(HOLLOW_BRUSH));
			CPen* oldPen = dc->SelectObject(&rowPen);
			dc->RoundRect(&itemRowRect, CPoint(8, 8));
			dc->SelectObject(oldPen);
			dc->SelectObject(oldBrush);
		}

		CRect addRect;
		CRect deleteRect;
		bool showAdd = false;
		bool showDelete = false;
		if (!GetRuleTreeActionRects(item, addRect, deleteRect, showAdd, showDelete))
		{
			*pResult = CDRF_DODEFAULT;
			return;
		}

		auto drawActionButton = [&](const CRect& rect, const char* text)
		{
			if (rect.IsRectEmpty())
				return;
			CRect buttonRect(rect);
			CBrush fillBrush(RGB(247, 248, 250));
			CPen borderPen(PS_SOLID, 1, RGB(171, 180, 190));
			CBrush* oldBrush = dc->SelectObject(&fillBrush);
			CPen* oldPen = dc->SelectObject(&borderPen);
			dc->RoundRect(&buttonRect, CPoint(6, 6));
			dc->SelectObject(oldPen);
			dc->SelectObject(oldBrush);
			dc->SetBkMode(TRANSPARENT);
			dc->SetTextColor(RGB(17, 24, 39));
			dc->DrawTextA(text, &buttonRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		};

		if (showAdd)
			drawActionButton(addRect, "+");
		*pResult = CDRF_DODEFAULT;
		return;
	}
	default:
		*pResult = CDRF_DODEFAULT;
		return;
	}
}

void CProfileEditorDialog::OnRuleTreeClick(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	if (pResult != nullptr)
		*pResult = 0;
	if (!::IsWindow(RuleTree.GetSafeHwnd()))
		return;

	DWORD messagePos = GetMessagePos();
	CPoint clickPoint(GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos));
	RuleTree.ScreenToClient(&clickPoint);

	UINT hitFlags = 0;
	HTREEITEM item = RuleTree.HitTest(clickPoint, &hitFlags);
	if (item == nullptr)
		return;

	CRect addRect;
	CRect deleteRect;
	bool showAdd = false;
	bool showDelete = false;
	if (!GetRuleTreeActionRects(item, addRect, deleteRect, showAdd, showDelete))
		return;

	if (showAdd && addRect.PtInRect(clickPoint))
	{
		const auto it = RuleTreeSelectionMap.find(item);
		if (it != RuleTreeSelectionMap.end())
		{
			SelectedRuleIndex = it->second.first;
			SelectedRuleCriterionIndex = it->second.second;
			OnRuleAddParameterClicked();
		}
		return;
	}

	RuleTree.SelectItem(item);
}

void CProfileEditorDialog::OnRuleAddClicked()
{
	if (Owner == nullptr)
		return;

	StructuredTagColorRule newRule;
	newRule.name = "Rule " + std::to_string(static_cast<int>(RuleBuffer.size() + 1));
	newRule.source = "vacdm";
	newRule.token = "tobt";
	newRule.condition = "any";
	newRule.tagType = "any";
	newRule.status = "any";
	newRule.detail = "any";
	newRule.applyTag = true;
	newRule.tagR = 0;
	newRule.tagG = 170;
	newRule.tagB = 0;

	RuleBuffer.push_back(newRule);
	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	SelectedRuleIndex = static_cast<int>(RuleBuffer.size()) - 1;
	SelectedRuleCriterionIndex = -1;
	RebuildRulesList();
	SelectRuleNodeInTree(SelectedRuleIndex, -1);
	RefreshRuleControls();
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnRuleAddParameterClicked()
{
	if (Owner == nullptr)
		return;

	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
	{
		OnRuleAddClicked();
		SelectedRuleCriterionIndex = 0;
		RebuildRulesList();
		SelectRuleNodeInTree(SelectedRuleIndex, SelectedRuleCriterionIndex);
		RefreshRuleControls();
		UpdatePageVisibility();
		return;
	}

	StructuredTagColorRule::Criterion criterion;
	if (!ReadRuleCriterionFromControls(criterion))
	{
		criterion.source = "vacdm";
		criterion.token = "tsat";
		criterion.condition = "any";
	}

	StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
	if (rule.criteria.empty())
		rule.criteria.push_back(StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition });

	int insertIndex = static_cast<int>(rule.criteria.size());
	if (SelectedRuleCriterionIndex >= 0 && SelectedRuleCriterionIndex < static_cast<int>(rule.criteria.size()))
		insertIndex = SelectedRuleCriterionIndex + 1;
	rule.criteria.insert(rule.criteria.begin() + insertIndex, criterion);
	rule.source = rule.criteria.front().source;
	rule.token = rule.criteria.front().token;
	rule.condition = rule.criteria.front().condition;

	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	SelectedRuleCriterionIndex = insertIndex;
	RebuildRulesList();
	SelectRuleNodeInTree(SelectedRuleIndex, SelectedRuleCriterionIndex);
	RefreshRuleControls();
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnRuleRemoveClicked()
{
	if (Owner == nullptr)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;

	if (SelectedRuleCriterionIndex >= 0)
	{
		StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
		if (rule.criteria.empty())
			rule.criteria.push_back(StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition });

		if (SelectedRuleCriterionIndex < static_cast<int>(rule.criteria.size()))
		{
			rule.criteria.erase(rule.criteria.begin() + SelectedRuleCriterionIndex);
			if (rule.criteria.empty())
			{
				RuleBuffer.erase(RuleBuffer.begin() + SelectedRuleIndex);
				SelectedRuleCriterionIndex = -1;
			}
			else
			{
				if (SelectedRuleCriterionIndex >= static_cast<int>(rule.criteria.size()))
					SelectedRuleCriterionIndex = static_cast<int>(rule.criteria.size()) - 1;
				rule.source = rule.criteria.front().source;
				rule.token = rule.criteria.front().token;
				rule.condition = rule.criteria.front().condition;
			}
		}
	}
	else
	{
		RuleBuffer.erase(RuleBuffer.begin() + SelectedRuleIndex);
	}

	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	if (SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		SelectedRuleIndex = static_cast<int>(RuleBuffer.size()) - 1;
	if (SelectedRuleIndex < 0)
		SelectedRuleCriterionIndex = -1;
	RebuildRulesList();
	SelectRuleNodeInTree(SelectedRuleIndex, SelectedRuleCriterionIndex);
	RefreshRuleControls();
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnRuleSourceChanged()
{
	if (UpdatingControls)
		return;
	if (SelectedRuleCriterionIndex < 0)
		return;

	const std::string sourceText = ReadComboText(RuleSourceCombo);
	if (sourceText.empty())
		return;

	UpdatingControls = true;
	PopulateRuleTokenCombo(sourceText, "");
	const std::string tokenText = ReadComboText(RuleTokenCombo);
	PopulateRuleConditionCombo(sourceText, tokenText, "any");
	UpdatingControls = false;
	ApplyRuleCriterionControlChanges(true);
}

void CProfileEditorDialog::OnRuleNameChanged()
{
	if (UpdatingControls)
		return;
	if (SelectedRuleCriterionIndex >= 0)
		return;
	ApplyRuleControlChanges(true);
}

void CProfileEditorDialog::OnRuleFieldChanged()
{
	if (UpdatingControls)
		return;
	const bool isParameterSelection = (SelectedRuleCriterionIndex >= 0);

	int focusedId = 0;
	if (CWnd* focused = GetFocus())
		focusedId = focused->GetDlgCtrlID();

	if (!isParameterSelection)
	{
		if (focusedId == IDC_PE_RULE_TARGET_EDIT || focusedId == IDC_PE_RULE_TARGET_CHECK || focusedId == IDC_PE_RULE_TARGET_SWATCH)
			RuleColorActiveSwatchId = IDC_PE_RULE_TARGET_SWATCH;
		else if (focusedId == IDC_PE_RULE_TAG_EDIT || focusedId == IDC_PE_RULE_TAG_CHECK || focusedId == IDC_PE_RULE_TAG_SWATCH)
			RuleColorActiveSwatchId = IDC_PE_RULE_TAG_SWATCH;
		else if (focusedId == IDC_PE_RULE_TEXT_EDIT || focusedId == IDC_PE_RULE_TEXT_CHECK || focusedId == IDC_PE_RULE_TEXT_SWATCH)
			RuleColorActiveSwatchId = IDC_PE_RULE_TEXT_SWATCH;
	}

	const bool isRuleColorTextEditChange =
		!isParameterSelection &&
		(focusedId == IDC_PE_RULE_TARGET_EDIT ||
		 focusedId == IDC_PE_RULE_TAG_EDIT ||
		 focusedId == IDC_PE_RULE_TEXT_EDIT);

	if (isRuleColorTextEditChange)
	{
		CEdit* activeEdit = nullptr;
		if (focusedId == IDC_PE_RULE_TARGET_EDIT)
			activeEdit = &RuleTargetEdit;
		else if (focusedId == IDC_PE_RULE_TAG_EDIT)
			activeEdit = &RuleTagEdit;
		else if (focusedId == IDC_PE_RULE_TEXT_EDIT)
			activeEdit = &RuleTextEdit;

		if (activeEdit != nullptr)
		{
			CString rgbaText;
			activeEdit->GetWindowText(rgbaText);
			int r = 255;
			int g = 255;
			int b = 255;
			int a = 255;
			bool hasAlpha = false;
			if (TryParseRgbaQuad(std::string(rgbaText.GetString()), r, g, b, a, hasAlpha))
			{
				RuleColorDraftR = r;
				RuleColorDraftG = g;
				RuleColorDraftB = b;
				RuleColorDraftA = hasAlpha ? a : 255;
				RuleColorDraftValid = true;
				RuleColorDraftDirty = true;
				double hue = 0.0;
				double saturation = 0.0;
				double value = 1.0;
				RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
				RuleColorDraftHue = hue;
				RuleColorDraftSaturation = saturation;
				RuleColorDraftHueSaturationValid = true;
				SyncRuleColorValueSliderFromDraft();
				SyncRuleColorOpacitySliderFromDraft();
				RuleColorApplyButton.EnableWindow(TRUE);
				InvalidateRuleColorSwatches();
			}
		}
		return;
	}

	const bool shouldRefreshConditionChoices =
		(focusedId == IDC_PE_RULE_SOURCE_COMBO) ||
		(focusedId == IDC_PE_RULE_TOKEN_COMBO);

	if (shouldRefreshConditionChoices)
	{
		const std::string sourceText = ReadComboText(RuleSourceCombo);
		const std::string tokenText = ReadComboText(RuleTokenCombo);
		const std::string selectedCondition = ReadComboText(RuleConditionCombo);
		UpdatingControls = true;
		PopulateRuleConditionCombo(sourceText, tokenText, selectedCondition);
		UpdatingControls = false;
	}

	const bool shouldRefreshStatusChoices =
		!isParameterSelection &&
		(focusedId == IDC_PE_RULE_TYPE_COMBO);

	if (shouldRefreshStatusChoices)
	{
		const std::string selectedType = ReadComboText(RuleTypeCombo);
		const std::string selectedStatus = ReadComboText(RuleStatusCombo);
		UpdatingControls = true;
		PopulateRuleStatusCombo(selectedType, selectedStatus);
		UpdatingControls = false;
	}

	if (isParameterSelection)
	{
		ApplyRuleCriterionControlChanges(true);
		return;
	}

	const BOOL targetEnabled = (RuleTargetCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	const BOOL tagEnabled = (RuleTagCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	const BOOL textEnabled = (RuleTextCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	RuleTargetSwatch.EnableWindow(TRUE);
	RuleTagSwatch.EnableWindow(TRUE);
	RuleTextSwatch.EnableWindow(TRUE);
	RuleTargetEdit.EnableWindow(targetEnabled);
	RuleTagEdit.EnableWindow(tagEnabled);
	RuleTextEdit.EnableWindow(textEnabled);
	InvalidateRuleColorSwatches();
	ApplyRuleControlChanges(true);
	SyncRuleColorEditorFromActiveControl();
}

void CProfileEditorDialog::OnProfileSelectionChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const std::string selectedName = GetSelectedProfileName();
	if (selectedName.empty())
	{
		RefreshProfileControls();
		return;
	}

	SetEditTextPreserveCaret(ProfileNameEdit, selectedName);
	if (!Owner->SetActiveProfileForEditor(selectedName, false))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileAddClicked()
{
	if (Owner == nullptr)
		return;

	CString requestedNameText;
	ProfileNameEdit.GetWindowText(requestedNameText);
	std::string requestedName = TrimAsciiWhitespaceCopy(std::string(requestedNameText.GetString()));
	if (requestedName.empty())
		requestedName = "Profile";

	std::string createdName;
	if (!Owner->AddProfileForEditor(requestedName, false, &createdName))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileDuplicateClicked()
{
	if (Owner == nullptr)
		return;

	CString requestedNameText;
	ProfileNameEdit.GetWindowText(requestedNameText);
	std::string requestedName = TrimAsciiWhitespaceCopy(std::string(requestedNameText.GetString()));
	if (requestedName.empty())
	{
		const std::string currentName = GetSelectedProfileName();
		requestedName = currentName.empty() ? "Profile Copy" : (currentName + " Copy");
	}

	std::string createdName;
	if (!Owner->AddProfileForEditor(requestedName, true, &createdName))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileRenameClicked()
{
	if (Owner == nullptr)
		return;

	const std::string oldName = GetSelectedProfileName();
	if (oldName.empty())
		return;

	CString newNameText;
	ProfileNameEdit.GetWindowText(newNameText);
	const std::string newName = TrimAsciiWhitespaceCopy(std::string(newNameText.GetString()));
	if (newName.empty())
		return;

	if (!Owner->RenameProfileForEditor(oldName, newName))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileDeleteClicked()
{
	if (Owner == nullptr)
		return;

	const std::string selectedName = GetSelectedProfileName();
	if (selectedName.empty())
		return;

	CString confirmText;
	confirmText.Format("Delete profile '%s'?", selectedName.c_str());
	if (AfxMessageBox(confirmText, MB_ICONQUESTION | MB_YESNO) != IDYES)
		return;

	if (!Owner->DeleteProfileForEditor(selectedName))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileProModeToggled()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const std::string selectedName = GetSelectedProfileName();
	if (selectedName.empty())
		return;

	const bool enabled = (ProfileProModeCheck.GetCheck() == BST_CHECKED);
	if (!Owner->SetProfileProModeEnabledForEditor(selectedName, enabled))
		return;

	SyncFromRadar();
	PageTabs.SetCurSel(kTabProfile);
	UpdatePageVisibility();
}

void CProfileEditorDialog::OnProfileRepoLinkClicked()
{
	const HINSTANCE result = ::ShellExecuteA(
		GetSafeHwnd(),
		"open",
		"https://github.com/IWantPizzaa/vSMR",
		nullptr,
		nullptr,
		SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(result) <= 32)
		AfxMessageBox("Unable to open the repository link.", MB_ICONWARNING | MB_OK);
}

void CProfileEditorDialog::OnProfileCoffeeLinkClicked()
{
	const HINSTANCE result = ::ShellExecuteA(
		GetSafeHwnd(),
		"open",
		"https://buymeacoffee.com/i_want_pizzaa",
		nullptr,
		nullptr,
		SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(result) <= 32)
		AfxMessageBox("Unable to open the support link.", MB_ICONWARNING | MB_OK);
}

void CProfileEditorDialog::SelectRuleColorEditorTarget(UINT swatchControlId)
{
	CButton* checkBox = nullptr;
	CEdit* edit = nullptr;
	if (!GetRuleColorEditorTargetControls(swatchControlId, checkBox, edit) || checkBox == nullptr || edit == nullptr)
		return;

	const UINT previousSwatchId = RuleColorActiveSwatchId;
	if (RuleColorDraftDirty &&
		previousSwatchId != swatchControlId &&
		SelectedRuleIndex >= 0 &&
		SelectedRuleIndex < static_cast<int>(RuleBuffer.size()))
	{
		const StructuredTagColorRule& savedRule = RuleBuffer[SelectedRuleIndex];
		CButton* previousCheck = nullptr;
		CEdit* previousEdit = nullptr;
		if (GetRuleColorEditorTargetControls(previousSwatchId, previousCheck, previousEdit) && previousEdit != nullptr)
		{
			bool applySaved = false;
			int savedR = 255;
			int savedG = 255;
			int savedB = 255;
			int savedA = 255;
			switch (previousSwatchId)
			{
			case IDC_PE_RULE_TARGET_SWATCH:
				applySaved = savedRule.applyTarget;
				savedR = savedRule.targetR;
				savedG = savedRule.targetG;
				savedB = savedRule.targetB;
				savedA = savedRule.targetA;
				break;
			case IDC_PE_RULE_TAG_SWATCH:
				applySaved = savedRule.applyTag;
				savedR = savedRule.tagR;
				savedG = savedRule.tagG;
				savedB = savedRule.tagB;
				savedA = savedRule.tagA;
				break;
			case IDC_PE_RULE_TEXT_SWATCH:
				applySaved = savedRule.applyText;
				savedR = savedRule.textR;
				savedG = savedRule.textG;
				savedB = savedRule.textB;
				savedA = savedRule.textA;
				break;
			default:
				break;
			}

			UpdatingControls = true;
			if (previousCheck != nullptr)
				previousCheck->SetCheck(applySaved ? BST_CHECKED : BST_UNCHECKED);
			SetEditTextPreserveCaret(*previousEdit, FormatRgbaQuad(savedR, savedG, savedB, savedA));
			UpdatingControls = false;
		}

		RuleColorDraftDirty = false;
		RuleColorApplyButton.EnableWindow(FALSE);
	}

	RuleColorActiveSwatchId = swatchControlId;
	UpdatingControls = true;
	checkBox->SetCheck(BST_CHECKED);
	UpdatingControls = false;

	// Keep focus aligned with the selected target so OnRuleFieldChanged does not
	// immediately infer the previous edit as the active swatch.
	if (::IsWindow(checkBox->GetSafeHwnd()) && checkBox->IsWindowEnabled())
		checkBox->SetFocus();

	OnRuleFieldChanged();
	SyncRuleColorEditorFromActiveControl();
}

void CProfileEditorDialog::OnRuleTargetSwatchClicked()
{
	SelectRuleColorEditorTarget(IDC_PE_RULE_TARGET_SWATCH);
}

void CProfileEditorDialog::OnRuleTagSwatchClicked()
{
	SelectRuleColorEditorTarget(IDC_PE_RULE_TAG_SWATCH);
}

void CProfileEditorDialog::OnRuleTextSwatchClicked()
{
	SelectRuleColorEditorTarget(IDC_PE_RULE_TEXT_SWATCH);
}

void CProfileEditorDialog::OnRuleColorApplyClicked()
{
	if (!RuleColorDraftValid || SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;
	ApplyRuleColorDraftToActiveControl();
}

void CProfileEditorDialog::OnRuleColorResetClicked()
{
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;

	// Reset must restore the editor from the saved rule state, not from the
	// current draft text/value controls.
	RefreshRuleControls();
	RuleColorApplyButton.EnableWindow(FALSE);
}

void CProfileEditorDialog::OnTagTypeChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selected = TagTypeCombo.GetCurSel();
	if (selected == CB_ERR)
		return;

	CString selectedType;
	TagTypeCombo.GetLBText(selected, selectedType);
	TagEditorType = Owner->NormalizeTagDefinitionType(std::string(selectedType.GetString()));
	RefreshTagStatusOptions();
	TagEditorSeparateDetailed = !Owner->GetTagDefinitionDetailedSameAsDefinition(TagEditorType, TagEditorStatus);
	UpdatingControls = true;
	TagLinkDetailedToggle.SetCheck(TagEditorSeparateDetailed ? BST_CHECKED : BST_UNCHECKED);
	UpdatingControls = false;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	UpdatePageVisibility();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagLinkToggleChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	TagEditorSeparateDetailed = (TagLinkDetailedToggle.GetCheck() == BST_CHECKED);
	Owner->SetTagDefinitionDetailedSameAsDefinition(TagEditorType, TagEditorStatus, !TagEditorSeparateDetailed, true);

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	UpdatePageVisibility();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagAutoDeconflictionToggled()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const bool enabled = (TagAutoDeconflictionToggle.GetCheck() == BST_CHECKED);
	if (!Owner->SetTagAutoDeconflictionEnabledForEditor(enabled, true))
	{
		UpdatingControls = true;
		TagAutoDeconflictionToggle.SetCheck(Owner->GetTagAutoDeconflictionEnabledForEditor() ? BST_CHECKED : BST_UNCHECKED);
		UpdatingControls = false;
	}
}

void CProfileEditorDialog::OnTagStatusChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selected = TagStatusCombo.GetCurSel();
	if (selected == CB_ERR)
		return;

	const std::vector<std::string> statuses = Owner->GetTagDefinitionStatusesForType(TagEditorType);
	if (selected < 0 || selected >= static_cast<int>(statuses.size()))
		return;

	TagEditorStatus = statuses[static_cast<size_t>(selected)];
	TagEditorSeparateDetailed = !Owner->GetTagDefinitionDetailedSameAsDefinition(TagEditorType, TagEditorStatus);
	UpdatingControls = true;
	TagLinkDetailedToggle.SetCheck(TagEditorSeparateDetailed ? BST_CHECKED : BST_UNCHECKED);
	UpdatingControls = false;
	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	UpdatePageVisibility();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagLineChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	CWnd* focused = GetFocus();
	if (focused == nullptr)
		return;

	bool editingDetailed = false;
	CEdit* sourceEdit = nullptr;
	switch (focused->GetDlgCtrlID())
	{
	case IDC_PE_TAG_DEFINITION_EDIT:
		sourceEdit = &TagDefinitionEdit;
		break;
	case IDC_PE_TAG_DETAILED_EDIT:
		editingDetailed = true;
		sourceEdit = &TagDetailedDefinitionEdit;
		break;
	default:
		return;
	}
	TagEditorSelectedLine = min(3, max(0, static_cast<int>(sourceEdit->LineFromChar(-1))));
	TagEditorSelectedLineDetailed = editingDetailed;

	if (editingDetailed && !TagEditorSeparateDetailed)
		return;

	CString definitionText;
	sourceEdit->GetWindowText(definitionText);
	const std::vector<std::string> newLines = SplitTagDefinitionEditorText(definitionText.GetString(), 4);

	std::vector<std::string> existingLines = Owner->GetTagDefinitionLineStrings(
		TagEditorType,
		editingDetailed,
		4,
		true,
		TagEditorStatus);
	while (existingLines.size() < 4)
		existingLines.push_back("");

	if (existingLines == newLines)
		return;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	for (int i = 0; i < 4; ++i)
	{
		Owner->SetTagDefinitionLineString(
			TagEditorType,
			editingDetailed,
			i,
			newLines[i],
			TagEditorStatus);
	}
	RefreshTagPreview();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagLineFocus()
{
	if (UpdatingControls)
		return;

	CWnd* focused = GetFocus();
	if (focused == nullptr)
		return;

	switch (focused->GetDlgCtrlID())
	{
	case IDC_PE_TAG_DEFINITION_EDIT:
		TagEditorSelectedLine = min(3, max(0, static_cast<int>(TagDefinitionEdit.LineFromChar(-1))));
		TagEditorSelectedLineDetailed = false;
		break;
	case IDC_PE_TAG_DETAILED_EDIT:
		TagEditorSelectedLine = min(3, max(0, static_cast<int>(TagDetailedDefinitionEdit.LineFromChar(-1))));
		TagEditorSelectedLineDetailed = true;
		break;
	default:
		break;
	}
}

void CProfileEditorDialog::OnTagAddTokenClicked()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selectedTokenIndex = TagTokenCombo.GetCurSel();
	if (selectedTokenIndex == CB_ERR)
		return;

	CString tokenText;
	TagTokenCombo.GetLBText(selectedTokenIndex, tokenText);
	const std::string token = tokenText.GetString();
	if (token.empty())
		return;

	bool detailedLine = TagEditorSelectedLineDetailed;
	CWnd* focused = GetFocus();
	CEdit* targetEdit = nullptr;
	if (focused != nullptr)
	{
		switch (focused->GetDlgCtrlID())
		{
		case IDC_PE_TAG_DEFINITION_EDIT:
			targetEdit = &TagDefinitionEdit;
			TagEditorSelectedLineDetailed = false;
			detailedLine = false;
			break;
		case IDC_PE_TAG_DETAILED_EDIT:
			targetEdit = &TagDetailedDefinitionEdit;
			TagEditorSelectedLineDetailed = true;
			detailedLine = true;
			break;
		default:
			break;
		}
	}

	if (!TagEditorSeparateDetailed)
		detailedLine = false;

	if (targetEdit == nullptr)
		targetEdit = detailedLine ? &TagDetailedDefinitionEdit : &TagDefinitionEdit;

	if (targetEdit == nullptr)
		return;

	CString currentText;
	targetEdit->GetWindowText(currentText);
	std::string updatedText = currentText.GetString();
	int selStart = 0;
	int selEnd = 0;
	targetEdit->GetSel(selStart, selEnd);
	selStart = max(0, selStart);
	selEnd = max(selStart, selEnd);
	const auto needsSpacing = [](char ch) -> bool
	{
		return ch != '\r' && ch != '\n' && std::isspace(static_cast<unsigned char>(ch)) == 0;
	};
	std::string insertText = token;
	if (selStart > 0 && selStart <= static_cast<int>(updatedText.size()) && needsSpacing(updatedText[selStart - 1]))
		insertText = " " + insertText;
	if (selEnd < static_cast<int>(updatedText.size()) && needsSpacing(updatedText[selEnd]))
		insertText += " ";
	updatedText.replace(static_cast<size_t>(selStart), static_cast<size_t>(selEnd - selStart), insertText);

	UpdatingControls = true;
	targetEdit->SetWindowTextA(updatedText.c_str());
	const int newCaret = selStart + static_cast<int>(insertText.size());
	targetEdit->SetSel(newCaret, newCaret);
	UpdatingControls = false;
	TagEditorSelectedLine = min(3, max(0, static_cast<int>(targetEdit->LineFromChar(-1))));
	TagEditorSelectedLineDetailed = detailedLine;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	const std::vector<std::string> updatedLines = SplitTagDefinitionEditorText(updatedText, 4);
	for (int i = 0; i < 4; ++i)
	{
		Owner->SetTagDefinitionLineString(
			TagEditorType,
			detailedLine,
			i,
			updatedLines[i],
			TagEditorStatus);
	}
	RefreshTagPreview();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnColorTreeSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	if (pResult != nullptr)
		*pResult = 0;
	if (UpdatingControls || Owner == nullptr)
		return;

	const std::string selectedPath = GetSelectedTreePath();
	if (selectedPath.empty())
		return;

	if (Owner->SelectProfileColorPathForEditor(selectedPath))
	{
		LoadDraftColorFromSelection();
		RefreshEditorFieldsFromSelection();
	}
}

void CProfileEditorDialog::OnColorTreeCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMTVCUSTOMDRAW* customDraw = reinterpret_cast<NMTVCUSTOMDRAW*>(pNMHDR);
	switch (customDraw->nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		*pResult = CDRF_NOTIFYITEMDRAW;
		return;
	case CDDS_ITEMPREPAINT:
		*pResult = CDRF_NOTIFYPOSTPAINT;
		return;
	case CDDS_ITEMPOSTPAINT:
	{
		HTREEITEM item = reinterpret_cast<HTREEITEM>(customDraw->nmcd.dwItemSpec);
		auto itPath = ColorTreeItemPaths.find(item);
		if (itPath == ColorTreeItemPaths.end() || Owner == nullptr)
		{
			*pResult = CDRF_DODEFAULT;
			return;
		}

		int r = Owner->GetProfileColorComponentValue(itPath->second, 'r', 255);
		int g = Owner->GetProfileColorComponentValue(itPath->second, 'g', 255);
		int b = Owner->GetProfileColorComponentValue(itPath->second, 'b', 255);

		CDC dc;
		dc.Attach(customDraw->nmcd.hdc);
		CRect rowRect;
		if (ColorPathTree.GetItemRect(item, &rowRect, FALSE))
		{
			CRect clientRect;
			ColorPathTree.GetClientRect(&clientRect);
			CRect swatchRect(max(rowRect.left + 8, clientRect.right - 22), rowRect.top + 3, clientRect.right - 6, rowRect.bottom - 3);
			CBrush fillBrush(RGB(r, g, b));
			CPen borderPen(PS_SOLID, 1, RGB(170, 170, 170));
			CBrush* oldBrush = dc.SelectObject(&fillBrush);
			CPen* oldPen = dc.SelectObject(&borderPen);
			dc.RoundRect(&swatchRect, CPoint(6, 6));
			dc.SelectObject(oldPen);
			dc.SelectObject(oldBrush);
		}
		dc.Detach();

		*pResult = CDRF_DODEFAULT;
		return;
	}
	default:
		*pResult = CDRF_DODEFAULT;
		return;
	}
}

void CProfileEditorDialog::OnFixedScaleSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	FixedScaleSlider.GetClientRect(&clientRect);
	DrawHorizontalModernSlider(dc, clientRect, FixedScaleSlider);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

void CProfileEditorDialog::OnBoostFactorSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	BoostFactorSlider.GetClientRect(&clientRect);
	DrawHorizontalModernSlider(dc, clientRect, BoostFactorSlider);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

void CProfileEditorDialog::OnColorValueSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	ColorValueSlider.GetClientRect(&clientRect);
	dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	if (DraftColorValid)
		RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
	int topR = 255;
	int topG = 255;
	int topB = 255;
	HsvToRgb(hue, saturation, 1.0, topR, topG, topB);
	DrawVerticalValueSlider(dc, clientRect, ColorValueSlider.GetPos(), topR, topG, topB);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

void CProfileEditorDialog::OnColorOpacitySliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	ColorOpacitySlider.GetClientRect(&clientRect);
	dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

	const int channelWidth = max(10, min(16, clientRect.Width() - 8));
	const int channelLeft = clientRect.left + ((clientRect.Width() - channelWidth) / 2);
	const int channelTop = clientRect.top + 4;
	const int channelBottom = clientRect.bottom - 4;
	CRect channelRect(channelLeft, channelTop, channelLeft + channelWidth, channelBottom);

	CRgn clipRegion;
	clipRegion.CreateRoundRectRgn(channelRect.left, channelRect.top, channelRect.right + 1, channelRect.bottom + 1, 8, 8);
	const int savedDc = dc.SaveDC();
	dc.SelectClipRgn(&clipRegion);

	// Draw checkerboard transparency background in the channel.
	const int checkerSize = 4;
	for (int y = channelRect.top; y < channelRect.bottom; y += checkerSize)
	{
		for (int x = channelRect.left; x < channelRect.right; x += checkerSize)
		{
			const bool lightCell = (((x - channelRect.left) / checkerSize) + ((y - channelRect.top) / checkerSize)) % 2 == 0;
			const COLORREF checkerColor = lightCell ? RGB(244, 244, 244) : RGB(226, 226, 226);
			const int w = min(checkerSize, channelRect.right - x);
			const int h = min(checkerSize, channelRect.bottom - y);
			dc.FillSolidRect(x, y, w, h, checkerColor);
		}
	}

	const int gradientHeight = max(1, channelRect.Height());
	for (int y = 0; y < gradientHeight; ++y)
	{
		const double t = min(1.0, max(0.0, static_cast<double>(y) / static_cast<double>(max(1, gradientHeight - 1))));
		const double alpha = 1.0 - t;
		const int r = static_cast<int>(round((static_cast<double>(DraftColorR) * alpha) + (240.0 * (1.0 - alpha))));
		const int g = static_cast<int>(round((static_cast<double>(DraftColorG) * alpha) + (240.0 * (1.0 - alpha))));
		const int b = static_cast<int>(round((static_cast<double>(DraftColorB) * alpha) + (240.0 * (1.0 - alpha))));
		dc.FillSolidRect(channelRect.left, channelRect.top + y, channelRect.Width(), 1, RGB(r, g, b));
	}
	dc.RestoreDC(savedDc);

	CPen channelBorder(PS_SOLID, 1, RGB(186, 186, 186));
	CPen* oldPen = dc.SelectObject(&channelBorder);
	CBrush* oldBrush = static_cast<CBrush*>(dc.SelectStockObject(HOLLOW_BRUSH));
	dc.RoundRect(&channelRect, CPoint(8, 8));

	const int sliderPos = min(100, max(0, ColorOpacitySlider.GetPos()));
	const double sliderT = static_cast<double>(sliderPos) / 100.0;
	const int thumbHeight = 14;
	const int thumbHalf = thumbHeight / 2;
	const int centerY = channelRect.top + static_cast<int>(round((1.0 - sliderT) * static_cast<double>(max(1, channelRect.Height() - 1))));
	const int thumbLeft = max(clientRect.left + 1, channelRect.left - 3);
	const int thumbRight = min(clientRect.right - 1, channelRect.right + 3);
	const int thumbTop = max(clientRect.top + 1, centerY - thumbHalf);
	const int thumbBottom = min(clientRect.bottom - 1, centerY + thumbHalf + 1);
	CRect thumbRect(thumbLeft, thumbTop, thumbRight, thumbBottom);
	thumbRect.DeflateRect(1, 1);
	CBrush thumbOuterBrush(RGB(255, 255, 255));
	CPen thumbOuterPen(PS_SOLID, 1, RGB(212, 212, 212));
	dc.SelectObject(&thumbOuterPen);
	dc.SelectObject(&thumbOuterBrush);
	dc.RoundRect(&thumbRect, CPoint(10, 10));

	CRect thumbInner = thumbRect;
	thumbInner.DeflateRect(3, 3);
	CBrush thumbInnerBrush(RGB(64, 182, 227));
	CPen thumbInnerPen(PS_SOLID, 1, RGB(64, 182, 227));
	dc.SelectObject(&thumbInnerPen);
	dc.SelectObject(&thumbInnerBrush);
	dc.RoundRect(&thumbInner, CPoint(8, 8));

	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

void CProfileEditorDialog::OnRuleColorValueSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	RuleColorValueSlider.GetClientRect(&clientRect);
	dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	if (RuleColorDraftValid)
		RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
	int topR = 255;
	int topG = 255;
	int topB = 255;
	HsvToRgb(hue, saturation, 1.0, topR, topG, topB);
	DrawVerticalValueSlider(dc, clientRect, RuleColorValueSlider.GetPos(), topR, topG, topB);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

void CProfileEditorDialog::OnRuleColorOpacitySliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult == nullptr)
		return;

	NMCUSTOMDRAW* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
	if (customDraw->dwDrawStage != CDDS_PREPAINT)
	{
		*pResult = CDRF_DODEFAULT;
		return;
	}

	CDC dc;
	dc.Attach(customDraw->hdc);

	CRect clientRect;
	RuleColorOpacitySlider.GetClientRect(&clientRect);
	dc.FillSolidRect(&clientRect, kEditorThemeBackgroundColor);

	const int channelWidth = max(10, min(16, clientRect.Width() - 8));
	const int channelLeft = clientRect.left + ((clientRect.Width() - channelWidth) / 2);
	const int channelTop = clientRect.top + 4;
	const int channelBottom = clientRect.bottom - 4;
	CRect channelRect(channelLeft, channelTop, channelLeft + channelWidth, channelBottom);

	CRgn clipRegion;
	clipRegion.CreateRoundRectRgn(channelRect.left, channelRect.top, channelRect.right + 1, channelRect.bottom + 1, 8, 8);
	const int savedDc = dc.SaveDC();
	dc.SelectClipRgn(&clipRegion);

	const int checkerSize = 4;
	for (int y = channelRect.top; y < channelRect.bottom; y += checkerSize)
	{
		for (int x = channelRect.left; x < channelRect.right; x += checkerSize)
		{
			const bool lightCell = (((x - channelRect.left) / checkerSize) + ((y - channelRect.top) / checkerSize)) % 2 == 0;
			const COLORREF checkerColor = lightCell ? RGB(244, 244, 244) : RGB(226, 226, 226);
			const int w = min(checkerSize, channelRect.right - x);
			const int h = min(checkerSize, channelRect.bottom - y);
			dc.FillSolidRect(x, y, w, h, checkerColor);
		}
	}

	const int gradientHeight = max(1, channelRect.Height());
	for (int y = 0; y < gradientHeight; ++y)
	{
		const double t = min(1.0, max(0.0, static_cast<double>(y) / static_cast<double>(max(1, gradientHeight - 1))));
		const double alpha = 1.0 - t;
		const int r = static_cast<int>(round((static_cast<double>(RuleColorDraftR) * alpha) + (240.0 * (1.0 - alpha))));
		const int g = static_cast<int>(round((static_cast<double>(RuleColorDraftG) * alpha) + (240.0 * (1.0 - alpha))));
		const int b = static_cast<int>(round((static_cast<double>(RuleColorDraftB) * alpha) + (240.0 * (1.0 - alpha))));
		dc.FillSolidRect(channelRect.left, channelRect.top + y, channelRect.Width(), 1, RGB(r, g, b));
	}
	dc.RestoreDC(savedDc);

	CPen channelBorder(PS_SOLID, 1, RGB(186, 186, 186));
	CPen* oldPen = dc.SelectObject(&channelBorder);
	CBrush* oldBrush = static_cast<CBrush*>(dc.SelectStockObject(HOLLOW_BRUSH));
	dc.RoundRect(&channelRect, CPoint(8, 8));

	const int sliderPos = min(100, max(0, RuleColorOpacitySlider.GetPos()));
	const double sliderT = static_cast<double>(sliderPos) / 100.0;
	const int thumbHeight = 14;
	const int thumbHalf = thumbHeight / 2;
	const int centerY = channelRect.top + static_cast<int>(round((1.0 - sliderT) * static_cast<double>(max(1, channelRect.Height() - 1))));
	const int thumbLeft = max(clientRect.left + 1, channelRect.left - 3);
	const int thumbRight = min(clientRect.right - 1, channelRect.right + 3);
	const int thumbTop = max(clientRect.top + 1, centerY - thumbHalf);
	const int thumbBottom = min(clientRect.bottom - 1, centerY + thumbHalf + 1);
	CRect thumbRect(thumbLeft, thumbTop, thumbRight, thumbBottom);
	thumbRect.DeflateRect(1, 1);
	CBrush thumbOuterBrush(RGB(255, 255, 255));
	CPen thumbOuterPen(PS_SOLID, 1, RGB(212, 212, 212));
	dc.SelectObject(&thumbOuterPen);
	dc.SelectObject(&thumbOuterBrush);
	dc.RoundRect(&thumbRect, CPoint(10, 10));

	CRect thumbInner = thumbRect;
	thumbInner.DeflateRect(3, 3);
	CBrush thumbInnerBrush(RGB(64, 182, 227));
	CPen thumbInnerPen(PS_SOLID, 1, RGB(64, 182, 227));
	dc.SelectObject(&thumbInnerPen);
	dc.SelectObject(&thumbInnerBrush);
	dc.RoundRect(&thumbInner, CPoint(8, 8));

	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);
	dc.Detach();

	*pResult = CDRF_SKIPDEFAULT;
}

bool CProfileEditorDialog::TryApplyColorWheelPoint(const CPoint& screenPoint)
{
	if (Owner == nullptr)
		return false;

	double hue = 0.0;
	double saturation = 0.0;
	if (!TryReadHueSaturationFromWheel(ColorWheel, screenPoint, hue, saturation))
		return false;

	const double value = ::IsWindow(ColorValueSlider.GetSafeHwnd())
		? min(1.0, max(0.0, static_cast<double>(ColorValueSlider.GetPos()) / 100.0))
		: 1.0;

	int r = 255;
	int g = 255;
	int b = 255;
	HsvToRgb(hue, saturation, value, r, g, b);

	DraftColorR = r;
	DraftColorG = g;
	DraftColorB = b;
	DraftColorValid = true;
	DraftColorHue = hue;
	DraftColorSaturation = saturation;
	DraftColorHueSaturationValid = true;
	UpdateDraftColorControls();
	return true;
}

bool CProfileEditorDialog::TryApplyColorValueSliderPoint(const CPoint& screenPoint)
{
	if (!DraftColorValid || !::IsWindow(ColorValueSlider.GetSafeHwnd()))
		return false;

	int newPos = ColorValueSlider.GetPos();
	if (!TryReadVerticalSliderPosFromPoint(ColorValueSlider, screenPoint, newPos))
		return false;

	if (ColorValueSlider.GetPos() != newPos)
		ColorValueSlider.SetPos(newPos);
	ApplyDraftColorValueFromSlider();
	return true;
}

bool CProfileEditorDialog::TryApplyColorOpacitySliderPoint(const CPoint& screenPoint)
{
	if (!DraftColorValid || !::IsWindow(ColorOpacitySlider.GetSafeHwnd()))
		return false;

	int newPos = ColorOpacitySlider.GetPos();
	if (!TryReadVerticalSliderPosFromPoint(ColorOpacitySlider, screenPoint, newPos))
		return false;

	if (ColorOpacitySlider.GetPos() != newPos)
		ColorOpacitySlider.SetPos(newPos);
	ApplyDraftColorOpacityFromSlider();
	return true;
}

bool CProfileEditorDialog::TryApplyRuleColorWheelPoint(const CPoint& screenPoint)
{
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return false;

	double hue = 0.0;
	double saturation = 0.0;
	if (!TryReadHueSaturationFromWheel(RuleColorWheel, screenPoint, hue, saturation))
		return false;
	const double value = ::IsWindow(RuleColorValueSlider.GetSafeHwnd())
		? min(1.0, max(0.0, static_cast<double>(RuleColorValueSlider.GetPos()) / 100.0))
		: 1.0;

	int r = 255;
	int g = 255;
	int b = 255;
	HsvToRgb(hue, saturation, value, r, g, b);
	RuleColorDraftR = r;
	RuleColorDraftG = g;
	RuleColorDraftB = b;
	RuleColorDraftValid = true;
	RuleColorDraftDirty = true;
	RuleColorDraftHue = hue;
	RuleColorDraftSaturation = saturation;
	RuleColorDraftHueSaturationValid = true;
	RuleColorApplyButton.EnableWindow(TRUE);
	InvalidateRuleColorSwatches();

	CButton* check = nullptr;
	CEdit* edit = nullptr;
	if (GetRuleColorEditorTargetControls(RuleColorActiveSwatchId, check, edit) && edit != nullptr)
	{
		UpdatingControls = true;
		SetEditTextPreserveCaret(*edit, FormatRgbaQuad(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, RuleColorDraftA));
		UpdatingControls = false;
	}
	return true;
}

bool CProfileEditorDialog::TryApplyRuleColorValueSliderPoint(const CPoint& screenPoint)
{
	if (!RuleColorDraftValid || !::IsWindow(RuleColorValueSlider.GetSafeHwnd()))
		return false;

	int newPos = RuleColorValueSlider.GetPos();
	if (!TryReadVerticalSliderPosFromPoint(RuleColorValueSlider, screenPoint, newPos))
		return false;

	if (RuleColorValueSlider.GetPos() != newPos)
		RuleColorValueSlider.SetPos(newPos);
	ApplyRuleColorValueFromSlider();
	return true;
}

bool CProfileEditorDialog::TryApplyRuleColorOpacitySliderPoint(const CPoint& screenPoint)
{
	if (!RuleColorDraftValid || !::IsWindow(RuleColorOpacitySlider.GetSafeHwnd()))
		return false;

	int newPos = RuleColorOpacitySlider.GetPos();
	if (!TryReadVerticalSliderPosFromPoint(RuleColorOpacitySlider, screenPoint, newPos))
		return false;

	if (RuleColorOpacitySlider.GetPos() != newPos)
		RuleColorOpacitySlider.SetPos(newPos);
	ApplyRuleColorOpacityFromSlider();
	return true;
}

LRESULT CProfileEditorDialog::OnColorWheelTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyColorWheelPoint(screenPoint);
	return 0;
}

LRESULT CProfileEditorDialog::OnColorValueSliderTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyColorValueSliderPoint(screenPoint);
	return 0;
}

LRESULT CProfileEditorDialog::OnColorOpacitySliderTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyColorOpacitySliderPoint(screenPoint);
	return 0;
}

LRESULT CProfileEditorDialog::OnRuleColorWheelTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyRuleColorWheelPoint(screenPoint);
	return 0;
}

LRESULT CProfileEditorDialog::OnRuleColorValueSliderTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyRuleColorValueSliderPoint(screenPoint);
	return 0;
}

LRESULT CProfileEditorDialog::OnRuleColorOpacitySliderTrack(WPARAM wParam, LPARAM lParam)
{
	const CPoint screenPoint(static_cast<int>(wParam), static_cast<int>(lParam));
	TryApplyRuleColorOpacitySliderPoint(screenPoint);
	return 0;
}

void CProfileEditorDialog::OnColorWheelClicked()
{
	CPoint cursorScreen;
	if (!::GetCursorPos(&cursorScreen))
		return;

	TryApplyColorWheelPoint(cursorScreen);
}

void CProfileEditorDialog::OnApplyColorClicked()
{
	if (Owner == nullptr || !DraftColorValid)
		return;

	const bool includeAlpha = DraftColorHasAlpha || DraftColorA != 255;
	if (Owner->SetSelectedProfileColorForEditor(DraftColorR, DraftColorG, DraftColorB, DraftColorA, includeAlpha, true))
	{
		RefreshEditorFieldsFromSelection();
		ColorPathTree.Invalidate(FALSE);
	}
}

void CProfileEditorDialog::OnResetColorClicked()
{
	if (Owner == nullptr)
		return;
	LoadDraftColorFromSelection();
	UpdateDraftColorControls();
}

void CProfileEditorDialog::OnRgbaEditChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	CString rgbaText;
	EditRgba.GetWindowText(rgbaText);
	rgbaText.Trim();
	if (rgbaText.IsEmpty())
		return;

	int r = 0, g = 0, b = 0, a = 255;
	bool hasAlpha = false;
	if (!TryParseRgbaQuad(std::string(rgbaText.GetString()), r, g, b, a, hasAlpha))
		return;

	DraftColorR = r;
	DraftColorG = g;
	DraftColorB = b;
	DraftColorA = a;
	DraftColorHasAlpha = hasAlpha;
	DraftColorValid = true;
	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
	DraftColorHue = hue;
	DraftColorSaturation = saturation;
	DraftColorHueSaturationValid = true;
	UpdateDraftColorControls();
}

void CProfileEditorDialog::OnHexEditChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	CString hexText;
	EditHex.GetWindowText(hexText);
	hexText.Trim();
	if (hexText.IsEmpty())
		return;

	int r = 0, g = 0, b = 0, a = 255;
	bool hasAlpha = false;
	if (!TryParseHexColor(std::string(hexText.GetString()), r, g, b, a, hasAlpha))
		return;

	DraftColorR = r;
	DraftColorG = g;
	DraftColorB = b;
	if (hasAlpha)
	{
		DraftColorA = a;
		DraftColorHasAlpha = true;
	}
	else
	{
		DraftColorA = 255;
		DraftColorHasAlpha = false;
	}
	DraftColorValid = true;
	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);
	DraftColorHue = hue;
	DraftColorSaturation = saturation;
	DraftColorHueSaturationValid = true;
	UpdateDraftColorControls(true, false);
}

void CProfileEditorDialog::OnTabSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	const int tab = PageTabs.GetCurSel();
	if (tab == kTabIcons)
		SyncTagEditorControlsFromRadar();
	else if (tab == kTabProfile)
	{
		RebuildProfileList();
		RefreshProfileControls();
	}
	UpdatePageVisibility();
	ForceChildRepaint();
	if (pResult != nullptr)
		*pResult = 0;
}

void CProfileEditorDialog::OnNavColorsClicked()
{
	PageTabs.SetCurSel(kTabColors);
	UpdatePageVisibility();
	ForceChildRepaint();
}

void CProfileEditorDialog::OnNavIconClicked()
{
	PageTabs.SetCurSel(kTabIcons);
	SyncTagEditorControlsFromRadar();
	UpdatePageVisibility();
	ForceChildRepaint();
}

void CProfileEditorDialog::OnNavRulesClicked()
{
	PageTabs.SetCurSel(kTabRules);
	UpdatePageVisibility();
	ForceChildRepaint();
}

void CProfileEditorDialog::OnNavProfileClicked()
{
	PageTabs.SetCurSel(kTabProfile);
	RebuildProfileList();
	RefreshProfileControls();
	UpdatePageVisibility();
	ForceChildRepaint();
}

void CProfileEditorDialog::OnIconStyleChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	std::string style = "realistic";
	UINT clickedControlId = 0;
	if (const MSG* currentMessage = GetCurrentMessage())
	{
		if (currentMessage->message == WM_COMMAND)
			clickedControlId = LOWORD(currentMessage->wParam);
	}

	if (clickedControlId == IDC_PE_ICON_STYLE_ARROW)
		style = "triangle";
	else if (clickedControlId == IDC_PE_ICON_STYLE_DIAMOND)
		style = "diamond";
	else if (clickedControlId == IDC_PE_ICON_STYLE_REALISTIC)
		style = "realistic";
	else if (IconStyleArrow.GetCheck() == BST_CHECKED)
		style = "triangle";
	else if (IconStyleDiamond.GetCheck() == BST_CHECKED)
		style = "diamond";

	if (Owner->SetActiveTargetIconStyle(style, true))
	{
		Owner->RequestRefresh();
		SyncIconControlsFromRadar();
	}
}

void CProfileEditorDialog::OnFixedPixelToggled()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const bool enabled = (FixedPixelCheck.GetCheck() == BST_CHECKED);
	if (Owner->SetFixedPixelTargetIconSizeEnabled(enabled, true))
		Owner->RequestRefresh();
}

void CProfileEditorDialog::OnFixedScaleChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const double selectedScale = ParseComboScaleSelection(FixedScaleCombo, Owner->GetFixedPixelTriangleIconScale());
	FixedScaleSlider.SetPos(ScaleToSliderPos(selectedScale));
	UpdateIconScaleValueLabels();
	if (Owner->SetFixedPixelTriangleIconScale(selectedScale, true))
		Owner->RequestRefresh();
}

void CProfileEditorDialog::OnSmallBoostToggled()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const bool enabled = (SmallBoostCheck.GetCheck() == BST_CHECKED);
	if (Owner->SetSmallTargetIconBoostEnabled(enabled, true))
		Owner->RequestRefresh();
}

void CProfileEditorDialog::OnBoostFactorChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const double selectedFactor = ParseComboScaleSelection(BoostFactorCombo, Owner->GetSmallTargetIconBoostFactor());
	BoostFactorSlider.SetPos(ScaleToSliderPos(selectedFactor));
	UpdateIconScaleValueLabels();
	if (Owner->SetSmallTargetIconBoostFactor(selectedFactor, true))
		Owner->RequestRefresh();
}

void CProfileEditorDialog::OnBoostResolutionChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selected = BoostResolutionCombo.GetCurSel();
	if (selected == CB_ERR)
		return;

	CString selectedText;
	BoostResolutionCombo.GetLBText(selected, selectedText);
	if (_stricmp(selectedText.GetString(), "2K") == 0)
	{
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_CHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
	}
	else if (_stricmp(selectedText.GetString(), "4K") == 0)
	{
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_CHECKED);
	}
	else
	{
		BoostResolution1080Button.SetCheck(BST_CHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
	}

	if (Owner->SetSmallTargetIconBoostResolutionPreset(std::string(selectedText.GetString()), true))
		Owner->RequestRefresh();

	// Keep icon-shape radio state consistent after preset toggles.
	SyncIconControlsFromRadar();
}

void CProfileEditorDialog::OnBoostResolutionPresetChanged()
{
	if (UpdatingControls)
		return;

	UINT clickedControlId = 0;
	if (const MSG* currentMessage = GetCurrentMessage())
	{
		if (currentMessage->message == WM_COMMAND)
			clickedControlId = LOWORD(currentMessage->wParam);
	}

	if (clickedControlId == IDC_PE_ICON_RES_2K)
	{
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_CHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
		SelectComboEntryByText(BoostResolutionCombo, "2K");
	}
	else if (clickedControlId == IDC_PE_ICON_RES_4K)
	{
		BoostResolution1080Button.SetCheck(BST_UNCHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_CHECKED);
		SelectComboEntryByText(BoostResolutionCombo, "4K");
	}
	else
	{
		BoostResolution1080Button.SetCheck(BST_CHECKED);
		BoostResolution2KButton.SetCheck(BST_UNCHECKED);
		BoostResolution4KButton.SetCheck(BST_UNCHECKED);
		SelectComboEntryByText(BoostResolutionCombo, "1080p");
	}

	OnBoostResolutionChanged();
}

BEGIN_MESSAGE_MAP(CProfileEditorDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_MOVE()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_WM_HSCROLL()
	ON_WM_VSCROLL()
	ON_WM_SHOWWINDOW()
	ON_WM_DESTROY()
	ON_WM_PAINT()
	ON_WM_DRAWITEM()
	ON_WM_CTLCOLOR()
	ON_MESSAGE(WM_PE_COLOR_WHEEL_TRACK, &CProfileEditorDialog::OnColorWheelTrack)
	ON_MESSAGE(WM_PE_COLOR_VALUE_TRACK, &CProfileEditorDialog::OnColorValueSliderTrack)
	ON_MESSAGE(WM_PE_COLOR_OPACITY_TRACK, &CProfileEditorDialog::OnColorOpacitySliderTrack)
	ON_MESSAGE(WM_PE_RULE_COLOR_WHEEL_TRACK, &CProfileEditorDialog::OnRuleColorWheelTrack)
	ON_MESSAGE(WM_PE_RULE_COLOR_VALUE_TRACK, &CProfileEditorDialog::OnRuleColorValueSliderTrack)
	ON_MESSAGE(WM_PE_RULE_COLOR_OPACITY_TRACK, &CProfileEditorDialog::OnRuleColorOpacitySliderTrack)
	ON_NOTIFY(TVN_SELCHANGED, IDC_PE_COLOR_TREE, &CProfileEditorDialog::OnColorTreeSelectionChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_TREE, &CProfileEditorDialog::OnColorTreeCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_FIXED_SCALE_SLIDER, &CProfileEditorDialog::OnFixedScaleSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_BOOST_FACTOR_SLIDER, &CProfileEditorDialog::OnBoostFactorSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_VALUE_SLIDER, &CProfileEditorDialog::OnColorValueSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_OPACITY_SLIDER, &CProfileEditorDialog::OnColorOpacitySliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_RULE_COLOR_VALUE_SLIDER, &CProfileEditorDialog::OnRuleColorValueSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_RULE_COLOR_OPACITY_SLIDER, &CProfileEditorDialog::OnRuleColorOpacitySliderCustomDraw)
	ON_STN_CLICKED(IDC_PE_COLOR_WHEEL, &CProfileEditorDialog::OnColorWheelClicked)
	ON_BN_CLICKED(IDC_PE_APPLY_BUTTON, &CProfileEditorDialog::OnApplyColorClicked)
	ON_BN_CLICKED(IDC_PE_RESET_BUTTON, &CProfileEditorDialog::OnResetColorClicked)
	ON_EN_CHANGE(IDC_PE_EDIT_RGBA, &CProfileEditorDialog::OnRgbaEditChanged)
	ON_EN_CHANGE(IDC_PE_EDIT_HEX, &CProfileEditorDialog::OnHexEditChanged)
	ON_NOTIFY(TCN_SELCHANGE, IDC_PE_TAB, &CProfileEditorDialog::OnTabSelectionChanged)
	ON_BN_CLICKED(IDC_PE_NAV_COLORS, &CProfileEditorDialog::OnNavColorsClicked)
	ON_BN_CLICKED(IDC_PE_NAV_ICON, &CProfileEditorDialog::OnNavIconClicked)
	ON_BN_CLICKED(IDC_PE_NAV_RULES, &CProfileEditorDialog::OnNavRulesClicked)
	ON_BN_CLICKED(IDC_PE_NAV_PROFILE, &CProfileEditorDialog::OnNavProfileClicked)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_ARROW, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_DIAMOND, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_REALISTIC, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_FIXED_PIXEL_CHECK, &CProfileEditorDialog::OnFixedPixelToggled)
	ON_CBN_SELCHANGE(IDC_PE_FIXED_SCALE_COMBO, &CProfileEditorDialog::OnFixedScaleChanged)
	ON_BN_CLICKED(IDC_PE_SMALL_BOOST_CHECK, &CProfileEditorDialog::OnSmallBoostToggled)
	ON_CBN_SELCHANGE(IDC_PE_BOOST_FACTOR_COMBO, &CProfileEditorDialog::OnBoostFactorChanged)
	ON_CBN_SELCHANGE(IDC_PE_BOOST_RES_COMBO, &CProfileEditorDialog::OnBoostResolutionChanged)
	ON_BN_CLICKED(IDC_PE_ICON_RES_1080, &CProfileEditorDialog::OnBoostResolutionPresetChanged)
	ON_BN_CLICKED(IDC_PE_ICON_RES_2K, &CProfileEditorDialog::OnBoostResolutionPresetChanged)
	ON_BN_CLICKED(IDC_PE_ICON_RES_4K, &CProfileEditorDialog::OnBoostResolutionPresetChanged)
	ON_NOTIFY(TVN_SELCHANGED, IDC_PE_RULE_TREE, &CProfileEditorDialog::OnRuleTreeSelectionChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_RULE_TREE, &CProfileEditorDialog::OnRuleTreeCustomDraw)
	ON_NOTIFY(NM_CLICK, IDC_PE_RULE_TREE, &CProfileEditorDialog::OnRuleTreeClick)
	ON_BN_CLICKED(IDC_PE_RULE_ADD_BUTTON, &CProfileEditorDialog::OnRuleAddClicked)
	ON_BN_CLICKED(IDC_PE_RULE_ADD_PARAM_BUTTON, &CProfileEditorDialog::OnRuleAddParameterClicked)
	ON_BN_CLICKED(IDC_PE_RULE_REMOVE_BUTTON, &CProfileEditorDialog::OnRuleRemoveClicked)
	ON_EN_CHANGE(IDC_PE_RULE_NAME_EDIT, &CProfileEditorDialog::OnRuleNameChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_SOURCE_COMBO, &CProfileEditorDialog::OnRuleSourceChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_TOKEN_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_CONDITION_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_EDITCHANGE(IDC_PE_RULE_CONDITION_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_TYPE_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_STATUS_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_DETAIL_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TARGET_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_STN_CLICKED(IDC_PE_RULE_TARGET_SWATCH, &CProfileEditorDialog::OnRuleTargetSwatchClicked)
	ON_EN_CHANGE(IDC_PE_RULE_TARGET_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TAG_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_STN_CLICKED(IDC_PE_RULE_TAG_SWATCH, &CProfileEditorDialog::OnRuleTagSwatchClicked)
	ON_EN_CHANGE(IDC_PE_RULE_TAG_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TEXT_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_STN_CLICKED(IDC_PE_RULE_TEXT_SWATCH, &CProfileEditorDialog::OnRuleTextSwatchClicked)
	ON_EN_CHANGE(IDC_PE_RULE_TEXT_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_COLOR_APPLY_BUTTON, &CProfileEditorDialog::OnRuleColorApplyClicked)
	ON_BN_CLICKED(IDC_PE_RULE_COLOR_RESET_BUTTON, &CProfileEditorDialog::OnRuleColorResetClicked)
	ON_LBN_SELCHANGE(IDC_PE_PROFILE_LIST, &CProfileEditorDialog::OnProfileSelectionChanged)
	ON_BN_CLICKED(IDC_PE_PROFILE_ADD_BUTTON, &CProfileEditorDialog::OnProfileAddClicked)
	ON_BN_CLICKED(IDC_PE_PROFILE_DUPLICATE_BUTTON, &CProfileEditorDialog::OnProfileDuplicateClicked)
	ON_BN_CLICKED(IDC_PE_PROFILE_RENAME_BUTTON, &CProfileEditorDialog::OnProfileRenameClicked)
	ON_BN_CLICKED(IDC_PE_PROFILE_DELETE_BUTTON, &CProfileEditorDialog::OnProfileDeleteClicked)
	ON_BN_CLICKED(IDC_PE_PROFILE_PRO_MODE_CHECK, &CProfileEditorDialog::OnProfileProModeToggled)
	ON_STN_CLICKED(IDC_PE_PROFILE_REPO_LINK, &CProfileEditorDialog::OnProfileRepoLinkClicked)
	ON_STN_CLICKED(IDC_PE_PROFILE_COFFEE_LINK, &CProfileEditorDialog::OnProfileCoffeeLinkClicked)
	ON_CBN_SELCHANGE(IDC_PE_TAG_TYPE_COMBO, &CProfileEditorDialog::OnTagTypeChanged)
	ON_CBN_SELCHANGE(IDC_PE_TAG_STATUS_COMBO, &CProfileEditorDialog::OnTagStatusChanged)
	ON_BN_CLICKED(IDC_PE_TAG_TOKEN_ADD_BUTTON, &CProfileEditorDialog::OnTagAddTokenClicked)
	ON_BN_CLICKED(IDC_PE_TAG_AUTO_DECONFLICTION, &CProfileEditorDialog::OnTagAutoDeconflictionToggled)
	ON_BN_CLICKED(IDC_PE_TAG_LINK_DETAILED, &CProfileEditorDialog::OnTagLinkToggleChanged)
	ON_EN_CHANGE(IDC_PE_TAG_DEFINITION_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE1_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE2_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE3_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE4_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_DETAILED_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE1_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE2_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE3_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE4_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_SETFOCUS(IDC_PE_TAG_DEFINITION_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE1_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE2_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE3_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE4_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_DETAILED_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE1_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE2_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE3_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE4_EDIT, &CProfileEditorDialog::OnTagLineFocus)
END_MESSAGE_MAP()
