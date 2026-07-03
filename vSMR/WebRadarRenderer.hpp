#pragma once

#include "Logger.h"

#include "..\third_party\WebView2\include\WebView2.h"
#include <filesystem>
#include <string>
#include <wrl.h>

#pragma comment(lib, "..\\third_party\\WebView2\\x86\\WebView2LoaderStatic.lib")
#pragma comment(lib, "version.lib")

class CWebRadarRenderer
{
public:
	~CWebRadarRenderer()
	{
		Shutdown();
	}

	bool EnsureVisible(HWND parentWindow, const RECT& bounds, const std::string& dllPath)
	{
		if (parentWindow == nullptr || !::IsWindow(parentWindow))
			return false;
		if (CreationFailed)
			return false;

		if (!ResolveContentRoot(dllPath))
		{
			if (!MissingAssetsLogged)
			{
				Logger::info("WebRadarRenderer: web assets not found beside DLL or repository source tree");
				MissingAssetsLogged = true;
			}
			return false;
		}
		MissingAssetsLogged = false;

		ParentWindow = parentWindow;
		LastBounds = bounds;

		if (Controller)
		{
			Controller->put_Bounds(LastBounds);
			Controller->put_IsVisible(PageReady ? TRUE : FALSE);
			return PageReady;
		}

		if (CreationStarted)
			return false;

		CreationStarted = true;
		PageReady = false;
		const std::wstring userDataFolder = ContentRoot + L"\\WebView2UserData";

		HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
			nullptr,
			userDataFolder.c_str(),
			nullptr,
			Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[this](HRESULT environmentResult, ICoreWebView2Environment* environment) -> HRESULT
				{
					if (FAILED(environmentResult) || environment == nullptr || ParentWindow == nullptr || !::IsWindow(ParentWindow))
					{
						Logger::info("WebRadarRenderer: WebView2 environment creation failed");
						CreationStarted = false;
						CreationFailed = true;
						return environmentResult;
					}

					Environment = environment;
					return Environment->CreateCoreWebView2Controller(
						ParentWindow,
						Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							[this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT
							{
								if (FAILED(controllerResult) || controller == nullptr)
								{
									Logger::info("WebRadarRenderer: WebView2 controller creation failed");
									CreationStarted = false;
									CreationFailed = true;
									return controllerResult;
								}

								Controller = controller;
								Controller->put_Bounds(LastBounds);
								Controller->put_IsVisible(FALSE);
								Controller->get_CoreWebView2(&WebView);

								Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
								if (WebView && SUCCEEDED(WebView.As(&webView3)) && webView3)
								{
									webView3->SetVirtualHostNameToFolderMapping(
										L"vsmr.local",
										ContentRoot.c_str(),
										COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
								}

								if (WebView)
								{
									WebView->add_WebMessageReceived(
										Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
											[this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
											{
												LPWSTR json = nullptr;
												if (args != nullptr && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json != nullptr)
												{
													HandleWebMessage(CWebRadarRenderer::WideToUtf8(json));
													::CoTaskMemFree(json);
												}
												return S_OK;
											}).Get(),
										&MessageToken);
									MessageHandlerRegistered = true;

									WebView->add_NavigationCompleted(
										Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
											[this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
											{
												BOOL success = FALSE;
												if (args != nullptr)
													args->get_IsSuccess(&success);
												Logger::info(std::string("WebRadarRenderer: navigation completed success=") + (success ? "true" : "false"));
												if (!success)
												{
													CreationFailed = true;
													PageReady = false;
													Hide();
												}
												return S_OK;
											}).Get(),
										&NavigationToken);
									NavigationHandlerRegistered = true;

									WebView->Navigate(NavigationUri.c_str());
								}

								CreationStarted = false;
								return S_OK;
							}).Get());
				}).Get());

		if (FAILED(result))
		{
			Logger::info("WebRadarRenderer: CreateCoreWebView2EnvironmentWithOptions failed");
			CreationStarted = false;
			CreationFailed = true;
			return false;
		}

		return false;
	}

	bool HasFailed() const
	{
		return CreationFailed;
	}

	void Hide()
	{
		if (Controller)
			Controller->put_IsVisible(FALSE);
	}

	void Shutdown()
	{
		if (WebView)
		{
			if (MessageHandlerRegistered)
			{
				WebView->remove_WebMessageReceived(MessageToken);
				MessageHandlerRegistered = false;
			}
			if (NavigationHandlerRegistered)
			{
				WebView->remove_NavigationCompleted(NavigationToken);
				NavigationHandlerRegistered = false;
			}
		}
		if (Controller)
		{
			Controller->Close();
			Controller.Reset();
		}
		WebView.Reset();
		Environment.Reset();
		CreationStarted = false;
		PageReady = false;
	}

private:
	void HandleWebMessage(const std::string& json)
	{
		Logger::info("WebRadarRenderer: web message " + json);

		if (json.find("\"type\":\"geojson-loaded\"") != std::string::npos)
		{
			PageReady = true;
			CreationFailed = false;
			if (Controller)
			{
				Controller->put_Bounds(LastBounds);
				Controller->put_IsVisible(TRUE);
			}
			if (ParentWindow != nullptr && ::IsWindow(ParentWindow))
				::InvalidateRect(ParentWindow, nullptr, FALSE);
			return;
		}

		if (json.find("\"type\":\"geojson-error\"") != std::string::npos ||
			json.find("\"type\":\"map-error\"") != std::string::npos)
		{
			PageReady = false;
			CreationFailed = true;
			Hide();
			if (ParentWindow != nullptr && ::IsWindow(ParentWindow))
				::InvalidateRect(ParentWindow, nullptr, FALSE);
		}
	}

	static std::string WideToUtf8(const wchar_t* value)
	{
		if (value == nullptr || value[0] == L'\0')
			return std::string();

		const int length = ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
		if (length <= 0)
			return std::string();

		std::string result(static_cast<size_t>(length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
		if (!result.empty() && result.back() == '\0')
			result.pop_back();
		return result;
	}

	static std::wstring ToWide(const std::string& value)
	{
		if (value.empty())
			return std::wstring();

		const int length = ::MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, nullptr, 0);
		if (length <= 0)
			return std::wstring(value.begin(), value.end());

		std::wstring result(static_cast<size_t>(length), L'\0');
		::MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, result.data(), length);
		if (!result.empty() && result.back() == L'\0')
			result.pop_back();
		return result;
	}

	static bool FileExists(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::exists(path, error) && !error;
	}

	bool ResolveContentRoot(const std::string& dllPath)
	{
		std::filesystem::path runtimeRoot(dllPath);
		if (runtimeRoot.empty())
			runtimeRoot = std::filesystem::current_path();

		const std::filesystem::path runtimeMap = runtimeRoot / "web" / "map.html";
		if (FileExists(runtimeMap))
		{
			ContentRoot = ToWide(runtimeRoot.string());
			NavigationUri = L"https://vsmr.local/web/map.html";
			return true;
		}

		const std::filesystem::path repositoryRoot = runtimeRoot.parent_path();
		const std::filesystem::path sourceMap = repositoryRoot / "vSMR" / "web" / "map.html";
		if (FileExists(sourceMap))
		{
			ContentRoot = ToWide(repositoryRoot.string());
			NavigationUri = L"https://vsmr.local/vSMR/web/map.html";
			return true;
		}

		return false;
	}

	Microsoft::WRL::ComPtr<ICoreWebView2Environment> Environment;
	Microsoft::WRL::ComPtr<ICoreWebView2Controller> Controller;
	Microsoft::WRL::ComPtr<ICoreWebView2> WebView;
	HWND ParentWindow = nullptr;
	RECT LastBounds = {};
	std::wstring ContentRoot;
	std::wstring NavigationUri;
	EventRegistrationToken MessageToken = {};
	EventRegistrationToken NavigationToken = {};
	bool CreationStarted = false;
	bool CreationFailed = false;
	bool PageReady = false;
	bool MissingAssetsLogged = false;
	bool MessageHandlerRegistered = false;
	bool NavigationHandlerRegistered = false;
};
