#include "updater/UpdaterReleaseModel.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace vsmr::updater::release_model
{
	namespace
	{
		bool ParseUint64(const std::string& text, std::uint64_t& value)
		{
			if (text.empty() || (text.size() > 1 && text.front() == '0'))
				return false;

			std::uint64_t result = 0;
			for (const unsigned char character : text)
			{
				if (!std::isdigit(character))
					return false;
				const std::uint64_t digit = character - '0';
				if (result > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
					return false;
				result = result * 10 + digit;
			}
			value = result;
			return true;
		}

		std::vector<std::string> Split(const std::string& value, char separator)
		{
			std::vector<std::string> parts;
			std::size_t start = 0;
			for (;;)
			{
				const std::size_t end = value.find(separator, start);
				parts.push_back(value.substr(start, end == std::string::npos ? end : end - start));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return parts;
		}
	}

	SemVer ParseSemVer(std::string value)
	{
		SemVer result;
		if (!value.empty() && (value.front() == 'v' || value.front() == 'V'))
			value.erase(value.begin());
		if (value.empty() || value.size() > 64)
			return result;

		const std::size_t plus = value.find('+');
		if (plus != std::string::npos)
		{
			if (value.find('+', plus + 1) != std::string::npos)
				return result;
			const auto buildIdentifiers = Split(value.substr(plus + 1), '.');
			for (const std::string& identifier : buildIdentifiers)
			{
				if (identifier.empty() ||
					!std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
						return std::isalnum(character) != 0 || character == '-';
					}))
				{
					return result;
				}
			}
		}

		const std::string withoutBuild = value.substr(0, plus);
		const std::size_t dash = withoutBuild.find('-');
		const std::string core = withoutBuild.substr(0, dash);
		const auto numbers = Split(core, '.');
		if (numbers.size() != 3 ||
			!ParseUint64(numbers[0], result.major) ||
			!ParseUint64(numbers[1], result.minor) ||
			!ParseUint64(numbers[2], result.patch))
		{
			return result;
		}

		if (dash != std::string::npos)
		{
			const auto identifiers = Split(withoutBuild.substr(dash + 1), '.');
			if (identifiers.empty())
				return result;
			for (const std::string& identifier : identifiers)
			{
				if (identifier.empty())
					return SemVer{};
				if (!std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
					return std::isalnum(character) != 0 || character == '-';
				}))
				{
					return SemVer{};
				}
				SemVerIdentifier parsed;
				parsed.text = identifier;
				parsed.numeric = std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
					return std::isdigit(character) != 0;
				});
				if (parsed.numeric && !ParseUint64(identifier, parsed.number))
					return SemVer{};
				result.prerelease.push_back(std::move(parsed));
			}
		}
		result.normalized = value;
		result.valid = true;
		return result;
	}

	int CompareSemVer(const SemVer& left, const SemVer& right) noexcept
	{
		if (left.major != right.major)
			return left.major < right.major ? -1 : 1;
		if (left.minor != right.minor)
			return left.minor < right.minor ? -1 : 1;
		if (left.patch != right.patch)
			return left.patch < right.patch ? -1 : 1;
		if (left.prerelease.empty() != right.prerelease.empty())
			return left.prerelease.empty() ? 1 : -1;
		const std::size_t count = (std::min)(left.prerelease.size(), right.prerelease.size());
		for (std::size_t index = 0; index < count; ++index)
		{
			const auto& a = left.prerelease[index];
			const auto& b = right.prerelease[index];
			if (a.numeric && b.numeric && a.number != b.number)
				return a.number < b.number ? -1 : 1;
			if (a.numeric != b.numeric)
				return a.numeric ? -1 : 1;
			if (a.text != b.text)
				return a.text < b.text ? -1 : 1;
		}
		if (left.prerelease.size() == right.prerelease.size())
			return 0;
		return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
	}

	bool SameSemVerIdentity(const std::string& left, const std::string& right)
	{
		const SemVer parsedLeft = ParseSemVer(left);
		const SemVer parsedRight = ParseSemVer(right);
		return parsedLeft.valid && parsedRight.valid &&
			parsedLeft.normalized == parsedRight.normalized;
	}

	bool ChannelAccepts(const SemVer& version, UpdateChannel channel) noexcept
	{
		return channel == UpdateChannel::Beta || version.prerelease.empty();
	}
}
