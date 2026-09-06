#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace VsmrVsid
{
	inline constexpr std::size_t MaximumFieldBytes = 32U;

	enum class CommandAction
	{
		AutomaticModeStatus,
		AutomaticModeToggle,
		LfpgMinimumTaxiing,
		LfpgGroundCrossing,
		LfpgLinked,
		LfpgUnlinked,
		Synchronize,
		ReloadConfiguration
	};

	enum class LfpgOperatingMode
	{
		MinimumTaxiing,
		GroundCrossing
	};

	enum class LfpgLinkMode
	{
		Linked,
		Unlinked
	};

	struct AircraftData
	{
		std::string sid;
		std::string runway;
		std::string clearedFlightLevel;

		bool operator==(const AircraftData& other) const noexcept
		{
			return sid == other.sid &&
				runway == other.runway &&
				clearedFlightLevel == other.clearedFlightLevel;
		}

		bool operator!=(const AircraftData& other) const noexcept
		{
			return !(*this == other);
		}
	};

	inline std::string NormalizeFieldValue(std::string_view value)
	{
		if (value.size() > MaximumFieldBytes ||
			std::find(value.begin(), value.end(), '\0') != value.end())
		{
			return {};
		}

		std::size_t first = 0;
		std::size_t last = value.size();
		while (first < last && std::isspace(static_cast<unsigned char>(value[first])) != 0)
			++first;
		while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
			--last;

		for (std::size_t index = first; index < last; ++index)
		{
			const unsigned char character = static_cast<unsigned char>(value[index]);
			if (character < 0x20U || character == 0x7fU)
				return {};
		}
		return std::string(value.substr(first, last - first));
	}

	inline void AddTagTokens(
		std::map<std::string, std::string>& tokens,
		const AircraftData* data)
	{
		tokens["vsid_sid"] = data != nullptr ? data->sid : "";
		tokens["vsid_rwy"] = data != nullptr ? data->runway : "";
		tokens["vsid_cfl"] = data != nullptr ? data->clearedFlightLevel : "";
	}

	inline bool CommandRequiresAirport(CommandAction action) noexcept
	{
		return action == CommandAction::AutomaticModeToggle ||
			action == CommandAction::LfpgMinimumTaxiing ||
			action == CommandAction::LfpgGroundCrossing ||
			action == CommandAction::LfpgLinked ||
			action == CommandAction::LfpgUnlinked;
	}

	inline std::string NormalizeAirport(std::string_view airport)
	{
		std::size_t first = 0U;
		std::size_t last = airport.size();
		while (first < last &&
			std::isspace(static_cast<unsigned char>(airport[first])) != 0)
		{
			++first;
		}
		while (last > first &&
			std::isspace(static_cast<unsigned char>(airport[last - 1U])) != 0)
		{
			--last;
		}

		std::string normalized;
		for (std::size_t index = first; index < last; ++index)
		{
			const unsigned char character =
				static_cast<unsigned char>(airport[index]);
			if (character >= 'a' && character <= 'z')
				normalized.push_back(static_cast<char>(character - 'a' + 'A'));
			else if ((character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9'))
				normalized.push_back(static_cast<char>(character));
			else
				return {};
		}
		return normalized.size() == 4U ? normalized : std::string();
	}

	inline std::string BuildCommand(CommandAction action, std::string_view airport)
	{
		const std::string normalizedAirport = NormalizeAirport(airport);
		if (CommandRequiresAirport(action) && normalizedAirport.empty())
			return {};

		switch (action)
		{
		case CommandAction::AutomaticModeStatus:
			return ".vsid auto status";
		case CommandAction::AutomaticModeToggle:
			return ".vsid auto " + normalizedAirport;
		case CommandAction::LfpgMinimumTaxiing:
		case CommandAction::LfpgGroundCrossing:
			return normalizedAirport == "LFPG"
				? ".vsid rule LFPG opposing"
				: std::string();
		case CommandAction::LfpgLinked:
		case CommandAction::LfpgUnlinked:
			return normalizedAirport == "LFPG"
				? ".vsid rule LFPG linked"
				: std::string();
		case CommandAction::Synchronize:
			return ".vsid sync";
		case CommandAction::ReloadConfiguration:
			return ".vsid reload";
		default:
			return {};
		}
	}

	struct RuntimeActionDefinition
	{
		CommandAction action;
		const char* objectId;
		const char* label;
		const char* tooltip;
	};

	inline bool HasPublishedAircraftData(const AircraftData& data) noexcept
	{
		return !data.sid.empty() ||
			!data.runway.empty() ||
			!data.clearedFlightLevel.empty();
	}

	inline constexpr std::array<RuntimeActionDefinition, 1> AirportRuntimeActions = { {
		{ CommandAction::AutomaticModeToggle,
			"runtime.vsid.auto", "Toggle auto",
			"Toggle vSID automatic mode for the active airport" }
	} };

	inline constexpr std::array<RuntimeActionDefinition, 2> LfpgModeActions = { {
		{ CommandAction::LfpgMinimumTaxiing,
			"runtime.vsid.lfpg-minimum-taxiing", "Minimum Taxiing",
			"LFPG Minimum Taxiing mode (Roulage Mini)" },
		{ CommandAction::LfpgGroundCrossing,
			"runtime.vsid.lfpg-ground-crossing", "Ground Crossing",
			"LFPG Ground Crossing mode (Croisement au sol)" }
	} };

	inline constexpr std::array<RuntimeActionDefinition, 2> LfpgLinkActions = { {
		{ CommandAction::LfpgLinked,
			"runtime.vsid.lfpg-linked", "Linked",
			"LFPG Linked mode (Lie)" },
		{ CommandAction::LfpgUnlinked,
			"runtime.vsid.lfpg-unlinked", "Unlinked",
			"LFPG Unlinked mode (Non lie)" }
	} };

	inline constexpr std::array<RuntimeActionDefinition, 3> GeneralRuntimeActions = { {
		{ CommandAction::AutomaticModeStatus,
			"runtime.vsid.auto-status", "Auto status",
			"Show vSID automatic-mode status" },
		{ CommandAction::Synchronize,
			"runtime.vsid.sync", "Synchronize",
			"Synchronize vSID with the network" },
		{ CommandAction::ReloadConfiguration,
			"runtime.vsid.reload", "Reload config",
			"Reload vSID configuration files" }
	} };

	inline bool TryParseRuntimeActionId(
		std::string_view objectId,
		CommandAction& outAction) noexcept
	{
		auto findAction = [&](const auto& definitions)
		{
			for (const RuntimeActionDefinition& definition : definitions)
			{
				if (definition.objectId == objectId)
				{
					outAction = definition.action;
					return true;
				}
			}
			return false;
		};
		return findAction(AirportRuntimeActions) ||
			findAction(LfpgModeActions) ||
			findAction(LfpgLinkActions) ||
			findAction(GeneralRuntimeActions);
	}

}
