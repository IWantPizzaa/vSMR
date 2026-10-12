#include "PerformanceDiagnosticsTests.hpp"
#include "diagnostics/PerformanceDiagnostics.hpp"
#include "rapidjson/document.h"

#include <cmath>
#include <limits>
#include <memory>

std::vector<std::string> RunPerformanceDiagnosticsTests()
{
	using namespace VsmrPerformance;
	std::vector<std::string> failures;
	auto expect = [&](bool condition, const char* message)
	{
		if (!condition) failures.emplace_back(message);
	};
	auto snapshot = [](const PerformanceDiagnostics& diagnostics, std::uint32_t window = 0)
	{
		return diagnostics.GetSnapshot(window, MaximumFrameSamples, {}, {}, 0, 0, 0);
	};
	auto record = [](PerformanceDiagnostics& diagnostics, double start, double work = 1.0)
	{
		FrameSample sample;
		sample.callbackStartMilliseconds = start;
		sample.timestampMilliseconds = static_cast<std::uint64_t>(start);
		sample.frameMilliseconds = work;
		diagnostics.BeginFrame();
		diagnostics.RecordFrame(sample);
	};
	auto near = [](double actual, double expected)
	{
		return std::abs(actual - expected) < 1.0e-8;
	};

	auto fractional = std::make_unique<PerformanceDiagnostics>();
	record(*fractional, 1000.0);
	record(*fractional, 1000.25);
	record(*fractional, 1000.75);
	const auto fractionalResult = snapshot(*fractional);
	expect(fractionalResult.callbackInterval.sampleCount == 2 &&
		near(fractionalResult.callbackInterval.average, 0.375) &&
		near(fractionalResult.callbackInterval.p99, 0.5),
		"Callback cadence retains fractional milliseconds and excludes first observation");
	expect(near(fractionalResult.callbackCadenceHz, 1000.0 / 0.375) &&
		near(fractionalResult.callbackCadenceOnePercentLowHz, 2000.0),
		"Callback cadence uses entry gaps rather than plugin work durations");

	auto distribution = std::make_unique<PerformanceDiagnostics>();
	double start = 1000.0;
	record(*distribution, start, 1.0);
	for (int interval = 1; interval <= 200; ++interval)
	{
		start += interval;
		record(*distribution, start, static_cast<double>(interval));
	}
	const auto result = snapshot(*distribution);
	expect(result.callbackInterval.sampleCount == 200 && near(result.callbackInterval.p95, 190.0) &&
		near(result.callbackInterval.p99, 198.0) && near(result.frame.p99, 198.0),
		"Diagnostics calculate nearest-rank p99 from complete retained samples");
	expect(near(result.callbackCadenceOnePercentLowHz, 1000.0 / 199.5),
		"One percent low uses mean of slowest ceil(N / 100) intervals");

	rapidjson::Document report;
	const std::string json = BuildJsonReport(result, "test", "TEST", "profile");
	report.Parse(json.c_str());
	expect(!report.HasParseError() && report.IsObject() && report.HasMember("callbackCadenceDefinition") &&
		report["timings"]["callbackInterval"]["p99"].GetDouble() == 198.0 &&
		report["frames"][1]["callbackIntervalMilliseconds"].GetDouble() == 1.0 &&
		json.find("not display FPS") != std::string::npos,
		"Performance report exports p99 and explicitly labels event-driven callback cadence");

	auto windowed = std::make_unique<PerformanceDiagnostics>();
	const double now = PerformanceDiagnostics::HighResolutionMonotonicMilliseconds();
	record(*windowed, now - 1500.0);
	record(*windowed, now - 500.0);
	record(*windowed, now - 400.0);
	const auto windowResult = snapshot(*windowed, 1);
	expect(windowResult.frame.sampleCount == 2 && windowResult.callbackInterval.sampleCount == 1 &&
		near(windowResult.callbackInterval.average, 100.0),
		"Cadence window excludes intervals beginning before selected window");

	fractional->Reset();
	record(*fractional, 2000.0);
	expect(snapshot(*fractional).callbackInterval.sampleCount == 0,
		"Diagnostics reset clears previous callback entry timestamp");
	FrameSample invalid;
	invalid.callbackStartMilliseconds = std::numeric_limits<double>::quiet_NaN();
	invalid.callbackIntervalMilliseconds = 42.0;
	fractional->RecordFrame(invalid);
	expect(snapshot(*fractional).callbackInterval.sampleCount == 0,
		"Missing or invalid callback timestamp cannot inject a cadence interval");

	auto bounded = std::make_unique<PerformanceDiagnostics>();
	for (std::size_t i = 0; i < MaximumFrameSamples + 20; ++i)
		record(*bounded, 1000.0 + static_cast<double>(i));
	const auto boundedResult = snapshot(*bounded);
	expect(boundedResult.frame.sampleCount == MaximumFrameSamples &&
		boundedResult.callbackInterval.sampleCount == MaximumFrameSamples &&
		near(boundedResult.callbackInterval.average, 1.0),
		"Cadence remains bounded and preserves intervals across ring rollover");
	return failures;
}
