#include "stdafx.h"
#include "AvisoEditorDialog.hpp"
#include "SMRRadar.hpp"
#include "afxdialogex.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

IMPLEMENT_DYNAMIC(CAvisoEditorDialog, CDialogEx)

namespace
{
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

	std::string ToUpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	bool EqualsNoCase(const std::string& left, const std::string& right)
	{
		if (left.size() != right.size())
			return false;
		for (size_t i = 0; i < left.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i])))
				return false;
		}
		return true;
	}

	std::vector<std::string> SplitString(const std::string& text, char delimiter)
	{
		std::vector<std::string> parts;
		std::string current;
		for (char c : text)
		{
			if (c == delimiter)
			{
				parts.push_back(TrimAsciiWhitespaceCopy(current));
				current.clear();
			}
			else
			{
				current.push_back(c);
			}
		}
		parts.push_back(TrimAsciiWhitespaceCopy(current));
		return parts;
	}

	bool LooksLikeHexColor(const std::string& text)
	{
		if (text.empty())
			return true;
		if (text.size() != 7 || text[0] != '#')
			return false;
		for (size_t i = 1; i < text.size(); ++i)
		{
			const char c = text[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
				return false;
		}
		return true;
	}

	const char* GeometryTypeFromFeature(const rapidjson::Value& feature)
	{
		if (!feature.IsObject() ||
			!feature.HasMember("geometry") ||
			!feature["geometry"].IsObject() ||
			!feature["geometry"].HasMember("type") ||
			!feature["geometry"]["type"].IsString())
		{
			return "";
		}
		return feature["geometry"]["type"].GetString();
	}
}

CAvisoEditorDialog::CAvisoEditorDialog(CSMRRadar* owner, CWnd* pParent /*=NULL*/)
	: CDialogEx(CAvisoEditorDialog::IDD, pParent),
	Owner(owner)
{
	Document.SetObject();
}

CAvisoEditorDialog::~CAvisoEditorDialog()
{
}

void CAvisoEditorDialog::SetOwner(CSMRRadar* owner)
{
	Owner = owner;
}

void CAvisoEditorDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BOOL CAvisoEditorDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	CWnd* statusWnd = GetDlgItem(IDC_AVISO_EDITOR_STATUS);
	if (statusWnd != nullptr && ::IsWindow(statusWnd->GetSafeHwnd()))
		statusWnd->ShowWindow(SW_HIDE);

	CreateEditorControls();
	Initialized = true;
	SyncFromRadar();
	return TRUE;
}

void CAvisoEditorDialog::SetStatusText(const std::string& text)
{
	if (::IsWindow(StatusLabel.GetSafeHwnd()))
		StatusLabel.SetWindowText(text.c_str());
}

void CAvisoEditorDialog::SyncFromRadar()
{
	if (!ControlsCreated)
		return;

	LoadDocumentFromCurrentAviso(true);
	LayoutControls();
}

void CAvisoEditorDialog::CreateEditorControls()
{
	if (ControlsCreated)
		return;

	const DWORD staticStyle = WS_CHILD | WS_VISIBLE;
	const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
	const DWORD multilineEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
	const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

	PathLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_AE_PATH_LABEL);
	StatusLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_AE_STATUS_LABEL);
	ObjectList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, CRect(0, 0, 0, 0), this, IDC_AE_OBJECT_LIST);
	ReloadButton.Create("Reload", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_RELOAD_BUTTON);
	SaveButton.Create("Save", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_SAVE_BUTTON);
	AddLabelButton.Create("Add Label", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_ADD_LABEL_BUTTON);
	AddLineButton.Create("Add Line", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_ADD_LINE_BUTTON);
	DuplicateButton.Create("Duplicate", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_DUPLICATE_BUTTON);
	DeleteButton.Create("Delete", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_DELETE_BUTTON);
	ApplyButton.Create("Apply", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_APPLY_BUTTON);
	CloseButton.Create("Close", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_CLOSE_BUTTON);
	VisibleCheck.Create("Visible", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_AE_VISIBLE_CHECK);
	DetailsHeader.Create("Object properties", staticStyle, CRect(0, 0, 0, 0), this, IDC_AE_DETAILS_HEADER);

	NameLabel.Create("Name", staticStyle, CRect(0, 0, 0, 0), this);
	NameEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_NAME_EDIT);
	LayerLabel.Create("Layer", staticStyle, CRect(0, 0, 0, 0), this);
	LayerEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_LAYER_EDIT);
	ObjectTypeLabel.Create("Object type", staticStyle, CRect(0, 0, 0, 0), this);
	ObjectTypeEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_OBJECT_TYPE_EDIT);
	GeometryLabel.Create("Geometry", staticStyle, CRect(0, 0, 0, 0), this);
	GeometryEdit.Create(editStyle | ES_READONLY, CRect(0, 0, 0, 0), this, IDC_AE_GEOMETRY_EDIT);
	FillLabel.Create("Fill", staticStyle, CRect(0, 0, 0, 0), this);
	FillEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_FILL_EDIT);
	FillOpacityLabel.Create("Fill opacity", staticStyle, CRect(0, 0, 0, 0), this);
	FillOpacityEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_FILL_OPACITY_EDIT);
	StrokeLabel.Create("Stroke", staticStyle, CRect(0, 0, 0, 0), this);
	StrokeEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_STROKE_EDIT);
	StrokeOpacityLabel.Create("Stroke opacity", staticStyle, CRect(0, 0, 0, 0), this);
	StrokeOpacityEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_STROKE_OPACITY_EDIT);
	StrokeWidthLabel.Create("Line width", staticStyle, CRect(0, 0, 0, 0), this);
	StrokeWidthEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_STROKE_WIDTH_EDIT);
	TextLabel.Create("Text", staticStyle, CRect(0, 0, 0, 0), this);
	TextEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_EDIT);
	TextFontLabel.Create("Text font", staticStyle, CRect(0, 0, 0, 0), this);
	TextFontEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_FONT_EDIT);
	TextColorLabel.Create("Text color", staticStyle, CRect(0, 0, 0, 0), this);
	TextColorEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_COLOR_EDIT);
	TextSizeLabel.Create("Text size", staticStyle, CRect(0, 0, 0, 0), this);
	TextSizeEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_SIZE_EDIT);
	TextAnchorLabel.Create("Text anchor", staticStyle, CRect(0, 0, 0, 0), this);
	TextAnchorEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_ANCHOR_EDIT);
	HaloColorLabel.Create("Halo color", staticStyle, CRect(0, 0, 0, 0), this);
	HaloColorEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_HALO_COLOR_EDIT);
	HaloWidthLabel.Create("Halo width", staticStyle, CRect(0, 0, 0, 0), this);
	HaloWidthEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_HALO_WIDTH_EDIT);
	LongitudeLabel.Create("Longitude", staticStyle, CRect(0, 0, 0, 0), this);
	LongitudeEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_LONGITUDE_EDIT);
	LatitudeLabel.Create("Latitude", staticStyle, CRect(0, 0, 0, 0), this);
	LatitudeEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_LATITUDE_EDIT);
	CoordinatesLabel.Create("Coordinates", staticStyle, CRect(0, 0, 0, 0), this);
	CoordinatesEdit.Create(multilineEditStyle, CRect(0, 0, 0, 0), this, IDC_AE_COORDINATES_EDIT);

	ControlsCreated = true;
	LayoutControls();
}

void CAvisoEditorDialog::LayoutControls()
{
	if (!ControlsCreated || !::IsWindow(GetSafeHwnd()))
		return;

	CRect client;
	GetClientRect(&client);
	const int margin = 8;
	const int gap = 6;
	const int buttonH = 24;
	const int rowH = 23;
	const int topH = 38;
	const int leftW = std::clamp(client.Width() / 3, 220, 310);
	const int listBottom = client.bottom - margin - ((buttonH + gap) * 3);
	const int rightX = margin + leftW + 12;
	const int rightW = (std::max)(120, static_cast<int>(client.right) - rightX - margin);

	PathLabel.MoveWindow(margin, margin, client.Width() - margin * 2, 16);
	StatusLabel.MoveWindow(margin, margin + 18, client.Width() - margin * 2, 16);
	ObjectList.MoveWindow(margin, margin + topH, leftW, (std::max)(80, listBottom - (margin + topH)));

	int leftButtonY = listBottom + gap;
	const int halfButtonW = (leftW - gap) / 2;
	ReloadButton.MoveWindow(margin, leftButtonY, halfButtonW, buttonH);
	SaveButton.MoveWindow(margin + halfButtonW + gap, leftButtonY, halfButtonW, buttonH);
	leftButtonY += buttonH + gap;
	AddLabelButton.MoveWindow(margin, leftButtonY, halfButtonW, buttonH);
	AddLineButton.MoveWindow(margin + halfButtonW + gap, leftButtonY, halfButtonW, buttonH);
	leftButtonY += buttonH + gap;
	DuplicateButton.MoveWindow(margin, leftButtonY, halfButtonW, buttonH);
	DeleteButton.MoveWindow(margin + halfButtonW + gap, leftButtonY, halfButtonW, buttonH);

	int y = margin + topH;
	DetailsHeader.MoveWindow(rightX, y, rightW, 18);
	VisibleCheck.MoveWindow(rightX + rightW - 86, y - 2, 86, 20);
	y += 24;

	const int labelW = 86;
	const int editW = (std::max)(80, (rightW - labelW * 2 - gap * 3) / 2);
	auto movePair = [&](CStatic& label, CEdit& edit, int column, int rowY)
	{
		const int columnX = rightX + column * (labelW + editW + gap);
		label.MoveWindow(columnX, rowY + 4, labelW, 16);
		edit.MoveWindow(columnX + labelW, rowY, editW, 20);
	};
	auto moveFull = [&](CStatic& label, CEdit& edit, int rowY, int height)
	{
		label.MoveWindow(rightX, rowY + 4, labelW, 16);
		edit.MoveWindow(rightX + labelW, rowY, rightW - labelW, height);
	};

	movePair(NameLabel, NameEdit, 0, y);
	movePair(LayerLabel, LayerEdit, 1, y);
	y += rowH;
	movePair(ObjectTypeLabel, ObjectTypeEdit, 0, y);
	movePair(GeometryLabel, GeometryEdit, 1, y);
	y += rowH + 2;
	movePair(FillLabel, FillEdit, 0, y);
	movePair(FillOpacityLabel, FillOpacityEdit, 1, y);
	y += rowH;
	movePair(StrokeLabel, StrokeEdit, 0, y);
	movePair(StrokeOpacityLabel, StrokeOpacityEdit, 1, y);
	y += rowH;
	movePair(StrokeWidthLabel, StrokeWidthEdit, 0, y);
	y += rowH + 2;
	moveFull(TextLabel, TextEdit, y, 20);
	y += rowH;
	movePair(TextFontLabel, TextFontEdit, 0, y);
	movePair(TextColorLabel, TextColorEdit, 1, y);
	y += rowH;
	movePair(TextSizeLabel, TextSizeEdit, 0, y);
	movePair(TextAnchorLabel, TextAnchorEdit, 1, y);
	y += rowH;
	movePair(HaloColorLabel, HaloColorEdit, 0, y);
	movePair(HaloWidthLabel, HaloWidthEdit, 1, y);
	y += rowH;
	movePair(LongitudeLabel, LongitudeEdit, 0, y);
	movePair(LatitudeLabel, LatitudeEdit, 1, y);
	y += rowH + 2;
	const int bottomButtonY = client.bottom - margin - buttonH;
	const int coordinatesH = (std::max)(50, bottomButtonY - y - gap);
	moveFull(CoordinatesLabel, CoordinatesEdit, y, coordinatesH);

	const int actionW = 92;
	CloseButton.MoveWindow(client.right - margin - actionW, bottomButtonY, actionW, buttonH);
	ApplyButton.MoveWindow(client.right - margin - actionW * 2 - gap, bottomButtonY, actionW, buttonH);
}

void CAvisoEditorDialog::HideAndNotifyOwner()
{
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnAvisoEditorWindowClosed();
}

void CAvisoEditorDialog::OnCancel()
{
	HideAndNotifyOwner();
}

void CAvisoEditorDialog::OnOK()
{
	OnApplyClicked();
}

void CAvisoEditorDialog::OnClose()
{
	HideAndNotifyOwner();
}

void CAvisoEditorDialog::OnMove(int x, int y)
{
	CDialogEx::OnMove(x, y);
}

void CAvisoEditorDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (!ControlsCreated || nType == SIZE_MINIMIZED || cx <= 0 || cy <= 0)
		return;
	if (LastLayoutWidth == cx && LastLayoutHeight == cy)
		return;
	LastLayoutWidth = cx;
	LastLayoutHeight = cy;
	LayoutControls();
}

void CAvisoEditorDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI != nullptr)
	{
		lpMMI->ptMinTrackSize.x = 560;
		lpMMI->ptMinTrackSize.y = 380;
	}
}

void CAvisoEditorDialog::OnObjectSelectionChanged()
{
	RefreshFieldsFromSelection();
}

void CAvisoEditorDialog::OnFieldChanged()
{
	if (UpdatingControls)
		return;
	PendingFieldChanges = true;
	SetStatusText("Field changes pending. Apply writes them to the AVISO and reloads the view.");
}

void CAvisoEditorDialog::OnApplyClicked()
{
	if (!ApplyFieldsToSelectedFeature(true, true))
		return;
	SaveDocument(true);
}

void CAvisoEditorDialog::OnSaveClicked()
{
	if (PendingFieldChanges && !ApplyFieldsToSelectedFeature(true, true))
		return;
	SaveDocument(true);
}

void CAvisoEditorDialog::OnReloadClicked()
{
	LoadDocumentFromCurrentAviso(true);
	if (Owner != nullptr)
		Owner->ForceReloadAvisoGeoJson();
}

void CAvisoEditorDialog::OnAddLabelClicked()
{
	if (!EnsureDocumentForEditing())
		return;

	double longitude = 0.0;
	double latitude = 0.0;
	TryGetDefaultInsertPosition(longitude, latitude);
	rapidjson::Value feature;
	BuildLabelFeature(longitude, latitude, feature);
	AddFeature(feature);
}

void CAvisoEditorDialog::OnAddLineClicked()
{
	if (!EnsureDocumentForEditing())
		return;

	double longitude = 0.0;
	double latitude = 0.0;
	TryGetDefaultInsertPosition(longitude, latitude);
	rapidjson::Value feature;
	BuildLineFeature(longitude, latitude, feature);
	AddFeature(feature);
}

void CAvisoEditorDialog::OnDuplicateClicked()
{
	const int featureIndex = GetSelectedFeatureIndex();
	if (featureIndex >= 0)
		DuplicateFeatureAt(featureIndex);
}

void CAvisoEditorDialog::OnDeleteClicked()
{
	const int featureIndex = GetSelectedFeatureIndex();
	if (featureIndex >= 0)
		DeleteFeatureAt(featureIndex);
}

void CAvisoEditorDialog::OnCloseClicked()
{
	HideAndNotifyOwner();
}

bool CAvisoEditorDialog::EnsureDocumentForEditing()
{
	if (!Document.IsObject())
		Document.SetObject();

	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	if (!Document.HasMember("type") || !Document["type"].IsString())
	{
		if (Document.HasMember("type"))
			Document.RemoveMember("type");
		rapidjson::Value typeValue;
		typeValue.SetString("FeatureCollection", allocator);
		Document.AddMember("type", typeValue, allocator);
	}

	if (!Document.HasMember("features") || !Document["features"].IsArray())
	{
		if (Document.HasMember("features"))
			Document.RemoveMember("features");
		rapidjson::Value features(rapidjson::kArrayType);
		Document.AddMember("features", features, allocator);
	}

	return true;
}

bool CAvisoEditorDialog::LoadDocumentFromCurrentAviso(bool keepSelection)
{
	if (Owner == nullptr)
	{
		SetStatusText("No active radar screen is available.");
		return false;
	}

	const int previousSelection = keepSelection ? GetSelectedFeatureIndex() : -1;
	LoadedPath = Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
	PathLabel.SetWindowText(LoadedPath.empty() ? "AVISO path: unavailable" : ("AVISO path: " + LoadedPath).c_str());

	if (LoadedPath.empty())
	{
		Document.SetObject();
		PopulateObjectList(-1);
		SetStatusText("No active airport is available for AVISO editing.");
		return false;
	}

	try
	{
		if (std::filesystem::exists(LoadedPath))
		{
			std::ifstream input(LoadedPath, std::ios::binary);
			if (!input.is_open())
			{
				SetStatusText("Unable to open AVISO file.");
				return false;
			}

			std::stringstream buffer;
			buffer << input.rdbuf();
			const std::string json = buffer.str();
			Document.Parse<0>(json.c_str());
			if (Document.HasParseError())
			{
				SetStatusText("AVISO GeoJSON parse failed at offset " + std::to_string(Document.GetErrorOffset()) + ".");
				Document.SetObject();
				PopulateObjectList(-1);
				return false;
			}
		}
		else
		{
			Document.SetObject();
			EnsureDocumentForEditing();
			SetStatusText("New AVISO file will be created on save.");
		}
	}
	catch (const std::exception& ex)
	{
		SetStatusText("AVISO load failed: " + std::string(ex.what()));
		return false;
	}
	catch (...)
	{
		SetStatusText("AVISO load failed.");
		return false;
	}

	EnsureDocumentForEditing();
	Dirty = false;
	PendingFieldChanges = false;
	PopulateObjectList(previousSelection);
	SetStatusText("AVISO loaded. Select an object to edit.");
	return true;
}

bool CAvisoEditorDialog::SaveDocument(bool reloadAfterSave)
{
	if (LoadedPath.empty())
	{
		SetStatusText("No AVISO path is available.");
		return false;
	}

	if (!EnsureDocumentForEditing())
		return false;

	try
	{
		const std::filesystem::path outputPath(LoadedPath);
		if (outputPath.has_parent_path())
			std::filesystem::create_directories(outputPath.parent_path());

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.SetIndent('\t', 1);
		Document.Accept(writer);

		std::ofstream output(LoadedPath, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			SetStatusText("Unable to write AVISO file.");
			return false;
		}
		output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.Size()));
		output.close();
	}
	catch (const std::exception& ex)
	{
		SetStatusText("AVISO save failed: " + std::string(ex.what()));
		return false;
	}
	catch (...)
	{
		SetStatusText("AVISO save failed.");
		return false;
	}

	Dirty = false;
	PendingFieldChanges = false;
	SetStatusText("AVISO saved and reloaded.");
	if (reloadAfterSave && Owner != nullptr)
		Owner->ForceReloadAvisoGeoJson();
	return true;
}

int CAvisoEditorDialog::GetSelectedFeatureIndex() const
{
	if (!::IsWindow(ObjectList.GetSafeHwnd()))
		return -1;

	const int selection = ObjectList.GetCurSel();
	if (selection == LB_ERR)
		return -1;

	const DWORD_PTR itemData = ObjectList.GetItemData(selection);
	if (itemData == static_cast<DWORD_PTR>(LB_ERR))
		return -1;
	return static_cast<int>(itemData);
}

rapidjson::Value* CAvisoEditorDialog::GetFeatureByIndex(int featureIndex)
{
	if (!Document.IsObject() ||
		!Document.HasMember("features") ||
		!Document["features"].IsArray() ||
		featureIndex < 0 ||
		static_cast<rapidjson::SizeType>(featureIndex) >= Document["features"].Size())
	{
		return nullptr;
	}
	return &Document["features"][static_cast<rapidjson::SizeType>(featureIndex)];
}

const rapidjson::Value* CAvisoEditorDialog::GetFeatureByIndex(int featureIndex) const
{
	return const_cast<CAvisoEditorDialog*>(this)->GetFeatureByIndex(featureIndex);
}

rapidjson::Value& CAvisoEditorDialog::EnsureFeatureProperties(rapidjson::Value& feature)
{
	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	if (!feature.HasMember("properties") || !feature["properties"].IsObject())
	{
		if (feature.HasMember("properties"))
			feature.RemoveMember("properties");
		rapidjson::Value properties(rapidjson::kObjectType);
		feature.AddMember("properties", properties, allocator);
	}
	return feature["properties"];
}

std::string CAvisoEditorDialog::GetFeatureGeometryType(const rapidjson::Value& feature) const
{
	return GeometryTypeFromFeature(feature);
}

bool CAvisoEditorDialog::IsPointLabelFeature(const rapidjson::Value& feature) const
{
	if (GetFeatureGeometryType(feature) != "Point")
		return false;

	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	const std::string role = ToUpperAscii(ReadStringProperty(properties, "geometry_role"));
	if (role == "TEXT_LABEL")
		return true;
	return !ReadStringProperty(properties, "text-field").empty() ||
		!ReadStringProperty(properties, "text").empty() ||
		!ReadStringProperty(properties, "label").empty() ||
		!ReadStringProperty(properties, "name").empty();
}

std::string CAvisoEditorDialog::BuildObjectListLabel(const rapidjson::Value& feature, int featureIndex) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	const bool visible = ReadBoolProperty(properties, "visible", true);
	const std::string geometryType = GetFeatureGeometryType(feature).empty() ? "Feature" : GetFeatureGeometryType(feature);
	std::string label = ReadStringProperty(properties, "name");
	if (label.empty() && IsPointLabelFeature(feature))
		label = ReadStringProperty(properties, "text-field", ReadStringProperty(properties, "text", ReadStringProperty(properties, "label")));
	if (label.empty())
		label = ReadStringProperty(properties, "layer");
	if (label.empty())
		label = "Object " + std::to_string(featureIndex + 1);

	if (!visible)
		label = "[hidden] " + label;

	label += " (";
	label += geometryType;
	label += ")";
	return label;
}

void CAvisoEditorDialog::PopulateObjectList(int preferredFeatureIndex)
{
	UpdatingControls = true;
	ObjectList.ResetContent();

	if (Document.IsObject() && Document.HasMember("features") && Document["features"].IsArray())
	{
		const rapidjson::Value& features = Document["features"];
		for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
		{
			const std::string label = BuildObjectListLabel(features[i], static_cast<int>(i));
			const int listIndex = ObjectList.AddString(label.c_str());
			if (listIndex != LB_ERR)
				ObjectList.SetItemData(listIndex, static_cast<DWORD_PTR>(i));
		}
	}

	int selectionToSet = -1;
	for (int i = 0; i < ObjectList.GetCount(); ++i)
	{
		if (static_cast<int>(ObjectList.GetItemData(i)) == preferredFeatureIndex)
		{
			selectionToSet = i;
			break;
		}
	}
	if (selectionToSet < 0 && ObjectList.GetCount() > 0)
		selectionToSet = 0;
	if (selectionToSet >= 0)
		ObjectList.SetCurSel(selectionToSet);

	UpdatingControls = false;
	RefreshFieldsFromSelection();
}

void CAvisoEditorDialog::RefreshFieldsFromSelection()
{
	UpdatingControls = true;
	const int featureIndex = GetSelectedFeatureIndex();
	LastSelectedFeatureIndex = featureIndex;
	const rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
	const bool hasFeature = feature != nullptr && feature->IsObject();
	const rapidjson::Value* properties = nullptr;
	if (hasFeature && feature->HasMember("properties") && (*feature)["properties"].IsObject())
		properties = &(*feature)["properties"];

	VisibleCheck.SetCheck(ReadBoolProperty(properties, "visible", true) ? BST_CHECKED : BST_UNCHECKED);
	SetEditText(NameEdit, ReadStringProperty(properties, "name"));
	SetEditText(LayerEdit, ReadStringProperty(properties, "layer"));
	SetEditText(ObjectTypeEdit, ReadStringProperty(properties, "object_type", ReadStringProperty(properties, "type")));
	SetEditText(GeometryEdit, hasFeature ? GetFeatureGeometryType(*feature) : "");
	SetEditText(FillEdit, ReadStringProperty(properties, "fill"));
	SetEditText(FillOpacityEdit, properties != nullptr && properties->HasMember("fill-opacity") && (*properties)["fill-opacity"].IsNumber() ? FormatDouble((*properties)["fill-opacity"].GetDouble()) : "");
	SetEditText(StrokeEdit, ReadStringProperty(properties, "stroke"));
	SetEditText(StrokeOpacityEdit, properties != nullptr && properties->HasMember("stroke-opacity") && (*properties)["stroke-opacity"].IsNumber() ? FormatDouble((*properties)["stroke-opacity"].GetDouble()) : "");
	SetEditText(StrokeWidthEdit, properties != nullptr && properties->HasMember("stroke-width") && (*properties)["stroke-width"].IsNumber() ? FormatDouble((*properties)["stroke-width"].GetDouble()) : "");

	const bool labelFeature = hasFeature && IsPointLabelFeature(*feature);
	SetEditText(TextEdit, ReadStringProperty(properties, "text-field", ReadStringProperty(properties, "text", ReadStringProperty(properties, "label"))));
	SetEditText(TextFontEdit, ReadStringProperty(properties, "text-font", "Arial"));
	SetEditText(TextColorEdit, ReadStringProperty(properties, "text-color"));
	SetEditText(TextSizeEdit, properties != nullptr && properties->HasMember("text-size") && (*properties)["text-size"].IsNumber() ? FormatDouble((*properties)["text-size"].GetDouble()) : "");
	SetEditText(TextAnchorEdit, ReadStringProperty(properties, "text-anchor", "center"));
	SetEditText(HaloColorEdit, ReadStringProperty(properties, "text-halo-color"));
	SetEditText(HaloWidthEdit, properties != nullptr && properties->HasMember("text-halo-width") && (*properties)["text-halo-width"].IsNumber() ? FormatDouble((*properties)["text-halo-width"].GetDouble()) : "");

	double longitude = 0.0;
	double latitude = 0.0;
	if (hasFeature &&
		feature->HasMember("geometry") &&
		(*feature)["geometry"].IsObject() &&
		(*feature)["geometry"].HasMember("coordinates") &&
		(*feature)["geometry"]["coordinates"].IsArray() &&
		(*feature)["geometry"]["coordinates"].Size() >= 2 &&
		(*feature)["geometry"]["coordinates"][static_cast<rapidjson::SizeType>(0)].IsNumber() &&
		(*feature)["geometry"]["coordinates"][static_cast<rapidjson::SizeType>(1)].IsNumber())
	{
		longitude = (*feature)["geometry"]["coordinates"][static_cast<rapidjson::SizeType>(0)].GetDouble();
		latitude = (*feature)["geometry"]["coordinates"][static_cast<rapidjson::SizeType>(1)].GetDouble();
		SetEditText(LongitudeEdit, FormatDouble(longitude));
		SetEditText(LatitudeEdit, FormatDouble(latitude));
	}
	else
	{
		SetEditText(LongitudeEdit, "");
		SetEditText(LatitudeEdit, "");
	}

	if (hasFeature && feature->HasMember("geometry") && (*feature)["geometry"].IsObject())
		SetEditText(CoordinatesEdit, GeometryCoordinatesToText((*feature)["geometry"]));
	else
		SetEditText(CoordinatesEdit, "");

	VisibleCheck.EnableWindow(hasFeature);
	SetEditEnabled(NameEdit, hasFeature);
	SetEditEnabled(LayerEdit, hasFeature);
	SetEditEnabled(ObjectTypeEdit, hasFeature);
	SetEditEnabled(FillEdit, hasFeature);
	SetEditEnabled(FillOpacityEdit, hasFeature);
	SetEditEnabled(StrokeEdit, hasFeature);
	SetEditEnabled(StrokeOpacityEdit, hasFeature);
	SetEditEnabled(StrokeWidthEdit, hasFeature);
	SetEditEnabled(TextEdit, labelFeature);
	SetEditEnabled(TextFontEdit, labelFeature);
	SetEditEnabled(TextColorEdit, labelFeature);
	SetEditEnabled(TextSizeEdit, labelFeature);
	SetEditEnabled(TextAnchorEdit, labelFeature);
	SetEditEnabled(HaloColorEdit, labelFeature);
	SetEditEnabled(HaloWidthEdit, labelFeature);
	SetEditEnabled(LongitudeEdit, labelFeature);
	SetEditEnabled(LatitudeEdit, labelFeature);
	SetEditEnabled(CoordinatesEdit, hasFeature && !labelFeature);
	ApplyButton.EnableWindow(hasFeature);
	DuplicateButton.EnableWindow(hasFeature);
	DeleteButton.EnableWindow(hasFeature);

	PendingFieldChanges = false;
	UpdatingControls = false;
}

bool CAvisoEditorDialog::ApplyFieldsToSelectedFeature(bool markDirty, bool showErrors)
{
	rapidjson::Value* feature = GetFeatureByIndex(GetSelectedFeatureIndex());
	if (feature == nullptr || !feature->IsObject())
		return false;

	rapidjson::Value& properties = EnsureFeatureProperties(*feature);
	SetBoolMember(properties, "visible", VisibleCheck.GetCheck() == BST_CHECKED);

	auto setOptionalString = [&](const char* key, const std::string& value)
	{
		const std::string trimmed = TrimAsciiWhitespaceCopy(value);
		if (trimmed.empty())
			RemoveMemberIfExists(properties, key);
		else
			SetStringMember(properties, key, trimmed);
	};
	auto setOptionalNumber = [&](const char* key, const std::string& value, double minValue, double maxValue) -> bool
	{
		const std::string trimmed = TrimAsciiWhitespaceCopy(value);
		if (trimmed.empty())
		{
			RemoveMemberIfExists(properties, key);
			return true;
		}

		double parsed = 0.0;
		if (!TryParseDouble(trimmed, parsed) || parsed < minValue || parsed > maxValue)
		{
			if (showErrors)
				SetStatusText(std::string("Invalid numeric value for ") + key + ".");
			return false;
		}
		SetNumberMember(properties, key, parsed);
		return true;
	};
	auto setOptionalHex = [&](const char* key, const std::string& value) -> bool
	{
		const std::string trimmed = TrimAsciiWhitespaceCopy(value);
		if (!LooksLikeHexColor(trimmed))
		{
			if (showErrors)
				SetStatusText(std::string("Invalid color for ") + key + ". Use #RRGGBB.");
			return false;
		}
		if (trimmed.empty())
			RemoveMemberIfExists(properties, key);
		else
			SetStringMember(properties, key, trimmed);
		return true;
	};

	setOptionalString("name", GetEditText(NameEdit));
	setOptionalString("layer", GetEditText(LayerEdit));
	setOptionalString("object_type", GetEditText(ObjectTypeEdit));
	RemoveMemberIfExists(properties, "type");

	if (!setOptionalHex("fill", GetEditText(FillEdit)) ||
		!setOptionalNumber("fill-opacity", GetEditText(FillOpacityEdit), 0.0, 1.0) ||
		!setOptionalHex("stroke", GetEditText(StrokeEdit)) ||
		!setOptionalNumber("stroke-opacity", GetEditText(StrokeOpacityEdit), 0.0, 1.0) ||
		!setOptionalNumber("stroke-width", GetEditText(StrokeWidthEdit), 0.0, 32.0))
	{
		return false;
	}

	if (!feature->HasMember("geometry") || !(*feature)["geometry"].IsObject())
	{
		if (showErrors)
			SetStatusText("Selected object has no editable geometry.");
		return false;
	}

	rapidjson::Value& geometry = (*feature)["geometry"];
	const bool labelFeature = IsPointLabelFeature(*feature);
	if (labelFeature)
	{
		double longitude = 0.0;
		double latitude = 0.0;
		if (!TryParseDouble(GetEditText(LongitudeEdit), longitude) || !TryParseDouble(GetEditText(LatitudeEdit), latitude))
		{
			if (showErrors)
				SetStatusText("Invalid label longitude or latitude.");
			return false;
		}

		SetStringMember(properties, "geometry_role", "TEXT_LABEL");
		setOptionalString("text-field", GetEditText(TextEdit).empty() ? "New label" : GetEditText(TextEdit));
		setOptionalString("text-font", GetEditText(TextFontEdit).empty() ? "Arial" : GetEditText(TextFontEdit));
		if (!setOptionalHex("text-color", GetEditText(TextColorEdit)) ||
			!setOptionalNumber("text-size", GetEditText(TextSizeEdit), 1.0, 128.0) ||
			!setOptionalHex("text-halo-color", GetEditText(HaloColorEdit)) ||
			!setOptionalNumber("text-halo-width", GetEditText(HaloWidthEdit), 0.0, 32.0))
		{
			return false;
		}
		setOptionalString("text-anchor", GetEditText(TextAnchorEdit).empty() ? "center" : GetEditText(TextAnchorEdit));

		rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
		if (geometry.HasMember("coordinates"))
			geometry.RemoveMember("coordinates");
		rapidjson::Value coordinates(rapidjson::kArrayType);
		coordinates.PushBack(longitude, allocator);
		coordinates.PushBack(latitude, allocator);
		geometry.AddMember("coordinates", coordinates, allocator);
	}
	else if (!ApplyCoordinatesTextToGeometry(geometry, GetEditText(CoordinatesEdit), showErrors))
	{
		return false;
	}

	if (markDirty)
		MarkDirty(true);
	PendingFieldChanges = false;
	PopulateObjectList(GetSelectedFeatureIndex());
	return true;
}

void CAvisoEditorDialog::AddFeature(rapidjson::Value& feature)
{
	EnsureDocumentForEditing();
	rapidjson::Value& features = Document["features"];
	const int newIndex = static_cast<int>(features.Size());
	features.PushBack(feature, Document.GetAllocator());
	RefreshAfterDocumentMutation(newIndex);
}

void CAvisoEditorDialog::DeleteFeatureAt(int featureIndex)
{
	EnsureDocumentForEditing();
	rapidjson::Value& features = Document["features"];
	if (featureIndex < 0 || static_cast<rapidjson::SizeType>(featureIndex) >= features.Size())
		return;

	for (rapidjson::SizeType i = static_cast<rapidjson::SizeType>(featureIndex); i + 1 < features.Size(); ++i)
		features[i] = features[i + 1];
	features.PopBack();
	RefreshAfterDocumentMutation((std::min)(featureIndex, static_cast<int>(features.Size()) - 1));
}

void CAvisoEditorDialog::DuplicateFeatureAt(int featureIndex)
{
	const rapidjson::Value* source = GetFeatureByIndex(featureIndex);
	if (source == nullptr)
		return;

	rapidjson::Value copy;
	CloneJsonValue(*source, copy);
	rapidjson::Value& properties = EnsureFeatureProperties(copy);
	const std::string baseName = ReadStringProperty(&properties, "name", BuildObjectListLabel(*source, featureIndex));
	SetStringMember(properties, "name", baseName + " Copy");
	AddFeature(copy);
}

void CAvisoEditorDialog::BuildLabelFeature(double longitude, double latitude, rapidjson::Value& feature)
{
	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	feature.SetObject();
	SetStringMember(feature, "type", "Feature");

	rapidjson::Value properties(rapidjson::kObjectType);
	rapidjson::Value value;
	value.SetString("New label", allocator);
	properties.AddMember("name", value, allocator);
	value.SetString("labels", allocator);
	properties.AddMember("layer", value, allocator);
	value.SetString("TEXT_LABEL", allocator);
	properties.AddMember("geometry_role", value, allocator);
	value.SetString("New label", allocator);
	properties.AddMember("text-field", value, allocator);
	value.SetString("Arial", allocator);
	properties.AddMember("text-font", value, allocator);
	value.SetString("#A0A0A0", allocator);
	properties.AddMember("text-color", value, allocator);
	properties.AddMember("text-size", 12.0, allocator);
	value.SetString("center", allocator);
	properties.AddMember("text-anchor", value, allocator);
	value.SetString("#000000", allocator);
	properties.AddMember("text-halo-color", value, allocator);
	properties.AddMember("text-halo-width", 1.0, allocator);
	properties.AddMember("visible", true, allocator);
	feature.AddMember("properties", properties, allocator);

	rapidjson::Value geometry(rapidjson::kObjectType);
	value.SetString("Point", allocator);
	geometry.AddMember("type", value, allocator);
	rapidjson::Value coordinates(rapidjson::kArrayType);
	coordinates.PushBack(longitude, allocator);
	coordinates.PushBack(latitude, allocator);
	geometry.AddMember("coordinates", coordinates, allocator);
	feature.AddMember("geometry", geometry, allocator);
}

void CAvisoEditorDialog::BuildLineFeature(double longitude, double latitude, rapidjson::Value& feature)
{
	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	feature.SetObject();
	SetStringMember(feature, "type", "Feature");

	rapidjson::Value properties(rapidjson::kObjectType);
	rapidjson::Value value;
	value.SetString("New line", allocator);
	properties.AddMember("name", value, allocator);
	value.SetString("lines", allocator);
	properties.AddMember("layer", value, allocator);
	value.SetString("line", allocator);
	properties.AddMember("object_type", value, allocator);
	value.SetString("#8C98AA", allocator);
	properties.AddMember("stroke", value, allocator);
	properties.AddMember("stroke-opacity", 0.85, allocator);
	properties.AddMember("stroke-width", 1.0, allocator);
	properties.AddMember("visible", true, allocator);
	feature.AddMember("properties", properties, allocator);

	rapidjson::Value geometry(rapidjson::kObjectType);
	value.SetString("MultiLineString", allocator);
	geometry.AddMember("type", value, allocator);
	rapidjson::Value allLines(rapidjson::kArrayType);
	rapidjson::Value line(rapidjson::kArrayType);
	const double delta = 0.001;
	rapidjson::Value pointA(rapidjson::kArrayType);
	pointA.PushBack(longitude - delta, allocator);
	pointA.PushBack(latitude, allocator);
	rapidjson::Value pointB(rapidjson::kArrayType);
	pointB.PushBack(longitude + delta, allocator);
	pointB.PushBack(latitude, allocator);
	line.PushBack(pointA, allocator);
	line.PushBack(pointB, allocator);
	allLines.PushBack(line, allocator);
	geometry.AddMember("coordinates", allLines, allocator);
	feature.AddMember("geometry", geometry, allocator);
}

void CAvisoEditorDialog::CloneJsonValue(const rapidjson::Value& source, rapidjson::Value& destination)
{
	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	if (source.IsObject())
	{
		destination.SetObject();
		for (rapidjson::Value::ConstMemberIterator it = source.MemberBegin(); it != source.MemberEnd(); ++it)
		{
			rapidjson::Value key;
			key.SetString(it->name.GetString(), static_cast<rapidjson::SizeType>(std::strlen(it->name.GetString())), allocator);
			rapidjson::Value value;
			CloneJsonValue(it->value, value);
			destination.AddMember(key, value, allocator);
		}
		return;
	}

	if (source.IsArray())
	{
		destination.SetArray();
		for (rapidjson::SizeType i = 0; i < source.Size(); ++i)
		{
			rapidjson::Value value;
			CloneJsonValue(source[i], value);
			destination.PushBack(value, allocator);
		}
		return;
	}

	if (source.IsString())
	{
		destination.SetString(source.GetString(), static_cast<rapidjson::SizeType>(source.GetStringLength()), allocator);
		return;
	}
	if (source.IsBool())
	{
		destination.SetBool(source.GetBool());
		return;
	}
	if (source.IsInt())
	{
		destination.SetInt(source.GetInt());
		return;
	}
	if (source.IsUint())
	{
		destination.SetUint(source.GetUint());
		return;
	}
	if (source.IsNumber())
	{
		destination.SetDouble(source.GetDouble());
		return;
	}
	destination.SetNull();
}

bool CAvisoEditorDialog::TryGetDefaultInsertPosition(double& longitude, double& latitude) const
{
	if (Owner != nullptr)
	{
		CPosition airportPosition;
		if (Owner->TryGetActiveAirportPosition(airportPosition))
		{
			longitude = airportPosition.m_Longitude;
			latitude = airportPosition.m_Latitude;
			return true;
		}
		if (Owner->AvisoGeoJsonHasBounds)
		{
			longitude = (Owner->AvisoGeoJsonMinLongitude + Owner->AvisoGeoJsonMaxLongitude) * 0.5;
			latitude = (Owner->AvisoGeoJsonMinLatitude + Owner->AvisoGeoJsonMaxLatitude) * 0.5;
			return true;
		}
	}

	if (Document.IsObject() && Document.HasMember("features") && Document["features"].IsArray())
	{
		const rapidjson::Value& features = Document["features"];
		for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
		{
			const rapidjson::Value& feature = features[i];
			if (!feature.IsObject() ||
				!feature.HasMember("geometry") ||
				!feature["geometry"].IsObject() ||
				!feature["geometry"].HasMember("coordinates"))
			{
				continue;
			}
			const rapidjson::Value& coordinates = feature["geometry"]["coordinates"];
			if (coordinates.IsArray() && coordinates.Size() >= 2 &&
				coordinates[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
				coordinates[static_cast<rapidjson::SizeType>(1)].IsNumber())
			{
				longitude = coordinates[static_cast<rapidjson::SizeType>(0)].GetDouble();
				latitude = coordinates[static_cast<rapidjson::SizeType>(1)].GetDouble();
				return true;
			}
		}
	}
	return false;
}

void CAvisoEditorDialog::RefreshAfterDocumentMutation(int selectedFeatureIndex)
{
	MarkDirty(true);
	PopulateObjectList(selectedFeatureIndex);
	SaveDocument(true);
}

void CAvisoEditorDialog::MarkDirty(bool dirty)
{
	Dirty = dirty;
	if (Dirty)
		SetStatusText("AVISO has unsaved changes.");
}

std::string CAvisoEditorDialog::GetEditText(const CEdit& edit) const
{
	CString value;
	const_cast<CEdit&>(edit).GetWindowText(value);
	return std::string(value.GetString());
}

void CAvisoEditorDialog::SetEditText(CEdit& edit, const std::string& text)
{
	if (::IsWindow(edit.GetSafeHwnd()))
		edit.SetWindowText(text.c_str());
}

void CAvisoEditorDialog::SetEditEnabled(CEdit& edit, bool enabled)
{
	if (::IsWindow(edit.GetSafeHwnd()))
		edit.EnableWindow(enabled ? TRUE : FALSE);
}

bool CAvisoEditorDialog::TryParseDouble(const std::string& text, double& value) const
{
	const std::string trimmed = TrimAsciiWhitespaceCopy(text);
	if (trimmed.empty())
		return false;

	char* endPtr = nullptr;
	value = std::strtod(trimmed.c_str(), &endPtr);
	return endPtr != trimmed.c_str() && endPtr != nullptr && *endPtr == '\0' && std::isfinite(value);
}

std::string CAvisoEditorDialog::FormatDouble(double value) const
{
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(8) << value;
	std::string text = stream.str();
	while (text.size() > 1 && text.back() == '0')
		text.pop_back();
	if (!text.empty() && text.back() == '.')
		text.pop_back();
	return text;
}

std::string CAvisoEditorDialog::ReadStringProperty(const rapidjson::Value* properties, const char* key, const std::string& fallback) const
{
	if (properties == nullptr || !properties->IsObject() || key == nullptr || !properties->HasMember(key) || !(*properties)[key].IsString())
		return fallback;
	return (*properties)[key].GetString();
}

double CAvisoEditorDialog::ReadNumberProperty(const rapidjson::Value* properties, const char* key, double fallback) const
{
	if (properties == nullptr || !properties->IsObject() || key == nullptr || !properties->HasMember(key) || !(*properties)[key].IsNumber())
		return fallback;
	return (*properties)[key].GetDouble();
}

bool CAvisoEditorDialog::ReadBoolProperty(const rapidjson::Value* properties, const char* key, bool fallback) const
{
	if (properties == nullptr || !properties->IsObject() || key == nullptr || !properties->HasMember(key))
		return fallback;
	if ((*properties)[key].IsBool())
		return (*properties)[key].GetBool();
	if ((*properties)[key].IsString())
	{
		const std::string value = ToUpperAscii(TrimAsciiWhitespaceCopy((*properties)[key].GetString()));
		if (value == "FALSE" || value == "0" || value == "NO" || value == "HIDDEN" || value == "NONE")
			return false;
		if (value == "TRUE" || value == "1" || value == "YES" || value == "VISIBLE")
			return true;
	}
	return fallback;
}

void CAvisoEditorDialog::SetStringMember(rapidjson::Value& object, const char* key, const std::string& value)
{
	if (!object.IsObject() || key == nullptr)
		return;

	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	if (object.HasMember(key))
		object.RemoveMember(key);
	rapidjson::Value keyValue;
	keyValue.SetString(key, allocator);
	rapidjson::Value stringValue;
	stringValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
	object.AddMember(keyValue, stringValue, allocator);
}

void CAvisoEditorDialog::SetNumberMember(rapidjson::Value& object, const char* key, double value)
{
	if (!object.IsObject() || key == nullptr)
		return;
	if (object.HasMember(key))
		object.RemoveMember(key);
	rapidjson::Value keyValue;
	keyValue.SetString(key, Document.GetAllocator());
	rapidjson::Value numberValue(value);
	object.AddMember(keyValue, numberValue, Document.GetAllocator());
}

void CAvisoEditorDialog::SetBoolMember(rapidjson::Value& object, const char* key, bool value)
{
	if (!object.IsObject() || key == nullptr)
		return;
	if (object.HasMember(key))
		object.RemoveMember(key);
	rapidjson::Value keyValue;
	keyValue.SetString(key, Document.GetAllocator());
	rapidjson::Value boolValue(value);
	object.AddMember(keyValue, boolValue, Document.GetAllocator());
}

void CAvisoEditorDialog::RemoveMemberIfExists(rapidjson::Value& object, const char* key)
{
	if (object.IsObject() && key != nullptr && object.HasMember(key))
		object.RemoveMember(key);
}

std::string CAvisoEditorDialog::GeometryCoordinatesToText(const rapidjson::Value& geometry) const
{
	if (!geometry.IsObject() || !geometry.HasMember("type") || !geometry["type"].IsString() ||
		!geometry.HasMember("coordinates") || !geometry["coordinates"].IsArray())
	{
		return "";
	}

	const std::string geometryType = geometry["type"].GetString();
	if (geometryType == "Point")
		return "";

	std::ostringstream stream;
	auto appendPoint = [&](const rapidjson::Value& point, bool firstPoint)
	{
		if (!point.IsArray() || point.Size() < 2 ||
			!point[static_cast<rapidjson::SizeType>(0)].IsNumber() ||
			!point[static_cast<rapidjson::SizeType>(1)].IsNumber())
			return;
		if (!firstPoint)
			stream << "; ";
		stream << FormatDouble(point[static_cast<rapidjson::SizeType>(0)].GetDouble()) << "," << FormatDouble(point[static_cast<rapidjson::SizeType>(1)].GetDouble());
	};

	const rapidjson::Value& coordinates = geometry["coordinates"];
	if (geometryType == "LineString")
	{
		for (rapidjson::SizeType i = 0; i < coordinates.Size(); ++i)
			appendPoint(coordinates[i], i == 0);
		return stream.str();
	}

	for (rapidjson::SizeType pathIndex = 0; pathIndex < coordinates.Size(); ++pathIndex)
	{
		const rapidjson::Value& path = coordinates[pathIndex];
		if (!path.IsArray())
			continue;
		if (pathIndex > 0)
			stream << "\r\n";

		if (geometryType == "Polygon")
		{
			for (rapidjson::SizeType i = 0; i < path.Size(); ++i)
				appendPoint(path[i], i == 0);
		}
		else if (geometryType == "MultiLineString")
		{
			for (rapidjson::SizeType i = 0; i < path.Size(); ++i)
				appendPoint(path[i], i == 0);
		}
	}
	return stream.str();
}

bool CAvisoEditorDialog::ApplyCoordinatesTextToGeometry(rapidjson::Value& geometry, const std::string& text, bool showErrors)
{
	if (!geometry.IsObject() || !geometry.HasMember("type") || !geometry["type"].IsString())
		return false;

	const std::string geometryType = geometry["type"].GetString();
	if (geometryType != "LineString" && geometryType != "MultiLineString" && geometryType != "Polygon")
		return true;

	std::vector<std::vector<std::pair<double, double>>> paths;
	const std::vector<std::string> lines = SplitString(text, '\n');
	for (const std::string& rawLine : lines)
	{
		const std::string line = TrimAsciiWhitespaceCopy(rawLine);
		if (line.empty())
			continue;

		std::vector<std::pair<double, double>> points;
		const std::vector<std::string> pointTexts = SplitString(line, ';');
		for (const std::string& pointText : pointTexts)
		{
			const std::vector<std::string> parts = SplitString(pointText, ',');
			if (parts.size() < 2)
			{
				if (showErrors)
					SetStatusText("Invalid coordinates. Use lon,lat; lon,lat.");
				return false;
			}

			double longitude = 0.0;
			double latitude = 0.0;
			if (!TryParseDouble(parts[0], longitude) || !TryParseDouble(parts[1], latitude))
			{
				if (showErrors)
					SetStatusText("Invalid numeric coordinate.");
				return false;
			}
			points.push_back({ longitude, latitude });
		}

		const size_t minPoints = geometryType == "Polygon" ? 3 : 2;
		if (points.size() < minPoints)
		{
			if (showErrors)
				SetStatusText("Not enough points for selected geometry.");
			return false;
		}
		paths.push_back(std::move(points));
	}

	if (paths.empty())
	{
		if (showErrors)
			SetStatusText("Coordinate list cannot be empty.");
		return false;
	}

	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	rapidjson::Value coordinates(rapidjson::kArrayType);
	if (geometryType == "LineString")
	{
		for (const auto& point : paths.front())
		{
			rapidjson::Value pointValue(rapidjson::kArrayType);
			pointValue.PushBack(point.first, allocator);
			pointValue.PushBack(point.second, allocator);
			coordinates.PushBack(pointValue, allocator);
		}
	}
	else
	{
		for (const auto& path : paths)
		{
			rapidjson::Value pathValue(rapidjson::kArrayType);
			for (const auto& point : path)
			{
				rapidjson::Value pointValue(rapidjson::kArrayType);
				pointValue.PushBack(point.first, allocator);
				pointValue.PushBack(point.second, allocator);
				pathValue.PushBack(pointValue, allocator);
			}
			coordinates.PushBack(pathValue, allocator);
		}
	}

	if (geometry.HasMember("coordinates"))
		geometry.RemoveMember("coordinates");
	geometry.AddMember("coordinates", coordinates, allocator);
	return true;
}

BEGIN_MESSAGE_MAP(CAvisoEditorDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_MOVE()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_LBN_SELCHANGE(IDC_AE_OBJECT_LIST, &CAvisoEditorDialog::OnObjectSelectionChanged)
	ON_BN_CLICKED(IDC_AE_RELOAD_BUTTON, &CAvisoEditorDialog::OnReloadClicked)
	ON_BN_CLICKED(IDC_AE_SAVE_BUTTON, &CAvisoEditorDialog::OnSaveClicked)
	ON_BN_CLICKED(IDC_AE_ADD_LABEL_BUTTON, &CAvisoEditorDialog::OnAddLabelClicked)
	ON_BN_CLICKED(IDC_AE_ADD_LINE_BUTTON, &CAvisoEditorDialog::OnAddLineClicked)
	ON_BN_CLICKED(IDC_AE_DUPLICATE_BUTTON, &CAvisoEditorDialog::OnDuplicateClicked)
	ON_BN_CLICKED(IDC_AE_DELETE_BUTTON, &CAvisoEditorDialog::OnDeleteClicked)
	ON_BN_CLICKED(IDC_AE_APPLY_BUTTON, &CAvisoEditorDialog::OnApplyClicked)
	ON_BN_CLICKED(IDC_AE_CLOSE_BUTTON, &CAvisoEditorDialog::OnCloseClicked)
	ON_BN_CLICKED(IDC_AE_VISIBLE_CHECK, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_NAME_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_LAYER_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_OBJECT_TYPE_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_FILL_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_FILL_OPACITY_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_STROKE_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_STROKE_OPACITY_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_STROKE_WIDTH_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_TEXT_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_TEXT_FONT_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_TEXT_COLOR_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_TEXT_SIZE_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_TEXT_ANCHOR_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_HALO_COLOR_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_HALO_WIDTH_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_LONGITUDE_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_LATITUDE_EDIT, &CAvisoEditorDialog::OnFieldChanged)
	ON_EN_CHANGE(IDC_AE_COORDINATES_EDIT, &CAvisoEditorDialog::OnFieldChanged)
END_MESSAGE_MAP()
