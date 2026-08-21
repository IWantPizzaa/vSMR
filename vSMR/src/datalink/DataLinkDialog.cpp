// DataLinkDialog.cpp : implementation file
//

#include "platform/windows/PrecompiledHeader.hpp"
#include "datalink/DataLinkDialog.hpp"
#include "afxdialogex.h"

#include <algorithm>
#include <commctrl.h>
#include <new>

#pragma comment(lib, "comctl32.lib")

namespace
{
	const COLORREF kBackgroundColor = RGB(37, 48, 51);
	const COLORREF kFooterColor = RGB(36, 47, 50);
	const COLORREF kPanelColor = RGB(41, 54, 57);
	const COLORREF kPanelTitleColor = RGB(34, 45, 48);
	const COLORREF kControlColor = RGB(32, 41, 44);
	const COLORREF kReadOnlyColor = RGB(36, 48, 51);
	const COLORREF kBorderColor = RGB(5, 7, 8);
	const COLORREF kInnerBorderColor = RGB(17, 23, 25);
	const COLORREF kTextColor = RGB(201, 212, 215);
	const COLORREF kLabelTextColor = RGB(168, 183, 188);
	const COLORREF kMutedTextColor = RGB(145, 161, 166);
	const COLORREF kAccentColor = RGB(80, 150, 180);
	const COLORREF kAccentFocusColor = RGB(112, 171, 197);
	const COLORREF kAccentHoverColor = RGB(148, 196, 223);
	const COLORREF kAccentPressedColor = RGB(65, 126, 151);
	const COLORREF kButtonColor = RGB(42, 56, 59);
	const COLORREF kButtonHoverColor = RGB(53, 71, 75);
	const COLORREF kButtonPressedColor = RGB(45, 61, 65);
	const COLORREF kDisabledBackgroundColor = RGB(31, 42, 45);
	const COLORREF kDisabledTextColor = RGB(91, 107, 112);
	const COLORREF kTitleBackgroundColor = RGB(9, 12, 13);
	const COLORREF kTitleStripeColor = RGB(23, 29, 31);
	const COLORREF kTitlePadColor = RGB(16, 20, 22);
	const COLORREF kTitleTextColor = RGB(211, 221, 224);
	const COLORREF kCloseBackgroundColor = RGB(21, 27, 29);
	const COLORREF kCloseHoverColor = RGB(57, 69, 74);
	const COLORREF kCloseBorderColor = RGB(104, 117, 122);
	const COLORREF kCloseTextColor = RGB(188, 200, 204);
	const COLORREF kScrollTrackColor = RGB(41, 50, 53);
	const COLORREF kScrollThumbColor = RGB(146, 146, 146);
	const int kMessageModeDialogUnitReduction = 64;
	const int kPanelHeaderHeightAt96Dpi = 19;
	const int kPanelCornerRadiusAt96Dpi = 6;
	const int kButtonCornerRadiusAt96Dpi = 6;
	const UINT_PTR kDatalinkEditSubclassId = 1;

	bool IsValidOptionalHhmm(const CString& rawValue)
	{
		CString value(rawValue);
		value.Trim();
		if (value.IsEmpty())
			return true;
		if (value.GetLength() != 4)
			return false;
		for (int index = 0; index < value.GetLength(); ++index)
		{
			if (_istdigit(value[index]) == 0)
				return false;
		}
		const int hour = (value[0] - _T('0')) * 10 + (value[1] - _T('0'));
		const int minute = (value[2] - _T('0')) * 10 + (value[3] - _T('0'));
		return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
	}

	CWnd* ResolveEuroScopeOwner(CWnd* requestedParent)
	{
		CWnd* candidate = requestedParent != NULL ? requestedParent : AfxGetMainWnd();
		if (candidate == NULL || !::IsWindow(candidate->GetSafeHwnd()))
			return nullptr;

		HWND owner = ::GetAncestor(candidate->GetSafeHwnd(), GA_ROOTOWNER);
		if (!::IsWindow(owner))
			owner = ::GetAncestor(candidate->GetSafeHwnd(), GA_ROOT);
		if (!::IsWindow(owner) || owner == candidate->GetSafeHwnd())
			return candidate;
		return CWnd::FromHandle(owner);
	}

	bool TryGetEuroScopeClientBounds(HWND dialogWindow, CRect& bounds)
	{
		bounds.SetRectEmpty();
		if (!::IsWindow(dialogWindow))
			return false;

		const HWND owner = ::GetWindow(dialogWindow, GW_OWNER);
		if (!::IsWindow(owner))
			return false;

		RECT client = {};
		if (!::GetClientRect(owner, &client))
			return false;
		POINT topLeft = { client.left, client.top };
		POINT bottomRight = { client.right, client.bottom };
		if (!::ClientToScreen(owner, &topLeft) ||
			!::ClientToScreen(owner, &bottomRight))
		{
			return false;
		}

		bounds = CRect(topLeft, bottomRight);
		bounds.NormalizeRect();
		return !bounds.IsRectEmpty();
	}

	int ScaleForDpi(HDC deviceContext, int value)
	{
		const int dpi = deviceContext != NULL
			? ::GetDeviceCaps(deviceContext, LOGPIXELSY)
			: 96;
		return (std::max)(1, ::MulDiv(value, dpi > 0 ? dpi : 96, 96));
	}

	struct DatalinkScrollMetrics
	{
		RECT track = {};
		RECT thumb = {};
		int maximumFirstLine = 0;
		bool visible = false;
	};

	struct DatalinkEditSubclassState
	{
		bool draggingScrollThumb = false;
		int scrollThumbGrabOffset = 0;
	};

	bool CalculateDatalinkScrollMetrics(
		HWND editWindow,
		HDC deviceContext,
		DatalinkScrollMetrics& scroll)
	{
		scroll = DatalinkScrollMetrics{};
		if (!::IsWindow(editWindow) || deviceContext == NULL ||
			(::GetWindowLongPtr(editWindow, GWL_STYLE) & ES_MULTILINE) == 0)
		{
			return false;
		}

		RECT client = {};
		if (!::GetClientRect(editWindow, &client) || ::IsRectEmpty(&client))
			return false;

		const int totalLines = static_cast<int>(::SendMessage(
			editWindow,
			EM_GETLINECOUNT,
			0,
			0));
		HGDIOBJ editFont = reinterpret_cast<HGDIOBJ>(::SendMessage(
			editWindow,
			WM_GETFONT,
			0,
			0));
		HGDIOBJ previousFont = editFont != NULL
			? ::SelectObject(deviceContext, editFont)
			: NULL;
		TEXTMETRIC metrics = {};
		::GetTextMetrics(deviceContext, &metrics);
		if (previousFont != NULL)
			::SelectObject(deviceContext, previousFont);

		RECT formatRect = {};
		::SendMessage(
			editWindow,
			EM_GETRECT,
			0,
			reinterpret_cast<LPARAM>(&formatRect));
		const int lineHeight = (std::max)(
			1,
			static_cast<int>(metrics.tmHeight));
		const int visibleLines = (std::max)(
			1,
			static_cast<int>(formatRect.bottom - formatRect.top) / lineHeight);
		if (totalLines <= visibleLines)
			return false;

		const int trackInset = ScaleForDpi(deviceContext, 3);
		const int trackWidth = ScaleForDpi(deviceContext, 9);
		scroll.track = {
			client.right - trackInset - trackWidth,
			client.top + trackInset,
			client.right - trackInset,
			client.bottom - trackInset
		};
		const int trackHeight = (std::max)(
			1,
			static_cast<int>(scroll.track.bottom - scroll.track.top));
		const int minimumThumbHeight = ScaleForDpi(deviceContext, 13);
		const int thumbHeight = (std::min)(
			trackHeight,
			(std::max)(
				minimumThumbHeight,
				::MulDiv(trackHeight, visibleLines, totalLines)));
		const int firstVisibleLine = static_cast<int>(::SendMessage(
			editWindow,
			EM_GETFIRSTVISIBLELINE,
			0,
			0));
		scroll.maximumFirstLine = (std::max)(1, totalLines - visibleLines);
		const int thumbTravel = (std::max)(0, trackHeight - thumbHeight);
		const int thumbTop = scroll.track.top + ::MulDiv(
			thumbTravel,
			(std::clamp)(firstVisibleLine, 0, scroll.maximumFirstLine),
			scroll.maximumFirstLine);
		scroll.thumb = {
			scroll.track.left,
			thumbTop,
			scroll.track.right,
			thumbTop + thumbHeight
		};
		scroll.visible = true;
		return true;
	}

	void ScrollDatalinkEditToThumb(
		HWND editWindow,
		const DatalinkScrollMetrics& scroll,
		int requestedThumbTop)
	{
		if (!scroll.visible)
			return;

		const int thumbHeight = static_cast<int>(scroll.thumb.bottom - scroll.thumb.top);
		const int thumbTravel = (std::max)(
			0,
			static_cast<int>(scroll.track.bottom - scroll.track.top) - thumbHeight);
		if (thumbTravel <= 0)
			return;

		const int thumbTop = (std::clamp)(
			requestedThumbTop,
			static_cast<int>(scroll.track.top),
			static_cast<int>(scroll.track.top) + thumbTravel);
		const int targetFirstLine = ::MulDiv(
			thumbTop - scroll.track.top,
			scroll.maximumFirstLine,
			thumbTravel);
		const int currentFirstLine = static_cast<int>(::SendMessage(
			editWindow,
			EM_GETFIRSTVISIBLELINE,
			0,
			0));
		const int lineDelta = targetFirstLine - currentFirstLine;
		if (lineDelta != 0)
		{
			::SendMessage(
				editWindow,
				EM_LINESCROLL,
				0,
				static_cast<LPARAM>(lineDelta));
			::InvalidateRect(editWindow, nullptr, FALSE);
		}
	}

	void DrawDatalinkEditFrame(HWND editWindow, HDC deviceContext)
	{
		if (!::IsWindow(editWindow) || deviceContext == NULL)
			return;

		RECT client = {};
		if (!::GetClientRect(editWindow, &client) || ::IsRectEmpty(&client))
			return;

		const int savedDc = ::SaveDC(deviceContext);
		const int radius = ScaleForDpi(deviceContext, kButtonCornerRadiusAt96Dpi);
		const COLORREF borderColor = ::GetFocus() == editWindow
			? kAccentFocusColor
			: kBorderColor;
		HPEN borderPen = ::CreatePen(PS_SOLID, 1, borderColor);
		HGDIOBJ previousPen = ::SelectObject(deviceContext, borderPen);
		HGDIOBJ previousBrush = ::SelectObject(deviceContext, ::GetStockObject(NULL_BRUSH));
		::RoundRect(
			deviceContext,
			client.left,
			client.top,
			client.right - 1,
			client.bottom - 1,
			radius,
			radius);
		::SelectObject(deviceContext, previousBrush);
		::SelectObject(deviceContext, previousPen);
		::DeleteObject(borderPen);

		DatalinkScrollMetrics scroll;
		if (CalculateDatalinkScrollMetrics(editWindow, deviceContext, scroll))
		{
			::SetDCBrushColor(deviceContext, kScrollTrackColor);
			::FillRect(
				deviceContext,
				&scroll.track,
				static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
			const int thumbInset = ScaleForDpi(deviceContext, 2);
			RECT visualThumb = scroll.thumb;
			::InflateRect(&visualThumb, -thumbInset, -thumbInset);
			::SetDCBrushColor(deviceContext, kScrollThumbColor);
			HGDIOBJ oldThumbPen = ::SelectObject(
				deviceContext,
				::GetStockObject(NULL_PEN));
			HGDIOBJ oldThumbBrush = ::SelectObject(
				deviceContext,
				::GetStockObject(DC_BRUSH));
			::RoundRect(
				deviceContext,
				visualThumb.left,
				visualThumb.top,
				visualThumb.right,
				visualThumb.bottom,
				ScaleForDpi(deviceContext, 7),
				ScaleForDpi(deviceContext, 7));
			::SelectObject(deviceContext, oldThumbBrush);
			::SelectObject(deviceContext, oldThumbPen);
		}

		::RestoreDC(deviceContext, savedDc);
	}

	LRESULT CALLBACK DatalinkEditSubclassProc(
		HWND editWindow,
		UINT message,
		WPARAM wParam,
		LPARAM lParam,
		UINT_PTR subclassId,
		DWORD_PTR referenceData)
	{
		DatalinkEditSubclassState* state =
			reinterpret_cast<DatalinkEditSubclassState*>(referenceData);
		if (message == WM_NCDESTROY)
		{
			if (::GetCapture() == editWindow)
				::ReleaseCapture();
			::RemoveWindowSubclass(
				editWindow,
				DatalinkEditSubclassProc,
				subclassId);
			const LRESULT result = ::DefSubclassProc(editWindow, message, wParam, lParam);
			delete state;
			return result;
		}

		if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
			state != nullptr)
		{
			HDC deviceContext = ::GetDC(editWindow);
			DatalinkScrollMetrics scroll;
			const bool hasScroll = CalculateDatalinkScrollMetrics(
				editWindow,
				deviceContext,
				scroll);
			if (deviceContext != NULL)
				::ReleaseDC(editWindow, deviceContext);
			const POINT point = {
				static_cast<int>(static_cast<short>(LOWORD(lParam))),
				static_cast<int>(static_cast<short>(HIWORD(lParam)))
			};
			if (hasScroll && ::PtInRect(&scroll.track, point))
			{
				::SetFocus(editWindow);
				const int thumbHeight = static_cast<int>(
					scroll.thumb.bottom - scroll.thumb.top);
				state->scrollThumbGrabOffset = ::PtInRect(&scroll.thumb, point)
					? point.y - scroll.thumb.top
					: thumbHeight / 2;
				ScrollDatalinkEditToThumb(
					editWindow,
					scroll,
					point.y - state->scrollThumbGrabOffset);
				state->draggingScrollThumb = true;
				::SetCapture(editWindow);
				return 0;
			}
		}
		else if (message == WM_MOUSEMOVE && state != nullptr &&
			state->draggingScrollThumb && ::GetCapture() == editWindow)
		{
			HDC deviceContext = ::GetDC(editWindow);
			DatalinkScrollMetrics scroll;
			const bool hasScroll = CalculateDatalinkScrollMetrics(
				editWindow,
				deviceContext,
				scroll);
			if (deviceContext != NULL)
				::ReleaseDC(editWindow, deviceContext);
			if (hasScroll)
			{
				const int mouseY = static_cast<int>(static_cast<short>(HIWORD(lParam)));
				ScrollDatalinkEditToThumb(
					editWindow,
					scroll,
					mouseY - state->scrollThumbGrabOffset);
			}
			return 0;
		}
		else if (message == WM_LBUTTONUP && state != nullptr &&
			state->draggingScrollThumb)
		{
			state->draggingScrollThumb = false;
			if (::GetCapture() == editWindow)
				::ReleaseCapture();
			return 0;
		}
		else if ((message == WM_CAPTURECHANGED || message == WM_CANCELMODE) &&
			state != nullptr)
		{
			state->draggingScrollThumb = false;
		}
		else if (message == WM_SETCURSOR)
		{
			if (state != nullptr && state->draggingScrollThumb &&
				::GetCapture() == editWindow)
			{
				::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
				return TRUE;
			}
			POINT point = {};
			::GetCursorPos(&point);
			::ScreenToClient(editWindow, &point);
			HDC deviceContext = ::GetDC(editWindow);
			DatalinkScrollMetrics scroll;
			const bool overScroll = CalculateDatalinkScrollMetrics(
				editWindow,
				deviceContext,
				scroll) &&
				::PtInRect(&scroll.track, point);
			if (deviceContext != NULL)
				::ReleaseDC(editWindow, deviceContext);
			if (overScroll)
			{
				::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
				return TRUE;
			}
		}

		const LRESULT result = ::DefSubclassProc(editWindow, message, wParam, lParam);
		if (message == WM_PAINT)
		{
			HDC deviceContext = ::GetDC(editWindow);
			DrawDatalinkEditFrame(editWindow, deviceContext);
			if (deviceContext != NULL)
				::ReleaseDC(editWindow, deviceContext);
		}
		else if (message == WM_SETFOCUS ||
			message == WM_KILLFOCUS ||
			message == WM_MOUSEWHEEL ||
			message == WM_VSCROLL ||
			message == WM_KEYUP ||
			message == WM_CHAR ||
			message == WM_SETTEXT)
		{
			::InvalidateRect(editWindow, nullptr, FALSE);
		}
		return result;
	}

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

	bool IsDatalinkEditControl(int controlId)
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
		case IDC_CTOT:
		case IDC_TSAT:
		case IDC_FREQ:
		case IDC_MESSAGE:
		case IDC_ORIG:
			return true;
		default:
			return false;
		}
	}

	bool IsDatalinkButtonControl(int controlId)
	{
		return controlId == IDOK ||
			controlId == IDCANCEL ||
			controlId == IDC_DATALINK_CLOSE;
	}
}


// CDataLinkDialog dialog

IMPLEMENT_DYNAMIC(CDataLinkDialog, CDialogEx)

CDataLinkDialog::CDataLinkDialog(CWnd* pParent /*=NULL*/)
	: CDialogEx(CDataLinkDialog::IDD, ResolveEuroScopeOwner(pParent))
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
	, m_HotControlId(0)
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
	ON_WM_ERASEBKGND()
	ON_WM_DRAWITEM()
	ON_WM_PAINT()
	ON_WM_WINDOWPOSCHANGING()
END_MESSAGE_MAP()


// CDataLinkDialog message handlers


BOOL CDataLinkDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	ModifyStyle(
		WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
		WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER,
		WS_POPUP | WS_CLIPCHILDREN,
		SWP_FRAMECHANGED);
	ModifyStyleEx(
		WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
		WS_EX_APPWINDOW,
		WS_EX_TOOLWINDOW,
		SWP_FRAMECHANGED);

	m_ResolvedMode = ResolveDialogMode();

	m_BackgroundBrush.CreateSolidBrush(kBackgroundColor);
	m_PanelBrush.CreateSolidBrush(kPanelColor);
	m_EditBrush.CreateSolidBrush(kControlColor);
	m_ReadOnlyBrush.CreateSolidBrush(kReadOnlyColor);

	LOGFONT interfaceLogFont = {};
	if (GetFont() != NULL)
		GetFont()->GetLogFont(&interfaceLogFont);
	CClientDC fontDc(this);
	interfaceLogFont.lfHeight = -::MulDiv(
		8,
		fontDc.GetDeviceCaps(LOGPIXELSY),
		72);
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
		{
			if (controlId != IDC_ORIG)
				control->ModifyStyle(WS_TABSTOP, 0);
		}
	}

	for (CWnd* child = GetWindow(GW_CHILD); child != NULL; child = child->GetNextWindow())
	{
		if (!IsDatalinkEditControl(child->GetDlgCtrlID()))
			continue;
		child->ModifyStyle(WS_BORDER, 0, SWP_FRAMECHANGED);
		child->ModifyStyleEx(
			WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE,
			0,
			SWP_FRAMECHANGED);
		CClientDC editDc(child);
		const LONG_PTR editStyle = ::GetWindowLongPtr(
			child->GetSafeHwnd(),
			GWL_STYLE);
		const int leftMargin = ScaleForDpi(editDc.GetSafeHdc(), 4);
		const int rightMargin = ScaleForDpi(
			editDc.GetSafeHdc(),
			(editStyle & ES_MULTILINE) != 0 ? 12 : 4);
		child->SendMessage(
			EM_SETMARGINS,
			EC_LEFTMARGIN | EC_RIGHTMARGIN,
			MAKELPARAM(leftMargin, rightMargin));
		CRect editClient;
		child->GetClientRect(&editClient);
		if ((editStyle & ES_MULTILINE) != 0)
		{
			const int verticalMargin = ScaleForDpi(editDc.GetSafeHdc(), 2);
			RECT formatRect = {
				editClient.left + leftMargin,
				editClient.top + verticalMargin,
				(std::max)(
					editClient.left + leftMargin + 1,
					editClient.right - rightMargin),
				(std::max)(
					editClient.top + verticalMargin + 1,
					editClient.bottom - verticalMargin)
			};
			child->SendMessage(
				EM_SETRECTNP,
				0,
				reinterpret_cast<LPARAM>(&formatRect));
		}
		const int editRadius = ScaleForDpi(
			editDc.GetSafeHdc(),
			kButtonCornerRadiusAt96Dpi);
		HRGN editRegion = ::CreateRoundRectRgn(
			0,
			0,
			editClient.Width() + 1,
			editClient.Height() + 1,
			editRadius,
			editRadius);
		if (editRegion != NULL &&
			::SetWindowRgn(child->GetSafeHwnd(), editRegion, TRUE) == 0)
		{
			::DeleteObject(editRegion);
		}
		DatalinkEditSubclassState* editState =
			new (std::nothrow) DatalinkEditSubclassState();
		if (!::SetWindowSubclass(
			child->GetSafeHwnd(),
			DatalinkEditSubclassProc,
			kDatalinkEditSubclassId,
			reinterpret_cast<DWORD_PTR>(editState)))
		{
			delete editState;
		}
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
	CRect dialogClient;
	GetClientRect(&dialogClient);
	const int dialogRadius = ScaleForDpi(
		fontDc.GetSafeHdc(),
		4);
	HRGN dialogRegion = ::CreateRoundRectRgn(
		0,
		0,
		dialogClient.Width() + 1,
		dialogClient.Height() + 1,
		dialogRadius,
		dialogRadius);
	if (dialogRegion != NULL &&
		::SetWindowRgn(GetSafeHwnd(), dialogRegion, TRUE) == 0)
	{
		::DeleteObject(dialogRegion);
	}
	CenterWindow(GetOwner());
	ConstrainToEuroScopeClient();
	Invalidate(TRUE);

	if (CWnd* initialFocus = GetDlgItem(m_ResolvedMode == DialogMode::Message ? IDC_MESSAGE : IDC_TSAT))
		initialFocus->SetFocus();

	return FALSE;
}

BOOL CDataLinkDialog::PreTranslateMessage(MSG* pMsg)
{
	if (pMsg != NULL && pMsg->message == WM_MOUSEMOVE)
	{
		const int hoveredControlId = ::IsWindow(pMsg->hwnd)
			? ::GetDlgCtrlID(pMsg->hwnd)
			: 0;
		const int nextHotControlId = IsDatalinkButtonControl(hoveredControlId)
			? hoveredControlId
			: 0;
		if (nextHotControlId != m_HotControlId)
		{
			const int previousHotControlId = m_HotControlId;
			m_HotControlId = nextHotControlId;
			if (CWnd* previous = GetDlgItem(previousHotControlId))
				previous->Invalidate(FALSE);
			if (CWnd* current = GetDlgItem(m_HotControlId))
				current->Invalidate(FALSE);
		}
		if (m_HotControlId != 0)
		{
			TRACKMOUSEEVENT tracking = {};
			tracking.cbSize = sizeof(tracking);
			tracking.dwFlags = TME_LEAVE;
			tracking.hwndTrack = pMsg->hwnd;
			::TrackMouseEvent(&tracking);
		}
	}
	else if (pMsg != NULL && pMsg->message == WM_MOUSELEAVE && m_HotControlId != 0)
	{
		CWnd* hotControl = GetDlgItem(m_HotControlId);
		if (hotControl != nullptr && pMsg->hwnd == hotControl->GetSafeHwnd())
		{
			const int previousHotControlId = m_HotControlId;
			m_HotControlId = 0;
			if (CWnd* previous = GetDlgItem(previousHotControlId))
				previous->Invalidate(FALSE);
		}
	}

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

void CDataLinkDialog::ConstrainToEuroScopeClient()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;

	CRect available;
	if (!TryGetEuroScopeClientBounds(GetSafeHwnd(), available))
		return;

	CRect current;
	GetWindowRect(&current);
	const int left = (std::clamp)(
		current.left,
		available.left,
		(std::max)(available.left, available.right - current.Width()));
	const int top = (std::clamp)(
		current.top,
		available.top,
		(std::max)(available.top, available.bottom - current.Height()));
	if (left == current.left && top == current.top)
		return;

	SetWindowPos(
		nullptr,
		left,
		top,
		0,
		0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CDataLinkDialog::OnWindowPosChanging(WINDOWPOS* lpwndpos)
{
	CDialogEx::OnWindowPosChanging(lpwndpos);
	if (lpwndpos == NULL || (lpwndpos->flags & SWP_NOMOVE) != 0)
		return;

	CRect available;
	if (!TryGetEuroScopeClientBounds(GetSafeHwnd(), available))
		return;

	CRect current;
	GetWindowRect(&current);
	const int width = (lpwndpos->flags & SWP_NOSIZE) != 0
		? current.Width()
		: lpwndpos->cx;
	const int height = (lpwndpos->flags & SWP_NOSIZE) != 0
		? current.Height()
		: lpwndpos->cy;
	const int minimumLeft = static_cast<int>(available.left);
	const int minimumTop = static_cast<int>(available.top);
	const int maximumLeft = (std::max)(
		minimumLeft,
		static_cast<int>(available.right) - (std::max)(1, width));
	const int maximumTop = (std::max)(
		minimumTop,
		static_cast<int>(available.bottom) - (std::max)(1, height));
	lpwndpos->x = (std::clamp)(
		lpwndpos->x,
		minimumLeft,
		maximumLeft);
	lpwndpos->y = (std::clamp)(
		lpwndpos->y,
		minimumTop,
		maximumTop);
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
	dc.FillSolidRect(rect, kBackgroundColor);
	const int cornerRadius = ScaleForDpi(drawItem->hDC, kPanelCornerRadiusAt96Dpi);
	const int headerHeight = (std::min)(
		rect.Height() - 2,
		ScaleForDpi(drawItem->hDC, kPanelHeaderHeightAt96Dpi));
	const int savedDc = dc.SaveDC();
	CRgn panelClip;
	panelClip.CreateRoundRectRgn(
		rect.left,
		rect.top,
		rect.right + 1,
		rect.bottom + 1,
		cornerRadius,
		cornerRadius);
	dc.SelectClipRgn(&panelClip);
	dc.FillSolidRect(rect, kPanelColor);
	CRect headerRect(rect.left + 1, rect.top + 1, rect.right - 1, rect.top + headerHeight);
	dc.FillSolidRect(headerRect, kPanelTitleColor);
	CRect dividerRect(rect.left + 1, headerRect.bottom - 1, rect.right - 1, headerRect.bottom);
	dc.FillSolidRect(dividerRect, kInnerBorderColor);
	dc.RestoreDC(savedDc);

	CPen borderPen(PS_SOLID, 1, kBorderColor);
	CPen* oldPen = dc.SelectObject(&borderPen);
	CBrush* oldBrush = static_cast<CBrush*>(dc.SelectStockObject(NULL_BRUSH));
	CRect borderRect(rect);
	borderRect.right -= 1;
	borderRect.bottom -= 1;
	dc.RoundRect(borderRect, CPoint(cornerRadius, cornerRadius));
	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);

	CRect captionRect(
		rect.left + ScaleForDpi(drawItem->hDC, 6),
		rect.top + 1,
		rect.right - ScaleForDpi(drawItem->hDC, 6),
		rect.top + headerHeight - 1);
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(RGB(214, 224, 226));
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
	dc.FillSolidRect(rect, kFooterColor);
	const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
	const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
	const bool hot = (drawItem->itemState & ODS_HOTLIGHT) != 0 ||
		static_cast<int>(drawItem->CtlID) == m_HotControlId;
	const bool focused = (drawItem->itemState & ODS_FOCUS) != 0;
	const COLORREF fillColor = disabled
		? kDisabledBackgroundColor
		: primary
			? (pressed ? kAccentPressedColor : hot ? kAccentHoverColor : kAccentColor)
			: (pressed ? kButtonPressedColor : hot ? kButtonHoverColor : kButtonColor);
	const COLORREF borderColor = focused && !disabled ? kAccentFocusColor : kBorderColor;
	const int cornerRadius = ScaleForDpi(drawItem->hDC, kButtonCornerRadiusAt96Dpi);
	CBrush fillBrush(fillColor);
	CPen borderPen(PS_SOLID, 1, borderColor);
	CBrush* oldBrush = dc.SelectObject(&fillBrush);
	CPen* oldPen = dc.SelectObject(&borderPen);
	CRect buttonRect(rect);
	buttonRect.right -= 1;
	buttonRect.bottom -= 1;
	dc.RoundRect(buttonRect, CPoint(cornerRadius, cornerRadius));
	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(disabled ? kDisabledTextColor : (primary ? RGB(242, 247, 248) : kTextColor));
	CFont* oldFont = dc.SelectObject(primary ? &m_BoldFont : &m_InterfaceFont);
	if (pressed)
		rect.OffsetRect(0, 1);
	dc.DrawText(caption, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	dc.SelectObject(oldFont);
	dc.Detach();
}

void CDataLinkDialog::DrawCloseButton(LPDRAWITEMSTRUCT drawItem)
{
	if (drawItem == NULL)
		return;

	CDC dc;
	dc.Attach(drawItem->hDC);
	CRect rect(drawItem->rcItem);
	const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
	const bool hot = (drawItem->itemState & ODS_HOTLIGHT) != 0 ||
		static_cast<int>(drawItem->CtlID) == m_HotControlId;
	const COLORREF fillColor = pressed || hot
		? kCloseHoverColor
		: kCloseBackgroundColor;
	dc.FillSolidRect(rect, fillColor);
	dc.Draw3dRect(rect, kCloseBorderColor, kBorderColor);
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(kCloseTextColor);
	CFont* oldFont = dc.SelectObject(&m_InterfaceFont);
	if (pressed)
		rect.OffsetRect(0, 1);
	dc.DrawText(_T("X"), rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
	dc.SelectObject(oldFont);
	dc.Detach();
}

void CDataLinkDialog::DrawDialogBorder(CDC& dc)
{
	CRect client;
	GetClientRect(&client);
	dc.Draw3dRect(client, kBorderColor, kBorderColor);
}

void CDataLinkDialog::OnPaint()
{
	CDialogEx::OnPaint();
	CClientDC dc(this);
	DrawDialogBorder(dc);
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
		if (pWnd != NULL && pWnd->GetDlgCtrlID() >= IDC_DATALINK_CALLSIGN_LABEL &&
			pWnd->GetDlgCtrlID() <= IDC_DATALINK_ADDITIONAL_LABEL)
		{
			pDC->SetTextColor(kLabelTextColor);
			return static_cast<HBRUSH>(m_PanelBrush.GetSafeHandle());
		}
		pDC->SetTextColor(kMutedTextColor);
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

BOOL CDataLinkDialog::OnEraseBkgnd(CDC* pDC)
{
	if (pDC == NULL)
		return FALSE;

	CRect client;
	GetClientRect(&client);
	pDC->FillSolidRect(client, kBackgroundColor);

	if (CWnd* primaryButton = GetDlgItem(IDOK))
	{
		CRect buttonRect;
		primaryButton->GetWindowRect(&buttonRect);
		ScreenToClient(&buttonRect);
		const int footerTop = (std::max)(
			client.top + 1,
			buttonRect.top - ScaleForDpi(pDC->GetSafeHdc(), 6));
		CRect footerRect(client.left + 1, footerTop, client.right - 1, client.bottom - 1);
		pDC->FillSolidRect(footerRect, kFooterColor);
		CRect dividerRect(footerRect.left, footerRect.top, footerRect.right, footerRect.top + 1);
		pDC->FillSolidRect(dividerRect, kInnerBorderColor);
	}

	return TRUE;
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
		dc.FillSolidRect(rect, kTitleBackgroundColor);

		CPen stripePen(PS_SOLID, 1, kTitleStripeColor);
		CPen* oldPen = dc.SelectObject(&stripePen);
		for (int x = rect.left - rect.Height(); x < rect.right; x += 5)
		{
			dc.MoveTo(x, rect.bottom - 1);
			dc.LineTo(x + rect.Height(), rect.top);
			dc.MoveTo(x + 1, rect.bottom - 1);
			dc.LineTo(x + rect.Height() + 1, rect.top);
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
		dc.FillSolidRect(titleRect, kTitlePadColor);
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(kTitleTextColor);
		dc.DrawText(title, titleRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		dc.SelectObject(oldFont);
		dc.Draw3dRect(rect, kBorderColor, kBorderColor);
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
		DrawCloseButton(lpDrawItemStruct);
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
	m_TSAT.Trim();
	m_CTOT.Trim();
	if (!IsValidOptionalHhmm(m_TSAT))
	{
		MessageBox(
			_T("TSAT must be a valid four-digit UTC time (HHMM), or left empty."),
			_T("Invalid TSAT"),
			MB_OK | MB_ICONWARNING);
		if (CWnd* field = GetDlgItem(IDC_TSAT))
			field->SetFocus();
		return;
	}
	if (!IsValidOptionalHhmm(m_CTOT))
	{
		MessageBox(
			_T("CTOT must be a valid four-digit UTC time (HHMM), or left empty."),
			_T("Invalid CTOT"),
			MB_OK | MB_ICONWARNING);
		if (CWnd* field = GetDlgItem(IDC_CTOT))
			field->SetFocus();
		return;
	}

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
