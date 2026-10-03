#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Session state owned by the plugin, never by a particular ASR/inset.
// Accessed from EuroScope's UI callbacks; the caller supplies monotonic time.
class TimerCountdownState
{
public:
	static constexpr int Count = 4;

	bool Start(int minutes, std::uint64_t now)
	{
		if (!Valid(minutes) || Running(minutes))
			return false;
		auto& timer = timers_[static_cast<std::size_t>(minutes - 1)];
		timer.deadline = now + static_cast<std::uint64_t>(minutes) * 60000ULL;
		timer.expired = false;
		return true;
	}

	bool Reset(int minutes)
	{
		if (!Valid(minutes) || (!Running(minutes) && !Expired(minutes)))
			return false;
		timers_[static_cast<std::size_t>(minutes - 1)] = {};
		return true;
	}

	// Consume expirations once, independent of how many views are open/visible.
	bool Update(std::uint64_t now)
	{
		bool alarmDue = false;
		for (auto& timer : timers_)
		{
			if (timer.deadline == 0 || now < timer.deadline)
				continue;
			timer.deadline = 0;
			timer.expired = true;
			alarmDue = true;
		}
		return alarmDue;
	}

	bool Running(int minutes) const
	{
		return Valid(minutes) && timers_[static_cast<std::size_t>(minutes - 1)].deadline != 0;
	}

	bool Expired(int minutes) const
	{
		return Valid(minutes) && timers_[static_cast<std::size_t>(minutes - 1)].expired;
	}

	int RemainingSeconds(int minutes, std::uint64_t now) const
	{
		if (!Running(minutes))
			return 0;
		const auto deadline = timers_[static_cast<std::size_t>(minutes - 1)].deadline;
		return now >= deadline ? 0 : static_cast<int>((deadline - now + 999ULL) / 1000ULL);
	}

private:
	static bool Valid(int minutes) { return minutes >= 1 && minutes <= Count; }
	struct Timer
	{
		std::uint64_t deadline = 0;
		bool expired = false;
	};
	std::array<Timer, Count> timers_{};
};
