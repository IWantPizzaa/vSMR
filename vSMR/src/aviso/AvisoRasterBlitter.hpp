#pragma once

#include <Windows.h>

namespace Gdiplus
{
	class Graphics;
}

namespace VsmrAviso
{
	// Win32 rectangles use exclusive right and bottom edges. Near-native axes
	// remain exactly 1:1 to prevent AlphaBlend from introducing a centre seam.
	struct RasterBlitPlan
	{
		RECT source{};
		RECT destination{};
		RECT clip{};
		int sourceWidth = 0;
		int sourceHeight = 0;
		int destinationWidth = 0;
		int destinationHeight = 0;
		bool scaled = false;
	};

	bool TryBuildRasterBlitPlan(
		const RECT& source,
		const RECT& destination,
		const RECT& clip,
		RasterBlitPlan& plan) noexcept;

	class AvisoRasterBlitter final
	{
	public:
		AvisoRasterBlitter() = default;
		~AvisoRasterBlitter() noexcept;
		AvisoRasterBlitter(const AvisoRasterBlitter&) = delete;
		AvisoRasterBlitter& operator=(const AvisoRasterBlitter&) = delete;

		bool Blend(
			Gdiplus::Graphics& graphics,
			HDC destinationDc,
			HBITMAP sourceBitmap,
			const RECT& source,
			const RECT& destination,
			const RECT& clip) noexcept;

	private:
		bool EnsureSourceDc(HDC destinationDc) noexcept;
		HDC sourceDc_ = nullptr;
	};
}
