#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VsmrRimcasLogic
{
	// Retain records and their string storage between refreshes. Only [0, size_)
	// belongs to the current refresh; stale records must never participate.
	class RunwayTraffic
	{
	public:
		struct Entry { std::string first; std::string second; std::size_t sequence; };
		void Clear() { size_ = 0; }
		void Add(const std::string& runway, const std::string& callsign)
		{
			if (size_ == entries_.size()) entries_.push_back({ runway, callsign, size_ });
			else
			{
				entries_[size_].first = runway;
				entries_[size_].second = callsign;
				entries_[size_].sequence = size_;
			}
			++size_;
		}
		void Sort()
		{
			// A multimap preserves insertion order for equivalent keys. Including
			// the sequence preserves that behavior without stable_sort's buffer.
			std::sort(entries_.begin(), End(), [](const Entry& a, const Entry& b)
			{
				return a.first == b.first ? a.sequence < b.sequence : a.first < b.first;
			});
		}
		auto EqualRange(std::string_view runway) const
		{
			const auto first = std::lower_bound(entries_.begin(), End(), runway,
				[](const Entry& entry, std::string_view key) { return entry.first < key; });
			const auto last = std::upper_bound(first, End(), runway,
				[](std::string_view key, const Entry& entry) { return key < entry.first; });
			return std::make_pair(first, last);
		}
		std::size_t Capacity() const { return entries_.capacity(); }
	private:
		std::vector<Entry> entries_;
		std::size_t size_ = 0;
		std::vector<Entry>::iterator End() { return entries_.begin() + size_; }
		std::vector<Entry>::const_iterator End() const { return entries_.begin() + size_; }
	};

	class RunwayCountdowns
	{
		struct Entry { std::string runway; int seconds; std::string callsign; };
	public:
		void Clear() { size_ = 0; }
		void Set(const std::string& runway, int seconds, const std::string& callsign)
		{
			for (std::size_t i = 0; i < size_; ++i)
				if (entries_[i].runway == runway && entries_[i].seconds == seconds)
				{ entries_[i].callsign = callsign; return; }
			if (size_ == entries_.size()) entries_.push_back({ runway, seconds, callsign });
			else
			{
				entries_[size_].runway = runway;
				entries_[size_].seconds = seconds;
				entries_[size_].callsign = callsign;
			}
			++size_;
		}
		bool HasRunway(std::string_view runway) const
		{
			for (std::size_t i = 0; i < size_; ++i)
				if (entries_[i].runway == runway) return true;
			return false;
		}
		// As before, the last observed aircraft wins an occupied display slot.
		const std::string* Find(std::string_view runway, int seconds) const
		{
			for (std::size_t i = 0; i < size_; ++i)
				if (entries_[i].runway == runway && entries_[i].seconds == seconds)
					return &entries_[i].callsign;
			return nullptr;
		}
		std::size_t Capacity() const { return entries_.capacity(); }
	private:
		std::vector<Entry> entries_;
		std::size_t size_ = 0;
	};
}
