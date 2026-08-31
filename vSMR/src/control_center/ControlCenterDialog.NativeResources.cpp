#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterDialog.Internal.hpp"

#include "control_center/ControlCenterBridge.hpp"
#include "radar/RadarScreen.hpp"
#include "shared/logging/Logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace VsmrControlCenterDialogInternal;

namespace VsmrControlCenterDialogInternal
{
	bool ReadTextFile(
		const std::filesystem::path& path,
		std::string& text,
		size_t maximumBytes)
	{
		text.clear();
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
			return false;
		input.seekg(0, std::ios::end);
		const std::streamoff length = input.tellg();
		if (length < 0 || static_cast<unsigned long long>(length) > maximumBytes)
			return false;
		input.seekg(0, std::ios::beg);
		std::ostringstream stream;
		stream << input.rdbuf();
		text = stream.str();
		if (text.size() > maximumBytes)
		{
			text.clear();
			return false;
		}
		return static_cast<bool>(input) || input.eof();
	}
}

namespace
{
	enum class ResourceFileDialogResult
	{
		Selected,
		Cancelled,
		Failed
	};

	ResourceFileDialogResult SelectResourceFile(
		HWND ownerWindow,
		bool profiles,
		std::filesystem::path& selectedPath) noexcept
	{
		selectedPath.clear();
		try
		{
			std::array<wchar_t, 32768> pathBuffer{};
			const wchar_t profileFilters[] =
				L"vSMR profiles (*.json)\0*.json\0All files (*.*)\0*.*\0";
			const wchar_t avisoFilters[] =
				L"GeoJSON (*.geojson;*.json)\0*.geojson;*.json\0All files (*.*)\0*.*\0";

			OPENFILENAMEW dialog{};
			dialog.lStructSize = sizeof(dialog);
			dialog.hwndOwner = ownerWindow;
			dialog.lpstrFilter = profiles ? profileFilters : avisoFilters;
			dialog.nFilterIndex = 1;
			dialog.lpstrFile = pathBuffer.data();
			dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
			dialog.lpstrDefExt = profiles ? L"json" : L"geojson";
			dialog.Flags =
				OFN_EXPLORER |
				OFN_FILEMUSTEXIST |
				OFN_HIDEREADONLY |
				OFN_NOCHANGEDIR |
				OFN_PATHMUSTEXIST;

			if (!::GetOpenFileNameW(&dialog))
			{
				return ::CommDlgExtendedError() == 0
					? ResourceFileDialogResult::Cancelled
					: ResourceFileDialogResult::Failed;
			}

			selectedPath = std::filesystem::path(pathBuffer.data());
			return selectedPath.empty()
				? ResourceFileDialogResult::Failed
				: ResourceFileDialogResult::Selected;
		}
		catch (...)
		{
			selectedPath.clear();
			return ResourceFileDialogResult::Failed;
		}
	}
}

void CVsmrControlCenterDialog::RequestComputerResource(
	const std::string& resource,
	const std::string& requestId)
{
	const bool profiles = resource == "profiles";
	std::filesystem::path path;
	const ResourceFileDialogResult selection = SelectResourceFile(
		GetSafeHwnd(),
		profiles,
		path);
	if (selection == ResourceFileDialogResult::Cancelled)
		return;
	if (selection != ResourceFileDialogResult::Selected)
	{
		if (Bridge)
			Bridge->PushError(requestId, "Unable to open the Windows file picker.");
		return;
	}

	std::string text;
	if (!ReadTextFile(path, text, kMaximumResourceBytes))
	{
		if (Bridge)
			Bridge->PushError(requestId, "Unable to read the selected file or it exceeds the 16 MB resource limit.");
		return;
	}
	if (Bridge)
		Bridge->HandleLoadedResource(
			resource,
			path.u8string(),
			requestId,
			text,
			path.u8string());
}

void CVsmrControlCenterDialog::RequestResetDefaults(
	const std::string& requestId)
{
	const std::filesystem::path resourceFolder(ResolveWebResourceFolder());
	const std::filesystem::path profilesPath =
		resourceFolder / L"defaults" / L"vSMR_Profiles.json";
	const std::filesystem::path dataDirectory = Owner != nullptr && !Owner->GetDataPath().empty()
		? std::filesystem::u8path(Owner->GetDataPath())
		: std::filesystem::u8path(
			Owner != nullptr && !Owner->GetDllPath().empty()
				? Owner->GetDllPath()
				: Logger::DLL_PATH) / "vSMR_Data";
	std::string activeAirport = Owner != nullptr
		? Owner->getActiveAirport()
		: std::string();
	activeAirport.erase(
		std::remove_if(
			activeAirport.begin(),
			activeAirport.end(),
			[](unsigned char character) {
				return std::isspace(character) != 0;
			}),
		activeAirport.end());
	std::transform(
		activeAirport.begin(),
		activeAirport.end(),
		activeAirport.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::toupper(character));
		});
	const bool hasNormalizedAirport =
		activeAirport.size() == 4 &&
		std::all_of(
			activeAirport.begin(),
			activeAirport.end(),
			[](unsigned char character) {
				return std::isalnum(character) != 0;
			});
	std::filesystem::path avisoPath;
	if (hasNormalizedAirport)
	{
		const std::filesystem::path avisoDirectory = dataDirectory / "AVISO";
		avisoPath = avisoDirectory /
			std::filesystem::path(activeAirport + ".geojson");
	}
	std::error_code avisoExistsError;
	const bool hasMatchingAvisoDefault = !avisoPath.empty() &&
		std::filesystem::is_regular_file(avisoPath, avisoExistsError);

	std::string profilesText;
	std::string avisoText;
	if (!ReadTextFile(profilesPath, profilesText, kMaximumResourceBytes))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"The bundled profile defaults are missing.");
		return;
	}
	if (hasMatchingAvisoDefault && !ReadTextFile(avisoPath, avisoText, kMaximumResourceBytes))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"The bundled " + activeAirport +
				" AVISO default could not be read.");
		return;
	}

	if (!Bridge)
		return;

	std::string validationError;
	if (!Bridge->ValidateLoadedResource(
			"profiles",
			profilesText,
			validationError))
	{
		Bridge->PushError(
			requestId,
			validationError.empty()
				? "The bundled profile defaults are invalid."
				: validationError);
		return;
	}
	if (hasMatchingAvisoDefault &&
		!Bridge->ValidateLoadedResource(
			"aviso",
			avisoText,
			validationError))
	{
		Bridge->PushError(
			requestId,
			validationError.empty()
				? "The bundled " + activeAirport +
					" AVISO default is invalid."
				: validationError);
		return;
	}

	if (hasMatchingAvisoDefault)
	{
		// Stage the optional airport AVISO first.  Profiles are sent last so
		// the Web UI treats their validated arrival as completion of the
		// multi-resource recovery request and cannot save a half-staged reset.
		if (!Bridge->HandleLoadedResource(
			"aviso",
			"bundled defaults",
			requestId,
			avisoText))
		{
			return;
		}
	}
	Bridge->HandleLoadedResource(
		"profiles",
		"bundled defaults",
		requestId,
		profilesText);
}

std::wstring CVsmrControlCenterDialog::ResolveWebResourceFolder() const
{
	std::vector<std::filesystem::path> candidates;
	if (Owner != nullptr)
	{
		candidates.emplace_back(
			std::filesystem::u8path(Owner->GetDataPath()) / "vSMR_webUI");
		candidates.emplace_back(
			std::filesystem::u8path(Owner->GetDllPath()) / "vSMR_webUI");
	}
	try
	{
		candidates.emplace_back(
			std::filesystem::current_path() / "vSMR" / "src" / "control_center" / "web");
		candidates.emplace_back(
			std::filesystem::current_path() / "src" / "control_center" / "web");
	}
	catch (...)
	{
	}

	for (const std::filesystem::path& candidate : candidates)
	{
		try
		{
			const std::array<const char*, 4> requiredAssets = {
				"index.html",
				"styles.css",
				"data.js",
				"app-bundle.js"
			};
			const bool complete = std::all_of(
				requiredAssets.begin(),
				requiredAssets.end(),
				[&candidate](const char* name)
				{
					return std::filesystem::is_regular_file(candidate / name);
				});
			if (complete)
				return std::filesystem::absolute(candidate).wstring();
		}
		catch (...)
		{
		}
	}
	return {};
}
