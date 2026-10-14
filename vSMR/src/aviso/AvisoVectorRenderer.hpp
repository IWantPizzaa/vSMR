#pragma once

#include "aviso/AvisoRasterRenderer.hpp"

namespace VsmrAviso
{
	// Retained vector geometry, rasterized at the current viewport's native
	// pixels. The result is never a rescaled raster from an earlier viewport.
	// One renderer belongs to one thread; main and inset keep separate instances.
	class AvisoVectorRenderer final
	{
	public:
		struct Timings
		{
			double prepareMilliseconds = 0.0;
			double geometryMilliseconds = 0.0;
			double labelMilliseconds = 0.0;
			double flushMilliseconds = 0.0;
			double readbackMilliseconds = 0.0;
			double glyphMilliseconds = 0.0;
			double meshSetupMilliseconds = 0.0;
			double meshBuildMilliseconds = 0.0;
			double meshPackMilliseconds = 0.0;
			double meshDrawMilliseconds = 0.0;
			double meshFillMilliseconds = 0.0;
			double meshHaloMilliseconds = 0.0;
			double meshBinMilliseconds = 0.0;
		};
		enum class Backend { Hardware, Software };
		enum class TextMode { Vector, NativeAnalytic };
		explicit AvisoVectorRenderer(Backend preferred = Backend::Hardware,
			TextMode textMode = TextMode::NativeAnalytic);
		~AvisoVectorRenderer();
		AvisoVectorRenderer(const AvisoVectorRenderer&) = delete;
		AvisoVectorRenderer& operator=(const AvisoVectorRenderer&) = delete;

		// Projected request corners are absolute screen coordinates. Origin is
		// the exact integer top-left of the native-pixel destination rectangle.
		std::unique_ptr<VsmrRadarTypes::AvisoRasterRenderResult> Render(
			const VsmrRadarTypes::AvisoRasterRenderRequest& request,
			double screenOriginX = 0.0, double screenOriginY = 0.0,
			std::unique_ptr<VsmrRadarTypes::AvisoRasterRenderResult> reusable = {});
		const AvisoRasterRenderStats& LastStats() const noexcept;
		bool UsesHardware() const noexcept;
		const std::string& BackendDescription() const noexcept;
		void ReleaseResources() noexcept;
		HRESULT LastError() const noexcept;
		const Timings& LastTimings() const noexcept;
		std::size_t CachedGlyphPoints() const noexcept;
		std::uint64_t EstimatedGpuImageBytes() const noexcept;
		std::uint64_t CachedNativeMeshBytes() const noexcept;
		bool UsesNativeLabels() const noexcept;
		std::size_t LastVisibleLabelCount() const noexcept;
		bool LabelGeometricCentroid(std::size_t labelIndex, PointD& centroid, double& area) noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
