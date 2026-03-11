#include "stdafx.h"
#include "HttpHelper.hpp"
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

//
// HttpHelper Class by Even Rognlien, used with permission
//

std::string HttpHelper::downloadedContents;
std::mutex HttpHelper::downloadMutex;

namespace
{
	std::string DownloadStringWithWinHttp(const std::string& url)
	{
		if (url.empty())
			return "";

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

		const int timeoutMs = 6000;
		WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

		HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
		if (connect == NULL)
		{
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

		BOOL ok = WinHttpSendRequest(request,
			L"Accept: application/json\r\n",
			(DWORD)-1L,
			WINHTTP_NO_REQUEST_DATA,
			0,
			0,
			0);
		if (ok == TRUE)
			ok = WinHttpReceiveResponse(request, NULL);

		std::string response;
		if (ok == TRUE)
		{
			DWORD available = 0;
			do
			{
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
			} while (available > 0);
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);

		return response;
	}
}

HttpHelper::HttpHelper()  {

}

// Used for downloading strings from web:
size_t HttpHelper::handle_data(void *ptr, size_t size, size_t nmemb, void *stream) {
	int numbytes = size*nmemb;
	// The data is not null-terminated, so get the last character, and replace it with '\0'. 
	char lastchar = *((char *)ptr + numbytes - 1);
	*((char *)ptr + numbytes - 1) = '\0';
	downloadedContents.append((char *)ptr);
	downloadedContents.append(1, lastchar);
	*((char *)ptr + numbytes - 1) = lastchar;  // Might not be necessary. 
	return size*nmemb;
}


std::string HttpHelper::downloadStringFromURL(std::string url) {
	std::lock_guard<std::mutex> guard(downloadMutex);
	return DownloadStringWithWinHttp(url);
}

HttpHelper::~HttpHelper() {

}
