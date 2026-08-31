#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"
#include "datalink/DatalinkProtocolSupport.hpp"
#include "plugin/PluginMetadata.hpp"
#include "radar/RadarScreen.hpp"
#include "radar/RadarScreen.Registry.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using VsmrDatalinkProtocol::EncodeUrlQueryComponent;
using VsmrDatalinkProtocol::RedactSensitiveValue;

namespace
{
	struct RuntimeCredentialsSnapshot
	{
		std::string callsign;
		std::string password;
	};

	RuntimeCredentialsSnapshot SnapshotRuntimeCredentials()
	{
		std::lock_guard<std::mutex> guard(DatalinkControlMutex);
		return { logonCallsign, logonCode };
	}
}
bool CSMRPlugin::QueueNetworkJob(std::function<void()> job)
{
	if (!job || PluginShutdownRequested.load(std::memory_order_acquire) ||
		NetworkCancellationRequested.load(std::memory_order_acquire))
	{
		return false;
	}

	std::unique_lock<std::mutex> lock(NetworkWorkerMutex);
	if (NetworkWorkersStopping)
		return false;
	if (NetworkWorkers.empty())
	{
		try
		{
			NetworkWorkers.emplace_back(&CSMRPlugin::NetworkWorkerMain, this);
			try
			{
				NetworkWorkers.emplace_back(&CSMRPlugin::NetworkWorkerMain, this);
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Network worker pool running with one worker: " +
					std::string(ex.what()));
			}
		}
		catch (const std::exception& ex)
		{
			Logger::info(
				"Unable to start network worker: " +
				std::string(ex.what()));
			return false;
		}
		catch (...)
		{
			Logger::info("Unable to start network worker: unknown error");
			return false;
		}
	}

	try
	{
		NetworkJobs.emplace_back(std::move(job));
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Unable to queue network request: " +
			std::string(ex.what()));
		return false;
	}
	lock.unlock();
	NetworkWorkerCondition.notify_one();
	return true;
}

void CSMRPlugin::NetworkWorkerMain()
{
	VsmrCrashRuntime::OwnedThreadRole crashThreadRole("network worker");
	{
		std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
		++NetworkWorkerThreadsRunning;
	}
	struct RunningWorkerGuard
	{
		std::mutex& mutex;
		std::size_t& count;
		~RunningWorkerGuard()
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (count > 0)
				--count;
		}
	} runningWorkerGuard{ NetworkWorkerMutex, NetworkWorkerThreadsRunning };
	try
	{
		for (;;)
		{
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(NetworkWorkerMutex);
				NetworkWorkerCondition.wait(lock, [this]() {
					return NetworkWorkersStopping || !NetworkJobs.empty();
				});
				if (NetworkWorkersStopping)
					return;
				job = std::move(NetworkJobs.front());
				NetworkJobs.pop_front();
				++NetworkJobsInFlight;
			}
			struct InFlightJobGuard
			{
				std::mutex& mutex;
				std::size_t& count;
				~InFlightJobGuard()
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (count > 0)
						--count;
				}
			} inFlightJobGuard{ NetworkWorkerMutex, NetworkJobsInFlight };

			try
			{
				job();
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Network job exception: " +
						std::string(ex.what()));
			}
			catch (...)
			{
				Logger::info("Network job exception: unknown");
			}
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Network worker terminated by exception: " +
			std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("Network worker terminated by unknown exception");
	}
}

void CSMRPlugin::StopNetworkWorkers()
{
	NetworkCancellationRequested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
		NetworkWorkersStopping = true;
		NetworkJobs.clear();
	}
	NetworkWorkerCondition.notify_all();
	for (std::thread& worker : NetworkWorkers)
	{
		if (worker.joinable())
			::CancelSynchronousIo(worker.native_handle());
	}
	for (std::thread& worker : NetworkWorkers)
	{
		if (worker.joinable())
			worker.join();
	}
	{
		std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
		NetworkWorkers.clear();
		NetworkWorkerThreadsRunning = 0;
		NetworkJobsInFlight = 0;
	}
}

WorkerQueueSnapshot CSMRPlugin::GetWorkerQueueSnapshot()
{
	WorkerQueueSnapshot snapshot;
	{
		std::lock_guard<std::mutex> lock(NetworkWorkerMutex);
		snapshot.networkWorkers = NetworkWorkerThreadsRunning;
		snapshot.networkQueued = NetworkJobs.size();
		snapshot.networkInFlight = NetworkJobsInFlight;
	}
	{
		std::lock_guard<std::mutex> lock(WeatherFetchMutex);
		snapshot.weatherWorkerRunning = WeatherWorkerRunning;
		snapshot.weatherQueued = WeatherFetchQueue.size();
		snapshot.weatherInFlight = WeatherFetchesInFlight;
	}
	return snapshot;
}

bool CSMRPlugin::WriteDiagnosticsReport(
	std::string& reportPath,
	std::string& error)
{
	reportPath.clear();
	error.clear();
	try
	{
		const std::filesystem::path pluginDirectory =
			std::filesystem::u8path(Logger::DLL_PATH);
		const std::filesystem::path dataDirectory =
			pluginDirectory / "vSMR_Data";
		const std::filesystem::path webUiDirectory =
			dataDirectory / "vSMR_webUI";
		std::filesystem::path diagnosticsDirectory =
			dataDirectory / "Diagnostics";
		std::error_code pathError;
		std::filesystem::create_directories(diagnosticsDirectory, pathError);
		if (pathError)
		{
			wchar_t temporaryPath[32768] = {};
			const DWORD length = ::GetTempPathW(
				static_cast<DWORD>(std::size(temporaryPath)),
				temporaryPath);
			if (length == 0 || length >= std::size(temporaryPath))
			{
				error = "Unable to resolve a writable diagnostics folder.";
				return false;
			}
			diagnosticsDirectory =
				std::filesystem::path(temporaryPath) / "vSMR_Diagnostics";
			pathError.clear();
			std::filesystem::create_directories(
				diagnosticsDirectory,
				pathError);
			if (pathError)
			{
				error = "Unable to create a diagnostics folder.";
				return false;
			}
		}

		SYSTEMTIME utc = {};
		::GetSystemTime(&utc);
		char fileName[96] = {};
		_snprintf_s(
			fileName,
			_TRUNCATE,
			"vSMR_diagnostics_%04u%02u%02u_%02u%02u%02u_%lu.txt",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			static_cast<unsigned long>(::GetCurrentProcessId()));
		const std::filesystem::path target = diagnosticsDirectory / fileName;
		std::filesystem::path temporary = target;
		temporary += ".tmp";

		const std::filesystem::path logPath =
			pluginDirectory / "vsmr.log";
		bool logWritable = false;
		{
			std::ofstream logProbe(logPath, std::ios::binary | std::ios::app);
			logWritable = logProbe.good();
		}

		const std::string profilesPath = GetActiveProfilesConfigPath();
		const DatalinkControlState datalink = GetDatalinkControlState();
		const RuntimeCredentialsSnapshot credentials =
			SnapshotRuntimeCredentials();
		const std::vector<std::string> recentLogMessages =
			Logger::recent_messages();
		const WorkerQueueSnapshot workerQueues = GetWorkerQueueSnapshot();

		auto singleLine = [](std::string value) {
			for (char& character : value)
			{
				if (character == '\r' || character == '\n' || character == '\t')
					character = ' ';
			}
			if (value.size() > 512)
				value.resize(512);
			return value;
		};
		auto yesNo = [](bool value) { return value ? "yes" : "no"; };
		std::error_code existsError;
		std::ostringstream report;
		report << "vSMR support diagnostics\n";
		report << "version=" << VsmrPluginVersion << "\n";
#if defined(_M_IX86)
		report << "architecture=x86\n";
#elif defined(_M_X64)
		report << "architecture=x64\n";
#else
		report << "architecture=unknown\n";
#endif
		report << "process_id=" << ::GetCurrentProcessId() << "\n";
		report << "generated_utc="
			<< utc.wYear << '-'
			<< std::setfill('0') << std::setw(2) << utc.wMonth << '-'
			<< std::setw(2) << utc.wDay << 'T'
			<< std::setw(2) << utc.wHour << ':'
			<< std::setw(2) << utc.wMinute << ':'
			<< std::setw(2) << utc.wSecond << "Z\n";
		report << "plugin_directory=" << pluginDirectory.u8string() << "\n";
		report << "data_directory=" << dataDirectory.u8string() << "\n";
		report << "data_directory_exists="
			<< yesNo(std::filesystem::is_directory(dataDirectory, existsError)) << "\n";
		existsError.clear();
		report << "webui_directory=" << webUiDirectory.u8string() << "\n";
		report << "webui_directory_exists="
			<< yesNo(std::filesystem::is_directory(webUiDirectory, existsError)) << "\n";
		report << "profiles_path=" << profilesPath << "\n";
		existsError.clear();
		report << "profiles_exists="
			<< yesNo(!profilesPath.empty() && std::filesystem::is_regular_file(
				std::filesystem::u8path(profilesPath), existsError)) << "\n";
		report << "logging_enabled=" << yesNo(Logger::ENABLED) << "\n";
		report << "logging_mode=" << Logger::mode_name(Logger::get_mode()) << "\n";
		report << "log_path=" << logPath.u8string() << "\n";
		report << "log_writable=" << yesNo(logWritable) << "\n";
		report << "crash_reporter_active="
			<< yesNo(VsmrCrashReporter::IsInstalled()) << "\n";
		report << "crash_reporter_status="
			<< singleLine(VsmrCrashReporter::GetRegistrationStatus()) << "\n";
		report << "crash_report_directory="
			<< singleLine(VsmrCrashReporter::GetReportDirectory()) << "\n";
		report << "active_airport=" << singleLine(datalink.activeAirport) << "\n";
		report << "cpdlc_connected=" << yesNo(datalink.connected) << "\n";
		report << "cpdlc_connecting=" << yesNo(datalink.connecting) << "\n";
		report << "cpdlc_polling=" << yesNo(datalink.pollInProgress) << "\n";
		report << "cpdlc_has_protected_code=" << yesNo(datalink.hasPassword) << "\n";
		report << "cpdlc_status=" << singleLine(datalink.statusMessage) << "\n";
		report << "vacdm_configured=" << yesNo(datalink.vacdmConfigured) << "\n";
		report << "cdm_auto_enabled=" << yesNo(datalink.cdmAutoEnabled) << "\n";
		report << "network_workers=" << workerQueues.networkWorkers << "\n";
		report << "network_jobs_queued=" << workerQueues.networkQueued << "\n";
		report << "network_jobs_in_flight=" << workerQueues.networkInFlight << "\n";
		report << "weather_worker_running=" << yesNo(workerQueues.weatherWorkerRunning) << "\n";
		report << "weather_requests_queued=" << workerQueues.weatherQueued << "\n";
		report << "weather_requests_in_flight=" << workerQueues.weatherInFlight << "\n";
		report << "radar_screens=" << RadarScreensOpened.size() << "\n";
		report << "shutdown_requested="
			<< yesNo(PluginShutdownRequested.load(std::memory_order_acquire)) << "\n";
		report << "recent_log_entries="
			<< (std::min)(recentLogMessages.size(), static_cast<size_t>(64))
			<< "\n";
		report << "Recent entries can contain operational callsigns; credentials are redacted.\n";
		const size_t firstRecentEntry = recentLogMessages.size() > 64
			? recentLogMessages.size() - 64
			: 0;
		for (size_t index = firstRecentEntry;
			index < recentLogMessages.size();
			++index)
		{
			std::string redacted = RedactSensitiveValue(
				recentLogMessages[index],
				credentials.password);
			redacted = RedactSensitiveValue(
				redacted,
				EncodeUrlQueryComponent(credentials.password));
			report << "recent_log=" << singleLine(redacted) << "\n";
		}
		report << "Secrets, message payloads, and endpoint query strings are intentionally omitted.\n";

		{
			std::ofstream output(
				temporary,
				std::ios::binary | std::ios::trunc);
			if (!output)
			{
				error = "Unable to create the diagnostics report.";
				return false;
			}
			output << report.str();
			output.flush();
			if (!output.good())
			{
				output.close();
				std::filesystem::remove(temporary, pathError);
				error = "Unable to write the diagnostics report.";
				return false;
			}
		}
		pathError.clear();
		std::filesystem::rename(temporary, target, pathError);
		if (pathError)
		{
			std::filesystem::remove(temporary, existsError);
			error = "Unable to finalize the diagnostics report.";
			return false;
		}
		reportPath = target.u8string();
		return true;
	}
	catch (const std::exception& ex)
	{
		error = std::string("Diagnostics failed: ") + ex.what();
		return false;
	}
	catch (...)
	{
		error = "Diagnostics failed unexpectedly.";
		return false;
	}
}
