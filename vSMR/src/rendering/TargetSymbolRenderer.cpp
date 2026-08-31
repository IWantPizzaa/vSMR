#include "platform/windows/PrecompiledHeader.hpp"
#include "rendering/TargetSymbolRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	Gdiplus::Color ToGdiColor(const VsmrScene::Color& color)
	{
		return Gdiplus::Color(color.alpha, color.red, color.green, color.blue);
	}

	double ClampFinite(
		double value,
		double fallback,
		double minimum,
		double maximum)
	{
		if (!std::isfinite(value))
			return fallback;
		return std::clamp(value, minimum, maximum);
	}

	BYTE BlendChannel(std::uint8_t from, BYTE to, double amount)
	{
		return static_cast<BYTE>(std::clamp(
			static_cast<int>(std::lround(
				static_cast<double>(from) +
				(static_cast<double>(to) - static_cast<double>(from)) * amount)),
			0,
			255));
	}

	RECT CenteredRect(const POINT& center, int width, int height)
	{
		width = (std::max)(1, width);
		height = (std::max)(1, height);
		RECT bounds{};
		bounds.left = center.x - width / 2;
		bounds.top = center.y - height / 2;
		bounds.right = bounds.left + width;
		bounds.bottom = bounds.top + height;
		return bounds;
	}

	class BoundsBuilder
	{
	public:
		void Add(const RECT& bounds)
		{
			if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
				return;
			if (!m_Valid)
			{
				m_Bounds = bounds;
				m_Valid = true;
				return;
			}

			m_Bounds.left = (std::min)(m_Bounds.left, bounds.left);
			m_Bounds.top = (std::min)(m_Bounds.top, bounds.top);
			m_Bounds.right = (std::max)(m_Bounds.right, bounds.right);
			m_Bounds.bottom = (std::max)(m_Bounds.bottom, bounds.bottom);
		}

		void AddPoints(
			const Gdiplus::PointF* points,
			std::size_t pointCount,
			float padding = 0.0f)
		{
			if (points == nullptr || pointCount == 0)
				return;

			float minimumX = points[0].X;
			float maximumX = points[0].X;
			float minimumY = points[0].Y;
			float maximumY = points[0].Y;
			for (std::size_t index = 1; index < pointCount; ++index)
			{
				const Gdiplus::PointF& point = points[index];
				minimumX = (std::min)(minimumX, point.X);
				maximumX = (std::max)(maximumX, point.X);
				minimumY = (std::min)(minimumY, point.Y);
				maximumY = (std::max)(maximumY, point.Y);
			}
			Add(RECT{
				static_cast<LONG>(std::floor(minimumX - padding)),
				static_cast<LONG>(std::floor(minimumY - padding)),
				static_cast<LONG>(std::ceil(maximumX + padding)) + 1,
				static_cast<LONG>(std::ceil(maximumY + padding)) + 1 });
		}

		void AddPoints(const std::vector<Gdiplus::PointF>& points, float padding = 0.0f)
		{
			AddPoints(points.data(), points.size(), padding);
		}

		bool Valid() const
		{
			return m_Valid;
		}

		RECT Get() const
		{
			return m_Bounds;
		}

	private:
		RECT m_Bounds{};
		bool m_Valid = false;
	};

	VsmrScene::GeoPoint DestinationPoint(
		const VsmrScene::GeoPoint& origin,
		double headingDegrees,
		double distanceMeters)
	{
		const double angularDistance = (distanceMeters * 0.00053996) / 60.0 * kPi / 180.0;
		const double track = headingDegrees / 180.0 * kPi;
		const double sourceLatitude = origin.latitude / 180.0 * kPi;
		const double sourceLongitude = origin.longitude / 180.0 * kPi;
		const double latitude = std::asin(
			std::sin(sourceLatitude) * std::cos(angularDistance) +
			std::cos(sourceLatitude) * std::sin(angularDistance) * std::cos(track));
		const double longitude = std::cos(latitude) == 0.0
			? sourceLongitude
			: std::fmod(
				sourceLongitude +
				std::asin(std::sin(track) * std::sin(angularDistance) / std::cos(latitude)) +
				kPi,
				2.0 * kPi) - kPi;

		return VsmrScene::GeoPoint{
			latitude / kPi * 180.0,
			longitude / kPi * 180.0,
			true };
	}

	double NormalizeHeading(double headingDegrees)
	{
		if (!std::isfinite(headingDegrees))
			return 0.0;
		double normalized = std::fmod(headingDegrees, 360.0);
		return normalized < 0.0 ? normalized + 360.0 : normalized;
	}

	void ProjectPolygon(
		const std::vector<VsmrScene::GeoPoint>& source,
		const std::function<POINT(const VsmrScene::GeoPoint&)>& projectPoint,
		double symbolScale,
		std::vector<Gdiplus::PointF>& polygon)
	{
		polygon.clear();
		polygon.reserve(source.size());
		for (const VsmrScene::GeoPoint& sourcePoint : source)
		{
			if (!sourcePoint.valid)
				continue;
			const POINT point = projectPoint(sourcePoint);
			polygon.emplace_back(
				static_cast<Gdiplus::REAL>(point.x),
				static_cast<Gdiplus::REAL>(point.y));
		}

		if (polygon.size() < 3 || std::abs(symbolScale - 1.0) <= 0.0001)
			return;

		Gdiplus::REAL centerX = 0.0f;
		Gdiplus::REAL centerY = 0.0f;
		for (const Gdiplus::PointF& point : polygon)
		{
			centerX += point.X;
			centerY += point.Y;
		}
		centerX /= static_cast<Gdiplus::REAL>(polygon.size());
		centerY /= static_cast<Gdiplus::REAL>(polygon.size());
		for (Gdiplus::PointF& point : polygon)
		{
			point.X = centerX + static_cast<Gdiplus::REAL>((point.X - centerX) * symbolScale);
			point.Y = centerY + static_cast<Gdiplus::REAL>((point.Y - centerY) * symbolScale);
		}
	}

	bool DrawPrimaryPolygon(
		Gdiplus::Graphics& graphics,
		const std::vector<VsmrScene::GeoPoint>& source,
		const std::function<POINT(const VsmrScene::GeoPoint&)>& projectPoint,
		double symbolScale,
		const Gdiplus::Color& color,
		BoundsBuilder& bounds,
		std::vector<Gdiplus::PointF>& polygon)
	{
		ProjectPolygon(source, projectPoint, symbolScale, polygon);
		if (polygon.size() < 3)
			return false;

		Gdiplus::SolidBrush brush(color);
		graphics.FillPolygon(&brush, polygon.data(), static_cast<INT>(polygon.size()));
		bounds.AddPoints(polygon);
		return true;
	}

	bool DrawTrails(
		Gdiplus::Graphics& graphics,
		const VsmrScene::Target& target,
		const VsmrScene::TargetPresentation& presentation,
		const std::function<POINT(const VsmrScene::GeoPoint&)>& projectPoint,
		const std::function<bool(const POINT&, int)>& pointVisible,
		double symbolScale,
		BoundsBuilder& bounds,
		std::vector<Gdiplus::PointF>& polygon)
	{
		if (!presentation.trailEnabled)
			return false;

		bool drawn = false;
		if (target.style.icon == VsmrScene::IconStyle::Nova)
		{
			constexpr BYTE afterglowChannels[] = { 255, 219, 183 };
			for (int historyIndex = 2; historyIndex >= 0; --historyIndex)
			{
				const std::vector<VsmrScene::GeoPoint>& source =
					target.primaryReturnAfterglow[static_cast<std::size_t>(historyIndex)];
				ProjectPolygon(source, projectPoint, symbolScale, polygon);
				if (polygon.size() < 3)
					continue;

				const BYTE channel = afterglowChannels[historyIndex];
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, channel, channel));
				graphics.FillPolygon(&brush, polygon.data(), static_cast<INT>(polygon.size()));
				bounds.AddPoints(polygon);
				drawn = true;
			}
		}

		const std::size_t pointCount = target.trailPositions.size();
		const VsmrScene::Color& sourceColor = target.style.color;
		for (std::size_t index = 0; index < pointCount; ++index)
		{
			const VsmrScene::GeoPoint& history = target.trailPositions[index];
			if (!history.valid)
				continue;
			const POINT point = projectPoint(history);
			if (pointVisible && !pointVisible(point, 7))
				continue;

			const double age = pointCount > 1
				? static_cast<double>(index) / static_cast<double>(pointCount - 1)
				: 0.0;
			if (target.style.icon == VsmrScene::IconStyle::Nova)
			{
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
				graphics.FillRectangle(&brush, point.x - 1, point.y - 1, 2, 2);
				bounds.Add(RECT{ point.x - 1, point.y - 1, point.x + 1, point.y + 1 });
				drawn = true;
				continue;
			}

			const BYTE red = BlendChannel(sourceColor.red, 112, age);
			const BYTE green = BlendChannel(sourceColor.green, 112, age);
			const BYTE blue = BlendChannel(sourceColor.blue, 112, age);
			const BYTE newestAlpha = static_cast<BYTE>((std::min)(static_cast<int>(sourceColor.alpha), 220));
			const BYTE alpha = BlendChannel(newestAlpha, 38, age);
			if (target.style.icon == VsmrScene::IconStyle::Realistic)
			{
				const int diameter = std::clamp(
					static_cast<int>(std::lround(5.0 - age * 2.0)),
					2,
					5);
				const int radius = diameter / 2;
				Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, red, green, blue));
				graphics.FillEllipse(&brush, point.x - radius, point.y - radius, diameter, diameter);
				bounds.Add(RECT{
					point.x - radius,
					point.y - radius,
					point.x - radius + diameter,
					point.y - radius + diameter });
				drawn = true;
				continue;
			}

			const int diameter = std::clamp(
				static_cast<int>(std::lround(9.0 - age * 4.0)),
				4,
				9);
			const int radius = diameter / 2;
			Gdiplus::Pen pen(Gdiplus::Color(alpha, red, green, blue), 1.5f);
			graphics.DrawEllipse(&pen, point.x - radius, point.y - radius, diameter, diameter);
			bounds.Add(RECT{
				point.x - radius - 1,
				point.y - radius - 1,
				point.x - radius + diameter + 1,
				point.y - radius + diameter + 1 });
			drawn = true;
		}
		return drawn;
	}

	double ScreenRotationDegrees(
		const VsmrScene::Target& target,
		const POINT& center,
		const std::function<POINT(const VsmrScene::GeoPoint&)>& projectPoint)
	{
		VsmrScene::GeoPoint headingProbe = target.headingProbe;
		if (!headingProbe.valid && target.position.valid)
			headingProbe = DestinationPoint(target.position, NormalizeHeading(target.headingTrueDegrees), 50.0);
		if (!headingProbe.valid)
			return 0.0;

		const POINT headingPoint = projectPoint(headingProbe);
		const double forwardX = static_cast<double>(headingPoint.x - center.x);
		const double forwardY = static_cast<double>(headingPoint.y - center.y);
		if (!std::isfinite(forwardX) || !std::isfinite(forwardY) ||
			std::hypot(forwardX, forwardY) < 0.01)
		{
			return 0.0;
		}
		return std::atan2(forwardY, forwardX) * 180.0 / kPi + 90.0;
	}

	RECT BoundsFromPoints(const Gdiplus::PointF* points, std::size_t pointCount)
	{
		BoundsBuilder bounds;
		bounds.AddPoints(points, pointCount);
		return bounds.Get();
	}
}

namespace VsmrTargetRendering
{
	Frame::Frame(Gdiplus::Graphics& graphics, FrameSettings settings)
		: m_Graphics(graphics),
		  m_Settings(std::move(settings)),
		  m_SavedInterpolationMode(graphics.GetInterpolationMode()),
		  m_SavedPixelOffsetMode(graphics.GetPixelOffsetMode()),
		  m_SavedCompositingQuality(graphics.GetCompositingQuality())
	{
		m_Settings.presentation.symbolScale = ClampFinite(
			m_Settings.presentation.symbolScale,
			1.0,
			0.5,
			1.5);
		m_Settings.pixelsPerMeter = ClampFinite(
			m_Settings.pixelsPerMeter,
			0.0,
			0.0,
			1000.0);
		const bool realisticPresentation =
			m_Settings.presentation.icon == VsmrScene::IconStyle::Realistic;
		if (realisticPresentation && m_Settings.iconCache.beginFrame)
			m_CacheFrame = m_Settings.iconCache.beginFrame();

		m_FastBitmapMode =
			m_Settings.optimizeRealisticBitmapQuality && realisticPresentation;
		if (m_FastBitmapMode)
		{
			m_Graphics.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
			m_Graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighSpeed);
			m_Graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
		}
	}

	Frame::~Frame()
	{
		if (!m_FastBitmapMode)
			return;
		m_Graphics.SetInterpolationMode(m_SavedInterpolationMode);
		m_Graphics.SetPixelOffsetMode(m_SavedPixelOffsetMode);
		m_Graphics.SetCompositingQuality(m_SavedCompositingQuality);
	}

	DrawResult Frame::DrawTarget(
		const VsmrScene::Target& target,
		const DrawOptions& options)
	{
		DrawResult result;
		if (!target.position.valid || !m_Settings.projectPoint)
			return result;

		auto trace = [&](const char* step)
		{
			if (m_Settings.trace)
				m_Settings.trace(target, step);
		};
		trace("begin");

		result.center = m_Settings.projectPoint(target.position);
		const double symbolScale = m_Settings.presentation.symbolScale;
		BoundsBuilder visualBounds;
		if (options.drawTrail)
		{
			result.trailDrawn = DrawTrails(
				m_Graphics,
				target,
				m_Settings.presentation,
				m_Settings.projectPoint,
				m_Settings.pointVisible,
				symbolScale,
				visualBounds,
				m_PolygonScratch);
		}

		if (options.drawPrimaryReturn &&
			target.style.icon == VsmrScene::IconStyle::Nova &&
			target.style.showPrimaryReturn)
		{
			result.primaryReturnDrawn = DrawPrimaryPolygon(
				m_Graphics,
				target.primaryReturnPolygon,
				m_Settings.projectPoint,
				symbolScale,
				ToGdiColor(target.style.primaryReturnColor),
				visualBounds,
				m_PolygonScratch);
		}

		const Gdiplus::Color targetColor = ToGdiColor(target.style.color);
		int nominalIconSize = 12;
		if (target.style.icon == VsmrScene::IconStyle::Nova)
		{
			trace("nova");
			Gdiplus::Pen symbolPen(Gdiplus::Color(255, 255, 255, 255), 1.0f);
			const Gdiplus::REAL scale = static_cast<Gdiplus::REAL>(symbolScale);
			if (target.transponderModeC)
			{
				Gdiplus::PointF points[] = {
					{ static_cast<Gdiplus::REAL>(result.center.x), static_cast<Gdiplus::REAL>(result.center.y) - 6.0f * scale },
					{ static_cast<Gdiplus::REAL>(result.center.x) - 6.0f * scale, static_cast<Gdiplus::REAL>(result.center.y) },
					{ static_cast<Gdiplus::REAL>(result.center.x), static_cast<Gdiplus::REAL>(result.center.y) + 6.0f * scale },
					{ static_cast<Gdiplus::REAL>(result.center.x) + 6.0f * scale, static_cast<Gdiplus::REAL>(result.center.y) },
					{ static_cast<Gdiplus::REAL>(result.center.x), static_cast<Gdiplus::REAL>(result.center.y) - 6.0f * scale }
				};
				m_Graphics.DrawLines(&symbolPen, points, static_cast<INT>(_countof(points)));
				result.symbolBounds = BoundsFromPoints(points, _countof(points));
			}
			else
			{
				const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>(result.center.x);
				const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>(result.center.y);
				m_Graphics.DrawLine(&symbolPen, centerX, centerY, centerX - 4.0f * scale, centerY - 4.0f * scale);
				m_Graphics.DrawLine(&symbolPen, centerX, centerY, centerX + 4.0f * scale, centerY - 4.0f * scale);
				m_Graphics.DrawLine(&symbolPen, centerX, centerY, centerX - 4.0f * scale, centerY + 4.0f * scale);
				m_Graphics.DrawLine(&symbolPen, centerX, centerY, centerX + 4.0f * scale, centerY + 4.0f * scale);
				const int extent = static_cast<int>(std::ceil(4.0 * symbolScale));
				result.symbolBounds = CenteredRect(result.center, extent * 2, extent * 2);
			}
			nominalIconSize = static_cast<int>(std::ceil(12.0 * symbolScale));
		}
		else
		{
			Gdiplus::Bitmap* sourceBitmap = nullptr;
			if (target.style.icon == VsmrScene::IconStyle::Realistic &&
				m_Settings.iconCache.getSourceBitmap)
			{
				sourceBitmap = m_Settings.iconCache.getSourceBitmap(target.style.assetKey);
			}

			const bool sourceBitmapValid =
				sourceBitmap != nullptr &&
				sourceBitmap->GetLastStatus() == Gdiplus::Ok &&
				sourceBitmap->GetWidth() > 0 &&
				sourceBitmap->GetHeight() > 0;
			if (sourceBitmapValid)
			{
				trace("realistic");
				double drawWidth = 40.0;
				double drawHeight = 40.0;
				if (m_Settings.pixelsPerMeter > 0.0)
				{
					drawWidth = target.style.wingspanMeters * m_Settings.pixelsPerMeter * symbolScale;
					drawHeight = target.style.lengthMeters * m_Settings.pixelsPerMeter * symbolScale;
				}
				drawWidth = ClampFinite(drawWidth, 1.0, 1.0, 1200.0);
				drawHeight = ClampFinite(drawHeight, 1.0, 1.0, 1200.0);

				int pixelWidth = std::clamp(static_cast<int>(std::lround(drawWidth)), 1, 2048);
				int pixelHeight = std::clamp(static_cast<int>(std::lround(drawHeight)), 1, 2048);
				std::string scaledCacheKey;
				Gdiplus::Bitmap* scaledBitmap = nullptr;
				if (m_Settings.iconCache.getScaledBitmap)
				{
					scaledBitmap = m_Settings.iconCache.getScaledBitmap(
						target.style.assetKey,
						sourceBitmap,
						sourceBitmap->GetWidth(),
						sourceBitmap->GetHeight(),
						targetColor,
						drawWidth,
						drawHeight,
						m_CacheFrame,
						pixelWidth,
						pixelHeight,
						scaledCacheKey);
				}
				pixelWidth = std::clamp(pixelWidth, 1, 2048);
				pixelHeight = std::clamp(pixelHeight, 1, 2048);

				const double rotationDegrees = ScreenRotationDegrees(
					target,
					result.center,
					m_Settings.projectPoint);
				CachedBitmap rotatedBitmap;
				if (scaledBitmap != nullptr &&
					!scaledCacheKey.empty() &&
					m_Settings.iconCache.getRotatedBitmap)
				{
					rotatedBitmap = m_Settings.iconCache.getRotatedBitmap(
						scaledCacheKey,
						scaledBitmap,
						pixelWidth,
						pixelHeight,
						rotationDegrees,
						m_CacheFrame);
				}

				if (rotatedBitmap.bitmap != nullptr &&
					rotatedBitmap.bitmap->GetLastStatus() == Gdiplus::Ok &&
					rotatedBitmap.bitmap->GetWidth() > 0 &&
					rotatedBitmap.bitmap->GetHeight() > 0)
				{
					const int bitmapWidth = static_cast<int>(rotatedBitmap.bitmap->GetWidth());
					const int bitmapHeight = static_cast<int>(rotatedBitmap.bitmap->GetHeight());
					m_Graphics.DrawImage(
						rotatedBitmap.bitmap,
						static_cast<INT>(result.center.x - rotatedBitmap.centerX),
						static_cast<INT>(result.center.y - rotatedBitmap.centerY));
					result.symbolBounds = RECT{
						result.center.x - rotatedBitmap.centerX,
						result.center.y - rotatedBitmap.centerY,
						result.center.x - rotatedBitmap.centerX + bitmapWidth,
						result.center.y - rotatedBitmap.centerY + bitmapHeight };
					nominalIconSize = (std::max)(bitmapWidth, bitmapHeight);
				}
				else
				{
					Gdiplus::GraphicsState state = m_Graphics.Save();
					Gdiplus::Matrix transform;
					transform.Translate(
						static_cast<Gdiplus::REAL>(result.center.x),
						static_cast<Gdiplus::REAL>(result.center.y));
					transform.Rotate(static_cast<Gdiplus::REAL>(rotationDegrees));
					transform.Translate(
						static_cast<Gdiplus::REAL>(-pixelWidth / 2.0),
						static_cast<Gdiplus::REAL>(-pixelHeight / 2.0));
					m_Graphics.SetTransform(&transform);

					if (scaledBitmap != nullptr)
					{
						m_Graphics.DrawImage(scaledBitmap, 0, 0);
					}
					else
					{
						const Gdiplus::REAL tintAlpha = static_cast<Gdiplus::REAL>(targetColor.GetAlpha()) / 255.0f;
						Gdiplus::ColorMatrix colorMatrix = {
							{
								{ static_cast<Gdiplus::REAL>(targetColor.GetR()) / 255.0f, 0.0f, 0.0f, 0.0f, 0.0f },
								{ 0.0f, static_cast<Gdiplus::REAL>(targetColor.GetG()) / 255.0f, 0.0f, 0.0f, 0.0f },
								{ 0.0f, 0.0f, static_cast<Gdiplus::REAL>(targetColor.GetB()) / 255.0f, 0.0f, 0.0f },
								{ 0.0f, 0.0f, 0.0f, tintAlpha, 0.0f },
								{ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }
							}
						};
						Gdiplus::ImageAttributes attributes;
						attributes.SetColorMatrix(
							&colorMatrix,
							Gdiplus::ColorMatrixFlagsDefault,
							Gdiplus::ColorAdjustTypeBitmap);
						Gdiplus::RectF destination(
							0.0f,
							0.0f,
							static_cast<Gdiplus::REAL>(pixelWidth),
							static_cast<Gdiplus::REAL>(pixelHeight));
						m_Graphics.DrawImage(
							sourceBitmap,
							destination,
							0.0f,
							0.0f,
							static_cast<Gdiplus::REAL>(sourceBitmap->GetWidth()),
							static_cast<Gdiplus::REAL>(sourceBitmap->GetHeight()),
							Gdiplus::UnitPixel,
							&attributes);
					}
					m_Graphics.Restore(state);

					const double rotationRadians = rotationDegrees * kPi / 180.0;
					const double absoluteCosine = std::abs(std::cos(rotationRadians));
					const double absoluteSine = std::abs(std::sin(rotationRadians));
					const int rotatedWidth = static_cast<int>(std::ceil(
						pixelWidth * absoluteCosine + pixelHeight * absoluteSine));
					const int rotatedHeight = static_cast<int>(std::ceil(
						pixelWidth * absoluteSine + pixelHeight * absoluteCosine));
					result.symbolBounds = CenteredRect(result.center, rotatedWidth, rotatedHeight);
					nominalIconSize = (std::max)(rotatedWidth, rotatedHeight);
				}
				result.realisticBitmapDrawn = true;
			}
			else
			{
				const double lengthPixels = ClampFinite(
					m_Settings.pixelsPerMeter * 20.0 * symbolScale,
					1.0,
					0.5,
					220.0);
				const double halfWidthPixels = ClampFinite(
					m_Settings.pixelsPerMeter * 12.0 * symbolScale,
					0.5,
					0.35,
					110.0);

				if (target.style.icon == VsmrScene::IconStyle::Diamond)
				{
					trace("diamond");
					const double diagonalPixels = std::clamp(lengthPixels + halfWidthPixels, 10.0, 220.0);
					const double sidePixels = diagonalPixels / std::sqrt(2.0);
					const double halfSide = sidePixels / 2.0;
					const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(result.center.x - halfSide);
					const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(result.center.y - halfSide);
					const Gdiplus::REAL side = static_cast<Gdiplus::REAL>(sidePixels);
					const Gdiplus::REAL radius = std::clamp(
						static_cast<Gdiplus::REAL>(sidePixels * 0.22),
						2.0f,
						static_cast<Gdiplus::REAL>(sidePixels / 2.0));
					const Gdiplus::REAL diameter = radius * 2.0f;

					Gdiplus::GraphicsPath path;
					path.AddArc(left, top, diameter, diameter, 180.0f, 90.0f);
					path.AddArc(left + side - diameter, top, diameter, diameter, 270.0f, 90.0f);
					path.AddArc(left + side - diameter, top + side - diameter, diameter, diameter, 0.0f, 90.0f);
					path.AddArc(left, top + side - diameter, diameter, diameter, 90.0f, 90.0f);
					path.CloseFigure();

					const double rotationDegrees = ScreenRotationDegrees(
						target,
						result.center,
						m_Settings.projectPoint) - 45.0;
					Gdiplus::GraphicsState state = m_Graphics.Save();
					Gdiplus::Matrix transform;
					transform.RotateAt(
						static_cast<Gdiplus::REAL>(rotationDegrees),
						Gdiplus::PointF(
							static_cast<Gdiplus::REAL>(result.center.x),
							static_cast<Gdiplus::REAL>(result.center.y)));
					m_Graphics.MultiplyTransform(&transform);
					Gdiplus::SolidBrush brush(targetColor);
					m_Graphics.FillPath(&brush, &path);
					m_Graphics.Restore(state);

					nominalIconSize = static_cast<int>((std::max)(12.0, diagonalPixels));
					result.symbolBounds = CenteredRect(result.center, nominalIconSize, nominalIconSize);
				}
				else
				{
					trace("arrow");
					const double rotationDegrees = ScreenRotationDegrees(
						target,
						result.center,
						m_Settings.projectPoint);
					const double forwardRadians = (rotationDegrees - 90.0) * kPi / 180.0;
					const double forwardX = std::cos(forwardRadians);
					const double forwardY = std::sin(forwardRadians);
					const double rightX = -forwardY;
					const double rightY = forwardX;
					const double centerX = static_cast<double>(result.center.x);
					const double centerY = static_cast<double>(result.center.y);
					const double baseX = centerX - forwardX * lengthPixels * 0.33;
					const double baseY = centerY - forwardY * lengthPixels * 0.33;
					Gdiplus::PointF points[] = {
						{ static_cast<Gdiplus::REAL>(centerX + forwardX * lengthPixels), static_cast<Gdiplus::REAL>(centerY + forwardY * lengthPixels) },
						{ static_cast<Gdiplus::REAL>(baseX + rightX * halfWidthPixels), static_cast<Gdiplus::REAL>(baseY + rightY * halfWidthPixels) },
						{ static_cast<Gdiplus::REAL>(centerX - forwardX * lengthPixels * 0.05), static_cast<Gdiplus::REAL>(centerY - forwardY * lengthPixels * 0.05) },
						{ static_cast<Gdiplus::REAL>(baseX - rightX * halfWidthPixels), static_cast<Gdiplus::REAL>(baseY - rightY * halfWidthPixels) }
					};
					Gdiplus::SolidBrush brush(targetColor);
					m_Graphics.FillPolygon(&brush, points, static_cast<INT>(_countof(points)));
					result.symbolBounds = BoundsFromPoints(points, _countof(points));
					nominalIconSize = static_cast<int>((std::max)(12.0, lengthPixels + halfWidthPixels));
				}
			}
		}

		visualBounds.Add(result.symbolBounds);
		result.visualBounds = visualBounds.Valid() ? visualBounds.Get() : result.symbolBounds;
		const int hitSize = (std::max)((std::max)(1, options.minimumHitSize), nominalIconSize);
		result.hitBounds = CenteredRect(result.center, hitSize, hitSize);
		result.drawn = true;
		trace("end");
		return result;
	}
}
