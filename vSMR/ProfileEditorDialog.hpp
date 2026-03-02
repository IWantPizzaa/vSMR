#pragma once

#include "resource.h"
#include <string>
#include <vector>

class CSMRRadar;
struct StructuredTagColorRule;

class CProfileEditorDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CProfileEditorDialog)

public:
	explicit CProfileEditorDialog(CSMRRadar* owner, CWnd* pParent = NULL);
	virtual ~CProfileEditorDialog();

	enum { IDD = IDD_PROFILE_EDITOR_DIALOG };

	void SetOwner(CSMRRadar* owner);
	void SyncFromRadar();

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnCancel() override;
	virtual void OnOK() override;

	afx_msg void OnClose();
	afx_msg void OnMove(int x, int y);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnColorPathSelectionChanged();
	afx_msg void OnPickColorClicked();
	afx_msg void OnRefreshColorsClicked();
	afx_msg void OnRgbEditChanged();
	afx_msg void OnHexEditChanged();
	afx_msg void OnTabSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnIconStyleChanged();
	afx_msg void OnFixedPixelToggled();
	afx_msg void OnFixedScaleChanged();
	afx_msg void OnSmallBoostToggled();
	afx_msg void OnBoostFactorChanged();
	afx_msg void OnBoostResolutionChanged();
	afx_msg void OnRuleSelectionChanged();
	afx_msg void OnRuleAddClicked();
	afx_msg void OnRuleRemoveClicked();
	afx_msg void OnRuleSourceChanged();
	afx_msg void OnRuleFieldChanged();

	DECLARE_MESSAGE_MAP()

private:
	enum
	{
		IDC_PE_COLOR_LIST = 9100,
		IDC_PE_SELECTED_PATH = 9101,
		IDC_PE_LABEL_R = 9102,
		IDC_PE_LABEL_G = 9103,
		IDC_PE_LABEL_B = 9104,
		IDC_PE_LABEL_A = 9105,
		IDC_PE_LABEL_HEX = 9106,
		IDC_PE_EDIT_R = 9107,
		IDC_PE_EDIT_G = 9108,
		IDC_PE_EDIT_B = 9109,
		IDC_PE_EDIT_A = 9110,
		IDC_PE_EDIT_HEX = 9111,
		IDC_PE_PICK_BUTTON = 9112,
		IDC_PE_REFRESH_BUTTON = 9113,
		IDC_PE_TAB = 9114,
		IDC_PE_ICON_STYLE_ARROW = 9115,
		IDC_PE_ICON_STYLE_DIAMOND = 9116,
		IDC_PE_ICON_STYLE_REALISTIC = 9117,
		IDC_PE_FIXED_PIXEL_CHECK = 9118,
		IDC_PE_FIXED_SCALE_LABEL = 9119,
		IDC_PE_FIXED_SCALE_COMBO = 9120,
		IDC_PE_SMALL_BOOST_CHECK = 9121,
		IDC_PE_BOOST_FACTOR_LABEL = 9122,
		IDC_PE_BOOST_FACTOR_COMBO = 9123,
		IDC_PE_BOOST_RES_LABEL = 9124,
		IDC_PE_BOOST_RES_COMBO = 9125,
		IDC_PE_RULE_LIST = 9126,
		IDC_PE_RULE_ADD_BUTTON = 9127,
		IDC_PE_RULE_REMOVE_BUTTON = 9128,
		IDC_PE_RULE_SOURCE_LABEL = 9129,
		IDC_PE_RULE_SOURCE_COMBO = 9130,
		IDC_PE_RULE_TOKEN_LABEL = 9131,
		IDC_PE_RULE_TOKEN_COMBO = 9132,
		IDC_PE_RULE_CONDITION_LABEL = 9133,
		IDC_PE_RULE_CONDITION_EDIT = 9134,
		IDC_PE_RULE_TYPE_LABEL = 9135,
		IDC_PE_RULE_TYPE_COMBO = 9136,
		IDC_PE_RULE_STATUS_LABEL = 9137,
		IDC_PE_RULE_STATUS_COMBO = 9138,
		IDC_PE_RULE_DETAIL_LABEL = 9139,
		IDC_PE_RULE_DETAIL_COMBO = 9140,
		IDC_PE_RULE_TARGET_CHECK = 9141,
		IDC_PE_RULE_TARGET_EDIT = 9142,
		IDC_PE_RULE_TAG_CHECK = 9143,
		IDC_PE_RULE_TAG_EDIT = 9144,
		IDC_PE_RULE_TEXT_CHECK = 9145,
		IDC_PE_RULE_TEXT_EDIT = 9146
	};

	CSMRRadar* Owner = nullptr;
	bool Initialized = false;
	bool UpdatingControls = false;
	bool ControlsCreated = false;

	CTabCtrl PageTabs;

	CListBox ColorPathList;
	CStatic SelectedPathText;
	CStatic LabelR;
	CStatic LabelG;
	CStatic LabelB;
	CStatic LabelA;
	CStatic LabelHex;
	CEdit EditR;
	CEdit EditG;
	CEdit EditB;
	CEdit EditA;
	CEdit EditHex;
	CButton PickColorButton;
	CButton RefreshButton;

	CButton IconStyleArrow;
	CButton IconStyleDiamond;
	CButton IconStyleRealistic;
	CButton FixedPixelCheck;
	CStatic FixedScaleLabel;
	CComboBox FixedScaleCombo;
	CButton SmallBoostCheck;
	CStatic BoostFactorLabel;
	CComboBox BoostFactorCombo;
	CStatic BoostResolutionLabel;
	CComboBox BoostResolutionCombo;

	CListBox RulesList;
	CButton RuleAddButton;
	CButton RuleRemoveButton;
	CStatic RuleSourceLabel;
	CComboBox RuleSourceCombo;
	CStatic RuleTokenLabel;
	CComboBox RuleTokenCombo;
	CStatic RuleConditionLabel;
	CEdit RuleConditionEdit;
	CStatic RuleTypeLabel;
	CComboBox RuleTypeCombo;
	CStatic RuleStatusLabel;
	CComboBox RuleStatusCombo;
	CStatic RuleDetailLabel;
	CComboBox RuleDetailCombo;
	CButton RuleTargetCheck;
	CEdit RuleTargetEdit;
	CButton RuleTagCheck;
	CEdit RuleTagEdit;
	CButton RuleTextCheck;
	CEdit RuleTextEdit;

	std::vector<StructuredTagColorRule> RuleBuffer;
	int SelectedRuleIndex = -1;

	void HideAndNotifyOwner();
	void NotifyWindowRectChanged();
	void CreateEditorControls();
	void LayoutControls();
	void UpdatePageVisibility();
	void RebuildColorPathList();
	void RefreshEditorFieldsFromSelection();
	void SyncIconControlsFromRadar();
	void PopulateIconCombos();
	void PopulateRuleCombos();
	void PopulateRuleTokenCombo(const std::string& source, const std::string& selectedToken);
	void RebuildRulesList();
	void RefreshRuleControls();
	bool TryReadEditInt(CEdit& edit, int& outValue) const;
	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool TryParseRgbTriplet(const std::string& text, int& r, int& g, int& b) const;
	std::string FormatRgbTriplet(int r, int g, int b) const;
	bool ReadRuleFromControls(StructuredTagColorRule& outRule) const;
	void ApplyRuleControlChanges(bool keepSelection);
	double ParseComboScaleSelection(CComboBox& combo, double fallback) const;
	void SelectComboEntryByText(CComboBox& combo, const std::string& text);
};
