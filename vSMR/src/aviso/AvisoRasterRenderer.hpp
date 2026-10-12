#pragma once

#include "radar/RadarScreenTypes.hpp"

#include <functional>
#include <memory>

namespace VsmrAviso
{
	struct AvisoRasterRenderStats
	{
		std::size_t featureCandidates = 0;
		std::size_t labelCandidates = 0;
		std::size_t pathsDrawn = 0;
		std::size_t pathsCulled = 0;
		std::size_t projectedVertices = 0;
		std::size_t glyphCacheHits = 0;
		std::size_t glyphCacheMisses = 0;
	};

	// One renderer per worker. Its reusable scratch buffers and GDI+ resources
	// are never shared between concurrent main/inset raster builds.
	class AvisoRasterRenderer final
	{
	public:
		AvisoRasterRenderer();
		~AvisoRasterRenderer();
		AvisoRasterRenderer(const AvisoRasterRenderer&) = delete;
		AvisoRasterRenderer& operator=(const AvisoRasterRenderer&) = delete;

		std::unique_ptr<VsmrRadarTypes::AvisoRasterRenderResult> Render(
			const VsmrRadarTypes::AvisoRasterRenderRequest& request,
			const std::function<bool()>& cancelled = {});
		const AvisoRasterRenderStats& LastStats() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
