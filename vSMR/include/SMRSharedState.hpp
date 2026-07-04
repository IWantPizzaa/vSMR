#pragma once

#include <string>
#include <unordered_set>
#include <asio/io_service.hpp>

namespace SMRSharedData
{
	extern std::unordered_set<std::string> ReleasedTracks;
	extern std::unordered_set<std::string> ManuallyCorrelated;
}

namespace SMRPluginSharedData
{
	extern asio::io_service io_service;
}
