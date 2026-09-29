#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/PluginHttpSupport.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "crash/CrashRuntime.hpp"
#include "weather/WeatherStore.hpp"

#include <atomic>
#include <ctime>
#include <exception>
#include <system_error>
#include <utility>

namespace
{
	const size_t WeatherResponseLimitBytes = 4096U;
}

void CSMRPlugin::StopWeatherFetchWorker()
{
	WeatherFetchCancellationRequested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> guard(WeatherFetchMutex);
		WeatherFetchStop = true;
		WeatherFetchQueue.clear();
		WeatherFetchQueued.clear();
	}
	WeatherFetchCondition.notify_all();
	if (WeatherFetchThread.joinable())
	{
		::CancelSynchronousIo(WeatherFetchThread.native_handle());
		WeatherFetchThread.join();
	}
	{
		std::lock_guard<std::mutex> guard(WeatherFetchMutex);
		WeatherWorkerRunning = false;
		WeatherFetchesInFlight = 0;
	}
}

void CSMRPlugin::OnNewMetarReceived(const char* sStation, const char* sFullMetar)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnNewMetarReceived");
	if (!PluginShutdownRequested.load(std::memory_order_relaxed))
		VsmrWeather::Update(sStation, sFullMetar);
}

void CSMRPlugin::QueueWeatherFetch(const std::string& rawStation)
{
	const std::string station = VsmrWeather::NormalizeIcao(rawStation);
	if (station.empty() || PluginShutdownRequested.load(std::memory_order_relaxed))
		return;

	VsmrWeather::Snapshot snapshot;
	const bool hasSnapshot = VsmrWeather::TryGet(station, snapshot);
	const std::time_t now = std::time(nullptr);
	const std::time_t refreshInterval = hasSnapshot ? 5 * 60 : 60;

	std::lock_guard<std::mutex> guard(WeatherFetchMutex);
	if (WeatherFetchStop || WeatherFetchQueued.find(station) != WeatherFetchQueued.end())
		return;

	const auto lastAttempt = WeatherLastAttemptUtc.find(station);
	if (lastAttempt != WeatherLastAttemptUtc.end() &&
		now >= lastAttempt->second && now - lastAttempt->second < refreshInterval)
	{
		return;
	}

	if (!WeatherFetchThread.joinable())
	{
		try
		{
			WeatherFetchThread = std::thread(&CSMRPlugin::WeatherFetchThreadMain, this);
			WeatherWorkerRunning = true;
		}
		catch (const std::system_error&)
		{
			WeatherLastAttemptUtc[station] = now;
			return;
		}
	}

	WeatherLastAttemptUtc[station] = now;
	WeatherFetchQueued.insert(station);
	WeatherFetchQueue.push_back(station);
	WeatherFetchCondition.notify_one();
}

void CSMRPlugin::WeatherFetchThreadMain()
{
	VsmrCrashRuntime::OwnedThreadRole crashThreadRole("weather worker");
	struct RunningWeatherGuard
	{
		std::mutex& mutex;
		bool& running;
		~RunningWeatherGuard()
		{
			std::lock_guard<std::mutex> lock(mutex);
			running = false;
		}
	} runningWeatherGuard{ WeatherFetchMutex, WeatherWorkerRunning };
	try
	{
		for (;;)
		{
			std::string station;
			{
				std::unique_lock<std::mutex> lock(WeatherFetchMutex);
				WeatherFetchCondition.wait(lock, [this]() {
					return WeatherFetchStop || !WeatherFetchQueue.empty();
					});
				if (WeatherFetchStop)
					return;

				station = std::move(WeatherFetchQueue.front());
				WeatherFetchQueue.pop_front();
				WeatherFetchQueued.erase(station);
				++WeatherFetchesInFlight;
			}
			struct InFlightWeatherGuard
			{
				std::mutex& mutex;
				std::size_t& count;
				~InFlightWeatherGuard()
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (count > 0)
						--count;
				}
			} inFlightWeatherGuard{ WeatherFetchMutex, WeatherFetchesInFlight };

			try
			{
				const std::time_t requestStartedUtc = std::time(nullptr);
				const std::string url =
					"https://metar.vatsim.net/metar.php?id=" + station;
				const std::string report = VsmrPluginRuntime::GetHttpHelper().downloadStringFromURL(
					url,
					3500,
					&WeatherFetchCancellationRequested,
					WeatherResponseLimitBytes);
				if (!PluginShutdownRequested.load(std::memory_order_relaxed) &&
					!report.empty() && report.size() <= WeatherResponseLimitBytes)
				{
					VsmrWeather::Update(station, report, requestStartedUtc, true);
				}
			}
			catch (const std::exception& ex)
			{
				Logger::info(
					"Weather fetch exception: " +
					std::string(ex.what()));
			}
			catch (...)
			{
				Logger::info("Weather fetch exception: unknown");
			}
		}
	}
	catch (const std::exception& ex)
	{
		Logger::info(
			"Weather worker terminated by exception: " +
			std::string(ex.what()));
	}
	catch (...)
	{
		Logger::info("Weather worker terminated by unknown exception");
	}
}
