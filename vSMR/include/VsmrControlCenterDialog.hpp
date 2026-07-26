#pragma once

#include "resource.h"

#include <string>

class CSMRRadar;

class CVsmrControlCenterDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CVsmrControlCenterDialog)

public:
	enum class Page
	{
		Overview,
		Profiles,
		Aviso,
		Maps,
		Settings
	};

	explicit CVsmrControlCenterDialog(CSMRRadar* owner, CWnd* parent = nullptr);
	virtual ~CVsmrControlCenterDialog();

	enum { IDD = IDD_VSMR_CONTROL_CENTER_DIALOG };

	void SetOwner(CSMRRadar* owner);
	void ShowPage(Page page);
	void SyncFromRadar();

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnCancel() override;
	virtual void OnOK() override;

	afx_msg void OnClose();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg void OnPaint();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg void OnNavOverviewClicked();
	afx_msg void OnNavProfilesClicked();
	afx_msg void OnNavAvisoClicked();
	afx_msg void OnNavMapsClicked();
	afx_msg void OnNavSettingsClicked();
	afx_msg void OnOverviewProfilesClicked();
	afx_msg void OnOverviewAvisoClicked();
	afx_msg void OnOverviewMapsClicked();
	afx_msg void OnReloadConfigClicked();
	afx_msg void OnReloadAvisoClicked();
	afx_msg void OnOpenMapsExternalClicked();
	afx_msg void OnReloadMapsTextClicked();
	afx_msg void OnSaveMapsTextClicked();
	afx_msg void OnOpenConfigExternalClicked();
	afx_msg void OnOpenDataFolderClicked();
	afx_msg void OnOpenPluginFolderClicked();

	DECLARE_MESSAGE_MAP()

private:
	enum
	{
		IDC_VCC_NAV_OVERVIEW = 9601,
		IDC_VCC_NAV_PROFILES = 9602,
		IDC_VCC_NAV_AVISO = 9603,
		IDC_VCC_NAV_MAPS = 9604,
		IDC_VCC_NAV_SETTINGS = 9605,
		IDC_VCC_OVERVIEW_PROFILES = 9610,
		IDC_VCC_OVERVIEW_AVISO = 9611,
		IDC_VCC_OVERVIEW_MAPS = 9612,
		IDC_VCC_RELOAD_CONFIG = 9613,
		IDC_VCC_RELOAD_AVISO = 9614,
		IDC_VCC_OPEN_MAPS_EXTERNAL = 9620,
		IDC_VCC_RELOAD_MAPS_TEXT = 9621,
		IDC_VCC_SAVE_MAPS_TEXT = 9622,
		IDC_VCC_OPEN_CONFIG_EXTERNAL = 9630,
		IDC_VCC_OPEN_DATA_FOLDER = 9631,
		IDC_VCC_OPEN_PLUGIN_FOLDER = 9632
	};

	void CreateControls();
	void CreateButton(CButton& button, const char* text, UINT id);
	void CreateStatic(CStatic& label, const char* text);
	void CreateReadOnlyEdit(CEdit& edit);
	void LayoutControls();
	void LayoutHostedEditors();
	CRect ContentRect() const;
	void HidePageControls();
	void HideHostedEditors();
	void UpdateNavState();
	void UpdateHeader();
	void SetStatusText(const std::string& text);
	void SetPageText(const std::string& title, const std::string& subtitle);
	void LoadMapsText();
	bool SaveMapsText();
	bool HostProfileEditor();
	bool HostAvisoEditor();
	void PrepareHostedDialog(CDialogEx* dialog, const CRect& targetRect);
	std::string BuildOverviewText() const;
	std::string BuildSettingsText() const;
	std::string ActiveProfileName() const;
	std::string ActiveAirportName() const;
	std::string ActiveAvisoPath() const;
	bool OpenPathExternal(const std::string& path, bool folderMode);

	CSMRRadar* Owner = nullptr;
	Page CurrentPage = Page::Overview;
	bool ControlsCreated = false;

	CFont UiFont;
	CFont TitleFont;
	CFont SidebarTitleFont;
	CBrush BackgroundBrush;
	CBrush SidebarBrush;
	CBrush HeaderBrush;
	CBrush EditBrush;

	CStatic HeaderTitleLabel;
	CStatic HeaderSubtitleLabel;
	CStatic PageTitleLabel;
	CStatic PageSubtitleLabel;
	CStatic StatusLabel;
	CStatic MapsPathLabel;

	CButton NavOverviewButton;
	CButton NavProfilesButton;
	CButton NavAvisoButton;
	CButton NavMapsButton;
	CButton NavSettingsButton;
	CButton OverviewProfilesButton;
	CButton OverviewAvisoButton;
	CButton OverviewMapsButton;
	CButton ReloadConfigButton;
	CButton ReloadAvisoButton;
	CButton OpenMapsExternalButton;
	CButton ReloadMapsTextButton;
	CButton SaveMapsTextButton;
	CButton OpenConfigExternalButton;
	CButton OpenDataFolderButton;
	CButton OpenPluginFolderButton;

	CEdit OverviewEdit;
	CEdit SettingsEdit;
	CEdit MapsPathEdit;
	CEdit MapsRawEdit;
};
