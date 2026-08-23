// CPDLCSettingsDialog.cpp : implementation file
//

#include "platform/windows/PrecompiledHeader.hpp"
#include "datalink/CPDLCSettingsDialog.hpp"
#include "afxdialogex.h"


// CCPDLCSettingsDialog dialog

IMPLEMENT_DYNAMIC(CCPDLCSettingsDialog, CDialogEx)

CCPDLCSettingsDialog::CCPDLCSettingsDialog(CWnd* pParent /*=NULL*/)
	: CDialogEx(CCPDLCSettingsDialog::IDD, pParent)
	, m_Password(_T("PASSWORD"))
{

}

CCPDLCSettingsDialog::~CCPDLCSettingsDialog()
{
}

void CCPDLCSettingsDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_PASSWORD, m_Password);
}


BEGIN_MESSAGE_MAP(CCPDLCSettingsDialog, CDialogEx)
	ON_BN_CLICKED(IDOK, &CCPDLCSettingsDialog::OnBnClickedOk)
END_MESSAGE_MAP()

void CCPDLCSettingsDialog::OnBnClickedOk()
{
	CDialogEx::OnOK();
}
