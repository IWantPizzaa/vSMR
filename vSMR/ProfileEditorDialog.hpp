#pragma once

#include "resource.h"
#include <string>

class CSMRRadar;

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
		IDC_PE_BOOST_RES_COMBO = 9125
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

	void HideAndNotifyOwner();
	void NotifyWindowRectChanged();
	void CreateEditorControls();
	void LayoutControls();
	void UpdatePageVisibility();
	void RebuildColorPathList();
	void RefreshEditorFieldsFromSelection();
	void SyncIconControlsFromRadar();
	void PopulateIconCombos();
	bool TryReadEditInt(CEdit& edit, int& outValue) const;
	bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha) const;
	double ParseComboScaleSelection(CComboBox& combo, double fallback) const;
	void SelectComboEntryByText(CComboBox& combo, const std::string& text);
};
