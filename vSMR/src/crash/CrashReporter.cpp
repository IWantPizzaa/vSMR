#include "platform/windows/PrecompiledHeader.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashReportProtocol.hpp"
#include "crash/CrashReportSupport.hpp"

#include <bcrypt.h>
#include <werapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Wer.lib")

namespace
{
	using VsmrCrashProtocol::SharedState;

	constexpr wchar_t kWerRegistryPath[] =
		L"Software\\Microsoft\\Windows\\Windows Error Reporting\\RuntimeExceptionHelperModules";
	constexpr wchar_t kHandlerRelativePath[] =
		L"vSMR_Data\\CrashReporter\\vSMRCrashHandler.dll";
	alignas(64) SharedState gSharedState{};
	std::atomic<bool> gInstalled{ false };
	std::mutex gLifecycleMutex;
	std::wstring gHandlerPath;
	std::wstring gReportDirectoryDisplay;
	std::string gRegistrationStatus = "inactive";
	bool gWerRegistered = false;

	bool IsSharedStateReady() noexcept
	{
		return ::InterlockedCompareExchange(&gSharedState.ready, 0, 0) != 0;
	}

	std::wstring GetModulePath(HMODULE module)
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD length = ::GetModuleFileNameW(
				module,
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return {};
			if (length + 1U < buffer.size())
				return std::wstring(buffer.data(), length);
			if (buffer.size() >= 32768U)
				return {};
			buffer.resize(std::min<std::size_t>(32768U, buffer.size() * 2U));
		}
	}

	HMODULE LoadSystemLibrary(const wchar_t* filename)
	{
		std::array<wchar_t, MAX_PATH> systemDirectory{};
		const UINT length = ::GetSystemDirectoryW(
			systemDirectory.data(),
			static_cast<UINT>(systemDirectory.size()));
		if (length == 0 || length >= systemDirectory.size())
			return nullptr;
		std::wstring path(systemDirectory.data(), length);
		path.push_back(L'\\');
		path += filename;
		return ::LoadLibraryW(path.c_str());
	}

	std::string HashFileSha256(const std::wstring& path)
	{
		using OpenAlgorithmFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
		using GetPropertyFn = NTSTATUS(WINAPI*)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
		using CreateHashFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
		using HashDataFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
		using FinishHashFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
		using DestroyHashFn = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE);
		using CloseAlgorithmFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);

		HMODULE bcrypt = LoadSystemLibrary(L"bcrypt.dll");
		if (bcrypt == nullptr)
			return {};
		const auto openAlgorithm = reinterpret_cast<OpenAlgorithmFn>(::GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
		const auto getProperty = reinterpret_cast<GetPropertyFn>(::GetProcAddress(bcrypt, "BCryptGetProperty"));
		const auto createHash = reinterpret_cast<CreateHashFn>(::GetProcAddress(bcrypt, "BCryptCreateHash"));
		const auto hashData = reinterpret_cast<HashDataFn>(::GetProcAddress(bcrypt, "BCryptHashData"));
		const auto finishHash = reinterpret_cast<FinishHashFn>(::GetProcAddress(bcrypt, "BCryptFinishHash"));
		const auto destroyHash = reinterpret_cast<DestroyHashFn>(::GetProcAddress(bcrypt, "BCryptDestroyHash"));
		const auto closeAlgorithm = reinterpret_cast<CloseAlgorithmFn>(::GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));
		if (openAlgorithm == nullptr || getProperty == nullptr || createHash == nullptr ||
			hashData == nullptr || finishHash == nullptr || destroyHash == nullptr ||
			closeAlgorithm == nullptr)
		{
			::FreeLibrary(bcrypt);
			return {};
		}

		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hashHandle = nullptr;
		std::string result;
		if (openAlgorithm(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
		{
			ULONG objectLength = 0;
			ULONG hashLength = 0;
			ULONG copied = 0;
			if (getProperty(algorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) >= 0 &&
				getProperty(algorithm, BCRYPT_HASH_LENGTH,
					reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &copied, 0) >= 0 &&
				objectLength > 0 && hashLength == 32U)
			{
				std::vector<UCHAR> object(objectLength);
				std::array<UCHAR, 32> digest{};
				if (createHash(algorithm, &hashHandle, object.data(), objectLength,
					nullptr, 0, 0) >= 0)
				{
					const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
						FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
					if (file != INVALID_HANDLE_VALUE)
					{
						std::array<UCHAR, 64U * 1024U> buffer{};
						bool ok = true;
						for (;;)
						{
							DWORD bytesRead = 0;
							if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) == FALSE)
							{
								ok = false;
								break;
							}
							if (bytesRead == 0)
								break;
							if (hashData(hashHandle, buffer.data(), bytesRead, 0) < 0)
							{
								ok = false;
								break;
							}
						}
						::CloseHandle(file);
						if (ok && finishHash(hashHandle, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0)
						{
							static constexpr char digits[] = "0123456789abcdef";
							result.resize(digest.size() * 2U);
							for (std::size_t index = 0; index < digest.size(); ++index)
							{
								result[index * 2U] = digits[digest[index] >> 4U];
								result[index * 2U + 1U] = digits[digest[index] & 0x0FU];
							}
						}
					}
				}
			}
		}
		if (hashHandle != nullptr)
			destroyHash(hashHandle);
		if (algorithm != nullptr)
			closeAlgorithm(algorithm, 0);
		::FreeLibrary(bcrypt);
		return result;
	}

	std::string GetPdbIdentity(HMODULE module, std::uint32_t moduleSize)
	{
		if (module == nullptr || moduleSize < sizeof(IMAGE_DOS_HEADER))
			return {};
		const auto* base = reinterpret_cast<const BYTE*>(module);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0 ||
			static_cast<std::uint32_t>(dosHeader->e_lfanew) > moduleSize - sizeof(IMAGE_NT_HEADERS))
			return {};
		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
			ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
			return {};
		const IMAGE_DATA_DIRECTORY directory =
			ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
			directory.VirtualAddress > moduleSize || directory.Size > moduleSize - directory.VirtualAddress)
			return {};

		const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
		const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
		for (std::size_t index = 0; index < count; ++index)
		{
			const IMAGE_DEBUG_DIRECTORY& entry = entries[index];
			if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.AddressOfRawData == 0 ||
				entry.SizeOfData < 24U || entry.AddressOfRawData > moduleSize ||
				entry.SizeOfData > moduleSize - entry.AddressOfRawData)
				continue;
			const BYTE* codeView = base + entry.AddressOfRawData;
			if (std::memcmp(codeView, "RSDS", 4U) != 0)
				continue;
			GUID guid{};
			DWORD age = 0;
			std::memcpy(&guid, codeView + 4U, sizeof(guid));
			std::memcpy(&age, codeView + 4U + sizeof(guid), sizeof(age));
			std::array<char, 96> text{};
			_snprintf_s(text.data(), text.size(), _TRUNCATE,
				"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X age=%lu",
				static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
				guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
				guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7],
				static_cast<unsigned long>(age));
			return text.data();
		}
		return {};
	}

	std::string GetFileVersion(const std::wstring& path)
	{
		using GetSizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
		using GetInfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
		using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
		HMODULE versionModule = LoadSystemLibrary(L"version.dll");
		if (versionModule == nullptr)
			return {};
		const auto getSize = reinterpret_cast<GetSizeFn>(::GetProcAddress(versionModule, "GetFileVersionInfoSizeW"));
		const auto getInfo = reinterpret_cast<GetInfoFn>(::GetProcAddress(versionModule, "GetFileVersionInfoW"));
		const auto query = reinterpret_cast<QueryFn>(::GetProcAddress(versionModule, "VerQueryValueW"));
		std::string result;
		if (getSize != nullptr && getInfo != nullptr && query != nullptr)
		{
			DWORD ignored = 0;
			const DWORD size = getSize(path.c_str(), &ignored);
			if (size > 0 && size <= 4U * 1024U * 1024U)
			{
				std::vector<BYTE> data(size);
				if (getInfo(path.c_str(), 0, size, data.data()) != FALSE)
				{
					VS_FIXEDFILEINFO* fixed = nullptr;
					UINT fixedSize = 0;
					if (query(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedSize) != FALSE &&
						fixed != nullptr && fixedSize >= sizeof(*fixed) && fixed->dwSignature == 0xFEEF04BDUL)
					{
						std::array<char, 64> text{};
						_snprintf_s(text.data(), text.size(), _TRUNCATE, "%u.%u.%u.%u",
							HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
							HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
						result = text.data();
					}
				}
			}
		}
		::FreeLibrary(versionModule);
		return result;
	}

	std::string ReadSmallFile(const std::filesystem::path& path)
	{
		const std::wstring nativePath = VsmrCrashSupport::MakeNativePath(path);
		const HANDLE file = ::CreateFileW(nativePath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return {};
		LARGE_INTEGER size{};
		if (::GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0 || size.QuadPart > 256 * 1024)
		{
			::CloseHandle(file);
			return {};
		}
		std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
		DWORD read = 0;
		const bool ok = ::ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE;
		::CloseHandle(file);
		if (!ok)
			return {};
		contents.resize(read);
		return contents;
	}

	std::string JsonString(const std::string& json, const char* key)
	{
		const std::string marker = std::string("\"") + key + "\"";
		std::size_t position = json.find(marker);
		if (position == std::string::npos)
			return {};
		position = json.find(':', position + marker.size());
		if (position == std::string::npos)
			return {};
		position = json.find('"', position + 1U);
		if (position == std::string::npos)
			return {};
		const std::size_t end = json.find('"', position + 1U);
		if (end == std::string::npos)
			return {};
		return json.substr(position + 1U, end - position - 1U);
	}

	bool JsonBool(const std::string& json, const char* key, bool& value)
	{
		const std::string marker = std::string("\"") + key + "\"";
		std::size_t position = json.find(marker);
		if (position == std::string::npos)
			return false;
		position = json.find(':', position + marker.size());
		if (position == std::string::npos)
			return false;
		const std::size_t start = json.find_first_not_of(" \t\r\n", position + 1U);
		if (start == std::string::npos)
			return false;
		if (json.compare(start, 4U, "true") == 0)
		{
			value = true;
			return true;
		}
		if (json.compare(start, 5U, "false") == 0)
		{
			value = false;
			return true;
		}
		return false;
	}

	void ReadBuildMetadata(const std::filesystem::path& moduleDirectory, SharedState& state)
	{
		const std::string json = ReadSmallFile(moduleDirectory / L"vSMR_Data" / L"RELEASE-METADATA.json");
		if (json.empty())
		{
			VsmrCrashProtocol::CopyText(state.sourceState, "metadata-unavailable");
			return;
		}
		const std::string commit = JsonString(json, "git_commit");
		const std::string builtUtc = JsonString(json, "built_utc");
		VsmrCrashProtocol::CopyText(state.gitCommit, commit.c_str());
		VsmrCrashProtocol::CopyText(state.buildTimestampUtc, builtUtc.c_str());
		bool dirty = false;
		bool publishable = false;
		const bool hasDirty = JsonBool(json, "source_dirty", dirty);
		const bool hasPublishable = JsonBool(json, "publishable", publishable);
		if (hasDirty && dirty)
			VsmrCrashProtocol::CopyText(state.sourceState, "dirty");
		else if (hasPublishable && publishable)
			VsmrCrashProtocol::CopyText(state.sourceState, "clean-publishable");
		else if (hasDirty)
			VsmrCrashProtocol::CopyText(state.sourceState, "clean-local");
		else
			VsmrCrashProtocol::CopyText(state.sourceState, "metadata-partial");
	}

	bool GetImageSize(HMODULE module, std::uint32_t& size)
	{
		if (module == nullptr)
			return false;
		const auto* base = reinterpret_cast<const BYTE*>(module);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
			return false;
		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->OptionalHeader.SizeOfImage == 0)
			return false;
		size = ntHeaders->OptionalHeader.SizeOfImage;
		return true;
	}

	LONG AddWerAllowlistValue(const std::wstring& handlerPath, REGSAM view)
	{
		HKEY key = nullptr;
		DWORD disposition = 0;
		const LONG openResult = ::RegCreateKeyExW(HKEY_CURRENT_USER, kWerRegistryPath, 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | view, nullptr, &key, &disposition);
		if (openResult != ERROR_SUCCESS)
			return openResult;
		const DWORD enabled = 0;
		const LONG setResult = ::RegSetValueExW(key, handlerPath.c_str(), 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
		::RegCloseKey(key);
		return setResult;
	}

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return {};
		const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (required <= 0)
			return {};
		std::string converted(static_cast<std::size_t>(required), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			value.data(), static_cast<int>(value.size()), converted.data(), required,
			nullptr, nullptr) <= 0)
			return {};
		return converted;
	}
}

namespace VsmrCrashReporter
{
	bool Install(const char* version, const wchar_t* installRoot)
	{
		std::lock_guard<std::mutex> guard(gLifecycleMutex);
		if (gInstalled.load(std::memory_order_acquire))
			return true;

		try
		{
			VsmrCrashProtocol::InitializeSharedState(gSharedState);
			const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
			std::uint32_t moduleSize = 0;
			const std::wstring modulePath = GetModulePath(module);
			if (modulePath.empty() || !GetImageSize(module, moduleSize))
			{
				gRegistrationStatus = "inactive: vSMR image information unavailable";
				return false;
			}

			// Selecting a durable report directory before registering WER
			const std::filesystem::path moduleDirectory =
				installRoot != nullptr && *installRoot != L'\0'
				? std::filesystem::path(installRoot)
				: std::filesystem::path(modulePath).parent_path();
			const std::filesystem::path selectedDirectory =
				VsmrCrashSupport::SelectReportDirectory(moduleDirectory);
			if (selectedDirectory.empty())
			{
				gRegistrationStatus = "inactive: no writable crash-report directory";
				return false;
			}
			VsmrCrashSupport::ApplyRetention(selectedDirectory);

			const std::wstring reportDirectoryNative =
				VsmrCrashSupport::MakeNativePath(selectedDirectory);
			gReportDirectoryDisplay = VsmrCrashSupport::DisplayPath(reportDirectoryNative);
			gHandlerPath = VsmrCrashSupport::MakeNativePath(moduleDirectory / kHandlerRelativePath);
			const DWORD handlerAttributes = ::GetFileAttributesW(gHandlerPath.c_str());
			if (handlerAttributes == INVALID_FILE_ATTRIBUTES || (handlerAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				gRegistrationStatus = "inactive: vSMRCrashHandler.dll is missing";
				return false;
			}

			// Capturing enough build identity to match reports with private symbols
			gSharedState.moduleBase = reinterpret_cast<std::uintptr_t>(module);
			gSharedState.moduleSize = moduleSize;
			VsmrCrashProtocol::CopyText(gSharedState.reportDirectory, reportDirectoryNative.c_str());
			VsmrCrashProtocol::CopyText(gSharedState.modulePath, modulePath.c_str());
			VsmrCrashProtocol::CopyText(gSharedState.pluginVersion, version != nullptr ? version : "unknown");
			const std::string dllHash = HashFileSha256(VsmrCrashSupport::MakeNativePath(modulePath));
			VsmrCrashProtocol::CopyText(gSharedState.dllSha256, dllHash.empty() ? "unavailable" : dllHash.c_str());
			const std::string pdbIdentity = GetPdbIdentity(module, moduleSize);
			VsmrCrashProtocol::CopyText(gSharedState.pdbIdentity, pdbIdentity.empty() ? "unavailable" : pdbIdentity.c_str());
			const std::wstring hostPath = GetModulePath(nullptr);
			const std::string hostVersion = hostPath.empty()
				? std::string{}
				: GetFileVersion(VsmrCrashSupport::MakeNativePath(hostPath));
			VsmrCrashProtocol::CopyText(gSharedState.euroScopeVersion, hostVersion.empty() ? "unavailable" : hostVersion.c_str());
			ReadBuildMetadata(moduleDirectory, gSharedState);

			// EuroScope is x86 but Windows can consult either per-user registry view
			const LONG registry32 = AddWerAllowlistValue(gHandlerPath, KEY_WOW64_32KEY);
			const LONG registry64 = AddWerAllowlistValue(gHandlerPath, KEY_WOW64_64KEY);
			if (registry32 != ERROR_SUCCESS && registry64 != ERROR_SUCCESS)
			{
				gRegistrationStatus = "inactive: WER allowlist failed (x86=" +
					std::to_string(registry32) + ", x64=" + std::to_string(registry64) + ")";
				return false;
			}

			// The helper must never observe a partially initialized shared state
			VsmrCrashProtocol::MarkReady(gSharedState);
			const HRESULT registration = ::WerRegisterRuntimeExceptionModule(gHandlerPath.c_str(), &gSharedState);
			if (FAILED(registration))
			{
				::InterlockedExchange(&gSharedState.ready, 0);
				std::array<char, 160> message{};
				_snprintf_s(message.data(), message.size(), _TRUNCATE,
					"inactive: WerRegisterRuntimeExceptionModule failed (0x%08lX)",
					static_cast<unsigned long>(registration));
				gRegistrationStatus = message.data();
				return false;
			}

			gWerRegistered = true;
			gInstalled.store(true, std::memory_order_release);
			gRegistrationStatus = "active: WER out-of-process handler (registry x86=" +
				std::string(registry32 == ERROR_SUCCESS ? "ok" : std::to_string(registry32)) +
				", x64=" + std::string(registry64 == ERROR_SUCCESS ? "ok" : std::to_string(registry64)) + ")";
			VsmrCrashProtocol::PublishBreadcrumb(gSharedState, "lifecycle", "crash reporter registered");
			return true;
		}
		catch (...)
		{
			if (gWerRegistered)
			{
				::WerUnregisterRuntimeExceptionModule(gHandlerPath.c_str(), &gSharedState);
				gWerRegistered = false;
			}
			gInstalled.store(false, std::memory_order_release);
			::InterlockedExchange(&gSharedState.ready, 0);
			gRegistrationStatus = "inactive: exception during crash-reporter setup";
			return false;
		}
	}

	void Remove()
	{
		std::lock_guard<std::mutex> guard(gLifecycleMutex);
		::InterlockedExchange(&gSharedState.ready, 0);
		HRESULT unregisterResult = S_OK;
		if (gWerRegistered)
			unregisterResult = ::WerUnregisterRuntimeExceptionModule(gHandlerPath.c_str(), &gSharedState);
		gWerRegistered = false;
		gInstalled.store(false, std::memory_order_release);
		if (FAILED(unregisterResult))
		{
			std::array<char, 128> message{};
			_snprintf_s(message.data(), message.size(), _TRUNCATE,
				"inactive: WER unregister failed (0x%08lX)", static_cast<unsigned long>(unregisterResult));
			gRegistrationStatus = message.data();
		}
		else
		{
			gRegistrationStatus = "inactive: unregistered";
		}
	}

	bool IsInstalled()
	{
		return gInstalled.load(std::memory_order_acquire);
	}

	std::string GetReportDirectory()
	{
		std::lock_guard<std::mutex> guard(gLifecycleMutex);
		return WideToUtf8(gReportDirectoryDisplay);
	}

	std::string GetRegistrationStatus()
	{
		std::lock_guard<std::mutex> guard(gLifecycleMutex);
		return gRegistrationStatus;
	}

	void RecordState(const char* key, const char* value) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishState(gSharedState, key, value);
	}

	void RecordBreadcrumb(const char* category, const char* value) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishBreadcrumb(gSharedState, category, value);
	}

	void RecordThreadRole(const char* role) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishThreadRole(gSharedState, role);
	}

	void RecordLog(const char* message) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishLog(gSharedState, message);
	}

	void RecordRadarState(std::uintptr_t screenToken, const char* airport,
		const char* profile, const char* radar, const char* inset) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishRadarState(gSharedState, screenToken, airport, profile, radar, inset);
	}

	void ClearRadarState(std::uintptr_t screenToken) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::ClearRadarState(gSharedState, screenToken);
	}

	void RecordCallback(const char* callback, std::uintptr_t screenToken) noexcept
	{
		if (IsSharedStateReady())
			VsmrCrashProtocol::PublishCallback(gSharedState, callback, screenToken);
	}
}
