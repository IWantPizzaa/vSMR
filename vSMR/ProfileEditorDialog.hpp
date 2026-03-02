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
	afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
	afx_msg void OnColorPathSelectionChanged();
	afx_msg void OnColorPathLevelChanged();
	afx_msg void OnPickColorClicked();
	afx_msg void OnRefreshColorsClicked();
	afx_msg void OnRgbEditChanged();
	afx_msg void OnRgbaEditChanged();
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
	afx_msg void OnTagTypeChanged();
	afx_msg void OnTagStatusChanged();
	afx_msg void OnTagLinkToggleChanged();
	afx_msg void OnTagLineChanged();
	afx_msg void OnTagLineFocus();
	afx_msg void OnTagAddTokenClicked();

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
		IDC_PE_RULE_CONDITION_COMBO = 9134,
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
		IDC_PE_RULE_TEXT_EDIT = 9146,
		IDC_PE_TAG_TYPE_LABEL = 9147,
		IDC_PE_TAG_TYPE_COMBO = 9148,
		IDC_PE_TAG_STATUS_LABEL = 9151,
		IDC_PE_TAG_STATUS_COMBO = 9152,
		IDC_PE_TAG_DEF_HEADER = 9153,
		IDC_PE_TAG_LINE1_LABEL = 9154,
		IDC_PE_TAG_LINE1_EDIT = 9155,
		IDC_PE_TAG_LINE2_LABEL = 9156,
		IDC_PE_TAG_LINE2_EDIT = 9157,
		IDC_PE_TAG_LINE3_LABEL = 9158,
		IDC_PE_TAG_LINE3_EDIT = 9159,
		IDC_PE_TAG_LINE4_LABEL = 9160,
		IDC_PE_TAG_LINE4_EDIT = 9161,
		IDC_PE_TAG_LINK_DETAILED = 9162,
		IDC_PE_TAG_DETAILED_HEADER = 9163,
		IDC_PE_TAG_D_LINE1_LABEL = 9164,
		IDC_PE_TAG_D_LINE1_EDIT = 9165,
		IDC_PE_TAG_D_LINE2_LABEL = 9166,
		IDC_PE_TAG_D_LINE2_EDIT = 9167,
		IDC_PE_TAG_D_LINE3_LABEL = 9168,
		IDC_PE_TAG_D_LINE3_EDIT = 9169,
		IDC_PE_TAG_D_LINE4_LABEL = 9170,
		IDC_PE_TAG_D_LINE4_EDIT = 9171,
		IDC_PE_TAG_PREVIEW_LABEL = 9172,
		IDC_PE_TAG_PREVIEW_EDIT = 9173,
		IDC_PE_TAG_TOKEN_LABEL = 9174,
		IDC_PE_TAG_TOKEN_COMBO = 9175,
		IDC_PE_TAG_TOKEN_ADD_BUTTON = 9176,
		IDC_PE_COLOR_PATH_LABEL = 9177,
		IDC_PE_COLOR_PATH_L1 = 9178,
		IDC_PE_COLOR_PATH_L2 = 9179,
		IDC_PE_COLOR_PATH_L3 = 9180,
		IDC_PE_COLOR_PATH_L4 = 9181,
		IDC_PE_COLOR_PATH_L5 = 9182,
		IDC_PE_LABEL_RGBA = 9183,
		IDC_PE_EDIT_RGBA = 9184
	};

	CSMRRadar* Owner = nullptr;
	bool Initialized = false;
	bool UpdatingControls = false;
	bool ControlsCreated = false;

	CTabCtrl PageTabs;

	CListBox ColorPathList;
	CStatic ColorPathLabel;
	CComboBox ColorPathLevel1;
	CComboBox ColorPathLevel2;
	CComboBox ColorPathLevel3;
	CComboBox ColorPathLevel4;
	CComboBox ColorPathLevel5;
	CStatic SelectedPathText;
	CStatic LabelRgba;
	CEdit EditRgba;
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
	CComboBox RuleConditionCombo;
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

	CStatic TagTypeLabel;
	CComboBox TagTypeCombo;
	CStatic TagStatusLabel;
	CComboBox TagStatusCombo;
	CStatic TagTokenLabel;
	CComboBox TagTokenCombo;
	CButton TagAddTokenButton;
	CStatic TagDefinitionHeader;
	CStatic TagLine1Label;
	CEdit TagLine1Edit;
	CStatic TagLine2Label;
	CEdit TagLine2Edit;
	CStatic TagLine3Label;
	CEdit TagLine3Edit;
	CStatic TagLine4Label;
	CEdit TagLine4Edit;
	CButton TagLinkDetailedToggle;
	CStatic TagDetailedHeader;
	CStatic TagDetailedLine1Label;
	CEdit TagDetailedLine1Edit;
	CStatic TagDetailedLine2Label;
	CEdit TagDetailedLine2Edit;
	CStatic TagDetailedLine3Label;
	CEdit TagDetailedLine3Edit;
	CStatic TagDetailedLine4Label;
	CEdit TagDetailedLine4Edit;
	CStatic TagPreviewLabel;
	CEdit TagPreviewEdit;

	std::string TagEditorType = "departure";
	bool TagEditorSeparateDetailed = false;
	std::string TagEditorStatus = "default";
	int TagEditorSelectedLine = 0;
	bool TagEditorSelectedLineDetailed = false;

	std::vector<StructuredTagColorRule> RuleBuffer;
	int SelectedRuleIndex = -1;
	std::vector<std::string> ColorPathEntries;

	void HideAndNotifyOwner();
	void NotifyWindowRectChanged();
	void ForceChildRepaint();
	void CreateEditorControls();
	void LayoutControls();
	void UpdatePageVisibility();
	void RebuildColorPathList();
	void ApplyColorPathSelection(const std::string& selectedPath);
	void PopulateColorPathLevelCombo(CComboBox& combo, int level, const std::vector<std::string>& prefix, const std::string& selectedSegment);
	std::vector<std::string> SplitPathSegments(const std::string& path) const;
	std::string JoinPathSegments(const std::vector<std::string>& segments, size_t count) const;
	std::string ResolveColorPathFromLevelSelection() const;
	void RefreshEditorFieldsFromSelection();
	void SyncIconControlsFromRadar();
	void PopulateIconCombos();
	void PopulateRuleCombos();
	void PopulateRuleTokenCombo(const std::string& source, const std::string& selectedToken);
	void PopulateRuleConditionCombo(const std::string& source, const std::string& token, const std::string& selectedCondition);
	void RebuildRulesList();
	void RefreshRuleControls();
	void SyncTagEditorControlsFromRadar();
	void PopulateTagTokenCombo();
	void RefreshTagStatusOptions();
	void RefreshTagDefinitionLines();
	void RefreshTagPreview();
	bool TryReadEditInt(CEdit& edit, int& outValue) const;
	bool TryParseRgbaQuad(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool TryParseRgbTriplet(const std::string& text, int& r, int& g, int& b) const;
	std::string FormatRgbTriplet(int r, int g, int b) const;
	bool ReadRuleFromControls(StructuredTagColorRule& outRule) const;
	void ApplyRuleControlChanges(bool keepSelection);
	double ParseComboScaleSelection(CComboBox& combo, double fallback) const;
	void SelectComboEntryByText(CComboBox& combo, const std::string& text);
};
