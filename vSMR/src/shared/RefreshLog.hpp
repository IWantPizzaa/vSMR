#pragma once

#include "shared/logging/Logger.hpp"

// Arguments always have normal C++ evaluation semantics in every configuration.
// Gate expensive formatting explicitly at its call site.
inline void VsmrRefreshLog(const std::string& message)
{
#if defined(_DEBUG)
	Logger::info(message);
#else
	(void)message;
#endif
}
