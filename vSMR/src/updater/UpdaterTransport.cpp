#include "updater/UpdaterTransport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vsmr::updater::transport
{
	namespace
	{
		class FileHandle
		{
		public:
			explicit FileHandle(HANDLE value = nullptr) noexcept : value_(value) {}
			~FileHandle()
			{
				reset();
			}
			FileHandle(const FileHandle&) = delete;
			FileHandle& operator=(const FileHandle&) = delete;
			HANDLE get() const noexcept
			{
				return value_;
			}
			explicit operator bool() const noexcept
			{
				return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
			}
			void reset(HANDLE value = nullptr) noexcept
			{
				if (*this)
					::CloseHandle(value_);
				value_ = value;
			}

		private:
			HANDLE value_ = nullptr;
		};

		class InternetHandle
		{
		public:
			explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
			~InternetHandle()
			{
				if (value_ != nullptr)
					::WinHttpCloseHandle(value_);
			}
			InternetHandle(const InternetHandle&) = delete;
			InternetHandle& operator=(const InternetHandle&) = delete;
			HINTERNET get() const noexcept
			{
				return value_;
			}
			explicit operator bool() const noexcept
			{
				return value_ != nullptr;
			}

		private:
			HINTERNET value_ = nullptr;
		};

		std::string WideToUtf8(const std::wstring& value)
		{
			if (value.empty())
				return {};
			const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return {};
			std::string result(static_cast<std::size_t>(size), '\0');
			return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
					   result.data(), size, nullptr, nullptr) == size
					   ? result
					   : std::string();
		}

		std::wstring Utf8ToWide(const std::string& value)
		{
			if (value.empty())
				return {};
			const int size = ::MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0)
				return {};
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
					   result.data(), size) == size
					   ? result
					   : std::wstring();
		}

		std::wstring ToLowerWide(std::wstring value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
			return value;
		}

		bool EndsWithNoCase(const std::wstring& value, const std::wstring& suffix)
		{
			return suffix.size() <= value.size() &&
				   ToLowerWide(value.substr(value.size() - suffix.size())) == ToLowerWide(suffix);
		}

		bool IsAllowedDownloadHost(const std::wstring& host)
		{
			const std::wstring normalized = ToLowerWide(host);
			return normalized == L"api.github.com" || normalized == L"github.com" ||
				   normalized == L"release-assets.githubusercontent.com" ||
				   normalized == L"objects.githubusercontent.com" ||
				   normalized == L"github-releases.githubusercontent.com" ||
				   EndsWithNoCase(normalized, L".githubusercontent.com");
		}

		struct ParsedUrl
		{
			std::wstring host;
			std::wstring resource;
			INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
		};

		bool ParseAllowedHttpsUrl(const std::wstring& url, ParsedUrl& result)
		{
			if (url.empty() || url.find(L'#') != std::wstring::npos)
				return false;
			URL_COMPONENTS components{};
			components.dwStructSize = sizeof(components);
			components.dwSchemeLength = components.dwHostNameLength = components.dwUserNameLength =
				components.dwPasswordLength = components.dwUrlPathLength = components.dwExtraInfoLength =
					static_cast<DWORD>(-1);
			if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS ||
				components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
				components.dwPasswordLength != 0 || components.nPort != INTERNET_DEFAULT_HTTPS_PORT)
				return false;
			result.host.assign(components.lpszHostName, components.dwHostNameLength);
			if (!IsAllowedDownloadHost(result.host))
				return false;
			result.resource.clear();
			if (components.dwUrlPathLength > 0)
				result.resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
			if (components.dwExtraInfoLength > 0)
				result.resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
			if (result.resource.empty())
				result.resource = L"/";
			result.port = components.nPort;
			return true;
		}

		std::string QueryHeader(HINTERNET request, DWORD query, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX)
		{
			DWORD size = 0;
			::WinHttpQueryHeaders(request, query, name, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
			if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t))
				return {};
			std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
			return ::WinHttpQueryHeaders(request, query, name, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)
					   ? WideToUtf8(std::wstring(buffer.data()))
					   : std::string();
		}

		std::wstring ResolveRedirect(const std::wstring& currentUrl, const std::string& location)
		{
			const std::wstring wideLocation = Utf8ToWide(location);
			if (wideLocation.empty())
				return {};
			if (wideLocation.rfind(L"https://", 0) == 0)
				return wideLocation;
			ParsedUrl current;
			if (!ParseAllowedHttpsUrl(currentUrl, current) || wideLocation.front() != L'/')
				return {};
			return L"https://" + current.host + wideLocation;
		}
	} // namespace

	Response HttpGet(const Request& parameters, const std::function<bool()>& isCancelled,
		const std::function<bool(int)>& reportDownloadProgress)
	{
		Response response;
		std::wstring url = parameters.initialUrl;
		const ULONGLONG started = ::GetTickCount64();
		auto remaining = [&]() -> DWORD
		{
			const ULONGLONG elapsed = ::GetTickCount64() - started;
			return elapsed >= parameters.timeoutMs ? 0 : parameters.timeoutMs - static_cast<DWORD>(elapsed);
		};
		InternetHandle session(::WinHttpOpen(L"vSMR-Updater/1.0 (+https://github.com/IWantPizzaa/vSMR)",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session)
		{
			response.error = "winhttp_session";
			return response;
		}
		::WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, static_cast<int>(parameters.timeoutMs));
		for (int redirect = 0; redirect <= 5; ++redirect)
		{
			if (isCancelled() || remaining() == 0)
			{
				response.error = isCancelled() ? "cancelled" : "timeout";
				return response;
			}
			ParsedUrl parsed;
			if (!ParseAllowedHttpsUrl(url, parsed))
			{
				response.error = "unsafe_url";
				return response;
			}
			InternetHandle connection(::WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0));
			if (!connection)
			{
				response.error = "connect_failed";
				return response;
			}
			InternetHandle request(::WinHttpOpenRequest(connection.get(), L"GET", parsed.resource.c_str(), nullptr,
				WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
			if (!request)
			{
				response.error = "request_failed";
				return response;
			}
			DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
			::WinHttpSetTimeouts(request.get(), 3000, 3000, 3000, remaining());
			std::wstring headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
			if (!parameters.ifNoneMatch.empty())
				headers += L"If-None-Match: " + Utf8ToWide(parameters.ifNoneMatch) + L"\r\n";
			std::uint64_t resumeOffset = 0;
			if (!parameters.outputFile.empty())
			{
				std::error_code error;
				resumeOffset = fs::file_size(parameters.outputFile, error);
				if (error || resumeOffset >= parameters.maximumBytes ||
					(parameters.expectedSize > 0 && resumeOffset >= parameters.expectedSize))
					resumeOffset = 0;
				if (resumeOffset > 0)
					headers += L"Range: bytes=" + std::to_wstring(resumeOffset) + L"-\r\n";
			}
			BOOL ok = ::WinHttpSendRequest(
				request.get(), headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
			if (ok)
				ok = ::WinHttpReceiveResponse(request.get(), nullptr);
			if (!ok)
			{
				response.error = ::GetLastError() == ERROR_WINHTTP_TIMEOUT ? "timeout" : "network_error";
				return response;
			}
			DWORD status = 0, statusSize = sizeof(status);
			if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
			{
				response.error = "missing_status";
				return response;
			}
			if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
			{
				url = ResolveRedirect(url, QueryHeader(request.get(), WINHTTP_QUERY_LOCATION));
				if (url.empty())
				{
					response.error = "unsafe_redirect";
					return response;
				}
				continue;
			}
			response.statusCode = status;
			response.etag = QueryHeader(request.get(), WINHTTP_QUERY_ETAG);
			response.retryAfter = QueryHeader(request.get(), WINHTTP_QUERY_RETRY_AFTER);
			response.rateLimitReset = QueryHeader(request.get(), WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Reset");
			response.finalUrl = url;
			if (status == 304 || status == 403 || status == 429 || status < 200 || status >= 300)
				return response;
			const bool append = !parameters.outputFile.empty() && resumeOffset > 0 && status == 206;
			if (!append)
				resumeOffset = 0;
			FileHandle file;
			if (!parameters.outputFile.empty())
			{
				std::error_code error;
				fs::create_directories(parameters.outputFile.parent_path(), error);
				if (error)
				{
					response.error = "staging_directory";
					return response;
				}
				file.reset(::CreateFileW(parameters.outputFile.c_str(), GENERIC_WRITE, 0, nullptr,
					append ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
				if (!file)
				{
					response.error = "staging_file";
					return response;
				}
				if (append)
					::SetFilePointer(file.get(), 0, nullptr, FILE_END);
			}
			std::uint64_t received = resumeOffset;
			int lastPercent = -1;
			std::array<BYTE, 64 * 1024> buffer{};
			for (;;)
			{
				if (isCancelled() || remaining() == 0)
				{
					response.error = isCancelled() ? "cancelled" : "timeout";
					return response;
				}
				DWORD read = 0;
				if (!::WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
				{
					response.error = "read_failed";
					return response;
				}
				if (read == 0)
					break;
				if (received > parameters.maximumBytes - read)
				{
					response.error = "response_too_large";
					return response;
				}
				received += read;
				if (file)
				{
					DWORD written = 0;
					if (!::WriteFile(file.get(), buffer.data(), read, &written, nullptr) || written != read)
					{
						response.error = "write_failed";
						return response;
					}
				}
				else
					response.body.insert(response.body.end(), buffer.begin(), buffer.begin() + read);
				if (parameters.expectedSize > 0 && !parameters.outputFile.empty())
				{
					const int percent =
						static_cast<int>((std::min)(100ULL, received * 100ULL / parameters.expectedSize));
					if (percent >= lastPercent + 5)
					{
						lastPercent = percent;
						if (!reportDownloadProgress(percent))
						{
							response.error = "cancelled";
							return response;
						}
					}
				}
			}
			if (file && !::FlushFileBuffers(file.get()))
			{
				response.error = "flush_failed";
				return response;
			}
			if (parameters.expectedSize > 0 && received != parameters.expectedSize)
			{
				response.error = "size_mismatch";
				return response;
			}
			return response;
		}
		response.error = "too_many_redirects";
		return response;
	}
} // namespace vsmr::updater::transport
