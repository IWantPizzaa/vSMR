#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>
#include "BenchmarkFixture.hpp"
#include "aviso/AvisoRasterRenderer.hpp"
#include "aviso/AvisoRenderScene.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace Gdiplus;
using namespace VsmrRadarTypes;
using namespace VsmrAvisoBenchmark;
namespace fs = std::filesystem;

namespace
{
	using Clock = std::chrono::steady_clock;
	double Elapsed(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	struct BitmapView
	{
		DIBSECTION dib{};
		explicit BitmapView(HBITMAP bitmap)
		{
			if (GetObject(bitmap, sizeof(dib), &dib) != sizeof(dib) || !dib.dsBm.bmBits)
				throw std::runtime_error("Expected a 32-bit DIB section");
		}
		BYTE* Pixels() const { return static_cast<BYTE*>(dib.dsBm.bmBits); }
	};

	struct Difference
	{
		std::uint64_t pixels = 0;
		int maxChannel = 0;
		double meanChannel = 0;
	};

	Difference Compare(HBITMAP first, HBITMAP second, bool ignoreAlpha = false)
	{
		GdiFlush();
		const BitmapView left(first), right(second);
		if (left.dib.dsBm.bmWidth != right.dib.dsBm.bmWidth || left.dib.dsBm.bmHeight != right.dib.dsBm.bmHeight)
			throw std::runtime_error("Image dimensions differ");
		Difference result;
		std::uint64_t total = 0;
		const int channels = ignoreAlpha ? 3 : 4;
		const size_t pixelCount = static_cast<size_t>(left.dib.dsBm.bmWidth) * left.dib.dsBm.bmHeight;
		for (size_t p = 0; p < pixelCount; ++p)
		{
			bool changed = false;
			for (int c = 0; c < channels; ++c)
			{
				const size_t offset = p * 4 + static_cast<size_t>(c);
				const int delta = std::abs(static_cast<int>(left.Pixels()[offset]) - right.Pixels()[offset]);
				result.maxChannel = (std::max)(result.maxChannel, delta);
				total += static_cast<std::uint64_t>(delta);
				changed |= delta != 0;
			}
			result.pixels += changed ? 1 : 0;
		}
		result.meanChannel = static_cast<double>(total) / (static_cast<double>(pixelCount) * channels);
		return result;
	}

	void SaveImage(HBITMAP handle, const fs::path& path)
	{
		const BitmapView view(handle);
		Bitmap bitmap(view.dib.dsBm.bmWidth, view.dib.dsBm.bmHeight, view.dib.dsBm.bmWidthBytes,
			PixelFormat32bppPARGB, view.Pixels());
		UINT count = 0, bytes = 0;
		GetImageEncodersSize(&count, &bytes);
		std::vector<BYTE> storage(bytes);
		auto* encoders = reinterpret_cast<ImageCodecInfo*>(storage.data());
		GetImageEncoders(count, bytes, encoders);
		for (UINT i = 0; i < count; ++i)
			if (std::wstring(encoders[i].MimeType) == L"image/png")
			{
				if (bitmap.Save(path.c_str(), &encoders[i].Clsid) != Ok)
					throw std::runtime_error("Could not save image");
				return;
			}
		throw std::runtime_error("No PNG encoder");
	}

	struct Surface
	{
		HDC dc = nullptr;
		HBITMAP bitmap = nullptr;
		HGDIOBJ previous = nullptr;
		std::unique_ptr<Graphics> graphics;
		Surface(int width, int height)
		{
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
			void* bits = nullptr;
			bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			dc = CreateCompatibleDC(nullptr);
			if (!bitmap || !dc) throw std::runtime_error("Cannot create benchmark target");
			previous = SelectObject(dc, bitmap);
			graphics = std::make_unique<Graphics>(dc);
		}
		~Surface()
		{
			graphics.reset();
			if (dc) { SelectObject(dc, previous); DeleteDC(dc); }
			if (bitmap) DeleteObject(bitmap);
		}
		void Clear(Color color)
		{
			if (graphics->Clear(color) != Ok) throw std::runtime_error("Cannot clear target");
		}
	};

	struct Statistics
	{
		double mean = 0, median = 0, p95 = 0, maximum = 0;
		explicit Statistics(std::vector<double> samples)
		{
			if (samples.empty()) return;
			mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
			std::sort(samples.begin(), samples.end());
			median = samples[samples.size() / 2];
			p95 = samples[static_cast<size_t>(std::ceil(0.95 * samples.size())) - 1];
			maximum = samples.back();
		}
	};

	AvisoRasterRenderRequest MakeRequest(const Fixture& fixture, const fs::path& path, double zoom, double rotation)
	{
		AvisoRasterRenderRequest request;
		request.path = path.u8string();
		request.features = fixture.features; request.labels = fixture.labels; request.groupVisibility = fixture.visibility;
		request.rasterWidth = 1920; request.rasterHeight = 1080;
		request.viewportZoomLevel = zoom > 1 ? 14 : 8;
		double centerLon = (fixture.minLongitude + fixture.maxLongitude) * 0.5;
		double centerLat = (fixture.minLatitude + fixture.maxLatitude) * 0.5;
		if (zoom > 1 && !fixture.labels->empty())
		{
			// Some perimeter polygons span far outside the detailed airport. A
			// label median chooses a reproducible occupied detail viewport.
			std::vector<double> longitudes, latitudes;
			for (const auto& label : *fixture.labels)
			{
				longitudes.push_back(label.position.longitude); latitudes.push_back(label.position.latitude);
			}
			std::sort(longitudes.begin(), longitudes.end()); std::sort(latitudes.begin(), latitudes.end());
			centerLon = longitudes[longitudes.size() / 2]; centerLat = latitudes[latitudes.size() / 2];
		}
		const double halfLon = (fixture.maxLongitude - fixture.minLongitude) * 0.55 / zoom;
		const double halfLat = (fixture.maxLatitude - fixture.minLatitude) * 0.55 / zoom;
		request.displayMinLongitude = request.renderMinLongitude = centerLon - halfLon;
		request.displayMaxLongitude = request.renderMaxLongitude = centerLon + halfLon;
		request.displayMinLatitude = request.renderMinLatitude = centerLat - halfLat;
		request.displayMaxLatitude = request.renderMaxLatitude = centerLat + halfLat;
		request.scaleX = request.rasterWidth / (2 * halfLon); request.scaleY = request.rasterHeight / (2 * halfLat);
		const double angle = rotation * 3.14159265358979323846 / 180;
		auto rotate = [&](double x, double y) {
			const double dx = x - 960, dy = y - 540;
			return PointF(static_cast<REAL>(960 + dx * std::cos(angle) - dy * std::sin(angle)),
				static_cast<REAL>(540 + dx * std::sin(angle) + dy * std::cos(angle)));
		};
		request.projectedTopLeft = rotate(0, 0); request.projectedTopRight = rotate(1920, 0);
		request.projectedBottomLeft = rotate(0, 1080); request.projectedBottomRight = rotate(1920, 1080);
		return request;
	}

	void Require(const std::unique_ptr<AvisoRasterRenderResult>& result)
	{
		if (!result || !result->bitmap) throw std::runtime_error("Renderer returned no raster");
	}

	template <typename Render>
	double Measure(Render&& render)
	{
		const auto start = Clock::now();
		auto result = render();
		Require(result);
		return Elapsed(start); // Bitmap destruction deliberately follows the timed rebuild.
	}

	void WriteRow(std::ostream& csv, const std::string& scene, const char* kind, const std::vector<double>& before,
		const std::vector<double>& after, const Difference& diff, double cold, double sceneBuild)
	{
		const Statistics a(before), b(after);
		csv << scene << ',' << kind << ',' << before.size() << ',' << a.mean << ',' << b.mean << ','
			<< a.median << ',' << b.median << ',' << a.p95 << ',' << b.p95 << ',' << a.maximum << ',' << b.maximum << ','
			<< (b.mean ? a.mean / b.mean : 0) << ',' << diff.pixels << ',' << diff.maxChannel << ',' << diff.meanChannel << ','
			<< cold << ',' << sceneBuild << '\n';
		std::cout << scene << ' ' << kind << ": " << std::fixed << std::setprecision(3) << a.mean << " -> " << b.mean
			<< " ms, " << std::setprecision(2) << (b.mean ? a.mean / b.mean : 0) << "x; differing pixels=" << diff.pixels
			<< ", max channel=" << diff.maxChannel << std::endl;
	}

	bool RunScene(const Fixture& fixture, const fs::path& path, const fs::path& output, const std::string& name,
		AvisoRasterRenderRequest request, int samples, std::ostream& csv)
	{
		const auto sceneStart = Clock::now();
		request.scene = VsmrAviso::AvisoRenderScene::Build(*request.features, *request.labels);
		const double sceneBuild = Elapsed(sceneStart);
		VsmrAviso::AvisoRasterRenderer optimized;
		const double cold = Measure([&]() { return optimized.Render(request); });
		auto original = RenderBaseline(request);
		auto changed = optimized.Render(request);
		Require(original); Require(changed);
		const Difference diff = Compare(original->bitmap, changed->bitmap);
		const auto counters = optimized.LastStats();
		std::cout << "  candidates=" << counters.featureCandidates << '/' << request.features->size() << ", labels="
			<< counters.labelCandidates << '/' << request.labels->size() << ", projected vertices=" << counters.projectedVertices
			<< ", culled paths=" << counters.pathsCulled << ", glyph hits/misses=" << counters.glyphCacheHits << '/' << counters.glyphCacheMisses << std::endl;
		if (diff.pixels != 0 || name == "LFPG_whole_dark")
		{
			SaveImage(original->bitmap, output / (name + "_baseline.png"));
			SaveImage(changed->bitmap, output / (name + "_optimized.png"));
		}
		std::vector<double> before, after;
		for (int warm = 0; warm < 3; ++warm)
		{
			Measure([&]() { return RenderBaseline(request); });
			Measure([&]() { return optimized.Render(request); });
		}
		for (int i = 0; i < samples; ++i)
		{
			// Alternate order to reduce thermal/scheduling bias.
			if (i % 2 == 0)
			{
				before.push_back(Measure([&]() { return RenderBaseline(request); }));
				after.push_back(Measure([&]() { return optimized.Render(request); }));
			}
			else
			{
				after.push_back(Measure([&]() { return optimized.Render(request); }));
				before.push_back(Measure([&]() { return RenderBaseline(request); }));
			}
		}
		WriteRow(csv, name, "rebuild", before, after, diff, cold, sceneBuild);
		bool faithful = diff.pixels == 0;

		// Replay pan/zoom against a warmed renderer so glyph-cache reuse is tested
		// across changing positions, not only identical rebuilds.
		before.clear(); after.clear();
		Difference replayDiff;
		for (int i = 0; i < 12; ++i)
		{
			auto replay = request;
			const double lonSpan = request.displayMaxLongitude - request.displayMinLongitude;
			const double latSpan = request.displayMaxLatitude - request.displayMinLatitude;
			const double offsetLon = lonSpan * (static_cast<double>(i) - 5.5) * 0.0043;
			const double offsetLat = latSpan * std::sin(static_cast<double>(i) * 0.7) * 0.019;
			const double zoomAmount = i % 3 == 0 ? 0.025 : 0;
			replay.displayMinLongitude = replay.renderMinLongitude = request.displayMinLongitude + offsetLon + lonSpan * zoomAmount;
			replay.displayMaxLongitude = replay.renderMaxLongitude = request.displayMaxLongitude + offsetLon - lonSpan * zoomAmount;
			replay.displayMinLatitude = replay.renderMinLatitude = request.displayMinLatitude + offsetLat + latSpan * zoomAmount;
			replay.displayMaxLatitude = replay.renderMaxLatitude = request.displayMaxLatitude + offsetLat - latSpan * zoomAmount;
			replay.scaleX = replay.rasterWidth / (replay.displayMaxLongitude - replay.displayMinLongitude);
			replay.scaleY = replay.rasterHeight / (replay.displayMaxLatitude - replay.displayMinLatitude);
			auto begin = Clock::now(); auto oldReplay = RenderBaseline(replay); Require(oldReplay); before.push_back(Elapsed(begin));
			begin = Clock::now(); auto newReplay = optimized.Render(replay); Require(newReplay); after.push_back(Elapsed(begin));
			const auto stepDiff = Compare(oldReplay->bitmap, newReplay->bitmap);
			replayDiff.pixels += stepDiff.pixels;
			replayDiff.maxChannel = (std::max)(replayDiff.maxChannel, stepDiff.maxChannel);
			replayDiff.meanChannel += stepDiff.meanChannel / 12;
			if (stepDiff.pixels && replayDiff.pixels == stepDiff.pixels)
			{
				SaveImage(oldReplay->bitmap, output / (name + "_pan_baseline.png"));
				SaveImage(newReplay->bitmap, output / (name + "_pan_optimized.png"));
			}
		}
		WriteRow(csv, name, "pan_zoom_replay", before, after, replayDiff, 0, 0);
		faithful &= replayDiff.pixels == 0;

		// Exercise changing visibility, zoom policy, scaling and non-parallelogram projection
		// against the preserved baseline with the same cache instance.
		auto toggled = request;
		auto groups = std::make_shared<std::unordered_map<std::string, bool>>(*fixture.visibility);
		for (auto& entry : *groups) entry.second = !entry.second;
		toggled.groupVisibility = std::move(groups);
		toggled.groupGeneration = 1;
		toggled.viewportZoomLevel = 2; toggled.labelZoomLevel = 2;
		toggled.rasterScale = 1.25; toggled.displayScale = 1.15;
		toggled.projectedBottomRight.X += 19.25f;
		auto oldToggled = RenderBaseline(toggled), newToggled = optimized.Render(toggled);
		Require(oldToggled); Require(newToggled);
		const auto toggledDiff = Compare(oldToggled->bitmap, newToggled->bitmap);
		WriteRow(csv, name, "visibility_scale_projection", {}, {}, toggledDiff, 0, 0);
		faithful &= toggledDiff.pixels == 0;
		if (toggledDiff.pixels)
		{
			SaveImage(oldToggled->bitmap, output / (name + "_variant_baseline.png"));
			SaveImage(newToggled->bitmap, output / (name + "_variant_optimized.png"));
		}

		// The opaque cache path must match the original composed over its background.
		const size_t palette = request.colorPalette == "dark" ? 0 : request.colorPalette == "light" ? 1 : 2;
		const Color background = fixture.backgrounds[palette];
		auto opaqueRequest = request; opaqueRequest.backgroundColor = background;
		auto opaque = optimized.Render(opaqueRequest); Require(opaque);
		const RECT source{ 0, 0, request.rasterWidth, request.rasterHeight };
		Surface originalTarget(request.rasterWidth, request.rasterHeight), changedTarget(request.rasterWidth, request.rasterHeight);
		VsmrAviso::AvisoRasterBlitter blitter;
		originalTarget.Clear(background); changedTarget.Clear(background);
		if (!blitter.Blend(*originalTarget.graphics, originalTarget.dc, original->bitmap, source, source, source, false) ||
			!blitter.Blend(*changedTarget.graphics, changedTarget.dc, opaque->bitmap, source, source, source, true))
			throw std::runtime_error("Cannot compose raster");
		Difference composition = Compare(originalTarget.bitmap, changedTarget.bitmap, true);
		// GDI+ source-over and AlphaBlend use different integer rounding; allow <=2/255.
		faithful &= composition.maxChannel <= 2;
		before.clear(); after.clear();
		for (int i = 0; i < samples * 4; ++i)
		{
			auto begin = Clock::now();
			originalTarget.Clear(background);
			if (!blitter.Blend(*originalTarget.graphics, originalTarget.dc, original->bitmap, source, source, source, false))
				throw std::runtime_error("AlphaBlend failed");
			GdiFlush(); before.push_back(Elapsed(begin));
			begin = Clock::now();
			changedTarget.Clear(background);
			if (!blitter.Blend(*changedTarget.graphics, changedTarget.dc, opaque->bitmap, source, source, source, true))
				throw std::runtime_error("BitBlt failed");
			GdiFlush(); after.push_back(Elapsed(begin));
		}
		WriteRow(csv, name, "native_present", before, after, composition, 0, 0);
		before.clear(); after.clear();
		for (int i = 0; i < samples * 4; ++i)
		{
			auto begin = Clock::now();
			originalTarget.Clear(background);
			if (!blitter.Blend(*originalTarget.graphics, originalTarget.dc, original->bitmap, source, source, source, false))
				throw std::runtime_error("AlphaBlend failed");
			GdiFlush(); before.push_back(Elapsed(begin));
			begin = Clock::now();
			if (!blitter.Blend(*changedTarget.graphics, changedTarget.dc, opaque->bitmap, source, source, source, true))
				throw std::runtime_error("BitBlt failed");
			GdiFlush(); after.push_back(Elapsed(begin));
		}
		WriteRow(csv, name, "covered_native_present", before, after, composition, 0, 0);

		const RECT scaledDestination{ 0, 0, 1379, 777 };
		originalTarget.Clear(background); changedTarget.Clear(background);
		if (!blitter.Blend(*originalTarget.graphics, originalTarget.dc, original->bitmap, source, scaledDestination, scaledDestination, false) ||
			!blitter.Blend(*changedTarget.graphics, changedTarget.dc, opaque->bitmap, source, scaledDestination, scaledDestination, true))
			throw std::runtime_error("Cannot compose scaled raster");
		const auto scaledDifference = Compare(originalTarget.bitmap, changedTarget.bitmap, true);
		WriteRow(csv, name, "scaled_present_fidelity", {}, {}, scaledDifference, 0, 0);
		faithful &= scaledDifference.maxChannel <= 2;
		Surface originalScaled(scaledDestination.right, scaledDestination.bottom), changedScaled(scaledDestination.right, scaledDestination.bottom);
		before.clear(); after.clear();
		for (int i = 0; i < samples * 4; ++i)
		{
			auto begin = Clock::now();
			originalScaled.Clear(background);
			if (!blitter.Blend(*originalScaled.graphics, originalScaled.dc, original->bitmap, source, scaledDestination, scaledDestination, false))
				throw std::runtime_error("Scaled AlphaBlend failed");
			GdiFlush(); before.push_back(Elapsed(begin));
			begin = Clock::now();
			if (!blitter.Blend(*changedScaled.graphics, changedScaled.dc, opaque->bitmap, source, scaledDestination, scaledDestination, true))
				throw std::runtime_error("Scaled StretchBlt failed");
			GdiFlush(); after.push_back(Elapsed(begin));
		}
		WriteRow(csv, name, "covered_scaled_present", before, after, scaledDifference, 0, 0);
		(void)path;
		return faithful;
	}
}

int main(int argc, char** argv)
{
	ULONG_PTR token = 0;
	GdiplusStartupInput startup;
	if (GdiplusStartup(&token, &startup, nullptr) != Ok) return 1;
	int exitCode = 0;
	try
	{
		const fs::path data = argc > 1 ? fs::u8path(argv[1]) : fs::path("vSMR/data/AVISO");
		const fs::path output = argc > 2 ? fs::u8path(argv[2]) : fs::path("artifacts/aviso-benchmark");
		const int samples = argc > 3 ? (std::max)(1, std::stoi(argv[3])) : 40;
		fs::create_directories(output);
		std::ofstream csv(output / "measurements.csv");
		csv << std::setprecision(9) << "scene,operation,samples,baseline_mean_ms,optimized_mean_ms,baseline_median_ms,optimized_median_ms,baseline_p95_ms,optimized_p95_ms,baseline_max_ms,optimized_max_ms,speedup,differing_pixels,max_channel_difference,mean_channel_difference,optimized_cold_ms,scene_build_ms\n";
		for (const char* airport : { "LFPG", "LFPO", "LFML" })
		{
			const fs::path path = data / (std::string(airport) + ".geojson");
			const Fixture fixture = LoadFixture(path);
			std::cout << airport << ": " << fixture.features->size() << " features, " << fixture.labels->size() << " labels" << std::endl;
			for (double zoom : { 1.0, 5.0 })
				for (const char* palette : { "dark", "light", "real" })
				{
					auto request = MakeRequest(fixture, path, zoom, zoom > 1 ? 23.0 : 0.0);
					request.colorPalette = palette;
					const std::string name = std::string(airport) + (zoom > 1 ? "_detail_rotated_" : "_whole_") + palette;
					if (!RunScene(fixture, path, output, name, std::move(request), samples, csv)) exitCode = 2;
				}
			if (std::string(airport) == "LFPG")
				for (double zoom : { 1.0, 5.0 })
				{
					auto request = MakeRequest(fixture, path, zoom, zoom > 1 ? 23.0 : 0.0);
					// Double-resolution cache retains the 1920x1080 projected display,
					// matching the worker's overscan/supersampling raster scaling.
					request.rasterWidth *= 2; request.rasterHeight *= 2; request.rasterScale = 2;
					const std::string name = std::string(airport) + (zoom > 1 ? "_detail_2x_raster_dark" : "_whole_2x_raster_dark");
					if (!RunScene(fixture, path, output, name, std::move(request), samples, csv)) exitCode = 2;
				}
		}
		std::cout << "CSV: " << (output / "measurements.csv").u8string() << std::endl;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl; exitCode = 1;
	}
	GdiplusShutdown(token);
	return exitCode;
}
