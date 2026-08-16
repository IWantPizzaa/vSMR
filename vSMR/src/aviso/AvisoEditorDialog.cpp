#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoEditorDialog.hpp"
#include "radar/RadarScreen.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "afxdialogex.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

extern std::vector<CSMRRadar*> RadarScreensOpened;

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

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	bool StartsWithNoCase(const std::string& text, const std::string& prefix)
	{
		if (prefix.size() > text.size())
			return false;
		for (size_t i = 0; i < prefix.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
				return false;
		}
		return true;
	}

	bool ContainsNoCase(const std::string& text, const std::string& needle)
	{
		if (needle.empty())
			return true;
		return ToLowerAscii(text).find(ToLowerAscii(needle)) != std::string::npos;
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

	std::string NormalizePathForCompare(const std::string& path)
	{
		try
		{
			return std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().string();
		}
		catch (...)
		{
			return path;
		}
	}

	bool PathsEqualNoCase(const std::string& left, const std::string& right)
	{
		return !left.empty() && !right.empty() && EqualsNoCase(NormalizePathForCompare(left), NormalizePathForCompare(right));
	}

	std::string NormalizeAirportCandidate(std::string value)
	{
		value = ToUpperAscii(TrimAsciiWhitespaceCopy(value));
		if (value.size() != 4)
			return "";
		for (char c : value)
		{
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
				return "";
		}
		if (value == "JSON" || value == "AVIS" || value == "GEOJ")
			return "";
		return value;
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

	constexpr int kPropertyTabGeneral = 0;
	constexpr int kPropertyTabStyle = 1;
	constexpr int kPropertyTabText = 2;
	constexpr int kPropertyTabGeometry = 3;
	constexpr int kPropertyTabRaw = 4;
	const COLORREF kEditorBorderColor = RGB(198, 204, 214);
	const COLORREF kEditorThemeBackgroundColor = RGB(240, 240, 240);
	const COLORREF kEditorSidebarBackgroundColor = RGB(250, 250, 251);
	const COLORREF kEditorTextColor = RGB(17, 24, 39);
	const COLORREF kEditorMutedTextColor = RGB(107, 114, 128);
	const int kOffscreenPos = -5000;

	const char* kFilterAll = "All";
	const char* kFilterVisible = "Visible";
	const char* kFilterHidden = "Hidden";
	const char* kScopeSelectedObject = "Selected object";
	const char* kScopeSelectedObjects = "All selected objects";
	const char* kScopeFilteredObjects = "All filtered objects";
	const char* kScopeSameLayer = "Same layer";
	const char* kScopeSameStyle = "Same style";
	const UINT_PTR kSearchDebounceTimerId = 1701;
	const UINT_PTR kSelectionRefreshTimerId = 1702;
	const UINT kSearchDebounceMs = 150;
	const UINT kSelectionRefreshMs = 40;
	const UINT kFilterMenuCommandBase = 9600;
	const UINT kLoadAvisoFromComputerCommand = 10600;
	const UINT kLoadAvisoFromGithubCommand = 10601;
}

CAvisoEditorDialog::CAvisoEditorDialog(CSMRRadar* owner, CWnd* pParent /*=NULL*/)
	: CDialogEx(CAvisoEditorDialog::IDD, pParent),
	Owner(owner),
	Document(Model.MutableDocument())
{
	Model.ResetToEmpty();
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
	ModifyStyle(0, WS_CLIPCHILDREN | WS_CLIPSIBLINGS);

	CWnd* statusWnd = GetDlgItem(IDC_AVISO_EDITOR_STATUS);
	if (statusWnd != nullptr && ::IsWindow(statusWnd->GetSafeHwnd()))
		statusWnd->ShowWindow(SW_HIDE);

	CreateEditorControls();
	Initialized = true;
	SyncFromRadar();
	ForceChildRepaint();
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

	if (!PromptForUnsavedChanges("loading another AVISO file"))
		return;
	LoadDocumentFromCurrentAviso(true);
	LayoutControls();
}

void CAvisoEditorDialog::CreateEditorControls()
{
	if (ControlsCreated)
		return;

	const DWORD staticStyle = WS_CHILD | WS_VISIBLE;
	const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
	const DWORD multilineEditStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
	const DWORD comboStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL;
	const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;

	SidebarPanel.Create("", staticStyle | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_AE_SIDEBAR_PANEL);
	SidebarTitle.Create("AVISO", staticStyle | SS_CENTER, CRect(0, 0, 0, 0), this, IDC_AE_SIDEBAR_TITLE);
	SidebarDivider.Create("", staticStyle | SS_ETCHEDVERT, CRect(0, 0, 0, 0), this, IDC_AE_SIDEBAR_DIVIDER);
	PageTitleLabel.Create("AVISO Editor", staticStyle, CRect(0, 0, 0, 0), this, IDC_AE_PAGE_TITLE);
	PageSubtitleLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_AE_PAGE_SUBTITLE);
	BrowserPanel.Create("", staticStyle | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_AE_BROWSER_PANEL);
	InspectorPanel.Create("", staticStyle | WS_BORDER, CRect(0, 0, 0, 0), this, IDC_AE_INSPECTOR_PANEL);
	BrowserHeader.Create("Objects", staticStyle, CRect(0, 0, 0, 0), this, IDC_AE_BROWSER_HEADER);
	PathLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_AE_PATH_LABEL);
	StatusLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this, IDC_AE_STATUS_LABEL);
	SearchLabel.Create("Search", staticStyle, CRect(0, 0, 0, 0), this);
	SearchEdit.Create(editStyle, CRect(0, 0, 0, 0), this, IDC_AE_SEARCH_EDIT);
#ifdef EM_SETCUEBANNER
	::SendMessageW(SearchEdit.GetSafeHwnd(), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search objects, layers, stands..."));
#endif
	LayerFilterLabel.Create("Layer", staticStyle, CRect(0, 0, 0, 0), this);
	LayerFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_LAYER_FILTER_COMBO);
	ObjectTypeFilterLabel.Create("Type", staticStyle, CRect(0, 0, 0, 0), this);
	ObjectTypeFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_OBJECT_TYPE_FILTER_COMBO);
	GeometryFilterLabel.Create("Geometry", staticStyle, CRect(0, 0, 0, 0), this);
	GeometryFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_GEOMETRY_FILTER_COMBO);
	VisibilityFilterLabel.Create("Visibility", staticStyle, CRect(0, 0, 0, 0), this);
	VisibilityFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_VISIBILITY_FILTER_COMBO);
	CategoryFilterLabel.Create("Category", staticStyle, CRect(0, 0, 0, 0), this);
	CategoryFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_CATEGORY_FILTER_COMBO);
	StyleFilterLabel.Create("Style", staticStyle, CRect(0, 0, 0, 0), this);
	StyleFilterCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_STYLE_FILTER_COMBO);
	ObjectCountLabel.Create("", staticStyle | SS_LEFTNOWORDWRAP, CRect(0, 0, 0, 0), this);
	ObjectList.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA, CRect(0, 0, 0, 0), this, IDC_AE_OBJECT_LIST);
	ObjectList.SetExtendedStyle(ObjectList.GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	ObjectList.SetBkColor(kEditorThemeBackgroundColor);
	ObjectList.SetTextBkColor(kEditorThemeBackgroundColor);
	ObjectList.SetTextColor(kEditorTextColor);
	ObjectList.InsertColumn(0, "Visible", LVCFMT_LEFT, 58);
	ObjectList.InsertColumn(1, "Name/Text", LVCFMT_LEFT, 210);
	ObjectList.InsertColumn(2, "Layer", LVCFMT_LEFT, 120);
	ObjectList.InsertColumn(3, "Object Type", LVCFMT_LEFT, 110);
	ObjectList.InsertColumn(4, "Geometry", LVCFMT_LEFT, 92);
	FilterButton.Create("Filters", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_FILTER_BUTTON);
	ClearFiltersButton.Create("Clear", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_CLEAR_FILTERS_BUTTON);
	CurrentPageButton.Create("Object editor", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_CURRENT_PAGE_BUTTON);
	LoadAvisoButton.Create("Load GeoJSON", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_LOAD_AVISO_BUTTON);
	MapsJsonButton.Create("Maps JSON", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_MAPS_JSON_BUTTON);
	SaveButton.Create("Save", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_SAVE_BUTTON);
	DeleteButton.Create("Delete", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_DELETE_BUTTON);
	ApplyButton.Create("Apply", buttonStyle, CRect(0, 0, 0, 0), this, IDC_AE_APPLY_BUTTON);
	VisibleCheck.Create("Visible", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(0, 0, 0, 0), this, IDC_AE_VISIBLE_CHECK);
	DetailsHeader.Create("Object properties", staticStyle, CRect(0, 0, 0, 0), this, IDC_AE_DETAILS_HEADER);
	PropertyTabs.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP, CRect(0, 0, 0, 0), this, IDC_AE_PROPERTY_TABS);
	PropertyTabs.InsertItem(kPropertyTabGeneral, "General");
	PropertyTabs.InsertItem(kPropertyTabStyle, "Style");
	PropertyTabs.InsertItem(kPropertyTabText, "Text");
	PropertyTabs.InsertItem(kPropertyTabGeometry, "Geometry");
	PropertyTabs.InsertItem(kPropertyTabRaw, "Raw");
	ApplyScopeLabel.Create("Apply to", staticStyle, CRect(0, 0, 0, 0), this);
	ApplyScopeCombo.Create(comboStyle, CRect(0, 0, 0, 0), this, IDC_AE_APPLY_SCOPE_COMBO);
	ApplyScopeCombo.AddString(kScopeSelectedObject);
	ApplyScopeCombo.AddString(kScopeSelectedObjects);
	ApplyScopeCombo.AddString(kScopeFilteredObjects);
	ApplyScopeCombo.AddString(kScopeSameLayer);
	ApplyScopeCombo.AddString(kScopeSameStyle);
	ApplyScopeCombo.SetCurSel(0);
	ApplyScopeLabel.ShowWindow(SW_HIDE);
	ApplyScopeCombo.ShowWindow(SW_HIDE);

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
	TextEdit.Create(multilineEditStyle, CRect(0, 0, 0, 0), this, IDC_AE_TEXT_EDIT);
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
	CoordinatesEdit.Create(multilineEditStyle | ES_READONLY, CRect(0, 0, 0, 0), this, IDC_AE_COORDINATES_EDIT);
	RawLabel.Create("Raw GeoJSON", staticStyle, CRect(0, 0, 0, 0), this);
	RawEdit.Create(multilineEditStyle | ES_READONLY, CRect(0, 0, 0, 0), this, IDC_AE_RAW_EDIT);

	LayerFilterLabel.ShowWindow(SW_HIDE);
	LayerFilterCombo.ShowWindow(SW_HIDE);
	ObjectTypeFilterLabel.ShowWindow(SW_HIDE);
	ObjectTypeFilterCombo.ShowWindow(SW_HIDE);
	GeometryFilterLabel.ShowWindow(SW_HIDE);
	GeometryFilterCombo.ShowWindow(SW_HIDE);
	VisibilityFilterLabel.ShowWindow(SW_HIDE);
	VisibilityFilterCombo.ShowWindow(SW_HIDE);
	CategoryFilterLabel.ShowWindow(SW_HIDE);
	CategoryFilterCombo.ShowWindow(SW_HIDE);
	StyleFilterLabel.ShowWindow(SW_HIDE);
	StyleFilterCombo.ShowWindow(SW_HIDE);

	PopulateFilterCombos();
	ApplyProfileEditorVisualStyle();
	ControlsCreated = true;
	LayoutControls();
	ApplyThemedEditBorders();
}

void CAvisoEditorDialog::ApplyProfileEditorVisualStyle()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;

	if (HeaderBarBrush.GetSafeHandle() == nullptr)
		HeaderBarBrush.CreateSolidBrush(kEditorThemeBackgroundColor);
	if (SidebarBrush.GetSafeHandle() == nullptr)
		SidebarBrush.CreateSolidBrush(kEditorSidebarBackgroundColor);

	auto softenPanelBorder = [](CStatic& panel)
	{
		if (!::IsWindow(panel.GetSafeHwnd()))
			return;
		panel.ModifyStyle(SS_ETCHEDFRAME, WS_BORDER);
		panel.ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
		panel.SetWindowPos(nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	};
	softenPanelBorder(SidebarPanel);
	softenPanelBorder(BrowserPanel);
	softenPanelBorder(InspectorPanel);
	SidebarPanel.ShowWindow(SW_HIDE);
	BrowserPanel.ShowWindow(SW_HIDE);
	InspectorPanel.ShowWindow(SW_HIDE);
	SidebarDivider.ShowWindow(SW_HIDE);

	ObjectList.ModifyStyleEx(WS_EX_CLIENTEDGE, 0);
	ObjectList.SetBkColor(kEditorThemeBackgroundColor);
	ObjectList.SetTextBkColor(kEditorThemeBackgroundColor);
	ObjectList.SetTextColor(kEditorTextColor);

	if (GetFont() != nullptr)
	{
		LOGFONT lf = {};
		GetFont()->GetLogFont(&lf);
		LOGFONT titleLf = lf;
		titleLf.lfHeight = (std::max)(18L, lf.lfHeight + 8);
		titleLf.lfWeight = FW_BOLD;
		TitleFont.CreateFontIndirect(&titleLf);

		LOGFONT sectionLf = lf;
		sectionLf.lfWeight = FW_BOLD;
		SectionHeaderFont.CreateFontIndirect(&sectionLf);

		LOGFONT uniformLf = lf;
		uniformLf.lfHeight = lf.lfHeight - 2;
		UniformUiFont.CreateFontIndirect(&uniformLf);

		LOGFONT monoLf = lf;
		strcpy_s(monoLf.lfFaceName, LF_FACESIZE, "Consolas");
		MonoFont.CreateFontIndirect(&monoLf);

		CFont* uniformFont = UniformUiFont.GetSafeHandle() != nullptr ? &UniformUiFont : GetFont();
		for (CWnd* child = GetWindow(GW_CHILD); child != nullptr; child = child->GetNextWindow())
		{
			if (::IsWindow(child->GetSafeHwnd()) && uniformFont != nullptr)
				child->SetFont(uniformFont, TRUE);
		}

		if (TitleFont.GetSafeHandle() != nullptr)
			PageTitleLabel.SetFont(&TitleFont, TRUE);
		if (SectionHeaderFont.GetSafeHandle() != nullptr)
		{
			SidebarTitle.SetFont(&SectionHeaderFont, TRUE);
			BrowserHeader.SetFont(&SectionHeaderFont, TRUE);
			DetailsHeader.SetFont(&SectionHeaderFont, TRUE);
		}
		if (MonoFont.GetSafeHandle() != nullptr)
		{
			ObjectList.SetFont(&MonoFont, TRUE);
			CoordinatesEdit.SetFont(&MonoFont, TRUE);
			RawEdit.SetFont(&MonoFont, TRUE);
		}
	}

	SidebarPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	BrowserPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	InspectorPanel.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void CAvisoEditorDialog::ApplyThemedEditBorders()
{
	auto normalizeEdit = [](CEdit& edit)
	{
		if (!::IsWindow(edit.GetSafeHwnd()))
			return;
		edit.ModifyStyle(0, WS_BORDER);
		edit.ModifyStyleEx(0, WS_EX_CLIENTEDGE);
		edit.SetWindowPos(nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		edit.SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
	};

	normalizeEdit(SearchEdit);
	normalizeEdit(NameEdit);
	normalizeEdit(LayerEdit);
	normalizeEdit(ObjectTypeEdit);
	normalizeEdit(GeometryEdit);
	normalizeEdit(FillEdit);
	normalizeEdit(FillOpacityEdit);
	normalizeEdit(StrokeEdit);
	normalizeEdit(StrokeOpacityEdit);
	normalizeEdit(StrokeWidthEdit);
	normalizeEdit(TextEdit);
	normalizeEdit(TextFontEdit);
	normalizeEdit(TextColorEdit);
	normalizeEdit(TextSizeEdit);
	normalizeEdit(TextAnchorEdit);
	normalizeEdit(HaloColorEdit);
	normalizeEdit(HaloWidthEdit);
	normalizeEdit(LongitudeEdit);
	normalizeEdit(LatitudeEdit);
	normalizeEdit(CoordinatesEdit);
	normalizeEdit(RawEdit);
}

void CAvisoEditorDialog::ForceChildRepaint()
{
	if (::IsWindow(GetSafeHwnd()))
		RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void CAvisoEditorDialog::LayoutControls()
{
	if (!ControlsCreated || !::IsWindow(GetSafeHwnd()))
		return;

	CRect client;
	GetClientRect(&client);
	const int sidebarWidth = 128;
	const int sidebarPad = 16;
	const int navButtonHeight = 38;
	const int navGap = 8;
	const int mainPad = 18;
	const int innerPad = 16;
	const int panelGap = 16;
	const int rowHeight = 24;
	const int fieldHeight = 24;
	const int comboDropH = 140;
	const int actionButtonWidth = 96;
	const int actionButtonHeight = 38;
	const int actionButtonGap = 10;

	SidebarPanelRect.SetRect(client.left, client.top, client.left + sidebarWidth, client.top + (std::max)(120, client.Height()));
	SidebarPanel.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	SidebarTitle.MoveWindow(client.left + sidebarPad, client.top + 18, sidebarWidth - (sidebarPad * 2), 20, TRUE);
	SidebarDivider.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);

	int navY = client.top + 46;
	const int navWidth = sidebarWidth - (sidebarPad * 2);
	CurrentPageButton.MoveWindow(client.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);
	navY += navButtonHeight + navGap;
	LoadAvisoButton.MoveWindow(client.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);
	navY += navButtonHeight + navGap;
	MapsJsonButton.MoveWindow(client.left + sidebarPad, navY, navWidth, navButtonHeight, TRUE);

	const int mainLeft = client.left + sidebarWidth + mainPad;
	const int mainTop = client.top + mainPad;
	const int mainWidth = (std::max)(220, client.Width() - sidebarWidth - (mainPad * 2));
	const int headerHeight = 50;
	const int actionsRight = mainLeft + mainWidth;
	const int bottomActionY = (std::max)(mainTop + headerHeight + 80, static_cast<int>(client.bottom) - mainPad - actionButtonHeight);

	SaveButton.MoveWindow(actionsRight - actionButtonWidth, bottomActionY, actionButtonWidth, actionButtonHeight, TRUE);
	ApplyButton.MoveWindow(actionsRight - (actionButtonWidth * 2) - actionButtonGap, bottomActionY, actionButtonWidth, actionButtonHeight, TRUE);
	DeleteButton.MoveWindow(actionsRight - (actionButtonWidth * 3) - (actionButtonGap * 2), bottomActionY, actionButtonWidth, actionButtonHeight, TRUE);
	PageTitleLabel.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	PageSubtitleLabel.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	const int headerTextWidth = (std::max)(160, mainWidth);
	PathLabel.MoveWindow(mainLeft, mainTop + 5, headerTextWidth, 16, TRUE);
	StatusLabel.MoveWindow(mainLeft, mainTop + 25, headerTextWidth, 16, TRUE);

	const int pageTop = mainTop + headerHeight + 14;
	const int pageHeight = (std::max)(140, bottomActionY - pageTop - 12);
	const int availableWidth = (std::max)(420, mainWidth - panelGap);
	const int minInspectorWidth = 360;
	int browserW = std::clamp(availableWidth * 54 / 100, 400, availableWidth - minInspectorWidth);
	if (browserW < 260)
		browserW = availableWidth / 2;
	const int inspectorW = (std::max)(240, availableWidth - browserW);
	const int browserX = mainLeft;
	const int inspectorX = browserX + browserW + panelGap;

	BrowserPanelRect.SetRect(browserX, pageTop, browserX + browserW, pageTop + pageHeight);
	InspectorPanelRect.SetRect(inspectorX, pageTop, inspectorX + inspectorW, pageTop + pageHeight);
	BrowserPanel.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	InspectorPanel.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	BrowserHeader.MoveWindow(browserX + 10, pageTop + 10, 120, 24, TRUE);
	ObjectCountLabel.MoveWindow(browserX + browserW - innerPad - 210, pageTop + 12, 210, 18, TRUE);

	int leftY = pageTop + 42;
	const int contentLeft = browserX + innerPad;
	const int contentRight = browserX + browserW - innerPad;
	const int contentW = (std::max)(120, contentRight - contentLeft);
	auto hideFilterControl = [](CWnd& window)
	{
		if (!::IsWindow(window.GetSafeHwnd()))
			return;
		window.ShowWindow(SW_HIDE);
		window.MoveWindow(kOffscreenPos, kOffscreenPos, 10, 10, TRUE);
	};

	SearchLabel.MoveWindow(contentLeft, leftY + 4, 58, 16, TRUE);
	const int clearButtonW = 62;
	const int filterButtonW = 84;
	const int filterButtonGap = 8;
	const int searchX = contentLeft + 64;
	const int searchW = (std::max)(80, contentW - 64 - filterButtonW - clearButtonW - (filterButtonGap * 2));
	SearchEdit.MoveWindow(searchX, leftY, searchW, fieldHeight, TRUE);
	FilterButton.MoveWindow(searchX + searchW + filterButtonGap, leftY - 1, filterButtonW, fieldHeight + 2, TRUE);
	ClearFiltersButton.MoveWindow(searchX + searchW + filterButtonGap + filterButtonW + filterButtonGap, leftY - 1, clearButtonW, fieldHeight + 2, TRUE);
	leftY += rowHeight + 12;

	hideFilterControl(LayerFilterLabel);
	hideFilterControl(LayerFilterCombo);
	hideFilterControl(ObjectTypeFilterLabel);
	hideFilterControl(ObjectTypeFilterCombo);
	hideFilterControl(GeometryFilterLabel);
	hideFilterControl(GeometryFilterCombo);
	hideFilterControl(VisibilityFilterLabel);
	hideFilterControl(VisibilityFilterCombo);
	hideFilterControl(StyleFilterLabel);
	hideFilterControl(StyleFilterCombo);
	hideFilterControl(CategoryFilterLabel);
	hideFilterControl(CategoryFilterCombo);

	const int listBottom = pageTop + pageHeight - innerPad;
	ObjectList.MoveWindow(contentLeft, leftY, contentW, (std::max)(80, listBottom - leftY), TRUE);
	const int visibleColumnW = 54;
	const int geometryColumnW = std::clamp(contentW / 6, 68, 92);
	const int objectTypeColumnW = std::clamp(contentW / 5, 76, 112);
	const int layerColumnW = std::clamp(contentW / 4, 92, 118);
	const int nameColumnW = (std::max)(80, contentW - visibleColumnW - geometryColumnW - objectTypeColumnW - layerColumnW - 4);
	ObjectList.SetColumnWidth(0, visibleColumnW);
	ObjectList.SetColumnWidth(1, nameColumnW);
	ObjectList.SetColumnWidth(2, layerColumnW);
	ObjectList.SetColumnWidth(3, objectTypeColumnW);
	ObjectList.SetColumnWidth(4, geometryColumnW);

	const int inspectorLeft = inspectorX + innerPad;
	const int inspectorRight = inspectorX + inspectorW - innerPad;
	const int inspectorContentW = (std::max)(120, inspectorRight - inspectorLeft);
	int y = pageTop + 12;
	DetailsHeader.MoveWindow(inspectorLeft, y, (std::max)(140, inspectorContentW - 92), 24, TRUE);
	VisibleCheck.MoveWindow(inspectorRight - 84, y - 1, 84, 22, TRUE);
	y += 30;
	ApplyScopeLabel.ShowWindow(SW_SHOW);
	ApplyScopeCombo.ShowWindow(SW_SHOW);
	ApplyScopeLabel.MoveWindow(inspectorLeft, y + 4, 58, 16, TRUE);
	ApplyScopeCombo.MoveWindow(inspectorLeft + 64, y, (std::max)(90, inspectorContentW - 64), comboDropH, TRUE);
	y += rowHeight + 8;
	PropertyTabs.MoveWindow(inspectorLeft, y, inspectorContentW, 26, TRUE);
	y += 36;

	const int labelW = 92;
	const int fieldW = (std::max)(90, inspectorContentW - labelW);
	auto moveRow = [&](CStatic& label, CEdit& edit, int rowY)
	{
		label.MoveWindow(inspectorLeft, rowY + 4, labelW, 16, TRUE);
		edit.MoveWindow(inspectorLeft + labelW, rowY, fieldW, fieldHeight, TRUE);
	};
	auto moveFull = [&](CStatic& label, CEdit& edit, int rowY, int height)
	{
		label.MoveWindow(inspectorLeft, rowY + 4, labelW, 16, TRUE);
		edit.MoveWindow(inspectorLeft + labelW, rowY, fieldW, height, TRUE);
	};

	const int tabTopY = y;
	const int inspectorBottom = pageTop + pageHeight - innerPad;
	moveRow(NameLabel, NameEdit, y);
	y += rowHeight;
	moveRow(LayerLabel, LayerEdit, y);
	y += rowHeight;
	moveRow(ObjectTypeLabel, ObjectTypeEdit, y);
	y += rowHeight;
	moveRow(GeometryLabel, GeometryEdit, y);
	y = tabTopY;
	moveRow(FillLabel, FillEdit, y);
	y += rowHeight;
	moveRow(FillOpacityLabel, FillOpacityEdit, y);
	y += rowHeight;
	moveRow(StrokeLabel, StrokeEdit, y);
	y += rowHeight;
	moveRow(StrokeOpacityLabel, StrokeOpacityEdit, y);
	y += rowHeight;
	moveRow(StrokeWidthLabel, StrokeWidthEdit, y);
	y = tabTopY;
	moveFull(TextLabel, TextEdit, y, 90);
	y += 96;
	moveRow(TextFontLabel, TextFontEdit, y);
	y += rowHeight;
	moveRow(TextColorLabel, TextColorEdit, y);
	y += rowHeight;
	moveRow(TextSizeLabel, TextSizeEdit, y);
	y += rowHeight;
	moveRow(TextAnchorLabel, TextAnchorEdit, y);
	y += rowHeight;
	moveRow(HaloColorLabel, HaloColorEdit, y);
	y += rowHeight;
	moveRow(HaloWidthLabel, HaloWidthEdit, y);
	y = tabTopY;
	moveRow(LongitudeLabel, LongitudeEdit, y);
	y += rowHeight;
	moveRow(LatitudeLabel, LatitudeEdit, y);
	y += rowHeight + 2;
	const int coordinatesH = (std::max)(50, inspectorBottom - y);
	moveFull(CoordinatesLabel, CoordinatesEdit, y, coordinatesH);
	RawLabel.MoveWindow(inspectorLeft, tabTopY + 4, labelW, 16, TRUE);
	RawEdit.MoveWindow(inspectorLeft + labelW, tabTopY, fieldW, (std::max)(80, inspectorBottom - tabTopY), TRUE);

	UpdatePropertyTabVisibility();
}

void CAvisoEditorDialog::PopulateFilterCombos()
{
	if (!::IsWindow(LayerFilterCombo.GetSafeHwnd()))
		return;

	const std::string selectedLayer = ReadComboText(LayerFilterCombo).empty() ? kFilterAll : ReadComboText(LayerFilterCombo);
	const std::string selectedObjectType = ReadComboText(ObjectTypeFilterCombo).empty() ? kFilterAll : ReadComboText(ObjectTypeFilterCombo);
	const std::string selectedGeometry = ReadComboText(GeometryFilterCombo).empty() ? kFilterAll : ReadComboText(GeometryFilterCombo);
	const std::string selectedVisibility = ReadComboText(VisibilityFilterCombo).empty() ? kFilterAll : ReadComboText(VisibilityFilterCombo);
	const std::string selectedCategory = ReadComboText(CategoryFilterCombo).empty() ? kFilterAll : ReadComboText(CategoryFilterCombo);
	const std::string selectedStyle = ReadComboText(StyleFilterCombo).empty() ? kFilterAll : ReadComboText(StyleFilterCombo);

	auto populateDynamicCombo = [&](CComboBox& combo, const std::set<std::string>& values, const std::string& selected)
	{
		combo.ResetContent();
		combo.AddString(kFilterAll);
		for (const std::string& value : values)
			combo.AddString(value.c_str());
		SelectComboEntryByText(combo, selected);
		if (combo.GetCurSel() == CB_ERR)
			combo.SetCurSel(0);
	};

	populateDynamicCombo(LayerFilterCombo, Model.GetLayers(), selectedLayer);
	populateDynamicCombo(ObjectTypeFilterCombo, Model.GetObjectTypes(), selectedObjectType);
	populateDynamicCombo(GeometryFilterCombo, Model.GetGeometryTypes(), selectedGeometry);
	populateDynamicCombo(CategoryFilterCombo, Model.GetCategories(), selectedCategory);
	populateDynamicCombo(StyleFilterCombo, Model.GetStyleIds(), selectedStyle);

	VisibilityFilterCombo.ResetContent();
	VisibilityFilterCombo.AddString(kFilterAll);
	VisibilityFilterCombo.AddString(kFilterVisible);
	VisibilityFilterCombo.AddString(kFilterHidden);
	SelectComboEntryByText(VisibilityFilterCombo, selectedVisibility);
	if (VisibilityFilterCombo.GetCurSel() == CB_ERR)
		VisibilityFilterCombo.SetCurSel(0);
}

void CAvisoEditorDialog::UpdatePropertyTabVisibility()
{
	if (!ControlsCreated)
		return;

	const int selectedTab = PropertyTabs.GetCurSel() == -1 ? kPropertyTabGeneral : PropertyTabs.GetCurSel();
	auto show = [](CWnd& window, bool visible)
	{
		if (::IsWindow(window.GetSafeHwnd()))
			window.ShowWindow(visible ? SW_SHOW : SW_HIDE);
	};

	const bool showGeneral = selectedTab == kPropertyTabGeneral;
	show(NameLabel, showGeneral);
	show(NameEdit, showGeneral);
	show(LayerLabel, showGeneral);
	show(LayerEdit, showGeneral);
	show(ObjectTypeLabel, showGeneral);
	show(ObjectTypeEdit, showGeneral);
	show(GeometryLabel, showGeneral);
	show(GeometryEdit, showGeneral);

	const bool showStyle = selectedTab == kPropertyTabStyle;
	show(FillLabel, showStyle);
	show(FillEdit, showStyle);
	show(FillOpacityLabel, showStyle);
	show(FillOpacityEdit, showStyle);
	show(StrokeLabel, showStyle);
	show(StrokeEdit, showStyle);
	show(StrokeOpacityLabel, showStyle);
	show(StrokeOpacityEdit, showStyle);
	show(StrokeWidthLabel, showStyle);
	show(StrokeWidthEdit, showStyle);

	const bool showText = selectedTab == kPropertyTabText;
	show(TextLabel, showText);
	show(TextEdit, showText);
	show(TextFontLabel, showText);
	show(TextFontEdit, showText);
	show(TextColorLabel, showText);
	show(TextColorEdit, showText);
	show(TextSizeLabel, showText);
	show(TextSizeEdit, showText);
	show(TextAnchorLabel, showText);
	show(TextAnchorEdit, showText);
	show(HaloColorLabel, showText);
	show(HaloColorEdit, showText);
	show(HaloWidthLabel, showText);
	show(HaloWidthEdit, showText);

	const bool showGeometry = selectedTab == kPropertyTabGeometry;
	show(LongitudeLabel, showGeometry);
	show(LongitudeEdit, showGeometry);
	show(LatitudeLabel, showGeometry);
	show(LatitudeEdit, showGeometry);
	show(CoordinatesLabel, showGeometry);
	show(CoordinatesEdit, showGeometry);

	const bool showRaw = selectedTab == kPropertyTabRaw;
	show(RawLabel, showRaw);
	show(RawEdit, showRaw);
}

void CAvisoEditorDialog::HideAndNotifyOwner()
{
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnAvisoEditorWindowClosed();
}

void CAvisoEditorDialog::OnCancel()
{
	if (!PromptForUnsavedChanges("closing the editor"))
		return;
	HideAndNotifyOwner();
}

void CAvisoEditorDialog::OnOK()
{
	OnApplyClicked();
}

void CAvisoEditorDialog::OnClose()
{
	if (!PromptForUnsavedChanges("closing the editor"))
		return;
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
	Invalidate(TRUE);
}

void CAvisoEditorDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI != nullptr)
	{
		lpMMI->ptMinTrackSize.x = 980;
		lpMMI->ptMinTrackSize.y = 640;
	}
}

void CAvisoEditorDialog::OnShowWindow(BOOL bShow, UINT nStatus)
{
	CDialogEx::OnShowWindow(bShow, nStatus);
	if (bShow && ControlsCreated)
	{
		LayoutControls();
		ForceChildRepaint();
	}
}

void CAvisoEditorDialog::OnDestroy()
{
	CDialogEx::OnDestroy();
}

void CAvisoEditorDialog::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	dc.FillSolidRect(&client, kEditorThemeBackgroundColor);

	const auto drawCardRect = [&](const CRect& sourceRect)
	{
		CRect rect(sourceRect);
		if (rect.Width() <= 2 || rect.Height() <= 2)
			return;

		CRect card(rect);
		card.DeflateRect(1, 1);
		CBrush fillBrush(kEditorThemeBackgroundColor);
		CPen borderPen(PS_SOLID, 1, kEditorBorderColor);
		CBrush* oldBrush = dc.SelectObject(&fillBrush);
		CPen* oldPen = dc.SelectObject(&borderPen);
		dc.RoundRect(&card, CPoint(12, 12));
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);
	};

	const auto drawSectionDivider = [&](CWnd& header, const CRect& panelRect)
	{
		if (!::IsWindow(header.GetSafeHwnd()) || !header.IsWindowVisible() || panelRect.IsRectEmpty())
			return;

		CRect headerRect;
		header.GetWindowRect(&headerRect);
		ScreenToClient(&headerRect);

		const int lineY = (std::min)(panelRect.bottom - 12, headerRect.bottom + 3);
		const int lineLeft = panelRect.left + 12;
		const int lineRight = panelRect.right - 12;
		if (lineRight <= lineLeft)
			return;

		CPen dividerPen(PS_SOLID, 1, RGB(210, 215, 223));
		CPen* oldPen = dc.SelectObject(&dividerPen);
		dc.MoveTo(lineLeft, lineY);
		dc.LineTo(lineRight, lineY);
		dc.SelectObject(oldPen);
	};

	if (!SidebarPanelRect.IsRectEmpty())
	{
		dc.FillSolidRect(&SidebarPanelRect, kEditorSidebarBackgroundColor);
		CPen dividerPen(PS_SOLID, 1, kEditorBorderColor);
		CPen* oldPen = dc.SelectObject(&dividerPen);
		dc.MoveTo(SidebarPanelRect.right - 1, SidebarPanelRect.top);
		dc.LineTo(SidebarPanelRect.right - 1, SidebarPanelRect.bottom);
		dc.SelectObject(oldPen);
	}

	drawCardRect(BrowserPanelRect);
	drawCardRect(InspectorPanelRect);
	drawSectionDivider(BrowserHeader, BrowserPanelRect);
	drawSectionDivider(DetailsHeader, InspectorPanelRect);
}

void CAvisoEditorDialog::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (lpDrawItemStruct == nullptr)
	{
		CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
		return;
	}

	const UINT controlId = lpDrawItemStruct->CtlID;
	const bool isModernButton =
		controlId == IDC_AE_CURRENT_PAGE_BUTTON ||
		controlId == IDC_AE_LOAD_AVISO_BUTTON ||
		controlId == IDC_AE_MAPS_JSON_BUTTON ||
		controlId == IDC_AE_SAVE_BUTTON ||
		controlId == IDC_AE_DELETE_BUTTON ||
		controlId == IDC_AE_APPLY_BUTTON ||
		controlId == IDC_AE_FILTER_BUTTON ||
		controlId == IDC_AE_CLEAR_FILTERS_BUTTON;
	if (!isModernButton)
	{
		CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
		return;
	}

	CDC dc;
	dc.Attach(lpDrawItemStruct->hDC);
	CRect outerRect(lpDrawItemStruct->rcItem);
	CRect localOuter(0, 0, outerRect.Width(), outerRect.Height());

	CDC memDc;
	memDc.CreateCompatibleDC(&dc);
	CBitmap frameBitmap;
	frameBitmap.CreateCompatibleBitmap(&dc, (std::max)(1, localOuter.Width()), (std::max)(1, localOuter.Height()));
	CBitmap* oldFrameBitmap = memDc.SelectObject(&frameBitmap);

	const bool isSidebarButton =
		controlId == IDC_AE_CURRENT_PAGE_BUTTON ||
		controlId == IDC_AE_LOAD_AVISO_BUTTON ||
		controlId == IDC_AE_MAPS_JSON_BUTTON;
	memDc.FillSolidRect(&localOuter, isSidebarButton ? kEditorSidebarBackgroundColor : kEditorThemeBackgroundColor);

	const bool isDisabled = (lpDrawItemStruct->itemState & ODS_DISABLED) != 0;
	const bool isPressed = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
	const bool isHot = (lpDrawItemStruct->itemState & ODS_HOTLIGHT) != 0;
	const bool isActivePageButton = controlId == IDC_AE_CURRENT_PAGE_BUTTON;
	const bool isPrimaryButton = controlId == IDC_AE_SAVE_BUTTON || controlId == IDC_AE_APPLY_BUTTON;
	const bool isDestructiveButton = controlId == IDC_AE_DELETE_BUTTON;

	COLORREF fillColor = RGB(247, 248, 250);
	COLORREF borderColor = RGB(171, 180, 190);
	COLORREF textColor = kEditorTextColor;
	if (isActivePageButton && !isDisabled)
	{
		fillColor = RGB(221, 236, 255);
		borderColor = RGB(63, 120, 208);
		textColor = RGB(37, 99, 235);
	}
	else if (isPrimaryButton && !isDisabled)
	{
		fillColor = isPressed ? RGB(47, 118, 234) : RGB(59, 130, 246);
		borderColor = RGB(37, 99, 235);
		textColor = RGB(255, 255, 255);
	}
	else if (isDestructiveButton && !isDisabled)
	{
		fillColor = RGB(255, 255, 255);
		borderColor = RGB(220, 38, 38);
		textColor = RGB(153, 27, 27);
	}
	else if (isSidebarButton)
	{
		fillColor = kEditorSidebarBackgroundColor;
		borderColor = kEditorSidebarBackgroundColor;
		textColor = RGB(31, 41, 55);
	}

	if (isDisabled)
	{
		fillColor = RGB(232, 232, 232);
		borderColor = RGB(194, 194, 194);
		textColor = RGB(138, 138, 138);
	}
	else if (!isPrimaryButton && isPressed)
	{
		fillColor = isDestructiveButton ? RGB(254, 226, 226) : RGB(221, 236, 255);
		borderColor = isDestructiveButton ? RGB(185, 28, 28) : RGB(63, 120, 208);
	}
	else if (!isPrimaryButton && isHot)
	{
		fillColor = isDestructiveButton ? RGB(254, 242, 242) : RGB(236, 245, 255);
		borderColor = isDestructiveButton ? RGB(239, 68, 68) : RGB(102, 153, 224);
	}

	CRect buttonRect(localOuter);
	buttonRect.DeflateRect(1, 1);
	CBrush fillBrush(fillColor);
	CPen borderPen(PS_SOLID, 1, borderColor);
	CBrush* oldBrush = memDc.SelectObject(&fillBrush);
	CPen* oldPen = memDc.SelectObject(&borderPen);
	memDc.RoundRect(&buttonRect, CPoint(10, 10));
	memDc.SelectObject(oldPen);
	memDc.SelectObject(oldBrush);

	CString buttonText;
	if (CWnd* button = GetDlgItem(controlId))
		button->GetWindowText(buttonText);
	CFont* oldFont = nullptr;
	if (CWnd* button = GetDlgItem(controlId))
	{
		CFont* font = button->GetFont();
		if (font != nullptr)
			oldFont = memDc.SelectObject(font);
	}
	memDc.SetBkMode(TRANSPARENT);
	memDc.SetTextColor(textColor);
	CRect textRect(buttonRect);
	memDc.DrawText(buttonText, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	if (oldFont != nullptr)
		memDc.SelectObject(oldFont);

	if ((lpDrawItemStruct->itemState & ODS_FOCUS) != 0)
	{
		CRect focusRect(buttonRect);
		focusRect.DeflateRect(4, 3);
		memDc.DrawFocusRect(&focusRect);
	}

	dc.BitBlt(outerRect.left, outerRect.top, localOuter.Width(), localOuter.Height(), &memDc, 0, 0, SRCCOPY);
	memDc.SelectObject(oldFrameBitmap);
	dc.Detach();
}

HBRUSH CAvisoEditorDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
	if (pWnd == nullptr)
		return hbr;

	const int controlId = pWnd->GetDlgCtrlID();
	if (nCtlColor == CTLCOLOR_BTN)
		return hbr;

	if ((controlId == IDC_AE_SIDEBAR_PANEL || controlId == IDC_AE_SIDEBAR_TITLE) && SidebarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorSidebarBackgroundColor);
		pDC->SetTextColor(controlId == IDC_AE_SIDEBAR_TITLE ? RGB(92, 101, 116) : kEditorTextColor);
		return static_cast<HBRUSH>(SidebarBrush.GetSafeHandle());
	}

	if ((nCtlColor == CTLCOLOR_STATIC || nCtlColor == CTLCOLOR_DLG) && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		if (controlId == IDC_AE_PAGE_SUBTITLE || controlId == IDC_AE_PATH_LABEL || controlId == IDC_AE_STATUS_LABEL)
			pDC->SetTextColor(kEditorMutedTextColor);
		else
			pDC->SetTextColor(kEditorTextColor);
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}

	if (nCtlColor == CTLCOLOR_EDIT)
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(kEditorTextColor);
		return ::GetSysColorBrush(COLOR_WINDOW);
	}

	if (nCtlColor == CTLCOLOR_LISTBOX && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		pDC->SetTextColor(kEditorTextColor);
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}

	const bool useThemeBackground = [&]()
	{
		switch (controlId)
		{
		case IDC_AE_PATH_LABEL:
		case IDC_AE_STATUS_LABEL:
		case IDC_AE_PAGE_TITLE:
		case IDC_AE_PAGE_SUBTITLE:
		case IDC_AE_BROWSER_PANEL:
		case IDC_AE_INSPECTOR_PANEL:
		case IDC_AE_BROWSER_HEADER:
		case IDC_AE_DETAILS_HEADER:
		case IDC_AE_SEARCH_EDIT:
		case IDC_AE_LAYER_FILTER_COMBO:
		case IDC_AE_OBJECT_TYPE_FILTER_COMBO:
		case IDC_AE_GEOMETRY_FILTER_COMBO:
		case IDC_AE_VISIBILITY_FILTER_COMBO:
		case IDC_AE_STYLE_FILTER_COMBO:
		case IDC_AE_CATEGORY_FILTER_COMBO:
		case IDC_AE_OBJECT_LIST:
		case IDC_AE_VISIBLE_CHECK:
		case IDC_AE_NAME_EDIT:
		case IDC_AE_LAYER_EDIT:
		case IDC_AE_OBJECT_TYPE_EDIT:
		case IDC_AE_GEOMETRY_EDIT:
		case IDC_AE_FILL_EDIT:
		case IDC_AE_FILL_OPACITY_EDIT:
		case IDC_AE_STROKE_EDIT:
		case IDC_AE_STROKE_OPACITY_EDIT:
		case IDC_AE_STROKE_WIDTH_EDIT:
		case IDC_AE_TEXT_EDIT:
		case IDC_AE_TEXT_FONT_EDIT:
		case IDC_AE_TEXT_COLOR_EDIT:
		case IDC_AE_TEXT_SIZE_EDIT:
		case IDC_AE_TEXT_ANCHOR_EDIT:
		case IDC_AE_HALO_COLOR_EDIT:
		case IDC_AE_HALO_WIDTH_EDIT:
		case IDC_AE_LONGITUDE_EDIT:
		case IDC_AE_LATITUDE_EDIT:
		case IDC_AE_COORDINATES_EDIT:
		case IDC_AE_RAW_EDIT:
			return true;
		default:
			return false;
		}
	}();
	if (useThemeBackground && HeaderBarBrush.GetSafeHandle() != nullptr)
	{
		pDC->SetBkColor(kEditorThemeBackgroundColor);
		if (controlId == IDC_AE_PAGE_SUBTITLE || controlId == IDC_AE_PATH_LABEL || controlId == IDC_AE_STATUS_LABEL)
			pDC->SetTextColor(kEditorMutedTextColor);
		else
			pDC->SetTextColor(kEditorTextColor);
		return static_cast<HBRUSH>(HeaderBarBrush.GetSafeHandle());
	}

	return hbr;
}

void CAvisoEditorDialog::OnObjectListItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult != nullptr)
		*pResult = 0;
	if (UpdatingControls || RestoringObjectSelection || pNMHDR == nullptr)
		return;

	const NMLISTVIEW* listViewChange = reinterpret_cast<const NMLISTVIEW*>(pNMHDR);
	if ((listViewChange->uChanged & LVIF_STATE) == 0)
		return;
	if ((listViewChange->uNewState & LVIS_SELECTED) == 0 && (listViewChange->uOldState & LVIS_SELECTED) == 0)
		return;

	SelectionRefreshPending = true;
	SetTimer(kSelectionRefreshTimerId, kSelectionRefreshMs, nullptr);
}

void CAvisoEditorDialog::OnObjectListGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult)
{
	if (pResult != nullptr)
		*pResult = 0;
	if (pNMHDR == nullptr)
		return;

	NMLVDISPINFO* displayInfo = reinterpret_cast<NMLVDISPINFO*>(pNMHDR);
	if ((displayInfo->item.mask & LVIF_TEXT) == 0 || displayInfo->item.pszText == nullptr || displayInfo->item.cchTextMax <= 0)
		return;

	const std::string text = GetObjectListCellText(displayInfo->item.iItem, displayInfo->item.iSubItem);
	::lstrcpyn(displayInfo->item.pszText, text.c_str(), displayInfo->item.cchTextMax);
}

void CAvisoEditorDialog::OnFilterChanged()
{
	if (UpdatingControls)
		return;
	UINT controlId = 0;
	const MSG* message = GetCurrentMessage();
	if (message != nullptr && message->hwnd != nullptr)
		controlId = static_cast<UINT>(::GetDlgCtrlID(message->hwnd));
	if (controlId == IDC_AE_SEARCH_EDIT)
	{
		SetTimer(kSearchDebounceTimerId, kSearchDebounceMs, nullptr);
		return;
	}
	KillTimer(kSearchDebounceTimerId);
	PopulateObjectList(GetSelectedFeatureIndex());
}

void CAvisoEditorDialog::OnPropertyTabChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	UNREFERENCED_PARAMETER(pNMHDR);
	if (pResult != nullptr)
		*pResult = 0;
	UpdatePropertyTabVisibility();
	if (PropertyTabs.GetCurSel() == kPropertyTabRaw)
		UpdateRawEditForSelection(GetFeatureByIndex(GetSelectedFeatureIndex()));
}

void CAvisoEditorDialog::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == kSearchDebounceTimerId)
	{
		KillTimer(kSearchDebounceTimerId);
		if (!UpdatingControls)
			PopulateObjectList(GetSelectedFeatureIndex());
		return;
	}
	if (nIDEvent == kSelectionRefreshTimerId)
	{
		KillTimer(kSelectionRefreshTimerId);
		if (!UpdatingControls && SelectionRefreshPending)
		{
			SelectionRefreshPending = false;
			if (ResolvePendingChangesBeforeSelectionRefresh())
				RefreshFieldsFromSelection();
		}
		return;
	}
	CDialogEx::OnTimer(nIDEvent);
}

void CAvisoEditorDialog::OnFieldChanged()
{
	if (UpdatingControls)
		return;

	UINT controlId = 0;
	const MSG* message = GetCurrentMessage();
	if (message != nullptr && message->hwnd != nullptr)
		controlId = static_cast<UINT>(::GetDlgCtrlID(message->hwnd));
	if (controlId == 0)
	{
		CWnd* focus = GetFocus();
		if (focus != nullptr && ::IsWindow(focus->GetSafeHwnd()))
			controlId = static_cast<UINT>(focus->GetDlgCtrlID());
	}

	const unsigned int dirtyFlag = DirtyFlagForControlId(controlId);
	if (dirtyFlag == 0)
		return;

	DirtyFieldMask |= dirtyFlag;
	PendingFieldChanges = DirtyFieldMask != 0;
	SetStatusText("Field changes pending. Apply changes updates the current selection; Save writes the file.");
}

void CAvisoEditorDialog::OnApplyClicked()
{
	if (ApplyPendingFieldsToCurrentSelection(true))
		SetStatusText("Applied changes in memory. Press Save to write the AVISO file.");
}

void CAvisoEditorDialog::OnSaveClicked()
{
	if (PendingFieldChanges && !ApplyPendingFieldsToCurrentSelection(true))
		return;
	SaveDocument(false);
}

void CAvisoEditorDialog::OnDeleteClicked()
{
	std::vector<int> selectedFeatureIndices = GetSelectedFeatureIndices();
	if (selectedFeatureIndices.empty())
		return;

	if (selectedFeatureIndices.size() > 1)
	{
		const std::string message = "Delete " + std::to_string(selectedFeatureIndices.size()) + " selected AVISO objects?";
		if (MessageBox(message.c_str(), "AVISO Editor", MB_ICONWARNING | MB_YESNO) != IDYES)
			return;
	}

	std::sort(selectedFeatureIndices.begin(), selectedFeatureIndices.end(), std::greater<int>());
	for (int featureIndex : selectedFeatureIndices)
		DeleteFeatureAt(featureIndex);
}

void CAvisoEditorDialog::OnCurrentPageClicked()
{
	SetStatusText("Object editor is open.");
}

void CAvisoEditorDialog::OnLoadAvisoClicked()
{
	if (!PromptForUnsavedChanges("loading another AVISO file"))
		return;

	HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr)
		return;
	::AppendMenu(menu, MF_STRING, kLoadAvisoFromComputerCommand, "From computer...");
	::AppendMenu(menu, MF_STRING, kLoadAvisoFromGithubCommand, "From GitHub URL in clipboard");

	CRect buttonRect;
	LoadAvisoButton.GetWindowRect(&buttonRect);
	const UINT command = ::TrackPopupMenu(
		menu,
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
		buttonRect.left,
		buttonRect.bottom + 2,
		0,
		GetSafeHwnd(),
		nullptr);
	::DestroyMenu(menu);

	if (command == kLoadAvisoFromGithubCommand)
	{
		const std::string clipboardText = ReadClipboardText();
		if (clipboardText.empty())
		{
			SetStatusText("Copy a GitHub GeoJSON URL to the clipboard first.");
			return;
		}
		ImportAvisoGeoJsonFromGithubUrl(clipboardText);
		return;
	}
	if (command != kLoadAvisoFromComputerCommand)
		return;

	CString initialDir;
	if (!LoadedPath.empty())
	{
		try
		{
			const std::filesystem::path parentPath = std::filesystem::path(LoadedPath).parent_path();
			if (!parentPath.empty())
				initialDir = parentPath.string().c_str();
		}
		catch (...)
		{
			initialDir.Empty();
		}
	}

	CFileDialog dialog(
		TRUE,
		"geojson",
		nullptr,
		OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
		"AVISO GeoJSON (*.geojson)|*.geojson|JSON files (*.json)|*.json|All files (*.*)|*.*||",
		this);
	if (!initialDir.IsEmpty())
		dialog.m_ofn.lpstrInitialDir = initialDir.GetString();

	if (dialog.DoModal() != IDOK)
		return;

	ImportAvisoGeoJsonFromFile(std::string(dialog.GetPathName().GetString()));
}

void CAvisoEditorDialog::OnMapsJsonClicked()
{
	if (Owner == nullptr || Owner->mapsPath.empty())
	{
		SetStatusText("vSMR_Maps.json path is unavailable.");
		return;
	}

	const HINSTANCE result = ::ShellExecuteA(GetSafeHwnd(), "open", Owner->mapsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(result) <= 32)
	{
		SetStatusText("Unable to open vSMR_Maps.json.");
		return;
	}
	SetStatusText("Opened vSMR_Maps.json in the associated editor.");
}

bool CAvisoEditorDialog::ImportAvisoGeoJsonFromFile(const std::string& sourcePath)
{
	if (sourcePath.empty())
	{
		SetStatusText("No GeoJSON file selected.");
		return false;
	}

	std::ifstream input(sourcePath, std::ios::binary);
	if (!input)
	{
		SetStatusText("Unable to open selected GeoJSON file.");
		return false;
	}

	std::ostringstream buffer;
	buffer << input.rdbuf();
	return ImportAvisoGeoJsonText(buffer.str(), sourcePath);
}

bool CAvisoEditorDialog::ImportAvisoGeoJsonFromGithubUrl(const std::string& url)
{
	const std::string normalizedUrl = NormalizeGithubGeoJsonUrl(url);
	if (!StartsWithNoCase(normalizedUrl, "https://raw.githubusercontent.com/"))
	{
		SetStatusText("Clipboard does not contain a valid HTTPS GitHub file URL.");
		return false;
	}

	SetStatusText("Downloading AVISO GeoJSON from GitHub...");
	HttpHelper http;
	const std::string geoJsonText = http.downloadStringFromURL(normalizedUrl);
	if (geoJsonText.empty())
	{
		SetStatusText("GitHub GeoJSON download failed.");
		return false;
	}
	return ImportAvisoGeoJsonText(geoJsonText, normalizedUrl);
}

bool CAvisoEditorDialog::ImportAvisoGeoJsonText(const std::string& geoJsonText, const std::string& sourceHint)
{
	if (Owner == nullptr)
	{
		SetStatusText("No active radar screen is available.");
		return false;
	}
	if (geoJsonText.empty())
	{
		SetStatusText("Selected GeoJSON file is empty.");
		return false;
	}

	rapidjson::Document parsed;
	parsed.Parse<0>(geoJsonText.c_str());
	if (parsed.HasParseError() || !parsed.IsObject())
	{
		SetStatusText("Selected file is not valid GeoJSON.");
		return false;
	}
	if (!parsed.HasMember("features") || !parsed["features"].IsArray())
	{
		SetStatusText("Selected GeoJSON must contain a features array.");
		return false;
	}

	AvisoDocumentModel validationModel;
	rapidjson::Document& validationDocument = validationModel.MutableDocument();
	validationDocument.Parse<0>(geoJsonText.c_str());
	if (validationDocument.HasParseError())
	{
		SetStatusText("Selected file is not valid GeoJSON.");
		return false;
	}
	validationModel.MarkIndexesDirty();
	const AvisoValidationResult validationResult =
		validationModel.ValidateAndRecalculate();
	if (!validationResult.ok)
	{
		SetStatusText(validationResult.errorText.empty()
			? "Selected AVISO GeoJSON failed validation."
			: validationResult.errorText);
		return false;
	}

	const std::string airport = DetectAirportForAvisoImport(parsed, sourceHint);
	if (airport.empty())
	{
		SetStatusText("Could not detect the airport ICAO. Use a filename like LFBO.geojson or metadata.icao.");
		return false;
	}

	const bool sourceIsRemote = StartsWithNoCase(sourceHint, "http://") || StartsWithNoCase(sourceHint, "https://");
	std::string selectedPath = sourceHint;
	bool storedRemoteFile = false;
	if (sourceIsRemote)
	{
		const std::filesystem::path dataDirectory = Owner->DataPath.empty()
			? (std::filesystem::path(Owner->DllPath) / "vSMR_Data")
			: std::filesystem::path(Owner->DataPath);
		std::string storageError;
		if (!VsmrResourceFiles::StoreGithubDownload(
			VsmrResourceFiles::Kind::Aviso,
			dataDirectory.string(),
			sourceHint,
			airport,
			geoJsonText,
			selectedPath,
			storageError))
		{
			SetStatusText(storageError.empty()
				? "Unable to store downloaded AVISO."
				: storageError);
			return false;
		}
		storedRemoteFile = true;
	}
	else if (!std::filesystem::exists(selectedPath))
	{
		SetStatusText("Selected AVISO file no longer exists.");
		return false;
	}

	const std::string previousActiveAirport = Owner->getActiveAirport();
	const std::string previousLoadedPath = LoadedPath;
	bool overrideChanged = false;
	bool hadPreviousOverride = false;
	std::string previousOverridePath;

	// Load into the editor before changing the live renderer. The downloaded
	// file has already been validated, but this also catches an I/O race before
	// an override is published to the other radar screens.
	std::error_code selectedPathError;
	const bool selectedPathAvailable =
		std::filesystem::is_regular_file(selectedPath, selectedPathError) &&
		!selectedPathError;
	const bool documentLoaded = selectedPathAvailable && LoadDocumentFromPath(
		selectedPath,
		false,
		"Loaded AVISO variant for " + airport + ". Select an object to edit.");
	if (documentLoaded)
	{
		Owner->setActiveAirport(airport);
		const auto previousOverride = Owner->AvisoGeoJsonOverridePaths.find(airport);
		hadPreviousOverride = previousOverride != Owner->AvisoGeoJsonOverridePaths.end();
		if (hadPreviousOverride)
			previousOverridePath = previousOverride->second;
		Owner->SetAvisoGeoJsonOverrideForAirport(airport, selectedPath);
		overrideChanged = true;
	}
	const bool selectedPathActivated = documentLoaded && PathsEqualNoCase(
		Owner->ResolveAvisoGeoJsonPathForAirport(airport),
		selectedPath);
	const bool rendererReloaded = selectedPathActivated && Owner->ForceReloadAvisoGeoJson();
	if (!rendererReloaded)
	{
		if (overrideChanged)
		{
			Owner->SetAvisoGeoJsonOverrideForAirport(
				airport,
				hadPreviousOverride ? previousOverridePath : std::string());
		}
		if (_stricmp(previousActiveAirport.c_str(), airport.c_str()) != 0)
			Owner->setActiveAirport(previousActiveAirport);
		Owner->ForceReloadAvisoGeoJson();

		if (!previousLoadedPath.empty())
		{
			LoadDocumentFromPath(
				previousLoadedPath,
				false,
				"Previous AVISO restored after the import failed.");
		}
		else
		{
			LoadDocumentFromCurrentAviso(false);
		}

		bool removedStoredFile = true;
		if (storedRemoteFile)
		{
			std::error_code removeError;
			removedStoredFile = std::filesystem::remove(selectedPath, removeError);
			if (!removedStoredFile && !removeError)
				removedStoredFile = !std::filesystem::exists(selectedPath, removeError) && !removeError;
		}
		SetStatusText(removedStoredFile
			? "Unable to activate the selected AVISO. The previous source was restored."
			: "Unable to activate the selected AVISO. The previous source was restored, but the downloaded variant could not be removed.");
		return false;
	}

	// SetAvisoGeoJsonOverrideForAirport propagates the source to all radar
	// screens. Publish it to their Control Centers only after both editor and
	// renderer activation have succeeded so rollback paths never become visible.
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar == nullptr || radar->VsmrControlCenterDialog == nullptr)
			continue;
		radar->VsmrControlCenterDialog->SyncFromRadar("resource-source");
	}
	SetStatusText("Loaded AVISO variant for " + airport + ".");
	return true;
}

std::string CAvisoEditorDialog::DetectAirportForAvisoImport(const rapidjson::Document& document, const std::string& sourceHint) const
{
	const char* keys[] = { "icao", "icao_code", "airport_icao", "airport_code", "airport", "active_airport" };
	auto findInObject = [&](const rapidjson::Value& object) -> std::string
	{
		if (!object.IsObject())
			return "";
		for (const char* key : keys)
		{
			if (object.HasMember(key) && object[key].IsString())
			{
				const std::string candidate = NormalizeAirportCandidate(object[key].GetString());
				if (!candidate.empty())
					return candidate;
			}
		}
		return "";
	};

	std::string airport = findInObject(document);
	if (!airport.empty())
		return airport;
	if (document.HasMember("metadata") && document["metadata"].IsObject())
	{
		airport = findInObject(document["metadata"]);
		if (!airport.empty())
			return airport;
	}
	if (document.HasMember("properties") && document["properties"].IsObject())
	{
		airport = findInObject(document["properties"]);
		if (!airport.empty())
			return airport;
	}

	std::string hint = sourceHint;
	std::replace(hint.begin(), hint.end(), '\\', '/');
	const size_t queryOffset = hint.find_first_of("?#");
	if (queryOffset != std::string::npos)
		hint = hint.substr(0, queryOffset);
	const size_t slashOffset = hint.find_last_of('/');
	if (slashOffset != std::string::npos)
		hint = hint.substr(slashOffset + 1);
	hint = ToUpperAscii(hint);

	const std::string avisoPrefix = "AVISO_";
	const size_t avisoOffset = hint.find(avisoPrefix);
	if (avisoOffset != std::string::npos && avisoOffset + avisoPrefix.size() + 4 <= hint.size())
	{
		airport = NormalizeAirportCandidate(hint.substr(avisoOffset + avisoPrefix.size(), 4));
		if (!airport.empty())
			return airport;
	}

	std::string token;
	for (char c : hint)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		{
			token.push_back(c);
			continue;
		}
		airport = NormalizeAirportCandidate(token);
		if (!airport.empty())
			return airport;
		token.clear();
	}
	return NormalizeAirportCandidate(token);
}

std::string CAvisoEditorDialog::NormalizeGithubGeoJsonUrl(const std::string& url) const
{
	std::string trimmed = TrimAsciiWhitespaceCopy(url);
	if (!trimmed.empty() && (trimmed.front() == '"' || trimmed.front() == '<'))
		trimmed.erase(trimmed.begin());
	if (!trimmed.empty() && (trimmed.back() == '"' || trimmed.back() == '>'))
		trimmed.pop_back();
	const size_t newlineOffset = trimmed.find_first_of("\r\n\t ");
	if (newlineOffset != std::string::npos)
		trimmed = trimmed.substr(0, newlineOffset);

	const std::string rawPrefix = "https://raw.githubusercontent.com/";
	if (StartsWithNoCase(trimmed, rawPrefix))
		return trimmed;

	const std::string githubPrefix = "https://github.com/";
	const std::string githubWwwPrefix = "https://www.github.com/";
	size_t prefixSize = 0;
	if (StartsWithNoCase(trimmed, githubPrefix))
		prefixSize = githubPrefix.size();
	else if (StartsWithNoCase(trimmed, githubWwwPrefix))
		prefixSize = githubWwwPrefix.size();
	else
		return "";

	const size_t queryOffset = trimmed.find_first_of("?#");
	if (queryOffset != std::string::npos)
		trimmed = trimmed.substr(0, queryOffset);
	const std::string lower = ToLowerAscii(trimmed);

	size_t fileMarkerOffset = lower.find("/blob/", prefixSize);
	if (fileMarkerOffset == std::string::npos)
		fileMarkerOffset = lower.find("/raw/", prefixSize);
	if (fileMarkerOffset == std::string::npos)
		return "";

	const std::string ownerRepo = trimmed.substr(prefixSize, fileMarkerOffset - prefixSize);
	const std::string branchAndPath = trimmed.substr(fileMarkerOffset + 6);
	if (ownerRepo.empty() || branchAndPath.empty())
		return "";
	return rawPrefix + ownerRepo + "/" + branchAndPath;
}

std::string CAvisoEditorDialog::ReadClipboardText() const
{
	if (!::OpenClipboard(GetSafeHwnd()))
		return "";

	std::string result;
	if (HANDLE unicodeHandle = ::GetClipboardData(CF_UNICODETEXT))
	{
		if (const wchar_t* wideText = static_cast<const wchar_t*>(::GlobalLock(unicodeHandle)))
		{
			const int requiredBytes = ::WideCharToMultiByte(CP_UTF8, 0, wideText, -1, nullptr, 0, nullptr, nullptr);
			if (requiredBytes > 1)
			{
				result.resize(static_cast<size_t>(requiredBytes - 1));
				::WideCharToMultiByte(CP_UTF8, 0, wideText, -1, result.data(), requiredBytes, nullptr, nullptr);
			}
			::GlobalUnlock(unicodeHandle);
		}
	}

	if (result.empty())
	{
		if (HANDLE textHandle = ::GetClipboardData(CF_TEXT))
		{
			if (const char* text = static_cast<const char*>(::GlobalLock(textHandle)))
			{
				result = text;
				::GlobalUnlock(textHandle);
			}
		}
	}

	::CloseClipboard();
	return TrimAsciiWhitespaceCopy(result);
}

void CAvisoEditorDialog::OnFilterButtonClicked()
{
	if (!::IsWindow(FilterButton.GetSafeHwnd()))
		return;

	struct FilterMenuAction
	{
		CComboBox* combo = nullptr;
		std::string groupName;
		std::string value;
	};

	std::vector<FilterMenuAction> actions;
	HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr)
		return;

	auto appendItem = [&](HMENU submenu, CComboBox& combo, const std::string& groupName, const std::string& value)
	{
		if (submenu == nullptr || actions.size() >= 800)
			return;

		const UINT commandId = kFilterMenuCommandBase + static_cast<UINT>(actions.size());
		const std::string currentValue = ReadComboText(combo).empty() ? kFilterAll : ReadComboText(combo);
		UINT flags = MF_STRING;
		if (EqualsNoCase(currentValue, value))
			flags |= MF_CHECKED;
		::AppendMenu(submenu, flags, commandId, value.c_str());
		actions.push_back(FilterMenuAction{ &combo, groupName, value });
	};

	auto appendDynamicSubmenu = [&](const char* title, CComboBox& combo, const std::set<std::string>& values)
	{
		HMENU submenu = ::CreatePopupMenu();
		if (submenu == nullptr)
			return;

		appendItem(submenu, combo, title, kFilterAll);
		if (!values.empty())
			::AppendMenu(submenu, MF_SEPARATOR, 0, nullptr);
		for (const std::string& value : values)
			appendItem(submenu, combo, title, value);

		::AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), title);
	};

	auto appendFixedSubmenu = [&](const char* title, CComboBox& combo, const std::vector<std::string>& values)
	{
		HMENU submenu = ::CreatePopupMenu();
		if (submenu == nullptr)
			return;

		for (size_t i = 0; i < values.size(); ++i)
		{
			if (i == 1)
				::AppendMenu(submenu, MF_SEPARATOR, 0, nullptr);
			appendItem(submenu, combo, title, values[i]);
		}

		::AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), title);
	};

	appendDynamicSubmenu("Layer", LayerFilterCombo, Model.GetLayers());
	appendDynamicSubmenu("Object type", ObjectTypeFilterCombo, Model.GetObjectTypes());
	appendDynamicSubmenu("Geometry", GeometryFilterCombo, Model.GetGeometryTypes());
	appendFixedSubmenu("Visibility", VisibilityFilterCombo, std::vector<std::string>{ kFilterAll, kFilterVisible, kFilterHidden });
	appendDynamicSubmenu("Category", CategoryFilterCombo, Model.GetCategories());
	appendDynamicSubmenu("Style", StyleFilterCombo, Model.GetStyleIds());

	CRect buttonRect;
	FilterButton.GetWindowRect(&buttonRect);
	const UINT command = ::TrackPopupMenu(
		menu,
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
		buttonRect.left,
		buttonRect.bottom + 2,
		0,
		GetSafeHwnd(),
		nullptr);
	::DestroyMenu(menu);

	if (command < kFilterMenuCommandBase)
		return;
	const size_t actionIndex = static_cast<size_t>(command - kFilterMenuCommandBase);
	if (actionIndex >= actions.size() || actions[actionIndex].combo == nullptr)
		return;

	KillTimer(kSearchDebounceTimerId);
	SelectComboEntryByText(*actions[actionIndex].combo, actions[actionIndex].value);
	PopulateObjectList(GetSelectedFeatureIndex());

	if (EqualsNoCase(actions[actionIndex].value, kFilterAll))
		SetStatusText(actions[actionIndex].groupName + " filter cleared.");
	else
		SetStatusText(actions[actionIndex].groupName + " filter: " + actions[actionIndex].value);
}

void CAvisoEditorDialog::OnClearFiltersClicked()
{
	if (!ControlsCreated)
		return;

	KillTimer(kSearchDebounceTimerId);
	UpdatingControls = true;
	SetEditText(SearchEdit, "");
	SelectComboEntryByText(LayerFilterCombo, kFilterAll);
	SelectComboEntryByText(ObjectTypeFilterCombo, kFilterAll);
	SelectComboEntryByText(GeometryFilterCombo, kFilterAll);
	SelectComboEntryByText(VisibilityFilterCombo, kFilterAll);
	SelectComboEntryByText(CategoryFilterCombo, kFilterAll);
	SelectComboEntryByText(StyleFilterCombo, kFilterAll);
	UpdatingControls = false;

	PopulateObjectList(GetSelectedFeatureIndex());
	SetStatusText("Filters cleared.");
}

bool CAvisoEditorDialog::PromptForUnsavedChanges(const char* actionText)
{
	if (!Dirty && !PendingFieldChanges)
		return true;

	const std::string action = actionText != nullptr ? actionText : "continuing";
	const std::string message = "AVISO has unsaved changes. Save before " + action + "?";
	const int response = MessageBox(message.c_str(), "AVISO Editor", MB_ICONQUESTION | MB_YESNOCANCEL);
	if (response == IDCANCEL)
		return false;
	if (response == IDNO)
	{
		PendingFieldChanges = false;
		DirtyFieldMask = 0;
		Dirty = false;
		return true;
	}
	if (PendingFieldChanges)
	{
		if (!ApplyPendingFieldsToCurrentSelection(true))
			return false;
	}
	return SaveDocument(false);
}

bool CAvisoEditorDialog::EnsureDocumentForEditing()
{
	Model.CreateEmptyFeatureCollection();
	return true;
}

bool CAvisoEditorDialog::LoadDocumentFromPath(const std::string& path, bool keepSelection, const std::string& loadedStatusText)
{
	const int previousSelection = keepSelection ? GetSelectedFeatureIndex() : -1;
	LoadedPath = path;
	PathLabel.SetWindowText(LoadedPath.empty() ? "AVISO path: unavailable" : ("AVISO path: " + LoadedPath).c_str());

	if (LoadedPath.empty())
	{
		Model.ResetToEmpty();
		PopulateFilterCombos();
		PopulateObjectList(-1);
		SetStatusText("No AVISO file is available for editing.");
		return false;
	}

	std::string errorText;
	if (!Model.LoadFromFile(LoadedPath, errorText))
	{
		SetStatusText(errorText.empty() ? "AVISO load failed." : errorText);
		Model.ResetToEmpty();
		PopulateFilterCombos();
		PopulateObjectList(-1);
		return false;
	}

	EnsureDocumentForEditing();
	Dirty = false;
	PendingFieldChanges = false;
	DirtyFieldMask = 0;
	PopulateFilterCombos();
	PopulateObjectList(previousSelection);
	if (std::filesystem::exists(LoadedPath))
		SetStatusText(loadedStatusText.empty() ? "AVISO loaded. Select an object to edit." : loadedStatusText);
	else
		SetStatusText("New AVISO file will be created on save.");
	return true;
}

bool CAvisoEditorDialog::LoadDocumentFromCurrentAviso(bool keepSelection)
{
	if (Owner == nullptr)
	{
		SetStatusText("No active radar screen is available.");
		return false;
	}

	const std::string path = Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
	if (path.empty())
	{
		Model.ResetToEmpty();
		LoadedPath.clear();
		PathLabel.SetWindowText("AVISO path: unavailable");
		PopulateFilterCombos();
		PopulateObjectList(-1);
		SetStatusText("No active airport is available for AVISO editing.");
		return false;
	}
	return LoadDocumentFromPath(path, keepSelection, "AVISO loaded. Select an object to edit.");
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

	std::string errorText;
	if (!Model.SaveAtomically(LoadedPath, errorText))
	{
		SetStatusText(errorText.empty() ? "AVISO save failed." : errorText);
		return false;
	}

	Dirty = false;
	PendingFieldChanges = false;
	DirtyFieldMask = 0;
	bool reloadRadar = false;
	if (reloadAfterSave && Owner != nullptr)
	{
		const std::string activeAvisoPath = Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
		reloadRadar = PathsEqualNoCase(LoadedPath, activeAvisoPath);
	}

	SetStatusText(reloadRadar ? "AVISO saved and radar reloaded." : "AVISO saved.");
	if (reloadRadar && Owner != nullptr)
		Owner->ForceReloadAvisoGeoJson();
	return true;
}

int CAvisoEditorDialog::GetSelectedFeatureIndex() const
{
	if (!::IsWindow(ObjectList.GetSafeHwnd()))
		return -1;

	POSITION position = const_cast<CListCtrl&>(ObjectList).GetFirstSelectedItemPosition();
	if (position == nullptr)
		return -1;

	const int itemIndex = const_cast<CListCtrl&>(ObjectList).GetNextSelectedItem(position);
	if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= FilteredFeatureIndices.size())
		return -1;
	return FilteredFeatureIndices[static_cast<size_t>(itemIndex)];
}

std::vector<int> CAvisoEditorDialog::GetSelectedFeatureIndices() const
{
	std::vector<int> indices;
	if (!::IsWindow(ObjectList.GetSafeHwnd()))
		return indices;

	POSITION position = const_cast<CListCtrl&>(ObjectList).GetFirstSelectedItemPosition();
	while (position != nullptr)
	{
		const int itemIndex = const_cast<CListCtrl&>(ObjectList).GetNextSelectedItem(position);
		if (itemIndex >= 0 && static_cast<size_t>(itemIndex) < FilteredFeatureIndices.size())
			indices.push_back(FilteredFeatureIndices[static_cast<size_t>(itemIndex)]);
	}
	return indices;
}

std::vector<int> CAvisoEditorDialog::GetFilteredFeatureIndices() const
{
	return FilteredFeatureIndices;
}

std::vector<int> CAvisoEditorDialog::GetBatchTargetFeatureIndices() const
{
	const std::string scope = ReadComboText(const_cast<CComboBox&>(ApplyScopeCombo));
	if (EqualsNoCase(scope, kScopeSelectedObjects))
		return GetSelectedFeatureIndices();
	if (EqualsNoCase(scope, kScopeFilteredObjects))
		return GetFilteredFeatureIndices();
	if (EqualsNoCase(scope, kScopeSameLayer))
	{
		std::vector<int> indices;
		const rapidjson::Value* selectedFeature = GetFeatureByIndex(GetSelectedFeatureIndex());
		if (selectedFeature == nullptr)
			return indices;
		const std::string selectedLayer = GetFeatureLayer(*selectedFeature);
		if (selectedLayer.empty())
			return indices;

		if (Document.IsObject() && Document.HasMember("features") && Document["features"].IsArray())
		{
			const rapidjson::Value& features = Document["features"];
			for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
			{
				if (EqualsNoCase(GetFeatureLayer(features[i]), selectedLayer))
					indices.push_back(static_cast<int>(i));
			}
		}
		return indices;
	}
	if (EqualsNoCase(scope, kScopeSameStyle))
	{
		std::vector<int> indices;
		const rapidjson::Value* selectedFeature = GetFeatureByIndex(GetSelectedFeatureIndex());
		if (selectedFeature == nullptr || !selectedFeature->IsObject() ||
			!selectedFeature->HasMember("properties") || !(*selectedFeature)["properties"].IsObject())
		{
			return indices;
		}
		const std::string selectedStyle = ReadStringProperty(&(*selectedFeature)["properties"], "style_id");
		if (selectedStyle.empty())
			return indices;

		if (Document.IsObject() && Document.HasMember("features") && Document["features"].IsArray())
		{
			const rapidjson::Value& features = Document["features"];
			for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
			{
				if (!features[i].IsObject() || !features[i].HasMember("properties") || !features[i]["properties"].IsObject())
					continue;
				if (EqualsNoCase(ReadStringProperty(&features[i]["properties"], "style_id"), selectedStyle))
					indices.push_back(static_cast<int>(i));
			}
		}
		return indices;
	}

	const int selected = GetSelectedFeatureIndex();
	return selected >= 0 ? std::vector<int>{ selected } : std::vector<int>{};
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
	return IsPointGeometry(feature) && IsEditableTextFeature(feature);
}

bool CAvisoEditorDialog::IsEditableTextFeature(const rapidjson::Value& feature) const
{
	if (!IsPointGeometry(feature))
		return false;

	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	if (properties == nullptr)
		return false;
	if (EqualsNoCase(ReadStringProperty(properties, "object_type"), "Label"))
		return true;
	const std::string role = ToUpperAscii(ReadStringProperty(properties, "geometry_role"));
	return role == "TEXT_LABEL";
}

bool CAvisoEditorDialog::IsPointGeometry(const rapidjson::Value& feature) const
{
	return GetFeatureGeometryType(feature) == "Point";
}

bool CAvisoEditorDialog::IsFeatureVisible(const rapidjson::Value* properties) const
{
	if (!ReadBoolProperty(properties, "visible", true))
		return false;

	const std::string visibility = ToUpperAscii(TrimAsciiWhitespaceCopy(ReadStringProperty(properties, "visibility")));
	if (visibility == "NONE" || visibility == "HIDDEN" || visibility == "FALSE" || visibility == "OFF" || visibility == "0")
		return false;
	return true;
}

std::string CAvisoEditorDialog::GetFeatureDisplayText(const rapidjson::Value& feature, int featureIndex) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	const std::string name = ReadStringProperty(properties, "name");
	const std::string text = ReadStringProperty(properties, "text-field",
		ReadStringProperty(properties, "text",
			ReadStringProperty(properties, "label",
				ReadStringProperty(properties, "title",
					ReadStringProperty(properties, "description")))));
	if (!name.empty() && !text.empty() && !EqualsNoCase(name, text))
		return name + " | " + text;
	if (!name.empty())
		return name;
	if (!text.empty())
		return text;

	const std::string layer = GetFeatureLayer(feature);
	if (!layer.empty())
		return layer;
	return "Object " + std::to_string(featureIndex + 1);
}

std::string CAvisoEditorDialog::GetFeatureLayer(const rapidjson::Value& feature) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	return ReadStringProperty(properties, "layer");
}

std::string CAvisoEditorDialog::GetFeatureObjectType(const rapidjson::Value& feature) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	return ReadStringProperty(properties, "object_type", ReadStringProperty(properties, "type"));
}

std::string CAvisoEditorDialog::GetFeatureSearchText(const rapidjson::Value& feature, int featureIndex) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	std::string text = GetFeatureDisplayText(feature, featureIndex);
	text += " ";
	text += GetFeatureLayer(feature);
	text += " ";
	text += GetFeatureObjectType(feature);
	text += " ";
	text += GetFeatureGeometryType(feature);

	const char* keys[] = { "name", "text-field", "text", "label", "title", "description", "layer", "object_type", "type", "geometry_role", "label_class", "category", "section" };
	for (const char* key : keys)
	{
		const std::string value = ReadStringProperty(properties, key);
		if (!value.empty())
		{
			text += " ";
			text += value;
		}
	}
	return text;
}

bool CAvisoEditorDialog::FeatureMatchesFilters(const rapidjson::Value& feature, int featureIndex) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	const std::string searchText = TrimAsciiWhitespaceCopy(GetEditText(SearchEdit));
	if (!searchText.empty() && !ContainsNoCase(GetFeatureSearchText(feature, featureIndex), searchText))
		return false;

	const std::string layerFilter = ReadComboText(const_cast<CComboBox&>(LayerFilterCombo));
	if (!layerFilter.empty() && !EqualsNoCase(layerFilter, kFilterAll) && !EqualsNoCase(GetFeatureLayer(feature), layerFilter))
		return false;

	const std::string objectTypeFilter = ReadComboText(const_cast<CComboBox&>(ObjectTypeFilterCombo));
	if (!objectTypeFilter.empty() && !EqualsNoCase(objectTypeFilter, kFilterAll) && !EqualsNoCase(GetFeatureObjectType(feature), objectTypeFilter))
		return false;

	const std::string geometryFilter = ReadComboText(const_cast<CComboBox&>(GeometryFilterCombo));
	if (!geometryFilter.empty() && !EqualsNoCase(geometryFilter, kFilterAll) && !EqualsNoCase(GetFeatureGeometryType(feature), geometryFilter))
		return false;

	const bool visible = IsFeatureVisible(properties);
	const std::string visibilityFilter = ReadComboText(const_cast<CComboBox&>(VisibilityFilterCombo));
	if (EqualsNoCase(visibilityFilter, kFilterVisible) && !visible)
		return false;
	if (EqualsNoCase(visibilityFilter, kFilterHidden) && visible)
		return false;

	const std::string categoryFilter = ReadComboText(const_cast<CComboBox&>(CategoryFilterCombo));
	if (!FeatureMatchesCategory(feature, categoryFilter))
		return false;

	return true;
}

bool CAvisoEditorDialog::FeatureMatchesCategory(const rapidjson::Value& feature, const std::string& category) const
{
	if (category.empty() || EqualsNoCase(category, kFilterAll))
		return true;

	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	return EqualsNoCase(ReadStringProperty(properties, "category"), category);
}

std::string CAvisoEditorDialog::BuildObjectListLabel(const rapidjson::Value& feature, int featureIndex) const
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	const bool visible = IsFeatureVisible(properties);
	const std::string geometryType = GetFeatureGeometryType(feature).empty() ? "Feature" : GetFeatureGeometryType(feature);
	std::string label = GetFeatureDisplayText(feature, featureIndex);

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
	FilteredFeatureIndices = Model.FilterFeatures(BuildCurrentFilter());
	ObjectList.SetItemCountEx(static_cast<int>(FilteredFeatureIndices.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
	ObjectList.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);

	int selectionToSet = -1;
	for (int i = 0; i < static_cast<int>(FilteredFeatureIndices.size()); ++i)
	{
		if (FilteredFeatureIndices[static_cast<size_t>(i)] == preferredFeatureIndex)
		{
			selectionToSet = i;
			break;
		}
	}
	if (selectionToSet < 0 && !FilteredFeatureIndices.empty())
		selectionToSet = 0;
	if (selectionToSet >= 0)
	{
		ObjectList.SetItemState(selectionToSet, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ObjectList.EnsureVisible(selectionToSet, FALSE);
	}

	ObjectList.Invalidate();
	UpdateObjectCountText();

	UpdatingControls = false;
	RefreshFieldsFromSelection();
}

void CAvisoEditorDialog::UpdateObjectCountText()
{
	if (!::IsWindow(ObjectCountLabel.GetSafeHwnd()))
		return;

	const int selectedCount = ::IsWindow(ObjectList.GetSafeHwnd()) ? ObjectList.GetSelectedCount() : 0;
	std::string text = std::to_string(FilteredFeatureIndices.size()) + " / " + std::to_string(Model.FeatureCount()) + " objects";
	if (selectedCount > 0)
		text += " | " + std::to_string(selectedCount) + " selected";
	ObjectCountLabel.SetWindowText(text.c_str());

	if (::IsWindow(FilterButton.GetSafeHwnd()) && ::IsWindow(ClearFiltersButton.GetSafeHwnd()))
	{
		int activeFilters = 0;
		auto countActiveCombo = [&](CComboBox& combo)
		{
			const std::string value = ReadComboText(combo);
			if (!value.empty() && !EqualsNoCase(value, kFilterAll))
				++activeFilters;
		};

		if (!TrimAsciiWhitespaceCopy(GetEditText(SearchEdit)).empty())
			++activeFilters;
		countActiveCombo(LayerFilterCombo);
		countActiveCombo(ObjectTypeFilterCombo);
		countActiveCombo(GeometryFilterCombo);
		countActiveCombo(VisibilityFilterCombo);
		countActiveCombo(CategoryFilterCombo);
		countActiveCombo(StyleFilterCombo);

		const std::string filterButtonText = activeFilters > 0 ? "Filters (" + std::to_string(activeFilters) + ")" : "Filters";
		FilterButton.SetWindowText(filterButtonText.c_str());
		ClearFiltersButton.EnableWindow(activeFilters > 0 ? TRUE : FALSE);
	}
}

void CAvisoEditorDialog::RestoreObjectSelection(int featureIndex)
{
	if (featureIndex < 0)
		RestoreObjectSelection(std::vector<int>());
	else
		RestoreObjectSelection(std::vector<int>{ featureIndex });
}

void CAvisoEditorDialog::RestoreObjectSelection(const std::vector<int>& featureIndices)
{
	if (!::IsWindow(ObjectList.GetSafeHwnd()))
		return;

	std::set<int> targetFeatureIndices(featureIndices.begin(), featureIndices.end());
	int firstSelectedRow = -1;
	RestoringObjectSelection = true;
	ObjectList.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	for (int row = 0; row < static_cast<int>(FilteredFeatureIndices.size()); ++row)
	{
		if (targetFeatureIndices.find(FilteredFeatureIndices[static_cast<size_t>(row)]) != targetFeatureIndices.end())
		{
			if (firstSelectedRow < 0)
				firstSelectedRow = row;
			const UINT state = firstSelectedRow == row ? (LVIS_SELECTED | LVIS_FOCUSED) : LVIS_SELECTED;
			ObjectList.SetItemState(row, state, LVIS_SELECTED | LVIS_FOCUSED);
		}
	}
	if (firstSelectedRow >= 0)
		ObjectList.EnsureVisible(firstSelectedRow, FALSE);
	UpdateObjectCountText();
	RestoringObjectSelection = false;
}

bool CAvisoEditorDialog::ResolvePendingChangesBeforeSelectionRefresh()
{
	if (!PendingFieldChanges || DirtyFieldMask == 0)
		return true;

	const std::vector<int> previousFeatureIndices = LastSelectedFeatureIndices;
	const int previousFeatureIndex = previousFeatureIndices.empty() ? LastSelectedFeatureIndex : previousFeatureIndices.front();
	if (previousFeatureIndices.empty() && previousFeatureIndex < 0)
	{
		PendingFieldChanges = false;
		DirtyFieldMask = 0;
		return true;
	}

	const int response = MessageBox(
		"Apply pending field changes before changing selection?",
		"AVISO Editor",
		MB_ICONQUESTION | MB_YESNOCANCEL);
	if (response == IDCANCEL)
	{
		RestoreObjectSelection(previousFeatureIndices.empty() ? std::vector<int>{ previousFeatureIndex } : previousFeatureIndices);
		return false;
	}
	if (response == IDNO)
	{
		PendingFieldChanges = false;
		DirtyFieldMask = 0;
		return true;
	}

	if (previousFeatureIndices.size() > 1)
	{
		if (!ApplyBatchFieldsToFeatures(previousFeatureIndices, true))
		{
			RestoreObjectSelection(previousFeatureIndices);
			return false;
		}
		return true;
	}

	if (!ApplyFieldsToFeature(previousFeatureIndex, true, true, false, true))
	{
		RestoreObjectSelection(previousFeatureIndices.empty() ? std::vector<int>{ previousFeatureIndex } : previousFeatureIndices);
		return false;
	}
	return true;
}

AvisoFeatureFilter CAvisoEditorDialog::BuildCurrentFilter() const
{
	AvisoFeatureFilter filter;
	filter.search = TrimAsciiWhitespaceCopy(GetEditText(SearchEdit));
	filter.layer = ReadComboText(const_cast<CComboBox&>(LayerFilterCombo));
	filter.category = ReadComboText(const_cast<CComboBox&>(CategoryFilterCombo));
	filter.objectType = ReadComboText(const_cast<CComboBox&>(ObjectTypeFilterCombo));
	filter.geometryType = ReadComboText(const_cast<CComboBox&>(GeometryFilterCombo));
	filter.visibility = ReadComboText(const_cast<CComboBox&>(VisibilityFilterCombo));
	filter.styleId = ReadComboText(const_cast<CComboBox&>(StyleFilterCombo));
	return filter;
}

std::string CAvisoEditorDialog::GetObjectListCellText(int rowIndex, int subItem) const
{
	if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= FilteredFeatureIndices.size())
		return "";

	AvisoDocumentModel& mutableModel = const_cast<AvisoDocumentModel&>(Model);
	const AvisoFeatureSummary* summary = mutableModel.GetSummaryByFeatureIndex(FilteredFeatureIndices[static_cast<size_t>(rowIndex)]);
	if (summary == nullptr)
		return "";

	switch (subItem)
	{
	case 0:
		return summary->visible ? "Yes" : "No";
	case 1:
		return summary->nameText;
	case 2:
		return summary->layer;
	case 3:
		return summary->objectType;
	case 4:
		return summary->geometryType;
	case 5:
		return summary->category;
	case 6:
		return summary->styleId;
	default:
		return "";
	}
}

void CAvisoEditorDialog::RefreshFieldsFromSelection()
{
	UpdatingControls = true;
	const std::vector<int> selectedFeatureIndices = GetSelectedFeatureIndices();
	const int selectedCount = static_cast<int>(selectedFeatureIndices.size());
	const bool hasSelection = selectedCount > 0;
	const bool singleSelection = selectedCount == 1;
	const int featureIndex = hasSelection ? selectedFeatureIndices.front() : -1;
	LastSelectedFeatureIndex = featureIndex;
	LastSelectedFeatureIndices = selectedFeatureIndices;
	UpdateObjectCountText();

	if (selectedCount == 0)
		DetailsHeader.SetWindowText("No object selected");
	else if (selectedCount == 1)
		DetailsHeader.SetWindowText("Object properties");
	else
		DetailsHeader.SetWindowText((std::to_string(selectedCount) + " objects selected").c_str());

	const rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
	const bool hasFeature = feature != nullptr && feature->IsObject();
	const rapidjson::Value* properties = nullptr;
	if (hasFeature && feature->HasMember("properties") && (*feature)["properties"].IsObject())
		properties = &(*feature)["properties"];

	VisibleCheck.SetCheck(IsFeatureVisible(properties) ? BST_CHECKED : BST_UNCHECKED);
	SetEditText(NameEdit, ReadStringProperty(properties, "name"));
	SetEditText(LayerEdit, ReadStringProperty(properties, "layer"));
	SetEditText(ObjectTypeEdit, ReadStringProperty(properties, "object_type", ReadStringProperty(properties, "type")));
	SetEditText(GeometryEdit, hasFeature ? GetFeatureGeometryType(*feature) : "");
	SetEditText(FillEdit, ReadStringProperty(properties, "fill"));
	SetEditText(FillOpacityEdit, properties != nullptr && properties->HasMember("fill-opacity") && (*properties)["fill-opacity"].IsNumber() ? FormatDouble((*properties)["fill-opacity"].GetDouble()) : "");
	SetEditText(StrokeEdit, ReadStringProperty(properties, "stroke"));
	SetEditText(StrokeOpacityEdit, properties != nullptr && properties->HasMember("stroke-opacity") && (*properties)["stroke-opacity"].IsNumber() ? FormatDouble((*properties)["stroke-opacity"].GetDouble()) : "");
	SetEditText(StrokeWidthEdit, properties != nullptr && properties->HasMember("stroke-width") && (*properties)["stroke-width"].IsNumber() ? FormatDouble((*properties)["stroke-width"].GetDouble()) : "");

	const bool pointGeometry = hasFeature && IsPointGeometry(*feature);
	const bool labelFeature = hasFeature && IsEditableTextFeature(*feature);
	const rapidjson::Value* textProperties = properties;
	bool hasTextFeature = labelFeature;
	if (!hasTextFeature)
	{
		for (int selectedFeatureIndex : selectedFeatureIndices)
		{
			const rapidjson::Value* selectedFeature = GetFeatureByIndex(selectedFeatureIndex);
			if (selectedFeature == nullptr || !selectedFeature->IsObject() || !IsEditableTextFeature(*selectedFeature))
				continue;
			hasTextFeature = true;
			if (selectedFeature->HasMember("properties") && (*selectedFeature)["properties"].IsObject())
				textProperties = &(*selectedFeature)["properties"];
			break;
		}
	}

	SetEditText(TextEdit, singleSelection && labelFeature ? ReadStringProperty(textProperties, "text-field",
		ReadStringProperty(textProperties, "text",
			ReadStringProperty(textProperties, "label",
				ReadStringProperty(textProperties, "title",
					ReadStringProperty(textProperties, "description"))))) : "");
	SetEditText(TextFontEdit, hasTextFeature ? ReadStringProperty(textProperties, "text-font", "Arial") : "");
	SetEditText(TextColorEdit, hasTextFeature ? ReadStringProperty(textProperties, "text-color") : "");
	SetEditText(TextSizeEdit, hasTextFeature && textProperties != nullptr && textProperties->HasMember("text-size") && (*textProperties)["text-size"].IsNumber() ? FormatDouble((*textProperties)["text-size"].GetDouble()) : "");
	SetEditText(TextAnchorEdit, singleSelection && labelFeature ? ReadStringProperty(textProperties, "text-anchor", "center") : "");
	SetEditText(HaloColorEdit, hasTextFeature ? ReadStringProperty(textProperties, "text-halo-color") : "");
	SetEditText(HaloWidthEdit, hasTextFeature && textProperties != nullptr && textProperties->HasMember("text-halo-width") && (*textProperties)["text-halo-width"].IsNumber() ? FormatDouble((*textProperties)["text-halo-width"].GetDouble()) : "");

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
		SetEditText(LongitudeEdit, FormatCoordinateDouble(longitude));
		SetEditText(LatitudeEdit, FormatCoordinateDouble(latitude));
	}
	else
	{
		SetEditText(LongitudeEdit, "");
		SetEditText(LatitudeEdit, "");
	}

	if (hasFeature && feature->HasMember("geometry") && (*feature)["geometry"].IsObject())
		SetEditText(CoordinatesEdit, GeometryCoordinatesSummary((*feature)["geometry"]));
	else
		SetEditText(CoordinatesEdit, "");

	UpdateRawEditForSelection(hasFeature ? feature : nullptr);

	VisibleCheck.EnableWindow(hasSelection);
	SetEditEnabled(NameEdit, singleSelection && hasFeature);
	SetEditEnabled(LayerEdit, singleSelection && hasFeature);
	SetEditEnabled(ObjectTypeEdit, singleSelection && hasFeature);
	SetEditEnabled(FillEdit, hasSelection);
	SetEditEnabled(FillOpacityEdit, hasSelection);
	SetEditEnabled(StrokeEdit, hasSelection);
	SetEditEnabled(StrokeOpacityEdit, hasSelection);
	SetEditEnabled(StrokeWidthEdit, hasSelection);
	SetEditEnabled(TextEdit, singleSelection && labelFeature);
	SetEditEnabled(TextFontEdit, hasTextFeature);
	SetEditEnabled(TextColorEdit, hasTextFeature);
	SetEditEnabled(TextSizeEdit, hasTextFeature);
	SetEditEnabled(TextAnchorEdit, singleSelection && labelFeature);
	SetEditEnabled(HaloColorEdit, hasTextFeature);
	SetEditEnabled(HaloWidthEdit, hasTextFeature);
	SetEditEnabled(LongitudeEdit, singleSelection && pointGeometry);
	SetEditEnabled(LatitudeEdit, singleSelection && pointGeometry);
	SetEditEnabled(CoordinatesEdit, singleSelection && hasFeature);
	SetEditEnabled(RawEdit, singleSelection && hasFeature);
	ApplyButton.EnableWindow(hasSelection);
	DeleteButton.EnableWindow(hasSelection);

	PendingFieldChanges = false;
	DirtyFieldMask = 0;
	UpdatingControls = false;
}

void CAvisoEditorDialog::UpdateRawEditForSelection(const rapidjson::Value* feature)
{
	if (!::IsWindow(RawEdit.GetSafeHwnd()))
		return;

	if (feature == nullptr || !feature->IsObject())
	{
		SetEditText(RawEdit, "");
		return;
	}

	if (PropertyTabs.GetCurSel() != kPropertyTabRaw)
	{
		SetEditText(RawEdit, "Raw GeoJSON is generated when the Raw tab is active.");
		return;
	}

	rapidjson::Value displayFeature;
	CloneJsonValue(*feature, displayFeature);
	if (displayFeature.IsObject() &&
		displayFeature.HasMember("geometry") &&
		displayFeature["geometry"].IsObject() &&
		displayFeature["geometry"].HasMember("coordinates"))
	{
		const std::string summary = GeometryCoordinatesSummary(displayFeature["geometry"]);
		displayFeature["geometry"].RemoveMember("coordinates");
		rapidjson::Value key;
		key.SetString("coordinates", Document.GetAllocator());
		rapidjson::Value value;
		value.SetString(summary.c_str(), static_cast<rapidjson::SizeType>(summary.size()), Document.GetAllocator());
		displayFeature["geometry"].AddMember(key, value, Document.GetAllocator());
	}

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	writer.SetIndent('\t', 1);
	displayFeature.Accept(writer);
	SetEditText(RawEdit, buffer.GetString());
}

bool CAvisoEditorDialog::ApplyFieldsToSelectedFeature(bool markDirty, bool showErrors)
{
	if (IsFieldDirty(kDirtyRaw))
	{
		if (showErrors)
			SetStatusText("Raw GeoJSON editing is read-only until the validated raw editor is available.");
		return false;
	}
	return ApplyFieldsToFeature(GetSelectedFeatureIndex(), markDirty, showErrors, false, true);
}

bool CAvisoEditorDialog::ApplyFieldsToFeature(int featureIndex, bool markDirty, bool showErrors, bool batchStyleOnly, bool detachSharedStyle)
{
	rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
	if (feature == nullptr || !feature->IsObject())
		return false;

	rapidjson::Value originalFeature;
	CloneJsonValue(*feature, originalFeature);
	auto rollbackAndFail = [&]() -> bool
	{
		rapidjson::Value restoredFeature;
		CloneJsonValue(originalFeature, restoredFeature);
		*feature = restoredFeature;
		Model.MarkIndexesDirty();
		return false;
	};

	rapidjson::Value& properties = EnsureFeatureProperties(*feature);
	if (IsFieldDirty(kDirtyVisible))
	{
		const bool visibleChecked = VisibleCheck.GetCheck() == BST_CHECKED;
		SetBoolMember(properties, "visible", visibleChecked);
		if (visibleChecked)
			RemoveMemberIfExists(properties, "visibility");
	}

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

	if (!batchStyleOnly)
	{
		if (IsFieldDirty(kDirtyName))
			setOptionalString("name", GetEditText(NameEdit));
		if (IsFieldDirty(kDirtyLayer))
			setOptionalString("layer", GetEditText(LayerEdit));
		if (IsFieldDirty(kDirtyObjectType))
		{
			setOptionalString("object_type", GetEditText(ObjectTypeEdit));
			RemoveMemberIfExists(properties, "type");
		}
	}

	if (IsFieldDirty(kDirtyFill) && !setOptionalHex("fill", GetEditText(FillEdit)))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyFillOpacity) && !setOptionalNumber("fill-opacity", GetEditText(FillOpacityEdit), 0.0, 1.0))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyStroke) && !setOptionalHex("stroke", GetEditText(StrokeEdit)))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyStrokeOpacity) && !setOptionalNumber("stroke-opacity", GetEditText(StrokeOpacityEdit), 0.0, 1.0))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyStrokeWidth) && !setOptionalNumber("stroke-width", GetEditText(StrokeWidthEdit), 0.25, 8.0))
		return rollbackAndFail();

	const bool selectedTextTab = PropertyTabs.GetCurSel() == kPropertyTabText;
	if (!batchStyleOnly && IsFieldDirty(kDirtyText))
	{
		const char* textKey = "text-field";
		const char* knownTextKeys[] = { "text-field", "text", "label", "title", "description", "name" };
		for (const char* key : knownTextKeys)
		{
			if (properties.HasMember(key) && properties[key].IsString())
			{
				textKey = key;
				break;
			}
		}
		const bool textKeyIsName = EqualsNoCase(textKey, "name");
		if (selectedTextTab || !textKeyIsName)
			setOptionalString(textKey, GetEditText(TextEdit));
		if (IsPointGeometry(*feature) && (selectedTextTab || !textKeyIsName) && !TrimAsciiWhitespaceCopy(GetEditText(TextEdit)).empty())
			SetStringMember(properties, "geometry_role", "TEXT_LABEL");
	}

	if (IsFieldDirty(kDirtyTextFont))
		setOptionalString("text-font", GetEditText(TextFontEdit).empty() ? "Arial" : GetEditText(TextFontEdit));
	if (IsFieldDirty(kDirtyTextColor) && !setOptionalHex("text-color", GetEditText(TextColorEdit)))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyTextSize) && !setOptionalNumber("text-size", GetEditText(TextSizeEdit), 6.0, 32.0))
		return rollbackAndFail();
	if (!batchStyleOnly && IsFieldDirty(kDirtyTextAnchor))
		setOptionalString("text-anchor", GetEditText(TextAnchorEdit).empty() ? "center" : GetEditText(TextAnchorEdit));
	if (IsFieldDirty(kDirtyHaloColor) && !setOptionalHex("text-halo-color", GetEditText(HaloColorEdit)))
		return rollbackAndFail();
	if (IsFieldDirty(kDirtyHaloWidth) && !setOptionalNumber("text-halo-width", GetEditText(HaloWidthEdit), 0.0, 6.0))
		return rollbackAndFail();

	if (batchStyleOnly)
	{
		if (HasDirtyStyleFields())
		{
			if (detachSharedStyle && !EnsureDetachedStyleForFeature(*feature, featureIndex))
				return rollbackAndFail();
			SyncDirtyStyleFieldsToStylePaint(properties);
		}
		return true;
	}

	if (IsAnyGeometryFieldDirty() &&
		(!feature->HasMember("geometry") || !(*feature)["geometry"].IsObject()))
	{
		if (showErrors)
			SetStatusText("Selected object has no editable geometry.");
		return rollbackAndFail();
	}

	if (IsAnyGeometryFieldDirty())
	{
		rapidjson::Value& geometry = (*feature)["geometry"];
		const bool pointGeometry = IsPointGeometry(*feature);
		if (pointGeometry)
		{
			double longitude = 0.0;
			double latitude = 0.0;
			if (!TryParseDouble(GetEditText(LongitudeEdit), longitude) || !TryParseDouble(GetEditText(LatitudeEdit), latitude))
			{
				if (showErrors)
					SetStatusText("Invalid label longitude or latitude.");
				return rollbackAndFail();
			}

			rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
			if (geometry.HasMember("coordinates"))
				geometry.RemoveMember("coordinates");
			rapidjson::Value coordinates(rapidjson::kArrayType);
			coordinates.PushBack(longitude, allocator);
			coordinates.PushBack(latitude, allocator);
			geometry.AddMember("coordinates", coordinates, allocator);
		}
		else if (IsFieldDirty(kDirtyCoordinates) && !ApplyCoordinatesTextToGeometry(geometry, GetEditText(CoordinatesEdit), showErrors))
		{
			return rollbackAndFail();
		}
	}

	if (HasDirtyStyleFields())
	{
		if (detachSharedStyle && !EnsureDetachedStyleForFeature(*feature, featureIndex))
			return rollbackAndFail();
		SyncDirtyStyleFieldsToStylePaint(properties);
	}

	const bool geometryWasDirty = IsAnyGeometryFieldDirty();
	if (markDirty && DirtyFieldMask != 0)
	{
		MarkDirty(true);
		PendingFieldChanges = false;
		DirtyFieldMask = 0;
		PopulateFilterCombos();
		PopulateObjectList(GetSelectedFeatureIndex());
	}
	if (geometryWasDirty)
		Model.MarkFeatureGeometryDirty(featureIndex);
	Model.MarkIndexesDirty();
	return true;
}

bool CAvisoEditorDialog::ApplyRawJsonToSelectedFeature(bool showErrors)
{
	const int featureIndex = GetSelectedFeatureIndex();
	rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
	if (feature == nullptr || !feature->IsObject())
		return false;

	const std::string rawText = GetEditText(RawEdit);
	rapidjson::Document parsed;
	parsed.Parse<0>(rawText.c_str());
	if (parsed.HasParseError() || !parsed.IsObject())
	{
		if (showErrors)
			SetStatusText("Raw GeoJSON must be a valid feature object.");
		return false;
	}

	rapidjson::Value replacement;
	CloneJsonValue(parsed, replacement);
	*feature = replacement;
	MarkDirty(true);
	Model.MarkFeatureGeometryDirty(featureIndex);
	PendingFieldChanges = false;
	DirtyFieldMask = 0;
	Model.MarkIndexesDirty();
	PopulateFilterCombos();
	PopulateObjectList(featureIndex);
	return true;
}

bool CAvisoEditorDialog::ApplyBatchFieldsToFeatures(const std::vector<int>& featureIndices, bool showErrors)
{
	if (featureIndices.empty())
	{
		if (showErrors)
			SetStatusText("No objects are selected.");
		return false;
	}
	if ((DirtyFieldMask & kDirtyBatchEditableMask) == 0)
	{
		if (showErrors)
			SetStatusText("No batch-editable field changes pending.");
		return false;
	}
	if ((DirtyFieldMask & ~kDirtyBatchEditableMask) != 0)
	{
		if (showErrors)
			SetStatusText("Only visibility and style fields can be applied to multiple selected objects.");
		return false;
	}

	const int selectedFeatureIndex = GetSelectedFeatureIndex();
	bool detachSharedStyle = true;
	rapidjson::StringBuffer rollbackBuffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> rollbackWriter(rollbackBuffer);
	Document.Accept(rollbackWriter);

	auto rollbackBatch = [&]() -> bool
	{
		rapidjson::Document restoredDocument;
		restoredDocument.Parse<0>(rollbackBuffer.GetString());
		if (!restoredDocument.HasParseError())
			CloneJsonValue(restoredDocument, Document);
		Model.MarkIndexesDirty();
		PopulateFilterCombos();
		PopulateObjectList(selectedFeatureIndex);
		return false;
	};

	if (detachSharedStyle && HasDirtyStyleFields() && Model.HasStyleCatalog() &&
		Document.IsObject() && Document.HasMember("styles") && Document["styles"].IsObject())
	{
		std::string sourceStyleId;
		for (int featureIndex : featureIndices)
		{
			const rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
			if (feature != nullptr && feature->IsObject() && feature->HasMember("properties") && (*feature)["properties"].IsObject())
			{
				sourceStyleId = ReadStringProperty(&(*feature)["properties"], "style_id");
				if (!sourceStyleId.empty())
					break;
			}
		}

		rapidjson::Value& styles = Document["styles"];
		std::string newStyleId;
		const std::string prefix = sourceStyleId.empty() ? "selection.custom" : sourceStyleId + ".selection";
		for (int suffix = 1; suffix < 100000; ++suffix)
		{
			newStyleId = prefix + "." + std::to_string(suffix);
			if (!styles.HasMember(newStyleId.c_str()))
				break;
		}
		if (newStyleId.empty() || styles.HasMember(newStyleId.c_str()))
			return rollbackBatch();

		rapidjson::Value clonedStyle;
		if (!sourceStyleId.empty() && styles.HasMember(sourceStyleId.c_str()) && styles[sourceStyleId.c_str()].IsObject())
			CloneJsonValue(styles[sourceStyleId.c_str()], clonedStyle);
		else
			clonedStyle.SetObject();
		if (!clonedStyle.IsObject())
			clonedStyle.SetObject();
		if (!clonedStyle.HasMember("paint") || !clonedStyle["paint"].IsObject())
		{
			if (clonedStyle.HasMember("paint"))
				clonedStyle.RemoveMember("paint");
			rapidjson::Value paint(rapidjson::kObjectType);
			clonedStyle.AddMember("paint", paint, Document.GetAllocator());
		}
		SetStringMember(clonedStyle, "name", "Selection style");

		rapidjson::Value styleKey;
		styleKey.SetString(newStyleId.c_str(), static_cast<rapidjson::SizeType>(newStyleId.size()), Document.GetAllocator());
		styles.AddMember(styleKey, clonedStyle, Document.GetAllocator());

		for (int featureIndex : featureIndices)
		{
			rapidjson::Value* feature = GetFeatureByIndex(featureIndex);
			if (feature == nullptr || !feature->IsObject())
				return rollbackBatch();
			rapidjson::Value& properties = EnsureFeatureProperties(*feature);
			SetStringMember(properties, "style_id", newStyleId);
		}
		detachSharedStyle = false;
	}

	int changedCount = 0;
	for (int featureIndex : featureIndices)
	{
		if (ApplyFieldsToFeature(featureIndex, false, showErrors, true, detachSharedStyle))
			++changedCount;
		else
			return rollbackBatch();
	}

	if (changedCount > 0)
	{
		MarkDirty(true);
		PendingFieldChanges = false;
		DirtyFieldMask = 0;
		Model.MarkIndexesDirty();
		PopulateFilterCombos();
		PopulateObjectList(selectedFeatureIndex);
		SetStatusText("Applied changes to " + std::to_string(changedCount) + " selected object(s).");
	}
	return changedCount > 0;
}

bool CAvisoEditorDialog::ApplyPendingFieldsToCurrentSelection(bool showErrors)
{
	if (!PendingFieldChanges || DirtyFieldMask == 0)
	{
		if (showErrors)
			SetStatusText("No field changes pending.");
		return false;
	}

	if (IsFieldDirty(kDirtyRaw))
	{
		if (showErrors)
			SetStatusText("Raw GeoJSON editing is read-only until the validated raw editor is available.");
		return false;
	}

	const std::vector<int> selectedFeatureIndices = GetSelectedFeatureIndices();
	if (selectedFeatureIndices.empty())
	{
		if (showErrors)
			SetStatusText("Select an object before applying changes.");
		return false;
	}

	if (selectedFeatureIndices.size() == 1)
		return ApplyFieldsToSelectedFeature(true, showErrors);

	if ((DirtyFieldMask & ~kDirtyBatchEditableMask) != 0)
	{
		if (showErrors)
			SetStatusText("Only visibility and style fields can be applied to multiple selected objects.");
		return false;
	}

	return ApplyBatchFieldsToFeatures(selectedFeatureIndices, showErrors);
}

void CAvisoEditorDialog::DeleteFeatureAt(int featureIndex)
{
	EnsureDocumentForEditing();
	rapidjson::Value& features = Document["features"];
	if (featureIndex < 0 || static_cast<rapidjson::SizeType>(featureIndex) >= features.Size())
		return;

	Model.NoteFeatureDeleted(featureIndex);
	for (rapidjson::SizeType i = static_cast<rapidjson::SizeType>(featureIndex); i + 1 < features.Size(); ++i)
		features[i] = features[i + 1];
	features.PopBack();
	Model.MarkIndexesDirty();
	RefreshAfterDocumentMutation((std::min)(featureIndex, static_cast<int>(features.Size()) - 1));
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
	if (source.IsInt64())
	{
		destination.SetInt64(source.GetInt64());
		return;
	}
	if (source.IsUint64())
	{
		destination.SetUint64(source.GetUint64());
		return;
	}
	if (source.IsNumber())
	{
		destination.SetDouble(source.GetDouble());
		return;
	}
	destination.SetNull();
}

void CAvisoEditorDialog::RefreshAfterDocumentMutation(int selectedFeatureIndex)
{
	MarkDirty(true);
	Model.MarkIndexesDirty();
	PopulateFilterCombos();
	PopulateObjectList(selectedFeatureIndex);
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

unsigned int CAvisoEditorDialog::DirtyFlagForControlId(UINT controlId) const
{
	switch (controlId)
	{
	case IDC_AE_VISIBLE_CHECK:
		return kDirtyVisible;
	case IDC_AE_NAME_EDIT:
		return kDirtyName;
	case IDC_AE_LAYER_EDIT:
		return kDirtyLayer;
	case IDC_AE_OBJECT_TYPE_EDIT:
		return kDirtyObjectType;
	case IDC_AE_FILL_EDIT:
		return kDirtyFill;
	case IDC_AE_FILL_OPACITY_EDIT:
		return kDirtyFillOpacity;
	case IDC_AE_STROKE_EDIT:
		return kDirtyStroke;
	case IDC_AE_STROKE_OPACITY_EDIT:
		return kDirtyStrokeOpacity;
	case IDC_AE_STROKE_WIDTH_EDIT:
		return kDirtyStrokeWidth;
	case IDC_AE_TEXT_EDIT:
		return kDirtyText;
	case IDC_AE_TEXT_FONT_EDIT:
		return kDirtyTextFont;
	case IDC_AE_TEXT_COLOR_EDIT:
		return kDirtyTextColor;
	case IDC_AE_TEXT_SIZE_EDIT:
		return kDirtyTextSize;
	case IDC_AE_TEXT_ANCHOR_EDIT:
		return kDirtyTextAnchor;
	case IDC_AE_HALO_COLOR_EDIT:
		return kDirtyHaloColor;
	case IDC_AE_HALO_WIDTH_EDIT:
		return kDirtyHaloWidth;
	case IDC_AE_LONGITUDE_EDIT:
		return kDirtyLongitude;
	case IDC_AE_LATITUDE_EDIT:
		return kDirtyLatitude;
	case IDC_AE_COORDINATES_EDIT:
		return kDirtyCoordinates;
	case IDC_AE_RAW_EDIT:
		return kDirtyRaw;
	default:
		return 0;
	}
}

bool CAvisoEditorDialog::IsFieldDirty(unsigned int flag) const
{
	return (DirtyFieldMask & flag) != 0;
}

bool CAvisoEditorDialog::IsAnyGeometryFieldDirty() const
{
	return (DirtyFieldMask & (kDirtyLongitude | kDirtyLatitude | kDirtyCoordinates)) != 0;
}

bool CAvisoEditorDialog::HasDirtyStyleFields() const
{
	const unsigned int styleMask =
		kDirtyFill | kDirtyFillOpacity | kDirtyStroke | kDirtyStrokeOpacity | kDirtyStrokeWidth |
		kDirtyTextFont | kDirtyTextColor | kDirtyTextSize | kDirtyTextAnchor | kDirtyHaloColor | kDirtyHaloWidth;
	return (DirtyFieldMask & styleMask) != 0;
}

bool CAvisoEditorDialog::EnsureDetachedStyleForFeature(rapidjson::Value& feature, int featureIndex)
{
	if (!Model.HasStyleCatalog() ||
		!Document.IsObject() ||
		!Document.HasMember("styles") ||
		!Document["styles"].IsObject() ||
		!feature.IsObject())
	{
		return true;
	}

	rapidjson::Value& properties = EnsureFeatureProperties(feature);
	const std::string oldStyleId = ReadStringProperty(&properties, "style_id");
	if (oldStyleId.empty())
		return true;

	rapidjson::Value& styles = Document["styles"];
	if (!styles.HasMember(oldStyleId.c_str()) || !styles[oldStyleId.c_str()].IsObject())
		return true;

	std::string prefix = oldStyleId + ".selection";
	for (char& c : prefix)
	{
		if (std::isspace(static_cast<unsigned char>(c)) != 0)
			c = '_';
	}

	std::string newStyleId;
	for (int suffix = 1; suffix < 100000; ++suffix)
	{
		newStyleId = prefix + "." + std::to_string(featureIndex + 1) + "." + std::to_string(suffix);
		if (!styles.HasMember(newStyleId.c_str()))
			break;
	}
	if (newStyleId.empty() || styles.HasMember(newStyleId.c_str()))
		return false;

	rapidjson::Value clonedStyle;
	CloneJsonValue(styles[oldStyleId.c_str()], clonedStyle);
	if (clonedStyle.IsObject())
	{
		const std::string cloneName = ReadStringProperty(&clonedStyle, "name", oldStyleId) + " selection";
		SetStringMember(clonedStyle, "name", cloneName);
	}

	rapidjson::Value styleKey;
	styleKey.SetString(newStyleId.c_str(), static_cast<rapidjson::SizeType>(newStyleId.size()), Document.GetAllocator());
	styles.AddMember(styleKey, clonedStyle, Document.GetAllocator());
	SetStringMember(properties, "style_id", newStyleId);
	return true;
}

void CAvisoEditorDialog::SyncDirtyStyleFieldsToStylePaint(const rapidjson::Value& properties)
{
	if (!Model.HasStyleCatalog() ||
		!Document.IsObject() ||
		!Document.HasMember("styles") ||
		!Document["styles"].IsObject())
	{
		return;
	}

	const std::string styleId = ReadStringProperty(&properties, "style_id");
	if (styleId.empty())
		return;
	rapidjson::Value& styles = Document["styles"];
	if (!styles.HasMember(styleId.c_str()) || !styles[styleId.c_str()].IsObject())
		return;

	rapidjson::Value& style = styles[styleId.c_str()];
	if (!style.HasMember("paint") || !style["paint"].IsObject())
	{
		if (style.HasMember("paint"))
			style.RemoveMember("paint");
		rapidjson::Value paint(rapidjson::kObjectType);
		style.AddMember("paint", paint, Document.GetAllocator());
	}
	rapidjson::Value& paint = style["paint"];

	auto syncString = [&](unsigned int dirtyFlag, const char* key)
	{
		if (!IsFieldDirty(dirtyFlag))
			return;
		const std::string value = ReadStringProperty(&properties, key);
		if (value.empty())
			RemoveMemberIfExists(paint, key);
		else
			SetStringMember(paint, key, value);
	};
	auto syncNumber = [&](unsigned int dirtyFlag, const char* key)
	{
		if (!IsFieldDirty(dirtyFlag))
			return;
		if (properties.HasMember(key) && properties[key].IsNumber())
			SetNumberMember(paint, key, properties[key].GetDouble());
		else
			RemoveMemberIfExists(paint, key);
	};

	syncString(kDirtyFill, "fill");
	syncNumber(kDirtyFillOpacity, "fill-opacity");
	syncString(kDirtyStroke, "stroke");
	syncNumber(kDirtyStrokeOpacity, "stroke-opacity");
	syncNumber(kDirtyStrokeWidth, "stroke-width");
	syncString(kDirtyTextFont, "text-font");
	syncString(kDirtyTextColor, "text-color");
	syncNumber(kDirtyTextSize, "text-size");
	syncString(kDirtyTextAnchor, "text-anchor");
	syncString(kDirtyHaloColor, "text-halo-color");
	syncNumber(kDirtyHaloWidth, "text-halo-width");
}

std::string CAvisoEditorDialog::ReadComboText(CComboBox& combo) const
{
	if (!::IsWindow(combo.GetSafeHwnd()))
		return "";

	CString value;
	const int selection = combo.GetCurSel();
	if (selection != CB_ERR)
		combo.GetLBText(selection, value);
	else
		combo.GetWindowText(value);
	return std::string(value.GetString());
}

void CAvisoEditorDialog::SelectComboEntryByText(CComboBox& combo, const std::string& text)
{
	if (!::IsWindow(combo.GetSafeHwnd()))
		return;

	int index = combo.FindStringExact(-1, text.c_str());
	if (index == CB_ERR)
	{
		for (int i = 0; i < combo.GetCount(); ++i)
		{
			CString itemText;
			combo.GetLBText(i, itemText);
			if (EqualsNoCase(std::string(itemText.GetString()), text))
			{
				index = i;
				break;
			}
		}
	}
	if (index != CB_ERR)
		combo.SetCurSel(index);
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

std::string CAvisoEditorDialog::FormatCoordinateDouble(double value) const
{
	std::ostringstream stream;
	stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
	return stream.str();
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

std::string CAvisoEditorDialog::GeometryCoordinatesSummary(const rapidjson::Value& geometry) const
{
	if (!geometry.IsObject() || !geometry.HasMember("type") || !geometry["type"].IsString() ||
		!geometry.HasMember("coordinates") || !geometry["coordinates"].IsArray())
	{
		return "No editable geometry coordinates.";
	}

	const std::string geometryType = geometry["type"].GetString();
	const rapidjson::Value& coordinates = geometry["coordinates"];
	size_t pointCount = 0;
	size_t pathCount = 0;

	std::function<void(const rapidjson::Value&, int)> visit = [&](const rapidjson::Value& value, int depth)
	{
		if (!value.IsArray())
			return;
		if (value.Size() >= 2 &&
			value[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
			value[static_cast<rapidjson::SizeType>(1)].IsNumber())
		{
			++pointCount;
			return;
		}
		if (depth == 1)
			++pathCount;
		for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
			visit(value[i], depth + 1);
	};

	if (geometryType == "Point")
	{
		if (coordinates.Size() >= 2 &&
			coordinates[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
			coordinates[static_cast<rapidjson::SizeType>(1)].IsNumber())
		{
			std::ostringstream stream;
			stream << "Point: lon " << FormatCoordinateDouble(coordinates[static_cast<rapidjson::SizeType>(0)].GetDouble())
				<< ", lat " << FormatCoordinateDouble(coordinates[static_cast<rapidjson::SizeType>(1)].GetDouble());
			return stream.str();
		}
		return "Point: invalid coordinates.";
	}

	visit(coordinates, 0);
	std::ostringstream stream;
	stream << geometryType << ": " << pointCount << " coordinate point(s)";
	if (pathCount > 0)
		stream << " across " << pathCount << " path/ring group(s)";
	stream << ". Coordinates are preserved unless an explicit coordinate editor changes them.";
	return stream.str();
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
		stream << FormatCoordinateDouble(point[static_cast<rapidjson::SizeType>(0)].GetDouble()) << "," << FormatCoordinateDouble(point[static_cast<rapidjson::SizeType>(1)].GetDouble());
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
	ON_WM_SHOWWINDOW()
	ON_WM_DESTROY()
	ON_WM_PAINT()
	ON_WM_DRAWITEM()
	ON_WM_CTLCOLOR()
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_AE_OBJECT_LIST, &CAvisoEditorDialog::OnObjectListItemChanged)
	ON_NOTIFY(LVN_GETDISPINFO, IDC_AE_OBJECT_LIST, &CAvisoEditorDialog::OnObjectListGetDispInfo)
	ON_NOTIFY(TCN_SELCHANGE, IDC_AE_PROPERTY_TABS, &CAvisoEditorDialog::OnPropertyTabChanged)
	ON_WM_TIMER()
	ON_EN_CHANGE(IDC_AE_SEARCH_EDIT, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_LAYER_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_OBJECT_TYPE_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_GEOMETRY_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_VISIBILITY_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_CATEGORY_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_CBN_SELCHANGE(IDC_AE_STYLE_FILTER_COMBO, &CAvisoEditorDialog::OnFilterChanged)
	ON_BN_CLICKED(IDC_AE_CURRENT_PAGE_BUTTON, &CAvisoEditorDialog::OnCurrentPageClicked)
	ON_BN_CLICKED(IDC_AE_LOAD_AVISO_BUTTON, &CAvisoEditorDialog::OnLoadAvisoClicked)
	ON_BN_CLICKED(IDC_AE_MAPS_JSON_BUTTON, &CAvisoEditorDialog::OnMapsJsonClicked)
	ON_BN_CLICKED(IDC_AE_SAVE_BUTTON, &CAvisoEditorDialog::OnSaveClicked)
	ON_BN_CLICKED(IDC_AE_FILTER_BUTTON, &CAvisoEditorDialog::OnFilterButtonClicked)
	ON_BN_CLICKED(IDC_AE_CLEAR_FILTERS_BUTTON, &CAvisoEditorDialog::OnClearFiltersClicked)
	ON_BN_CLICKED(IDC_AE_DELETE_BUTTON, &CAvisoEditorDialog::OnDeleteClicked)
	ON_BN_CLICKED(IDC_AE_APPLY_BUTTON, &CAvisoEditorDialog::OnApplyClicked)
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
	ON_EN_CHANGE(IDC_AE_RAW_EDIT, &CAvisoEditorDialog::OnFieldChanged)
END_MESSAGE_MAP()
