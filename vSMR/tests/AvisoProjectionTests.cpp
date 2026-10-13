#include "AvisoProjectionTests.hpp"
#include "aviso/AvisoProjection.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace
{
	using VsmrAviso::BilinearProjection;
	using VsmrAviso::GeoBounds;
	using VsmrAviso::GeoPoint;
	using VsmrAviso::InsetProjection;
	using VsmrAviso::PointD;
	using Failures = std::vector<std::string>;

	void Check(bool condition, const char* message, Failures& failures)
	{
		if (!condition) failures.emplace_back(message);
	}

	double Distance(const PointD& left, const PointD& right)
	{
		return std::hypot(left.X - right.X, left.Y - right.Y);
	}

	void TestCursorAnchoring(Failures& failures)
	{
		for (const double latitude : { -80.0, -48.0, 0.0, 48.0, 80.0 })
		{
			for (const double degrees : { -175.0, -90.0, -35.0, 0.0, 35.0, 90.0, 179.0, 360.0 })
			{
				for (const PointD extent : { PointD{ 1000.0, 700.0 }, PointD{ 1001.0, 701.0 }, PointD{ 1919.0, 1079.0 } })
				{
					const InsetProjection original{ { 2.55, latitude }, { 137.0 + extent.X * 0.5, 73.0 + extent.Y * 0.5 }, 20.0, degrees };
					for (const PointD cursor : { original.viewportCenter,
						PointD{ 138.0, 74.0 }, PointD{ 136.0 + extent.X, 72.0 + extent.Y },
						PointD{ 143.0 + extent.X * 0.73, 85.0 + extent.Y * 0.27 } })
					{
						const GeoPoint anchor = original.Unproject(cursor);
						Check(Distance(original.Project(anchor), cursor) < 1e-7,
							"AVISO inverse projection retains the exact mouse pixel at every tested rotation", failures);
						auto projection = original;
						for (int cycle = 0; cycle < 30; ++cycle)
						{
							for (const double scale : { 24.0, 31.0, 120.0, 800.0, 2400.0, 400.0, 60.0, 20.0 })
							{
								projection = projection.ZoomedAt(cursor, scale);
								Check(Distance(projection.Project(anchor), cursor) < 1e-6,
									"Repeated AVISO zoom/unzoom keeps one geographic mouse anchor at the identical screen pixel", failures);
							}
						}
						Check(std::abs(projection.center.longitude - original.center.longitude) < 1e-10 &&
							std::abs(projection.center.latitude - original.center.latitude) < 1e-10,
							"AVISO zoom round trips introduce no accumulating geographic center drift", failures);
					}
					const PointD start{ original.viewportCenter.X + 75.0, original.viewportCenter.Y - 51.0 };
					const GeoPoint anchor = original.Unproject(start);
					const PointD moved{ start.X + 187.0, start.Y - 131.0 };
					const auto panned = original.AnchoredAt(anchor, moved, original.scale, original.rotationDegrees);
					Check(Distance(panned.Project(anchor), moved) < 1e-7,
						"AVISO pan preserves the grabbed geographic point while latitude-dependent longitude scale changes", failures);
				}
			}
		}

		// The old formula computes the new longitude scale at the mouse anchor's
		// latitude. Drawing uses the viewport center latitude, so this drifts even
		// before raster source/destination rounding introduces a second snap.
		const InsetProjection original{ { 2.55, 80.0 }, { 637.5, 423.5 }, 20.0, 0.0 };
		const PointD cursor{ 1024.0, 684.0 };
		const GeoPoint anchor = original.Unproject(cursor);
		auto legacy = original.ZoomedAt(cursor, 24.0);
		const double legacyLongitudeDegreesPerPixel = InsetProjection::MetersPerNm /
			(24.0 * InsetProjection::LongitudeMetersPerDegree * InsetProjection::LongitudeCosine(anchor.latitude));
		legacy.center.longitude = anchor.longitude - (cursor.X - original.viewportCenter.X) * legacyLongitudeDegreesPerPixel;
		Check(Distance(legacy.Project(anchor), cursor) > 1.0,
			"Regression fixture reproduces more than one pixel of the old latitude-dependent zoom anchor drift", failures);
		Check(Distance(original.ZoomedAt(cursor, 24.0).Project(anchor), cursor) < 1e-7,
			"Correct zoom uses the resulting viewport center cosine and removes the reproduced drift", failures);
		for (const double latitude : { -85.0, 85.0 })
		{
			const InsetProjection limited{ { 2.55, latitude }, { 647.5, 426.5 }, 240.0, 0.0 };
			const PointD pointer{ 725.0, limited.viewportCenter.Y + (latitude > 0.0 ? 100.0 : -100.0) };
			InsetProjection result = limited;
			Check(!limited.TryZoomedAt(pointer, 20.0, result) && result.scale == limited.scale &&
				result.center.latitude == limited.center.latitude && result.center.longitude == limited.center.longitude,
				"Impossible AVISO anchored zoom at the latitude limit preserves the complete previous view", failures);
			Check(limited.TryZoomedAt(pointer, 800.0, result) &&
				Distance(result.Project(limited.Unproject(pointer)), pointer) < 1e-7,
				"AVISO zoom away from a latitude limit remains exactly cursor-anchored", failures);
		}
		InsetProjection rejected;
		Check(!original.TryZoomedAt(cursor, 0.0, rejected) &&
			!original.TryZoomedAt(cursor, (std::numeric_limits<double>::infinity)(), rejected),
			"AVISO rejects invalid zoom scales without producing a displaced view", failures);
	}

	void TestSharedProjection(Failures& failures)
	{
		for (const double degrees : { -90.0, -35.0, 0.0, 35.0, 90.0, 179.0 })
		{
			for (const double latitude : { -85.0, -48.0, 0.0, 48.0, 85.0 })
			{
				const InsetProjection inset{ { 2.55, latitude }, { 647.5, 426.5 }, 120.0, degrees };
				const GeoBounds bounds = inset.Bounds(1001.0, 701.0);
				const auto projection = BilinearProjection::FromCorners(bounds,
					inset.Project({ bounds.minLongitude, bounds.maxLatitude }),
					inset.Project({ bounds.maxLongitude, bounds.maxLatitude }),
					inset.Project({ bounds.minLongitude, bounds.minLatitude }),
					inset.Project({ bounds.maxLongitude, bounds.minLatitude }));
				Check(projection.IsValid(), "The AVISO shared projection accepts complete rotated viewport bases", failures);
				Check(Distance(projection.Project(inset.center), inset.viewportCenter) < 1e-7,
					"Odd AVISO viewport sizes and latitude bounds retain the exact half-pixel center", failures);
				for (const PointD pixel : { PointD{ 200.0, 175.0 }, inset.viewportCenter, PointD{ 1150.0, 775.0 }, PointD{ -30.0, 1450.0 } })
				{
					const GeoPoint geographic = inset.Unproject(pixel);
					Check(Distance(projection.Project(geographic), pixel) < 1e-6,
						"Native AVISO geometry and interaction use the identical geographic transform, including outside the viewport", failures);
					GeoPoint roundTrip;
					Check(projection.TryUnproject(pixel, roundTrip) && Distance(inset.Project(roundTrip), pixel) < 1e-6,
						"The AVISO complete projection inverse agrees with the independent inset inverse", failures);
				}
				double recoveredRotation = 0.0;
				Check(VsmrAviso::TryNorthRotationFromCorners(
					inset.Project({ bounds.minLongitude, bounds.maxLatitude }),
					inset.Project({ bounds.maxLongitude, bounds.maxLatitude }),
					inset.Project({ bounds.minLongitude, bounds.minLatitude }),
					inset.Project({ bounds.maxLongitude, bounds.minLatitude }), recoveredRotation) &&
					std::abs(std::remainder(recoveredRotation - degrees, 360.0)) < 1e-8,
					"Complete host projection axes recover the inset angle without mouse-dependent north probes", failures);
			}
		}
	}

	void TestBilinearInverse(Failures& failures)
	{
		const GeoBounds bounds{ 2.0, 48.0, 3.0, 49.0 };
		const auto projection = BilinearProjection::FromCorners(bounds,
			PointD{ 120.25, 74.75 }, PointD{ 1600.75, 224.5 }, PointD{ -20.5, 850.25 }, PointD{ 1540.0, 1075.0 });
		const auto identical = BilinearProjection::FromCorners(bounds,
			PointD{ 120.25, 74.75 }, PointD{ 1600.75, 224.5 }, PointD{ -20.5, 850.25 }, PointD{ 1540.0, 1075.0 });
		const auto shifted = BilinearProjection::FromCorners(bounds,
			PointD{ 120.25 + 1e-7, 74.75 }, PointD{ 1600.75 + 1e-7, 224.5 },
			PointD{ -20.5 + 1e-7, 850.25 }, PointD{ 1540.0 + 1e-7, 1075.0 });
		const auto changedFourthCorner = BilinearProjection::FromCorners(bounds,
			PointD{ 120.25, 74.75 }, PointD{ 1600.75, 224.5 }, PointD{ -20.5, 850.25 }, PointD{ 1540.0, 1075.0 + 1e-7 });
		const auto changedGeography = BilinearProjection::FromCorners(GeoBounds{ 2.0 + 1e-10, 48.0, 3.0, 49.0 },
			PointD{ 120.25, 74.75 }, PointD{ 1600.75, 224.5 }, PointD{ -20.5, 850.25 }, PointD{ 1540.0, 1075.0 });
		Check(projection.SameMapping(identical) && identical.SameMapping(projection),
			"Native AVISO cache recognizes an identical complete double-precision projection", failures);
		Check(!projection.SameMapping(shifted) && !projection.SameMapping(changedFourthCorner) &&
			!projection.SameMapping(changedGeography),
			"Native AVISO cache detects subpixel shifts, fourth-corner changes and geographic basis changes exactly", failures);
		Check(!projection.IsAffine() && projection.LongitudeSpan() == 1.0 && projection.LatitudeSpan() == 1.0,
			"A non-affine host basis exposes its complete geographic spans to vector rendering", failures);
		const auto affine = BilinearProjection::FromCorners(bounds,
			PointD{ 120.25, 74.75 }, PointD{ 1600.75, 224.5 }, PointD{ -20.5, 850.25 }, PointD{ 1460.0, 1000.0 });
		Check(affine.IsAffine() && affine.Horizontal().X == 1480.5 && affine.Vertical().Y == 775.5 &&
			affine.CrossTerm().X == 0.0 && affine.CrossTerm().Y == 0.0,
			"An exact rotated affine host basis stays eligible for retained vector geometry", failures);
		for (int row = -3; row <= 13; ++row)
		{
			for (int column = -3; column <= 13; ++column)
			{
				const GeoPoint point{ 2.0 + column * 0.1, 48.0 + row * 0.1 };
				GeoPoint recovered;
				Check(projection.TryUnproject(projection.Project(point), recovered) &&
					std::abs(recovered.longitude - point.longitude) < 1e-10 &&
					std::abs(recovered.latitude - point.latitude) < 1e-10,
					"AVISO inverse retains a non-affine fourth-corner term instead of replacing rotation with a source AABB", failures);
			}
		}
		GeoPoint recovered;
		const auto singular = BilinearProjection::FromCorners(bounds,
			PointD{ 0.0, 0.0 }, PointD{ 10.0, 10.0 }, PointD{ 20.0, 20.0 }, PointD{ 30.0, 30.0 });
		Check(!singular.TryUnproject({ 1.0, 1.0 }, recovered), "AVISO rejects singular geographic inverse projections", failures);
		const auto invalid = BilinearProjection::FromCorners(GeoBounds{},
			PointD{ 0.0, 0.0 }, PointD{ 10.0, 0.0 }, PointD{ 0.0, 10.0 }, PointD{ 10.0, 10.0 });
		Check(!invalid.IsValid() && !invalid.TryUnproject({ 1.0, 1.0 }, recovered),
			"AVISO rejects empty geographic projection extents", failures);
		Check(!projection.TryUnproject({ (std::numeric_limits<double>::infinity)(), 1.0 }, recovered),
			"AVISO rejects non-finite mouse coordinates before inversion", failures);
		double rotation = 0.0;
		Check(!VsmrAviso::TryNorthRotationFromCorners(PointD{}, PointD{}, PointD{}, PointD{}, rotation),
			"Degenerate host projection axes are rejected before computing the inset angle", failures);
	}

	void TestRotatedVisibleBounds(Failures& failures)
	{
		const auto contains = [](const GeoBounds& bounds, const GeoPoint& point) {
			return point.longitude >= bounds.minLongitude - 1e-10 && point.longitude <= bounds.maxLongitude + 1e-10 &&
				point.latitude >= bounds.minLatitude - 1e-10 && point.latitude <= bounds.maxLatitude + 1e-10;
		};
		for (const PointD extent : { PointD{ 1001.0, 701.0 }, PointD{ 701.0, 1001.0 }, PointD{ 3840.0, 1080.0 } })
		{
			for (const double rotation : { 0.0, 17.0, 45.0, 90.0, 115.0, 179.0 })
			{
				for (const double latitude : { -80.0, 0.0, 80.0 })
				{
					const InsetProjection projection{ { 2.55, latitude }, { 137.0 + extent.X * 0.5, 73.0 + extent.Y * 0.5 }, 120.0, rotation };
					const auto bounds = projection.VisibleBounds(extent.X, extent.Y, 180.0);
					for (int row = 0; row <= 10; ++row)
					{
						for (int column = 0; column <= 10; ++column)
						{
							const PointD screen{ 137.0 - 180.0 + (extent.X + 360.0) * column / 10.0,
								73.0 - 180.0 + (extent.Y + 360.0) * row / 10.0 };
							Check(contains(bounds, projection.Unproject(screen)),
								"Rotated AVISO target culling retains every point in the viewport and its fixed screen margin", failures);
						}
					}
				}
			}
		}
		const InsetProjection quarterTurn{ { 2.55, 48.0 }, { 637.5, 423.5 }, 120.0, 90.0 };
		const GeoPoint visibleTarget = quarterTurn.Unproject({ quarterTurn.viewportCenter.X + 590.0, quarterTurn.viewportCenter.Y });
		const GeoBounds oldBounds = quarterTurn.Bounds(1201.0, 701.0);
		const double oldLatitudeMargin = (oldBounds.maxLatitude - oldBounds.minLatitude) * 0.25;
		Check(visibleTarget.latitude > oldBounds.maxLatitude + oldLatitudeMargin &&
			contains(quarterTurn.VisibleBounds(1201.0, 701.0, 180.0), visibleTarget),
			"Regression fixture restores a visible quarter-turn aircraft discarded by the old unrotated geographic filter", failures);
	}

	void TestRecoveredHostProjection(Failures& failures)
	{
		for (const PointD extent : { PointD{1920,1080}, PointD{1919,1079}, PointD{1001,701}, PointD{701,1001} })
		{
			const auto pixels = VsmrAviso::InverseProjectionSamplePoints({137,73},extent.X,extent.Y);
			for (const double latitude : { 0.0, 48.742169, 80.0 })
			{
				for (const double rotation : { 0.0, 17.0, 35.0, 75.0, 89.9, 90.0, 115.0, 180.0, 270.0 })
				{
					for (const double scale : { 20.0, 350.0, 2400.0 })
					{
						const InsetProjection host{ {2.557391,latitude},
							{137+extent.X*0.5+0.371,73+extent.Y*0.5+0.239},scale,rotation };
						std::array<VsmrAviso::ProjectionSample,9> samples;
						for (std::size_t i=0;i<samples.size();++i) samples[i]={pixels[i],host.Unproject(pixels[i])};
						BilinearProjection recovered;
						Check(VsmrAviso::TryRecoverAffineProjection(host.Bounds(extent.X,extent.Y),samples,recovered) &&
							recovered.IsAffine(0.0),
							"Precise SDK inverse probes recover an exactly affine full-view basis at every tested rotation, latitude and scale",failures);
						const auto& vertical = recovered.Vertical();
						const double northRotation = std::atan2(-vertical.X, vertical.Y) * 180.0 / 3.14159265358979323846;
						Check(std::abs(std::remainder(northRotation - rotation, 360.0)) < 1e-7,
							"Inset north rotation from the recovered full host basis remains exact across main zoom scales", failures);
						for (const PointD pixel : { PointD{137+extent.X*0.2731,73+extent.Y*0.6817},
							PointD{137+extent.X*0.8173,73+extent.Y*0.2291}, PointD{137+extent.X*0.499,73+extent.Y*0.503} })
						{
							const auto geographic=host.Unproject(pixel);
							Check(Distance(recovered.Project(geographic),pixel)<1e-5,
								"Recovered host projection keeps independent fractional mouse anchors and cropped-view coordinates exact",failures);
							GeoPoint inverse;
							Check(recovered.TryUnproject(pixel,inverse) && Distance(host.Project(inverse),pixel)<1e-5,
								"Recovered projection inverse agrees with the host across arbitrary crop and cursor positions",failures);
						}
					}
				}
			}
		}
		const InsetProjection host{{2.557391,48.742169},{637.871,423.739},350,0};
		const auto bounds=host.Bounds(1001,701);
		const auto pixels=VsmrAviso::InverseProjectionSamplePoints({137,73},1001,701);
		std::array<VsmrAviso::ProjectionSample,9> samples;
		for(std::size_t i=0;i<samples.size();++i) samples[i]={pixels[i],host.Unproject(pixels[i])};
		BilinearProjection recovered;
		Check(VsmrAviso::TryRecoverAffineProjection(bounds,samples,recovered),"Host recovery regression fixture is valid",failures);
		const auto rounded=[](PointD point){return PointD{std::round(point.X),std::round(point.Y)};};
		const auto old=BilinearProjection::FromCorners(bounds,
			rounded(host.Project({bounds.minLongitude,bounds.maxLatitude})),rounded(host.Project({bounds.maxLongitude,bounds.maxLatitude})),
			rounded(host.Project({bounds.minLongitude,bounds.minLatitude})),rounded(host.Project({bounds.maxLongitude,bounds.minLatitude})));
		const PointD cursor{829,271};
		Check(Distance(old.Project(host.Unproject(cursor)),cursor)>0.25 &&
			Distance(recovered.Project(host.Unproject(cursor)),cursor)<1e-5,
			"Inverse recovery removes a reproduced rounded-corner cursor displacement without widening its acceptance tolerance",failures);
		const auto original=recovered;
		auto curved=samples;
		for(std::size_t i=0;i<curved.size();++i)
		{
			const double u=(pixels[i].X-137)/1001.0;
			curved[i].geographic.longitude+=host.LongitudeDegreesPerPixel()*0.4*u*(1-u);
		}
		Check(!VsmrAviso::TryRecoverAffineProjection(bounds,curved,recovered) && recovered.SameMapping(original),
			"Nonlinear inverse behavior between matching host corners is rejected by independent center and edge-quarter probes",failures);
		auto noisy=samples;
		noisy[8].geographic.latitude+=host.LatitudeDegreesPerPixel()*0.001;
		Check(!VsmrAviso::TryRecoverAffineProjection(bounds,noisy,recovered),
			"Recovery rejects even a one-thousandth-pixel independent probe mismatch instead of masking host approximation",failures);
		noisy=samples;noisy[7].geographic.longitude=(std::numeric_limits<double>::infinity)();
		Check(!VsmrAviso::TryRecoverAffineProjection(bounds,noisy,recovered),"Recovery rejects non-finite SDK geographic samples",failures);
		noisy=samples;for(auto& sample:noisy) sample.geographic=samples[0].geographic;
		Check(!VsmrAviso::TryRecoverAffineProjection(bounds,noisy,recovered),"Recovery rejects singular host inverse axes",failures);
		noisy=samples;
		for(auto& sample:noisy)
		{
			sample.geographic.longitude=std::round(sample.geographic.longitude*1e5)/1e5;
			sample.geographic.latitude=std::round(sample.geographic.latitude*1e5)/1e5;
		}
		Check(!VsmrAviso::TryRecoverAffineProjection(bounds,noisy,recovered),"Rounded geographic inverse samples retain the SDK corner fallback",failures);
	}

	void TestRecoveredNorthAngle(Failures& failures)
	{
		const auto pixels = VsmrAviso::InverseProjectionSamplePoints({137,73},1001,701);
		double minimumRoundedAngle = (std::numeric_limits<double>::infinity)();
		double maximumRoundedAngle = -(std::numeric_limits<double>::infinity)();
		for (const double phase : { 0.0, 0.17, 0.31, 0.49, 0.63, 0.81 })
		{
			const InsetProjection host{ {2.557391,48.742169}, {637.871 + phase,423.739 - phase},350,17 };
			const auto bounds = host.Bounds(1001,701);
			std::array<VsmrAviso::ProjectionSample,9> samples;
			for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = {pixels[i],host.Unproject(pixels[i])};
			BilinearProjection recovered;
			Check(VsmrAviso::TryRecoverAffineProjection(bounds,samples,recovered),
				"Inset angle recovery accepts precise host origins at every tested fractional phase", failures);
			const auto& vertical = recovered.Vertical();
			const double northRotation = std::atan2(-vertical.X, vertical.Y) * 180.0 / 3.14159265358979323846;
			Check(std::abs(northRotation - 17.0) < 1e-7,
				"Fractional host position changes do not rotate the independently zoomed inset", failures);
			const auto rounded = [&](GeoPoint point) {
				const PointD screen = host.Project(point);
				return PointD{ std::round(screen.X), std::round(screen.Y) };
			};
			double roundedAngle = 0.0;
			Check(VsmrAviso::TryNorthRotationFromCorners(
				rounded({bounds.minLongitude,bounds.maxLatitude}), rounded({bounds.maxLongitude,bounds.maxLatitude}),
				rounded({bounds.minLongitude,bounds.minLatitude}), rounded({bounds.maxLongitude,bounds.minLatitude}), roundedAngle),
				"Rounded-forward north angle regression fixture is valid", failures);
			minimumRoundedAngle = (std::min)(minimumRoundedAngle, roundedAngle);
			maximumRoundedAngle = (std::max)(maximumRoundedAngle, roundedAngle);
		}
		Check(maximumRoundedAngle - minimumRoundedAngle > 0.01,
			"Regression fixture reproduces rounded-forward angle changes while precise inverse rotation stays fixed", failures);
	}

	void TestFractionalWheelZoom(Failures& failures)
	{
		const PointD cursor{ 829.0, 271.0 };
		const double incrementalMultiplier = std::pow(1.18, 1.0 / 120.0);
		const double reverseMultiplier = std::pow(1.18, -1.0 / 120.0);
		for (const double scale : { 1.0, 4.75, 19.25, 20.0, 350.0, 2000.0 })
		{
			for (const double rotation : { -35.0, 0.0, 35.0, 90.0 })
			{
				const InsetProjection original{ {2.557391,48.742169}, {637.5,423.5}, scale, rotation };
				const GeoPoint anchor = original.Unproject(cursor);
				InsetProjection oneNotch;
				Check(original.TryZoomedByAt(cursor, 1.18, oneNotch),
					"A full AVISO wheel notch changes every valid tested scale", failures);
				auto incremental = original;
				for (int step = 0; step < 120; ++step)
				{
					InsetProjection next;
					Check(incremental.TryZoomedByAt(cursor, incrementalMultiplier, next) && next.scale > incremental.scale,
						"Every precision wheel delta changes the AVISO scale without integer rounding", failures);
					incremental = next;
					Check(Distance(incremental.Project(anchor), cursor) < 1e-6,
						"Every fractional AVISO zoom frame preserves the geographic cursor anchor", failures);
				}
				Check(std::abs(incremental.scale - oneNotch.scale) < 1e-9 &&
					std::abs(incremental.center.latitude - oneNotch.center.latitude) < 1e-9 &&
					std::abs(incremental.center.longitude - oneNotch.center.longitude) < 1e-9,
					"One hundred twenty precision wheel deltas match one complete AVISO notch", failures);
				for (int step = 0; step < 120; ++step)
				{
					InsetProjection next;
					Check(incremental.TryZoomedByAt(cursor, reverseMultiplier, next) && next.scale < incremental.scale,
						"Reverse precision wheel steps unzoom AVISO continuously", failures);
					incremental = next;
					Check(Distance(incremental.Project(anchor), cursor) < 1e-6,
						"Every fractional AVISO unzoom frame preserves the geographic cursor anchor", failures);
				}
				Check(std::abs(incremental.scale - original.scale) < 1e-9 &&
					std::abs(incremental.center.latitude - original.center.latitude) < 1e-9 &&
					std::abs(incremental.center.longitude - original.center.longitude) < 1e-9,
					"Fractional wheel zoom and unzoom return to the original scale and geographic center", failures);
			}
		}
		for (const double historicalScale : { 1.25, 4.0, 19.0 })
		{
			const InsetProjection restored{ {2.557391,48.742169}, {637.5,423.5}, historicalScale,17.0 };
			const GeoPoint anchor = restored.Unproject(cursor);
			InsetProjection unzoomed;
			Check(restored.TryZoomedByAt(cursor, 1.0 / 1.18, unzoomed) && unzoomed.scale < restored.scale &&
				Distance(unzoomed.Project(anchor), cursor) < 1e-6,
				"Unzooming a restored scale below twenty reduces it instead of jumping upward", failures);
		}
		const InsetProjection minimum{ {2.557391,48.742169}, {637.5,423.5},1.0,17.0 };
		const InsetProjection maximum{ {2.557391,48.742169}, {637.5,423.5},2400.0,17.0 };
		auto result = minimum;
		Check(!minimum.TryZoomedByAt(cursor, reverseMultiplier, result) && result.scale == minimum.scale &&
			result.center.latitude == minimum.center.latitude && result.center.longitude == minimum.center.longitude,
			"Unzooming at the supported minimum scale leaves the view unchanged", failures);
		Check(!maximum.TryZoomedByAt(cursor, incrementalMultiplier, result) && result.scale == minimum.scale,
			"Zooming at the supported maximum scale does not publish a replacement view", failures);
		const auto almostMinimum = InsetProjection{ minimum.center, minimum.viewportCenter,1.01,minimum.rotationDegrees };
		const GeoPoint anchor = almostMinimum.Unproject(cursor);
		Check(almostMinimum.TryZoomedByAt(cursor, 1.0 / 1.18, result) && result.scale == 1.0 &&
			Distance(result.Project(anchor), cursor) < 1e-6,
			"Reaching the minimum scale clamps continuously and retains the cursor anchor", failures);
		for (const double multiplier : { 0.0, -1.0, 1.0, (std::numeric_limits<double>::infinity)(),
			(std::numeric_limits<double>::quiet_NaN)() })
		{
			result = minimum;
			Check(!minimum.TryZoomedByAt(cursor, multiplier, result) && result.scale == minimum.scale,
				"Invalid or neutral AVISO wheel multipliers leave the view unchanged", failures);
		}
	}
}

std::vector<std::string> RunAvisoProjectionTests()
{
	Failures failures;
	TestCursorAnchoring(failures);
	TestSharedProjection(failures);
	TestBilinearInverse(failures);
	TestRotatedVisibleBounds(failures);
	TestRecoveredHostProjection(failures);
	TestRecoveredNorthAngle(failures);
	TestFractionalWheelZoom(failures);
	return failures;
}
