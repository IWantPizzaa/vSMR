#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <werapi.h>

#include "CrashReportProtocol.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

static_assert(sizeof(void*) == 4U,
	"vSMRCrashHandler must be built for the same x86 architecture as EuroScope.");

namespace
{
	using VsmrCrashProtocol::BreadcrumbRecord;
	using VsmrCrashProtocol::CallbackRecord;
	using VsmrCrashProtocol::LogRecord;
	using VsmrCrashProtocol::RadarStateRecord;
	using VsmrCrashProtocol::SharedState;
	using VsmrCrashProtocol::StateRecord;
	using VsmrCrashProtocol::ThreadRoleRecord;

	enum class Association
	{
		None,
		DirectInstruction,
		StackFrame,
		WorkerThread
	};

	enum class StackWalkStatus
	{
		Unavailable,
		InitializationFailed,
		WalkFailed,
		Complete,
		Partial,
		Truncated
	};

	struct StackWalkResult
	{
		std::vector<std::uint64_t> frames;
		StackWalkStatus status = StackWalkStatus::Unavailable;
	};

	struct ScopedHandle
	{
		HANDLE value = INVALID_HANDLE_VALUE;

		~ScopedHandle()
		{
			if (value != nullptr && value != INVALID_HANDLE_VALUE)
				::CloseHandle(value);
		}

		ScopedHandle() = default;
		ScopedHandle(const ScopedHandle&) = delete;
		ScopedHandle& operator=(const ScopedHandle&) = delete;
	};

	SRWLOCK gCallbackLock = SRWLOCK_INIT;

	struct DbgHelpApi
	{
		using MiniDumpWriteDumpFn = decltype(&::MiniDumpWriteDump);
		using StackWalk64Fn = decltype(&::StackWalk64);
		using SymInitializeWFn = decltype(&::SymInitializeW);
		using SymCleanupFn = decltype(&::SymCleanup);
		using SymFunctionTableAccess64Fn = decltype(&::SymFunctionTableAccess64);
		using SymGetModuleBase64Fn = decltype(&::SymGetModuleBase64);

		HMODULE module = nullptr;
		MiniDumpWriteDumpFn miniDumpWriteDump = nullptr;
		StackWalk64Fn stackWalk64 = nullptr;
		SymInitializeWFn symInitializeW = nullptr;
		SymCleanupFn symCleanup = nullptr;
		SymFunctionTableAccess64Fn symFunctionTableAccess64 = nullptr;
		SymGetModuleBase64Fn symGetModuleBase64 = nullptr;

		~DbgHelpApi()
		{
			if (module != nullptr)
				::FreeLibrary(module);
		}

		bool Load()
		{
#if defined(VSMR_CRASH_HARNESS_NO_DBGHELP)
			// Compile-time-only harness seam. The packaged helper is never built
			// with this definition and has no runtime failure-injection switch.
			return false;
#else
			std::array<wchar_t, MAX_PATH> systemDirectory{};
			const UINT length = ::GetSystemDirectoryW(
				systemDirectory.data(),
				static_cast<UINT>(systemDirectory.size()));
			if (length == 0 || length >= systemDirectory.size())
				return false;
			std::wstring path(systemDirectory.data(), length);
			path += L"\\dbghelp.dll";
			module = ::LoadLibraryW(path.c_str());
			if (module == nullptr)
				return false;

			miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
				::GetProcAddress(module, "MiniDumpWriteDump"));
			stackWalk64 = reinterpret_cast<StackWalk64Fn>(
				::GetProcAddress(module, "StackWalk64"));
			symInitializeW = reinterpret_cast<SymInitializeWFn>(
				::GetProcAddress(module, "SymInitializeW"));
			symCleanup = reinterpret_cast<SymCleanupFn>(
				::GetProcAddress(module, "SymCleanup"));
			symFunctionTableAccess64 = reinterpret_cast<SymFunctionTableAccess64Fn>(
				::GetProcAddress(module, "SymFunctionTableAccess64"));
			symGetModuleBase64 = reinterpret_cast<SymGetModuleBase64Fn>(
				::GetProcAddress(module, "SymGetModuleBase64"));
			return miniDumpWriteDump != nullptr;
#endif
		}

		bool CanWalkStack() const
		{
			return stackWalk64 != nullptr && symInitializeW != nullptr &&
				symCleanup != nullptr && symFunctionTableAccess64 != nullptr &&
				symGetModuleBase64 != nullptr;
		}
	};

	bool HasTerminator(const char* value, std::size_t capacity)
	{
		return value != nullptr && std::memchr(value, '\0', capacity) != nullptr;
	}

	bool HasTerminator(const wchar_t* value, std::size_t capacity)
	{
		if (value == nullptr)
			return false;
		for (std::size_t index = 0; index < capacity; ++index)
		{
			if (value[index] == L'\0')
				return true;
		}
		return false;
	}

	bool IsValidState(const SharedState& state)
	{
		if (state.magic != VsmrCrashProtocol::kMagic ||
			state.schemaVersion != VsmrCrashProtocol::kSchemaVersion ||
			state.structureSize != sizeof(SharedState) || state.ready == 0 ||
			state.moduleBase == 0 || state.moduleSize == 0 ||
			state.moduleSize > 1024U * 1024U * 1024U)
			return false;
		if (state.moduleBase + state.moduleSize < state.moduleBase)
			return false;
		return HasTerminator(state.reportDirectory, VsmrCrashProtocol::kPathChars) &&
			HasTerminator(state.modulePath, VsmrCrashProtocol::kPathChars) &&
			state.reportDirectory[0] != L'\0';
	}

	bool ReadRemoteStateOnce(
		HANDLE process,
		const void* remoteAddress,
		SharedState& state)
	{
		if (process == nullptr || remoteAddress == nullptr)
			return false;
		SIZE_T bytesRead = 0;
		if (::ReadProcessMemory(
			process,
			remoteAddress,
			&state,
			sizeof(state),
			&bytesRead) == FALSE || bytesRead != sizeof(state))
			return false;
		return IsValidState(state);
	}

	template <typename Record, std::size_t Count>
	void KeepOnlyStableRecords(
		const Record(&before)[Count],
		Record(&after)[Count])
	{
		for (std::size_t index = 0; index < Count; ++index)
		{
			if (before[index].commit == 0 ||
				before[index].commit != after[index].commit ||
				std::memcmp(&before[index], &after[index], sizeof(Record)) != 0)
			{
				after[index].commit = 0;
			}
		}
	}

	bool ReadRemoteState(
		HANDLE process,
		const void* remoteAddress,
		SharedState& state)
	{
		auto before = std::unique_ptr<SharedState>(new (std::nothrow) SharedState{});
		if (!before || !ReadRemoteStateOnce(process, remoteAddress, *before) ||
			!ReadRemoteStateOnce(process, remoteAddress, state))
			return false;
		if (before->moduleBase != state.moduleBase ||
			before->moduleSize != state.moduleSize ||
			std::wcscmp(before->reportDirectory, state.reportDirectory) != 0 ||
			std::wcscmp(before->modulePath, state.modulePath) != 0)
			return false;

		KeepOnlyStableRecords(before->states, state.states);
		KeepOnlyStableRecords(before->breadcrumbs, state.breadcrumbs);
		KeepOnlyStableRecords(before->logs, state.logs);
		KeepOnlyStableRecords(before->threadRoles, state.threadRoles);
		KeepOnlyStableRecords(before->callbacks, state.callbacks);
		for (std::size_t index = 0; index < VsmrCrashProtocol::kRadarStateCount; ++index)
		{
			const RadarStateRecord& previous = before->radarStates[index];
			RadarStateRecord& current = state.radarStates[index];
			if (previous.active != 1 || current.active != 1 || previous.commit == 0 ||
				previous.commit != current.commit ||
				std::memcmp(&previous, &current, sizeof(current)) != 0)
			{
				current.active = 0;
				current.commit = 0;
			}
		}
		return true;
	}

	bool IsInVsmr(const SharedState& state, std::uint64_t address)
	{
		return address >= state.moduleBase &&
			address < state.moduleBase + state.moduleSize;
	}

	StackWalkResult WalkStack(
		DbgHelpApi& api,
		const WER_RUNTIME_EXCEPTION_INFORMATION& exceptionInformation)
	{
		StackWalkResult result;
		if (!api.CanWalkStack() || exceptionInformation.hProcess == nullptr ||
			exceptionInformation.hThread == nullptr)
			return result;

		CONTEXT context = exceptionInformation.context;
		STACKFRAME64 frame{};
		DWORD machine = 0;
#if defined(_M_IX86)
		machine = IMAGE_FILE_MACHINE_I386;
		frame.AddrPC.Offset = context.Eip;
		frame.AddrFrame.Offset = context.Ebp;
		frame.AddrStack.Offset = context.Esp;
#elif defined(_M_X64)
		machine = IMAGE_FILE_MACHINE_AMD64;
		frame.AddrPC.Offset = context.Rip;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrStack.Offset = context.Rsp;
#else
		return result;
#endif
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Mode = AddrModeFlat;
		if (frame.AddrPC.Offset != 0)
			result.frames.push_back(frame.AddrPC.Offset);

		if (api.symInitializeW(exceptionInformation.hProcess, nullptr, TRUE) == FALSE)
		{
			result.status = StackWalkStatus::InitializationFailed;
			return result;
		}
		for (std::size_t index = 0; index < 127U; ++index)
		{
			const DWORD64 previous = frame.AddrPC.Offset;
			const BOOL walked = api.stackWalk64(
				machine,
				exceptionInformation.hProcess,
				exceptionInformation.hThread,
				&frame,
				&context,
				nullptr,
				api.symFunctionTableAccess64,
				api.symGetModuleBase64,
				nullptr);
			if (walked == FALSE || frame.AddrPC.Offset == previous)
			{
				result.status = result.frames.size() > 1U
					? StackWalkStatus::Partial
					: StackWalkStatus::WalkFailed;
				break;
			}
			if (frame.AddrPC.Offset == 0)
			{
				result.status = StackWalkStatus::Complete;
				break;
			}
			result.frames.push_back(frame.AddrPC.Offset);
			if (index == 126U)
				result.status = StackWalkStatus::Truncated;
		}
		api.symCleanup(exceptionInformation.hProcess);
		return result;
	}

	const ThreadRoleRecord* LatestThreadRole(const SharedState& state, DWORD threadId)
	{
		const ThreadRoleRecord* latest = nullptr;
		std::uint32_t latestCommit = 0;
		for (const auto& record : state.threadRoles)
		{
			const std::uint32_t commit = static_cast<std::uint32_t>(record.commit);
			if (commit == 0 || record.threadId != threadId ||
				!HasTerminator(record.role, sizeof(record.role)))
				continue;
			if (latest == nullptr || commit > latestCommit)
			{
				latest = &record;
				latestCommit = commit;
			}
		}
		return latest;
	}

	const CallbackRecord* LatestCallback(const SharedState& state, DWORD threadId)
	{
		const CallbackRecord* latest = nullptr;
		std::uint32_t latestCommit = 0;
		for (const auto& record : state.callbacks)
		{
			const std::uint32_t commit = static_cast<std::uint32_t>(record.commit);
			if (commit == 0 || record.threadId != threadId ||
				!HasTerminator(record.callback, sizeof(record.callback)))
				continue;
			if (latest == nullptr || commit > latestCommit)
			{
				latest = &record;
				latestCommit = commit;
			}
		}
		return latest;
	}

	bool IsWorkerRole(const ThreadRoleRecord* role)
	{
		if (role == nullptr || role->role[0] == '\0')
			return false;
		if (std::strcmp(role->role, "inactive") == 0 ||
			std::strcmp(role->role, "euroscope callback") == 0)
			return false;
		// All roles installed by OwnedThreadRole contain this marker. Do not
		// treat arbitrary host/UI labels as evidence that vSMR owned a thread.
		return std::strstr(role->role, "worker") != nullptr;
	}

	std::string OneLine(const char* value, std::size_t capacity)
	{
		if (!HasTerminator(value, capacity))
			return "<invalid>";
		std::string result(value);
		for (char& character : result)
		{
			const unsigned char code = static_cast<unsigned char>(character);
			if (character == '\r' || character == '\n' || character == '=' || code < 0x20U)
				character = ' ';
		}
		return result;
	}

	std::string WideToUtf8(const wchar_t* value, std::size_t capacity)
	{
		if (!HasTerminator(value, capacity))
			return "<invalid>";
		const int length = static_cast<int>(std::wcslen(value));
		if (length == 0)
			return {};
		const int required = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
		if (required <= 0)
			return "<unicode-conversion-failed>";
		std::string result(static_cast<std::size_t>(required), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
			result.data(), required, nullptr, nullptr) <= 0)
			return "<unicode-conversion-failed>";
		return OneLine(result.c_str(), result.size() + 1U);
	}

	void AppendFormat(std::string& destination, const char* format, ...)
	{
		std::array<char, 2048> buffer{};
		va_list arguments;
		va_start(arguments, format);
		const int length = _vsnprintf_s(
			buffer.data(), buffer.size(), _TRUNCATE, format, arguments);
		va_end(arguments);
		if (length > 0)
			destination.append(buffer.data(), static_cast<std::size_t>(length));
	}

	std::string FormatFileTime(std::uint64_t raw)
	{
		ULARGE_INTEGER value{};
		value.QuadPart = raw;
		FILETIME fileTime{};
		fileTime.dwLowDateTime = value.LowPart;
		fileTime.dwHighDateTime = value.HighPart;
		SYSTEMTIME utc{};
		if (::FileTimeToSystemTime(&fileTime, &utc) == FALSE)
			return "unavailable";
		std::array<char, 40> text{};
		_snprintf_s(text.data(), text.size(), _TRUNCATE,
			"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
			utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
			utc.wSecond, utc.wMilliseconds);
		return text.data();
	}

	template <typename Record>
	std::vector<const Record*> OrderedCommitted(
		const Record* records,
		std::size_t count)
	{
		std::vector<const Record*> ordered;
		ordered.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (records[index].commit != 0)
				ordered.push_back(&records[index]);
		}
		std::sort(ordered.begin(), ordered.end(), [](const Record* left, const Record* right)
			{
				return static_cast<std::uint32_t>(left->commit) <
					static_cast<std::uint32_t>(right->commit);
			});
		return ordered;
	}

	const char* AssociationName(Association association)
	{
		switch (association)
		{
		case Association::DirectInstruction:
			return "vsmr_instruction_pointer";
		case Association::StackFrame:
			return "vsmr_stack_frame";
		case Association::WorkerThread:
			return "vsmr_owned_worker_thread";
		default:
			return "none";
		}
	}

	const char* AttributionStrength(Association association)
	{
		return association == Association::DirectInstruction
			? "direct"
			: association == Association::None ? "none" : "associated-not-proof";
	}

	std::string BuildMinimalTextReport(
		const SharedState& state,
		const WER_RUNTIME_EXCEPTION_INFORMATION& exceptionInformation,
		Association earlyAssociation,
		DWORD processId,
		std::uint64_t processCreationTime,
		DWORD threadId,
		const SYSTEMTIME& utc)
	{
		std::string report;
		report.reserve(4096U);
		report += "vSMR fatal exception report\r\n";
		report += "report_format=2\r\n";
		AppendFormat(report, "utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\r\n",
			utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
			utc.wSecond, utc.wMilliseconds);
		AppendFormat(report,
			"process_id=%lu\r\nprocess_creation_filetime=0x%016llX\r\nthread_id=%lu\r\n",
			static_cast<unsigned long>(processId),
			static_cast<unsigned long long>(processCreationTime),
			static_cast<unsigned long>(threadId));
		AppendFormat(report, "exception_code=0x%08lX\r\nexception_flags=0x%08lX\r\n",
			static_cast<unsigned long>(exceptionInformation.exceptionRecord.ExceptionCode),
			static_cast<unsigned long>(exceptionInformation.exceptionRecord.ExceptionFlags));
		AppendFormat(report, "exception_address=0x%016llX\r\n",
			static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(
				exceptionInformation.exceptionRecord.ExceptionAddress)));
		AppendFormat(report, "vsmr_module_base=0x%016llX\r\nvsmr_module_size=%lu\r\n",
			static_cast<unsigned long long>(state.moduleBase),
			static_cast<unsigned long>(state.moduleSize));
		AppendFormat(report, "early_association=%s\r\nearly_attribution_strength=%s\r\n",
			AssociationName(earlyAssociation), AttributionStrength(earlyAssociation));
		report += "version=" + OneLine(state.pluginVersion, sizeof(state.pluginVersion)) + "\r\n";
		report += "git_commit=" + OneLine(state.gitCommit, sizeof(state.gitCommit)) + "\r\n";
		report += "source_state=" + OneLine(state.sourceState, sizeof(state.sourceState)) + "\r\n";
		report += "built_utc=" + OneLine(state.buildTimestampUtc, sizeof(state.buildTimestampUtc)) + "\r\n";
		report += "dll_sha256=" + OneLine(state.dllSha256, sizeof(state.dllSha256)) + "\r\n";
		report += "pdb_identity=" + OneLine(state.pdbIdentity, sizeof(state.pdbIdentity)) + "\r\n";
		report += "euroscope_version=" + OneLine(state.euroScopeVersion, sizeof(state.euroScopeVersion)) + "\r\n";
		report += "vsmr_module=" + WideToUtf8(state.modulePath, VsmrCrashProtocol::kPathChars) + "\r\n";
		if (exceptionInformation.pwszReportId != nullptr)
		{
			// The report id belongs to the helper process, unlike pContext, and is
			// documented by WER as a local constant string.
			report += "wer_report_id=" + WideToUtf8(exceptionInformation.pwszReportId, 1024U) + "\r\n";
		}

		if ((exceptionInformation.exceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
			exceptionInformation.exceptionRecord.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
			exceptionInformation.exceptionRecord.NumberParameters >= 2)
		{
			AppendFormat(report, "access_kind=%llu\r\naccess_address=0x%016llX\r\n",
				static_cast<unsigned long long>(exceptionInformation.exceptionRecord.ExceptionInformation[0]),
				static_cast<unsigned long long>(exceptionInformation.exceptionRecord.ExceptionInformation[1]));
		}
		report += "details_status=pending\r\n";
		report += "dump_status=pending\r\n";
		report += "privacy=Minidumps can contain operational or sensitive stack memory; review before sharing.\r\n";
		report += "exception_disposition=left_to_euroscope_and_windows\r\n";
		return report;
	}

	std::string BuildTextReportDetails(
		const SharedState& state,
		Association association,
		const std::vector<std::uint64_t>& stackFrames,
		StackWalkStatus stackWalkStatus,
		const ThreadRoleRecord* threadRole,
		const CallbackRecord* callback)
	{
		std::string report;
		report.reserve(44U * 1024U);
		report += "details_begin\r\n";
		AppendFormat(report, "association=%s\r\nattribution_strength=%s\r\n",
			AssociationName(association), AttributionStrength(association));
		report += "attribution_note=A stack frame or worker context shows association with vSMR, not proof that vSMR caused the fault.\r\n";

		report += "thread_role=";
		report += threadRole != nullptr
			? OneLine(threadRole->role, sizeof(threadRole->role))
			: "unavailable";
		report += "\r\nlast_callback=";
		report += callback != nullptr
			? OneLine(callback->callback, sizeof(callback->callback))
			: "unavailable";
		report += "\r\n";
		if (callback != nullptr)
			AppendFormat(report, "last_callback_screen=0x%016llX\r\n",
				static_cast<unsigned long long>(callback->screenToken));

		const auto stateRecords = OrderedCommitted(state.states, VsmrCrashProtocol::kStateRecordCount);
		std::map<std::string, const StateRecord*> latestState;
		for (const StateRecord* record : stateRecords)
		{
			if (!HasTerminator(record->key, sizeof(record->key)) ||
				!HasTerminator(record->value, sizeof(record->value)))
				continue;
			latestState[OneLine(record->key, sizeof(record->key))] = record;
		}
		for (const auto& entry : latestState)
			report += "state." + entry.first + "=" +
				OneLine(entry.second->value, sizeof(entry.second->value)) + "\r\n";

		report += "screen_states_begin\r\n";
		for (const RadarStateRecord& screen : state.radarStates)
		{
			if (screen.active != 1 || screen.commit == 0 ||
				!HasTerminator(screen.airport, sizeof(screen.airport)) ||
				!HasTerminator(screen.profile, sizeof(screen.profile)) ||
				!HasTerminator(screen.radar, sizeof(screen.radar)) ||
				!HasTerminator(screen.inset, sizeof(screen.inset)))
				continue;
			AppendFormat(report, "screen=0x%016llX correlated=%s thread=%lu utc=%s airport=%s profile=%s radar=%s inset=%s\r\n",
				static_cast<unsigned long long>(screen.screenToken),
				callback != nullptr && callback->screenToken == screen.screenToken ? "yes" : "no",
				static_cast<unsigned long>(screen.threadId),
				FormatFileTime(screen.utcFileTime).c_str(),
				OneLine(screen.airport, sizeof(screen.airport)).c_str(),
				OneLine(screen.profile, sizeof(screen.profile)).c_str(),
				OneLine(screen.radar, sizeof(screen.radar)).c_str(),
				OneLine(screen.inset, sizeof(screen.inset)).c_str());
		}
		report += "screen_states_end\r\n";

		report += "stack_begin\r\n";
		const char* stackStatus = "unavailable";
		const char* stackComplete = "no";
		switch (stackWalkStatus)
		{
		case StackWalkStatus::InitializationFailed:
			stackStatus = "initialization_failed";
			break;
		case StackWalkStatus::WalkFailed:
			stackStatus = "walk_failed";
			break;
		case StackWalkStatus::Complete:
			stackStatus = "complete";
			stackComplete = "yes";
			break;
		case StackWalkStatus::Partial:
			stackStatus = "partial";
			break;
		case StackWalkStatus::Truncated:
			stackStatus = "truncated_at_128_frames";
			break;
		default:
			break;
		}
		AppendFormat(report, "stack_walk_status=%s\r\nstack_walk_complete=%s\r\n",
			stackStatus, stackComplete);
		for (std::size_t index = 0; index < stackFrames.size(); ++index)
		{
			AppendFormat(report, "frame.%03zu=0x%016llX%s\r\n", index,
				static_cast<unsigned long long>(stackFrames[index]),
				IsInVsmr(state, stackFrames[index]) ? " vSMR" : "");
		}
		report += "stack_end\r\n";

		const auto breadcrumbs = OrderedCommitted(
			state.breadcrumbs, VsmrCrashProtocol::kBreadcrumbCount);
		report += "breadcrumbs_begin\r\n";
		for (const BreadcrumbRecord* record : breadcrumbs)
		{
			if (!HasTerminator(record->category, sizeof(record->category)) ||
				!HasTerminator(record->value, sizeof(record->value)))
				continue;
			AppendFormat(report, "%s thread=%lu category=%s value=%s\r\n",
				FormatFileTime(record->utcFileTime).c_str(),
				static_cast<unsigned long>(record->threadId),
				OneLine(record->category, sizeof(record->category)).c_str(),
				OneLine(record->value, sizeof(record->value)).c_str());
		}
		report += "breadcrumbs_end\r\n";

		const auto logs = OrderedCommitted(state.logs, VsmrCrashProtocol::kLogRecordCount);
		report += "recent_log_begin\r\n";
		for (const LogRecord* record : logs)
		{
			if (!HasTerminator(record->message, sizeof(record->message)))
				continue;
			AppendFormat(report, "%s thread=%lu message=%s\r\n",
				FormatFileTime(record->utcFileTime).c_str(),
				static_cast<unsigned long>(record->threadId),
				OneLine(record->message, sizeof(record->message)).c_str());
		}
		report += "recent_log_end\r\n";
		report += "details_status=complete\r\n";
		report += "details_end\r\n";
		return report;
	}

	bool WriteAll(HANDLE file, const std::string& contents)
	{
		std::size_t offset = 0;
		while (offset < contents.size())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
				contents.size() - offset, 1024U * 1024U));
			DWORD written = 0;
			if (::WriteFile(file, contents.data() + offset, chunk, &written, nullptr) == FALSE ||
				written != chunk)
				return false;
			offset += written;
		}
		return true;
	}

	std::wstring JoinPath(const wchar_t* directory, const std::wstring& filename)
	{
		std::wstring result(directory);
		if (!result.empty() && result.back() != L'\\' && result.back() != L'/')
			result.push_back(L'\\');
		result += filename;
		return result;
	}

	std::wstring ReportBaseName(DWORD processId, std::uint64_t processCreationTime)
	{
		std::array<wchar_t, 160> baseName{};
		_snwprintf_s(baseName.data(), baseName.size(), _TRUNCATE,
			L"vSMR-crash-P%lu-C%016llX",
			static_cast<unsigned long>(processId),
			static_cast<unsigned long long>(processCreationTime));
		return baseName.data();
	}

	bool CreateTextReport(
		const SharedState& state,
		DWORD processId,
		std::uint64_t processCreationTime,
		const std::string& report,
		HANDLE& textFile,
		std::wstring& dumpPath,
		std::wstring& temporaryDumpPath)
	{
		const std::wstring baseName = ReportBaseName(processId, processCreationTime);
		const std::wstring textPath = JoinPath(
			state.reportDirectory, baseName + L".txt");
		// The deterministic CREATE_NEW text file is also the cross-process
		// claim. Concurrent callbacks for one target process cannot create a
		// second report, while creation time prevents stale PID reuse collisions.
		textFile = ::CreateFileW(textPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (textFile == INVALID_HANDLE_VALUE)
			return false;
		if (!WriteAll(textFile, report) || ::FlushFileBuffers(textFile) == FALSE)
		{
			::CloseHandle(textFile);
			textFile = INVALID_HANDLE_VALUE;
			return false;
		}
		dumpPath = JoinPath(state.reportDirectory, baseName + L".dmp");
		temporaryDumpPath = dumpPath + L".tmp";
		return true;
	}

	bool AppendTextReport(HANDLE textFile, const std::string& report)
	{
		LARGE_INTEGER end{};
		if (textFile == INVALID_HANDLE_VALUE ||
			::SetFilePointerEx(textFile, end, nullptr, FILE_END) == FALSE)
			return false;
		return WriteAll(textFile, report) && ::FlushFileBuffers(textFile) != FALSE;
	}

	bool WriteDumpAttempt(
		DbgHelpApi& api,
		const WER_RUNTIME_EXCEPTION_INFORMATION& exceptionInformation,
		const std::wstring& temporaryPath,
		MINIDUMP_TYPE dumpType,
		DWORD processId,
		DWORD threadId,
		DWORD& error)
	{
		::DeleteFileW(temporaryPath.c_str());
		const HANDLE dumpFile = ::CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0,
			nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (dumpFile == INVALID_HANDLE_VALUE)
		{
			error = ::GetLastError();
			return false;
		}

		EXCEPTION_RECORD exceptionRecord = exceptionInformation.exceptionRecord;
		exceptionRecord.ExceptionRecord = nullptr;
		CONTEXT context = exceptionInformation.context;
		EXCEPTION_POINTERS pointers{};
		pointers.ExceptionRecord = &exceptionRecord;
		pointers.ContextRecord = &context;
		MINIDUMP_EXCEPTION_INFORMATION dumpException{};
		dumpException.ThreadId = threadId;
		dumpException.ExceptionPointers = &pointers;
		dumpException.ClientPointers = FALSE;

		const bool written = api.miniDumpWriteDump(
			exceptionInformation.hProcess,
			processId,
			dumpFile,
			dumpType,
			&dumpException,
			nullptr,
			nullptr) != FALSE;
		error = written ? ERROR_SUCCESS : ::GetLastError();
		const bool flushed = ::FlushFileBuffers(dumpFile) != FALSE;
		const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
		::CloseHandle(dumpFile);
		if (!written || !flushed)
		{
			if (written && !flushed)
				error = flushError;
			::DeleteFileW(temporaryPath.c_str());
			return false;
		}
		return true;
	}

	void AppendDumpResult(
		HANDLE textFile,
		bool written,
		DWORD error,
		std::uint64_t bytes,
		bool usedNormalFallback)
	{
		std::string status;
		AppendFormat(status,
			"dump_status=%s\r\ndump_error=0x%08lX\r\ndump_bytes=%llu\r\ndump_normal_fallback=%s\r\n",
			written ? "written" : "failed",
			static_cast<unsigned long>(error),
			static_cast<unsigned long long>(bytes),
			usedNormalFallback ? "yes" : "no");
		AppendTextReport(textFile, status);
	}

	void GenerateCrashReport(
		void* contextAddress,
		const WER_RUNTIME_EXCEPTION_INFORMATION* exceptionInformation)
	{
		if (exceptionInformation == nullptr || exceptionInformation->dwSize < sizeof(*exceptionInformation) ||
			exceptionInformation->bIsFatal == FALSE || exceptionInformation->hProcess == nullptr ||
			exceptionInformation->hThread == nullptr)
			return;

		auto state = std::unique_ptr<SharedState>(new (std::nothrow) SharedState{});
		if (!state || !ReadRemoteState(exceptionInformation->hProcess, contextAddress, *state))
			return;

		const DWORD processId = ::GetProcessId(exceptionInformation->hProcess);
		const DWORD threadId = ::GetThreadId(exceptionInformation->hThread);
		if (processId == 0 || threadId == 0)
			return;
		FILETIME processCreation{}, processExit{}, processKernel{}, processUser{};
		if (::GetProcessTimes(exceptionInformation->hProcess, &processCreation,
			&processExit, &processKernel, &processUser) == FALSE)
			return;
		ULARGE_INTEGER processCreationValue{};
		processCreationValue.LowPart = processCreation.dwLowDateTime;
		processCreationValue.HighPart = processCreation.dwHighDateTime;
		const std::wstring existingTextPath = JoinPath(
			state->reportDirectory,
			ReportBaseName(processId, processCreationValue.QuadPart) + L".txt");
		if (::GetFileAttributesW(existingTextPath.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;

		const std::uint64_t exceptionAddress = reinterpret_cast<std::uintptr_t>(
			exceptionInformation->exceptionRecord.ExceptionAddress);
		Association association = IsInVsmr(*state, exceptionAddress)
			? Association::DirectInstruction
			: Association::None;
		const ThreadRoleRecord* const role = LatestThreadRole(*state, threadId);
		if (association == Association::None && IsWorkerRole(role))
			association = Association::WorkerThread;
		const CallbackRecord* const callback = LatestCallback(*state, threadId);
		SYSTEMTIME utc{};
		::GetSystemTime(&utc);

		ScopedHandle textFile;
		std::wstring dumpPath;
		std::wstring temporaryDumpPath;
		auto createMinimalReport = [&]()
		{
			const std::string minimalReport = BuildMinimalTextReport(
				*state,
				*exceptionInformation,
				association,
				processId,
				processCreationValue.QuadPart,
				threadId,
				utc);
			return CreateTextReport(*state, processId, processCreationValue.QuadPart,
				minimalReport, textFile.value, dumpPath, temporaryDumpPath);
		};

		// Direct instruction-pointer and registered worker-thread associations
		// are already known without DbgHelp. Claim the deterministic report and
		// durably preserve its minimal header before loading or calling DbgHelp.
		const bool associatedBeforeDbgHelp = association != Association::None;
		if (associatedBeforeDbgHelp && !createMinimalReport())
			return;

		DbgHelpApi dbgHelp;
		const bool dbgHelpLoaded = dbgHelp.Load();
		StackWalkResult stackWalk;
		if (dbgHelpLoaded)
		{
			stackWalk = WalkStack(dbgHelp, *exceptionInformation);
			if (association != Association::DirectInstruction &&
				std::any_of(stackWalk.frames.begin(), stackWalk.frames.end(),
					[&state](std::uint64_t address) { return IsInVsmr(*state, address); }))
				association = Association::StackFrame;
		}
		if (stackWalk.frames.empty() && exceptionAddress != 0)
		{
			stackWalk.frames.push_back(exceptionAddress);
		}
		if (association == Association::None)
			return;
		// Stack-only association cannot be known without walking. Once found,
		// preserve the same minimal header before formatting further detail or
		// attempting a dump.
		if (!associatedBeforeDbgHelp && !createMinimalReport())
			return;

		const std::string reportDetails = BuildTextReportDetails(
			*state,
			association,
			stackWalk.frames,
			stackWalk.status,
			role,
			callback);
		AppendTextReport(textFile.value, reportDetails);

		bool dumpWritten = false;
		bool usedNormalFallback = false;
		DWORD dumpError = ERROR_PROC_NOT_FOUND;
		if (dbgHelpLoaded && dbgHelp.miniDumpWriteDump != nullptr)
		{
			const MINIDUMP_TYPE preferredType = static_cast<MINIDUMP_TYPE>(
				MiniDumpNormal |
				MiniDumpWithThreadInfo |
				MiniDumpWithUnloadedModules |
				MiniDumpWithProcessThreadData);
			dumpWritten = WriteDumpAttempt(dbgHelp, *exceptionInformation,
				temporaryDumpPath, preferredType, processId, threadId, dumpError);
			if (!dumpWritten)
			{
				usedNormalFallback = true;
				dumpWritten = WriteDumpAttempt(dbgHelp, *exceptionInformation,
					temporaryDumpPath, MiniDumpNormal, processId, threadId, dumpError);
			}
		}

		std::uint64_t dumpBytes = 0;
		if (dumpWritten)
		{
			if (::MoveFileExW(temporaryDumpPath.c_str(), dumpPath.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
			{
				dumpError = ::GetLastError();
				dumpWritten = false;
				::DeleteFileW(temporaryDumpPath.c_str());
			}
			else
			{
				const HANDLE completedDump = ::CreateFileW(dumpPath.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if (completedDump != INVALID_HANDLE_VALUE)
				{
					LARGE_INTEGER size{};
					if (::GetFileSizeEx(completedDump, &size) != FALSE && size.QuadPart > 0)
						dumpBytes = static_cast<std::uint64_t>(size.QuadPart);
					::CloseHandle(completedDump);
				}
			}
		}
		AppendDumpResult(textFile.value, dumpWritten, dumpError, dumpBytes, usedNormalFallback);
	}

	void GenerateCrashReportCppGuard(
		void* contextAddress,
		const WER_RUNTIME_EXCEPTION_INFORMATION* exceptionInformation) noexcept
	{
		try
		{
			GenerateCrashReport(contextAddress, exceptionInformation);
		}
		catch (...)
		{
			// The callback is best effort. WER must remain free to continue its
			// native crash flow even if our own reporter cannot finish.
		}
	}

	void GenerateCrashReportSehGuard(
		void* contextAddress,
		const WER_RUNTIME_EXCEPTION_INFORMATION* exceptionInformation) noexcept
	{
		__try
		{
			GenerateCrashReportCppGuard(contextAddress, exceptionInformation);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
}

extern "C" HRESULT OutOfProcessExceptionEventCallback(
	PVOID context,
	const PWER_RUNTIME_EXCEPTION_INFORMATION exceptionInformation,
	PBOOL ownershipClaimed,
	PWSTR eventName,
	PDWORD eventNameSize,
	PDWORD signatureCount)
{
	if (ownershipClaimed != nullptr)
		*ownershipClaimed = FALSE;
	if (eventNameSize != nullptr)
	{
		if (eventName != nullptr && *eventNameSize > 0)
			eventName[0] = L'\0';
		*eventNameSize = 0;
	}
	if (signatureCount != nullptr)
		*signatureCount = 0;
	::AcquireSRWLockExclusive(&gCallbackLock);
	GenerateCrashReportSehGuard(context, exceptionInformation);
	::ReleaseSRWLockExclusive(&gCallbackLock);
	// Deliberately do not claim the event: EuroScope, other plug-ins, a
	// configured debugger, and native WER retain their normal behavior.
	return S_OK;
}

extern "C" HRESULT OutOfProcessExceptionEventSignatureCallback(
	PVOID context,
	const PWER_RUNTIME_EXCEPTION_INFORMATION exceptionInformation,
	DWORD index,
	PWSTR name,
	PDWORD nameSize,
	PWSTR value,
	PDWORD valueSize)
{
	(void)context;
	(void)exceptionInformation;
	(void)index;
	if (nameSize != nullptr)
	{
		if (name != nullptr && *nameSize > 0)
			name[0] = L'\0';
		*nameSize = 0;
	}
	if (valueSize != nullptr)
	{
		if (value != nullptr && *valueSize > 0)
			value[0] = L'\0';
		*valueSize = 0;
	}
	// EventCallback never claims ownership, so WER does not call this export.
	return S_OK;
}

extern "C" HRESULT OutOfProcessExceptionEventDebuggerLaunchCallback(
	PVOID context,
	const PWER_RUNTIME_EXCEPTION_INFORMATION exceptionInformation,
	PBOOL isCustomDebugger,
	PWSTR debuggerLaunch,
	PDWORD debuggerLaunchSize,
	PBOOL isDebuggerAutolaunch)
{
	(void)context;
	(void)exceptionInformation;
	if (debuggerLaunchSize != nullptr)
	{
		if (debuggerLaunch != nullptr && *debuggerLaunchSize > 0)
			debuggerLaunch[0] = L'\0';
		*debuggerLaunchSize = 0;
	}
	if (isCustomDebugger != nullptr)
		*isCustomDebugger = FALSE;
	if (isDebuggerAutolaunch != nullptr)
		*isDebuggerAutolaunch = FALSE;
	return S_OK;
}

static_assert(std::is_same_v<
	decltype(&OutOfProcessExceptionEventCallback),
	PFN_WER_RUNTIME_EXCEPTION_EVENT>,
	"The WER event callback must exactly match the Windows SDK ABI.");
static_assert(std::is_same_v<
	decltype(&OutOfProcessExceptionEventSignatureCallback),
	PFN_WER_RUNTIME_EXCEPTION_EVENT_SIGNATURE>,
	"The WER signature callback must exactly match the Windows SDK ABI.");
static_assert(std::is_same_v<
	decltype(&OutOfProcessExceptionEventDebuggerLaunchCallback),
	PFN_WER_RUNTIME_EXCEPTION_DEBUGGER_LAUNCH>,
	"The WER debugger callback must exactly match the Windows SDK ABI.");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		::DisableThreadLibraryCalls(instance);
	return TRUE;
}
