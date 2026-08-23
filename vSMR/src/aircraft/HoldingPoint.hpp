#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VsmrHoldingPoint
{
	inline constexpr const char* Marker = "HP:";
	inline constexpr std::size_t MaximumValueLength = 8;
	inline constexpr auto PendingValueLifetime = std::chrono::seconds(15);
	inline constexpr auto AssumptionRestoreWindow = std::chrono::seconds(10);

	struct PendingValue
	{
		std::string value;
		std::chrono::steady_clock::time_point submittedAt;
	};

	struct AssumptionState
	{
		bool initialized = false;
		bool wasAssumed = false;
		bool restoreAttempted = false;
		std::chrono::steady_clock::time_point restoreUntil{};
	};

	inline std::mutex PendingValuesMutex;
	inline std::unordered_map<std::string, PendingValue> PendingValues;
	inline std::unordered_map<std::string, std::string> KnownValues;
	inline std::unordered_map<std::string, AssumptionState> AssumptionStates;

	inline std::string Trim(const std::string& value)
	{
		const auto first = std::find_if_not(
			value.begin(), value.end(),
			[](unsigned char character) { return std::isspace(character) != 0; });
		if (first == value.end())
			return "";

		const auto last = std::find_if_not(
			value.rbegin(), value.rend(),
			[](unsigned char character) { return std::isspace(character) != 0; }).base();
		return std::string(first, last);
	}

	inline bool IsMarkerToken(const std::string& token)
	{
		return token.size() >= 3 &&
			std::toupper(static_cast<unsigned char>(token[0])) == 'H' &&
			std::toupper(static_cast<unsigned char>(token[1])) == 'P' &&
			token[2] == ':';
	}

	inline std::string NormalizeCallsign(const std::string& callsign)
	{
		std::string normalized = Trim(callsign);
		normalized.erase(
			std::remove_if(
				normalized.begin(), normalized.end(),
				[](unsigned char character) { return std::isspace(character) != 0; }),
			normalized.end());
		std::transform(
			normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char character) { return static_cast<char>(std::toupper(character)); });
		return normalized;
	}

	inline bool Normalize(const std::string& input, std::string& normalized, std::string* error = nullptr)
	{
		normalized = Trim(input);
		std::transform(
			normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char character) { return static_cast<char>(std::toupper(character)); });

		if (normalized.empty())
			return true;
		if (normalized.size() > MaximumValueLength)
		{
			if (error != nullptr)
				*error = "Holding points can contain at most 8 characters.";
			return false;
		}

		const bool valid = std::all_of(
			normalized.begin(), normalized.end(),
			[](unsigned char character)
			{
				return std::isalnum(character) != 0 || character == '-' || character == '/';
			});
		if (!valid)
		{
			if (error != nullptr)
				*error = "Use only letters, numbers, '-' or '/' in a holding point.";
			return false;
		}

		return true;
	}

	inline std::vector<std::string> SplitScratchpad(const std::string& scratchpad)
	{
		std::istringstream stream(scratchpad);
		std::vector<std::string> tokens;
		std::string token;
		while (stream >> token)
			tokens.push_back(std::move(token));
		return tokens;
	}

	inline std::string JoinScratchpad(const std::vector<std::string>& tokens)
	{
		std::string result;
		for (const std::string& token : tokens)
		{
			if (!result.empty())
				result.push_back(' ');
			result += token;
		}
		return result;
	}

	inline std::string Read(const std::string& scratchpad)
	{
		for (const std::string& token : SplitScratchpad(scratchpad))
		{
			if (!IsMarkerToken(token))
				continue;

			std::string value;
			if (Normalize(token.substr(3), value))
				return value;
		}
		return "";
	}

	inline std::string WithoutHoldingPoint(const std::string& scratchpad)
	{
		std::vector<std::string> retained;
		for (const std::string& token : SplitScratchpad(scratchpad))
		{
			if (!IsMarkerToken(token))
				retained.push_back(token);
		}
		return JoinScratchpad(retained);
	}

	inline std::string Write(const std::string& scratchpad, const std::string& normalizedValue)
	{
		std::vector<std::string> tokens;
		for (const std::string& token : SplitScratchpad(scratchpad))
		{
			if (!IsMarkerToken(token))
				tokens.push_back(token);
		}
		if (!normalizedValue.empty())
			tokens.emplace_back(std::string(Marker) + normalizedValue);
		return JoinScratchpad(tokens);
	}

	inline void RememberPending(const std::string& callsign, const std::string& normalizedValue)
	{
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues[key] = { normalizedValue, std::chrono::steady_clock::now() };
		if (normalizedValue.empty())
			KnownValues.erase(key);
		else
			KnownValues[key] = normalizedValue;
	}

	inline void ForgetPending(const std::string& callsign)
	{
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.erase(key);
		KnownValues.erase(key);
		AssumptionStates.erase(key);
	}

	inline void ClearPending()
	{
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.clear();
		KnownValues.clear();
		AssumptionStates.clear();
	}

	inline std::vector<std::string> KnownCallsigns()
	{
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		std::vector<std::string> callsigns;
		callsigns.reserve(KnownValues.size());
		for (const auto& entry : KnownValues)
			callsigns.push_back(entry.first);
		return callsigns;
	}

	// EuroScope can briefly remove controller-assigned scratchpad data when a
	// flight is assumed. Preserve only a value that vSMR previously observed,
	// and only during the short transition into the assumed state. An explicit
	// edit to an empty value removes KnownValues and is therefore never restored.
	inline std::optional<std::string> ObserveAssumption(
		const std::string& callsign,
		const std::string& scratchpad,
		bool isAssumed)
	{
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return std::nullopt;

		const std::string synchronizedValue = Read(scratchpad);
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		if (!synchronizedValue.empty())
			KnownValues[key] = synchronizedValue;

		AssumptionState& state = AssumptionStates[key];
		if (!state.initialized || (isAssumed && !state.wasAssumed))
		{
			state.initialized = true;
			state.restoreAttempted = false;
			state.restoreUntil = isAssumed ? now + AssumptionRestoreWindow : now;
		}
		else if (!isAssumed)
		{
			state.restoreAttempted = false;
			state.restoreUntil = now;
		}
		state.wasAssumed = isAssumed;

		if (!isAssumed || !synchronizedValue.empty() || state.restoreAttempted || now > state.restoreUntil)
			return std::nullopt;

		const auto known = KnownValues.find(key);
		if (known == KnownValues.end() || known->second.empty())
			return std::nullopt;

		state.restoreAttempted = true;
		return known->second;
	}

	inline std::string Resolve(const std::string& callsign, const std::string& scratchpad)
	{
		const std::string synchronizedValue = Read(scratchpad);
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return synchronizedValue;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		if (!synchronizedValue.empty())
			KnownValues[key] = synchronizedValue;
		const auto pending = PendingValues.find(key);
		if (pending == PendingValues.end())
			return synchronizedValue;

		if (pending->second.value == synchronizedValue)
		{
			PendingValues.erase(pending);
			return synchronizedValue;
		}

		if (std::chrono::steady_clock::now() - pending->second.submittedAt <= PendingValueLifetime)
			return pending->second.value;

		PendingValues.erase(pending);
		return synchronizedValue;
	}
}
