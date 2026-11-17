#pragma once

#include "integrations/CdmBridgeData.hpp"
#include "shared/JsonDocument.hpp"

#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace VsmrCdm
{
	inline constexpr std::size_t MaximumPanelBytes = 65536U;
	inline constexpr int ProxyActionBase = 100;

	struct AirportStatus
	{
		std::string icao;
		std::string master;
		bool local = false;
		bool pending = false;
		bool operator==(const AirportStatus& other) const
		{
			return icao == other.icao && master == other.master && local == other.local && pending == other.pending;
		}
	};

	struct AirportPanel
	{
		bool canControl = false;
		bool nativePanel = false;
		bool atfcm = false;
		bool bmi = false;
		std::string capacityAirport;
		std::vector<AirportStatus> airports;
		bool operator==(const AirportPanel& other) const
		{
			return canControl == other.canControl && nativePanel == other.nativePanel && atfcm == other.atfcm &&
				bmi == other.bmi && capacityAirport == other.capacityAirport && airports == other.airports;
		}
		bool operator!=(const AirportPanel& other) const { return !(*this == other); }
	};

	inline bool IsAirport(std::string_view value)
	{
		return value.size() == 4U && std::all_of(value.begin(), value.end(), [](unsigned char c) {
			return c >= 'A' && c <= 'Z';
		});
	}

	inline std::optional<AirportPanel> ParseAirportPanel(std::string_view input)
	{
		if (input.size() > MaximumPanelBytes) return {};
		rapidjson::Document document;
		VsmrJson::ParseDocument(document, input);
		if (document.HasParseError() || !document.IsObject() || !document.HasMember("version") ||
			!document["version"].IsInt() || document["version"].GetInt() != 1) return {};
		for (const char* key : { "can_control", "native_panel", "atfcm", "bmi" })
			if (!document.HasMember(key) || !document[key].IsBool()) return {};
		if (!document.HasMember("capacity_airport") || !document["capacity_airport"].IsString() ||
			!document.HasMember("airports") || !document["airports"].IsArray() || document["airports"].Size() > 512U) return {};
		AirportPanel panel;
		panel.canControl = document["can_control"].GetBool();
		panel.nativePanel = document["native_panel"].GetBool();
		panel.atfcm = document["atfcm"].GetBool();
		panel.bmi = document["bmi"].GetBool();
		panel.capacityAirport.assign(document["capacity_airport"].GetString(), document["capacity_airport"].GetStringLength());
		if (!panel.capacityAirport.empty() && !IsAirport(panel.capacityAirport)) return {};
		std::set<std::string> seen;
		for (const auto& entry : document["airports"].GetArray())
		{
			if (!entry.IsObject() || !entry.HasMember("icao") || !entry["icao"].IsString() ||
				!entry.HasMember("master") || !entry["master"].IsString() || !entry.HasMember("local") || !entry["local"].IsBool()) return {};
			AirportStatus airport;
			airport.icao.assign(entry["icao"].GetString(), entry["icao"].GetStringLength());
			airport.master.assign(entry["master"].GetString(), entry["master"].GetStringLength());
			airport.local = entry["local"].GetBool();
			if (entry.HasMember("pending"))
			{
				if (!entry["pending"].IsBool()) return {};
				airport.pending = entry["pending"].GetBool();
			}
			if (!IsAirport(airport.icao) || !seen.insert(airport.icao).second || airport.master.size() > 32U ||
				(!airport.master.empty() && NormalizeStringField(airport.master) != airport.master)) return {};
			panel.airports.push_back(std::move(airport));
		}
		return panel;
	}

	enum class CommandAction { Master, Slave, NativePanel, Atfcm, Capacity, Refresh };

	inline bool RequiresAirport(CommandAction action)
	{
		return action == CommandAction::Master || action == CommandAction::Slave || action == CommandAction::Capacity;
	}

	inline std::string BuildCommand(CommandAction action, const std::string& airport)
	{
		if (RequiresAirport(action) && !IsAirport(airport)) return {};
		switch (action)
		{
		case CommandAction::Master: return ".cdm master " + airport;
		case CommandAction::Slave: return ".cdm slave " + airport;
		case CommandAction::NativePanel: return ".cdm panel";
		case CommandAction::Atfcm: return ".cdm atfcm";
		case CommandAction::Capacity: return ".cdm capacity " + airport;
		case CommandAction::Refresh: return ".cdm refresh";
		}
		return {};
	}
}
