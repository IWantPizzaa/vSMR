#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace VsmrAviso
{
	// Keep projection and interaction in double precision. Convert to the drawing
	// API's float coordinates only after projecting the final screen position.
	struct PointD
	{
		double X = 0.0;
		double Y = 0.0;
	};

	struct GeoPoint
	{
		double longitude = 0.0;
		double latitude = 0.0;
	};

	struct GeoBounds
	{
		double minLongitude = 0.0;
		double minLatitude = 0.0;
		double maxLongitude = 0.0;
		double maxLatitude = 0.0;
	};

	inline PointD RotatePoint(PointD point, double degrees) noexcept
	{
		const double radians = degrees * 3.14159265358979323846 / 180.0;
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		return { point.X * cosine - point.Y * sine, point.X * sine + point.Y * cosine };
	}

	template <typename Point>
	bool TryNorthRotationFromCorners(const Point& topLeft, const Point& topRight,
		const Point& bottomLeft, const Point& bottomRight, double& degrees) noexcept
	{
		// Use both complete north axes of the host viewport. Short, integer-pixel
		// north probes at the moving mouse anchor introduce visible angle noise.
		const double northX = ((static_cast<double>(topLeft.X) - static_cast<double>(bottomLeft.X)) +
			(static_cast<double>(topRight.X) - static_cast<double>(bottomRight.X))) * 0.5;
		const double northY = ((static_cast<double>(topLeft.Y) - static_cast<double>(bottomLeft.Y)) +
			(static_cast<double>(topRight.Y) - static_cast<double>(bottomRight.Y))) * 0.5;
		if (!std::isfinite(northX) || !std::isfinite(northY) || std::hypot(northX, northY) < 2.0)
			return false;
		degrees = std::atan2(northX, -northY) * 180.0 / 3.14159265358979323846;
		return true;
	}

	// The same geographic-to-screen transform is used for every frame. Unlike a
	// cached raster's axis-aligned crop, it retains the complete rotated basis and
	// the fourth-corner term of a host projection that is not quite affine.
	class BilinearProjection final
	{
	public:
		static BilinearProjection FromAffine(const GeoBounds& bounds, const GeoPoint& geographicReference,
			const PointD& screenReference, const PointD& longitudeAxis, const PointD& latitudeAxis) noexcept
		{
			BilinearProjection result;
			result.bounds_ = bounds;
			const double longitude = bounds.minLongitude - geographicReference.longitude;
			const double latitude = bounds.maxLatitude - geographicReference.latitude;
			result.origin_ = { screenReference.X + longitudeAxis.X * longitude + latitudeAxis.X * latitude,
				screenReference.Y + longitudeAxis.Y * longitude + latitudeAxis.Y * latitude };
			result.horizontal_ = { longitudeAxis.X * (bounds.maxLongitude - bounds.minLongitude),
				longitudeAxis.Y * (bounds.maxLongitude - bounds.minLongitude) };
			result.vertical_ = { -latitudeAxis.X * (bounds.maxLatitude - bounds.minLatitude),
				-latitudeAxis.Y * (bounds.maxLatitude - bounds.minLatitude) };
			// An accepted affine fit has no fourth-corner rounding residual.
			return result;
		}

		template <typename Point>
		static BilinearProjection FromCorners(const GeoBounds& bounds,
			const Point& topLeft, const Point& topRight,
			const Point& bottomLeft, const Point& bottomRight) noexcept
		{
			BilinearProjection result;
			result.bounds_ = bounds;
			result.origin_ = { static_cast<double>(topLeft.X), static_cast<double>(topLeft.Y) };
			result.horizontal_ = { static_cast<double>(topRight.X) - result.origin_.X,
				static_cast<double>(topRight.Y) - result.origin_.Y };
			result.vertical_ = { static_cast<double>(bottomLeft.X) - result.origin_.X,
				static_cast<double>(bottomLeft.Y) - result.origin_.Y };
			result.cross_ = { static_cast<double>(bottomRight.X) - result.origin_.X -
				result.horizontal_.X - result.vertical_.X,
				static_cast<double>(bottomRight.Y) - result.origin_.Y -
				result.horizontal_.Y - result.vertical_.Y };
			return result;
		}

		bool IsValid() const noexcept
		{
			return std::isfinite(bounds_.minLongitude) && std::isfinite(bounds_.maxLongitude) &&
				std::isfinite(bounds_.minLatitude) && std::isfinite(bounds_.maxLatitude) &&
				bounds_.maxLongitude > bounds_.minLongitude && bounds_.maxLatitude > bounds_.minLatitude &&
				Finite(origin_) && Finite(horizontal_) && Finite(vertical_) && Finite(cross_);
		}

		bool SameMapping(const BilinearProjection& other) const noexcept
		{
			return bounds_.minLongitude == other.bounds_.minLongitude && bounds_.maxLongitude == other.bounds_.maxLongitude &&
				bounds_.minLatitude == other.bounds_.minLatitude && bounds_.maxLatitude == other.bounds_.maxLatitude &&
				origin_.X == other.origin_.X && origin_.Y == other.origin_.Y &&
				horizontal_.X == other.horizontal_.X && horizontal_.Y == other.horizontal_.Y &&
				vertical_.X == other.vertical_.X && vertical_.Y == other.vertical_.Y &&
				cross_.X == other.cross_.X && cross_.Y == other.cross_.Y;
		}

		const GeoBounds& Bounds() const noexcept { return bounds_; }
		const PointD& Horizontal() const noexcept { return horizontal_; }
		const PointD& Vertical() const noexcept { return vertical_; }
		const PointD& CrossTerm() const noexcept { return cross_; }
		double LongitudeSpan() const noexcept { return bounds_.maxLongitude - bounds_.minLongitude; }
		double LatitudeSpan() const noexcept { return bounds_.maxLatitude - bounds_.minLatitude; }
		bool IsAffine(double tolerance = 1e-7) const noexcept
		{
			return std::abs(cross_.X) <= tolerance && std::abs(cross_.Y) <= tolerance;
		}

		PointD Project(double longitude, double latitude) const noexcept
		{
			const double u = (longitude - bounds_.minLongitude) /
				(bounds_.maxLongitude - bounds_.minLongitude);
			const double v = (bounds_.maxLatitude - latitude) /
				(bounds_.maxLatitude - bounds_.minLatitude);
			return ProjectNormalized(u, v);
		}

		PointD Project(const GeoPoint& point) const noexcept
		{
			return Project(point.longitude, point.latitude);
		}

		bool TryUnproject(const PointD& point, GeoPoint& geographic) const noexcept
		{
			if (!IsValid() || !Finite(point)) return false;
			const PointD offset{ point.X - origin_.X, point.Y - origin_.Y };
			const double determinant = Cross(horizontal_, vertical_);
			if (Singular(determinant, horizontal_, vertical_)) return false;
			double u = Cross(offset, vertical_) / determinant;
			double v = Cross(horizontal_, offset) / determinant;
			for (int iteration = 0; iteration < 12; ++iteration)
			{
				const PointD projected = ProjectNormalized(u, v);
				const PointD residual{ projected.X - point.X, projected.Y - point.Y };
				if (std::hypot(residual.X, residual.Y) <= 1e-8)
				{
					geographic = { bounds_.minLongitude + u * (bounds_.maxLongitude - bounds_.minLongitude),
						bounds_.maxLatitude - v * (bounds_.maxLatitude - bounds_.minLatitude) };
					return std::isfinite(geographic.longitude) && std::isfinite(geographic.latitude);
				}
				const PointD horizontal{ horizontal_.X + cross_.X * v, horizontal_.Y + cross_.Y * v };
				const PointD vertical{ vertical_.X + cross_.X * u, vertical_.Y + cross_.Y * u };
				const double localDeterminant = Cross(horizontal, vertical);
				if (Singular(localDeterminant, horizontal, vertical)) return false;
				u -= Cross(residual, vertical) / localDeterminant;
				v -= Cross(horizontal, residual) / localDeterminant;
				if (!std::isfinite(u) || !std::isfinite(v)) return false;
			}
			return false;
		}

	private:
		static bool Finite(const PointD& point) noexcept
		{
			return std::isfinite(point.X) && std::isfinite(point.Y);
		}

		static double Cross(const PointD& left, const PointD& right) noexcept
		{
			return left.X * right.Y - left.Y * right.X;
		}

		static bool Singular(double determinant, const PointD& horizontal, const PointD& vertical) noexcept
		{
			const double magnitude = std::hypot(horizontal.X, horizontal.Y) * std::hypot(vertical.X, vertical.Y);
			return !std::isfinite(determinant) || magnitude == 0.0 || std::abs(determinant) <= magnitude * 1e-14;
		}

		PointD ProjectNormalized(double u, double v) const noexcept
		{
			return { origin_.X + horizontal_.X * u + vertical_.X * v + cross_.X * u * v,
				origin_.Y + horizontal_.Y * u + vertical_.Y * v + cross_.Y * u * v };
		}

		GeoBounds bounds_;
		PointD origin_;
		PointD horizontal_;
		PointD vertical_;
		PointD cross_;
	};

	struct ProjectionSample
	{
		PointD screen;
		GeoPoint geographic;
	};

	inline std::array<PointD, 9> InverseProjectionSamplePoints(PointD topLeft, double width, double height) noexcept
	{
		// Pixel-to-position APIs accept integer pixels. Include complete viewport
		// axes, a fourth corner, its center and asymmetric edge-quarter probes.
		// Probe actual pixels inside the half-open viewport, so a host that clamps
		// points beyond its right/bottom edge can still supply a valid inverse.
		width = (std::max)(0.0, width - 1.0);
		height = (std::max)(0.0, height - 1.0);
		const double right = topLeft.X + width, bottom = topLeft.Y + height;
		return { topLeft, PointD{right,topLeft.Y}, PointD{topLeft.X,bottom}, PointD{right,bottom},
			PointD{topLeft.X+std::floor(width*0.5),topLeft.Y+std::floor(height*0.5)},
			PointD{topLeft.X+std::floor(width*0.25),topLeft.Y},
			PointD{topLeft.X+std::floor(width*0.75),bottom},
			PointD{topLeft.X,topLeft.Y+std::floor(height*0.75)},
			PointD{right,topLeft.Y+std::floor(height*0.25)} };
	}

	template <std::size_t SampleCount>
	bool TryRecoverAffineProjection(const GeoBounds& bounds,
		const std::array<ProjectionSample, SampleCount>& samples, BilinearProjection& recovered,
		double maximumPixelResidual = 1e-5) noexcept
	{
		static_assert(SampleCount >= 6, "Projection recovery requires independent validation samples");
		if (!std::isfinite(maximumPixelResidual) || maximumPixelResidual < 0.0) return false;
		for (const auto& sample : samples)
		{
			if (!std::isfinite(sample.screen.X) || !std::isfinite(sample.screen.Y) ||
				!std::isfinite(sample.geographic.longitude) || !std::isfinite(sample.geographic.latitude)) return false;
		}
		const GeoPoint first{ samples[1].geographic.longitude - samples[0].geographic.longitude,
			samples[1].geographic.latitude - samples[0].geographic.latitude };
		const GeoPoint second{ samples[2].geographic.longitude - samples[0].geographic.longitude,
			samples[2].geographic.latitude - samples[0].geographic.latitude };
		const PointD horizontal{ samples[1].screen.X - samples[0].screen.X, samples[1].screen.Y - samples[0].screen.Y };
		const PointD vertical{ samples[2].screen.X - samples[0].screen.X, samples[2].screen.Y - samples[0].screen.Y };
		const double determinant = first.longitude * second.latitude - first.latitude * second.longitude;
		const double magnitude = std::hypot(first.longitude, first.latitude) * std::hypot(second.longitude, second.latitude);
		if (!std::isfinite(determinant) || magnitude == 0.0 || std::abs(determinant) <= magnitude * 1e-12 ||
			horizontal.X * vertical.Y - horizontal.Y * vertical.X == 0.0) return false;
		const PointD longitudeAxis{ (horizontal.X * second.latitude - vertical.X * first.latitude) / determinant,
			(horizontal.Y * second.latitude - vertical.Y * first.latitude) / determinant };
		const PointD latitudeAxis{ (vertical.X * first.longitude - horizontal.X * second.longitude) / determinant,
			(vertical.Y * first.longitude - horizontal.Y * second.longitude) / determinant };
		const auto candidate = BilinearProjection::FromAffine(bounds, samples[0].geographic,
			samples[0].screen, longitudeAxis, latitudeAxis);
		if (!candidate.IsValid()) return false;
		// Do not replace a non-affine host projection with an approximation. Every
		// probe, including those excluded from the fit, must reproduce its actual
		// known pixel to a tolerance far below any visible rounding displacement.
		for (const auto& sample : samples)
		{
			const PointD projected = candidate.Project(sample.geographic);
			if (!std::isfinite(projected.X) || !std::isfinite(projected.Y) ||
				std::hypot(projected.X - sample.screen.X, projected.Y - sample.screen.Y) > maximumPixelResidual) return false;
		}
		recovered = candidate;
		return true;
	}

	// Exact local tangent-plane projection used by the independent AVISO inset.
	// Scale is pixels per nautical mile; viewportCenter includes half-pixels for
	// odd viewport dimensions, so drawing and mouse anchoring have the same origin.
	struct InsetProjection
	{
		static constexpr double MetersPerNm = 1852.0;
		static constexpr double LatitudeMetersPerDegree = 110540.0;
		static constexpr double LongitudeMetersPerDegree = 111320.0;

		GeoPoint center;
		PointD viewportCenter;
		double scale = 1.0;
		double rotationDegrees = 0.0;

		static double LongitudeCosine(double latitude) noexcept
		{
			return (std::max)(0.05, std::abs(std::cos(latitude * 3.14159265358979323846 / 180.0)));
		}

		double LatitudeDegreesPerPixel() const noexcept
		{
			return MetersPerNm / (scale * LatitudeMetersPerDegree);
		}

		double LongitudeDegreesPerPixel() const noexcept
		{
			return MetersPerNm / (scale * LongitudeMetersPerDegree * LongitudeCosine(center.latitude));
		}

		PointD Project(const GeoPoint& geographic) const noexcept
		{
			const PointD screen = RotatePoint({ (geographic.longitude - center.longitude) / LongitudeDegreesPerPixel(),
				(center.latitude - geographic.latitude) / LatitudeDegreesPerPixel() }, rotationDegrees);
			return { viewportCenter.X + screen.X, viewportCenter.Y + screen.Y };
		}

		GeoPoint Unproject(const PointD& screen) const noexcept
		{
			const PointD offset = RotatePoint({ screen.X - viewportCenter.X, screen.Y - viewportCenter.Y }, -rotationDegrees);
			return { center.longitude + offset.X * LongitudeDegreesPerPixel(),
				center.latitude - offset.Y * LatitudeDegreesPerPixel() };
		}

		GeoBounds Bounds(double viewportWidth, double viewportHeight) const noexcept
		{
			const double halfLongitude = viewportWidth * LongitudeDegreesPerPixel() * 0.5;
			const double halfLatitude = viewportHeight * LatitudeDegreesPerPixel() * 0.5;
			return { center.longitude - halfLongitude, center.latitude - halfLatitude,
				center.longitude + halfLongitude, center.latitude + halfLatitude };
		}

		GeoBounds VisibleBounds(double viewportWidth, double viewportHeight, double screenMargin = 0.0) const noexcept
		{
			const double halfWidth = viewportWidth * 0.5 + screenMargin;
			const double halfHeight = viewportHeight * 0.5 + screenMargin;
			const PointD corners[] = {
				{ viewportCenter.X - halfWidth, viewportCenter.Y - halfHeight },
				{ viewportCenter.X + halfWidth, viewportCenter.Y - halfHeight },
				{ viewportCenter.X - halfWidth, viewportCenter.Y + halfHeight },
				{ viewportCenter.X + halfWidth, viewportCenter.Y + halfHeight }
			};
			GeoBounds result{ center.longitude, center.latitude, center.longitude, center.latitude };
			for (const PointD& corner : corners)
			{
				const GeoPoint geographic = Unproject(corner);
				result.minLongitude = (std::min)(result.minLongitude, geographic.longitude);
				result.maxLongitude = (std::max)(result.maxLongitude, geographic.longitude);
				result.minLatitude = (std::min)(result.minLatitude, geographic.latitude);
				result.maxLatitude = (std::max)(result.maxLatitude, geographic.latitude);
			}
			return result;
		}

		InsetProjection ZoomedAt(const PointD& screen, double newScale) const noexcept
		{
			return AnchoredAt(Unproject(screen), screen, newScale, rotationDegrees);
		}

		bool TryZoomedAt(const PointD& screen, double newScale, InsetProjection& result) const noexcept
		{
			if (!std::isfinite(screen.X) || !std::isfinite(screen.Y) ||
				!std::isfinite(newScale) || newScale <= 0.0) return false;
			const GeoPoint anchor = Unproject(screen);
			const PointD offset = RotatePoint({ screen.X - viewportCenter.X, screen.Y - viewportCenter.Y }, -rotationDegrees);
			const double newLatitude = anchor.latitude + offset.Y * MetersPerNm / (newScale * LatitudeMetersPerDegree);
			// A clamped center cannot preserve this cursor anchor. Treat that step
			// as a view limit rather than changing scale and displacing the map.
			if (!std::isfinite(newLatitude) || newLatitude < -85.0 || newLatitude > 85.0) return false;
			result = AnchoredAt(anchor, screen, newScale, rotationDegrees);
			return true;
		}

		bool TryZoomedByAt(const PointD& screen, double multiplier, InsetProjection& result) const noexcept
		{
			if (!std::isfinite(scale) || scale < 1.0 || scale > 2400.0 ||
				!std::isfinite(multiplier) || multiplier <= 0.0 || multiplier == 1.0) return false;
			// Retain every fractional wheel step. Integer rounding discards small
			// deltas entirely, and a higher minimum can reverse an unzoom request
			// when restoring a valid historical scale below that minimum.
			const double newScale = std::clamp(scale * multiplier, 1.0, 2400.0);
			return newScale != scale && TryZoomedAt(screen, newScale, result);
		}

		InsetProjection AnchoredAt(const GeoPoint& anchor, const PointD& screen,
			double newScale, double newRotationDegrees) const noexcept
		{
			InsetProjection result = *this;
			result.scale = newScale;
			result.rotationDegrees = newRotationDegrees;
			const PointD offset = RotatePoint({ screen.X - viewportCenter.X, screen.Y - viewportCenter.Y }, -newRotationDegrees);
			result.center.latitude = std::clamp(anchor.latitude + offset.Y * result.LatitudeDegreesPerPixel(), -85.0, 85.0);
			// Longitude scale depends on the resulting viewport center latitude,
			// not the latitude of the anchor under the pointer.
			result.center.longitude = anchor.longitude - offset.X * result.LongitudeDegreesPerPixel();
			return result;
		}
	};
}
