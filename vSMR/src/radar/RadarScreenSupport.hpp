#pragma once

#include <string>

// Path resolution is shared by radar construction and AVISO loading. Keeping
// it here avoids coupling the general radar coordinator to AVISO internals.
namespace VsmrRadarSupport
{
	std::string ResolvePluginDataDirectoryPath(const std::string& dllPath);
	std::string ResolvePluginFilePath(
		const std::string& dllPath,
		const char* fileName);
	std::string ResolvePluginDirectoryPath(
		const std::string& dllPath,
		const char* directoryName);
}
