#define VSMR_UPDATE_SIGNER_CERT_SHA256 ""
#include "../../src/updater/UpdaterCore.cpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")

namespace vsmr::updater::harness
{
	constexpr char kOldVersion[] = "2.0.0-beta.3";
	constexpr char kNewVersion[] = "2.0.0-beta.4";

	struct Inputs
	{
		fs::path workspace;
		fs::path scratch;
		fs::path loaderBinary;
		fs::path baseArchive;
		fs::path traversalArchive;
		fs::path duplicateArchive;
		fs::path oversizedArchive;
		fs::path cmsContent;
		fs::path cmsSignature;
		std::string cmsSignerSha256;
		std::string cmsWrongSignerSha256;
	};

	struct Environment
	{
		fs::path root;
		fs::path install;
		fs::path feed;
		fs::path storage;
	};

	class DirectoryWriteDeny
	{
	public:
		DirectoryWriteDeny() = default;
		~DirectoryWriteDeny()
		{
			Restore();
			if (deniedAcl_ != nullptr)
				::LocalFree(deniedAcl_);
			if (originalDescriptor_ != nullptr)
				::LocalFree(originalDescriptor_);
			if (worldSid_ != nullptr)
				::FreeSid(worldSid_);
		}
		DirectoryWriteDeny(const DirectoryWriteDeny&) = delete;
		DirectoryWriteDeny& operator=(const DirectoryWriteDeny&) = delete;

		bool Apply(const fs::path& path)
		{
			path_ = path;
			if (::GetNamedSecurityInfoW(
				const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
				DACL_SECURITY_INFORMATION, nullptr, nullptr, &originalAcl_, nullptr,
				&originalDescriptor_) != ERROR_SUCCESS)
			{
				return false;
			}
			SID_IDENTIFIER_AUTHORITY authority = SECURITY_WORLD_SID_AUTHORITY;
			if (!::AllocateAndInitializeSid(
				&authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &worldSid_))
			{
				return false;
			}
			EXPLICIT_ACCESSW access{};
			access.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD;
			access.grfAccessMode = DENY_ACCESS;
			access.grfInheritance = NO_INHERITANCE;
			::BuildTrusteeWithSidW(&access.Trustee, worldSid_);
			if (::SetEntriesInAclW(1, &access, originalAcl_, &deniedAcl_) != ERROR_SUCCESS)
				return false;
			if (::SetNamedSecurityInfoW(
				const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
				DACL_SECURITY_INFORMATION, nullptr, nullptr, deniedAcl_, nullptr) != ERROR_SUCCESS)
			{
				return false;
			}
			active_ = true;
			return true;
		}

		bool Restore() noexcept
		{
			if (!active_)
				return true;
			const DWORD status = ::SetNamedSecurityInfoW(
				const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
				DACL_SECURITY_INFORMATION, nullptr, nullptr, originalAcl_, nullptr);
			if (status == ERROR_SUCCESS)
				active_ = false;
			return status == ERROR_SUCCESS;
		}

	private:
		fs::path path_;
		PSECURITY_DESCRIPTOR originalDescriptor_ = nullptr;
		PACL originalAcl_ = nullptr;
		PACL deniedAcl_ = nullptr;
		PSID worldSid_ = nullptr;
		bool active_ = false;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	void WriteUtf8(const fs::path& path, const std::string& value)
	{
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		if (error)
			throw std::runtime_error("cannot create fixture directory");
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
			throw std::runtime_error("cannot write fixture file");
		output.write(value.data(), static_cast<std::streamsize>(value.size()));
		if (!output.good())
			throw std::runtime_error("fixture write failed");
	}

	void CopyRequired(const fs::path& source, const fs::path& destination)
	{
		std::error_code error;
		fs::create_directories(destination.parent_path(), error);
		if (error || !fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error) || error)
			throw std::runtime_error("fixture copy failed");
	}

	void AppendBytes(const fs::path& path, const std::string& bytes)
	{
		std::ofstream output(path, std::ios::binary | std::ios::app);
		if (!output.is_open())
			throw std::runtime_error("cannot append fixture marker");
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}

	std::string Hash(const fs::path& path)
	{
		std::string digest;
		Require(Sha256File(path, digest), "SHA-256 failed");
		return digest;
	}

	std::uint64_t Size(const fs::path& path)
	{
		std::error_code error;
		const auto value = fs::file_size(path, error);
		Require(!error, "file size failed");
		return value;
	}

	std::string ReleaseMetadata(
		const std::string& version,
		const fs::path& loader,
		const fs::path& runtime)
	{
		std::ostringstream json;
		json << "{\n"
			<< "  \"schema_version\": 1,\n"
			<< "  \"product\": \"vSMR\",\n"
			<< "  \"version\": \"" << version << "\",\n"
			<< "  \"publishable\": true,\n"
			<< "  \"git_commit\": \"offline-updater-harness\",\n"
			<< "  \"loader\": {\"relative_path\": \"vSMR.dll\", \"version\": \"1.0.0\", \"size\": "
			<< Size(loader) << ", \"sha256\": \"" << Hash(loader) << "\"},\n"
			<< "  \"runtime\": {\"relative_path\": \"vSMR_Data/Runtime/vSMR.Runtime.dll\", \"version\": \""
			<< version << "\", \"abi\": 1, \"size\": " << Size(runtime)
			<< ", \"sha256\": \"" << Hash(runtime) << "\"},\n"
			<< "  \"automatic_update\": {\"publishable\": true}\n"
			<< "}\n";
		return json.str();
	}

	Environment CreateEnvironment(const Inputs& inputs, const std::string& name)
	{
		Environment environment;
		environment.root = inputs.scratch / Utf8ToWide(name);
		environment.install = environment.root / L"install";
		environment.feed = environment.root / L"feed";
		environment.storage = environment.root / L"storage";
		std::error_code error;
		fs::remove_all(environment.root, error);
		error.clear();
		fs::create_directories(environment.install / L"vSMR_Data" / L"Runtime", error);
		Require(!error, "cannot create isolated install");
		fs::create_directories(environment.feed, error);
		Require(!error, "cannot create isolated feed");
		fs::create_directories(environment.storage, error);
		Require(!error, "cannot create isolated storage");

		CopyRequired(inputs.loaderBinary, environment.install / L"vSMR.dll");
		const fs::path runtime = environment.install / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
		CopyRequired(inputs.loaderBinary, runtime);
		AppendBytes(runtime, "vsmr-offline-old-runtime");
		WriteUtf8(
			environment.install / L"vSMR_Data" / L"RELEASE-METADATA.json",
			ReleaseMetadata(kOldVersion, environment.install / L"vSMR.dll", runtime));
		WriteUtf8(environment.install / L"vSMR_Data" / L"user-preserved.txt", "user-data\n");
		return environment;
	}

	void WriteConfig(
		const Environment& environment,
		bool autoCheck,
		bool autoDownload,
		bool autoInstall,
		const char* channel = "beta",
		bool protectModifiedAviso = true)
	{
		std::ostringstream json;
		json << "{\"schema_version\":1,\"auto_check\":" << (autoCheck ? "true" : "false")
			<< ",\"auto_download\":" << (autoDownload ? "true" : "false")
			<< ",\"auto_install\":" << (autoInstall ? "true" : "false")
			<< ",\"protect_modified_aviso\":" << (protectModifiedAviso ? "true" : "false")
			<< ",\"channel\":\"" << channel << "\",\"skipped_version\":\"\"}\n";
		WriteUtf8(environment.storage / L"config.json", json.str());
	}

	void WriteAction(const Environment& environment, const char* action)
	{
		std::ostringstream json;
		json << "{\"schema_version\":1,\"request_id\":\"offline-harness\",\"action\":\""
			<< action << "\",\"requested_utc\":\"2026-08-18T00:00:00Z\"}\n";
		WriteUtf8(environment.storage / L"action.json", json.str());
	}

	void AddManifest(
		const Inputs& inputs,
		const Environment& environment,
		const std::string& version = kNewVersion,
		const std::string& minimumLoaderVersion = "1.0.0",
		const std::string& packagedLoaderVersion = "1.0.0",
		bool corruptArchiveHash = false)
	{
		const std::string archiveName = "vSMR-" + version + ".zip";
		const fs::path archive = environment.feed / Utf8ToWide(archiveName);
		CopyRequired(inputs.baseArchive, archive);
		std::string archiveHash = Hash(archive);
		if (corruptArchiveHash)
			archiveHash.assign(64, '0');
		const std::string channel = ParseSemVer(version).prerelease.empty() ? "stable" : "beta";
		std::ostringstream json;
		json << "{\n"
			<< "  \"schema_version\": 1,\n"
			<< "  \"product\": \"vSMR\",\n"
			<< "  \"version\": \"" << version << "\",\n"
			<< "  \"channel\": \"" << channel << "\",\n"
			<< "  \"publishable\": true,\n"
			<< "  \"archive\": {\"name\": \"" << archiveName << "\", \"size\": "
			<< Size(archive) << ", \"sha256\": \"" << archiveHash << "\"},\n"
			<< "  \"loader\": {\"name\": \"vSMR.dll\", \"version\": \""
			<< packagedLoaderVersion << "\", \"size\": " << Size(inputs.loaderBinary)
			<< ", \"sha256\": \"" << Hash(inputs.loaderBinary) << "\"},\n"
			<< "  \"minimum_loader_version\": \"" << minimumLoaderVersion << "\",\n"
			<< "  \"runtime_abi\": 1,\n"
			<< "  \"runtime_relative_path\": \"vSMR_Data/Runtime/vSMR.Runtime.dll\"\n"
			<< "}\n";
		WriteUtf8(
			environment.feed / Utf8ToWide("vSMR-" + version + ".update.json"),
			json.str());
	}

	StartupOptions Options(
		const Environment& environment,
		const std::string& currentVersion = kOldVersion)
	{
		StartupOptions options;
		options.installRoot = environment.install;
		options.dataRoot = environment.install / L"vSMR_Data";
		options.canonicalRuntimePath = options.dataRoot / L"Runtime" / L"vSMR.Runtime.dll";
		options.loaderPath = environment.install / L"vSMR.dll";
		options.currentVersion = currentVersion;
		options.loaderVersion = "1.0.0";
		options.defaultChannel = UpdateChannel::Beta;
		options.hostProcessId = ::GetCurrentProcessId();
		options.expectedRuntimeAbi = 1;
		options.overallDeadlineMs = 30000;
		options.testFeedDirectory = environment.feed;
		options.testStorageDirectory = environment.storage;
		options.allowUnsignedTestManifest = true;
		return options;
	}

	std::string ActiveVersion(const Environment& environment)
	{
		std::string version;
		Require(ReadReleaseVersion(environment.install / L"vSMR_Data", version), "active version unreadable");
		return version;
	}

	std::string RequiredText(const fs::path& path)
	{
		std::string value;
		Require(ReadText(path, value, 1024 * 1024), "required text fixture could not be read");
		return value;
	}

	bool JsonStringArrayContains(
		const rapidjson::Value& object,
		const char* member,
		const char* expected)
	{
		if (!object.IsObject() || !object.HasMember(member) || !object[member].IsArray())
			return false;
		const rapidjson::Value& values = object[member];
		for (rapidjson::SizeType index = 0; index < values.Size(); ++index)
		{
			const rapidjson::Value& item = values[index];
			if (item.IsString() && std::string(item.GetString(), item.GetStringLength()) == expected)
				return true;
		}
		return false;
	}

	void RequireAvisoReloadReport(
		const Environment& environment,
		bool expectedProtection,
		const char* expectedResultMember)
	{
		const std::string json = RequiredText(
			environment.install / L"vSMR_Data" / L"AVISO-UPDATE-REPORT.json");
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		Require(!document.HasParseError() && document.IsObject(), "AVISO reload report is invalid");
		Require(JsonString(document, "operation") == "manual_reload",
			"installer did not record manual AVISO reload semantics");
		Require(JsonString(document, "policy") == "all",
			"manual AVISO reload did not request all packaged maps");
		Require(document.HasMember("protected_modified_files") &&
			document["protected_modified_files"].IsBool() &&
			document["protected_modified_files"].GetBool() == expectedProtection,
			"installer did not receive the requested AVISO protection setting");
		Require(JsonStringArrayContains(document, expectedResultMember, "TEST.geojson"),
			"AVISO reload report did not record TEST.geojson in the expected result");
	}

	void EditJson(const fs::path& path, const std::function<void(rapidjson::Document&)>& edit)
	{
		std::string json;
		Require(ReadText(path, json, 128 * 1024), "JSON fixture could not be read");
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		Require(!document.HasParseError() && document.IsObject(), "JSON fixture is invalid");
		edit(document);
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		document.Accept(writer);
		WriteUtf8(path, std::string(buffer.GetString(), buffer.Size()) + "\n");
	}

	StartupResult SyntheticHealthResult(const Environment& environment)
	{
		StartupResult result;
		result.selectedVersion = kNewVersion;
		result.previousVersion = kOldVersion;
		result.previousRuntimeSha256 = Hash(
			environment.install / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll");
		result.rollbackBackupPath = environment.storage / L"backups" / L"synthetic";
		result.previousRuntimePath = result.rollbackBackupPath /
			L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
		return result;
	}

	using Snapshot = std::map<std::wstring, std::pair<std::uint64_t, long long>>;

	Snapshot SnapshotTree(const fs::path& root)
	{
		Snapshot result;
		std::error_code error;
		if (!fs::is_directory(root, error) || error)
			return result;
		for (fs::recursive_directory_iterator iterator(
			root, fs::directory_options::skip_permission_denied, error), end;
			!error && iterator != end; iterator.increment(error))
		{
			if (iterator->is_regular_file(error) && !error)
			{
				const auto relative = fs::relative(iterator->path(), root, error).wstring();
				if (!error)
				{
					const auto size = iterator->file_size(error);
					const auto modified = fs::last_write_time(iterator->path(), error);
					if (!error)
						result.emplace(relative, std::make_pair(size, modified.time_since_epoch().count()));
				}
			}
			error.clear();
		}
		return result;
	}

	void TestSemVerAndChannel(const fs::path& scratch)
	{
		const auto beta2 = ParseSemVer("2.0.0-beta.2");
		const auto beta10 = ParseSemVer("2.0.0-beta.10");
		const auto stable = ParseSemVer("2.0.0");
		Require(beta2.valid && beta10.valid && stable.valid, "valid SemVer rejected");
		Require(CompareSemVer(beta10, beta2) > 0, "numeric prerelease ordering is wrong");
		Require(CompareSemVer(stable, beta10) > 0, "stable must sort after prerelease");
		Require(!ChannelAccepts(beta10, UpdateChannel::Stable), "stable channel accepted beta");
		Require(ChannelAccepts(beta10, UpdateChannel::Beta) &&
			ChannelAccepts(stable, UpdateChannel::Beta), "beta channel selection is wrong");
		Require(!ParseSemVer("2.0.0+bad/path").valid, "unsafe SemVer build suffix accepted");

		const std::string json = R"JSON([
 {"draft":false,"prerelease":false,"tag_name":"2.1.0-beta.1","html_url":"https://github.com/IWantPizzaa/vSMR/releases/1","assets":[
  {"name":"vSMR-2.1.0-beta.1.update.json","browser_download_url":"https://github.com/a","size":1},
  {"name":"vSMR-2.1.0-beta.1.update.json.p7s","browser_download_url":"https://github.com/b","size":1},
  {"name":"vSMR-2.1.0-beta.1.zip","browser_download_url":"https://github.com/c","size":1}]},
 {"draft":false,"prerelease":true,"tag_name":"2.0.1","html_url":"https://github.com/IWantPizzaa/vSMR/releases/2","assets":[
  {"name":"vSMR-2.0.1.update.json","browser_download_url":"https://github.com/d","size":1},
  {"name":"vSMR-2.0.1.update.json.p7s","browser_download_url":"https://github.com/e","size":1},
  {"name":"vSMR-2.0.1.zip","browser_download_url":"https://github.com/f","size":1}]}
])JSON";
		const std::vector<std::uint8_t> bytes(json.begin(), json.end());
		const auto releases = ParseReleases(bytes);
		Require(releases.size() == 2, "release JSON parsing failed");
		const auto stableSelected = SelectRelease(
			releases, ParseSemVer("2.0.0"), UpdateChannel::Stable, "", scratch, false);
		const auto betaSelected = SelectRelease(
			releases, ParseSemVer("2.0.0"), UpdateChannel::Beta, "", scratch, false);
		Require(stableSelected && stableSelected->version.normalized == "2.0.1",
			"GitHub prerelease flag incorrectly affected stable selection");
		Require(betaSelected && betaSelected->version.normalized == "2.1.0-beta.1",
			"beta channel did not choose highest SemVer");
	}

	void TestUnsafeArchives(const Inputs& inputs)
	{
		const std::array<std::pair<const wchar_t*, const fs::path*>, 3> cases = {{
			{ L"traversal", &inputs.traversalArchive },
			{ L"duplicate", &inputs.duplicateArchive },
			{ L"oversized", &inputs.oversizedArchive }
		}};
		for (const auto& item : cases)
		{
			Environment environment = CreateEnvironment(inputs, "unsafe-" + WideToUtf8(item.first));
			StartupOptions options = Options(environment);
			Context context(options);
			std::string error;
			const bool extracted = SafelyExtractArchive(
				context, *item.second, environment.storage / L"extract", error);
			Require(!extracted && error == "extraction_failed", "unsafe ZIP was accepted");
			Require(!IsRegularFile(environment.root / L"escape.txt"), "ZIP traversal escaped destination");
		}
	}

	template <typename Function>
	void Run(const char* name, int& failures, Function&& function)
	{
		try
		{
			function();
			std::cout << "[PASS] " << name << "\n";
		}
		catch (const std::exception& exception)
		{
			++failures;
			std::cerr << "[FAIL] " << name << ": " << exception.what() << "\n";
		}
	}

	int RunAll(const Inputs& inputs)
	{
		int failures = 0;
		const fs::path productionStorage = GetUpdaterStorageDirectory();
		const Snapshot productionBefore = SnapshotTree(productionStorage);

		Run("SemVer/channel ignores GitHub prerelease flag", failures, [&]() {
			TestSemVerAndChannel(inputs.scratch / L"selection-quarantine");
		});

		Run("unsigned production loader rejects update trust", failures, [&]() {
			StartupOptions options;
			options.loaderPath = inputs.loaderBinary;
			Require(ResolveTrustedSignerHash(options).empty(), "unsigned production loader unexpectedly trusted");
		});

		Run("durable storage and session lock identities are stable", failures, [&]() {
			const fs::path storage = GetUpdaterStorageDirectory();
			Require(!storage.empty() && storage == LocalAppDataUpdaterCandidate(),
				"durable updater state switched away from its LocalAppData identity");
			const fs::path lockStorage = GetProductionSessionLockStorageRoot();
			Require(!lockStorage.empty() && ProbeWritableDirectory(lockStorage),
				"no proven-writable deterministic session-lock storage is available");
			const fs::path loaderSharedPath = GetInstallationSessionLockPath(inputs.workspace);
			const fs::path updaterExclusivePath = SessionLockPath(lockStorage, inputs.workspace);
			Require(loaderSharedPath == updaterExclusivePath,
				"loader shared and updater exclusive leases use different lock identities");
		});

		Run("runtime shadow lease blocks substitution and permits loading", failures, [&]() {
			const fs::path root = inputs.scratch / L"shadow-lease";
			const fs::path renamedRoot = inputs.scratch / L"shadow-lease-renamed";
			const fs::path shadow = root / L"vSMR.Runtime.shadow.dll";
			const fs::path replacement = root / L"replacement.dll";
			std::error_code filesystemError;
			fs::remove_all(root, filesystemError);
			filesystemError.clear();
			fs::remove_all(renamedRoot, filesystemError);
			CopyRequired(inputs.loaderBinary, shadow);
			CopyRequired(inputs.loaderBinary, replacement);
			const std::string expectedHash = Hash(shadow);

			UniqueHandle lease(::CreateFileW(
				shadow.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
			Require(static_cast<bool>(lease), "could not acquire finalized shadow lease");

			UniqueHandle writer(::CreateFileW(
				shadow.c_str(), GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			Require(!writer, "shadow lease allowed an overwrite handle");
			Require(!::MoveFileExW(
				replacement.c_str(), shadow.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
				"shadow lease allowed path replacement");
			Require(!::DeleteFileW(shadow.c_str()), "shadow lease allowed deletion");

			const bool parentRenamed = ::MoveFileExW(
				root.c_str(), renamedRoot.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
			if (parentRenamed)
			{
				::MoveFileExW(renamedRoot.c_str(), root.c_str(), MOVEFILE_WRITE_THROUGH);
				throw std::runtime_error("shadow lease allowed parent-directory substitution");
			}
			std::vector<wchar_t> resolvedBuffer(32768U, L'\0');
			const DWORD resolvedLength = ::GetFinalPathNameByHandleW(
				lease.get(), resolvedBuffer.data(),
				static_cast<DWORD>(resolvedBuffer.size()),
				FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			Require(resolvedLength > 0 && resolvedLength < resolvedBuffer.size(),
				"could not resolve the leased shadow path");
			const fs::path resolvedShadow(std::wstring(
				resolvedBuffer.data(), static_cast<std::size_t>(resolvedLength)));
			Require(Hash(resolvedShadow) == expectedHash,
				"leased shadow changed before LoadLibraryExW");

			HMODULE module = ::LoadLibraryExW(
				resolvedShadow.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			Require(module != nullptr,
				"LoadLibraryExW failed while the verified shadow lease was held");
			::FreeLibrary(module);
		});

		Run("detached CMS accepts valid pinned signer", failures, [&]() {
			std::vector<std::uint8_t> content;
			std::vector<std::uint8_t> signature;
			Require(ReadBytes(inputs.cmsContent, content, kMaximumManifestBytes),
				"CMS content fixture could not be read");
			Require(ReadBytes(inputs.cmsSignature, signature, kMaximumSignatureBytes),
				"CMS signature fixture could not be read");
			std::string error;
			Require(VerifyDetachedCms(content, signature, inputs.cmsSignerSha256, error),
				"valid detached CMS was rejected: " + error);
		});

		Run("detached CMS rejects tampered content", failures, [&]() {
			std::vector<std::uint8_t> content;
			std::vector<std::uint8_t> signature;
			Require(ReadBytes(inputs.cmsContent, content, kMaximumManifestBytes) && !content.empty(),
				"CMS content fixture could not be read");
			Require(ReadBytes(inputs.cmsSignature, signature, kMaximumSignatureBytes),
				"CMS signature fixture could not be read");
			content.front() ^= 0x01;
			std::string error;
			Require(!VerifyDetachedCms(content, signature, inputs.cmsSignerSha256, error) &&
				error == "manifest_signature_invalid",
				"tampered detached CMS content was accepted");
		});

		Run("detached CMS rejects wrong signer pin", failures, [&]() {
			std::vector<std::uint8_t> content;
			std::vector<std::uint8_t> signature;
			Require(ReadBytes(inputs.cmsContent, content, kMaximumManifestBytes),
				"CMS content fixture could not be read");
			Require(ReadBytes(inputs.cmsSignature, signature, kMaximumSignatureBytes),
				"CMS signature fixture could not be read");
			std::string error;
			Require(!VerifyDetachedCms(content, signature, inputs.cmsWrongSignerSha256, error) &&
				error == "manifest_signer_mismatch",
				"detached CMS signed by a different pinned signer was accepted");
		});

		Run("no update", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "no-update");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::Current, "empty fixture feed did not stay current");
			Require(ActiveVersion(environment) == kOldVersion, "no-update mutated installation");
		});

		Run("auto-check disabled", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "auto-check-disabled");
			AddManifest(inputs, environment);
			WriteConfig(environment, false, true, true);
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::Current, "auto_check=false was ignored");
			Require(ActiveVersion(environment) == kOldVersion, "disabled check mutated installation");
		});

		Run("check-now respects auto-download=false", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "check-now-no-download");
			AddManifest(inputs, environment);
			WriteConfig(environment, false, false, true);
			WriteAction(environment, "check_now");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::UpdateAvailable, "check_now bypassed download preference");
			Require(ActiveVersion(environment) == kOldVersion, "check_now installed despite auto_download=false");
		});

		Run("auto-install disabled", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "auto-install-disabled");
			AddManifest(inputs, environment);
			WriteConfig(environment, true, true, false);
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::UpdateAvailable, "auto_install=false was ignored");
			Require(ActiveVersion(environment) == kOldVersion, "auto_install=false mutated installation");
			Require(!fs::exists(environment.storage / L"backups"), "backup created before install permission");
		});

		Run("AVISO reload selects installed release and honors protection", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "aviso-reload");
			AddManifest(inputs, environment, kNewVersion);
			const StartupResult initial = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(initial.status == StartupStatus::Updated && initial.updateActivated &&
				ActiveVersion(environment) == kNewVersion,
				"could not establish the installed release for AVISO reload");
			Require(ConfirmRuntimeHealthy(initial), "initial AVISO fixture update was not confirmed healthy");

			const fs::path avisoPath = environment.install /
				L"vSMR_Data" / L"AVISO" / L"TEST.geojson";
			const std::string packagedAviso = RequiredText(avisoPath);
			const std::string locallyModified = "{\"locally_modified\":true}\n";
			WriteUtf8(avisoPath, locallyModified);

			// A newer fixture is deliberately present. reload_aviso must choose the
			// exact installed beta.4 manifest, even on the stable channel. The beta.5
			// manifest points at a beta.4 package and would fail package validation if
			// it were selected accidentally.
			AddManifest(inputs, environment, "2.0.0-beta.5");
			WriteConfig(environment, false, false, false, "stable", true);
			WriteAction(environment, "reload_aviso");
			const StartupResult protectedReload =
				PrepareUpdateBeforeRuntimeLoad(Options(environment, kNewVersion));
			Require(protectedReload.status == StartupStatus::Updated &&
				protectedReload.updateActivated && protectedReload.selectedVersion == kNewVersion &&
				ActiveVersion(environment) == kNewVersion,
				"queued AVISO reload did not install the exact current release");
			Require(protectedReload.message.find(L"AVISO data was reloaded") != std::wstring::npos,
				"AVISO reload did not expose a clear completion message");
			Require(RequiredText(avisoPath) == locallyModified,
				"protected AVISO reload overwrote a locally modified map");
			Require(RequiredText(environment.install / L"vSMR_Data" / L"AVISO_Updates" /
				Utf8ToWide(kNewVersion) / L"TEST.geojson") == packagedAviso,
				"protected AVISO reload did not retain the incoming official map");
			RequireAvisoReloadReport(environment, true, "preserved_modified");
			Require(ConfirmRuntimeHealthy(protectedReload),
				"protected AVISO reload was not confirmed healthy");

			const std::string secondLocalEdit = "{\"locally_modified\":\"overwrite me\"}\n";
			WriteUtf8(avisoPath, secondLocalEdit);
			WriteConfig(environment, false, false, false, "stable", false);
			WriteAction(environment, "reload_aviso");
			const StartupResult replacingReload =
				PrepareUpdateBeforeRuntimeLoad(Options(environment, kNewVersion));
			Require(replacingReload.status == StartupStatus::Updated &&
				replacingReload.updateActivated && replacingReload.selectedVersion == kNewVersion &&
				ActiveVersion(environment) == kNewVersion,
				"explicit replacing AVISO reload did not complete");
			Require(RequiredText(avisoPath) == packagedAviso,
				"disabled AVISO protection did not restore the official map");
			RequireAvisoReloadReport(environment, false, "updated");
			Require(ConfirmRuntimeHealthy(replacingReload),
				"replacing AVISO reload was not confirmed healthy");
		});

		Run("archive hash mismatch", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "hash-mismatch");
			AddManifest(inputs, environment, kNewVersion, "1.0.0", "1.0.0", true);
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::FailedOpen && result.errorCode == "archive_hash_mismatch",
				"archive hash mismatch was not rejected");
			Require(ActiveVersion(environment) == kOldVersion, "hash mismatch mutated installation");
		});

		Run("ZIP traversal/duplicate/size caps", failures, [&]() {
			TestUnsafeArchives(inputs);
		});

		Run("progress cancellation before mutation", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "cancelled");
			AddManifest(inputs, environment);
			StartupOptions options = Options(environment);
			options.progressCallback = [](const Progress& progress) {
				return progress.stage != ProgressStage::Checking;
			};
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(options);
			Require(result.status == StartupStatus::Cancelled, "progress cancellation not honored");
			Require(ActiveVersion(environment) == kOldVersion, "cancelled update mutated installation");
			Require(!fs::exists(environment.storage / L"backups"), "cancelled update created a backup");
		});

		Run("invalid health journals fail closed", failures, [&]() {
			for (const char* variant : { "malformed", "schema", "install-root" })
			{
				const Environment environment = CreateEnvironment(
					inputs, std::string("invalid-journal-") + variant);
				StartupOptions options = Options(environment);
				StartupResult marker = SyntheticHealthResult(environment);
				Require(WriteHealthMarker(options, environment.storage, marker),
					"synthetic health marker could not be written");
				if (std::string(variant) == "malformed")
					WriteUtf8(marker.healthMarkerPath, "{not-json\n");
				else if (std::string(variant) == "schema")
					EditJson(marker.healthMarkerPath, [](rapidjson::Document& document) {
						document["schema_version"].SetInt(999);
					});
				else
					EditJson(marker.healthMarkerPath, [](rapidjson::Document& document) {
						document["install_root"].SetString("C:/wrong-install", document.GetAllocator());
					});
				const StartupResult result = PrepareUpdateBeforeRuntimeLoad(options);
				Require(result.status == StartupStatus::FailedOpen && result.selectedRuntimePath.empty() &&
					result.errorCode == "health_marker_invalid",
					"invalid journal did not fail closed");
			}
		});

		Run("unwritable durable storage does not hide a pending journal", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "unwritable-pending-journal");
			const StartupOptions options = Options(environment);
			StartupResult marker = SyntheticHealthResult(environment);
			Require(WriteHealthMarker(options, environment.storage, marker),
				"synthetic health marker could not be written");
			WriteUtf8(marker.healthMarkerPath, "{not-json\n");
			DirectoryWriteDeny writeDeny;
			Require(writeDeny.Apply(environment.storage),
				"could not make the durable fixture storage read-only");
			Require(!ProbeWritableDirectory(environment.storage),
				"read-only fixture unexpectedly passed the write probe");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(options);
			Require(result.status == StartupStatus::FailedOpen &&
				result.selectedRuntimePath.empty() &&
				result.errorCode == "health_marker_invalid",
				"a temporarily unwritable durable journal was skipped");
			Require(writeDeny.Restore(), "could not restore fixture directory permissions");
		});

		Run("deadline before mutation", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "deadline");
			AddManifest(inputs, environment);
			StartupOptions options = Options(environment);
			options.overallDeadlineMs = 10;
			options.progressCallback = [](const Progress& progress) {
				if (progress.stage == ProgressStage::Checking)
					::Sleep(20);
				return true;
			};
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(options);
			Require(result.status == StartupStatus::FailedOpen && result.errorCode == "deadline",
				"deadline was not enforced");
			Require(ActiveVersion(environment) == kOldVersion, "deadline update mutated installation");
		});

		Run("active shared session defers install", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "active-session");
			AddManifest(inputs, environment);
			const fs::path lockPath = SessionLockPath(environment.storage, environment.install);
			std::error_code error;
			fs::create_directories(lockPath.parent_path(), error);
			Require(!error, "cannot create lock directory");
			UniqueHandle shared(::CreateFileW(
				lockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
			Require(static_cast<bool>(shared), "cannot acquire fixture shared session lock");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::Deferred, "active session did not defer update");
			Require(ActiveVersion(environment) == kOldVersion, "deferred update mutated installation");
		});

		Run("transaction child retains exclusive session lock", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "inherited-transaction-lock");
			UniqueHandle owner = AcquireExclusiveSessionLock(environment.storage, environment.install);
			Require(static_cast<bool>(owner), "cannot acquire transaction lock");
			std::atomic<bool> childRunning{ false };
			bool childSucceeded = false;
			std::thread child([&]() {
				DWORD exitCode = 0;
				childSucceeded = RunProcess(
					PowerShellPath(),
					{ L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-Command",
					  L"Start-Sleep -Milliseconds 1250" },
					5000, exitCode, true,
					[&]() { childRunning.store(true); },
					{ owner.get() });
			});
			for (int index = 0; index < 20 && !childRunning.load(); ++index)
				::Sleep(50);
			const bool observedRunning = childRunning.load();
			bool blockedWhileChildRan = false;
			if (observedRunning)
			{
				owner.reset();
				UniqueHandle whileChildRuns = AcquireExclusiveSessionLock(environment.storage, environment.install);
				blockedWhileChildRan = !whileChildRuns;
			}
			child.join();
			Require(observedRunning, "transaction child did not start");
			Require(blockedWhileChildRan, "child did not retain inherited exclusive lock");
			Require(childSucceeded, "transaction child failed");
			UniqueHandle afterChild = AcquireExclusiveSessionLock(environment.storage, environment.install);
			Require(static_cast<bool>(afterChild), "child did not release inherited lock on exit");
		});

		Run("minimum loader requires manual update", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "loader-too-old");
			AddManifest(inputs, environment, kNewVersion, "9.0.0", "9.0.0");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::Deferred && result.loaderUpdateDeferred &&
				result.errorCode == "manual_loader_update_required", "minimum loader did not defer manually");
			Require(ActiveVersion(environment) == kOldVersion, "loader-incompatible update mutated installation");
		});

		Run("compatible unsigned fixture same-launch install", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "same-launch");
			AddManifest(inputs, environment);
			const std::string loaderBefore = Hash(environment.install / L"vSMR.dll");
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.status == StartupStatus::Updated && result.updateActivated,
				"compatible fixture did not activate");
			Require(ActiveVersion(environment) == kNewVersion, "new data was not active in same launch");
			Require(Hash(environment.install / L"vSMR.dll") == loaderBefore, "stable loader was replaced");
			Require(IsRegularFile(result.healthMarkerPath), "health marker missing after activation");
			Require(ConfirmRuntimeHealthy(result), "healthy runtime confirmation failed");
			Require(!IsRegularFile(result.healthMarkerPath), "confirmed health marker was not cleared");
		});

		Run("spaces and Unicode installation path", failures, [&]() {
			Environment environment = CreateEnvironment(inputs, "unicode-path-template");
			const fs::path unicodeRoot = inputs.scratch / L"unicode space \u03A9";
			std::error_code error;
			fs::remove_all(unicodeRoot, error);
			error.clear();
			fs::rename(environment.root, unicodeRoot, error);
			Require(!error, "Unicode fixture root rename failed");
			environment.root = unicodeRoot;
			environment.install = unicodeRoot / L"install";
			environment.feed = unicodeRoot / L"feed";
			environment.storage = unicodeRoot / L"storage";
			AddManifest(inputs, environment);
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(result.updateActivated && ActiveVersion(environment) == kNewVersion,
				"spaces/Unicode same-launch update failed");
			Require(ConfirmRuntimeHealthy(result), "spaces/Unicode confirmation failed");
		});

		Run("failed/healthy marker ordering is success-wins", failures, [&]() {
			{
				const Environment environment = CreateEnvironment(inputs, "failed-then-healthy");
				AddManifest(inputs, environment);
				const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
				Require(result.updateActivated && MarkRuntimeUnhealthy(result), "failed phase was not persisted");
				Require(ConfirmRuntimeHealthy(result), "healthy phase did not supersede failed phase");
				Require(!MarkRuntimeUnhealthy(result), "failure recreated a confirmed marker");
			}
			{
				const Environment environment = CreateEnvironment(inputs, "healthy-then-failed");
				AddManifest(inputs, environment);
				const StartupResult result = PrepareUpdateBeforeRuntimeLoad(Options(environment));
				Require(result.updateActivated && ConfirmRuntimeHealthy(result), "healthy phase failed");
				Require(!MarkRuntimeUnhealthy(result), "failure overrode prior confirmation");
			}
		});

		Run("runtime-create failure rolls back package", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "runtime-rollback");
			AddManifest(inputs, environment);
			const StartupOptions options = Options(environment);
			const StartupResult result = PrepareUpdateBeforeRuntimeLoad(options);
			Require(result.updateActivated, "rollback fixture did not activate");
			fs::path restored;
			std::wstring rollbackError;
			Require(RollbackPreparedUpdate(options, result, &restored, &rollbackError),
				"RollbackPreparedUpdate failed");
			Require(restored == fs::absolute(options.canonicalRuntimePath).lexically_normal(),
				"rollback returned wrong runtime");
			Require(ActiveVersion(environment) == kOldVersion, "rollback did not restore old data");
			Require(!IsRegularFile(result.healthMarkerPath), "rollback left health marker");
			const fs::path quarantine = environment.storage / L"quarantine" / L"2.0.0-beta.4.json";
			Require(IsRegularFile(quarantine), "runtime-create failure was not quarantined");
			WriteAction(environment, "retry_update");
			const StartupResult retry = PrepareUpdateBeforeRuntimeLoad(options);
			Require(retry.updateActivated, "explicit retry did not clear and revalidate quarantine");
			Require(!IsRegularFile(quarantine), "explicit retry left quarantine marker");
			Require(ConfirmRuntimeHealthy(retry), "retried runtime confirmation failed");
		});

		Run("interrupted installing journal reconciles", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "interrupted-installing");
			AddManifest(inputs, environment);
			StartupOptions options = Options(environment);
			StartupResult update = PrepareUpdateBeforeRuntimeLoad(options);
			Require(update.updateActivated, "interrupted fixture did not activate");
			Require(WriteHealthMarker(options, environment.storage, update, "installing"),
				"could not simulate installing journal");
			options.currentVersion = kNewVersion;
			const StartupResult recovered = PrepareUpdateBeforeRuntimeLoad(options);
			Require(recovered.status == StartupStatus::FailedOpen &&
				recovered.errorCode == "interrupted_update_rolled_back",
				"installing journal was not reconciled");
			Require(ActiveVersion(environment) == kOldVersion, "installing reconciliation did not restore old data");
			Require(!IsRegularFile(environment.storage / L"quarantine" / L"2.0.0-beta.4.json"),
				"interrupted installation incorrectly quarantined a valid release");
		});

		Run("exited attempt owner triggers rollback", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "exited-owner");
			AddManifest(inputs, environment);
			StartupResult update = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(update.updateActivated, "exited-owner fixture did not activate");
			std::array<wchar_t, MAX_PATH + 1> windows{};
			const UINT windowsLength = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
			Require(windowsLength > 0 && windowsLength < windows.size(), "Windows directory unavailable");
			const fs::path command = fs::path(std::wstring(windows.data(), windowsLength)) /
				L"System32" / L"cmd.exe";
			std::wstring commandLine = L"cmd.exe /d /c exit 0";
			std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
			mutableCommand.push_back(L'\0');
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			Require(::CreateProcessW(
				command.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
				CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
				"dead-owner child could not be created");
			UniqueHandle processHandle(process.hProcess);
			UniqueHandle threadHandle(process.hThread);
			Require(::WaitForSingleObject(processHandle.get(), 5000) == WAIT_OBJECT_0,
				"dead-owner child did not exit");
			std::uint64_t created = 0;
			bool alive = true;
			Require(ProcessCreationStamp(process.dwProcessId, created, alive) && !alive && created != 0,
				"dead-owner identity could not be captured");
			EditJson(update.healthMarkerPath, [&](rapidjson::Document& document) {
				document["attempt_pid"].SetUint64(process.dwProcessId);
				document["attempt_process_created_100ns"].SetUint64(created);
			});
			StartupOptions recoveryOptions = Options(environment, kNewVersion);
			const StartupResult recovered = PrepareUpdateBeforeRuntimeLoad(recoveryOptions);
			Require(recovered.errorCode == "unhealthy_update_rolled_back" &&
				ActiveVersion(environment) == kOldVersion,
				"exited attempt owner was not rolled back");
		});

		Run("rollback requires exact previous version identity", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "exact-previous-version");
			AddManifest(inputs, environment);
			StartupResult update = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(update.updateActivated && MarkRuntimeUnhealthy(update),
				"exact-version fixture did not enter failed phase");
			EditJson(update.healthMarkerPath, [](rapidjson::Document& document) {
				document["previous_version"].SetString(
					"2.0.0-beta.2+different-build", document.GetAllocator());
			});
			StartupOptions recoveryOptions = Options(environment, kNewVersion);
			const StartupResult recovered = PrepareUpdateBeforeRuntimeLoad(recoveryOptions);
			Require(recovered.status == StartupStatus::FailedOpen &&
				recovered.selectedRuntimePath.empty() &&
				recovered.errorCode == "rollback_verification_failed",
				"same-precedence different-build rollback identity was accepted");
		});

		Run("concurrent live attempt is not rolled back", failures, [&]() {
			const Environment environment = CreateEnvironment(inputs, "live-attempt");
			AddManifest(inputs, environment);
			StartupResult update = PrepareUpdateBeforeRuntimeLoad(Options(environment));
			Require(update.updateActivated, "live-attempt fixture did not activate");
			StartupOptions secondOptions = Options(environment, kNewVersion);
			const StartupResult second = PrepareUpdateBeforeRuntimeLoad(secondOptions);
			Require(second.status == StartupStatus::Updated && second.updateActivated,
				"live validation attempt was rolled back");
			Require(ActiveVersion(environment) == kNewVersion, "live attempt changed active data");
			Require(ConfirmRuntimeHealthy(update), "live attempt confirmation failed");
		});

		Run("test storage never writes LOCALAPPDATA updater", failures, [&]() {
			const Snapshot productionAfter = SnapshotTree(productionStorage);
			Require(productionBefore == productionAfter,
				"isolated fixture run changed production LocalAppData updater state");
		});

		if (failures == 0)
			std::cout << "All offline updater tests passed.\n";
		else
			std::cerr << failures << " offline updater test(s) failed.\n";
		return failures == 0 ? 0 : 1;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 12)
	{
		std::wcerr << L"Usage: vSMRUpdaterHarness.exe <workspace> <scratch> <loader> <base.zip> "
			L"<traversal.zip> <duplicate.zip> <oversized.zip> <cms-content> <cms-signature> "
			L"<cms-signer-sha256> <cms-wrong-signer-sha256>\n";
		return 2;
	}
	vsmr::updater::harness::Inputs inputs;
	inputs.workspace = std::filesystem::absolute(argv[1]).lexically_normal();
	inputs.scratch = std::filesystem::absolute(argv[2]).lexically_normal();
	inputs.loaderBinary = std::filesystem::absolute(argv[3]).lexically_normal();
	inputs.baseArchive = std::filesystem::absolute(argv[4]).lexically_normal();
	inputs.traversalArchive = std::filesystem::absolute(argv[5]).lexically_normal();
	inputs.duplicateArchive = std::filesystem::absolute(argv[6]).lexically_normal();
	inputs.oversizedArchive = std::filesystem::absolute(argv[7]).lexically_normal();
	inputs.cmsContent = std::filesystem::absolute(argv[8]).lexically_normal();
	inputs.cmsSignature = std::filesystem::absolute(argv[9]).lexically_normal();
	inputs.cmsSignerSha256 = vsmr::updater::ToLowerAscii(vsmr::updater::WideToUtf8(argv[10]));
	inputs.cmsWrongSignerSha256 = vsmr::updater::ToLowerAscii(vsmr::updater::WideToUtf8(argv[11]));
	return vsmr::updater::harness::RunAll(inputs);
}
