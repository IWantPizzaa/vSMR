#pragma once

#include "resource.h"

class CSMRRadar;

class CProfileEditorDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CProfileEditorDialog)

public:
	explicit CProfileEditorDialog(CSMRRadar* owner, CWnd* pParent = NULL);
	virtual ~CProfileEditorDialog();

	enum { IDD = IDD_PROFILE_EDITOR_DIALOG };

	void SetOwner(CSMRRadar* owner);

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnCancel() override;
	virtual void OnOK() override;

	afx_msg void OnClose();
	afx_msg void OnMove(int x, int y);
	afx_msg void OnSize(UINT nType, int cx, int cy);

	DECLARE_MESSAGE_MAP()

private:
	CSMRRadar* Owner = nullptr;
	bool Initialized = false;

	void HideAndNotifyOwner();
	void NotifyWindowRectChanged();
};
