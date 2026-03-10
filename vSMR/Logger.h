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
	static bool ENABLED;
	static string DLL_PATH;

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
			"CSMRRadar::OnOverScreenObject(",
			"CSMRRadar::OnRadarTargetPositionUpdate(",
			"CSMRRadar::RefreshAirportActivity("
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

		// Keep rare function-trace lines for crash diagnosis, but still drop the
		// known hot-loop traces that would flood vsmr.log during normal sessions.
		if (is_compiler_signature_trace(message) && is_high_volume_trace_message(message))
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
