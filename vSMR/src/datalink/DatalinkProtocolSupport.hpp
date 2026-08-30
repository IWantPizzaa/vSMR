#pragma once

#include <string>

namespace EuroScopePlugIn
{
	class CFlightPlan;
	class CPlugIn;
}

// These helpers contain protocol and credential rules only. Connection state
// and worker lifetime remain owned by the plug-in coordinator.
namespace VsmrDatalinkProtocol
{
	struct PdcFrequencySelection
	{
		double frequency = 0.0;
		std::string controller;
		std::string source;
	};

	std::string FormatPdcFrequency(double frequency);
	PdcFrequencySelection ResolvePdcNextFrequency(
		EuroScopePlugIn::CPlugIn* plugIn,
		const EuroScopePlugIn::CFlightPlan& flightPlan);

	bool ProtectHoppieCredential(
		const std::string& plaintext,
		std::string& protectedValue);
	bool UnprotectHoppieCredential(
		const std::string& storedValue,
		std::string& plaintext,
		bool& wasPlaintext);
	std::string EncodeUrlQueryComponent(const std::string& text);
	bool IsHoppieOkResponse(const std::string& raw);
	std::string RedactSensitiveValue(std::string text, const std::string& secret);
	std::string BuildHoppieLoginFailureMessage(
		const std::string& raw,
		const std::string& password);
}
