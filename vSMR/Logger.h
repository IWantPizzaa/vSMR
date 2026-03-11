#pragma once
#include "stdafx.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <mutex>
#include <ctime>

using namespace std;

class Logger {
public:
	enum class Mode
	{
		Normal,
		Verbose
	};

	static bool ENABLED;
	static string DLL_PATH;
	static Mode CURRENT_MODE;

	static void set_mode(Mode mode)
	{
		CURRENT_MODE = mode;
	}

	static Mode get_mode()
	{
		return CURRENT_MODE;
	}

	static bool is_verbose_mode()
	{
		return CURRENT_MODE == Mode::Verbose;
	}

	static const char* mode_name(Mode mode)
	{
		return mode == Mode::Verbose ? "verbose" : "normal";
	}

	static std::string build_local_timestamp() {
		std::time_t now = std::time(nullptr);
		if (now <= 0)
			return "";

		std::tm localTime = {};
		if (localtime_s(&localTime, &now) != 0)
			return "";

		char buffer[20] = {};
		if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime) == 0)
			return "";

		return std::string(buffer);
	}

	static std::mutex& log_write_mutex() {
		static std::mutex mutex;
		return mutex;
	}

	static bool is_compiler_signature_trace(const string& message) {
		return message.find("__cdecl") != string::npos ||
			message.find("__thiscall") != string::npos ||
			message.find("__stdcall") != string::npos;
	}

	static bool is_high_volume_trace_message(const string& message) {
		static const char* hotTraceMarkers[] = {
			"CRimcas::OnRefreshBegin(",
			"CRimcas::OnRefreshEnd(",
			"CRimcas::OnRefresh(",
			"CRimcas::AddRunwayArea(",
			"CRimcas::GetRunwayArea(",
			"CRimcas::GetAcInRunwayArea(",
			"CRimcas::GetAcInRunwayAreaSoon(",
			"CRimcas::AcOnRunwayFunc(",
			"CRimcas::isAcOnRunway(",
			"CRimcas::getAlert(",
			"CRimcas::getMovementAlert(",
			"CRimcas::GetAircraftColor(",
			"CSMRPlugin::OnTimer(",
			"CSMRPlugin::OnGetTagItem(",
			"CSMRPlugin::OnFlightPlanDisconnect(",
			"CSMRRadar::OnOverScreenObject(",
			"CSMRRadar::OnRadarTargetPositionUpdate(",
			"CSMRRadar::RefreshAirportActivity(",
			"CSMRRadar::GenerateTagData(",
			"CSMRRadar::GetBottomLine(",
			"CSMRRadar::OnFlightPlanDisconnect("
		};

		for (const char* marker : hotTraceMarkers)
		{
			if (message.find(marker) != string::npos)
				return true;
		}

		return false;
	}

	static bool should_skip_info_message(const string& message) {
		if (message.empty())
			return true;

		// Verbose mode is intentionally chatty for crash forensics.
		if (is_verbose_mode())
			return false;

		// In normal mode we keep the log concise by dropping function-signature
		// traces and profile-editor step-by-step instrumentation.
		if (is_compiler_signature_trace(message))
			return true;
		if (message.rfind("ProfileEditor: ", 0) == 0)
			return true;

		return false;
	}

	static void info(string message) {
		if (Logger::should_skip_info_message(message))
			return;

		if (!Logger::ENABLED || Logger::DLL_PATH.length() == 0)
			return;

		std::lock_guard<std::mutex> guard(Logger::log_write_mutex());
		std::ofstream file(Logger::DLL_PATH + "\\vsmr.log", std::ofstream::out | std::ofstream::app);
		if (!file.is_open())
			return;

		const std::string timestamp = Logger::build_local_timestamp();
		if (!timestamp.empty())
			file << timestamp << " ";
		file << "INFO: " << message << endl;
	}
};
