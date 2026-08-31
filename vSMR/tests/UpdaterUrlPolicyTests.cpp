#include "UpdaterUrlPolicyTests.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "updater/UpdaterTransport.hpp"
#include "updater/UpdaterUrlPolicy.hpp"

#include <array>

namespace
{
	void Check(
		bool condition,
		const char* message,
		std::vector<std::string>& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}
}

std::vector<std::string> RunUpdaterUrlPolicyTests()
{
	using vsmr::updater::url_policy::ParsedHttpsUrl;
	using vsmr::updater::url_policy::TryParseAllowedHttpsUrl;
	using vsmr::updater::url_policy::TryResolveAllowedRedirect;

	std::vector<std::string> failures;
	const std::array<std::wstring, 7> allowedUrls = {
		L"https://api.github.com/repos/IWantPizzaa/vSMR/releases?per_page=30",
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v1/package.zip",
		L"https://release-assets.githubusercontent.com/github-production-release-asset/file",
		L"https://objects.githubusercontent.com/github-production-release-asset/file",
		L"https://github-releases.githubusercontent.com/file",
		L"https://raw.githubusercontent.com/IWantPizzaa/vSMR/dev/file",
		L"https://API.GITHUB.COM:443/" };
	for (const std::wstring& url : allowedUrls)
	{
		ParsedHttpsUrl parsed;
		Check(
			TryParseAllowedHttpsUrl(url, parsed) &&
				!parsed.host.empty() && !parsed.resource.empty() && parsed.port == 443,
			"updater URL policy accepts an approved HTTPS release URL",
			failures);
	}

	ParsedHttpsUrl parsed;
	Check(
		TryParseAllowedHttpsUrl(L"https://github.com/releases?download=1", parsed) &&
			parsed.resource == L"/releases?download=1",
		"updater URL policy preserves the request path and query string",
		failures);
	Check(
		TryParseAllowedHttpsUrl(L"https://github.com", parsed) &&
			parsed.resource == L"/",
		"updater URL policy supplies the root resource for a host-only URL",
		failures);

	const std::array<std::wstring, 10> rejectedUrls = {
		L"",
		L"http://github.com/file",
		L"https://github.com:444/file",
		L"https://user@github.com/file",
		L"https://user:password@github.com/file",
		L"https://github.com/file#fragment",
		L"https://githubusercontent.com/file",
		L"https://notgithubusercontent.com/file",
		L"https://github.com.example.invalid/file",
		L"https://example.invalid/file" };
	for (const std::wstring& url : rejectedUrls)
	{
		ParsedHttpsUrl rejected;
		Check(
			!TryParseAllowedHttpsUrl(url, rejected),
			"updater URL policy rejects a URL outside the release security boundary",
			failures);
	}

	std::wstring redirect;
	Check(
		TryResolveAllowedRedirect(
			L"https://api.github.com/releases",
			L"HTTPS://github.com/IWantPizzaa/vSMR/releases/download/file.zip",
			redirect) &&
		redirect.rfind(L"HTTPS://", 0) == 0,
		"updater URL policy accepts case-insensitive absolute HTTPS redirects",
		failures);
	Check(
		TryResolveAllowedRedirect(
			L"https://api.github.com/releases",
			L"/repos/IWantPizzaa/vSMR/releases",
			redirect) &&
		redirect == L"https://api.github.com/repos/IWantPizzaa/vSMR/releases",
		"updater URL policy resolves root-relative redirects on the approved host",
		failures);
	Check(
		!TryResolveAllowedRedirect(
			L"https://api.github.com/releases",
			L"https://example.invalid/package.zip",
			redirect),
		"updater URL policy rejects redirects to an unapproved host",
		failures);

	using vsmr::updater::transport::policy::ClassifyTimeoutSetup;
	using vsmr::updater::transport::policy::TimeoutSetupStatus;
	using vsmr::updater::transport::policy::WouldExceedMaximumBytes;
	Check(
		ClassifyTimeoutSetup(0, false) == TimeoutSetupStatus::DeadlineExpired &&
		ClassifyTimeoutSetup(1000, false) == TimeoutSetupStatus::ConfigurationFailed &&
		ClassifyTimeoutSetup(1000, true) == TimeoutSetupStatus::Ready,
		"updater transport distinguishes deadline expiry from timeout configuration failure",
		failures);
	Check(
		WouldExceedMaximumBytes(0, 1, 0) &&
		!WouldExceedMaximumBytes(9, 1, 10) &&
		!WouldExceedMaximumBytes(10, 0, 10) &&
		WouldExceedMaximumBytes(10, 1, 10) &&
		WouldExceedMaximumBytes(11, 0, 10),
		"updater transport enforces byte limits without unsigned underflow",
		failures);
	using vsmr::updater::internal::ClassifyProcessFailureExitCode;
	Check(
		ClassifyProcessFailureExitCode(true, ERROR_SUCCESS) == ERROR_TIMEOUT &&
		ClassifyProcessFailureExitCode(false, ERROR_ACCESS_DENIED) == ERROR_ACCESS_DENIED &&
		ClassifyProcessFailureExitCode(false, ERROR_SUCCESS) == ERROR_GEN_FAILURE,
		"updater process failures preserve Windows errors and classify deadline expiry",
		failures);

	using vsmr::updater::UpdateChannel;
	using vsmr::updater::release_model::ChannelAccepts;
	using vsmr::updater::release_model::CompareSemVer;
	using vsmr::updater::release_model::ParseSemVer;
	using vsmr::updater::release_model::SameSemVerIdentity;

	const auto stable = ParseSemVer("v1.4.2");
	const auto beta = ParseSemVer("1.4.3-beta.2");
	Check(
		stable.valid && stable.normalized == "1.4.2" &&
		stable.major == 1 && stable.minor == 4 && stable.patch == 2,
		"updater release model normalizes a valid prefixed version",
		failures);
	Check(
		beta.valid && CompareSemVer(stable, beta) < 0 &&
		!ChannelAccepts(beta, UpdateChannel::Stable) &&
		ChannelAccepts(beta, UpdateChannel::Beta),
		"updater release model orders versions and enforces channels",
		failures);
	Check(
		CompareSemVer(ParseSemVer("1.0.0-rc.2"), ParseSemVer("1.0.0-rc.10")) < 0 &&
		CompareSemVer(ParseSemVer("1.0.0-rc.10"), ParseSemVer("1.0.0")) < 0,
		"updater release model follows semantic prerelease precedence",
		failures);
	Check(
		!ParseSemVer("1.02.3").valid && !ParseSemVer("1.2").valid &&
		!ParseSemVer("1.2.3-").valid && !ParseSemVer("1.2.3+").valid,
		"updater release model rejects malformed versions",
		failures);
	Check(
		SameSemVerIdentity("v1.2.3-beta", "1.2.3-beta") &&
		!SameSemVerIdentity("1.2.3", "1.2.4"),
		"updater release model compares normalized release identities",
		failures);

	return failures;
}
