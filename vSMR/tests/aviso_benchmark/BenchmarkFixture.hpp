#pragma once

#include "radar/RadarScreenTypes.hpp"
#include <array>
#include <filesystem>

namespace VsmrAvisoBenchmark
{
	struct Fixture
	{
		std::shared_ptr<const std::vector<VsmrRadarTypes::AvisoFeature>> features;
		std::shared_ptr<const std::vector<VsmrRadarTypes::AvisoLabel>> labels;
		std::shared_ptr<const std::unordered_map<std::string, bool>> visibility;
		std::array<Gdiplus::Color, 3> backgrounds;
		double minLongitude = 0;
		double maxLongitude = 0;
		double minLatitude = 0;
		double maxLatitude = 0;
	};

	Fixture LoadFixture(const std::filesystem::path& path);
	std::unique_ptr<VsmrRadarTypes::AvisoRasterRenderResult> RenderBaseline(
		const VsmrRadarTypes::AvisoRasterRenderRequest& request);
}
