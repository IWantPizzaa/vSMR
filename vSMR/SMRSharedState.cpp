#include "stdafx.h"
#include "SMRSharedState.hpp"

namespace SMRSharedData
{
	std::unordered_set<std::string> ReleasedTracks;
	std::unordered_set<std::string> ManuallyCorrelated;
}

namespace SMRPluginSharedData
{
	asio::io_service io_service;
}
