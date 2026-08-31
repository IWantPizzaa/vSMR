#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"
#include "updater/UpdaterTransport.hpp"
#include "updater/UpdaterUrlPolicy.hpp"
#include "updater/UpdaterVerification.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rapidjson/document.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	using release_model::ChannelAccepts;
	using release_model::CompareSemVer;
	using release_model::ParseSemVer;
	using release_model::SameSemVerIdentity;
	using release_model::SemVer;

	HttpResponse HttpGetTransport(
		Context& context,
		const std::wstring& initialUrl,
		DWORD timeoutMs,
		std::uint64_t maximumBytes,
		const std::string& ifNoneMatch,
		const fs::path& outputFile,
		std::uint64_t expectedSize)
	{
		transport::Request request;
		request.initialUrl = initialUrl;
		request.timeoutMs = timeoutMs;
		request.maximumBytes = maximumBytes;
		request.ifNoneMatch = ifNoneMatch;
		request.outputFile = outputFile;
		request.expectedSize = expectedSize;
		return transport::HttpGet(
			request,
			[&context]() { return context.cancelled; },
			[&context](int percent) {
				return Report(context, ProgressStage::Downloading, percent,
					L"Downloading vSMR update...");
			});
	}

	const ReleaseAsset* FindAsset(const Release& release, const std::string& name)
	{
		for (const auto& asset : release.assets)
		{
			if (asset.name == name)
				return &asset;
		}
		return nullptr;
	}

	std::vector<Release> ParseReleases(const std::vector<std::uint8_t>& bytes)
	{
		std::vector<Release> releases;
		rapidjson::Document document;
		const std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsArray())
			return releases;
		for (rapidjson::SizeType releaseIndex = 0; releaseIndex < document.Size(); ++releaseIndex)
		{
			const rapidjson::Value& item = document[releaseIndex];
			if (!item.IsObject() || JsonBool(item, "draft", true))
				continue;
			Release release;
			release.version = ParseSemVer(JsonString(item, "tag_name"));
			if (!release.version.valid)
				continue;
			release.htmlUrl = JsonString(item, "html_url");
			if (item.HasMember("assets") && item["assets"].IsArray())
			{
				std::set<std::string> names;
				const rapidjson::Value& assets = item["assets"];
				for (rapidjson::SizeType assetIndex = 0; assetIndex < assets.Size(); ++assetIndex)
				{
					const rapidjson::Value& value = assets[assetIndex];
					if (!value.IsObject())
						continue;
					ReleaseAsset asset;
					asset.name = JsonString(value, "name");
					asset.url = Utf8ToWide(JsonString(value, "browser_download_url"));
					asset.size = JsonUint64(value, "size");
					asset.digest = ToLowerAscii(JsonString(value, "digest"));
					url_policy::ParsedHttpsUrl parsed;
					if (asset.name.empty() || !names.insert(asset.name).second ||
						asset.url.empty() ||
						!url_policy::TryParseAllowedHttpsUrl(asset.url, parsed))
					{
						continue;
					}
					release.assets.push_back(std::move(asset));
				}
			}
			releases.push_back(std::move(release));
		}
		return releases;
	}

	std::optional<Release> SelectRelease(
		const std::vector<Release>& releases,
		const SemVer& installed,
		UpdateChannel channel,
		const std::string& skippedVersion,
		const fs::path& storageRoot,
		bool selectInstalledVersion)
	{
		std::optional<Release> selected;
		for (const auto& release : releases)
		{
			const int comparison = CompareSemVer(release.version, installed);
			if (selectInstalledVersion)
			{
				if (comparison != 0)
					continue;
			}
			else if (!ChannelAccepts(release.version, channel) ||
				comparison <= 0 || release.version.normalized == skippedVersion ||
				IsRegularFile(storageRoot / L"quarantine" /
					(Utf8ToWide(release.version.normalized) + L".json")))
			{
				continue;
			}
			const std::string base = "vSMR-" + release.version.normalized;
			if (FindAsset(release, base + ".update.json") == nullptr ||
				FindAsset(release, base + ".update.json.p7s") == nullptr ||
				FindAsset(release, base + ".zip") == nullptr)
			{
				continue;
			}
			if (!selected || CompareSemVer(release.version, selected->version) > 0)
				selected = release;
		}
		return selected;
	}

	bool LoadRemoteReleases(Context& context, std::vector<Release>& releases, std::string& error)
	{
		std::string etag;
		ReadText(context.storageRoot / L"releases.etag", etag, 1024);
		if (etag.find('\r') != std::string::npos || etag.find('\n') != std::string::npos)
			etag.clear();
		const DWORD timeout = RemainingMs(context, kMetadataTimeoutMs);
		if (timeout < 1000)
		{
			error = "deadline";
			return false;
		}
		HttpResponse response = HttpGetTransport(
			context, kApiUrl, timeout, kMaximumMetadataBytes, etag);
		std::vector<std::uint8_t> body;
		if (response.statusCode == 304)
		{
			if (!ReadBytes(context.storageRoot / L"releases-cache.json", body, kMaximumMetadataBytes))
			{
				error = "cache_missing";
				return false;
			}
		}
		else if (response.statusCode == 200 && response.error.empty())
		{
			body = std::move(response.body);
			AtomicWrite(context.storageRoot / L"releases-cache.json", body.data(), body.size());
			if (!response.etag.empty() && response.etag.size() <= 512 &&
				response.etag.find('\r') == std::string::npos && response.etag.find('\n') == std::string::npos)
			{
				AtomicWriteText(context.storageRoot / L"releases.etag", response.etag);
			}
		}
		else
		{
			if (response.statusCode == 403 || response.statusCode == 429)
			{
				context.state.status = "rate_limited";
				context.state.nextCheckUtc = UtcAfterSeconds(15 * 60);
				error = "github_rate_limited";
			}
			else
			{
				error = response.error.empty()
					? "github_http_" + std::to_string(response.statusCode)
					: response.error;
			}
			return false;
		}
		releases = ParseReleases(body);
		if (releases.empty())
		{
			error = "release_response_invalid";
			return false;
		}
		return true;
	}

	bool ParseManifest(
		const std::vector<std::uint8_t>& bytes,
		Manifest& manifest,
		std::string& error)
	{
		rapidjson::Document document;
		const std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
		{
			error = "manifest_json_invalid";
			return false;
		}
		if (!document.HasMember("schema_version") || !document["schema_version"].IsInt() ||
			document["schema_version"].GetInt() != 1 || JsonString(document, "product") != "vSMR")
		{
			error = "manifest_schema_unsupported";
			return false;
		}
		manifest.version = ParseSemVer(JsonString(document, "version"));
		manifest.publishable = JsonBool(document, "publishable", false);
		manifest.channel = ToLowerAscii(JsonString(document, "channel"));
		manifest.minimumLoaderVersion = ParseSemVer(JsonString(document, "minimum_loader_version"));
		manifest.runtimeRelativePath = JsonString(document, "runtime_relative_path");
		const std::uint64_t runtimeAbi = JsonUint64(document, "runtime_abi");
		manifest.runtimeAbi = runtimeAbi <= (std::numeric_limits<std::uint32_t>::max)()
			? static_cast<std::uint32_t>(runtimeAbi) : 0;
		if (!manifest.publishable || !manifest.version.valid || !manifest.minimumLoaderVersion.valid ||
			(manifest.channel != "stable" && manifest.channel != "beta") ||
			(manifest.version.prerelease.empty() ? manifest.channel != "stable" : manifest.channel != "beta") ||
			manifest.runtimeRelativePath != "vSMR_Data/Runtime/vSMR.Runtime.dll" ||
			manifest.runtimeAbi == 0)
		{
			error = "manifest_fields_invalid";
			return false;
		}
		if (!document.HasMember("archive") || !document["archive"].IsObject())
		{
			error = "manifest_archive_missing";
			return false;
		}
		const auto& archive = document["archive"];
		manifest.archiveName = JsonString(archive, "name");
		manifest.archiveSize = JsonUint64(archive, "size");
		manifest.archiveSha256 = ToLowerAscii(JsonString(archive, "sha256"));
		if (manifest.archiveName != "vSMR-" + manifest.version.normalized + ".zip" ||
			manifest.archiveSize == 0 || manifest.archiveSize > kMaximumArchiveBytes ||
			!IsHex(manifest.archiveSha256, 64))
		{
			error = "manifest_archive_invalid";
			return false;
		}
		if (!document.HasMember("loader") || !document["loader"].IsObject())
		{
			error = "manifest_loader_missing";
			return false;
		}
		const auto& loader = document["loader"];
		manifest.loaderName = JsonString(loader, "name");
		manifest.loaderVersion = JsonString(loader, "version");
		manifest.loaderSize = JsonUint64(loader, "size");
		manifest.loaderSha256 = ToLowerAscii(JsonString(loader, "sha256"));
		const SemVer packagedLoaderVersion = ParseSemVer(manifest.loaderVersion);
		if (manifest.loaderName != "vSMR.dll" ||
			!packagedLoaderVersion.valid ||
			CompareSemVer(packagedLoaderVersion, manifest.minimumLoaderVersion) < 0 ||
			manifest.loaderSize == 0 || manifest.loaderSize > 32ULL * 1024ULL * 1024ULL ||
			!IsHex(manifest.loaderSha256, 64))
		{
			error = "manifest_loader_invalid";
			return false;
		}
		return true;
	}

	bool ValidateManifestForRelease(
		const Manifest& manifest,
		const Release& release,
		const ReleaseAsset& archiveAsset,
		std::string& error)
	{
		if (!SameSemVerIdentity(
			manifest.version.normalized, release.version.normalized))
		{
			error = "manifest_version_mismatch";
			return false;
		}
		if (manifest.archiveName != archiveAsset.name || manifest.archiveSize != archiveAsset.size)
		{
			error = "manifest_asset_mismatch";
			return false;
		}
		if (!archiveAsset.digest.empty() &&
			archiveAsset.digest != "sha256:" + manifest.archiveSha256)
		{
			error = "github_asset_digest_mismatch";
			return false;
		}
		return true;
	}

	bool LoadAndVerifyRemoteManifest(
		Context& context,
		const Release& release,
		const std::string& trustedSigner,
		Manifest& manifest,
		std::vector<std::uint8_t>& manifestBytes,
		std::string& error)
	{
		const std::string base = "vSMR-" + release.version.normalized;
		const ReleaseAsset* manifestAsset = FindAsset(release, base + ".update.json");
		const ReleaseAsset* signatureAsset = FindAsset(release, base + ".update.json.p7s");
		const ReleaseAsset* archiveAsset = FindAsset(release, base + ".zip");
		if (manifestAsset == nullptr || signatureAsset == nullptr || archiveAsset == nullptr ||
			manifestAsset->size == 0 || manifestAsset->size > kMaximumManifestBytes ||
			signatureAsset->size == 0 || signatureAsset->size > kMaximumSignatureBytes)
		{
			error = "release_assets_missing";
			return false;
		}
		DWORD timeout = RemainingMs(context, kAssetMetadataTimeoutMs);
		if (timeout < 1000)
		{
			error = "deadline";
			return false;
		}
		HttpResponse manifestResponse = HttpGetTransport(
			context, manifestAsset->url, timeout, kMaximumManifestBytes);
		if (manifestResponse.statusCode != 200 || !manifestResponse.error.empty() ||
			manifestResponse.body.size() != manifestAsset->size)
		{
			error = manifestResponse.error.empty() ? "manifest_download_failed" : manifestResponse.error;
			return false;
		}
		timeout = RemainingMs(context, kAssetMetadataTimeoutMs);
		if (timeout < 1000)
		{
			error = "deadline";
			return false;
		}
		HttpResponse signatureResponse = HttpGetTransport(
			context, signatureAsset->url, timeout, kMaximumSignatureBytes);
		if (signatureResponse.statusCode != 200 || !signatureResponse.error.empty() ||
			signatureResponse.body.size() != signatureAsset->size)
		{
			error = signatureResponse.error.empty() ? "signature_download_failed" : signatureResponse.error;
			return false;
		}
		if (!verification::VerifyDetachedCms(
			manifestResponse.body, signatureResponse.body,
			trustedSigner, error))
		{
			return false;
		}
		manifestBytes = std::move(manifestResponse.body);
		if (!ParseManifest(manifestBytes, manifest, error) ||
			!ValidateManifestForRelease(manifest, release, *archiveAsset, error))
		{
			return false;
		}
		return true;
	}

	std::optional<FixtureCandidate> SelectFixture(
		const StartupOptions& options,
		const SemVer& installed,
		UpdateChannel channel,
		const std::string& skippedVersion,
		const fs::path& storageRoot,
		bool selectInstalledVersion,
		std::string& error)
	{
		if (options.testFeedDirectory.empty())
			return std::nullopt;
		if (!options.allowUnsignedTestManifest)
		{
			error = "unsigned_test_feed_not_enabled";
			return std::nullopt;
		}
		std::error_code filesystemError;
		if (!fs::is_directory(options.testFeedDirectory, filesystemError) || filesystemError)
		{
			error = "test_feed_missing";
			return std::nullopt;
		}
		std::optional<FixtureCandidate> selected;
		for (fs::directory_iterator iterator(options.testFeedDirectory, filesystemError), end;
			!filesystemError && iterator != end; iterator.increment(filesystemError))
		{
			if (!iterator->is_regular_file(filesystemError) || filesystemError)
				continue;
			const std::wstring name = iterator->path().filename().wstring();
			if (!EndsWithNoCase(name, L".update.json"))
				continue;
			FixtureCandidate candidate;
			candidate.manifestPath = iterator->path();
			if (!ReadBytes(candidate.manifestPath, candidate.manifestBytes, kMaximumManifestBytes))
				continue;
			std::string parseError;
			if (!ParseManifest(candidate.manifestBytes, candidate.manifest, parseError))
			{
				continue;
			}
			const int comparison = CompareSemVer(candidate.manifest.version, installed);
			if (selectInstalledVersion)
			{
				if (comparison != 0)
					continue;
			}
			else if (!ChannelAccepts(candidate.manifest.version, channel) ||
				comparison <= 0 || candidate.manifest.version.normalized == skippedVersion ||
				IsRegularFile(storageRoot / L"quarantine" /
					(Utf8ToWide(candidate.manifest.version.normalized) + L".json")))
			{
				continue;
			}
			candidate.archivePath = options.testFeedDirectory / Utf8ToWide(candidate.manifest.archiveName);
			if (!IsRegularFile(candidate.archivePath))
				continue;
			if (!selected || CompareSemVer(candidate.manifest.version, selected->manifest.version) > 0)
				selected = std::move(candidate);
		}
		if (filesystemError)
			error = "test_feed_enumeration_failed";
		else if (!selected)
			error.clear();
		return selected;
	}

}
