#pragma once

#include "resource.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class CSMRRadar;
class VsmrControlCenterBridge;

class CVsmrControlCenterDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CVsmrControlCenterDialog)

public:
	enum class Page
	{
		Display,
		Aviso,
		Alerts,
		Groups,
		Modes,
		Profiles,
		Settings
	};

	explicit CVsmrControlCenterDialog(CSMRRadar* owner, CWnd* parent = nullptr);
	virtual ~CVsmrControlCenterDialog();

	enum { IDD = IDD_VSMR_CONTROL_CENTER_DIALOG };

	void SetOwner(CSMRRadar* owner);
	void ShowPage(Page page);
	void SyncFromRadar();
	void RestoreWindowPlacementOrDefault(const CRect& fallback);

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnCancel() override;
	virtual void OnOK() override;

	afx_msg void OnClose();
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMove(int x, int y);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg LRESULT OnGithubDownloadComplete(WPARAM wParam, LPARAM lParam);

	DECLARE_MESSAGE_MAP()

private:
	struct WebViewHostState;
	struct GithubDownloadResult;

	void InitializeWebView();
	HRESULT OnWebViewEnvironmentCreated(HRESULT result, IUnknown* environment);
	HRESULT OnWebViewControllerCreated(HRESULT result, IUnknown* controller);
	void ConfigureWebView();
	void ResizeWebView();
	void ShutdownWebView();
	void ShowFallback(const std::string& message);
	void SendJsonToWebView(const std::string& json);
	void BeginNativeWindowDrag();
	void RequestComputerResource(
		const std::string& resource,
		const std::string& requestId);
	void RequestResetDefaults(const std::string& requestId);
	void RequestGithubResource(
		const std::string& resource,
		const std::string& url,
		const std::string& requestId);
	void SaveWindowPlacement();
	std::wstring ResolveWebResourceFolder() const;
	std::wstring WebViewUserDataFolder() const;
	std::wstring WindowPlacementPath() const;
	std::string PageName(Page page) const;

	CSMRRadar* Owner = nullptr;
	Page CurrentPage = Page::Display;
	bool Closing = false;
	bool WindowPlacementDirty = false;
	bool WebViewReady = false;

	CStatic FallbackLabel;
	std::unique_ptr<WebViewHostState> WebHost;
	std::unique_ptr<VsmrControlCenterBridge> Bridge;
	std::shared_ptr<std::atomic<bool>> LifetimeToken;
	std::thread GithubDownloadThread;
	std::atomic<bool> GithubDownloadInProgress{ false };
};
