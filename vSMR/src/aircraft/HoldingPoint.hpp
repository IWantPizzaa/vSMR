#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
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

	struct PendingValue
	{
		std::string value;
		std::chrono::steady_clock::time_point submittedAt;
	};

	inline std::mutex PendingValuesMutex;
	inline std::unordered_map<std::string, PendingValue> PendingValues;

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

	inline std::vector<std::string> SplitRemarks(const std::string& remarks)
	{
		std::istringstream stream(remarks);
		std::vector<std::string> tokens;
		std::string token;
		while (stream >> token)
			tokens.push_back(std::move(token));
		return tokens;
	}

	inline std::string JoinRemarks(const std::vector<std::string>& tokens)
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

	inline std::string Read(const std::string& remarks)
	{
		for (const std::string& token : SplitRemarks(remarks))
		{
			if (!IsMarkerToken(token))
				continue;

			std::string value;
			if (Normalize(token.substr(3), value))
				return value;
		}
		return "";
	}

	inline std::string WithoutHoldingPoint(const std::string& text)
	{
		std::vector<std::string> retained;
		for (const std::string& token : SplitRemarks(text))
		{
			if (!IsMarkerToken(token))
				retained.push_back(token);
		}
		return JoinRemarks(retained);
	}

	inline std::string Write(const std::string& remarks, const std::string& normalizedValue)
	{
		std::vector<std::string> tokens;
		for (const std::string& token : SplitRemarks(remarks))
		{
			if (!IsMarkerToken(token))
				tokens.push_back(token);
		}
		if (!normalizedValue.empty())
			tokens.emplace_back(std::string(Marker) + normalizedValue);
		return JoinRemarks(tokens);
	}

	inline void RememberPending(const std::string& callsign, const std::string& normalizedValue)
	{
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues[key] = { normalizedValue, std::chrono::steady_clock::now() };
	}

	inline void ForgetPending(const std::string& callsign)
	{
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.erase(key);
	}

	inline void ClearPending()
	{
		std::lock_guard<std::mutex> guard(PendingValuesMutex);
		PendingValues.clear();
	}

	inline std::string Resolve(const std::string& callsign, const std::string& remarks)
	{
		const std::string synchronizedValue = Read(remarks);
		const std::string key = NormalizeCallsign(callsign);
		if (key.empty())
			return synchronizedValue;

		std::lock_guard<std::mutex> guard(PendingValuesMutex);
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
