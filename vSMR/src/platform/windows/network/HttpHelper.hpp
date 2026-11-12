#pragma once
#include <atomic>
#include <cstddef>
#include <string>

// The outcome of an API call. completed is false when no full response came
// back (network failure, timeout, cancellation, oversized body); status and
// body are then meaningless.
struct HttpResponse
{
	bool completed = false;
	unsigned long status = 0;
	std::string body;
};

class HttpHelper
{
public:
	HttpHelper();
	std::string downloadStringFromURL(
		std::string url,
		int timeoutMs = 6000,
		const std::atomic<bool>* cancelRequested = nullptr,
		size_t maxResponseBytes = 8U * 1024U * 1024U);
	// POSTs a JSON body and returns the response whatever its status, so API
	// errors can be reported. https only, except plain http to localhost or
	// 127.0.0.1 for a locally run server.
	HttpResponse sendJsonRequest(
		const std::string& url,
		const std::string& jsonBody,
		const std::string& authorization,
		int timeoutMs = 6000,
		const std::atomic<bool>* cancelRequested = nullptr,
		size_t maxResponseBytes = 1024U * 1024U);
	[[nodiscard]] static bool IsAllowedApiUrl(const std::string& url);
	[[nodiscard]] static bool IsValidHttpsUrl(
		const std::string& url,
		std::string* host = nullptr);
	[[nodiscard]] static bool IsHttpsUrlForHost(
		const std::string& url,
		const std::string& expectedHost);
	~HttpHelper();
};
