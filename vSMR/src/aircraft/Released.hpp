#pragma once

#include "aircraft/HoldingPoint.hpp"

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// "Released to ground": shared between controllers through the flight-plan
// remarks like the holding point, as one VSMRREL/HHMM token (UTC release time).
// A well-formed token means released. DMAN writes and removes the same token.
namespace VsmrReleased
{
	inline constexpr const char* Marker = "VSMRREL/";
	inline constexpr auto PendingValueLifetime = VsmrHoldingPoint::PendingValueLifetime;

	enum class ToggleAction
	{
		Unrelease,
		Release,
		// Releasing a flight that is not ready yet makes it ready first (CDM's ASRT).
		ReadyThenRelease
	};

	// Un-releasing never touches the ASRT.
	inline ToggleAction DecideToggle(bool released, bool hasAsrt) noexcept
	{
		if (released)
			return ToggleAction::Unrelease;
		return hasAsrt ? ToggleAction::Release : ToggleAction::ReadyThenRelease;
	}

	inline bool IsReleaseTime(const std::string& value)
	{
		if (value.size() != 4)
			return false;
		for (const char character : value)
		{
			if (character < '0' || character > '9')
				return false;
		}
		const int hours = (value[0] - '0') * 10 + (value[1] - '0');
		const int minutes = (value[2] - '0') * 10 + (value[3] - '0');
		return hours < 24 && minutes < 60;
	}

	inline std::string FormatReleaseTime(const std::tm& utc)
	{
		if (utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59)
			return {};
		const char text[] = {
			static_cast<char>('0' + utc.tm_hour / 10), static_cast<char>('0' + utc.tm_hour % 10),
			static_cast<char>('0' + utc.tm_min / 10), static_cast<char>('0' + utc.tm_min % 10), '\0' };
		return text;
	}

	inline bool IsMarkerToken(const std::string& token)
	{
		return VsmrHoldingPoint::StartsWithAsciiCaseInsensitive(token, Marker);
	}

	// Release time of the last well-formed token, empty when the flight is not released.
	inline std::string Read(const std::string& remarks)
	{
		std::string latestValue;
		for (const std::string& token : VsmrHoldingPoint::SplitRemarks(remarks))
		{
			if (!IsMarkerToken(token))
				continue;
			const std::string value = token.substr(std::char_traits<char>::length(Marker));
			if (IsReleaseTime(value))
				latestValue = value;
		}
		return latestValue;
	}

	// Replaces every marker token, malformed ones included, while preserving other remarks.
	// An empty or malformed time removes the flag.
	inline std::string Write(const std::string& remarks, const std::string& releaseTime)
	{
		std::vector<std::string> tokens;
		for (std::string& token : VsmrHoldingPoint::SplitRemarks(remarks))
		{
			if (!IsMarkerToken(token))
				tokens.push_back(std::move(token));
		}
		if (IsReleaseTime(releaseTime))
			tokens.emplace_back(std::string(Marker) + releaseTime);
		return VsmrHoldingPoint::JoinRemarks(tokens);
	}

	// Keeping local toggles visible while EuroScope synchronizes the remarks
	struct PendingValue
	{
		std::string value;
		std::chrono::steady_clock::time_point submittedAt;
	};

	inline std::mutex PendingValuesMutex;
	inline std::unordered_map<std::string, PendingValue> PendingValues;
	// Callsigns retained for airborne remark cleanup
	inline std::unordered_set<std::string> ReleasedCallsigns;

	inline void TrackLocked(const std::string& key, const std::string& value)
	{
		if (value.empty())
			ReleasedCallsigns.erase(key);
		else
			ReleasedCallsigns.insert(key);
	}

	inline void RememberPending(const std::string& callsign, const std::string& releaseTime)
	{
		const std::string key = VsmrHoldingPoint::NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues[key] = { releaseTime, std::chrono::steady_clock::now() };
		TrackLocked(key, releaseTime);
	}

	inline void ForgetPending(const std::string& callsign)
	{
		const std::string key = VsmrHoldingPoint::NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.erase(key);
		ReleasedCallsigns.erase(key);
	}

	inline void ClearPending()
	{
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.clear();
		ReleasedCallsigns.clear();
	}

	inline std::vector<std::string> KnownCallsigns()
	{
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		return std::vector<std::string>(ReleasedCallsigns.begin(), ReleasedCallsigns.end());
	}

	// Release time to display: the synchronized one, or a local toggle EuroScope
	// has not echoed yet. Empty means not released.
	inline std::string Resolve(const std::string& callsign, const std::string& remarks)
	{
		const std::string synchronizedValue = Read(remarks);
		const std::string key = VsmrHoldingPoint::NormalizeCallsign(callsign);
		if (key.empty())
			return synchronizedValue;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		const auto pending = PendingValues.find(key);
		if (pending != PendingValues.end())
		{
			// Keeping the optimistic value until EuroScope echoes it or the toggle times out
			if (pending->second.value != synchronizedValue &&
				std::chrono::steady_clock::now() - pending->second.submittedAt <= PendingValueLifetime)
			{
				return pending->second.value;
			}
			PendingValues.erase(pending);
		}
		TrackLocked(key, synchronizedValue);
		return synchronizedValue;
	}
}
