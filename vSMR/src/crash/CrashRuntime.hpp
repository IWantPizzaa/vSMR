#pragma once

#include "crash/CrashReporter.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace VsmrCrashRuntime
{
	inline void RecordCurrentThreadRole(const char* role) noexcept
	{
		static thread_local char currentRole[80]{};
		const char* const normalized = role != nullptr ? role : "unknown";
		if (std::strncmp(currentRole, normalized, sizeof(currentRole)) == 0)
			return;

		std::size_t index = 0;
		for (; index + 1 < sizeof(currentRole) && normalized[index] != '\0'; ++index)
			currentRole[index] = normalized[index];
		currentRole[index] = '\0';
		for (++index; index < sizeof(currentRole); ++index)
			currentRole[index] = '\0';
		VsmrCrashReporter::RecordThreadRole(currentRole);
	}

	inline void RecordCurrentThreadCallback(
		const char* callback,
		std::uintptr_t screenToken = 0) noexcept
	{
		static thread_local char currentCallback[112]{};
		static thread_local std::uintptr_t currentScreenToken = 0;
		static thread_local std::uint32_t identicalCallbackCount = 0;
		const char* const normalized = callback != nullptr ? callback : "unknown";
		if (currentScreenToken == screenToken &&
			std::strncmp(currentCallback, normalized, sizeof(currentCallback)) == 0)
		{
			++identicalCallbackCount;
			if (identicalCallbackCount < 128U)
				return;
		}

		identicalCallbackCount = 0;
		std::size_t index = 0;
		for (; index + 1 < sizeof(currentCallback) && normalized[index] != '\0'; ++index)
			currentCallback[index] = normalized[index];
		currentCallback[index] = '\0';
		for (++index; index < sizeof(currentCallback); ++index)
			currentCallback[index] = '\0';
		currentScreenToken = screenToken;
		VsmrCrashReporter::RecordCallback(currentCallback, screenToken);
	}

	inline void RecordEuroScopeCallback(
		const char* callback,
		std::uintptr_t screenToken = 0) noexcept
	{
		RecordCurrentThreadRole("euroscope callback");
		RecordCurrentThreadCallback(callback, screenToken);
	}

	class OwnedThreadRole final
	{
	public:
		explicit OwnedThreadRole(const char* role) noexcept
		{
			ULONG stackGuarantee = 64U * 1024U;
			::SetThreadStackGuarantee(&stackGuarantee);
			RecordCurrentThreadRole(role);
		}

		~OwnedThreadRole()
		{
			RecordCurrentThreadRole("inactive");
		}

		OwnedThreadRole(const OwnedThreadRole&) = delete;
		OwnedThreadRole& operator=(const OwnedThreadRole&) = delete;
	};
}
