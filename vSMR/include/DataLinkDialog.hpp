#pragma once


// CDataLinkDialog dialog

#include "resource.h"

class CDataLinkDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CDataLinkDialog)

public:
	enum class DialogMode
	{
		Auto = 0,
		Pdc,
		Message
	};

	CDataLinkDialog(CWnd* pParent = NULL);   // standard constructor
	virtual ~CDataLinkDialog();
	void SetDialogMode(DialogMode mode);

	CString m_Callsign;
	CString m_Aircraft;
	CString m_From;
	CString m_Dest;
	CString m_CTOT;
	CString m_TSAT;
	CString m_Rwy;
	CString m_Departure;
	CString m_SSR;
	CString m_Freq;
	CString m_Message;
	CString m_Req;
	CString m_Climb;

// Dialog Data
	enum { IDD = IDD_DIALOG1 };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	virtual BOOL OnInitDialog();
	virtual BOOL PreTranslateMessage(MSG* pMsg);

	DECLARE_MESSAGE_MAP()

	DialogMode ResolveDialogMode() const;
	void ConfigureModeLayout();
	void ConstrainToEuroScopeClient();
	void SetControlLimit(int controlId, UINT limit);
	void DrawPanel(LPDRAWITEMSTRUCT drawItem, const CString& caption);
	void DrawButton(LPDRAWITEMSTRUCT drawItem, const CString& caption, bool primary);
	void DrawCloseButton(LPDRAWITEMSTRUCT drawItem);
	void DrawDialogBorder(CDC& dc);

	DialogMode m_DialogMode;
	DialogMode m_ResolvedMode;
	CBrush m_BackgroundBrush;
	CBrush m_PanelBrush;
	CBrush m_EditBrush;
	CBrush m_ReadOnlyBrush;
	CFont m_InterfaceFont;
	CFont m_BoldFont;
	int m_FullWindowWidth;
	int m_FullWindowHeight;
	int m_HotControlId;

public:
	afx_msg void OnBnClickedOk();
	afx_msg void OnBnClickedClose();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
	afx_msg void OnPaint();
	afx_msg void OnWindowPosChanging(WINDOWPOS* lpwndpos);
};
