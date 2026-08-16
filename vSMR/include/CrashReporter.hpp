#pragma once

#include <string>

namespace VsmrCrashReporter
{
	// Installs a process-wide observer that only records fatal exceptions whose
	// instruction address belongs to vSMR.dll. The exception is never consumed.
	bool Install(const char* version);
	void Remove();
	bool IsInstalled();

	// Known SEH recovery regions can suppress first-chance reporting so a handled
	// exception is not mislabeled as a process crash.
	void SetCurrentThreadSuppressed(bool suppressed);

	std::string GetReportDirectory();
}
