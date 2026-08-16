#pragma once

#include <string>

namespace VsmrResourceFiles
{
	enum class Kind
	{
		Aviso,
		Profiles
	};

	bool NormalizeExistingFilePath(
		const std::string& sourcePath,
		std::string& normalizedPath,
		std::string& errorText);

	bool StoreGithubDownload(
		Kind kind,
		const std::string& dataPath,
		const std::string& sourceUrl,
		const std::string& airport,
		const std::string& contents,
		std::string& storedPath,
		std::string& errorText);
}
