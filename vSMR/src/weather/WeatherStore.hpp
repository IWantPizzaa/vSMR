#pragma once

#include <ctime>
#include <string>

namespace VsmrWeather
{
	// A small, copyable view of the latest weather received for one station.
	// Optional METAR fields are represented by explicit flags so zero remains a
	// valid value for calm wind and parsed numeric fields.
	struct Snapshot
	{
		std::string icao;
		// Normalized, single-space raw report used by the responsive METAR
		// inset. It is captured with the parsed values so every viewport sees
		// one coherent weather observation.
		std::string rawReport;
		// UTC time at which EuroScope delivered this report.
		std::time_t receivedUtc = 0;
		// UTC observation time decoded from DDHHMMZ when present.
		std::time_t observationUtc = 0;
		// Effective data timestamp used by existing consumers: observation time
		// when available, otherwise receipt time.
		std::time_t updatedUtc = 0;
		// EuroScope callbacks win ties against the asynchronous HTTP fallback.
		bool fromFallback = false;

		bool hasWind = false;
		bool windVariable = false;
		bool windCalm = false;
		int windDirectionDegrees = 0;
		int windSpeedKnots = 0;

		bool hasWindGust = false;
		int windGustKnots = 0;

		bool hasWindVariation = false;
		int windVariationFromDegrees = 0;
		int windVariationToDegrees = 0;

		bool hasQnh = false;
		int qnhHpa = 0;
	};

	// Trim and uppercase a four-character ICAO station identifier. Invalid
	// identifiers return an empty string.
	std::string NormalizeIcao(const std::string& icao);

	// Parse the operational fields used by the weather inset. METAR and SPECI
	// prefixes are optional because EuroScope supplies the complete raw report
	// in either form. A true result means at least wind or QNH was decoded.
	// Passing zero for receivedUtc records the current UTC time. DDHHMMZ is
	// resolved to the nearest valid month around that receipt time.
	bool ParseReport(
		const std::string& icao,
		const std::string& report,
		Snapshot& snapshot,
		std::time_t receivedUtc = 0);

	// Parse and atomically replace the process-wide snapshot for a station.
	// NIL, malformed, unsupported, and out-of-order older reports leave the
	// last usable snapshot untouched.
	bool Update(
		const std::string& icao,
		const std::string& report,
		std::time_t receivedUtc = 0,
		bool fromFallback = false);
	bool Update(const char* icao, const char* report);

	// Readers receive an independent copy and never retain cache-owned storage.
	bool TryGet(const std::string& icao, Snapshot& snapshot);

	void Erase(const std::string& icao);
	void Clear();
}
