#include "stdafx.h"
#include "ProfileEditorDialog.hpp"
#include "SMRRadar.hpp"
#include "afxdialogex.h"

IMPLEMENT_DYNAMIC(CProfileEditorDialog, CDialogEx)

CProfileEditorDialog::CProfileEditorDialog(CSMRRadar* owner, CWnd* pParent /*=NULL*/)
	: CDialogEx(CProfileEditorDialog::IDD, pParent)
	, Owner(owner)
{
}

CProfileEditorDialog::~CProfileEditorDialog()
{
}

void CProfileEditorDialog::SetOwner(CSMRRadar* owner)
{
	Owner = owner;
}

void CProfileEditorDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BOOL CProfileEditorDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetDlgItemTextA(IDC_PROFILE_EDITOR_STATUS, "Slice 1 foundation active. This modeless window now hosts the profile editor migration.");

	Initialized = true;
	NotifyWindowRectChanged();
	return TRUE;
}

void CProfileEditorDialog::HideAndNotifyOwner()
{
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnProfileEditorWindowClosed();
}

void CProfileEditorDialog::NotifyWindowRectChanged()
{
	if (!Initialized || Owner == nullptr || !::IsWindow(GetSafeHwnd()) || !IsWindowVisible() || IsIconic())
		return;

	CRect windowRect;
	GetWindowRect(&windowRect);
	Owner->OnProfileEditorWindowLayoutChanged(windowRect);
}

void CProfileEditorDialog::OnCancel()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnOK()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnClose()
{
	HideAndNotifyOwner();
}

void CProfileEditorDialog::OnMove(int x, int y)
{
	CDialogEx::OnMove(x, y);
	NotifyWindowRectChanged();
}

void CProfileEditorDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	NotifyWindowRectChanged();
}

BEGIN_MESSAGE_MAP(CProfileEditorDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_MOVE()
	ON_WM_SIZE()
END_MESSAGE_MAP()
