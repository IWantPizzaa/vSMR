#pragma once

#include "scene/RadarScene.hpp"
#include "rendering/TargetProjection.hpp"
#include "rendering/BrushCache.hpp"

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
		IconCacheCallbacks iconCache;
		std::function<void(const VsmrScene::Target&, const char*)> trace;
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

		template<class Project, class Visible = AlwaysVisible>
		DrawResult DrawTarget(const VsmrScene::Target& target, const Project& project,
			const Visible& visible = AlwaysVisible{}, const DrawOptions& options = DrawOptions{})
		{
			if (!target.position.valid) return {};
			m_Projected.Update(target, m_Settings.presentation, options, project, visible);
			return DrawProjectedTarget(target, options);
		}

	private:
		DrawResult DrawProjectedTarget(const VsmrScene::Target& target, const DrawOptions& options);
		VsmrRendering::BrushCache m_Brushes;
		Gdiplus::Graphics& m_Graphics;
		FrameSettings m_Settings;
		std::uint64_t m_CacheFrame = 0;
		Gdiplus::InterpolationMode m_SavedInterpolationMode;
		Gdiplus::PixelOffsetMode m_SavedPixelOffsetMode;
		Gdiplus::CompositingQuality m_SavedCompositingQuality;
		bool m_FastBitmapMode = false;
		ProjectedTarget m_Projected;
	};
}
