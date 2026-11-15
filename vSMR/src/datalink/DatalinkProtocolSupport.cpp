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

	DATA_BLOB DatalinkSecretEntropy()
	{
		static char entropy[] = "vSMR CPDLC credential v1";
		DATA_BLOB blob = {};
		blob.pbData = reinterpret_cast<BYTE*>(entropy);
		blob.cbData = static_cast<DWORD>(strlen(entropy));
		return blob;
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
	if (plugIn == nullptr || !flightPlan.IsValid())
		return {};

	const char* originRaw = flightPlan.GetFlightPlanData().GetOrigin();
	const std::string origin = originRaw != nullptr ? originRaw : "";
	const char* runwayRaw = flightPlan.GetFlightPlanData().GetDepartureRwy();
	const std::string runway = runwayRaw != nullptr ? runwayRaw : "";
	const CController myself = plugIn->ControllerMyself();
	const std::string myCallsign = myself.IsValid() && myself.GetCallsign() != nullptr
		? ToUpperAsciiCopy(myself.GetCallsign()) : "";
	const std::string myPosition = myself.IsValid() && myself.GetPositionId() != nullptr
		? ToUpperAsciiCopy(myself.GetPositionId()) : "";
	const char* coordinatedRaw = flightPlan.GetCoordinatedNextController();
	const std::string coordinatedId = ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(
		coordinatedRaw != nullptr ? coordinatedRaw : ""));

	std::vector<VsmrPdcFrequencyPolicy::Controller> controllers;
	auto snapshot = [&](const CController& controller)
	{
		if (!controller.IsValid() || !controller.IsController())
			return;
		VsmrPdcFrequencyPolicy::Controller candidate;
		candidate.callsign = controller.GetCallsign() != nullptr ? controller.GetCallsign() : "";
		candidate.positionId = controller.GetPositionId() != nullptr ? controller.GetPositionId() : "";
		candidate.frequency = controller.GetPrimaryFrequency();
		const std::string callsign = ToUpperAsciiCopy(candidate.callsign);
		const std::string position = ToUpperAsciiCopy(candidate.positionId);
		candidate.isOwn = (!myCallsign.empty() && callsign == myCallsign) ||
			(!myPosition.empty() && position == myPosition);
		candidate.isCoordinated = !coordinatedId.empty() &&
			(callsign == coordinatedId || position == coordinatedId);
		controllers.push_back(std::move(candidate));
	};
	// Some SDK enumerations omit the user's station or the coordinated one.
	// Include actual snapshots explicitly; the pure policy still requires origin.
	snapshot(myself);
	if (!coordinatedId.empty())
		snapshot(plugIn->ControllerSelect(coordinatedId.c_str()));
	std::size_t controllerGuard = 0;
	for (CController controller = plugIn->ControllerSelectFirst();
		controller.IsValid() && controllerGuard < 4096;
		controller = plugIn->ControllerSelectNext(controller), ++controllerGuard)
	{
		snapshot(controller);
	}
	return VsmrPdcFrequencyPolicy::Resolve(origin, runway, controllers);
}

bool ProtectDatalinkSecret(
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
	DATA_BLOB entropy = DatalinkSecretEntropy();
	DATA_BLOB encrypted = {};
	if (!::CryptProtectData(
		&input,
		L"vSMR PDC key",
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

bool UnprotectDatalinkSecret(
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
	DATA_BLOB entropy = DatalinkSecretEntropy();
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
}
