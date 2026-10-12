#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterRenderer.hpp"
#include "aviso/AvisoRasterGeometry.hpp"
#include "aviso/AvisoRenderScene.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

using namespace Gdiplus;
using VsmrRadarTypes::AvisoFeature;
using VsmrRadarTypes::AvisoLabel;
using VsmrRadarTypes::AvisoPoint;
using VsmrRadarTypes::AvisoRasterRenderRequest;
using VsmrRadarTypes::AvisoRasterRenderResult;
using VsmrAvisoSupport::AvisoMin;
using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::ToUpperAscii;

namespace
{
	bool IsAvisoGroupedItemVisible(
		const std::vector<std::string>& groupIds,
		const std::unordered_map<std::string, bool>* visibilityById)
	{
		if (groupIds.empty() || visibilityById == nullptr)
			return true;

		bool foundKnownGroup = false;
		for (const std::string& groupId : groupIds)
		{
			const auto found = visibilityById->find(groupId);
			if (found == visibilityById->end())
			{
				// Legacy/foreign group references remain visible until a
				// definition explicitly controls them.
				return true;
			}

			foundKnownGroup = true;
			if (found->second)
				return true;
		}

		// Multiple membership uses union semantics: an item is visible when
		// at least one of its configured groups is visible.
		return !foundKnownGroup;
	}

	class ScopedHBitmap
	{
	public:
		~ScopedHBitmap()
		{
			Reset();
		}

		void Reset(HBITMAP bitmap = nullptr)
		{
			if (bitmap_ != nullptr)
				::DeleteObject(bitmap_);
			bitmap_ = bitmap;
		}

		HBITMAP Get() const
		{
			return bitmap_;
		}

		HBITMAP Release()
		{
			HBITMAP bitmap = bitmap_;
			bitmap_ = nullptr;
			return bitmap;
		}

	private:
		HBITMAP bitmap_ = nullptr;
	};

}

struct VsmrAviso::AvisoRasterRenderer::Impl
{
	struct LabelGlyph
	{
		double rasterScale = 0.0;
		double displayScale = 0.0;
		REAL originX = 0.0f;
		REAL originY = 0.0f;
		std::unique_ptr<GraphicsPath> path;
	};

	std::shared_ptr<const std::vector<AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<AvisoLabel>> labelSnapshot;
	std::shared_ptr<const AvisoRenderScene> scene;
	std::vector<LabelGlyph> glyphs;
	// GDI+ keeps both points and point types. Limit outline storage to about
	// 5 MB per worker; an unusually large label can still render uncached.
	static constexpr std::size_t GlyphPointBudget = 524288U;
	static constexpr std::size_t ResourceEntryBudget = 2048U;
	std::size_t glyphPointCount = 0;
	std::vector<std::uint32_t> featureCandidates;
	std::vector<std::uint32_t> labelCandidates;
	std::vector<PointF> points;
	std::map<ARGB, std::unique_ptr<SolidBrush>> brushes;
	std::map<std::uint64_t, std::unique_ptr<Pen>> polygonPens;
	std::map<std::uint64_t, std::unique_ptr<Pen>> linePens;
	double penRasterScale = 0.0;
	double penDisplayScale = 0.0;
	AvisoRasterRenderStats stats;

	void Prepare(const AvisoRasterRenderRequest& request)
	{
		stats = {};
		if (featureSnapshot != request.features || labelSnapshot != request.labels)
		{
			// Allocate the replacement before publishing cache identity. A
			// transient allocation failure must leave the previous cache valid.
			auto replacementScene = request.scene != nullptr ? request.scene :
				AvisoRenderScene::Build(*request.features, *request.labels);
			std::vector<LabelGlyph> replacementGlyphs(request.labels->size());
			featureSnapshot = request.features;
			labelSnapshot = request.labels;
			scene = std::move(replacementScene);
			glyphs = std::move(replacementGlyphs);
			glyphPointCount = 0;
			brushes.clear();
			polygonPens.clear();
			linePens.clear();
		}
		else if (request.scene != nullptr)
			scene = request.scene;
		if (penRasterScale != request.rasterScale || penDisplayScale != request.displayScale)
		{
			polygonPens.clear();
			linePens.clear();
			penRasterScale = request.rasterScale;
			penDisplayScale = request.displayScale;
		}
	}

	SolidBrush* Brush(Color color)
	{
		if (brushes.size() >= ResourceEntryBudget && brushes.find(color.GetValue()) == brushes.end())
			brushes.clear();
		auto& brush = brushes[color.GetValue()];
		if (brush == nullptr)
		{
			auto replacement = std::make_unique<SolidBrush>(color);
			if (replacement->GetLastStatus() != Ok)
				throw std::runtime_error("AVISO brush creation failed");
			brush = std::move(replacement);
		}
		return brush.get();
	}

	Pen* Stroke(Color color, REAL width, bool roundedCaps)
	{
		std::uint32_t widthBits = 0;
		static_assert(sizeof(widthBits) == sizeof(width), "REAL must be a 32-bit float");
		std::memcpy(&widthBits, &width, sizeof(width));
		const std::uint64_t key = (static_cast<std::uint64_t>(color.GetValue()) << 32U) | widthBits;
		auto& pens = roundedCaps ? linePens : polygonPens;
		if (pens.size() >= ResourceEntryBudget && pens.find(key) == pens.end())
			pens.clear();
		auto& pen = pens[key];
		if (pen == nullptr)
		{
			auto replacement = std::make_unique<Pen>(color, width);
			if (replacement->GetLastStatus() != Ok || replacement->SetLineJoin(LineJoinRound) != Ok)
				throw std::runtime_error("AVISO pen creation failed");
			if (roundedCaps)
			{
				if (replacement->SetStartCap(LineCapRound) != Ok || replacement->SetEndCap(LineCapRound) != Ok)
					throw std::runtime_error("AVISO round pen creation failed");
			}
			pen = std::move(replacement);
		}
		return pen.get();
	}
};

VsmrAviso::AvisoRasterRenderer::AvisoRasterRenderer() : impl_(std::make_unique<Impl>())
{
}

VsmrAviso::AvisoRasterRenderer::~AvisoRasterRenderer() = default;

const VsmrAviso::AvisoRasterRenderStats& VsmrAviso::AvisoRasterRenderer::LastStats() const noexcept
{
	return impl_->stats;
}

std::unique_ptr<AvisoRasterRenderResult> VsmrAviso::AvisoRasterRenderer::Render(
	const AvisoRasterRenderRequest& request, const std::function<bool()>& cancelled)
{
	impl_->stats = {};
	if (request.features == nullptr ||
		request.labels == nullptr ||
		request.rasterWidth <= 0 ||
		request.rasterHeight <= 0 ||
		request.rasterWidth > 6400 ||
		request.rasterHeight > 6400 ||
		static_cast<std::uint64_t>(request.rasterWidth) *
			static_cast<std::uint64_t>(request.rasterHeight) > 32000000ULL)
	{
		return nullptr;
	}

	auto renderCancelled = [&]() -> bool
	{
		return cancelled && cancelled();
	};
	if (renderCancelled())
		return nullptr;
	impl_->Prepare(request);
	if (renderCancelled())
		return nullptr;

	BITMAPINFO bitmapInfo = {};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = request.rasterWidth;
	bitmapInfo.bmiHeader.biHeight = -request.rasterHeight;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;

	void* dibBits = nullptr;
	ScopedHBitmap dibBitmap;
	dibBitmap.Reset(::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &dibBits, nullptr, 0));
	if (dibBitmap.Get() == nullptr || dibBits == nullptr)
		return nullptr;

	const int rasterStride = request.rasterWidth * 4;
	auto raster = std::make_unique<Bitmap>(
		request.rasterWidth,
		request.rasterHeight,
		rasterStride,
		PixelFormat32bppPARGB,
		static_cast<BYTE*>(dibBits));
	if (raster == nullptr || raster->GetLastStatus() != Ok)
		return nullptr;

	Graphics rasterGraphics(raster.get());
	if (rasterGraphics.GetLastStatus() != Ok)
		return nullptr;

	rasterGraphics.SetPageUnit(UnitPixel);
	// Translucent backgrounds must be composed over the host on the UI thread.
	// Bake only a fully opaque color, including for direct standalone callers.
	const Color rasterBackground = request.backgroundColor.GetAlpha() == 255
		? request.backgroundColor : Color(0, 0, 0, 0);
	rasterGraphics.Clear(rasterBackground);
	rasterGraphics.SetSmoothingMode(SmoothingModeAntiAlias);
	rasterGraphics.SetPixelOffsetMode(PixelOffsetModeHalf);
	rasterGraphics.SetCompositingQuality(CompositingQualityHighSpeed);

	const double displayMinLon = request.displayMinLongitude;
	const double displayMaxLon = request.displayMaxLongitude;
	const double displayMinLat = request.displayMinLatitude;
	const double displayMaxLat = request.displayMaxLatitude;
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
		return nullptr;
	if (renderCancelled())
		return nullptr;
	const double centerLatitudeRadians = ((displayMinLat + displayMaxLat) * 0.5) * 3.14159265358979323846 / 180.0;
	const double metersPerPixelLon = (lonSpan * 111320.0 * std::cos(centerLatitudeRadians)) /
		AvisoMax(request.scaleX * lonSpan, 1.0);
	const double metersPerPixelLat = (latSpan * 110540.0) /
		AvisoMax(request.scaleY * latSpan, 1.0);
	const double metersPerPixel = AvisoMax(metersPerPixelLon, metersPerPixelLat);

	auto projectScreenPoint = [&](double longitude, double latitude) -> PointF
	{
		const double u = (longitude - displayMinLon) / lonSpan;
		const double v = (displayMaxLat - latitude) / latSpan;
		const double topX = static_cast<double>(request.projectedTopLeft.X) + static_cast<double>(request.projectedTopRight.X - request.projectedTopLeft.X) * u;
		const double bottomX = static_cast<double>(request.projectedBottomLeft.X) + static_cast<double>(request.projectedBottomRight.X - request.projectedBottomLeft.X) * u;
		const double topY = static_cast<double>(request.projectedTopLeft.Y) + static_cast<double>(request.projectedTopRight.Y - request.projectedTopLeft.Y) * u;
		const double bottomY = static_cast<double>(request.projectedBottomLeft.Y) + static_cast<double>(request.projectedBottomRight.Y - request.projectedBottomLeft.Y) * u;
		return PointF(
			static_cast<REAL>(topX + (bottomX - topX) * v),
			static_cast<REAL>(topY + (bottomY - topY) * v));
	};

	const PointF rasterRenderTopLeft = projectScreenPoint(request.renderMinLongitude, request.renderMaxLatitude);
	const PointF rasterRenderTopRight = projectScreenPoint(request.renderMaxLongitude, request.renderMaxLatitude);
	const PointF rasterRenderBottomLeft = projectScreenPoint(request.renderMinLongitude, request.renderMinLatitude);
	const PointF rasterRenderBottomRight = projectScreenPoint(request.renderMaxLongitude, request.renderMinLatitude);
	const double rasterRenderLeft = AvisoMin(AvisoMin(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMin(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderTop = AvisoMin(AvisoMin(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMin(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderRight = AvisoMax(AvisoMax(rasterRenderTopLeft.X, rasterRenderTopRight.X), AvisoMax(rasterRenderBottomLeft.X, rasterRenderBottomRight.X));
	const double rasterRenderBottom = AvisoMax(AvisoMax(rasterRenderTopLeft.Y, rasterRenderTopRight.Y), AvisoMax(rasterRenderBottomLeft.Y, rasterRenderBottomRight.Y));
	const double rasterRenderWidth = rasterRenderRight - rasterRenderLeft;
	const double rasterRenderHeight = rasterRenderBottom - rasterRenderTop;
	if (rasterRenderWidth <= 0.0 || rasterRenderHeight <= 0.0)
		return nullptr;
	const double rasterCoordinateScaleX = static_cast<double>(request.rasterWidth) / rasterRenderWidth;
	const double rasterCoordinateScaleY = static_cast<double>(request.rasterHeight) / rasterRenderHeight;

	auto projectRasterPoint = [&](const AvisoPoint& coordinate) -> PointF
	{
		const PointF screenPoint = projectScreenPoint(coordinate.longitude, coordinate.latitude);
		const double x = (static_cast<double>(screenPoint.X) - rasterRenderLeft) * rasterCoordinateScaleX;
		const double y = (static_cast<double>(screenPoint.Y) - rasterRenderTop) * rasterCoordinateScaleY;
		return PointF(static_cast<REAL>(x), static_cast<REAL>(y));
	};

	const double minRasterPointDistance = AvisoMax(0.35 * request.rasterScale, 0.5);
	const double minRasterPointDistanceSquared = minRasterPointDistance * minRasterPointDistance;
	auto appendRasterPoint = [&](std::vector<PointF>& points, const AvisoPoint& coordinate, bool preserveVertex)
	{
		++impl_->stats.projectedVertices;
		// Measure only after the full rotated projection and raster scaling.
		VsmrAviso::AppendProjectedRasterPoint(points, projectRasterPoint(coordinate),
			minRasterPointDistanceSquared, preserveVertex);
	};

	// Cull individual paths as well as features. A single MultiLineString may
	// contain thousands of distant segments, despite intersecting the viewport.
	// Test the projected bounds with an antialias/stroke guard so rotated maps
	// and strokes whose centerline lies outside the raster remain correct.
	auto pathVisible = [&](std::size_t featureIndex, std::size_t pathIndex, double strokeWidth) {
		// A singleton's path bounds are already its feature bounds. Most airport
		// polygons are short singleton rings, so avoid four redundant projections.
		if ((*request.features)[featureIndex].paths.size() == 1U)
			return true;
		const auto& bounds = impl_->scene->PathBounds(featureIndex, pathIndex);
		const PointF corners[] = {
			projectRasterPoint({ bounds.minLongitude, bounds.minLatitude }),
			projectRasterPoint({ bounds.minLongitude, bounds.maxLatitude }),
			projectRasterPoint({ bounds.maxLongitude, bounds.minLatitude }),
			projectRasterPoint({ bounds.maxLongitude, bounds.maxLatitude })
		};
		const double margin = (std::max)(strokeWidth * 0.5, 1.0) + 2.0;
		const double left = (std::min)({ corners[0].X, corners[1].X, corners[2].X, corners[3].X });
		const double right = (std::max)({ corners[0].X, corners[1].X, corners[2].X, corners[3].X });
		const double top = (std::min)({ corners[0].Y, corners[1].Y, corners[2].Y, corners[3].Y });
		const double bottom = (std::max)({ corners[0].Y, corners[1].Y, corners[2].Y, corners[3].Y });
		const bool visible = right >= -margin && left <= request.rasterWidth + margin &&
			bottom >= -margin && top <= request.rasterHeight + margin;
		if (!visible)
			++impl_->stats.pathsCulled;
		return visible;
	};

	const AvisoGeoBounds renderBounds{ request.renderMinLongitude, request.renderMinLatitude,
		request.renderMaxLongitude, request.renderMaxLatitude };
	impl_->scene->QueryFeatures(renderBounds, impl_->featureCandidates);
	impl_->scene->QueryLabels(renderBounds, impl_->labelCandidates);
	impl_->stats.featureCandidates = impl_->featureCandidates.size();
	impl_->stats.labelCandidates = impl_->labelCandidates.size();
	std::vector<PointF>& rasterPoints = impl_->points;
	for (const std::uint32_t featureIndex : impl_->featureCandidates)
	{
		const AvisoFeature& feature = (*request.features)[featureIndex];
		if (renderCancelled())
			return nullptr;
		if (!IsAvisoGroupedItemVisible(feature.groupIds, request.groupVisibility.get()))
			continue;
		if (feature.minimumZoomLevel > 0 && request.viewportZoomLevel < feature.minimumZoomLevel)
			continue;

		Color featureFillColor = request.colorPalette == "real"
			? feature.realFillColor
			: request.colorPalette == "light" ? feature.lightFillColor : feature.fillColor;
		Color featureStrokeColor = request.colorPalette == "real"
			? feature.realStrokeColor
			: request.colorPalette == "light" ? feature.lightStrokeColor : feature.strokeColor;

		if (feature.maxLatitude < request.renderMinLatitude ||
			feature.minLatitude > request.renderMaxLatitude ||
			feature.maxLongitude < request.renderMinLongitude ||
			feature.minLongitude > request.renderMaxLongitude)
		{
			continue;
		}

		const double featurePixelWidth = (feature.maxLongitude - feature.minLongitude) * request.scaleX * rasterCoordinateScaleX;
		const double featurePixelHeight = (feature.maxLatitude - feature.minLatitude) * request.scaleY * rasterCoordinateScaleY;
		if (featurePixelWidth < 0.5 && featurePixelHeight < 0.5)
			continue;
		if (feature.polygon)
		{
			const bool outline = request.colorPalette == "real" ? feature.realPolygonOutline
				: request.colorPalette == "light" ? feature.lightPolygonOutline : feature.polygonOutline;
			const bool fillVisible = featureFillColor.GetAlpha() > 0;
			const bool outlineVisible = outline && featureStrokeColor.GetAlpha() > 0 && feature.strokeWidth > 0.0f;
			if (!fillVisible && !outlineVisible)
				continue;
			const REAL strokeWidth = feature.strokeWidth * static_cast<float>(request.rasterScale * request.displayScale);
			for (std::size_t pathIndex = 0; pathIndex < feature.paths.size(); ++pathIndex)
			{
				const auto& ring = feature.paths[pathIndex];
				if (renderCancelled())
					return nullptr;
				if (ring.size() < 3)
					continue;
				if (!pathVisible(featureIndex, pathIndex, outlineVisible ? strokeWidth : 0.0))
					continue;

				rasterPoints.clear();
				rasterPoints.reserve(ring.size());
				for (size_t pointIndex = 0; pointIndex < ring.size(); ++pointIndex)
				{
					if ((pointIndex & 0xff) == 0 && renderCancelled())
						return nullptr;
					appendRasterPoint(rasterPoints, ring[pointIndex], true);
				}

				if (rasterPoints.size() < 3)
					continue;
				if (renderCancelled())
					return nullptr;

				++impl_->stats.pathsDrawn;
				if (fillVisible)
				{
					rasterGraphics.FillPolygon(impl_->Brush(featureFillColor), rasterPoints.data(), static_cast<INT>(rasterPoints.size()), FillModeAlternate);
				}
				if (outlineVisible)
				{
					rasterGraphics.DrawPolygon(impl_->Stroke(featureStrokeColor, strokeWidth, false), rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
				}
			}
			continue;
		}

		if (featureStrokeColor.GetAlpha() == 0 || feature.strokeWidth <= 0.0f)
			continue;

		const REAL strokeWidth = feature.strokeWidth * static_cast<float>(request.rasterScale * request.displayScale);
		Pen* linePen = impl_->Stroke(featureStrokeColor, strokeWidth, true);
		for (std::size_t pathIndex = 0; pathIndex < feature.paths.size(); ++pathIndex)
		{
			const auto& line = feature.paths[pathIndex];
			if (renderCancelled())
				return nullptr;
			if (line.size() < 2)
				continue;
			if (!pathVisible(featureIndex, pathIndex, strokeWidth))
				continue;

			rasterPoints.clear();
			rasterPoints.reserve(line.size());
			for (size_t pointIndex = 0; pointIndex < line.size(); ++pointIndex)
			{
				if ((pointIndex & 0xff) == 0 && renderCancelled())
					return nullptr;
				appendRasterPoint(rasterPoints, line[pointIndex], pointIndex == 0 || pointIndex + 1 == line.size());
			}

			if (rasterPoints.size() >= 2)
			{
				if (renderCancelled())
					return nullptr;
				++impl_->stats.pathsDrawn;
				rasterGraphics.DrawLines(linePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
			}
		}
	}

	auto isDenseLabelVisible = [&](const AvisoLabel& label) -> bool
	{
		const int labelZoom = request.labelZoomLevel >= 0 ? request.labelZoomLevel : request.viewportZoomLevel;
		if (label.minimumZoomLevel > 0 && labelZoom < label.minimumZoomLevel)
			return false;
		if (label.maxMetersPerPixel > 0.0 && metersPerPixel > label.maxMetersPerPixel)
			return false;
		return true;
	};
	auto labelRectForAnchor = [](const PointF& point, REAL widthPx, REAL heightPx, const std::string& anchor) -> RectF
	{
		const std::string normalizedAnchor = ToUpperAscii(anchor);
		REAL x = point.X - (widthPx * 0.5f);
		REAL y = point.Y - (heightPx * 0.5f);
		if (normalizedAnchor.find("LEFT") != std::string::npos)
			x = point.X;
		else if (normalizedAnchor.find("RIGHT") != std::string::npos)
			x = point.X - widthPx;
		if (normalizedAnchor.find("TOP") != std::string::npos)
			y = point.Y;
		else if (normalizedAnchor.find("BOTTOM") != std::string::npos)
			y = point.Y - heightPx;
		return RectF(x, y, widthPx, heightPx);
	};

	if (!request.labels->empty())
	{
		rasterGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		FontFamily fallbackLabelFontFamily(L"Arial");
		StringFormat labelFormat;
		labelFormat.SetAlignment(StringAlignmentCenter);
		labelFormat.SetLineAlignment(StringAlignmentCenter);
		labelFormat.SetFormatFlags(StringFormatFlagsNoWrap);
		auto getLabelEmSize = [&](float textSize) -> REAL
		{
			const float scaledSize = static_cast<float>(std::clamp(static_cast<double>(textSize * static_cast<float>(request.rasterScale)), 6.0, 40.0) * request.displayScale);
			const int fontKey = static_cast<int>(std::lround(static_cast<double>(scaledSize) * 10.0));
			return static_cast<REAL>(fontKey) / 10.0f;
		};

		for (const std::uint32_t labelIndex : impl_->labelCandidates)
		{
			const AvisoLabel& label = (*request.labels)[labelIndex];
			if (renderCancelled())
				return nullptr;
			if (!IsAvisoGroupedItemVisible(label.groupIds, request.groupVisibility.get()))
				continue;

			const std::wstring* renderedText = &label.text;

			if (label.position.latitude < request.renderMinLatitude ||
				label.position.latitude > request.renderMaxLatitude ||
				label.position.longitude < request.renderMinLongitude ||
				label.position.longitude > request.renderMaxLongitude ||
				renderedText->empty() ||
				!isDenseLabelVisible(label))
			{
				continue;
			}

			const REAL labelEmSize = getLabelEmSize(label.textSize);
			const PointF labelPoint = projectRasterPoint(label.position);
			const REAL textLength = static_cast<REAL>(renderedText->length());
			const REAL scaledTextSize = static_cast<REAL>(label.textSize * request.rasterScale * request.displayScale);
			const REAL haloPadding = static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * request.displayScale), 0.0) * 3.0);
			const REAL layoutWidth = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * AvisoMax(static_cast<double>(textLength), 1.0) * 0.9f + haloPadding * 2.0f), 14.0));
			const REAL layoutHeight = static_cast<REAL>(AvisoMax(static_cast<double>(scaledTextSize * 1.65f + haloPadding * 2.0f), 10.0));
			const RectF layoutRect = labelRectForAnchor(labelPoint, layoutWidth, layoutHeight, label.textAnchor);
			auto& glyph = impl_->glyphs[labelIndex];
			std::unique_ptr<GraphicsPath> uncachedGlyph;
			GraphicsPath* textPath = glyph.path.get();
			if (glyph.path == nullptr || glyph.rasterScale != request.rasterScale ||
				glyph.displayScale != request.displayScale ||
				glyph.originX != layoutRect.X || glyph.originY != layoutRect.Y)
			{
				++impl_->stats.glyphCacheMisses;
				FontFamily labelFontFamily(label.fontFamily.empty() ? L"Arial" : label.fontFamily.c_str());
				const FontFamily* fontFamily = labelFontFamily.GetLastStatus() == Ok ? &labelFontFamily : &fallbackLabelFontFamily;
				auto replacement = std::make_unique<GraphicsPath>();
				if (replacement->AddString(renderedText->c_str(), static_cast<INT>(renderedText->length()),
					fontFamily, FontStyleRegular, labelEmSize, layoutRect, &labelFormat) != Ok ||
					replacement->GetPointCount() <= 0)
				{
					// Font/GDI allocation failures may be transient. Do not turn a
					// failed outline into a permanent cache hit on subsequent builds.
					continue;
				}
				if (glyph.path != nullptr)
				{
					impl_->glyphPointCount -= static_cast<std::size_t>(glyph.path->GetPointCount());
					glyph.path.reset();
				}
				const std::size_t replacementPoints = static_cast<std::size_t>(replacement->GetPointCount());
				if (replacementPoints <= Impl::GlyphPointBudget - impl_->glyphPointCount)
				{
					impl_->glyphPointCount += replacementPoints;
					glyph.path = std::move(replacement);
					glyph.rasterScale = request.rasterScale;
					glyph.displayScale = request.displayScale;
					glyph.originX = layoutRect.X;
					glyph.originY = layoutRect.Y;
					textPath = glyph.path.get();
				}
				else
				{
					uncachedGlyph = std::move(replacement);
					textPath = uncachedGlyph.get();
				}
			}
			else
				++impl_->stats.glyphCacheHits;

			if (textPath == nullptr || textPath->GetPointCount() <= 0)
				continue;
			// GDI+ rounds glyph points at the absolute layout origin. Reusing a
			// translated outline changes a few antialias samples during panning;
			// cache only at the exact origin to retain the original raster pixels.

			const Color labelHaloColor = request.colorPalette == "real"
				? label.realHaloColor
				: request.colorPalette == "light" ? label.lightHaloColor : label.haloColor;
			const Color labelTextColor = request.colorPalette == "real"
				? label.realTextColor
				: request.colorPalette == "light" ? label.lightTextColor : label.textColor;
			if (label.haloWidth > 0.0f && labelHaloColor.GetAlpha() > 0)
			{
				rasterGraphics.DrawPath(impl_->Stroke(labelHaloColor,
					static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * request.displayScale * 2.0f), 1.0)), false), textPath);
			}

			if (labelTextColor.GetAlpha() > 0)
			{
				rasterGraphics.FillPath(impl_->Brush(labelTextColor), textPath);
			}
		}
	}

	if (renderCancelled())
		return nullptr;

	auto result = std::make_unique<AvisoRasterRenderResult>();
	result->requestId = request.requestId;
	result->labelZoomLevel = request.labelZoomLevel;
	result->viewportZoomLevel = request.viewportZoomLevel;
	result->groupGeneration = request.groupGeneration;
	result->colorPalette = request.colorPalette;
	result->backgroundColor = rasterBackground;
	result->displayScale = request.displayScale;
	result->bitmap = dibBitmap.Release();
	result->path = request.path;
	result->rasterWidth = request.rasterWidth;
	result->rasterHeight = request.rasterHeight;
	result->displayMinLongitude = request.displayMinLongitude;
	result->displayMinLatitude = request.displayMinLatitude;
	result->displayMaxLongitude = request.displayMaxLongitude;
	result->displayMaxLatitude = request.displayMaxLatitude;
	result->renderMinLongitude = request.renderMinLongitude;
	result->renderMinLatitude = request.renderMinLatitude;
	result->renderMaxLongitude = request.renderMaxLongitude;
	result->renderMaxLatitude = request.renderMaxLatitude;
	result->projectedTopLeft = request.projectedTopLeft;
	result->projectedTopRight = request.projectedTopRight;
	result->projectedBottomLeft = request.projectedBottomLeft;
	result->projectedBottomRight = request.projectedBottomRight;
	return result;
}
