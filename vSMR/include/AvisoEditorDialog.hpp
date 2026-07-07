#pragma once

#include "resource.h"
#include "SMRRadar.hpp"
#include "rapidjson/document.h"

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
	afx_msg void OnObjectSelectionChanged();
	afx_msg void OnFieldChanged();
	afx_msg void OnApplyClicked();
	afx_msg void OnSaveClicked();
	afx_msg void OnReloadClicked();
	afx_msg void OnAddLabelClicked();
	afx_msg void OnAddLineClicked();
	afx_msg void OnDuplicateClicked();
	afx_msg void OnDeleteClicked();
	afx_msg void OnCloseClicked();

	DECLARE_MESSAGE_MAP()

private:
	enum
	{
		IDC_AE_PATH_LABEL = 9401,
		IDC_AE_STATUS_LABEL = 9402,
		IDC_AE_OBJECT_LIST = 9403,
		IDC_AE_RELOAD_BUTTON = 9404,
		IDC_AE_SAVE_BUTTON = 9405,
		IDC_AE_ADD_LABEL_BUTTON = 9406,
		IDC_AE_ADD_LINE_BUTTON = 9407,
		IDC_AE_DUPLICATE_BUTTON = 9408,
		IDC_AE_DELETE_BUTTON = 9409,
		IDC_AE_APPLY_BUTTON = 9410,
		IDC_AE_CLOSE_BUTTON = 9411,
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
		IDC_AE_DETAILS_HEADER = 9432
	};

	CSMRRadar* Owner = nullptr;
	bool ControlsCreated = false;
	bool Initialized = false;
	bool UpdatingControls = false;
	bool Dirty = false;
	bool PendingFieldChanges = false;
	int LastSelectedFeatureIndex = -1;
	int LastLayoutWidth = -1;
	int LastLayoutHeight = -1;
	std::string LoadedPath;
	rapidjson::Document Document;

	CStatic PathLabel;
	CStatic StatusLabel;
	CListBox ObjectList;
	CButton ReloadButton;
	CButton SaveButton;
	CButton AddLabelButton;
	CButton AddLineButton;
	CButton DuplicateButton;
	CButton DeleteButton;
	CButton ApplyButton;
	CButton CloseButton;
	CButton VisibleCheck;
	CStatic DetailsHeader;
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

	void CreateEditorControls();
	void LayoutControls();
	void HideAndNotifyOwner();
	void SetStatusText(const std::string& text);
	bool LoadDocumentFromCurrentAviso(bool keepSelection);
	bool EnsureDocumentForEditing();
	bool SaveDocument(bool reloadAfterSave);
	void PopulateObjectList(int preferredFeatureIndex);
	void RefreshFieldsFromSelection();
	bool ApplyFieldsToSelectedFeature(bool markDirty, bool showErrors);
	int GetSelectedFeatureIndex() const;
	rapidjson::Value* GetFeatureByIndex(int featureIndex);
	const rapidjson::Value* GetFeatureByIndex(int featureIndex) const;
	rapidjson::Value& EnsureFeatureProperties(rapidjson::Value& feature);
	std::string GetFeatureGeometryType(const rapidjson::Value& feature) const;
	std::string BuildObjectListLabel(const rapidjson::Value& feature, int featureIndex) const;
	void AddFeature(rapidjson::Value& feature);
	void DeleteFeatureAt(int featureIndex);
	void DuplicateFeatureAt(int featureIndex);
	void BuildLabelFeature(double longitude, double latitude, rapidjson::Value& feature);
	void BuildLineFeature(double longitude, double latitude, rapidjson::Value& feature);
	void CloneJsonValue(const rapidjson::Value& source, rapidjson::Value& destination);
	bool TryGetDefaultInsertPosition(double& longitude, double& latitude) const;
	void RefreshAfterDocumentMutation(int selectedFeatureIndex);
	void MarkDirty(bool dirty);

	std::string GetEditText(const CEdit& edit) const;
	void SetEditText(CEdit& edit, const std::string& text);
	void SetEditEnabled(CEdit& edit, bool enabled);
	bool TryParseDouble(const std::string& text, double& value) const;
	std::string FormatDouble(double value) const;
	bool IsPointLabelFeature(const rapidjson::Value& feature) const;
	std::string ReadStringProperty(const rapidjson::Value* properties, const char* key, const std::string& fallback = "") const;
	double ReadNumberProperty(const rapidjson::Value* properties, const char* key, double fallback) const;
	bool ReadBoolProperty(const rapidjson::Value* properties, const char* key, bool fallback) const;
	void SetStringMember(rapidjson::Value& object, const char* key, const std::string& value);
	void SetNumberMember(rapidjson::Value& object, const char* key, double value);
	void SetBoolMember(rapidjson::Value& object, const char* key, bool value);
	void RemoveMemberIfExists(rapidjson::Value& object, const char* key);
	std::string GeometryCoordinatesToText(const rapidjson::Value& geometry) const;
	bool ApplyCoordinatesTextToGeometry(rapidjson::Value& geometry, const std::string& text, bool showErrors);
};
