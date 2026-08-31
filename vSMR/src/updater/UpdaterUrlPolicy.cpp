#include "updater/UpdaterUrlPolicy.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace vsmr::updater::url_policy
{
	namespace
	{
		std::wstring ToLowerWide(std::wstring value)
		{
			std::transform(
				value.begin(),
				value.end(),
				value.begin(),
				[](wchar_t character)
				{
					return static_cast<wchar_t>(std::towlower(character));
				});
			return value;
		}

		bool EndsWith(const std::wstring& value, const std::wstring& suffix)
		{
			return suffix.size() <= value.size() &&
				value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		bool IsAllowedDownloadHost(const std::wstring& host)
		{
			const std::wstring normalized = ToLowerWide(host);
			return normalized == L"api.github.com" ||
				normalized == L"github.com" ||
				normalized == L"release-assets.githubusercontent.com" ||
				normalized == L"objects.githubusercontent.com" ||
				normalized == L"github-releases.githubusercontent.com" ||
				EndsWith(normalized, L".githubusercontent.com");
		}
	}

	bool TryParseAllowedHttpsUrl(
		const std::wstring& url,
		ParsedHttpsUrl& result)
	{
		if (url.empty() || url.find(L'#') != std::wstring::npos)
			return false;

		URL_COMPONENTS components{};
		components.dwStructSize = sizeof(components);
		components.dwSchemeLength = static_cast<DWORD>(-1);
		components.dwHostNameLength = static_cast<DWORD>(-1);
		components.dwUserNameLength = static_cast<DWORD>(-1);
		components.dwPasswordLength = static_cast<DWORD>(-1);
		components.dwUrlPathLength = static_cast<DWORD>(-1);
		components.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
			components.nScheme != INTERNET_SCHEME_HTTPS ||
			components.dwHostNameLength == 0 ||
			components.dwUserNameLength != 0 ||
			components.dwPasswordLength != 0 ||
			components.nPort != INTERNET_DEFAULT_HTTPS_PORT)
		{
			return false;
		}

		ParsedHttpsUrl parsed;
		parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
		if (!IsAllowedDownloadHost(parsed.host))
			return false;

		if (components.dwUrlPathLength > 0)
			parsed.resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength > 0)
			parsed.resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
		if (parsed.resource.empty())
			parsed.resource = L"/";
		parsed.port = static_cast<std::uint16_t>(components.nPort);
		result = std::move(parsed);
		return true;
	}

	bool TryResolveAllowedRedirect(
		const std::wstring& currentUrl,
		const std::wstring& location,
		std::wstring& result)
	{
		if (location.empty())
			return false;

		ParsedHttpsUrl parsedLocation;
		if (TryParseAllowedHttpsUrl(location, parsedLocation))
		{
			result = location;
			return true;
		}
		if (location.front() != L'/')
			return false;

		ParsedHttpsUrl parsedCurrent;
		if (!TryParseAllowedHttpsUrl(currentUrl, parsedCurrent))
			return false;

		std::wstring resolved = L"https://" + parsedCurrent.host + location;
		if (!TryParseAllowedHttpsUrl(resolved, parsedLocation))
			return false;
		result = std::move(resolved);
		return true;
	}
}
