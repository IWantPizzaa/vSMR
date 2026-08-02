#include "stdafx.h"
#include "WeatherData.hpp"

#include <cctype>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	std::mutex& WeatherCacheMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	std::unordered_map<std::string, VsmrWeather::Snapshot>& WeatherCache()
	{
		static std::unordered_map<std::string, VsmrWeather::Snapshot> cache;
		return cache;
	}

	std::string TrimAsciiWhitespace(const std::string& value)
	{
		size_t first = 0;
		while (first < value.size() &&
			std::isspace(static_cast<unsigned char>(value[first])) != 0)
		{
			++first;
		}

		size_t last = value.size();
		while (last > first &&
			std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
		{
			--last;
		}
		return value.substr(first, last - first);
	}

	std::string UpperAscii(const std::string& value)
	{
		std::string result(value);
		for (char& character : result)
		{
			character = static_cast<char>(
				std::toupper(static_cast<unsigned char>(character)));
		}
		return result;
	}

	bool IsAsciiAlphaNumeric(char character)
	{
		const unsigned char value = static_cast<unsigned char>(character);
		return std::isalpha(value) != 0 || std::isdigit(value) != 0;
	}

	bool TryParseDigits(
		const std::string& value,
		size_t offset,
		size_t length,
		int& parsed)
	{
		if (length == 0 || offset > value.size() || length > value.size() - offset)
			return false;

		int result = 0;
		for (size_t index = offset; index < offset + length; ++index)
		{
			const unsigned char character = static_cast<unsigned char>(value[index]);
			if (std::isdigit(character) == 0)
				return false;
			result = (result * 10) + (value[index] - '0');
		}
		parsed = result;
		return true;
	}

	std::vector<std::string> TokenizeReport(const std::string& report)
	{
		std::istringstream input(UpperAscii(report));
		std::vector<std::string> tokens;
		std::string token;
		while (input >> token)
		{
			while (!token.empty() && token.back() == '=')
				token.pop_back();
			if (!token.empty())
				tokens.push_back(std::move(token));
		}
		return tokens;
	}

	bool TryParseWindToken(const std::string& token, VsmrWeather::Snapshot& snapshot)
	{
		const bool knots = token.size() >= 7 && token.compare(token.size() - 2, 2, "KT") == 0;
		const bool metersPerSecond = token.size() >= 8 && token.compare(token.size() - 3, 3, "MPS") == 0;
		if (!knots && !metersPerSecond)
			return false;

		const std::string body = token.substr(0, token.size() - (knots ? 2 : 3));
		if (body.size() < 5)
			return false;

		const bool variable = body.compare(0, 3, "VRB") == 0;
		int direction = 0;
		if (!variable && !TryParseDigits(body, 0, 3, direction))
			return false;
		if (!variable && (direction < 0 || direction > 360))
			return false;

		const size_t gustSeparator = body.find('G', 3);
		const size_t speedEnd = gustSeparator == std::string::npos
			? body.size()
			: gustSeparator;
		const size_t speedLength = speedEnd - 3;
		if (speedLength < 2 || speedLength > 3)
			return false;

		int speed = 0;
		if (!TryParseDigits(body, 3, speedLength, speed))
			return false;

		bool hasGust = false;
		int gust = 0;
		if (gustSeparator != std::string::npos)
		{
			const size_t gustOffset = gustSeparator + 1;
			const size_t gustLength = body.size() - gustOffset;
			if (gustLength < 2 || gustLength > 3 ||
				!TryParseDigits(body, gustOffset, gustLength, gust))
			{
				return false;
			}
			hasGust = true;
		}

		snapshot.hasWind = true;
		snapshot.windVariable = variable;
		snapshot.windCalm = speed == 0;
		snapshot.windDirectionDegrees = variable ? 0 : direction;
		const auto toKnots = [&](int value) -> int
		{
			return knots
				? value
				: static_cast<int>(std::lround(static_cast<double>(value) * 1.9438444924406));
		};
		snapshot.windSpeedKnots = toKnots(speed);
		snapshot.hasWindGust = hasGust;
		snapshot.windGustKnots = hasGust ? toKnots(gust) : 0;
		return true;
	}

	bool TryParseVariationToken(const std::string& token, int& from, int& to)
	{
		if (token.size() != 7 || token[3] != 'V' ||
			!TryParseDigits(token, 0, 3, from) ||
			!TryParseDigits(token, 4, 3, to))
		{
			return false;
		}
		return from >= 0 && from <= 360 && to >= 0 && to <= 360;
	}

	bool TryParseObservationToken(
		const std::string& token,
		std::time_t receivedUtc,
		std::time_t& observationUtc)
	{
		if (token.size() != 7 || token[6] != 'Z')
			return false;

		int day = 0;
		int hour = 0;
		int minute = 0;
		if (!TryParseDigits(token, 0, 2, day) ||
			!TryParseDigits(token, 2, 2, hour) ||
			!TryParseDigits(token, 4, 2, minute) ||
			day < 1 || day > 31 || hour < 0 || hour > 23 ||
			minute < 0 || minute > 59)
		{
			return false;
		}

		std::tm received = {};
		if (::gmtime_s(&received, &receivedUtc) != 0)
			return false;

		bool found = false;
		double nearestDistanceSeconds = 0.0;
		std::time_t nearest = 0;
		for (int monthOffset = -1; monthOffset <= 1; ++monthOffset)
		{
			int candidateYear = received.tm_year + 1900;
			int candidateMonth = received.tm_mon + monthOffset;
			while (candidateMonth < 0)
			{
				candidateMonth += 12;
				--candidateYear;
			}
			while (candidateMonth >= 12)
			{
				candidateMonth -= 12;
				++candidateYear;
			}

			std::tm candidate = {};
			candidate.tm_year = candidateYear - 1900;
			candidate.tm_mon = candidateMonth;
			candidate.tm_mday = day;
			candidate.tm_hour = hour;
			candidate.tm_min = minute;
			candidate.tm_sec = 0;
			candidate.tm_isdst = 0;
			const std::time_t candidateUtc = ::_mkgmtime(&candidate);
			if (candidateUtc == static_cast<std::time_t>(-1))
				continue;

			// _mkgmtime normalizes impossible dates such as 31 February. Reject
			// those rather than silently interpreting a different observation.
			std::tm normalized = {};
			if (::gmtime_s(&normalized, &candidateUtc) != 0 ||
				normalized.tm_year != candidateYear - 1900 ||
				normalized.tm_mon != candidateMonth ||
				normalized.tm_mday != day ||
				normalized.tm_hour != hour ||
				normalized.tm_min != minute)
			{
				continue;
			}

			const double distanceSeconds = std::fabs(std::difftime(candidateUtc, receivedUtc));
			if (!found || distanceSeconds < nearestDistanceSeconds)
			{
				found = true;
				nearestDistanceSeconds = distanceSeconds;
				nearest = candidateUtc;
			}
		}

		if (!found)
			return false;

		observationUtc = nearest;
		return true;
	}

	bool TryParseQnhToken(const std::string& token, int& qnh)
	{
		if (token.size() == 5 && token[0] == 'Q')
			return TryParseDigits(token, 1, 4, qnh);

		int altimeterHundredths = 0;
		if (token.size() == 5 && token[0] == 'A' &&
			TryParseDigits(token, 1, 4, altimeterHundredths))
		{
			qnh = static_cast<int>(std::lround(
				(static_cast<double>(altimeterHundredths) / 100.0) * 33.8638866667));
			return true;
		}

		return false;
	}
}

namespace VsmrWeather
{
	std::string NormalizeIcao(const std::string& icao)
	{
		const std::string normalized = UpperAscii(TrimAsciiWhitespace(icao));
		if (normalized.size() != 4)
			return "";
		for (char character : normalized)
		{
			if (!IsAsciiAlphaNumeric(character))
				return "";
		}
		return normalized;
	}

	bool ParseReport(
		const std::string& icao,
		const std::string& report,
		Snapshot& snapshot,
		std::time_t receivedUtc)
	{
		Snapshot parsed;
		parsed.icao = NormalizeIcao(icao);
		if (parsed.icao.empty() || TrimAsciiWhitespace(report).empty())
		{
			snapshot = Snapshot{};
			return false;
		}

		parsed.receivedUtc = receivedUtc > 0 ? receivedUtc : std::time(nullptr);
		parsed.updatedUtc = parsed.receivedUtc;
		const std::vector<std::string> tokens = TokenizeReport(report);
		for (const std::string& token : tokens)
		{
			if (parsed.observationUtc == 0)
			{
				std::time_t observationUtc = 0;
				if (TryParseObservationToken(token, parsed.receivedUtc, observationUtc))
				{
					parsed.observationUtc = observationUtc;
					parsed.updatedUtc = observationUtc;
				}
			}

			if (!parsed.hasWind && TryParseWindToken(token, parsed))
				continue;

			if (!parsed.hasWindVariation)
			{
				int from = 0;
				int to = 0;
				if (TryParseVariationToken(token, from, to))
				{
					parsed.hasWindVariation = true;
					parsed.windVariationFromDegrees = from;
					parsed.windVariationToDegrees = to;
					continue;
				}
			}

			if (!parsed.hasQnh)
			{
				int qnh = 0;
				if (TryParseQnhToken(token, qnh))
				{
					parsed.hasQnh = true;
					parsed.qnhHpa = qnh;
				}
			}
		}

		if (!parsed.hasWind && !parsed.hasQnh)
		{
			snapshot = Snapshot{};
			return false;
		}

		snapshot = std::move(parsed);
		return true;
	}

	bool Update(
		const std::string& icao,
		const std::string& report,
		std::time_t receivedUtc,
		bool fromFallback)
	{
		Snapshot snapshot;
		if (!ParseReport(icao, report, snapshot, receivedUtc))
			return false;
		snapshot.fromFallback = fromFallback;

		std::lock_guard<std::mutex> guard(WeatherCacheMutex());
		const auto existing = WeatherCache().find(snapshot.icao);
		if (existing != WeatherCache().end())
		{
			const std::time_t incomingDataUtc = snapshot.observationUtc > 0
				? snapshot.observationUtc
				: snapshot.receivedUtc;
			const std::time_t existingDataUtc = existing->second.observationUtc > 0
				? existing->second.observationUtc
				: existing->second.receivedUtc;
			if (incomingDataUtc > 0 && existingDataUtc > 0 && incomingDataUtc < existingDataUtc)
			{
				return false;
			}
			if (incomingDataUtc > 0 && incomingDataUtc == existingDataUtc)
			{
				if (snapshot.fromFallback && !existing->second.fromFallback)
					return false;
				if (snapshot.fromFallback == existing->second.fromFallback &&
					snapshot.receivedUtc < existing->second.receivedUtc)
				{
					return false;
				}
			}
		}
		WeatherCache()[snapshot.icao] = std::move(snapshot);
		return true;
	}

	bool Update(const char* icao, const char* report)
	{
		if (icao == nullptr || report == nullptr)
			return false;
		return Update(std::string(icao), std::string(report), 0, false);
	}

	bool TryGet(const std::string& icao, Snapshot& snapshot)
	{
		const std::string normalized = NormalizeIcao(icao);
		if (normalized.empty())
			return false;

		std::lock_guard<std::mutex> guard(WeatherCacheMutex());
		const auto found = WeatherCache().find(normalized);
		if (found == WeatherCache().end())
			return false;
		snapshot = found->second;
		return true;
	}

	void Erase(const std::string& icao)
	{
		const std::string normalized = NormalizeIcao(icao);
		if (normalized.empty())
			return;

		std::lock_guard<std::mutex> guard(WeatherCacheMutex());
		WeatherCache().erase(normalized);
	}

	void Clear()
	{
		std::lock_guard<std::mutex> guard(WeatherCacheMutex());
		WeatherCache().clear();
	}
}
