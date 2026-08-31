#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterDialog.Internal.hpp"

#include "control_center/ControlCenterBridge.hpp"

#include <utility>

using namespace VsmrControlCenterDialogInternal;

IMPLEMENT_DYNAMIC(CVsmrControlCenterDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CVsmrControlCenterDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_MOVE()
	ON_WM_MOVING()
	ON_WM_TIMER()
	ON_WM_GETMINMAXINFO()
	ON_MESSAGE(kGithubDownloadCompleteMessage, OnGithubDownloadComplete)
	ON_MESSAGE(kWebViewMessageReceivedMessage, OnWebViewMessageReceived)
	ON_MESSAGE(kWebViewFallbackMessage, OnWebViewFallback)
	ON_MESSAGE(kWebViewReadyMessage, OnWebViewReady)
END_MESSAGE_MAP()

CVsmrControlCenterDialog::CVsmrControlCenterDialog(
	CSMRRadar* owner,
	CWnd* parent)
	: CDialogEx(CVsmrControlCenterDialog::IDD, parent),
	Owner(owner),
	WebHost(std::make_unique<WebViewHostState>()),
	LifetimeToken(std::make_shared<std::atomic<bool>>(true))
{
}

CVsmrControlCenterDialog::~CVsmrControlCenterDialog()
{
	if (LifetimeToken)
		LifetimeToken->store(false);
	StopGithubDownload();
	StopWebViewThread();
}

void CVsmrControlCenterDialog::SetOwner(CSMRRadar* owner)
{
	Owner = owner;
	if (Bridge)
		Bridge->SetOwner(owner);
}

void CVsmrControlCenterDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BOOL CVsmrControlCenterDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetWindowTextA("vSMR");
	ModifyStyle(
		WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
		WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER,
		WS_POPUP,
		SWP_FRAMECHANGED);
	ModifyStyleEx(
		WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
		WS_EX_APPWINDOW,
		WS_EX_TOOLWINDOW,
		SWP_FRAMECHANGED);

	FallbackLabel.Create(
		"Starting vSMR Control Center...",
		WS_CHILD | WS_VISIBLE | SS_CENTER,
		CRect(0, 0, 0, 0),
		this);

	VsmrBridgeHostCallbacks callbacks;
	callbacks.sendJson = [this](const std::string& json) {
		SendJsonToWebView(json);
	};
	callbacks.closeWindow = [this]() {
		OnClose();
	};
	callbacks.beginWindowDrag = [this]() {
		BeginNativeWindowDrag();
	};
	callbacks.requestComputerLoad =
		[this](const std::string& resource, const std::string& requestId) {
			RequestComputerResource(resource, requestId);
		};
	callbacks.requestResetDefaults =
		[this](const std::string& requestId) {
			RequestResetDefaults(requestId);
		};
	callbacks.cancelPendingResources = [this]() {
		StopGithubDownload();
	};
	callbacks.requestGithubLoad =
		[this](
			const std::string& resource,
			const std::string& url,
			const std::string& requestId) {
			RequestGithubResource(resource, url, requestId);
		};
	Bridge = std::make_unique<VsmrControlCenterBridge>(
		Owner,
		std::move(callbacks));

	SetTimer(
		kEuroScopeBoundsTimerId,
		kEuroScopeBoundsTimerIntervalMs,
		nullptr);
	InitializeWebView();
	ResizeWebView();
	return TRUE;
}

void CVsmrControlCenterDialog::ShowPage(Page page)
{
	CurrentPage = page;
	if (!WebHost)
		return;
	WebHost->preservePageOnNextOpen.store(false);
	WebHost->requestedPage.store(static_cast<int>(page));
	const HWND threadWindow = WebHost->threadWindow.load();
	if (WebViewReady.load() && ::IsWindow(threadWindow))
		::PostMessage(threadWindow, kWebViewShowPageMessage, 0, 0);
}

void CVsmrControlCenterDialog::ShowLastPage()
{
	if (!WebHost)
		return;
	WebHost->preservePageOnNextOpen.store(true);
	const HWND threadWindow = WebHost->threadWindow.load();
	if (WebViewReady.load() && ::IsWindow(threadWindow))
		::PostMessage(threadWindow, kWebViewShowPageMessage, 0, 0);
}

void CVsmrControlCenterDialog::SyncFromRadar(const std::string& reason)
{
	if (Bridge && WebViewReady.load())
		Bridge->PushAuthoritativeState(reason.empty() ? "runtime" : reason);
}

std::string CVsmrControlCenterDialog::PageName(Page page) const
{
	switch (page)
	{
	case Page::Aviso: return "aviso";
	case Page::Alerts: return "alerts";
	case Page::Groups: return "groups";
	case Page::Modes: return "modes";
	case Page::Profiles: return "profiles";
	case Page::Settings: return "settings";
	default: return "display";
	}
}
