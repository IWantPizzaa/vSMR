#include "UpdaterUrlPolicyTests.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "updater/UpdaterTransport.hpp"
#include "updater/UpdaterUrlPolicy.hpp"
#include "updater/UpdaterVerification.hpp"

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
	const std::array<std::wstring, 6> allowedUrls = {
		L"https://api.github.com/repos/IWantPizzaa/vSMR/releases?per_page=30",
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v1/package.zip",
		L"https://release-assets.githubusercontent.com/github-production-release-asset/file",
		L"https://objects.githubusercontent.com/github-production-release-asset/file",
		L"https://github-releases.githubusercontent.com/file",
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

	const std::array<std::wstring, 13> rejectedUrls = {
		L"",
		L"http://github.com/file",
		L"https://github.com:444/file",
		L"https://user@github.com/file",
		L"https://user:password@github.com/file",
		L"https://github.com/file#fragment",
		L"https://githubusercontent.com/file",
		L"https://notgithubusercontent.com/file",
		L"https://github.com.example.invalid/file",
		L"https://example.invalid/file",
		L"https://raw.githubusercontent.com/file",
		L"https://arbitrary.githubusercontent.com/file",
		std::wstring(L"https://github.com/file\0suffix", 30) };
	for (const std::wstring& url : rejectedUrls)
	{
		ParsedHttpsUrl rejected;
		Check(
			!TryParseAllowedHttpsUrl(url, rejected),
			"updater URL policy rejects a URL outside the release security boundary",
			failures);
	}

	std::wstring redirect;
	using vsmr::updater::url_policy::IsProjectReleaseAssetUrl;
	Check(IsProjectReleaseAssetUrl(
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.6/vSMR-2.0.0-beta.6.zip",
		L"2.0.0-beta.6", L"vSMR-2.0.0-beta.6.zip"),
		"release assets are bound to the project and normalized tag", failures);
	for (const auto& url : {
		L"https://github.com/attacker/vSMR/releases/download/v2.0.0-beta.6/vSMR-2.0.0-beta.6.zip",
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.5/vSMR-2.0.0-beta.6.zip",
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.6/other.zip",
		L"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.6/vSMR-2.0.0-beta.6.zip?redirect=evil",
		L"https://objects.githubusercontent.com/vSMR-2.0.0-beta.6.zip" })
		Check(!IsProjectReleaseAssetUrl(url, L"2.0.0-beta.6", L"vSMR-2.0.0-beta.6.zip"),
			"initial assets reject foreign repositories, tags, names and CDN URLs", failures);
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

	using namespace vsmr::updater::internal;
	using vsmr::updater::verification::RequiresManifestSignature;
	Check(!RequiresManifestSignature(false, false) && RequiresManifestSignature(false, true) &&
		RequiresManifestSignature(true, false) && RequiresManifestSignature(true, true),
		"unsigned updates are accepted only when neither manifest nor installation requires signing", failures);
	const std::string manifestJson = R"json({"schema_version":1,"product":"vSMR","version":"2.0.0-beta.6",
		"publishable":true,"signature_required":false,"channel":"beta","minimum_loader_version":"1.2.0",
		"runtime_relative_path":"vSMR_Data/Runtime/vSMR.Runtime.dll","runtime_abi":1,
		"archive":{"name":"vSMR-2.0.0-beta.6.zip","size":123,"sha256":")json" + std::string(64, 'a') +
		R"json("},"loader":{"name":"vSMR.dll","version":"1.2.0","size":12,"sha256":")json" + std::string(64, 'b') + "\"}}";
	auto bytes = [](const std::string& json) { return std::vector<std::uint8_t>(json.begin(), json.end()); };
	Manifest manifest;
	std::string manifestError;
	Check(ParseManifest(bytes(manifestJson), manifest, manifestError) && !manifest.signatureRequired,
		"manifest parser accepts explicit unsigned beta releases", failures);
	std::string legacy = manifestJson;
	legacy.erase(legacy.find("\"signature_required\":false,"), std::string("\"signature_required\":false,").size());
	Check(ParseManifest(bytes(legacy), manifest, manifestError) && manifest.signatureRequired,
		"legacy manifests still require signatures", failures);
	std::string malformed = manifestJson;
	malformed.replace(malformed.find("\"signature_required\":false"), std::string("\"signature_required\":false").size(),
		"\"signature_required\":\"false\"");
	Check(!ParseManifest(bytes(malformed), manifest, manifestError), "malformed signature policy rejected", failures);
	malformed = manifestJson;
	malformed.replace(malformed.find("\"publishable\":true"), std::string("\"publishable\":true").size(), "\"publishable\":false");
	Check(!ParseManifest(bytes(malformed), manifest, manifestError), "unsigned validation manifests remain rejected", failures);
	const std::string releaseJson = R"json([{"draft":false,"tag_name":"v2.0.0-beta.6","assets":[
		{"name":"vSMR-2.0.0-beta.6.zip","size":123,"browser_download_url":"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.6/vSMR-2.0.0-beta.6.zip"},
		{"name":"vSMR-2.0.0-beta.6.update.json","size":500,"browser_download_url":"https://github.com/IWantPizzaa/vSMR/releases/download/v2.0.0-beta.6/vSMR-2.0.0-beta.6.update.json"}]}])json";
	const auto releases = ParseReleases(bytes(releaseJson));
	Check(releases.size() == 1 && releases[0].assets.size() == 2 &&
		SelectRelease(releases, ParseSemVer("2.0.0-beta.5"), UpdateChannel::Beta, "", {}, false).has_value() &&
		!SelectRelease(releases, ParseSemVer("2.0.0-beta.5"), UpdateChannel::Stable, "", {}, false).has_value(),
		"beta feed discovers unsigned two-asset releases without exposing them to stable users", failures);
	if (!releases.empty() && !releases[0].assets.empty())
	{
		Check(ParseManifest(bytes(manifestJson), manifest, manifestError) &&
			ValidateManifestForRelease(manifest, releases[0], releases[0].assets[0], manifestError),
			"unsigned manifest must still match its release and asset", failures);
		auto asset = releases[0].assets[0];
		asset.digest = "sha256:" + std::string(64, 'c');
		Check(!ValidateManifestForRelease(manifest, releases[0], asset, manifestError),
			"GitHub digest mismatch rejected for unsigned updates", failures);
	}
	return failures;
}
