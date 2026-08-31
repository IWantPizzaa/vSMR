#pragma once

#include "control_center/ControlCenterDialog.hpp"
#include "control_center/WebMessageValidation.hpp"

#include "WebView2.h"
#include <wrl.h>

#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>

namespace VsmrControlCenterDialogInternal
{
	inline constexpr UINT kWebViewMessageReceivedMessage = WM_APP + 0x560;
	inline constexpr UINT kWebViewFallbackMessage = WM_APP + 0x561;
	inline constexpr UINT kWebViewReadyMessage = WM_APP + 0x562;
	inline constexpr UINT kWebViewResizeMessage = WM_APP + 0x563;
	inline constexpr UINT kWebViewSendJsonMessage = WM_APP + 0x564;
	inline constexpr UINT kWebViewShowPageMessage = WM_APP + 0x565;
	inline constexpr UINT kWebViewParentMovedMessage = WM_APP + 0x566;
	inline constexpr UINT kWebViewShutdownMessage = WM_APP + 0x567;
	inline constexpr UINT kWebViewBeginWindowDragMessage = WM_APP + 0x568;
	inline constexpr UINT kWebViewPageReadyMessage = WM_APP + 0x569;
	inline constexpr UINT_PTR kEuroScopeBoundsTimerId = 0x5A1;
	inline constexpr UINT kEuroScopeBoundsTimerIntervalMs = 250;
	inline constexpr int kFixedWindowWidth = 728;
	inline constexpr int kFixedWindowHeight = 500;
	inline constexpr std::size_t kMaximumResourceBytes = 16u * 1024u * 1024u;
	inline constexpr std::size_t kMaximumWindowPlacementBytes = 64u * 1024u;
	inline constexpr std::size_t kMaximumQueuedInboundWebMessageBytes =
		VsmrWebMessageValidation::MaximumInboundMessageBytes;
	inline constexpr std::size_t kMaximumQueuedInboundWebMessages = 64;
	inline constexpr std::size_t kMaximumQueuedOutboundWebMessageBytes =
		64U * 1024U * 1024U;
	inline constexpr std::size_t kMaximumQueuedOutboundWebMessages = 64;
	inline constexpr const wchar_t* kVirtualHostName = L"app.vsmr";
	inline constexpr const wchar_t* kVirtualOriginPrefix = L"https://app.vsmr/";
	inline constexpr const wchar_t* kAircraftIconVirtualHostName = L"icons.vsmr";
	inline constexpr const wchar_t* kWebViewHostWindowClass =
		L"vSMR.ControlCenter.WebViewHost";

	bool ReadTextFile(
		const std::filesystem::path& path,
		std::string& text,
		std::size_t maximumBytes = (std::numeric_limits<std::size_t>::max)());
}

// WebView2 and cross-thread queue details remain private to dialog
// implementation units while sharing one well-defined lifetime owner.
struct CVsmrControlCenterDialog::WebViewHostState
{
	std::mutex crossThreadMutex;
	std::deque<std::string> outboundJson;
	std::size_t outboundJsonBytes = 0;
	bool outboundNotificationPending = false;
	std::deque<std::string> inboundJson;
	std::size_t inboundJsonBytes = 0;
	bool inboundNotificationPending = false;
	std::deque<std::string> fallbackMessages;
	std::wstring resourceFolder;
	std::wstring userDataFolder;
	std::wstring canonicalDocumentUri;
	HWND dialogWindow = nullptr;
	HINSTANCE moduleInstance = nullptr;
	std::atomic<HWND> threadWindow{ nullptr };
	std::atomic<DWORD> threadId{ 0 };
	std::atomic<bool> stopRequested{ false };
	std::atomic<int> clientWidth{
		VsmrControlCenterDialogInternal::kFixedWindowWidth };
	std::atomic<int> clientHeight{
		VsmrControlCenterDialogInternal::kFixedWindowHeight };
	std::atomic<int> requestedPage{
		static_cast<int>(CVsmrControlCenterDialog::Page::Display) };
	std::atomic<bool> preservePageOnNextOpen{ false };
	std::atomic<bool> pageReady{ false };
	std::atomic<unsigned long> rejectedInboundMessages{ 0 };
	std::atomic<unsigned long> outboundQueueIssues{ 0 };
	bool windowClassAcquired = false;
	bool environmentCreationPending = false;
	bool controllerCreationPending = false;
	Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
	Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
	Microsoft::WRL::ComPtr<ICoreWebView2> webView;
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
