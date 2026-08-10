#include "stdafx.h"
#include "SMRRadar.hpp"
#include "AvisoDocumentModel.hpp"
#include "AvisoEditorDialog.hpp"
#include "InsetWindow.h"

#include <cctype>

extern std::vector<CSMRRadar*> RadarScreensOpened;

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

	CRect BuildDefaultAvisoEditorWindowRect()
	{
		CRect fallback(120, 120, 1060, 740);
		CWnd* mainWindow = AfxGetMainWnd();
		if (mainWindow != nullptr && ::IsWindow(mainWindow->GetSafeHwnd()))
		{
			CRect mainRect;
			mainWindow->GetWindowRect(&mainRect);
			if (!mainRect.IsRectEmpty())
			{
				fallback.left = mainRect.left + 80;
				fallback.top = mainRect.top + 80;
				fallback.right = fallback.left + 940;
				fallback.bottom = fallback.top + 620;
			}
		}
		return fallback;
	}
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
		? (std::filesystem::path(DllPath) / "vSMR_Data")
		: std::filesystem::path(DataPath);
	const std::filesystem::path preferredPath = dataDirectory / "AVISO" / (airportUpper + ".geojson");
	return preferredPath.string();
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
				std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().string();
		}
		catch (...) {}
	}

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

	AvisoDocumentModel validationModel;
	std::string validationError;
	if (!validationModel.LoadFromFile(path, validationError))
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
	if (AvisoEditorDialog && ::IsWindow(AvisoEditorDialog->GetSafeHwnd()) && AvisoEditorDialog->IsWindowVisible())
		AvisoEditorDialog->SyncFromRadar();
	RequestRefresh();
	return loaded;
}

bool CSMRRadar::EnsureAvisoEditorWindowCreated()
{
	if (AvisoEditorDialog && ::IsWindow(AvisoEditorDialog->GetSafeHwnd()))
	{
		AvisoEditorDialog->SetOwner(this);
		return true;
	}

	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	AvisoEditorDialog = std::make_unique<CAvisoEditorDialog>(this, AfxGetMainWnd());
	if (!AvisoEditorDialog->Create(CAvisoEditorDialog::IDD, AfxGetMainWnd()))
	{
		AvisoEditorDialog.reset();
		return false;
	}

	const CRect windowRect = BuildDefaultAvisoEditorWindowRect();
	AvisoEditorDialog->SetWindowPos(
		nullptr,
		windowRect.left,
		windowRect.top,
		max(820, windowRect.Width()),
		max(520, windowRect.Height()),
		SWP_NOZORDER | SWP_NOACTIVATE);
	AvisoEditorDialog->ShowWindow(SW_HIDE);
	return true;
}

void CSMRRadar::OpenAvisoEditorWindow()
{
	OpenVsmrControlCenterWindow("aviso");
}

void CSMRRadar::CloseAvisoEditorWindow()
{
	if (!AvisoEditorDialog || !::IsWindow(AvisoEditorDialog->GetSafeHwnd()))
		return;

	AvisoEditorDialog->ShowWindow(SW_HIDE);
}

void CSMRRadar::DestroyAvisoEditorWindow()
{
	if (!AvisoEditorDialog)
		return;

	if (::IsWindow(AvisoEditorDialog->GetSafeHwnd()))
		AvisoEditorDialog->DestroyWindow();

	AvisoEditorDialog.reset();
}

void CSMRRadar::OnAvisoEditorWindowClosed()
{
	RequestRefresh();
}
