#pragma once

#include <d2d1.h>
#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VsmrAviso
{
	// Analytic native-pixel label renderer. The source is the established
	// GDI+ outline converted to an ID2D1PathGeometry, never a glyph bitmap.
	class AvisoGlyphAnalyticRenderer final
	{
	public:
		struct Instance
		{
			ID2D1PathGeometry* outline = nullptr;
			D2D1_POINT_2F anchor{}; // Relative to the current native viewport origin.
			float haloWidth = 0.0f;
			D2D1_COLOR_F haloColor{}; // Straight RGBA; renderer premultiplies it.
			D2D1_COLOR_F textColor{};
		};

		AvisoGlyphAnalyticRenderer();
		~AvisoGlyphAnalyticRenderer();
		AvisoGlyphAnalyticRenderer(const AvisoGlyphAnalyticRenderer&) = delete;
		AvisoGlyphAnalyticRenderer& operator=(const AvisoGlyphAnalyticRenderer&) = delete;

		// Flush the caller's pending Direct2D batch before this Direct3D pass.
		// All immediate-context state is restored. Instances stay in source order;
		// each label computes halo then fill coverage from exact triangle/pixel intersections.
		// Failure returns false and Texture() is null, permitting vector fallback.
		bool Render(ID3D11Device* device, ID3D11DeviceContext* immediate,
			ID2D1Factory* factory, ID2D1StrokeStyle* haloStyle,
			UINT width, UINT height, const std::vector<Instance>& instances) noexcept;
		ID3D11Texture2D* Texture() const noexcept;
		UINT SampleCount() const noexcept;
		HRESULT LastError() const noexcept;
		const std::string& LastErrorMessage() const noexcept;
		std::size_t LastVertexCount() const noexcept;
		std::uint64_t CachedMeshBytes() const noexcept;
		std::uint64_t GpuImageBytes() const noexcept;
		std::uint64_t EstimatedImageBytes() const noexcept { return GpuImageBytes(); }
		std::uint64_t GpuBufferBytes() const noexcept;
		bool GlyphAreaCentroid(ID2D1Factory* factory, ID2D1PathGeometry* outline,
			D2D1_POINT_2F& centroid, double& area) noexcept;
		void Clear() noexcept;
		void RetainOutlines(const std::vector<ID2D1PathGeometry*>& outlines) noexcept;
		void ReleaseResources() noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
