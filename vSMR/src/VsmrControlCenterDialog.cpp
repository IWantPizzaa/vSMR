#include "stdafx.h"
#include "VsmrControlCenterDialog.hpp"

#include "HttpHelper.hpp"
#include "Logger.h"
#include "SMRRadar.hpp"
#include "VsmrControlCenterBridge.hpp"

#include "WebView2.h"
#include <wrl.h>

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

IMPLEMENT_DYNAMIC(CVsmrControlCenterDialog, CDialogEx)

namespace
{
	constexpr UINT kGithubDownloadCompleteMessage = WM_APP + 0x552;
	constexpr UINT kWebViewMessageReceivedMessage = WM_APP + 0x560;
	constexpr UINT kWebViewFallbackMessage = WM_APP + 0x561;
	constexpr UINT kWebViewReadyMessage = WM_APP + 0x562;
	constexpr UINT kWebViewResizeMessage = WM_APP + 0x563;
	constexpr UINT kWebViewSendJsonMessage = WM_APP + 0x564;
	constexpr UINT kWebViewShowPageMessage = WM_APP + 0x565;
	constexpr UINT kWebViewParentMovedMessage = WM_APP + 0x566;
	constexpr UINT kWebViewShutdownMessage = WM_APP + 0x567;
	constexpr UINT kWebViewBeginWindowDragMessage = WM_APP + 0x568;
	constexpr UINT kWebViewPageReadyMessage = WM_APP + 0x569;
	constexpr int kDefaultClientWidth = 728;
	constexpr int kDefaultClientHeight = 500;
	constexpr int kMinimumWindowWidth = 646;
	constexpr int kMinimumWindowHeight = 480;
	const wchar_t* kVirtualHostName = L"app.vsmr";
	const wchar_t* kVirtualOriginPrefix = L"https://app.vsmr/";
	const wchar_t* kWebViewHostWindowClass = L"vSMR.ControlCenter.WebViewHost";
	std::mutex gWebViewHostWindowClassMutex;
	unsigned int gWebViewHostWindowClassUsers = 0;
	HINSTANCE gWebViewHostWindowClassInstance = nullptr;

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty())
			return {};
		const int length = ::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0);
		if (length <= 0)
			return std::wstring(value.begin(), value.end());
		std::wstring output(static_cast<size_t>(length), L'\0');
		::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			output.data(),
			length);
		return output;
	}

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return {};
		const int length = ::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
		{
			std::string fallback;
			fallback.reserve(value.size());
			for (wchar_t character : value)
				fallback.push_back(
					character >= 0 && character <= 0x7f
						? static_cast<char>(character)
						: '?');
			return fallback;
		}
		std::string output(static_cast<size_t>(length), '\0');
		::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			output.data(),
			length,
			nullptr,
			nullptr);
		return output;
	}

	std::wstring LocalAppDataPath()
	{
		wchar_t buffer[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(
			L"LOCALAPPDATA",
			buffer,
			static_cast<DWORD>(std::size(buffer)));
		if (length > 0 && length < std::size(buffer))
			return std::wstring(buffer, length);

		const DWORD tempLength = ::GetTempPathW(
			static_cast<DWORD>(std::size(buffer)),
			buffer);
		if (tempLength > 0 && tempLength < std::size(buffer))
			return std::wstring(buffer, tempLength);
		return L".";
	}

	bool IsAllowedNavigation(const std::wstring& uri)
	{
		if (uri == L"about:blank")
			return true;
		if (uri.size() < wcslen(kVirtualOriginPrefix))
			return false;
		return _wcsnicmp(
			uri.c_str(),
			kVirtualOriginPrefix,
			wcslen(kVirtualOriginPrefix)) == 0;
	}

	CRect ClampWindowRectToMonitor(const CRect& requested)
	{
		CRect result = requested;
		int width = (std::max)(kMinimumWindowWidth, result.Width());
		int height = (std::max)(kMinimumWindowHeight, result.Height());

		RECT requestedRect = result;
		HMONITOR monitor = ::MonitorFromRect(
			&requestedRect,
			MONITOR_DEFAULTTONEAREST);
		MONITORINFO monitorInfo = {};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (monitor == nullptr || !::GetMonitorInfoW(monitor, &monitorInfo))
			return CRect(
				result.left,
				result.top,
				result.left + width,
				result.top + height);

		const CRect workArea(monitorInfo.rcWork);
		width = (std::min)(width, workArea.Width());
		height = (std::min)(height, workArea.Height());
		const int left = (std::clamp)(
			result.left,
			workArea.left,
			(std::max)(workArea.left, workArea.right - width));
		const int top = (std::clamp)(
			result.top,
			workArea.top,
			(std::max)(workArea.top, workArea.bottom - height));
		return CRect(left, top, left + width, top + height);
	}

	bool ReadTextFile(const std::filesystem::path& path, std::string& text)
	{
		text.clear();
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
			return false;
		std::ostringstream stream;
		stream << input.rdbuf();
		text = stream.str();
		return static_cast<bool>(input) || input.eof();
	}

	bool WriteTextFileAtomically(
		const std::filesystem::path& path,
		const std::string& text)
	{
		try
		{
			if (path.has_parent_path())
				std::filesystem::create_directories(path.parent_path());
			const std::filesystem::path temp =
				path.string() + ".tmp." + std::to_string(::GetCurrentProcessId());
			{
				std::ofstream output(
					temp,
					std::ios::binary | std::ios::trunc);
				if (!output.is_open())
					return false;
				output.write(
					text.data(),
					static_cast<std::streamsize>(text.size()));
				output.flush();
				output.close();
				if (!output)
				{
					std::error_code ignored;
					std::filesystem::remove(temp, ignored);
					return false;
				}
			}
			if (!::MoveFileExW(
				temp.c_str(),
				path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				std::error_code ignored;
				std::filesystem::remove(temp, ignored);
				return false;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}

struct CVsmrControlCenterDialog::WebViewHostState
{
	std::mutex crossThreadMutex;
	std::deque<std::string> outboundJson;
	std::deque<std::string> inboundJson;
	std::deque<std::string> fallbackMessages;
	std::wstring resourceFolder;
	std::wstring userDataFolder;
	HWND dialogWindow = nullptr;
	HINSTANCE moduleInstance = nullptr;
	std::atomic<HWND> threadWindow{ nullptr };
	std::atomic<bool> stopRequested{ false };
	std::atomic<int> clientWidth{ kDefaultClientWidth };
	std::atomic<int> clientHeight{ kDefaultClientHeight };
	std::atomic<int> requestedPage{
		static_cast<int>(CVsmrControlCenterDialog::Page::Display) };
	std::atomic<bool> pageReady{ false };
	bool windowClassAcquired = false;
	bool environmentCreationPending = false;
	bool controllerCreationPending = false;
	ComPtr<ICoreWebView2Environment> environment;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> webView;
	EventRegistrationToken navigationStartingToken = {};
	EventRegistrationToken newWindowToken = {};
	EventRegistrationToken webMessageToken = {};
	EventRegistrationToken permissionToken = {};
	bool navigationStartingRegistered = false;
	bool newWindowRegistered = false;
	bool webMessageRegistered = false;
	bool permissionRegistered = false;
	bool comInitialized = false;
};

struct CVsmrControlCenterDialog::GithubDownloadResult
{
	std::string resource;
	std::string source;
	std::string requestId;
	std::string body;
};

BEGIN_MESSAGE_MAP(CVsmrControlCenterDialog, CDialogEx)
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_WM_MOVE()
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
	StopWebViewThread();
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();
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

	SetWindowTextA("vSMR Control Center");
	ModifyStyle(
		WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
		0,
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

	InitializeWebView();
	ResizeWebView();
	return TRUE;
}

void CVsmrControlCenterDialog::InitializeWebView()
{
	const std::wstring resourceFolder = ResolveWebResourceFolder();
	if (resourceFolder.empty())
	{
		ShowFallback(
			"vSMR web resources were not found. Reinstall the vSMR_webUI folder next to vSMR.dll.");
		return;
	}

	const std::wstring userDataFolder = WebViewUserDataFolder();
	try
	{
		std::filesystem::create_directories(userDataFolder);
	}
	catch (const std::exception& ex)
	{
		ShowFallback(
			"Unable to create the WebView2 data folder: " +
			std::string(ex.what()));
		return;
	}

	if (!WebHost || WebViewThread.joinable())
		return;

	WebHost->resourceFolder = resourceFolder;
	WebHost->userDataFolder = userDataFolder;
	WebHost->dialogWindow = GetSafeHwnd();
	WebHost->moduleInstance = AfxGetInstanceHandle();
	WebHost->requestedPage.store(static_cast<int>(CurrentPage));
	CRect client;
	GetClientRect(&client);
	WebHost->clientWidth.store((std::max)(0, client.Width()));
	WebHost->clientHeight.store((std::max)(0, client.Height()));

	try
	{
		WebViewThread = std::thread(
			&CVsmrControlCenterDialog::WebViewThreadMain,
			this);
	}
	catch (const std::exception& ex)
	{
		ShowFallback(
			"Unable to start the WebView2 UI thread: " +
			std::string(ex.what()));
	}
}

bool CVsmrControlCenterDialog::AcquireWebViewHostWindowClass()
{
	if (!WebHost || WebHost->windowClassAcquired)
		return WebHost != nullptr;

	std::lock_guard<std::mutex> lock(gWebViewHostWindowClassMutex);
	if (gWebViewHostWindowClassUsers == 0)
	{
		WNDCLASSEXW windowClass = {};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.style = CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc =
			&CVsmrControlCenterDialog::WebViewThreadWindowProc;
		windowClass.hInstance = WebHost->moduleInstance;
		windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
		windowClass.hbrBackground =
			reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		windowClass.lpszClassName = kWebViewHostWindowClass;
		if (::RegisterClassExW(&windowClass) == 0)
		{
			if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return false;
			WNDCLASSEXW existingClass = {};
			existingClass.cbSize = sizeof(existingClass);
			if (!::GetClassInfoExW(
					WebHost->moduleInstance,
					kWebViewHostWindowClass,
					&existingClass) ||
				existingClass.lpfnWndProc !=
					&CVsmrControlCenterDialog::WebViewThreadWindowProc)
			{
				return false;
			}
		}
		gWebViewHostWindowClassInstance = WebHost->moduleInstance;
	}
	else if (gWebViewHostWindowClassInstance != WebHost->moduleInstance)
	{
		return false;
	}

	++gWebViewHostWindowClassUsers;
	WebHost->windowClassAcquired = true;
	return true;
}

void CVsmrControlCenterDialog::ReleaseWebViewHostWindowClass()
{
	if (!WebHost || !WebHost->windowClassAcquired)
		return;

	std::lock_guard<std::mutex> lock(gWebViewHostWindowClassMutex);
	WebHost->windowClassAcquired = false;
	if (gWebViewHostWindowClassUsers == 0)
		return;
	--gWebViewHostWindowClassUsers;
	if (gWebViewHostWindowClassUsers != 0)
		return;

	if (::UnregisterClassW(
		kWebViewHostWindowClass,
		gWebViewHostWindowClassInstance))
	{
		gWebViewHostWindowClassInstance = nullptr;
	}
	else
	{
		Logger::info(
			"Control Center WebView2: unable to unregister the STA host window class (error " +
			std::to_string(::GetLastError()) + ").");
	}
}

void CVsmrControlCenterDialog::WebViewThreadMain()
{
	const HRESULT comResult = ::CoInitializeEx(
		nullptr,
		COINIT_APARTMENTTHREADED);
	if (FAILED(comResult))
	{
		ShowFallback(
			"Unable to initialize the dedicated WebView2 STA thread (HRESULT " +
			std::to_string(static_cast<unsigned long>(comResult)) + ").");
		return;
	}
	WebHost->comInitialized = true;

	if (!AcquireWebViewHostWindowClass())
	{
		ShowFallback("Unable to register the WebView2 host window class.");
		ShutdownWebView();
		return;
	}

	const int width = WebHost->clientWidth.load();
	const int height = WebHost->clientHeight.load();
	const HWND threadWindow = ::CreateWindowExW(
		WS_EX_NOPARENTNOTIFY,
		kWebViewHostWindowClass,
		L"",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0,
		0,
		width,
		height,
		WebHost->dialogWindow,
		nullptr,
		WebHost->moduleInstance,
		this);
	if (threadWindow == nullptr)
	{
		ShowFallback("Unable to create the WebView2 STA host window.");
		ShutdownWebView();
		ReleaseWebViewHostWindowClass();
		return;
	}
	WebHost->threadWindow.store(threadWindow);
	if (WebHost->stopRequested.load())
	{
		ShutdownWebView();
		::DestroyWindow(threadWindow);
		WebHost->threadWindow.store(nullptr);
		ReleaseWebViewHostWindowClass();
		return;
	}

	std::weak_ptr<std::atomic<bool>> weakLifetime = LifetimeToken;
	WebHost->environmentCreationPending = true;
	const HRESULT createResult = ::CreateCoreWebView2EnvironmentWithOptions(
		nullptr,
		WebHost->userDataFolder.c_str(),
		nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this, weakLifetime](
				HRESULT result,
				ICoreWebView2Environment* environment) -> HRESULT
			{
				WebHost->environmentCreationPending = false;
				const auto lifetime = weakLifetime.lock();
				HRESULT callbackResult = S_OK;
				if (lifetime && lifetime->load() &&
					!WebHost->stopRequested.load())
				{
					callbackResult = OnWebViewEnvironmentCreated(
						result,
						environment);
				}
				MaybeStopWebViewThreadOnStaThread();
				return callbackResult;
			}).Get());
	if (FAILED(createResult))
	{
		WebHost->environmentCreationPending = false;
		ShowFallback(
			"Unable to start WebView2. Install the x86 Microsoft Edge WebView2 Evergreen Runtime.");
	}
	MaybeStopWebViewThreadOnStaThread();

	MSG message = {};
	while (::GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		::TranslateMessage(&message);
		::DispatchMessageW(&message);
	}

	ShutdownWebView();
	if (::IsWindow(threadWindow))
		::DestroyWindow(threadWindow);
	WebHost->threadWindow.store(nullptr);
	ReleaseWebViewHostWindowClass();
}

LRESULT CALLBACK CVsmrControlCenterDialog::WebViewThreadWindowProc(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam)
{
	CVsmrControlCenterDialog* dialog = reinterpret_cast<CVsmrControlCenterDialog*>(
		::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE)
	{
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		dialog = create != nullptr
			? static_cast<CVsmrControlCenterDialog*>(create->lpCreateParams)
			: nullptr;
		::SetWindowLongPtrW(
			window,
			GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(dialog));
	}

	if (dialog != nullptr)
	{
		switch (message)
		{
		case kWebViewResizeMessage:
			dialog->ResizeWebViewOnStaThread();
			return 0;
		case kWebViewSendJsonMessage:
			dialog->SendQueuedJsonOnStaThread();
			return 0;
		case kWebViewShowPageMessage:
			dialog->ShowRequestedPageOnStaThread();
			return 0;
		case kWebViewParentMovedMessage:
			dialog->NotifyWebViewParentMovedOnStaThread();
			return 0;
		case kWebViewShutdownMessage:
			::ShowWindow(window, SW_HIDE);
			dialog->MaybeStopWebViewThreadOnStaThread();
			return 0;
		case kWebViewBeginWindowDragMessage:
		{
			::ReleaseCapture();
			POINT cursor = {};
			::GetCursorPos(&cursor);
			if (::IsWindow(dialog->WebHost->dialogWindow))
			{
				::PostMessage(
					dialog->WebHost->dialogWindow,
					WM_NCLBUTTONDOWN,
					HTCAPTION,
					MAKELPARAM(cursor.x, cursor.y));
			}
			return 0;
		}
		case kWebViewPageReadyMessage:
			dialog->CompleteWebViewPageReadyOnStaThread();
			return 0;
		case WM_DESTROY:
			dialog->WebHost->stopRequested.store(true);
			dialog->MaybeStopWebViewThreadOnStaThread();
			return 0;
		default:
			break;
		}
	}

	if (message == WM_NCDESTROY)
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	return ::DefWindowProcW(window, message, wParam, lParam);
}

void CVsmrControlCenterDialog::MaybeStopWebViewThreadOnStaThread()
{
	if (WebHost && WebHost->stopRequested.load() &&
		!WebHost->environmentCreationPending &&
		!WebHost->controllerCreationPending)
	{
		::PostQuitMessage(0);
	}
}

HRESULT CVsmrControlCenterDialog::OnWebViewEnvironmentCreated(
	HRESULT result,
	IUnknown* environmentUnknown)
{
	if (FAILED(result) || environmentUnknown == nullptr)
	{
		ShowFallback(
			"Microsoft Edge WebView2 Runtime is unavailable. Install the x86 Evergreen Runtime and reopen the Control Center.");
		return S_OK;
	}

	ComPtr<ICoreWebView2Environment> environment;
	const HRESULT queryResult = environmentUnknown->QueryInterface(
		IID_PPV_ARGS(&environment));
	if (FAILED(queryResult) || !environment)
	{
		ShowFallback("WebView2 returned an unsupported environment.");
		return S_OK;
	}
	WebHost->environment = environment;

	std::weak_ptr<std::atomic<bool>> weakLifetime = LifetimeToken;
	WebHost->controllerCreationPending = true;
	const HRESULT createResult =
		WebHost->environment->CreateCoreWebView2Controller(
		WebHost->threadWindow.load(),
		Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
			[this, weakLifetime](
				HRESULT controllerResult,
				ICoreWebView2Controller* controller) -> HRESULT
			{
				WebHost->controllerCreationPending = false;
				const auto lifetime = weakLifetime.lock();
				HRESULT callbackResult = S_OK;
				if (lifetime && lifetime->load() &&
					!WebHost->stopRequested.load())
				{
					callbackResult = OnWebViewControllerCreated(
						controllerResult,
						controller);
				}
				MaybeStopWebViewThreadOnStaThread();
				return callbackResult;
			}).Get());
	if (FAILED(createResult))
	{
		WebHost->controllerCreationPending = false;
		ShowFallback("Unable to start the WebView2 controller.");
	}
	return createResult;
}

HRESULT CVsmrControlCenterDialog::OnWebViewControllerCreated(
	HRESULT result,
	IUnknown* controllerUnknown)
{
	if (FAILED(result) || controllerUnknown == nullptr)
	{
		ShowFallback("Unable to create the WebView2 controller.");
		return S_OK;
	}

	ComPtr<ICoreWebView2Controller> controller;
	const HRESULT queryResult = controllerUnknown->QueryInterface(
		IID_PPV_ARGS(&controller));
	if (FAILED(queryResult) || !controller)
	{
		ShowFallback("WebView2 returned an unsupported controller.");
		return S_OK;
	}
	WebHost->controller = controller;
	if (FAILED(WebHost->controller->get_CoreWebView2(&WebHost->webView)) ||
		!WebHost->webView)
	{
		ShowFallback("Unable to create the WebView2 browser instance.");
		return S_OK;
	}

	ConfigureWebView();
	return S_OK;
}

void CVsmrControlCenterDialog::ConfigureWebView()
{
	if (!WebHost->webView || !WebHost->controller)
		return;

	ComPtr<ICoreWebView2Settings> settings;
	if (SUCCEEDED(WebHost->webView->get_Settings(&settings)) && settings)
	{
		settings->put_IsScriptEnabled(TRUE);
		settings->put_IsWebMessageEnabled(TRUE);
		settings->put_AreDefaultContextMenusEnabled(FALSE);
		settings->put_AreDevToolsEnabled(FALSE);
		settings->put_IsStatusBarEnabled(FALSE);
		settings->put_IsZoomControlEnabled(FALSE);
		settings->put_AreHostObjectsAllowed(FALSE);

		ComPtr<ICoreWebView2Settings3> settings3;
		if (SUCCEEDED(settings.As(&settings3)) && settings3)
			settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
	}

	ComPtr<ICoreWebView2Controller4> controller4;
	if (SUCCEEDED(WebHost->controller.As(&controller4)) && controller4)
		controller4->put_AllowExternalDrop(FALSE);

	ComPtr<ICoreWebView2_3> webView3;
	if (FAILED(WebHost->webView.As(&webView3)) || !webView3)
	{
		ShowFallback(
			"The installed WebView2 Runtime does not support local resource mapping.");
		return;
	}

	if (FAILED(webView3->SetVirtualHostNameToFolderMapping(
		kVirtualHostName,
		WebHost->resourceFolder.c_str(),
		COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
	{
		ShowFallback("Unable to map the bundled vSMR web resources.");
		return;
	}

	std::weak_ptr<std::atomic<bool>> weakLifetime = LifetimeToken;
	if (SUCCEEDED(WebHost->webView->add_NavigationStarting(
		Callback<ICoreWebView2NavigationStartingEventHandler>(
			[weakLifetime](
				ICoreWebView2*,
				ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
			{
				const auto lifetime = weakLifetime.lock();
				if (!lifetime || !lifetime->load() || args == nullptr)
					return S_OK;
				LPWSTR rawUri = nullptr;
				if (SUCCEEDED(args->get_Uri(&rawUri)) && rawUri != nullptr)
				{
					const bool allowed = IsAllowedNavigation(rawUri);
					::CoTaskMemFree(rawUri);
					if (!allowed)
						args->put_Cancel(TRUE);
				}
				return S_OK;
			}).Get(),
		&WebHost->navigationStartingToken)))
	{
		WebHost->navigationStartingRegistered = true;
	}

	if (SUCCEEDED(WebHost->webView->add_NewWindowRequested(
		Callback<ICoreWebView2NewWindowRequestedEventHandler>(
			[weakLifetime](
				ICoreWebView2*,
				ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
			{
				const auto lifetime = weakLifetime.lock();
				if (lifetime && lifetime->load() && args != nullptr)
					args->put_Handled(TRUE);
				return S_OK;
			}).Get(),
		&WebHost->newWindowToken)))
	{
		WebHost->newWindowRegistered = true;
	}

	if (SUCCEEDED(WebHost->webView->add_PermissionRequested(
		Callback<ICoreWebView2PermissionRequestedEventHandler>(
			[weakLifetime](
				ICoreWebView2*,
				ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT
			{
				const auto lifetime = weakLifetime.lock();
				if (lifetime && lifetime->load() && args != nullptr)
					args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
				return S_OK;
			}).Get(),
		&WebHost->permissionToken)))
	{
		WebHost->permissionRegistered = true;
	}

	if (SUCCEEDED(WebHost->webView->add_WebMessageReceived(
		Callback<ICoreWebView2WebMessageReceivedEventHandler>(
			[this, weakLifetime](
				ICoreWebView2*,
				ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
			{
				const auto lifetime = weakLifetime.lock();
				if (!lifetime || !lifetime->load() || args == nullptr)
					return S_OK;
				LPWSTR rawJson = nullptr;
				if (SUCCEEDED(args->get_WebMessageAsJson(&rawJson)) &&
					rawJson != nullptr)
				{
					QueueWebMessageForDialog(
						WideToUtf8(std::wstring(rawJson)));
					::CoTaskMemFree(rawJson);
				}
				return S_OK;
			}).Get(),
		&WebHost->webMessageToken)))
	{
		WebHost->webMessageRegistered = true;
	}

	WebHost->controller->put_IsVisible(TRUE);
	ResizeWebViewOnStaThread();
	WebViewReady.store(true);

	std::wstring uri =
		std::wstring(kVirtualOriginPrefix) +
		L"index.html?ui=control&hosted=1&page=" +
		Utf8ToWide(PageName(static_cast<Page>(
			WebHost->requestedPage.load())));
	WebHost->webView->Navigate(uri.c_str());
}

void CVsmrControlCenterDialog::ResizeWebView()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;
	CRect client;
	GetClientRect(&client);
	if (WebHost)
	{
		WebHost->clientWidth.store((std::max)(0, client.Width()));
		WebHost->clientHeight.store((std::max)(0, client.Height()));
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
			::PostMessage(threadWindow, kWebViewResizeMessage, 0, 0);
	}
	if (::IsWindow(FallbackLabel.GetSafeHwnd()))
		FallbackLabel.MoveWindow(client, TRUE);
}

void CVsmrControlCenterDialog::ResizeWebViewOnStaThread()
{
	if (!WebHost)
		return;
	const HWND threadWindow = WebHost->threadWindow.load();
	if (!::IsWindow(threadWindow))
		return;
	const int width = (std::max)(0, WebHost->clientWidth.load());
	const int height = (std::max)(0, WebHost->clientHeight.load());
	::SetWindowPos(
		threadWindow,
		nullptr,
		0,
		0,
		width,
		height,
		SWP_NOACTIVATE | SWP_NOZORDER);
	if (WebHost->controller)
	{
		const RECT bounds = { 0, 0, width, height };
		WebHost->controller->put_Bounds(bounds);
	}
}

void CVsmrControlCenterDialog::NotifyWebViewParentMovedOnStaThread()
{
	if (WebHost && WebHost->controller)
		WebHost->controller->NotifyParentWindowPositionChanged();
}

void CVsmrControlCenterDialog::SendQueuedJsonOnStaThread()
{
	if (!WebHost || !WebHost->webView || !WebViewReady.load() ||
		!WebHost->pageReady.load())
		return;
	std::deque<std::string> pending;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		pending.swap(WebHost->outboundJson);
	}
	for (const std::string& json : pending)
	{
		if (json.empty())
			continue;
		const std::wstring wideJson = Utf8ToWide(json);
		WebHost->webView->PostWebMessageAsJson(wideJson.c_str());
	}
}

void CVsmrControlCenterDialog::ShowRequestedPageOnStaThread()
{
	if (!WebHost || !WebHost->webView || !WebViewReady.load() ||
		!WebHost->pageReady.load())
		return;
	const Page page = static_cast<Page>(WebHost->requestedPage.load());
	const std::wstring script =
		L"window.vsmrControlCenter && window.vsmrControlCenter.open(\"" +
		Utf8ToWide(PageName(page)) +
		L"\");";
	WebHost->webView->ExecuteScript(script.c_str(), nullptr);
}

void CVsmrControlCenterDialog::CompleteWebViewPageReadyOnStaThread()
{
	if (!WebHost || !WebHost->pageReady.load() || !WebViewReady.load())
		return;
	SendQueuedJsonOnStaThread();
	ShowRequestedPageOnStaThread();
	const HWND threadWindow = WebHost->threadWindow.load();
	if (::IsWindow(threadWindow))
		::ShowWindow(threadWindow, SW_SHOWNA);
	if (LifetimeToken && LifetimeToken->load() &&
		::IsWindow(WebHost->dialogWindow))
	{
		::PostMessage(
			WebHost->dialogWindow,
			kWebViewReadyMessage,
			0,
			0);
	}
}

void CVsmrControlCenterDialog::ShutdownWebView()
{
	if (!WebHost)
		return;

	if (WebHost->webView)
	{
		if (WebHost->navigationStartingRegistered)
			WebHost->webView->remove_NavigationStarting(
				WebHost->navigationStartingToken);
		if (WebHost->newWindowRegistered)
			WebHost->webView->remove_NewWindowRequested(
				WebHost->newWindowToken);
		if (WebHost->webMessageRegistered)
			WebHost->webView->remove_WebMessageReceived(
				WebHost->webMessageToken);
		if (WebHost->permissionRegistered)
			WebHost->webView->remove_PermissionRequested(
				WebHost->permissionToken);
	}
	if (WebHost->controller)
		WebHost->controller->Close();
	WebHost->webView.Reset();
	WebHost->controller.Reset();
	WebHost->environment.Reset();
	if (WebHost->comInitialized)
	{
		::CoUninitialize();
		WebHost->comInitialized = false;
	}
	WebViewReady.store(false);
	WebHost->pageReady.store(false);
}

void CVsmrControlCenterDialog::StopWebViewThread()
{
	if (!WebViewThread.joinable())
		return;
	if (WebHost)
	{
		WebHost->stopRequested.store(true);
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
			::PostMessage(threadWindow, kWebViewShutdownMessage, 0, 0);
	}
	WebViewThread.join();
}

void CVsmrControlCenterDialog::ShowFallback(const std::string& message)
{
	if (WebHost && WebHost->dialogWindow != nullptr &&
		::GetCurrentThreadId() !=
			::GetWindowThreadProcessId(WebHost->dialogWindow, nullptr))
	{
		const auto lifetime = LifetimeToken;
		if (!lifetime || !lifetime->load())
			return;
		{
			std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
			WebHost->fallbackMessages.push_back(message);
		}
		if (::IsWindow(WebHost->dialogWindow))
			::PostMessage(WebHost->dialogWindow, kWebViewFallbackMessage, 0, 0);
		return;
	}

	Logger::info("Control Center WebView2: " + message);
	if (::IsWindow(FallbackLabel.GetSafeHwnd()))
	{
		FallbackLabel.SetWindowTextA(message.c_str());
		FallbackLabel.ShowWindow(SW_SHOW);
		ResizeWebView();
	}
}

void CVsmrControlCenterDialog::QueueWebMessageForDialog(
	const std::string& json)
{
	if (!WebHost || json.empty())
		return;
	const auto lifetime = LifetimeToken;
	if (!lifetime || !lifetime->load())
		return;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		WebHost->inboundJson.push_back(json);
	}
	if (::IsWindow(WebHost->dialogWindow))
		::PostMessage(
			WebHost->dialogWindow,
			kWebViewMessageReceivedMessage,
			0,
			0);
}

void CVsmrControlCenterDialog::SendJsonToWebView(const std::string& json)
{
	if (!WebHost || json.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		WebHost->outboundJson.push_back(json);
	}
	const HWND threadWindow = WebHost->threadWindow.load();
	if (::IsWindow(threadWindow))
		::PostMessage(threadWindow, kWebViewSendJsonMessage, 0, 0);
}

void CVsmrControlCenterDialog::BeginNativeWindowDrag()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;
	if (WebHost)
	{
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
		{
			::PostMessage(
				threadWindow,
				kWebViewBeginWindowDragMessage,
				0,
				0);
			return;
		}
	}
	::ReleaseCapture();
	SendMessage(WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void CVsmrControlCenterDialog::RequestComputerResource(
	const std::string& resource,
	const std::string& requestId)
{
	const bool profiles = resource == "profiles";
	CFileDialog dialog(
		TRUE,
		profiles ? "json" : "geojson",
		nullptr,
		OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY,
		profiles
			? "vSMR profiles (*.json)|*.json|All files (*.*)|*.*||"
			: "GeoJSON (*.geojson;*.json)|*.geojson;*.json|All files (*.*)|*.*||",
		this);
	if (dialog.DoModal() != IDOK)
		return;

	const std::filesystem::path path(
		static_cast<LPCSTR>(dialog.GetPathName()));
	std::string text;
	if (!ReadTextFile(path, text))
	{
		if (Bridge)
			Bridge->PushError(requestId, "Unable to read the selected file.");
		return;
	}
	if (Bridge)
		Bridge->HandleLoadedResource(
			resource,
			path.string(),
			requestId,
			text);
}

void CVsmrControlCenterDialog::RequestResetDefaults(
	const std::string& requestId)
{
	const std::filesystem::path resourceFolder(ResolveWebResourceFolder());
	const std::filesystem::path profilesPath =
		resourceFolder / L"defaults" / L"vSMR_Profiles.json";
	const std::filesystem::path avisoPath =
		resourceFolder / L"defaults" / L"AVISO_LFPG.geojson";

	std::string profilesText;
	std::string avisoText;
	if (!ReadTextFile(profilesPath, profilesText) ||
		!ReadTextFile(avisoPath, avisoText))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"The bundled profile or LFPG AVISO defaults are missing.");
		return;
	}

	if (!Bridge)
		return;

	std::string validationError;
	if (!Bridge->ValidateLoadedResource(
			"profiles",
			profilesText,
			validationError) ||
		!Bridge->ValidateLoadedResource(
			"aviso",
			avisoText,
			validationError))
	{
		Bridge->PushError(
			requestId,
			validationError.empty()
				? "The bundled profile or LFPG AVISO defaults are invalid."
				: validationError);
		return;
	}

	Bridge->HandleLoadedResource(
		"profiles",
		"bundled defaults",
		requestId,
		profilesText);
	Bridge->HandleLoadedResource(
		"aviso",
		"bundled defaults",
		requestId,
		avisoText);
}

void CVsmrControlCenterDialog::RequestGithubResource(
	const std::string& resource,
	const std::string& url,
	const std::string& requestId)
{
	if (GithubDownloadInProgress.exchange(true))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"Another GitHub resource is still loading.");
		return;
	}
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();

	const HWND target = GetSafeHwnd();
	std::weak_ptr<std::atomic<bool>> weakLifetime = LifetimeToken;
	GithubDownloadThread = std::thread(
		[target, weakLifetime, resource, url, requestId]()
		{
			HttpHelper helper;
			auto result = std::make_unique<GithubDownloadResult>();
			result->resource = resource;
			result->source = url;
			result->requestId = requestId;
			result->body = helper.downloadStringFromURL(url);

			const auto lifetime = weakLifetime.lock();
			if (!lifetime || !lifetime->load() || !::IsWindow(target))
				return;
			GithubDownloadResult* raw = result.release();
			if (!::PostMessage(
				target,
				kGithubDownloadCompleteMessage,
				0,
				reinterpret_cast<LPARAM>(raw)))
				delete raw;
		});
}

LRESULT CVsmrControlCenterDialog::OnGithubDownloadComplete(
	WPARAM,
	LPARAM lParam)
{
	std::unique_ptr<GithubDownloadResult> result(
		reinterpret_cast<GithubDownloadResult*>(lParam));
	GithubDownloadInProgress.store(false);
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();
	if (!result)
		return 0;
	if (result->body.empty())
	{
		if (Bridge)
			Bridge->PushError(
				result->requestId,
				"GitHub download failed or returned an empty file.");
		return 0;
	}
	if (Bridge)
		Bridge->HandleLoadedResource(
			result->resource,
			result->source,
			result->requestId,
			result->body);
	return 0;
}

LRESULT CVsmrControlCenterDialog::OnWebViewMessageReceived(WPARAM, LPARAM)
{
	if (!WebHost)
		return 0;
	std::deque<std::string> pending;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		pending.swap(WebHost->inboundJson);
	}
	bool uiReady = false;
	for (const std::string& json : pending)
	{
		rapidjson::Document message;
		message.Parse<0>(json.c_str());
		uiReady = uiReady ||
			(!message.HasParseError() &&
				message.IsObject() &&
				message.HasMember("type") &&
				message["type"].IsString() &&
				std::strcmp(message["type"].GetString(), "ui.ready") == 0);
		if (Bridge && LifetimeToken && LifetimeToken->load())
			Bridge->HandleWebMessage(json);
	}
	if (uiReady)
	{
		WebHost->pageReady.store(true);
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
			::PostMessage(threadWindow, kWebViewPageReadyMessage, 0, 0);
	}
	return 0;
}

LRESULT CVsmrControlCenterDialog::OnWebViewFallback(WPARAM, LPARAM)
{
	if (!WebHost)
		return 0;
	std::deque<std::string> pending;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		pending.swap(WebHost->fallbackMessages);
	}
	for (const std::string& message : pending)
		ShowFallback(message);
	return 0;
}

LRESULT CVsmrControlCenterDialog::OnWebViewReady(WPARAM, LPARAM)
{
	if (WebViewReady.load() && ::IsWindow(FallbackLabel.GetSafeHwnd()))
		FallbackLabel.ShowWindow(SW_HIDE);
	return 0;
}

void CVsmrControlCenterDialog::ShowPage(Page page)
{
	CurrentPage = page;
	if (!WebHost)
		return;
	WebHost->requestedPage.store(static_cast<int>(page));
	const HWND threadWindow = WebHost->threadWindow.load();
	if (WebViewReady.load() && ::IsWindow(threadWindow))
		::PostMessage(threadWindow, kWebViewShowPageMessage, 0, 0);
}

void CVsmrControlCenterDialog::SyncFromRadar()
{
	if (Bridge && WebViewReady.load())
		Bridge->PushAuthoritativeState("runtime");
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

std::wstring CVsmrControlCenterDialog::ResolveWebResourceFolder() const
{
	std::vector<std::filesystem::path> candidates;
	if (Owner != nullptr)
	{
		candidates.emplace_back(
			std::filesystem::path(Owner->DllPath) / "vSMR_webUI");
		candidates.emplace_back(
			std::filesystem::path(Owner->DataPath) / "vSMR_webUI");
	}
	try
	{
		candidates.emplace_back(
			std::filesystem::current_path() / "vSMR_webUI");
	}
	catch (...)
	{
	}

	for (const std::filesystem::path& candidate : candidates)
	{
		try
		{
			if (std::filesystem::is_regular_file(candidate / "index.html") &&
				std::filesystem::is_regular_file(candidate / "styles.css") &&
				std::filesystem::is_regular_file(candidate / "app.js"))
				return std::filesystem::absolute(candidate).wstring();
		}
		catch (...)
		{
		}
	}
	return {};
}

std::wstring CVsmrControlCenterDialog::WebViewUserDataFolder() const
{
	return (
		std::filesystem::path(LocalAppDataPath()) /
		"vSMR" /
		"WebView2").wstring();
}

std::wstring CVsmrControlCenterDialog::WindowPlacementPath() const
{
	return (
		std::filesystem::path(LocalAppDataPath()) /
		"vSMR" /
		"control-center-window.json").wstring();
}

void CVsmrControlCenterDialog::RestoreWindowPlacementOrDefault(
	const CRect& fallback)
{
	CRect requested = fallback;
	std::string text;
	const std::filesystem::path path(WindowPlacementPath());
	if (ReadTextFile(path, text))
	{
		rapidjson::Document document;
		document.Parse<0>(text.c_str());
		if (!document.HasParseError() && document.IsObject())
		{
			auto readInt = [&](const char* key, int fallbackValue)
			{
				return document.HasMember(key) && document[key].IsInt()
					? document[key].GetInt()
					: fallbackValue;
			};
			const int width = readInt("width", fallback.Width());
			const int height = readInt("height", fallback.Height());
			requested.left = readInt("x", fallback.left);
			requested.top = readInt("y", fallback.top);
			requested.right = requested.left + width;
			requested.bottom = requested.top + height;
		}
	}

	requested = ClampWindowRectToMonitor(requested);
	SetWindowPos(
		nullptr,
		requested.left,
		requested.top,
		requested.Width(),
		requested.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE);
	WindowPlacementDirty = false;
}

void CVsmrControlCenterDialog::SaveWindowPlacement()
{
	if (!::IsWindow(GetSafeHwnd()))
		return;
	CRect window;
	GetWindowRect(&window);
	if (window.IsRectEmpty())
		return;

	rapidjson::Document document;
	document.SetObject();
	document.AddMember("x", window.left, document.GetAllocator());
	document.AddMember("y", window.top, document.GetAllocator());
	document.AddMember("width", window.Width(), document.GetAllocator());
	document.AddMember("height", window.Height(), document.GetAllocator());
	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	document.Accept(writer);
	if (WriteTextFileAtomically(
		std::filesystem::path(WindowPlacementPath()),
		std::string(buffer.GetString(), buffer.Size())))
		WindowPlacementDirty = false;
}

void CVsmrControlCenterDialog::OnClose()
{
	SaveWindowPlacement();
	ShowWindow(SW_HIDE);
	if (Owner != nullptr)
		Owner->OnVsmrControlCenterWindowClosed();
}

void CVsmrControlCenterDialog::OnCancel()
{
	OnClose();
}

void CVsmrControlCenterDialog::OnOK()
{
	// Enter belongs to the focused editor, not to the modeless host dialog.
}

void CVsmrControlCenterDialog::OnDestroy()
{
	Closing = true;
	if (LifetimeToken)
		LifetimeToken->store(false);
	SaveWindowPlacement();
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();
	GithubDownloadInProgress.store(false);
	StopWebViewThread();
	Bridge.reset();
	CDialogEx::OnDestroy();
}

void CVsmrControlCenterDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (nType != SIZE_MINIMIZED)
	{
		WindowPlacementDirty = true;
		ResizeWebView();
	}
}

void CVsmrControlCenterDialog::OnMove(int x, int y)
{
	CDialogEx::OnMove(x, y);
	if (!Closing)
	{
		WindowPlacementDirty = true;
		if (WebHost)
		{
			const HWND threadWindow = WebHost->threadWindow.load();
			if (::IsWindow(threadWindow))
				::PostMessage(
					threadWindow,
					kWebViewParentMovedMessage,
					0,
					0);
		}
	}
}

void CVsmrControlCenterDialog::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);
	if (lpMMI != nullptr)
	{
		lpMMI->ptMinTrackSize.x = kMinimumWindowWidth;
		lpMMI->ptMinTrackSize.y = kMinimumWindowHeight;
	}
}
