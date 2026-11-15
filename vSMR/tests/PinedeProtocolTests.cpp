#include "PinedeProtocolTests.hpp"

#include "datalink/PinedeProtocol.hpp"
#include "platform/windows/network/HttpHelper.hpp"

#include "rapidjson/document.h"

#include <algorithm>
#include <set>
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

	return failures;
}
