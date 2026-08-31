#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterPerformance.hpp"

#include "crash/CrashReportSupport.hpp"
#include "shared/RapidJsonUtils.hpp"

#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
	using Allocator = rapidjson::Document::AllocatorType;
	using VsmrRapidJson::AddString;

	std::string FormatUtcMilliseconds(std::uint64_t utcMilliseconds)
	{
		if (utcMilliseconds == 0)
			return {};
		const std::time_t seconds = static_cast<std::time_t>(utcMilliseconds / 1000ULL);
		std::tm utc = {};
		if (gmtime_s(&utc, &seconds) != 0)
			return {};
		char formatted[40] = {};
		_snprintf_s(
			formatted,
			_TRUNCATE,
			"%04d-%02d-%02dT%02d:%02d:%02d.%03lluZ",
			utc.tm_year + 1900,
			utc.tm_mon + 1,
			utc.tm_mday,
			utc.tm_hour,
			utc.tm_min,
			utc.tm_sec,
			static_cast<unsigned long long>(utcMilliseconds % 1000ULL));
		return formatted;
	}

	std::filesystem::path EnvironmentDirectory(const wchar_t* variable)
	{
		if (variable == nullptr || variable[0] == L'\0')
			return {};
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetEnvironmentVariableW(
				variable,
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (required == 0 || required > 32768U)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data());
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	std::filesystem::path TemporaryDirectory()
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetTempPathW(
				static_cast<DWORD>(buffer.size()),
				buffer.data());
			if (required == 0 || required > 32768U)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data());
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	void AddUint64(
		rapidjson::Value& object,
		const char* key,
		std::uint64_t value,
		Allocator& allocator)
	{
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value number;
		number.SetUint64(value);
		object.AddMember(keyValue, number, allocator);
	}

	void AddSize(
		rapidjson::Value& object,
		const char* key,
		std::size_t value,
		Allocator& allocator)
	{
		AddUint64(object, key, static_cast<std::uint64_t>(value), allocator);
	}

	void AddDistribution(
		rapidjson::Value& timings,
		const char* name,
		const VsmrPerformance::Distribution& distribution,
		double latest,
		bool hasLatest,
		Allocator& allocator)
	{
		rapidjson::Value item(rapidjson::kObjectType);
		AddSize(item, "sampleCount", distribution.sampleCount, allocator);
		if (distribution.sampleCount > 0)
		{
			if (hasLatest)
				item.AddMember("latest", latest, allocator);
			item.AddMember("average", distribution.average, allocator);
			item.AddMember("median", distribution.median, allocator);
			item.AddMember("p95", distribution.p95, allocator);
			item.AddMember("max", distribution.maximum, allocator);
		}
		rapidjson::Value key;
		key.SetString(name, allocator);
		timings.AddMember(key, item, allocator);
	}

	std::vector<std::string> PerformanceRefreshReasonLabels(std::uint32_t mask)
	{
		std::vector<std::string> labels = VsmrPerformance::RefreshReasonNames(mask);
		if (labels.empty())
			labels.emplace_back("unspecified");
		return labels;
	}

	std::string JoinPerformanceRefreshReasons(std::uint32_t mask)
	{
		const std::vector<std::string> labels = PerformanceRefreshReasonLabels(mask);
		std::ostringstream output;
		for (std::size_t index = 0; index < labels.size(); ++index)
		{
			if (index != 0)
				output << " + ";
			output << labels[index];
		}
		return output.str();
	}

	void AddCacheItem(
		rapidjson::Value& caches,
		const char* id,
		const char* name,
		std::uint64_t exactHits,
		std::uint64_t previewHits,
		std::uint64_t misses,
		std::size_t entries,
		Allocator& allocator)
	{
		rapidjson::Value item(rapidjson::kObjectType);
		AddString(item, "id", id, allocator);
		AddString(item, "name", name, allocator);
		AddUint64(item, "hits", exactHits, allocator);
		AddUint64(item, "previewHits", previewHits, allocator);
		AddUint64(item, "misses", misses, allocator);
		const std::uint64_t accesses = exactHits + previewHits + misses;
		if (accesses > 0)
			item.AddMember(
				"hitRate",
				static_cast<double>(exactHits + previewHits) /
					static_cast<double>(accesses),
				allocator);
		else
		{
			rapidjson::Value nullValue;
			nullValue.SetNull();
			item.AddMember("hitRate", nullValue, allocator);
		}
		AddSize(item, "entries", entries, allocator);
		caches.PushBack(item, allocator);
	}

	std::string SerializePretty(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.SetIndent('\t', 1);
		value.Accept(writer);
		return std::string(buffer.GetString(), buffer.Size());
	}

	static std::uint64_t MonotonicToUtc(
		const VsmrPerformance::Snapshot& snapshot,
		std::uint64_t monotonicMilliseconds)
	{
		if (snapshot.collectionStartedUtcMilliseconds == 0 ||
			snapshot.collectionStartedMonotonicMilliseconds == 0)
		{
			return 0;
		}
		if (monotonicMilliseconds >= snapshot.collectionStartedMonotonicMilliseconds)
		{
			return snapshot.collectionStartedUtcMilliseconds +
				(monotonicMilliseconds - snapshot.collectionStartedMonotonicMilliseconds);
		}
		const std::uint64_t difference =
			snapshot.collectionStartedMonotonicMilliseconds - monotonicMilliseconds;
		return difference <= snapshot.collectionStartedUtcMilliseconds
			? snapshot.collectionStartedUtcMilliseconds - difference
			: 0;
	}

	void AddTargetSummary(
		rapidjson::Value& targets,
		const char* name,
		const VsmrPerformance::Snapshot& snapshot,
		bool visible,
		Allocator& allocator)
	{
		std::uint64_t total = 0;
		std::size_t maximum = 0;
		for (const VsmrPerformance::FrameSample& sample : snapshot.series)
		{
			const std::size_t value = visible
				? sample.visibleTargets
				: sample.processedTargets;
			total += static_cast<std::uint64_t>(value);
			maximum = (std::max)(maximum, value);
		}

		rapidjson::Value summary(rapidjson::kObjectType);
		if (!snapshot.series.empty())
		{
			const std::size_t latest = visible
				? snapshot.latestFrame.visibleTargets
				: snapshot.latestFrame.processedTargets;
			AddSize(summary, "latest", latest, allocator);
			summary.AddMember(
				"average",
				static_cast<double>(total) / static_cast<double>(snapshot.series.size()),
				allocator);
			AddSize(summary, "max", maximum, allocator);
		}
		rapidjson::Value key;
		key.SetString(name, allocator);
		targets.AddMember(key, summary, allocator);
	}
}

namespace VsmrControlCenterPerformance
{
	std::uint32_t NormalizeWindowSeconds(int requested) noexcept
	{
		switch (requested)
		{
		case 30:
		case 120:
		case 600:
			return static_cast<std::uint32_t>(requested);
		default:
			return 120;
		}
	}

	std::size_t NormalizeSeriesPoints(int requested) noexcept
	{
		if (requested <= 0)
			return 120;
		return static_cast<std::size_t>((std::clamp)(requested, 1, 600));
	}

	bool WriteReportAtomically(
		const std::string& reportJson,
		const std::filesystem::path& dataDirectory,
		std::string& reportPath,
		std::string& error)
	{
		reportPath.clear();
		error.clear();
		if (reportJson.empty())
		{
			error = "The performance report is empty.";
			return false;
		}

		const std::filesystem::path localAppData = EnvironmentDirectory(L"LOCALAPPDATA");
		const std::filesystem::path temporaryBase = TemporaryDirectory();
		const std::array<std::filesystem::path, 3> candidates = {
			dataDirectory.empty() ? std::filesystem::path{} : dataDirectory / L"Diagnostics",
			localAppData.empty()
				? std::filesystem::path{}
				: localAppData / L"vSMR" / L"Diagnostics",
			temporaryBase.empty()
				? std::filesystem::path{}
				: temporaryBase / L"vSMR" / L"Diagnostics"
		};
		const std::filesystem::path directory =
			VsmrCrashSupport::SelectFirstWritableDirectory(candidates);
		if (directory.empty())
		{
			error = "No writable performance diagnostics folder is available.";
			return false;
		}

		SYSTEMTIME utc = {};
		::GetSystemTime(&utc);
		char timestamp[40] = {};
		_snprintf_s(
			timestamp,
			_TRUNCATE,
			"%04u%02u%02u_%02u%02u%02u_%03u",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			utc.wMilliseconds);

		static volatile LONG temporarySequence = 0;
		const LONG sequence = ::InterlockedIncrement(&temporarySequence);
		const std::wstring temporaryName =
			L".vsmr-performance-" + std::to_wstring(::GetCurrentProcessId()) +
			L"-" + std::to_wstring(::GetTickCount64()) +
			L"-" + std::to_wstring(sequence) + L".tmp";
		const std::filesystem::path temporary = directory / temporaryName;
		const std::wstring nativeTemporary = VsmrCrashSupport::MakeNativePath(temporary);
		HANDLE output = ::CreateFileW(
			nativeTemporary.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (output == INVALID_HANDLE_VALUE)
		{
			error = "Unable to create the temporary performance report.";
			return false;
		}

		bool writeSucceeded = true;
		std::size_t writtenTotal = 0;
		while (writtenTotal < reportJson.size())
		{
			const DWORD requested = static_cast<DWORD>((std::min)(
				reportJson.size() - writtenTotal,
				static_cast<std::size_t>(1024U * 1024U)));
			DWORD written = 0;
			if (::WriteFile(
				output,
				reportJson.data() + writtenTotal,
				requested,
				&written,
				nullptr) == FALSE || written != requested)
			{
				writeSucceeded = false;
				break;
			}
			writtenTotal += written;
		}
		if (writeSucceeded)
			writeSucceeded = ::FlushFileBuffers(output) != FALSE;
		::CloseHandle(output);
		if (!writeSucceeded)
		{
			::DeleteFileW(nativeTemporary.c_str());
			error = "Unable to write the performance report.";
			return false;
		}

		for (unsigned int suffix = 0; suffix < 1000; ++suffix)
		{
			std::string filename = "vSMR_performance_" + std::string(timestamp);
			if (suffix > 0)
				filename += "_" + std::to_string(suffix + 1);
			filename += ".json";
			const std::filesystem::path target = directory / filename;
			const std::wstring nativeTarget = VsmrCrashSupport::MakeNativePath(target);
			if (::MoveFileExW(
				nativeTemporary.c_str(),
				nativeTarget.c_str(),
				MOVEFILE_WRITE_THROUGH) != FALSE)
			{
				reportPath = std::filesystem::path(
					VsmrCrashSupport::DisplayPath(nativeTarget)).u8string();
				return true;
			}
			const DWORD moveError = ::GetLastError();
			if (moveError != ERROR_FILE_EXISTS && moveError != ERROR_ALREADY_EXISTS)
				break;
		}

		::DeleteFileW(nativeTemporary.c_str());
		error = "Unable to finalize the performance report.";
		return false;
	}

	void BuildPayload(
		const VsmrPerformance::Snapshot& snapshot,
		const VsmrControlCenterPerformanceContext& context,
		std::size_t maximumSeriesPoints,
		VsmrControlCenterPerformancePeaks& peaks,
		rapidjson::Value& payload,
		Allocator& allocator)
	{
		payload.SetObject();
		payload.AddMember("schemaVersion", 1, allocator);
		payload.AddMember("available", true, allocator);
		AddString(
			payload,
			"generatedAtUtc",
			FormatUtcMilliseconds(VsmrPerformance::PerformanceDiagnostics::UtcMilliseconds()),
			allocator);
		// A diagnostics reset advances the generation. Reset the presentation peaks
		// with it so values from the previous sampling session are not retained.
		if (peaks.generation != snapshot.generation)
		{
			peaks.processGdiObjects = 0;
			peaks.cachedBitmaps = 0;
			peaks.estimatedBitmapBytes = 0;
			peaks.avisoPendingDepth = 0;
			peaks.generation = snapshot.generation;
		}

		rapidjson::Value source(rapidjson::kObjectType);
		AddString(source, "airport", context.airport, allocator);
		AddString(source, "profile", context.profile, allocator);
		AddString(source, "radarId", context.radarId, allocator);
		payload.AddMember("source", source, allocator);

		rapidjson::Value window(rapidjson::kObjectType);
		window.AddMember("seconds", snapshot.windowSeconds, allocator);
		AddSize(window, "samples", snapshot.frame.sampleCount, allocator);
		const std::uint64_t retainedFrames = (std::min)(
			snapshot.totalFrames,
			static_cast<std::uint64_t>(VsmrPerformance::MaximumFrameSamples));
		const std::uint64_t overwrittenFrames = snapshot.totalFrames - retainedFrames;
		AddUint64(window, "overwritten", overwrittenFrames, allocator);
		AddUint64(window, "dropped", overwrittenFrames, allocator);
		const std::uint64_t observedStart = snapshot.series.empty()
			? 0
			: snapshot.series.front().timestampMilliseconds;
		const std::uint64_t observedEnd = snapshot.series.empty()
			? 0
			: snapshot.series.back().timestampMilliseconds;
		window.AddMember(
			"observedSeconds",
			observedEnd >= observedStart && observedStart != 0
				? static_cast<double>(observedEnd - observedStart) / 1000.0
				: 0.0,
			allocator);
		AddString(
			window,
			"fromUtc",
			FormatUtcMilliseconds(MonotonicToUtc(snapshot, observedStart)),
			allocator);
		AddString(
			window,
			"toUtc",
			FormatUtcMilliseconds(MonotonicToUtc(snapshot, observedEnd)),
			allocator);
		payload.AddMember("window", window, allocator);

		rapidjson::Value timings(rapidjson::kObjectType);
		const bool hasLatest = snapshot.hasLatestFrame;
		AddDistribution(timings, "frame", snapshot.frame, snapshot.latestFrame.frameMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "scene", snapshot.scene, snapshot.latestFrame.sceneMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "aviso", snapshot.aviso, snapshot.latestFrame.avisoMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "targets", snapshot.targets, snapshot.latestFrame.targetsMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "rimcas", snapshot.rimcas, snapshot.latestFrame.rimcasMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "tags", snapshot.tags, snapshot.latestFrame.tagsMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "srw", snapshot.srw, snapshot.latestFrame.srwMilliseconds, hasLatest, allocator);
		AddDistribution(
			timings,
			"avisoInset",
			snapshot.avisoInset,
			snapshot.latestFrame.avisoInsetMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(timings, "rdf", snapshot.rdf, snapshot.latestFrame.rdfMilliseconds, hasLatest, allocator);
		AddDistribution(
			timings,
			"insetChrome",
			snapshot.insetChrome,
			snapshot.latestFrame.insetChromeMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneAvisoLoad",
			snapshot.sceneAvisoLoad,
			snapshot.latestFrame.sceneAvisoLoadMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneControllerCapture",
			snapshot.sceneControllerCapture,
			snapshot.latestFrame.sceneControllerCaptureMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneTargetCapture",
			snapshot.sceneTargetCapture,
			snapshot.latestFrame.sceneTargetCaptureMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneFinalize",
			snapshot.sceneFinalize,
			snapshot.latestFrame.sceneFinalizeMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"euroScopeLookups",
			snapshot.euroScopeLookups,
			snapshot.latestFrame.euroScopeLookupMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"avisoRasterRebuild",
			snapshot.avisoRasterRebuild,
			0.0,
			false,
			allocator);
		payload.AddMember("timings", timings, allocator);

		rapidjson::Value caches(rapidjson::kArrayType);
		AddCacheItem(
			caches,
			"aviso-main",
			"AVISO raster (main)",
			snapshot.mainAviso.exactHits,
			snapshot.mainAviso.previewHits,
			snapshot.mainAviso.misses,
			snapshot.resources.mainAvisoBitmapCount,
			allocator);
		AddCacheItem(
			caches,
			"aviso-insets",
			"AVISO raster (insets)",
			snapshot.insetAviso.exactHits,
			snapshot.insetAviso.previewHits,
			snapshot.insetAviso.misses,
			snapshot.resources.insetAvisoBitmapCount,
			allocator);
		AddCacheItem(
			caches,
			"aircraft-source",
			"Aircraft source bitmap",
			snapshot.aircraftSourceCache.hits,
			0,
			snapshot.aircraftSourceCache.misses,
			snapshot.aircraftSourceCache.entries,
			allocator);
		AddCacheItem(
			caches,
			"realistic-scaled",
			"Realistic icon scale",
			snapshot.realisticScaledCache.hits,
			0,
			snapshot.realisticScaledCache.misses,
			snapshot.realisticScaledCache.entries,
			allocator);
		AddCacheItem(
			caches,
			"realistic-rotated",
			"Realistic icon rotation",
			snapshot.realisticRotatedCache.hits,
			0,
			snapshot.realisticRotatedCache.misses,
			snapshot.realisticRotatedCache.entries,
			allocator);
		payload.AddMember("caches", caches, allocator);

		rapidjson::Value targets(rapidjson::kObjectType);
		AddTargetSummary(targets, "processed", snapshot, false, allocator);
		AddTargetSummary(targets, "visible", snapshot, true, allocator);
		payload.AddMember("targets", targets, allocator);

		rapidjson::Value refresh(rapidjson::kObjectType);
		if (snapshot.hasLatestFrame)
		{
			AddString(
				refresh,
				"latestReason",
				JoinPerformanceRefreshReasons(snapshot.latestFrame.refreshReasonMask),
				allocator);
			rapidjson::Value latestReasons(rapidjson::kArrayType);
			for (const std::string& label : PerformanceRefreshReasonLabels(snapshot.latestFrame.refreshReasonMask))
			{
				rapidjson::Value value;
				value.SetString(label.c_str(), static_cast<rapidjson::SizeType>(label.size()), allocator);
				latestReasons.PushBack(value, allocator);
			}
			refresh.AddMember("latestReasons", latestReasons, allocator);
		}
		rapidjson::Value reasonCounts(rapidjson::kArrayType);
		auto addRefreshReasonCount = [&](const char* id, const char* label, std::uint64_t count)
		{
			if (count == 0)
				return;
			rapidjson::Value item(rapidjson::kObjectType);
			AddString(item, "id", id, allocator);
			AddString(item, "reason", label, allocator);
			AddUint64(item, "count", count, allocator);
			reasonCounts.PushBack(item, allocator);
		};
		addRefreshReasonCount("unspecified", "Unspecified", snapshot.refresh.reasons.unspecified);
		addRefreshReasonCount("initial", "Initial frame", snapshot.refresh.reasons.initial);
		addRefreshReasonCount("mainViewChanged", "Main view changed", snapshot.refresh.reasons.mainViewChanged);
		addRefreshReasonCount("insetPanZoom", "Inset pan or zoom", snapshot.refresh.reasons.insetPanZoom);
		addRefreshReasonCount("insetMoveResize", "Inset move or resize", snapshot.refresh.reasons.insetMoveResize);
		addRefreshReasonCount("hover", "Hover", snapshot.refresh.reasons.hover);
		addRefreshReasonCount(
			"targetOrFlightPlanUpdate",
			"Target or flight-plan update",
			snapshot.refresh.reasons.targetOrFlightPlanUpdate);
		addRefreshReasonCount("controllerUpdate", "Controller update", snapshot.refresh.reasons.controllerUpdate);
		addRefreshReasonCount("profileUpdate", "Profile update", snapshot.refresh.reasons.profileUpdate);
		addRefreshReasonCount("airportUpdate", "Airport update", snapshot.refresh.reasons.airportUpdate);
		addRefreshReasonCount("avisoWorkerUpdate", "AVISO worker update", snapshot.refresh.reasons.avisoWorkerUpdate);
		addRefreshReasonCount("userActionExternal", "User or external action", snapshot.refresh.reasons.userActionExternal);
		addRefreshReasonCount("avisoDataChanged", "AVISO data changed", snapshot.refresh.reasons.avisoDataChanged);
		refresh.AddMember("reasonCounts", reasonCounts, allocator);
		AddString(refresh, "reasonScope", "selectedRetainedFrameWindow", allocator);
		refresh.AddMember("reasonCountsMayOverlap", true, allocator);
		refresh.AddMember("spikeThresholdMilliseconds", snapshot.refresh.spikeThresholdMilliseconds, allocator);
		AddString(refresh, "spikeComparison", ">=", allocator);
		AddUint64(refresh, "spikeCount", snapshot.refresh.spikeCount, allocator);
		auto addSpike = [&](const char* name, bool present, const VsmrPerformance::FrameSample& sample)
		{
			if (!present)
				return;
			rapidjson::Value spike(rapidjson::kObjectType);
			AddUint64(spike, "frameId", sample.frameId, allocator);
			spike.AddMember("frameMilliseconds", sample.frameMilliseconds, allocator);
			AddUint64(
				spike,
				"ageMilliseconds",
				observedEnd >= sample.timestampMilliseconds
					? observedEnd - sample.timestampMilliseconds
					: 0,
				allocator);
			AddString(spike, "reason", JoinPerformanceRefreshReasons(sample.refreshReasonMask), allocator);
			AddString(spike, "primaryReason", VsmrPerformance::PrimaryRefreshReasonName(sample.refreshReasonMask), allocator);
			AddSize(spike, "processedTargets", sample.processedTargets, allocator);
			AddSize(spike, "visibleTargets", sample.visibleTargets, allocator);
			spike.AddMember("avisoMilliseconds", sample.avisoMilliseconds, allocator);
			spike.AddMember("avisoInsetMilliseconds", sample.avisoInsetMilliseconds, allocator);

			const std::pair<const char*, double> stages[] = {
				{ "Scene capture", sample.sceneMilliseconds },
				{ "Main AVISO", sample.avisoMilliseconds },
				{ "AVISO inset", sample.avisoInsetMilliseconds },
				{ "Targets", sample.targetsMilliseconds },
				{ "RIMCAS", sample.rimcasMilliseconds },
				{ "Tags", sample.tagsMilliseconds },
				{ "SRW", sample.srwMilliseconds },
				{ "RDF", sample.rdfMilliseconds },
				{ "Inset chrome", sample.insetChromeMilliseconds }
			};
			const std::pair<const char*, double>* dominantStage = &stages[0];
			for (const auto& stage : stages)
			{
				if (stage.second > dominantStage->second)
					dominantStage = &stage;
			}
			std::ostringstream context;
			context.imbue(std::locale::classic());
			context << "Largest measured slice: " << dominantStage->first << " "
				<< std::fixed << std::setprecision(2) << dominantStage->second << " ms";
			AddString(spike, "context", context.str(), allocator);
			rapidjson::Value key;
			key.SetString(name, allocator);
			refresh.AddMember(key, spike, allocator);
		};
		addSpike("worstSpike", snapshot.refresh.hasWorstSpike, snapshot.refresh.worstSpike);
		addSpike("latestSpike", snapshot.refresh.hasLatestSpike, snapshot.refresh.latestSpike);
		payload.AddMember("refresh", refresh, allocator);

		peaks.processGdiObjects = (std::max)(
			peaks.processGdiObjects,
			snapshot.resources.processGdiObjects);
		peaks.cachedBitmaps = (std::max)(
			peaks.cachedBitmaps,
			snapshot.resources.ownedBitmapCount);
		peaks.estimatedBitmapBytes = (std::max)(
			peaks.estimatedBitmapBytes,
			snapshot.resources.estimatedBitmapBytes);
		rapidjson::Value graphics(rapidjson::kObjectType);
		graphics.AddMember("processGdiObjects", snapshot.resources.processGdiObjects, allocator);
		AddSize(graphics, "vsmrCachedBitmaps", snapshot.resources.ownedBitmapCount, allocator);
		graphics.AddMember("peakProcessGdiObjects", peaks.processGdiObjects, allocator);
		AddSize(graphics, "peakVsmrCachedBitmaps", peaks.cachedBitmaps, allocator);
		AddUint64(graphics, "estimatedBitmapBytes", snapshot.resources.estimatedBitmapBytes, allocator);
		AddUint64(graphics, "peakEstimatedBitmapBytes", peaks.estimatedBitmapBytes, allocator);
		AddSize(graphics, "aircraftBitmapCount", snapshot.resources.aircraftBitmapCount, allocator);
		AddSize(graphics, "realisticIconBitmapCount", snapshot.resources.realisticIconBitmapCount, allocator);
		AddSize(graphics, "mainAvisoBitmapCount", snapshot.resources.mainAvisoBitmapCount, allocator);
		AddSize(graphics, "insetAvisoBitmapCount", snapshot.resources.insetAvisoBitmapCount, allocator);
		payload.AddMember("graphics", graphics, allocator);

		const std::size_t avisoPendingDepth =
			snapshot.mainAviso.queue.pending + snapshot.insetAviso.queue.pending;
		const std::size_t avisoInFlight =
			snapshot.mainAviso.queue.inFlight + snapshot.insetAviso.queue.inFlight;
		const std::size_t avisoCompleted =
			snapshot.mainAviso.queue.completed + snapshot.insetAviso.queue.completed;
		peaks.avisoPendingDepth = (std::max)(peaks.avisoPendingDepth, avisoPendingDepth);
		const VsmrControlCenterWorkerQueues& pluginQueues = context.workerQueues;
		rapidjson::Value worker(rapidjson::kObjectType);
		worker.AddMember("active", avisoInFlight > 0, allocator);
		AddSize(worker, "pendingDepth", avisoPendingDepth, allocator);
		AddSize(worker, "inFlight", avisoInFlight, allocator);
		AddSize(worker, "maxDepth", peaks.avisoPendingDepth, allocator);
		AddUint64(
			worker,
			"supersededRequests",
			snapshot.mainAviso.requestsSuperseded + snapshot.insetAviso.requestsSuperseded,
			allocator);
		rapidjson::Value queues(rapidjson::kArrayType);
		auto addQueue = [&](const char* name, bool active, std::size_t pending, std::size_t inFlight, std::size_t workers, std::size_t completed)
		{
			rapidjson::Value queue(rapidjson::kObjectType);
			AddString(queue, "name", name, allocator);
			queue.AddMember("active", active, allocator);
			AddSize(queue, "pendingDepth", pending, allocator);
			AddSize(queue, "inFlight", inFlight, allocator);
			AddSize(queue, "workers", workers, allocator);
			AddSize(queue, "completedWaiting", completed, allocator);
			queues.PushBack(queue, allocator);
		};
		addQueue(
			"AVISO",
			avisoInFlight > 0,
			avisoPendingDepth,
			avisoInFlight,
			snapshot.mainAviso.queue.workers + snapshot.insetAviso.queue.workers,
			avisoCompleted);
		addQueue(
			"Network",
			pluginQueues.networkInFlight > 0,
			pluginQueues.networkQueued,
			pluginQueues.networkInFlight,
			pluginQueues.networkWorkers,
			0);
		addQueue(
			"Weather",
			pluginQueues.weatherInFlight > 0,
			pluginQueues.weatherQueued,
			pluginQueues.weatherInFlight,
			pluginQueues.weatherWorkerRunning ? 1U : 0U,
			0);
		worker.AddMember("queues", queues, allocator);
		payload.AddMember("worker", worker, allocator);

		rapidjson::Value aviso(rapidjson::kObjectType);
		AddUint64(
			aviso,
			"framesDelayed",
			snapshot.avisoDelayedFrames,
			allocator);
		AddUint64(
			aviso,
			"framesUsingFallback",
			snapshot.avisoFallbackFrames,
			allocator);
		const std::uint64_t totalBuilds =
			snapshot.mainAviso.rasterBuilds + snapshot.insetAviso.rasterBuilds;
		const std::uint64_t failedBuilds =
			snapshot.mainAviso.rasterBuildFailures + snapshot.insetAviso.rasterBuildFailures;
		AddUint64(
			aviso,
			"rebuildsCompleted",
			totalBuilds >= failedBuilds ? totalBuilds - failedBuilds : 0,
			allocator);
		AddUint64(
			aviso,
			"blankDelayedFrames",
			snapshot.avisoBlankDelayedFrames,
			allocator);
		AddUint64(
			aviso,
			"requestsQueued",
			snapshot.mainAviso.requestsQueued + snapshot.insetAviso.requestsQueued,
			allocator);
		AddUint64(
			aviso,
			"requestsCoalesced",
			snapshot.mainAviso.requestsCoalesced + snapshot.insetAviso.requestsCoalesced,
			allocator);
		AddUint64(
			aviso,
			"requestsSuperseded",
			snapshot.mainAviso.requestsSuperseded + snapshot.insetAviso.requestsSuperseded,
			allocator);
		AddUint64(
			aviso,
			"requestsDebounced",
			snapshot.mainAviso.requestsDebounced + snapshot.insetAviso.requestsDebounced,
			allocator);
		AddUint64(
			aviso,
			"rasterBuilds",
			totalBuilds,
			allocator);
		AddUint64(
			aviso,
			"rasterBuildFailures",
			failedBuilds,
			allocator);
		AddUint64(
			aviso,
			"rasterBuildsCancelled",
			snapshot.mainAviso.rasterBuildsCancelled + snapshot.insetAviso.rasterBuildsCancelled,
			allocator);
		AddUint64(
			aviso,
			"resultsApplied",
			snapshot.mainAviso.resultsApplied + snapshot.insetAviso.resultsApplied,
			allocator);
		AddUint64(
			aviso,
			"resultsDiscarded",
			snapshot.mainAviso.resultsDiscarded + snapshot.insetAviso.resultsDiscarded,
			allocator);
		auto addAvisoViewport = [&](const char* name, const VsmrPerformance::AvisoSnapshot& source)
		{
			rapidjson::Value viewport(rapidjson::kObjectType);
			AddUint64(viewport, "requestsQueued", source.requestsQueued, allocator);
			AddUint64(viewport, "requestsCoalesced", source.requestsCoalesced, allocator);
			AddUint64(viewport, "requestsSuperseded", source.requestsSuperseded, allocator);
			AddUint64(viewport, "requestsDebounced", source.requestsDebounced, allocator);
			AddUint64(viewport, "rasterBuilds", source.rasterBuilds, allocator);
			AddUint64(viewport, "rasterBuildFailures", source.rasterBuildFailures, allocator);
			AddUint64(viewport, "rasterBuildsCancelled", source.rasterBuildsCancelled, allocator);
			AddUint64(viewport, "resultsApplied", source.resultsApplied, allocator);
			AddUint64(viewport, "resultsDiscarded", source.resultsDiscarded, allocator);
			rapidjson::Value key;
			key.SetString(name, allocator);
			aviso.AddMember(key, viewport, allocator);
		};
		addAvisoViewport("main", snapshot.mainAviso);
		addAvisoViewport("inset", snapshot.insetAviso);
		payload.AddMember("aviso", aviso, allocator);

		rapidjson::Value series(rapidjson::kArrayType);
		const std::size_t availableSeries = snapshot.series.size();
		const std::size_t wantedSeries = (std::min)(availableSeries, maximumSeriesPoints);
		for (std::size_t index = 0; index < wantedSeries; ++index)
		{
			const std::size_t sourceIndex = wantedSeries <= 1
				? availableSeries - 1
				: (index * (availableSeries - 1)) / (wantedSeries - 1);
			const VsmrPerformance::FrameSample& sample = snapshot.series[sourceIndex];
			rapidjson::Value point(rapidjson::kObjectType);
			AddUint64(
				point,
				"offsetMs",
				sample.timestampMilliseconds >= observedStart
					? sample.timestampMilliseconds - observedStart
					: 0,
				allocator);
			point.AddMember("frameMs", sample.frameMilliseconds, allocator);
			point.AddMember("avisoMs", sample.avisoMilliseconds, allocator);
			point.AddMember("avisoInsetMs", sample.avisoInsetMilliseconds, allocator);
			point.AddMember("sceneMs", sample.sceneMilliseconds, allocator);
			point.AddMember("srwMs", sample.srwMilliseconds, allocator);
			point.AddMember("rdfMs", sample.rdfMilliseconds, allocator);
			point.AddMember("insetChromeMs", sample.insetChromeMilliseconds, allocator);
			point.AddMember("refreshReasonMask", sample.refreshReasonMask, allocator);
			series.PushBack(point, allocator);
		}
		payload.AddMember("series", series, allocator);
	}

	bool AddWorkerQueuesToReport(
		const std::string& nativeReport,
		const VsmrControlCenterWorkerQueues& queues,
		std::string& report,
		std::string& error)
	{
		report.clear();
		error.clear();
		rapidjson::Document document;
		document.Parse<0>(nativeReport.c_str());
		if (document.HasParseError() || !document.IsObject())
		{
			error = "The native performance report could not be serialized.";
			return false;
		}

		Allocator& allocator = document.GetAllocator();
		if (!document.HasMember("type"))
			AddString(document, "type", "vSMR.performance-report", allocator);
		rapidjson::Value workerQueues(rapidjson::kObjectType);
		rapidjson::Value network(rapidjson::kObjectType);
		AddSize(network, "workers", queues.networkWorkers, allocator);
		AddSize(network, "queued", queues.networkQueued, allocator);
		AddSize(network, "inFlight", queues.networkInFlight, allocator);
		workerQueues.AddMember("network", network, allocator);
		rapidjson::Value weather(rapidjson::kObjectType);
		weather.AddMember("workerRunning", queues.weatherWorkerRunning, allocator);
		AddSize(weather, "queued", queues.weatherQueued, allocator);
		AddSize(weather, "inFlight", queues.weatherInFlight, allocator);
		workerQueues.AddMember("weather", weather, allocator);
		if (document.HasMember("workerQueues"))
			document.RemoveMember("workerQueues");
		document.AddMember("workerQueues", workerQueues, allocator);
		report = SerializePretty(document);
		return !report.empty();
	}
}
