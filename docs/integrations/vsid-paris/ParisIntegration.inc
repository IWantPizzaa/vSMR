// Included by vSIDPlugin.cpp; see vsid-paris.patch for the class declarations.
#include "ParisRunwayRules.hpp"

bool vsid::VSIDPlugin::UpdateParisRules()
{
	using namespace VsmrParis;
	Flow pg = Flow::Unknown;
	Flow po = Flow::Unknown;
	// Read both arrival and departure selections even when PG/PO aren't active
	// vSID airports (a regional controller may only have their own airport active).
	for (auto runway = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY);
		runway.IsValid(); runway = SectorFileElementSelectNext(runway, EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY))
	{
		const char* airport = runway.GetAirportName();
		if (airport == nullptr || (std::string_view(airport) != "LFPG" && std::string_view(airport) != "LFPO")) continue;
		for (int end = 0; end != 2; ++end)
		{
			if (!runway.IsElementActive(true, end) && !runway.IsElementActive(false, end)) continue;
			const char* name = runway.GetRunwayName(end);
			Flow& flow = std::string_view(airport) == "LFPG" ? pg : po;
			flow = MergeFlow(flow, name == nullptr ? Flow::Mixed : RunwayFlow(airport, name));
		}
	}
	bool changed = false;
	std::string snapshot;
	for (auto& [icao, airport] : activeAirports)
	{
		if (!Supports(icao) || !airport.customRules.contains("paris_auto")) continue;
		const State state = Resolve(airport.customRules, pg, po);
		changed = Apply(airport.customRules, state) || changed;
		snapshot += Serialize(icao, state);
	}
	if (bridgeApi_ != nullptr && bridgeProvider_ != nullptr)
	{
		const ESB_Value value = ESB_Str(snapshot.c_str());
		bridgeApi_->set_global(bridgeProvider_, bridgeParisField_, &value);
	}
	return changed;
}

bool vsid::VSIDPlugin::HandleParisCommand(const vsid::Command& cmd)
{
	if (!vsid::utils::svEqualCi(cmd.command, "paris")) return false;
	if (cmd.params.size() != 2)
	{
		vsid::Logger::log(LogLevel::Info, "Usage: .vsid paris ICAO linked|unlinked|auto");
		return true;
	}
	const auto found = activeAirports.find(cmd.params[0]);
	if (found == activeAirports.end() || !VsmrParis::Supports(found->first) ||
		!found->second.customRules.contains("paris_auto"))
	{
		vsid::Logger::log(LogLevel::Info, "Paris rules require an active airport with the Paris configuration installed.");
		return true;
	}
	const bool automatic = vsid::utils::svEqualCi(cmd.params[1], "auto");
	const bool linked = vsid::utils::svEqualCi(cmd.params[1], "linked");
	if (!automatic && !linked && !vsid::utils::svEqualCi(cmd.params[1], "unlinked"))
	{
		vsid::Logger::log(LogLevel::Info, "Usage: .vsid paris ICAO linked|unlinked|auto");
		return true;
	}
	auto& rules = found->second.customRules;
	bool changed = rules["paris_auto"] != automatic;
	rules["paris_auto"] = automatic;
	if (!automatic)
	{
		changed = changed || rules["linked"] != linked || rules["unlinked"] == linked;
		rules["linked"] = linked;
		rules["unlinked"] = !linked;
	}
	changed = UpdateParisRules() || changed;
	if (changed) ReprocessCustomRules();
	vsid::Logger::log(LogLevel::Info, std::format("[{}] Paris runway rules: {}", found->first,
		automatic ? "AUTO" : linked ? "LINKED (manual)" : "UNLINKED (manual)"));
	return true;
}
