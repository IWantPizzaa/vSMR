#pragma once

#include "radar/RadarScreenTypes.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace VsmrAviso
{
	struct AvisoGeoBounds
	{
		double minLongitude = 0.0;
		double minLatitude = 0.0;
		double maxLongitude = 0.0;
		double maxLatitude = 0.0;

		bool Intersects(const AvisoGeoBounds& other) const noexcept;
		bool Contains(const AvisoGeoBounds& other) const noexcept;
	};

	// Geometry and label positions do not change when groups or palettes change.
	// Compile their bounds once and share the immutable index between viewports.
	// Queries retain document order because translucent shapes depend on it.
	class AvisoRenderScene final
	{
	public:
		static std::shared_ptr<const AvisoRenderScene> Build(
			const std::vector<VsmrRadarTypes::AvisoFeature>& features,
			const std::vector<VsmrRadarTypes::AvisoLabel>& labels);

		void QueryFeatures(const AvisoGeoBounds& bounds,
			std::vector<std::uint32_t>& orderedIndices) const;
		void QueryLabels(const AvisoGeoBounds& bounds,
			std::vector<std::uint32_t>& orderedIndices) const;
		const AvisoGeoBounds& PathBounds(std::size_t featureIndex,
			std::size_t pathIndex) const noexcept;

	private:
		struct Node
		{
			AvisoGeoBounds bounds;
			std::uint32_t first = 0;
			std::uint32_t count = 0;
			std::uint32_t left = 0;
			std::uint32_t right = 0;
		};

		struct Index
		{
			std::vector<AvisoGeoBounds> bounds;
			std::vector<std::uint32_t> items;
			std::vector<Node> nodes;

			void Build();
			std::uint32_t BuildNode(std::uint32_t first, std::uint32_t count);
			void Query(const AvisoGeoBounds& query,
				std::vector<std::uint32_t>& orderedIndices) const;
			void QueryNode(std::uint32_t nodeIndex, const AvisoGeoBounds& query,
				std::vector<std::uint32_t>& indices) const;
		};

		Index features_;
		Index labels_;
		std::vector<std::vector<AvisoGeoBounds>> pathBounds_;
	};
}
