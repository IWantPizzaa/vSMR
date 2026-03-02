#include "stdafx.h"
#include "ProfileEditorDialog.hpp"
#include "SMRRadar.hpp"
#include "afxdialogex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

IMPLEMENT_DYNAMIC(CProfileEditorDialog, CDialogEx)

CProfileEditorDialog::CProfileEditorDialog(CSMRRadar* owner, CWnd* pParent /*=NULL*/)
	: CDialogEx(CProfileEditorDialog::IDD, pParent)
	, Owner(owner)
{
}

CProfileEditorDialog::~CProfileEditorDialog()
{
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

	SetDlgItemTextA(IDC_PROFILE_EDITOR_STATUS, "Slice 3 active: Colors + Icons pages are now in this detached editor.");

	CreateEditorControls();
	Initialized = true;
	SyncFromRadar();
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
	UpdatePageVisibility();
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
	NotifyWindowRectChanged();
}

void CProfileEditorDialog::CreateEditorControls()
{
	if (ControlsCreated)
		return;

	const DWORD commonEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;

	PageTabs.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, CRect(0, 0, 0, 0), this, IDC_PE_TAB);
	PageTabs.InsertItem(0, "Colors");
	PageTabs.InsertItem(1, "Icons");

	ColorPathList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, CRect(0, 0, 0, 0), this, IDC_PE_COLOR_LIST);
	SelectedPathText.Create("Selected: ", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_SELECTED_PATH);
	LabelR.Create("R", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_R);
	LabelG.Create("G", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_G);
	LabelB.Create("B", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_B);
	LabelA.Create("A", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_A);
	LabelHex.Create("HEX", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_LABEL_HEX);
	EditR.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_R);
	EditG.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_G);
	EditB.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_B);
	EditA.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_A);
	EditHex.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_EDIT_HEX);
	PickColorButton.Create("Pick...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_PICK_BUTTON);
	RefreshButton.Create("Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_REFRESH_BUTTON);

	IconStyleArrow.Create("Arrow", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_ARROW);
	IconStyleDiamond.Create("Diamond", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_DIAMOND);
	IconStyleRealistic.Create("Realistic", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_ICON_STYLE_REALISTIC);
	FixedPixelCheck.Create("Fixed Pixel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_PIXEL_CHECK);
	FixedScaleLabel.Create("Fixed Size", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_LABEL);
	FixedScaleCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_FIXED_SCALE_COMBO);
	SmallBoostCheck.Create("Small Icon Boost", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_SMALL_BOOST_CHECK);
	BoostFactorLabel.Create("Boost Factor", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_LABEL);
	BoostFactorCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_FACTOR_COMBO);
	BoostResolutionLabel.Create("Resolution", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_LABEL);
	BoostResolutionCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_BOOST_RES_COMBO);

	PopulateIconCombos();
	ControlsCreated = true;
	LayoutControls();
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

void CProfileEditorDialog::LayoutControls()
{
	if (!ControlsCreated)
		return;

	CRect clientRect;
	GetClientRect(&clientRect);

	const int pad = 10;
	const int statusHeight = 16;
	const int statusTop = 8;
	const int tabsTop = statusTop + statusHeight + 6;

	CWnd* statusWnd = GetDlgItem(IDC_PROFILE_EDITOR_STATUS);
	if (statusWnd != nullptr && ::IsWindow(statusWnd->GetSafeHwnd()))
		statusWnd->MoveWindow(pad, statusTop, max(120, clientRect.Width() - (pad * 2)), statusHeight, TRUE);

	PageTabs.MoveWindow(pad, tabsTop, max(120, clientRect.Width() - (pad * 2)), max(120, clientRect.Height() - tabsTop - pad), TRUE);

	CRect pageRect;
	PageTabs.GetWindowRect(&pageRect);
	ScreenToClient(&pageRect);
	PageTabs.AdjustRect(FALSE, &pageRect);

	const int innerPad = 10;
	const int colorListWidth = max(180, (pageRect.Width() / 2) - (innerPad * 2));
	const int colorListHeight = max(120, pageRect.Height() - (innerPad * 2));
	const int colorLeft = pageRect.left + innerPad;
	const int colorTop = pageRect.top + innerPad;
	const int rightLeft = colorLeft + colorListWidth + innerPad;
	const int rightWidth = max(170, pageRect.right - rightLeft - innerPad);

	ColorPathList.MoveWindow(colorLeft, colorTop, colorListWidth, colorListHeight, TRUE);

	const int rowHeight = 22;
	const int labelWidth = 22;
	const int editWidth = 58;
	const int controlGap = 8;
	const int buttonHeight = 24;
	int y = colorTop;

	SelectedPathText.MoveWindow(rightLeft, y, rightWidth, 34, TRUE);
	y += 38;
	LabelR.MoveWindow(rightLeft, y + 4, labelWidth, rowHeight, TRUE);
	EditR.MoveWindow(rightLeft + labelWidth, y, editWidth, rowHeight, TRUE);
	LabelG.MoveWindow(rightLeft + labelWidth + editWidth + controlGap, y + 4, labelWidth, rowHeight, TRUE);
	EditG.MoveWindow(rightLeft + labelWidth + editWidth + controlGap + labelWidth, y, editWidth, rowHeight, TRUE);
	y += rowHeight + 6;

	LabelB.MoveWindow(rightLeft, y + 4, labelWidth, rowHeight, TRUE);
	EditB.MoveWindow(rightLeft + labelWidth, y, editWidth, rowHeight, TRUE);
	LabelA.MoveWindow(rightLeft + labelWidth + editWidth + controlGap, y + 4, labelWidth, rowHeight, TRUE);
	EditA.MoveWindow(rightLeft + labelWidth + editWidth + controlGap + labelWidth, y, editWidth, rowHeight, TRUE);
	y += rowHeight + 6;

	LabelHex.MoveWindow(rightLeft, y + 4, 34, rowHeight, TRUE);
	EditHex.MoveWindow(rightLeft + 34, y, max(120, rightWidth - 34), rowHeight, TRUE);
	y += rowHeight + 10;

	PickColorButton.MoveWindow(rightLeft, y, 78, buttonHeight, TRUE);
	RefreshButton.MoveWindow(rightLeft + 86, y, 78, buttonHeight, TRUE);

	int iconY = pageRect.top + innerPad;
	const int iconLeft = pageRect.left + innerPad;
	const int iconRightWidth = max(180, pageRect.Width() - (innerPad * 2));
	const int checkboxHeight = 20;
	const int comboLabelWidth = 90;
	const int comboWidth = max(120, iconRightWidth - comboLabelWidth - 8);

	IconStyleArrow.MoveWindow(iconLeft, iconY, 90, checkboxHeight, TRUE);
	IconStyleDiamond.MoveWindow(iconLeft + 96, iconY, 90, checkboxHeight, TRUE);
	IconStyleRealistic.MoveWindow(iconLeft + 192, iconY, 100, checkboxHeight, TRUE);
	iconY += checkboxHeight + 12;

	FixedPixelCheck.MoveWindow(iconLeft, iconY, 140, checkboxHeight, TRUE);
	iconY += checkboxHeight + 8;

	FixedScaleLabel.MoveWindow(iconLeft, iconY + 4, comboLabelWidth, rowHeight, TRUE);
	FixedScaleCombo.MoveWindow(iconLeft + comboLabelWidth, iconY, comboWidth, rowHeight + 120, TRUE);
	iconY += rowHeight + 10;

	SmallBoostCheck.MoveWindow(iconLeft, iconY, 160, checkboxHeight, TRUE);
	iconY += checkboxHeight + 8;

	BoostFactorLabel.MoveWindow(iconLeft, iconY + 4, comboLabelWidth, rowHeight, TRUE);
	BoostFactorCombo.MoveWindow(iconLeft + comboLabelWidth, iconY, comboWidth, rowHeight + 120, TRUE);
	iconY += rowHeight + 10;

	BoostResolutionLabel.MoveWindow(iconLeft, iconY + 4, comboLabelWidth, rowHeight, TRUE);
	BoostResolutionCombo.MoveWindow(iconLeft + comboLabelWidth, iconY, comboWidth, rowHeight + 120, TRUE);

	UpdatePageVisibility();
}

void CProfileEditorDialog::UpdatePageVisibility()
{
	if (!ControlsCreated)
		return;

	const bool showIcons = (PageTabs.GetCurSel() == 1);
	const int colorShowMode = showIcons ? SW_HIDE : SW_SHOW;
	const int iconShowMode = showIcons ? SW_SHOW : SW_HIDE;

	ColorPathList.ShowWindow(colorShowMode);
	SelectedPathText.ShowWindow(colorShowMode);
	LabelR.ShowWindow(colorShowMode);
	LabelG.ShowWindow(colorShowMode);
	LabelB.ShowWindow(colorShowMode);
	LabelA.ShowWindow(colorShowMode);
	LabelHex.ShowWindow(colorShowMode);
	EditR.ShowWindow(colorShowMode);
	EditG.ShowWindow(colorShowMode);
	EditB.ShowWindow(colorShowMode);
	EditA.ShowWindow(colorShowMode);
	EditHex.ShowWindow(colorShowMode);
	PickColorButton.ShowWindow(colorShowMode);
	RefreshButton.ShowWindow(colorShowMode);

	IconStyleArrow.ShowWindow(iconShowMode);
	IconStyleDiamond.ShowWindow(iconShowMode);
	IconStyleRealistic.ShowWindow(iconShowMode);
	FixedPixelCheck.ShowWindow(iconShowMode);
	FixedScaleLabel.ShowWindow(iconShowMode);
	FixedScaleCombo.ShowWindow(iconShowMode);
	SmallBoostCheck.ShowWindow(iconShowMode);
	BoostFactorLabel.ShowWindow(iconShowMode);
	BoostFactorCombo.ShowWindow(iconShowMode);
	BoostResolutionLabel.ShowWindow(iconShowMode);
	BoostResolutionCombo.ShowWindow(iconShowMode);
}

void CProfileEditorDialog::RebuildColorPathList()
{
	if (Owner == nullptr)
		return;

	std::string previousSelection = Owner->GetSelectedProfileColorPathForEditor();
	const std::vector<std::string> paths = Owner->GetProfileColorPathsForEditor();

	UpdatingControls = true;
	ColorPathList.ResetContent();
	for (const std::string& path : paths)
		ColorPathList.AddString(path.c_str());

	int selectedIndex = LB_ERR;
	if (!previousSelection.empty())
		selectedIndex = ColorPathList.FindStringExact(-1, previousSelection.c_str());
	if (selectedIndex == LB_ERR && ColorPathList.GetCount() > 0)
		selectedIndex = 0;

	if (selectedIndex != LB_ERR)
	{
		ColorPathList.SetCurSel(selectedIndex);
		CString selectedText;
		ColorPathList.GetText(selectedIndex, selectedText);
		Owner->SelectProfileColorPathForEditor(std::string(selectedText.GetString()));
	}
	UpdatingControls = false;
}

void CProfileEditorDialog::RefreshEditorFieldsFromSelection()
{
	if (Owner == nullptr)
		return;

	int r = 0, g = 0, b = 0, a = 255;
	bool hasAlpha = false;
	const std::string selectedPath = Owner->GetSelectedProfileColorPathForEditor();
	if (!selectedPath.empty())
		Owner->GetSelectedProfileColorForEditor(r, g, b, a, hasAlpha);

	char rgbBuffer[32] = {};
	char alphaBuffer[16] = {};
	char hexBuffer[16] = {};
	sprintf_s(rgbBuffer, sizeof(rgbBuffer), "%d", r);
	sprintf_s(alphaBuffer, sizeof(alphaBuffer), "%d", a);
	if (hasAlpha || a != 255)
		sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X%02X", r, g, b, a);
	else
		sprintf_s(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X", r, g, b);

	CString selectedLabel;
	selectedLabel.Format("Selected: %s", selectedPath.empty() ? "(none)" : selectedPath.c_str());

	UpdatingControls = true;
	SelectedPathText.SetWindowTextA(selectedLabel);
	EditR.SetWindowTextA(rgbBuffer);
	sprintf_s(rgbBuffer, sizeof(rgbBuffer), "%d", g);
	EditG.SetWindowTextA(rgbBuffer);
	sprintf_s(rgbBuffer, sizeof(rgbBuffer), "%d", b);
	EditB.SetWindowTextA(rgbBuffer);
	EditA.SetWindowTextA(alphaBuffer);
	EditHex.SetWindowTextA(hexBuffer);
	UpdatingControls = false;
}

bool CProfileEditorDialog::TryReadEditInt(CEdit& edit, int& outValue) const
{
	CString text;
	edit.GetWindowText(text);
	text.Trim();
	if (text.IsEmpty())
		return false;

	const char* raw = text.GetString();
	char* endPtr = nullptr;
	long parsed = std::strtol(raw, &endPtr, 10);
	if (endPtr == raw || *endPtr != '\0')
		return false;
	if (parsed < 0 || parsed > 255)
		return false;

	outValue = static_cast<int>(parsed);
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

	const std::string preset = Owner->GetSmallTargetIconBoostResolutionPreset();
	if (preset == "2k")
		SelectComboEntryByText(BoostResolutionCombo, "2K");
	else if (preset == "4k")
		SelectComboEntryByText(BoostResolutionCombo, "4K");
	else
		SelectComboEntryByText(BoostResolutionCombo, "1080p");

	UpdatingControls = false;
}

void CProfileEditorDialog::OnColorPathSelectionChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	const int selectedIndex = ColorPathList.GetCurSel();
	if (selectedIndex == LB_ERR)
		return;

	CString selectedText;
	ColorPathList.GetText(selectedIndex, selectedText);
	if (Owner->SelectProfileColorPathForEditor(std::string(selectedText.GetString())))
		RefreshEditorFieldsFromSelection();
}

void CProfileEditorDialog::OnPickColorClicked()
{
	if (Owner == nullptr)
		return;

	int r = 0, g = 0, b = 0, a = 255;
	bool hasAlpha = false;
	if (!Owner->GetSelectedProfileColorForEditor(r, g, b, a, hasAlpha))
		return;

	CColorDialog picker(RGB(r, g, b), CC_FULLOPEN | CC_RGBINIT, this);
	if (picker.DoModal() != IDOK)
		return;

	COLORREF selected = picker.GetColor();
	const bool applyOk = Owner->SetSelectedProfileColorForEditor(
		GetRValue(selected),
		GetGValue(selected),
		GetBValue(selected),
		a,
		hasAlpha || a != 255,
		true);
	if (applyOk)
		RefreshEditorFieldsFromSelection();
}

void CProfileEditorDialog::OnRefreshColorsClicked()
{
	SyncFromRadar();
}

void CProfileEditorDialog::OnRgbEditChanged()
{
	if (UpdatingControls || Owner == nullptr)
		return;

	int r = 0, g = 0, b = 0, a = 255;
	if (!TryReadEditInt(EditR, r) || !TryReadEditInt(EditG, g) || !TryReadEditInt(EditB, b))
		return;

	int parsedAlpha = 255;
	if (TryReadEditInt(EditA, parsedAlpha))
		a = parsedAlpha;

	const bool includeAlpha = (a != 255);
	if (Owner->SetSelectedProfileColorForEditor(r, g, b, a, includeAlpha, true))
		RefreshEditorFieldsFromSelection();
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

	if (Owner->SetSelectedProfileColorForEditor(r, g, b, a, hasAlpha, true))
		RefreshEditorFieldsFromSelection();
}

void CProfileEditorDialog::OnTabSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	(void)pNMHDR;
	UpdatePageVisibility();
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
	ON_LBN_SELCHANGE(IDC_PE_COLOR_LIST, &CProfileEditorDialog::OnColorPathSelectionChanged)
	ON_BN_CLICKED(IDC_PE_PICK_BUTTON, &CProfileEditorDialog::OnPickColorClicked)
	ON_BN_CLICKED(IDC_PE_REFRESH_BUTTON, &CProfileEditorDialog::OnRefreshColorsClicked)
	ON_EN_CHANGE(IDC_PE_EDIT_R, &CProfileEditorDialog::OnRgbEditChanged)
	ON_EN_CHANGE(IDC_PE_EDIT_G, &CProfileEditorDialog::OnRgbEditChanged)
	ON_EN_CHANGE(IDC_PE_EDIT_B, &CProfileEditorDialog::OnRgbEditChanged)
	ON_EN_CHANGE(IDC_PE_EDIT_A, &CProfileEditorDialog::OnRgbEditChanged)
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
END_MESSAGE_MAP()
