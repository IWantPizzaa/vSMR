#include "stdafx.h"
#include "ProfileEditorDialog.hpp"
#include "SMRRadar.hpp"
#include "afxdialogex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>

IMPLEMENT_DYNAMIC(CProfileEditorDialog, CDialogEx)

namespace
{
	const UINT WM_PE_COLOR_WHEEL_TRACK = WM_APP + 417;
	const UINT WM_PE_COLOR_VALUE_TRACK = WM_APP + 418;
	const UINT WM_PE_COLOR_OPACITY_TRACK = WM_APP + 419;
	const UINT WM_PE_RULE_COLOR_WHEEL_TRACK = WM_APP + 420;
	const UINT WM_PE_RULE_COLOR_VALUE_TRACK = WM_APP + 421;
	const COLORREF kEditorBorderColor = RGB(160, 160, 160);
	const COLORREF kEditorThemeBackgroundColor = RGB(240, 240, 240);
	std::map<HWND, WNDPROC> gThemedEditOldProcs;
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

		HPEN borderPen = ::CreatePen(PS_SOLID, 1, kEditorBorderColor);
		HGDIOBJ oldPen = ::SelectObject(hdc, borderPen);
		HGDIOBJ oldBrush = ::SelectObject(hdc, ::GetStockObject(HOLLOW_BRUSH));
		::Rectangle(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom);
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
		case WM_ENABLE:
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		case WM_SETTEXT:
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
	CDialogEx::OnInitDialog();

	CWnd* statusWnd = GetDlgItem(IDC_PROFILE_EDITOR_STATUS);
	if (statusWnd != nullptr && ::IsWindow(statusWnd->GetSafeHwnd()))
		statusWnd->ShowWindow(SW_HIDE);

	CreateEditorControls();
	Initialized = true;
	SyncFromRadar();
	ForceChildRepaint();
	NotifyWindowRectChanged();
	return TRUE;
}

void CProfileEditorDialog::SyncFromRadar()
{
	if (!ControlsCreated || Owner == nullptr)
		return;

	RebuildColorPathList();
	RefreshEditorFieldsFromSelection();
	SyncIconControlsFromRadar();
	RebuildRulesList();
	RefreshRuleControls();
	SyncTagEditorControlsFromRadar();
	UpdatePageVisibility();
	ForceChildRepaint();
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

	return false;
}

void CProfileEditorDialog::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	if (pScrollBar != nullptr)
	{
		const HWND sourceHwnd = pScrollBar->GetSafeHwnd();
		if (sourceHwnd == FixedScaleSlider.GetSafeHwnd())
		{
			UpdateIconScaleValueLabels();
			if (!UpdatingControls && Owner != nullptr)
			{
				const double scale = SliderPosToScale(FixedScaleSlider.GetPos());
				if (Owner->SetFixedPixelTriangleIconScale(scale, true))
					Owner->RequestRefresh();
			}
			FixedScaleSlider.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		}
		else if (sourceHwnd == BoostFactorSlider.GetSafeHwnd())
		{
			UpdateIconScaleValueLabels();
			if (!UpdatingControls && Owner != nullptr)
			{
				const double scale = SliderPosToScale(BoostFactorSlider.GetPos());
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

	restoreTrackingProcMap(gColorWheelOldProcs, gColorWheelOwnerWindows);
	restoreTrackingProcMap(gColorValueSliderOldProcs, gColorValueSliderOwnerWindows);
	restoreTrackingProcMap(gColorOpacitySliderOldProcs, gColorOpacitySliderOwnerWindows);
	restoreTrackingProcMap(gRuleColorWheelOldProcs, gRuleColorWheelOwnerWindows);
	restoreTrackingProcMap(gRuleColorValueSliderOldProcs, gRuleColorValueSliderOwnerWindows);
}

HBRUSH CProfileEditorDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
	if (pWnd == nullptr)
		return hbr;

	if (nCtlColor == CTLCOLOR_LISTBOX && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		pDC->SetTextColor(RGB(17, 24, 39));
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}

	const int controlId = pWnd->GetDlgCtrlID();
	const bool useColorThemeBackground = [&]()
	{
		switch (controlId)
		{
		case IDC_PE_COLOR_LEFT_PANEL:
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
	if (controlId == IDC_PE_RULE_LIST)
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

		if (lpDrawItemStruct->itemID != static_cast<UINT>(-1))
		{
			const bool isSelected = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
			CRect itemRect(localOuter);
			itemRect.DeflateRect(4, 2);

			if (isSelected)
			{
				CBrush selectedBrush(RGB(37, 120, 209));
				CPen selectedPen(PS_SOLID, 1, RGB(37, 120, 209));
				CPen* oldPen = memDc.SelectObject(&selectedPen);
				CBrush* oldBrush = memDc.SelectObject(&selectedBrush);
				memDc.RoundRect(&itemRect, CPoint(8, 8));
				memDc.SelectObject(oldBrush);
				memDc.SelectObject(oldPen);
			}

			CString itemText;
			RulesList.GetText(static_cast<int>(lpDrawItemStruct->itemID), itemText);

			CFont* oldFont = nullptr;
			if (RulesList.GetFont() != nullptr)
				oldFont = memDc.SelectObject(RulesList.GetFont());

			memDc.SetBkMode(TRANSPARENT);
			memDc.SetTextColor(isSelected ? RGB(255, 255, 255) : RGB(17, 24, 39));
			CRect textRect(itemRect);
			textRect.DeflateRect(8, 0);
			memDc.DrawText(itemText, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0)
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
		(controlId == IDC_PE_TAG_TOKEN_ADD_BUTTON);
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
		memDc.FillSolidRect(&localOuter, kEditorThemeBackgroundColor);

		const bool isDisabled = (lpDrawItemStruct->itemState & ODS_DISABLED) != 0;
		const bool isPressed = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
		const bool isHot = (lpDrawItemStruct->itemState & ODS_HOTLIGHT) != 0;

		COLORREF fillColor = RGB(247, 248, 250);
		COLORREF borderColor = RGB(171, 180, 190);
		COLORREF textColor = RGB(17, 24, 39);
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

		if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0)
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
			double value = 1.0;
			RgbToHsv(drawR, drawG, drawB, hue, saturation, value);
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
	bool swatchEnabled = true;
	if (isRulePreviewSwatch)
	{
		if (RuleColorDraftValid)
		{
			fillColor = RGB(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB);
			swatchEnabled = true;
		}
		else
		{
			COLORREF activeColor = RGB(240, 240, 240);
			bool activeEnabled = false;
			if (ResolveRuleSwatchColor(RuleColorActiveSwatchId, activeColor, activeEnabled))
			{
				fillColor = activeColor;
				swatchEnabled = true;
			}
			else
			{
				fillColor = RGB(240, 240, 240);
				swatchEnabled = false;
			}
		}
	}
	else if (isRuleSwatch)
	{
		COLORREF ruleColor = RGB(240, 240, 240);
		if (ResolveRuleSwatchColor(controlId, ruleColor, swatchEnabled))
			fillColor = ruleColor;
		else
			swatchEnabled = false;
	}

	CBrush fillBrush(fillColor);
	CPen borderPen(PS_SOLID, 1, swatchEnabled ? RGB(186, 186, 186) : RGB(200, 200, 200));
	CBrush* oldBrush = memDc.SelectObject(&fillBrush);
	CPen* oldPen = memDc.SelectObject(&borderPen);
	memDc.RoundRect(&roundedRect, CPoint(10, 10));
	memDc.SelectObject(oldPen);
	memDc.SelectObject(oldBrush);

	if (isRuleSwatch && controlId == RuleColorActiveSwatchId)
	{
		CRect activeRect(localOuter);
		activeRect.DeflateRect(1, 1);
		CPen activePen(PS_SOLID, 1, RGB(64, 132, 230));
		CPen* oldActivePen = memDc.SelectObject(&activePen);
		CBrush* oldActiveBrush = static_cast<CBrush*>(memDc.SelectStockObject(HOLLOW_BRUSH));
		memDc.RoundRect(&activeRect, CPoint(10, 10));
		memDc.SelectObject(oldActiveBrush);
		memDc.SelectObject(oldActivePen);
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

void CProfileEditorDialog::CreateEditorControls()
{
	if (ControlsCreated)
		return;

	const DWORD commonEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;

	PageTabs.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, CRect(0, 0, 0, 0), this, IDC_PE_TAB);
	PageTabs.InsertItem(0, "Colors");
	PageTabs.InsertItem(1, "Icons");
	PageTabs.InsertItem(2, "Rules");
	PageTabs.InsertItem(3, "Tags");

	ColorLeftPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_LEFT_PANEL);
	ColorRightPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_RIGHT_PANEL);
	ColorPathTree.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_TREE);
	ColorPathLabel.Create("Colors", WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_PATH_LABEL);
	SelectedPathText.Create("Selected:", WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_PE_SELECTED_PATH);
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

	IconStyleArrow.Create("Arrow", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_ARROW);
	IconStyleDiamond.Create("Diamond", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_DIAMOND);
	IconStyleRealistic.Create("Realistic", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_REALISTIC);
	IconPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_ICON_PANEL);
	IconSeparator1.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR1);
	IconSeparator2.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR2);
	IconSeparator3.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, CRect(0, 0, 0, 0), this, IDC_PE_ICON_SEPARATOR3);
	FixedPixelCheck.Create("Fixed Pixel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_PIXEL_CHECK);
	FixedScaleLabel.Create("Fixed Size", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_LABEL);
	FixedScaleValueLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_VALUE);
	FixedScaleSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_SLIDER);
	FixedScaleTickMinLabel.Create("0.10x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MIN);
	FixedScaleTickMidLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MID);
	FixedScaleTickMaxLabel.Create("2.00x", WS_CHILD | WS_VISIBLE | SS_RIGHT, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_TICK_MAX);
	FixedScaleCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_COMBO);
	SmallBoostCheck.Create("Small Icon Boost", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_SMALL_BOOST_CHECK);
	BoostFactorLabel.Create("Boost Factor", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_LABEL);
	BoostFactorValueLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_VALUE);
	BoostFactorSlider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_SLIDER);
	BoostFactorTickMinLabel.Create("0.10x", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MIN);
	BoostFactorTickMidLabel.Create("1.00x", WS_CHILD | WS_VISIBLE | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MID);
	BoostFactorTickMaxLabel.Create("2.00x", WS_CHILD | WS_VISIBLE | SS_RIGHT, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_TICK_MAX);
	BoostFactorCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_COMBO);
	BoostResolutionLabel.Create("Resolution", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_LABEL);
	BoostResolutionCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_COMBO);
	IconPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	RuleLeftPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LEFT_PANEL);
	RuleRightPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_RULE_RIGHT_PANEL);
	RuleLeftHeader.Create("Rules", WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LEFT_HEADER);
	RuleRightHeader.Create("Rule Details", WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_PE_RULE_RIGHT_HEADER);
	RulesList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LIST);
	RuleTree.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TREE);
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
	RuleColorPreviewLabel.Create("Live Preview", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_PREVIEW_LABEL);
	RuleColorPreviewSwatch.Create("", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_PREVIEW_SWATCH);
	RuleColorApplyButton.Create("Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_APPLY_BUTTON);
	RuleColorResetButton.Create("Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_RULE_COLOR_RESET_BUTTON);
	RulesList.SetItemHeight(0, 24);
	RuleTree.SetIndent(16);
	RuleTree.SendMessage(TVM_SETITEMHEIGHT, 22, 0);
	RuleTree.SetBkColor(kEditorThemeBackgroundColor);
	RuleTree.SetTextColor(RGB(17, 24, 39));

	TagPanel.Create("", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, CRect(0, 0, 0, 0), this, IDC_PE_TAG_PANEL);
	TagHeaderPanel.Create("Tags", WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_PE_TAG_HEADER_PANEL);
	TagTypeLabel.Create("Type", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TYPE_LABEL);
	TagTypeCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TYPE_COMBO);
	TagStatusLabel.Create("Status", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_STATUS_LABEL);
	TagStatusCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_STATUS_COMBO);
	TagTokenLabel.Create("Add", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_LABEL);
	TagTokenCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_COMBO);
	TagAddTokenButton.Create("Insert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, IDC_PE_TAG_TOKEN_ADD_BUTTON);
	TagDefinitionHeader.Create("Definition", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DEF_HEADER);
	TagLine1Label.Create("L1", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE1_LABEL);
	TagLine1Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE1_EDIT);
	TagLine2Label.Create("L2", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE2_LABEL);
	TagLine2Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE2_EDIT);
	TagLine3Label.Create("L3", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE3_LABEL);
	TagLine3Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE3_EDIT);
	TagLine4Label.Create("L4", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE4_LABEL);
	TagLine4Edit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINE4_EDIT);
	TagLinkDetailedToggle.Create("Custom Hover Detailes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_FLAT, CRect(0, 0, 0, 0), this, IDC_PE_TAG_LINK_DETAILED);
	TagDetailedHeader.Create("Definition Detailed", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_TAG_DETAILED_HEADER);
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

	TagTypeCombo.ResetContent();
	TagTypeCombo.AddString("departure");
	TagTypeCombo.AddString("arrival");
	TagTypeCombo.AddString("airborne");
	TagTypeCombo.SetCurSel(0);
	TagLinkDetailedToggle.SetCheck(BST_UNCHECKED);
	PopulateTagTokenCombo();

	PopulateIconCombos();
	PopulateRuleCombos();
	HeaderBarBrush.CreateSolidBrush(kEditorThemeBackgroundColor);
	FixedScaleSlider.SetRange(10, 200, TRUE);
	FixedScaleSlider.SetTicFreq(5);
	FixedScaleSlider.SetLineSize(1);
	FixedScaleSlider.SetPageSize(10);
	BoostFactorSlider.SetRange(10, 200, TRUE);
	BoostFactorSlider.SetTicFreq(5);
	BoostFactorSlider.SetLineSize(1);
	BoostFactorSlider.SetPageSize(10);
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
	ColorPathTree.SetIndent(16);
	ColorPathTree.SetBkColor(kEditorThemeBackgroundColor);
	ColorPathTree.SetTextColor(RGB(17, 24, 39));
	if (GetFont() != nullptr)
	{
		LOGFONT lf = {};
		GetFont()->GetLogFont(&lf);
		LOGFONT monoLf = lf;
		strcpy_s(monoLf.lfFaceName, LF_FACESIZE, "Consolas");
		MonoFont.CreateFontIndirect(&monoLf);

		// Apply a unified font to all editor controls.
		for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetNextWindow())
		{
			if (::IsWindow(child->GetSafeHwnd()))
				child->SetFont(&MonoFont, TRUE);
		}
	}
	ApplyThemedEditBorders();
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
		if (gThemedEditOldProcs.find(hwnd) != gThemedEditOldProcs.end())
		{
			DrawThemedBorder(hwnd);
			return;
		}

		WNDPROC oldProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ThemedEditWndProc)));
		if (oldProc == nullptr)
			return;

		gThemedEditOldProcs[hwnd] = oldProc;
		DrawThemedBorder(hwnd);
	};

	attachBorder(EditRgba);
	attachBorder(EditHex);
	attachBorder(RuleNameEdit);
	attachBorder(RuleTargetEdit);
	attachBorder(RuleTagEdit);
	attachBorder(RuleTextEdit);
	attachBorder(TagLine1Edit);
	attachBorder(TagLine2Edit);
	attachBorder(TagLine3Edit);
	attachBorder(TagLine4Edit);
	attachBorder(TagDetailedLine1Edit);
	attachBorder(TagDetailedLine2Edit);
	attachBorder(TagDetailedLine3Edit);
	attachBorder(TagDetailedLine4Edit);
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
	else if (normalizedSource == "custom")
	{
		tokens = { "asid", "ssid", "deprwy", "seprwy", "arvrwy", "srvrwy" };
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
	RuleSourceCombo.AddString("vacdm");
	RuleSourceCombo.AddString("runway");
	RuleSourceCombo.AddString("custom");
	RuleSourceCombo.SetCurSel(0);

	RuleTypeCombo.ResetContent();
	const char* typeOptions[] = { "any", "departure", "arrival", "airborne", "uncorrelated" };
	for (const char* value : typeOptions)
		RuleTypeCombo.AddString(value);
	RuleTypeCombo.SetCurSel(0);

	RuleStatusCombo.ResetContent();
	const char* statusOptions[] = { "any", "default", "nofpl", "nsts", "push", "stup", "taxi", "depa", "arr", "airdep", "airarr", "airdep_onrunway", "airarr_onrunway" };
	for (const char* value : statusOptions)
		RuleStatusCombo.AddString(value);
	RuleStatusCombo.SetCurSel(0);

	RuleDetailCombo.ResetContent();
	RuleDetailCombo.AddString("any");
	RuleDetailCombo.AddString("normal");
	RuleDetailCombo.AddString("detailed");
	RuleDetailCombo.SetCurSel(0);

	PopulateRuleTokenCombo("vacdm", "tobt");
	PopulateRuleConditionCombo("vacdm", "tobt", "any");
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
	else if (normalizedSource == "custom")
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

void CProfileEditorDialog::LayoutControls()
{
	if (!ControlsCreated)
		return;

	CRect clientRect;
	GetClientRect(&clientRect);

	const int pad = 8;
	const int tabsTop = pad;

	PageTabs.MoveWindow(pad, tabsTop, max(120, clientRect.Width() - (pad * 2)), max(120, clientRect.Height() - tabsTop - pad), TRUE);

	CRect pageRect;
	PageTabs.GetWindowRect(&pageRect);
	ScreenToClient(&pageRect);
	PageTabs.AdjustRect(FALSE, &pageRect);

	const int innerPad = 10;
	const int colorGap = 12;
	const int availableWidth = max(200, pageRect.Width() - (innerPad * 2) - colorGap);
	const int colorLeftWidth = max(230, static_cast<int>(availableWidth * 0.46));
	const int colorRightWidth = max(170, availableWidth - colorLeftWidth);
	const int colorTop = pageRect.top + innerPad;
	const int panelHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int colorLeft = pageRect.left + innerPad;
	const int rightLeft = colorLeft + colorLeftWidth + colorGap;

	const int rowHeight = 24;
	const int buttonHeight = 28;

	// Colors: left tree panel + right editor panel
	ColorLeftPanel.MoveWindow(colorLeft, colorTop, colorLeftWidth, panelHeight, TRUE);
	ColorRightPanel.MoveWindow(rightLeft, colorTop, colorRightWidth, panelHeight, TRUE);
	ColorPathLabel.MoveWindow(colorLeft + 1, colorTop + 1, max(60, colorLeftWidth - 2), 30, TRUE);
	const int colorTreeTop = colorTop + 32;
	const int colorTreeHeight = max(80, panelHeight - 34);
	ColorPathTree.MoveWindow(colorLeft + 8, colorTreeTop + 4, max(80, colorLeftWidth - 16), max(60, colorTreeHeight - 12), TRUE);

	SelectedPathText.MoveWindow(rightLeft + 1, colorTop + 1, max(60, colorRightWidth - 2), 30, TRUE);

	int y = colorTop + 40;
	const int previewWidth = max(120, colorRightWidth - 28);
	ColorPreviewLabel.MoveWindow(rightLeft + 14, y, previewWidth, rowHeight, TRUE);
	y += rowHeight + 4;

	ColorPreviewSwatch.MoveWindow(rightLeft + 14, y, previewWidth, 52, TRUE);
	y += 52 + 10;

	const int sliderGap = 16;
	const int sliderWidth = 24;
	const int sliderColumnsWidth = (sliderWidth * 2) + sliderGap;
	const int wheelToSliderGap = 10;
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
	y = wheelTop + wheelSize + 10;

	const int rgbaLabelWidth = 56;
	LabelRgba.MoveWindow(rightLeft + 14, y + 3, rgbaLabelWidth, rowHeight, TRUE);
	EditRgba.MoveWindow(rightLeft + 14 + rgbaLabelWidth + 6, y, max(120, previewWidth - rgbaLabelWidth - 6), rowHeight, TRUE);
	y += rowHeight + 10;

	LabelHex.MoveWindow(rightLeft + 14, y + 3, rgbaLabelWidth, rowHeight, TRUE);
	EditHex.MoveWindow(rightLeft + 14 + rgbaLabelWidth + 6, y, max(120, previewWidth - rgbaLabelWidth - 6), rowHeight, TRUE);
	y += rowHeight + 14;

	ApplyColorButton.MoveWindow(rightLeft + 14, y, 60, buttonHeight, TRUE);
	ResetColorButton.MoveWindow(rightLeft + 82, y, 60, buttonHeight, TRUE);

	const int iconLeft = pageRect.left + innerPad;
	const int iconTop = pageRect.top + innerPad;
	const int iconWidth = max(220, pageRect.Width() - (innerPad * 2));
	const int iconHeight = max(140, pageRect.Height() - (innerPad * 2));
	IconPanel.MoveWindow(iconLeft, iconTop, iconWidth, iconHeight, TRUE);

	const int iconPad = 12;
	const int iconContentLeft = iconLeft + iconPad;
	const int iconContentRight = iconLeft + iconWidth - iconPad;
	const int iconContentWidth = max(120, iconContentRight - iconContentLeft);
	const int iconCheckboxHeight = 22;
	const int iconSliderHeight = 24;
	const int iconTickHeight = 16;
	const int iconTickWidth = 54;

	int iconY = iconTop + 12;
	IconStyleArrow.MoveWindow(iconContentLeft, iconY, 76, iconCheckboxHeight, TRUE);
	IconStyleDiamond.MoveWindow(iconContentLeft + 84, iconY, 92, iconCheckboxHeight, TRUE);
	IconStyleRealistic.MoveWindow(iconContentLeft + 186, iconY, 96, iconCheckboxHeight, TRUE);
	iconY += iconCheckboxHeight + 10;

	IconSeparator1.MoveWindow(iconLeft + 1, iconY, max(50, iconWidth - 2), 2, TRUE);
	iconY += 12;

	FixedPixelCheck.MoveWindow(iconContentLeft, iconY, 140, iconCheckboxHeight, TRUE);
	iconY += iconCheckboxHeight + 6;

	FixedScaleLabel.MoveWindow(iconContentLeft, iconY + 2, 110, rowHeight, TRUE);
	FixedScaleValueLabel.MoveWindow(iconContentLeft + 110, iconY + 2, 70, rowHeight, TRUE);
	iconY += rowHeight + 2;
	FixedScaleSlider.MoveWindow(iconContentLeft, iconY, iconContentWidth, iconSliderHeight, TRUE);
	iconY += iconSliderHeight + 2;
	FixedScaleTickMinLabel.MoveWindow(iconContentLeft, iconY, iconTickWidth, iconTickHeight, TRUE);
	FixedScaleTickMidLabel.MoveWindow(iconContentLeft + (iconContentWidth / 2) - (iconTickWidth / 2), iconY, iconTickWidth, iconTickHeight, TRUE);
	FixedScaleTickMaxLabel.MoveWindow(iconContentRight - iconTickWidth, iconY, iconTickWidth, iconTickHeight, TRUE);
	iconY += iconTickHeight + 8;

	IconSeparator2.MoveWindow(iconLeft + 1, iconY, max(50, iconWidth - 2), 2, TRUE);
	iconY += 12;

	SmallBoostCheck.MoveWindow(iconContentLeft, iconY, 160, iconCheckboxHeight, TRUE);
	iconY += iconCheckboxHeight + 6;

	BoostFactorLabel.MoveWindow(iconContentLeft, iconY + 2, 110, rowHeight, TRUE);
	BoostFactorValueLabel.MoveWindow(iconContentLeft + 110, iconY + 2, 70, rowHeight, TRUE);
	iconY += rowHeight + 2;
	BoostFactorSlider.MoveWindow(iconContentLeft, iconY, iconContentWidth, iconSliderHeight, TRUE);
	iconY += iconSliderHeight + 2;
	BoostFactorTickMinLabel.MoveWindow(iconContentLeft, iconY, iconTickWidth, iconTickHeight, TRUE);
	BoostFactorTickMidLabel.MoveWindow(iconContentLeft + (iconContentWidth / 2) - (iconTickWidth / 2), iconY, iconTickWidth, iconTickHeight, TRUE);
	BoostFactorTickMaxLabel.MoveWindow(iconContentRight - iconTickWidth, iconY, iconTickWidth, iconTickHeight, TRUE);
	iconY += iconTickHeight + 8;

	IconSeparator3.MoveWindow(iconLeft + 1, iconY, max(50, iconWidth - 2), 2, TRUE);
	iconY += 12;

	BoostResolutionLabel.MoveWindow(iconContentLeft, iconY + 2, 100, rowHeight, TRUE);
	iconY += rowHeight + 4;
	BoostResolutionCombo.MoveWindow(iconContentLeft, iconY, iconContentWidth, rowHeight + 220, TRUE);
	FixedScaleCombo.MoveWindow(-5000, -5000, 10, 10, TRUE);
	BoostFactorCombo.MoveWindow(-5000, -5000, 10, 10, TRUE);

	const int rulesTop = pageRect.top + innerPad;
	const int rulesPanelHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int rulesGap = 12;
	const int rulesAvailableWidth = max(220, pageRect.Width() - (innerPad * 2) - rulesGap);
	const int rulesLeftWidth = max(220, static_cast<int>(rulesAvailableWidth * 0.50));
	const int rulesRightWidth = max(220, rulesAvailableWidth - rulesLeftWidth);
	const int rulesLeft = pageRect.left + innerPad;
	const int rulesRightLeft = rulesLeft + rulesLeftWidth + rulesGap;

	RuleLeftPanel.MoveWindow(rulesLeft, rulesTop, rulesLeftWidth, rulesPanelHeight, TRUE);
	RuleRightPanel.MoveWindow(rulesRightLeft, rulesTop, rulesRightWidth, rulesPanelHeight, TRUE);
	RuleLeftHeader.MoveWindow(rulesLeft + 1, rulesTop + 1, max(60, rulesLeftWidth - 2), 30, TRUE);
	RuleRightHeader.MoveWindow(rulesRightLeft + 1, rulesTop + 1, max(60, rulesRightWidth - 2), 30, TRUE);

	const int ruleListTop = rulesTop + 36;
	const int ruleButtonAreaHeight = 44;
	const int ruleListHeight = max(80, rulesPanelHeight - 36 - ruleButtonAreaHeight - 8);
	const int ruleListWidth = max(90, rulesLeftWidth - 16);
	RulesList.MoveWindow(-5000, -5000, 10, 10, TRUE);
	RuleTree.MoveWindow(rulesLeft + 8, ruleListTop, ruleListWidth, ruleListHeight, TRUE);
	const int ruleButtonsY = rulesTop + rulesPanelHeight - 38;
	RuleAddButton.MoveWindow(rulesLeft + 12, ruleButtonsY, 84, buttonHeight, TRUE);
	RuleAddParameterButton.MoveWindow(-5000, -5000, 10, 10, TRUE);
	RuleRemoveButton.MoveWindow(-5000, -5000, 10, 10, TRUE);

	int rulesY = rulesTop + 44;
	const int rulesLabelWidth = 120;
	const int rulesContentLeft = rulesRightLeft + 14;
	const int rulesContentWidth = max(120, rulesRightWidth - 28);
	const int rulesFieldLeft = rulesContentLeft + rulesLabelWidth + 10;
	const int rulesLabelLeft = rulesContentLeft;
	const int rulesFieldWidth = max(90, rulesRightWidth - (rulesFieldLeft - rulesRightLeft) - 14);
	int paramY = rulesY;
	RuleSourceLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleSourceCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);
	paramY += rowHeight + 10;
	RuleTokenLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTokenCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);
	paramY += rowHeight + 10;
	RuleConditionLabel.MoveWindow(rulesLabelLeft, paramY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleConditionCombo.MoveWindow(rulesFieldLeft, paramY, rulesFieldWidth, rowHeight + 220, TRUE);

	int effectY = rulesY;
	RuleNameLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleNameEdit.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight, TRUE);
	effectY += rowHeight + 10;
	RuleTypeLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTypeCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 10;
	RuleStatusLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleStatusCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 10;
	RuleDetailLabel.MoveWindow(rulesLabelLeft, effectY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleDetailCombo.MoveWindow(rulesFieldLeft, effectY, rulesFieldWidth, rowHeight + 220, TRUE);
	effectY += rowHeight + 14;

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
	const int ruleWheelSize = max(78, min(140, rulesContentWidth - ruleWheelSliderWidth - ruleWheelSliderGap));
	const int ruleWheelGroupWidth = ruleWheelSize + ruleWheelSliderGap + ruleWheelSliderWidth;
	const int ruleWheelLeft = rulesContentLeft + max(0, (rulesContentWidth - ruleWheelGroupWidth) / 2);
	const int ruleWheelTop = ruleWheelLabelTop + rowHeight + 2;
	const int ruleWheelSliderLeft = ruleWheelLeft + ruleWheelSize + ruleWheelSliderGap;
	const int ruleValueLabelWidth = 40;
	const int ruleValueLabelLeft = ruleWheelSliderLeft - ((ruleValueLabelWidth - ruleWheelSliderWidth) / 2);
	RuleColorWheelLabel.MoveWindow(ruleWheelLeft, ruleWheelLabelTop, max(60, ruleWheelSize), rowHeight, TRUE);
	RuleColorWheel.MoveWindow(ruleWheelLeft, ruleWheelTop, ruleWheelSize, ruleWheelSize, TRUE);
	RuleColorValueLabel.MoveWindow(ruleValueLabelLeft, ruleWheelLabelTop, ruleValueLabelWidth, rowHeight, TRUE);
	RuleColorValueSlider.MoveWindow(ruleWheelSliderLeft, ruleWheelTop, ruleWheelSliderWidth, ruleWheelSize, TRUE);
	effectY = ruleWheelTop + ruleWheelSize + 10;

	const int rulePreviewWidth = max(130, min(rulesContentWidth, 220));
	const int rulePreviewLeft = rulesContentLeft + max(0, (rulesContentWidth - rulePreviewWidth) / 2);
	RuleColorPreviewLabel.MoveWindow(rulePreviewLeft, effectY, rulePreviewWidth, rowHeight, TRUE);
	effectY += rowHeight + 4;
	RuleColorPreviewSwatch.MoveWindow(rulePreviewLeft, effectY, rulePreviewWidth, 44, TRUE);
	effectY += 44 + 10;

	const int ruleActionButtonsWidth = 60 + 8 + 60;
	const int ruleActionLeft = rulesContentLeft + max(0, (rulesContentWidth - ruleActionButtonsWidth) / 2);
	RuleColorApplyButton.MoveWindow(ruleActionLeft, effectY, 60, buttonHeight, TRUE);
	RuleColorResetButton.MoveWindow(ruleActionLeft + 68, effectY, 60, buttonHeight, TRUE);

	const int tagLeft = pageRect.left + innerPad;
	const int tagTop = pageRect.top + innerPad;
	const int tagWidth = max(240, pageRect.Width() - (innerPad * 2));
	const int tagHeight = max(120, pageRect.Height() - (innerPad * 2));
	TagPanel.MoveWindow(tagLeft, tagTop, tagWidth, tagHeight, TRUE);
	TagHeaderPanel.MoveWindow(tagLeft + 1, tagTop + 1, max(60, tagWidth - 2), 30, TRUE);

	const int tagPad = 12;
	const int tagLabelWidth = 52;
	const int tagContentLeft = tagLeft + tagPad;
	const int tagFieldLeft = tagContentLeft + tagLabelWidth + 10;
	const int tagRight = tagLeft + tagWidth - tagPad;
	const int baseFieldWidth = max(110, tagRight - tagFieldLeft);
	const int tokenButtonWidth = 62;
	const int tokenComboWidth = max(90, baseFieldWidth - tokenButtonWidth - 8);

	int tagY = tagTop + 42;
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

	TagLine1Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagLine1Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 8;

	TagLine2Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagLine2Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 8;

	TagLine3Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagLine3Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 8;

	TagLine4Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagLine4Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 10;

	TagLinkDetailedToggle.MoveWindow(tagContentLeft, tagY, max(180, tagWidth - (tagPad * 2)), rowHeight, TRUE);
	tagY += rowHeight + 8;

	TagDetailedHeader.MoveWindow(tagContentLeft, tagY, max(100, tagWidth - (tagPad * 2)), rowHeight, TRUE);
	tagY += rowHeight + 6;

	TagDetailedLine1Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagDetailedLine1Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 6;

	TagDetailedLine2Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagDetailedLine2Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 6;

	TagDetailedLine3Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagDetailedLine3Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);
	tagY += rowHeight + 6;

	TagDetailedLine4Label.MoveWindow(tagContentLeft, tagY + 4, 26, rowHeight, TRUE);
	TagDetailedLine4Edit.MoveWindow(tagContentLeft + 34, tagY, max(120, tagWidth - (tagPad * 2) - 34), rowHeight, TRUE);

	// Keep live preview generated, but hidden to match the compact editor layout.
	TagPreviewLabel.MoveWindow(-5000, -5000, 10, 10, TRUE);
	TagPreviewEdit.MoveWindow(-5000, -5000, 10, 10, TRUE);

	UpdatePageVisibility();
}

void CProfileEditorDialog::UpdatePageVisibility()
{
	if (!ControlsCreated)
		return;

	const int selectedTab = PageTabs.GetCurSel();
	const bool showColors = (selectedTab == 0);
	const bool showIcons = (selectedTab == 1);
	const bool showRules = (selectedTab == 2);
	const bool showTagEditor = (selectedTab == 3);
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

	ColorLeftPanel.ShowWindow(colorShowMode);
	ColorRightPanel.ShowWindow(colorShowMode);
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

	IconStyleArrow.ShowWindow(iconShowMode);
	IconStyleDiamond.ShowWindow(iconShowMode);
	IconStyleRealistic.ShowWindow(iconShowMode);
	IconPanel.ShowWindow(iconShowMode);
	IconSeparator1.ShowWindow(iconShowMode);
	IconSeparator2.ShowWindow(iconShowMode);
	IconSeparator3.ShowWindow(iconShowMode);
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
	BoostResolutionCombo.ShowWindow(iconShowMode);

	RulesList.ShowWindow(SW_HIDE);
	RuleTree.ShowWindow(ruleShowMode);
	RuleLeftPanel.ShowWindow(ruleShowMode);
	RuleRightPanel.ShowWindow(ruleShowMode);
	RuleLeftHeader.ShowWindow(ruleShowMode);
	RuleRightHeader.ShowWindow(ruleShowMode);
	RuleAddButton.ShowWindow(ruleShowMode);
	RuleAddParameterButton.ShowWindow(SW_HIDE);
	RuleRemoveButton.ShowWindow(SW_HIDE);
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
	RuleColorPreviewLabel.ShowWindow(ruleEffectShowMode);
	RuleColorPreviewSwatch.ShowWindow(ruleEffectShowMode);
	RuleColorApplyButton.ShowWindow(ruleEffectShowMode);
	RuleColorResetButton.ShowWindow(ruleEffectShowMode);

	TagPanel.ShowWindow(tagShowMode);
	TagHeaderPanel.ShowWindow(tagShowMode);
	TagTypeLabel.ShowWindow(tagShowMode);
	TagTypeCombo.ShowWindow(tagShowMode);
	TagStatusLabel.ShowWindow(tagShowMode);
	TagStatusCombo.ShowWindow(tagShowMode);
	TagTokenLabel.ShowWindow(tagShowMode);
	TagTokenCombo.ShowWindow(tagShowMode);
	TagAddTokenButton.ShowWindow(tagShowMode);
	TagDefinitionHeader.ShowWindow(tagShowMode);
	TagLine1Label.ShowWindow(tagShowMode);
	TagLine1Edit.ShowWindow(tagShowMode);
	TagLine2Label.ShowWindow(tagShowMode);
	TagLine2Edit.ShowWindow(tagShowMode);
	TagLine3Label.ShowWindow(tagShowMode);
	TagLine3Edit.ShowWindow(tagShowMode);
	TagLine4Label.ShowWindow(tagShowMode);
	TagLine4Edit.ShowWindow(tagShowMode);
	TagLinkDetailedToggle.ShowWindow(tagShowMode);
	TagDetailedHeader.ShowWindow(detailedTagShowMode);
	TagDetailedLine1Label.ShowWindow(detailedTagShowMode);
	TagDetailedLine1Edit.ShowWindow(detailedTagShowMode);
	TagDetailedLine2Label.ShowWindow(detailedTagShowMode);
	TagDetailedLine2Edit.ShowWindow(detailedTagShowMode);
	TagDetailedLine3Label.ShowWindow(detailedTagShowMode);
	TagDetailedLine3Edit.ShowWindow(detailedTagShowMode);
	TagDetailedLine4Label.ShowWindow(detailedTagShowMode);
	TagDetailedLine4Edit.ShowWindow(detailedTagShowMode);
	TagPreviewLabel.ShowWindow(SW_HIDE);
	TagPreviewEdit.ShowWindow(SW_HIDE);
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
				HTREEITEM item = ColorPathTree.InsertItem(segments[i].c_str(), parent);
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

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(DraftColorR, DraftColorG, DraftColorB, hue, saturation, value);

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

	CString selectedLabel;
	selectedLabel.Format("Selected: %s", selectedPath.empty() ? "(none)" : selectedPath.c_str());
	SelectedPathText.SetWindowTextA(selectedLabel);

	if (!DraftColorValid)
		LoadDraftColorFromSelection();
	UpdateDraftColorControls();
}

bool CProfileEditorDialog::TryParseRgbaQuad(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const
{
	std::vector<int> values;
	values.reserve(4);
	int current = 0;
	bool inNumber = false;

	auto flushNumber = [&]()
	{
		if (!inNumber)
			return;
		values.push_back(current);
		current = 0;
		inNumber = false;
	};

	for (char ch : text)
	{
		if (std::isdigit(static_cast<unsigned char>(ch)))
		{
			inNumber = true;
			current = (current * 10) + (ch - '0');
			continue;
		}
		if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
		{
			flushNumber();
			continue;
		}
		return false;
	}
	flushNumber();

	if (values.size() != 3 && values.size() != 4)
		return false;
	for (int value : values)
	{
		if (value < 0 || value > 255)
			return false;
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
	if (pos < 10)
		pos = 10;
	if (pos > 200)
		pos = 200;
	return static_cast<double>(pos) / 100.0;
}

int CProfileEditorDialog::ScaleToSliderPos(double scale) const
{
	int pos = static_cast<int>(std::lround(scale * 100.0));
	if (pos < 10)
		pos = 10;
	if (pos > 200)
		pos = 200;
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
	IconStyleArrow.SetCheck(style == "triangle" ? BST_CHECKED : BST_UNCHECKED);
	IconStyleDiamond.SetCheck(style == "diamond" ? BST_CHECKED : BST_UNCHECKED);
	IconStyleRealistic.SetCheck(style == "realistic" ? BST_CHECKED : BST_UNCHECKED);

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
		SelectComboEntryByText(BoostResolutionCombo, "2K");
	else if (preset == "4k")
		SelectComboEntryByText(BoostResolutionCombo, "4K");
	else
		SelectComboEntryByText(BoostResolutionCombo, "1080p");

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
		ruleLabel.Format("%s  (%d)", ruleName.c_str(), conditionCount);
		const HTREEITEM ruleItem = RuleTree.InsertItem(ruleLabel);
		RuleTreeSelectionMap[ruleItem] = std::make_pair(static_cast<int>(i), -1);

		std::vector<StructuredTagColorRule::Criterion> criteria = rule.criteria;
		if (criteria.empty())
			criteria.push_back(StructuredTagColorRule::Criterion{ rule.source, rule.token, rule.condition });

		for (size_t c = 0; c < criteria.size(); ++c)
		{
			const StructuredTagColorRule::Criterion& criterion = criteria[c];
			CString criterionLabel;
			criterionLabel.Format("-- %s.%s  =  %s", criterion.source.c_str(), criterion.token.c_str(), criterion.condition.c_str());
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
	showDelete = true;

	CRect itemRect;
	if (!RuleTree.GetItemRect(item, &itemRect, TRUE))
		return false;

	CRect treeRect;
	RuleTree.GetClientRect(&treeRect);
	const int btnSize = 14;
	const int gap = 6;
	const int y = itemRect.top + max(0, ((itemRect.Height() - btnSize) / 2));
	int right = treeRect.right - 8;

	deleteRect = CRect(right - btnSize, y, right, y + btnSize);
	right -= (btnSize + gap);
	if (showAdd)
		addRect = CRect(right - btnSize, y, right, y + btnSize);

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

void CProfileEditorDialog::SyncRuleColorEditorFromActiveControl()
{
	RuleColorDraftValid = false;
	RuleColorDraftDirty = false;
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
	CString colorText;
	edit->GetWindowText(colorText);
	if (!TryParseRgbTriplet(std::string(colorText.GetString()), r, g, b))
	{
		const StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
		if (RuleColorActiveSwatchId == IDC_PE_RULE_TARGET_SWATCH)
		{
			r = rule.targetR; g = rule.targetG; b = rule.targetB;
		}
		else if (RuleColorActiveSwatchId == IDC_PE_RULE_TAG_SWATCH)
		{
			r = rule.tagR; g = rule.tagG; b = rule.tagB;
		}
		else
		{
			r = rule.textR; g = rule.textG; b = rule.textB;
		}
	}

	RuleColorDraftR = r;
	RuleColorDraftG = g;
	RuleColorDraftB = b;
	RuleColorDraftValid = true;
	RuleColorDraftDirty = false;
	SyncRuleColorValueSliderFromDraft();
	InvalidateRuleColorSwatches();
}

void CProfileEditorDialog::ApplyRuleColorValueFromSlider()
{
	if (UpdatingControls || !RuleColorDraftValid)
		return;
	if (!::IsWindow(RuleColorValueSlider.GetSafeHwnd()))
		return;

	double hue = 0.0;
	double saturation = 0.0;
	double value = 1.0;
	RgbToHsv(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB, hue, saturation, value);
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
	SetEditTextPreserveCaret(*edit, FormatRgbTriplet(RuleColorDraftR, RuleColorDraftG, RuleColorDraftB));
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
		SelectComboEntryByText(RuleTypeCombo, "any");
		SelectComboEntryByText(RuleStatusCombo, "any");
		SelectComboEntryByText(RuleDetailCombo, "any");
		RuleTargetCheck.SetCheck(BST_UNCHECKED);
		RuleTagCheck.SetCheck(BST_UNCHECKED);
		RuleTextCheck.SetCheck(BST_UNCHECKED);
		SetEditTextPreserveCaret(RuleTargetEdit, "255,255,255");
		SetEditTextPreserveCaret(RuleTagEdit, "255,255,255");
		SetEditTextPreserveCaret(RuleTextEdit, "255,255,255");
		RuleColorDraftValid = false;
		RuleColorDraftDirty = false;
		RuleColorActiveSwatchId = IDC_PE_RULE_TARGET_SWATCH;
		RuleColorValueSlider.SetPos(100);
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
		SelectComboEntryByText(RuleTypeCombo, rule.tagType);
		SelectComboEntryByText(RuleStatusCombo, rule.status);
		SelectComboEntryByText(RuleDetailCombo, rule.detail);
		RuleTargetCheck.SetCheck(rule.applyTarget ? BST_CHECKED : BST_UNCHECKED);
		RuleTagCheck.SetCheck(rule.applyTag ? BST_CHECKED : BST_UNCHECKED);
		RuleTextCheck.SetCheck(rule.applyText ? BST_CHECKED : BST_UNCHECKED);
		SetEditTextPreserveCaret(RuleTargetEdit, FormatRgbTriplet(rule.targetR, rule.targetG, rule.targetB));
		SetEditTextPreserveCaret(RuleTagEdit, FormatRgbTriplet(rule.tagR, rule.tagG, rule.tagB));
		SetEditTextPreserveCaret(RuleTextEdit, FormatRgbTriplet(rule.textR, rule.textG, rule.textB));
		RuleTargetSwatch.EnableWindow(TRUE);
		RuleTagSwatch.EnableWindow(TRUE);
		RuleTextSwatch.EnableWindow(TRUE);
		RuleTargetEdit.EnableWindow(rule.applyTarget ? TRUE : FALSE);
		RuleTagEdit.EnableWindow(rule.applyTag ? TRUE : FALSE);
		RuleTextEdit.EnableWindow(rule.applyText ? TRUE : FALSE);
		RuleColorWheel.EnableWindow(TRUE);
		RuleColorValueSlider.EnableWindow(TRUE);
		RuleColorApplyButton.EnableWindow(FALSE);
		RuleColorResetButton.EnableWindow(TRUE);
	}

	UpdatingControls = false;
	if (isParameterSelection)
	{
		RuleColorDraftValid = false;
		RuleColorDraftDirty = false;
	}
	else
	{
		SyncRuleColorEditorFromActiveControl();
	}
}

void CProfileEditorDialog::SyncTagEditorControlsFromRadar()
{
	if (Owner == nullptr)
		return;
	PopulateTagTokenCombo();

	bool contextDetailed = false;
	Owner->GetTagDefinitionEditorContext(TagEditorType, contextDetailed, TagEditorStatus);
	TagEditorSeparateDetailed = !Owner->GetTagDefinitionDetailedSameAsDefinition();
	if (contextDetailed != TagEditorSeparateDetailed)
		Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);

	UpdatingControls = true;
	SelectComboEntryByText(TagTypeCombo, TagEditorType);
	TagLinkDetailedToggle.SetCheck(TagEditorSeparateDetailed ? BST_CHECKED : BST_UNCHECKED);
	UpdatingControls = false;

	RefreshTagStatusOptions();
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	UpdatePageVisibility();
}

void CProfileEditorDialog::RefreshTagStatusOptions()
{
	if (Owner == nullptr)
		return;

	const std::vector<std::string> statuses = Owner->GetTagDefinitionStatusesForType(TagEditorType);
	const std::string normalizedStatus = Owner->NormalizeTagDefinitionDepartureStatus(TagEditorStatus);

	UpdatingControls = true;
	TagStatusCombo.ResetContent();
	int selectedIndex = 0;
	for (size_t i = 0; i < statuses.size(); ++i)
	{
		TagStatusCombo.AddString(statuses[i].c_str());
		if (_stricmp(statuses[i].c_str(), normalizedStatus.c_str()) == 0)
			selectedIndex = static_cast<int>(i);
	}
	if (TagStatusCombo.GetCount() > 0)
		TagStatusCombo.SetCurSel(selectedIndex);
	UpdatingControls = false;

	if (TagStatusCombo.GetCount() > 0)
	{
		CString selectedStatus;
		TagStatusCombo.GetLBText(TagStatusCombo.GetCurSel(), selectedStatus);
		TagEditorStatus = Owner->NormalizeTagDefinitionDepartureStatus(std::string(selectedStatus.GetString()));
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
	SetEditTextPreserveCaret(TagLine1Edit, lines.size() > 0 ? lines[0] : "");
	SetEditTextPreserveCaret(TagLine2Edit, lines.size() > 1 ? lines[1] : "");
	SetEditTextPreserveCaret(TagLine3Edit, lines.size() > 2 ? lines[2] : "");
	SetEditTextPreserveCaret(TagLine4Edit, lines.size() > 3 ? lines[3] : "");
	SetEditTextPreserveCaret(TagDetailedLine1Edit, detailedLines.size() > 0 ? detailedLines[0] : "");
	SetEditTextPreserveCaret(TagDetailedLine2Edit, detailedLines.size() > 1 ? detailedLines[1] : "");
	SetEditTextPreserveCaret(TagDetailedLine3Edit, detailedLines.size() > 2 ? detailedLines[2] : "");
	SetEditTextPreserveCaret(TagDetailedLine4Edit, detailedLines.size() > 3 ? detailedLines[3] : "");
	UpdatingControls = false;
}

void CProfileEditorDialog::RefreshTagPreview()
{
	if (Owner == nullptr)
		return;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	const std::vector<std::string> definitionPreviewLines = Owner->BuildTagDefinitionPreviewLinesForContext(
		TagEditorType,
		false,
		TagEditorStatus);

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
		const std::vector<std::string> detailedPreviewLines = Owner->BuildTagDefinitionPreviewLinesForContext(
			TagEditorType,
			true,
			TagEditorStatus);
		previewText += "\r\n\r\nDefinition Detailed";
		previewText += "\r\n";
		for (size_t i = 0; i < detailedPreviewLines.size(); ++i)
		{
			previewText += detailedPreviewLines[i];
			if (i + 1 < detailedPreviewLines.size())
				previewText += "\r\n";
		}
	}

	TagPreviewEdit.SetWindowTextA(previewText.c_str());
}

bool CProfileEditorDialog::TryParseRgbTriplet(const std::string& text, int& r, int& g, int& b) const
{
	std::vector<int> values;
	values.reserve(3);
	int current = 0;
	bool inNumber = false;

	auto flushNumber = [&]()
	{
		if (!inNumber)
			return;
		values.push_back(current);
		current = 0;
		inNumber = false;
	};

	for (char ch : text)
	{
		if (std::isdigit(static_cast<unsigned char>(ch)))
		{
			inNumber = true;
			current = (current * 10) + (ch - '0');
			continue;
		}
		if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
		{
			flushNumber();
			continue;
		}
		return false;
	}
	flushNumber();

	if (values.size() != 3)
		return false;
	for (int value : values)
	{
		if (value < 0 || value > 255)
			return false;
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

std::string CProfileEditorDialog::ReadComboText(CComboBox& combo) const
{
	CString text;
	combo.GetWindowText(text);
	text.Trim();
	if (!text.IsEmpty())
		return std::string(text.GetString());

	const int index = combo.GetCurSel();
	if (index == CB_ERR)
		return "";

	combo.GetLBText(index, text);
	return std::string(text.GetString());
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
	rule.tagType = Owner->NormalizeStructuredRuleTagType(ReadComboText(const_cast<CComboBox&>(RuleTypeCombo)));
	rule.status = Owner->NormalizeStructuredRuleStatus(ReadComboText(const_cast<CComboBox&>(RuleStatusCombo)));
	rule.detail = Owner->NormalizeStructuredRuleDetail(ReadComboText(const_cast<CComboBox&>(RuleDetailCombo)));

	rule.applyTarget = (RuleTargetCheck.GetCheck() == BST_CHECKED);
	rule.applyTag = (RuleTagCheck.GetCheck() == BST_CHECKED);
	rule.applyText = (RuleTextCheck.GetCheck() == BST_CHECKED);

	CString rgbText;
	if (rule.applyTarget)
	{
		const_cast<CEdit&>(RuleTargetEdit).GetWindowText(rgbText);
		if (!TryParseRgbTriplet(std::string(rgbText.GetString()), rule.targetR, rule.targetG, rule.targetB))
			return false;
	}
	if (rule.applyTag)
	{
		const_cast<CEdit&>(RuleTagEdit).GetWindowText(rgbText);
		if (!TryParseRgbTriplet(std::string(rgbText.GetString()), rule.tagR, rule.tagG, rule.tagB))
			return false;
	}
	if (rule.applyText)
	{
		const_cast<CEdit&>(RuleTextEdit).GetWindowText(rgbText);
		if (!TryParseRgbTriplet(std::string(rgbText.GetString()), rule.textR, rule.textG, rule.textB))
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
		RefreshRuleControls();
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
		RefreshRuleControls();
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
		const bool selected = (::IsWindow(RuleTree.GetSafeHwnd()) && RuleTree.GetSelectedItem() == item);
		pTreeCd->clrText = RGB(17, 24, 39);
		pTreeCd->clrTextBk = selected ? RGB(238, 245, 255) : kEditorThemeBackgroundColor;
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
		CRect itemRowRect;
		if (RuleTree.GetItemRect(item, &itemRowRect, FALSE))
		{
			CRect treeClientRect;
			RuleTree.GetClientRect(&treeClientRect);
			itemRowRect.left = max(2, itemRowRect.left - 2);
			itemRowRect.right = max(itemRowRect.left + 8, treeClientRect.right - 2);
			itemRowRect.DeflateRect(0, 1);
			const bool selected = (RuleTree.GetSelectedItem() == item);
			CPen rowPen(PS_SOLID, 1, selected ? RGB(88, 137, 214) : RGB(214, 220, 228));
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
		if (showDelete)
			drawActionButton(deleteRect, "X");
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

	if (showDelete && deleteRect.PtInRect(clickPoint))
	{
		const auto it = RuleTreeSelectionMap.find(item);
		if (it != RuleTreeSelectionMap.end())
		{
			SelectedRuleIndex = it->second.first;
			SelectedRuleCriterionIndex = it->second.second;
			OnRuleRemoveClicked();
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

void CProfileEditorDialog::SelectRuleColorEditorTarget(UINT swatchControlId)
{
	CButton* checkBox = nullptr;
	CEdit* edit = nullptr;
	if (!GetRuleColorEditorTargetControls(swatchControlId, checkBox, edit) || checkBox == nullptr || edit == nullptr)
		return;

	RuleColorActiveSwatchId = swatchControlId;
	UpdatingControls = true;
	checkBox->SetCheck(BST_CHECKED);
	UpdatingControls = false;
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
	SyncRuleColorEditorFromActiveControl();
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

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagLinkToggleChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	TagEditorSeparateDetailed = (TagLinkDetailedToggle.GetCheck() == BST_CHECKED);
	Owner->SetTagDefinitionDetailedSameAsDefinition(!TagEditorSeparateDetailed, true);

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	UpdatePageVisibility();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagStatusChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selected = TagStatusCombo.GetCurSel();
	if (selected == CB_ERR)
		return;

	CString selectedStatus;
	TagStatusCombo.GetLBText(selected, selectedStatus);
	TagEditorStatus = Owner->NormalizeTagDefinitionDepartureStatus(std::string(selectedStatus.GetString()));
	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	RefreshTagDefinitionLines();
	RefreshTagPreview();
	Owner->RequestRefresh();
}

void CProfileEditorDialog::OnTagLineChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	CWnd* focused = GetFocus();
	if (focused == nullptr)
		return;

	int lineIndex = -1;
	bool editingDetailed = false;
	CEdit* sourceEdit = nullptr;
	switch (focused->GetDlgCtrlID())
	{
	case IDC_PE_TAG_LINE1_EDIT:
		lineIndex = 0;
		sourceEdit = &TagLine1Edit;
		break;
	case IDC_PE_TAG_LINE2_EDIT:
		lineIndex = 1;
		sourceEdit = &TagLine2Edit;
		break;
	case IDC_PE_TAG_LINE3_EDIT:
		lineIndex = 2;
		sourceEdit = &TagLine3Edit;
		break;
	case IDC_PE_TAG_LINE4_EDIT:
		lineIndex = 3;
		sourceEdit = &TagLine4Edit;
		break;
	case IDC_PE_TAG_D_LINE1_EDIT:
		lineIndex = 0;
		editingDetailed = true;
		sourceEdit = &TagDetailedLine1Edit;
		break;
	case IDC_PE_TAG_D_LINE2_EDIT:
		lineIndex = 1;
		editingDetailed = true;
		sourceEdit = &TagDetailedLine2Edit;
		break;
	case IDC_PE_TAG_D_LINE3_EDIT:
		lineIndex = 2;
		editingDetailed = true;
		sourceEdit = &TagDetailedLine3Edit;
		break;
	case IDC_PE_TAG_D_LINE4_EDIT:
		lineIndex = 3;
		editingDetailed = true;
		sourceEdit = &TagDetailedLine4Edit;
		break;
	default:
		return;
	}
	TagEditorSelectedLine = lineIndex;
	TagEditorSelectedLineDetailed = editingDetailed;

	if (editingDetailed && !TagEditorSeparateDetailed)
		return;

	CString lineText;
	sourceEdit->GetWindowText(lineText);
	const std::string newValue = lineText.GetString();

	const std::vector<std::string> existingLines = Owner->GetTagDefinitionLineStrings(
		TagEditorType,
		editingDetailed,
		4,
		true,
		TagEditorStatus);
	if (lineIndex < static_cast<int>(existingLines.size()) && existingLines[lineIndex] == newValue)
		return;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	Owner->SetTagDefinitionLineString(
		TagEditorType,
		editingDetailed,
		lineIndex,
		newValue,
		TagEditorStatus);
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
	case IDC_PE_TAG_LINE1_EDIT:
		TagEditorSelectedLine = 0;
		TagEditorSelectedLineDetailed = false;
		break;
	case IDC_PE_TAG_LINE2_EDIT:
		TagEditorSelectedLine = 1;
		TagEditorSelectedLineDetailed = false;
		break;
	case IDC_PE_TAG_LINE3_EDIT:
		TagEditorSelectedLine = 2;
		TagEditorSelectedLineDetailed = false;
		break;
	case IDC_PE_TAG_LINE4_EDIT:
		TagEditorSelectedLine = 3;
		TagEditorSelectedLineDetailed = false;
		break;
	case IDC_PE_TAG_D_LINE1_EDIT:
		TagEditorSelectedLine = 0;
		TagEditorSelectedLineDetailed = true;
		break;
	case IDC_PE_TAG_D_LINE2_EDIT:
		TagEditorSelectedLine = 1;
		TagEditorSelectedLineDetailed = true;
		break;
	case IDC_PE_TAG_D_LINE3_EDIT:
		TagEditorSelectedLine = 2;
		TagEditorSelectedLineDetailed = true;
		break;
	case IDC_PE_TAG_D_LINE4_EDIT:
		TagEditorSelectedLine = 3;
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

	CWnd* focused = GetFocus();
	if (focused != nullptr)
	{
		switch (focused->GetDlgCtrlID())
		{
		case IDC_PE_TAG_LINE1_EDIT:
			TagEditorSelectedLine = 0;
			TagEditorSelectedLineDetailed = false;
			break;
		case IDC_PE_TAG_LINE2_EDIT:
			TagEditorSelectedLine = 1;
			TagEditorSelectedLineDetailed = false;
			break;
		case IDC_PE_TAG_LINE3_EDIT:
			TagEditorSelectedLine = 2;
			TagEditorSelectedLineDetailed = false;
			break;
		case IDC_PE_TAG_LINE4_EDIT:
			TagEditorSelectedLine = 3;
			TagEditorSelectedLineDetailed = false;
			break;
		case IDC_PE_TAG_D_LINE1_EDIT:
			TagEditorSelectedLine = 0;
			TagEditorSelectedLineDetailed = true;
			break;
		case IDC_PE_TAG_D_LINE2_EDIT:
			TagEditorSelectedLine = 1;
			TagEditorSelectedLineDetailed = true;
			break;
		case IDC_PE_TAG_D_LINE3_EDIT:
			TagEditorSelectedLine = 2;
			TagEditorSelectedLineDetailed = true;
			break;
		case IDC_PE_TAG_D_LINE4_EDIT:
			TagEditorSelectedLine = 3;
			TagEditorSelectedLineDetailed = true;
			break;
		default:
			break;
		}
	}

	bool detailedLine = TagEditorSelectedLineDetailed;
	if (!TagEditorSeparateDetailed)
		detailedLine = false;

	CEdit* targetEdit = nullptr;
	if (!detailedLine)
	{
		switch (TagEditorSelectedLine)
		{
		case 0: targetEdit = &TagLine1Edit; break;
		case 1: targetEdit = &TagLine2Edit; break;
		case 2: targetEdit = &TagLine3Edit; break;
		case 3: targetEdit = &TagLine4Edit; break;
		default: targetEdit = &TagLine1Edit; TagEditorSelectedLine = 0; break;
		}
	}
	else
	{
		switch (TagEditorSelectedLine)
		{
		case 0: targetEdit = &TagDetailedLine1Edit; break;
		case 1: targetEdit = &TagDetailedLine2Edit; break;
		case 2: targetEdit = &TagDetailedLine3Edit; break;
		case 3: targetEdit = &TagDetailedLine4Edit; break;
		default: targetEdit = &TagDetailedLine1Edit; TagEditorSelectedLine = 0; break;
		}
	}

	if (targetEdit == nullptr)
		return;

	CString currentText;
	targetEdit->GetWindowText(currentText);
	std::string newLine = currentText.GetString();
	if (!newLine.empty())
		newLine += " ";
	newLine += token;

	UpdatingControls = true;
	targetEdit->SetWindowTextA(newLine.c_str());
	UpdatingControls = false;

	Owner->SetTagDefinitionEditorContext(TagEditorType, TagEditorSeparateDetailed, TagEditorStatus);
	Owner->SetTagDefinitionLineString(
		TagEditorType,
		detailedLine,
		TagEditorSelectedLine,
		newLine,
		TagEditorStatus);
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
	RuleColorApplyButton.EnableWindow(TRUE);
	InvalidateRuleColorSwatches();
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
	UpdateDraftColorControls(true, false);
}

void CProfileEditorDialog::OnTabSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	if (PageTabs.GetCurSel() == 3)
		SyncTagEditorControlsFromRadar();
	UpdatePageVisibility();
	ForceChildRepaint();
	if (pResult != nullptr)
		*pResult = 0;
}

void CProfileEditorDialog::OnIconStyleChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	std::string style = "realistic";
	if (IconStyleArrow.GetCheck() == BST_CHECKED)
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
	if (Owner->SetSmallTargetIconBoostResolutionPreset(std::string(selectedText.GetString()), true))
		Owner->RequestRefresh();
}

BEGIN_MESSAGE_MAP(CProfileEditorDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_MOVE()
	ON_WM_SIZE()
	ON_WM_HSCROLL()
	ON_WM_VSCROLL()
	ON_WM_SHOWWINDOW()
	ON_WM_DESTROY()
	ON_WM_DRAWITEM()
	ON_WM_CTLCOLOR()
	ON_MESSAGE(WM_PE_COLOR_WHEEL_TRACK, &CProfileEditorDialog::OnColorWheelTrack)
	ON_MESSAGE(WM_PE_COLOR_VALUE_TRACK, &CProfileEditorDialog::OnColorValueSliderTrack)
	ON_MESSAGE(WM_PE_COLOR_OPACITY_TRACK, &CProfileEditorDialog::OnColorOpacitySliderTrack)
	ON_MESSAGE(WM_PE_RULE_COLOR_WHEEL_TRACK, &CProfileEditorDialog::OnRuleColorWheelTrack)
	ON_MESSAGE(WM_PE_RULE_COLOR_VALUE_TRACK, &CProfileEditorDialog::OnRuleColorValueSliderTrack)
	ON_NOTIFY(TVN_SELCHANGED, IDC_PE_COLOR_TREE, &CProfileEditorDialog::OnColorTreeSelectionChanged)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_TREE, &CProfileEditorDialog::OnColorTreeCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_FIXED_SCALE_SLIDER, &CProfileEditorDialog::OnFixedScaleSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_BOOST_FACTOR_SLIDER, &CProfileEditorDialog::OnBoostFactorSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_VALUE_SLIDER, &CProfileEditorDialog::OnColorValueSliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_COLOR_OPACITY_SLIDER, &CProfileEditorDialog::OnColorOpacitySliderCustomDraw)
	ON_NOTIFY(NM_CUSTOMDRAW, IDC_PE_RULE_COLOR_VALUE_SLIDER, &CProfileEditorDialog::OnRuleColorValueSliderCustomDraw)
	ON_STN_CLICKED(IDC_PE_COLOR_WHEEL, &CProfileEditorDialog::OnColorWheelClicked)
	ON_BN_CLICKED(IDC_PE_APPLY_BUTTON, &CProfileEditorDialog::OnApplyColorClicked)
	ON_BN_CLICKED(IDC_PE_RESET_BUTTON, &CProfileEditorDialog::OnResetColorClicked)
	ON_EN_CHANGE(IDC_PE_EDIT_RGBA, &CProfileEditorDialog::OnRgbaEditChanged)
	ON_EN_CHANGE(IDC_PE_EDIT_HEX, &CProfileEditorDialog::OnHexEditChanged)
	ON_NOTIFY(TCN_SELCHANGE, IDC_PE_TAB, &CProfileEditorDialog::OnTabSelectionChanged)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_ARROW, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_DIAMOND, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_ICON_STYLE_REALISTIC, &CProfileEditorDialog::OnIconStyleChanged)
	ON_BN_CLICKED(IDC_PE_FIXED_PIXEL_CHECK, &CProfileEditorDialog::OnFixedPixelToggled)
	ON_CBN_SELCHANGE(IDC_PE_FIXED_SCALE_COMBO, &CProfileEditorDialog::OnFixedScaleChanged)
	ON_BN_CLICKED(IDC_PE_SMALL_BOOST_CHECK, &CProfileEditorDialog::OnSmallBoostToggled)
	ON_CBN_SELCHANGE(IDC_PE_BOOST_FACTOR_COMBO, &CProfileEditorDialog::OnBoostFactorChanged)
	ON_CBN_SELCHANGE(IDC_PE_BOOST_RES_COMBO, &CProfileEditorDialog::OnBoostResolutionChanged)
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
	ON_CBN_SELCHANGE(IDC_PE_TAG_TYPE_COMBO, &CProfileEditorDialog::OnTagTypeChanged)
	ON_CBN_SELCHANGE(IDC_PE_TAG_STATUS_COMBO, &CProfileEditorDialog::OnTagStatusChanged)
	ON_BN_CLICKED(IDC_PE_TAG_TOKEN_ADD_BUTTON, &CProfileEditorDialog::OnTagAddTokenClicked)
	ON_BN_CLICKED(IDC_PE_TAG_LINK_DETAILED, &CProfileEditorDialog::OnTagLinkToggleChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE1_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE2_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE3_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_LINE4_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE1_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE2_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE3_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_CHANGE(IDC_PE_TAG_D_LINE4_EDIT, &CProfileEditorDialog::OnTagLineChanged)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE1_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE2_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE3_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_LINE4_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE1_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE2_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE3_EDIT, &CProfileEditorDialog::OnTagLineFocus)
	ON_EN_SETFOCUS(IDC_PE_TAG_D_LINE4_EDIT, &CProfileEditorDialog::OnTagLineFocus)
END_MESSAGE_MAP()
