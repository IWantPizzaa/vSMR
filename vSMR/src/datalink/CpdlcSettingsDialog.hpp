#pragma once

#include "platform/windows/ResourceIds.h"

// CCPDLCSettingsDialog dialog

class CCPDLCSettingsDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CCPDLCSettingsDialog)

public:
	CCPDLCSettingsDialog(CWnd* pParent = NULL);   // standard constructor
	virtual ~CCPDLCSettingsDialog();

	CString m_Password;

// Dialog Data
	enum { IDD = IDD_DIALOG2 };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedOk();
};
