#pragma once

#include <vector>

class CSMRPlugin;
class CSMRRadar;

// EuroScope owns the callback boundary in CSMRPlugin. Keeping command dispatch
// here leaves that callback responsible only for host-state and crash guards.
class VsmrPluginCommandHandler final
{
public:
	static bool Handle(
		CSMRPlugin& plugin,
		const char* commandLine,
		const std::vector<CSMRRadar*>& radarScreens);
};
