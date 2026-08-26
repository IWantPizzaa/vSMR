#include "platform/windows/PrecompiledHeader.hpp"

#include "rdf/RdfOverlay.hpp"

#include "shared/logging/Logger.hpp"
#include "radar/RadarScreen.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"

#include <winhttp.h>

#include "rapidjson/document.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

extern std::vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	constexpr wchar_t TrackAudioHost[] = L"127.0.0.1";
	constexpr INTERNET_PORT TrackAudioPort = 49080;
	constexpr wchar_t TrackAudioPath[] = L"/ws";
	constexpr DWORD NetworkTimeoutMs = 1500;
	constexpr std::size_t ReceiveBufferSize = 8192;
	constexpr std::size_t MaximumMessageSize = 64U * 1024U;
	constexpr int RdfRingRadiusPixels = 20;

	using FrequencyHz = std::int64_t;

	class RdfService final
	{
	public:
		~RdfService()
		{
			Stop();
		}

		void Start(CSMRPlugin* plugin, bool enabled)
		{
			std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
			(void)plugin;
			const bool enabledChanged =
				Enabled.exchange(enabled, std::memory_order_acq_rel) != enabled;
			if (enabledChanged)
				MarkChanged();
			if (enabled)
				EnsureWorkerLocked();
			else
				StopWorkerLocked();
		}

		void Stop()
		{
			std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
			Enabled.store(false, std::memory_order_release);
			StopWorkerLocked();
		}

		void OnTimer()
		{
			{
				std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
				if (Enabled.load(std::memory_order_acquire))
					EnsureWorkerLocked();
			}

			const std::uint64_t generation =
				Generation.load(std::memory_order_acquire);
			if (generation == LastUiGeneration)
				return;
			LastUiGeneration = generation;
			for (CSMRRadar* radar : RadarScreensOpened)
			{
				if (radar != nullptr && !radar->IsShutdownRequested())
					radar->RequestRefresh();
			}
		}

		void SetEnabled(bool enabled)
		{
			std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
			const bool enabledChanged =
				Enabled.exchange(enabled, std::memory_order_acq_rel) != enabled;
			if (enabledChanged)
				MarkChanged();
			if (enabled)
				EnsureWorkerLocked();
			else
				StopWorkerLocked();
		}

		VsmrRdf::Status GetStatus() const
		{
			VsmrRdf::Status status;
			status.enabled = Enabled.load(std::memory_order_acquire);
			status.trackAudioConnected = Connected.load(std::memory_order_acquire);
			std::lock_guard<std::mutex> transmissionGuard(TransmissionMutex);
			status.activeTransmissionCount = Transmissions.size();
			return status;
		}

		std::vector<std::string> TransmissionSnapshot() const
		{
			std::vector<std::string> result;
			std::lock_guard<std::mutex> transmissionGuard(TransmissionMutex);
			result.reserve(Transmissions.size());
			for (const auto& entry : Transmissions)
				result.push_back(entry.first);
			return result;
		}

	private:
		void EnsureWorkerLocked()
		{
			if (Worker.joinable())
			{
				if (WorkerRunning.load(std::memory_order_acquire))
					return;
				Worker.join();
			}

			StopRequested.store(false, std::memory_order_release);
			WorkerRunning.store(true, std::memory_order_release);
			try
			{
				Worker = std::thread(&RdfService::WorkerMain, this);
			}
			catch (...)
			{
				WorkerRunning.store(false, std::memory_order_release);
				Connected.store(false, std::memory_order_release);
			}
		}

		void StopWorkerLocked()
		{
			StopRequested.store(true, std::memory_order_release);
			RetryCondition.notify_all();
			ShutdownActiveWebSocket();
			if (Worker.joinable())
				Worker.join();

			WorkerRunning.store(false, std::memory_order_release);
			Connected.store(false, std::memory_order_release);
			ClearTransmissions();
		}

		void WorkerMain() noexcept
		{
			VsmrCrashRuntime::OwnedThreadRole crashThreadRole("RDF worker");
			unsigned int retrySeconds = 1;
			while (!ShouldStop())
			{
				const bool connectedOnce = RunConnection();
				SetConnected(false);
				if (ShouldStop())
					break;

				std::unique_lock<std::mutex> retryLock(RetryMutex);
				RetryCondition.wait_for(
					retryLock,
					std::chrono::seconds(connectedOnce ? 1U : retrySeconds),
					[this]() { return ShouldStop(); });
				retrySeconds = connectedOnce
					? 1U
					: (std::min)(retrySeconds * 2U, 30U);
			}

			SetConnected(false);
			WorkerRunning.store(false, std::memory_order_release);
		}

		bool ShouldStop() const
		{
			return StopRequested.load(std::memory_order_acquire) ||
				!Enabled.load(std::memory_order_acquire);
		}

		bool RunConnection() noexcept
		{
			HINTERNET session = ::WinHttpOpen(
				L"vSMR/2.0 RDF",
				WINHTTP_ACCESS_TYPE_NO_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				0);
			if (session == nullptr)
				return false;

			::WinHttpSetTimeouts(
				session,
				NetworkTimeoutMs,
				NetworkTimeoutMs,
				NetworkTimeoutMs,
				NetworkTimeoutMs);

			HINTERNET connection = nullptr;
			HINTERNET request = nullptr;
			HINTERNET webSocket = nullptr;
			bool connectedOnce = false;

			if (!ShouldStop())
				connection = ::WinHttpConnect(session, TrackAudioHost, TrackAudioPort, 0);
			if (connection != nullptr && !ShouldStop())
			{
				request = ::WinHttpOpenRequest(
					connection,
					L"GET",
					TrackAudioPath,
					nullptr,
					WINHTTP_NO_REFERER,
					WINHTTP_DEFAULT_ACCEPT_TYPES,
					0);
			}

			if (request != nullptr)
			{
				const bool upgradeSet = ::WinHttpSetOption(
					request,
					WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
					nullptr,
					0) != FALSE;
				const bool requestSent = upgradeSet &&
					::WinHttpSendRequest(
						request,
						WINHTTP_NO_ADDITIONAL_HEADERS,
						0,
						WINHTTP_NO_REQUEST_DATA,
						0,
						0,
						0) != FALSE;
				const bool responseReceived = requestSent &&
					::WinHttpReceiveResponse(request, nullptr) != FALSE;
				if (responseReceived && !ShouldStop())
					webSocket = ::WinHttpWebSocketCompleteUpgrade(request, 0);

				if (webSocket != nullptr)
				{
					::WinHttpCloseHandle(request);
					request = nullptr;
					if (PublishActiveWebSocket(webSocket))
					{
						connectedOnce = true;
						ReceiveMessages(webSocket);
						UnpublishActiveWebSocket(webSocket);
					}
				}
			}

			if (webSocket != nullptr)
			{
				::WinHttpCloseHandle(webSocket);
				webSocket = nullptr;
			}
			if (request != nullptr)
			{
				::WinHttpCloseHandle(request);
				request = nullptr;
			}
			if (connection != nullptr)
				::WinHttpCloseHandle(connection);
			::WinHttpCloseHandle(session);
			return connectedOnce;
		}

		void ReceiveMessages(HINTERNET webSocket) noexcept
		{
			SetConnected(true);
			std::vector<unsigned char> buffer(ReceiveBufferSize);
			std::string message;
			bool discardMessage = false;

			while (!ShouldStop())
			{
				DWORD bytesRead = 0;
				WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType =
					WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
				const DWORD result = ::WinHttpWebSocketReceive(
					webSocket,
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					&bytesRead,
					&bufferType);
				if (result == ERROR_WINHTTP_TIMEOUT)
					continue;
				if (result != ERROR_SUCCESS || bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
					break;

				const bool utf8 =
					bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
					bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
				const bool finalFragment =
					bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
					bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

				if (!utf8)
					discardMessage = true;
				if (!discardMessage && bytesRead > 0)
				{
					if (message.size() + bytesRead > MaximumMessageSize)
						discardMessage = true;
					else
						message.append(
							reinterpret_cast<const char*>(buffer.data()),
							static_cast<std::size_t>(bytesRead));
				}

				if (finalFragment)
				{
					if (!discardMessage && utf8)
						ProcessMessage(message);
					message.clear();
					discardMessage = false;
				}
			}
		}

		void ProcessMessage(const std::string& text) noexcept
		{
			rapidjson::Document document;
			document.Parse<0>(text.c_str());
			if (document.HasParseError() || !document.IsObject() ||
				!document.HasMember("type") || !document["type"].IsString() ||
				!document.HasMember("value") || !document["value"].IsObject())
			{
				return;
			}

			const std::string type(
				document["type"].GetString(),
				document["type"].GetStringLength());
			if (type != "kRxBegin" && type != "kRxEnd")
				return;

			const rapidjson::Value& value = document["value"];
			if (!value.HasMember("callsign") || !value["callsign"].IsString() ||
				!value.HasMember("pFrequencyHz") || !value["pFrequencyHz"].IsNumber())
			{
				return;
			}

			std::string callsign = NormalizeCallsign(
				value["callsign"].GetString(),
				value["callsign"].GetStringLength());
			const double rawFrequency = value["pFrequencyHz"].GetDouble();
			if (callsign.empty() || !std::isfinite(rawFrequency) ||
				rawFrequency < 0.0 || rawFrequency > 10000000000.0)
			{
				return;
			}

			const FrequencyHz frequency = static_cast<FrequencyHz>(std::llround(rawFrequency));
			std::lock_guard<std::mutex> transmissionGuard(TransmissionMutex);
			// Tracking each frequency prevents one receiver from ending another active transmission
			if (type == "kRxBegin")
			{
				if (Transmissions[callsign].insert(frequency).second)
					MarkChanged();
				return;
			}

			auto callsignIt = Transmissions.find(callsign);
			if (callsignIt == Transmissions.end())
				return;
			if (callsignIt->second.erase(frequency) == 0)
				return;
			if (callsignIt->second.empty())
				Transmissions.erase(callsignIt);
			MarkChanged();
		}

		static std::string NormalizeCallsign(const char* text, std::size_t length)
		{
			if (text == nullptr || length == 0 || length > 64)
				return {};

			std::string result(text, length);
			for (char& character : result)
			{
				const unsigned char value = static_cast<unsigned char>(character);
				if (value <= 0x20 || value > 0x7e)
					return {};
				if (character >= 'a' && character <= 'z')
					character = static_cast<char>(character - 'a' + 'A');
			}
			return result;
		}

		void SetConnected(bool connected)
		{
			const bool previous = Connected.exchange(connected, std::memory_order_acq_rel);
			if (!connected)
				ClearTransmissions();
			if (previous == connected)
				return;
			VsmrCrashReporter::RecordState(
				"rdf connection",
				connected ? "connected" : "disconnected");
			MarkChanged();

			Logger::info(connected
				? "vSMR RDF connected to TrackAudio"
				: "vSMR RDF disconnected from TrackAudio");
		}

		void ClearTransmissions()
		{
			std::lock_guard<std::mutex> transmissionGuard(TransmissionMutex);
			if (Transmissions.empty())
				return;
			Transmissions.clear();
			MarkChanged();
		}

		void MarkChanged()
		{
			Generation.fetch_add(1, std::memory_order_release);
		}

		bool PublishActiveWebSocket(HINTERNET webSocket)
		{
			std::lock_guard<std::mutex> operationGuard(OperationMutex);
			if (ShouldStop() || ActiveWebSocket != nullptr)
				return false;
			ActiveWebSocket = webSocket;
			return true;
		}

		void UnpublishActiveWebSocket(HINTERNET webSocket)
		{
			std::lock_guard<std::mutex> operationGuard(OperationMutex);
			if (ActiveWebSocket == webSocket)
				ActiveWebSocket = nullptr;
		}

		void ShutdownActiveWebSocket()
		{
			std::lock_guard<std::mutex> operationGuard(OperationMutex);
			if (ActiveWebSocket != nullptr)
			{
				// Shutdown operates only on the send side and is safe while the
				// worker owns a pending receive. The worker remains the sole owner
				// that closes the synchronous WinHTTP handle.
				::WinHttpWebSocketShutdown(
					ActiveWebSocket,
					WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
					nullptr,
					0);
			}
		}

		mutable std::mutex LifecycleMutex;
		mutable std::mutex TransmissionMutex;
		mutable std::mutex OperationMutex;
		std::mutex RetryMutex;
		std::condition_variable RetryCondition;
		std::thread Worker;
		std::atomic<bool> Enabled{ false };
		std::atomic<bool> StopRequested{ false };
		std::atomic<bool> Connected{ false };
		std::atomic<bool> WorkerRunning{ false };
		std::atomic<std::uint64_t> Generation{ 0 };
		std::uint64_t LastUiGeneration = 0;
		std::map<std::string, std::set<FrequencyHz>> Transmissions;
		HINTERNET ActiveWebSocket = nullptr;
	};

	RdfService& Service()
	{
		static RdfService service;
		return service;
	}

	bool PointInside(const POINT& point, const RECT& viewport)
	{
		return point.x >= viewport.left && point.x <= viewport.right &&
			point.y >= viewport.top && point.y <= viewport.bottom;
	}
}

namespace VsmrRdf
{
	void Start(CSMRPlugin* plugin, bool enabled)
	{
		Service().Start(plugin, enabled);
	}

	void Stop()
	{
		Service().Stop();
	}

	void OnTimer()
	{
		Service().OnTimer();
	}

	void SetEnabled(bool enabled)
	{
		Service().SetEnabled(enabled);
	}

	Status GetStatus()
	{
		return Service().GetStatus();
	}

	void Draw(
		HDC dc,
		CSMRRadar* radar,
		const RECT& viewport,
		const Projector& projector)
	{
		if (dc == nullptr || radar == nullptr || !projector ||
			viewport.right <= viewport.left || viewport.bottom <= viewport.top)
		{
			return;
		}

		const std::vector<std::string> callsigns = Service().TransmissionSnapshot();
		if (callsigns.empty())
			return;

		const VsmrScene::RadarScene* scene = radar->GetCurrentRadarScene();
		if (scene == nullptr)
			return;

		std::vector<POINT> positions;
		positions.reserve(callsigns.size());
		try
		{
			for (const std::string& callsign : callsigns)
			{
				const VsmrScene::Target* target = scene->FindTarget(callsign);
				if (target == nullptr && !callsign.empty() &&
					callsign.back() >= 'A' && callsign.back() <= 'Z')
				{
					const auto controller = std::find_if(scene->controllers.begin(), scene->controllers.end(), [&](const VsmrScene::ControllerState& item)
					{
						return _stricmp(item.callsign.c_str(), callsign.c_str()) == 0;
					});
					if (controller != scene->controllers.end())
						target = scene->FindTarget(callsign.substr(0, callsign.size() - 1));
				}

				if (target == nullptr || !target->position.valid)
					continue;
				EuroScopePlugIn::CPosition position;
				position.m_Latitude = target->position.latitude;
				position.m_Longitude = target->position.longitude;
				positions.push_back(projector(position));
			}
		}
		catch (...)
		{
			// Never allow either EuroScope data access or a viewport projector to
			// unwind through a radar refresh callback.
			return;
		}

		if (positions.empty())
			return;

		const int savedDc = ::SaveDC(dc);
		if (savedDc == 0)
			return;
		if (::IntersectClipRect(
			dc,
			viewport.left,
			viewport.top,
			viewport.right,
			viewport.bottom) == ERROR)
		{
			::RestoreDC(dc, savedDc);
			return;
		}

		const COLORREF color = positions.size() > 1
			? RGB(255, 0, 0)
			: RGB(255, 255, 255);
		HPEN pen = ::CreatePen(PS_SOLID, 1, color);
		if (pen == nullptr)
		{
			::RestoreDC(dc, savedDc);
			return;
		}

		::SelectObject(dc, pen);
		::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
		const POINT center = {
			viewport.left + (viewport.right - viewport.left) / 2,
			viewport.top + (viewport.bottom - viewport.top) / 2
		};
		for (const POINT& position : positions)
		{
			if (PointInside(position, viewport))
			{
				::Ellipse(
					dc,
					position.x - RdfRingRadiusPixels,
					position.y - RdfRingRadiusPixels,
					position.x + RdfRingRadiusPixels,
					position.y + RdfRingRadiusPixels);
			}
			else
			{
				::MoveToEx(dc, center.x, center.y, nullptr);
				::LineTo(dc, position.x, position.y);
			}
		}

		::RestoreDC(dc, savedDc);
		::DeleteObject(pen);
	}
}
