// Benchmark-only extraction of e774ac5 (dev). Production does not compile this file.
// Renderer body is preserved; host cancellation is replaced by the request token.
#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>
#include "radar/RadarScreenTypes.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"

#include "BenchmarkFixture.hpp"
#include <algorithm>
#include <cmath>
using namespace Gdiplus;
using namespace VsmrRadarTypes;
using VsmrAvisoSupport::AvisoMax;
using VsmrAvisoSupport::AvisoMin;
using VsmrAvisoSupport::ToUpperAscii;
namespace {
    // Frozen original vertex filter: future production changes must not alter the baseline.
    void AppendBaselineProjectedRasterPoint(std::vector<PointF>& points, const PointF& point,
        double minDistanceSquared, bool preserveVertex)
    {
        if (!points.empty())
        {
            const double dx = static_cast<double>(point.X) - points.back().X;
            const double dy = static_cast<double>(point.Y) - points.back().Y;
            if (dx == 0.0 && dy == 0.0) return;
            if (!preserveVertex && dx * dx + dy * dy < minDistanceSquared) return;
        }
        points.push_back(point);
    }
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
namespace VsmrAvisoBenchmark {
std::unique_ptr<AvisoRasterRenderResult> RenderBaseline(const AvisoRasterRenderRequest& request)
{
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
		return request.cancellationToken != nullptr && request.cancellationToken->load(std::memory_order_acquire) != request.requestId;
	};
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
	rasterGraphics.Clear(Color(0, 0, 0, 0));
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
		// Measure only after the full rotated projection and raster scaling.
		AppendBaselineProjectedRasterPoint(points, projectRasterPoint(coordinate),
			minRasterPointDistanceSquared, preserveVertex);
	};

	std::vector<PointF> rasterPoints;
	for (const AvisoFeature& feature : *request.features)
	{
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
			for (const std::vector<AvisoPoint>& ring : feature.paths)
			{
				if (renderCancelled())
					return nullptr;
				if (ring.size() < 3)
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

				if (featureFillColor.GetAlpha() > 0)
				{
					SolidBrush fillBrush(featureFillColor);
					rasterGraphics.FillPolygon(&fillBrush, rasterPoints.data(), static_cast<INT>(rasterPoints.size()), FillModeAlternate);
				}
				if (outline && featureStrokeColor.GetAlpha() > 0 && feature.strokeWidth > 0.0f)
				{
					Pen outlinePen(featureStrokeColor, feature.strokeWidth * static_cast<float>(request.rasterScale * request.displayScale));
					outlinePen.SetLineJoin(LineJoinRound);
					rasterGraphics.DrawPolygon(&outlinePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
				}
			}
			continue;
		}

		if (featureStrokeColor.GetAlpha() == 0 || feature.strokeWidth <= 0.0f)
			continue;

		Pen linePen(featureStrokeColor, feature.strokeWidth * static_cast<float>(request.rasterScale * request.displayScale));
		linePen.SetLineJoin(LineJoinRound);
		linePen.SetStartCap(LineCapRound);
		linePen.SetEndCap(LineCapRound);
		for (const std::vector<AvisoPoint>& line : feature.paths)
		{
			if (renderCancelled())
				return nullptr;
			if (line.size() < 2)
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
				rasterGraphics.DrawLines(&linePen, rasterPoints.data(), static_cast<INT>(rasterPoints.size()));
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

		for (const AvisoLabel& label : *request.labels)
		{
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
			FontFamily labelFontFamily(label.fontFamily.empty() ? L"Arial" : label.fontFamily.c_str());
			const FontFamily* fontFamily = labelFontFamily.GetLastStatus() == Ok ? &labelFontFamily : &fallbackLabelFontFamily;

			GraphicsPath textPath;
			textPath.AddString(
				renderedText->c_str(),
				static_cast<INT>(renderedText->length()),
				fontFamily,
				FontStyleRegular,
				labelEmSize,
				layoutRect,
				&labelFormat);

			if (textPath.GetPointCount() <= 0)
				continue;

			const Color labelHaloColor = request.colorPalette == "real"
				? label.realHaloColor
				: request.colorPalette == "light" ? label.lightHaloColor : label.haloColor;
			const Color labelTextColor = request.colorPalette == "real"
				? label.realTextColor
				: request.colorPalette == "light" ? label.lightTextColor : label.textColor;
			if (label.haloWidth > 0.0f && labelHaloColor.GetAlpha() > 0)
			{
				Pen haloPen(labelHaloColor, static_cast<REAL>(AvisoMax(static_cast<double>(label.haloWidth * request.rasterScale * request.displayScale * 2.0f), 1.0)));
				haloPen.SetLineJoin(LineJoinRound);
				rasterGraphics.DrawPath(&haloPen, &textPath);
			}

			if (labelTextColor.GetAlpha() > 0)
			{
				SolidBrush textBrush(labelTextColor);
				rasterGraphics.FillPath(&textBrush, &textPath);
			}
		}
	}

	if (renderCancelled())
		return nullptr;

	auto result = std::make_unique<AvisoRasterRenderResult>();
	result->requestId = request.requestId;
	result->labelZoomLevel = request.labelZoomLevel;
	result->groupGeneration = request.groupGeneration;
	result->colorPalette = request.colorPalette;
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

}
