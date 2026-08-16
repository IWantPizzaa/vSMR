#pragma once

// This header is shared by vSMR.dll, the out-of-process WER callback DLL, and
// the isolated crash harness. Keep it independent of MFC and the EuroScope SDK.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace VsmrCrashProtocol
{
	constexpr std::uint32_t kMagic = 0x52435356U; // "VSCR" on little-endian Windows.
	constexpr std::uint32_t kSchemaVersion = 1U;
	constexpr std::size_t kPathChars = 2048U;
	constexpr std::size_t kStateRecordCount = 32U;
	constexpr std::size_t kBreadcrumbCount = 64U;
	constexpr std::size_t kLogRecordCount = 64U;
	constexpr std::size_t kThreadRoleCount = 64U;
	constexpr std::size_t kRadarStateCount = 16U;
	constexpr std::size_t kCallbackCount = 64U;

	template <std::size_t Capacity>
	inline void CopyText(char(&destination)[Capacity], const char* source) noexcept
	{
		static_assert(Capacity > 0, "A fixed string needs room for its terminator.");
		std::size_t index = 0;
		if (source != nullptr)
		{
			for (; index + 1 < Capacity && source[index] != '\0'; ++index)
				destination[index] = source[index];
		}
		destination[index] = '\0';
		for (++index; index < Capacity; ++index)
			destination[index] = '\0';
	}

	template <std::size_t Capacity>
	inline void CopyText(wchar_t(&destination)[Capacity], const wchar_t* source) noexcept
	{
		static_assert(Capacity > 0, "A fixed string needs room for its terminator.");
		std::size_t index = 0;
		if (source != nullptr)
		{
			for (; index + 1 < Capacity && source[index] != L'\0'; ++index)
				destination[index] = source[index];
		}
		destination[index] = L'\0';
		for (++index; index < Capacity; ++index)
			destination[index] = L'\0';
	}

	inline std::uint64_t CurrentUtcFileTime() noexcept
	{
		FILETIME fileTime{};
		::GetSystemTimeAsFileTime(&fileTime);
		ULARGE_INTEGER value{};
		value.LowPart = fileTime.dwLowDateTime;
		value.HighPart = fileTime.dwHighDateTime;
		return value.QuadPart;
	}

	struct StateRecord
	{
		volatile LONG commit;
		DWORD threadId;
		std::uint64_t utcFileTime;
		char key[48];
		char value[192];
	};

	struct BreadcrumbRecord
	{
		volatile LONG commit;
		DWORD threadId;
		std::uint64_t utcFileTime;
		char category[48];
		char value[256];
	};

	struct LogRecord
	{
		volatile LONG commit;
		DWORD threadId;
		std::uint64_t utcFileTime;
		char message[320];
	};

	struct ThreadRoleRecord
	{
		volatile LONG commit;
		DWORD threadId;
		std::uint64_t utcFileTime;
		char role[80];
	};

	struct RadarStateRecord
	{
		// active: 0 = unused, -1 = being claimed, 1 = readable.
		volatile LONG active;
		volatile LONG commit;
		std::uint64_t screenToken;
		DWORD threadId;
		std::uint64_t utcFileTime;
		char airport[16];
		char profile[96];
		char radar[96];
		char inset[96];
	};

	struct CallbackRecord
	{
		volatile LONG commit;
		DWORD threadId;
		std::uint64_t utcFileTime;
		std::uint64_t screenToken;
		char callback[112];
	};

	// pContext passed to WerRegisterRuntimeExceptionModule is the address of one
	// of these records in the target process. The WER DLL must copy it with
	// ReadProcessMemory; it must never dereference pContext in its own process.
	struct SharedState
	{
		std::uint32_t magic;
		std::uint32_t schemaVersion;
		std::uint32_t structureSize;
		std::uint32_t reserved;
		volatile LONG ready;
		volatile LONG stateCounter;
		volatile LONG breadcrumbCounter;
		volatile LONG logCounter;
		volatile LONG threadRoleCounter;
		volatile LONG radarStateCounter;
		volatile LONG callbackCounter;
		std::uint32_t moduleSize;
		std::uint64_t moduleBase;
		wchar_t reportDirectory[kPathChars];
		wchar_t modulePath[kPathChars];
		char pluginVersion[64];
		char gitCommit[64];
		char dllSha256[65];
		char pdbIdentity[96];
		char euroScopeVersion[64];
		char buildTimestampUtc[40];
		char sourceState[24];
		StateRecord states[kStateRecordCount];
		BreadcrumbRecord breadcrumbs[kBreadcrumbCount];
		LogRecord logs[kLogRecordCount];
		ThreadRoleRecord threadRoles[kThreadRoleCount];
		RadarStateRecord radarStates[kRadarStateCount];
		CallbackRecord callbacks[kCallbackCount];
	};

	static_assert(std::is_standard_layout<SharedState>::value,
		"The remote crash context must remain a standard-layout POD.");
	static_assert(sizeof(SharedState) < 128U * 1024U,
		"The crash context should remain cheap to copy out of the target.");

	inline void InitializeSharedState(SharedState& state) noexcept
	{
		std::memset(&state, 0, sizeof(state));
		state.magic = kMagic;
		state.schemaVersion = kSchemaVersion;
		state.structureSize = static_cast<std::uint32_t>(sizeof(state));
	}

	inline void MarkReady(SharedState& state) noexcept
	{
		::MemoryBarrier();
		::InterlockedExchange(&state.ready, 1);
	}

	inline void PublishState(SharedState& state, const char* key, const char* value) noexcept
	{
		const LONG ticket = ::InterlockedIncrement(&state.stateCounter);
		StateRecord& record = state.states[
			(static_cast<std::uint32_t>(ticket) - 1U) % kStateRecordCount];
		::InterlockedExchange(&record.commit, 0);
		record.threadId = ::GetCurrentThreadId();
		record.utcFileTime = CurrentUtcFileTime();
		CopyText(record.key, key);
		CopyText(record.value, value);
		::MemoryBarrier();
		::InterlockedExchange(&record.commit, ticket);
	}

	inline void PublishBreadcrumb(
		SharedState& state,
		const char* category,
		const char* value) noexcept
	{
		const LONG ticket = ::InterlockedIncrement(&state.breadcrumbCounter);
		BreadcrumbRecord& record = state.breadcrumbs[
			(static_cast<std::uint32_t>(ticket) - 1U) % kBreadcrumbCount];
		::InterlockedExchange(&record.commit, 0);
		record.threadId = ::GetCurrentThreadId();
		record.utcFileTime = CurrentUtcFileTime();
		CopyText(record.category, category);
		CopyText(record.value, value);
		::MemoryBarrier();
		::InterlockedExchange(&record.commit, ticket);
	}

	inline void PublishLog(SharedState& state, const char* message) noexcept
	{
		const LONG ticket = ::InterlockedIncrement(&state.logCounter);
		LogRecord& record = state.logs[
			(static_cast<std::uint32_t>(ticket) - 1U) % kLogRecordCount];
		::InterlockedExchange(&record.commit, 0);
		record.threadId = ::GetCurrentThreadId();
		record.utcFileTime = CurrentUtcFileTime();
		CopyText(record.message, message);
		::MemoryBarrier();
		::InterlockedExchange(&record.commit, ticket);
	}

	inline void PublishThreadRole(SharedState& state, const char* role) noexcept
	{
		const LONG ticket = ::InterlockedIncrement(&state.threadRoleCounter);
		ThreadRoleRecord& record = state.threadRoles[
			(static_cast<std::uint32_t>(ticket) - 1U) % kThreadRoleCount];
		::InterlockedExchange(&record.commit, 0);
		record.threadId = ::GetCurrentThreadId();
		record.utcFileTime = CurrentUtcFileTime();
		CopyText(record.role, role);
		::MemoryBarrier();
		::InterlockedExchange(&record.commit, ticket);
	}

	inline RadarStateRecord* FindRadarState(
		SharedState& state,
		std::uintptr_t screenToken,
		bool claim) noexcept
	{
		if (screenToken == 0)
			return nullptr;

		const std::uint64_t token = static_cast<std::uint64_t>(screenToken);
		for (auto& record : state.radarStates)
		{
			if (record.active == 1 && record.screenToken == token)
				return &record;
		}
		if (!claim)
			return nullptr;

		for (auto& record : state.radarStates)
		{
			if (::InterlockedCompareExchange(&record.active, -1, 0) != 0)
				continue;
			record.screenToken = token;
			record.commit = 0;
			::MemoryBarrier();
			::InterlockedExchange(&record.active, 1);
			return &record;
		}
		return nullptr;
	}

	inline void PublishRadarState(
		SharedState& state,
		std::uintptr_t screenToken,
		const char* airport,
		const char* profile,
		const char* radar,
		const char* inset) noexcept
	{
		RadarStateRecord* const record = FindRadarState(state, screenToken, true);
		if (record == nullptr)
			return;

		const LONG ticket = ::InterlockedIncrement(&state.radarStateCounter);
		::InterlockedExchange(&record->commit, 0);
		record->threadId = ::GetCurrentThreadId();
		record->utcFileTime = CurrentUtcFileTime();
		CopyText(record->airport, airport);
		CopyText(record->profile, profile);
		CopyText(record->radar, radar);
		CopyText(record->inset, inset);
		::MemoryBarrier();
		::InterlockedExchange(&record->commit, ticket);
	}

	inline void ClearRadarState(SharedState& state, std::uintptr_t screenToken) noexcept
	{
		RadarStateRecord* const record = FindRadarState(state, screenToken, false);
		if (record == nullptr)
			return;
		::InterlockedExchange(&record->commit, 0);
		::MemoryBarrier();
		record->screenToken = 0;
		::InterlockedExchange(&record->active, 0);
	}

	inline void PublishCallback(
		SharedState& state,
		const char* callback,
		std::uintptr_t screenToken) noexcept
	{
		const LONG ticket = ::InterlockedIncrement(&state.callbackCounter);
		CallbackRecord& record = state.callbacks[
			(static_cast<std::uint32_t>(ticket) - 1U) % kCallbackCount];
		::InterlockedExchange(&record.commit, 0);
		record.threadId = ::GetCurrentThreadId();
		record.utcFileTime = CurrentUtcFileTime();
		record.screenToken = static_cast<std::uint64_t>(screenToken);
		CopyText(record.callback, callback);
		::MemoryBarrier();
		::InterlockedExchange(&record.commit, ticket);
	}
}
