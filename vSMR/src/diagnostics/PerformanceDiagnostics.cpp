#include "platform/windows/PrecompiledHeader.hpp"

#include "diagnostics/PerformanceDiagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>

namespace
{
	using VsmrPerformance::Distribution;
	using VsmrPerformance::FrameSample;

	Distribution CalculateDistribution(std::vector<double> values)
	{
		values.erase(
			std::remove_if(values.begin(), values.end(), [](double value)
			{
				return !std::isfinite(value) || value < 0.0;
			}),
			values.end());
		Distribution result;
		result.sampleCount = values.size();
		if (values.empty())
			return result;

		const double sum = std::accumulate(values.begin(), values.end(), 0.0);
		result.average = sum / static_cast<double>(values.size());
		std::sort(values.begin(), values.end());
		const std::size_t middle = values.size() / 2;
		result.median = (values.size() & 1U) != 0U
			? values[middle]
			: (values[middle - 1] + values[middle]) * 0.5;
		const std::size_t p95Rank = (std::max)(
			static_cast<std::size_t>(1),
			static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size()))));
		result.p95 = values[(std::min)(p95Rank - 1, values.size() - 1)];
		result.maximum = values.back();
		return result;
	}

	template <typename Selector>
	Distribution CalculateFrameDistribution(
		const std::vector<FrameSample>& samples,
		Selector selector)
	{
		std::vector<double> values;
		values.reserve(samples.size());
		for (const FrameSample& sample : samples)
			values.push_back(selector(sample));
		return CalculateDistribution(std::move(values));
	}

	double CacheHitRate(std::uint64_t hits, std::uint64_t misses)
	{
		const std::uint64_t total = hits + misses;
		return total == 0
			? 0.0
			: static_cast<double>(hits) / static_cast<double>(total);
	}

	std::string EscapeJson(const std::string& source)
	{
		std::ostringstream output;
		for (const unsigned char character : source)
		{
			switch (character)
			{
			case '"': output << "\\\""; break;
			case '\\': output << "\\\\"; break;
			case '\b': output << "\\b"; break;
			case '\f': output << "\\f"; break;
			case '\n': output << "\\n"; break;
			case '\r': output << "\\r"; break;
			case '\t': output << "\\t"; break;
			default:
				if (character < 0x20)
				{
					output << "\\u"
						<< std::hex << std::setw(4) << std::setfill('0')
						<< static_cast<unsigned int>(character)
						<< std::dec << std::setfill(' ');
				}
				else
				{
					output << static_cast<char>(character);
				}
				break;
			}
		}
		return output.str();
	}

	void WriteDistribution(
		std::ostringstream& output,
		const Distribution& distribution)
	{
		output << "{\"sampleCount\":" << distribution.sampleCount;
		if (distribution.sampleCount == 0)
		{
			output << ",\"average\":null,\"median\":null,\"p95\":null,\"maximum\":null}";
			return;
		}
		output << ",\"average\":" << distribution.average
			<< ",\"median\":" << distribution.median
			<< ",\"p95\":" << distribution.p95
			<< ",\"maximum\":" << distribution.maximum << '}';
	}

	void WriteAvisoSnapshot(
		std::ostringstream& output,
		const VsmrPerformance::AvisoSnapshot& aviso)
	{
		output << "{\"exactHits\":" << aviso.exactHits
			<< ",\"previewHits\":" << aviso.previewHits
			<< ",\"misses\":" << aviso.misses
			<< ",\"delayedFrames\":" << aviso.delayedFrames
			<< ",\"blankDelayedFrames\":" << aviso.blankDelayedFrames
			<< ",\"requestsQueued\":" << aviso.requestsQueued
			<< ",\"requestsCoalesced\":" << aviso.requestsCoalesced
			<< ",\"requestsSuperseded\":" << aviso.requestsSuperseded
			<< ",\"rasterBuilds\":" << aviso.rasterBuilds
			<< ",\"rasterBuildFailures\":" << aviso.rasterBuildFailures
			<< ",\"resultsApplied\":" << aviso.resultsApplied
			<< ",\"resultsDiscarded\":" << aviso.resultsDiscarded
			<< ",\"queue\":{\"pending\":" << aviso.queue.pending
			<< ",\"inFlight\":" << aviso.queue.inFlight
			<< ",\"completed\":" << aviso.queue.completed
			<< ",\"workers\":" << aviso.queue.workers << "}"
			<< ",\"rasterRebuildMilliseconds\":";
		WriteDistribution(output, aviso.rasterRebuildMilliseconds);
		output << ",\"queueWaitMilliseconds\":";
		WriteDistribution(output, aviso.queueWaitMilliseconds);
		output << '}';
	}
}

VsmrPerformance::PerformanceDiagnostics::PerformanceDiagnostics()
	: CollectionStartedUtcMilliseconds(UtcMilliseconds()),
	CollectionStartedMonotonicMilliseconds(MonotonicMilliseconds())
{
}

std::uint64_t VsmrPerformance::PerformanceDiagnostics::MonotonicMilliseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t VsmrPerformance::PerformanceDiagnostics::UtcMilliseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

void VsmrPerformance::PerformanceDiagnostics::Reset()
{
	std::lock_guard<std::mutex> guard(Mutex);
	FrameSamples = {};
	NextFrameSample = 0;
	FrameSampleCount = 0;
	TotalFrames = 0;
	++Generation;
	CollectionStartedUtcMilliseconds = UtcMilliseconds();
	CollectionStartedMonotonicMilliseconds = MonotonicMilliseconds();
	MainAviso = {};
	InsetAviso = {};
	AircraftSourceCache.hits.store(0, std::memory_order_relaxed);
	AircraftSourceCache.misses.store(0, std::memory_order_relaxed);
	RealisticScaledCache.hits.store(0, std::memory_order_relaxed);
	RealisticScaledCache.misses.store(0, std::memory_order_relaxed);
	RealisticRotatedCache.hits.store(0, std::memory_order_relaxed);
	RealisticRotatedCache.misses.store(0, std::memory_order_relaxed);
	Resources = {};
	AvisoDelayedFrames = 0;
	AvisoFallbackFrames = 0;
	AvisoBlankDelayedFrames = 0;
	CurrentFrameAvisoDelayed = false;
	CurrentFrameAvisoFallback = false;
	CurrentFrameAvisoBlank = false;
}

void VsmrPerformance::PerformanceDiagnostics::BeginFrame() noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		CurrentFrameAvisoDelayed = false;
		CurrentFrameAvisoFallback = false;
		CurrentFrameAvisoBlank = false;
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordFrame(FrameSample sample) noexcept
{
	try
	{
		if (sample.timestampMilliseconds == 0)
			sample.timestampMilliseconds = MonotonicMilliseconds();
		std::lock_guard<std::mutex> guard(Mutex);
		if (CurrentFrameAvisoDelayed)
			++AvisoDelayedFrames;
		if (CurrentFrameAvisoFallback)
			++AvisoFallbackFrames;
		if (CurrentFrameAvisoBlank)
			++AvisoBlankDelayedFrames;
		CurrentFrameAvisoDelayed = false;
		CurrentFrameAvisoFallback = false;
		CurrentFrameAvisoBlank = false;
		FrameSamples[NextFrameSample] = std::move(sample);
		NextFrameSample = (NextFrameSample + 1) % FrameSamples.size();
		FrameSampleCount = (std::min)(FrameSampleCount + 1, FrameSamples.size());
		++TotalFrames;
	}
	catch (...)
	{
		// Diagnostics must never unwind through a EuroScope refresh callback.
	}
}

VsmrPerformance::PerformanceDiagnostics::AvisoCounters&
VsmrPerformance::PerformanceDiagnostics::AvisoFor(AvisoViewport viewport) noexcept
{
	return viewport == AvisoViewport::Main ? MainAviso : InsetAviso;
}

const VsmrPerformance::PerformanceDiagnostics::AvisoCounters&
VsmrPerformance::PerformanceDiagnostics::AvisoFor(AvisoViewport viewport) const noexcept
{
	return viewport == AvisoViewport::Main ? MainAviso : InsetAviso;
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoCacheOutcome(
	AvisoViewport viewport,
	AvisoCacheOutcome outcome,
	bool delayed,
	bool blank) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		AvisoCounters& counters = AvisoFor(viewport);
		switch (outcome)
		{
		case AvisoCacheOutcome::Exact: ++counters.exactHits; break;
		case AvisoCacheOutcome::Preview: ++counters.previewHits; break;
		case AvisoCacheOutcome::Miss: ++counters.misses; break;
		}
		if (delayed)
		{
			++counters.delayedFrames;
			CurrentFrameAvisoDelayed = true;
		}
		if (outcome == AvisoCacheOutcome::Preview)
			CurrentFrameAvisoFallback = true;
		if (delayed && blank)
		{
			++counters.blankDelayedFrames;
			CurrentFrameAvisoBlank = true;
		}
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoRequestQueued(
	AvisoViewport viewport,
	bool superseded) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		AvisoCounters& counters = AvisoFor(viewport);
		++counters.requestsQueued;
		if (superseded)
			++counters.requestsSuperseded;
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoRequestCoalesced(
	AvisoViewport viewport) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		++AvisoFor(viewport).requestsCoalesced;
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoRasterBuild(
	AvisoViewport viewport,
	double rebuildMilliseconds,
	double queueWaitMilliseconds,
	bool succeeded) noexcept
{
	try
	{
		if (!std::isfinite(rebuildMilliseconds) || rebuildMilliseconds < 0.0)
			rebuildMilliseconds = 0.0;
		if (!std::isfinite(queueWaitMilliseconds) || queueWaitMilliseconds < 0.0)
			queueWaitMilliseconds = 0.0;
		std::lock_guard<std::mutex> guard(Mutex);
		AvisoCounters& counters = AvisoFor(viewport);
		++counters.rasterBuilds;
		if (!succeeded)
			++counters.rasterBuildFailures;
		AvisoBuildSample& sample = counters.buildSamples[counters.nextBuildSample];
		sample.timestampMilliseconds = MonotonicMilliseconds();
		sample.rebuildMilliseconds = rebuildMilliseconds;
		sample.queueWaitMilliseconds = queueWaitMilliseconds;
		counters.nextBuildSample = (counters.nextBuildSample + 1) % counters.buildSamples.size();
		counters.buildSampleCount = (std::min)(
			counters.buildSampleCount + 1,
			counters.buildSamples.size());
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoResultApplied(
	AvisoViewport viewport) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		++AvisoFor(viewport).resultsApplied;
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAvisoResultDiscarded(
	AvisoViewport viewport) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		++AvisoFor(viewport).resultsDiscarded;
	}
	catch (...)
	{
	}
}

void VsmrPerformance::PerformanceDiagnostics::RecordAircraftSourceCache(bool hit) noexcept
{
	if (hit)
		AircraftSourceCache.hits.fetch_add(1, std::memory_order_relaxed);
	else
		AircraftSourceCache.misses.fetch_add(1, std::memory_order_relaxed);
}

void VsmrPerformance::PerformanceDiagnostics::RecordRealisticScaledCache(bool hit) noexcept
{
	if (hit)
		RealisticScaledCache.hits.fetch_add(1, std::memory_order_relaxed);
	else
		RealisticScaledCache.misses.fetch_add(1, std::memory_order_relaxed);
}

void VsmrPerformance::PerformanceDiagnostics::RecordRealisticRotatedCache(bool hit) noexcept
{
	if (hit)
		RealisticRotatedCache.hits.fetch_add(1, std::memory_order_relaxed);
	else
		RealisticRotatedCache.misses.fetch_add(1, std::memory_order_relaxed);
}

void VsmrPerformance::PerformanceDiagnostics::RecordResources(
	const ResourceSample& resources) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(Mutex);
		Resources = resources;
	}
	catch (...)
	{
	}
}

VsmrPerformance::Snapshot VsmrPerformance::PerformanceDiagnostics::GetSnapshot(
	std::uint32_t windowSeconds,
	std::size_t maximumSeriesPoints,
	const AvisoQueueDepth& mainQueue,
	const AvisoQueueDepth& insetQueue,
	std::size_t aircraftSourceEntries,
	std::size_t realisticScaledEntries,
	std::size_t realisticRotatedEntries) const
{
	std::vector<FrameSample> allFrames;
	AvisoCounters mainAviso;
	AvisoCounters insetAviso;
	CacheCounters aircraftSourceCache;
	CacheCounters realisticScaledCache;
	CacheCounters realisticRotatedCache;
	ResourceSample resources;
	Snapshot result;
	{
		std::lock_guard<std::mutex> guard(Mutex);
		result.generation = Generation;
		result.collectionStartedUtcMilliseconds = CollectionStartedUtcMilliseconds;
		result.collectionStartedMonotonicMilliseconds = CollectionStartedMonotonicMilliseconds;
		result.totalFrames = TotalFrames;
		allFrames.reserve(FrameSampleCount);
		const std::size_t first = (NextFrameSample + FrameSamples.size() - FrameSampleCount) % FrameSamples.size();
		for (std::size_t index = 0; index < FrameSampleCount; ++index)
			allFrames.push_back(FrameSamples[(first + index) % FrameSamples.size()]);
		mainAviso = MainAviso;
		insetAviso = InsetAviso;
		resources = Resources;
		result.avisoDelayedFrames = AvisoDelayedFrames;
		result.avisoFallbackFrames = AvisoFallbackFrames;
		result.avisoBlankDelayedFrames = AvisoBlankDelayedFrames;
	}
	aircraftSourceCache.hits = AircraftSourceCache.hits.load(std::memory_order_relaxed);
	aircraftSourceCache.misses = AircraftSourceCache.misses.load(std::memory_order_relaxed);
	realisticScaledCache.hits = RealisticScaledCache.hits.load(std::memory_order_relaxed);
	realisticScaledCache.misses = RealisticScaledCache.misses.load(std::memory_order_relaxed);
	realisticRotatedCache.hits = RealisticRotatedCache.hits.load(std::memory_order_relaxed);
	realisticRotatedCache.misses = RealisticRotatedCache.misses.load(std::memory_order_relaxed);

	result.windowSeconds = windowSeconds;
	result.windowEndMilliseconds = MonotonicMilliseconds();
	const std::uint64_t requestedDuration = static_cast<std::uint64_t>(windowSeconds) * 1000ULL;
	result.windowStartMilliseconds = windowSeconds == 0 || requestedDuration > result.windowEndMilliseconds
		? 0
		: result.windowEndMilliseconds - requestedDuration;

	std::vector<FrameSample> frames;
	frames.reserve(allFrames.size());
	for (const FrameSample& sample : allFrames)
	{
		if (windowSeconds == 0 || sample.timestampMilliseconds >= result.windowStartMilliseconds)
			frames.push_back(sample);
	}
	if (!frames.empty())
	{
		result.hasLatestFrame = true;
		result.latestFrame = frames.back();
	}

	result.frame = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.frameMilliseconds; });
	result.scene = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.sceneMilliseconds; });
	result.aviso = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.avisoMilliseconds; });
	result.targets = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.targetsMilliseconds; });
	result.rimcas = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.rimcasMilliseconds; });
	result.tags = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.tagsMilliseconds; });
	result.srw = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.srwMilliseconds; });
	result.euroScopeLookups = CalculateFrameDistribution(frames, [](const FrameSample& sample) { return sample.euroScopeLookupMilliseconds; });

	auto fillAviso = [&](const AvisoCounters& source, const AvisoQueueDepth& queue, AvisoSnapshot& destination)
	{
		destination.exactHits = source.exactHits;
		destination.previewHits = source.previewHits;
		destination.misses = source.misses;
		destination.delayedFrames = source.delayedFrames;
		destination.blankDelayedFrames = source.blankDelayedFrames;
		destination.requestsQueued = source.requestsQueued;
		destination.requestsCoalesced = source.requestsCoalesced;
		destination.requestsSuperseded = source.requestsSuperseded;
		destination.rasterBuilds = source.rasterBuilds;
		destination.rasterBuildFailures = source.rasterBuildFailures;
		destination.resultsApplied = source.resultsApplied;
		destination.resultsDiscarded = source.resultsDiscarded;
		destination.queue = queue;

		std::vector<double> rebuildValues;
		std::vector<double> queueWaitValues;
		rebuildValues.reserve(source.buildSampleCount);
		queueWaitValues.reserve(source.buildSampleCount);
		const std::size_t first = (source.nextBuildSample + source.buildSamples.size() - source.buildSampleCount) % source.buildSamples.size();
		for (std::size_t index = 0; index < source.buildSampleCount; ++index)
		{
			const AvisoBuildSample& sample = source.buildSamples[(first + index) % source.buildSamples.size()];
			if (windowSeconds != 0 && sample.timestampMilliseconds < result.windowStartMilliseconds)
				continue;
			rebuildValues.push_back(sample.rebuildMilliseconds);
			queueWaitValues.push_back(sample.queueWaitMilliseconds);
		}
		destination.rasterRebuildMilliseconds = CalculateDistribution(std::move(rebuildValues));
		destination.queueWaitMilliseconds = CalculateDistribution(std::move(queueWaitValues));
	};
	fillAviso(mainAviso, mainQueue, result.mainAviso);
	fillAviso(insetAviso, insetQueue, result.insetAviso);

	std::vector<double> allAvisoRebuilds;
	auto appendBuilds = [&](const AvisoCounters& source)
	{
		const std::size_t first = (source.nextBuildSample + source.buildSamples.size() - source.buildSampleCount) % source.buildSamples.size();
		for (std::size_t index = 0; index < source.buildSampleCount; ++index)
		{
			const AvisoBuildSample& sample = source.buildSamples[(first + index) % source.buildSamples.size()];
			if (windowSeconds == 0 || sample.timestampMilliseconds >= result.windowStartMilliseconds)
				allAvisoRebuilds.push_back(sample.rebuildMilliseconds);
		}
	};
	appendBuilds(mainAviso);
	appendBuilds(insetAviso);
	result.avisoRasterRebuild = CalculateDistribution(std::move(allAvisoRebuilds));

	result.aircraftSourceCache.hits = aircraftSourceCache.hits;
	result.aircraftSourceCache.misses = aircraftSourceCache.misses;
	result.aircraftSourceCache.hitRate = CacheHitRate(aircraftSourceCache.hits, aircraftSourceCache.misses);
	result.aircraftSourceCache.entries = aircraftSourceEntries;
	result.realisticScaledCache.hits = realisticScaledCache.hits;
	result.realisticScaledCache.misses = realisticScaledCache.misses;
	result.realisticScaledCache.hitRate = CacheHitRate(realisticScaledCache.hits, realisticScaledCache.misses);
	result.realisticScaledCache.entries = realisticScaledEntries;
	result.realisticRotatedCache.hits = realisticRotatedCache.hits;
	result.realisticRotatedCache.misses = realisticRotatedCache.misses;
	result.realisticRotatedCache.hitRate = CacheHitRate(realisticRotatedCache.hits, realisticRotatedCache.misses);
	result.realisticRotatedCache.entries = realisticRotatedEntries;
	result.resources = resources;

	if (maximumSeriesPoints == 0 || frames.empty())
	{
		result.series.clear();
	}
	else if (frames.size() <= maximumSeriesPoints)
	{
		result.series = std::move(frames);
	}
	else if (maximumSeriesPoints == 1)
	{
		result.series.push_back(frames.back());
	}
	else
	{
		result.series.reserve(maximumSeriesPoints);
		const double step = static_cast<double>(frames.size() - 1) /
			static_cast<double>(maximumSeriesPoints - 1);
		std::size_t previousIndex = frames.size();
		for (std::size_t point = 0; point < maximumSeriesPoints; ++point)
		{
			std::size_t sourceIndex = static_cast<std::size_t>(std::llround(static_cast<double>(point) * step));
			sourceIndex = (std::min)(sourceIndex, frames.size() - 1);
			if (sourceIndex == previousIndex)
				continue;
			result.series.push_back(frames[sourceIndex]);
			previousIndex = sourceIndex;
		}
	}
	return result;
}

std::string VsmrPerformance::BuildJsonReport(
	const Snapshot& snapshot,
	const std::string& version,
	const std::string& airport,
	const std::string& profile)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::setprecision(6) << std::fixed;
	output << "{\n  \"schemaVersion\": 1,"
		<< "\n  \"generatedUtcMilliseconds\": " << PerformanceDiagnostics::UtcMilliseconds() << ','
		<< "\n  \"version\": \"" << EscapeJson(version) << "\","
		<< "\n  \"airport\": \"" << EscapeJson(airport) << "\","
		<< "\n  \"profile\": \"" << EscapeJson(profile) << "\","
		<< "\n  \"frameDefinition\": \"vSMR measured render-pipeline interval within REFRESH_PHASE_BEFORE_TAGS\","
		<< "\n  \"gdiScope\": \"process-wide GetGuiResources count\","
		<< "\n  \"generation\": " << snapshot.generation << ','
		<< "\n  \"collectionStartedUtcMilliseconds\": " << snapshot.collectionStartedUtcMilliseconds << ','
		<< "\n  \"collectionStartedMonotonicMilliseconds\": " << snapshot.collectionStartedMonotonicMilliseconds << ','
		<< "\n  \"windowSeconds\": " << snapshot.windowSeconds << ','
		<< "\n  \"windowStartMonotonicMilliseconds\": " << snapshot.windowStartMilliseconds << ','
		<< "\n  \"windowEndMonotonicMilliseconds\": " << snapshot.windowEndMilliseconds << ','
		<< "\n  \"observedStartMonotonicMilliseconds\": "
		<< (snapshot.series.empty() ? 0 : snapshot.series.front().timestampMilliseconds) << ','
		<< "\n  \"observedEndMonotonicMilliseconds\": "
		<< (snapshot.series.empty() ? 0 : snapshot.series.back().timestampMilliseconds) << ','
		<< "\n  \"ringCapacity\": " << MaximumFrameSamples << ','
		<< "\n  \"ringRetainedFrames\": "
		<< (std::min)(snapshot.totalFrames, static_cast<std::uint64_t>(MaximumFrameSamples)) << ','
		<< "\n  \"windowFrameSamples\": " << snapshot.series.size() << ','
		<< "\n  \"ringOverwrittenFrames\": "
		<< (snapshot.totalFrames > MaximumFrameSamples
			? snapshot.totalFrames - MaximumFrameSamples
			: 0) << ','
		<< "\n  \"totalFrames\": " << snapshot.totalFrames << ','
		<< "\n  \"timings\": {";
	const std::pair<const char*, const Distribution*> timingValues[] = {
		{ "frame", &snapshot.frame },
		{ "scene", &snapshot.scene },
		{ "aviso", &snapshot.aviso },
		{ "targets", &snapshot.targets },
		{ "rimcas", &snapshot.rimcas },
		{ "tags", &snapshot.tags },
		{ "srw", &snapshot.srw },
		{ "euroScopeLookups", &snapshot.euroScopeLookups },
		{ "avisoRasterRebuild", &snapshot.avisoRasterRebuild }
	};
	for (std::size_t index = 0;
		index < sizeof(timingValues) / sizeof(timingValues[0]);
		++index)
	{
		if (index != 0)
			output << ',';
		output << "\n    \"" << timingValues[index].first << "\": ";
		WriteDistribution(output, *timingValues[index].second);
	}
	output << "\n  },\n  \"aviso\": {\n    \"main\": ";
	WriteAvisoSnapshot(output, snapshot.mainAviso);
	output << ",\n    \"inset\": ";
	WriteAvisoSnapshot(output, snapshot.insetAviso);
	output << "\n  },\n  \"avisoFrames\": {"
		<< "\"delayed\":" << snapshot.avisoDelayedFrames
		<< ",\"usingFallback\":" << snapshot.avisoFallbackFrames
		<< ",\"blankWhileDelayed\":" << snapshot.avisoBlankDelayedFrames
		<< "},\n  \"caches\": {"
		<< "\n    \"aircraftSource\": {\"hits\":" << snapshot.aircraftSourceCache.hits
		<< ",\"misses\":" << snapshot.aircraftSourceCache.misses
		<< ",\"hitRate\":" << snapshot.aircraftSourceCache.hitRate
		<< ",\"entries\":" << snapshot.aircraftSourceCache.entries << "},"
		<< "\n    \"realisticScaled\": {\"hits\":" << snapshot.realisticScaledCache.hits
		<< ",\"misses\":" << snapshot.realisticScaledCache.misses
		<< ",\"hitRate\":" << snapshot.realisticScaledCache.hitRate
		<< ",\"entries\":" << snapshot.realisticScaledCache.entries << "},"
		<< "\n    \"realisticRotated\": {\"hits\":" << snapshot.realisticRotatedCache.hits
		<< ",\"misses\":" << snapshot.realisticRotatedCache.misses
		<< ",\"hitRate\":" << snapshot.realisticRotatedCache.hitRate
		<< ",\"entries\":" << snapshot.realisticRotatedCache.entries << "}"
		<< "\n  },\n  \"resources\": {"
		<< "\"timestampMilliseconds\":" << snapshot.resources.timestampMilliseconds
		<< ",\"processGdiObjects\":" << snapshot.resources.processGdiObjects
		<< ",\"ownedBitmapCount\":" << snapshot.resources.ownedBitmapCount
		<< ",\"aircraftBitmapCount\":" << snapshot.resources.aircraftBitmapCount
		<< ",\"realisticIconBitmapCount\":" << snapshot.resources.realisticIconBitmapCount
		<< ",\"mainAvisoBitmapCount\":" << snapshot.resources.mainAvisoBitmapCount
		<< ",\"insetAvisoBitmapCount\":" << snapshot.resources.insetAvisoBitmapCount
		<< ",\"estimatedBitmapBytes\":" << snapshot.resources.estimatedBitmapBytes
		<< "},\n  \"frames\": [";
	for (std::size_t index = 0; index < snapshot.series.size(); ++index)
	{
		const FrameSample& frame = snapshot.series[index];
		if (index != 0)
			output << ',';
		output << "\n    {\"frameId\":" << frame.frameId
			<< ",\"timestampMilliseconds\":" << frame.timestampMilliseconds
			<< ",\"frame\":" << frame.frameMilliseconds
			<< ",\"scene\":" << frame.sceneMilliseconds
			<< ",\"aviso\":" << frame.avisoMilliseconds
			<< ",\"targets\":" << frame.targetsMilliseconds
			<< ",\"rimcas\":" << frame.rimcasMilliseconds
			<< ",\"tags\":" << frame.tagsMilliseconds
			<< ",\"srw\":" << frame.srwMilliseconds
			<< ",\"euroScopeLookups\":" << frame.euroScopeLookupMilliseconds
			<< ",\"processedTargets\":" << frame.processedTargets
			<< ",\"capturedTargets\":" << frame.capturedTargets
			<< ",\"radarFilteredTargets\":" << frame.radarFilteredTargets
			<< ",\"iconTargets\":" << frame.iconTargets
			<< ",\"tagTargets\":" << frame.tagTargets
			<< ",\"visibleTargets\":" << frame.visibleTargets
			<< ",\"visibleTags\":" << frame.visibleTags << '}';
	}
	output << "\n  ]\n}\n";
	return output.str();
}
