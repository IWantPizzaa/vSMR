#pragma once

#include "resource.h"
#include "SMRRadar.hpp"
#include <map>
#include <string>
#include <vector>

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
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
	afx_msg void OnDestroy();
	afx_msg void OnPaint();
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
	afx_msg void OnFixedScaleSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnBoostFactorSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnColorTreeSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnColorTreeCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnColorValueSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnColorOpacitySliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg LRESULT OnColorWheelTrack(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnColorValueSliderTrack(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnColorOpacitySliderTrack(WPARAM wParam, LPARAM lParam);
	afx_msg void OnColorWheelClicked();
	afx_msg void OnApplyColorClicked();
	afx_msg void OnResetColorClicked();
	afx_msg void OnRgbaEditChanged();
	afx_msg void OnHexEditChanged();
	afx_msg void OnTabSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnNavColorsClicked();
	afx_msg void OnNavIconClicked();
	afx_msg void OnNavRulesClicked();
	afx_msg void OnNavProfileClicked();
	afx_msg void OnIconStyleChanged();
	afx_msg void OnFixedPixelToggled();
	afx_msg void OnFixedScaleChanged();
	afx_msg void OnSmallBoostToggled();
	afx_msg void OnBoostFactorChanged();
	afx_msg void OnBoostResolutionChanged();
	afx_msg void OnBoostResolutionPresetChanged();
	afx_msg void OnRuleSelectionChanged();
	afx_msg void OnRuleTreeSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnRuleTreeCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnRuleTreeClick(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnRuleAddClicked();
	afx_msg void OnRuleAddParameterClicked();
	afx_msg void OnRuleRemoveClicked();
	afx_msg void OnRuleSourceChanged();
	afx_msg void OnRuleNameChanged();
	afx_msg void OnRuleFieldChanged();
	afx_msg void OnProfileSelectionChanged();
	afx_msg void OnProfileAddClicked();
	afx_msg void OnProfileDuplicateClicked();
	afx_msg void OnProfileRenameClicked();
	afx_msg void OnProfileDeleteClicked();
	afx_msg void OnRuleColorValueSliderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg LRESULT OnRuleColorWheelTrack(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnRuleColorValueSliderTrack(WPARAM wParam, LPARAM lParam);
	afx_msg void OnRuleColorApplyClicked();
	afx_msg void OnRuleColorResetClicked();
	afx_msg void OnRuleTargetSwatchClicked();
	afx_msg void OnRuleTagSwatchClicked();
	afx_msg void OnRuleTextSwatchClicked();
	afx_msg void OnTagTypeChanged();
	afx_msg void OnTagStatusChanged();
	afx_msg void OnTagLinkToggleChanged();
	afx_msg void OnTagLineChanged();
	afx_msg void OnTagLineFocus();
	afx_msg void OnTagAddTokenClicked();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);

	DECLARE_MESSAGE_MAP()

private:
	enum
	{
		IDC_PE_SELECTED_PATH = 9101,
		IDC_PE_LABEL_HEX = 9106,
		IDC_PE_EDIT_HEX = 9111,
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
		IDC_PE_LABEL_RGBA = 9183,
		IDC_PE_EDIT_RGBA = 9184,
		IDC_PE_COLOR_TREE = 9185,
		IDC_PE_COLOR_PICKER_LABEL = 9186,
		IDC_PE_COLOR_PREVIEW_LABEL = 9188,
		IDC_PE_COLOR_PREVIEW_SWATCH = 9189,
		IDC_PE_APPLY_BUTTON = 9190,
		IDC_PE_RESET_BUTTON = 9191,
		IDC_PE_COLOR_LEFT_PANEL = 9192,
		IDC_PE_COLOR_RIGHT_PANEL = 9193,
		IDC_PE_ICON_PANEL = 9194,
		IDC_PE_ICON_SEPARATOR1 = 9195,
		IDC_PE_ICON_SEPARATOR2 = 9196,
		IDC_PE_ICON_SEPARATOR3 = 9197,
		IDC_PE_FIXED_SCALE_SLIDER = 9198,
		IDC_PE_BOOST_FACTOR_SLIDER = 9199,
		IDC_PE_FIXED_SCALE_VALUE = 9200,
		IDC_PE_BOOST_FACTOR_VALUE = 9201,
		IDC_PE_FIXED_SCALE_TICK_MIN = 9202,
		IDC_PE_FIXED_SCALE_TICK_MID = 9203,
		IDC_PE_FIXED_SCALE_TICK_MAX = 9204,
		IDC_PE_BOOST_FACTOR_TICK_MIN = 9205,
		IDC_PE_BOOST_FACTOR_TICK_MID = 9206,
		IDC_PE_BOOST_FACTOR_TICK_MAX = 9207,
		IDC_PE_RULE_LEFT_PANEL = 9208,
		IDC_PE_RULE_RIGHT_PANEL = 9209,
		IDC_PE_RULE_LEFT_HEADER = 9210,
		IDC_PE_RULE_RIGHT_HEADER = 9211,
		IDC_PE_TAG_PANEL = 9212,
		IDC_PE_TAG_HEADER_PANEL = 9213,
		IDC_PE_RULE_TARGET_SWATCH = 9214,
		IDC_PE_RULE_TAG_SWATCH = 9215,
		IDC_PE_RULE_TEXT_SWATCH = 9216
		, IDC_PE_COLOR_WHEEL = 9217
		, IDC_PE_COLOR_VALUE_LABEL = 9218
		, IDC_PE_COLOR_VALUE_SLIDER = 9219
		, IDC_PE_COLOR_OPACITY_LABEL = 9220
		, IDC_PE_COLOR_OPACITY_SLIDER = 9221
		, IDC_PE_RULE_COLOR_WHEEL_LABEL = 9222
		, IDC_PE_RULE_COLOR_WHEEL = 9223
		, IDC_PE_RULE_COLOR_VALUE_LABEL = 9224
		, IDC_PE_RULE_COLOR_VALUE_SLIDER = 9225
		, IDC_PE_RULE_COLOR_PREVIEW_LABEL = 9226
		, IDC_PE_RULE_COLOR_PREVIEW_SWATCH = 9227
		, IDC_PE_RULE_COLOR_APPLY_BUTTON = 9228
		, IDC_PE_RULE_COLOR_RESET_BUTTON = 9229
		, IDC_PE_RULE_TREE = 9230
		, IDC_PE_RULE_ADD_PARAM_BUTTON = 9231
		, IDC_PE_RULE_NAME_LABEL = 9232
		, IDC_PE_RULE_NAME_EDIT = 9233
		, IDC_PE_PROFILE_PANEL = 9234
		, IDC_PE_PROFILE_HEADER = 9235
		, IDC_PE_PROFILE_LIST = 9236
		, IDC_PE_PROFILE_NAME_LABEL = 9237
		, IDC_PE_PROFILE_NAME_EDIT = 9238
		, IDC_PE_PROFILE_ADD_BUTTON = 9239
		, IDC_PE_PROFILE_DUPLICATE_BUTTON = 9240
		, IDC_PE_PROFILE_RENAME_BUTTON = 9241
		, IDC_PE_PROFILE_DELETE_BUTTON = 9242
		, IDC_PE_SIDEBAR_PANEL = 9243
		, IDC_PE_SIDEBAR_TITLE = 9244
		, IDC_PE_NAV_COLORS = 9245
		, IDC_PE_NAV_ICON = 9246
		, IDC_PE_NAV_TAGS = 9247
		, IDC_PE_NAV_RULES = 9248
		, IDC_PE_NAV_PROFILE = 9249
		, IDC_PE_PAGE_TITLE = 9250
		, IDC_PE_PAGE_SUBTITLE = 9251
		, IDC_PE_ICON_SHAPE_PANEL = 9252
		, IDC_PE_ICON_SHAPE_HEADER = 9253
		, IDC_PE_ICON_SIZE_HEADER = 9254
		, IDC_PE_ICON_DISPLAY_PANEL = 9255
		, IDC_PE_ICON_DISPLAY_HEADER = 9256
		, IDC_PE_ICON_PREVIEW_PANEL = 9257
		, IDC_PE_ICON_PREVIEW_HEADER = 9258
		, IDC_PE_ICON_PREVIEW_SWATCH = 9259
		, IDC_PE_ICON_PREVIEW_HINT = 9260
		, IDC_PE_SIDEBAR_DIVIDER = 9261
		, IDC_PE_ICON_RES_1080 = 9262
		, IDC_PE_ICON_RES_2K = 9263
		, IDC_PE_ICON_RES_4K = 9264
		, IDC_PE_TAG_DEFINITION_EDIT = 9265
		, IDC_PE_TAG_DETAILED_EDIT = 9266
	};

	CSMRRadar* Owner = nullptr;
	bool Initialized = false;
	bool UpdatingControls = false;
	bool ControlsCreated = false;
	int LastVisibilityTab = -1;
	bool LastVisibilityDetailedTag = false;

	CTabCtrl PageTabs;
	CStatic SidebarPanel;
	CStatic SidebarTitle;
	CStatic SidebarDivider;
	CButton NavColorsButton;
	CButton NavIconButton;
	CButton NavRulesButton;
	CButton NavProfileButton;
	CStatic PageTitleLabel;
	CStatic PageSubtitleLabel;

	CStatic ColorLeftPanel;
	CStatic ColorRightPanel;
	CTreeCtrl ColorPathTree;
	CStatic ColorPathLabel;
	CStatic SelectedPathText;
	CStatic ColorPickerLabel;
	CStatic ColorPreviewLabel;
	CStatic ColorPreviewSwatch;
	CStatic ColorWheel;
	CStatic ColorValueLabel;
	CSliderCtrl ColorValueSlider;
	CStatic ColorOpacityLabel;
	CSliderCtrl ColorOpacitySlider;
	CStatic LabelRgba;
	CEdit EditRgba;
	CStatic LabelHex;
	CEdit EditHex;
	CButton ApplyColorButton;
	CButton ResetColorButton;

	CButton IconStyleArrow;
	CButton IconStyleDiamond;
	CButton IconStyleRealistic;
	CStatic IconPanel;
	CStatic IconShapePanel;
	CStatic IconShapeHeader;
	CStatic IconSizeHeader;
	CStatic IconDisplayPanel;
	CStatic IconDisplayHeader;
	CStatic IconPreviewPanel;
	CStatic IconPreviewHeader;
	CStatic IconPreviewSwatch;
	CStatic IconPreviewHint;
	CStatic IconSeparator1;
	CStatic IconSeparator2;
	CStatic IconSeparator3;
	CButton FixedPixelCheck;
	CStatic FixedScaleLabel;
	CStatic FixedScaleValueLabel;
	CSliderCtrl FixedScaleSlider;
	CStatic FixedScaleTickMinLabel;
	CStatic FixedScaleTickMidLabel;
	CStatic FixedScaleTickMaxLabel;
	CComboBox FixedScaleCombo;
	CButton SmallBoostCheck;
	CStatic BoostFactorLabel;
	CStatic BoostFactorValueLabel;
	CSliderCtrl BoostFactorSlider;
	CStatic BoostFactorTickMinLabel;
	CStatic BoostFactorTickMidLabel;
	CStatic BoostFactorTickMaxLabel;
	CComboBox BoostFactorCombo;
	CStatic BoostResolutionLabel;
	CComboBox BoostResolutionCombo;
	CButton BoostResolution1080Button;
	CButton BoostResolution2KButton;
	CButton BoostResolution4KButton;

	CListBox RulesList;
	CTreeCtrl RuleTree;
	CStatic RuleLeftPanel;
	CStatic RuleRightPanel;
	CStatic RuleLeftHeader;
	CStatic RuleRightHeader;
	CButton RuleAddButton;
	CButton RuleAddParameterButton;
	CButton RuleRemoveButton;
	CStatic RuleNameLabel;
	CEdit RuleNameEdit;
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
	CStatic RuleTargetSwatch;
	CEdit RuleTargetEdit;
	CButton RuleTagCheck;
	CStatic RuleTagSwatch;
	CEdit RuleTagEdit;
	CButton RuleTextCheck;
	CStatic RuleTextSwatch;
	CEdit RuleTextEdit;
	CStatic RuleColorWheelLabel;
	CStatic RuleColorWheel;
	CStatic RuleColorValueLabel;
	CSliderCtrl RuleColorValueSlider;
	CStatic RuleColorPreviewLabel;
	CStatic RuleColorPreviewSwatch;
	CButton RuleColorApplyButton;
	CButton RuleColorResetButton;
	CStatic ProfilePanel;
	CStatic ProfileHeader;
	CListBox ProfileList;
	CStatic ProfileNameLabel;
	CEdit ProfileNameEdit;
	CButton ProfileAddButton;
	CButton ProfileDuplicateButton;
	CButton ProfileRenameButton;
	CButton ProfileDeleteButton;

	CStatic TagTypeLabel;
	CStatic TagPanel;
	CStatic TagHeaderPanel;
	CComboBox TagTypeCombo;
	CStatic TagStatusLabel;
	CComboBox TagStatusCombo;
	CStatic TagTokenLabel;
	CComboBox TagTokenCombo;
	CButton TagAddTokenButton;
	CStatic TagDefinitionHeader;
	CEdit TagDefinitionEdit;
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
	CEdit TagDetailedDefinitionEdit;
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
	std::vector<std::string> ProfileNames;
	int SelectedProfileListIndex = -1;
	int SelectedRuleIndex = -1;
	int SelectedRuleCriterionIndex = -1;
	std::map<HTREEITEM, std::pair<int, int>> RuleTreeSelectionMap;
	UINT RuleColorActiveSwatchId = 0;
	int RuleColorDraftR = 255;
	int RuleColorDraftG = 255;
	int RuleColorDraftB = 255;
	bool RuleColorDraftValid = false;
	bool RuleColorDraftDirty = false;
	std::vector<std::string> ColorPathEntries;
	std::map<HTREEITEM, std::string> ColorTreeItemPaths;
	int DraftColorR = 255;
	int DraftColorG = 255;
	int DraftColorB = 255;
	int DraftColorA = 255;
	bool DraftColorHasAlpha = false;
	bool DraftColorValid = false;
	CBrush HeaderBarBrush;
	CBrush SidebarBrush;
	CFont MonoFont;
	CFont TitleFont;
	CFont SectionHeaderFont;
	CFont UniformUiFont;

	void HideAndNotifyOwner();
	void NotifyWindowRectChanged();
	void ForceChildRepaint();
	void UnsubclassEditorControls();
	bool HandleColorSliderScroll(CScrollBar* pScrollBar, UINT nSBCode, UINT nPos);
	void CreateEditorControls();
	void LayoutControls();
	void UpdatePageVisibility();
	void RebuildColorPathList();
	void RebuildColorPathTree(const std::string& selectedPath);
	bool SelectColorPathInTree(const std::string& path);
	std::string GetSelectedTreePath() const;
	void UpdateDraftColorControls(bool updateRgba = true, bool updateHex = true, bool invalidateWheel = true);
	void LoadDraftColorFromSelection();
	std::vector<std::string> SplitPathSegments(const std::string& path) const;
	void RefreshEditorFieldsFromSelection();
	void SyncIconControlsFromRadar();
	void PopulateIconCombos();
	void PopulateRuleCombos();
	void PopulateRuleTokenCombo(const std::string& source, const std::string& selectedToken);
	void PopulateRuleConditionCombo(const std::string& source, const std::string& token, const std::string& selectedCondition);
	void RebuildProfileList();
	void RefreshProfileControls();
	std::string GetSelectedProfileName() const;
	std::string ReadComboText(CComboBox& combo) const;
	void RebuildRulesList();
	void SelectRuleNodeInTree(int ruleIndex, int criterionIndex);
	bool ResolveRuleSelectionFromTree(int& outRuleIndex, int& outCriterionIndex) const;
	bool GetRuleTreeActionRects(HTREEITEM item, CRect& addRect, CRect& deleteRect, bool& showAdd, bool& showDelete) const;
	void RefreshRuleControls();
	void SyncTagEditorControlsFromRadar();
	void PopulateTagTokenCombo();
	void RefreshTagStatusOptions();
	void RefreshTagDefinitionLines();
	void RefreshTagPreview();
	bool TryParseRgbaQuad(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	bool TryParseRgbTriplet(const std::string& text, int& r, int& g, int& b) const;
	std::string FormatRgbTriplet(int r, int g, int b) const;
	bool ReadRuleFromControls(StructuredTagColorRule& outRule) const;
	bool ReadRuleCriterionFromControls(StructuredTagColorRule::Criterion& outCriterion) const;
	void ApplyRuleControlChanges(bool keepSelection);
	void ApplyRuleCriterionControlChanges(bool keepSelection);
	double ParseComboScaleSelection(CComboBox& combo, double fallback) const;
	double SliderPosToScale(int pos) const;
	int ScaleToSliderPos(double scale) const;
	void UpdateIconScaleValueLabels();
	void SelectComboEntryByText(CComboBox& combo, const std::string& text);
	void ApplyThemedEditBorders();
	void ApplyThemedComboBorders();
	void SetEditTextPreserveCaret(CEdit& edit, const std::string& text);
	void UpdateRulesListItemLabel(int index);
	void InvalidateRuleColorSwatches();
	bool ResolveRuleSwatchColor(UINT controlId, COLORREF& outColor, bool& outEnabled) const;
	void SelectRuleColorEditorTarget(UINT swatchControlId);
	bool GetRuleColorEditorTargetControls(UINT swatchControlId, CButton*& outCheck, CEdit*& outEdit) const;
	void SyncRuleColorEditorFromActiveControl();
	void SyncRuleColorValueSliderFromDraft();
	void ApplyRuleColorValueFromSlider();
	void ApplyRuleColorDraftToActiveControl();
	bool TryApplyRuleColorWheelPoint(const CPoint& screenPoint);
	bool TryApplyRuleColorValueSliderPoint(const CPoint& screenPoint);
	bool TryApplyColorWheelPoint(const CPoint& screenPoint);
	bool TryApplyColorValueSliderPoint(const CPoint& screenPoint);
	bool TryApplyColorOpacitySliderPoint(const CPoint& screenPoint);
	void SyncColorValueSliderFromDraft();
	void SyncColorOpacitySliderFromDraft();
	void ApplyDraftColorValueFromSlider();
	void ApplyDraftColorOpacityFromSlider();
	void EnsureColorWheelBitmap(const CRect& wheelRect);
	CBitmap ColorWheelBitmap;
	CSize ColorWheelBitmapSize = CSize(0, 0);
};
