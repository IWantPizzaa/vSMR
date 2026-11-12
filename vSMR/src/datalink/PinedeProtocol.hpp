#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

// PDC through PINEDE, the French vACC's datalink server. Pilots ask LFXX for
// their clearance; PINEDE routes each request to the controller responsible
// for the departure airport and sends every uplink itself. vSMR polls PINEDE
// for the requests this station should show and posts the controller's
// answers. The rules for who receives what live in PINEDE, not here.
//
// Pure helpers only: JSON in and out, and the mapping from PINEDE's request
// states to vSMR's tag lists. Transport and threading stay in the plug-in.
namespace VsmrPinede
{
	constexpr const char* kDefaultBaseUrl = "https://api.cpdlc.vatsim.fr";

	// Request states PINEDE returns from /v1/dcl/poll
	enum class RequestStatus
	{
		Pending,  // waiting for a controller
		Standby,  // a controller answered STANDBY
		Cleared,  // clearance sent
		Acked,    // pilot answered WILCO / ROGER
		Unknown,
	};

	struct DclRequest
	{
		std::string callsign;
		std::string departure;
		std::string destination;
		std::string requestText;
		RequestStatus status = RequestStatus::Unknown;
		std::string claimedBy;
		// The pilot's latest message while no controller has answered it
		std::string pilotMessage;
	};

	struct PollResult
	{
		std::vector<DclRequest> requests;
		// Airports this station receives when nobody better is online
		std::vector<std::string> airports;
	};

	// What the datalink tag items and menus read, rebuilt from each poll
	struct TagLists
	{
		std::vector<std::string> demandingClearance;
		std::vector<std::string> standby;
		std::vector<std::string> message;
		std::vector<std::string> wilco;
		std::vector<std::string> messageSent;
		std::set<std::string> cleared;
		// Text shown in the PDC and message dialogs
		std::map<std::string, std::string> displayedText;
	};

	struct ClearanceFields
	{
		std::string callsign;
		std::string destination;
		std::string runway;
		std::string sid;
		std::string initialClimb;
		std::string squawk;
		std::string tsat;
		std::string ctot;
		std::string frequency;
		std::string fallbackFrequency;
		std::string message;
	};

	// PINEDE refuses free text longer than this
	constexpr std::size_t kMaxFreeTextBytes = 200;

	// The dialogs fill an empty field with "no"; neither is text to send
	bool HasFreeText(const std::string& text);
	// Cut to kMaxFreeTextBytes without splitting a UTF-8 character
	std::string LimitFreeText(const std::string& text);

	// "https://api.cpdlc.vatsim.fr" + "/v1/dcl/poll", tolerating a trailing slash
	std::string JoinUrl(const std::string& baseUrl, const std::string& path);

	std::string BuildPollBody(const std::string& controllerCallsign, const std::string& frequency);
	std::string BuildStationBody(const std::string& controllerCallsign);
	std::string BuildAircraftBody(const std::string& controllerCallsign, const std::string& callsign);
	std::string BuildMessageBody(const std::string& controllerCallsign, const std::string& callsign, const std::string& text);
	std::string BuildClearanceBody(const std::string& controllerCallsign, const ClearanceFields& fields);

	RequestStatus ParseStatus(const std::string& status);
	bool ParsePollResponse(const std::string& json, PollResult& result);

	// Requests whose status is not one PINEDE documents are left out.
	// wilcoDismissed holds aircraft whose WILCO flag the controller no longer
	// needs (airborne), so a later poll does not raise it again.
	TagLists BuildTagLists(const PollResult& result, const std::set<std::string>& wilcoDismissed);

	// A line for the controller explaining a failed call
	std::string DescribeFailure(bool completed, unsigned long status, const std::string& body);
}
