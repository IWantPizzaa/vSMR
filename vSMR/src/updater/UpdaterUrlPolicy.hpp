#pragma once

#include <cstdint>
#include <string>

namespace vsmr::updater::url_policy
{
	struct ParsedHttpsUrl
	{
		std::wstring host;
		std::wstring resource;
		std::uint16_t port = 443;
	};

	// Accepts only HTTPS URLs without credentials or fragments, on the fixed
	// GitHub hosts used by release metadata and asset delivery.
	bool TryParseAllowedHttpsUrl(
		const std::wstring& url,
		ParsedHttpsUrl& result);

	// Resolves the absolute and root-relative redirect forms accepted by the
	// updater, then applies the same host and transport policy as initial URLs.
	bool TryResolveAllowedRedirect(
		const std::wstring& currentUrl,
		const std::wstring& location,
		std::wstring& result);
}
