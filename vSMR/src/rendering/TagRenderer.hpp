#pragma once

#include "scene/RadarScene.hpp"

#include <afxwin.h>
#include <GdiPlus.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace VsmrTagRendering
{
	struct ElementLayout
	{
		std::string text;
		int action = 0;
		bool bold = false;
		VsmrScene::Color color;
		int width = 0;
		int height = 0;
	};

	struct LineLayout
	{
		std::vector<ElementLayout> elements;
		int width = 0;
	};

	struct Layout
	{
		std::vector<LineLayout> lines;
		int width = 0;
		int height = 0;

		bool Empty() const noexcept;
	};

	class FontContext final
	{
	public:
		FontContext(
			Gdiplus::Graphics& graphics,
			Gdiplus::Font* regularFont,
			int minimumBlankWidth = 1);
		FontContext(
			Gdiplus::Graphics& graphics,
			Gdiplus::Font* regularFont,
			Gdiplus::Font* boldFont,
			int blankWidth,
			int lineHeight);

		bool IsValid() const noexcept;
		Gdiplus::Font* RegularFont() const noexcept;
		Gdiplus::Font* BoldFont() const noexcept;
		const Gdiplus::StringFormat& Format() const noexcept;
		int BlankWidth() const noexcept;
		int LineHeight() const noexcept;
		const std::wstring& Utf16Text(const std::string& text) const;
		Gdiplus::Size Measure(const std::string& text, bool bold = false) const;

	private:
		Gdiplus::Graphics* graphics_ = nullptr;
		Gdiplus::Font* regularFont_ = nullptr;
		Gdiplus::Font* boldFont_ = nullptr;
		std::unique_ptr<Gdiplus::Font> ownedBoldFont_;
		Gdiplus::StringFormat format_;
		int blankWidth_ = 2;
		int lineHeight_ = 12;
		mutable std::unordered_map<std::string, std::wstring> decodedText_;
		mutable std::unordered_map<std::string, Gdiplus::Size> regularMeasurements_;
		mutable std::unordered_map<std::string, Gdiplus::Size> boldMeasurements_;
	};

	struct TopBand
	{
		std::string text;
		int action = 0;
		Gdiplus::Color background = Gdiplus::Color(255, 0, 0, 0);
		Gdiplus::Color textColor = Gdiplus::Color(255, 255, 255, 255);
	};

	enum class TopBandHitMode
	{
		None,
		TextOnly,
		FullBand
	};

	struct PaintOptions
	{
		POINT targetPoint = {};
		POINT tagCenter = {};
		Gdiplus::Color background = Gdiplus::Color(255, 0, 0, 0);
		Gdiplus::Color leaderColor = Gdiplus::Color(255, 255, 255, 255);
		bool roundedCorners = true;
		bool highlighted = false;
		bool centerLines = false;
		bool drawLeader = true;
		bool extendScratchpadHit = false;
		bool symmetricBounds = false;
		int scratchpadAction = 0;
		int contentHeightTrim = 0;
		unsigned int backgroundAlphaNumerator = 255;
		const TopBand* topBand = nullptr;
		TopBandHitMode topBandHitMode = TopBandHitMode::None;
	};

	struct HitRegion
	{
		int action = 0;
		CRect area;
	};

	struct PaintResult
	{
		CRect bounds;
		std::vector<HitRegion> hitRegions;
	};

	struct DetachedTopBand
	{
		std::string text;
		Gdiplus::Color background = Gdiplus::Color(255, 0, 0, 0);
		Gdiplus::Color textColor = Gdiplus::Color(255, 255, 255, 255);
	};

	bool MeasureLayout(
		const FontContext& fonts,
		const VsmrScene::TagVariant& variant,
		Layout& output);

	CRect CalculateBounds(
		const FontContext& fonts,
		const Layout& layout,
		const PaintOptions& options);

	PaintResult Paint(
		Gdiplus::Graphics& graphics,
		const FontContext& fonts,
		const Layout& layout,
		const PaintOptions& options);

	CRect PaintDetachedTopBand(
		Gdiplus::Graphics& graphics,
		const FontContext& fonts,
		const CRect& bodyBounds,
		const DetachedTopBand& band);
}
