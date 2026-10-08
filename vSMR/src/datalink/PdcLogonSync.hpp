#pragma once

#include <cstddef>
#include <string>

class PdcLogonSync
{
public:
	void MarkManualEdit() noexcept { manualOverride = true; }

	std::string Resolve(bool controllerConnected, int facility,
		const std::string& position, std::size_t avisoCount) const
	{
		// VATSIM facilities: 2 delivery, 3 ground, 4 tower.
		if (manualOverride || !controllerConnected || facility < 2 || facility > 4 ||
			avisoCount != 1 || position.size() < 6 || position[4] != '_')
			return {};
		std::string airport = position.substr(0, 4);
		for (char& c : airport)
		{
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			if (c < 'A' || c > 'Z') return {};
		}
		return airport;
	}

private:
	// Lifetime is the plugin session, deliberately not a network connection.
	bool manualOverride = false;
};
