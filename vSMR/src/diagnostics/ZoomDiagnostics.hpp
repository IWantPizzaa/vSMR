#pragma once

#include "diagnostics/PerformanceDiagnostics.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>

namespace VsmrPerformance
{
	// Radar/UI-thread only. No disk I/O, allocations or extra SDK reads per event.
	struct ZoomTimingSample
	{
		std::uint64_t elapsedMs = 0, frames = 0, gaps = 0, gapTotalMs = 0, gapMaxMs = 0;
		std::uint64_t wheelClaimed = 0, wheelPassed = 0, wheelUp = 0, wheelDown = 0;
		std::uint32_t wheelMessageAgeMaxMs = 0;
		double frameMaxMs = 0, mainMaxMs = 0, insetMaxMs = 0;
	};

	class ZoomTimingWindow
	{
	public:
		void Reset() noexcept { *this = {}; }
		void RecordWheel(bool claimed, int delta, std::uint32_t messageAgeMs) noexcept
		{
			if (claimed) ++sample_.wheelClaimed; else ++sample_.wheelPassed;
			if (delta > 0) ++sample_.wheelUp;
			if (delta < 0) ++sample_.wheelDown;
			sample_.wheelMessageAgeMaxMs = (std::max)(sample_.wheelMessageAgeMaxMs, messageAgeMs);
		}

		std::optional<ZoomTimingSample> RecordFrame(const FrameSample& frame, bool enabled) noexcept
		{
			if (!enabled) { Reset(); return std::nullopt; }
			const auto now = frame.timestampMilliseconds;
			if (!started_ || now < previousFrameMs_) {
				started_ = true;
				startedMs_ = now;
				previousFrameMs_ = now;
			} else {
				const auto gap = now - previousFrameMs_;
				++sample_.gaps;
				sample_.gapTotalMs += gap;
				sample_.gapMaxMs = (std::max)(sample_.gapMaxMs, gap);
				previousFrameMs_ = now;
			}
			++sample_.frames;
			sample_.frameMaxMs = (std::max)(sample_.frameMaxMs, frame.frameMilliseconds);
			sample_.mainMaxMs = (std::max)(sample_.mainMaxMs, frame.avisoMilliseconds);
			sample_.insetMaxMs = (std::max)(sample_.insetMaxMs, frame.avisoInsetMilliseconds);
			if (now - startedMs_ < 2000) return std::nullopt;
			sample_.elapsedMs = now - startedMs_;
			const auto result = sample_;
			sample_ = {};
			startedMs_ = now;
			return result;
		}
	private:
		ZoomTimingSample sample_;
		bool started_ = false;
		std::uint64_t startedMs_ = 0, previousFrameMs_ = 0;
	};

	inline std::string FormatZoomDiagnostics(const ZoomTimingSample& timing, const Snapshot& snapshot)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << std::fixed << std::setprecision(1)
			<< "ZoomPerf window_ms=" << timing.elapsedMs << " frames=" << timing.frames
			<< " frame_max_ms=" << timing.frameMaxMs
			<< " gap_avg_ms=" << (timing.gaps ? static_cast<double>(timing.gapTotalMs) / timing.gaps : 0.0)
			<< " gap_max_ms=" << timing.gapMaxMs
			<< " main_max_ms=" << timing.mainMaxMs << " inset_max_ms=" << timing.insetMaxMs
			<< " wheel_claimed=" << timing.wheelClaimed << " wheel_passed=" << timing.wheelPassed
			<< " wheel_up=" << timing.wheelUp << " wheel_down=" << timing.wheelDown
			<< " wheel_message_age_max_ms=" << timing.wheelMessageAgeMaxMs;
		// Cache counters are cumulative; compare successive records from the same view.
		// Build timings cover the last two seconds and exclude cancelled builds.
		auto append = [&](const char* name, const AvisoSnapshot& a) {
			out << ' ' << name << "{exact_total=" << a.exactHits << ",preview_total=" << a.previewHits
				<< ",miss_total=" << a.misses << ",blank_total=" << a.blankDelayedFrames
				<< ",queued_total=" << a.requestsQueued << ",superseded_total=" << a.requestsSuperseded
				<< ",debounced_total=" << a.requestsDebounced << ",cancelled_total=" << a.rasterBuildsCancelled
				<< ",failed_total=" << a.rasterBuildFailures << ",applied_total=" << a.resultsApplied
				<< ",discarded_total=" << a.resultsDiscarded
				<< ",build_samples_2s=" << a.rasterRebuildMilliseconds.sampleCount
				<< ",build_max_ms_2s=" << a.rasterRebuildMilliseconds.maximum
				<< ",wait_max_ms_2s=" << a.queueWaitMilliseconds.maximum
				<< ",pending=" << a.queue.pending << ",in_flight=" << a.queue.inFlight << '}';
		};
		append("main", snapshot.mainAviso);
		append("inset", snapshot.insetAviso);
		return out.str();
	}
}
