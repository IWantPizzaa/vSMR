#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagDataFormatting.hpp"
#include "radar/RadarUiSupport.hpp"
#include <cstdlib>

namespace
{
	using VsmrTags::TagDataInput;
	using VsmrTags::TokenValues;

	std::string FormatCallsign(const TagDataInput& input)
	{
		std::string callsign = input.callsign;
		if (!input.receivedFlightPlan) return callsign;
		const char assigned = input.assignedCommunication;
		const char filed = input.filedCommunication;
		const bool recognized = assigned == 't' || assigned == 'T' || assigned == 'r' || assigned == 'R' || assigned == 'v' || assigned == 'V';
		const char communication = recognized ? assigned : filed;
		if (communication == 't' || communication == 'T' || communication == 'r' || communication == 'R')
		{
			callsign += '/';
			callsign += communication;
		}
		switch (input.flightPlanState)
		{
		case EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED: return ">>" + callsign;
		case EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED: return callsign + ">>";
		case EuroScopePlugIn::FLIGHT_PLAN_STATE_ASSUMED: return "[" + callsign + "]";
		default: return callsign;
		}
	}

	void FormatFlightPlan(const TagDataInput& input, TokenValues& output)
	{
		const bool correlatedPlan = input.receivedFlightPlan && input.correlated;
		std::string type = input.receivedFlightPlan ? input.aircraftType : "NoFPL";
		if (type.size() > 4 && type != "NoFPL") type.resize(4);
		const bool squawkError = !input.assignedSquawk.empty() && !input.squawk.empty() &&
			!VsmrRadarUiSupport::startsWith(input.squawk.c_str(), input.assignedSquawk.c_str());
		const std::string error = squawkError ? "A" + input.assignedSquawk : "";
		output["actype"] = type;
		output["sctype"] = squawkError ? error : type;
		output["sqerror"] = error;
		output["wake"] = correlatedPlan ? std::string(1, input.wake) : "?";
		output["asid"] = correlatedPlan ? input.sid : "SID";
		std::string shortSid = output.at("asid");
		if (input.hasFlightPlan && input.correlated && shortSid.size() > 5)
			shortSid = shortSid.substr(0, 3) + shortSid.substr(shortSid.size() - 2);
		output["ssid"] = shortSid;
		output["origin"] = correlatedPlan ? input.origin : "????";
		output["dest"] = correlatedPlan ? input.destination : "????";
		output["groundstatus"] = correlatedPlan
			? (input.lineup ? "LNUP" : (input.groundState.empty() ? "STS" : input.groundState)) : "STS";
		output["clearance"] = input.hasFlightPlan && input.correlated ? (input.clearance ? "[x]" : "[ ]") : "";
		output["uk_stand"] = input.stand;
		output["remark"] = input.remark;
		output["scratchpad"] = input.scratchpad.empty() ? "..." : input.scratchpad;
		output["holdingpoint"] = input.holdingPoint;
	}

	void FormatPosition(const TagDataInput& input, TokenValues& output)
	{
		const std::string speed = std::to_string(input.groundSpeed);
		const std::string departure = input.receivedFlightPlan && !input.departureRunway.empty() ? input.departureRunway : "RWY";
		const std::string arrival = input.receivedFlightPlan && !input.arrivalRunway.empty() ? input.arrivalRunway : "RWY";
		output["deprwy"] = departure;
		output["seprwy"] = input.hasRadarTarget && input.groundSpeed > 25 ? speed : departure;
		output["arvrwy"] = arrival;
		output["srvrwy"] = input.hasRadarTarget && input.groundSpeed < 25 ? arrival : speed;
		std::string gate = input.hasFlightPlan ? input.scratchpad : "";
		VsmrRadarUiSupport::replaceAll(gate, "STAND=", "");
		if (gate.size() > 4) gate.resize(4);
		if (gate.empty() || gate == "0" || !input.correlated) gate = "NoGate";
		output["gate"] = gate;
		output["sate"] = input.hasRadarTarget && input.groundSpeed > 25 ? speed : gate;
		const bool belowTransition = input.flightLevel <= input.transitionAltitude;
		output["flightlevel"] = (std::string(belowTransition ? "A" : "") + VsmrRadarUiSupport::padWithZeros(
			belowTransition ? 4 : 5, belowTransition ? input.pressureAltitude : input.flightLevel)).substr(0, 3);
		output["tendency"] = std::abs(input.altitudeDelta) < 50 ? "-" : (input.altitudeDelta < 0 ? "|" : "^");
		output["gs"] = speed;
		output["ssr"] = input.squawk;
	}

	void ApplyProFallback(const TagDataInput& input, TokenValues& output)
	{
		if (!input.proMode || input.correlated) return;
		output["actype"] = "NoFPL";
		if (input.groundSpeed <= 50) return;
		output["callsign"] = input.squawk;
		if (input.primary)
		{
			output["flightlevel"] = "NoALT";
			output["tendency"] = "?";
			output["callsign"] = output.at("systemid");
		}
	}
}

void VsmrTags::FormatTagData(const TagDataInput& input, TokenValues& output)
{
	output.ResetValues();
	const std::string callsign = FormatCallsign(input);
	const std::string systemSource = callsign.empty() ? "000000" : callsign;
	output["systemid"] = "T:" + systemSource.substr(systemSource.size() > 1 ? 1 : 0, 6);
	output["callsign"] = callsign;
	FormatFlightPlan(input, output);
	FormatPosition(input, output);
	for (const char* key : { "aobt", "atot", "aort", "event_booking" }) output[key] = "";
	ApplyProFallback(input, output);
}
