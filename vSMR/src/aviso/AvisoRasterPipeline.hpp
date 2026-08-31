#pragma once

#include "diagnostics/PerformanceDiagnostics.hpp"
#include "radar/RadarScreenTypes.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace VsmrAviso
{
	// Owns the asynchronous work queue for one AVISO viewport. Rendering and
	// host integration stay behind callbacks so main and inset views share the
	// lifecycle without sharing a worker or mutable viewport state.
	class AvisoRasterPipeline final
	{
	public:
		using Request = VsmrRadarTypes::AvisoRasterRenderRequest;
		using Result = VsmrRadarTypes::AvisoRasterRenderResult;

		enum class QueueStatus
		{
			Rejected,
			Coalesced,
			Queued
		};

		struct DiagnosticsCallbacks
		{
			std::function<void(bool superseded)> requestQueued;
			std::function<void()> requestCoalesced;
			std::function<void()> requestDebounced;
			std::function<void(double rebuildMilliseconds, double queueWaitMilliseconds, bool succeeded)> rasterBuilt;
			std::function<void()> rasterBuildCancelled;
			std::function<void()> resultDiscarded;
		};

		struct Callbacks
		{
			std::function<std::unique_ptr<Result>(const Request&)> render;
			std::function<bool(const Request&)> isExternallyCancelled;
			std::function<bool()> isExternalStopRequested;
			std::function<void()> requestRefresh;
			std::function<void()> workerThreadStarted;
			std::function<void()> workerThreadStopped;
			std::function<void(const Request&)> renderStarted;
			std::function<void(const std::string&)> reportError;
			DiagnosticsCallbacks diagnostics;
		};

		explicit AvisoRasterPipeline(
			Callbacks callbacks,
			std::string workerName = "AVISO render worker");
		~AvisoRasterPipeline();

		AvisoRasterPipeline(const AvisoRasterPipeline&) = delete;
		AvisoRasterPipeline& operator=(const AvisoRasterPipeline&) = delete;
		AvisoRasterPipeline(AvisoRasterPipeline&&) = delete;
		AvisoRasterPipeline& operator=(AvisoRasterPipeline&&) = delete;

		// cacheAvailable enables the existing short debounce while a previous
		// raster can remain visible. Stop is terminal and joins the worker.
		QueueStatus Queue(Request request, bool cacheAvailable);
		void InvalidateRequests();
		void Stop();

		std::unique_ptr<Result> TakeCompleted();
		void AllowRetryForDiscardedResult(std::uint64_t requestId);

		bool HasPendingWork() const noexcept;
		VsmrPerformance::AvisoQueueDepth QueueDepth() const;
		bool IsRequestCancelled(const Request& request) const noexcept;

	private:
		struct RequestKey
		{
			std::string path;
			bool useDayPalette = false;
			std::uint64_t groupGeneration = 0;
			int rasterWidth = 0;
			int rasterHeight = 0;
			double minLongitude = 0.0;
			double minLatitude = 0.0;
			double maxLongitude = 0.0;
			double maxLatitude = 0.0;
			Gdiplus::PointF projectedTopLeft;
			Gdiplus::PointF projectedTopRight;
			Gdiplus::PointF projectedBottomLeft;
			Gdiplus::PointF projectedBottomRight;
		};

		bool StartWorkerLocked(std::string& error);
		void WorkerMain();
		void UpdatePendingStateLocked() noexcept;
		bool IsExternalStopRequested() const noexcept;
		void ReportError(const std::string& message) const noexcept;

		static RequestKey MakeRequestKey(const Request& request);
		static bool RequestsMatch(const RequestKey& previous, const Request& request) noexcept;

		Callbacks callbacks_;
		std::string workerName_;

		mutable std::mutex mutex_;
		std::mutex stopMutex_;
		std::condition_variable condition_;
		std::thread worker_;
		bool workerStarted_ = false;
		bool renderInFlight_ = false;
		std::atomic<bool> stopRequested_{ false };
		std::atomic<bool> pendingWork_{ false };

		std::shared_ptr<std::atomic<std::uint64_t>> cancellationToken_ =
			std::make_shared<std::atomic<std::uint64_t>>(0);
		std::uint64_t nextRequestId_ = 0;
		std::uint64_t latestRequestId_ = 0;
		std::unique_ptr<Request> pendingRequest_;
		std::unique_ptr<Result> completedResult_;
		std::optional<RequestKey> lastRequestKey_;
	};
}
