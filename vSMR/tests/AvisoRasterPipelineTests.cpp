#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "AvisoRasterPipelineTests.hpp"
#include "aviso/AvisoRasterPipeline.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	using Pipeline = VsmrAviso::AvisoRasterPipeline;
	using Failures = std::vector<std::string>;

	constexpr auto TestTimeout = std::chrono::seconds(5);

	void Check(bool condition, const char* message, Failures& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}

	class TestEvent final
	{
	public:
		template <typename Predicate>
		bool Wait(Predicate predicate)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, TestTimeout, std::move(predicate));
		}

		bool WaitForIdle(Pipeline& pipeline)
		{
			const auto deadline = std::chrono::steady_clock::now() + TestTimeout;
			std::unique_lock<std::mutex> lock(mutex_);
			while (pipeline.HasPendingWork())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				condition_.wait_for(lock, std::chrono::milliseconds(2));
			}
			return true;
		}

		void Notify() noexcept
		{
			condition_.notify_all();
		}

	private:
		std::mutex mutex_;
		std::condition_variable condition_;
	};

	class RenderGate final
	{
	public:
		void EnterAndWait()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			entered_ = true;
			condition_.notify_all();
			condition_.wait(lock, [&]() { return released_; });
		}

		bool WaitUntilEntered()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, TestTimeout, [&]() { return entered_; });
		}

		void Release()
		{
			{
				std::lock_guard<std::mutex> lock(mutex_);
				released_ = true;
			}
			condition_.notify_all();
		}

	private:
		std::mutex mutex_;
		std::condition_variable condition_;
		bool entered_ = false;
		bool released_ = false;
	};

	Pipeline::Request MakeRequest(const std::string& path)
	{
		static const auto features =
			std::make_shared<const std::vector<VsmrRadarTypes::AvisoFeature>>();
		static const auto labels =
			std::make_shared<const std::vector<VsmrRadarTypes::AvisoLabel>>();

		Pipeline::Request request;
		request.path = path;
		request.features = features;
		request.labels = labels;
		request.groupGeneration = 7;
		request.rasterWidth = 64;
		request.rasterHeight = 48;
		request.displayMinLongitude = 1.0;
		request.displayMinLatitude = 2.0;
		request.displayMaxLongitude = 3.0;
		request.displayMaxLatitude = 4.0;
		request.projectedTopLeft = Gdiplus::PointF(0.0f, 0.0f);
		request.projectedTopRight = Gdiplus::PointF(64.0f, 0.0f);
		request.projectedBottomLeft = Gdiplus::PointF(0.0f, 48.0f);
		request.projectedBottomRight = Gdiplus::PointF(64.0f, 48.0f);
		return request;
	}

	std::unique_ptr<Pipeline::Result> MakeResult(const Pipeline::Request& request)
	{
		auto result = std::make_unique<Pipeline::Result>();
		result->requestId = request.requestId;
		result->groupGeneration = request.groupGeneration;
		result->useDayPalette = request.useDayPalette;
		result->path = request.path;
		result->rasterWidth = request.rasterWidth;
		result->rasterHeight = request.rasterHeight;
		result->displayMinLongitude = request.displayMinLongitude;
		result->displayMinLatitude = request.displayMinLatitude;
		result->displayMaxLongitude = request.displayMaxLongitude;
		result->displayMaxLatitude = request.displayMaxLatitude;
		result->projectedTopLeft = request.projectedTopLeft;
		result->projectedTopRight = request.projectedTopRight;
		result->projectedBottomLeft = request.projectedBottomLeft;
		result->projectedBottomRight = request.projectedBottomRight;
		return result;
	}

	void TestCompletionAndCoalescing(Failures& failures)
	{
		TestEvent completed;
		std::atomic<int> renderCalls{ 0 };
		std::atomic<int> refreshCalls{ 0 };
		std::atomic<int> coalescedCalls{ 0 };

		Pipeline::Callbacks callbacks;
		callbacks.render = [&](const Pipeline::Request& request)
		{
			renderCalls.fetch_add(1, std::memory_order_relaxed);
			return MakeResult(request);
		};
		callbacks.requestRefresh = [&]()
		{
			refreshCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};
		callbacks.diagnostics.requestCoalesced = [&]()
		{
			coalescedCalls.fetch_add(1, std::memory_order_relaxed);
		};

		Pipeline pipeline(std::move(callbacks), "test AVISO worker");
		const Pipeline::Request request = MakeRequest("completion");
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO pipeline accepts a valid render request",
			failures);
		Check(
			completed.Wait([&]() { return refreshCalls.load(std::memory_order_relaxed) == 1; }),
			"AVISO pipeline publishes a completed render within the timeout",
			failures);

		const auto result = pipeline.TakeCompleted();
		Check(
			result != nullptr && result->path == request.path && result->requestId != 0,
			"AVISO pipeline returns the latest completed render",
			failures);
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Coalesced,
			"AVISO pipeline coalesces an unchanged viewport request",
			failures);
		Check(
			renderCalls.load(std::memory_order_relaxed) == 1 &&
				coalescedCalls.load(std::memory_order_relaxed) == 1,
			"AVISO coalescing avoids a duplicate render and records diagnostics",
			failures);
		pipeline.Stop();
	}

	void TestSupersession(Failures& failures)
	{
		TestEvent completed;
		RenderGate firstRender;
		std::atomic<int> renderCalls{ 0 };
		std::atomic<int> refreshCalls{ 0 };
		std::atomic<int> discardedCalls{ 0 };
		std::atomic<int> supersededCalls{ 0 };
		std::atomic<bool> firstRenderCancelled{ false };

		Pipeline::Callbacks callbacks;
		callbacks.render = [&](const Pipeline::Request& request)
		{
			const int call = renderCalls.fetch_add(1, std::memory_order_relaxed) + 1;
			if (call == 1)
			{
				firstRender.EnterAndWait();
				firstRenderCancelled.store(
					request.cancellationToken != nullptr &&
						request.cancellationToken->load(std::memory_order_acquire) != request.requestId,
					std::memory_order_relaxed);
			}
			return MakeResult(request);
		};
		callbacks.requestRefresh = [&]()
		{
			refreshCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};
		callbacks.diagnostics.requestQueued = [&](bool superseded)
		{
			if (superseded)
				supersededCalls.fetch_add(1, std::memory_order_relaxed);
		};
		callbacks.diagnostics.resultDiscarded = [&]()
		{
			discardedCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};

		Pipeline pipeline(std::move(callbacks), "test AVISO worker");
		const Pipeline::Request firstRequest = MakeRequest("superseded");
		const Pipeline::Request latestRequest = MakeRequest("latest");
		Check(
			pipeline.Queue(firstRequest, false) == Pipeline::QueueStatus::Queued,
			"AVISO supersession test queues the first request",
			failures);
		const bool entered = firstRender.WaitUntilEntered();
		Check(entered, "AVISO supersession test enters the first render", failures);
		if (!entered)
		{
			firstRender.Release();
			pipeline.Stop();
			return;
		}

		Check(
			pipeline.Queue(latestRequest, false) == Pipeline::QueueStatus::Queued,
			"AVISO pipeline queues a newer request while rendering",
			failures);
		firstRender.Release();
		Check(
			completed.Wait([&]() { return refreshCalls.load(std::memory_order_relaxed) == 1; }),
			"AVISO pipeline publishes the superseding request",
			failures);

		const auto result = pipeline.TakeCompleted();
		Check(
			result != nullptr && result->path == latestRequest.path,
			"AVISO pipeline retains only the latest superseding result",
			failures);
		Check(
			firstRenderCancelled.load(std::memory_order_relaxed) &&
				discardedCalls.load(std::memory_order_relaxed) == 1 &&
				supersededCalls.load(std::memory_order_relaxed) == 1,
			"AVISO pipeline cancels and discards superseded in-flight work",
			failures);
		pipeline.Stop();
	}

	void TestInvalidation(Failures& failures)
	{
		TestEvent completed;
		RenderGate firstRender;
		std::atomic<int> renderCalls{ 0 };
		std::atomic<int> refreshCalls{ 0 };
		std::atomic<int> discardedCalls{ 0 };
		std::atomic<bool> invalidatedRenderCancelled{ false };

		Pipeline::Callbacks callbacks;
		callbacks.render = [&](const Pipeline::Request& request)
		{
			const int call = renderCalls.fetch_add(1, std::memory_order_relaxed) + 1;
			if (call == 1)
			{
				firstRender.EnterAndWait();
				invalidatedRenderCancelled.store(
					request.cancellationToken != nullptr &&
						request.cancellationToken->load(std::memory_order_acquire) != request.requestId,
					std::memory_order_relaxed);
			}
			return MakeResult(request);
		};
		callbacks.requestRefresh = [&]()
		{
			refreshCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};
		callbacks.diagnostics.resultDiscarded = [&]()
		{
			discardedCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};

		Pipeline pipeline(std::move(callbacks), "test AVISO worker");
		const Pipeline::Request request = MakeRequest("invalidated");
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO invalidation test queues its first request",
			failures);
		const bool entered = firstRender.WaitUntilEntered();
		Check(entered, "AVISO invalidation test enters the first render", failures);
		if (!entered)
		{
			firstRender.Release();
			pipeline.Stop();
			return;
		}

		pipeline.InvalidateRequests();
		firstRender.Release();
		Check(
			completed.Wait([&]() { return discardedCalls.load(std::memory_order_relaxed) == 1; }),
			"AVISO invalidation discards an in-flight stale result",
			failures);
		Check(
			invalidatedRenderCancelled.load(std::memory_order_relaxed) &&
				pipeline.TakeCompleted() == nullptr,
			"AVISO invalidation cancels publication of the old request",
			failures);
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO invalidation permits the same viewport to be queued again",
			failures);
		Check(
			completed.Wait([&]() { return refreshCalls.load(std::memory_order_relaxed) == 1; }),
			"AVISO invalidation retry completes",
			failures);
		Check(
			pipeline.TakeCompleted() != nullptr,
			"AVISO invalidation retry publishes a result",
			failures);
		pipeline.Stop();
	}

	void TestTransientFailureRetry(Failures& failures)
	{
		TestEvent completed;
		std::atomic<int> renderCalls{ 0 };
		std::atomic<int> failedBuilds{ 0 };
		std::atomic<int> refreshCalls{ 0 };

		Pipeline::Callbacks callbacks;
		callbacks.render = [&](const Pipeline::Request& request)
		{
			const int call = renderCalls.fetch_add(1, std::memory_order_relaxed) + 1;
			return call == 1 ? nullptr : MakeResult(request);
		};
		callbacks.requestRefresh = [&]()
		{
			refreshCalls.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};
		callbacks.diagnostics.rasterBuilt = [&](double, double, bool succeeded)
		{
			if (!succeeded)
				failedBuilds.fetch_add(1, std::memory_order_relaxed);
			completed.Notify();
		};

		Pipeline pipeline(std::move(callbacks), "test AVISO worker");
		const Pipeline::Request request = MakeRequest("transient failure");
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO transient-failure test queues its first request",
			failures);
		Check(
			completed.Wait([&]() { return failedBuilds.load(std::memory_order_relaxed) == 1; }),
			"AVISO pipeline reports a transient null render",
			failures);
		Check(
			completed.WaitForIdle(pipeline),
			"AVISO pipeline settles after a transient null render",
			failures);
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO transient null render clears the coalescing key for retry",
			failures);
		Check(
			completed.Wait([&]() { return refreshCalls.load(std::memory_order_relaxed) == 1; }),
			"AVISO transient retry completes",
			failures);
		Check(
			renderCalls.load(std::memory_order_relaxed) == 2 &&
				pipeline.TakeCompleted() != nullptr,
			"AVISO transient retry publishes its second render",
			failures);
		pipeline.Stop();
	}

	void TestStop(Failures& failures)
	{
		TestEvent stoppedEvent;
		RenderGate render;
		std::atomic<bool> stopStarted{ false };
		std::atomic<bool> stopReturned{ false };
		std::atomic<bool> renderExited{ false };

		Pipeline::Callbacks callbacks;
		callbacks.render = [&](const Pipeline::Request& request)
		{
			render.EnterAndWait();
			renderExited.store(true, std::memory_order_release);
			return MakeResult(request);
		};

		Pipeline pipeline(std::move(callbacks), "test AVISO worker");
		const Pipeline::Request request = MakeRequest("stop");
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Queued,
			"AVISO stop test queues an in-flight render",
			failures);
		const bool entered = render.WaitUntilEntered();
		Check(entered, "AVISO stop test enters the render callback", failures);
		if (!entered)
		{
			render.Release();
			pipeline.Stop();
			return;
		}

		std::thread stopper([&]()
		{
			stopStarted.store(true, std::memory_order_release);
			stoppedEvent.Notify();
			pipeline.Stop();
			stopReturned.store(true, std::memory_order_release);
			stoppedEvent.Notify();
		});
		Check(
			stoppedEvent.Wait([&]() { return stopStarted.load(std::memory_order_acquire); }),
			"AVISO stop test starts shutdown on another thread",
			failures);
		render.Release();
		const bool stopped = stoppedEvent.Wait([&]()
		{
			return stopReturned.load(std::memory_order_acquire);
		});
		Check(stopped, "AVISO Stop joins the active render worker within the timeout", failures);
		stopper.join();

		pipeline.Stop();
		const VsmrPerformance::AvisoQueueDepth depth = pipeline.QueueDepth();
		Check(
			renderExited.load(std::memory_order_acquire) &&
				depth.pending == 0 && depth.inFlight == 0 &&
				depth.completed == 0 && depth.workers == 0,
			"AVISO Stop is idempotent and leaves no worker-owned state",
			failures);
		Check(
			pipeline.Queue(request, false) == Pipeline::QueueStatus::Rejected,
			"AVISO Stop is terminal and rejects later requests",
			failures);
	}
}

std::vector<std::string> RunAvisoRasterPipelineTests()
{
	Failures failures;
	TestCompletionAndCoalescing(failures);
	TestSupersession(failures);
	TestInvalidation(failures);
	TestTransientFailureRetry(failures);
	TestStop(failures);
	return failures;
}
