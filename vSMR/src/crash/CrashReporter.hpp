#pragma once

#include <cstdint>
#include <string>

namespace VsmrCrashReporter
{
	// Registers vSMR's out-of-process Windows Error Reporting callback. No
	// exception handler or dump writer is installed inside EuroScope.
	bool Install(const char* version, const wchar_t* installRoot = nullptr);
	void Remove();
	bool IsInstalled();
	std::string GetReportDirectory();
	std::string GetRegistrationStatus();

	// These functions only update fixed-size, preallocated diagnostic records.
	// Values are truncated rather than allocated and are safe to call from hot
	// callback/worker paths during normal execution.
	void RecordState(const char* key, const char* value) noexcept;
	void RecordBreadcrumb(const char* category, const char* value) noexcept;
	void RecordThreadRole(const char* role) noexcept;
	void RecordLog(const char* message) noexcept;
	void RecordRadarState(
		std::uintptr_t screenToken,
		const char* airport,
		const char* profile,
		const char* radar,
		const char* inset) noexcept;
	void ClearRadarState(std::uintptr_t screenToken) noexcept;
	void RecordCallback(
		const char* callback,
		std::uintptr_t screenToken = 0) noexcept;
}
