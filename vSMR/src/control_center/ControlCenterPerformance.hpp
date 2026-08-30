#pragma once

#include "diagnostics/PerformanceDiagnostics.hpp"

#include "rapidjson/document.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

struct VsmrControlCenterWorkerQueues
{
	std::size_t networkWorkers = 0;
	std::size_t networkQueued = 0;
	std::size_t networkInFlight = 0;
	bool weatherWorkerRunning = false;
	std::size_t weatherQueued = 0;
	std::size_t weatherInFlight = 0;
};

struct VsmrControlCenterPerformanceContext
{
	std::string radarId;
	std::string airport;
	std::string profile;
	VsmrControlCenterWorkerQueues workerQueues;
};

// Peaks belong to one Control Center bridge. Sharing them between radar
// windows would make a reset in one window change the report shown by another.
struct VsmrControlCenterPerformancePeaks
{
	std::uint32_t processGdiObjects = 0;
	std::size_t cachedBitmaps = 0;
	std::uint64_t estimatedBitmapBytes = 0;
	std::size_t avisoPendingDepth = 0;
	std::uint64_t generation = 0;

	void Reset() noexcept
	{
		*this = {};
	}
};

namespace VsmrControlCenterPerformance
{
	using Allocator = rapidjson::Document::AllocatorType;

	std::uint32_t NormalizeWindowSeconds(int requested) noexcept;
	std::size_t NormalizeSeriesPoints(int requested) noexcept;

	void BuildPayload(
		const VsmrPerformance::Snapshot& snapshot,
		const VsmrControlCenterPerformanceContext& context,
		std::size_t maximumSeriesPoints,
		VsmrControlCenterPerformancePeaks& peaks,
		rapidjson::Value& payload,
		Allocator& allocator);

	bool AddWorkerQueuesToReport(
		const std::string& nativeReport,
		const VsmrControlCenterWorkerQueues& queues,
		std::string& report,
		std::string& error);

	bool WriteReportAtomically(
		const std::string& reportJson,
		const std::filesystem::path& dataDirectory,
		std::string& reportPath,
		std::string& error);
}
