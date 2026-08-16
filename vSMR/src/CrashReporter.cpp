#include "stdafx.h"
#include "CrashReporter.hpp"

#include <DbgHelp.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
		HANDLE,
		DWORD,
		HANDLE,
		MINIDUMP_TYPE,
		PMINIDUMP_EXCEPTION_INFORMATION,
		PMINIDUMP_USER_STREAM_INFORMATION,
		PMINIDUMP_CALLBACK_INFORMATION);

	std::atomic<void*> gHandler{ nullptr };
	HMODULE gDbgHelpModule = nullptr;
	MiniDumpWriteDumpFn gMiniDumpWriteDump = nullptr;
	ULONG_PTR gModuleStart = 0;
	ULONG_PTR gModuleEnd = 0;
	DWORD gSuppressionTls = TLS_OUT_OF_INDEXES;
	volatile LONG gReportStarted = 0;
	std::array<wchar_t, 1024> gReportDirectory{};
	std::array<char, 64> gVersion{};

	bool IsFatalExceptionCode(DWORD code)
	{
		switch (code)
		{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_DATATYPE_MISALIGNMENT:
		case EXCEPTION_FLT_DENORMAL_OPERAND:
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		case EXCEPTION_FLT_INEXACT_RESULT:
		case EXCEPTION_FLT_INVALID_OPERATION:
		case EXCEPTION_FLT_OVERFLOW:
		case EXCEPTION_FLT_STACK_CHECK:
		case EXCEPTION_FLT_UNDERFLOW:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_INT_OVERFLOW:
		case EXCEPTION_INVALID_DISPOSITION:
		case EXCEPTION_NONCONTINUABLE_EXCEPTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case 0xC0000374UL: // STATUS_HEAP_CORRUPTION
		case 0xC0000409UL: // STATUS_STACK_BUFFER_OVERRUN / fail-fast
			return true;
		default:
			return false;
		}
	}

	bool IsDirectory(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::create_directories(path, error);
		return !error && std::filesystem::is_directory(path, error) && !error;
	}

	std::wstring ResolveReportDirectory(HMODULE module)
	{
		std::array<wchar_t, 32768> modulePathBuffer{};
		const DWORD moduleLength = ::GetModuleFileNameW(
			module,
			modulePathBuffer.data(),
			static_cast<DWORD>(modulePathBuffer.size()));
		if (moduleLength > 0 && moduleLength < modulePathBuffer.size())
		{
			const std::filesystem::path modulePath(
				std::wstring(modulePathBuffer.data(), moduleLength));
			const std::filesystem::path preferred =
				modulePath.parent_path() / L"vSMR_Data" / L"CrashReports";
			if (IsDirectory(preferred))
				return preferred.wstring();
		}

		std::array<wchar_t, 32768> localAppData{};
		const DWORD localLength = ::GetEnvironmentVariableW(
			L"LOCALAPPDATA",
			localAppData.data(),
			static_cast<DWORD>(localAppData.size()));
		if (localLength > 0 && localLength < localAppData.size())
		{
			const std::filesystem::path fallback =
				std::filesystem::path(std::wstring(localAppData.data(), localLength)) /
				L"vSMR" /
				L"CrashReports";
			if (IsDirectory(fallback))
				return fallback.wstring();
		}

		std::array<wchar_t, 32768> temporaryDirectory{};
		const DWORD temporaryLength = ::GetTempPathW(
			static_cast<DWORD>(temporaryDirectory.size()),
			temporaryDirectory.data());
		if (temporaryLength > 0 && temporaryLength < temporaryDirectory.size())
		{
			const std::filesystem::path fallback =
				std::filesystem::path(std::wstring(temporaryDirectory.data(), temporaryLength)) /
				L"vSMR_CrashReports";
			if (IsDirectory(fallback))
				return fallback.wstring();
		}

		return {};
	}

	void LoadDbgHelp()
	{
		std::array<wchar_t, MAX_PATH> systemDirectory{};
		const UINT length = ::GetSystemDirectoryW(
			systemDirectory.data(),
			static_cast<UINT>(systemDirectory.size()));
		if (length == 0 || length >= systemDirectory.size())
			return;

		std::wstring dbgHelpPath(systemDirectory.data(), length);
		dbgHelpPath += L"\\dbghelp.dll";
		gDbgHelpModule = ::LoadLibraryW(dbgHelpPath.c_str());
		if (gDbgHelpModule == nullptr)
			return;

		gMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
			::GetProcAddress(gDbgHelpModule, "MiniDumpWriteDump"));
	}

	LONG CALLBACK HandleVectoredException(EXCEPTION_POINTERS* exceptionPointers)
	{
		if (exceptionPointers == nullptr || exceptionPointers->ExceptionRecord == nullptr)
			return EXCEPTION_CONTINUE_SEARCH;
		if (gSuppressionTls != TLS_OUT_OF_INDEXES && ::TlsGetValue(gSuppressionTls) != nullptr)
			return EXCEPTION_CONTINUE_SEARCH;

		const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
		const DWORD code = record->ExceptionCode;
		const ULONG_PTR instruction = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
		if (!IsFatalExceptionCode(code) || instruction < gModuleStart || instruction >= gModuleEnd)
			return EXCEPTION_CONTINUE_SEARCH;
		if (::InterlockedCompareExchange(&gReportStarted, 1, 0) != 0)
			return EXCEPTION_CONTINUE_SEARCH;
		if (gReportDirectory[0] == L'\0')
			return EXCEPTION_CONTINUE_SEARCH;

		SYSTEMTIME utc{};
		::GetSystemTime(&utc);
		const DWORD processId = ::GetCurrentProcessId();
		const DWORD threadId = ::GetCurrentThreadId();

		std::array<wchar_t, 160> baseName{};
		_snwprintf_s(
			baseName.data(),
			baseName.size(),
			_TRUNCATE,
			L"vSMR-crash-%04u%02u%02u-%02u%02u%02u-%03u-P%lu-T%lu",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			utc.wMilliseconds,
			static_cast<unsigned long>(processId),
			static_cast<unsigned long>(threadId));

		std::array<wchar_t, 1400> dumpPath{};
		std::array<wchar_t, 1400> textPath{};
		_snwprintf_s(
			dumpPath.data(),
			dumpPath.size(),
			_TRUNCATE,
			L"%ls\\%ls.dmp",
			gReportDirectory.data(),
			baseName.data());
		_snwprintf_s(
			textPath.data(),
			textPath.size(),
			_TRUNCATE,
			L"%ls\\%ls.txt",
			gReportDirectory.data(),
			baseName.data());

		bool dumpWritten = false;
		DWORD dumpError = ERROR_PROC_NOT_FOUND;
		if (gMiniDumpWriteDump != nullptr)
		{
			const HANDLE dumpFile = ::CreateFileW(
				dumpPath.data(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
				nullptr);
			if (dumpFile != INVALID_HANDLE_VALUE)
			{
				MINIDUMP_EXCEPTION_INFORMATION exceptionInformation{};
				exceptionInformation.ThreadId = threadId;
				exceptionInformation.ExceptionPointers = exceptionPointers;
				exceptionInformation.ClientPointers = FALSE;
				const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
					MiniDumpNormal |
					MiniDumpWithThreadInfo |
					MiniDumpWithUnloadedModules);
				dumpWritten = gMiniDumpWriteDump(
					::GetCurrentProcess(),
					processId,
					dumpFile,
					dumpType,
					&exceptionInformation,
					nullptr,
					nullptr) != FALSE;
				dumpError = dumpWritten ? ERROR_SUCCESS : ::GetLastError();
				::FlushFileBuffers(dumpFile);
				::CloseHandle(dumpFile);
				if (!dumpWritten)
					::DeleteFileW(dumpPath.data());
			}
			else
			{
				dumpError = ::GetLastError();
			}
		}

		std::array<char, 2300> report{};
		const ULONG_PTR moduleOffset = instruction - gModuleStart;
		const bool accessViolation =
			code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR;
		const ULONG_PTR accessKind =
			accessViolation && record->NumberParameters >= 1 ? record->ExceptionInformation[0] : 0;
		const ULONG_PTR accessAddress =
			accessViolation && record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;
		const int reportLength = _snprintf_s(
			report.data(),
			report.size(),
			_TRUNCATE,
			"vSMR fatal exception report\r\n"
			"version=%s\r\n"
			"utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\r\n"
			"process_id=%lu\r\n"
			"thread_id=%lu\r\n"
			"exception_code=0x%08lX\r\n"
			"exception_flags=0x%08lX\r\n"
			"exception_address=0x%p\r\n"
			"vsmr_module_base=0x%p\r\n"
			"vsmr_module_offset=0x%llX\r\n"
			"access_kind=%llu\r\n"
			"access_address=0x%llX\r\n"
			"minidump=%s\r\n"
			"minidump_error=%lu\r\n"
			"exception_disposition=continued_to_euroscope\r\n"
			"privacy=Minidumps can contain operational or sensitive process memory; review before sharing.\r\n",
			gVersion.data(),
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			utc.wMilliseconds,
			static_cast<unsigned long>(processId),
			static_cast<unsigned long>(threadId),
			static_cast<unsigned long>(code),
			static_cast<unsigned long>(record->ExceptionFlags),
			record->ExceptionAddress,
			reinterpret_cast<void*>(gModuleStart),
			static_cast<unsigned long long>(moduleOffset),
			static_cast<unsigned long long>(accessKind),
			static_cast<unsigned long long>(accessAddress),
			dumpWritten ? "written" : "failed",
			static_cast<unsigned long>(dumpError));

		const HANDLE textFile = ::CreateFileW(
			textPath.data(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (textFile != INVALID_HANDLE_VALUE)
		{
			DWORD bytesWritten = 0;
			const DWORD bytesToWrite = reportLength > 0
				? static_cast<DWORD>(reportLength)
				: 0;
			if (bytesToWrite > 0)
				::WriteFile(textFile, report.data(), bytesToWrite, &bytesWritten, nullptr);
			::FlushFileBuffers(textFile);
			::CloseHandle(textFile);
		}

		return EXCEPTION_CONTINUE_SEARCH;
	}
}

namespace VsmrCrashReporter
{
	bool Install(const char* version)
	{
		if (gHandler.load(std::memory_order_acquire) != nullptr)
			return true;

		const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
		if (dosHeader == nullptr || dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const BYTE*>(module) + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->OptionalHeader.SizeOfImage == 0)
			return false;

		const std::wstring reportDirectory = ResolveReportDirectory(module);
		if (reportDirectory.empty() || reportDirectory.size() >= gReportDirectory.size())
			return false;
		wcsncpy_s(
			gReportDirectory.data(),
			gReportDirectory.size(),
			reportDirectory.c_str(),
			_TRUNCATE);
		strncpy_s(
			gVersion.data(),
			gVersion.size(),
			version != nullptr ? version : "unknown",
			_TRUNCATE);

		gModuleStart = reinterpret_cast<ULONG_PTR>(module);
		gModuleEnd = gModuleStart + ntHeaders->OptionalHeader.SizeOfImage;
		gReportStarted = 0;
		gSuppressionTls = ::TlsAlloc();
		LoadDbgHelp();

		void* handler = ::AddVectoredExceptionHandler(0, HandleVectoredException);
		if (handler == nullptr)
		{
			if (gSuppressionTls != TLS_OUT_OF_INDEXES)
			{
				::TlsFree(gSuppressionTls);
				gSuppressionTls = TLS_OUT_OF_INDEXES;
			}
			if (gDbgHelpModule != nullptr)
			{
				::FreeLibrary(gDbgHelpModule);
				gDbgHelpModule = nullptr;
				gMiniDumpWriteDump = nullptr;
			}
			gReportDirectory[0] = L'\0';
			return false;
		}

		gHandler.store(handler, std::memory_order_release);
		return true;
	}

	void Remove()
	{
		void* handler = gHandler.exchange(nullptr, std::memory_order_acq_rel);
		if (handler != nullptr)
			::RemoveVectoredExceptionHandler(handler);

		if (gSuppressionTls != TLS_OUT_OF_INDEXES)
		{
			::TlsFree(gSuppressionTls);
			gSuppressionTls = TLS_OUT_OF_INDEXES;
		}
		if (gDbgHelpModule != nullptr)
		{
			::FreeLibrary(gDbgHelpModule);
			gDbgHelpModule = nullptr;
			gMiniDumpWriteDump = nullptr;
		}
		gModuleStart = 0;
		gModuleEnd = 0;
	}

	void SetCurrentThreadSuppressed(bool suppressed)
	{
		if (gSuppressionTls == TLS_OUT_OF_INDEXES)
			return;
		::TlsSetValue(gSuppressionTls, suppressed ? reinterpret_cast<void*>(1) : nullptr);
	}

	bool IsInstalled()
	{
		return gHandler.load(std::memory_order_acquire) != nullptr;
	}

	std::string GetReportDirectory()
	{
		if (gReportDirectory[0] == L'\0')
			return {};

		const int required = ::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			gReportDirectory.data(),
			-1,
			nullptr,
			0,
			nullptr,
			nullptr);
		if (required <= 1)
			return {};
		std::string utf8(static_cast<size_t>(required), '\0');
		if (::WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			gReportDirectory.data(),
			-1,
			utf8.data(),
			required,
			nullptr,
			nullptr) <= 0)
		{
			return {};
		}
		utf8.resize(static_cast<size_t>(required - 1));
		return utf8;
	}
}
