#pragma once
#include <string>
#include <mutex>

class HttpHelper
{
private:
	static std::mutex downloadMutex;

public:
	HttpHelper();
	std::string downloadStringFromURL(std::string url);
	~HttpHelper();

};
