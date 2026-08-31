#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterDialog.Internal.hpp"

#include "control_center/ControlCenterBridge.hpp"
#include "control_center/WebMessageValidation.hpp"
#include "crash/CrashRuntime.hpp"
#include "radar/RadarScreen.hpp"
#include "shared/logging/Logger.hpp"

#include "WebView2.h"
#include <wrl.h>

#include <cwchar>
#include <limits>
#include <new>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace VsmrControlCenterDialogInternal;

namespace
{
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

	struct CoTaskMemStringDeleter
	{
		void operator()(wchar_t* value) const noexcept
		{
			::CoTaskMemFree(value);
		}
	};

	using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemStringDeleter>;

	bool TryWideToUtf8Bounded(
		const wchar_t* value,
		size_t maximumBytes,
		std::string& output)
	{
		output.clear();
		if (value == nullptr || maximumBytes == 0)
			return false;

		size_t wideLength = 0;
		while (wideLength <= maximumBytes && value[wideLength] != L'\0')
			++wideLength;
		if (wideLength == 0 || wideLength > maximumBytes ||
			wideLength > static_cast<size_t>((std::numeric_limits<int>::max)()))
		{
			return false;
		}

		const int encodedLength = ::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			value,
			static_cast<int>(wideLength),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (encodedLength <= 0 ||
			static_cast<size_t>(encodedLength) > maximumBytes)
		{
			return false;
		}

		output.resize(static_cast<size_t>(encodedLength));
		const int convertedLength = ::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			value,
			static_cast<int>(wideLength),
			output.data(),
			encodedLength,
			nullptr,
			nullptr);
		if (convertedLength != encodedLength)
		{
			output.clear();
			return false;
		}
		return true;
	}

	bool EqualsCanonicalUri(
		const wchar_t* observed,
		const std::wstring& canonical) noexcept
	{
		return observed != nullptr && !canonical.empty() &&
			std::wcsncmp(observed, canonical.c_str(), canonical.size()) == 0 &&
			observed[canonical.size()] == L'\0';
	}

	bool IsCanonicalMainDocumentMessage(
		ICoreWebView2* webView,
		ICoreWebView2WebMessageReceivedEventArgs* args,
		const std::wstring& canonicalDocumentUri) noexcept
	{
		if (webView == nullptr || args == nullptr || canonicalDocumentUri.empty())
			return false;

		LPWSTR rawMessageSource = nullptr;
		if (FAILED(args->get_Source(&rawMessageSource)) ||
			rawMessageSource == nullptr)
		{
			return false;
		}
		CoTaskMemString messageSource(rawMessageSource);

		LPWSTR rawMainSource = nullptr;
		if (FAILED(webView->get_Source(&rawMainSource)) || rawMainSource == nullptr)
			return false;
		CoTaskMemString mainSource(rawMainSource);

		return EqualsCanonicalUri(messageSource.get(), canonicalDocumentUri) &&
			EqualsCanonicalUri(mainSource.get(), canonicalDocumentUri);
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
}

void CVsmrControlCenterDialog::InitializeWebView()
{
	const std::wstring resourceFolder = ResolveWebResourceFolder();
	if (resourceFolder.empty())
	{
		ShowFallback(
			"vSMR web resources were not found. Reinstall vSMR_Data\\vSMR_webUI next to vSMR.dll.");
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
	WebHost->stopRequested.store(false);
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
	catch (...)
	{
		ShowFallback("Unable to start the WebView2 UI thread.");
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
	VsmrCrashRuntime::OwnedThreadRole crashThreadRole("Control Center WebView worker");
	try
	{
		WebViewThreadMainImpl();
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Control Center WebView2 thread exception: " +
			std::string(ex.what()));
		ShowFallback("The Control Center browser stopped unexpectedly.");
		if (WebHost)
		{
			const HWND threadWindow = WebHost->threadWindow.load();
			ShutdownWebView();
			if (::IsWindow(threadWindow))
				::DestroyWindow(threadWindow);
			WebHost->threadWindow.store(nullptr);
			ReleaseWebViewHostWindowClass();
		}
	}
	catch (...)
	{
		Logger::info("Control Center WebView2 thread exception: unknown");
		ShowFallback("The Control Center browser stopped unexpectedly.");
		if (WebHost)
		{
			const HWND threadWindow = WebHost->threadWindow.load();
			ShutdownWebView();
			if (::IsWindow(threadWindow))
				::DestroyWindow(threadWindow);
			WebHost->threadWindow.store(nullptr);
			ReleaseWebViewHostWindowClass();
		}
	}
	if (WebHost)
		WebHost->threadId.store(0);
}

void CVsmrControlCenterDialog::WebViewThreadMainImpl()
{
	WebHost->threadId.store(::GetCurrentThreadId());
	MSG queueSeed = {};
	::PeekMessageW(&queueSeed, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

	// WebView2 must be created and driven from the same STA thread
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
				try
				{
					if (WebHost == nullptr)
						return E_UNEXPECTED;
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
				}
				catch (CException* exception)
				{
					if (exception != nullptr)
						exception->Delete();
					if (WebHost != nullptr)
						WebHost->environmentCreationPending = false;
					ReportWebViewCallbackFailure(
						"environment completion",
						"MFC exception");
				}
				catch (const std::exception& exception)
				{
					if (WebHost != nullptr)
						WebHost->environmentCreationPending = false;
					ReportWebViewCallbackFailure(
						"environment completion",
						exception.what());
				}
				catch (...)
				{
					if (WebHost != nullptr)
						WebHost->environmentCreationPending = false;
					ReportWebViewCallbackFailure(
						"environment completion");
				}
				try
				{
					MaybeStopWebViewThreadOnStaThread();
				}
				catch (...)
				{
				}
				return E_FAIL;
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
		try
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
		catch (CException* exception)
		{
			if (exception != nullptr)
				exception->Delete();
			dialog->ReportWebViewCallbackFailure(
				"STA window procedure",
				"MFC exception");
			return 0;
		}
		catch (const std::exception& exception)
		{
			dialog->ReportWebViewCallbackFailure(
				"STA window procedure",
				exception.what());
			return 0;
		}
		catch (...)
		{
			dialog->ReportWebViewCallbackFailure(
				"STA window procedure");
			return 0;
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
				try
				{
					if (WebHost == nullptr)
						return E_UNEXPECTED;
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
				}
				catch (CException* exception)
				{
					if (exception != nullptr)
						exception->Delete();
					if (WebHost != nullptr)
						WebHost->controllerCreationPending = false;
					ReportWebViewCallbackFailure(
						"controller completion",
						"MFC exception");
				}
				catch (const std::exception& exception)
				{
					if (WebHost != nullptr)
						WebHost->controllerCreationPending = false;
					ReportWebViewCallbackFailure(
						"controller completion",
						exception.what());
				}
				catch (...)
				{
					if (WebHost != nullptr)
						WebHost->controllerCreationPending = false;
					ReportWebViewCallbackFailure(
						"controller completion");
				}
				try
				{
					MaybeStopWebViewThreadOnStaThread();
				}
				catch (...)
				{
				}
				return E_FAIL;
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

	// Expose the same aircraft silhouettes used by the native radar renderer.
	// Mapping only the icon directory keeps the rest of vSMR_Data unavailable to
	// the WebView while allowing the Control Center to preview the real A320 PNG.
	if (Owner != nullptr && !Owner->GetIconsPath().empty())
	{
		try
		{
			const std::filesystem::path iconFolder =
				std::filesystem::absolute(std::filesystem::u8path(Owner->GetIconsPath()));
			if (std::filesystem::is_regular_file(iconFolder / "a320.png"))
			{
				const HRESULT iconMappingResult = webView3->SetVirtualHostNameToFolderMapping(
					kAircraftIconVirtualHostName,
					iconFolder.wstring().c_str(),
					COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
				if (FAILED(iconMappingResult))
				{
					Logger::info(
						"Control Center WebView2: unable to map aircraft icon resources (HRESULT " +
						std::to_string(static_cast<long>(iconMappingResult)) + ").");
				}
			}
		}
		catch (const std::exception& exception)
		{
			Logger::info(
				"Control Center WebView2: unable to resolve aircraft icon resources: " +
				std::string(exception.what()));
		}
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
				ICoreWebView2* webView,
				ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
			{
				try
				{
					const auto lifetime = weakLifetime.lock();
					if (!lifetime || !lifetime->load() || args == nullptr ||
						WebHost == nullptr)
					{
						return S_OK;
					}
					if (!IsCanonicalMainDocumentMessage(
						webView,
						args,
						WebHost->canonicalDocumentUri))
					{
						LogRejectedWebMessage("non-canonical document source");
						return S_OK;
					}

					LPWSTR rawJson = nullptr;
					if (FAILED(args->get_WebMessageAsJson(&rawJson)) ||
						rawJson == nullptr)
					{
						LogRejectedWebMessage("message body unavailable");
						return S_OK;
					}
					CoTaskMemString ownedJson(rawJson);
					std::string json;
					if (!TryWideToUtf8Bounded(
						ownedJson.get(),
						VsmrWebMessageValidation::MaximumInboundMessageBytes,
						json))
					{
						LogRejectedWebMessage("invalid encoding or message size");
						return S_OK;
					}
					if (!VsmrWebMessageValidation::HasValidInboundWebMessageShape(json))
					{
						LogRejectedWebMessage("invalid JSON envelope");
						return S_OK;
					}
					QueueWebMessageForDialog(std::move(json));
				}
				catch (const std::exception&)
				{
					LogRejectedWebMessage("validation exception");
				}
				catch (...)
				{
					LogRejectedWebMessage("unexpected validation failure");
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
	WebHost->canonicalDocumentUri = uri;
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
		WebHost->outboundJsonBytes = 0;
		WebHost->outboundNotificationPending = false;
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
	const bool preservePage = WebHost->preservePageOnNextOpen.exchange(false);
	const Page page = static_cast<Page>(WebHost->requestedPage.load());
	const std::wstring script = preservePage
		? L"window.vsmrControlCenter && window.vsmrControlCenter.open();"
		: L"window.vsmrControlCenter && window.vsmrControlCenter.open(\"" +
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
	WebHost->canonicalDocumentUri.clear();
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
	DWORD threadId = 0;
	if (WebHost)
	{
		WebHost->stopRequested.store(true);
		const HWND threadWindow = WebHost->threadWindow.load();
		if (::IsWindow(threadWindow))
			::PostMessage(threadWindow, kWebViewShutdownMessage, 0, 0);
		threadId = WebHost->threadId.load();
	}
	const HANDLE threadHandle = WebViewThread.native_handle();
	if (::WaitForSingleObject(threadHandle, 3000) == WAIT_TIMEOUT)
	{
		Logger::info(
			"Control Center WebView2 creation is still pending after 3 seconds; requesting COM/I/O cancellation while keeping the STA message pump alive.");
		if (WebHost)
			threadId = WebHost->threadId.load();
		if (threadId != 0)
		{
			::CoCancelCall(threadId, 0);
			::CancelSynchronousIo(threadHandle);
			const HWND threadWindow = WebHost != nullptr
				? WebHost->threadWindow.load()
				: nullptr;
			if (::IsWindow(threadWindow))
				::PostMessage(threadWindow, kWebViewShutdownMessage, 0, 0);
		}
		if (::WaitForSingleObject(threadHandle, 3000) == WAIT_TIMEOUT)
		{
			Logger::info(
				"Control Center WebView2 thread is still stopping after cancellation; waiting for its outstanding callback to preserve safe DLL ownership.");
		}
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

bool CVsmrControlCenterDialog::QueueWebMessageForDialog(
	std::string json)
{
	if (!WebHost || json.empty() ||
		json.size() > VsmrWebMessageValidation::MaximumInboundMessageBytes)
	{
		return false;
	}
	const auto lifetime = LifetimeToken;
	if (!lifetime || !lifetime->load())
		return false;

	bool queueLimitReached = false;
	bool shouldNotifyDialog = false;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		queueLimitReached =
			WebHost->inboundJson.size() >= kMaximumQueuedInboundWebMessages ||
			WebHost->inboundJsonBytes >
				kMaximumQueuedInboundWebMessageBytes - json.size();
		if (!queueLimitReached)
		{
			const size_t jsonBytes = json.size();
			WebHost->inboundJson.push_back(std::move(json));
			WebHost->inboundJsonBytes += jsonBytes;
			shouldNotifyDialog = !WebHost->inboundNotificationPending;
			WebHost->inboundNotificationPending = true;
		}
	}
	if (queueLimitReached)
	{
		LogRejectedWebMessage("inbound queue limit reached");
		return false;
	}
	if (!shouldNotifyDialog)
		return true;

	if (!::IsWindow(WebHost->dialogWindow) ||
		!::PostMessage(
			WebHost->dialogWindow,
			kWebViewMessageReceivedMessage,
			0,
			0))
	{
		// The dialog can disappear between validation and notification
		{
			std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
			WebHost->inboundJson.clear();
			WebHost->inboundJsonBytes = 0;
			WebHost->inboundNotificationPending = false;
		}
		LogRejectedWebMessage("dialog notification failed");
		return false;
	}
	return true;
}

void CVsmrControlCenterDialog::LogRejectedWebMessage(
	const char* reason) noexcept
{
	if (!WebHost)
		return;
	const unsigned long rejected =
		WebHost->rejectedInboundMessages.fetch_add(1) + 1;
	// Log the first failure and powers of two to avoid a log-flood path
	if (rejected != 1 && (rejected & (rejected - 1)) != 0)
		return;
	try
	{
		Logger::info(
			"Control Center WebView2 rejected inbound message reason=" +
			std::string(reason != nullptr ? reason : "unknown") +
			" total=" + std::to_string(rejected));
	}
	catch (...)
	{
	}
}

void CVsmrControlCenterDialog::LogOutboundQueueIssue(
	const char* reason) noexcept
{
	if (!WebHost)
		return;
	const unsigned long issueCount =
		WebHost->outboundQueueIssues.fetch_add(1) + 1;
	// Log the first failure and powers of two to avoid a log-flood path
	if (issueCount != 1 && (issueCount & (issueCount - 1)) != 0)
		return;
	try
	{
		Logger::info(
			"Control Center WebView2 outbound queue issue reason=" +
			std::string(reason != nullptr ? reason : "unknown") +
			" total=" + std::to_string(issueCount));
	}
	catch (...)
	{
	}
}

void CVsmrControlCenterDialog::ReportWebViewCallbackFailure(
	const char* callback,
	const char* detail) noexcept
{
	try
	{
		Logger::info(
			"Control Center WebView2 callback failed callback=" +
			std::string(callback != nullptr ? callback : "unknown") +
			" reason=" +
			std::string(detail != nullptr ? detail : "unknown"));
	}
	catch (...)
	{
	}

	try
	{
		ShowFallback("The Control Center browser operation failed.");
	}
	catch (...)
	{
	}
}

void CVsmrControlCenterDialog::SendJsonToWebView(
	const std::string& json) noexcept
{
	if (!WebHost || json.empty())
		return;
	if (json.size() > kMaximumQueuedOutboundWebMessageBytes)
	{
		LogOutboundQueueIssue("message exceeds queue byte limit");
		return;
	}

	bool queueLimitReached = false;
	bool shouldNotifyStaThread = false;
	try
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		queueLimitReached =
			WebHost->outboundJson.size() >=
				kMaximumQueuedOutboundWebMessages ||
			WebHost->outboundJsonBytes >
				kMaximumQueuedOutboundWebMessageBytes - json.size();
		if (!queueLimitReached)
		{
			WebHost->outboundJson.push_back(json);
			WebHost->outboundJsonBytes += json.size();
			shouldNotifyStaThread =
				!WebHost->outboundNotificationPending;
			WebHost->outboundNotificationPending = true;
		}
	}
	catch (...)
	{
		LogOutboundQueueIssue("queue operation failed");
		return;
	}

	if (queueLimitReached)
	{
		LogOutboundQueueIssue("queue limit reached");
		return;
	}
	if (!shouldNotifyStaThread)
		return;

	const HWND threadWindow = WebHost->threadWindow.load();
	if (!::IsWindow(threadWindow))
	{
		// Startup may not have created the STA window yet. Page-ready handling
		// drains this bounded queue after the WebView has been initialized.
		return;
	}
	if (!::PostMessage(threadWindow, kWebViewSendJsonMessage, 0, 0))
	{
		try
		{
			std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
			WebHost->outboundNotificationPending = false;
		}
		catch (...)
		{
		}
		LogOutboundQueueIssue("STA notification failed; message retained");
	}
}

LRESULT CVsmrControlCenterDialog::OnWebViewMessageReceived(WPARAM, LPARAM)
{
	if (!WebHost)
		return 0;
	std::deque<std::string> pending;
	{
		std::lock_guard<std::mutex> lock(WebHost->crossThreadMutex);
		pending.swap(WebHost->inboundJson);
		WebHost->inboundJsonBytes = 0;
		WebHost->inboundNotificationPending = false;
	}
	bool uiReady = false;
	for (const std::string& json : pending)
	{
		try
		{
			std::string selector;
			if (!VsmrWebMessageValidation::TryGetInboundWebMessageSelector(
				json,
				selector))
			{
				LogRejectedWebMessage("queued message failed revalidation");
				continue;
			}
			uiReady = uiReady || selector == "ui.ready";
			if (Bridge && LifetimeToken && LifetimeToken->load())
				Bridge->HandleWebMessage(json);
		}
		catch (const std::exception&)
		{
			LogRejectedWebMessage("message dispatch exception");
		}
		catch (...)
		{
			LogRejectedWebMessage("unexpected message dispatch failure");
		}
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
