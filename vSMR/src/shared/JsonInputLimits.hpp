#pragma once

#include "shared/RapidJsonStringViewStream.hpp"
#include "rapidjson/reader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace VsmrJsonInputLimits
{
	struct Limits
	{
		std::size_t maximumDepth = 64U;
		std::size_t maximumValues = 0U;
		std::size_t maximumContainerEntries = 0U;
		std::size_t maximumStringBytes = 0U;
		std::size_t maximumFeatures = 0U;
		std::size_t maximumCoordinatePairs = 0U;
	};

	namespace Detail
	{
		inline bool ValidateRawStructure(
			std::string_view json,
			const Limits& limits,
			std::string& error)
		{
			std::size_t depth = 0U;
			std::size_t stringBytes = 0U;
			bool inString = false;
			bool escaped = false;
			for (const char character : json)
			{
				if (inString)
				{
					if (escaped)
						escaped = false;
					else if (character == '\\')
						escaped = true;
					else if (character == '"')
					{
						inString = false;
						continue;
					}
					if (limits.maximumStringBytes != 0U &&
						++stringBytes > limits.maximumStringBytes)
					{
						error = "JSON contains a string or property name longer than the configured limit.";
						return false;
					}
					continue;
				}

				if (character == '"')
				{
					inString = true;
					stringBytes = 0U;
				}
				else if (character == '{' || character == '[')
				{
					++depth;
					if (limits.maximumDepth != 0U &&
						depth > limits.maximumDepth)
					{
						error = "JSON exceeds the configured nesting-depth limit.";
						return false;
					}
				}
				else if (character == '}' || character == ']')
				{
					if (depth == 0U)
					{
						error = "JSON container structure is invalid.";
						return false;
					}
					--depth;
				}
			}
			return true;
		}

		class LimitHandler final
			: public rapidjson::BaseReaderHandler<rapidjson::UTF8<> >
		{
		public:
			explicit LimitHandler(const Limits& limits)
				: ActiveLimits(limits)
			{
				Frames.reserve(limits.maximumDepth);
			}

			void Null() { AcceptScalar(false); }
			void Bool(bool) { AcceptScalar(false); }
			void Int(int) { AcceptScalar(true); }
			void Uint(unsigned int) { AcceptScalar(true); }
			void Int64(std::int64_t) { AcceptScalar(true); }
			void Uint64(std::uint64_t) { AcceptScalar(true); }
			void Double(double) { AcceptScalar(true); }

			void String(
				const char* value,
				rapidjson::SizeType length,
				bool)
			{
				if (ActiveLimits.maximumStringBytes != 0U &&
					static_cast<std::size_t>(length) > ActiveLimits.maximumStringBytes)
				{
					Fail("JSON contains a string or property name longer than the configured limit.");
				}

				if (!Frames.empty() && Frames.back().kind == ContainerKind::Object &&
					Frames.back().expectingKey)
				{
					Frame& frame = Frames.back();
					frame.pendingKey = ClassifyKey(value, length);
					frame.expectingKey = false;
					return;
				}

				AcceptScalar(false);
			}

			void StartObject() { StartContainer(ContainerKind::Object); }

			void EndObject(rapidjson::SizeType memberCount)
			{
				if (Frames.empty() || Frames.back().kind != ContainerKind::Object)
				{
					Fail("JSON container structure is invalid.");
					return;
				}
				if (!Frames.back().expectingKey)
					Fail("JSON object has a property without a value.");
				CheckContainerSize(memberCount, "JSON contains an object with too many members.");
				Frames.pop_back();
				CompleteContainerValue();
			}

			void StartArray() { StartContainer(ContainerKind::Array); }

			void EndArray(rapidjson::SizeType elementCount)
			{
				if (Frames.empty() || Frames.back().kind != ContainerKind::Array)
				{
					Fail("JSON container structure is invalid.");
					return;
				}

				const Frame frame = Frames.back();
				CheckContainerSize(elementCount, "JSON contains an array with too many elements.");
				if (frame.featuresArray && ActiveLimits.maximumFeatures != 0U &&
					static_cast<std::size_t>(elementCount) > ActiveLimits.maximumFeatures)
				{
					Fail("AVISO GeoJSON exceeds the configured feature limit.");
				}
				if (frame.inCoordinates && frame.onlyDirectNumbers && elementCount >= 2U)
				{
					++CoordinatePairCount;
					if (ActiveLimits.maximumCoordinatePairs != 0U &&
						CoordinatePairCount > ActiveLimits.maximumCoordinatePairs)
					{
						Fail("AVISO GeoJSON exceeds the configured coordinate limit.");
					}
				}
				Frames.pop_back();
				CompleteContainerValue();
			}

			bool IsValid() const noexcept
			{
				return !Invalid && RootValueSeen && Frames.empty();
			}

			const std::string& Error() const noexcept { return ErrorText; }

		private:
			enum class ContainerKind
			{
				Object,
				Array
			};

			enum class KeyKind
			{
				Other,
				Features,
				Coordinates
			};

			struct Frame
			{
				ContainerKind kind = ContainerKind::Object;
				KeyKind pendingKey = KeyKind::Other;
				bool expectingKey = false;
				bool inCoordinates = false;
				bool featuresArray = false;
				bool onlyDirectNumbers = true;
			};

			static bool KeyEquals(
				const char* value,
				rapidjson::SizeType length,
				const char* expected) noexcept
			{
				const std::size_t expectedLength = std::strlen(expected);
				return static_cast<std::size_t>(length) == expectedLength &&
					std::memcmp(value, expected, expectedLength) == 0;
			}

			static KeyKind ClassifyKey(
				const char* value,
				rapidjson::SizeType length) noexcept
			{
				if (KeyEquals(value, length, "features"))
					return KeyKind::Features;
				if (KeyEquals(value, length, "coordinates"))
					return KeyKind::Coordinates;
				return KeyKind::Other;
			}

			void StartContainer(ContainerKind kind)
			{
				KeyKind parentKey = KeyKind::Other;
				bool inheritedCoordinates = false;
				if (!Frames.empty())
				{
					Frame& parent = Frames.back();
					inheritedCoordinates = parent.inCoordinates;
					if (parent.kind == ContainerKind::Object)
					{
						if (parent.expectingKey)
							Fail("JSON object contains a value without a property name.");
						parentKey = parent.pendingKey;
					}
					else
					{
						parent.onlyDirectNumbers = false;
					}
				}
				else if (RootValueSeen)
				{
					Fail("JSON contains more than one root value.");
				}

				CountValue();
				RootValueSeen = true;
				Frame frame;
				frame.kind = kind;
				frame.expectingKey = kind == ContainerKind::Object;
				frame.inCoordinates = inheritedCoordinates ||
					parentKey == KeyKind::Coordinates;
				frame.featuresArray = kind == ContainerKind::Array &&
					parentKey == KeyKind::Features;
				Frames.push_back(frame);
				if (ActiveLimits.maximumDepth != 0U &&
					Frames.size() > ActiveLimits.maximumDepth)
				{
					Fail("JSON exceeds the configured nesting-depth limit.");
				}
			}

			void AcceptScalar(bool numeric)
			{
				if (Frames.empty())
				{
					if (RootValueSeen)
						Fail("JSON contains more than one root value.");
					RootValueSeen = true;
				}
				else
				{
					Frame& parent = Frames.back();
					if (parent.kind == ContainerKind::Object)
					{
						if (parent.expectingKey)
							Fail("JSON object contains a value without a property name.");
						parent.expectingKey = true;
						parent.pendingKey = KeyKind::Other;
					}
					else if (!numeric)
					{
						parent.onlyDirectNumbers = false;
					}
				}
				CountValue();
			}

			void CompleteContainerValue()
			{
				if (Frames.empty())
					return;
				Frame& parent = Frames.back();
				if (parent.kind == ContainerKind::Object)
				{
					if (parent.expectingKey)
						Fail("JSON object contains a value without a property name.");
					parent.expectingKey = true;
					parent.pendingKey = KeyKind::Other;
				}
				else
				{
					parent.onlyDirectNumbers = false;
				}
			}

			void CountValue()
			{
				++ValueCount;
				if (ActiveLimits.maximumValues != 0U &&
					ValueCount > ActiveLimits.maximumValues)
				{
					Fail("JSON contains too many values.");
				}
			}

			void CheckContainerSize(
				rapidjson::SizeType count,
				const char* message)
			{
				if (ActiveLimits.maximumContainerEntries != 0U &&
					static_cast<std::size_t>(count) >
						ActiveLimits.maximumContainerEntries)
				{
					Fail(message);
				}
			}

			void Fail(const char* message)
			{
				Invalid = true;
				if (ErrorText.empty())
					ErrorText = message;
			}

			Limits ActiveLimits;
			std::vector<Frame> Frames;
			std::size_t ValueCount = 0U;
			std::size_t CoordinatePairCount = 0U;
			bool RootValueSeen = false;
			bool Invalid = false;
			std::string ErrorText;
		};
	}

	inline bool Validate(
		std::string_view json,
		const Limits& limits,
		std::string& error)
	{
		error.clear();
		if (json.empty())
		{
			error = "JSON input is empty.";
			return false;
		}
		if (json.find('\0') != std::string_view::npos)
		{
			error = "JSON input contains an embedded NUL byte.";
			return false;
		}
		if (!Detail::ValidateRawStructure(json, limits, error))
			return false;

		rapidjson::Reader reader;
		VsmrJson::StringViewStream stream(json);
		Detail::LimitHandler handler(limits);
		if (!reader.Parse<rapidjson::kParseDefaultFlags>(stream, handler))
		{
			error = "JSON syntax validation failed before DOM construction.";
			return false;
		}
		if (!handler.IsValid())
		{
			error = handler.Error().empty()
				? "JSON failed resource-limit validation."
				: handler.Error();
			return false;
		}
		return true;
	}
}
