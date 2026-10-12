#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRenderScene.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace
{
	VsmrAviso::AvisoGeoBounds UnionBounds(const VsmrAviso::AvisoGeoBounds& left,
		const VsmrAviso::AvisoGeoBounds& right) noexcept
	{
		return { (std::min)(left.minLongitude, right.minLongitude),
			(std::min)(left.minLatitude, right.minLatitude),
			(std::max)(left.maxLongitude, right.maxLongitude),
			(std::max)(left.maxLatitude, right.maxLatitude) };
	}
}

bool VsmrAviso::AvisoGeoBounds::Intersects(const AvisoGeoBounds& other) const noexcept
{
	return maxLongitude >= other.minLongitude && minLongitude <= other.maxLongitude &&
		maxLatitude >= other.minLatitude && minLatitude <= other.maxLatitude;
}

bool VsmrAviso::AvisoGeoBounds::Contains(const AvisoGeoBounds& other) const noexcept
{
	return minLongitude <= other.minLongitude && maxLongitude >= other.maxLongitude &&
		minLatitude <= other.minLatitude && maxLatitude >= other.maxLatitude;
}

std::shared_ptr<const VsmrAviso::AvisoRenderScene> VsmrAviso::AvisoRenderScene::Build(
	const std::vector<VsmrRadarTypes::AvisoFeature>& features,
	const std::vector<VsmrRadarTypes::AvisoLabel>& labels)
{
	if (features.size() > (std::numeric_limits<std::uint32_t>::max)() ||
		labels.size() > (std::numeric_limits<std::uint32_t>::max)())
		throw std::length_error("AVISO scene exceeds 32-bit index capacity");

	auto scene = std::make_shared<AvisoRenderScene>();
	scene->features_.bounds.reserve(features.size());
	scene->pathBounds_.reserve(features.size());
	for (const auto& feature : features)
	{
		scene->features_.bounds.push_back({ feature.minLongitude, feature.minLatitude,
			feature.maxLongitude, feature.maxLatitude });
		std::vector<AvisoGeoBounds> pathBounds;
		pathBounds.reserve(feature.paths.size());
		for (const auto& path : feature.paths)
		{
			AvisoGeoBounds bounds{ feature.minLongitude, feature.minLatitude,
				feature.maxLongitude, feature.maxLatitude };
			if (!path.empty())
			{
				bounds = { path.front().longitude, path.front().latitude,
					path.front().longitude, path.front().latitude };
				for (const auto& point : path)
					bounds = UnionBounds(bounds, { point.longitude, point.latitude,
						point.longitude, point.latitude });
			}
			pathBounds.push_back(bounds);
		}
		scene->pathBounds_.push_back(std::move(pathBounds));
	}
	scene->labels_.bounds.reserve(labels.size());
	for (const auto& label : labels)
		scene->labels_.bounds.push_back({ label.position.longitude, label.position.latitude,
			label.position.longitude, label.position.latitude });
	scene->features_.Build();
	scene->labels_.Build();
	return scene;
}

void VsmrAviso::AvisoRenderScene::Index::Build()
{
	items.resize(bounds.size());
	std::iota(items.begin(), items.end(), 0U);
	nodes.reserve(bounds.size() / 4U + 1U);
	if (!items.empty())
		BuildNode(0U, static_cast<std::uint32_t>(items.size()));
}

std::uint32_t VsmrAviso::AvisoRenderScene::Index::BuildNode(
	std::uint32_t first, std::uint32_t count)
{
	const std::uint32_t nodeIndex = static_cast<std::uint32_t>(nodes.size());
	Node node;
	node.first = first;
	node.count = count;
	node.bounds = bounds[items[first]];
	for (std::uint32_t offset = 1; offset < count; ++offset)
		node.bounds = UnionBounds(node.bounds, bounds[items[first + offset]]);
	nodes.push_back(node);
	if (count <= 16U)
		return nodeIndex;

	const bool splitLongitude = node.bounds.maxLongitude - node.bounds.minLongitude >=
		node.bounds.maxLatitude - node.bounds.minLatitude;
	const std::uint32_t leftCount = count / 2U;
	std::nth_element(items.begin() + first, items.begin() + first + leftCount,
		items.begin() + first + count, [&](std::uint32_t left, std::uint32_t right) {
			const auto& a = bounds[left];
			const auto& b = bounds[right];
			return splitLongitude
				? a.minLongitude + a.maxLongitude < b.minLongitude + b.maxLongitude
				: a.minLatitude + a.maxLatitude < b.minLatitude + b.maxLatitude;
		});
	const std::uint32_t left = BuildNode(first, leftCount);
	const std::uint32_t right = BuildNode(first + leftCount, count - leftCount);
	nodes[nodeIndex].count = 0;
	nodes[nodeIndex].left = left;
	nodes[nodeIndex].right = right;
	return nodeIndex;
}

void VsmrAviso::AvisoRenderScene::Index::Query(
	const AvisoGeoBounds& query, std::vector<std::uint32_t>& orderedIndices) const
{
	orderedIndices.clear();
	if (nodes.empty())
		return;
	if (query.Contains(nodes.front().bounds))
	{
		orderedIndices.resize(items.size());
		std::iota(orderedIndices.begin(), orderedIndices.end(), 0U);
		return;
	}
	QueryNode(0U, query, orderedIndices);
	std::sort(orderedIndices.begin(), orderedIndices.end());
}

void VsmrAviso::AvisoRenderScene::Index::QueryNode(
	std::uint32_t nodeIndex, const AvisoGeoBounds& query,
	std::vector<std::uint32_t>& indices) const
{
	const Node& node = nodes[nodeIndex];
	if (!node.bounds.Intersects(query))
		return;
	if (node.count != 0)
	{
		for (std::uint32_t offset = 0; offset < node.count; ++offset)
		{
			const std::uint32_t item = items[node.first + offset];
			if (bounds[item].Intersects(query))
				indices.push_back(item);
		}
		return;
	}
	QueryNode(node.left, query, indices);
	QueryNode(node.right, query, indices);
}

void VsmrAviso::AvisoRenderScene::QueryFeatures(const AvisoGeoBounds& bounds,
	std::vector<std::uint32_t>& orderedIndices) const
{
	features_.Query(bounds, orderedIndices);
}

void VsmrAviso::AvisoRenderScene::QueryLabels(const AvisoGeoBounds& bounds,
	std::vector<std::uint32_t>& orderedIndices) const
{
	labels_.Query(bounds, orderedIndices);
}

const VsmrAviso::AvisoGeoBounds& VsmrAviso::AvisoRenderScene::PathBounds(
	std::size_t featureIndex, std::size_t pathIndex) const noexcept
{
	return pathBounds_[featureIndex][pathIndex];
}
