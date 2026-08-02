// DataLinkDialog.cpp : implementation file
//

#include "stdafx.h"
#include "DataLinkDialog.hpp"
#include "afxdialogex.h"

namespace
{
	const COLORREF kBackgroundColor = RGB(37, 48, 51);
	const COLORREF kPanelColor = RGB(41, 54, 57);
	const COLORREF kPanelTitleColor = RGB(33, 43, 46);
	const COLORREF kControlColor = RGB(32, 41, 44);
	const COLORREF kReadOnlyColor = RGB(35, 47, 50);
	const COLORREF kBorderColor = RGB(5, 7, 8);
	const COLORREF kInnerBorderColor = RGB(17, 23, 25);
	const COLORREF kTextColor = RGB(201, 214, 219);
	const COLORREF kMutedTextColor = RGB(145, 160, 165);
	const COLORREF kAccentColor = RGB(80, 150, 180);
	const COLORREF kAccentPressedColor = RGB(65, 126, 151);
	const COLORREF kButtonColor = RGB(42, 56, 59);
	const COLORREF kButtonPressedColor = RGB(53, 71, 75);
	const int kMessageModeDialogUnitReduction = 64;

	bool HasText(const CString& value)
	{
		CString copy(value);
		copy.Trim();
		return !copy.IsEmpty();
	}

	bool IsReadOnlyDatalinkControl(int controlId)
	{
		switch (controlId)
		{
		case IDC_CALLSIGN:
		case IDC_ACT:
		case IDC_FROM:
		case IDC_DEST:
		case IDC_RWY:
		case IDC_SID:
		case IDC_CLB:
		case IDC_SSR:
		case IDC_ORIG:
			return true;
		default:
			return false;
		}
	}
}


// CDataLinkDialog dialog

IMPLEMENT_DYNAMIC(CDataLinkDialog, CDialogEx)

CDataLinkDialog::CDataLinkDialog(CWnd* pParent /*=NULL*/)
	: CDialogEx(CDataLinkDialog::IDD, pParent != NULL ? pParent : AfxGetMainWnd())
	, m_Callsign(_T(""))
	, m_Aircraft(_T(""))
	, m_From(_T(""))
	, m_Dest(_T(""))
	, m_CTOT(_T(""))
	, m_TSAT(_T(""))
	, m_Rwy(_T(""))
	, m_Departure(_T(""))
	, m_SSR(_T(""))
	, m_Freq(_T("121.800"))
	, m_Message(_T(""))
	, m_Req(_T(""))
	, m_Climb(_T(""))
	, m_DialogMode(DialogMode::Auto)
	, m_ResolvedMode(DialogMode::Pdc)
	, m_FullWindowWidth(0)
	, m_FullWindowHeight(0)
{

}

void CDataLinkDialog::SetDialogMode(DialogMode mode)
{
	m_DialogMode = mode;
}

CDataLinkDialog::~CDataLinkDialog()
{
}

void CDataLinkDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_CALLSIGN, m_Callsign);
	DDX_Text(pDX, IDC_ACT, m_Aircraft);
	DDX_Text(pDX, IDC_FROM, m_From);
	DDX_Text(pDX, IDC_DEST, m_Dest);
	DDX_Text(pDX, IDC_CTOT, m_CTOT);
	DDX_Text(pDX, IDC_TSAT, m_TSAT);
	DDX_Text(pDX, IDC_RWY, m_Rwy);
	DDX_Text(pDX, IDC_SID, m_Departure);
	DDX_Text(pDX, IDC_SSR, m_SSR);
	DDX_Text(pDX, IDC_FREQ, m_Freq);
	DDX_Text(pDX, IDC_MESSAGE, m_Message);
	DDX_Text(pDX, IDC_ORIG, m_Req);
	DDX_Text(pDX, IDC_CLB, m_Climb);
}


BEGIN_MESSAGE_MAP(CDataLinkDialog, CDialogEx)
	ON_BN_CLICKED(IDOK, &CDataLinkDialog::OnBnClickedOk)
	ON_BN_CLICKED(IDC_DATALINK_CLOSE, &CDataLinkDialog::OnBnClickedClose)
	ON_WM_CTLCOLOR()
	ON_WM_DRAWITEM()
END_MESSAGE_MAP()


// CDataLinkDialog message handlers


BOOL CDataLinkDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_ResolvedMode = ResolveDialogMode();

	m_BackgroundBrush.CreateSolidBrush(kBackgroundColor);
	m_PanelBrush.CreateSolidBrush(kPanelColor);
	m_EditBrush.CreateSolidBrush(kControlColor);
	m_ReadOnlyBrush.CreateSolidBrush(kReadOnlyColor);

	LOGFONT interfaceLogFont = {};
	if (GetFont() != NULL)
		GetFont()->GetLogFont(&interfaceLogFont);
	else
		interfaceLogFont.lfHeight = -12;
	interfaceLogFont.lfWeight = FW_NORMAL;
	_tcscpy_s(interfaceLogFont.lfFaceName, LF_FACESIZE, _T("Tahoma"));
	m_InterfaceFont.CreateFontIndirect(&interfaceLogFont);
	interfaceLogFont.lfWeight = FW_BOLD;
	m_BoldFont.CreateFontIndirect(&interfaceLogFont);

	for (CWnd* child = GetWindow(GW_CHILD); child != NULL; child = child->GetNextWindow())
		child->SetFont(&m_InterfaceFont, FALSE);

	const int boldControls[] = {
		IDC_DATALINK_FLIGHT_PANEL,
		IDC_DATALINK_CLEARANCE_PANEL,
		IDC_DATALINK_MESSAGE_PANEL
	};
	for (int controlId : boldControls)
	{
		if (CWnd* control = GetDlgItem(controlId))
			control->SetFont(&m_BoldFont, FALSE);
	}

	const int readOnlyControls[] = {
		IDC_CALLSIGN, IDC_ACT, IDC_FROM, IDC_DEST,
		IDC_RWY, IDC_SID, IDC_CLB, IDC_SSR, IDC_ORIG
	};
	for (int controlId : readOnlyControls)
	{
		if (CWnd* control = GetDlgItem(controlId))
			control->ModifyStyle(WS_TABSTOP, 0);
	}

	SetControlLimit(IDC_CALLSIGN, 12);
	SetControlLimit(IDC_ACT, 12);
	SetControlLimit(IDC_FROM, 4);
	SetControlLimit(IDC_DEST, 4);
	SetControlLimit(IDC_RWY, 5);
	SetControlLimit(IDC_SID, 16);
	SetControlLimit(IDC_CLB, 10);
	SetControlLimit(IDC_SSR, 4);
	SetControlLimit(IDC_CTOT, 4);
	SetControlLimit(IDC_TSAT, 4);
	SetControlLimit(IDC_FREQ, 7);
	SetControlLimit(IDC_MESSAGE, 220);
	SetControlLimit(IDC_ORIG, 512);

	CRect windowRect;
	GetWindowRect(&windowRect);
	m_FullWindowWidth = windowRect.Width();
	m_FullWindowHeight = windowRect.Height();

	ConfigureModeLayout();
	CenterWindow(GetOwner());

	if (CWnd* initialFocus = GetDlgItem(m_ResolvedMode == DialogMode::Message ? IDC_MESSAGE : IDC_CTOT))
		initialFocus->SetFocus();

	return FALSE;
}

BOOL CDataLinkDialog::PreTranslateMessage(MSG* pMsg)
{
	CWnd* titleBar = GetDlgItem(IDC_DATALINK_TITLEBAR);
	if (pMsg != NULL && titleBar != NULL && pMsg->hwnd == titleBar->GetSafeHwnd() &&
		pMsg->message == WM_LBUTTONDOWN)
	{
		POINT cursor = {};
		::GetCursorPos(&cursor);
		ReleaseCapture();
		SendMessage(WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
		return TRUE;
	}
	if (pMsg != NULL && pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN)
	{
		CWnd* focusedControl = GetFocus();
		if (focusedControl != NULL)
		{
			const int focusedControlId = focusedControl->GetDlgCtrlID();
			if (focusedControlId == IDCANCEL || focusedControlId == IDC_DATALINK_CLOSE)
			{
				OnBnClickedClose();
				return TRUE;
			}
		}
		OnBnClickedOk();
		return TRUE;
	}

	return CDialogEx::PreTranslateMessage(pMsg);
}

CDataLinkDialog::DialogMode CDataLinkDialog::ResolveDialogMode() const
{
	if (m_DialogMode != DialogMode::Auto)
		return m_DialogMode;

	// The existing PDC call populates one or more clearance-owned fields. The
	// generic Message call only supplies the flight summary and received text.
	if (HasText(m_Rwy) || HasText(m_Departure) || HasText(m_SSR) ||
		HasText(m_Climb) || HasText(m_CTOT) || HasText(m_TSAT))
	{
		return DialogMode::Pdc;
	}

	return DialogMode::Message;
}

void CDataLinkDialog::ConfigureModeLayout()
{
	const bool messageMode = (m_ResolvedMode == DialogMode::Message);
	const CString windowTitle = messageMode ? _T("vSMR - Message") : _T("vSMR - PDC");
	SetWindowText(windowTitle);
	SetDlgItemText(IDOK, messageMode ? _T("Send Message") : _T("Send PDC"));
	SetDlgItemText(IDC_DATALINK_MESSAGE_PANEL, messageMode ? _T("Message") : _T("Request and message"));
	SetDlgItemText(IDC_DATALINK_REQUEST_LABEL, messageMode ? _T("Received message") : _T("Pilot request"));
	SetDlgItemText(IDC_DATALINK_ADDITIONAL_LABEL, messageMode ? _T("Reply") : _T("Additional message"));

	if (!messageMode)
		return;

	const int clearanceControls[] = {
		IDC_DATALINK_CLEARANCE_PANEL,
		IDC_DATALINK_RWY_LABEL, IDC_RWY,
		IDC_DATALINK_SID_LABEL, IDC_SID,
		IDC_DATALINK_CLIMB_LABEL, IDC_CLB,
		IDC_DATALINK_SSR_LABEL, IDC_SSR,
		IDC_DATALINK_CTOT_LABEL, IDC_CTOT,
		IDC_DATALINK_TSAT_LABEL, IDC_TSAT,
		IDC_DATALINK_FREQ_LABEL, IDC_FREQ
	};
	for (int controlId : clearanceControls)
	{
		if (CWnd* control = GetDlgItem(controlId))
			control->ShowWindow(SW_HIDE);
	}

	CRect reductionRect(0, 0, 0, kMessageModeDialogUnitReduction);
	MapDialogRect(&reductionRect);
	const int verticalReduction = reductionRect.bottom;
	const int controlsToMove[] = {
		IDC_DATALINK_MESSAGE_PANEL,
		IDC_DATALINK_REQUEST_LABEL, IDC_ORIG,
		IDC_DATALINK_ADDITIONAL_LABEL, IDC_MESSAGE,
		IDCANCEL, IDOK
	};
	for (int controlId : controlsToMove)
	{
		CWnd* control = GetDlgItem(controlId);
		if (control == NULL)
			continue;

		CRect rect;
		control->GetWindowRect(&rect);
		ScreenToClient(&rect);
		rect.OffsetRect(0, -verticalReduction);
		control->MoveWindow(rect, FALSE);
	}

	SetWindowPos(
		NULL,
		0,
		0,
		m_FullWindowWidth,
		m_FullWindowHeight > verticalReduction ? m_FullWindowHeight - verticalReduction : 1,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	Invalidate(FALSE);
}

void CDataLinkDialog::SetControlLimit(int controlId, UINT limit)
{
	if (CWnd* edit = GetDlgItem(controlId))
		edit->SendMessage(EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
}

void CDataLinkDialog::DrawPanel(LPDRAWITEMSTRUCT drawItem, const CString& caption)
{
	if (drawItem == NULL)
		return;

	CDC dc;
	dc.Attach(drawItem->hDC);
	CRect rect(drawItem->rcItem);
	dc.FillSolidRect(rect, kPanelColor);
	dc.Draw3dRect(rect, kBorderColor, kInnerBorderColor);

	CRect captionRect(rect.left + 6, rect.top, rect.right - 5, rect.top + 13);
	dc.FillSolidRect(captionRect, kPanelColor);
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(kTextColor);
	CFont* oldFont = dc.SelectObject(&m_BoldFont);
	dc.DrawText(caption, captionRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	dc.SelectObject(oldFont);
	dc.Detach();
}

void CDataLinkDialog::DrawButton(LPDRAWITEMSTRUCT drawItem, const CString& caption, bool primary)
{
	if (drawItem == NULL)
		return;

	CDC dc;
	dc.Attach(drawItem->hDC);
	CRect rect(drawItem->rcItem);
	const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
	const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
	const COLORREF fillColor = primary
		? (pressed ? kAccentPressedColor : kAccentColor)
		: (pressed ? kButtonPressedColor : kButtonColor);

	dc.FillSolidRect(rect, fillColor);
	dc.Draw3dRect(rect, kBorderColor, kBorderColor);
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(disabled ? kMutedTextColor : (primary ? RGB(242, 247, 248) : kTextColor));
	CFont* oldFont = dc.SelectObject(primary ? &m_BoldFont : &m_InterfaceFont);
	if (pressed)
		rect.OffsetRect(0, 1);
	dc.DrawText(caption, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	if ((drawItem->itemState & ODS_FOCUS) != 0)
	{
		CRect focusRect(drawItem->rcItem);
		focusRect.DeflateRect(3, 3);
		dc.DrawFocusRect(focusRect);
	}
	dc.SelectObject(oldFont);
	dc.Detach();
}

HBRUSH CDataLinkDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH defaultBrush = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
	if (pDC == NULL)
		return defaultBrush;

	switch (nCtlColor)
	{
	case CTLCOLOR_DLG:
		return static_cast<HBRUSH>(m_BackgroundBrush.GetSafeHandle());
	case CTLCOLOR_STATIC:
		if (pWnd != NULL && IsReadOnlyDatalinkControl(pWnd->GetDlgCtrlID()))
		{
			pDC->SetBkMode(OPAQUE);
			pDC->SetBkColor(kReadOnlyColor);
			pDC->SetTextColor(kMutedTextColor);
			return static_cast<HBRUSH>(m_ReadOnlyBrush.GetSafeHandle());
		}
		pDC->SetBkMode(TRANSPARENT);
		pDC->SetTextColor(kMutedTextColor);
		if (pWnd != NULL && pWnd->GetDlgCtrlID() >= IDC_DATALINK_CALLSIGN_LABEL &&
			pWnd->GetDlgCtrlID() <= IDC_DATALINK_ADDITIONAL_LABEL)
		{
			return static_cast<HBRUSH>(m_PanelBrush.GetSafeHandle());
		}
		return static_cast<HBRUSH>(m_BackgroundBrush.GetSafeHandle());
	case CTLCOLOR_EDIT:
	{
		const bool readOnly = pWnd != NULL && (pWnd->GetStyle() & ES_READONLY) != 0;
		const COLORREF background = readOnly ? kReadOnlyColor : kControlColor;
		pDC->SetBkMode(OPAQUE);
		pDC->SetBkColor(background);
		pDC->SetTextColor(readOnly ? kMutedTextColor : kTextColor);
		return static_cast<HBRUSH>((readOnly ? m_ReadOnlyBrush : m_EditBrush).GetSafeHandle());
	}
	case CTLCOLOR_BTN:
		pDC->SetBkMode(TRANSPARENT);
		pDC->SetTextColor(kTextColor);
		return static_cast<HBRUSH>(m_BackgroundBrush.GetSafeHandle());
	default:
		return defaultBrush;
	}
}

void CDataLinkDialog::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (lpDrawItemStruct == NULL)
		return;

	if (nIDCtl == IDC_DATALINK_TITLEBAR)
	{
		CDC dc;
		dc.Attach(lpDrawItemStruct->hDC);
		CRect rect(lpDrawItemStruct->rcItem);
		dc.FillSolidRect(rect, RGB(9, 12, 13));

		CPen stripePen(PS_SOLID, 1, RGB(27, 35, 38));
		CPen* oldPen = dc.SelectObject(&stripePen);
		for (int x = rect.left - rect.Height(); x < rect.right; x += 5)
		{
			dc.MoveTo(x, rect.bottom - 1);
			dc.LineTo(x + rect.Height(), rect.top);
		}
		dc.SelectObject(oldPen);

		const CString title = m_ResolvedMode == DialogMode::Message
			? _T("vSMR \x00B7 Message")
			: _T("vSMR \x00B7 PDC");
		CFont* oldFont = dc.SelectObject(&m_BoldFont);
		const CSize titleSize = dc.GetTextExtent(title);
		CRect titleRect(
			rect.CenterPoint().x - titleSize.cx / 2 - 7,
			rect.top,
			rect.CenterPoint().x + (titleSize.cx + 1) / 2 + 7,
			rect.bottom);
		dc.FillSolidRect(titleRect, kPanelTitleColor);
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(RGB(211, 221, 224));
		dc.DrawText(title, titleRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		dc.SelectObject(oldFont);
		dc.Detach();
		return;
	}

	if (nIDCtl == IDC_DATALINK_FLIGHT_PANEL)
	{
		DrawPanel(lpDrawItemStruct, _T("Flight"));
		return;
	}
	if (nIDCtl == IDC_DATALINK_CLEARANCE_PANEL)
	{
		DrawPanel(lpDrawItemStruct, _T("Clearance"));
		return;
	}
	if (nIDCtl == IDC_DATALINK_MESSAGE_PANEL)
	{
		DrawPanel(lpDrawItemStruct, m_ResolvedMode == DialogMode::Message ? _T("Message") : _T("Request and message"));
		return;
	}
	if (nIDCtl == IDC_DATALINK_CLOSE)
	{
		DrawButton(lpDrawItemStruct, _T("X"), false);
		return;
	}
	if (nIDCtl == IDOK)
	{
		DrawButton(lpDrawItemStruct, m_ResolvedMode == DialogMode::Message ? _T("Send Message") : _T("Send PDC"), true);
		return;
	}
	if (nIDCtl == IDCANCEL)
	{
		DrawButton(lpDrawItemStruct, _T("Cancel"), false);
		return;
	}

	CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

void CDataLinkDialog::OnBnClickedOk()
{
	if (!UpdateData(TRUE))
		return;

	if (m_CTOT.IsEmpty())
		m_CTOT = _T("no");
	if (m_TSAT.IsEmpty())
		m_TSAT = _T("no");
	if (m_Message.IsEmpty())
		m_Message = _T("no");
	if (m_Freq.IsEmpty())
		m_Freq = _T("no");

	EndDialog(IDOK);
}

void CDataLinkDialog::OnBnClickedClose()
{
	EndDialog(IDCANCEL);
}
