#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VsmrRimcasLogic
{
	// Refresh-local records use contiguous storage. Clear retains the high-water
	// capacity; runway ordering is needed only when the completed frame is read.
	class RunwayTraffic
	{
	public:
		using Entry = std::pair<std::string, std::string>;
		void Clear() { entries_.clear(); }
		void Add(const std::string& runway, const std::string& callsign)
		{ entries_.emplace_back(runway, callsign); }
		void Sort()
		{ std::sort(entries_.begin(), entries_.end()); }
		auto EqualRange(std::string_view runway) const
		{
			const auto first = std::lower_bound(entries_.begin(), entries_.end(), runway,
				[](const Entry& entry, std::string_view key) { return entry.first < key; });
			const auto last = std::upper_bound(first, entries_.end(), runway,
				[](std::string_view key, const Entry& entry) { return key < entry.first; });
			return std::make_pair(first, last);
		}
		std::size_t Capacity() const { return entries_.capacity(); }
	private:
		std::vector<Entry> entries_;
	};

	class RunwayCountdowns
	{
		struct Entry { std::string runway; int seconds; std::string callsign; };
	public:
		void Clear() { entries_.clear(); }
		// The IAW has one displayed aircraft per countdown slot. Preserve the
		// existing last-observed selection when two aircraft share that slot.
		void Set(const std::string& runway, int seconds, const std::string& callsign)
		{
			for (auto& entry : entries_)
				if (entry.runway == runway && entry.seconds == seconds)
				{ entry.callsign = callsign; return; }
			entries_.push_back({ runway, seconds, callsign });
		}
		bool HasRunway(std::string_view runway) const
		{
			return std::any_of(entries_.begin(), entries_.end(),
				[&](const Entry& entry) { return entry.runway == runway; });
		}
		const std::string* Find(std::string_view runway, int seconds) const
		{
			for (const auto& entry : entries_)
				if (entry.runway == runway && entry.seconds == seconds) return &entry.callsign;
			return nullptr;
		}
		std::size_t Capacity() const { return entries_.capacity(); }
	private:
		std::vector<Entry> entries_;
	};
}
