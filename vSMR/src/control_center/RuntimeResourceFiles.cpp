#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "shared/TextUtils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
	std::string SanitizeFilePart(std::string value)
	{
		for (char& character : value)
		{
			const unsigned char byte = static_cast<unsigned char>(character);
			if (std::isalnum(byte) == 0 && character != '-' && character != '_')
				character = '_';
		}
		while (!value.empty() && value.front() == '_')
			value.erase(value.begin());
		while (!value.empty() && value.back() == '_')
			value.pop_back();
		return value;
	}

	std::string SourceFileName(std::string sourceUrl)
	{
		std::replace(sourceUrl.begin(), sourceUrl.end(), '\\', '/');
		const size_t suffix = sourceUrl.find_first_of("?#");
		if (suffix != std::string::npos)
			sourceUrl.resize(suffix);
		const size_t slash = sourceUrl.find_last_of('/');
		return slash == std::string::npos
			? sourceUrl
			: sourceUrl.substr(slash + 1);
	}

	std::filesystem::path AbsoluteNormalizedPath(
		const std::filesystem::path& value,
		std::error_code& error)
	{
		std::filesystem::path absolute = std::filesystem::absolute(value, error);
		if (error)
			return {};
		return absolute.lexically_normal();
	}

	std::filesystem::path BuildDownloadBasePath(
		VsmrResourceFiles::Kind kind,
		const std::filesystem::path& dataDirectory,
		const std::string& sourceUrl,
		const std::string& airport)
	{
		const std::filesystem::path sourceName =
			std::filesystem::u8path(SourceFileName(sourceUrl));
		std::string stem = SanitizeFilePart(sourceName.stem().u8string());
		std::string extension = ToLowerAsciiCopy(sourceName.extension().u8string());

		std::filesystem::path targetDirectory;
		if (kind == VsmrResourceFiles::Kind::Aviso)
		{
			targetDirectory = dataDirectory / "AVISO";
			if (extension != ".geojson" && extension != ".json")
				extension = ".geojson";
			// The airport is runtime state, not a filename. Treat it as untrusted
			// here because storage happens before the bridge validates the import.
			// Only an ICAO-shaped token may participate in a target filename.
			const std::string airportUpper = NormalizeAirportCodeCopy(airport);
			if (stem.empty())
				stem = airportUpper.empty() ? "AVISO" : airportUpper;
			else if (!airportUpper.empty() && ToUpperAsciiCopy(stem).find(airportUpper) == std::string::npos)
				stem = airportUpper + "_" + stem;
		}
		else
		{
			targetDirectory = dataDirectory / "Profiles";
			extension = ".json";
			if (stem.empty())
				stem = "vSMR_Profiles";
		}

		// GitHub downloads are always variants. Even a URL whose basename is the
		// canonical <ICAO>.geojson or vSMR_Profiles.json can never target
		// the user's original file.
		const std::string upperStem = ToUpperAsciiCopy(stem);
		if (upperStem.size() < 7 ||
			upperStem.substr(upperStem.size() - 7) != "_GITHUB")
		{
			stem += "_github";
		}
		return targetDirectory / (stem + extension);
	}
}

bool VsmrResourceFiles::NormalizeExistingFilePath(
	const std::string& sourcePath,
	std::string& normalizedPath,
	std::string& errorText)
{
	normalizedPath.clear();
	errorText.clear();
	if (sourcePath.empty())
	{
		errorText = "No file was selected.";
		return false;
	}

	std::error_code error;
	const std::filesystem::path path = AbsoluteNormalizedPath(
		std::filesystem::u8path(sourcePath),
		error);
	if (error || path.empty() ||
		!std::filesystem::is_regular_file(path, error) || error)
	{
		errorText = "The selected file is no longer available.";
		return false;
	}
	normalizedPath = path.u8string();
	return true;
}

bool VsmrResourceFiles::StoreGithubDownload(
	Kind kind,
	const std::string& dataPath,
	const std::string& sourceUrl,
	const std::string& airport,
	const std::string& contents,
	std::string& storedPath,
	std::string& errorText)
{
	storedPath.clear();
	errorText.clear();
	if (dataPath.empty() || contents.empty())
	{
		errorText = "The runtime data folder or downloaded file is unavailable.";
		return false;
	}

	std::error_code error;
	const std::filesystem::path dataDirectory = AbsoluteNormalizedPath(
		std::filesystem::u8path(dataPath),
		error);
	if (error || dataDirectory.empty())
	{
		errorText = "The vSMR_Data folder path is invalid.";
		return false;
	}

	const std::filesystem::path basePath = BuildDownloadBasePath(
		kind,
		dataDirectory,
		sourceUrl,
		airport);
	std::filesystem::create_directories(basePath.parent_path(), error);
	if (error)
	{
		errorText = "Unable to create the downloaded-resource folder.";
		return false;
	}

	std::filesystem::path temporaryPath;
	HANDLE temporaryFile = INVALID_HANDLE_VALUE;
	const std::string temporaryStem =
		".vsmr-download-" + std::to_string(::GetCurrentProcessId()) + "-" +
		std::to_string(::GetTickCount64());
	for (unsigned int attempt = 1; attempt < 10000; ++attempt)
	{
		temporaryPath = basePath.parent_path() /
			(temporaryStem + (attempt > 1 ? "-" + std::to_string(attempt) : "") + ".tmp");
		temporaryFile = ::CreateFileW(
			temporaryPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (temporaryFile != INVALID_HANDLE_VALUE)
			break;
		const DWORD createError = ::GetLastError();
		if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS)
			break;
	}
	if (temporaryFile == INVALID_HANDLE_VALUE)
	{
		errorText = "Unable to create an exclusive downloaded-resource file.";
		return false;
	}

	bool writeSucceeded = true;
	size_t writtenTotal = 0;
	while (writtenTotal < contents.size())
	{
		const size_t remaining = contents.size() - writtenTotal;
		const DWORD requested = static_cast<DWORD>(std::min<size_t>(remaining, 1024u * 1024u));
		DWORD written = 0;
		if (!::WriteFile(
			temporaryFile,
			contents.data() + writtenTotal,
			requested,
			&written,
			nullptr) || written != requested)
		{
			writeSucceeded = false;
			break;
		}
		writtenTotal += written;
	}
	if (writeSucceeded)
		writeSucceeded = ::FlushFileBuffers(temporaryFile) != FALSE;
	::CloseHandle(temporaryFile);
	if (!writeSucceeded)
	{
		error.clear();
		std::filesystem::remove(temporaryPath, error);
		errorText = "Unable to finish writing the downloaded-resource file.";
		return false;
	}

	for (unsigned int variant = 1; variant < 10000; ++variant)
	{
		std::filesystem::path candidate = basePath;
		if (variant > 1)
		{
			candidate = basePath.parent_path() /
				std::filesystem::u8path(
					basePath.stem().u8string() + "_" +
					std::to_string(variant) +
					basePath.extension().u8string());
		}
		error.clear();
		if (std::filesystem::exists(candidate, error))
			continue;
		if (error)
			break;

		if (::MoveFileExW(
			temporaryPath.c_str(),
			candidate.c_str(),
			MOVEFILE_WRITE_THROUGH))
		{
			storedPath = candidate.lexically_normal().u8string();
			return true;
		}

		const DWORD moveError = ::GetLastError();
		if (moveError != ERROR_FILE_EXISTS && moveError != ERROR_ALREADY_EXISTS)
			break;
	}

	error.clear();
	std::filesystem::remove(temporaryPath, error);
	errorText = "Unable to install the downloaded resource without overwriting an existing file.";
	return false;
}
