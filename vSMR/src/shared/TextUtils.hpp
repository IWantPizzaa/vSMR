#pragma once

#include <algorithm>
#include <cctype>
#include <string>

inline static std::string ToUpperAsciiCopy(const std::string& value)
{
	std::string normalized = value;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
		});
	return normalized;
}

inline static std::string ToLowerAsciiCopy(const std::string& value)
{
	std::string normalized = value;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
		});
	return normalized;
}

inline static bool AsciiCaseInsensitiveEquals(
	const std::string& left,
	const std::string& right)
{
	if (left.size() != right.size())
		return false;

	return std::equal(
		left.begin(),
		left.end(),
		right.begin(),
		[](unsigned char leftCharacter, unsigned char rightCharacter) {
			return std::tolower(leftCharacter) == std::tolower(rightCharacter);
		});
}

inline static std::string TrimAsciiWhitespaceCopy(const std::string& text)
{
	size_t start = 0;
	while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
		++start;
	size_t end = text.size();
	while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
		--end;
	return text.substr(start, end - start);
}

// Preset storage accepts any non-empty airport key. Whitespace is removed only
// at the edges and the remaining bytes are canonicalized for stable lookup.
inline static std::string NormalizeAirportKeyCopy(const std::string& value)
{
	return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(value));
}

// Resource filenames require a strict four-character ASCII airport token.
// Deliberately do not trim: surrounding whitespace remains invalid input.
inline static std::string NormalizeAirportCodeCopy(const std::string& value)
{
	std::string normalized = ToUpperAsciiCopy(value);
	if (normalized.size() != 4)
		return {};

	for (const char character : normalized)
	{
		if (!((character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9')))
		{
			return {};
		}
	}
	return normalized;
}
