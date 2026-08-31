#pragma once

#include "scene/RadarScene.hpp"

#include <Windows.h>
#include <GdiPlus.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VsmrTargetRendering
{
	struct CachedBitmap
	{
		Gdiplus::Bitmap* bitmap = nullptr;
		int centerX = 0;
		int centerY = 0;
	};

	struct IconCacheCallbacks
	{
		std::function<std::uint64_t()> beginFrame;
		std::function<Gdiplus::Bitmap*(const std::string&)> getSourceBitmap;
		std::function<Gdiplus::Bitmap*(
			const std::string&,
			Gdiplus::Bitmap*,
			UINT,
			UINT,
			const Gdiplus::Color&,
			double,
			double,
			std::uint64_t,
			int&,
			int&,
			std::string&)> getScaledBitmap;
		std::function<CachedBitmap(
			const std::string&,
			Gdiplus::Bitmap*,
			int,
			int,
			double,
			std::uint64_t)> getRotatedBitmap;
	};

	struct FrameSettings
	{
		VsmrScene::TargetPresentation presentation;
		double pixelsPerMeter = 0.0;
		bool optimizeRealisticBitmapQuality = true;
		std::function<POINT(const VsmrScene::GeoPoint&)> projectPoint;
		std::function<bool(const POINT&, int)> pointVisible;
		IconCacheCallbacks iconCache;
		std::function<void(const VsmrScene::Target&, const char*)> trace;
	};

	struct DrawOptions
	{
		bool drawTrail = true;
		bool drawPrimaryReturn = true;
		int minimumHitSize = 12;
	};

	struct DrawResult
	{
		bool drawn = false;
		bool trailDrawn = false;
		bool primaryReturnDrawn = false;
		bool realisticBitmapDrawn = false;
		POINT center{};
		RECT symbolBounds{};
		// Visual bounds include the trail and primary return; hit bounds cover only
		// the selectable symbol and remain unclipped for the viewport to intersect.
		RECT visualBounds{};
		RECT hitBounds{};
	};

	// Own one instance for the duration of a viewport target pass. It applies the
	// bitmap rendering mode once and restores the caller's GDI+ state on exit.
	class Frame final
	{
	public:
		Frame(Gdiplus::Graphics& graphics, FrameSettings settings);
		~Frame();

		Frame(const Frame&) = delete;
		Frame& operator=(const Frame&) = delete;
		Frame(Frame&&) = delete;
		Frame& operator=(Frame&&) = delete;

		DrawResult DrawTarget(
			const VsmrScene::Target& target,
			const DrawOptions& options = DrawOptions{});

	private:
		Gdiplus::Graphics& m_Graphics;
		FrameSettings m_Settings;
		std::uint64_t m_CacheFrame = 0;
		Gdiplus::InterpolationMode m_SavedInterpolationMode;
		Gdiplus::PixelOffsetMode m_SavedPixelOffsetMode;
		Gdiplus::CompositingQuality m_SavedCompositingQuality;
		bool m_FastBitmapMode = false;
		std::vector<Gdiplus::PointF> m_PolygonScratch;
	};
}
