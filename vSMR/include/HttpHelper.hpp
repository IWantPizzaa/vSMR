#pragma once
#include <atomic>
#include <cstddef>
#include <string>

class HttpHelper
{
public:
	HttpHelper();
	std::string downloadStringFromURL(
		std::string url,
		int timeoutMs = 6000,
		const std::atomic<bool>* cancelRequested = nullptr,
		size_t maxResponseBytes = 8U * 1024U * 1024U);
	static bool IsValidHttpsUrl(
		const std::string& url,
		std::string* host = nullptr);
	static bool IsHttpsUrlForHost(
		const std::string& url,
		const std::string& expectedHost);
	~HttpHelper();
};
