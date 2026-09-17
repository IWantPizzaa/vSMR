// Included by vSIDPlugin.cpp; see vsid-paris.patch for the class declarations.
#include "ParisRunwayRules.hpp"

void vsid::VSIDPlugin::PublishParisRules()
{
	using namespace VsmrParis;
	std::string snapshot;
	for (const auto& [icao, airport] : activeAirports)
	{
		if (!Supports(icao) || !airport.customRules.contains("linked")) continue;
		const State state = Resolve(airport.customRules, icao);
		snapshot += Serialize(icao, state);
	}
	if (bridgeApi_ != nullptr && bridgeProvider_ != nullptr)
	{
		const ESB_Value value = ESB_Str(snapshot.c_str());
		bridgeApi_->set_global(bridgeProvider_, bridgeParisField_, &value);
	}
}

bool vsid::VSIDPlugin::HandleParisCommand(const vsid::Command& cmd)
{
	if (!vsid::utils::svEqualCi(cmd.command, "paris")) return false;
	if (cmd.params.size() != 2)
	{
		vsid::Logger::log(LogLevel::Info, "Usage: .vsid paris ICAO linked|unlinked|wlpg|elpg|wipg|eipg");
		return true;
	}
	const auto found = activeAirports.find(cmd.params[0]);
	if (found == activeAirports.end() || !VsmrParis::Supports(found->first) ||
		!found->second.customRules.contains("linked"))
	{
		vsid::Logger::log(LogLevel::Info, "Paris rules require an active airport with the Paris configuration installed.");
		return true;
	}
	std::string selection(cmd.params[1]);
	for (char& c : selection) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	auto& rules = found->second.customRules;
	const auto before = rules;
	if (!VsmrParis::Select(rules, found->first, selection))
	{
		vsid::Logger::log(LogLevel::Info, "Use linked|unlinked at LFPG/LFPO, or wlpg|elpg|wipg|eipg at LFPN/LFPV/LFPT/LFOB.");
		return true;
	}
	const bool changed = rules != before;
	PublishParisRules();
	if (changed) ReprocessCustomRules();
	vsid::Logger::log(LogLevel::Info, std::format("[{}] Paris runway rules: {}", found->first,
		selection + " (manual)"));
	return true;
}
