#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "radar/RadarScreen.Registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "rapidjson/document.h"

namespace
{
	std::mutex LineupOverrideMutex;
	std::map<std::string, std::chrono::steady_clock::time_point> LineupOverrides;
	std::mutex HoldingPointEditMutex;
	std::string PendingHoldingPointCallsign;
	using HoldingPointRunways = std::map<std::string, std::vector<std::string>>;
	std::mutex HoldingPointCatalogMutex;
	std::map<std::string, HoldingPointRunways> HoldingPointCatalog;

	std::string NormalizeLineupCallsign(const char* callsign)
	{
		std::string normalized = callsign != nullptr ? callsign : "";
		normalized.erase(
			std::remove_if(normalized.begin(), normalized.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
			normalized.end());
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return normalized;
	}

	std::string NormalizeHoldingPointCatalogKey(const std::string& value)
	{
		std::string normalized = VsmrHoldingPoint::Trim(value);
		normalized.erase(
			std::remove_if(
				normalized.begin(), normalized.end(),
				[](unsigned char character) { return std::isspace(character) != 0; }),
			normalized.end());
		std::transform(
			normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char character) { return static_cast<char>(std::toupper(character)); });
		if (normalized.size() > 3 && normalized.compare(0, 3, "RWY") == 0)
			normalized.erase(0, 3);
		return normalized;
	}

	void LoadHoldingPointCatalog(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			Logger::info("Holding-point catalog not found: " + path.u8string());
			return;
		}

		std::stringstream buffer;
		buffer << input.rdbuf();
		const std::string json = buffer.str();
		rapidjson::Document document;
		if (document.Parse<0>(json.c_str()).HasParseError() || !document.IsObject())
		{
			Logger::info("Holding-point catalog is invalid: " + path.u8string());
			return;
		}

		std::map<std::string, HoldingPointRunways> loaded;
		for (auto airport = document.MemberBegin(); airport != document.MemberEnd(); ++airport)
		{
			if (!airport->name.IsString() || !airport->value.IsObject())
				continue;
			const std::string airportKey = NormalizeHoldingPointCatalogKey(airport->name.GetString());
			if (airportKey.empty())
				continue;

			HoldingPointRunways runways;
			for (auto runway = airport->value.MemberBegin(); runway != airport->value.MemberEnd(); ++runway)
			{
				if (!runway->name.IsString() || !runway->value.IsArray())
					continue;
				const std::string runwayKey = NormalizeHoldingPointCatalogKey(runway->name.GetString());
				if (runwayKey.empty())
					continue;

				std::vector<std::string> points;
				for (rapidjson::SizeType index = 0; index < runway->value.Size(); ++index)
				{
					const rapidjson::Value& entry = runway->value[index];
					if (!entry.IsString())
						continue;
					std::string point;
					if (VsmrHoldingPoint::Normalize(entry.GetString(), point) &&
						!point.empty() &&
						std::find(points.begin(), points.end(), point) == points.end())
					{
						points.push_back(std::move(point));
					}
				}
				if (!points.empty())
					runways.emplace(runwayKey, std::move(points));
			}
			if (!runways.empty())
				loaded.emplace(airportKey, std::move(runways));
		}

		{
			std::lock_guard<std::mutex> guard(HoldingPointCatalogMutex);
			HoldingPointCatalog = std::move(loaded);
		}
		Logger::info("Loaded holding-point catalog: " + path.u8string());
	}

	std::vector<std::string> HoldingPointsForFlightPlan(const CFlightPlan& flightPlan)
	{
		if (!flightPlan.IsValid())
			return {};
		const char* airportRaw = flightPlan.GetFlightPlanData().GetOrigin();
		const char* runwayRaw = flightPlan.GetFlightPlanData().GetDepartureRwy();
		const std::string airport = NormalizeHoldingPointCatalogKey(airportRaw != nullptr ? airportRaw : "");
		const std::string runway = NormalizeHoldingPointCatalogKey(runwayRaw != nullptr ? runwayRaw : "");
		if (airport.empty() || runway.empty())
			return {};

		std::lock_guard<std::mutex> guard(HoldingPointCatalogMutex);
		const auto airportIt = HoldingPointCatalog.find(airport);
		if (airportIt == HoldingPointCatalog.end())
			return {};
		const auto runwayIt = airportIt->second.find(runway);
		return runwayIt != airportIt->second.end() ? runwayIt->second : std::vector<std::string>();
	}}

bool VsmrGroundState::SetLineupOverride(const char* callsign)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return false;
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides[normalized] = std::chrono::steady_clock::now();
	return true;
}

void VsmrGroundState::ClearLineupOverride(const char* callsign)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return;
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides.erase(normalized);
}

void VsmrGroundState::ClearAllLineupOverrides()
{
	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	LineupOverrides.clear();
}

bool VsmrGroundState::IsLineupOverrideActive(const char* callsign, GroundStateCategory observedCategory)
{
	const std::string normalized = NormalizeLineupCallsign(callsign);
	if (normalized.empty())
		return false;

	std::lock_guard<std::mutex> guard(LineupOverrideMutex);
	const auto overrideIt = LineupOverrides.find(normalized);
	if (overrideIt == LineupOverrides.end())
		return false;

	if (observedCategory == GroundStateCategory::Taxi || observedCategory == GroundStateCategory::Lnup)
		return true;

	// SetScratchPadString("TAXI") updates EuroScope asynchronously on some
	// installations. Keep the local state briefly, then fail safely if the
	// host never reports TAXI or another controller changes the status.
	if (std::chrono::steady_clock::now() - overrideIt->second < std::chrono::seconds(2))
		return true;

	LineupOverrides.erase(overrideIt);
	return false;
}


void CSMRPlugin::LoadHoldingPointCatalog(const std::string& path)
{
	::LoadHoldingPointCatalog(std::filesystem::u8path(path));
}

bool CSMRPlugin::HandleHoldingPointFunctionCall(
	int FunctionId,
	const char* sItemString,
	RECT Area)
{
	// ----- Handling holding point actions -----
	auto applyHoldingPoint = [&](const std::string& callsign, const std::string& input) -> bool
	{
		std::string holdingPoint;
		std::string error;
		if (!VsmrHoldingPoint::Normalize(input, holdingPoint, &error))
		{
			DisplayUserMessage("vSMR", "Holding Point", error.c_str(), true, true, false, false, false);
			return false;
		}

		CFlightPlan flightPlan = FlightPlanSelect(callsign.c_str());
		if (!flightPlan.IsValid())
		{
			DisplayUserMessage("vSMR", "Holding Point", "The selected flight plan is no longer available.", true, true, false, false, false);
			return false;
		}

		CFlightPlanData flightPlanData = flightPlan.GetFlightPlanData();
		const char* rawRemarks = flightPlanData.GetRemarks();
		const std::string currentRemarks = rawRemarks != nullptr ? rawRemarks : "";
		const std::string updatedRemarks = VsmrHoldingPoint::Write(currentRemarks, holdingPoint);
		if (updatedRemarks != currentRemarks &&
			(!flightPlanData.SetRemarks(updatedRemarks.c_str()) || !flightPlanData.AmendFlightPlan()))
		{
			DisplayUserMessage(
				"vSMR",
				"Holding Point",
				"EuroScope rejected the flight plan remarks update. Shorten the remarks or check that you can amend this flight plan.",
				true, true, false, false, false);
			return false;
		}
		VsmrHoldingPoint::RememberPending(callsign, holdingPoint);

		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar != nullptr && !radar->IsShutdownRequested())
				radar->RequestRefresh();
		}
		return true;
	};

	if (FunctionId == TAG_FUNC_HOLDING_POINT_EDIT)
	{
		CFlightPlan flightPlan = FlightPlanSelectASEL();
		if (!flightPlan.IsValid())
		{
			DisplayUserMessage("vSMR", "Holding Point", "Select a correlated aircraft first.", true, true, false, false, false);
			return true;
		}

		const char* callsign = flightPlan.GetCallsign();
		if (callsign == nullptr || callsign[0] == '\0')
			return true;
		{
			std::lock_guard<std::mutex> guard(HoldingPointEditMutex);
			PendingHoldingPointCallsign = callsign;
		}

		const char* rawRemarks = flightPlan.GetFlightPlanData().GetRemarks();
		const std::string holdingPoint = VsmrHoldingPoint::Resolve(
			callsign,
			rawRemarks != nullptr ? rawRemarks : "");
		const char* runwayRaw = flightPlan.GetFlightPlanData().GetDepartureRwy();
		const std::string runway = NormalizeHoldingPointCatalogKey(runwayRaw != nullptr ? runwayRaw : "");
		const std::vector<std::string> holdingPoints = HoldingPointsForFlightPlan(flightPlan);
		const std::string title = runway.empty() ? "Holding point" : "Holding point RWY " + runway;
		OpenPopupList(Area, title.c_str(), 1);
		AddPopupListElement("[...]", "", TAG_FUNC_HOLDING_POINT_MANUAL, false);
		AddPopupListElement("None", "", TAG_FUNC_HOLDING_POINT_CLEAR, holdingPoint.empty());
		for (const std::string& point : holdingPoints)
			AddPopupListElement(point.c_str(), "", TAG_FUNC_HOLDING_POINT_SELECT, point == holdingPoint);
		return true;
	}

	if (FunctionId == TAG_FUNC_HOLDING_POINT_MANUAL)
	{
		std::string callsign;
		{
			std::lock_guard<std::mutex> guard(HoldingPointEditMutex);
			callsign = PendingHoldingPointCallsign;
		}
		if (callsign.empty())
			return true;
		CFlightPlan flightPlan = FlightPlanSelect(callsign.c_str());
		if (!flightPlan.IsValid())
			return true;
		const char* rawRemarks = flightPlan.GetFlightPlanData().GetRemarks();
		const std::string holdingPoint = VsmrHoldingPoint::Resolve(
			callsign,
			rawRemarks != nullptr ? rawRemarks : "");
		OpenPopupEdit(Area, TAG_FUNC_HOLDING_POINT_COMMIT, holdingPoint.c_str());
		return true;
	}

	if (FunctionId == TAG_FUNC_HOLDING_POINT_SELECT ||
		FunctionId == TAG_FUNC_HOLDING_POINT_CLEAR ||
		FunctionId == TAG_FUNC_HOLDING_POINT_COMMIT)
	{
		std::string callsign;
		{
			std::lock_guard<std::mutex> guard(HoldingPointEditMutex);
			callsign.swap(PendingHoldingPointCallsign);
		}
		if (callsign.empty())
			return true;
		const char* value = FunctionId == TAG_FUNC_HOLDING_POINT_CLEAR
			? ""
			: (sItemString != nullptr ? sItemString : "");
		(void)applyHoldingPoint(callsign, value);
		return true;
	}


	return false;
}
