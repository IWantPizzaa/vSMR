#pragma once
#include <mutex>
#include <string>

class HttpHelper
{
public:
	HttpHelper();
	std::string downloadStringFromURL(const std::string& url);
	~HttpHelper();

private:
	// This helper instance is shared across plugin worker threads.
	static std::mutex downloadMutex;
};
