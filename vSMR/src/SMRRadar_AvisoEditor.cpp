#include "stdafx.h"
#include "SMRRadar.hpp"
#include "AvisoEditorDialog.hpp"
#include "InsetWindow.h"

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
		CRect fallback(120, 120, 820, 600);
		CWnd* mainWindow = AfxGetMainWnd();
		if (mainWindow != nullptr && ::IsWindow(mainWindow->GetSafeHwnd()))
		{
			CRect mainRect;
			mainWindow->GetWindowRect(&mainRect);
			if (!mainRect.IsRectEmpty())
			{
				fallback.left = mainRect.left + 80;
				fallback.top = mainRect.top + 80;
				fallback.right = fallback.left + 700;
				fallback.bottom = fallback.top + 480;
			}
		}
		return fallback;
	}
}

std::string CSMRRadar::GetAvisoGeoJsonEditorPathForAirport(const std::string& airport) const
{
	const std::string airportUpper = ToUpperAscii(TrimAsciiWhitespaceCopy(airport));
	if (airportUpper.empty())
		return "";

	const std::string existingPath = ResolveAvisoGeoJsonPathForAirport(airportUpper);
	if (!existingPath.empty())
		return existingPath;

	const std::filesystem::path dataDirectory = DataPath.empty()
		? (std::filesystem::path(DllPath) / "vSMR_Data")
		: std::filesystem::path(DataPath);
	const std::filesystem::path preferredPath = dataDirectory / "AVISO" / ("AVISO_" + airportUpper + ".geojson");
	return preferredPath.string();
}

bool CSMRRadar::ForceReloadAvisoGeoJson()
{
	if (IsShutdownRequested())
		return false;

	const std::string airport = getActiveAirport();
	AvisoGeoJsonResolvedAirport.clear();
	AvisoGeoJsonResolvedDllPath.clear();
	AvisoGeoJsonResolvedPath.clear();
	AvisoGeoJsonFeatures.clear();
	AvisoGeoJsonLabels.clear();
	AvisoGeoJsonFeatureSnapshot.reset();
	AvisoGeoJsonLabelSnapshot.reset();
	AvisoGeoJsonLoadedPath.clear();
	AvisoGeoJsonViewInitializedPath.clear();
	AvisoGeoJsonLastStatTick = 0;
	AvisoGeoJsonLoadAttempted = false;
	AvisoGeoJsonLoaded = false;
	AvisoGeoJsonRenderDisabled = false;
	AvisoGeoJsonRenderDisabledPath.clear();
	AvisoGeoJsonHasBounds = false;
	AvisoGeoJsonMinLongitude = 0.0;
	AvisoGeoJsonMinLatitude = 0.0;
	AvisoGeoJsonMaxLongitude = 0.0;
	AvisoGeoJsonMaxLatitude = 0.0;
	AvisoGeoJsonLastViewValid = false;
	AvisoGeoJsonLastViewPath.clear();
	AvisoGeoJsonLastViewChangeTick = 0;
	ClearAvisoGeoJsonRasterCache();
	{
		std::lock_guard<std::mutex> guard(AvisoGeoJsonRenderMutex);
		++AvisoGeoJsonRenderLatestRequestId;
		AvisoGeoJsonPendingRenderRequest.reset();
		AvisoGeoJsonCompletedRenderResult.reset();
		AvisoGeoJsonRenderLastRequestValid = false;
	}

	for (auto& kv : appWindows)
	{
		CInsetWindow* appWindow = kv.second.get();
		if (appWindow != nullptr && appWindow->IsAvisoViewport())
			appWindow->ClearAvisoViewportCache();
	}

	const std::string path = ResolveAvisoGeoJsonPathForAirport(airport);
	const bool loaded = !path.empty() && IsRegularFileNoThrow(path) && EnsureAvisoGeoJsonLoaded(path);
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
		max(560, windowRect.Width()),
		max(380, windowRect.Height()),
		SWP_NOZORDER | SWP_NOACTIVATE);
	AvisoEditorDialog->ShowWindow(SW_HIDE);
	return true;
}

void CSMRRadar::OpenAvisoEditorWindow()
{
	if (!EnsureAvisoEditorWindowCreated())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "AVISO Editor", "Failed to open AVISO Editor window.", true, true, false, false, false);
		RequestRefresh();
		return;
	}

	AvisoEditorDialog->ShowWindow(SW_SHOW);
	AvisoEditorDialog->BringWindowToTop();
	AvisoEditorDialog->SyncFromRadar();
	RequestRefresh();
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
