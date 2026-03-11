#pragma once
#include <string>
#include <mutex>
#include <curl\curl.h>
#include <curl\easy.h>

class HttpHelper
{
public:
	HttpHelper();
	std::string downloadStringFromURL(std::string url);
	~HttpHelper();

private:
	static std::string downloadedContents;
	static std::mutex downloadMutex;
	static size_t handle_data(void *ptr, size_t size, size_t nmemb, void *stream);
};
