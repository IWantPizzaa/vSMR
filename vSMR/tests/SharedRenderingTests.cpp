#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "SharedRenderingTests.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace
{
	void Check(
		bool condition,
		const char* message,
		std::vector<std::string>& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}

	int Width(const RECT& rect)
	{
		return static_cast<int>(rect.right - rect.left);
	}

	int Height(const RECT& rect)
	{
		return static_cast<int>(rect.bottom - rect.top);
	}

	class DibBitmap final
	{
	public:
		DibBitmap(int width, int height)
		{
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* pixels = nullptr;
			bitmap_ = ::CreateDIBSection(
				nullptr,
				&info,
				DIB_RGB_COLORS,
				&pixels,
				nullptr,
				0);
			pixels_ = static_cast<std::uint32_t*>(pixels);
		}

		~DibBitmap()
		{
			if (bitmap_ != nullptr)
				::DeleteObject(bitmap_);
		}

		DibBitmap(const DibBitmap&) = delete;
		DibBitmap& operator=(const DibBitmap&) = delete;

		bool IsValid() const noexcept { return bitmap_ != nullptr && pixels_ != nullptr; }
		HBITMAP Handle() const noexcept { return bitmap_; }
		std::uint32_t* Pixels() const noexcept { return pixels_; }

	private:
		HBITMAP bitmap_ = nullptr;
		std::uint32_t* pixels_ = nullptr;
	};

	void TestAvisoRasterBlending(std::vector<std::string>& failures)
	{
		constexpr int width = 8;
		constexpr int height = 8;
		constexpr std::uint32_t blue = 0xFF0000FFU;
		constexpr std::uint32_t red = 0xFFFF0000U;
		DibBitmap destination(width, height);
		DibBitmap source(4, 4);
		HDC destinationDc = ::CreateCompatibleDC(nullptr);
		if (!destination.IsValid() || !source.IsValid() || destinationDc == nullptr)
		{
			if (destinationDc != nullptr)
				::DeleteDC(destinationDc);
			failures.emplace_back("GDI resources are available for AVISO raster blending tests");
			return;
		}

		std::fill(destination.Pixels(), destination.Pixels() + width * height, blue);
		std::fill(source.Pixels(), source.Pixels() + 16, red);
		HGDIOBJ previousBitmap = ::SelectObject(destinationDc, destination.Handle());
		if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR)
		{
			::DeleteDC(destinationDc);
			failures.emplace_back("destination bitmap can be selected for AVISO raster tests");
			return;
		}

		{
			Gdiplus::Graphics graphics(destinationDc);
			VsmrAviso::AvisoRasterBlitter blitter;
			::SetStretchBltMode(destinationDc, BLACKONWHITE);
			HRGN originalClip = ::CreateRectRgn(1, 1, 7, 7);
			::SelectClipRgn(destinationDc, originalClip);
			::DeleteObject(originalClip);
			RECT clipBefore{};
			::GetClipBox(destinationDc, &clipBefore);

			Check(
				blitter.Blend(
					graphics,
					destinationDc,
					source.Handle(),
					RECT{ 0, 0, 4, 4 },
					RECT{ 2, 2, 6, 6 },
					RECT{ 3, 3, 5, 5 }),
				"AVISO raster blitter alpha-blends a valid bitmap",
				failures);
			::GdiFlush();

			bool pixelsMatch = true;
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const bool insideClip = x >= 3 && x < 5 && y >= 3 && y < 5;
					const std::uint32_t expected = insideClip ? red : blue;
					pixelsMatch = pixelsMatch &&
						destination.Pixels()[y * width + x] == expected;
				}
			}
			Check(
				pixelsMatch,
				"AVISO raster blitter changes only pixels inside the clipping rectangle",
				failures);
			Check(
				blitter.Blend(
					graphics,
					destinationDc,
					source.Handle(),
					RECT{ 0, 0, 4, 4 },
					RECT{ 2, 2, 6, 6 },
					RECT{ 3, 3, 5, 5 }),
				"AVISO raster blitter reuses its compatible source DC",
				failures);

			RECT clipAfter{};
			::GetClipBox(destinationDc, &clipAfter);
			Check(
				::EqualRect(&clipBefore, &clipAfter) != FALSE &&
					::GetStretchBltMode(destinationDc) == BLACKONWHITE,
				"AVISO raster blitter restores destination DC clipping and stretch state",
				failures);
		}

		::SelectClipRgn(destinationDc, nullptr);
		::SelectObject(destinationDc, previousBitmap);
		::DeleteDC(destinationDc);
	}

	void TestAvisoRasterBlitPlanning(std::vector<std::string>& failures)
	{
		const RECT source = { 2, 3, 102, 53 };
		const RECT clip = { 4, 5, 204, 155 };
		VsmrAviso::RasterBlitPlan plan;
		Check(
			VsmrAviso::TryBuildRasterBlitPlan(
				source,
				RECT{ 10, 20, 111, 69 },
				clip,
				plan),
			"AVISO raster blitter accepts positive source and destination rectangles",
			failures);
		Check(
			plan.destination.left == 10 && plan.destination.top == 20 &&
				plan.destination.right == 110 && plan.destination.bottom == 70 &&
				plan.destinationWidth == 100 && plan.destinationHeight == 50 &&
				!plan.scaled,
			"AVISO raster blitter keeps near-native axes exactly one-to-one",
			failures);
		Check(
			plan.source.left == source.left && plan.source.top == source.top &&
				plan.source.right == source.right && plan.source.bottom == source.bottom &&
				plan.clip.left == clip.left && plan.clip.top == clip.top &&
				plan.clip.right == clip.right && plan.clip.bottom == clip.bottom,
			"AVISO raster blitter preserves the caller source and clip rectangles",
			failures);

		Check(
			VsmrAviso::TryBuildRasterBlitPlan(
				source,
				RECT{ 10, 20, 112, 73 },
				clip,
				plan) &&
				plan.destinationWidth == 102 && plan.destinationHeight == 53 && plan.scaled,
			"AVISO raster blitter retains intentional zoom scaling",
			failures);
		Check(
			!VsmrAviso::TryBuildRasterBlitPlan(
				RECT{ 5, 5, 5, 8 },
				RECT{ 0, 0, 10, 10 },
				clip,
				plan),
			"AVISO raster blitter rejects empty source rectangles",
			failures);
	}

	VsmrScene::Target MakeTarget(VsmrScene::IconStyle icon)
	{
		VsmrScene::Target target;
		target.position = { 48.0, 2.0, true };
		target.headingProbe = { 48.0001, 2.0, true };
		target.headingTrueDegrees = 0.0;
		target.style.icon = icon;
		target.style.color = { 255, 255, 255, 255 };
		return target;
	}

	void TestCollapsedProjectionArrow(
		Gdiplus::Graphics& graphics,
		std::vector<std::string>& failures)
	{
		VsmrTargetRendering::FrameSettings settings;
		settings.presentation.icon = VsmrScene::IconStyle::Triangle;
		settings.pixelsPerMeter = 0.0;
		settings.projectPoint = [](const VsmrScene::GeoPoint&) -> POINT
		{
			return { 32, 32 };
		};
		settings.pointVisible = [](const POINT&, int)
		{
			return true;
		};

		const VsmrScene::Target target = MakeTarget(VsmrScene::IconStyle::Triangle);
		VsmrTargetRendering::DrawResult result;
		{
			VsmrTargetRendering::Frame frame(graphics, std::move(settings));
			result = frame.DrawTarget(target);
		}
		Check(result.drawn, "shared target renderer draws a valid target", failures);
		Check(
			Width(result.symbolBounds) >= 2 && Height(result.symbolBounds) >= 2,
			"minimum-size arrow remains visible when geographic projection rounds to one point",
			failures);
		Check(
			Width(result.hitBounds) >= 12 && Height(result.hitBounds) >= 12,
			"shared target renderer preserves the minimum symbol hit area",
			failures);

		VsmrTargetRendering::FrameSettings insetSettings;
		insetSettings.presentation.icon = VsmrScene::IconStyle::Triangle;
		insetSettings.projectPoint = [](const VsmrScene::GeoPoint&) -> POINT
		{
			return { 32, 32 };
		};
		VsmrTargetRendering::DrawOptions insetOptions;
		insetOptions.minimumHitSize = 18;
		VsmrTargetRendering::Frame insetFrame(graphics, std::move(insetSettings));
		const VsmrTargetRendering::DrawResult insetResult =
			insetFrame.DrawTarget(target, insetOptions);
		Check(
			Width(insetResult.hitBounds) >= 18 && Height(insetResult.hitBounds) >= 18,
			"viewport-specific target hit areas are preserved",
			failures);
	}

	void TestGraphicsStateRestoration(
		Gdiplus::Graphics& graphics,
		std::vector<std::string>& failures)
	{
		graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
		graphics.SetCompositingQuality(Gdiplus::CompositingQualityGammaCorrected);
		int frameCount = 0;

		VsmrTargetRendering::FrameSettings settings;
		settings.presentation.icon = VsmrScene::IconStyle::Realistic;
		settings.projectPoint = [](const VsmrScene::GeoPoint&) -> POINT
		{
			return { 24, 24 };
		};
		settings.iconCache.beginFrame = [&]() -> std::uint64_t
		{
			return static_cast<std::uint64_t>(++frameCount);
		};
		settings.iconCache.getSourceBitmap = [](const std::string&) -> Gdiplus::Bitmap*
		{
			return nullptr;
		};

		{
			VsmrTargetRendering::Frame frame(graphics, settings);
			Check(
				graphics.GetPixelOffsetMode() == Gdiplus::PixelOffsetModeHighSpeed &&
				graphics.GetCompositingQuality() == Gdiplus::CompositingQualityHighSpeed,
				"realistic target pass enables its fast bitmap mode",
				failures);
			frame.DrawTarget(MakeTarget(VsmrScene::IconStyle::Realistic));
		}
		Check(frameCount == 1, "realistic target pass advances the cache frame once", failures);
		Check(
			graphics.GetInterpolationMode() == Gdiplus::InterpolationModeHighQualityBicubic &&
			graphics.GetPixelOffsetMode() == Gdiplus::PixelOffsetModeHalf &&
			graphics.GetCompositingQuality() == Gdiplus::CompositingQualityGammaCorrected,
			"target renderer restores caller graphics modes",
			failures);

		settings.optimizeRealisticBitmapQuality = false;
		{
			VsmrTargetRendering::Frame frame(graphics, settings);
			Check(
				graphics.GetInterpolationMode() == Gdiplus::InterpolationModeHighQualityBicubic,
				"mixed target/tag pass can retain caller graphics modes",
				failures);
		}
		Check(
			frameCount == 2,
			"mixed target/tag pass still advances the realistic icon cache frame",
			failures);
	}

	void TestSharedTagGeometry(
		Gdiplus::Graphics& graphics,
		std::vector<std::string>& failures)
	{
		Gdiplus::Font font(
			Gdiplus::FontFamily::GenericSansSerif(),
			12.0f,
			Gdiplus::FontStyleRegular,
			Gdiplus::UnitPixel);
		VsmrTagRendering::FontContext fonts(graphics, &font, 2);
		Check(fonts.IsValid(), "shared tag test font is available", failures);
		const std::wstring& decoded = fonts.Utf16Text("\xC3\x89");
		Check(
			decoded == L"\u00C9",
			"tag text is decoded from UTF-8 before GDI+ measurement",
			failures);
		Check(
			&decoded == &fonts.Utf16Text("\xC3\x89"),
			"decoded tag text is reused within the frame font context",
			failures);
		const std::string legacyText(1, static_cast<char>(0xE9));
		wchar_t legacyCharacter = L'\0';
		const int legacyLength = MultiByteToWideChar(
			CP_ACP,
			0,
			legacyText.data(),
			static_cast<int>(legacyText.size()),
			&legacyCharacter,
			1);
		if (legacyLength == 1)
		{
			Check(
				fonts.Utf16Text(legacyText) == std::wstring(1, legacyCharacter),
				"legacy EuroScope text falls back to the active Windows code page",
				failures);
		}
		else
		{
			Check(
				fonts.Utf16Text(legacyText).empty(),
				"undecodable legacy tag text fails closed on UTF-8 system locales",
				failures);
		}

		VsmrScene::TagVariant variant;
		VsmrScene::TagLine line;
		VsmrScene::TagElement callsign;
		callsign.text = "TEST123";
		callsign.action = 10;
		callsign.effectiveColor = { 255, 255, 255, 255 };
		line.elements.push_back(callsign);
		VsmrScene::TagElement scratchpad;
		scratchpad.text = "A";
		scratchpad.action = 20;
		scratchpad.effectiveColor = { 255, 255, 255, 0 };
		line.elements.push_back(scratchpad);
		variant.lines.push_back(line);

		VsmrTagRendering::Layout layout;
		Check(
			VsmrTagRendering::MeasureLayout(fonts, variant, layout),
			"shared tag renderer measures populated layouts",
			failures);
		VsmrTagRendering::PaintOptions options;
		options.targetPoint = { 8, 8 };
		options.tagCenter = { 48, 36 };
		options.background = Gdiplus::Color(255, 30, 30, 30);
		options.symmetricBounds = true;
		options.extendScratchpadHit = true;
		options.scratchpadAction = 20;
		const CRect arranged = VsmrTagRendering::CalculateBounds(fonts, layout, options);
		Check(
			arranged.Height() == layout.height + 2,
			"tag bounds preserve the complete centered line box",
			failures);
		const VsmrTagRendering::PaintResult painted =
			VsmrTagRendering::Paint(graphics, fonts, layout, options);
		Check(
			painted.bounds == arranged,
			"tag measurement and painting share one bounds calculation",
			failures);
		Check(
			painted.hitRegions.size() == 2,
			"shared tag renderer publishes one hit region per element",
			failures);
		if (painted.hitRegions.size() == 2)
		{
			Check(
				painted.hitRegions[1].area.right == painted.bounds.right - 1,
				"scratchpad hit region extends to the rounded tag edge",
				failures);
		}

		VsmrTagRendering::DetachedTopBand band;
		band.text = "ALERT";
		band.background = Gdiplus::Color(255, 180, 30, 30);
		const CRect bandBounds = VsmrTagRendering::PaintDetachedTopBand(
			graphics,
			fonts,
			painted.bounds,
			band);
		Check(
			!bandBounds.IsRectEmpty() && bandBounds.bottom == painted.bounds.top,
			"detached alert band remains outside the tag body hit area",
			failures);
	}
}

std::vector<std::string> RunSharedRenderingBehaviorTests()
{
	std::vector<std::string> failures;
	TestAvisoRasterBlitPlanning(failures);
	Gdiplus::GdiplusStartupInput input;
	ULONG_PTR token = 0;
	if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
	{
		failures.emplace_back("GDI+ starts for shared rendering behavior tests");
		return failures;
	}

	{
		TestAvisoRasterBlending(failures);
		Gdiplus::Bitmap canvas(96, 72, PixelFormat32bppARGB);
		Gdiplus::Graphics graphics(&canvas);
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		TestCollapsedProjectionArrow(graphics, failures);
		TestGraphicsStateRestoration(graphics, failures);
		TestSharedTagGeometry(graphics, failures);
	}
	Gdiplus::GdiplusShutdown(token);
	return failures;
}
