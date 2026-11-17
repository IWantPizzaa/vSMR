#include "PinedeProtocolTests.hpp"

#include "datalink/PinedeProtocol.hpp"
#include "datalink/PdcFrequencyPolicy.hpp"
#include "platform/windows/network/HttpHelper.hpp"

#include "rapidjson/document.h"

#include <algorithm>
#include <set>
#include <limits>
#include <string>

namespace
{
	void Check(
		bool condition,
		const char* message,
		std::vector<std::string>& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}

	bool Contains(const std::vector<std::string>& list, const std::string& callsign)
	{
		return std::find(list.begin(), list.end(), callsign) != list.end();
	}

	std::string Member(const std::string& json, const char* key)
	{
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject() || !document.HasMember(key) || !document[key].IsString())
			return "<absent>";
		return document[key].GetString();
	}

	const char* kPoll = R"({
		"airports": ["LFPG", "LFPB"],
		"requests": [
			{"callsign": "AFR123", "departure": "LFPG", "destination": "LFMN", "request_text": "REQUEST PREDEP CLEARANCE AFR123", "status": "PENDING", "claimed_by": null, "pilot_message": null},
			{"callsign": "BAW1", "departure": "LFPG", "destination": "EGLL", "request_text": "PDC REQUEST", "status": "STANDBY", "claimed_by": "LFPG_DEL", "pilot_message": "STAND K47"},
			{"callsign": "dlh4", "departure": "LFPG", "destination": "EDDF", "request_text": "PDC", "status": "CLEARED", "claimed_by": "LFPG_DEL", "pilot_message": null},
			{"callsign": "EZY5", "departure": "LFPG", "destination": "EGKK", "request_text": "PDC", "status": "ACKED", "claimed_by": "LFPG_DEL", "pilot_message": null},
			{"callsign": "RYR6", "departure": "LFPG", "status": "SOMETHING_NEW"},
			{"departure": "LFPG", "status": "PENDING"},
			"not an object"
		]
	})";
}

std::vector<std::string> RunPinedeProtocolTests()
{
	using namespace VsmrPinede;
	std::vector<std::string> failures;

	// ----- Departure-airport frequency selection, independent of SDK enumeration -----
	using FrequencyController = VsmrPdcFrequencyPolicy::Controller;
	const auto resolveFrequency = [](const std::string& origin, const std::string& runway,
		const std::vector<FrequencyController>& controllers)
	{
		return VsmrPdcFrequencyPolicy::Resolve(origin, runway, controllers);
	};
	std::vector<FrequencyController> controllers = {
		{ "LFPG_APP", "PAR", 126.650, true, true, true },
		{ "LFPO_GND", "POG", 121.705 },
		{ "LFPG_GND", "PGG", 121.605 },
	};
	auto frequency = resolveFrequency("LFPO", "24", controllers);
	Check(frequency.controller == "LFPO_GND" && frequency.frequency == 121.705,
		"PDC frequency: LFPG issuer/coordinated PAR does not replace the LFPO departure ground", failures);
	frequency = resolveFrequency("LFPG", "26R", controllers);
	Check(frequency.controller == "LFPG_GND" && frequency.frequency == 121.605,
		"PDC frequency: changing the flight's origin selects its own airport", failures);
	std::reverse(controllers.begin(), controllers.end());
	Check(resolveFrequency("LFPO", "24", controllers).controller == "LFPO_GND",
		"PDC frequency: connected-controller enumeration order does not change the airport", failures);
	Check(resolveFrequency("LFPO", "24", { controllers.back() }).controller.empty(),
		"PDC frequency: no LFPO candidate leaves frequency empty instead of using LFPG/PAR", failures);
	Check(resolveFrequency("", "", controllers).frequency == 0.0 &&
		resolveFrequency("LFPO_EXTRA", "24", controllers).frequency == 0.0,
		"PDC frequency: missing or malformed origin never enables a station fallback", failures);
	Check(resolveFrequency(" lfpo ", "24", controllers).controller == "LFPO_GND",
		"PDC frequency: origin is normalized without consulting the issuer's airport", failures);
	Check(resolveFrequency("LFPO", "24", { { "LFPOX_GND", "PAR", 121.705 } }).frequency == 0.0,
		"PDC frequency: a longer airport prefix is not the departure airport", failures);
	Check(resolveFrequency("LFPO", "24", { { "LOCAL", "LFPO_GND", 121.705 } }).controller == "LOCAL",
		"PDC frequency: an explicit departure-airport position identity is accepted", failures);

	std::vector<FrequencyController> facilities = {
		{ "LFPO_DEL", "POD", 121.505 }, { "LFPO_RMP", "POR", 121.510 },
		{ "LFPO_GND", "POG", 121.705 }, { "LFPO_TWR", "POT", 118.705 },
		{ "LFPO_APP", "POA", 123.875 }, { "LFPO_CTR", "POC", 125.000 },
	};
	const char* expectedFacilities[] = { "LFPO_DEL", "LFPO_RMP", "LFPO_GND", "LFPO_TWR", "LFPO_APP", "LFPO_CTR" };
	for (const char* expected : expectedFacilities)
	{
		Check(resolveFrequency("LFPO", "24", facilities).controller == expected,
			"PDC frequency: DEL/RMP/GND/TWR/APP/CTR priority is preserved", failures);
		facilities.erase(facilities.begin());
	}
	Check(resolveFrequency("LFPO", "24", { { "LFPO_DEP", "POD", 123.875 } }).controller == "LFPO_DEP",
		"PDC frequency: a connected departure position is eligible for the departure airport", failures);
	const double nan = (std::numeric_limits<double>::quiet_NaN)();
	const double infinity = (std::numeric_limits<double>::infinity)();
	for (double invalid : { 0.0, -1.0, nan, infinity, 117.995, 137.000, 199.998 })
	{
		Check(resolveFrequency("LFPO", "24", {
			{ "LFPO_DEL", "POD", invalid }, { "LFPO_GND", "POG", 121.705 }
		}).controller == "LFPO_GND",
			"PDC frequency: invalid/unknown primary frequency cannot mask a valid local controller", failures);
	}
	Check(resolveFrequency("LFPO", "24", {
		{ "LFPO_DEL", "POD", 121.505, false }, { "LFPO_OBS", "POO", 121.510 },
		{ "LFPO_GND", "POG", 121.705 }
	}).controller == "LFPO_GND",
		"PDC frequency: observers and unidentified facilities are not clearance positions", failures);
	std::vector<FrequencyController> towers = {
		{ "LFPG_S_TWR", "PGS", 120.900, true, true, true },
		{ "LFPG_N_TWR", "PGN", 119.250 },
	};
	Check(resolveFrequency("LFPG", "26R", towers).controller == "LFPG_N_TWR" &&
		resolveFrequency("LFPG", "08L", towers).controller == "LFPG_N_TWR" &&
		resolveFrequency("LFPG", "27L", towers).controller == "LFPG_S_TWR" &&
		resolveFrequency("LFPG", "09R", towers).controller == "LFPG_S_TWR",
		"PDC frequency: LFPG runway N/S affinity wins over own/coordinated tie-breakers", failures);
	towers.push_back({ "LFPG_GND", "PGG", 121.605 });
	Check(resolveFrequency("LFPG", "26R", towers).controller == "LFPG_GND",
		"PDC frequency: runway affinity never outranks the lower staffed facility", failures);
	std::vector<FrequencyController> equal = {
		{ "LFPO_B_GND", "POB", 121.705 }, { "LFPO_A_GND", "POA", 121.710 }
	};
	Check(resolveFrequency("LFPO", "24", equal).controller == "LFPO_A_GND",
		"PDC frequency: equal local facilities use a stable identity tie-breaker", failures);
	std::reverse(equal.begin(), equal.end());
	Check(resolveFrequency("LFPO", "24", equal).controller == "LFPO_A_GND",
		"PDC frequency: equal-score resolution is independent of enumeration order", failures);
	equal.back().isCoordinated = true;
	Check(resolveFrequency("LFPO", "24", equal).controller == "LFPO_B_GND",
		"PDC frequency: coordinated controller is a tie-breaker only inside the departure airport", failures);
	equal.front().isOwn = true;
	Check(resolveFrequency("LFPO", "24", equal).controller == "LFPO_A_GND",
		"PDC frequency: own station is a tie-breaker only inside the departure airport", failures);

	// ----- Addresses -----
	Check(JoinUrl("https://api.cpdlc.vatsim.fr", "/v1/dcl/poll") == "https://api.cpdlc.vatsim.fr/v1/dcl/poll",
		"PINEDE: routes join the base address", failures);
	Check(JoinUrl("https://api.cpdlc.vatsim.fr/", "/v1/dcl/poll") == "https://api.cpdlc.vatsim.fr/v1/dcl/poll",
		"PINEDE: a trailing slash on the base address is tolerated", failures);
	Check(HttpHelper::IsAllowedApiUrl(JoinUrl(kDefaultBaseUrl, "/v1/dcl/poll")),
		"PINEDE: the built-in address is accepted", failures);
	Check(HttpHelper::IsAllowedApiUrl("http://localhost:7002/v1/dcl/poll") &&
		HttpHelper::IsAllowedApiUrl("http://127.0.0.1:7002/v1/dcl/poll"),
		"PINEDE: plain http is allowed to a server on this machine", failures);
	Check(!HttpHelper::IsAllowedApiUrl("http://api.cpdlc.vatsim.fr/v1/dcl/poll") &&
		!HttpHelper::IsAllowedApiUrl("http://localhost.example.com/v1/dcl/poll") &&
		!HttpHelper::IsAllowedApiUrl("https://user@api.cpdlc.vatsim.fr/v1/dcl/poll") &&
		!HttpHelper::IsAllowedApiUrl("https://api.cpdlc.vatsim.fr:8443/v1/dcl/poll"),
		"PINEDE: the key never travels in clear or to an unexpected port", failures);
	Check(!HttpHelper::IsValidHttpsUrl("http://localhost:7002/"),
		"PINEDE: the loopback exception does not loosen ordinary downloads", failures);

	// ----- Request bodies -----
	const std::string poll = BuildPollBody("LFPG_DEL", "121.805");
	Check(Member(poll, "controller_callsign") == "LFPG_DEL" && Member(poll, "frequency") == "121.805",
		"PINEDE: poll names the station and its frequency", failures);
	Check(Member(BuildPollBody("LFPG_DEL", ""), "frequency") == "<absent>",
		"PINEDE: an unknown frequency is left out", failures);
	const std::string message = BuildMessageBody("LFPG_DEL", "AFR123", "EXPECT \"DELAY\" 10 MIN");
	Check(Member(message, "text") == "EXPECT \"DELAY\" 10 MIN" && Member(message, "callsign") == "AFR123",
		"PINEDE: message text is escaped, not spliced", failures);

	ClearanceFields fields;
	fields.callsign = "AFR123";
	fields.destination = "LFMN";
	fields.runway = "09L";
	fields.sid = "OPALE1A";
	fields.initialClimb = "FL070";
	fields.squawk = "1000";
	fields.tsat = "1230";
	fields.ctot = "no";
	fields.frequency = "";
	fields.fallbackFrequency = "126.655";
	const std::string clearance = BuildClearanceBody("LFPG_DEL", fields);
	Check(Member(clearance, "initial_climb") == "FL070" && Member(clearance, "fallback_frequency") == "126.655" &&
		Member(clearance, "runway") == "09L" && Member(clearance, "tsat") == "1230",
		"PINEDE: clearance carries the dialog's fields", failures);
	Check(Member(clearance, "ctot") == "<absent>" && Member(clearance, "frequency") == "<absent>" &&
		Member(clearance, "message") == "<absent>",
		"PINEDE: empty or 'no' optional clearance fields are left out", failures);

	Check(!HasFreeText("") && !HasFreeText("  \r\n") && !HasFreeText("no") && !HasFreeText(" NO ") &&
		HasFreeText("NO DELAY EXPECTED"),
		"PINEDE: an empty dialog field or its 'no' placeholder is not text", failures);
	const std::string longText(250, 'A');
	Check(Member(BuildMessageBody("LFPG_DEL", "AFR123", longText), "text").size() == kMaxFreeTextBytes,
		"PINEDE: free text is cut to what PINEDE accepts", failures);
	std::string accented = std::string(199, 'A') + "\xC3\xA9" + "BC";
	Check(LimitFreeText(accented).size() == 199,
		"PINEDE: cutting free text never splits a UTF-8 character", failures);
	fields.message = "no";
	Check(Member(BuildClearanceBody("LFPG_DEL", fields), "message") == "<absent>",
		"PINEDE: the dialog's 'no' message placeholder is not sent", failures);
	fields.message = longText;
	Check(Member(BuildClearanceBody("LFPG_DEL", fields), "message").size() == kMaxFreeTextBytes,
		"PINEDE: a long clearance message is cut to what PINEDE accepts", failures);

	// ----- Poll answer -----
	PollResult result;
	Check(ParsePollResponse(kPoll, result), "PINEDE: a poll answer parses", failures);
	Check(result.airports.size() == 2 && result.airports[0] == "LFPG", "PINEDE: covered airports are read", failures);
	Check(result.requests.size() == 5, "PINEDE: entries without a callsign or not objects are skipped", failures);
	Check(!result.requests.empty() && result.requests[2].callsign == "DLH4", "PINEDE: callsigns are upper-cased", failures);
	Check(!ParsePollResponse("{}", result) && !ParsePollResponse("not json", result) &&
		!ParsePollResponse(R"({"requests": {}})", result),
		"PINEDE: an answer without a request list is refused", failures);

	ParsePollResponse(kPoll, result);
	TagLists lists = BuildTagLists(result, {});
	Check(Contains(lists.demandingClearance, "AFR123") && !Contains(lists.standby, "AFR123"),
		"PINEDE: PENDING asks for a clearance", failures);
	Check(Contains(lists.demandingClearance, "BAW1") && Contains(lists.standby, "BAW1"),
		"PINEDE: STANDBY still asks for a clearance, with the standby flag", failures);
	Check(Contains(lists.messageSent, "DLH4") && lists.cleared.count("DLH4") == 1 && !Contains(lists.wilco, "DLH4"),
		"PINEDE: CLEARED shows the clearance as sent", failures);
	Check(Contains(lists.messageSent, "EZY5") && Contains(lists.wilco, "EZY5"),
		"PINEDE: ACKED adds the WILCO flag", failures);
	Check(!Contains(lists.demandingClearance, "RYR6") && !Contains(lists.messageSent, "RYR6") &&
		lists.displayedText.count("RYR6") == 0,
		"PINEDE: an undocumented status is ignored", failures);
	Check(Contains(lists.message, "BAW1") && lists.displayedText["BAW1"] == "STAND K47" &&
		lists.displayedText["AFR123"] == "REQUEST PREDEP CLEARANCE AFR123",
		"PINEDE: a pilot message is flagged and shown instead of the request", failures);
	lists = BuildTagLists(result, { "EZY5" });
	Check(Contains(lists.messageSent, "EZY5") && !Contains(lists.wilco, "EZY5"),
		"PINEDE: a dismissed WILCO stays dismissed across polls", failures);

	// ----- Failures shown to the controller -----
	Check(DescribeFailure(false, 0, "").find("could not be reached") != std::string::npos,
		"PINEDE: an unreachable server is reported as such", failures);
	Check(DescribeFailure(true, 401, "").find("PDC key") != std::string::npos,
		"PINEDE: a refused key points at the key", failures);
	Check(DescribeFailure(true, 409, R"({"message":"Handled by LFPG_N_GND","statusCode":409})") == "Handled by LFPG_N_GND.",
		"PINEDE: a conflict shows PINEDE's reason", failures);
	Check(DescribeFailure(true, 400, R"({"message":"Clearance not sent: SID, SQUAWK missing","statusCode":400})") == "Clearance not sent: SID, SQUAWK missing.",
		"PINEDE: an incomplete clearance says what is missing", failures);
	Check(DescribeFailure(true, 500, "<html>").find("500") != std::string::npos,
		"PINEDE: an unexpected status is reported with its code", failures);

	// ----- One poller per station -----
	Check(YieldsToDman("LFPG_DEL", "LFPG_DEL") && YieldsToDman(" lfpg_del\t", "LFPG_DEL ") &&
		YieldsToDman("LFPG_DEL", "lfpg_del"),
		"PINEDE: vSMR yields when DMAN handles its own station, whatever the case and spacing", failures);
	Check(!YieldsToDman("LFPG_DEL", "LFPG_N_GND") && !YieldsToDman("LFPG_DEL", "LFPG_DEL2"),
		"PINEDE: DMAN handling another station leaves vSMR polling", failures);
	Check(!YieldsToDman("", "LFPG_DEL") && !YieldsToDman("  ", "LFPG_DEL") &&
		!YieldsToDman("", "") && !YieldsToDman("LFPG_DEL", ""),
		"PINEDE: an unset station or an unknown own callsign never yields", failures);

	return failures;
}
