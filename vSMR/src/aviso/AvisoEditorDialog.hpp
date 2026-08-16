#pragma once

#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
#include "aviso/AvisoDocumentModel.hpp"

#include <string>
#include <vector>

class CAvisoEditorDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CAvisoEditorDialog)

public:
	explicit CAvisoEditorDialog(CSMRRadar* owner, CWnd* pParent = NULL);
	virtual ~CAvisoEditorDialog();

	enum { IDD = IDD_AVISO_EDITOR_DIALOG };

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
	afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
	afx_msg void OnDestroy();
	afx_msg void OnPaint();
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg void OnObjectListItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnObjectListGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnFilterChanged();
	afx_msg void OnPropertyTabChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnFieldChanged();
	afx_msg void OnApplyClicked();
	afx_msg void OnSaveClicked();
	afx_msg void OnDeleteClicked();
	afx_msg void OnCurrentPageClicked();
	afx_msg void OnLoadAvisoClicked();
	afx_msg void OnMapsJsonClicked();
	afx_msg void OnFilterButtonClicked();
	afx_msg void OnClearFiltersClicked();

	DECLARE_MESSAGE_MAP()

private:
	enum
	{
		IDC_AE_PATH_LABEL = 9401,
		IDC_AE_STATUS_LABEL = 9402,
		IDC_AE_OBJECT_LIST = 9403,
		IDC_AE_CURRENT_PAGE_BUTTON = 9404,
		IDC_AE_SAVE_BUTTON = 9405,
		IDC_AE_LOAD_AVISO_BUTTON = 9406,
		IDC_AE_MAPS_JSON_BUTTON = 9407,
		IDC_AE_DELETE_BUTTON = 9409,
		IDC_AE_APPLY_BUTTON = 9410,
		IDC_AE_VISIBLE_CHECK = 9412,
		IDC_AE_NAME_EDIT = 9413,
		IDC_AE_LAYER_EDIT = 9414,
		IDC_AE_OBJECT_TYPE_EDIT = 9415,
		IDC_AE_GEOMETRY_EDIT = 9416,
		IDC_AE_FILL_EDIT = 9417,
		IDC_AE_FILL_OPACITY_EDIT = 9418,
		IDC_AE_STROKE_EDIT = 9419,
		IDC_AE_STROKE_OPACITY_EDIT = 9420,
		IDC_AE_STROKE_WIDTH_EDIT = 9421,
		IDC_AE_TEXT_EDIT = 9422,
		IDC_AE_TEXT_FONT_EDIT = 9423,
		IDC_AE_TEXT_COLOR_EDIT = 9424,
		IDC_AE_TEXT_SIZE_EDIT = 9425,
		IDC_AE_TEXT_ANCHOR_EDIT = 9426,
		IDC_AE_HALO_COLOR_EDIT = 9427,
		IDC_AE_HALO_WIDTH_EDIT = 9428,
		IDC_AE_LONGITUDE_EDIT = 9429,
		IDC_AE_LATITUDE_EDIT = 9430,
		IDC_AE_COORDINATES_EDIT = 9431,
		IDC_AE_DETAILS_HEADER = 9432,
		IDC_AE_SEARCH_EDIT = 9433,
		IDC_AE_LAYER_FILTER_COMBO = 9434,
		IDC_AE_OBJECT_TYPE_FILTER_COMBO = 9435,
		IDC_AE_GEOMETRY_FILTER_COMBO = 9436,
		IDC_AE_VISIBILITY_FILTER_COMBO = 9437,
		IDC_AE_STYLE_FILTER_COMBO = 9438,
		IDC_AE_PROPERTY_TABS = 9439,
		IDC_AE_RAW_EDIT = 9440,
		IDC_AE_APPLY_SCOPE_COMBO = 9441,
		IDC_AE_CATEGORY_FILTER_COMBO = 9442,
		IDC_AE_SIDEBAR_PANEL = 9444,
		IDC_AE_SIDEBAR_TITLE = 9445,
		IDC_AE_SIDEBAR_DIVIDER = 9446,
		IDC_AE_PAGE_TITLE = 9447,
		IDC_AE_PAGE_SUBTITLE = 9448,
		IDC_AE_BROWSER_PANEL = 9449,
		IDC_AE_INSPECTOR_PANEL = 9450,
		IDC_AE_BROWSER_HEADER = 9451,
		IDC_AE_FILTER_BUTTON = 9452,
		IDC_AE_CLEAR_FILTERS_BUTTON = 9453
	};
	enum FieldDirtyFlags : unsigned int
	{
		kDirtyVisible = 1u << 0,
		kDirtyName = 1u << 1,
		kDirtyLayer = 1u << 2,
		kDirtyObjectType = 1u << 3,
		kDirtyFill = 1u << 4,
		kDirtyFillOpacity = 1u << 5,
		kDirtyStroke = 1u << 6,
		kDirtyStrokeOpacity = 1u << 7,
		kDirtyStrokeWidth = 1u << 8,
		kDirtyText = 1u << 9,
		kDirtyTextFont = 1u << 10,
		kDirtyTextColor = 1u << 11,
		kDirtyTextSize = 1u << 12,
		kDirtyTextAnchor = 1u << 13,
		kDirtyHaloColor = 1u << 14,
		kDirtyHaloWidth = 1u << 15,
		kDirtyLongitude = 1u << 16,
		kDirtyLatitude = 1u << 17,
		kDirtyCoordinates = 1u << 18,
		kDirtyRaw = 1u << 19,
		kDirtyBatchEditableMask = kDirtyVisible | kDirtyFill | kDirtyFillOpacity | kDirtyStroke | kDirtyStrokeOpacity | kDirtyStrokeWidth |
			kDirtyTextFont | kDirtyTextColor | kDirtyTextSize | kDirtyHaloColor | kDirtyHaloWidth
	};

	CSMRRadar* Owner = nullptr;
	bool ControlsCreated = false;
	bool Initialized = false;
	bool UpdatingControls = false;
	bool SelectionRefreshPending = false;
	bool RestoringObjectSelection = false;
	bool Dirty = false;
	bool PendingFieldChanges = false;
	unsigned int DirtyFieldMask = 0;
	int LastSelectedFeatureIndex = -1;
	std::vector<int> LastSelectedFeatureIndices;
	int LastLayoutWidth = -1;
	int LastLayoutHeight = -1;
	std::string LoadedPath;
	AvisoDocumentModel Model;
	rapidjson::Document& Document;
	std::vector<int> FilteredFeatureIndices;
	CRect SidebarPanelRect;
	CRect BrowserPanelRect;
	CRect InspectorPanelRect;

	CStatic PathLabel;
	CStatic StatusLabel;
	CStatic SidebarPanel;
	CStatic SidebarTitle;
	CStatic SidebarDivider;
	CStatic PageTitleLabel;
	CStatic PageSubtitleLabel;
	CStatic BrowserPanel;
	CStatic InspectorPanel;
	CStatic BrowserHeader;
	CStatic SearchLabel;
	CEdit SearchEdit;
	CStatic LayerFilterLabel;
	CComboBox LayerFilterCombo;
	CStatic ObjectTypeFilterLabel;
	CComboBox ObjectTypeFilterCombo;
	CStatic GeometryFilterLabel;
	CComboBox GeometryFilterCombo;
	CStatic VisibilityFilterLabel;
	CComboBox VisibilityFilterCombo;
	CStatic CategoryFilterLabel;
	CComboBox CategoryFilterCombo;
	CStatic StyleFilterLabel;
	CComboBox StyleFilterCombo;
	CStatic ObjectCountLabel;
	CListCtrl ObjectList;
	CButton FilterButton;
	CButton ClearFiltersButton;
	CButton CurrentPageButton;
	CButton LoadAvisoButton;
	CButton MapsJsonButton;
	CButton SaveButton;
	CButton DeleteButton;
	CButton ApplyButton;
	CButton VisibleCheck;
	CStatic DetailsHeader;
	CTabCtrl PropertyTabs;
	CStatic ApplyScopeLabel;
	CComboBox ApplyScopeCombo;
	CStatic NameLabel;
	CEdit NameEdit;
	CStatic LayerLabel;
	CEdit LayerEdit;
	CStatic ObjectTypeLabel;
	CEdit ObjectTypeEdit;
	CStatic GeometryLabel;
	CEdit GeometryEdit;
	CStatic FillLabel;
	CEdit FillEdit;
	CStatic FillOpacityLabel;
	CEdit FillOpacityEdit;
	CStatic StrokeLabel;
	CEdit StrokeEdit;
	CStatic StrokeOpacityLabel;
	CEdit StrokeOpacityEdit;
	CStatic StrokeWidthLabel;
	CEdit StrokeWidthEdit;
	CStatic TextLabel;
	CEdit TextEdit;
	CStatic TextFontLabel;
	CEdit TextFontEdit;
	CStatic TextColorLabel;
	CEdit TextColorEdit;
	CStatic TextSizeLabel;
	CEdit TextSizeEdit;
	CStatic TextAnchorLabel;
	CEdit TextAnchorEdit;
	CStatic HaloColorLabel;
	CEdit HaloColorEdit;
	CStatic HaloWidthLabel;
	CEdit HaloWidthEdit;
	CStatic LongitudeLabel;
	CEdit LongitudeEdit;
	CStatic LatitudeLabel;
	CEdit LatitudeEdit;
	CStatic CoordinatesLabel;
	CEdit CoordinatesEdit;
	CStatic RawLabel;
	CEdit RawEdit;
	CBrush HeaderBarBrush;
	CBrush SidebarBrush;
	CFont TitleFont;
	CFont SectionHeaderFont;
	CFont UniformUiFont;
	CFont MonoFont;

	void CreateEditorControls();
	void LayoutControls();
	void ApplyProfileEditorVisualStyle();
	void ApplyThemedEditBorders();
	void ForceChildRepaint();
	void PopulateFilterCombos();
	void UpdatePropertyTabVisibility();
	void HideAndNotifyOwner();
	bool PromptForUnsavedChanges(const char* actionText);
	void SetStatusText(const std::string& text);
	bool ImportAvisoGeoJsonFromFile(const std::string& sourcePath);
	bool ImportAvisoGeoJsonFromGithubUrl(const std::string& url);
	bool ImportAvisoGeoJsonText(const std::string& geoJsonText, const std::string& sourceHint);
	std::string DetectAirportForAvisoImport(const rapidjson::Document& document, const std::string& sourceHint) const;
	std::string NormalizeGithubGeoJsonUrl(const std::string& url) const;
	std::string ReadClipboardText() const;
	bool LoadDocumentFromPath(const std::string& path, bool keepSelection, const std::string& loadedStatusText);
	bool LoadDocumentFromCurrentAviso(bool keepSelection);
	bool EnsureDocumentForEditing();
	bool SaveDocument(bool reloadAfterSave);
	void PopulateObjectList(int preferredFeatureIndex);
	void UpdateObjectCountText();
	void RestoreObjectSelection(int featureIndex);
	void RestoreObjectSelection(const std::vector<int>& featureIndices);
	bool ResolvePendingChangesBeforeSelectionRefresh();
	AvisoFeatureFilter BuildCurrentFilter() const;
	std::string GetObjectListCellText(int rowIndex, int subItem) const;
	void RefreshFieldsFromSelection();
	void UpdateRawEditForSelection(const rapidjson::Value* feature);
	bool ApplyFieldsToSelectedFeature(bool markDirty, bool showErrors);
	bool ApplyFieldsToFeature(int featureIndex, bool markDirty, bool showErrors, bool batchStyleOnly, bool detachSharedStyle);
	bool ApplyRawJsonToSelectedFeature(bool showErrors);
	bool ApplyBatchFieldsToFeatures(const std::vector<int>& featureIndices, bool showErrors);
	bool ApplyPendingFieldsToCurrentSelection(bool showErrors);
	int GetSelectedFeatureIndex() const;
	std::vector<int> GetSelectedFeatureIndices() const;
	std::vector<int> GetFilteredFeatureIndices() const;
	std::vector<int> GetBatchTargetFeatureIndices() const;
	rapidjson::Value* GetFeatureByIndex(int featureIndex);
	const rapidjson::Value* GetFeatureByIndex(int featureIndex) const;
	rapidjson::Value& EnsureFeatureProperties(rapidjson::Value& feature);
	std::string GetFeatureGeometryType(const rapidjson::Value& feature) const;
	std::string GetFeatureDisplayText(const rapidjson::Value& feature, int featureIndex) const;
	std::string GetFeatureLayer(const rapidjson::Value& feature) const;
	std::string GetFeatureObjectType(const rapidjson::Value& feature) const;
	std::string GetFeatureSearchText(const rapidjson::Value& feature, int featureIndex) const;
	bool FeatureMatchesFilters(const rapidjson::Value& feature, int featureIndex) const;
	bool FeatureMatchesCategory(const rapidjson::Value& feature, const std::string& category) const;
	std::string BuildObjectListLabel(const rapidjson::Value& feature, int featureIndex) const;
	void DeleteFeatureAt(int featureIndex);
	void CloneJsonValue(const rapidjson::Value& source, rapidjson::Value& destination);
	void RefreshAfterDocumentMutation(int selectedFeatureIndex);
	void MarkDirty(bool dirty);

	std::string GetEditText(const CEdit& edit) const;
	void SetEditText(CEdit& edit, const std::string& text);
	void SetEditEnabled(CEdit& edit, bool enabled);
	unsigned int DirtyFlagForControlId(UINT controlId) const;
	bool IsFieldDirty(unsigned int flag) const;
	bool IsAnyGeometryFieldDirty() const;
	bool HasDirtyStyleFields() const;
	bool EnsureDetachedStyleForFeature(rapidjson::Value& feature, int featureIndex);
	void SyncDirtyStyleFieldsToStylePaint(const rapidjson::Value& properties);
	bool TryParseDouble(const std::string& text, double& value) const;
	std::string FormatDouble(double value) const;
	std::string FormatCoordinateDouble(double value) const;
	bool IsPointLabelFeature(const rapidjson::Value& feature) const;
	bool IsEditableTextFeature(const rapidjson::Value& feature) const;
	bool IsPointGeometry(const rapidjson::Value& feature) const;
	bool IsFeatureVisible(const rapidjson::Value* properties) const;
	std::string ReadStringProperty(const rapidjson::Value* properties, const char* key, const std::string& fallback = "") const;
	double ReadNumberProperty(const rapidjson::Value* properties, const char* key, double fallback) const;
	bool ReadBoolProperty(const rapidjson::Value* properties, const char* key, bool fallback) const;
	std::string ReadComboText(CComboBox& combo) const;
	void SelectComboEntryByText(CComboBox& combo, const std::string& text);
	void SetStringMember(rapidjson::Value& object, const char* key, const std::string& value);
	void SetNumberMember(rapidjson::Value& object, const char* key, double value);
	void SetBoolMember(rapidjson::Value& object, const char* key, bool value);
	void RemoveMemberIfExists(rapidjson::Value& object, const char* key);
	std::string GeometryCoordinatesToText(const rapidjson::Value& geometry) const;
	std::string GeometryCoordinatesSummary(const rapidjson::Value& geometry) const;
	bool ApplyCoordinatesTextToGeometry(rapidjson::Value& geometry, const std::string& text, bool showErrors);
};
