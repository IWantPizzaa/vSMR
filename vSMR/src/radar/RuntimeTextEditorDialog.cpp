#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RuntimeTextEditorDialog.hpp"
#include "platform/windows/ResourceIds.h"

#include "afxdialogex.h"

#include <algorithm>
#include <cctype>

namespace
{
	struct Palette
	{
		COLORREF background;
		COLORREF title;
		COLORREF field;
		COLORREF button;
		COLORREF accent;
		COLORREF border;
		COLORREF text;
		COLORREF muted;
		COLORREF accentText;
		COLORREF error;
	};

	Palette ResolvePalette(bool dayTheme)
	{
		if (dayTheme)
			return { RGB(171, 178, 180), RGB(115, 125, 128), RGB(247, 251, 252),
				RGB(173, 181, 183), RGB(63, 130, 159), RGB(63, 72, 76),
				RGB(23, 33, 38), RGB(70, 83, 88), RGB(247, 251, 252), RGB(141, 52, 58) };
		return { RGB(32, 42, 45), RGB(23, 29, 31), RGB(41, 56, 59),
			RGB(41, 57, 59), RGB(80, 150, 180), RGB(5, 7, 8),
			RGB(208, 217, 220), RGB(143, 161, 166), RGB(244, 248, 249), RGB(229, 167, 167) };
	}

	class RuntimeTextEditorDialog final : public CDialogEx
	{
	public:
		RuntimeTextEditorDialog(VsmrRuntimeTextEditor::Kind kind,
			const std::string& initialValue, bool dayTheme, CWnd* parent)
			: CDialogEx(IDD_VSMR_RUNTIME_TEXT_EDIT, parent),
			kind_(kind), initialValue_(initialValue.c_str()), palette_(ResolvePalette(dayTheme))
		{
		}

		const std::string& AcceptedValue() const noexcept { return acceptedValue_; }

	protected:
		BOOL OnInitDialog() override
		{
			CDialogEx::OnInitDialog();
			ModifyStyle(WS_CAPTION | WS_SYSMENU | WS_BORDER, WS_POPUP | WS_CLIPCHILDREN,
				SWP_FRAMECHANGED);
			ModifyStyleEx(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE,
				WS_EX_TOOLWINDOW, SWP_FRAMECHANGED);
			backgroundBrush_.CreateSolidBrush(palette_.background);
			fieldBrush_.CreateSolidBrush(palette_.field);

			LOGFONT font{};
			CClientDC dc(this);
			font.lfHeight = -::MulDiv(11, dc.GetDeviceCaps(LOGPIXELSY), 96);
			strcpy_s(font.lfFaceName, LF_FACESIZE, "Segoe UI");
			interfaceFont_.CreateFontIndirect(&font);
			font.lfWeight = FW_BOLD;
			boldFont_.CreateFontIndirect(&font);
			for (CWnd* child = GetWindow(GW_CHILD); child != nullptr;
				child = child->GetNextWindow())
				child->SetFont(&interfaceFont_, FALSE);

			const char* label = kind_ == VsmrRuntimeTextEditor::Kind::Airport
				? "Airport ICAO" : kind_ == VsmrRuntimeTextEditor::Kind::Preset
					? "Preset name" : "PDC login";
			CWnd* edit = GetDlgItem(IDC_RUNTIME_TEXT_VALUE);
			if (edit == nullptr || GetDlgItem(IDC_RUNTIME_TEXT_LABEL) == nullptr ||
				GetDlgItem(IDC_RUNTIME_TEXT_ERROR) == nullptr)
			{
				EndDialog(IDCANCEL);
				return TRUE;
			}
			SetDlgItemText(IDC_RUNTIME_TEXT_LABEL, label);
			GetDlgItem(IDC_RUNTIME_TEXT_LABEL)->SetFont(&boldFont_, FALSE);
			SetDlgItemText(IDC_RUNTIME_TEXT_VALUE, initialValue_);
			edit->SendMessage(EM_LIMITTEXT,
				kind_ == VsmrRuntimeTextEditor::Kind::Preset ? 128 :
				kind_ == VsmrRuntimeTextEditor::Kind::Airport ? 4 : 16);
			edit->ModifyStyle(WS_BORDER, 0, SWP_FRAMECHANGED);
			edit->ModifyStyleEx(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE,
				0, SWP_FRAMECHANGED);
			edit->SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
				MAKELPARAM(6, 6));
			CenterWindow();
			edit->SetFocus();
			edit->SendMessage(EM_SETSEL, 0, -1);
			return FALSE;
		}

		void OnOK() override
		{
			CString value;
			GetDlgItemText(IDC_RUNTIME_TEXT_VALUE, value);
			value.Trim();
			std::string candidate = static_cast<const char*>(CStringA(value));
			if (kind_ != VsmrRuntimeTextEditor::Kind::Preset)
				std::transform(candidate.begin(), candidate.end(), candidate.begin(),
					[](unsigned char character) { return static_cast<char>(std::toupper(character)); });
			const bool validAirport = candidate.size() == 4 &&
				std::all_of(candidate.begin(), candidate.end(), [](unsigned char character)
				{
					return std::isalnum(character) != 0;
				});
			const bool validPdc = !candidate.empty() && candidate.size() <= 16 &&
				std::all_of(candidate.begin(), candidate.end(), [](unsigned char character)
				{
					return std::isalnum(character) != 0 || character == '_' || character == '-';
				});
			const char* error = nullptr;
			if (kind_ == VsmrRuntimeTextEditor::Kind::Airport && !validAirport)
				error = "Enter a four-character airport ICAO code.";
			else if (kind_ == VsmrRuntimeTextEditor::Kind::Preset && candidate.empty())
				error = "Enter a preset name.";
			else if (kind_ == VsmrRuntimeTextEditor::Kind::PdcLogon && !validPdc)
				error = "Use 1-16 letters, numbers, _ or -.";
			if (error != nullptr)
			{
			SetDlgItemText(IDC_RUNTIME_TEXT_ERROR, error);
			GetDlgItem(IDC_RUNTIME_TEXT_VALUE)->SetFocus();
			return;
			}
			acceptedValue_ = std::move(candidate);
			CDialogEx::OnOK();
		}

		afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* control, UINT type)
		{
			if (type == CTLCOLOR_EDIT)
			{
				dc->SetTextColor(palette_.text);
				dc->SetBkColor(palette_.field);
				return static_cast<HBRUSH>(fieldBrush_.GetSafeHandle());
			}
			if (type == CTLCOLOR_STATIC)
			{
				dc->SetTextColor(control->GetDlgCtrlID() == IDC_RUNTIME_TEXT_ERROR
					? palette_.error : palette_.muted);
				dc->SetBkColor(palette_.background);
			}
			return static_cast<HBRUSH>(backgroundBrush_.GetSafeHandle());
		}

		afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }

		afx_msg void OnPaint()
		{
			CPaintDC dc(this);
			CRect client;
			GetClientRect(&client);
			dc.FillSolidRect(client, palette_.background);
			CRect title(client.left + 1, client.top + 1, client.right - 1, client.top + 26);
			dc.FillSolidRect(title, palette_.title);
			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(palette_.text);
			CFont* previousFont = dc.SelectObject(&boldFont_);
			title.DeflateRect(10, 0);
			dc.DrawText(_T("vSMR  /  Edit"), title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
			dc.SelectObject(previousFont);
			CRect field;
			GetDlgItem(IDC_RUNTIME_TEXT_VALUE)->GetWindowRect(&field);
			ScreenToClient(&field);
			field.InflateRect(2, 2);
			dc.Draw3dRect(field, palette_.border, palette_.border);
			dc.Draw3dRect(client, palette_.border, palette_.border);
		}

		afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT drawItem)
		{
			if (drawItem == nullptr)
				return;
			if (id != IDOK && id != IDCANCEL)
			{
				CDialogEx::OnDrawItem(id, drawItem);
				return;
			}
			CDC dc;
			dc.Attach(drawItem->hDC);
			const CRect area(drawItem->rcItem);
			const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
			const COLORREF fill = pressed ? palette_.title :
				id == IDOK ? palette_.accent : palette_.button;
			CBrush brush(fill);
			CPen pen(PS_SOLID, 1, palette_.border);
			CBrush* oldBrush = dc.SelectObject(&brush);
			CPen* oldPen = dc.SelectObject(&pen);
			dc.RoundRect(area, CPoint(6, 6));
			dc.SelectObject(oldPen);
			dc.SelectObject(oldBrush);
			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(id == IDOK && !pressed ? palette_.accentText : palette_.text);
			CFont* oldFont = dc.SelectObject(&interfaceFont_);
			dc.DrawText(id == IDOK ? _T("Save") : _T("Cancel"),
				CRect(area), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
			dc.SelectObject(oldFont);
			dc.Detach();
		}

		DECLARE_MESSAGE_MAP()

	private:
		VsmrRuntimeTextEditor::Kind kind_;
		CString initialValue_;
		std::string acceptedValue_;
		Palette palette_;
		CBrush backgroundBrush_;
		CBrush fieldBrush_;
		CFont interfaceFont_;
		CFont boldFont_;
	};

	BEGIN_MESSAGE_MAP(RuntimeTextEditorDialog, CDialogEx)
		ON_WM_CTLCOLOR()
		ON_WM_ERASEBKGND()
		ON_WM_PAINT()
		ON_WM_DRAWITEM()
	END_MESSAGE_MAP()
}

bool VsmrRuntimeTextEditor::Show(Kind kind, const std::string& initialValue,
	bool dayTheme, std::string& acceptedValue)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	acceptedValue.clear();
	try
	{
		RuntimeTextEditorDialog dialog(kind, initialValue, dayTheme, AfxGetMainWnd());
		INT_PTR result = dialog.DoModal();
		if (result == -1)
		{
			// EuroScope may not expose its main MFC window on every screen.
			RuntimeTextEditorDialog fallback(kind, initialValue, dayTheme, nullptr);
			result = fallback.DoModal();
			if (result != IDOK)
				return false;
			acceptedValue = fallback.AcceptedValue();
			return true;
		}
		if (result != IDOK)
			return false;
		acceptedValue = dialog.AcceptedValue();
		return true;
	}
	catch (...)
	{
		return false;
	}
}
