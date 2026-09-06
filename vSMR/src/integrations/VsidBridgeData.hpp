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
		LowVisibilityToggle,
		NightModeToggle,
		ListAreas,
		ListRules,
		ListRequests,
		Synchronize,
		ReloadConfiguration,
		ReloadEse
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
			action == CommandAction::LowVisibilityToggle ||
			action == CommandAction::NightModeToggle ||
			action == CommandAction::ListAreas ||
			action == CommandAction::ListRules ||
			action == CommandAction::ListRequests;
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
		case CommandAction::LowVisibilityToggle:
			return ".vsid lvp " + normalizedAirport;
		case CommandAction::NightModeToggle:
			return ".vsid night " + normalizedAirport;
		case CommandAction::ListAreas:
			return ".vsid area " + normalizedAirport;
		case CommandAction::ListRules:
			return ".vsid rule " + normalizedAirport;
		case CommandAction::ListRequests:
			return ".vsid req " + normalizedAirport;
		case CommandAction::Synchronize:
			return ".vsid sync";
		case CommandAction::ReloadConfiguration:
			return ".vsid reload";
		case CommandAction::ReloadEse:
			return ".vsid reload ese";
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

	inline constexpr std::array<RuntimeActionDefinition, 6> AirportRuntimeActions = { {
		{ CommandAction::AutomaticModeToggle,
			"runtime.vsid.auto", "Toggle auto",
			"Toggle vSID automatic mode for the active airport" },
		{ CommandAction::LowVisibilityToggle,
			"runtime.vsid.lvp", "Toggle LVP",
			"Toggle vSID low-visibility mode for the active airport" },
		{ CommandAction::NightModeToggle,
			"runtime.vsid.night", "Toggle night",
			"Toggle vSID night mode for the active airport" },
		{ CommandAction::ListAreas,
			"runtime.vsid.areas", "List areas",
			"Show vSID areas for the active airport" },
		{ CommandAction::ListRules,
			"runtime.vsid.rules", "List rules",
			"Show vSID rules for the active airport" },
		{ CommandAction::ListRequests,
			"runtime.vsid.requests", "List requests",
			"Show vSID requests for the active airport" }
	} };

	inline constexpr std::array<RuntimeActionDefinition, 4> GeneralRuntimeActions = { {
		{ CommandAction::AutomaticModeStatus,
			"runtime.vsid.auto-status", "Auto status",
			"Show vSID automatic-mode status" },
		{ CommandAction::Synchronize,
			"runtime.vsid.sync", "Synchronize",
			"Synchronize vSID with the network" },
		{ CommandAction::ReloadConfiguration,
			"runtime.vsid.reload", "Reload config",
			"Reload vSID configuration files" },
		{ CommandAction::ReloadEse,
			"runtime.vsid.reload-ese", "Reload ESE",
			"Reload vSID sector-file data" }
	} };
	static_assert(AirportRuntimeActions.size() % 2U == 0U);
	static_assert(GeneralRuntimeActions.size() % 2U == 0U);

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
			findAction(GeneralRuntimeActions);
	}

	inline const char* ActionDescription(CommandAction action) noexcept
	{
		switch (action)
		{
		case CommandAction::AutomaticModeStatus:
			return "automatic-mode status";
		case CommandAction::AutomaticModeToggle:
			return "automatic mode toggle";
		case CommandAction::LowVisibilityToggle:
			return "low-visibility mode toggle";
		case CommandAction::NightModeToggle:
			return "night mode toggle";
		case CommandAction::ListAreas:
			return "area list";
		case CommandAction::ListRules:
			return "rule list";
		case CommandAction::ListRequests:
			return "request list";
		case CommandAction::Synchronize:
			return "synchronization";
		case CommandAction::ReloadConfiguration:
			return "configuration reload";
		case CommandAction::ReloadEse:
			return "ESE reload";
		default:
			return "action";
		}
	}
}
