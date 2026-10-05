#pragma once

#include "scene/RadarScene.hpp"
#include <Windows.h>
#include <GdiPlus.h>
#include <cmath>

namespace VsmrTargetRendering
{
	struct DrawOptions
	{
		bool drawTrail = true;
		bool drawPrimaryReturn = true;
		int minimumHitSize = 12;
	};

	struct AlwaysVisible
	{
		bool operator()(const POINT&, int) const { return true; }
	};

	namespace detail
	{
		VsmrScene::GeoPoint HeadingProbe(const VsmrScene::Target& target);
		void ScalePolygon(std::vector<Gdiplus::PointF>& polygon, double symbolScale);
	}

	// Projection is compiled with each viewport's concrete callable. The GDI+
	// painting pass consumes these coordinates without per-point indirection.
	struct ProjectedTarget
	{
		struct TrailPoint { POINT point; std::size_t index; };
		POINT center{};
		POINT heading{};
		std::vector<TrailPoint> trail;
		std::vector<Gdiplus::PointF> primary;
		std::array<std::vector<Gdiplus::PointF>, 3> afterglow;

		template<class Project, class Visible>
		void Update(const VsmrScene::Target& target, const VsmrScene::TargetPresentation& presentation,
			const DrawOptions& options, const Project& project, const Visible& visible, int trailMargin = 7)
		{
			center = project(target.position);
			heading = center;
			if (target.style.icon != VsmrScene::IconStyle::Nova)
			{
				const auto probe = detail::HeadingProbe(target);
				if (probe.valid) heading = project(probe);
			}
			auto polygon = [&](const std::vector<VsmrScene::GeoPoint>& source, std::vector<Gdiplus::PointF>& result)
			{
				result.clear();
				result.reserve(source.size());
				for (const auto& point : source)
				{
					if (!point.valid) continue;
					const auto screen = project(point);
					result.emplace_back(static_cast<Gdiplus::REAL>(screen.x), static_cast<Gdiplus::REAL>(screen.y));
				}
				detail::ScalePolygon(result, presentation.symbolScale);
			};
			primary.clear();
			for (auto& history : afterglow) history.clear();
			if (target.style.icon == VsmrScene::IconStyle::Nova)
			{
				if (options.drawPrimaryReturn && target.style.showPrimaryReturn) polygon(target.primaryReturnPolygon, primary);
				if (options.drawTrail && presentation.trailEnabled)
					for (std::size_t i = 0; i < afterglow.size(); ++i) polygon(target.primaryReturnAfterglow[i], afterglow[i]);
			}
			trail.clear();
			if (!options.drawTrail || !presentation.trailEnabled) return;
			trail.reserve(target.trailPositions.size());
			for (std::size_t i = 0; i < target.trailPositions.size(); ++i)
			{
				if (target.style.icon == VsmrScene::IconStyle::Diamond)
				{
					// Keep the configured dot count, but sample the recent track at
					// quarter intervals. Interpolate along each segment, not toward
					// the current position, so turns retain their actual track shape.
					constexpr std::size_t samplesPerHistoryInterval = 4;
					const std::size_t segment = i / samplesPerHistoryInterval;
					const auto& from = segment == 0 ? target.position : target.trailPositions[segment - 1];
					const auto& to = target.trailPositions[segment];
					if (!from.valid || !to.valid) break;
					const auto start = project(from);
					const auto end = project(to);
					const double fraction = static_cast<double>(i % samplesPerHistoryInterval + 1) /
						static_cast<double>(samplesPerHistoryInterval);
					const POINT point = {
						static_cast<LONG>(std::lround(start.x + (static_cast<double>(end.x) - start.x) * fraction)),
						static_cast<LONG>(std::lround(start.y + (static_cast<double>(end.y) - start.y) * fraction)) };
					if (visible(point, trailMargin)) trail.push_back({ point, i });
					continue;
				}
				if (!target.trailPositions[i].valid) continue;
				const auto point = project(target.trailPositions[i]);
				if (visible(point, trailMargin)) trail.push_back({ point, i });
			}
		}
	};
}
