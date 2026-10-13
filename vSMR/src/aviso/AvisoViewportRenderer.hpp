#pragma once

#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoVectorRenderer.hpp"

#include <functional>
#include <optional>

namespace VsmrAviso
{
	// A cache may be reused only for exactly the same native-pixel view. Changed
	// views are vector-rendered immediately; there is no scaled preview or later
	// replacement bitmap with a different projection or quality level.
	class AvisoViewportRenderer final
	{
	public:
		explicit AvisoViewportRenderer(AvisoVectorRenderer::Backend backend = AvisoVectorRenderer::Backend::Hardware,
			AvisoVectorRenderer::TextMode text = AvisoVectorRenderer::TextMode::NativeAnalytic)
			: renderer_(backend, text) {}
		struct Outcome
		{
			bool drawn = false;
			bool cacheHit = false;
			bool buildAttempted = false;
			bool hardware = false;
			double buildMilliseconds = 0.0;
		};

		Outcome Draw(Gdiplus::Graphics& graphics, HDC destination,
			const RECT& viewport, const VsmrRadarTypes::AvisoRasterRenderRequest& request,
			const std::function<void()>& paintBackground);
		void Clear() noexcept;
		void ReleaseResources() noexcept;
		HBITMAP Bitmap() const noexcept;
		std::uint64_t BitmapBytes() const noexcept;
		std::string BackendDescription() const { return renderer_.BackendDescription(); }
		const AvisoVectorRenderer::Timings& LastTimings() const noexcept { return renderer_.LastTimings(); }
		const AvisoRasterRenderStats& LastStats() const noexcept { return renderer_.LastStats(); }
		std::size_t CachedGlyphPoints() const noexcept { return renderer_.CachedGlyphPoints(); }
		std::uint64_t EstimatedGpuImageBytes() const noexcept { return renderer_.EstimatedGpuImageBytes(); }
		std::uint64_t CachedNativeMeshBytes() const noexcept { return renderer_.CachedNativeMeshBytes(); }
		bool UsesNativeLabels() const noexcept { return renderer_.UsesNativeLabels(); }
		std::size_t LastVisibleLabelCount() const noexcept { return renderer_.LastVisibleLabelCount(); }

	private:
		bool Matches(const RECT& viewport,
			const VsmrRadarTypes::AvisoRasterRenderRequest& request) const noexcept;
		AvisoVectorRenderer renderer_;
		AvisoRasterBlitter blitter_;
		std::unique_ptr<VsmrRadarTypes::AvisoRasterRenderResult> cache_;
		std::optional<VsmrRadarTypes::AvisoRasterRenderRequest> key_;
		RECT viewport_{};
	};
}
