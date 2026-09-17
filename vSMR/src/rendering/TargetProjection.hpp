#pragma once

#include "scene/RadarScene.hpp"
#include <Windows.h>
#include <GdiPlus.h>

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
			const DrawOptions& options, const Project& project, const Visible& visible)
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
				if (!target.trailPositions[i].valid) continue;
				const auto point = project(target.trailPositions[i]);
				if (visible(point, 7)) trail.push_back({ point, i });
			}
		}
	};
}
