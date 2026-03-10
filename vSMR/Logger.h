#pragma once
#include "stdafx.h"
#include <string>
#include <sstream>
#include <sstream>
#include <iomanip>
#include <fstream>

using namespace std;

class Logger {
public:
	static bool ENABLED;
	static string DLL_PATH;

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

		if (message.rfind("ProfileEditor: ", 0) == 0)
			return true;

		return false;
	}

	static void info(string message) {
		if (Logger::should_skip_info_message(message))
			return;

		if (Logger::ENABLED && Logger::DLL_PATH.length() > 0) {
			std::ofstream file;
			file.open(Logger::DLL_PATH + "\\vsmr.log", std::ofstream::out | std::ofstream::app);
			file << "INFO: " << message << endl;
			file.close();
		}
	}
};
