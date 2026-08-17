#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace VsmrPerformance
{
	constexpr std::size_t MaximumFrameSamples = 2048;
	constexpr std::size_t MaximumAvisoBuildSamples = 256;

	enum class AvisoViewport
	{
		Main,
		Inset
	};

	enum class AvisoCacheOutcome
	{
		Exact,
		Preview,
		Miss
	};

	struct Distribution
	{
		std::size_t sampleCount = 0;
		double average = 0.0;
		double median = 0.0;
		double p95 = 0.0;
		double maximum = 0.0;
	};

	struct FrameSample
	{
		std::uint64_t frameId = 0;
		std::uint64_t timestampMilliseconds = 0;
		double frameMilliseconds = 0.0;
		double sceneMilliseconds = 0.0;
		double avisoMilliseconds = 0.0;
		double targetsMilliseconds = 0.0;
		double rimcasMilliseconds = 0.0;
		double tagsMilliseconds = 0.0;
		double srwMilliseconds = 0.0;
		double euroScopeLookupMilliseconds = 0.0;
		std::size_t processedTargets = 0;
		std::size_t capturedTargets = 0;
		std::size_t radarFilteredTargets = 0;
		std::size_t iconTargets = 0;
		std::size_t tagTargets = 0;
		std::size_t visibleTargets = 0;
		std::size_t visibleTags = 0;
	};

	struct CacheSnapshot
	{
		std::uint64_t hits = 0;
		std::uint64_t misses = 0;
		double hitRate = 0.0;
		std::size_t entries = 0;
	};

	struct AvisoQueueDepth
	{
		std::size_t pending = 0;
		std::size_t inFlight = 0;
		std::size_t completed = 0;
		std::size_t workers = 0;
	};

	struct AvisoSnapshot
	{
		std::uint64_t exactHits = 0;
		std::uint64_t previewHits = 0;
		std::uint64_t misses = 0;
		std::uint64_t delayedFrames = 0;
		std::uint64_t blankDelayedFrames = 0;
		std::uint64_t requestsQueued = 0;
		std::uint64_t requestsCoalesced = 0;
		std::uint64_t requestsSuperseded = 0;
		std::uint64_t rasterBuilds = 0;
		std::uint64_t rasterBuildFailures = 0;
		std::uint64_t resultsApplied = 0;
		std::uint64_t resultsDiscarded = 0;
		AvisoQueueDepth queue;
		Distribution rasterRebuildMilliseconds;
		Distribution queueWaitMilliseconds;
	};

	struct ResourceSample
	{
		std::uint64_t timestampMilliseconds = 0;
		std::uint32_t processGdiObjects = 0;
		std::size_t ownedBitmapCount = 0;
		std::size_t aircraftBitmapCount = 0;
		std::size_t realisticIconBitmapCount = 0;
		std::size_t mainAvisoBitmapCount = 0;
		std::size_t insetAvisoBitmapCount = 0;
		std::uint64_t estimatedBitmapBytes = 0;
	};

	struct Snapshot
	{
		std::uint64_t generation = 0;
		std::uint64_t collectionStartedUtcMilliseconds = 0;
		std::uint64_t collectionStartedMonotonicMilliseconds = 0;
		std::uint64_t totalFrames = 0;
		std::uint32_t windowSeconds = 0;
		std::uint64_t windowStartMilliseconds = 0;
		std::uint64_t windowEndMilliseconds = 0;
		FrameSample latestFrame;
		bool hasLatestFrame = false;
		std::vector<FrameSample> series;
		Distribution frame;
		Distribution scene;
		Distribution aviso;
		Distribution targets;
		Distribution rimcas;
		Distribution tags;
		Distribution srw;
		Distribution euroScopeLookups;
		Distribution avisoRasterRebuild;
		AvisoSnapshot mainAviso;
		AvisoSnapshot insetAviso;
		CacheSnapshot aircraftSourceCache;
		CacheSnapshot realisticScaledCache;
		CacheSnapshot realisticRotatedCache;
		ResourceSample resources;
		std::uint64_t avisoDelayedFrames = 0;
		std::uint64_t avisoFallbackFrames = 0;
		std::uint64_t avisoBlankDelayedFrames = 0;
	};

	class PerformanceDiagnostics
	{
	public:
		PerformanceDiagnostics();

		void Reset();
		void BeginFrame() noexcept;
		void RecordFrame(FrameSample sample) noexcept;
		void RecordAvisoCacheOutcome(
			AvisoViewport viewport,
			AvisoCacheOutcome outcome,
			bool delayed,
			bool blank) noexcept;
		void RecordAvisoRequestQueued(AvisoViewport viewport, bool superseded) noexcept;
		void RecordAvisoRequestCoalesced(AvisoViewport viewport) noexcept;
		void RecordAvisoRasterBuild(
			AvisoViewport viewport,
			double rebuildMilliseconds,
			double queueWaitMilliseconds,
			bool succeeded) noexcept;
		void RecordAvisoResultApplied(AvisoViewport viewport) noexcept;
		void RecordAvisoResultDiscarded(AvisoViewport viewport) noexcept;
		void RecordAircraftSourceCache(bool hit) noexcept;
		void RecordRealisticScaledCache(bool hit) noexcept;
		void RecordRealisticRotatedCache(bool hit) noexcept;
		void RecordResources(const ResourceSample& resources) noexcept;

		Snapshot GetSnapshot(
			std::uint32_t windowSeconds,
			std::size_t maximumSeriesPoints,
			const AvisoQueueDepth& mainQueue,
			const AvisoQueueDepth& insetQueue,
			std::size_t aircraftSourceEntries,
			std::size_t realisticScaledEntries,
			std::size_t realisticRotatedEntries) const;

		static std::uint64_t MonotonicMilliseconds() noexcept;
		static std::uint64_t UtcMilliseconds() noexcept;

	private:
		struct CacheCounters
		{
			std::uint64_t hits = 0;
			std::uint64_t misses = 0;
		};
		struct AtomicCacheCounters
		{
			std::atomic<std::uint64_t> hits{ 0 };
			std::atomic<std::uint64_t> misses{ 0 };
		};

		struct AvisoBuildSample
		{
			std::uint64_t timestampMilliseconds = 0;
			double rebuildMilliseconds = 0.0;
			double queueWaitMilliseconds = 0.0;
		};

		struct AvisoCounters
		{
			std::uint64_t exactHits = 0;
			std::uint64_t previewHits = 0;
			std::uint64_t misses = 0;
			std::uint64_t delayedFrames = 0;
			std::uint64_t blankDelayedFrames = 0;
			std::uint64_t requestsQueued = 0;
			std::uint64_t requestsCoalesced = 0;
			std::uint64_t requestsSuperseded = 0;
			std::uint64_t rasterBuilds = 0;
			std::uint64_t rasterBuildFailures = 0;
			std::uint64_t resultsApplied = 0;
			std::uint64_t resultsDiscarded = 0;
			std::array<AvisoBuildSample, MaximumAvisoBuildSamples> buildSamples{};
			std::size_t nextBuildSample = 0;
			std::size_t buildSampleCount = 0;
		};

		AvisoCounters& AvisoFor(AvisoViewport viewport) noexcept;
		const AvisoCounters& AvisoFor(AvisoViewport viewport) const noexcept;

		mutable std::mutex Mutex;
		std::array<FrameSample, MaximumFrameSamples> FrameSamples{};
		std::size_t NextFrameSample = 0;
		std::size_t FrameSampleCount = 0;
		std::uint64_t TotalFrames = 0;
		std::uint64_t Generation = 1;
		std::uint64_t CollectionStartedUtcMilliseconds = 0;
		std::uint64_t CollectionStartedMonotonicMilliseconds = 0;
		AvisoCounters MainAviso;
		AvisoCounters InsetAviso;
		AtomicCacheCounters AircraftSourceCache;
		AtomicCacheCounters RealisticScaledCache;
		AtomicCacheCounters RealisticRotatedCache;
		ResourceSample Resources;
		std::uint64_t AvisoDelayedFrames = 0;
		std::uint64_t AvisoFallbackFrames = 0;
		std::uint64_t AvisoBlankDelayedFrames = 0;
		bool CurrentFrameAvisoDelayed = false;
		bool CurrentFrameAvisoFallback = false;
		bool CurrentFrameAvisoBlank = false;
	};

	std::string BuildJsonReport(
		const Snapshot& snapshot,
		const std::string& version,
		const std::string& airport,
		const std::string& profile);
}
