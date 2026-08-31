#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vsmr::updater::transport
{
	namespace policy
	{
		enum class TimeoutSetupStatus
		{
			Ready,
			DeadlineExpired,
			ConfigurationFailed
		};

		constexpr TimeoutSetupStatus ClassifyTimeoutSetup(
			std::uint32_t remainingMs,
			bool configured) noexcept
		{
			return remainingMs == 0
				? TimeoutSetupStatus::DeadlineExpired
				: (configured
					? TimeoutSetupStatus::Ready
					: TimeoutSetupStatus::ConfigurationFailed);
		}

		constexpr bool WouldExceedMaximumBytes(
			std::uint64_t received,
			std::uint64_t incoming,
			std::uint64_t maximum) noexcept
		{
			return received > maximum || incoming > maximum - received;
		}
	}

	struct Response
	{
		std::uint32_t statusCode = 0;
		std::vector<std::uint8_t> body;
		std::string etag;
		std::string retryAfter;
		std::string rateLimitReset;
		std::wstring finalUrl;
		std::string error;
	};

	struct Request
	{
		std::wstring initialUrl;
		std::uint32_t timeoutMs = 0;
		std::uint64_t maximumBytes = 0;
		std::string ifNoneMatch;
		std::filesystem::path outputFile;
		std::uint64_t expectedSize = 0;
	};

	// The caller owns cancellation and progress policy; transport only preserves
	// the bounded, allow-listed HTTPS request semantics.
	Response HttpGet(
		const Request& request,
		const std::function<bool()>& isCancelled,
		const std::function<bool(int)>& reportDownloadProgress);
}
