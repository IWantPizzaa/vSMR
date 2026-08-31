#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterPipeline.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace
{
	std::uint64_t MonotonicMilliseconds() noexcept
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool WithinTolerance(double left, double right, double tolerance) noexcept
	{
		const double delta = left - right;
		return delta >= -tolerance && delta <= tolerance;
	}

	bool PointWithinTolerance(
		const Gdiplus::PointF& left,
		const Gdiplus::PointF& right,
		double tolerance) noexcept
	{
		return WithinTolerance(left.X, right.X, tolerance) &&
			WithinTolerance(left.Y, right.Y, tolerance);
	}

	template <typename Callback, typename... Args>
	void InvokeNoThrow(const Callback& callback, Args&&... args) noexcept
	{
		if (!callback)
			return;

		try
		{
			callback(std::forward<Args>(args)...);
		}
		catch (...)
		{
		}
	}
}

VsmrAviso::AvisoRasterPipeline::AvisoRasterPipeline(
	Callbacks callbacks,
	std::string workerName) :
	callbacks_(std::move(callbacks)),
	workerName_(std::move(workerName))
{
}

VsmrAviso::AvisoRasterPipeline::~AvisoRasterPipeline()
{
	Stop();
}

VsmrAviso::AvisoRasterPipeline::QueueStatus VsmrAviso::AvisoRasterPipeline::Queue(
	Request request,
	bool cacheAvailable)
{
	if (!callbacks_.render ||
		request.path.empty() ||
		request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0 ||
		stopRequested_.load(std::memory_order_acquire) ||
		IsExternalStopRequested())
	{
		return QueueStatus::Rejected;
	}

	bool supersededRequest = false;
	bool coalescedRequest = false;
	std::string workerError;
	{
		std::lock_guard<std::mutex> guard(mutex_);
		if (stopRequested_.load(std::memory_order_relaxed))
			return QueueStatus::Rejected;

		if (lastRequestKey_.has_value() && RequestsMatch(*lastRequestKey_, request))
		{
			coalescedRequest = true;
		}
		else if (!StartWorkerLocked(workerError))
		{
			lastRequestKey_.reset();
		}
		else
		{
			request.requestId = ++nextRequestId_;
			request.performanceQueuedAtMilliseconds = MonotonicMilliseconds();
			if (request.debounceMilliseconds == 0 && cacheAvailable)
				request.debounceMilliseconds = 24;
			request.cancellationToken = cancellationToken_;

			latestRequestId_ = request.requestId;
			cancellationToken_->store(request.requestId, std::memory_order_release);
			lastRequestKey_ = MakeRequestKey(request);
			supersededRequest = pendingRequest_ != nullptr || renderInFlight_;
			pendingRequest_ = std::make_unique<Request>(std::move(request));
			UpdatePendingStateLocked();
		}
	}

	if (coalescedRequest)
	{
		InvokeNoThrow(callbacks_.diagnostics.requestCoalesced);
		return QueueStatus::Coalesced;
	}
	if (!workerError.empty())
	{
		ReportError(workerError);
		return QueueStatus::Rejected;
	}

	InvokeNoThrow(callbacks_.diagnostics.requestQueued, supersededRequest);
	condition_.notify_one();
	return QueueStatus::Queued;
}

void VsmrAviso::AvisoRasterPipeline::InvalidateRequests()
{
	{
		std::lock_guard<std::mutex> guard(mutex_);
		pendingRequest_.reset();
		completedResult_.reset();
		latestRequestId_ = ++nextRequestId_;
		cancellationToken_->store(latestRequestId_, std::memory_order_release);
		lastRequestKey_.reset();
		UpdatePendingStateLocked();
	}
	condition_.notify_all();
}

void VsmrAviso::AvisoRasterPipeline::Stop()
{
	std::lock_guard<std::mutex> stopGuard(stopMutex_);
	{
		std::lock_guard<std::mutex> guard(mutex_);
		stopRequested_.store(true, std::memory_order_release);
		cancellationToken_->fetch_add(1, std::memory_order_release);
		pendingRequest_.reset();
		completedResult_.reset();
		lastRequestKey_.reset();
		UpdatePendingStateLocked();
	}

	condition_.notify_all();
	if (worker_.joinable())
		worker_.join();

	{
		std::lock_guard<std::mutex> guard(mutex_);
		workerStarted_ = false;
		renderInFlight_ = false;
		UpdatePendingStateLocked();
	}
}

std::unique_ptr<VsmrAviso::AvisoRasterPipeline::Result>
VsmrAviso::AvisoRasterPipeline::TakeCompleted()
{
	std::lock_guard<std::mutex> guard(mutex_);
	std::unique_ptr<Result> result = std::move(completedResult_);
	UpdatePendingStateLocked();
	return result;
}

void VsmrAviso::AvisoRasterPipeline::AllowRetryForDiscardedResult(
	std::uint64_t requestId)
{
	std::lock_guard<std::mutex> guard(mutex_);
	// Keep a newer request coalescible. Only the latest discarded result needs
	// its key cleared so the same viewport can be queued again.
	if (latestRequestId_ == requestId &&
		pendingRequest_ == nullptr &&
		!renderInFlight_)
	{
		lastRequestKey_.reset();
	}
}

bool VsmrAviso::AvisoRasterPipeline::HasPendingWork() const noexcept
{
	return pendingWork_.load(std::memory_order_relaxed);
}

VsmrPerformance::AvisoQueueDepth VsmrAviso::AvisoRasterPipeline::QueueDepth() const
{
	std::lock_guard<std::mutex> guard(mutex_);
	VsmrPerformance::AvisoQueueDepth result;
	result.pending = pendingRequest_ != nullptr ? 1U : 0U;
	result.inFlight = renderInFlight_ ? 1U : 0U;
	result.completed = completedResult_ != nullptr ? 1U : 0U;
	result.workers = workerStarted_ ? 1U : 0U;
	return result;
}

bool VsmrAviso::AvisoRasterPipeline::IsRequestCancelled(
	const Request& request) const noexcept
{
	if (stopRequested_.load(std::memory_order_acquire) ||
		(request.cancellationToken != nullptr &&
			request.cancellationToken->load(std::memory_order_acquire) != request.requestId))
	{
		return true;
	}

	if (IsExternalStopRequested())
		return true;

	if (!callbacks_.isExternallyCancelled)
		return false;
	try
	{
		return callbacks_.isExternallyCancelled(request);
	}
	catch (...)
	{
		return true;
	}
}

bool VsmrAviso::AvisoRasterPipeline::StartWorkerLocked(std::string& error)
{
	if (workerStarted_)
		return true;
	if (stopRequested_.load(std::memory_order_relaxed))
		return false;
	if (worker_.joinable())
		worker_.join();

	try
	{
		worker_ = std::thread(&AvisoRasterPipeline::WorkerMain, this);
		workerStarted_ = true;
		return true;
	}
	catch (const std::exception& ex)
	{
		error = workerName_ + " start failed: " + ex.what();
	}
	catch (...)
	{
		error = workerName_ + " start failed: unknown exception";
	}
	return false;
}

void VsmrAviso::AvisoRasterPipeline::WorkerMain()
{
	InvokeNoThrow(callbacks_.workerThreadStarted);
	struct WorkerExit final
	{
		const std::function<void()>& callback;
		~WorkerExit()
		{
			InvokeNoThrow(callback);
		}
	} workerExit{ callbacks_.workerThreadStopped };

	try
	{
		for (;;)
		{
			std::unique_ptr<Request> request;
			std::size_t debouncedRequests = 0;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				condition_.wait(lock, [&]() {
					return stopRequested_.load(std::memory_order_relaxed) || pendingRequest_ != nullptr;
				});

				if (stopRequested_.load(std::memory_order_relaxed))
					return;
				while (pendingRequest_ != nullptr &&
					pendingRequest_->debounceMilliseconds > 0)
				{
					const std::uint64_t observedRequestId = pendingRequest_->requestId;
					const std::uint64_t readyAt =
						pendingRequest_->performanceQueuedAtMilliseconds +
						pendingRequest_->debounceMilliseconds;
					const std::uint64_t now = MonotonicMilliseconds();
					if (now >= readyAt)
						break;

					condition_.wait_for(
						lock,
						std::chrono::milliseconds(static_cast<long long>(readyAt - now)),
						[&]() {
							return stopRequested_.load(std::memory_order_relaxed) ||
								pendingRequest_ == nullptr ||
								pendingRequest_->requestId != observedRequestId;
						});
					if (stopRequested_.load(std::memory_order_relaxed))
						return;
					if (pendingRequest_ != nullptr &&
						pendingRequest_->requestId != observedRequestId)
					{
						++debouncedRequests;
						continue;
					}
					break;
				}

				request = std::move(pendingRequest_);
				renderInFlight_ = request != nullptr;
				UpdatePendingStateLocked();
			}
			for (std::size_t index = 0; index < debouncedRequests; ++index)
				InvokeNoThrow(callbacks_.diagnostics.requestDebounced);

			if (request == nullptr)
				continue;

			InvokeNoThrow(callbacks_.renderStarted, *request);
			std::unique_ptr<Result> result;
			const std::uint64_t renderStartMilliseconds = MonotonicMilliseconds();
			const double queueWaitMilliseconds = request->performanceQueuedAtMilliseconds == 0
				? 0.0
				: static_cast<double>(
					renderStartMilliseconds - request->performanceQueuedAtMilliseconds);
			const auto renderStart = std::chrono::steady_clock::now();
			try
			{
				result = callbacks_.render(*request);
			}
			catch (CException* ex)
			{
				if (ex != nullptr)
					ex->Delete();
				ReportError(workerName_ + " caught MFC exception");
			}
			catch (const std::exception& ex)
			{
				ReportError(workerName_ + " caught exception: " + ex.what());
			}
			catch (...)
			{
				ReportError(workerName_ + " caught unknown exception");
			}

			const double rebuildMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - renderStart).count();
			const bool renderCancelled = result == nullptr && IsRequestCancelled(*request);
			if (renderCancelled)
			{
				InvokeNoThrow(callbacks_.diagnostics.rasterBuildCancelled);
			}
			else
			{
				InvokeNoThrow(
					callbacks_.diagnostics.rasterBuilt,
					rebuildMilliseconds,
					queueWaitMilliseconds,
					result != nullptr);
			}

			const bool externalStopAfterRender = IsExternalStopRequested();
			bool refreshRequested = false;
			bool discardedResult = false;
			bool stopAfterRender = false;
			{
				std::lock_guard<std::mutex> guard(mutex_);
				if (result == nullptr &&
					!renderCancelled &&
					request->requestId == latestRequestId_)
				{
					// Allocation and GDI failures are transient. Keeping the key would
					// coalesce every identical retry and leave the viewport stale.
					lastRequestKey_.reset();
				}

				stopAfterRender = stopRequested_.load(std::memory_order_relaxed) ||
					externalStopAfterRender;
				if (stopAfterRender)
				{
					discardedResult = result != nullptr;
				}
				else if (result != nullptr && result->requestId == latestRequestId_)
				{
					if (completedResult_ != nullptr)
						discardedResult = true;
					completedResult_ = std::move(result);
					refreshRequested = true;
				}
				else if (result != nullptr)
				{
					discardedResult = true;
				}

				renderInFlight_ = false;
				UpdatePendingStateLocked();
			}

			if (discardedResult)
				InvokeNoThrow(callbacks_.diagnostics.resultDiscarded);
			if (stopAfterRender)
				return;
			if (refreshRequested)
				InvokeNoThrow(callbacks_.requestRefresh);
		}
	}
	catch (const std::exception& ex)
	{
		ReportError(workerName_ + " stopped after exception: " + ex.what());
	}
	catch (...)
	{
		ReportError(workerName_ + " stopped after unknown exception");
	}

	std::lock_guard<std::mutex> guard(mutex_);
	pendingRequest_.reset();
	latestRequestId_ = ++nextRequestId_;
	cancellationToken_->store(latestRequestId_, std::memory_order_release);
	lastRequestKey_.reset();
	renderInFlight_ = false;
	workerStarted_ = false;
	UpdatePendingStateLocked();
}

void VsmrAviso::AvisoRasterPipeline::UpdatePendingStateLocked() noexcept
{
	pendingWork_.store(
		pendingRequest_ != nullptr || renderInFlight_ || completedResult_ != nullptr,
		std::memory_order_relaxed);
}

bool VsmrAviso::AvisoRasterPipeline::IsExternalStopRequested() const noexcept
{
	if (!callbacks_.isExternalStopRequested)
		return false;
	try
	{
		return callbacks_.isExternalStopRequested();
	}
	catch (...)
	{
		return true;
	}
}

void VsmrAviso::AvisoRasterPipeline::ReportError(
	const std::string& message) const noexcept
{
	InvokeNoThrow(callbacks_.reportError, message);
}

VsmrAviso::AvisoRasterPipeline::RequestKey
VsmrAviso::AvisoRasterPipeline::MakeRequestKey(const Request& request)
{
	RequestKey key;
	key.path = request.path;
	key.useDayPalette = request.useDayPalette;
	key.groupGeneration = request.groupGeneration;
	key.rasterWidth = request.rasterWidth;
	key.rasterHeight = request.rasterHeight;
	key.minLongitude = request.displayMinLongitude;
	key.minLatitude = request.displayMinLatitude;
	key.maxLongitude = request.displayMaxLongitude;
	key.maxLatitude = request.displayMaxLatitude;
	key.projectedTopLeft = request.projectedTopLeft;
	key.projectedTopRight = request.projectedTopRight;
	key.projectedBottomLeft = request.projectedBottomLeft;
	key.projectedBottomRight = request.projectedBottomRight;
	return key;
}

bool VsmrAviso::AvisoRasterPipeline::RequestsMatch(
	const RequestKey& previous,
	const Request& request) noexcept
{
	const double longitudeTolerance = (std::max)(
		std::abs(request.displayMaxLongitude - request.displayMinLongitude) /
			(static_cast<double>((std::max)(request.rasterWidth, 1)) * 0.5),
		1e-10);
	const double latitudeTolerance = (std::max)(
		std::abs(request.displayMaxLatitude - request.displayMinLatitude) /
			(static_cast<double>((std::max)(request.rasterHeight, 1)) * 0.5),
		1e-10);

	return previous.path == request.path &&
		previous.useDayPalette == request.useDayPalette &&
		previous.groupGeneration == request.groupGeneration &&
		std::abs(previous.rasterWidth - request.rasterWidth) <= 2 &&
		std::abs(previous.rasterHeight - request.rasterHeight) <= 2 &&
		WithinTolerance(previous.minLongitude, request.displayMinLongitude, longitudeTolerance) &&
		WithinTolerance(previous.minLatitude, request.displayMinLatitude, latitudeTolerance) &&
		WithinTolerance(previous.maxLongitude, request.displayMaxLongitude, longitudeTolerance) &&
		WithinTolerance(previous.maxLatitude, request.displayMaxLatitude, latitudeTolerance) &&
		PointWithinTolerance(previous.projectedTopLeft, request.projectedTopLeft, 0.75) &&
		PointWithinTolerance(previous.projectedTopRight, request.projectedTopRight, 0.75) &&
		PointWithinTolerance(previous.projectedBottomLeft, request.projectedBottomLeft, 0.75) &&
		PointWithinTolerance(previous.projectedBottomRight, request.projectedBottomRight, 0.75);
}
