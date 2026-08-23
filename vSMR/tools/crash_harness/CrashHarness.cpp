#include "crash/CrashReportProtocol.hpp"
#include "crash/CrashReportSupport.hpp"

#include <Windows.h>
#include <werapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
	VsmrCrashProtocol::SharedState gSharedState{};

	using EventCallback = PFN_WER_RUNTIME_EXCEPTION_EVENT;
	using SignatureCallback = PFN_WER_RUNTIME_EXCEPTION_EVENT_SIGNATURE;
	using DebuggerCallback = PFN_WER_RUNTIME_EXCEPTION_DEBUGGER_LAUNCH;

	bool GetImageRange(HMODULE module, std::uint64_t& base, std::uint32_t& size) noexcept
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
		if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0)
			return false;
		base = reinterpret_cast<std::uint64_t>(module);
		size = nt->OptionalHeader.SizeOfImage;
		return true;
	}

	bool InitializeState(const wchar_t* reportDirectory)
	{
		VsmrCrashProtocol::InitializeSharedState(gSharedState);
		const HMODULE module = ::GetModuleHandleW(nullptr);
		if (!GetImageRange(module, gSharedState.moduleBase, gSharedState.moduleSize))
			return false;

		std::array<wchar_t, VsmrCrashProtocol::kPathChars> modulePath{};
		const DWORD length = ::GetModuleFileNameW(
			module,
			modulePath.data(),
			static_cast<DWORD>(modulePath.size()));
		if (length == 0 || length >= modulePath.size())
			return false;

		VsmrCrashProtocol::CopyText(gSharedState.reportDirectory, reportDirectory);
		VsmrCrashProtocol::CopyText(gSharedState.modulePath, modulePath.data());
		VsmrCrashProtocol::CopyText(gSharedState.pluginVersion, "2.0.0-beta.4-harness");
		VsmrCrashProtocol::CopyText(gSharedState.gitCommit, "harness");
		VsmrCrashProtocol::CopyText(gSharedState.dllSha256, "harness");
		VsmrCrashProtocol::CopyText(gSharedState.pdbIdentity, "harness");
		VsmrCrashProtocol::CopyText(gSharedState.euroScopeVersion, "isolated-harness");
		VsmrCrashProtocol::CopyText(gSharedState.buildTimestampUtc, "harness");
		VsmrCrashProtocol::CopyText(gSharedState.sourceState, "harness");
		VsmrCrashProtocol::PublishState(gSharedState, "connection", "isolated");
		VsmrCrashProtocol::PublishBreadcrumb(gSharedState, "harness", "state initialized");
		VsmrCrashProtocol::PublishThreadRole(gSharedState, "crash harness main");
		VsmrCrashProtocol::PublishLog(gSharedState, "deterministic crash-harness record");
		VsmrCrashProtocol::MarkReady(gSharedState);
		return true;
	}

	__declspec(noinline) void TriggerAccessViolation()
	{
		*reinterpret_cast<volatile std::uint32_t*>(0) = 0x56534D52U;
	}

	__declspec(noinline) void TriggerStackOverflow(unsigned depth)
	{
		volatile char page[4096]{};
		page[depth % sizeof(page)] = static_cast<char>(depth);
		TriggerStackOverflow(depth + 1U);
		page[(depth + 1U) % sizeof(page)] = static_cast<char>(depth + 1U);
	}

	bool TriggerHandledAccessViolation()
	{
		__try
		{
			TriggerAccessViolation();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return true;
		}
		return false;
	}

	std::wstring Quote(const std::wstring& value)
	{
		return L"\"" + value + L"\"";
	}

	bool WriteSizedFile(const std::filesystem::path& path, DWORD size, BYTE marker)
	{
		const std::wstring nativePath = VsmrCrashSupport::MakeNativePath(path);
		const HANDLE file = ::CreateFileW(
			nativePath.c_str(),
			GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		std::vector<BYTE> contents(size, marker);
		DWORD written = 0;
		const bool success = size == 0 ||
			(::WriteFile(file, contents.data(), size, &written, nullptr) != FALSE && written == size);
		::CloseHandle(file);
		return success;
	}

	bool SetArtifactTime(
		const std::filesystem::path& path,
		std::filesystem::file_time_type timestamp)
	{
		std::error_code error;
		std::filesystem::last_write_time(path, timestamp, error);
		return !error;
	}

	int RunWritableSelection(const wchar_t* rejectedDirectory, const wchar_t* fallbackDirectory)
	{
		const std::array<std::filesystem::path, 2> candidates = {
			std::filesystem::path(rejectedDirectory),
			std::filesystem::path(fallbackDirectory)
		};
		std::size_t selectedIndex = static_cast<std::size_t>(-1);
		const std::filesystem::path selected =
			VsmrCrashSupport::SelectFirstWritableDirectory(candidates, &selectedIndex);
		if (selectedIndex != 1U || selected.empty() ||
			VsmrCrashSupport::ProbeWritableDirectory(candidates[0]) ||
			!VsmrCrashSupport::ProbeWritableDirectory(candidates[1]))
		{
			return 40;
		}
		std::wprintf(L"selected=1 path=%ls\n", selected.c_str());
		return 0;
	}

	bool CreateRetentionGroup(
		const std::filesystem::path& directory,
		const std::wstring& group,
		DWORD bytesPerFile,
		std::filesystem::file_time_type timestamp)
	{
		const auto textPath = directory / (group + L".txt");
		const auto dumpPath = directory / (group + L".dmp");
		return WriteSizedFile(textPath, bytesPerFile, 0x54U) &&
			WriteSizedFile(dumpPath, bytesPerFile, 0x44U) &&
			SetArtifactTime(textPath, timestamp) &&
			SetArtifactTime(dumpPath, timestamp);
	}

	int RunRetentionSupport(const wchar_t* rootDirectory)
	{
		const std::filesystem::path root(rootDirectory);
		const std::filesystem::path countDirectory = root / L"count";
		const std::filesystem::path sizeDirectory = root / L"size";
		std::error_code error;
		std::filesystem::create_directories(countDirectory, error);
		if (error)
			return 50;
		std::filesystem::create_directories(sizeDirectory, error);
		if (error)
			return 51;

		const auto now = std::filesystem::file_time_type::clock::now();
		for (int index = 0; index < 12; ++index)
		{
			std::array<wchar_t, 64> group{};
			_snwprintf_s(group.data(), group.size(), _TRUNCATE,
				L"vSMR-crash-count-%02d", index);
			if (!CreateRetentionGroup(
				countDirectory,
				group.data(),
				4U,
				now + std::chrono::seconds(index)))
			{
				return 52;
			}
		}
		const auto orphanText = countDirectory / L"vSMR-crash-orphan-text.txt";
		const auto orphanDump = countDirectory / L"vSMR-crash-orphan-dump.dmp";
		const auto temporaryDump = countDirectory / L"vSMR-crash-stale.dmp.tmp";
		const auto unrelated = countDirectory / L"operator-notes.txt";
		if (!WriteSizedFile(orphanText, 4U, 0x54U) ||
			!WriteSizedFile(orphanDump, 4U, 0x44U) ||
			!WriteSizedFile(temporaryDump, 4U, 0x58U) ||
			!WriteSizedFile(unrelated, 4U, 0x4EU) ||
			!SetArtifactTime(orphanText, now + std::chrono::seconds(101)) ||
			!SetArtifactTime(orphanDump, now + std::chrono::seconds(100)))
		{
			return 53;
		}

		const auto countResult = VsmrCrashSupport::ApplyRetention(countDirectory);
		if (countResult.groupsRetained != VsmrCrashSupport::kDefaultRetentionGroups ||
			countResult.groupsRemoved != 4U || countResult.filesRemoved != 9U ||
			!std::filesystem::exists(orphanText) ||
			!std::filesystem::exists(orphanDump) ||
			std::filesystem::exists(temporaryDump) ||
			!std::filesystem::exists(unrelated) ||
			std::filesystem::exists(countDirectory / L"vSMR-crash-count-00.txt") ||
			!std::filesystem::exists(countDirectory / L"vSMR-crash-count-11.txt"))
		{
			return 54;
		}

		for (int index = 0; index < 5; ++index)
		{
			std::array<wchar_t, 64> group{};
			_snwprintf_s(group.data(), group.size(), _TRUNCATE,
				L"vSMR-crash-size-%02d", index);
			if (!CreateRetentionGroup(
				sizeDirectory,
				group.data(),
				10U,
				now + std::chrono::seconds(index)))
			{
				return 55;
			}
		}
		const auto sizeResult = VsmrCrashSupport::ApplyRetention(sizeDirectory, 10U, 55U);
		if (sizeResult.groupsRetained != 2U || sizeResult.groupsRemoved != 3U ||
			sizeResult.filesRemoved != 6U || sizeResult.bytesRetained != 40U ||
			std::filesystem::exists(sizeDirectory / L"vSMR-crash-size-00.txt") ||
			!std::filesystem::exists(sizeDirectory / L"vSMR-crash-size-04.txt"))
		{
			return 56;
		}

		std::printf("retention=count:%zu,size:%zu\n",
			countResult.groupsRetained,
			sizeResult.groupsRetained);
		return 0;
	}

	int RunWaitingTarget(const wchar_t* reportDirectory)
	{
		if (!InitializeState(reportDirectory))
			return 10;
		std::printf("%llX\n", static_cast<unsigned long long>(
			reinterpret_cast<std::uintptr_t>(&gSharedState)));
		std::fflush(stdout);
		::Sleep(120000);
		return 0;
	}

	int RunDirectCallback(
		const wchar_t* handlerPath,
		const wchar_t* reportDirectory,
		unsigned callbackCount)
	{
		std::array<wchar_t, 32768> executablePath{};
		const DWORD executableLength = ::GetModuleFileNameW(
			nullptr,
			executablePath.data(),
			static_cast<DWORD>(executablePath.size()));
		if (executableLength == 0 || executableLength >= executablePath.size())
			return 20;

		SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
		HANDLE readPipe = nullptr;
		HANDLE writePipe = nullptr;
		if (!::CreatePipe(&readPipe, &writePipe, &security, 0))
			return 21;
		::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = writePipe;
		startup.hStdError = writePipe;
		startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
		PROCESS_INFORMATION process{};
		std::wstring command = Quote(std::wstring(executablePath.data(), executableLength)) +
			L" --wait-target " + Quote(reportDirectory);
		std::vector<wchar_t> mutableCommand(command.begin(), command.end());
		mutableCommand.push_back(L'\0');
		const BOOL created = ::CreateProcessW(
			nullptr,
			mutableCommand.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&startup,
			&process);
		::CloseHandle(writePipe);
		if (!created)
		{
			::CloseHandle(readPipe);
			return 22;
		}

		std::array<char, 128> addressText{};
		DWORD bytesRead = 0;
		const BOOL readOk = ::ReadFile(
			readPipe,
			addressText.data(),
			static_cast<DWORD>(addressText.size() - 1U),
			&bytesRead,
			nullptr);
		::CloseHandle(readPipe);
		if (!readOk || bytesRead == 0)
		{
			::TerminateProcess(process.hProcess, 23);
			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
			return 23;
		}
		addressText[bytesRead] = '\0';
		const std::uintptr_t remoteStateAddress = static_cast<std::uintptr_t>(
			std::strtoull(addressText.data(), nullptr, 16));

		VsmrCrashProtocol::SharedState snapshot{};
		SIZE_T copied = 0;
		if (remoteStateAddress == 0 ||
			!::ReadProcessMemory(
				process.hProcess,
				reinterpret_cast<const void*>(remoteStateAddress),
				&snapshot,
				sizeof(snapshot),
				&copied) ||
			copied != sizeof(snapshot))
		{
			::TerminateProcess(process.hProcess, 24);
			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
			return 24;
		}

		// Exercise clean WER loader lifetimes before the real callback invocation.
		for (int iteration = 0; iteration < 3; ++iteration)
		{
			HMODULE probe = ::LoadLibraryW(handlerPath);
			if (probe == nullptr ||
				::GetProcAddress(probe, "OutOfProcessExceptionEventCallback") == nullptr ||
				::GetProcAddress(probe, "OutOfProcessExceptionEventSignatureCallback") == nullptr ||
				::GetProcAddress(probe, "OutOfProcessExceptionEventDebuggerLaunchCallback") == nullptr)
			{
				if (probe != nullptr)
					::FreeLibrary(probe);
				::TerminateProcess(process.hProcess, 25);
				::CloseHandle(process.hThread);
				::CloseHandle(process.hProcess);
				return 25;
			}
			::FreeLibrary(probe);
		}

		const HMODULE handler = ::LoadLibraryW(handlerPath);
		if (handler == nullptr)
		{
			::TerminateProcess(process.hProcess, 26);
			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
			return 26;
		}
		const auto eventCallback = reinterpret_cast<EventCallback>(
			::GetProcAddress(handler, "OutOfProcessExceptionEventCallback"));
		const auto signatureCallback = reinterpret_cast<SignatureCallback>(
			::GetProcAddress(handler, "OutOfProcessExceptionEventSignatureCallback"));
		const auto debuggerCallback = reinterpret_cast<DebuggerCallback>(
			::GetProcAddress(handler, "OutOfProcessExceptionEventDebuggerLaunchCallback"));
		if (eventCallback == nullptr || signatureCallback == nullptr || debuggerCallback == nullptr)
		{
			::FreeLibrary(handler);
			::TerminateProcess(process.hProcess, 27);
			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
			return 27;
		}

		::SuspendThread(process.hThread);
		WER_RUNTIME_EXCEPTION_INFORMATION information{};
		information.dwSize = sizeof(information);
		information.hProcess = process.hProcess;
		information.hThread = process.hThread;
		information.exceptionRecord.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		information.exceptionRecord.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
		information.exceptionRecord.ExceptionAddress = reinterpret_cast<void*>(
			static_cast<std::uintptr_t>(snapshot.moduleBase + 0x1000U));
		information.context.ContextFlags = CONTEXT_FULL;
		::GetThreadContext(process.hThread, &information.context);
#if defined(_M_IX86)
		information.context.Eip = static_cast<DWORD>(snapshot.moduleBase + 0x1000U);
#endif
		information.pwszReportId = L"vSMR-harness-direct";
		information.bIsFatal = TRUE;

		std::atomic<unsigned> ready{ 0U };
		std::atomic<bool> begin{ callbackCount <= 1U };
		std::atomic<unsigned> failures{ 0U };
		auto invokeCallbacks = [&]()
		{
			ready.fetch_add(1U, std::memory_order_release);
			while (!begin.load(std::memory_order_acquire))
				std::this_thread::yield();

			BOOL claimed = TRUE;
			std::array<wchar_t, MAX_PATH> eventName{};
			DWORD eventNameChars = static_cast<DWORD>(eventName.size());
			DWORD signatureCount = 0;
			const HRESULT eventResult = eventCallback(
				reinterpret_cast<void*>(remoteStateAddress),
				&information,
				&claimed,
				eventName.data(),
				&eventNameChars,
				&signatureCount);

			std::array<wchar_t, MAX_PATH> name{};
			std::array<wchar_t, MAX_PATH> value{};
			DWORD nameChars = static_cast<DWORD>(name.size());
			DWORD valueChars = static_cast<DWORD>(value.size());
			const HRESULT signatureResult = signatureCallback(
				reinterpret_cast<void*>(remoteStateAddress),
				&information,
				0U,
				name.data(),
				&nameChars,
				value.data(),
				&valueChars);

			BOOL customDebugger = TRUE;
			BOOL autoLaunch = TRUE;
			std::array<wchar_t, MAX_PATH> debuggerCommand{};
			DWORD debuggerChars = static_cast<DWORD>(debuggerCommand.size());
			const HRESULT debuggerResult = debuggerCallback(
				reinterpret_cast<void*>(remoteStateAddress),
				&information,
				&customDebugger,
				debuggerCommand.data(),
				&debuggerChars,
				&autoLaunch);

			if (FAILED(eventResult) || claimed != FALSE ||
				FAILED(signatureResult) || FAILED(debuggerResult) ||
				customDebugger != FALSE || autoLaunch != FALSE)
			{
				failures.fetch_add(1U, std::memory_order_relaxed);
			}
		};

		std::vector<std::thread> callbackThreads;
		if (callbackCount <= 1U)
		{
			invokeCallbacks();
		}
		else
		{
			callbackThreads.reserve(callbackCount);
			for (unsigned index = 0; index < callbackCount; ++index)
				callbackThreads.emplace_back(invokeCallbacks);
			while (ready.load(std::memory_order_acquire) != callbackCount)
				std::this_thread::yield();
			begin.store(true, std::memory_order_release);
			for (auto& callbackThread : callbackThreads)
				callbackThread.join();
		}

		::FreeLibrary(handler);
		::TerminateProcess(process.hProcess, 0);
		::WaitForSingleObject(process.hProcess, 5000);
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		if (failures.load(std::memory_order_relaxed) != 0U)
			return 28;

		std::printf("claimed=0 callbacks=%u\n", callbackCount);
		return 0;
	}

	int RunWerScenario(const wchar_t* mode, const wchar_t* handlerPath, const wchar_t* reportDirectory)
	{
		if (!InitializeState(reportDirectory))
			return 30;
		::WerSetFlags(WER_FAULT_REPORTING_NO_UI);
		if (std::wcscmp(mode, L"--wer-register-cycle") == 0)
		{
			for (int iteration = 0; iteration < 4; ++iteration)
			{
				const HRESULT cycleRegistration =
					::WerRegisterRuntimeExceptionModule(handlerPath, &gSharedState);
				if (FAILED(cycleRegistration))
					return 36;
				const HRESULT cycleUnregistration =
					::WerUnregisterRuntimeExceptionModule(handlerPath, &gSharedState);
				if (FAILED(cycleUnregistration))
					return 37;
			}
			return 0;
		}
		const HRESULT registered = ::WerRegisterRuntimeExceptionModule(handlerPath, &gSharedState);
		if (FAILED(registered))
		{
			std::fwprintf(stderr, L"WerRegisterRuntimeExceptionModule failed: 0x%08lX\n", registered);
			return 31;
		}

		if (std::wcscmp(mode, L"--wer-handled") == 0)
		{
			const bool handled = TriggerHandledAccessViolation();
			::WerUnregisterRuntimeExceptionModule(handlerPath, &gSharedState);
			return handled ? 0 : 32;
		}
		if (std::wcscmp(mode, L"--wer-access") == 0)
		{
			TriggerAccessViolation();
			return 33;
		}
		if (std::wcscmp(mode, L"--wer-stack") == 0)
		{
			ULONG stackGuarantee = 64U * 1024U;
			::SetThreadStackGuarantee(&stackGuarantee);
			TriggerStackOverflow(1U);
			return 34;
		}

		::WerUnregisterRuntimeExceptionModule(handlerPath, &gSharedState);
		return 35;
	}
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc == 3 && std::wcscmp(argv[1], L"--wait-target") == 0)
		return RunWaitingTarget(argv[2]);
	if (argc == 4 && std::wcscmp(argv[1], L"--direct") == 0)
		return RunDirectCallback(argv[2], argv[3], 1U);
	if (argc == 4 && std::wcscmp(argv[1], L"--direct-concurrent") == 0)
		return RunDirectCallback(argv[2], argv[3], 8U);
	if (argc == 4 && std::wcscmp(argv[1], L"--support-select") == 0)
		return RunWritableSelection(argv[2], argv[3]);
	if (argc == 3 && std::wcscmp(argv[1], L"--support-retention") == 0)
		return RunRetentionSupport(argv[2]);
	if (argc == 4 && (
		std::wcscmp(argv[1], L"--wer-handled") == 0 ||
		std::wcscmp(argv[1], L"--wer-access") == 0 ||
		std::wcscmp(argv[1], L"--wer-stack") == 0 ||
		std::wcscmp(argv[1], L"--wer-register-cycle") == 0))
	{
		return RunWerScenario(argv[1], argv[2], argv[3]);
	}

	std::fwprintf(
		stderr,
		L"Usage:\n"
		L"  vSMRCrashHarness --direct <handler.dll> <report-dir>\n"
		L"  vSMRCrashHarness --direct-concurrent <handler.dll> <report-dir>\n"
		L"  vSMRCrashHarness --support-select <rejected-dir> <fallback-dir>\n"
		L"  vSMRCrashHarness --support-retention <report-dir>\n"
		L"  vSMRCrashHarness --wer-handled <handler.dll> <report-dir>\n"
		L"  vSMRCrashHarness --wer-access <handler.dll> <report-dir>\n"
		L"  vSMRCrashHarness --wer-stack <handler.dll> <report-dir>\n"
		L"  vSMRCrashHarness --wer-register-cycle <handler.dll> <report-dir>\n");
	return 2;
}
