#pragma once

#include <string>

namespace VsmrEuroScopeCommandLine
{
	enum class Owner
	{
		CdmReminder,
		Vsid
	};

	enum class SubmissionStatus
	{
		Idle,
		Pending,
		Confirmed,
		Ambiguous
	};

	// EuroScope does not expose command dispatch through its plug-in API. This
	// adapter targets only the empty command edit in the main bottom strip and
	// allows one pending submission at a time across all vSMR features.
	bool Begin(Owner owner, const std::string& command, std::string* outError = nullptr);
	SubmissionStatus Poll(Owner owner);
	bool IsBusy() noexcept;
	bool HasPending(Owner owner) noexcept;
	void Cancel(Owner owner) noexcept;
}
