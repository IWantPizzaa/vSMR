#pragma once
#include "stdafx.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <mutex>
#include <ctime>
#include <deque>
#include <vector>
#include <atomic>

using namespace std;

class Logger {
public:
	enum class Mode
	{
		Normal,
		Verbose
	};

	static std::atomic<bool> ENABLED;
	static string DLL_PATH;
	static std::atomic<Mode> CURRENT_MODE;

	static void set_mode(Mode mode)
	{
		CURRENT_MODE.store(mode, std::memory_order_release);
	}

	static Mode get_mode()
	{
		return CURRENT_MODE.load(std::memory_order_acquire);
	}

	static bool is_verbose_mode()
	{
		return CURRENT_MODE.load(std::memory_order_acquire) == Mode::Verbose;
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

	static std::deque<std::string>& recent_log_messages() {
		static std::deque<std::string> messages;
		return messages;
	}

	static std::vector<std::string> recent_messages() {
		std::lock_guard<std::mutex> guard(Logger::log_write_mutex());
		return std::vector<std::string>(
			Logger::recent_log_messages().begin(),
			Logger::recent_log_messages().end());
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
		if (is_high_volume_trace_message(message))
			return true;
		if (message.rfind("ProfileEditor: ", 0) == 0)
			return true;

		return false;
	}

	static void info(string message) {
		if (Logger::should_skip_info_message(message))
			return;

		const std::string timestamp = Logger::build_local_timestamp();
		const std::string formatted =
			(timestamp.empty() ? std::string() : timestamp + " ") +
			"INFO: " + message;
		std::lock_guard<std::mutex> guard(Logger::log_write_mutex());
		auto& recent = Logger::recent_log_messages();
		recent.push_back(formatted);
		while (recent.size() > 256)
			recent.pop_front();

		if (!Logger::ENABLED.load(std::memory_order_acquire) || Logger::DLL_PATH.length() == 0)
			return;

		const std::string logPath = Logger::DLL_PATH + "\\vsmr.log";
		// Keep support logs bounded. Probe periodically so verbose rendering traces
		// do not turn the size check itself into a hot path.
		static unsigned long writesUntilSizeProbe = 0;
		if (writesUntilSizeProbe == 0)
		{
			WIN32_FILE_ATTRIBUTE_DATA attributes = {};
			if (::GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &attributes) &&
				attributes.nFileSizeHigh == 0 &&
				attributes.nFileSizeLow >= 4U * 1024U * 1024U)
			{
				const std::string previousLogPath = logPath + ".1";
				::DeleteFileA(previousLogPath.c_str());
				::MoveFileExA(
					logPath.c_str(),
					previousLogPath.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			}
			writesUntilSizeProbe = 64;
		}
		else
		{
			--writesUntilSizeProbe;
		}

		std::ofstream file(logPath, std::ofstream::out | std::ofstream::app);
		if (!file.is_open())
			return;

		file << formatted << endl;
	}
};
