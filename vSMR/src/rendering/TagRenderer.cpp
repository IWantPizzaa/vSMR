#include "platform/windows/PrecompiledHeader.hpp"
#include "rendering/TagRenderer.hpp"

#include <algorithm>
#include <limits>

namespace
{
	Gdiplus::Rect ToGdiRect(const CRect& rect)
	{
		return Gdiplus::Rect(rect.left, rect.top, rect.Width(), rect.Height());
	}

	Gdiplus::Color ToGdiColor(const VsmrScene::Color& color)
	{
		return Gdiplus::Color(color.alpha, color.red, color.green, color.blue);
	}

	Gdiplus::Color ScaleAlpha(const Gdiplus::Color& color, unsigned int numerator)
	{
		const unsigned int clampedNumerator = (std::min)(numerator, 255U);
		const BYTE alpha = static_cast<BYTE>(
			(static_cast<unsigned int>(color.GetAlpha()) * clampedNumerator) / 255U);
		return Gdiplus::Color(alpha, color.GetR(), color.GetG(), color.GetB());
	}

	bool ClipLineToRect(
		const RECT& bounds,
		const POINT& from,
		const POINT& to,
		POINT& clippedFrom,
		POINT& clippedTo)
	{
		const double deltaX = static_cast<double>(to.x - from.x);
		const double deltaY = static_cast<double>(to.y - from.y);
		double entering = 0.0;
		double leaving = 1.0;
		auto clipEdge = [&](double direction, double distance) -> bool
		{
			if (direction == 0.0)
				return distance >= 0.0;
			const double ratio = distance / direction;
			if (direction < 0.0)
			{
				if (ratio > leaving)
					return false;
				entering = (std::max)(entering, ratio);
			}
			else
			{
				if (ratio < entering)
					return false;
				leaving = (std::min)(leaving, ratio);
			}
			return true;
		};

		if (!clipEdge(-deltaX, static_cast<double>(from.x - bounds.left)) ||
			!clipEdge(deltaX, static_cast<double>(bounds.right - from.x)) ||
			!clipEdge(-deltaY, static_cast<double>(from.y - bounds.top)) ||
			!clipEdge(deltaY, static_cast<double>(bounds.bottom - from.y)))
		{
			return false;
		}

		clippedFrom = {
			static_cast<LONG>(from.x + entering * deltaX),
			static_cast<LONG>(from.y + entering * deltaY) };
		clippedTo = {
			static_cast<LONG>(from.x + leaving * deltaX),
			static_cast<LONG>(from.y + leaving * deltaY) };
		return true;
	}

	void BuildRoundedPath(const Gdiplus::Rect& rect, Gdiplus::GraphicsPath& path)
	{
		constexpr int radius = 4;
		constexpr int diameter = radius * 2;
		path.Reset();
		path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
		path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
		path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
		path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
		path.CloseFigure();
	}

	void FillBackground(
		Gdiplus::Graphics& graphics,
		const CRect& bounds,
		const Gdiplus::Color& color,
		bool roundedCorners,
		bool highlighted,
		Gdiplus::GraphicsPath* roundedPath)
	{
		const Gdiplus::Rect rect = ToGdiRect(bounds);
		Gdiplus::SolidBrush brush(color);
		if (roundedCorners && roundedPath != nullptr)
		{
			BuildRoundedPath(rect, *roundedPath);
			graphics.FillPath(&brush, roundedPath);
			if (highlighted)
			{
				Gdiplus::Pen outline(Gdiplus::Color(255, 255, 255, 255));
				graphics.DrawPath(&outline, roundedPath);
			}
			return;
		}

		graphics.FillRectangle(&brush, rect);
		if (highlighted)
		{
			Gdiplus::Pen outline(Gdiplus::Color(255, 255, 255, 255));
			graphics.DrawRectangle(&outline, rect);
		}
	}
}

namespace VsmrTagRendering
{
	bool Layout::Empty() const noexcept
	{
		return lines.empty() || width <= 0 || height <= 0;
	}

	FontContext::FontContext(
		Gdiplus::Graphics& graphics,
		Gdiplus::Font* regularFont,
		int minimumBlankWidth) :
		graphics_(&graphics),
		regularFont_(regularFont),
		boldFont_(regularFont)
	{
		if (regularFont_ == nullptr)
			return;

		Gdiplus::FontFamily family;
		if (regularFont_->GetFamily(&family) == Gdiplus::Ok)
		{
			const INT style = regularFont_->GetStyle() | Gdiplus::FontStyleBold;
			ownedBoldFont_ = std::make_unique<Gdiplus::Font>(
				&family,
				regularFont_->GetSize(),
				style,
				Gdiplus::UnitPixel);
			if (ownedBoldFont_->GetLastStatus() == Gdiplus::Ok)
				boldFont_ = ownedBoldFont_.get();
			else
				ownedBoldFont_.reset();
		}

		blankWidth_ = (std::max)((std::max)(1, minimumBlankWidth), Measure(" ").Width);
		lineHeight_ = (std::max)(1, Measure("AZERTYUIOPQSDFGHJKLMWXCVBN").Height);
		if (boldFont_ != regularFont_)
			lineHeight_ = (std::max)(lineHeight_, Measure("AZERTYUIOPQSDFGHJKLMWXCVBN", true).Height);
	}

	FontContext::FontContext(
		Gdiplus::Graphics& graphics,
		Gdiplus::Font* regularFont,
		Gdiplus::Font* boldFont,
		int blankWidth,
		int lineHeight) :
		graphics_(&graphics),
		regularFont_(regularFont),
		boldFont_(boldFont != nullptr ? boldFont : regularFont),
		blankWidth_((std::max)(1, blankWidth)),
		lineHeight_((std::max)(1, lineHeight))
	{
	}

	bool FontContext::IsValid() const noexcept
	{
		return graphics_ != nullptr && regularFont_ != nullptr;
	}

	Gdiplus::Font* FontContext::RegularFont() const noexcept
	{
		return regularFont_;
	}

	Gdiplus::Font* FontContext::BoldFont() const noexcept
	{
		return boldFont_ != nullptr ? boldFont_ : regularFont_;
	}

	const Gdiplus::StringFormat& FontContext::Format() const noexcept
	{
		return format_;
	}

	int FontContext::BlankWidth() const noexcept
	{
		return blankWidth_;
	}

	int FontContext::LineHeight() const noexcept
	{
		return lineHeight_;
	}

	const std::wstring& FontContext::Utf16Text(const std::string& text) const
	{
		const auto existing = decodedText_.find(text);
		if (existing != decodedText_.end())
			return existing->second;

		std::wstring wide;
		if (!text.empty() && text.size() <= static_cast<size_t>((std::numeric_limits<int>::max)()))
		{
			const int sourceLength = static_cast<int>(text.size());
			auto decode = [&](UINT codePage, DWORD flags) -> bool
			{
				const int requiredLength = MultiByteToWideChar(
					codePage,
					flags,
					text.data(),
					sourceLength,
					nullptr,
					0);
				if (requiredLength <= 0)
					return false;

				wide.resize(static_cast<size_t>(requiredLength));
				return MultiByteToWideChar(
					codePage,
					flags,
					text.data(),
					sourceLength,
					wide.data(),
					requiredLength) == requiredLength;
			};

			// Scene text is UTF-8, while EuroScope callbacks may still expose text in
			// the active Windows code page. Preserve those legacy annotations when
			// the input is not valid UTF-8.
			if (!decode(CP_UTF8, MB_ERR_INVALID_CHARS))
			{
				wide.clear();
				decode(CP_ACP, 0);
			}
		}

		return decodedText_.emplace(text, std::move(wide)).first->second;
	}

	Gdiplus::Size FontContext::Measure(const std::string& text, bool bold) const
	{
		if (!IsValid() || text.empty())
			return Gdiplus::Size();

		auto& measurements = bold ? boldMeasurements_ : regularMeasurements_;
		const auto existing = measurements.find(text);
		if (existing != measurements.end())
			return existing->second;

		const std::wstring& wide = Utf16Text(text);
		if (wide.empty())
			return Gdiplus::Size();

		Gdiplus::RectF measured;
		Gdiplus::Font* font = bold ? BoldFont() : RegularFont();
		graphics_->MeasureString(
			wide.c_str(),
			static_cast<INT>(wide.size()),
			font,
			Gdiplus::PointF(0.0f, 0.0f),
			&format_,
			&measured);
		const Gdiplus::Size size(
			static_cast<INT>(measured.GetRight()),
			static_cast<INT>(measured.GetBottom()));
		measurements.emplace(text, size);
		return size;
	}

	bool MeasureLayout(
		const FontContext& fonts,
		const VsmrScene::TagVariant& variant,
		Layout& output)
	{
		output = Layout();
		if (!fonts.IsValid())
			return false;

		for (const VsmrScene::TagLine& sourceLine : variant.lines)
		{
			if (sourceLine.elements.empty())
				continue;

			LineLayout line;
			line.elements.reserve(sourceLine.elements.size());
			for (const VsmrScene::TagElement& sourceElement : sourceLine.elements)
			{
				ElementLayout element;
				element.text = sourceElement.text;
				element.action = sourceElement.action;
				element.bold = sourceElement.bold;
				element.color = sourceElement.effectiveColor;
				const Gdiplus::Size measured = fonts.Measure(element.text, element.bold);
				element.width = measured.Width;
				element.height = measured.Height;
				line.width += element.width;
				line.elements.push_back(std::move(element));
			}

			if (line.elements.empty())
				continue;
			line.width += fonts.BlankWidth() * (static_cast<int>(line.elements.size()) - 1);
			output.width = (std::max)(output.width, line.width);
			output.height += fonts.LineHeight();
			output.lines.push_back(std::move(line));
		}
		return !output.Empty();
	}

	CRect CalculateBounds(
		const FontContext& fonts,
		const Layout& layout,
		const PaintOptions& options)
	{
		if (!fonts.IsValid() || layout.Empty())
			return CRect();

		const Gdiplus::Size topBandSize = options.topBand != nullptr
			? fonts.Measure(options.topBand->text)
			: Gdiplus::Size();
		const int topBandHeight = options.topBand != nullptr ? fonts.LineHeight() : 0;
		const int contentWidth = (std::max)(layout.width, topBandSize.Width);
		const int padding = options.roundedCorners ? 1 : 0;
		const int totalWidth = contentWidth + padding * 2;
		const int bodyHeight = (std::max)(
			1,
			layout.height - (std::max)(0, options.contentHeightTrim));
		const int totalHeight = bodyHeight + topBandHeight + padding * 2;
		if (options.symmetricBounds)
		{
			return CRect(
				options.tagCenter.x - totalWidth / 2,
				options.tagCenter.y - totalHeight / 2,
				options.tagCenter.x + totalWidth / 2,
				options.tagCenter.y + totalHeight / 2);
		}

		return CRect(
			options.tagCenter.x - totalWidth / 2,
			options.tagCenter.y - totalHeight / 2,
			options.tagCenter.x - totalWidth / 2 + totalWidth,
			options.tagCenter.y - totalHeight / 2 + totalHeight);
	}

	PaintResult Paint(
		Gdiplus::Graphics& graphics,
		const FontContext& fonts,
		const Layout& layout,
		const PaintOptions& options)
	{
		PaintResult result;
		if (!fonts.IsValid() || layout.Empty())
			return result;

		const Gdiplus::Size topBandSize = options.topBand != nullptr
			? fonts.Measure(options.topBand->text)
			: Gdiplus::Size();
		const int topBandHeight = options.topBand != nullptr ? fonts.LineHeight() : 0;
		const int padding = options.roundedCorners ? 1 : 0;
		result.bounds = CalculateBounds(fonts, layout, options);
		if (result.bounds.IsRectEmpty())
			return result;

		Gdiplus::GraphicsPath roundedPath;
		FillBackground(
			graphics,
			result.bounds,
			ScaleAlpha(options.background, options.backgroundAlphaNumerator),
			options.roundedCorners,
			options.highlighted,
			&roundedPath);

		if (options.drawLeader)
		{
			POINT clippedFrom = {};
			POINT clippedTo = {};
			RECT bounds = result.bounds;
			if (ClipLineToRect(
				bounds,
				options.targetPoint,
				result.bounds.CenterPoint(),
				clippedFrom,
				clippedTo))
			{
				Gdiplus::Pen leader(options.leaderColor);
				graphics.DrawLine(
					&leader,
					Gdiplus::PointF(static_cast<Gdiplus::REAL>(options.targetPoint.x), static_cast<Gdiplus::REAL>(options.targetPoint.y)),
					Gdiplus::PointF(static_cast<Gdiplus::REAL>(clippedFrom.x), static_cast<Gdiplus::REAL>(clippedFrom.y)));
			}
		}

		const int textLeft = result.bounds.left + padding;
		const int textWidth = (std::max)(0, result.bounds.Width() - padding * 2);
		int textTop = result.bounds.top + padding;
		if (options.topBand != nullptr)
		{
			const CRect bandRect(
				result.bounds.left,
				result.bounds.top,
				result.bounds.right,
				result.bounds.top + padding + topBandHeight);
			const Gdiplus::GraphicsState state = graphics.Save();
			if (options.roundedCorners)
				graphics.SetClip(&roundedPath, Gdiplus::CombineModeIntersect);
			Gdiplus::SolidBrush bandBrush(options.topBand->background);
			graphics.FillRectangle(&bandBrush, ToGdiRect(bandRect));
			graphics.Restore(state);

			const int bandX = options.centerLines
				? textLeft + (std::max)(0, textWidth - topBandSize.Width) / 2
				: textLeft;
			const int bandY = textTop + (std::max)(0, topBandHeight - topBandSize.Height + 1) / 2;
			const std::wstring& bandText = fonts.Utf16Text(options.topBand->text);
			Gdiplus::SolidBrush bandTextBrush(options.topBand->textColor);
			graphics.DrawString(
				bandText.c_str(),
				static_cast<INT>(bandText.size()),
				fonts.RegularFont(),
				Gdiplus::PointF(static_cast<Gdiplus::REAL>(bandX), static_cast<Gdiplus::REAL>(bandY)),
				&fonts.Format(),
				&bandTextBrush);
			if (options.topBandHitMode == TopBandHitMode::FullBand)
			{
				result.hitRegions.push_back({ options.topBand->action, bandRect });
			}
			else if (options.topBandHitMode == TopBandHitMode::TextOnly && topBandSize.Width > 0)
			{
				result.hitRegions.push_back({
					options.topBand->action,
					CRect(
						bandX,
						textTop,
						bandX + topBandSize.Width,
						textTop + (std::max)(topBandSize.Height, topBandHeight)) });
			}
			textTop += topBandHeight;
		}

		for (const LineLayout& line : layout.lines)
		{
			int x = options.centerLines
				? textLeft + (std::max)(0, textWidth - line.width) / 2
				: textLeft;
			for (const ElementLayout& element : line.elements)
			{
				if (!element.text.empty())
				{
					const int y = textTop + (std::max)(0, fonts.LineHeight() - element.height + 1) / 2;
					const std::wstring& text = fonts.Utf16Text(element.text);
					Gdiplus::SolidBrush textBrush(ToGdiColor(element.color));
					graphics.DrawString(
						text.c_str(),
						static_cast<INT>(text.size()),
						element.bold ? fonts.BoldFont() : fonts.RegularFont(),
						Gdiplus::PointF(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y)),
						&fonts.Format(),
						&textBrush);
				}

				int hitWidth = element.width;
				if (options.extendScratchpadHit && element.action == options.scratchpadAction)
					hitWidth = (std::max)(
						hitWidth,
						static_cast<int>(result.bounds.right) - padding - x);
				if (hitWidth > 0)
				{
					result.hitRegions.push_back({
						element.action,
						CRect(
							x,
							textTop,
							x + hitWidth,
							textTop + (std::max)(element.height, fonts.LineHeight())) });
				}
				x += element.width + fonts.BlankWidth();
			}
			textTop += fonts.LineHeight();
		}

		return result;
	}

	CRect PaintDetachedTopBand(
		Gdiplus::Graphics& graphics,
		const FontContext& fonts,
		const CRect& bodyBounds,
		const DetachedTopBand& band)
	{
		if (!fonts.IsValid() || bodyBounds.IsRectEmpty() || band.text.empty())
			return CRect();

		const Gdiplus::Size textSize = fonts.Measure(band.text);
		const int bandHeight = (std::max)(1, textSize.Height);
		const CRect bandBounds(
			bodyBounds.left,
			bodyBounds.top - bandHeight,
			bodyBounds.right,
			bodyBounds.top);
		Gdiplus::SolidBrush backgroundBrush(band.background);
		graphics.FillRectangle(&backgroundBrush, ToGdiRect(bandBounds));

		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		const std::wstring& text = fonts.Utf16Text(band.text);
		Gdiplus::SolidBrush textBrush(band.textColor);
		graphics.DrawString(
			text.c_str(),
			static_cast<INT>(text.size()),
			fonts.RegularFont(),
			Gdiplus::PointF(
				static_cast<Gdiplus::REAL>((bandBounds.left + bandBounds.right) / 2),
				static_cast<Gdiplus::REAL>(bandBounds.top)),
			&format,
			&textBrush);
		return bandBounds;
	}
}
