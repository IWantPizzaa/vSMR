#pragma once

#include <string>

class PdcLogonSync
{
public:
	void MarkManualEdit() noexcept { manualOverride = true; }

	static std::string NormalizeAirport(const std::string& selectedAirport)
	{
		if (selectedAirport.size() != 4)
			return {};
		std::string airport = selectedAirport;
		for (char& c : airport)
		{
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			if (c < 'A' || c > 'Z') return {};
		}
		return airport;
	}

	std::string Resolve(const std::string& selectedAirport) const
	{
		return manualOverride ? std::string() : NormalizeAirport(selectedAirport);
	}

private:
	// A manual PDC login remains authoritative for the whole plugin session.
	bool manualOverride = false;
};
