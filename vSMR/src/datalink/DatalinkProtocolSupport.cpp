#include "platform/windows/PrecompiledHeader.hpp"

#include "datalink/DatalinkProtocolSupport.hpp"

#include "EuroScopePlugIn.h"
#include "shared/TextUtils.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

using EuroScopePlugIn::CController;
using EuroScopePlugIn::CFlightPlan;

namespace
{
	constexpr const char* ProtectedCredentialPrefix = "dpapi:";

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

	PdcControllerFacility DetectPdcControllerFacility(const std::string& rawIdentity)
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

	int PdcFacilityClearancePriority(PdcControllerFacility facility)
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

	const char* PdcFacilityName(PdcControllerFacility facility)
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

	bool PdcControllerMatchesAirport(const std::string& rawIdentity, const std::string& airport)
	{
		const std::string identity = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(rawIdentity));
		if (airport.size() != 4 || identity.size() < airport.size() || identity.compare(0, airport.size(), airport) != 0)
			return false;
		return identity.size() == airport.size() ||
			std::isalnum(static_cast<unsigned char>(identity[airport.size()])) == 0;
	}

	bool PdcIdentityHasToken(const std::string& rawIdentity, const std::string& expectedToken)
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

	int PdcRunwaySectorAffinity(
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

	DATA_BLOB HoppieCredentialEntropy()
	{
		static char entropy[] = "vSMR CPDLC credential v1";
		DATA_BLOB blob = {};
		blob.pbData = reinterpret_cast<BYTE*>(entropy);
		blob.cbData = static_cast<DWORD>(strlen(entropy));
		return blob;
	}

	std::string NormalizeHoppieResponse(const std::string& raw)
	{
		std::string normalized = raw;
		if (normalized.size() >= 3 &&
			static_cast<unsigned char>(normalized[0]) == 0xEF &&
			static_cast<unsigned char>(normalized[1]) == 0xBB &&
			static_cast<unsigned char>(normalized[2]) == 0xBF)
		{
			normalized.erase(0, 3);
		}
		normalized = TrimAsciiWhitespaceCopy(normalized);
		for (char& c : normalized)
		{
			if (c == '\r' || c == '\n' || c == '\t')
				c = ' ';
		}
		return normalized;
	}
}

namespace VsmrDatalinkProtocol
{
std::string FormatPdcFrequency(double frequency)
{
	if (!std::isfinite(frequency) || frequency <= 0.0)
		return "";
	std::ostringstream formatted;
	formatted << std::fixed << std::setprecision(3) << frequency;
	return formatted.str();
}

PdcFrequencySelection ResolvePdcNextFrequency(
	EuroScopePlugIn::CPlugIn* plugIn,
	const CFlightPlan& flightPlan)
{
	PdcFrequencySelection selection;
	if (plugIn == nullptr || !flightPlan.IsValid())
		return selection;

	const char* originRaw = flightPlan.GetFlightPlanData().GetOrigin();
	std::string origin = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(originRaw != nullptr ? originRaw : ""));
	if (origin.size() > 4)
		origin.resize(4);
	const char* runwayRaw = flightPlan.GetFlightPlanData().GetDepartureRwy();
	const std::string runway = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(runwayRaw != nullptr ? runwayRaw : ""));

	const CController myself = plugIn->ControllerMyself();
	const std::string myCallsign = myself.IsValid() && myself.GetCallsign() != nullptr ? myself.GetCallsign() : "";
	const std::string myPosition = myself.IsValid() && myself.GetPositionId() != nullptr ? myself.GetPositionId() : "";
	const char* coordinatedRaw = flightPlan.GetCoordinatedNextController();
	const std::string coordinatedId = TrimAsciiWhitespaceCopy(coordinatedRaw != nullptr ? coordinatedRaw : "");
	CController coordinated;
	if (!coordinatedId.empty())
		coordinated = plugIn->ControllerSelect(coordinatedId.c_str());

	// PDC is issued by the lowest staffed departure position. EuroScope's
	// coordinated controller describes the next handoff and can consequently
	// skip the Tower that is currently issuing clearances.
	if (origin.size() == 4)
	{
		int bestScore = -1;
		auto considerController = [&](const CController& controller)
		{
			if (!controller.IsValid() || !controller.IsController() || controller.GetPrimaryFrequency() <= 0.0)
				return;
			const std::string callsign = controller.GetCallsign() != nullptr ? controller.GetCallsign() : "";
			const std::string position = controller.GetPositionId() != nullptr ? controller.GetPositionId() : "";
			if (!PdcControllerMatchesAirport(callsign, origin) && !PdcControllerMatchesAirport(position, origin))
				return;

			PdcControllerFacility facility = DetectPdcControllerFacility(callsign);
			if (facility == PdcControllerFacility::Unknown)
				facility = DetectPdcControllerFacility(position);
			int score = PdcFacilityClearancePriority(facility);
			if (score < 0)
				return;
			score += PdcRunwaySectorAffinity(origin, runway, callsign, position);
			if ((!myCallsign.empty() && ToUpperAsciiCopy(callsign) == ToUpperAsciiCopy(myCallsign)) ||
				(!myPosition.empty() && ToUpperAsciiCopy(position) == ToUpperAsciiCopy(myPosition)))
				score += 10;
			if (!coordinatedId.empty() &&
				(ToUpperAsciiCopy(callsign) == ToUpperAsciiCopy(coordinatedId) ||
				 ToUpperAsciiCopy(position) == ToUpperAsciiCopy(coordinatedId)))
				score += 5;

			if (score > bestScore ||
				(score == bestScore && ToUpperAsciiCopy(callsign) < ToUpperAsciiCopy(selection.controller)))
			{
				bestScore = score;
				selection.frequency = controller.GetPrimaryFrequency();
				selection.controller = callsign;
				selection.source = std::string("lowest connected origin ") + PdcFacilityName(facility);
			}
		};

		// ControllerSelectFirst normally includes the user's own position, but
		// considering it explicitly keeps the clearance frequency correct if it does not.
		considerController(myself);
		std::size_t controllerGuard = 0;
		for (CController controller = plugIn->ControllerSelectFirst();
			controller.IsValid() && controllerGuard < 4096;
			controller = plugIn->ControllerSelectNext(controller), ++controllerGuard)
		{
			considerController(controller);
		}
		if (bestScore >= 0)
			return selection;
	}

	if (coordinated.IsValid() && coordinated.GetPrimaryFrequency() > 0.0)
	{
		selection.frequency = coordinated.GetPrimaryFrequency();
		selection.controller = coordinated.GetCallsign() != nullptr ? coordinated.GetCallsign() : coordinatedId;
		selection.source = "coordinated controller fallback";
		return selection;
	}

	if (myself.IsValid() && myself.GetPrimaryFrequency() > 0.0)
	{
		selection.frequency = myself.GetPrimaryFrequency();
		selection.controller = myCallsign;
		selection.source = "own frequency fallback";
	}
	return selection;
}

bool ProtectHoppieCredential(
	const std::string& plaintext,
	std::string& protectedValue)
{
	protectedValue.clear();
	if (plaintext.empty())
		return true;
	if (plaintext.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)()))
		return false;

	DATA_BLOB input = {};
	input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
	input.cbData = static_cast<DWORD>(plaintext.size());
	DATA_BLOB entropy = HoppieCredentialEntropy();
	DATA_BLOB encrypted = {};
	if (!::CryptProtectData(
		&input,
		L"vSMR Hoppie code",
		&entropy,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&encrypted))
	{
		return false;
	}

	DWORD encodedCharacters = 0;
	const DWORD base64Flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
	bool succeeded = ::CryptBinaryToStringA(
		encrypted.pbData,
		encrypted.cbData,
		base64Flags,
		nullptr,
		&encodedCharacters) != FALSE;
	std::string encoded;
	if (succeeded && encodedCharacters > 0)
	{
		encoded.resize(encodedCharacters, '\0');
		succeeded = ::CryptBinaryToStringA(
			encrypted.pbData,
			encrypted.cbData,
			base64Flags,
			encoded.data(),
			&encodedCharacters) != FALSE;
		if (succeeded)
		{
			while (!encoded.empty() && encoded.back() == '\0')
				encoded.pop_back();
		}
	}
	if (encrypted.pbData != nullptr)
	{
		::SecureZeroMemory(encrypted.pbData, encrypted.cbData);
		::LocalFree(encrypted.pbData);
	}
	if (!succeeded || encoded.empty())
		return false;
	protectedValue = std::string(ProtectedCredentialPrefix) + encoded;
	return true;
}

bool UnprotectHoppieCredential(
	const std::string& storedValue,
	std::string& plaintext,
	bool& wasPlaintext)
{
	plaintext.clear();
	wasPlaintext = false;
	if (storedValue.empty())
		return true;

	const size_t prefixLength = strlen(ProtectedCredentialPrefix);
	if (storedValue.compare(0, prefixLength, ProtectedCredentialPrefix) != 0)
	{
		plaintext = TrimAsciiWhitespaceCopy(storedValue);
		wasPlaintext = !plaintext.empty();
		return true;
	}

	const std::string encoded = storedValue.substr(prefixLength);
	if (encoded.empty())
		return false;
	DWORD decodedBytes = 0;
	if (!::CryptStringToBinaryA(
		encoded.c_str(),
		static_cast<DWORD>(encoded.size()),
		CRYPT_STRING_BASE64,
		nullptr,
		&decodedBytes,
		nullptr,
		nullptr) || decodedBytes == 0)
	{
		return false;
	}

	std::vector<BYTE> decoded(decodedBytes);
	if (!::CryptStringToBinaryA(
		encoded.c_str(),
		static_cast<DWORD>(encoded.size()),
		CRYPT_STRING_BASE64,
		decoded.data(),
		&decodedBytes,
		nullptr,
		nullptr))
	{
		::SecureZeroMemory(decoded.data(), decoded.size());
		return false;
	}

	DATA_BLOB encrypted = {};
	encrypted.pbData = decoded.data();
	encrypted.cbData = decodedBytes;
	DATA_BLOB entropy = HoppieCredentialEntropy();
	DATA_BLOB output = {};
	LPWSTR description = nullptr;
	const bool succeeded = ::CryptUnprotectData(
		&encrypted,
		&description,
		&entropy,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&output) != FALSE;
	::SecureZeroMemory(decoded.data(), decoded.size());
	if (description != nullptr)
		::LocalFree(description);
	if (!succeeded)
		return false;

	plaintext.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
	if (output.pbData != nullptr)
	{
		::SecureZeroMemory(output.pbData, output.cbData);
		::LocalFree(output.pbData);
	}
	return true;
}

std::string EncodeUrlQueryComponent(const std::string& text)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string encoded;
	encoded.reserve(text.size());
	for (unsigned char c : text)
	{
		const bool isAsciiAlphaNumeric =
			(c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9');
		if (isAsciiAlphaNumeric || c == '-' || c == '_' || c == '.' || c == '~')
		{
			encoded.push_back(static_cast<char>(c));
			continue;
		}
		encoded.push_back('%');
		encoded.push_back(hex[(c >> 4) & 0x0F]);
		encoded.push_back(hex[c & 0x0F]);
	}
	return encoded;
}

bool IsHoppieOkResponse(const std::string& raw)
{
	const std::string normalized = NormalizeHoppieResponse(raw);
	if (normalized.size() < 2 ||
		std::tolower(static_cast<unsigned char>(normalized[0])) != 'o' ||
		std::tolower(static_cast<unsigned char>(normalized[1])) != 'k')
	{
		return false;
	}
	return normalized.size() == 2 ||
		std::isspace(static_cast<unsigned char>(normalized[2])) != 0 ||
		normalized[2] == '{';
}

std::string RedactSensitiveValue(std::string text, const std::string& secret)
{
	if (secret.empty())
		return text;
	size_t position = 0;
	while ((position = text.find(secret, position)) != std::string::npos)
	{
		text.replace(position, secret.size(), "<redacted>");
		position += strlen("<redacted>");
	}
	return text;
}

std::string BuildHoppieLoginFailureMessage(
	const std::string& raw,
	const std::string& password)
{
	std::string response = NormalizeHoppieResponse(raw);
	response = RedactSensitiveValue(response, password);
	response = RedactSensitiveValue(response, EncodeUrlQueryComponent(password));
	if (response.empty())
	{
		return "Connection failed: Hoppie returned no response. Check the network or proxy and try again.";
	}
	const size_t maximumResponseLength = 160;
	if (response.size() > maximumResponseLength)
		response = response.substr(0, maximumResponseLength) + "...";
	return "Hoppie rejected the connection: " + response;
}
}
