#include "datalink/PinedeProtocol.hpp"

#include "shared/JsonDocument.hpp"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cctype>

namespace VsmrPinede
{
	namespace
	{
		using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

		void Field(Writer& writer, const char* key, const std::string& value)
		{
			writer.Key(key);
			writer.String(value.c_str(), static_cast<rapidjson::SizeType>(value.size()));
		}

		// Optional clearance fields are left out rather than sent empty, as PINEDE
		// treats an absent field as "not given"
		void OptionalField(Writer& writer, const char* key, const std::string& value)
		{
			if (!value.empty() && value != "no")
				Field(writer, key, value);
		}

		std::string Finish(const rapidjson::StringBuffer& buffer)
		{
			return std::string(buffer.GetString(), buffer.GetSize());
		}

		std::string StringMember(const rapidjson::Value& object, const char* key)
		{
			const auto it = object.FindMember(key);
			if (it == object.MemberEnd() || !it->value.IsString())
				return "";
			return std::string(it->value.GetString(), it->value.GetStringLength());
		}

		std::string Upper(std::string text)
		{
			for (char& c : text)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return text;
		}

		std::string TrimmedUpper(const std::string& text)
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return "";
			const auto last = text.find_last_not_of(" \t\r\n");
			return Upper(text.substr(first, last - first + 1));
		}

		void AddUnique(std::vector<std::string>& list, const std::string& callsign)
		{
			if (std::find(list.begin(), list.end(), callsign) == list.end())
				list.push_back(callsign);
		}
	}

	bool HasFreeText(const std::string& text)
	{
		const auto first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
			return false;
		const auto last = text.find_last_not_of(" \t\r\n");
		return Upper(text.substr(first, last - first + 1)) != "NO";
	}

	std::string LimitFreeText(const std::string& text)
	{
		if (text.size() <= kMaxFreeTextBytes)
			return text;
		std::size_t cut = kMaxFreeTextBytes;
		// step back over UTF-8 continuation bytes to a character boundary
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
			--cut;
		return text.substr(0, cut);
	}

	std::string JoinUrl(const std::string& baseUrl, const std::string& path)
	{
		std::string base = baseUrl;
		while (!base.empty() && base.back() == '/')
			base.pop_back();
		return path.empty() || path.front() == '/' ? base + path : base + "/" + path;
	}

	std::string BuildPollBody(const std::string& controllerCallsign, const std::string& frequency)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		Field(writer, "controller_callsign", controllerCallsign);
		OptionalField(writer, "frequency", frequency);
		writer.EndObject();
		return Finish(buffer);
	}

	std::string BuildStationBody(const std::string& controllerCallsign)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		Field(writer, "controller_callsign", controllerCallsign);
		writer.EndObject();
		return Finish(buffer);
	}

	std::string BuildAircraftBody(const std::string& controllerCallsign, const std::string& callsign)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		Field(writer, "controller_callsign", controllerCallsign);
		Field(writer, "callsign", callsign);
		writer.EndObject();
		return Finish(buffer);
	}

	std::string BuildMessageBody(const std::string& controllerCallsign, const std::string& callsign, const std::string& text)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		Field(writer, "controller_callsign", controllerCallsign);
		Field(writer, "callsign", callsign);
		Field(writer, "text", LimitFreeText(text));
		writer.EndObject();
		return Finish(buffer);
	}

	std::string BuildClearanceBody(const std::string& controllerCallsign, const ClearanceFields& fields)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		Field(writer, "controller_callsign", controllerCallsign);
		Field(writer, "callsign", fields.callsign);
		Field(writer, "destination", fields.destination);
		Field(writer, "runway", fields.runway);
		Field(writer, "sid", fields.sid);
		Field(writer, "initial_climb", fields.initialClimb);
		Field(writer, "squawk", fields.squawk);
		OptionalField(writer, "tsat", fields.tsat);
		OptionalField(writer, "ctot", fields.ctot);
		OptionalField(writer, "frequency", fields.frequency);
		Field(writer, "fallback_frequency", fields.fallbackFrequency);
		if (HasFreeText(fields.message))
			Field(writer, "message", LimitFreeText(fields.message));
		writer.EndObject();
		return Finish(buffer);
	}

	RequestStatus ParseStatus(const std::string& status)
	{
		if (status == "PENDING") return RequestStatus::Pending;
		if (status == "STANDBY") return RequestStatus::Standby;
		if (status == "CLEARED") return RequestStatus::Cleared;
		if (status == "ACKED") return RequestStatus::Acked;
		return RequestStatus::Unknown;
	}

	bool ParsePollResponse(const std::string& json, PollResult& result)
	{
		result = PollResult{};
		VsmrJsonInputLimits::Limits limits;
		limits.maximumDepth = 4U;
		limits.maximumValues = 100000U;
		limits.maximumContainerEntries = 5000U;
		limits.maximumStringBytes = 4096U;
		rapidjson::Document document;
		VsmrJson::ParseDocument(document, json, limits);
		if (document.HasParseError() || !document.IsObject())
			return false;

		const auto requests = document.FindMember("requests");
		if (requests == document.MemberEnd() || !requests->value.IsArray())
			return false;
		for (const auto& item : requests->value.GetArray())
		{
			if (!item.IsObject())
				continue;
			DclRequest request;
			request.callsign = Upper(StringMember(item, "callsign"));
			if (request.callsign.empty())
				continue;
			request.departure = StringMember(item, "departure");
			request.destination = StringMember(item, "destination");
			request.requestText = StringMember(item, "request_text");
			request.status = ParseStatus(StringMember(item, "status"));
			request.claimedBy = StringMember(item, "claimed_by");
			request.pilotMessage = StringMember(item, "pilot_message");
			result.requests.push_back(std::move(request));
		}

		const auto airports = document.FindMember("airports");
		if (airports != document.MemberEnd() && airports->value.IsArray())
		{
			for (const auto& airport : airports->value.GetArray())
				if (airport.IsString())
					result.airports.emplace_back(airport.GetString(), airport.GetStringLength());
		}
		return true;
	}

	TagLists BuildTagLists(const PollResult& result, const std::set<std::string>& wilcoDismissed)
	{
		TagLists lists;
		for (const DclRequest& request : result.requests)
		{
			const std::string& callsign = request.callsign;
			switch (request.status)
			{
			case RequestStatus::Pending:
				AddUnique(lists.demandingClearance, callsign);
				break;
			case RequestStatus::Standby:
				AddUnique(lists.demandingClearance, callsign);
				AddUnique(lists.standby, callsign);
				break;
			case RequestStatus::Cleared:
				AddUnique(lists.messageSent, callsign);
				lists.cleared.insert(callsign);
				break;
			case RequestStatus::Acked:
				AddUnique(lists.messageSent, callsign);
				lists.cleared.insert(callsign);
				if (wilcoDismissed.count(callsign) == 0)
					AddUnique(lists.wilco, callsign);
				break;
			case RequestStatus::Unknown:
				continue;
			}
			if (!request.pilotMessage.empty())
				AddUnique(lists.message, callsign);
			lists.displayedText[callsign] = request.pilotMessage.empty() ? request.requestText : request.pilotMessage;
		}
		return lists;
	}

	std::string DescribeFailure(bool completed, unsigned long status, const std::string& body)
	{
		if (!completed)
			return "PINEDE could not be reached. Check the network and try again.";

		// Nest answers {"message": "...", "statusCode": n}; show its own reason when there is one
		std::string reason;
		rapidjson::Document document;
		VsmrJsonInputLimits::Limits limits;
		limits.maximumDepth = 4U;
		limits.maximumValues = 64U;
		limits.maximumContainerEntries = 32U;
		limits.maximumStringBytes = 512U;
		VsmrJson::ParseDocument(document, body, limits);
		if (!document.HasParseError() && document.IsObject())
			reason = StringMember(document, "message");
		if (reason.size() > 160)
			reason = reason.substr(0, 160) + "...";

		switch (status)
		{
		case 400: return reason.empty() ? "PINEDE refused the request." : reason + ".";
		case 401: return "PINEDE refused the PDC key. Check it in the runtime menu.";
		case 403: return reason.empty() ? "PINEDE refused this station." : "PINEDE refused this station: " + reason;
		case 404: return reason.empty() ? "No open PDC request for this aircraft." : reason + ".";
		case 409: return reason.empty() ? "Another controller is handling this request." : reason + ".";
		case 502: return "PINEDE could not deliver the message to the aircraft. Try again.";
		case 503: return "PINEDE cannot check the PDC key right now. Try again shortly.";
		default:
			return "PINEDE answered " + std::to_string(status) + (reason.empty() ? "." : ": " + reason);
		}
	}

	bool YieldsToDman(const std::string& dmanStation, const std::string& ownCallsign)
	{
		const std::string station = TrimmedUpper(dmanStation);
		return !station.empty() && station == TrimmedUpper(ownCallsign);
	}
}
