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

	SetDlgItemTextA(IDC_PROFILE_EDITOR_STATUS, "Profile editor detached window: Colors / Icons / Rules with live updates.");

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
	RebuildRulesList();
	RefreshRuleControls();
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
	PageTabs.InsertItem(2, "Rules");

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

	RulesList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, CRect(0, 0, 0, 0), this, IDC_PE_RULE_LIST);
	RuleAddButton.Create("Add", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_RULE_ADD_BUTTON);
	RuleRemoveButton.Create("Remove", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_PE_RULE_REMOVE_BUTTON);
	RuleSourceLabel.Create("Source", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_SOURCE_LABEL);
	RuleSourceCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_SOURCE_COMBO);
	RuleTokenLabel.Create("Token", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TOKEN_LABEL);
	RuleTokenCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TOKEN_COMBO);
	RuleConditionLabel.Create("Condition", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_CONDITION_LABEL);
	RuleConditionEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_CONDITION_EDIT);
	RuleTypeLabel.Create("Tag Type", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TYPE_LABEL);
	RuleTypeCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TYPE_COMBO);
	RuleStatusLabel.Create("Status", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_STATUS_LABEL);
	RuleStatusCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_STATUS_COMBO);
	RuleDetailLabel.Create("Detail", WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_PE_RULE_DETAIL_LABEL);
	RuleDetailCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_PE_RULE_DETAIL_COMBO);
	RuleTargetCheck.Create("Target color", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TARGET_CHECK);
	RuleTargetEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TARGET_EDIT);
	RuleTagCheck.Create("Tag color", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TAG_CHECK);
	RuleTagEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TAG_EDIT);
	RuleTextCheck.Create("Text color", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TEXT_CHECK);
	RuleTextEdit.Create(commonEditStyle, CRect(0, 0, 0, 0), this, IDC_PE_RULE_TEXT_EDIT);

	PopulateIconCombos();
	PopulateRuleCombos();
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

	const int rulesLeft = pageRect.left + innerPad;
	const int rulesTop = pageRect.top + innerPad;
	const int rulesListWidth = max(190, (pageRect.Width() / 2) - (innerPad * 2));
	const int rulesListHeight = max(120, pageRect.Height() - (innerPad * 2) - buttonHeight - 6);
	const int rulesRightLeft = rulesLeft + rulesListWidth + innerPad;
	const int rulesRightWidth = max(180, pageRect.right - rulesRightLeft - innerPad);

	RulesList.MoveWindow(rulesLeft, rulesTop, rulesListWidth, rulesListHeight, TRUE);
	RuleAddButton.MoveWindow(rulesLeft, rulesTop + rulesListHeight + 6, 68, buttonHeight, TRUE);
	RuleRemoveButton.MoveWindow(rulesLeft + 76, rulesTop + rulesListHeight + 6, 68, buttonHeight, TRUE);

	int rulesY = rulesTop;
	const int rulesLabelWidth = 62;
	const int rulesFieldWidth = max(120, rulesRightWidth - rulesLabelWidth - 4);

	RuleSourceLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleSourceCombo.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight + 180, TRUE);
	rulesY += rowHeight + 6;

	RuleTokenLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTokenCombo.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight + 180, TRUE);
	rulesY += rowHeight + 6;

	RuleConditionLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleConditionEdit.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight, TRUE);
	rulesY += rowHeight + 6;

	RuleTypeLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleTypeCombo.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight + 180, TRUE);
	rulesY += rowHeight + 6;

	RuleStatusLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleStatusCombo.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight + 180, TRUE);
	rulesY += rowHeight + 6;

	RuleDetailLabel.MoveWindow(rulesRightLeft, rulesY + 4, rulesLabelWidth, rowHeight, TRUE);
	RuleDetailCombo.MoveWindow(rulesRightLeft + rulesLabelWidth, rulesY, rulesFieldWidth, rowHeight + 180, TRUE);
	rulesY += rowHeight + 10;

	RuleTargetCheck.MoveWindow(rulesRightLeft, rulesY, 95, rowHeight, TRUE);
	RuleTargetEdit.MoveWindow(rulesRightLeft + 96, rulesY, max(80, rulesRightWidth - 96), rowHeight, TRUE);
	rulesY += rowHeight + 6;

	RuleTagCheck.MoveWindow(rulesRightLeft, rulesY, 95, rowHeight, TRUE);
	RuleTagEdit.MoveWindow(rulesRightLeft + 96, rulesY, max(80, rulesRightWidth - 96), rowHeight, TRUE);
	rulesY += rowHeight + 6;

	RuleTextCheck.MoveWindow(rulesRightLeft, rulesY, 95, rowHeight, TRUE);
	RuleTextEdit.MoveWindow(rulesRightLeft + 96, rulesY, max(80, rulesRightWidth - 96), rowHeight, TRUE);

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
	const int colorShowMode = showColors ? SW_SHOW : SW_HIDE;
	const int iconShowMode = showIcons ? SW_SHOW : SW_HIDE;
	const int ruleShowMode = showRules ? SW_SHOW : SW_HIDE;

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

	RulesList.ShowWindow(ruleShowMode);
	RuleAddButton.ShowWindow(ruleShowMode);
	RuleRemoveButton.ShowWindow(ruleShowMode);
	RuleSourceLabel.ShowWindow(ruleShowMode);
	RuleSourceCombo.ShowWindow(ruleShowMode);
	RuleTokenLabel.ShowWindow(ruleShowMode);
	RuleTokenCombo.ShowWindow(ruleShowMode);
	RuleConditionLabel.ShowWindow(ruleShowMode);
	RuleConditionEdit.ShowWindow(ruleShowMode);
	RuleTypeLabel.ShowWindow(ruleShowMode);
	RuleTypeCombo.ShowWindow(ruleShowMode);
	RuleStatusLabel.ShowWindow(ruleShowMode);
	RuleStatusCombo.ShowWindow(ruleShowMode);
	RuleDetailLabel.ShowWindow(ruleShowMode);
	RuleDetailCombo.ShowWindow(ruleShowMode);
	RuleTargetCheck.ShowWindow(ruleShowMode);
	RuleTargetEdit.ShowWindow(ruleShowMode);
	RuleTagCheck.ShowWindow(ruleShowMode);
	RuleTagEdit.ShowWindow(ruleShowMode);
	RuleTextCheck.ShowWindow(ruleShowMode);
	RuleTextEdit.ShowWindow(ruleShowMode);
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

void CProfileEditorDialog::RebuildRulesList()
{
	if (Owner == nullptr)
		return;

	RuleBuffer = Owner->GetStructuredTagColorRules();
	const int previousSelection = SelectedRuleIndex;

	UpdatingControls = true;
	RulesList.ResetContent();
	for (size_t i = 0; i < RuleBuffer.size(); ++i)
	{
		const StructuredTagColorRule& rule = RuleBuffer[i];
		CString label;
		label.Format("%02d  %s.%s = %s", static_cast<int>(i + 1), rule.source.c_str(), rule.token.c_str(), rule.condition.c_str());
		RulesList.AddString(label);
	}

	if (RuleBuffer.empty())
	{
		SelectedRuleIndex = -1;
	}
	else
	{
		SelectedRuleIndex = previousSelection;
		if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
			SelectedRuleIndex = 0;
		RulesList.SetCurSel(SelectedRuleIndex);
	}
	UpdatingControls = false;
}

void CProfileEditorDialog::RefreshRuleControls()
{
	UpdatingControls = true;
	const bool hasSelection = (SelectedRuleIndex >= 0 && SelectedRuleIndex < static_cast<int>(RuleBuffer.size()));

	RuleRemoveButton.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleSourceCombo.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTokenCombo.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleConditionEdit.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTypeCombo.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleStatusCombo.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleDetailCombo.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTargetCheck.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTagCheck.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTextCheck.EnableWindow(hasSelection ? TRUE : FALSE);
	RuleTargetEdit.EnableWindow(FALSE);
	RuleTagEdit.EnableWindow(FALSE);
	RuleTextEdit.EnableWindow(FALSE);

	if (!hasSelection)
	{
		SelectComboEntryByText(RuleSourceCombo, "vacdm");
		PopulateRuleTokenCombo("vacdm", "tobt");
		RuleConditionEdit.SetWindowTextA("any");
		SelectComboEntryByText(RuleTypeCombo, "any");
		SelectComboEntryByText(RuleStatusCombo, "any");
		SelectComboEntryByText(RuleDetailCombo, "any");
		RuleTargetCheck.SetCheck(BST_UNCHECKED);
		RuleTagCheck.SetCheck(BST_UNCHECKED);
		RuleTextCheck.SetCheck(BST_UNCHECKED);
		RuleTargetEdit.SetWindowTextA("255,255,255");
		RuleTagEdit.SetWindowTextA("255,255,255");
		RuleTextEdit.SetWindowTextA("255,255,255");
		UpdatingControls = false;
		return;
	}

	const StructuredTagColorRule& rule = RuleBuffer[SelectedRuleIndex];
	SelectComboEntryByText(RuleSourceCombo, rule.source);
	PopulateRuleTokenCombo(rule.source, rule.token);
	RuleConditionEdit.SetWindowTextA(rule.condition.c_str());
	SelectComboEntryByText(RuleTypeCombo, rule.tagType);
	SelectComboEntryByText(RuleStatusCombo, rule.status);
	SelectComboEntryByText(RuleDetailCombo, rule.detail);
	RuleTargetCheck.SetCheck(rule.applyTarget ? BST_CHECKED : BST_UNCHECKED);
	RuleTagCheck.SetCheck(rule.applyTag ? BST_CHECKED : BST_UNCHECKED);
	RuleTextCheck.SetCheck(rule.applyText ? BST_CHECKED : BST_UNCHECKED);
	RuleTargetEdit.SetWindowTextA(FormatRgbTriplet(rule.targetR, rule.targetG, rule.targetB).c_str());
	RuleTagEdit.SetWindowTextA(FormatRgbTriplet(rule.tagR, rule.tagG, rule.tagB).c_str());
	RuleTextEdit.SetWindowTextA(FormatRgbTriplet(rule.textR, rule.textG, rule.textB).c_str());
	RuleTargetEdit.EnableWindow(rule.applyTarget ? TRUE : FALSE);
	RuleTagEdit.EnableWindow(rule.applyTag ? TRUE : FALSE);
	RuleTextEdit.EnableWindow(rule.applyText ? TRUE : FALSE);

	UpdatingControls = false;
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

bool CProfileEditorDialog::ReadRuleFromControls(StructuredTagColorRule& outRule) const
{
	if (Owner == nullptr)
		return false;

	auto readComboText = [](CComboBox& combo) -> std::string
	{
		const int index = combo.GetCurSel();
		if (index == CB_ERR)
			return "";
		CString text;
		combo.GetLBText(index, text);
		return std::string(text.GetString());
	};

	StructuredTagColorRule rule;
	rule.source = Owner->NormalizeStructuredRuleSource(readComboText(const_cast<CComboBox&>(RuleSourceCombo)));
	rule.token = Owner->NormalizeStructuredRuleToken(rule.source, readComboText(const_cast<CComboBox&>(RuleTokenCombo)));
	if (rule.token.empty())
		return false;

	CString conditionText;
	const_cast<CEdit&>(RuleConditionEdit).GetWindowText(conditionText);
	rule.condition = Owner->NormalizeStructuredRuleCondition(rule.source, std::string(conditionText.GetString()));
	rule.tagType = Owner->NormalizeStructuredRuleTagType(readComboText(const_cast<CComboBox&>(RuleTypeCombo)));
	rule.status = Owner->NormalizeStructuredRuleStatus(readComboText(const_cast<CComboBox&>(RuleStatusCombo)));
	rule.detail = Owner->NormalizeStructuredRuleDetail(readComboText(const_cast<CComboBox&>(RuleDetailCombo)));

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

void CProfileEditorDialog::ApplyRuleControlChanges(bool keepSelection)
{
	if (UpdatingControls || Owner == nullptr)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;

	StructuredTagColorRule updatedRule;
	if (!ReadRuleFromControls(updatedRule))
		return;

	RuleBuffer[SelectedRuleIndex] = updatedRule;
	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	const int preservedSelection = keepSelection ? SelectedRuleIndex : -1;
	RebuildRulesList();
	if (preservedSelection >= 0 && preservedSelection < RulesList.GetCount())
	{
		SelectedRuleIndex = preservedSelection;
		RulesList.SetCurSel(SelectedRuleIndex);
	}
	RefreshRuleControls();
}

void CProfileEditorDialog::OnRuleSelectionChanged()
{
	if (UpdatingControls)
		return;

	const int selectedIndex = RulesList.GetCurSel();
	SelectedRuleIndex = (selectedIndex == LB_ERR) ? -1 : selectedIndex;
	RefreshRuleControls();
}

void CProfileEditorDialog::OnRuleAddClicked()
{
	if (Owner == nullptr)
		return;

	StructuredTagColorRule newRule;
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
	RebuildRulesList();
	if (SelectedRuleIndex >= 0 && SelectedRuleIndex < RulesList.GetCount())
		RulesList.SetCurSel(SelectedRuleIndex);
	RefreshRuleControls();
}

void CProfileEditorDialog::OnRuleRemoveClicked()
{
	if (Owner == nullptr)
		return;
	if (SelectedRuleIndex < 0 || SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		return;

	RuleBuffer.erase(RuleBuffer.begin() + SelectedRuleIndex);
	if (!Owner->SetStructuredTagColorRules(RuleBuffer, true))
		return;

	Owner->RequestRefresh();
	if (SelectedRuleIndex >= static_cast<int>(RuleBuffer.size()))
		SelectedRuleIndex = static_cast<int>(RuleBuffer.size()) - 1;
	RebuildRulesList();
	if (SelectedRuleIndex >= 0 && SelectedRuleIndex < RulesList.GetCount())
		RulesList.SetCurSel(SelectedRuleIndex);
	RefreshRuleControls();
}

void CProfileEditorDialog::OnRuleSourceChanged()
{
	if (UpdatingControls)
		return;

	const int sourceIndex = RuleSourceCombo.GetCurSel();
	if (sourceIndex == CB_ERR)
		return;

	CString sourceText;
	RuleSourceCombo.GetLBText(sourceIndex, sourceText);
	UpdatingControls = true;
	PopulateRuleTokenCombo(std::string(sourceText.GetString()), "");
	UpdatingControls = false;
	ApplyRuleControlChanges(true);
}

void CProfileEditorDialog::OnRuleFieldChanged()
{
	if (UpdatingControls)
		return;

	RuleTargetEdit.EnableWindow(RuleTargetCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	RuleTagEdit.EnableWindow(RuleTagCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	RuleTextEdit.EnableWindow(RuleTextCheck.GetCheck() == BST_CHECKED ? TRUE : FALSE);
	ApplyRuleControlChanges(true);
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
	ON_LBN_SELCHANGE(IDC_PE_RULE_LIST, &CProfileEditorDialog::OnRuleSelectionChanged)
	ON_BN_CLICKED(IDC_PE_RULE_ADD_BUTTON, &CProfileEditorDialog::OnRuleAddClicked)
	ON_BN_CLICKED(IDC_PE_RULE_REMOVE_BUTTON, &CProfileEditorDialog::OnRuleRemoveClicked)
	ON_CBN_SELCHANGE(IDC_PE_RULE_SOURCE_COMBO, &CProfileEditorDialog::OnRuleSourceChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_TOKEN_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_EN_CHANGE(IDC_PE_RULE_CONDITION_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_TYPE_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_STATUS_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_CBN_SELCHANGE(IDC_PE_RULE_DETAIL_COMBO, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TARGET_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_EN_CHANGE(IDC_PE_RULE_TARGET_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TAG_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_EN_CHANGE(IDC_PE_RULE_TAG_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_BN_CLICKED(IDC_PE_RULE_TEXT_CHECK, &CProfileEditorDialog::OnRuleFieldChanged)
	ON_EN_CHANGE(IDC_PE_RULE_TEXT_EDIT, &CProfileEditorDialog::OnRuleFieldChanged)
END_MESSAGE_MAP()
