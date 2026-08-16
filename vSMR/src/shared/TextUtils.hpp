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
