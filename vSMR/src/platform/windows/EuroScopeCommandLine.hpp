#pragma once

#include <string>

namespace VsmrEuroScopeCommandLine
{
	enum class Owner
	{
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
	// Optional UI-thread progress pump, called every 50 ms only while pending.
	// The callback requests any host refresh itself and returns whether state
	// changed. Never called inline; no raw window invalidation is performed here.
	using ProgressCallback = bool (*)();
	bool Begin(Owner owner, const std::string& command, std::string* outError = nullptr,
		ProgressCallback progress = nullptr);
	SubmissionStatus Poll(Owner owner);
	bool IsBusy() noexcept;
	bool HasPending(Owner owner) noexcept;
	void Cancel(Owner owner) noexcept;
}
