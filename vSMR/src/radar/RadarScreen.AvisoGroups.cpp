#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoFeatureMetadata.hpp"
#include "aviso/AvisoRasterPipeline.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

using VsmrAvisoFeatureMetadata::ReadFeatureIdentity;
using VsmrAvisoFeatureMetadata::TrimAirportCode;
using VsmrAvisoFeatureMetadata::TryReadGroupIds;

std::vector<CSMRRadar::AvisoGroup> CSMRRadar::GetAvisoGroups() const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	return AvisoRuntimeGroups;
}

std::shared_ptr<const std::unordered_map<std::string, bool>> CSMRRadar::GetAvisoGroupVisibilitySnapshot(
	unsigned long long* outGeneration) const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	if (outGeneration != nullptr)
		*outGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	return AvisoGroupVisibilitySnapshot;
}

bool CSMRRadar::GetAvisoRenderSnapshots(
	std::shared_ptr<const std::vector<AvisoFeature>>& outFeatures,
	std::shared_ptr<const std::vector<AvisoLabel>>& outLabels,
	std::shared_ptr<const std::unordered_map<std::string, bool>>& outGroupVisibility,
	unsigned long long& outGeneration) const
{
	std::lock_guard<std::mutex> guard(AvisoGroupMutex);
	outFeatures = AvisoGeoJsonFeatureSnapshot;
	outLabels = AvisoGeoJsonLabelSnapshot;
	outGroupVisibility = AvisoGroupVisibilitySnapshot;
	outGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	return outFeatures != nullptr && outLabels != nullptr &&
		outGroupVisibility != nullptr;
}


bool CSMRRadar::ApplyAvisoGroupMembershipSnapshot(
	const rapidjson::Value& aviso,
	std::string* outError)
{
	auto fail = [&](const std::string& message) -> bool
	{
		if (outError != nullptr)
			*outError = message;
		return false;
	};
	if (outError != nullptr)
		outError->clear();

	if (!aviso.IsObject() ||
		!aviso.HasMember("features") ||
		!aviso["features"].IsArray())
	{
		return fail("Staged AVISO state must contain a features array.");
	}

	std::shared_ptr<const std::vector<AvisoFeature>> baseFeatures;
	std::shared_ptr<const std::vector<AvisoLabel>> baseLabels;
	size_t sourceFeatureCount = 0;
	unsigned long long baseGeneration = 0;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		baseFeatures = AvisoGeoJsonFeatureSnapshot;
		baseLabels = AvisoGeoJsonLabelSnapshot;
		sourceFeatureCount = AvisoGeoJsonSourceFeatureCount;
		baseGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	}
	if (baseFeatures == nullptr || baseLabels == nullptr)
		return fail("No loaded AVISO renderer snapshot is available.");

	const rapidjson::Value& stagedFeatures = aviso["features"];
	const size_t stagedFeatureCount = static_cast<size_t>(stagedFeatures.Size());
	std::vector<std::vector<std::string>> memberships(stagedFeatureCount);
	std::vector<std::string> featureIds(stagedFeatureCount);
	std::unordered_map<std::string, size_t> stagedIndexById;
	stagedIndexById.reserve(stagedFeatureCount);
	for (rapidjson::SizeType index = 0; index < stagedFeatures.Size(); ++index)
	{
		const rapidjson::Value& feature = stagedFeatures[index];
		if (!feature.IsObject() ||
			!feature.HasMember("properties") ||
			!feature["properties"].IsObject())
		{
			return fail(
				"Staged AVISO feature " + std::to_string(index + 1) +
				" must contain a properties object.");
		}

		if (!TryReadGroupIds(
			&feature["properties"],
			memberships[static_cast<size_t>(index)]))
		{
			return fail(
				"Staged AVISO feature " + std::to_string(index + 1) +
				" has an invalid group membership value.");
		}
		const size_t featureIndex = static_cast<size_t>(index);
		featureIds[featureIndex] = ReadFeatureIdentity(feature);
		if (!featureIds[featureIndex].empty() &&
			!stagedIndexById.emplace(featureIds[featureIndex], featureIndex).second)
		{
			return fail(
				"Staged AVISO feature ids must be unique when applying group membership.");
		}
	}

	auto featureSnapshot = std::make_shared<std::vector<AvisoFeature>>(*baseFeatures);
	auto labelSnapshot = std::make_shared<std::vector<AvisoLabel>>(*baseLabels);
	auto applyMembership = [&](auto& item) -> bool
	{
		if (item.sourceFeatureIndex < 0 ||
			static_cast<size_t>(item.sourceFeatureIndex) >= sourceFeatureCount)
		{
			return false;
		}

		size_t stagedIndex = 0;
		bool matched = false;
		if (!item.sourceFeatureId.empty())
		{
			const auto found = stagedIndexById.find(item.sourceFeatureId);
			if (found != stagedIndexById.end())
			{
				stagedIndex = found->second;
				matched = true;
			}
		}
		else
		{
			const size_t sourceIndex = static_cast<size_t>(item.sourceFeatureIndex);
			if (sourceIndex < stagedFeatureCount &&
				featureIds[sourceIndex].empty())
			{
				stagedIndex = sourceIndex;
				matched = true;
			}
		}

		// Geometry additions/deletions remain staged until Save. Leave any
		// loaded item without a safe staged identity match unchanged.
		if (matched)
			item.groupIds = memberships[stagedIndex];
		return true;
	};
	for (AvisoFeature& feature : *featureSnapshot)
	{
		if (!applyMembership(feature))
			return fail("Staged AVISO feature identities do not match the loaded renderer.");
	}
	for (AvisoLabel& label : *labelSnapshot)
	{
		if (!applyMembership(label))
			return fail("Staged AVISO feature identities do not match the loaded renderer.");
	}

	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		if (AvisoGroupGeneration.load(std::memory_order_relaxed) != baseGeneration ||
			AvisoGeoJsonFeatureSnapshot != baseFeatures ||
			AvisoGeoJsonLabelSnapshot != baseLabels ||
			AvisoGeoJsonSourceFeatureCount != sourceFeatureCount)
		{
			return fail("AVISO renderer state changed while applying staged membership.");
		}

		AvisoGeoJsonFeatureSnapshot = featureSnapshot;
		AvisoGeoJsonLabelSnapshot = labelSnapshot;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}

	InvalidateAvisoGroupRendering();
	return true;
}

void CSMRRadar::InvalidateAvisoGroupRendering()
{
	if (AvisoGeoJsonRenderPipeline != nullptr)
		AvisoGeoJsonRenderPipeline->InvalidateRequests();

	// Keep the last same-path raster as a stale preview while the new group or
	// ownership generation is rebuilt. Exact-cache checks still reject it.
	AvisoGeoJsonLastViewValid = false;
	AvisoGeoJsonLastViewPath.clear();
	AvisoGeoJsonLastViewChangeTick = 0;

	for (auto& appWindow : appWindows)
	{
		if (appWindow.second != nullptr && appWindow.second->IsAvisoViewport())
			appWindow.second->InvalidateAvisoViewportRendering();
	}

	try
	{
		RequestRefresh();
	}
	catch (...)
	{
	}
}

std::string CSMRRadar::GetAvisoColorPalette() const
{
	return AvisoUseDayColorPalette ? "day" : "night";
}

bool CSMRRadar::SetAvisoColorPalette(const std::string& rawPalette, bool persistToAsr)
{
	std::string palette = rawPalette;
	palette.erase(
		palette.begin(),
		std::find_if(
			palette.begin(),
			palette.end(),
			[](unsigned char value) { return !std::isspace(value); }));
	palette.erase(
		std::find_if(
			palette.rbegin(),
			palette.rend(),
			[](unsigned char value) { return !std::isspace(value); }).base(),
		palette.end());
	std::transform(
		palette.begin(),
		palette.end(),
		palette.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (palette != "day" && palette != "night")
		return false;

	const bool useDayPalette = palette == "day";
	if (AvisoUseDayColorPalette != useDayPalette)
	{
		AvisoUseDayColorPalette = useDayPalette;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		InvalidateAvisoGroupRendering();
	}

	if (persistToAsr)
	{
		SaveDataToAsr(
			"AvisoColorPalette",
			"AVISO day/night color palette",
			GetAvisoColorPalette().c_str());
	}
	return true;
}

bool CSMRRadar::SetAvisoGroupVisibility(const std::string& rawGroupId, bool visible)
{
	const std::string& groupId = rawGroupId;
	if (groupId.empty())
		return false;

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		auto found = std::find_if(
			AvisoRuntimeGroups.begin(),
			AvisoRuntimeGroups.end(),
			[&](const AvisoGroup& group) { return group.id == groupId; });
		if (found == AvisoRuntimeGroups.end())
			return false;
		if (found->visible == visible)
			return true;

		found->visible = visible;
		auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
		visibility->reserve(AvisoRuntimeGroups.size());
		for (const AvisoGroup& group : AvisoRuntimeGroups)
			(*visibility)[group.id] = group.visible;
		AvisoGroupVisibilitySnapshot = visibility;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		changed = true;
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}
bool CSMRRadar::ToggleAvisoGroupVisibility(const std::string& rawGroupId, bool* outVisible)
{
	const std::string& groupId = rawGroupId;
	if (groupId.empty())
		return false;

	bool nextVisibility = true;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		auto found = std::find_if(
			AvisoRuntimeGroups.begin(),
			AvisoRuntimeGroups.end(),
			[&](const AvisoGroup& group) { return group.id == groupId; });
		if (found == AvisoRuntimeGroups.end())
			return false;

		found->visible = !found->visible;
		nextVisibility = found->visible;
		auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
		visibility->reserve(AvisoRuntimeGroups.size());
		for (const AvisoGroup& group : AvisoRuntimeGroups)
			(*visibility)[group.id] = group.visible;
		AvisoGroupVisibilitySnapshot = visibility;
		AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
	}

	if (outVisible != nullptr)
		*outVisible = nextVisibility;
	InvalidateAvisoGroupRendering();
	return true;
}

bool CSMRRadar::SetAvisoGroupVisibilities(
	const std::vector<std::pair<std::string, bool>>& requestedVisibility)
{
	std::unordered_map<std::string, bool> visibilityById;
	for (const auto& entry : requestedVisibility)
	{
		const std::string& groupId = entry.first;
		if (!groupId.empty())
			visibilityById[groupId] = entry.second;
	}

	if (visibilityById.empty())
		return requestedVisibility.empty();

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		for (const auto& requested : visibilityById)
		{
			const auto found = std::find_if(
				AvisoRuntimeGroups.begin(),
				AvisoRuntimeGroups.end(),
				[&](const AvisoGroup& group) { return group.id == requested.first; });
			if (found == AvisoRuntimeGroups.end())
				return false;
		}

		for (AvisoGroup& group : AvisoRuntimeGroups)
		{
			const auto found = visibilityById.find(group.id);
			if (found == visibilityById.end())
				continue;
			if (group.visible != found->second)
			{
				group.visible = found->second;
				changed = true;
			}
		}

		if (changed)
		{
			auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
			visibility->reserve(AvisoRuntimeGroups.size());
			for (const AvisoGroup& group : AvisoRuntimeGroups)
				(*visibility)[group.id] = group.visible;
			AvisoGroupVisibilitySnapshot = visibility;
			AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}

bool CSMRRadar::UpdateAvisoGroups(const std::vector<AvisoGroup>& groups)
{
	std::vector<AvisoGroup> normalizedGroups;
	std::unordered_set<std::string> knownIds;
	normalizedGroups.reserve(groups.size());
	for (const AvisoGroup& source : groups)
	{
		AvisoGroup group = source;
		if (group.id.empty() || !knownIds.insert(group.id).second)
			continue;
		group.name = TrimAirportCode(group.name);
		if (group.name.empty())
			group.name = group.id;
		normalizedGroups.push_back(std::move(group));
	}

	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		if (AvisoRuntimeGroups.size() != normalizedGroups.size())
		{
			changed = true;
		}
		else
		{
			for (size_t i = 0; i < normalizedGroups.size(); ++i)
			{
				if (AvisoRuntimeGroups[i].id != normalizedGroups[i].id ||
					AvisoRuntimeGroups[i].name != normalizedGroups[i].name ||
					AvisoRuntimeGroups[i].visible != normalizedGroups[i].visible)
				{
					changed = true;
					break;
				}
			}
		}

		if (changed)
		{
			AvisoRuntimeGroups = std::move(normalizedGroups);
			auto visibility = std::make_shared<std::unordered_map<std::string, bool>>();
			visibility->reserve(AvisoRuntimeGroups.size());
			for (const AvisoGroup& group : AvisoRuntimeGroups)
				(*visibility)[group.id] = group.visible;
			AvisoGroupVisibilitySnapshot = visibility;
			AvisoGroupGeneration.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (changed)
		InvalidateAvisoGroupRendering();
	return true;
}
