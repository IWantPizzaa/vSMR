#include <Windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <ShlObj.h>
#include <winver.h>

#include "bootstrap/RuntimeApi.hpp"
#include "bootstrap/loader/LoaderVersion.hpp"
#include "updater/UpdaterCore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	constexpr wchar_t kRuntimeRelativePath[] =
		L"vSMR_Data\\Runtime\\vSMR.Runtime.dll";
	constexpr wchar_t kProgressWindowClass[] = L"vSMRBootstrapProgressWindow";
	constexpr wchar_t kRuntimeShadowMutexName[] =
		L"Local\\vSMR.RuntimeShadowCache.7AB341C5-3BCB-4591-AB16-D9A673E77438";
	constexpr DWORD kRuntimeShadowMutexWaitMs = 10000U;
	constexpr int kProgressMessageId = 1001;
	constexpr int kProgressBarId = 1002;
	constexpr int kCancelButtonId = 1003;

	std::mutex gLifecycleMutex;
	HMODULE gRuntimeModule = nullptr;
	VsmrRuntimeApi::ShutdownFunction gRuntimeShutdown = nullptr;
	EuroScopePlugIn::CPlugIn* gPluginInstance = nullptr;
	HANDLE gSessionLock = INVALID_HANDLE_VALUE;
	bool gShutdownRequested = false;
	std::atomic<unsigned long> gShadowGeneration{ 0UL };

	std::wstring WindowsErrorMessage(DWORD error)
	{
		wchar_t* buffer = nullptr;
		const DWORD length = ::FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			error,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<wchar_t*>(&buffer),
			0,
			nullptr);
		std::wstring message = length > 0 && buffer != nullptr
			? std::wstring(buffer, length)
			: L"Windows error " + std::to_wstring(error);
		if (buffer != nullptr)
			::LocalFree(buffer);
		while (!message.empty() &&
			(message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
		{
			message.pop_back();
		}
		return message;
	}

	class NamedMutexGuard final
	{
	public:
		NamedMutexGuard() = default;
		NamedMutexGuard(const NamedMutexGuard&) = delete;
		NamedMutexGuard& operator=(const NamedMutexGuard&) = delete;

		~NamedMutexGuard() noexcept
		{
			Release();
		}

		bool Acquire(
			const wchar_t* name,
			DWORD timeoutMs,
			std::wstring& errorMessage)
		{
			Release();
			handle_ = ::CreateMutexW(nullptr, FALSE, name);
			if (handle_ == nullptr)
			{
				errorMessage = L"Could not open the runtime-shadow mutex: " +
					WindowsErrorMessage(::GetLastError());
				return false;
			}

			const DWORD waitResult = ::WaitForSingleObject(handle_, timeoutMs);
			if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
			{
				ownsMutex_ = true;
				return true;
			}

			if (waitResult == WAIT_TIMEOUT)
			{
				errorMessage = L"Timed out waiting for another EuroScope process to "
					L"finish loading its vSMR runtime.";
			}
			else
			{
				const DWORD waitError = waitResult == WAIT_FAILED
					? ::GetLastError()
					: ERROR_GEN_FAILURE;
				errorMessage = L"Could not lock the runtime-shadow cache: " +
					WindowsErrorMessage(waitError);
			}
			Release();
			return false;
		}

		void Release() noexcept
		{
			if (handle_ == nullptr)
				return;
			if (ownsMutex_)
				::ReleaseMutex(handle_);
			::CloseHandle(handle_);
			handle_ = nullptr;
			ownsMutex_ = false;
		}

	private:
		HANDLE handle_ = nullptr;
		bool ownsMutex_ = false;
	};

	class FileHandleGuard final
	{
	public:
		FileHandleGuard() noexcept = default;
		explicit FileHandleGuard(HANDLE handle) noexcept : handle_(handle) {}
		FileHandleGuard(const FileHandleGuard&) = delete;
		FileHandleGuard& operator=(const FileHandleGuard&) = delete;
		FileHandleGuard(FileHandleGuard&& other) noexcept : handle_(other.Release()) {}
		FileHandleGuard& operator=(FileHandleGuard&& other) noexcept
		{
			if (this != &other)
				Reset(other.Release());
			return *this;
		}

		~FileHandleGuard() noexcept
		{
			Reset();
		}

		explicit operator bool() const noexcept
		{
			return handle_ != INVALID_HANDLE_VALUE;
		}

		HANDLE Get() const noexcept
		{
			return handle_;
		}

		void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
		{
			if (handle_ != INVALID_HANDLE_VALUE)
				::CloseHandle(handle_);
			handle_ = handle;
		}

		HANDLE Release() noexcept
		{
			const HANDLE released = handle_;
			handle_ = INVALID_HANDLE_VALUE;
			return released;
		}

	private:
		HANDLE handle_ = INVALID_HANDLE_VALUE;
	};

	bool ResolveHandlePath(
		HANDLE handle,
		std::filesystem::path& resolvedPath,
		std::wstring& errorMessage)
	{
		std::vector<wchar_t> buffer(512U, L'\0');
		for (;;)
		{
			const DWORD length = ::GetFinalPathNameByHandleW(
				handle,
				buffer.data(),
				static_cast<DWORD>(buffer.size()),
				FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (length == 0)
			{
				errorMessage = L"Could not resolve the finalized runtime shadow: " +
					WindowsErrorMessage(::GetLastError());
				return false;
			}
			if (length < buffer.size())
			{
				resolvedPath = std::filesystem::path(
					std::wstring(buffer.data(), static_cast<std::size_t>(length)));
				return true;
			}
			if (length >= 32768U)
			{
				errorMessage = L"The resolved runtime shadow path is too long.";
				return false;
			}
			buffer.assign(static_cast<std::size_t>(length) + 1U, L'\0');
		}
	}

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty())
			return {};
		const int required = ::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0);
		if (required <= 0)
			return std::wstring(value.begin(), value.end());
		std::wstring converted(static_cast<std::size_t>(required), L'\0');
		::MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			converted.data(),
			required);
		return converted;
	}

	std::filesystem::path ModulePath()
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD length = ::GetModuleFileNameW(
				reinterpret_cast<HMODULE>(&__ImageBase),
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return {};
			if (length + 1U < buffer.size())
				return std::filesystem::path(std::wstring(buffer.data(), length));
			if (buffer.size() >= 32768U)
				return {};
			buffer.resize(std::min<std::size_t>(32768U, buffer.size() * 2U));
		}
	}

	void AppendBootstrapLog(const std::wstring& message) noexcept
	{
		try
		{
			const std::filesystem::path directory =
				vsmr::updater::GetUpdaterStorageDirectory();
			if (directory.empty())
				return;
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error)
				return;

			SYSTEMTIME time{};
			::GetLocalTime(&time);
			std::wofstream stream(
				directory / L"bootstrap.log",
				std::ios::out | std::ios::app);
			if (!stream)
				return;
			stream << L'['
				<< time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
				<< time.wHour << L':' << time.wMinute << L':' << time.wSecond
				<< L"] " << message << L'\n';
		}
		catch (...)
		{
		}
	}

	std::string ReadRuntimeVersion(const std::filesystem::path& runtimePath)
	{
		DWORD ignored = 0;
		const DWORD size = ::GetFileVersionInfoSizeW(runtimePath.c_str(), &ignored);
		if (size == 0)
			return "0.0.0";

		std::vector<BYTE> information(size);
		if (!::GetFileVersionInfoW(
			runtimePath.c_str(),
			0,
			size,
			information.data()))
		{
			return "0.0.0";
		}

		struct Translation
		{
			WORD language;
			WORD codePage;
		};
		Translation* translations = nullptr;
		UINT translationBytes = 0;
		if (!::VerQueryValueW(
			information.data(),
			L"\\VarFileInfo\\Translation",
			reinterpret_cast<void**>(&translations),
			&translationBytes) ||
			translations == nullptr)
		{
			return "0.0.0";
		}

		const std::size_t translationCount = translationBytes / sizeof(Translation);
		for (std::size_t index = 0; index < translationCount; ++index)
		{
			wchar_t query[96]{};
			swprintf_s(
				query,
				_countof(query),
				L"\\StringFileInfo\\%04x%04x\\ProductVersion",
				translations[index].language,
				translations[index].codePage);
			wchar_t* productVersion = nullptr;
			UINT productVersionCharacters = 0;
			if (!::VerQueryValueW(
				information.data(),
				query,
				reinterpret_cast<void**>(&productVersion),
				&productVersionCharacters) ||
				productVersion == nullptr || productVersionCharacters <= 1U)
			{
				continue;
			}
			const std::wstring wideVersion(productVersion);
			if (!std::all_of(wideVersion.begin(), wideVersion.end(), [](wchar_t character)
				{
					return character <= 0x7f;
				}))
			{
				continue;
			}
			std::string version;
			version.reserve(wideVersion.size());
			for (const wchar_t character : wideVersion)
				version.push_back(static_cast<char>(character));
			if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
				version.erase(version.begin());
			return version;
		}
		return "0.0.0";
	}

	class ProgressWindow
	{
	public:
		explicit ProgressWindow(HINSTANCE instance) noexcept
			: instance_(instance)
		{
		}

		~ProgressWindow()
		{
			Close();
		}

		bool Update(const vsmr::updater::Progress& progress) noexcept
		{
			try
			{
				if (progress.stage == vsmr::updater::ProgressStage::Checking ||
					progress.stage == vsmr::updater::ProgressStage::Downloading ||
					progress.stage == vsmr::updater::ProgressStage::Verifying ||
					progress.stage == vsmr::updater::ProgressStage::Installing)
				{
					EnsureCreated();
				}
				if (window_ != nullptr)
				{
					if (!progress.message.empty())
						::SetWindowTextW(message_, progress.message.c_str());
					if (progress.percent >= 0)
					{
						::SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
						::SendMessageW(progress_, PBM_SETPOS,
							static_cast<WPARAM>(std::clamp(progress.percent, 0, 100)), 0);
					}
					else
					{
						::SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 30);
					}
					cancellable_ = progress.stage != vsmr::updater::ProgressStage::Installing;
					::EnableWindow(cancel_, cancellable_ ? TRUE : FALSE);
					::SetWindowTextW(
						cancel_,
						cancellable_ ? L"Start current version" : L"Finishing safely...");
					PumpMessages();
					::UpdateWindow(window_);
				}
			}
			catch (...)
			{
				// Progress UI failure must never affect plug-in startup.
			}
			return !cancelled_;
		}

		void Close() noexcept
		{
			if (window_ != nullptr)
			{
				::DestroyWindow(window_);
				window_ = nullptr;
			}
		}

	private:
		static LRESULT CALLBACK WindowProcedure(
			HWND window,
			UINT message,
			WPARAM wordParameter,
			LPARAM longParameter)
		{
			ProgressWindow* self = reinterpret_cast<ProgressWindow*>(
				::GetWindowLongPtrW(window, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				const auto* create = reinterpret_cast<const CREATESTRUCTW*>(longParameter);
				self = static_cast<ProgressWindow*>(create->lpCreateParams);
				::SetWindowLongPtrW(
					window,
					GWLP_USERDATA,
					reinterpret_cast<LONG_PTR>(self));
			}
			if (self != nullptr)
			{
				if (message == WM_COMMAND && LOWORD(wordParameter) == kCancelButtonId)
				{
					if (self->cancellable_)
						self->cancelled_ = true;
					return 0;
				}
				if (message == WM_CLOSE)
				{
					if (self->cancellable_)
						self->cancelled_ = true;
					return 0;
				}
			}
			return ::DefWindowProcW(window, message, wordParameter, longParameter);
		}

		void EnsureCreated()
		{
			if (window_ != nullptr)
				return;

			INITCOMMONCONTROLSEX controls{
				sizeof(controls),
				ICC_PROGRESS_CLASS
			};
			::InitCommonControlsEx(&controls);

			WNDCLASSW windowClass{};
			windowClass.lpfnWndProc = &ProgressWindow::WindowProcedure;
			windowClass.hInstance = instance_;
			windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
			windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			windowClass.lpszClassName = kProgressWindowClass;
			::RegisterClassW(&windowClass);

			const int width = 470;
			const int height = 156;
			const int left = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
			const int top = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;
			window_ = ::CreateWindowExW(
				WS_EX_TOOLWINDOW,
				kProgressWindowClass,
				L"vSMR update",
				WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
				left,
				top,
				width,
				height,
				nullptr,
				nullptr,
				instance_,
				this);
			if (window_ == nullptr)
				return;

			message_ = ::CreateWindowExW(
				0, L"STATIC", L"Preparing vSMR update...",
				WS_CHILD | WS_VISIBLE,
				18, 16, 420, 22,
				window_, reinterpret_cast<HMENU>(kProgressMessageId), instance_, nullptr);
			progress_ = ::CreateWindowExW(
				0, PROGRESS_CLASSW, nullptr,
				WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE,
				18, 43, 420, 18,
				window_, reinterpret_cast<HMENU>(kProgressBarId), instance_, nullptr);
			cancel_ = ::CreateWindowExW(
				0, L"BUTTON", L"Start current version",
				WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				282, 72, 156, 27,
				window_, reinterpret_cast<HMENU>(kCancelButtonId), instance_, nullptr);
			::SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
			::ShowWindow(window_, SW_SHOWNORMAL);
			::UpdateWindow(window_);
		}

		void PumpMessages() noexcept
		{
			MSG message{};
			while (window_ != nullptr &&
				::PeekMessageW(&message, window_, 0, 0, PM_REMOVE))
			{
				::TranslateMessage(&message);
				::DispatchMessageW(&message);
			}
		}

		HINSTANCE instance_ = nullptr;
		HWND window_ = nullptr;
		HWND message_ = nullptr;
		HWND progress_ = nullptr;
		HWND cancel_ = nullptr;
		bool cancelled_ = false;
		bool cancellable_ = true;
	};

	bool HashFileSha256(
		const std::filesystem::path& path,
		std::array<BYTE, 32U>& digest,
		std::wstring& errorMessage)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		HANDLE file = INVALID_HANDLE_VALUE;
		std::vector<BYTE> hashObject;
		bool success = false;

		do
		{
			if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
				&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
			{
				errorMessage = L"Windows could not initialize SHA-256.";
				break;
			}
			DWORD objectLength = 0;
			DWORD resultLength = 0;
			if (!BCRYPT_SUCCESS(::BCryptGetProperty(
				algorithm,
				BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength),
				&resultLength,
				0)) || objectLength == 0)
			{
				errorMessage = L"Windows could not prepare SHA-256.";
				break;
			}
			hashObject.resize(objectLength);
			if (!BCRYPT_SUCCESS(::BCryptCreateHash(
				algorithm,
				&hash,
				hashObject.data(),
				static_cast<ULONG>(hashObject.size()),
				nullptr,
				0,
				0)))
			{
				errorMessage = L"Windows could not create the SHA-256 state.";
				break;
			}

			file = ::CreateFileW(
				path.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				errorMessage = L"Could not read " + path.wstring() + L": " +
					WindowsErrorMessage(::GetLastError());
				break;
			}

			std::array<BYTE, 64U * 1024U> buffer{};
			for (;;)
			{
				DWORD bytesRead = 0;
				if (!::ReadFile(
					file,
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					&bytesRead,
					nullptr))
				{
					errorMessage = L"Could not hash " + path.wstring() + L": " +
						WindowsErrorMessage(::GetLastError());
					break;
				}
				if (bytesRead == 0)
				{
					if (!BCRYPT_SUCCESS(::BCryptFinishHash(
						hash,
						digest.data(),
						static_cast<ULONG>(digest.size()),
						0)))
					{
						errorMessage = L"Windows could not finish SHA-256.";
						break;
					}
					success = true;
					break;
				}
				if (!BCRYPT_SUCCESS(::BCryptHashData(hash, buffer.data(), bytesRead, 0)))
				{
					errorMessage = L"Windows could not update SHA-256.";
					break;
				}
			}
		} while (false);

		if (file != INVALID_HANDLE_VALUE)
			::CloseHandle(file);
		if (hash != nullptr)
			::BCryptDestroyHash(hash);
		if (algorithm != nullptr)
			::BCryptCloseAlgorithmProvider(algorithm, 0);
		return success;
	}

	std::wstring SafeVersionDirectory(const std::string& version)
	{
		std::wstring safe;
		for (const unsigned char character : version)
		{
			if (std::isalnum(character) || character == '.' || character == '-')
				safe.push_back(static_cast<wchar_t>(character));
		}
		return safe.empty() ? L"current" : safe;
	}

	std::vector<std::filesystem::path> RuntimeShadowDirectories(
		const std::string& version)
	{
		std::vector<std::filesystem::path> candidates;
		const std::wstring versionDirectory = SafeVersionDirectory(version);
		auto appendCandidate = [&](const std::filesystem::path& root)
			{
				if (root.empty())
					return;
				const std::filesystem::path candidate =
					(root / L"vSMR" / L"Runtime" / versionDirectory).lexically_normal();
				if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
					candidates.push_back(candidate);
			};

		std::array<wchar_t, MAX_PATH> localAppData{};
		if (SUCCEEDED(::SHGetFolderPathW(
			nullptr,
			CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
			nullptr,
			SHGFP_TYPE_CURRENT,
			localAppData.data())))
		{
			appendCandidate(std::filesystem::path(localAppData.data()));
		}
		std::array<wchar_t, 32768U> localAppDataEnvironment{};
		const DWORD environmentLength = ::GetEnvironmentVariableW(
			L"LOCALAPPDATA",
			localAppDataEnvironment.data(),
			static_cast<DWORD>(localAppDataEnvironment.size()));
		if (environmentLength > 0 && environmentLength < localAppDataEnvironment.size())
		{
			appendCandidate(std::filesystem::path(std::wstring(
				localAppDataEnvironment.data(), environmentLength)));
		}

		std::vector<wchar_t> temporary(32768U, L'\0');
		const DWORD length = ::GetTempPathW(
			static_cast<DWORD>(temporary.size()), temporary.data());
		if (length > 0 && length < temporary.size())
		{
			appendCandidate(std::filesystem::path(
				std::wstring(temporary.data(), length)));
		}
		return candidates;
	}

	void PruneRuntimeCache(
		const std::filesystem::path& runtimeRoot,
		const std::filesystem::path& preservedShadow) noexcept
	{
		try
		{
			struct Candidate
			{
				std::filesystem::path path;
				std::filesystem::file_time_type modified;
			};
			std::vector<Candidate> candidates;
			std::error_code error;
			if (!std::filesystem::is_directory(runtimeRoot, error) || error)
				return;

			for (std::filesystem::recursive_directory_iterator iterator(
				runtimeRoot,
				std::filesystem::directory_options::skip_permission_denied,
				error), end;
				iterator != end;
				iterator.increment(error))
			{
				if (error)
				{
					error.clear();
					continue;
				}
				if (!iterator->is_regular_file(error) || error)
				{
					error.clear();
					continue;
				}
				const std::filesystem::path& path = iterator->path();
				const std::wstring filename = path.filename().wstring();
				if (filename.rfind(L"vSMR.Runtime.", 0) != 0)
					continue;
				if (path.extension() == L".tmp")
				{
					std::filesystem::remove(path, error);
					error.clear();
					continue;
				}
				if (path.extension() != L".dll" || path == preservedShadow)
					continue;
				const auto modified = std::filesystem::last_write_time(path, error);
				if (!error)
					candidates.push_back({ path, modified });
				else
					error.clear();
			}

			std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
				{
					return left.modified > right.modified;
				});
			// The just-created shadow plus four older unlocked files gives a small
			// recovery/debugging window without unbounded per-launch growth.
			for (std::size_t index = 4U; index < candidates.size(); ++index)
			{
				std::filesystem::remove(candidates[index].path, error);
				error.clear();
			}
		}
		catch (...)
		{
		}
	}

	bool CreateVerifiedShadowCopyInDirectory(
		const std::filesystem::path& source,
		const std::array<BYTE, 32U>& sourceHash,
		const std::filesystem::path& directory,
		std::filesystem::path& shadowPath,
		std::wstring& errorMessage)
	{
		std::error_code filesystemError;
		std::filesystem::create_directories(directory, filesystemError);
		if (filesystemError)
		{
			errorMessage = L"Could not create the runtime shadow directory: " +
				Utf8ToWide(filesystemError.message());
			return false;
		}

		std::filesystem::path temporaryPath;
		bool copied = false;
		for (unsigned int attempt = 0; attempt < 8U; ++attempt)
		{
			std::uint64_t nonce = 0;
			if (!BCRYPT_SUCCESS(::BCryptGenRandom(
				nullptr,
				reinterpret_cast<PUCHAR>(&nonce),
				sizeof(nonce),
				BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
			{
				FILETIME now{};
				::GetSystemTimeAsFileTime(&now);
				nonce = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) |
					now.dwLowDateTime;
				nonce ^= ::GetTickCount64();
			}
			const unsigned long generation =
				gShadowGeneration.fetch_add(1UL, std::memory_order_relaxed) + 1UL;
			const std::wstring baseName = L"vSMR.Runtime." +
				std::to_wstring(::GetCurrentProcessId()) + L"." +
				std::to_wstring(generation) + L"." + std::to_wstring(nonce);
			temporaryPath = directory / (baseName + L".tmp");
			shadowPath = directory / (baseName + L".dll");
			if (::CopyFileW(source.c_str(), temporaryPath.c_str(), TRUE))
			{
				copied = true;
				break;
			}
			const DWORD copyError = ::GetLastError();
			if (copyError != ERROR_FILE_EXISTS && copyError != ERROR_ALREADY_EXISTS)
			{
				errorMessage = L"Could not create the runtime shadow copy: " +
					WindowsErrorMessage(copyError);
				return false;
			}
		}
		if (!copied)
		{
			errorMessage = L"Could not allocate a unique runtime shadow filename.";
			return false;
		}

		std::array<BYTE, 32U> shadowHash{};
		if (!HashFileSha256(temporaryPath, shadowHash, errorMessage) ||
			sourceHash != shadowHash)
		{
			if (errorMessage.empty())
				errorMessage = L"The runtime shadow copy failed SHA-256 verification.";
			std::filesystem::remove(temporaryPath, filesystemError);
			return false;
		}

		if (!::MoveFileExW(
			temporaryPath.c_str(),
			shadowPath.c_str(),
			MOVEFILE_WRITE_THROUGH))
		{
			errorMessage = L"Could not finalize the runtime shadow copy: " +
				WindowsErrorMessage(::GetLastError());
			std::filesystem::remove(temporaryPath, filesystemError);
			return false;
		}
		PruneRuntimeCache(directory.parent_path(), shadowPath);
		return true;
	}

	bool CreateVerifiedShadowCopy(
		const std::filesystem::path& source,
		const std::string& version,
		std::array<BYTE, 32U>& verifiedSourceHash,
		std::filesystem::path& shadowPath,
		std::wstring& errorMessage)
	{
		std::error_code filesystemError;
		if (!std::filesystem::is_regular_file(source, filesystemError) || filesystemError)
		{
			errorMessage = L"The selected vSMR runtime is missing: " + source.wstring();
			return false;
		}

		verifiedSourceHash.fill(0);
		if (!HashFileSha256(source, verifiedSourceHash, errorMessage))
			return false;

		const std::vector<std::filesystem::path> directories =
			RuntimeShadowDirectories(version);
		std::wstring lastError;
		for (const std::filesystem::path& directory : directories)
		{
			std::wstring candidateError;
			if (CreateVerifiedShadowCopyInDirectory(
				source,
				verifiedSourceHash,
				directory,
				shadowPath,
				candidateError))
			{
				return true;
			}
			lastError = std::move(candidateError);
			shadowPath.clear();
		}

		errorMessage = L"No writable runtime shadow directory is available.";
		if (!lastError.empty())
			errorMessage += L" " + lastError;
		return false;
	}

	bool AcquireSessionLock(
		const std::filesystem::path& installRoot,
		std::wstring& errorMessage)
	{
		if (gSessionLock != INVALID_HANDLE_VALUE)
			return true;
		const std::filesystem::path lockPath =
			vsmr::updater::GetInstallationSessionLockPath(installRoot);
		if (lockPath.empty())
		{
			errorMessage = L"The update session-lock path is unavailable.";
			return false;
		}
		std::error_code filesystemError;
		std::filesystem::create_directories(lockPath.parent_path(), filesystemError);
		if (filesystemError)
		{
			errorMessage = L"Could not create the update lock directory: " +
				Utf8ToWide(filesystemError.message());
			return false;
		}

		gSessionLock = ::CreateFileW(
			lockPath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_HIDDEN,
			nullptr);
		if (gSessionLock != INVALID_HANDLE_VALUE)
			return true;

		const DWORD error = ::GetLastError();
		if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
		{
			// PrepareUpdateBeforeRuntimeLoad made its decision before this lease.
			// Waiting for an installer would make that decision stale and could pair
			// an old runtime decision with newly swapped data. Fail this plug-in load
			// instead; a later load/startup will run Prepare again against one state.
			errorMessage = L"A vSMR update is currently changing this installation. "
				L"Load vSMR again after the update finishes.";
			return false;
		}

		errorMessage = L"Could not acquire the vSMR session lock: " +
			WindowsErrorMessage(error);
		return false;
	}

	void ReleaseSessionLockForRecovery() noexcept
	{
		if (gSessionLock != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(gSessionLock);
			gSessionLock = INVALID_HANDLE_VALUE;
		}
	}

	struct RuntimeAttempt
	{
		HMODULE module = nullptr;
		VsmrRuntimeApi::ShutdownFunction shutdown = nullptr;
		EuroScopePlugIn::CPlugIn* plugin = nullptr;
		std::filesystem::path shadowPath;
	};

	bool TryCreateRuntime(
		const std::filesystem::path& loaderPath,
		const std::filesystem::path& installRoot,
		const std::filesystem::path& dataRoot,
		const std::filesystem::path& canonicalRuntimePath,
		const std::string& version,
		RuntimeAttempt& attempt,
		std::wstring& errorMessage)
	{
		// Runtime shadows share one per-user LocalAppData cache. Keep copy,
		// pruning, and LoadLibrary under one cross-process guard so another
		// EuroScope process cannot delete a finalized-but-not-yet-loaded shadow.
		NamedMutexGuard shadowCacheGuard;
		if (!shadowCacheGuard.Acquire(
			kRuntimeShadowMutexName,
			kRuntimeShadowMutexWaitMs,
			errorMessage))
		{
			return false;
		}
		std::array<BYTE, 32U> verifiedRuntimeHash{};
		if (!CreateVerifiedShadowCopy(
			canonicalRuntimePath,
			version,
			verifiedRuntimeHash,
			attempt.shadowPath,
			errorMessage))
		{
			return false;
		}

		// The final path can live in the per-user Temp tree when LocalAppData is
		// unavailable. Lease the finalized file against writers and deletion, then
		// hash that exact path again while the lease remains held through
		// LoadLibraryExW. This closes the move-to-load substitution window without
		// weakening the loader's DLL search policy.
		FileHandleGuard shadowLease(::CreateFileW(
			attempt.shadowPath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr));
		if (!shadowLease)
		{
			const DWORD leaseError = ::GetLastError();
			errorMessage = L"Could not secure the finalized runtime shadow: " +
				WindowsErrorMessage(leaseError);
			::DeleteFileW(attempt.shadowPath.c_str());
			return false;
		}

		std::filesystem::path resolvedShadowPath;
		if (!ResolveHandlePath(shadowLease.Get(), resolvedShadowPath, errorMessage))
		{
			shadowLease.Reset();
			::DeleteFileW(attempt.shadowPath.c_str());
			return false;
		}

		std::array<BYTE, 32U> finalShadowHash{};
		if (!HashFileSha256(resolvedShadowPath, finalShadowHash, errorMessage) ||
			finalShadowHash != verifiedRuntimeHash)
		{
			if (errorMessage.empty())
				errorMessage = L"The finalized runtime shadow failed SHA-256 verification.";
			shadowLease.Reset();
			::DeleteFileW(attempt.shadowPath.c_str());
			return false;
		}

		attempt.module = ::LoadLibraryExW(
			resolvedShadowPath.c_str(),
			nullptr,
			LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		const DWORD loadError = attempt.module != nullptr
			? ERROR_SUCCESS
			: ::GetLastError();
		shadowLease.Reset();
		shadowCacheGuard.Release();
		if (attempt.module == nullptr)
		{
			::DeleteFileW(attempt.shadowPath.c_str());
			errorMessage = L"Could not load the vSMR runtime: " +
				WindowsErrorMessage(loadError);
			return false;
		}
		attempt.shadowPath = std::move(resolvedShadowPath);

		const auto getAbiVersion = reinterpret_cast<VsmrRuntimeApi::GetAbiVersionFunction>(
			::GetProcAddress(attempt.module, "VsmrRuntimeGetAbiVersion"));
		const auto create = reinterpret_cast<VsmrRuntimeApi::CreateFunction>(
			::GetProcAddress(attempt.module, "VsmrRuntimeCreate"));
		attempt.shutdown = reinterpret_cast<VsmrRuntimeApi::ShutdownFunction>(
			::GetProcAddress(attempt.module, "VsmrRuntimeShutdown"));
		if (getAbiVersion == nullptr || create == nullptr || attempt.shutdown == nullptr)
		{
			errorMessage = L"The selected runtime does not expose the vSMR loader ABI.";
			::FreeLibrary(attempt.module);
			attempt.module = nullptr;
			return false;
		}
		if (getAbiVersion() != VsmrRuntimeApi::AbiVersion)
		{
			errorMessage = L"The selected runtime requires an incompatible vSMR loader.";
			::FreeLibrary(attempt.module);
			attempt.module = nullptr;
			return false;
		}

		const std::wstring loaderNative = loaderPath.wstring();
		const std::wstring installNative = installRoot.wstring();
		const std::wstring dataNative = dataRoot.wstring();
		const std::wstring canonicalNative = canonicalRuntimePath.wstring();
		const std::wstring shadowNative = attempt.shadowPath.wstring();
		const VsmrRuntimeApi::BootstrapContext context{
			sizeof(VsmrRuntimeApi::BootstrapContext),
			VsmrRuntimeApi::AbiVersion,
			loaderNative.c_str(),
			installNative.c_str(),
			dataNative.c_str(),
			canonicalNative.c_str(),
			shadowNative.c_str()
		};
		std::array<char, 1024U> runtimeError{};
		if (!create(
			&context,
			&attempt.plugin,
			runtimeError.data(),
			runtimeError.size()) || attempt.plugin == nullptr)
		{
			errorMessage = runtimeError[0] != '\0'
				? Utf8ToWide(runtimeError.data())
				: L"The vSMR runtime rejected initialization.";
			::FreeLibrary(attempt.module);
			attempt.module = nullptr;
			attempt.shutdown = nullptr;
			attempt.plugin = nullptr;
			return false;
		}
		return true;
	}

	void PublishRuntime(RuntimeAttempt& attempt)
	{
		gRuntimeModule = attempt.module;
		gRuntimeShutdown = attempt.shutdown;
		gPluginInstance = attempt.plugin;
		attempt.module = nullptr;
		attempt.shutdown = nullptr;
		attempt.plugin = nullptr;
	}

	void ShowStartupFailure(const std::wstring& message) noexcept
	{
		AppendBootstrapLog(L"Startup failed: " + message);
		::MessageBoxW(
			nullptr,
			message.c_str(),
			L"vSMR could not start",
			MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}

	bool ReleasePublishedRuntime(std::wstring& errorMessage) noexcept
	{
		errorMessage.clear();
		bool runtimeReleased =
			gRuntimeModule == nullptr &&
			gRuntimeShutdown == nullptr &&
			gPluginInstance == nullptr;
		if (gRuntimeShutdown != nullptr)
		{
			try
			{
				runtimeReleased = gRuntimeShutdown();
			}
			catch (...)
			{
				runtimeReleased = false;
			}
		}

		if (!runtimeReleased)
		{
			errorMessage =
				L"The previous vSMR runtime still owns active EuroScope radar objects or callbacks.";
			return false;
		}

		HMODULE const releasedModule = gRuntimeModule;
		if (releasedModule != nullptr && !::FreeLibrary(releasedModule))
		{
			errorMessage = L"Windows could not unmap the released vSMR runtime: " +
				WindowsErrorMessage(::GetLastError());
			return false;
		}

		gPluginInstance = nullptr;
		gRuntimeShutdown = nullptr;
		gRuntimeModule = nullptr;
		ReleaseSessionLockForRecovery();
		gShutdownRequested = false;
		return true;
	}
}

void __declspec(dllexport) EuroScopePlugInInit(
	EuroScopePlugIn::CPlugIn** pluginInstance)
{
	if (pluginInstance == nullptr)
		return;
	*pluginInstance = nullptr;

	std::lock_guard<std::mutex> guard(gLifecycleMutex);
	if (gShutdownRequested)
	{
		std::wstring releaseError;
		if (!ReleasePublishedRuntime(releaseError))
		{
			ShowStartupFailure(
				releaseError +
				L" Close its radar displays and load vSMR again, or restart EuroScope.");
			return;
		}
		AppendBootstrapLog(
			L"Released the retained runtime generation before an in-session reload.");
	}
	if (gPluginInstance != nullptr)
	{
		*pluginInstance = gPluginInstance;
		return;
	}

	try
	{
		const std::filesystem::path loaderPath = ModulePath();
		if (loaderPath.empty())
		{
			ShowStartupFailure(L"Windows could not resolve the vSMR loader path.");
			return;
		}
		const std::filesystem::path installRoot = loaderPath.parent_path();
		const std::filesystem::path dataRoot = installRoot / L"vSMR_Data";
		const std::filesystem::path canonicalRuntimePath =
			installRoot / kRuntimeRelativePath;
		const std::string currentVersion = ReadRuntimeVersion(canonicalRuntimePath);

		// Preparing updates before any runtime code enters EuroScope
		ProgressWindow progress(reinterpret_cast<HINSTANCE>(&__ImageBase));
		vsmr::updater::StartupOptions updateOptions;
		updateOptions.installRoot = installRoot;
		updateOptions.dataRoot = dataRoot;
		updateOptions.canonicalRuntimePath = canonicalRuntimePath;
		updateOptions.loaderPath = loaderPath;
		updateOptions.currentVersion = currentVersion;
		updateOptions.loaderVersion = VsmrLoaderVersion::Value;
		updateOptions.defaultChannel = currentVersion.find('-') == std::string::npos
			? vsmr::updater::UpdateChannel::Stable
			: vsmr::updater::UpdateChannel::Beta;
		updateOptions.hostProcessId = ::GetCurrentProcessId();
		updateOptions.expectedRuntimeAbi = VsmrRuntimeApi::AbiVersion;
		updateOptions.overallDeadlineMs = 30000U;
		updateOptions.progressCallback = [&progress](const vsmr::updater::Progress& update)
			{
				return progress.Update(update);
			};

		const vsmr::updater::StartupResult update =
			vsmr::updater::PrepareUpdateBeforeRuntimeLoad(updateOptions);
		progress.Close();
		if (!update.message.empty())
			AppendBootstrapLog(update.message);

		std::filesystem::path selectedRuntimePath = update.selectedRuntimePath;
		if (selectedRuntimePath.empty())
		{
			if (update.status == vsmr::updater::StartupStatus::FailedOpen)
			{
				ShowStartupFailure(
					update.message.empty()
						? L"The updater could not prove that the installed runtime is safe to load."
						: update.message);
				return;
			}
			selectedRuntimePath = canonicalRuntimePath;
		}
		if (!selectedRuntimePath.is_absolute())
		{
			ShowStartupFailure(L"The updater returned a non-absolute runtime path.");
			return;
		}

		// Locking the selected runtime/data generation for this EuroScope session
		std::wstring lockError;
		if (!AcquireSessionLock(installRoot, lockError))
		{
			ShowStartupFailure(lockError);
			return;
		}

		const std::string selectedVersion = update.selectedVersion.empty()
			? ReadRuntimeVersion(selectedRuntimePath)
			: update.selectedVersion;
		// Starting the selected runtime in a verified shadow copy
		RuntimeAttempt runtime;
		std::wstring runtimeError;
		if (TryCreateRuntime(
			loaderPath,
			installRoot,
			dataRoot,
			selectedRuntimePath,
			selectedVersion,
			runtime,
			runtimeError))
		{
			PublishRuntime(runtime);
			*pluginInstance = gPluginInstance;
			if (update.updateActivated &&
				!vsmr::updater::ConfirmRuntimeHealthy(update))
			{
				AppendBootstrapLog(
					L"Runtime started, but its pending-health marker could not be cleared; "
					L"the updater will recover conservatively on the next launch.");
			}
			AppendBootstrapLog(L"Runtime activated: " + selectedRuntimePath.wstring());
			return;
		}

		AppendBootstrapLog(L"Runtime activation failed: " + runtimeError);
		if (update.updateActivated)
		{
			// Restoring the previous complete runtime/data pair after a failed start
			if (!vsmr::updater::MarkRuntimeUnhealthy(update))
			{
				AppendBootstrapLog(
					L"The failed runtime health marker could not be updated; "
					L"rollback will revalidate it and fail closed if necessary.");
			}
			// Rollback must take the same installation lock exclusively. No runtime
			// was published, so it is safe to unregister this loader temporarily.
			ReleaseSessionLockForRecovery();
			std::filesystem::path restoredRuntimePath;
			std::wstring rollbackError;
			if (vsmr::updater::RollbackPreparedUpdate(
				updateOptions,
				update,
				&restoredRuntimePath,
				&rollbackError))
			{
				std::wstring restoredLockError;
				if (!AcquireSessionLock(installRoot, restoredLockError))
				{
					runtimeError += L"\n\nRollback completed, but the restored installation could not be locked: " +
						restoredLockError;
					ShowStartupFailure(runtimeError);
					return;
				}
				RuntimeAttempt restoredRuntime;
				std::wstring restoredError;
				if (TryCreateRuntime(
					loaderPath,
					installRoot,
					dataRoot,
					restoredRuntimePath,
					ReadRuntimeVersion(restoredRuntimePath),
					restoredRuntime,
					restoredError))
				{
					PublishRuntime(restoredRuntime);
					*pluginInstance = gPluginInstance;
					AppendBootstrapLog(L"Updated runtime rolled back; restored runtime activated.");
					return;
				}
				runtimeError += L"\n\nRollback completed, but the restored runtime failed: " +
					restoredError;
			}
			else
			{
				runtimeError += L"\n\nThe update rollback also failed: " + rollbackError;
			}
		}

		ReleaseSessionLockForRecovery();
		ShowStartupFailure(runtimeError);
	}
	catch (const std::exception& exception)
	{
		ReleaseSessionLockForRecovery();
		ShowStartupFailure(Utf8ToWide(exception.what()));
	}
	catch (...)
	{
		ReleaseSessionLockForRecovery();
		ShowStartupFailure(L"The vSMR loader failed unexpectedly.");
	}
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
	std::lock_guard<std::mutex> guard(gLifecycleMutex);
	if (gShutdownRequested)
		return;
	gShutdownRequested = true;
	std::wstring releaseError;
	if (!ReleasePublishedRuntime(releaseError))
	{
		AppendBootstrapLog(
			L"Runtime shutdown retained its module: " + releaseError);
		return;
	}
}
