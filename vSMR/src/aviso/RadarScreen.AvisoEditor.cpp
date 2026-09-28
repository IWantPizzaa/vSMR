#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoOverrides.hpp"
#include "aviso/AvisoSharedConfig.hpp"
#include "config/LayeredConfig.hpp"
#include "insets/InsetWindow.hpp"

#include <cctype>

namespace
{
	std::string TrimAsciiWhitespaceCopy(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;
		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	}

	std::string ToUpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	bool IsAirportCode(const std::string& value)
	{
		return value.size() == 4 &&
			std::all_of(value.begin(), value.end(), [](unsigned char character) {
				return std::isalnum(character) != 0;
			});
	}

	bool IsRegularFileNoThrow(const std::filesystem::path& path)
	{
		try
		{
			return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
		}
		catch (...)
		{
			return false;
		}
	}

}

std::string CSMRRadar::GetAvisoDefaultSource(const std::string& path, std::string& error) const
{
	return CurrentConfig != nullptr ? VsmrAvisoOverrides::DefaultSource(*CurrentConfig,
		std::filesystem::u8path(DataPath), std::filesystem::u8path(path), error) : path;
}

bool CSMRRadar::ApplyAvisoUserOverrides(const std::string& path,
	rapidjson::Document& document, std::string& error) const
{
	if (CurrentConfig == nullptr ||
		!VsmrAvisoOverrides::IsManagedSource(std::filesystem::u8path(DataPath),
			std::filesystem::u8path(path))) return true;
	if (!CurrentConfig->isLayeredConfig())
	{
		const auto shared = VsmrAvisoSharedConfig::Read(std::filesystem::u8path(DataPath));
		if (!shared.error.empty()) { error = shared.error; return false; }
		const auto* airports = shared.document != nullptr
			? VsmrLayeredConfig::Member(*shared.document, "aviso") : nullptr;
		const auto* overrides = airports != nullptr ? VsmrLayeredConfig::Member(*airports,
			VsmrAvisoOverrides::AirportKey(std::filesystem::u8path(path)).c_str()) : nullptr;
		return VsmrAvisoOverrides::Apply(document, overrides, error);
	}
	return VsmrAvisoOverrides::Apply(document,
		VsmrAvisoOverrides::Find(*CurrentConfig,
			VsmrAvisoOverrides::AirportKey(std::filesystem::u8path(path))), error);
}

std::string CSMRRadar::GetAvisoOverridesRevision(bool forceRefresh) const
{
	if (CurrentConfig != nullptr && CurrentConfig->isLayeredConfig())
		return forceRefresh ? CurrentConfig->getPersistedConfigRevision() : CurrentConfig->getConfigRevision();
	const auto shared = VsmrAvisoSharedConfig::Read(std::filesystem::u8path(DataPath), forceRefresh);
	return shared.revision + ":" + shared.error;
}

std::string CSMRRadar::GetAvisoGeoJsonEditorPathForAirport(const std::string& airport) const
{
	const std::string airportUpper = ToUpperAscii(TrimAsciiWhitespaceCopy(airport));
	if (!IsAirportCode(airportUpper))
		return "";

	const std::string existingPath = ResolveAvisoGeoJsonPathForAirport(airportUpper);
	if (!existingPath.empty())
		return existingPath;

	const std::filesystem::path dataDirectory = DataPath.empty()
		? (std::filesystem::u8path(DllPath) / "vSMR_Data")
		: std::filesystem::u8path(DataPath);
	const std::filesystem::path preferredPath = dataDirectory / "AVISO" / (airportUpper + ".geojson");
	return preferredPath.u8string();
}

void CSMRRadar::SetAvisoGeoJsonOverrideForAirport(const std::string& airport, const std::string& path)
{
	const std::string airportUpper = ToUpperAscii(TrimAsciiWhitespaceCopy(airport));
	if (!IsAirportCode(airportUpper))
		return;

	std::string normalizedPath = path;
	if (!path.empty())
	{
		try
		{
			normalizedPath =
				std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().u8string();
		}
		catch (...)
		{
			// Preserve the caller-provided path when absolute normalization fails.
			// The normal AVISO validation path will report an unavailable source.
		}
	}

	// Keeping every open radar on the same airport-specific source
	std::vector<CSMRRadar*> targets;
	for (CSMRRadar* radar : RadarScreensOpened)
	{
		if (radar != nullptr &&
			std::find(targets.begin(), targets.end(), radar) == targets.end())
		{
			targets.push_back(radar);
		}
	}
	if (std::find(targets.begin(), targets.end(), this) == targets.end())
		targets.push_back(this);

	for (CSMRRadar* radar : targets)
	{
		if (normalizedPath.empty())
			radar->AvisoGeoJsonOverridePaths.erase(airportUpper);
		else
			radar->AvisoGeoJsonOverridePaths[airportUpper] = normalizedPath;
		radar->AvisoGeoJsonResolvedAirport.clear();
		radar->AvisoGeoJsonResolvedDllPath.clear();
		radar->AvisoGeoJsonResolvedPath.clear();
		radar->AvisoGeoJsonLastStatTick = 0;
		const std::string asrKey = "Insets." + airportUpper + ".AvisoFile";
		radar->SaveDataToAsr(
			asrKey.c_str(),
			"Airport-specific AVISO source file",
			normalizedPath.c_str());
		radar->RequestRefresh();
	}
}

bool CSMRRadar::ForceReloadAvisoGeoJson()
{
	if (IsShutdownRequested())
		return false;

	const std::string airport = getActiveAirport();
	const std::string path = ResolveAvisoGeoJsonPathForAirport(airport);
	if (path.empty() || !IsRegularFileNoThrow(path))
		return false;

	// Validating before touching the renderer keeps the current dataset on failure
	AvisoDocumentModel validationModel;
	std::string validationError;
	const std::string defaultSource = GetAvisoDefaultSource(path, validationError);
	if (defaultSource.empty() || !validationModel.LoadFromFile(defaultSource, validationError) ||
		!ApplyAvisoUserOverrides(path, validationModel.MutableDocument(), validationError))
	{
		Logger::info(
			"AVISO GeoJSON reload validation failed path=" + path +
			" error=" + validationError);
		return false;
	}
	const AvisoValidationResult semanticValidation =
		validationModel.ValidateAndRecalculate();
	if (!semanticValidation.ok)
	{
		Logger::info(
			"AVISO GeoJSON reload semantic validation failed path=" + path +
			" error=" + semanticValidation.errorText);
		return false;
	}

	AvisoGeoJsonResolvedAirport.clear();
	AvisoGeoJsonResolvedDllPath.clear();
	AvisoGeoJsonResolvedPath.clear();
	const bool previousLoadAttempted = AvisoGeoJsonLoadAttempted;
	const unsigned long previousLastStatTick = AvisoGeoJsonLastStatTick;
	AvisoGeoJsonLastFailedPath.clear();
	AvisoGeoJsonLastFailedTick = 0;
	AvisoGeoJsonLastFailedWriteTimeValid = false;
	AvisoGeoJsonLastStatTick = 0;
	AvisoGeoJsonLoadAttempted = false;

	const bool loaded = EnsureAvisoGeoJsonLoaded(path, false);
	if (!loaded)
	{
		// EnsureAvisoGeoJsonLoaded is transactional. Restore its throttle state as
		// well so the current renderer remains the authoritative loaded source.
		AvisoGeoJsonLoadAttempted = previousLoadAttempted;
		AvisoGeoJsonLastStatTick = previousLastStatTick;
	}
	RequestRefresh();
	return loaded;
}
