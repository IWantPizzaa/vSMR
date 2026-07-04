#pragma once

#include "EuroScopePlugIn.h"
#include "SMRDataTypes.hpp"
#include "SMRTextUtils.hpp"

#include <cctype>
#include <ctime>
#include <string>

inline static std::string NormalizeVacdmLookupCallsign(const char* rawCallsign)
{
	if (rawCallsign == NULL)
		return "";

	std::string callsign = TrimAsciiWhitespaceCopy(rawCallsign);
	if (callsign.empty())
		return "";

	size_t slashPos = callsign.find('/');
	if (slashPos != std::string::npos)
		callsign = callsign.substr(0, slashPos);

	return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
}

// Resolve pilot data by trying both correlated flight plan and radar target callsigns.
// This keeps tag rendering resilient when one side has not correlated yet.
inline static bool TryGetVacdmPilotDataForTarget(
	const EuroScopePlugIn::CRadarTarget& rt,
	const EuroScopePlugIn::CFlightPlan& fp,
	VacdmPilotData& outData)
{
	const std::string fpCallsign = NormalizeVacdmLookupCallsign(fp.IsValid() ? fp.GetCallsign() : NULL);
	if (!fpCallsign.empty() && TryGetVacdmPilotData(fpCallsign, outData))
		return true;

	const std::string radarCallsign = NormalizeVacdmLookupCallsign(rt.GetCallsign());
	if (!radarCallsign.empty() && radarCallsign != fpCallsign && TryGetVacdmPilotData(radarCallsign, outData))
		return true;

	return false;
}

// Render UTC timestamps in HHMM format for tag tokens.
inline static std::string FormatVacdmTimeToken(std::time_t utcTime)
{
	if (utcTime <= 0)
		return "";

	std::tm tmUtc = {};
	if (gmtime_s(&tmUtc, &utcTime) != 0)
		return "";

	char buffer[5] = {};
	if (std::strftime(buffer, sizeof(buffer), "%H%M", &tmUtc) != 4)
		return "";
	return std::string(buffer);
}

inline static std::string NormalizeHhmmToken(const std::string& text)
{
	std::string digits;
	for (char c : text)
	{
		if (std::isdigit(static_cast<unsigned char>(c)))
			digits.push_back(c);
	}
	if (digits.empty())
		return "";
	if (digits.size() > 4)
		digits = digits.substr(0, 4);
	while (digits.size() < 4)
		digits.insert(digits.begin(), '0');
	return digits;
}

inline static bool TryResolveVacdmTobtTextColor(const VacdmPilotData& pilot, int& outR, int& outG, int& outB)
{
	if (!pilot.hasTobt)
		return false;

	const COLORREF lightGreen = RGB(127, 252, 73);
	const COLORREF green = RGB(0, 181, 27);
	const COLORREF lightYellow = RGB(255, 255, 191);
	const COLORREF yellow = RGB(255, 255, 0);
	const COLORREF orange = RGB(255, 153, 0);
	const COLORREF grey = RGB(153, 153, 153);
	const COLORREF debug = RGB(255, 0, 255);

	const auto setColor = [&](COLORREF color) {
		outR = GetRValue(color);
		outG = GetGValue(color);
		outB = GetBValue(color);
		};

	if (!pilot.hasTsat || pilot.hasAsat)
	{
		setColor(grey);
		return true;
	}

	std::time_t now = std::time(nullptr);
	const long long timeSinceTobt = static_cast<long long>(std::difftime(now, pilot.tobtUtc));
	const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));
	const long long diffTsatTobt = static_cast<long long>(std::difftime(pilot.tsatUtc, pilot.tobtUtc));
	const std::string tobtState = ToUpperAsciiCopy(pilot.tobtState);

	if ((timeSinceTobt > 0 && (timeSinceTsat >= 5 * 60 || !pilot.hasTsat)) ||
		pilot.tobtUtc >= now + 60 * 60)
	{
		setColor(orange);
		return true;
	}

	if (diffTsatTobt >= 5 * 60 && (tobtState == "GUESS" || tobtState == "FLIGHTPLAN"))
	{
		setColor(lightYellow);
		return true;
	}

	if (diffTsatTobt >= 5 * 60 && tobtState == "CONFIRMED")
	{
		setColor(yellow);
		return true;
	}

	if (diffTsatTobt < 5 * 60 && tobtState == "CONFIRMED")
	{
		setColor(green);
		return true;
	}

	if (tobtState != "CONFIRMED")
	{
		setColor(lightGreen);
		return true;
	}

	setColor(debug);
	return true;
}

inline static bool TryResolveVacdmTsatTextColor(const VacdmPilotData& pilot, int& outR, int& outG, int& outB)
{
	if (!pilot.hasTsat)
		return false;

	const COLORREF lightGreen = RGB(127, 252, 73);
	const COLORREF lightBlue = RGB(53, 218, 235);
	const COLORREF green = RGB(0, 181, 27);
	const COLORREF blue = RGB(0, 0, 255);
	const COLORREF orange = RGB(255, 153, 0);
	const COLORREF red = RGB(255, 0, 0);
	const COLORREF grey = RGB(153, 153, 153);
	const COLORREF debug = RGB(255, 0, 255);

	const auto setColor = [&](COLORREF color) {
		outR = GetRValue(color);
		outG = GetGValue(color);
		outB = GetBValue(color);
		};

	if (pilot.hasAsat)
	{
		setColor(grey);
		return true;
	}

	std::time_t now = std::time(nullptr);
	const long long timeSinceTsat = static_cast<long long>(std::difftime(now, pilot.tsatUtc));

	if (timeSinceTsat <= 5 * 60 && timeSinceTsat >= -5 * 60)
	{
		setColor(pilot.hasCtot ? blue : green);
		return true;
	}

	if (timeSinceTsat < -5 * 60)
	{
		setColor(pilot.hasCtot ? lightBlue : lightGreen);
		return true;
	}

	if (timeSinceTsat > 5 * 60)
	{
		setColor(pilot.hasCtot ? red : orange);
		return true;
	}

	setColor(debug);
	return true;
}
