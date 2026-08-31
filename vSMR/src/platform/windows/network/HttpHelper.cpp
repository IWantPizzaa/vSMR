#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "winhttp.lib")

//
// HttpHelper Class by Even Rognlien, used with permission
//

namespace
{
	bool CrackValidatedHttpsUrl(
		const std::string& url,
		std::wstring* hostOut,
		std::wstring* wideUrlOut = nullptr)
	{
		if (url.empty() || url.find('#') != std::string::npos)
			return false;
		for (const unsigned char character : url)
		{
			if (character <= 0x20 || character == 0x7f)
				return false;
		}

		const int wideLength = ::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			url.data(),
			static_cast<int>(url.size()),
			nullptr,
			0);
		if (wideLength <= 0)
			return false;
		std::wstring wideUrl(static_cast<size_t>(wideLength), L'\0');
		if (::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			url.data(),
			static_cast<int>(url.size()),
			wideUrl.data(),
			wideLength) != wideLength)
		{
			return false;
		}

		URL_COMPONENTS components = {};
		components.dwStructSize = sizeof(components);
		components.dwSchemeLength = (DWORD)-1;
		components.dwHostNameLength = (DWORD)-1;
		components.dwUserNameLength = (DWORD)-1;
		components.dwPasswordLength = (DWORD)-1;
		components.dwUrlPathLength = (DWORD)-1;
		components.dwExtraInfoLength = (DWORD)-1;
		if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components) ||
			components.nScheme != INTERNET_SCHEME_HTTPS ||
			components.dwHostNameLength == 0 ||
			components.dwUserNameLength != 0 ||
			components.dwPasswordLength != 0)
		{
			return false;
		}

		std::wstring host(
			components.lpszHostName,
			components.dwHostNameLength);
		if (host.empty() || host.front() == L'.' || host.back() == L'.' ||
			host.front() == L'-' || host.back() == L'-')
		{
			return false;
		}
		for (const wchar_t character : host)
		{
			if (!(std::iswalnum(character) || character == L'.' || character == L'-'))
				return false;
		}

		if (hostOut != nullptr)
			*hostOut = std::move(host);
		if (wideUrlOut != nullptr)
			*wideUrlOut = std::move(wideUrl);
		return true;
	}

	std::string DownloadStringWithWinHttp(
		const std::string& url,
		int timeoutMs,
		const std::atomic<bool>* cancelRequested,
		size_t maxResponseBytes)
	{
		auto isCancelled = [cancelRequested]() -> bool
		{
			return cancelRequested != nullptr && cancelRequested->load(std::memory_order_acquire);
		};
		std::wstring wideUrl;
		if (url.empty() || isCancelled() || maxResponseBytes == 0 ||
			!CrackValidatedHttpsUrl(url, nullptr, &wideUrl))
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

		if (components.nScheme != INTERNET_SCHEME_HTTPS)
			return "";
		const DWORD requestFlags = WINHTTP_FLAG_SECURE;

		HINTERNET session = WinHttpOpen(L"vSMR/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session == NULL)
			return "";

		if (!WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs))
		{
			WinHttpCloseHandle(session);
			return "";
		}
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
		DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		if (!::WinHttpSetOption(
			request,
			WINHTTP_OPTION_REDIRECT_POLICY,
			&redirectPolicy,
			sizeof(redirectPolicy)))
		{
			WinHttpCloseHandle(request);
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
		if (!WinHttpSetTimeouts(request, remaining, remaining, remaining, remaining))
		{
			WinHttpCloseHandle(request);
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
		remaining = remainingTimeoutMs();
		if (ok == TRUE && !isCancelled() && remaining > 0)
		{
			ok = WinHttpSetTimeouts(
				request,
				remaining,
				remaining,
				remaining,
				remaining);
			if (ok == TRUE)
				ok = WinHttpReceiveResponse(request, NULL);
		}
		else
		{
			ok = FALSE;
		}

		std::string response;
		bool responseTooLarge = false;
		bool responseComplete = false;
		bool readFailed = false;
		if (ok == TRUE && !isCancelled())
		{
			DWORD statusCode = 0;
			DWORD statusCodeSize = sizeof(statusCode);
			if (!::WinHttpQueryHeaders(
				request,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&statusCode,
				&statusCodeSize,
				WINHTTP_NO_HEADER_INDEX) ||
				statusCode < 200 || statusCode >= 300)
			{
				ok = FALSE;
			}

			DWORD contentLength = 0;
			DWORD contentLengthSize = sizeof(contentLength);
			if (ok == TRUE && ::WinHttpQueryHeaders(
				request,
				WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&contentLength,
				&contentLengthSize,
				WINHTTP_NO_HEADER_INDEX) &&
				static_cast<size_t>(contentLength) > maxResponseBytes)
			{
				ok = FALSE;
				responseTooLarge = true;
			}
		}
		if (ok == TRUE && !isCancelled())
		{
			for (;;)
			{
				remaining = remainingTimeoutMs();
				if (isCancelled() || remaining == 0)
				{
					readFailed = true;
					break;
				}
				if (!WinHttpSetTimeouts(
						request,
						remaining,
						remaining,
						remaining,
						remaining))
				{
					readFailed = true;
					break;
				}
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available))
				{
					readFailed = true;
					break;
				}
				if (available == 0)
				{
					responseComplete = true;
					break;
				}

				const size_t remainingCapacity = maxResponseBytes - response.size();
				if (remainingCapacity == 0)
				{
					responseTooLarge = true;
					break;
				}
				const size_t boundedReadCapacity =
					remainingCapacity < static_cast<size_t>(64U * 1024U)
					? remainingCapacity + 1U
					: static_cast<size_t>(64U * 1024U);
				const DWORD readCapacity = static_cast<DWORD>((std::min)(
					static_cast<size_t>(available),
					boundedReadCapacity));
				std::vector<char> buffer(readCapacity);
				DWORD downloaded = 0;
				if (!WinHttpReadData(request, buffer.data(), readCapacity, &downloaded))
				{
					readFailed = true;
					break;
				}

				if (downloaded == 0)
				{
					readFailed = true;
					break;
				}

				if (static_cast<size_t>(downloaded) > remainingCapacity)
				{
					responseTooLarge = true;
					break;
				}
				response.append(buffer.data(), downloaded);
			}
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);

		return isCancelled() || responseTooLarge || readFailed ||
			!responseComplete ? "" : response;
	}
}

HttpHelper::HttpHelper()  {

}

std::string HttpHelper::downloadStringFromURL(
	std::string url,
	int timeoutMs,
	const std::atomic<bool>* cancelRequested,
	size_t maxResponseBytes) {
	return DownloadStringWithWinHttp(
		url,
		timeoutMs,
		cancelRequested,
		maxResponseBytes);
}

bool HttpHelper::IsValidHttpsUrl(
	const std::string& url,
	std::string* host)
{
	std::wstring wideHost;
	if (!CrackValidatedHttpsUrl(url, &wideHost))
		return false;
	if (host != nullptr)
	{
		host->clear();
		host->reserve(wideHost.size());
		for (const wchar_t character : wideHost)
		{
			if (character > 0x7f)
				return false;
			host->push_back(static_cast<char>(std::towlower(character)));
		}
	}
	return true;
}

bool HttpHelper::IsHttpsUrlForHost(
	const std::string& url,
	const std::string& expectedHost)
{
	std::string host;
	if (!IsValidHttpsUrl(url, &host))
		return false;
	std::string normalizedExpectedHost = expectedHost;
	std::transform(
		normalizedExpectedHost.begin(),
		normalizedExpectedHost.end(),
		normalizedExpectedHost.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	return host == normalizedExpectedHost;
}

HttpHelper::~HttpHelper() {

}
