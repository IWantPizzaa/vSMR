#pragma once
#include <atomic>
#include <string>

class HttpHelper
{
public:
	HttpHelper();
	std::string downloadStringFromURL(
		std::string url,
		int timeoutMs = 6000,
		const std::atomic<bool>* cancelRequested = nullptr);
	~HttpHelper();
};
