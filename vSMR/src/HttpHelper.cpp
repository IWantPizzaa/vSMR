#include "stdafx.h"
#include "HttpHelper.hpp"
#include <winhttp.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "winhttp.lib")

//
// HttpHelper Class by Even Rognlien, used with permission
//

namespace
{
	std::string DownloadStringWithWinHttp(
		const std::string& url,
		int timeoutMs,
		const std::atomic<bool>* cancelRequested)
	{
		auto isCancelled = [cancelRequested]() -> bool
		{
			return cancelRequested != nullptr && cancelRequested->load(std::memory_order_acquire);
		};
		if (url.empty() || isCancelled())
			return "";
		timeoutMs = std::clamp(timeoutMs, 1000, 30000);
		const ULONGLONG startedAt = ::GetTickCount64();
		auto remainingTimeoutMs = [&]() -> int
		{
			const ULONGLONG elapsed = ::GetTickCount64() - startedAt;
			if (elapsed >= static_cast<ULONGLONG>(timeoutMs))
				return 0;
			return (std::max)(1, timeoutMs - static_cast<int>(elapsed));
		};

		std::wstring wideUrl(url.begin(), url.end());
		URL_COMPONENTS components = {};
		components.dwStructSize = sizeof(components);
		components.dwSchemeLength = (DWORD)-1;
		components.dwHostNameLength = (DWORD)-1;
		components.dwUrlPathLength = (DWORD)-1;
		components.dwExtraInfoLength = (DWORD)-1;

		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components))
			return "";

		if (components.dwHostNameLength == 0)
			return "";

		std::wstring host(components.lpszHostName, components.dwHostNameLength);
		std::wstring resource;
		if (components.dwUrlPathLength > 0)
			resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength > 0)
			resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
		if (resource.empty())
			resource = L"/";

		const bool isSecure = (components.nScheme == INTERNET_SCHEME_HTTPS);
		const DWORD requestFlags = isSecure ? WINHTTP_FLAG_SECURE : 0;

		HINTERNET session = WinHttpOpen(L"vSMR/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session == NULL)
			return "";

		WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
		if (isCancelled() || remainingTimeoutMs() == 0)
		{
			WinHttpCloseHandle(session);
			return "";
		}

		HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
		if (connect == NULL)
		{
			WinHttpCloseHandle(session);
			return "";
		}
		if (isCancelled() || remainingTimeoutMs() == 0)
		{
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return "";
		}

		HINTERNET request = WinHttpOpenRequest(connect, L"GET", resource.c_str(), NULL,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
		if (request == NULL)
		{
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return "";
		}

		int remaining = remainingTimeoutMs();
		if (isCancelled() || remaining == 0)
		{
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return "";
		}
		WinHttpSetTimeouts(request, remaining, remaining, remaining, remaining);
		BOOL ok = WinHttpSendRequest(request,
			L"Accept: application/json\r\n",
			(DWORD)-1L,
			WINHTTP_NO_REQUEST_DATA,
			0,
			0,
			0);
		remaining = remainingTimeoutMs();
		if (ok == TRUE && !isCancelled() && remaining > 0)
		{
			WinHttpSetTimeouts(request, remaining, remaining, remaining, remaining);
			ok = WinHttpReceiveResponse(request, NULL);
		}
		else
		{
			ok = FALSE;
		}

		std::string response;
		if (ok == TRUE && !isCancelled())
		{
			DWORD available = 0;
			do
			{
				remaining = remainingTimeoutMs();
				if (isCancelled() || remaining == 0)
					break;
				WinHttpSetTimeouts(request, remaining, remaining, remaining, remaining);
				available = 0;
				if (!WinHttpQueryDataAvailable(request, &available))
					break;
				if (available == 0)
					break;

				std::vector<char> buffer(available);
				DWORD downloaded = 0;
				if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
					break;

				if (downloaded == 0)
					break;

				response.append(buffer.data(), downloaded);
			} while (available > 0 && !isCancelled());
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);

		return isCancelled() ? "" : response;
	}
}

HttpHelper::HttpHelper()  {

}

std::string HttpHelper::downloadStringFromURL(
	std::string url,
	int timeoutMs,
	const std::atomic<bool>* cancelRequested) {
	return DownloadStringWithWinHttp(url, timeoutMs, cancelRequested);
}

HttpHelper::~HttpHelper() {

}
