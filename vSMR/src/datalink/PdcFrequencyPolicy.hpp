#pragma once

#include "shared/TextUtils.hpp"

#include <cmath>
#include <string>
#include <tuple>
#include <vector>

// Pure departure-airport selection. A station's own or coordinated identity
// cannot establish coverage of another airport; those are tie-breakers only.
namespace VsmrPdcFrequencyPolicy
{
	struct Controller
	{
		std::string callsign;
		std::string positionId;
		double frequency = 0.0;
		bool isController = true;
		bool isOwn = false;
		bool isCoordinated = false;
	};

	struct Selection
	{
		double frequency = 0.0;
		std::string controller;
		std::string source;
	};

	namespace Detail
	{
		enum class PdcControllerFacility
		{
			Unknown = 0,
			Delivery,
			Ramp,
			Ground,
			Tower,
			Approach,
			Departure,
			Center
		};

		inline PdcControllerFacility DetectPdcControllerFacility(const std::string& rawIdentity)
		{
			const std::string identity = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawIdentity));
			std::string token;
			auto classifyToken = [](const std::string& value) -> PdcControllerFacility
			{
				if (value == "DEL" || value == "DELIVERY") return PdcControllerFacility::Delivery;
				if (value == "RMP" || value == "RAMP") return PdcControllerFacility::Ramp;
				if (value == "GND" || value == "GROUND") return PdcControllerFacility::Ground;
				if (value == "TWR" || value == "TOWER") return PdcControllerFacility::Tower;
				if (value == "APP" || value == "APPROACH") return PdcControllerFacility::Approach;
				if (value == "DEP" || value == "DEPARTURE") return PdcControllerFacility::Departure;
				if (value == "CTR" || value == "CENTER" || value == "CENTRE") return PdcControllerFacility::Center;
				return PdcControllerFacility::Unknown;
			};

			for (size_t index = 0; index <= identity.size(); ++index)
			{
				const char c = index < identity.size() ? identity[index] : '_';
				if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				{
					token.push_back(c);
					continue;
				}
				const PdcControllerFacility facility = classifyToken(token);
				if (facility != PdcControllerFacility::Unknown)
					return facility;
				token.clear();
			}
			return PdcControllerFacility::Unknown;
		}

		inline int PdcFacilityClearancePriority(PdcControllerFacility facility)
		{
			// The lowest staffed position in the departure chain issues the PDC and
			// must therefore be the frequency printed in it.
			switch (facility)
			{
			case PdcControllerFacility::Delivery: return 600;
			case PdcControllerFacility::Ramp: return 500;
			case PdcControllerFacility::Ground: return 400;
			case PdcControllerFacility::Tower: return 300;
			case PdcControllerFacility::Departure:
			case PdcControllerFacility::Approach: return 200;
			case PdcControllerFacility::Center: return 100;
			default: return -1;
			}
		}

		inline const char* PdcFacilityName(PdcControllerFacility facility)
		{
			switch (facility)
			{
			case PdcControllerFacility::Delivery: return "delivery";
			case PdcControllerFacility::Ramp: return "ramp";
			case PdcControllerFacility::Ground: return "ground";
			case PdcControllerFacility::Tower: return "tower";
			case PdcControllerFacility::Approach: return "approach";
			case PdcControllerFacility::Departure: return "departure";
			case PdcControllerFacility::Center: return "center";
			default: return "unknown";
			}
		}

		inline bool PdcControllerMatchesAirport(const std::string& rawIdentity, const std::string& airport)
		{
			const std::string identity = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawIdentity));
			if (airport.size() != 4 || identity.size() < airport.size() || identity.compare(0, airport.size(), airport) != 0)
				return false;
			return identity.size() == airport.size() ||
				std::isalnum(static_cast<unsigned char>(identity[airport.size()])) == 0;
		}

		inline bool PdcIdentityHasToken(const std::string& rawIdentity, const std::string& expectedToken)
		{
			const std::string identity = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawIdentity));
			std::string token;
			for (size_t index = 0; index <= identity.size(); ++index)
			{
				const char c = index < identity.size() ? identity[index] : '_';
				if (std::isalnum(static_cast<unsigned char>(c)) != 0)
				{
					token.push_back(c);
					continue;
				}
				if (token == expectedToken)
					return true;
				token.clear();
			}
			return false;
		}

		inline int PdcRunwaySectorAffinity(
			const std::string& airport,
			const std::string& runway,
			const std::string& callsign,
			const std::string& positionId)
		{
			// CDG has separate north/south local positions. Its 08/26 runway complex
			// is north and its 09/27 complex is south; use that only as a tie-breaker
			// when more than one valid local controller is connected.
			if (airport != "LFPG" || runway.size() < 2)
				return 0;
			const int runwayNumber = std::isdigit(static_cast<unsigned char>(runway[0])) != 0 &&
				std::isdigit(static_cast<unsigned char>(runway[1])) != 0
				? (runway[0] - '0') * 10 + (runway[1] - '0')
				: -1;
			const char* expectedSector =
				(runwayNumber == 8 || runwayNumber == 26) ? "N" :
				(runwayNumber == 9 || runwayNumber == 27) ? "S" : nullptr;
			if (expectedSector == nullptr)
				return 0;
			return PdcIdentityHasToken(callsign, expectedSector) ||
				PdcIdentityHasToken(positionId, expectedSector) ? 50 : 0;
		}
	}

	inline bool IsUsableFrequency(double frequency)
	{
		return std::isfinite(frequency) && frequency >= 118.0 && frequency <= 136.990;
	}

	inline Selection Resolve(
		const std::string& rawOrigin,
		const std::string& rawRunway,
		const std::vector<Controller>& controllers)
	{
		Selection selection;
		const std::string origin = NormalizeAirportCodeCopy(TrimAsciiWhitespaceCopy(rawOrigin));
		if (origin.empty())
			return selection;
		const std::string runway = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawRunway));
		int bestScore = -1;
		std::tuple<std::string, std::string, double> bestIdentity;
		for (const Controller& candidate : controllers)
		{
			if (!candidate.isController || !IsUsableFrequency(candidate.frequency))
				continue;
			const std::string callsign = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(candidate.callsign));
			const std::string position = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(candidate.positionId));
			if (!Detail::PdcControllerMatchesAirport(callsign, origin) &&
				!Detail::PdcControllerMatchesAirport(position, origin))
				continue;
			auto facility = Detail::DetectPdcControllerFacility(callsign);
			if (facility == Detail::PdcControllerFacility::Unknown)
				facility = Detail::DetectPdcControllerFacility(position);
			int score = Detail::PdcFacilityClearancePriority(facility);
			if (score < 0)
				continue;
			score += Detail::PdcRunwaySectorAffinity(origin, runway, callsign, position);
			if (candidate.isOwn)
				score += 10;
			if (candidate.isCoordinated)
				score += 5;
			const auto identity = std::make_tuple(callsign, position, candidate.frequency);
			if (score > bestScore || (score == bestScore && identity < bestIdentity))
			{
				bestScore = score;
				bestIdentity = identity;
				selection.frequency = candidate.frequency;
				selection.controller = callsign.empty() ? position : callsign;
				selection.source = std::string("lowest connected origin ") + Detail::PdcFacilityName(facility);
			}
		}
		// No local position was identified: leave the field for explicit input,
		// rather than silently sending the issuer's or a foreign handoff frequency.
		return selection;
	}
}
