#include "control_center/WebMessageValidation.hpp"
#include "shared/JsonInputLimits.hpp"
#include "shared/RapidJsonStringViewStream.hpp"

#include "rapidjson/reader.h"

#include <cstdint>
#include <cstring>

namespace
{
	constexpr unsigned int MaximumJsonDepth = 64;
	constexpr rapidjson::SizeType MaximumEnvelopeMembers = 16;
	constexpr rapidjson::SizeType MaximumMessageIdBytes = 256;
	constexpr rapidjson::SizeType MaximumMessageTypeBytes = 128;
	constexpr std::size_t MaximumEnvelopeValues = 2000000U;
	constexpr std::size_t MaximumEnvelopeContainerEntries = 1000000U;
	constexpr std::size_t MaximumEnvelopeStringBytes = 64U * 1024U;

	class InboundEnvelopeHandler final
		: public rapidjson::BaseReaderHandler<rapidjson::UTF8<>>
	{
	public:
		void Null() { AcceptScalar(); }
		void Bool(bool) { AcceptScalar(); }
		void Int(int value) { AcceptInteger(value == 1); }
		void Uint(unsigned int value) { AcceptInteger(value == 1); }
		void Int64(std::int64_t value) { AcceptInteger(value == 1); }
		void Uint64(std::uint64_t value) { AcceptInteger(value == 1); }
		void Double(double) { AcceptScalar(); }

		void String(
			const char* value,
			rapidjson::SizeType length,
			bool)
		{
			if (Depth != 1 || !RootIsObject)
				return;
			if (ExpectingRootKey)
			{
				AcceptRootKey(value, length);
				return;
			}

			bool accepted = true;
			switch (CurrentRootField)
			{
			case RootField::Id:
				accepted = length <= MaximumMessageIdBytes &&
					std::memchr(value, '\0', length) == nullptr;
				break;
			case RootField::Type:
			case RootField::Action:
				accepted = length > 0 && length <= MaximumMessageTypeBytes &&
					std::memchr(value, '\0', length) == nullptr;
				if (accepted)
				{
					HasMessageType = true;
					Selector.assign(value, static_cast<std::size_t>(length));
				}
				break;
			case RootField::Version:
				accepted = false;
				break;
			default:
				break;
			}
			FinishRootValue();
			if (!accepted)
				Invalid = true;
		}

		void StartObject()
		{
			if (Depth == 0)
			{
				if (RootStarted)
					Invalid = true;
				RootStarted = true;
				RootIsObject = true;
				Depth = 1;
				ExpectingRootKey = true;
				return;
			}
			if (Depth == 1)
				AcceptRootContainer();
			++Depth;
			if (Depth > MaximumJsonDepth)
				Invalid = true;
		}

		void EndObject(rapidjson::SizeType)
		{
			if (Depth == 0)
			{
				Invalid = true;
				return;
			}
			if (Depth == 1)
			{
				if (!RootIsObject || !ExpectingRootKey)
					Invalid = true;
				RootCompleted = true;
			}
			--Depth;
		}

		void StartArray()
		{
			if (Depth == 0)
			{
				RootStarted = true;
				RootIsObject = false;
				Invalid = true;
			}
			else if (Depth == 1)
			{
				AcceptRootContainer();
			}
			++Depth;
			if (Depth > MaximumJsonDepth)
				Invalid = true;
		}

		void EndArray(rapidjson::SizeType)
		{
			if (Depth == 0)
			{
				Invalid = true;
				return;
			}
			if (Depth == 1)
				RootCompleted = true;
			--Depth;
		}

		bool IsValid() const noexcept
		{
			return !Invalid && RootStarted && RootIsObject &&
				RootCompleted && Depth == 0 && HasMessageType &&
				SeenType != SeenAction;
		}

		const std::string& MessageSelector() const noexcept
		{
			return Selector;
		}

	private:
		enum class RootField
		{
			Other,
			Version,
			Id,
			Type,
			Action,
			Payload
		};

		static bool KeyEquals(
			const char* value,
			rapidjson::SizeType length,
			const char* expected) noexcept
		{
			const std::size_t expectedLength = std::strlen(expected);
			return length == expectedLength &&
				std::memcmp(value, expected, expectedLength) == 0;
		}

		static RootField ClassifyRootField(
			const char* value,
			rapidjson::SizeType length) noexcept
		{
			if (KeyEquals(value, length, "version")) return RootField::Version;
			if (KeyEquals(value, length, "id")) return RootField::Id;
			if (KeyEquals(value, length, "type")) return RootField::Type;
			if (KeyEquals(value, length, "action")) return RootField::Action;
			if (KeyEquals(value, length, "payload")) return RootField::Payload;
			return RootField::Other;
		}

		void AcceptRootKey(
			const char* value,
			rapidjson::SizeType length) noexcept
		{
			if (++RootMemberCount > MaximumEnvelopeMembers)
				Invalid = true;
			CurrentRootField = ClassifyRootField(value, length);
			ExpectingRootKey = false;
			MarkRootFieldSeen(CurrentRootField);
		}

		void MarkRootFieldSeen(RootField field) noexcept
		{
			bool* seen = nullptr;
			switch (field)
			{
			case RootField::Version: seen = &SeenVersion; break;
			case RootField::Id: seen = &SeenId; break;
			case RootField::Type: seen = &SeenType; break;
			case RootField::Action: seen = &SeenAction; break;
			case RootField::Payload: seen = &SeenPayload; break;
			default: return;
			}
			if (*seen)
				Invalid = true;
			*seen = true;
		}

		void AcceptInteger(bool isProtocolVersion)
		{
			if (Depth != 1)
				return;
			if (!RootIsObject || ExpectingRootKey)
			{
				Invalid = true;
				return;
			}
			const RootField field = CurrentRootField;
			const bool accepted = field == RootField::Version
				? isProtocolVersion
				: field != RootField::Id && field != RootField::Type &&
					field != RootField::Action;
			FinishRootValue();
			if (!accepted)
				Invalid = true;
		}

		void AcceptScalar()
		{
			if (Depth != 1)
				return;
			if (!RootIsObject || ExpectingRootKey)
			{
				Invalid = true;
				return;
			}
			const RootField field = CurrentRootField;
			FinishRootValue();
			if (field == RootField::Version || field == RootField::Id ||
				field == RootField::Type || field == RootField::Action)
			{
				Invalid = true;
			}
		}

		void AcceptRootContainer()
		{
			if (!RootIsObject || ExpectingRootKey)
			{
				Invalid = true;
				return;
			}
			const RootField field = CurrentRootField;
			FinishRootValue();
			if (field == RootField::Version || field == RootField::Id ||
				field == RootField::Type || field == RootField::Action)
			{
				Invalid = true;
			}
		}

		void FinishRootValue() noexcept
		{
			ExpectingRootKey = true;
			CurrentRootField = RootField::Other;
		}

		unsigned int Depth = 0;
		rapidjson::SizeType RootMemberCount = 0;
		RootField CurrentRootField = RootField::Other;
		bool RootStarted = false;
		bool RootIsObject = false;
		bool RootCompleted = false;
		bool ExpectingRootKey = false;
		bool HasMessageType = false;
		bool Invalid = false;
		bool SeenVersion = false;
		bool SeenId = false;
		bool SeenType = false;
		bool SeenAction = false;
		bool SeenPayload = false;
		std::string Selector;
	};
}

bool VsmrWebMessageValidation::TryGetInboundWebMessageSelector(
	std::string_view json,
	std::string& selector)
{
	selector.clear();
	if (json.empty() || json.size() > MaximumInboundMessageBytes ||
		json.find('\0') != std::string_view::npos)
	{
		return false;
	}
	VsmrJsonInputLimits::Limits limits;
	limits.maximumDepth = MaximumJsonDepth;
	limits.maximumValues = MaximumEnvelopeValues;
	limits.maximumContainerEntries = MaximumEnvelopeContainerEntries;
	limits.maximumStringBytes = MaximumEnvelopeStringBytes;
	std::string limitError;
	if (!VsmrJsonInputLimits::Validate(json, limits, limitError))
		return false;

	rapidjson::Reader reader;
	VsmrJson::StringViewStream stream(json);
	InboundEnvelopeHandler handler;
	if (!reader.Parse<rapidjson::kParseDefaultFlags>(stream, handler) ||
		!handler.IsValid())
	{
		return false;
	}
	selector = handler.MessageSelector();
	return true;
}

bool VsmrWebMessageValidation::HasValidInboundWebMessageShape(
	std::string_view json)
{
	std::string selector;
	return TryGetInboundWebMessageSelector(json, selector);
}
