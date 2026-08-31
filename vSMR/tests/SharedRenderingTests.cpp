#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "SharedRenderingTests.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"

#include <algorithm>
#include <cstddef>
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
	Gdiplus::GdiplusStartupInput input;
	ULONG_PTR token = 0;
	if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
	{
		failures.emplace_back("GDI+ starts for shared rendering behavior tests");
		return failures;
	}

	{
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
