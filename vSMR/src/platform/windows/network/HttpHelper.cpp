#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include "platform/windows/network/WinHttpSupport.hpp"
#include <algorithm>
#include <cwctype>
#include <limits>
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
			components.dwPasswordLength != 0 ||
			components.nPort != INTERNET_DEFAULT_HTTPS_PORT)
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

	bool IsLoopbackHost(const std::wstring& host)
	{
		std::wstring lower = host;
		for (wchar_t& character : lower)
			character = static_cast<wchar_t>(std::towlower(character));
		return lower == L"localhost" || lower == L"127.0.0.1";
	}

	// Plain http is only accepted for the local machine, where a developer runs
	// PINEDE; anything leaving the machine must use https on port 443.
	bool CrackRequestUrl(
		const std::string& url,
		bool allowLoopbackHttp,
		std::wstring& wideUrl)
	{
		if (CrackValidatedHttpsUrl(url, nullptr, &wideUrl))
			return true;
		if (!allowLoopbackHttp || url.empty() || url.find('#') != std::string::npos)
			return false;
		for (const unsigned char character : url)
		{
			if (character <= 0x20 || character >= 0x7f)
				return false;
		}
		std::wstring candidate(url.begin(), url.end());
		URL_COMPONENTS components = {};
		components.dwStructSize = sizeof(components);
		components.dwSchemeLength = (DWORD)-1;
		components.dwHostNameLength = (DWORD)-1;
		components.dwUserNameLength = (DWORD)-1;
		components.dwPasswordLength = (DWORD)-1;
		components.dwUrlPathLength = (DWORD)-1;
		components.dwExtraInfoLength = (DWORD)-1;
		if (!::WinHttpCrackUrl(candidate.c_str(), 0, 0, &components) ||
			components.nScheme != INTERNET_SCHEME_HTTP ||
			components.dwHostNameLength == 0 ||
			components.dwUserNameLength != 0 ||
			components.dwPasswordLength != 0 ||
			!IsLoopbackHost(std::wstring(components.lpszHostName, components.dwHostNameLength)))
		{
			return false;
		}
		wideUrl = std::move(candidate);
		return true;
	}

	struct WinHttpRequestOptions
	{
		const wchar_t* method = L"GET";
		std::string url;
		bool allowLoopbackHttp = false;
		std::wstring extraHeaders;
		std::string body;
		int timeoutMs = 6000;
		const std::atomic<bool>* cancelRequested = nullptr;
		size_t maxResponseBytes = 8U * 1024U * 1024U;
		// GET downloads stop at a non-2xx status; JSON APIs need the error body
		bool readErrorBodies = false;
	};

	HttpResponse PerformWinHttpRequest(const WinHttpRequestOptions& options)
	{
		HttpResponse result;
		const std::atomic<bool>* cancelRequested = options.cancelRequested;
		auto isCancelled = [cancelRequested]() -> bool
		{
			return cancelRequested != nullptr && cancelRequested->load(std::memory_order_acquire);
		};
		std::wstring wideUrl;
		if (options.url.empty() || isCancelled() || options.maxResponseBytes == 0 ||
			options.body.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)()) ||
			!CrackRequestUrl(options.url, options.allowLoopbackHttp, wideUrl))
			return result;
		const int timeoutMs = std::clamp(options.timeoutMs, 1000, 30000);
		const size_t maxResponseBytes = options.maxResponseBytes;
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
			return result;

		if (components.lpszHostName == nullptr || components.dwHostNameLength == 0 ||
			(components.dwUrlPathLength > 0 && components.lpszUrlPath == nullptr) ||
			(components.dwExtraInfoLength > 0 && components.lpszExtraInfo == nullptr))
			return result;

		std::wstring host(components.lpszHostName, components.dwHostNameLength);
		std::wstring resource;
		if (components.dwUrlPathLength > 0)
			resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength > 0)
			resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
		if (resource.empty())
			resource = L"/";

		const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
		if (!secure && !(options.allowLoopbackHttp && components.nScheme == INTERNET_SCHEME_HTTP && IsLoopbackHost(host)))
			return result;
		const DWORD requestFlags = secure ? WINHTTP_FLAG_SECURE : 0;

		VsmrHttp::InternetHandle session(WinHttpOpen(L"vSMR/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session)
			return result;

		if (secure && !VsmrHttp::RequireModernTls(session.get()))
			return result;

		if (!WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs))
			return result;
		if (isCancelled() || remainingTimeoutMs() == 0)
			return result;

		VsmrHttp::InternetHandle connect(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
		if (!connect)
			return result;
		if (isCancelled() || remainingTimeoutMs() == 0)
			return result;

		VsmrHttp::InternetHandle request(WinHttpOpenRequest(connect.get(), options.method, resource.c_str(), NULL,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags));
		if (!request)
			return result;
		DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		if (!::WinHttpSetOption(
			request.get(),
			WINHTTP_OPTION_REDIRECT_POLICY,
			&redirectPolicy,
			sizeof(redirectPolicy)))
		{
			return result;
		}

		int remaining = remainingTimeoutMs();
		if (isCancelled() || remaining == 0)
			return result;
		if (!WinHttpSetTimeouts(request.get(), remaining, remaining, remaining, remaining))
			return result;
		const std::wstring headers = L"Accept: application/json\r\n" + options.extraHeaders;
		BOOL ok = WinHttpSendRequest(request.get(),
			headers.c_str(),
			(DWORD)-1L,
			options.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(options.body.data()),
			static_cast<DWORD>(options.body.size()),
			static_cast<DWORD>(options.body.size()),
			0);
		remaining = remainingTimeoutMs();
		if (ok == TRUE && !isCancelled() && remaining > 0)
		{
			ok = WinHttpSetTimeouts(
				request.get(),
				remaining,
				remaining,
				remaining,
				remaining);
			if (ok == TRUE)
				ok = WinHttpReceiveResponse(request.get(), NULL);
		}
		else
		{
			ok = FALSE;
		}

		std::string response;
		bool responseTooLarge = false;
		bool responseComplete = false;
		bool readFailed = false;
		DWORD statusCode = 0;
		if (ok == TRUE && !isCancelled())
		{
			DWORD statusCodeSize = sizeof(statusCode);
			if (!::WinHttpQueryHeaders(
				request.get(),
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&statusCode,
				&statusCodeSize,
				WINHTTP_NO_HEADER_INDEX) ||
				(!options.readErrorBodies && (statusCode < 200 || statusCode >= 300)))
			{
				ok = FALSE;
			}

			DWORD contentLength = 0;
			DWORD contentLengthSize = sizeof(contentLength);
			if (ok == TRUE && ::WinHttpQueryHeaders(
				request.get(),
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
						request.get(),
						remaining,
						remaining,
						remaining,
						remaining))
				{
					readFailed = true;
					break;
				}
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request.get(), &available))
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
				if (!WinHttpReadData(request.get(), buffer.data(), readCapacity, &downloaded))
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

		if (isCancelled() || responseTooLarge || readFailed || !responseComplete)
			return result;
		result.completed = true;
		result.status = static_cast<unsigned long>(statusCode);
		result.body = std::move(response);
		return result;
	}

	std::string DownloadStringWithWinHttp(
		const std::string& url,
		int timeoutMs,
		const std::atomic<bool>* cancelRequested,
		size_t maxResponseBytes)
	{
		WinHttpRequestOptions options;
		options.url = url;
		options.timeoutMs = timeoutMs;
		options.cancelRequested = cancelRequested;
		options.maxResponseBytes = maxResponseBytes;
		HttpResponse response = PerformWinHttpRequest(options);
		return response.completed && response.status >= 200 && response.status < 300 ? response.body : "";
	}

	std::wstring WidenAscii(const std::string& text)
	{
		std::wstring wide;
		wide.reserve(text.size());
		for (const unsigned char character : text)
		{
			// header values are ASCII; anything else would let a value break out of its header
			if (character < 0x20 || character >= 0x7f)
				return L"";
			wide.push_back(static_cast<wchar_t>(character));
		}
		return wide;
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

HttpResponse HttpHelper::sendJsonRequest(
	const std::string& url,
	const std::string& jsonBody,
	const std::string& authorization,
	int timeoutMs,
	const std::atomic<bool>* cancelRequested,
	size_t maxResponseBytes)
{
	const std::wstring authorizationHeader = WidenAscii(authorization);
	if (!authorization.empty() && authorizationHeader.empty())
		return HttpResponse{};
	WinHttpRequestOptions options;
	options.method = L"POST";
	options.url = url;
	options.allowLoopbackHttp = true;
	options.extraHeaders = L"Content-Type: application/json\r\n";
	if (!authorizationHeader.empty())
		options.extraHeaders += L"Authorization: " + authorizationHeader + L"\r\n";
	options.body = jsonBody;
	options.timeoutMs = timeoutMs;
	options.cancelRequested = cancelRequested;
	options.maxResponseBytes = maxResponseBytes;
	options.readErrorBodies = true;
	return PerformWinHttpRequest(options);
}

bool HttpHelper::IsAllowedApiUrl(const std::string& url)
{
	std::wstring wideUrl;
	return CrackRequestUrl(url, true, wideUrl);
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
