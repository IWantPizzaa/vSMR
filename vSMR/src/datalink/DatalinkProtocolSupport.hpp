#pragma once

#include "datalink/PdcFrequencyPolicy.hpp"

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
	using PdcFrequencySelection = VsmrPdcFrequencyPolicy::Selection;

	std::string FormatPdcFrequency(double frequency);
	PdcFrequencySelection ResolvePdcNextFrequency(
		EuroScopePlugIn::CPlugIn* plugIn,
		const EuroScopePlugIn::CFlightPlan& flightPlan);

	bool ProtectDatalinkSecret(
		const std::string& plaintext,
		std::string& protectedValue);
	bool UnprotectDatalinkSecret(
		const std::string& storedValue,
		std::string& plaintext,
		bool& wasPlaintext);
	std::string EncodeUrlQueryComponent(const std::string& text);
	std::string RedactSensitiveValue(std::string text, const std::string& secret);
}
