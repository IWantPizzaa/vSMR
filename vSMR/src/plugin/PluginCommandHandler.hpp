#pragma once

#include <vector>

class CSMRPlugin;
class CSMRRadar;

class VsmrPluginCommandHandler final
{
public:
	static bool Handle(
		CSMRPlugin& plugin,
		const char* commandLine,
		const std::vector<CSMRRadar*>& radarScreens);
};
