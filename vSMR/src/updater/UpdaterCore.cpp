#include "updater/UpdaterCore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <shlobj.h>
#include <bcrypt.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef VSMR_UPDATE_SIGNER_CERT_SHA256
#define VSMR_UPDATE_SIGNER_CERT_SHA256 ""
#endif

namespace fs = std::filesystem;

namespace vsmr::updater
{
	namespace
	{
		constexpr wchar_t kApiUrl[] =
			L"https://api.github.com/repos/IWantPizzaa/vSMR/releases?per_page=30";
		constexpr std::uint64_t kMaximumMetadataBytes = 2ULL * 1024ULL * 1024ULL;
		constexpr std::uint64_t kMaximumManifestBytes = 64ULL * 1024ULL;
		constexpr std::uint64_t kMaximumSignatureBytes = 256ULL * 1024ULL;
		constexpr std::uint64_t kMaximumArchiveBytes = 256ULL * 1024ULL * 1024ULL;
		constexpr std::uint64_t kMinimumCheckIntervalSeconds = 15ULL * 60ULL;
		constexpr DWORD kMetadataTimeoutMs = 5000;
		constexpr DWORD kAssetMetadataTimeoutMs = 4000;
		constexpr DWORD kArchiveTimeoutMs = 75000;

		class UniqueHandle
		{
		public:
			UniqueHandle() = default;
			explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
			~UniqueHandle() { reset(); }
			UniqueHandle(const UniqueHandle&) = delete;
			UniqueHandle& operator=(const UniqueHandle&) = delete;
			UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
			UniqueHandle& operator=(UniqueHandle&& other) noexcept
			{
				if (this != &other)
					reset(other.release());
				return *this;
			}
			HANDLE get() const noexcept { return value_; }
			explicit operator bool() const noexcept
			{
				return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
			}
			HANDLE release() noexcept
			{
				HANDLE value = value_;
				value_ = nullptr;
				return value;
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
			InternetHandle() = default;
			explicit InternetHandle(HINTERNET value) noexcept : value_(value) {}
			~InternetHandle() { reset(); }
			InternetHandle(const InternetHandle&) = delete;
			InternetHandle& operator=(const InternetHandle&) = delete;
			InternetHandle(InternetHandle&& other) noexcept : value_(other.release()) {}
			InternetHandle& operator=(InternetHandle&& other) noexcept
			{
				if (this != &other)
					reset(other.release());
				return *this;
			}
			HINTERNET get() const noexcept { return value_; }
			explicit operator bool() const noexcept { return value_ != nullptr; }
			HINTERNET release() noexcept
			{
				HINTERNET value = value_;
				value_ = nullptr;
				return value;
			}
			void reset(HINTERNET value = nullptr) noexcept
			{
				if (value_ != nullptr)
					::WinHttpCloseHandle(value_);
				value_ = value;
			}

		private:
			HINTERNET value_ = nullptr;
		};

		class CertContextHandle
		{
		public:
			CertContextHandle() = default;
			explicit CertContextHandle(PCCERT_CONTEXT value) noexcept : value_(value) {}
			~CertContextHandle()
			{
				if (value_ != nullptr)
					::CertFreeCertificateContext(value_);
			}
			CertContextHandle(const CertContextHandle&) = delete;
			CertContextHandle& operator=(const CertContextHandle&) = delete;
			PCCERT_CONTEXT get() const noexcept { return value_; }
			explicit operator bool() const noexcept { return value_ != nullptr; }

		private:
			PCCERT_CONTEXT value_ = nullptr;
		};

		struct SemVerIdentifier
		{
			std::string text;
			bool numeric = false;
			std::uint64_t number = 0;
		};

		struct SemVer
		{
			std::uint64_t major = 0;
			std::uint64_t minor = 0;
			std::uint64_t patch = 0;
			std::vector<SemVerIdentifier> prerelease;
			std::string normalized;
			bool valid = false;
		};

		struct Config
		{
			bool autoCheck = true;
			bool autoDownload = true;
			bool autoInstall = true;
			bool protectModifiedAviso = true;
			UpdateChannel channel = UpdateChannel::Beta;
			std::string skippedVersion;
		};

		struct State
		{
			std::string status = "idle";
			std::string installedVersion;
			std::string selectedVersion;
			std::string availableVersion;
			int downloadPercent = -1;
			std::string lastCheckedUtc;
			std::string nextCheckUtc;
			std::string message;
			std::string errorCode;
			std::string error;
			bool restartRequired = false;
			bool loaderUpdateDeferred = false;
			std::string releaseUrl;
			std::string lastActionRequestId;
		};

		struct Action
		{
			std::string requestId;
			std::string action;
			bool valid = false;
		};

		struct ReleaseAsset
		{
			std::string name;
			std::wstring url;
			std::uint64_t size = 0;
			std::string digest;
		};

		struct Release
		{
			SemVer version;
			std::string htmlUrl;
			std::vector<ReleaseAsset> assets;
		};

		struct Manifest
		{
			SemVer version;
			std::string channel;
			std::string archiveName;
			std::uint64_t archiveSize = 0;
			std::string archiveSha256;
			SemVer minimumLoaderVersion;
			std::string runtimeRelativePath;
			std::string loaderName;
			std::string loaderVersion;
			std::uint64_t loaderSize = 0;
			std::string loaderSha256;
			bool publishable = false;
			std::uint32_t runtimeAbi = 0;
		};

		struct HttpResponse
		{
			DWORD statusCode = 0;
			std::vector<std::uint8_t> body;
			std::string etag;
			std::string retryAfter;
			std::string rateLimitReset;
			std::wstring finalUrl;
			std::string error;
		};

		fs::path GetProductionSessionLockStorageRoot() noexcept;

		struct Context
		{
			const StartupOptions& options;
			fs::path storageRoot;
			fs::path sessionLockStorageRoot;
			fs::path statePath;
			State state;
			ULONGLONG startedTick = ::GetTickCount64();
			bool cancelled = false;

			explicit Context(const StartupOptions& startupOptions)
				: options(startupOptions),
				storageRoot(startupOptions.testStorageDirectory.empty()
					? GetUpdaterStorageDirectory() : startupOptions.testStorageDirectory),
				sessionLockStorageRoot(startupOptions.testStorageDirectory.empty()
					? GetProductionSessionLockStorageRoot() : startupOptions.testStorageDirectory),
				statePath(storageRoot / L"state.json")
			{
				state.installedVersion = startupOptions.currentVersion;
			}
		};

		std::string WideToUtf8(const std::wstring& value)
		{
			if (value.empty())
				return {};
			const int size = ::WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return {};
			std::string result(static_cast<std::size_t>(size), '\0');
			if (::WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size)
			{
				return {};
			}
			return result;
		}

		std::wstring Utf8ToWide(const std::string& value)
		{
			if (value.empty())
				return {};
			const int size = ::MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0)
				return {};
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			if (::MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), result.data(), size) != size)
			{
				return {};
			}
			return result;
		}

		std::string ToLowerAscii(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		std::wstring ToLowerWide(std::wstring value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
				return static_cast<wchar_t>(std::towlower(character));
			});
			return value;
		}

		bool IsHex(const std::string& value, std::size_t length)
		{
			return value.size() == length &&
				std::all_of(value.begin(), value.end(), [](unsigned char character) {
					return std::isxdigit(character) != 0;
				});
		}

		std::string Hex(const BYTE* bytes, DWORD size)
		{
			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for (DWORD index = 0; index < size; ++index)
				output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
			return output.str();
		}

		std::string SecureRandomHex(DWORD byteCount)
		{
			if (byteCount == 0 || byteCount > 64)
				return {};
			std::vector<BYTE> bytes(byteCount);
			if (BCryptGenRandom(
				nullptr, bytes.data(), byteCount,
				BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
			{
				return {};
			}
			return Hex(bytes.data(), byteCount);
		}

		bool ProbeWritableDirectory(const fs::path& directory) noexcept
		{
			try
			{
				if (directory.empty())
					return false;
				std::error_code error;
				fs::create_directories(directory, error);
				if (error || !fs::is_directory(directory, error) || error)
					return false;

				for (unsigned int attempt = 0; attempt < 4; ++attempt)
				{
					std::string nonce = SecureRandomHex(12);
					if (nonce.empty())
					{
						std::ostringstream fallback;
						fallback << std::hex << ::GetCurrentProcessId() << '-'
							<< ::GetTickCount64() << '-' << attempt;
						nonce = fallback.str();
					}
					const fs::path probe = directory /
						(L".vsmr-write-probe-" + Utf8ToWide(nonce) + L".tmp");
					UniqueHandle file(::CreateFileW(
						probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
						FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr));
					if (!file)
					{
						if (::GetLastError() == ERROR_FILE_EXISTS ||
							::GetLastError() == ERROR_ALREADY_EXISTS)
						{
							continue;
						}
						return false;
					}
					const BYTE value = 0xA5;
					DWORD written = 0;
					const bool writeSucceeded = ::WriteFile(
						file.get(), &value, sizeof(value), &written, nullptr) != FALSE &&
						written == sizeof(value) && ::FlushFileBuffers(file.get()) != FALSE;
					file.reset();
					const bool deleteSucceeded = ::DeleteFileW(probe.c_str()) != FALSE;
					return writeSucceeded && deleteSucceeded;
				}
			}
			catch (...)
			{
			}
			return false;
		}

		fs::path LocalAppDataUpdaterCandidate() noexcept
		{
			try
			{
				PWSTR knownFolder = nullptr;
				if (SUCCEEDED(::SHGetKnownFolderPath(
					FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder)) &&
					knownFolder != nullptr)
				{
					fs::path result = fs::path(knownFolder) / L"vSMR" / L"Updater";
					::CoTaskMemFree(knownFolder);
					return result;
				}
				if (knownFolder != nullptr)
					::CoTaskMemFree(knownFolder);
				std::array<wchar_t, 32768> buffer{};
				const DWORD length = ::GetEnvironmentVariableW(
					L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length > 0 && length < buffer.size())
					return fs::path(std::wstring(buffer.data(), length)) /
						L"vSMR" / L"Updater";
			}
			catch (...)
			{
			}
			return {};
		}

		fs::path TemporaryUpdaterCandidate() noexcept
		{
			try
			{
				std::array<wchar_t, 32768> buffer{};
				const DWORD length = ::GetTempPathW(
					static_cast<DWORD>(buffer.size()), buffer.data());
				if (length > 0 && length < buffer.size())
					return fs::path(std::wstring(buffer.data(), length)) /
						L"vSMR" / L"Updater";
			}
			catch (...)
			{
			}
			return {};
		}

		fs::path GetProductionSessionLockStorageRoot() noexcept
		{
			// Session leases must never switch between LocalAppData and Temp while
			// another EuroScope process is alive. Keep their deterministic root in
			// the per-user Temp directory even when persistent updater state uses
			// LocalAppData.
			const fs::path temporary = TemporaryUpdaterCandidate();
			return ProbeWritableDirectory(temporary) ? temporary : fs::path{};
		}

		std::string UtcNow()
		{
			SYSTEMTIME time{};
			::GetSystemTime(&time);
			char buffer[32]{};
			std::snprintf(
				buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
				time.wYear, time.wMonth, time.wDay,
				time.wHour, time.wMinute, time.wSecond);
			return buffer;
		}

		std::string UtcAfterSeconds(std::uint64_t seconds)
		{
			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			ULARGE_INTEGER value{};
			value.LowPart = now.dwLowDateTime;
			value.HighPart = now.dwHighDateTime;
			value.QuadPart += seconds * 10000000ULL;
			FILETIME future{};
			future.dwLowDateTime = value.LowPart;
			future.dwHighDateTime = value.HighPart;
			SYSTEMTIME time{};
			if (!::FileTimeToSystemTime(&future, &time))
				return {};
			char buffer[32]{};
			std::snprintf(
				buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
				time.wYear, time.wMonth, time.wDay,
				time.wHour, time.wMinute, time.wSecond);
			return buffer;
		}

		std::uint64_t Fnv1a64(const std::wstring& value)
		{
			std::uint64_t hash = 14695981039346656037ULL;
			for (const wchar_t character : value)
			{
				const wchar_t folded = static_cast<wchar_t>(std::towlower(character));
				const BYTE* bytes = reinterpret_cast<const BYTE*>(&folded);
				for (std::size_t index = 0; index < sizeof(folded); ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}

		std::wstring HashName(const fs::path& installRoot)
		{
			std::error_code error;
			fs::path absolute = fs::absolute(installRoot, error);
			fs::path identity = (error ? installRoot : absolute).lexically_normal();
			UniqueHandle directory(::CreateFileW(
				identity.c_str(), 0,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
			if (directory)
			{
				const DWORD required = ::GetFinalPathNameByHandleW(
					directory.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
				if (required > 0 && required < 32768)
				{
					std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
					const DWORD written = ::GetFinalPathNameByHandleW(
						directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
						FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
					if (written > 0 && written < buffer.size())
					{
						std::wstring finalPath(buffer.data(), written);
						if (finalPath.rfind(L"\\\\?\\", 0) == 0)
							finalPath.erase(0, 4);
						identity = fs::path(finalPath).lexically_normal();
					}
				}
			}
			const std::wstring normalized = identity.wstring();
			std::wostringstream output;
			output << std::hex << std::setfill(L'0') << std::setw(16) << Fnv1a64(normalized);
			return output.str();
		}

		fs::path SessionLockPath(const fs::path& storageRoot, const fs::path& installRoot)
		{
			return storageRoot / L"locks" / (HashName(installRoot) + L".session.lock");
		}

		bool ReadBytes(const fs::path& path, std::vector<std::uint8_t>& output, std::uint64_t maximum)
		{
			std::error_code error;
			const auto size = fs::file_size(path, error);
			if (error || size > maximum || size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
				return false;
			std::ifstream input(path, std::ios::binary);
			if (!input.is_open())
				return false;
			output.resize(static_cast<std::size_t>(size));
			if (!output.empty())
				input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
			return input.good() || (input.eof() && input.gcount() == static_cast<std::streamsize>(output.size()));
		}

		bool ReadText(const fs::path& path, std::string& output, std::uint64_t maximum)
		{
			std::vector<std::uint8_t> bytes;
			if (!ReadBytes(path, bytes, maximum))
				return false;
			output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			return true;
		}

		bool AtomicWrite(const fs::path& path, const void* data, std::size_t size)
		{
			std::error_code error;
			fs::create_directories(path.parent_path(), error);
			if (error)
				return false;
			const fs::path temporary = path.wstring() + L".tmp." +
				std::to_wstring(::GetCurrentProcessId()) + L"." +
				std::to_wstring(::GetTickCount64());
			UniqueHandle file(::CreateFileW(
				temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY, nullptr));
			if (!file)
				return false;
			const BYTE* cursor = static_cast<const BYTE*>(data);
			std::size_t remaining = size;
			while (remaining > 0)
			{
				const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(1024 * 1024)));
				DWORD written = 0;
				if (!::WriteFile(file.get(), cursor, chunk, &written, nullptr) || written != chunk)
				{
					file.reset();
					::DeleteFileW(temporary.c_str());
					return false;
				}
				cursor += written;
				remaining -= written;
			}
			if (!::FlushFileBuffers(file.get()))
			{
				file.reset();
				::DeleteFileW(temporary.c_str());
				return false;
			}
			file.reset();
			if (!::MoveFileExW(
				temporary.c_str(), path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				::DeleteFileW(temporary.c_str());
				return false;
			}
			return true;
		}

		bool AtomicWriteText(const fs::path& path, const std::string& text)
		{
			return AtomicWrite(path, text.data(), text.size());
		}

		bool IsRegularFile(const fs::path& path)
		{
			std::error_code error;
			return fs::is_regular_file(path, error) && !error;
		}

		std::string JsonString(const rapidjson::Value& object, const char* name)
		{
			if (!object.IsObject() || !object.HasMember(name) || !object[name].IsString())
				return {};
			return object[name].GetString();
		}

		bool JsonBool(const rapidjson::Value& object, const char* name, bool fallback)
		{
			return object.IsObject() && object.HasMember(name) && object[name].IsBool()
				? object[name].GetBool() : fallback;
		}

		std::uint64_t JsonUint64(const rapidjson::Value& object, const char* name)
		{
			return object.IsObject() && object.HasMember(name) && object[name].IsUint64()
				? object[name].GetUint64() : 0;
		}

		void AddJsonString(
			rapidjson::Document& document,
			rapidjson::Value& object,
			const char* name,
			const std::string& value)
		{
			auto& allocator = document.GetAllocator();
			rapidjson::Value key(name, allocator);
			rapidjson::Value content;
			content.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
			object.AddMember(key, content, allocator);
		}

		std::string SerializeState(const State& state)
		{
			rapidjson::Document document;
			document.SetObject();
			document.AddMember("schema_version", 1, document.GetAllocator());
			AddJsonString(document, document, "status", state.status);
			AddJsonString(document, document, "installed_version", state.installedVersion);
			AddJsonString(document, document, "selected_version", state.selectedVersion);
			AddJsonString(document, document, "available_version", state.availableVersion);
			document.AddMember("download_percent", state.downloadPercent, document.GetAllocator());
			AddJsonString(document, document, "last_checked_utc", state.lastCheckedUtc);
			AddJsonString(document, document, "next_check_utc", state.nextCheckUtc);
			AddJsonString(document, document, "message", state.message);
			AddJsonString(document, document, "error_code", state.errorCode);
			AddJsonString(document, document, "error", state.error);
			document.AddMember("restart_required", state.restartRequired, document.GetAllocator());
			document.AddMember("loader_update_deferred", state.loaderUpdateDeferred, document.GetAllocator());
			AddJsonString(document, document, "release_url", state.releaseUrl);
			AddJsonString(document, document, "last_action_request_id", state.lastActionRequestId);
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			std::string result(buffer.GetString(), buffer.Size());
			result.push_back('\n');
			return result;
		}

		void PersistState(Context& context)
		{
			AtomicWriteText(context.statePath, SerializeState(context.state));
		}

		bool Report(Context& context, ProgressStage stage, int percent, const std::wstring& message)
		{
			context.state.message = WideToUtf8(message);
			context.state.downloadPercent = percent;
			PersistState(context);
			if (!context.options.progressCallback)
				return true;
			bool keepGoing = true;
			try
			{
				keepGoing = context.options.progressCallback(Progress{ stage, percent, message });
			}
			catch (...)
			{
				keepGoing = true;
			}
			context.cancelled = !keepGoing;
			return keepGoing;
		}

		DWORD RemainingMs(const Context& context, DWORD operationMaximum)
		{
			const ULONGLONG elapsed = ::GetTickCount64() - context.startedTick;
			if (elapsed >= context.options.overallDeadlineMs)
				return 0;
			const ULONGLONG remaining = context.options.overallDeadlineMs - elapsed;
			return static_cast<DWORD>((std::min)(remaining, static_cast<ULONGLONG>(operationMaximum)));
		}

		bool ParseUint64(const std::string& text, std::uint64_t& value)
		{
			if (text.empty() || (text.size() > 1 && text.front() == '0'))
				return false;
			std::uint64_t result = 0;
			for (const unsigned char character : text)
			{
				if (!std::isdigit(character))
					return false;
				const std::uint64_t digit = character - '0';
				if (result > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
					return false;
				result = result * 10 + digit;
			}
			value = result;
			return true;
		}

		std::vector<std::string> Split(const std::string& value, char separator)
		{
			std::vector<std::string> parts;
			std::size_t start = 0;
			for (;;)
			{
				const std::size_t end = value.find(separator, start);
				parts.push_back(value.substr(start, end == std::string::npos ? end : end - start));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return parts;
		}

		SemVer ParseSemVer(std::string value)
		{
			SemVer result;
			if (!value.empty() && (value.front() == 'v' || value.front() == 'V'))
				value.erase(value.begin());
			if (value.empty() || value.size() > 64)
				return result;
			const std::size_t plus = value.find('+');
			if (plus != std::string::npos)
			{
				if (value.find('+', plus + 1) != std::string::npos)
					return result;
				const auto buildIdentifiers = Split(value.substr(plus + 1), '.');
				for (const std::string& identifier : buildIdentifiers)
				{
					if (identifier.empty() ||
						!std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
							return std::isalnum(character) != 0 || character == '-';
						}))
					{
						return result;
					}
				}
			}
			const std::string withoutBuild = value.substr(0, plus);
			const std::size_t dash = withoutBuild.find('-');
			const std::string core = withoutBuild.substr(0, dash);
			const auto numbers = Split(core, '.');
			if (numbers.size() != 3 ||
				!ParseUint64(numbers[0], result.major) ||
				!ParseUint64(numbers[1], result.minor) ||
				!ParseUint64(numbers[2], result.patch))
			{
				return result;
			}
			if (dash != std::string::npos)
			{
				const auto identifiers = Split(withoutBuild.substr(dash + 1), '.');
				if (identifiers.empty())
					return result;
				for (const std::string& identifier : identifiers)
				{
					if (identifier.empty())
						return SemVer{};
					if (!std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
						return std::isalnum(character) != 0 || character == '-';
					}))
					{
						return SemVer{};
					}
					SemVerIdentifier parsed;
					parsed.text = identifier;
					parsed.numeric = std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
						return std::isdigit(character) != 0;
					});
					if (parsed.numeric && !ParseUint64(identifier, parsed.number))
						return SemVer{};
					result.prerelease.push_back(std::move(parsed));
				}
			}
			result.normalized = value;
			result.valid = true;
			return result;
		}

		int CompareSemVer(const SemVer& left, const SemVer& right)
		{
			if (left.major != right.major)
				return left.major < right.major ? -1 : 1;
			if (left.minor != right.minor)
				return left.minor < right.minor ? -1 : 1;
			if (left.patch != right.patch)
				return left.patch < right.patch ? -1 : 1;
			if (left.prerelease.empty() != right.prerelease.empty())
				return left.prerelease.empty() ? 1 : -1;
			const std::size_t count = (std::min)(left.prerelease.size(), right.prerelease.size());
			for (std::size_t index = 0; index < count; ++index)
			{
				const auto& a = left.prerelease[index];
				const auto& b = right.prerelease[index];
				if (a.numeric && b.numeric && a.number != b.number)
					return a.number < b.number ? -1 : 1;
				if (a.numeric != b.numeric)
					return a.numeric ? -1 : 1;
				if (a.text != b.text)
					return a.text < b.text ? -1 : 1;
			}
			if (left.prerelease.size() == right.prerelease.size())
				return 0;
			return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
		}

		bool SameSemVerIdentity(const std::string& left, const std::string& right)
		{
			const SemVer parsedLeft = ParseSemVer(left);
			const SemVer parsedRight = ParseSemVer(right);
			return parsedLeft.valid && parsedRight.valid &&
				parsedLeft.normalized == parsedRight.normalized;
		}

		bool ChannelAccepts(const SemVer& version, UpdateChannel channel)
		{
			return channel == UpdateChannel::Beta || version.prerelease.empty();
		}

		Config LoadConfig(const fs::path& path, UpdateChannel defaultChannel)
		{
			Config config;
			config.channel = defaultChannel;
			std::string json;
			if (!ReadText(path, json, 64 * 1024))
				return config;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject())
				return config;
			config.autoCheck = JsonBool(document, "auto_check", true);
			config.autoDownload = JsonBool(document, "auto_download", true);
			config.autoInstall = JsonBool(document, "auto_install", true);
			config.protectModifiedAviso = JsonBool(document, "protect_modified_aviso", true);
			config.skippedVersion = JsonString(document, "skipped_version");
			const std::string channel = ToLowerAscii(JsonString(document, "channel"));
			if (channel == "stable")
				config.channel = UpdateChannel::Stable;
			else if (channel == "beta")
				config.channel = UpdateChannel::Beta;
			return config;
		}

		Action ConsumeAction(const fs::path& storageRoot)
		{
			Action result;
			const fs::path actionPath = storageRoot / L"action.json";
			const fs::path processingPath = storageRoot / L"action.processing.json";
			::DeleteFileW(processingPath.c_str());
			if (!::MoveFileExW(actionPath.c_str(), processingPath.c_str(), MOVEFILE_WRITE_THROUGH))
				return result;
			std::string json;
			if (ReadText(processingPath, json, 64 * 1024))
			{
				rapidjson::Document document;
				document.Parse<0>(json.c_str());
				if (!document.HasParseError() && document.IsObject())
				{
					result.requestId = JsonString(document, "request_id");
					result.action = ToLowerAscii(JsonString(document, "action"));
					result.valid = !result.requestId.empty() &&
						(result.action == "check_now" || result.action == "retry_update" ||
						 result.action == "reload_aviso" || result.action == "clear_status");
				}
			}
			::DeleteFileW(processingPath.c_str());
			return result;
		}

		bool ParseUtcFileTime(const std::string& text, FILETIME& result)
		{
			if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
				text[13] != ':' || text[16] != ':' || text[19] != 'Z')
			{
				return false;
			}
			SYSTEMTIME time{};
			try
			{
				time.wYear = static_cast<WORD>(std::stoi(text.substr(0, 4)));
				time.wMonth = static_cast<WORD>(std::stoi(text.substr(5, 2)));
				time.wDay = static_cast<WORD>(std::stoi(text.substr(8, 2)));
				time.wHour = static_cast<WORD>(std::stoi(text.substr(11, 2)));
				time.wMinute = static_cast<WORD>(std::stoi(text.substr(14, 2)));
				time.wSecond = static_cast<WORD>(std::stoi(text.substr(17, 2)));
			}
			catch (...)
			{
				return false;
			}
			return ::SystemTimeToFileTime(&time, &result) == TRUE;
		}

		bool IsFutureUtc(const std::string& text)
		{
			FILETIME parsed{};
			if (!ParseUtcFileTime(text, parsed))
				return false;
			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			return ::CompareFileTime(&parsed, &now) > 0;
		}

		State LoadPreviousState(const fs::path& path)
		{
			State state;
			std::string json;
			if (!ReadText(path, json, 128 * 1024))
				return state;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject())
				return state;
			state.lastCheckedUtc = JsonString(document, "last_checked_utc");
			state.nextCheckUtc = JsonString(document, "next_check_utc");
			state.lastActionRequestId = JsonString(document, "last_action_request_id");
			state.availableVersion = JsonString(document, "available_version");
			return state;
		}

		bool EndsWithNoCase(const std::wstring& value, const std::wstring& suffix)
		{
			if (suffix.size() > value.size())
				return false;
			return ToLowerWide(value.substr(value.size() - suffix.size())) == ToLowerWide(suffix);
		}

		bool IsAllowedDownloadHost(const std::wstring& host)
		{
			const std::wstring normalized = ToLowerWide(host);
			return normalized == L"api.github.com" ||
				normalized == L"github.com" ||
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

		std::string QueryHeader(HINTERNET request, DWORD query)
		{
			DWORD size = 0;
			::WinHttpQueryHeaders(
				request, query, WINHTTP_HEADER_NAME_BY_INDEX,
				nullptr, &size, WINHTTP_NO_HEADER_INDEX);
			if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t))
				return {};
			std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
			if (!::WinHttpQueryHeaders(
				request, query, WINHTTP_HEADER_NAME_BY_INDEX,
				buffer.data(), &size, WINHTTP_NO_HEADER_INDEX))
			{
				return {};
			}
			return WideToUtf8(std::wstring(buffer.data()));
		}

		std::string QueryCustomHeader(HINTERNET request, const wchar_t* name)
		{
			DWORD size = 0;
			::WinHttpQueryHeaders(
				request, WINHTTP_QUERY_CUSTOM, name,
				nullptr, &size, WINHTTP_NO_HEADER_INDEX);
			if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t))
				return {};
			std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
			if (!::WinHttpQueryHeaders(
				request, WINHTTP_QUERY_CUSTOM, name,
				buffer.data(), &size, WINHTTP_NO_HEADER_INDEX))
			{
				return {};
			}
			return WideToUtf8(std::wstring(buffer.data()));
		}

		std::wstring ResolveRedirect(const std::wstring& currentUrl, const std::string& location)
		{
			const std::wstring wideLocation = Utf8ToWide(location);
			if (wideLocation.empty())
				return {};
			if (wideLocation.rfind(L"https://", 0) == 0)
				return wideLocation;
			ParsedUrl current;
			if (!ParseAllowedHttpsUrl(currentUrl, current))
				return {};
			if (wideLocation.front() == L'/')
				return L"https://" + current.host + wideLocation;
			return {};
		}

		bool WriteDownloadedChunk(HANDLE file, const BYTE* data, DWORD size)
		{
			DWORD written = 0;
			return ::WriteFile(file, data, size, &written, nullptr) && written == size;
		}

		HttpResponse HttpGet(
			Context& context,
			const std::wstring& initialUrl,
			DWORD timeoutMs,
			std::uint64_t maximumBytes,
			const std::string& ifNoneMatch = {},
			const fs::path& outputFile = {},
			std::uint64_t expectedSize = 0)
		{
			HttpResponse response;
			std::wstring url = initialUrl;
			const ULONGLONG started = ::GetTickCount64();
			auto remaining = [&]() -> DWORD {
				const ULONGLONG elapsed = ::GetTickCount64() - started;
				if (elapsed >= timeoutMs)
					return 0;
				return timeoutMs - static_cast<DWORD>(elapsed);
			};

			InternetHandle session(::WinHttpOpen(
				L"vSMR-Updater/1.0 (+https://github.com/IWantPizzaa/vSMR)",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
			if (!session)
			{
				response.error = "winhttp_session";
				return response;
			}
			::WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, static_cast<int>(timeoutMs));

			for (int redirect = 0; redirect <= 5; ++redirect)
			{
				if (context.cancelled || remaining() == 0)
				{
					response.error = context.cancelled ? "cancelled" : "timeout";
					return response;
				}
				ParsedUrl parsed;
				if (!ParseAllowedHttpsUrl(url, parsed))
				{
					response.error = "unsafe_url";
					return response;
				}
				InternetHandle connection(::WinHttpConnect(
					session.get(), parsed.host.c_str(), parsed.port, 0));
				if (!connection)
				{
					response.error = "connect_failed";
					return response;
				}
				InternetHandle request(::WinHttpOpenRequest(
					connection.get(), L"GET", parsed.resource.c_str(), nullptr,
					WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
				if (!request)
				{
					response.error = "request_failed";
					return response;
				}
				DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
				::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
					&redirectPolicy, sizeof(redirectPolicy));
				const DWORD operationRemaining = remaining();
				::WinHttpSetTimeouts(request.get(), 3000, 3000, 3000, operationRemaining);

				std::wstring headers =
					L"Accept: application/vnd.github+json\r\n"
					L"X-GitHub-Api-Version: 2022-11-28\r\n";
				if (!ifNoneMatch.empty())
					headers += L"If-None-Match: " + Utf8ToWide(ifNoneMatch) + L"\r\n";

				std::uint64_t resumeOffset = 0;
				if (!outputFile.empty())
				{
					std::error_code error;
					resumeOffset = fs::file_size(outputFile, error);
					if (error || resumeOffset >= maximumBytes ||
						(expectedSize > 0 && resumeOffset >= expectedSize))
					{
						resumeOffset = 0;
					}
					if (resumeOffset > 0)
						headers += L"Range: bytes=" + std::to_wstring(resumeOffset) + L"-\r\n";
				}

				BOOL ok = ::WinHttpSendRequest(
					request.get(), headers.c_str(), static_cast<DWORD>(-1),
					WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
				if (ok)
					ok = ::WinHttpReceiveResponse(request.get(), nullptr);
				if (!ok)
				{
					response.error = ::GetLastError() == ERROR_WINHTTP_TIMEOUT
						? "timeout" : "network_error";
					return response;
				}
				DWORD status = 0;
				DWORD statusSize = sizeof(status);
				if (!::WinHttpQueryHeaders(
					request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
					WINHTTP_NO_HEADER_INDEX))
				{
					response.error = "missing_status";
					return response;
				}
				if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
				{
					const std::string location = QueryHeader(request.get(), WINHTTP_QUERY_LOCATION);
					url = ResolveRedirect(url, location);
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
				response.rateLimitReset = QueryCustomHeader(request.get(), L"X-RateLimit-Reset");
				response.finalUrl = url;
				if (status == 304 || status == 403 || status == 429 || status < 200 || status >= 300)
					return response;

				const bool append = !outputFile.empty() && resumeOffset > 0 && status == 206;
				if (!append)
					resumeOffset = 0;
				UniqueHandle file;
				if (!outputFile.empty())
				{
					std::error_code error;
					fs::create_directories(outputFile.parent_path(), error);
					if (error)
					{
						response.error = "staging_directory";
						return response;
					}
					file.reset(::CreateFileW(
						outputFile.c_str(), GENERIC_WRITE, 0, nullptr,
						append ? OPEN_EXISTING : CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL, nullptr));
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
					if (context.cancelled || remaining() == 0)
					{
						response.error = context.cancelled ? "cancelled" : "timeout";
						return response;
					}
					DWORD read = 0;
					if (!::WinHttpReadData(request.get(), buffer.data(),
						static_cast<DWORD>(buffer.size()), &read))
					{
						response.error = "read_failed";
						return response;
					}
					if (read == 0)
						break;
					if (received > maximumBytes - read)
					{
						response.error = "response_too_large";
						return response;
					}
					received += read;
					if (file)
					{
						if (!WriteDownloadedChunk(file.get(), buffer.data(), read))
						{
							response.error = "write_failed";
							return response;
						}
					}
					else
					{
						response.body.insert(response.body.end(), buffer.begin(), buffer.begin() + read);
					}
					if (expectedSize > 0 && !outputFile.empty())
					{
						const int percent = static_cast<int>((std::min)(100ULL, received * 100ULL / expectedSize));
						if (percent >= lastPercent + 5)
						{
							lastPercent = percent;
							if (!Report(context, ProgressStage::Downloading, percent,
								L"Downloading vSMR update..."))
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
				if (expectedSize > 0 && received != expectedSize)
				{
					response.error = "size_mismatch";
					return response;
				}
				return response;
			}
			response.error = "too_many_redirects";
			return response;
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
						ParsedUrl parsed;
						if (asset.name.empty() || !names.insert(asset.name).second ||
							asset.url.empty() || !ParseAllowedHttpsUrl(asset.url, parsed))
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
			HttpResponse response = HttpGet(
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

		bool Sha256Bytes(const BYTE* bytes, std::size_t size, std::string& digest)
		{
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::vector<BYTE> hashObject;
			std::array<BYTE, 32> result{};
			DWORD objectLength = 0;
			DWORD resultLength = 0;
			bool success = false;
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
				goto cleanup;
			if (BCryptGetProperty(
				algorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
				&resultLength, 0) < 0 || objectLength == 0)
			{
				goto cleanup;
			}
			hashObject.resize(objectLength);
			if (BCryptCreateHash(
				algorithm, &hash, hashObject.data(), objectLength,
				nullptr, 0, 0) < 0)
			{
				goto cleanup;
			}
			while (size > 0)
			{
				const ULONG chunk = static_cast<ULONG>((std::min)(size, static_cast<std::size_t>(1024 * 1024)));
				if (BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0) < 0)
					goto cleanup;
				bytes += chunk;
				size -= chunk;
			}
			if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
				goto cleanup;
			digest = Hex(result.data(), static_cast<DWORD>(result.size()));
			success = true;

		cleanup:
			if (hash != nullptr)
				BCryptDestroyHash(hash);
			if (algorithm != nullptr)
				BCryptCloseAlgorithmProvider(algorithm, 0);
			return success;
		}

		bool Sha256File(const fs::path& path, std::string& digest)
		{
			UniqueHandle file(::CreateFileW(
				path.c_str(), GENERIC_READ, FILE_SHARE_READ,
				nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
			if (!file)
				return false;
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::vector<BYTE> hashObject;
			std::array<BYTE, 32> result{};
			std::array<BYTE, 128 * 1024> buffer{};
			DWORD objectLength = 0;
			DWORD resultLength = 0;
			bool success = false;
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
				goto cleanup;
			if (BCryptGetProperty(
				algorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
				&resultLength, 0) < 0 || objectLength == 0)
			{
				goto cleanup;
			}
			hashObject.resize(objectLength);
			if (BCryptCreateHash(
				algorithm, &hash, hashObject.data(), objectLength,
				nullptr, 0, 0) < 0)
			{
				goto cleanup;
			}
			for (;;)
			{
				DWORD read = 0;
				if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
					goto cleanup;
				if (read == 0)
					break;
				if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
					goto cleanup;
			}
			if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
				goto cleanup;
			digest = Hex(result.data(), static_cast<DWORD>(result.size()));
			success = true;

		cleanup:
			if (hash != nullptr)
				BCryptDestroyHash(hash);
			if (algorithm != nullptr)
				BCryptCloseAlgorithmProvider(algorithm, 0);
			return success;
		}

		bool VerifyAuthenticodeAndGetSignerHash(
			const fs::path& file,
			std::string& signerCertificateSha256)
		{
			WINTRUST_FILE_INFO fileInfo{};
			fileInfo.cbStruct = sizeof(fileInfo);
			fileInfo.pcwszFilePath = file.c_str();
			WINTRUST_DATA trustData{};
			trustData.cbStruct = sizeof(trustData);
			trustData.dwUIChoice = WTD_UI_NONE;
			trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
			trustData.dwUnionChoice = WTD_CHOICE_FILE;
			trustData.pFile = &fileInfo;
			trustData.dwStateAction = WTD_STATEACTION_VERIFY;
			trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
			GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
			const LONG trustStatus = ::WinVerifyTrust(nullptr, &policy, &trustData);
			trustData.dwStateAction = WTD_STATEACTION_CLOSE;
			::WinVerifyTrust(nullptr, &policy, &trustData);
			if (trustStatus != ERROR_SUCCESS)
				return false;

			HCERTSTORE store = nullptr;
			HCRYPTMSG message = nullptr;
			DWORD encoding = 0;
			DWORD contentType = 0;
			DWORD formatType = 0;
			if (!::CryptQueryObject(
				CERT_QUERY_OBJECT_FILE, file.c_str(),
				CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
				CERT_QUERY_FORMAT_FLAG_BINARY, 0,
				&encoding, &contentType, &formatType,
				&store, &message, nullptr))
			{
				return false;
			}
			DWORD signerSize = 0;
			bool success = false;
			if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) && signerSize > 0)
			{
				std::vector<BYTE> signerBuffer(signerSize);
				if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(), &signerSize))
				{
					const auto signer = reinterpret_cast<PCMSG_SIGNER_INFO>(signerBuffer.data());
					CERT_INFO certificateInfo{};
					certificateInfo.Issuer = signer->Issuer;
					certificateInfo.SerialNumber = signer->SerialNumber;
					CertContextHandle certificate(::CertFindCertificateInStore(
						store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
						CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr));
					if (certificate)
						success = Sha256Bytes(
							certificate.get()->pbCertEncoded,
							certificate.get()->cbCertEncoded,
							signerCertificateSha256);
				}
			}
			if (message != nullptr)
				::CryptMsgClose(message);
			if (store != nullptr)
				::CertCloseStore(store, 0);
			return success;
		}

		std::string ResolveTrustedSignerHash(const StartupOptions& options)
		{
			std::string configured = ToLowerAscii(VSMR_UPDATE_SIGNER_CERT_SHA256);
			configured.erase(std::remove_if(configured.begin(), configured.end(), [](unsigned char character) {
				return std::isspace(character) != 0;
			}), configured.end());
			if (IsHex(configured, 64))
				return configured;
			std::string signer;
			if (VerifyAuthenticodeAndGetSignerHash(options.loaderPath, signer) && IsHex(signer, 64))
				return ToLowerAscii(signer);
			return {};
		}

		bool VerifyDetachedCms(
			const std::vector<std::uint8_t>& content,
			const std::vector<std::uint8_t>& signature,
			const std::string& expectedSignerSha256,
			std::string& error)
		{
			if (!IsHex(expectedSignerSha256, 64))
			{
				error = "signature_required";
				return false;
			}
			CRYPT_VERIFY_MESSAGE_PARA parameters{};
			parameters.cbSize = sizeof(parameters);
			parameters.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
			const BYTE* contents[] = { content.data() };
			DWORD contentSizes[] = { static_cast<DWORD>(content.size()) };
			PCCERT_CONTEXT signerRaw = nullptr;
			if (!::CryptVerifyDetachedMessageSignature(
				&parameters, 0,
				signature.data(), static_cast<DWORD>(signature.size()),
				1, contents, contentSizes, &signerRaw))
			{
				error = "manifest_signature_invalid";
				return false;
			}
			CertContextHandle signer(signerRaw);
			std::string actualSigner;
			if (!Sha256Bytes(
				signer.get()->pbCertEncoded,
				signer.get()->cbCertEncoded,
				actualSigner) ||
				ToLowerAscii(actualSigner) != ToLowerAscii(expectedSignerSha256))
			{
				error = "manifest_signer_mismatch";
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
			HttpResponse manifestResponse = HttpGet(
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
			HttpResponse signatureResponse = HttpGet(
				context, signatureAsset->url, timeout, kMaximumSignatureBytes);
			if (signatureResponse.statusCode != 200 || !signatureResponse.error.empty() ||
				signatureResponse.body.size() != signatureAsset->size)
			{
				error = signatureResponse.error.empty() ? "signature_download_failed" : signatureResponse.error;
				return false;
			}
			if (!VerifyDetachedCms(
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

		struct FixtureCandidate
		{
			Manifest manifest;
			fs::path manifestPath;
			fs::path archivePath;
			std::vector<std::uint8_t> manifestBytes;
		};

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

		std::wstring QuoteCommandLineArgument(const std::wstring& argument)
		{
			if (argument.empty())
				return L"\"\"";
			if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
				return argument;
			std::wstring quoted = L"\"";
			std::size_t backslashes = 0;
			for (const wchar_t character : argument)
			{
				if (character == L'\\')
				{
					++backslashes;
					continue;
				}
				if (character == L'\"')
				{
					quoted.append(backslashes * 2 + 1, L'\\');
					quoted.push_back(L'\"');
					backslashes = 0;
					continue;
				}
				quoted.append(backslashes, L'\\');
				backslashes = 0;
				quoted.push_back(character);
			}
			quoted.append(backslashes * 2, L'\\');
			quoted.push_back(L'\"');
			return quoted;
		}

		fs::path PowerShellPath()
		{
			std::array<wchar_t, MAX_PATH + 1> windows{};
			const UINT length = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
			if (length == 0 || length >= windows.size())
				return {};
			const fs::path path = fs::path(std::wstring(windows.data(), length)) /
				L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
			return IsRegularFile(path) ? path : fs::path{};
		}

		bool RunProcess(
			const fs::path& executable,
			const std::vector<std::wstring>& arguments,
			DWORD timeoutMs,
			DWORD& exitCode,
			bool terminateOnTimeout,
			const std::function<void()>& pulse = {},
			const std::vector<HANDLE>& handlesToInherit = {})
		{
			if (executable.empty() || !IsRegularFile(executable))
				return false;
			std::wstring commandLine = QuoteCommandLineArgument(executable.wstring());
			for (const auto& argument : arguments)
			{
				commandLine.push_back(L' ');
				commandLine += QuoteCommandLineArgument(argument);
			}
			std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
			mutableCommand.push_back(L'\0');
			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(startup.StartupInfo);
			startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
			startup.StartupInfo.wShowWindow = SW_HIDE;
			std::vector<UniqueHandle> inheritedDuplicates;
			std::vector<HANDLE> inheritedValues;
			std::vector<BYTE> attributeStorage;
			if (!handlesToInherit.empty())
			{
				for (HANDLE handle : handlesToInherit)
				{
					if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
						return false;
					HANDLE duplicate = nullptr;
					if (!::DuplicateHandle(
						::GetCurrentProcess(), handle, ::GetCurrentProcess(), &duplicate,
						0, TRUE, DUPLICATE_SAME_ACCESS))
					{
						return false;
					}
					inheritedDuplicates.emplace_back(duplicate);
					inheritedValues.push_back(duplicate);
				}
				SIZE_T attributeBytes = 0;
				::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
				if (attributeBytes == 0)
					return false;
				attributeStorage.resize(attributeBytes);
				startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
					attributeStorage.data());
				if (!::InitializeProcThreadAttributeList(
					startup.lpAttributeList, 1, 0, &attributeBytes))
				{
					startup.lpAttributeList = nullptr;
					return false;
				}
				if (!::UpdateProcThreadAttribute(
						startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
						inheritedValues.data(), inheritedValues.size() * sizeof(HANDLE),
						nullptr, nullptr))
				{
					if (startup.lpAttributeList != nullptr)
						::DeleteProcThreadAttributeList(startup.lpAttributeList);
					return false;
				}
				startup.StartupInfo.cb = sizeof(startup);
			}
			PROCESS_INFORMATION process{};
			const BOOL created = ::CreateProcessW(
				executable.c_str(), mutableCommand.data(), nullptr, nullptr,
				handlesToInherit.empty() ? FALSE : TRUE,
				CREATE_NO_WINDOW | (handlesToInherit.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT),
				nullptr, nullptr, &startup.StartupInfo, &process);
			if (startup.lpAttributeList != nullptr)
				::DeleteProcThreadAttributeList(startup.lpAttributeList);
			// The parent keeps only its original transaction lock. The duplicated
			// inheritable handles exist solely in the child after CreateProcess.
			inheritedDuplicates.clear();
			if (!created)
			{
				return false;
			}
			UniqueHandle processHandle(process.hProcess);
			UniqueHandle threadHandle(process.hThread);
			const ULONGLONG started = ::GetTickCount64();
			for (;;)
			{
				const DWORD wait = ::WaitForSingleObject(processHandle.get(), 250);
				if (wait == WAIT_OBJECT_0)
					break;
				if (wait != WAIT_TIMEOUT)
					return false;
				if (pulse)
					pulse();
				if (timeoutMs != INFINITE && ::GetTickCount64() - started >= timeoutMs)
				{
					if (terminateOnTimeout)
					{
						::TerminateProcess(processHandle.get(), ERROR_TIMEOUT);
						::WaitForSingleObject(processHandle.get(), 5000);
					}
					return false;
				}
			}
			return ::GetExitCodeProcess(processHandle.get(), &exitCode) && exitCode == 0;
		}

		bool IsPathBelow(const fs::path& child, const fs::path& parent)
		{
			std::error_code error;
			const std::wstring childText = ToLowerWide(fs::absolute(child, error).lexically_normal().wstring());
			if (error)
				return false;
			const std::wstring parentText = ToLowerWide(fs::absolute(parent, error).lexically_normal().wstring());
			if (error || childText.size() <= parentText.size())
				return false;
			return childText.compare(0, parentText.size(), parentText) == 0 &&
				(childText[parentText.size()] == L'\\' || childText[parentText.size()] == L'/');
		}

		const char kSafeExtractionScript[] = R"VSMRPS(
param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archivePath = [IO.Path]::GetFullPath($Archive)
$destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
if (-not [IO.File]::Exists($archivePath)) { throw 'Archive missing.' }
if ([IO.Directory]::Exists($destinationPath)) { [IO.Directory]::Delete($destinationPath, $true) }
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
$rootPrefix = $destinationPath + [IO.Path]::DirectorySeparatorChar
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($zip.Entries.Count -gt 20000) { throw 'Archive has too many entries.' }
    [UInt64]$total = 0
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName.Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
            $relative.Contains(':') -or $relative.StartsWith('\') -or
            @($relative.Split('\') | Where-Object { $_ -eq '..' }).Count -gt 0) {
            throw "Unsafe archive entry: $relative"
        }
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($destinationPath, $relative))
        if (-not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Archive entry escapes destination: $relative"
        }
        $key = $target.TrimEnd('\','/')
        if (-not $seen.Add($key)) { throw "Duplicate archive entry: $relative" }
        if ([UInt64]$entry.Length -gt 268435456) { throw "Archive entry too large: $relative" }
        $total += [UInt64]$entry.Length
        if ($total -gt 805306368) { throw 'Archive expanded size is too large.' }
    }
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName.Replace('/', '\')
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($destinationPath, $relative))
        if ($relative.EndsWith('\')) {
            [IO.Directory]::CreateDirectory($target) | Out-Null
            continue
        }
        $parent = [IO.Path]::GetDirectoryName($target)
        if (-not [string]::IsNullOrWhiteSpace($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        $source = $entry.Open()
        try {
            $destinationFile = New-Object IO.FileStream($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $source.CopyTo($destinationFile) } finally { $destinationFile.Dispose() }
        }
        finally { $source.Dispose() }
    }
}
finally { $zip.Dispose() }
)VSMRPS";

		bool SafelyExtractArchive(
			Context& context,
			const fs::path& archive,
			const fs::path& destination,
			std::string& error)
		{
			if (!IsPathBelow(destination, context.storageRoot))
			{
				error = "unsafe_extraction_destination";
				return false;
			}
			const fs::path script = context.storageRoot / L"safe_extract.ps1";
			if (!AtomicWriteText(script, kSafeExtractionScript))
			{
				error = "extractor_write_failed";
				return false;
			}
			DWORD exitCode = 0;
			const DWORD timeout = RemainingMs(context, 30000);
			if (timeout < 1000)
			{
				error = "deadline";
				return false;
			}
			if (!RunProcess(
				PowerShellPath(),
				{ L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
				  L"-File", script.wstring(), L"-Archive", archive.wstring(),
				  L"-Destination", destination.wstring() },
				timeout, exitCode, true))
			{
				error = exitCode == ERROR_TIMEOUT ? "extraction_timeout" : "extraction_failed";
				return false;
			}
			return true;
		}

		class OwnedMutex
		{
		public:
			OwnedMutex() = default;
			explicit OwnedMutex(UniqueHandle handle) noexcept : handle_(std::move(handle)), owned_(true) {}
			~OwnedMutex()
			{
				if (owned_ && handle_)
					::ReleaseMutex(handle_.get());
			}
			OwnedMutex(const OwnedMutex&) = delete;
			OwnedMutex& operator=(const OwnedMutex&) = delete;
			OwnedMutex(OwnedMutex&& other) noexcept
				: handle_(std::move(other.handle_)), owned_(other.owned_)
			{
				other.owned_ = false;
			}
			explicit operator bool() const noexcept { return owned_ && static_cast<bool>(handle_); }

		private:
			UniqueHandle handle_;
			bool owned_ = false;
		};

		OwnedMutex AcquireUpdaterMutex(const fs::path& installRoot)
		{
			(void)installRoot;
			const std::wstring name = L"Local\\vSMR.Updater.Global";
			UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, name.c_str()));
			if (!mutex)
				return {};
			const DWORD wait = ::WaitForSingleObject(mutex.get(), 0);
			if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
				return {};
			return OwnedMutex(std::move(mutex));
		}

		OwnedMutex AcquireHealthMarkerMutex(const fs::path& markerPath)
		{
			std::error_code error;
			const std::wstring normalized = ToLowerWide(
				fs::absolute(markerPath, error).lexically_normal().wstring());
			if (error || normalized.empty())
				return {};
			std::wostringstream name;
			name << L"Local\\vSMR.Updater.Health." << std::hex << std::setfill(L'0')
				<< std::setw(16) << Fnv1a64(normalized);
			UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, name.str().c_str()));
			if (!mutex)
				return {};
			const DWORD wait = ::WaitForSingleObject(mutex.get(), 5000);
			if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
				return {};
			return OwnedMutex(std::move(mutex));
		}

		UniqueHandle AcquireExclusiveSessionLock(
			const fs::path& storageRoot,
			const fs::path& installRoot)
		{
			if (storageRoot.empty() || installRoot.empty())
				return {};
			const fs::path path = SessionLockPath(storageRoot, installRoot);
			std::error_code error;
			fs::create_directories(path.parent_path(), error);
			if (error)
				return {};
			return UniqueHandle(::CreateFileW(
				path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
		}

		bool CopyFileAtomically(const fs::path& source, const fs::path& destination)
		{
			std::error_code error;
			fs::create_directories(destination.parent_path(), error);
			if (error)
				return false;
			const fs::path temporary = destination.wstring() + L".tmp." + std::to_wstring(::GetTickCount64());
			if (!::CopyFileW(source.c_str(), temporary.c_str(), TRUE))
				return false;
			if (!::MoveFileExW(
				temporary.c_str(), destination.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				::DeleteFileW(temporary.c_str());
				return false;
			}
			return true;
		}

		bool ReadInstallationMetadata(
			const fs::path& dataRoot,
			std::string& installedVersion,
			fs::path& rollbackBackup)
		{
			std::string json;
			if (!ReadText(dataRoot / L"INSTALLATION.json", json, 128 * 1024))
				return false;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject())
				return false;
			installedVersion = JsonString(document, "installed_version");
			rollbackBackup = fs::path(Utf8ToWide(JsonString(document, "rollback_backup")));
			return ParseSemVer(installedVersion).valid && !rollbackBackup.empty();
		}

		fs::path FindNewestRollbackBackup(
			const fs::path& storageRoot,
			const fs::path& installRoot,
			const std::string& installingVersion)
		{
			const fs::path backupRoot = storageRoot / L"backups" / HashName(installRoot);
			std::error_code error;
			fs::path selected;
			fs::file_time_type selectedTime{};
			for (fs::directory_iterator iterator(backupRoot, error), end;
				!error && iterator != end; iterator.increment(error))
			{
				if (!iterator->is_directory(error) || error)
					continue;
				std::string json;
				if (!ReadText(iterator->path() / L"BACKUP-METADATA.json", json, 128 * 1024))
					continue;
				rapidjson::Document document;
				document.Parse<0>(json.c_str());
				if (document.HasParseError() || !document.IsObject() ||
					JsonString(document, "kind") != "vSMR complete pre-install backup" ||
					JsonString(document, "installing_version") != installingVersion)
				{
					continue;
				}
				std::error_code timeError;
				const auto time = fs::last_write_time(iterator->path(), timeError);
				if (!timeError && (selected.empty() || time > selectedTime))
				{
					selected = iterator->path();
					selectedTime = time;
				}
			}
			return selected;
		}

		bool ReadReleaseVersion(const fs::path& dataRoot, std::string& version)
		{
			std::string json;
			if (!ReadText(dataRoot / L"RELEASE-METADATA.json", json, 128 * 1024))
				return false;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject())
				return false;
			version = JsonString(document, "version");
			return ParseSemVer(version).valid;
		}

		fs::path RecoveryScriptPath(
			const fs::path& storageRoot,
			const fs::path& installRoot,
			const std::string& version)
		{
			return storageRoot / L"recovery" / HashName(installRoot) /
				Utf8ToWide(version) / L"restore_vsmr_backup.ps1";
		}

		bool StageRecoveryScript(
			Context& context,
			const fs::path& packageRoot,
			const Manifest& manifest,
			std::string& error)
		{
			const fs::path source = packageRoot / L"vSMR_Data" / L"Tools" / L"restore_vsmr_backup.ps1";
			const fs::path destination = RecoveryScriptPath(
				context.storageRoot, context.options.installRoot, manifest.version.normalized);
			if (!IsRegularFile(source) || !CopyFileAtomically(source, destination))
			{
				error = "recovery_helper_staging_failed";
				return false;
			}
			return true;
		}

		bool RunInstaller(
			Context& context,
			const fs::path& packageRoot,
			bool preserveLoader,
			bool reloadAviso,
			bool replaceModifiedAviso,
			HANDLE installationSessionLock,
			std::string& error)
		{
			const fs::path installer = packageRoot / L"vSMR_Data" / L"Tools" / L"install_vsmr.ps1";
			if (!IsRegularFile(installer))
			{
				error = "package_installer_missing";
				return false;
			}
			const fs::path backupRoot = context.storageRoot / L"backups" / HashName(context.options.installRoot);
			std::vector<std::wstring> arguments = {
				L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
				L"-File", installer.wstring(),
				L"-DestinationDirectory", context.options.installRoot.wstring(),
				L"-BackupRoot", backupRoot.wstring()
			};
			if (preserveLoader)
				arguments.push_back(L"-PreserveLoader");
			if (reloadAviso)
				arguments.push_back(L"-ReloadAviso");
			if (replaceModifiedAviso)
				arguments.push_back(L"-ReplaceModifiedAviso");
			DWORD exitCode = 0;
			ULONGLONG lastPulse = 0;
			if (!RunProcess(
				PowerShellPath(), arguments, INFINITE, exitCode, false,
				[&]() {
					const ULONGLONG now = ::GetTickCount64();
					if (now - lastPulse >= 1000)
					{
						lastPulse = now;
						Report(
							context, ProgressStage::Installing, -1,
							reloadAviso ? L"Reloading AVISO data..." : L"Installing vSMR update...");
					}
				},
				{ installationSessionLock }))
			{
				error = exitCode == ERROR_TIMEOUT ? "installer_timeout" : "installer_failed";
				return false;
			}
			return true;
		}

		fs::path HealthMarkerPath(const fs::path& storageRoot, const fs::path& installRoot)
		{
			return storageRoot / L"health" / (HashName(installRoot) + L".pending.json");
		}

		bool ProcessCreationStamp(DWORD processId, std::uint64_t& stamp, bool& alive)
		{
			alive = false;
			stamp = 0;
			UniqueHandle process(::OpenProcess(
				PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
				FALSE, processId));
			if (!process)
			{
				// ERROR_INVALID_PARAMETER is the documented result for a PID that no
				// longer exists. Other failures (notably access denied) leave the
				// identity unknown and must not trigger a rollback.
				if (::GetLastError() == ERROR_INVALID_PARAMETER)
					return true;
				return false;
			}
			alive = ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!::GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
				return false;
			ULARGE_INTEGER value{};
			value.LowPart = created.dwLowDateTime;
			value.HighPart = created.dwHighDateTime;
			stamp = value.QuadPart;
			return true;
		}

		bool WriteHealthMarker(
			const StartupOptions& options,
			const fs::path& storageRoot,
			StartupResult& result,
			const std::string& phase = "attempting")
		{
			result.healthMarkerPath = HealthMarkerPath(storageRoot, options.installRoot);
			result.updaterStoragePath = storageRoot;
			rapidjson::Document document;
			document.SetObject();
			document.AddMember("schema_version", 1, document.GetAllocator());
			AddJsonString(document, document, "install_root", WideToUtf8(options.installRoot.wstring()));
			AddJsonString(document, document, "version", result.selectedVersion);
			AddJsonString(document, document, "previous_version", result.previousVersion);
			AddJsonString(document, document, "previous_runtime_sha256", result.previousRuntimeSha256);
			AddJsonString(document, document, "phase", phase);
			AddJsonString(document, document, "rollback_backup", WideToUtf8(result.rollbackBackupPath.wstring()));
			AddJsonString(document, document, "previous_runtime", WideToUtf8(result.previousRuntimePath.wstring()));
			if (phase == "attempting")
			{
				const DWORD processId = options.hostProcessId == 0
					? ::GetCurrentProcessId() : options.hostProcessId;
				std::uint64_t processCreated = 0;
				bool alive = false;
				if (!ProcessCreationStamp(processId, processCreated, alive) || !alive)
					return false;
				document.AddMember("attempt_pid", static_cast<std::uint64_t>(processId), document.GetAllocator());
				document.AddMember("attempt_process_created_100ns", processCreated, document.GetAllocator());
			}
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return AtomicWriteText(
				result.healthMarkerPath,
				std::string(buffer.GetString(), buffer.Size()) + "\n");
		}

		bool ReadHealthMarker(
			const StartupOptions& options,
			const fs::path& markerPath,
			StartupResult& result,
			std::string& phase,
			std::uint32_t& attemptProcessId,
			std::uint64_t& attemptProcessCreated)
		{
			std::string json;
			if (!ReadText(markerPath, json, 128 * 1024))
				return false;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject() ||
				!document.HasMember("schema_version") || !document["schema_version"].IsInt() ||
				document["schema_version"].GetInt() != 1 ||
				JsonString(document, "install_root") != WideToUtf8(options.installRoot.wstring()))
			{
				return false;
			}
			result.selectedVersion = JsonString(document, "version");
			result.previousVersion = JsonString(document, "previous_version");
			result.previousRuntimeSha256 = ToLowerAscii(JsonString(document, "previous_runtime_sha256"));
			phase = JsonString(document, "phase");
			const std::uint64_t processId = JsonUint64(document, "attempt_pid");
			attemptProcessId = processId <= (std::numeric_limits<std::uint32_t>::max)()
				? static_cast<std::uint32_t>(processId) : 0;
			attemptProcessCreated = JsonUint64(document, "attempt_process_created_100ns");
			result.rollbackBackupPath = Utf8ToWide(JsonString(document, "rollback_backup"));
			result.previousRuntimePath = Utf8ToWide(JsonString(document, "previous_runtime"));
			result.healthMarkerPath = markerPath;
			result.installationRoot = options.installRoot;
			return (phase == "attempting" || phase == "installing" ||
				phase == "failed" || phase == "healthy") &&
				(phase != "attempting" || (attemptProcessId != 0 && attemptProcessCreated != 0)) &&
				ParseSemVer(result.selectedVersion).valid &&
				ParseSemVer(result.previousVersion).valid &&
				IsHex(result.previousRuntimeSha256, 64) &&
				(phase == "installing" ||
					(!result.rollbackBackupPath.empty() && !result.previousRuntimePath.empty()));
		}

		bool RewriteHealthMarkerPhase(
			const StartupResult& update,
			const std::set<std::string>& allowedCurrentPhases,
			const char* newPhase)
		{
			const fs::path storage = update.updaterStoragePath.empty()
				? GetUpdaterStorageDirectory() : update.updaterStoragePath;
			if (storage.empty() || update.healthMarkerPath.empty() ||
				!IsPathBelow(update.healthMarkerPath, storage / L"health"))
			{
				return false;
			}
			std::string json;
			if (!ReadText(update.healthMarkerPath, json, 128 * 1024))
				return false;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject() ||
				!document.HasMember("schema_version") || !document["schema_version"].IsInt() ||
				document["schema_version"].GetInt() != 1 ||
				update.installationRoot.empty() ||
				JsonString(document, "install_root") != WideToUtf8(update.installationRoot.wstring()) ||
				JsonString(document, "version") != update.selectedVersion ||
				JsonString(document, "previous_version") != update.previousVersion ||
				ToLowerAscii(JsonString(document, "previous_runtime_sha256")) !=
					ToLowerAscii(update.previousRuntimeSha256) ||
				JsonString(document, "rollback_backup") != WideToUtf8(update.rollbackBackupPath.wstring()) ||
				JsonString(document, "previous_runtime") != WideToUtf8(update.previousRuntimePath.wstring()) ||
				allowedCurrentPhases.find(JsonString(document, "phase")) == allowedCurrentPhases.end())
			{
				return false;
			}
			document["phase"].SetString(newPhase, document.GetAllocator());
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return AtomicWriteText(
				update.healthMarkerPath,
				std::string(buffer.GetString(), buffer.Size()) + "\n");
		}

		bool WriteQuarantineMarker(
			const fs::path& storageRoot,
			const std::string& version,
			const std::string& reason)
		{
			if (!ParseSemVer(version).valid)
				return false;
			rapidjson::Document document;
			document.SetObject();
			document.AddMember("schema_version", 1, document.GetAllocator());
			AddJsonString(document, document, "version", version);
			AddJsonString(document, document, "quarantined_utc", UtcNow());
			AddJsonString(document, document, "reason", reason);
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return AtomicWriteText(
				storageRoot / L"quarantine" / (Utf8ToWide(version) + L".json"),
				std::string(buffer.GetString(), buffer.Size()) + "\n");
		}

		bool IsX86PortableExecutable(const fs::path& path);

		bool RunRollback(
			Context& context,
			const StartupResult& update,
			fs::path& restoredRuntimePath,
			std::string& error,
			HANDLE existingSessionLock = nullptr,
			const char* quarantineReason = nullptr)
		{
			if (update.rollbackBackupPath.empty() ||
				!IsPathBelow(update.rollbackBackupPath, context.storageRoot / L"backups"))
			{
				error = "rollback_backup_unsafe";
				return false;
			}
			const fs::path backupRuntime = update.rollbackBackupPath /
				L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
			std::string expectedRuntimeHash;
			if (!IsRegularFile(backupRuntime) || !IsX86PortableExecutable(backupRuntime) ||
				!Sha256File(backupRuntime, expectedRuntimeHash) ||
				(!update.previousRuntimeSha256.empty() &&
					ToLowerAscii(expectedRuntimeHash) != ToLowerAscii(update.previousRuntimeSha256)))
			{
				error = "rollback_runtime_invalid";
				return false;
			}
			const fs::path restoreScript = RecoveryScriptPath(
				context.storageRoot, context.options.installRoot, update.selectedVersion);
			if (!IsRegularFile(restoreScript))
			{
				error = "rollback_script_missing";
				return false;
			}
			UniqueHandle sessionLock;
			if (existingSessionLock == nullptr || existingSessionLock == INVALID_HANDLE_VALUE)
				sessionLock = AcquireExclusiveSessionLock(
					context.sessionLockStorageRoot, context.options.installRoot);
			if ((existingSessionLock == nullptr || existingSessionLock == INVALID_HANDLE_VALUE) && !sessionLock)
			{
				error = "active_session";
				return false;
			}
			DWORD exitCode = 0;
			const HANDLE effectiveSessionLock = sessionLock ? sessionLock.get() : existingSessionLock;
			if (!RunProcess(
				PowerShellPath(),
				{ L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
				  L"-File", restoreScript.wstring(),
				  L"-DestinationDirectory", context.options.installRoot.wstring(),
				  L"-BackupDirectory", update.rollbackBackupPath.wstring(),
				  L"-PreserveLoader" },
				INFINITE, exitCode, false,
				[&]() { Report(context, ProgressStage::Fallback, -1, L"Rolling back vSMR update..."); },
				{ effectiveSessionLock }))
			{
				error = exitCode == ERROR_TIMEOUT ? "rollback_timeout" : "rollback_failed";
				return false;
			}
			restoredRuntimePath = fs::absolute(context.options.canonicalRuntimePath).lexically_normal();
			std::string restoredRuntimeHash;
			if (!IsRegularFile(restoredRuntimePath) ||
				!IsX86PortableExecutable(restoredRuntimePath) ||
				!Sha256File(restoredRuntimePath, restoredRuntimeHash) ||
				ToLowerAscii(restoredRuntimeHash) != ToLowerAscii(expectedRuntimeHash))
			{
				error = "restored_runtime_missing";
				return false;
			}
			std::string restoredVersion;
			if (!ReadReleaseVersion(context.options.dataRoot, restoredVersion) ||
				!SameSemVerIdentity(restoredVersion, update.previousVersion))
			{
				error = "rollback_verification_failed";
				return false;
			}
			if (quarantineReason != nullptr && quarantineReason[0] != '\0')
				WriteQuarantineMarker(context.storageRoot, update.selectedVersion, quarantineReason);
			if (!update.healthMarkerPath.empty())
				::DeleteFileW(update.healthMarkerPath.c_str());
			return true;
		}

		bool IsX86PortableExecutable(const fs::path& path)
		{
			UniqueHandle file(::CreateFileW(
				path.c_str(), GENERIC_READ, FILE_SHARE_READ,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!file)
				return false;
			IMAGE_DOS_HEADER dos{};
			DWORD read = 0;
			if (!::ReadFile(file.get(), &dos, sizeof(dos), &read, nullptr) ||
				read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
				dos.e_lfanew <= 0 || dos.e_lfanew > 16 * 1024 * 1024)
			{
				return false;
			}
			LARGE_INTEGER offset{};
			offset.QuadPart = dos.e_lfanew;
			if (!::SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN))
				return false;
			DWORD signature = 0;
			IMAGE_FILE_HEADER header{};
			if (!::ReadFile(file.get(), &signature, sizeof(signature), &read, nullptr) ||
				read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
				!::ReadFile(file.get(), &header, sizeof(header), &read, nullptr) ||
				read != sizeof(header))
			{
				return false;
			}
			return header.Machine == IMAGE_FILE_MACHINE_I386 &&
				(header.Characteristics & IMAGE_FILE_DLL) != 0;
		}

		bool ValidateExtractedPackage(
			Context& context,
			const fs::path& packageRoot,
			const Manifest& manifest,
			std::string& error)
		{
			const fs::path packagedLoader = packageRoot / L"vSMR.dll";
			const fs::path packagedRuntime = packageRoot / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
			std::error_code filesystemError;
			if (!IsRegularFile(packagedLoader) || !IsRegularFile(packagedRuntime) ||
				fs::file_size(packagedLoader, filesystemError) != manifest.loaderSize || filesystemError)
			{
				error = "package_binaries_missing";
				return false;
			}
			if (context.options.testFeedDirectory.empty())
			{
				std::string metadataJson;
				if (!ReadText(
					packageRoot / L"vSMR_Data" / L"RELEASE-METADATA.json",
					metadataJson, 256 * 1024))
				{
					error = "package_metadata_missing";
					return false;
				}
				rapidjson::Document metadata;
				metadata.Parse<0>(metadataJson.c_str());
				if (metadata.HasParseError() || !metadata.IsObject() ||
					!JsonBool(metadata, "publishable", false) ||
					JsonString(metadata, "version") != manifest.version.normalized ||
					!metadata.HasMember("automatic_update") ||
					!metadata["automatic_update"].IsObject() ||
					!JsonBool(metadata["automatic_update"], "publishable", false))
				{
					error = "package_not_publishable";
					return false;
				}
			}
			std::string loaderHash;
			if (!Sha256File(packagedLoader, loaderHash) ||
				ToLowerAscii(loaderHash) != manifest.loaderSha256)
			{
				error = "packaged_loader_hash_mismatch";
				return false;
			}
			if (!IsX86PortableExecutable(packagedLoader) || !IsX86PortableExecutable(packagedRuntime))
			{
				error = "package_architecture_invalid";
				return false;
			}
			const fs::path installer = packageRoot / L"vSMR_Data" / L"Tools" / L"install_vsmr.ps1";
			const fs::path restore = packageRoot / L"vSMR_Data" / L"Tools" / L"restore_vsmr_backup.ps1";
			if (!IsRegularFile(installer) || !IsRegularFile(restore))
			{
				error = "package_transaction_tools_missing";
				return false;
			}
			return true;
		}

		bool VerifyArchive(const fs::path& archive, const Manifest& manifest, std::string& error)
		{
			std::error_code filesystemError;
			const auto size = fs::file_size(archive, filesystemError);
			if (filesystemError || size != manifest.archiveSize)
			{
				error = "archive_size_mismatch";
				return false;
			}
			std::string digest;
			if (!Sha256File(archive, digest) || ToLowerAscii(digest) != manifest.archiveSha256)
			{
				error = "archive_hash_mismatch";
				return false;
			}
			return true;
		}

		bool PrepareRemoteArchive(
			Context& context,
			const Release& release,
			const Manifest& manifest,
			const std::vector<std::uint8_t>& manifestBytes,
			fs::path& archivePath,
			std::string& error)
		{
			const ReleaseAsset* asset = FindAsset(release, manifest.archiveName);
			if (asset == nullptr)
			{
				error = "archive_asset_missing";
				return false;
			}
			const fs::path versionRoot = context.storageRoot / L"staging" /
				HashName(context.options.installRoot) / Utf8ToWide(manifest.version.normalized);
			std::error_code filesystemError;
			fs::create_directories(versionRoot, filesystemError);
			if (filesystemError)
			{
				error = "staging_directory";
				return false;
			}
			AtomicWrite(
				versionRoot / Utf8ToWide("vSMR-" + manifest.version.normalized + ".update.json"),
				manifestBytes.data(), manifestBytes.size());
			archivePath = versionRoot / Utf8ToWide(manifest.archiveName);
			if (IsRegularFile(archivePath) && VerifyArchive(archivePath, manifest, error))
				return true;
			const fs::path partial = archivePath.wstring() + L".part";
			if (IsRegularFile(partial))
			{
				std::error_code sizeError;
				if (fs::file_size(partial, sizeError) == manifest.archiveSize && !sizeError)
				{
					if (VerifyArchive(partial, manifest, error) &&
						::MoveFileExW(partial.c_str(), archivePath.c_str(),
							MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					{
						return true;
					}
					::DeleteFileW(partial.c_str());
				}
			}
			const DWORD timeout = RemainingMs(context, kArchiveTimeoutMs);
			if (timeout < 1000)
			{
				error = "deadline";
				return false;
			}
			HttpResponse response = HttpGet(
				context, asset->url, timeout, kMaximumArchiveBytes,
				{}, partial, manifest.archiveSize);
			if ((response.statusCode != 200 && response.statusCode != 206) || !response.error.empty())
			{
				error = response.error.empty() ? "archive_download_failed" : response.error;
				return false;
			}
			if (!VerifyArchive(partial, manifest, error) ||
				!::MoveFileExW(partial.c_str(), archivePath.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				if (error.empty())
					error = "archive_commit_failed";
				return false;
			}
			return true;
		}

		StartupResult FailedOpen(
			Context& context,
			StartupResult result,
			const std::string& errorCode,
			const std::wstring& message,
			const std::string& stateStatus = "error")
		{
			result.status = errorCode == "cancelled"
				? StartupStatus::Cancelled : StartupStatus::FailedOpen;
			result.errorCode = errorCode;
			result.message = message;
			result.selectedRuntimePath = fs::absolute(context.options.canonicalRuntimePath).lexically_normal();
			result.selectedVersion = context.options.currentVersion;
			context.state.status = stateStatus;
			context.state.errorCode = errorCode;
			context.state.error = WideToUtf8(message);
			context.state.message = WideToUtf8(message);
			context.state.downloadPercent = -1;
			if (context.state.nextCheckUtc.empty())
				context.state.nextCheckUtc = UtcAfterSeconds(5 * 60);
			PersistState(context);
			Report(context, ProgressStage::Fallback, -1, message);
			return result;
		}

		StartupResult IntegrityFailure(
			Context& context,
			StartupResult result,
			const std::string& errorCode,
			const std::wstring& message)
		{
			result.status = StartupStatus::FailedOpen;
			result.selectedRuntimePath.clear();
			result.errorCode = errorCode;
			result.message = message;
			context.state.status = "error";
			context.state.errorCode = errorCode;
			context.state.error = WideToUtf8(message);
			context.state.message = context.state.error;
			PersistState(context);
			Report(context, ProgressStage::Fallback, -1, message);
			return result;
		}

		bool NormalizeOptions(const StartupOptions& source, StartupOptions& normalized)
		{
			try
			{
				normalized = source;
				normalized.installRoot = fs::absolute(source.installRoot).lexically_normal();
				normalized.dataRoot = fs::absolute(source.dataRoot).lexically_normal();
				normalized.canonicalRuntimePath = fs::absolute(source.canonicalRuntimePath).lexically_normal();
				normalized.loaderPath = fs::absolute(source.loaderPath).lexically_normal();
				if (!source.testFeedDirectory.empty())
					normalized.testFeedDirectory = fs::absolute(source.testFeedDirectory).lexically_normal();
				if (!source.testStorageDirectory.empty())
					normalized.testStorageDirectory = fs::absolute(source.testStorageDirectory).lexically_normal();
			}
			catch (...)
			{
				return false;
			}
			return !normalized.installRoot.empty() &&
				!normalized.dataRoot.empty() &&
				!normalized.canonicalRuntimePath.empty() &&
				!normalized.loaderPath.empty() &&
				IsPathBelow(normalized.dataRoot, normalized.installRoot) &&
				IsPathBelow(normalized.canonicalRuntimePath, normalized.dataRoot) &&
				ParseSemVer(normalized.currentVersion).valid &&
				ParseSemVer(normalized.loaderVersion).valid &&
				(normalized.testStorageDirectory.empty() ||
					(!normalized.testFeedDirectory.empty() && normalized.allowUnsignedTestManifest));
		}

		void CleanupNormalUpdaterState(const Context& context) noexcept;

		StartupResult PrepareUpdateImpl(const StartupOptions& startupOptions)
		{
			StartupResult result;
			result.selectedRuntimePath = startupOptions.canonicalRuntimePath;
			result.selectedVersion = startupOptions.currentVersion;
			result.installationRoot = startupOptions.installRoot;
			Context context(startupOptions);
			result.updaterStoragePath = context.storageRoot;
			std::error_code filesystemError;
			fs::create_directories(context.storageRoot, filesystemError);
			if (filesystemError)
				return FailedOpen(context, result, "storage_unavailable", L"Updater storage is unavailable.");

			OwnedMutex updaterMutex = AcquireUpdaterMutex(startupOptions.installRoot);
			if (!updaterMutex)
			{
				result.status = StartupStatus::Deferred;
				result.message = L"Another vSMR updater is already running.";
				context.state.status = "deferred";
				context.state.message = WideToUtf8(result.message);
				context.state.nextCheckUtc = UtcAfterSeconds(60);
				PersistState(context);
				return result;
			}

			// A marker survives process termination during runtime construction. Never
			// retry that runtime against its newly installed data; restore the complete
			// previous data package first and quarantine the failed version.
			const fs::path pendingHealth = HealthMarkerPath(context.storageRoot, startupOptions.installRoot);
			if (IsRegularFile(pendingHealth))
			{
				StartupResult unhealthy;
				unhealthy.updaterStoragePath = context.storageRoot;
				std::string healthPhase;
				std::uint32_t attemptProcessId = 0;
				std::uint64_t attemptProcessCreated = 0;
				if (!ReadHealthMarker(
					startupOptions, pendingHealth, unhealthy, healthPhase,
					attemptProcessId, attemptProcessCreated))
				{
					return IntegrityFailure(
						context, result, "health_marker_invalid",
						L"The pending update health marker is invalid.");
				}

				if (healthPhase == "healthy")
				{
					// A successful initializer may have confirmed while another Prepare
					// held the global mutex. The marker is authoritative and cleanup can
					// now be completed under the mutex held by this call.
					::DeleteFileW(pendingHealth.c_str());
				}
				else
				{
					UniqueHandle installingRecoveryLock;
					if (healthPhase == "installing")
					{
						// The installer child inherits this same exclusive file lock. If an
						// orphan transaction is still running, never inspect/delete its
						// journal or race it with a rollback.
						installingRecoveryLock = AcquireExclusiveSessionLock(
							context.sessionLockStorageRoot, startupOptions.installRoot);
						if (!installingRecoveryLock)
						{
							return IntegrityFailure(
								context, result, "install_transaction_still_active",
								L"An update transaction is still active; vSMR will not load until it finishes.");
						}
						std::string activeVersion;
						std::string activeRuntimeHash;
						if (ReadReleaseVersion(startupOptions.dataRoot, activeVersion) &&
							SameSemVerIdentity(activeVersion, unhealthy.previousVersion) &&
							IsX86PortableExecutable(startupOptions.canonicalRuntimePath) &&
							Sha256File(startupOptions.canonicalRuntimePath, activeRuntimeHash) &&
							ToLowerAscii(activeRuntimeHash) == ToLowerAscii(unhealthy.previousRuntimeSha256))
						{
							::DeleteFileW(pendingHealth.c_str());
							result.status = StartupStatus::FailedOpen;
							result.errorCode = "interrupted_update_was_not_committed";
							result.message = L"An interrupted update left the previous runtime intact.";
							context.state.status = "error";
							context.state.errorCode = result.errorCode;
							context.state.error = WideToUtf8(result.message);
							context.state.message = context.state.error;
							PersistState(context);
							return result;
						}
						unhealthy.rollbackBackupPath = FindNewestRollbackBackup(
							context.storageRoot, startupOptions.installRoot, unhealthy.selectedVersion);
						unhealthy.previousRuntimePath = unhealthy.rollbackBackupPath /
							L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
						if (unhealthy.rollbackBackupPath.empty() || !IsRegularFile(unhealthy.previousRuntimePath))
						{
							return IntegrityFailure(
								context, result, "interrupted_update_backup_missing",
								L"An interrupted update could not be matched to a complete rollback backup.");
						}
					}
					else if (healthPhase == "attempting")
					{
						bool attemptAlive = false;
						std::uint64_t liveCreation = 0;
						const bool attemptIdentityKnown = ProcessCreationStamp(
							attemptProcessId, liveCreation, attemptAlive);
						if (attemptIdentityKnown && attemptAlive && liveCreation == attemptProcessCreated)
						{
							unhealthy.status = StartupStatus::Updated;
							unhealthy.updateActivated = true;
							unhealthy.selectedRuntimePath = fs::absolute(startupOptions.canonicalRuntimePath).lexically_normal();
							unhealthy.availableVersion = unhealthy.selectedVersion;
							unhealthy.message = L"Another EuroScope process is validating this newly installed runtime.";
							return unhealthy;
						}
						if (!attemptIdentityKnown)
						{
							return IntegrityFailure(
								context, result, "health_attempt_owner_unknown",
								L"The updater could not safely determine whether another process is validating this runtime.");
						}
					}

					// Installing with a committed new tree, a dead/reused attempt owner,
					// or an explicit failed marker all require a complete data rollback.
					fs::path restored;
					std::string rollbackError;
					if (!RunRollback(
						context, unhealthy, restored, rollbackError,
						healthPhase == "installing" ? installingRecoveryLock.get() : nullptr,
						healthPhase == "installing" ? nullptr : "runtime_initialization_failed"))
					{
						return IntegrityFailure(
							context, result, rollbackError,
							L"The pending update could not be rolled back safely.");
					}
					result.status = StartupStatus::FailedOpen;
					result.selectedRuntimePath = restored;
					std::string restoredVersion;
					ReadReleaseVersion(startupOptions.dataRoot, restoredVersion);
					result.selectedVersion = restoredVersion;
					result.availableVersion = unhealthy.selectedVersion;
					result.errorCode = healthPhase == "installing"
						? "interrupted_update_rolled_back" : "unhealthy_update_rolled_back";
					result.message = healthPhase == "installing"
						? L"An interrupted update was rolled back before vSMR started."
						: L"A runtime that failed during initialization was rolled back.";
					context.state.status = "error";
					context.state.installedVersion = restoredVersion;
					context.state.availableVersion = unhealthy.selectedVersion;
					context.state.errorCode = result.errorCode;
					context.state.error = WideToUtf8(result.message);
					context.state.message = WideToUtf8(result.message);
					PersistState(context);
					return result;
				}
			}
			CleanupNormalUpdaterState(context);

			// Loading updater state and the next-startup action
			const State previousState = LoadPreviousState(context.statePath);
			context.state.lastCheckedUtc = previousState.lastCheckedUtc;
			context.state.nextCheckUtc = previousState.nextCheckUtc;
			context.state.lastActionRequestId = previousState.lastActionRequestId;
			const Action action = ConsumeAction(context.storageRoot);
			if (action.valid)
				context.state.lastActionRequestId = action.requestId;
			const bool forceAvisoReload = action.valid && action.action == "reload_aviso";
			const bool forceDiscovery = action.valid &&
				(action.action == "check_now" || action.action == "retry_update" || forceAvisoReload);
			const bool forceRetryInstall = action.valid && action.action == "retry_update";
			const bool forceExplicitInstall = forceRetryInstall || forceAvisoReload;
			if (forceRetryInstall && ParseSemVer(previousState.availableVersion).valid)
			{
				// An explicit retry is the only operation allowed to clear a runtime
				// quarantine. Discovery and all trust checks still run again below.
				::DeleteFileW((context.storageRoot / L"quarantine" /
					(Utf8ToWide(previousState.availableVersion) + L".json")).c_str());
			}
			const Config config = LoadConfig(context.storageRoot / L"config.json", startupOptions.defaultChannel);
			if ((!config.autoCheck && !forceDiscovery) ||
				(!forceDiscovery && IsFutureUtc(previousState.nextCheckUtc)))
			{
				context.state.status = "idle";
				context.state.message = !config.autoCheck
					? "Automatic update checks are disabled."
					: "The next update check is scheduled later.";
				context.state.error.clear();
				context.state.errorCode.clear();
				PersistState(context);
				return result;
			}

			// ----- Finding an eligible release -----
			context.state.status = "checking";
			context.state.error.clear();
			context.state.errorCode.clear();
			const wchar_t* checkingMessage = forceAvisoReload
				? L"Finding the installed vSMR release for AVISO reload..."
				: L"Checking for vSMR updates...";
			if (!Report(context, ProgressStage::Checking, -1, checkingMessage))
				return FailedOpen(context, result, "cancelled", L"Update check cancelled.", "idle");

			const SemVer installed = ParseSemVer(startupOptions.currentVersion);
			Manifest manifest;
			std::vector<std::uint8_t> manifestBytes;
			fs::path archivePath;
			std::string error;
			std::optional<Release> remoteRelease;
			std::optional<FixtureCandidate> fixture;
			if (!startupOptions.testFeedDirectory.empty())
			{
				fixture = SelectFixture(
					startupOptions, installed, config.channel,
					config.skippedVersion, context.storageRoot, forceAvisoReload, error);
				context.state.lastCheckedUtc = UtcNow();
				context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
				if (!fixture)
				{
					if (!error.empty())
						return FailedOpen(context, result, error, L"The local updater fixture could not be loaded.");
					if (forceAvisoReload)
					{
						return FailedOpen(
							context, result, "installed_release_not_found",
							L"The installed vSMR release is not available in the updater fixture; AVISOs were not changed.");
					}
					context.state.status = "up_to_date";
					context.state.message = "vSMR is up to date.";
					PersistState(context);
					return result;
				}
				manifest = fixture->manifest;
				manifestBytes = fixture->manifestBytes;
				archivePath = fixture->archivePath;
			}
			else
			{
				std::vector<Release> releases;
				if (!LoadRemoteReleases(context, releases, error))
				{
					context.state.lastCheckedUtc = UtcNow();
					return FailedOpen(
						context, result, error,
						error == "github_rate_limited"
							? L"GitHub rate-limited the updater; the installed runtime will be used."
							: L"The update check failed; the installed runtime will be used.",
						error == "github_rate_limited" ? "rate_limited" : "error");
				}
				context.state.lastCheckedUtc = UtcNow();
				context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
				remoteRelease = SelectRelease(
					releases, installed, config.channel,
					config.skippedVersion, context.storageRoot, forceAvisoReload);
				if (!remoteRelease)
				{
					if (forceAvisoReload)
					{
						return FailedOpen(
							context, result, "installed_release_not_found",
							L"The installed vSMR release is not available on GitHub; AVISOs were not changed.");
					}
					context.state.status = "up_to_date";
					context.state.message = "vSMR is up to date.";
					context.state.error.clear();
					context.state.errorCode.clear();
					PersistState(context);
					Report(context, ProgressStage::Complete, 100, L"vSMR is up to date.");
					return result;
				}
				result.availableVersion = remoteRelease->version.normalized;
				context.state.availableVersion = result.availableVersion;
				context.state.releaseUrl = remoteRelease->htmlUrl;
				const std::string trustedSigner = ResolveTrustedSignerHash(startupOptions);
				if (trustedSigner.empty())
				{
					return FailedOpen(
						context, result, "signature_required",
						L"A signed updater loader or pinned release certificate is required before automatic updates can be installed.");
				}
				if (!LoadAndVerifyRemoteManifest(
					context, *remoteRelease, trustedSigner,
					manifest, manifestBytes, error))
				{
					return FailedOpen(context, result, error, L"The release manifest could not be authenticated.");
				}
			}

			result.availableVersion = manifest.version.normalized;
			if (manifest.runtimeAbi != startupOptions.expectedRuntimeAbi)
			{
				return FailedOpen(
					context, result, "runtime_abi_incompatible",
					L"The available runtime uses an unsupported loader ABI.");
			}
			context.state.availableVersion = result.availableVersion;
			context.state.selectedVersion = result.availableVersion;
			if (!config.autoDownload && !forceExplicitInstall)
			{
				result.status = StartupStatus::UpdateAvailable;
				result.message = L"A vSMR update is available; automatic download is disabled.";
				context.state.status = "idle";
				context.state.message = WideToUtf8(result.message);
				PersistState(context);
				return result;
			}

			// ----- Downloading and staging the release -----
			context.state.status = "downloading";
			const wchar_t* downloadMessage = forceAvisoReload
				? L"Downloading the signed release for AVISO reload..."
				: L"Downloading vSMR update...";
			if (!Report(context, ProgressStage::Downloading, 0, downloadMessage))
				return FailedOpen(context, result, "cancelled", L"Update download cancelled.", "idle");
			if (fixture)
			{
				if (!VerifyArchive(archivePath, manifest, error))
					return FailedOpen(context, result, error, L"The local fixture archive failed verification.");
			}
			else if (!PrepareRemoteArchive(
				context, *remoteRelease, manifest, manifestBytes, archivePath, error))
			{
				return FailedOpen(
					context, result, error,
					error == "deadline" || error == "timeout"
						? L"The update download will resume on the next launch."
						: L"The update archive could not be downloaded or verified.",
					error == "deadline" || error == "timeout" ? "deferred" : "error");
			}

			context.state.status = "verifying";
			if (!Report(context, ProgressStage::Verifying, -1, L"Verifying vSMR update..."))
				return FailedOpen(context, result, "cancelled", L"Update verification cancelled.", "idle");
			const std::string attemptId = SecureRandomHex(16);
			if (attemptId.empty())
				return FailedOpen(context, result, "staging_nonce_failed", L"Secure staging could not be created.");
			const fs::path packageRoot = context.storageRoot / L"staging" /
				HashName(startupOptions.installRoot) /
				Utf8ToWide("attempt-" + manifest.version.normalized + "-" + attemptId) /
				L"package";
			if (!SafelyExtractArchive(context, archivePath, packageRoot, error) ||
				!ValidateExtractedPackage(context, packageRoot, manifest, error) ||
				!StageRecoveryScript(context, packageRoot, manifest, error))
			{
				return FailedOpen(context, result, error, L"The downloaded vSMR package failed validation.");
			}
			std::string expectedRuntimeHash;
			if (!Sha256File(
				packageRoot / L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll",
				expectedRuntimeHash))
			{
				return FailedOpen(context, result, "runtime_hash_unavailable", L"The packaged runtime could not be hashed.");
			}

			const SemVer currentLoaderVersion = ParseSemVer(startupOptions.loaderVersion);
			const bool loaderTooOld = !currentLoaderVersion.valid ||
				CompareSemVer(currentLoaderVersion, manifest.minimumLoaderVersion) < 0;
			if (loaderTooOld)
			{
				result.status = StartupStatus::Deferred;
				result.loaderUpdateDeferred = true;
				result.errorCode = "manual_loader_update_required";
				result.message = L"This release requires a newer vSMR loader. Install the full package manually.";
				context.state.status = "deferred";
				context.state.loaderUpdateDeferred = true;
				context.state.restartRequired = false;
				context.state.errorCode = result.errorCode;
				context.state.message = WideToUtf8(result.message);
				PersistState(context);
				return result;
			}

			if (!config.autoInstall && !forceExplicitInstall)
			{
				result.status = StartupStatus::UpdateAvailable;
				result.message = L"A verified vSMR update is ready for the next startup.";
				context.state.status = "idle";
				context.state.message = WideToUtf8(result.message);
				PersistState(context);
				return result;
			}
			if (RemainingMs(context, startupOptions.overallDeadlineMs) == 0)
			{
				return FailedOpen(
					context, result, "deadline",
					L"The startup update deadline expired before installation; the installed runtime will be used.",
					"deferred");
			}

			// ----- Installing the verified release -----
			UniqueHandle sessionLock = AcquireExclusiveSessionLock(
				context.sessionLockStorageRoot, startupOptions.installRoot);
			if (!sessionLock)
			{
				result.status = StartupStatus::Deferred;
				result.message = L"Another EuroScope session is using this vSMR installation; the update remains staged.";
				context.state.status = "deferred";
				context.state.message = WideToUtf8(result.message);
				context.state.restartRequired = true;
				PersistState(context);
				return result;
			}

			context.state.status = "installing";
			Report(
				context, ProgressStage::Installing, -1,
				forceAvisoReload ? L"Reloading AVISO data..." : L"Installing vSMR update...");
			result.selectedVersion = manifest.version.normalized;
			result.availableVersion = manifest.version.normalized;
			result.previousRuntimePath = startupOptions.canonicalRuntimePath;
			if (!ReadReleaseVersion(startupOptions.dataRoot, result.previousVersion))
				result.previousVersion = startupOptions.currentVersion;
			if (!Sha256File(startupOptions.canonicalRuntimePath, result.previousRuntimeSha256))
			{
				return FailedOpen(
					context, result, "current_runtime_hash_unavailable",
					L"The installed runtime could not be verified before updating.");
			}
			if (!WriteHealthMarker(startupOptions, context.storageRoot, result, "installing"))
			{
				return FailedOpen(
					context, result, "install_journal_unavailable",
					L"The updater could not create its durable installation journal.");
			}
			if (!RunInstaller(
				context, packageRoot, true, forceAvisoReload,
				!config.protectModifiedAviso, sessionLock.get(), error))
			{
				std::string activeVersion;
				std::string activeRuntimeHash;
				if (ReadReleaseVersion(startupOptions.dataRoot, activeVersion) &&
					SameSemVerIdentity(activeVersion, result.previousVersion) &&
					IsX86PortableExecutable(startupOptions.canonicalRuntimePath) &&
					Sha256File(startupOptions.canonicalRuntimePath, activeRuntimeHash) &&
					ToLowerAscii(activeRuntimeHash) == ToLowerAscii(result.previousRuntimeSha256))
				{
					::DeleteFileW(result.healthMarkerPath.c_str());
					return FailedOpen(context, result, error, L"The update installation failed and was rolled back.");
				}
				result.selectedVersion = manifest.version.normalized;
				result.rollbackBackupPath = FindNewestRollbackBackup(
					context.storageRoot, startupOptions.installRoot, manifest.version.normalized);
				fs::path restored;
				std::string rollbackError;
				if (!result.rollbackBackupPath.empty() &&
					RunRollback(context, result, restored, rollbackError, sessionLock.get(), nullptr))
				{
					result.selectedRuntimePath = restored;
					return FailedOpen(context, result, error, L"The interrupted update was rolled back.");
				}
				return IntegrityFailure(
					context, result, "installer_state_unknown",
					L"The update transaction did not complete and a safe runtime could not be verified.");
			}

			// Verifying the installed runtime before activation
			std::string installedVersion;
			fs::path rollbackBackup;
			std::string installedRuntimeHash;
			const bool installationMetadataValid =
				ReadInstallationMetadata(startupOptions.dataRoot, installedVersion, rollbackBackup);
			if (!installationMetadataValid)
				rollbackBackup = FindNewestRollbackBackup(
					context.storageRoot, startupOptions.installRoot, manifest.version.normalized);
			if (!installationMetadataValid ||
				!SameSemVerIdentity(installedVersion, manifest.version.normalized) ||
				!IsRegularFile(startupOptions.canonicalRuntimePath) ||
				!IsX86PortableExecutable(startupOptions.canonicalRuntimePath) ||
				!Sha256File(startupOptions.canonicalRuntimePath, installedRuntimeHash) ||
				ToLowerAscii(installedRuntimeHash) != ToLowerAscii(expectedRuntimeHash))
			{
				result.selectedVersion = manifest.version.normalized;
				result.rollbackBackupPath = rollbackBackup;
				fs::path restored;
				std::string rollbackError;
				if (!rollbackBackup.empty() && RunRollback(
					context, result, restored, rollbackError, sessionLock.get(),
					"post_install_verification_failed"))
				{
					result.selectedRuntimePath = restored;
					return FailedOpen(
						context, result, "post_install_verification_failed",
						L"The installed runtime failed verification; the previous version was restored.");
				}
				return IntegrityFailure(
					context, result, "post_install_rollback_failed",
					L"The installed runtime failed verification and the previous data could not be restored safely.");
			}

			result.status = StartupStatus::Updated;
			result.updateActivated = true;
			result.selectedRuntimePath = fs::absolute(startupOptions.canonicalRuntimePath).lexically_normal();
			result.selectedVersion = manifest.version.normalized;
			result.rollbackBackupPath = fs::absolute(rollbackBackup).lexically_normal();
			result.previousRuntimePath = result.rollbackBackupPath /
				L"vSMR_Data" / L"Runtime" / L"vSMR.Runtime.dll";
			result.message = forceAvisoReload
				? L"AVISO data was reloaded from the installed signed vSMR release."
				: L"vSMR was updated and will start with the new runtime.";
			if (!IsRegularFile(result.previousRuntimePath) ||
				!WriteHealthMarker(startupOptions, context.storageRoot, result))
			{
				fs::path restored;
				std::string rollbackError;
				if (RunRollback(context, result, restored, rollbackError, sessionLock.get(), nullptr))
				{
					result.selectedRuntimePath = restored;
					return FailedOpen(
						context, result, "rollback_safety_unavailable",
						L"The update could not establish its rollback marker; the previous version was restored.");
				}
				return IntegrityFailure(
					context, result, "rollback_safety_failure",
					L"The update could not establish or restore a safe runtime/data pair.");
			}

			context.state.status = "updated";
			context.state.installedVersion = result.selectedVersion;
			context.state.selectedVersion = result.selectedVersion;
			context.state.downloadPercent = 100;
			context.state.message = WideToUtf8(result.message);
			context.state.nextCheckUtc = UtcAfterSeconds(kMinimumCheckIntervalSeconds);
			PersistState(context);
			Report(context, ProgressStage::Complete, 100, result.message);
			return result;
		}

		void PruneDirectoryHistory(
			const fs::path& root,
			const fs::path& alwaysKeep,
			std::size_t additionalToKeep) noexcept
		{
			try
			{
				struct Candidate
				{
					fs::path path;
					fs::file_time_type modified;
				};
				std::error_code error;
				if (!fs::is_directory(root, error) || error)
					return;
				std::vector<Candidate> candidates;
				for (fs::directory_iterator iterator(root, error), end;
					!error && iterator != end; iterator.increment(error))
				{
					if (!iterator->is_directory(error) || error ||
						(!alwaysKeep.empty() && iterator->path() == alwaysKeep))
					{
						error.clear();
						continue;
					}
					const auto modified = fs::last_write_time(iterator->path(), error);
					if (!error)
						candidates.push_back({ iterator->path(), modified });
					else
						error.clear();
				}
				std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
					return left.modified > right.modified;
				});
				for (std::size_t index = additionalToKeep; index < candidates.size(); ++index)
				{
					if (IsPathBelow(candidates[index].path, root))
						fs::remove_all(candidates[index].path, error);
					error.clear();
				}
			}
			catch (...)
			{
			}
		}

		std::uint64_t DirectoryBytes(const fs::path& root) noexcept
		{
			std::uint64_t total = 0;
			try
			{
				std::error_code error;
				for (fs::recursive_directory_iterator iterator(
					root, fs::directory_options::skip_permission_denied, error), end;
					!error && iterator != end; iterator.increment(error))
				{
					if (iterator->is_regular_file(error))
					{
						const auto size = iterator->file_size(error);
						if (!error && total <= (std::numeric_limits<std::uint64_t>::max)() - size)
							total += size;
					}
					error.clear();
				}
			}
			catch (...)
			{
			}
			return total;
		}

		void PruneDirectoryBudget(
			const fs::path& root,
			const fs::path& alwaysKeep,
			std::uint64_t budgetBytes) noexcept
		{
			try
			{
				struct Candidate
				{
					fs::path path;
					fs::file_time_type modified;
					std::uint64_t bytes = 0;
				};
				std::error_code error;
				std::vector<Candidate> candidates;
				std::uint64_t total = 0;
				for (fs::directory_iterator iterator(root, error), end;
					!error && iterator != end; iterator.increment(error))
				{
					if (!iterator->is_directory(error) || error)
					{
						error.clear();
						continue;
					}
					Candidate candidate;
					candidate.path = iterator->path();
					candidate.modified = fs::last_write_time(candidate.path, error);
					if (error)
					{
						error.clear();
						continue;
					}
					candidate.bytes = DirectoryBytes(candidate.path);
					total += candidate.bytes;
					candidates.push_back(std::move(candidate));
				}
				std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
					return left.modified < right.modified;
				});
				for (const auto& candidate : candidates)
				{
					if (total <= budgetBytes)
						break;
					if (!alwaysKeep.empty() && candidate.path == alwaysKeep)
						continue;
					if (IsPathBelow(candidate.path, root))
					{
						fs::remove_all(candidate.path, error);
						if (!error)
							total = total >= candidate.bytes ? total - candidate.bytes : 0;
						error.clear();
					}
				}
			}
			catch (...)
			{
			}
		}

		void CleanupNormalUpdaterState(const Context& context) noexcept
		{
			const std::wstring installKey = HashName(context.options.installRoot);
			const fs::path staging = context.storageRoot / L"staging" / installKey;
			const fs::path recovery = context.storageRoot / L"recovery" / installKey;
			const fs::path backups = context.storageRoot / L"backups" / installKey;
			PruneDirectoryHistory(staging, {}, 2);
			PruneDirectoryHistory(recovery, {}, 3);
			PruneDirectoryHistory(backups, {}, 4);
			PruneDirectoryBudget(staging, {}, 768ULL * 1024ULL * 1024ULL);
			PruneDirectoryBudget(recovery, {}, 8ULL * 1024ULL * 1024ULL);
			PruneDirectoryBudget(backups, {}, 1536ULL * 1024ULL * 1024ULL);
		}

		void CleanupAfterHealthyRuntime(const StartupResult& update) noexcept
		{
			try
			{
				const fs::path storage = update.updaterStoragePath.empty()
					? GetUpdaterStorageDirectory() : update.updaterStoragePath;
				if (storage.empty() || update.rollbackBackupPath.empty() ||
					!IsPathBelow(update.rollbackBackupPath, storage / L"backups"))
				{
					return;
				}
				const fs::path backupInstallRoot = update.rollbackBackupPath.parent_path();
				const std::wstring installKey = backupInstallRoot.filename().wstring();
				PruneDirectoryHistory(backupInstallRoot, update.rollbackBackupPath, 2);
				PruneDirectoryHistory(storage / L"staging" / installKey, {}, 2);
				PruneDirectoryHistory(storage / L"recovery" / installKey, {}, 3);
				std::error_code error;
				const fs::path staging = storage / L"staging" / installKey;
				for (fs::recursive_directory_iterator iterator(
					staging, fs::directory_options::skip_permission_denied, error), end;
					!error && iterator != end; iterator.increment(error))
				{
					if (iterator->is_regular_file(error) && iterator->path().extension() == L".part")
						fs::remove(iterator->path(), error);
					error.clear();
				}
			}
			catch (...)
			{
			}
		}
	}

	fs::path GetUpdaterStorageDirectory() noexcept
	{
		try
		{
			// Durable config, state, staging, and health journals must have one
			// deterministic identity. Never switch an existing LocalAppData journal
			// to Temp merely because LocalAppData is temporarily read-only: doing so
			// could hide an unresolved transaction. Write failures are handled by the
			// caller; Temp is reserved for the stable cross-process session lease.
			return LocalAppDataUpdaterCandidate();
		}
		catch (...)
		{
		}
		return {};
	}

	fs::path GetInstallationSessionLockPath(const fs::path& installRoot) noexcept
	{
		try
		{
			const fs::path storage = GetProductionSessionLockStorageRoot();
			if (storage.empty())
				return {};
			return SessionLockPath(storage, installRoot);
		}
		catch (...)
		{
			return {};
		}
	}

	StartupResult PrepareUpdateBeforeRuntimeLoad(const StartupOptions& options) noexcept
	{
		StartupResult result;
		StartupOptions normalized;
		bool optionsNormalized = false;
		try
		{
			if (!NormalizeOptions(options, normalized))
			{
				result.status = StartupStatus::FailedOpen;
				result.selectedRuntimePath = options.canonicalRuntimePath;
				result.selectedVersion = options.currentVersion;
				result.errorCode = "invalid_startup_options";
				result.message = L"The updater received invalid installation paths or versions.";
				return result;
			}
			optionsNormalized = true;
			return PrepareUpdateImpl(normalized);
		}
		catch (const std::exception& exception)
		{
			result.status = StartupStatus::FailedOpen;
			bool transactionPending = false;
			try
			{
				if (optionsNormalized)
				{
					result.updaterStoragePath = normalized.testStorageDirectory.empty()
						? GetUpdaterStorageDirectory() : normalized.testStorageDirectory;
					transactionPending = !result.updaterStoragePath.empty() && IsRegularFile(
						HealthMarkerPath(result.updaterStoragePath, normalized.installRoot));
				}
			}
			catch (...)
			{
				transactionPending = optionsNormalized;
			}
			result.selectedRuntimePath = transactionPending
				? fs::path{} : options.canonicalRuntimePath;
			result.selectedVersion = options.currentVersion;
			result.errorCode = transactionPending
				? "unexpected_exception_with_pending_transaction" : "unexpected_exception";
			result.message = transactionPending
				? L"The updater failed while a package transaction was pending; no runtime will be loaded until recovery succeeds."
				: L"The updater failed unexpectedly: " + Utf8ToWide(exception.what());
			return result;
		}
		catch (...)
		{
			result.status = StartupStatus::FailedOpen;
			bool transactionPending = false;
			try
			{
				if (optionsNormalized)
				{
					result.updaterStoragePath = normalized.testStorageDirectory.empty()
						? GetUpdaterStorageDirectory() : normalized.testStorageDirectory;
					transactionPending = !result.updaterStoragePath.empty() && IsRegularFile(
						HealthMarkerPath(result.updaterStoragePath, normalized.installRoot));
				}
			}
			catch (...)
			{
				transactionPending = optionsNormalized;
			}
			result.selectedRuntimePath = transactionPending
				? fs::path{} : options.canonicalRuntimePath;
			result.selectedVersion = options.currentVersion;
			result.errorCode = transactionPending
				? "unexpected_exception_with_pending_transaction" : "unexpected_exception";
			result.message = transactionPending
				? L"The updater failed while a package transaction was pending; no runtime will be loaded until recovery succeeds."
				: L"The updater failed unexpectedly.";
			return result;
		}
	}

	bool RollbackPreparedUpdate(
		const StartupOptions& options,
		const StartupResult& update,
		fs::path* restoredRuntimePath,
		std::wstring* errorMessage) noexcept
	{
		try
		{
			if (!MarkRuntimeUnhealthy(update))
				throw std::runtime_error("runtime health marker could not be marked failed");
			StartupOptions normalized;
			if (!NormalizeOptions(options, normalized))
				throw std::runtime_error("invalid updater options");
			Context context(normalized);
			OwnedMutex updaterMutex = AcquireUpdaterMutex(normalized.installRoot);
			if (!updaterMutex)
				throw std::runtime_error("another updater is active");
			fs::path restored;
			std::string error;
			if (!RunRollback(
				context, update, restored, error, nullptr,
				"runtime_initialization_failed"))
				throw std::runtime_error(error);
			if (restoredRuntimePath != nullptr)
				*restoredRuntimePath = restored;
			std::string restoredVersion;
			ReadReleaseVersion(normalized.dataRoot, restoredVersion);
			context.state.status = "error";
			context.state.installedVersion = restoredVersion;
			context.state.availableVersion = update.selectedVersion;
			context.state.errorCode = "runtime_initialization_failed";
			context.state.error = "The new runtime failed initialization and was rolled back.";
			context.state.message = context.state.error;
			PersistState(context);
			return true;
		}
		catch (const std::exception& exception)
		{
			if (errorMessage != nullptr)
				*errorMessage = Utf8ToWide(exception.what());
			return false;
		}
		catch (...)
		{
			if (errorMessage != nullptr)
				*errorMessage = L"The update rollback failed unexpectedly.";
			return false;
		}
	}

	bool MarkRuntimeUnhealthy(const StartupResult& update) noexcept
	{
		try
		{
			if (!update.updateActivated || update.healthMarkerPath.empty())
				return false;
			OwnedMutex markerMutex = AcquireHealthMarkerMutex(update.healthMarkerPath);
			if (!markerMutex)
				return false;
			return RewriteHealthMarkerPhase(
				update, { "attempting", "failed" }, "failed");
		}
		catch (...)
		{
			return false;
		}
	}

	bool ConfirmRuntimeHealthy(const StartupResult& update) noexcept
	{
		try
		{
			if (!update.updateActivated || update.healthMarkerPath.empty())
				return true;
			const fs::path storage = update.updaterStoragePath.empty()
				? GetUpdaterStorageDirectory() : update.updaterStoragePath;
			if (storage.empty() || !IsPathBelow(update.healthMarkerPath, storage / L"health"))
				return false;
			OwnedMutex markerMutex = AcquireHealthMarkerMutex(update.healthMarkerPath);
			if (!markerMutex)
				return false;
			if (!IsRegularFile(update.healthMarkerPath))
				return true;
			if (!RewriteHealthMarkerPhase(
				update, { "attempting", "failed", "healthy" }, "healthy"))
			{
				return false;
			}
			// Cleanup touches staging shared by every installation. Only mutate it
			// while holding the same global mutex as Prepare. A durable `healthy`
			// marker lets cleanup be deferred without causing a future rollback.
			OwnedMutex updaterMutex = AcquireUpdaterMutex({});
			if (updaterMutex)
			{
				::DeleteFileW(update.healthMarkerPath.c_str());
				CleanupAfterHealthyRuntime(update);
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}
