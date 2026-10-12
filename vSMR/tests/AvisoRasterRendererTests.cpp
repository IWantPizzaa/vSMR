#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "AvisoRasterRendererTests.hpp"
#include "aviso/AvisoRasterRenderer.hpp"
#include "aviso/AvisoRenderScene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using namespace VsmrRadarTypes;
	using Failures = std::vector<std::string>;

	void Check(bool condition, const char* message, Failures& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}

	void TestSceneQueries(Failures& failures)
	{
		std::vector<AvisoFeature> features;
		std::vector<AvisoLabel> labels;
		for (std::uint32_t index = 0; index < 2048U; ++index)
		{
			const double x = static_cast<double>((index * 37U) % 64U);
			const double y = static_cast<double>((index * 113U) % 127U);
			AvisoFeature feature;
			feature.minLongitude = x;
			feature.minLatitude = y;
			feature.maxLongitude = x + 0.5;
			feature.maxLatitude = y + 0.75;
			feature.paths.push_back({ {x, y}, {x + 0.5, y + 0.75} });
			features.push_back(std::move(feature));
			AvisoLabel label;
			label.position = { x, y };
			labels.push_back(std::move(label));
		}
		const auto scene = VsmrAviso::AvisoRenderScene::Build(features, labels);
		std::vector<std::uint32_t> actual;
		for (std::uint32_t queryIndex = 0; queryIndex < 200U; ++queryIndex)
		{
			const double x = static_cast<double>((queryIndex * 19U) % 70U) - 2.0;
			const double y = static_cast<double>((queryIndex * 7U) % 130U) - 2.0;
			const VsmrAviso::AvisoGeoBounds bounds{ x, y, x + 6.5, y + 13.75 };
			std::vector<std::uint32_t> expected;
			for (std::uint32_t index = 0; index < features.size(); ++index)
			{
				const auto& feature = features[index];
				const VsmrAviso::AvisoGeoBounds featureBounds{ feature.minLongitude,
					feature.minLatitude, feature.maxLongitude, feature.maxLatitude };
				if (featureBounds.Intersects(bounds))
					expected.push_back(index);
			}
			scene->QueryFeatures(bounds, actual);
			Check(actual == expected, "AVISO BVH feature queries match inclusive brute-force bounds in document order", failures);
			expected.clear();
			for (std::uint32_t index = 0; index < labels.size(); ++index)
			{
				const auto& point = labels[index].position;
				if (point.longitude >= bounds.minLongitude && point.longitude <= bounds.maxLongitude &&
					point.latitude >= bounds.minLatitude && point.latitude <= bounds.maxLatitude)
					expected.push_back(index);
			}
			scene->QueryLabels(bounds, actual);
			Check(actual == expected, "AVISO BVH label queries match inclusive brute-force points in document order", failures);
		}
		scene->QueryFeatures({ -1000, -1000, 1000, 1000 }, actual);
		Check(actual.size() == features.size() && actual.front() == 0U && actual.back() == 2047U,
			"Full-scene queries preserve painter order without sorting the BVH", failures);
		scene->QueryFeatures({ 1000, 1000, 1001, 1001 }, actual);
		Check(actual.empty(), "Nonintersecting AVISO queries return no candidates", failures);
		const auto& pathBounds = scene->PathBounds(0, 0);
		Check(pathBounds.minLongitude == 0.0 && pathBounds.maxLongitude == 0.5 &&
			pathBounds.minLatitude == 0.0 && pathBounds.maxLatitude == 0.75,
			"Compiled AVISO path bounds retain geographic accuracy", failures);
		const auto empty = VsmrAviso::AvisoRenderScene::Build({}, {});
		empty->QueryFeatures({ 0, 0, 1, 1 }, actual);
		Check(actual.empty(), "Empty AVISO scenes remain queryable", failures);
	}

	AvisoRasterRenderRequest MakeRequest()
	{
		auto features = std::make_shared<std::vector<AvisoFeature>>();
		AvisoFeature polygon;
		polygon.polygon = true;
		polygon.paths = { { {0.1, 0.1}, {0.8, 0.1}, {0.8, 0.8}, {0.1, 0.8}, {0.1, 0.1} } };
		polygon.minLongitude = polygon.minLatitude = 0.1;
		polygon.maxLongitude = polygon.maxLatitude = 0.8;
		polygon.fillColor = Gdiplus::Color(200, 220, 20, 30);
		polygon.lightFillColor = Gdiplus::Color(200, 20, 220, 30);
		polygon.groupIds = { "surface" };
		features->push_back(polygon);
		polygon.paths = { { {0.3, 0.3}, {0.9, 0.3}, {0.9, 0.9}, {0.3, 0.9}, {0.3, 0.3} } };
		polygon.minLongitude = polygon.minLatitude = 0.3;
		polygon.maxLongitude = polygon.maxLatitude = 0.9;
		polygon.fillColor = Gdiplus::Color(100, 20, 30, 230);
		features->push_back(std::move(polygon));
		AvisoFeature lines;
		lines.strokeColor = Gdiplus::Color(255, 240, 240, 240);
		lines.strokeWidth = 20.0f;
		lines.paths = { { {-0.02, 0.2}, {-0.02, 0.8} }, { {0.25, 0.5}, {0.75, 0.5} },
			{ {10.0, 10.0}, {10.5, 10.5} }, { {-10.0, -10.0}, {-9.5, -9.5} } };
		lines.minLongitude = lines.minLatitude = -10;
		lines.maxLongitude = lines.maxLatitude = 10.5;
		features->push_back(std::move(lines));
		auto labels = std::make_shared<std::vector<AvisoLabel>>();
		AvisoLabel label;
		label.position = {0.5, 0.65};
		label.text = L"AVISO 12";
		label.textSize = 16.0f;
		label.haloWidth = 1.5f;
		labels->push_back(std::move(label));

		AvisoRasterRenderRequest request;
		request.path = "synthetic-renderer-test";
		request.features = features;
		request.labels = labels;
		request.scene = VsmrAviso::AvisoRenderScene::Build(*features, *labels);
		request.groupVisibility = std::make_shared<const std::unordered_map<std::string, bool>>(
			std::unordered_map<std::string, bool>{ {"surface", true} });
		request.rasterWidth = request.rasterHeight = 256;
		request.displayMaxLongitude = request.displayMaxLatitude = 1.0;
		request.renderMaxLongitude = request.renderMaxLatitude = 1.0;
		request.scaleX = request.scaleY = 256.0;
		request.projectedTopLeft = {0, 0};
		request.projectedTopRight = {256, 0};
		request.projectedBottomLeft = {0, 256};
		request.projectedBottomRight = {256, 256};
		return request;
	}

	std::vector<std::uint32_t> Pixels(HBITMAP bitmap)
	{
		DIBSECTION dib{};
		if (bitmap == nullptr || ::GetObject(bitmap, sizeof(dib), &dib) == 0 || dib.dsBm.bmBits == nullptr)
			return {};
		const auto* first = static_cast<const std::uint32_t*>(dib.dsBm.bmBits);
		return { first, first + static_cast<std::size_t>(dib.dsBm.bmWidth) *
			static_cast<std::size_t>(dib.dsBm.bmHeight) };
	}

	void TestRendererCacheAndCulling(Failures& failures)
	{
		VsmrAviso::AvisoRasterRenderer renderer;
		auto request = MakeRequest();
		auto first = renderer.Render(request);
		Check(first != nullptr, "Standalone AVISO renderer creates a raster", failures);
		if (first == nullptr)
			return;
		const auto pixels = Pixels(first->bitmap);
		Check(renderer.LastStats().pathsCulled == 2U && renderer.LastStats().projectedVertices == 14U,
			"AVISO path culling skips distant MultiLineString segments before projection", failures);
		Check(renderer.LastStats().glyphCacheMisses == 1U, "First AVISO raster builds a label glyph outline", failures);
		Check(pixels.size() == 65536U && (pixels[128U * 256U] >> 24U) != 0U,
			"AVISO path culling preserves thick strokes outside the geographic viewport", failures);
		auto second = renderer.Render(request);
		Check(second != nullptr && Pixels(second->bitmap) == pixels,
			"Warm AVISO geometry and glyph caches preserve raster pixels", failures);
		Check(renderer.LastStats().glyphCacheHits == 1U && renderer.LastStats().glyphCacheMisses == 0U,
			"Warm AVISO builds reuse glyph outlines", failures);
		request.colorPalette = "light";
		auto light = renderer.Render(request);
		Check(light != nullptr && Pixels(light->bitmap) != pixels && renderer.LastStats().glyphCacheHits == 1U,
			"Palette changes update AVISO colors while reusing glyph geometry", failures);
		request.displayScale = 1.5;
		auto larger = renderer.Render(request);
		Check(larger != nullptr && renderer.LastStats().glyphCacheMisses == 1U,
			"DPI scale changes rebuild AVISO font outlines and pen widths", failures);
		request.groupVisibility = std::make_shared<const std::unordered_map<std::string, bool>>(
			std::unordered_map<std::string, bool>{ {"surface", false} });
		auto hidden = renderer.Render(request);
		Check(hidden != nullptr && Pixels(hidden->bitmap) != Pixels(larger == nullptr ? nullptr : larger->bitmap),
			"AVISO group visibility remains effective with a shared compiled scene", failures);
		request.displayMinLongitude = request.renderMinLongitude = 0.125;
		request.displayMaxLongitude = request.renderMaxLongitude = 1.125;
		auto panned = renderer.Render(request);
		Check(panned != nullptr && renderer.LastStats().glyphCacheMisses == 1U,
			"Panning regenerates glyphs at the precise absolute origin to preserve GDI+ rounding", failures);
		VsmrAviso::AvisoRasterRenderer coldRenderer;
		auto coldPan = coldRenderer.Render(request);
		Check(panned != nullptr && coldPan != nullptr && Pixels(panned->bitmap) == Pixels(coldPan->bitmap),
			"Warm renderer panning matches a newly built raster pixel for pixel", failures);
		auto changedLabels = std::make_shared<std::vector<AvisoLabel>>(*request.labels);
		changedLabels->front().text = L"NEW LABEL";
		request.labels = changedLabels;
		auto changedText = renderer.Render(request);
		Check(changedText != nullptr && renderer.LastStats().glyphCacheMisses == 1U &&
			Pixels(changedText->bitmap) != Pixels(panned == nullptr ? nullptr : panned->bitmap),
			"Replacing an AVISO label snapshot invalidates its cached text outlines", failures);
		request.backgroundColor = Gdiplus::Color(255, 11, 22, 33);
		auto opaque = renderer.Render(request);
		const auto opaquePixels = Pixels(opaque == nullptr ? nullptr : opaque->bitmap);
		Check(opaque != nullptr && opaque->backgroundColor.GetValue() == request.backgroundColor.GetValue() &&
			!opaquePixels.empty() && opaquePixels.back() == 0xff0b1621U,
			"Opaque AVISO raster backgrounds retain the selected color and alpha", failures);
		request.backgroundColor = Gdiplus::Color(128, 11, 22, 33);
		auto translucent = renderer.Render(request);
		const auto translucentPixels = Pixels(translucent == nullptr ? nullptr : translucent->bitmap);
		Check(translucent != nullptr && translucent->backgroundColor.GetAlpha() == 0 &&
			!translucentPixels.empty() && translucentPixels.back() == 0U,
			"Translucent AVISO backgrounds remain host-composited transparent overlays", failures);
		Check(renderer.Render(request, []() { return true; }) == nullptr,
			"Cancelled AVISO builds do not allocate or publish a raster", failures);
		request.rasterWidth = 6401;
		Check(renderer.Render(request) == nullptr, "AVISO renderer enforces 32-bit raster allocation limits", failures);
	}
}

std::vector<std::string> RunAvisoRasterRendererTests()
{
	Failures failures;
	TestSceneQueries(failures);
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::GdiplusStartupInput startup;
	if (Gdiplus::GdiplusStartup(&gdiplusToken, &startup, nullptr) != Gdiplus::Ok)
	{
		failures.emplace_back("GDI+ startup failed for AVISO raster renderer tests");
		return failures;
	}
	TestRendererCacheAndCulling(failures);
	Gdiplus::GdiplusShutdown(gdiplusToken);
	return failures;
}
