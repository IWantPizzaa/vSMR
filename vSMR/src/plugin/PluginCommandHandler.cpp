#include "platform/windows/PrecompiledHeader.hpp"

#include "plugin/PluginCommandHandler.hpp"

#include "plugin/Plugin.hpp"
#include "radar/RadarScreen.hpp"
#include "rdf/RdfOverlay.hpp"
#include "shared/TextUtils.hpp"

#include <string>

bool VsmrPluginCommandHandler::Handle(
	CSMRPlugin& plugin,
	const char* commandLine,
	const std::vector<CSMRRadar*>& radarScreens)
{
	const std::string command = TrimAsciiWhitespaceCopy(
		commandLine == nullptr ? "" : std::string(commandLine));
	const std::string commandLower = ToLowerAsciiCopy(command);
	const auto startsWithCommand = [&commandLower](const char* prefix)
	{
		return prefix != nullptr && commandLower.rfind(prefix, 0) == 0;
	};

	if (commandLower == ".smr diagnostics" || commandLower == ".smr diag")
	{
		std::string reportPath;
		std::string error;
		if (plugin.WriteDiagnosticsReport(reportPath, error))
		{
			const std::string message =
				"Redacted diagnostics written to " + reportPath;
			plugin.DisplayUserMessage(
				"vSMR", "Diagnostics", message.c_str(),
				true, true, false, true, false);
			Logger::info("Diagnostics report written path=" + reportPath);
		}
		else
		{
			plugin.DisplayUserMessage(
				"vSMR", "Diagnostics", error.c_str(),
				true, true, false, true, false);
		}
		return true;
	}

	if (commandLower == ".smr reload")
	{
		for (CSMRRadar* radar : radarScreens)
		{
			if (radar != nullptr)
				radar->ReloadConfig();
		}
		plugin.DisplayUserMessage(
			"vSMR", "Config", "Reloaded vSMR runtime data",
			true, true, false, true, false);
		return true;
	}

	if (commandLower == ".smr rdf on" || commandLower == ".smr rdf off")
	{
		const bool enabled = commandLower == ".smr rdf on";
		VsmrRdf::SetEnabled(enabled);
		plugin.SaveDataToSettings(
			"rdf_enabled",
			"Enable the native vSMR RDF overlay",
			enabled ? "1" : "0");
		plugin.DisplayUserMessage(
			"vSMR",
			"RDF",
			enabled ? "Native RDF enabled" : "Native RDF disabled",
			true, true, false, true, false);
		for (CSMRRadar* radar : radarScreens)
		{
			if (radar != nullptr && !radar->IsShutdownRequested())
				radar->RequestRefresh();
		}
		return true;
	}

	if (startsWithCommand(".smr log"))
	{
		constexpr char prefix[] = ".smr log";
		constexpr std::size_t prefixLength = sizeof(prefix) - 1;
		const std::string argument = commandLower.size() > prefixLength
			? TrimAsciiWhitespaceCopy(commandLower.substr(prefixLength))
			: std::string();
		const auto publishLogStatus = [&plugin](const std::string& action)
		{
			std::string detail = action + " - vsmr.log ";
			detail += Logger::ENABLED ? "enabled" : "disabled";
			if (Logger::ENABLED)
			{
				detail += " (";
				detail += Logger::mode_name(Logger::get_mode());
				detail += ")";
			}
			detail += " at " + Logger::DLL_PATH + "\\vsmr.log";
			plugin.DisplayUserMessage(
				"vSMR", "Log", detail.c_str(),
				true, true, false, true, false);
			if (Logger::ENABLED)
			{
				Logger::info(
					"Logging active mode=" +
					std::string(Logger::mode_name(Logger::get_mode())));
			}
		};

		if (argument.empty())
		{
			if (Logger::ENABLED)
				Logger::ENABLED = false;
			else
			{
				Logger::ENABLED = true;
				Logger::set_mode(Logger::Mode::Normal);
			}
			publishLogStatus("Updated");
			return true;
		}
		if (argument == "status")
		{
			publishLogStatus("Status");
			return true;
		}
		if (argument == "off" || argument == "disable" || argument == "0")
		{
			Logger::ENABLED = false;
			publishLogStatus("Updated");
			return true;
		}
		if (argument == "on" || argument == "enable" || argument == "1" ||
			argument == "normal" || argument == "n")
		{
			Logger::ENABLED = true;
			Logger::set_mode(Logger::Mode::Normal);
			publishLogStatus("Updated");
			return true;
		}
		if (argument == "verbose" || argument == "v")
		{
			Logger::ENABLED = true;
			Logger::set_mode(Logger::Mode::Verbose);
			publishLogStatus("Updated");
			return true;
		}

		plugin.DisplayUserMessage(
			"vSMR", "Log", "Usage: .smr log [normal|verbose|off|status]",
			true, true, false, true, false);
		return true;
	}

	if (commandLower == ".smr editor")
	{
		for (CSMRRadar* radar : radarScreens)
		{
			if (radar == nullptr)
				continue;
			radar->OpenVsmrControlCenterWindow("overview");
			return true;
		}
		plugin.DisplayUserMessage(
			"vSMR", "Config",
			"No active SMR radar screen found to open the vSMR window.",
			true, true, false, true, false);
		return true;
	}

	if (commandLower == ".smr")
	{
		for (CSMRRadar* radar : radarScreens)
		{
			if (radar == nullptr)
				continue;
			radar->OpenVsmrControlCenterWindow("settings");
			return true;
		}
		plugin.DisplayUserMessage(
			"vSMR", "Config",
			"No active SMR radar screen found to open the vSMR window.",
			true, true, false, true, false);
		return true;
	}

	return false;
}
