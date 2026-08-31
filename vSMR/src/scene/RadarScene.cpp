#include "platform/windows/PrecompiledHeader.hpp"

#include "scene/RadarScene.hpp"
#include "scene/TargetRoleLogic.hpp"

#include "radar/RadarScreen.hpp"
#include "shared/TextUtils.hpp"
#include "tags/TagColorRules.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "tags/VacdmTagHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace TagColorRules = VsmrTagColorRules;

namespace
{
	using namespace EuroScopePlugIn;
	using namespace VsmrScene;
	constexpr int kRimcasStageTwoSpeedThresholdKt = 25;

	std::string CopyText(const char* text)
	{
		return text != nullptr ? text : "";
	}

	GeoPoint CopyPosition(const CPosition& position)
	{
		GeoPoint result;
		result.latitude = position.m_Latitude;
		result.longitude = position.m_Longitude;
		result.valid = std::isfinite(result.latitude) && std::isfinite(result.longitude);
		return result;
	}

	VsmrScene::Color CopyColor(const Gdiplus::Color& color)
	{
		return VsmrScene::Color{ color.GetAlpha(), color.GetR(), color.GetG(), color.GetB() };
	}

	std::size_t EstimateStringHeapBytes(const std::string& value)
	{
		const std::uintptr_t objectBegin = reinterpret_cast<std::uintptr_t>(&value);
		const std::uintptr_t objectEnd = objectBegin + sizeof(value);
		const std::uintptr_t dataAddress = reinterpret_cast<std::uintptr_t>(value.data());
		if (dataAddress >= objectBegin && dataAddress < objectEnd)
			return 0;
		return value.capacity() + 1;
	}

	std::size_t EstimateTagVariantHeapBytes(const TagVariant& variant)
	{
		std::size_t bytes = variant.lines.capacity() * sizeof(TagLine);
		for (const TagLine& line : variant.lines)
		{
			bytes += line.elements.capacity() * sizeof(TagElement);
			for (const TagElement& element : line.elements)
				bytes += EstimateStringHeapBytes(element.token) + EstimateStringHeapBytes(element.text);
		}
		return bytes;
	}

	int ActionForTagToken(const std::string& token)
	{
		const std::string key = ToLowerAsciiCopy(token);
		if (key == "callsign") return TAG_CITEM_CALLSIGN;
		if (key == "systemid") return TAG_CITEM_MANUALCORRELATE;
		if (key == "actype" || key == "sctype" || key == "sqerror" || key == "wake" || key == "origin" || key == "dest") return TAG_CITEM_FPBOX;
		if (key == "deprwy" || key == "seprwy" || key == "arvrwy" || key == "srvrwy") return TAG_CITEM_RWY;
		if (key == "gate" || key == "sate") return TAG_CITEM_GATE;
		if (key == "asid" || key == "ssid" || key == "sid" || key == "shid") return TAG_CITEM_SID;
		if (key == "groundstatus" || key == "gstatus") return TAG_CITEM_GROUNDSTATUS;
		if (key == "clearance" || key == "cleared") return TAG_CITEM_CLEARANCE;
		if (key == "uk_stand") return TAG_CITEM_UKSTAND;
		if (key == "remark") return TAG_CITEM_REMARK;
		if (key == "scratchpad") return TAG_CITEM_SCRATCHPAD;
		if (key == "holdingpoint") return TAG_CITEM_HOLDINGPOINT;
		return TAG_CITEM_NO;
	}

	const rapidjson::Value* ResolveTagDefinition(
		const rapidjson::Value& labels,
		const std::string& type,
		const std::string& status,
		bool detailed)
	{
		if (!labels.IsObject() || !labels.HasMember(type.c_str()) || !labels[type.c_str()].IsObject())
			return nullptr;

		const rapidjson::Value& section = labels[type.c_str()];
		bool inheritDetailed = false;
		auto readInheritance = [&](const rapidjson::Value& object, bool& value) -> bool
		{
			if (object.HasMember("definition_detailed_inherits_normal") && object["definition_detailed_inherits_normal"].IsBool())
			{
				value = object["definition_detailed_inherits_normal"].GetBool();
				return true;
			}
			if (object.HasMember("definition_detailed_same_as_definition") && object["definition_detailed_same_as_definition"].IsBool())
			{
				value = object["definition_detailed_same_as_definition"].GetBool();
				return true;
			}
			return false;
		};

		readInheritance(labels, inheritDetailed);
		readInheritance(section, inheritDetailed);
		const rapidjson::Value* statusSection = nullptr;
		if (!status.empty() && status != "default" &&
			section.HasMember("status_definitions") && section["status_definitions"].IsObject())
		{
			const rapidjson::Value& statuses = section["status_definitions"];
			auto findStatus = [&](const std::string& key) -> const rapidjson::Value*
			{
				if (statuses.HasMember(key.c_str()) && statuses[key.c_str()].IsObject())
					return &statuses[key.c_str()];
				return nullptr;
			};
			statusSection = findStatus(status);
			if (statusSection == nullptr && status == "airdep_onrunway")
				statusSection = findStatus("airdep");
			else if (statusSection == nullptr && status == "airarr_onrunway")
				statusSection = findStatus("airarr");
			if (statusSection != nullptr)
				readInheritance(*statusSection, inheritDetailed);
		}

		const char* key = (detailed && !inheritDetailed) ? "definition_detailed" : "definition";
		const char* legacyKey = (detailed && !inheritDetailed) ? "definitionDetailled" : nullptr;
		auto fromObject = [&](const rapidjson::Value& object) -> const rapidjson::Value*
		{
			if (object.HasMember(key) && object[key].IsArray())
				return &object[key];
			if (legacyKey != nullptr && object.HasMember(legacyKey) && object[legacyKey].IsArray())
				return &object[legacyKey];
			return nullptr;
		};

		if (statusSection != nullptr)
		{
			if (const rapidjson::Value* definition = fromObject(*statusSection))
				return definition;
		}
		if (const rapidjson::Value* definition = fromObject(section))
			return definition;

		if (detailed && !inheritDetailed)
		{
			if (statusSection != nullptr && statusSection->HasMember("definition") && (*statusSection)["definition"].IsArray())
				return &(*statusSection)["definition"];
			if (section.HasMember("definition") && section["definition"].IsArray())
				return &section["definition"];
		}
		return nullptr;
	}

	TagVariant BuildTagVariant(
		const rapidjson::Value& labels,
		const Target& target,
		bool detailed)
	{
		TagVariant result;
		const rapidjson::Value* definition = ResolveTagDefinition(
			labels,
			target.tag.definitionType,
			target.tag.status,
			detailed);
		if (definition == nullptr)
			return result;

		result.lines.reserve(definition->Size());
		for (rapidjson::SizeType lineIndex = 0; lineIndex < definition->Size(); ++lineIndex)
		{
			const rapidjson::Value& definitionLine = (*definition)[lineIndex];
			std::vector<std::string> rawTokens;
			if (definitionLine.IsArray())
			{
				rawTokens.reserve(definitionLine.Size());
				for (rapidjson::SizeType tokenIndex = 0; tokenIndex < definitionLine.Size(); ++tokenIndex)
				{
					if (definitionLine[tokenIndex].IsString())
						rawTokens.emplace_back(definitionLine[tokenIndex].GetString());
				}
			}
			else if (definitionLine.IsString())
			{
				rawTokens.emplace_back(definitionLine.GetString());
			}
			else
			{
				continue;
			}

			TagLine line;
			bool hasVisibleElement = false;
			line.elements.reserve(rawTokens.size());
			for (const std::string& rawToken : rawTokens)
			{
				DefinitionTokenStyleData styled = ParseDefinitionTokenStyle(rawToken);
				TagColorRules::VacdmColorRuleDefinition vacdmRule;
				TagColorRules::RunwayColorRuleDefinition runwayRule;
				if (TagColorRules::TryParseVacdmColorRuleToken(styled.token, vacdmRule) ||
					TagColorRules::TryParseRunwayColorRuleToken(styled.token, runwayRule))
				{
					continue;
				}

				TagElement element;
				element.token = styled.token;
				element.bold = styled.bold;
				element.hasCustomColor = styled.hasCustomColor;
				element.customColor = VsmrScene::Color{
					255,
					static_cast<std::uint8_t>(std::clamp(styled.colorR, 0, 255)),
					static_cast<std::uint8_t>(std::clamp(styled.colorG, 0, 255)),
					static_cast<std::uint8_t>(std::clamp(styled.colorB, 0, 255)) };
				element.clearanceToken = IsClearanceDefinitionToken(styled.token);
				element.action = ActionForTagToken(styled.token);
				if (element.clearanceToken)
					element.action = TAG_CITEM_CLEARANCE;

				if (element.clearanceToken)
				{
					std::string notClearedText;
					std::string clearedText;
					TryParseClearanceTokenDisplay(styled.token, notClearedText, clearedText);
					if (target.hasFlightPlan && target.correlated)
						element.text = target.tag.clearanceReceived ? clearedText : notClearedText;
				}
				else
				{
					auto exact = target.tag.tokens.find(styled.token);
					if (exact != target.tag.tokens.end())
					{
						element.text = exact->second;
					}
					else
					{
						element.text = styled.token;
						for (const auto& replacement : target.tag.tokens)
						{
							if (replacement.first.empty())
								continue;
							size_t offset = 0;
							while ((offset = element.text.find(replacement.first, offset)) != std::string::npos)
							{
								element.text.replace(offset, replacement.first.size(), replacement.second);
								offset += replacement.second.size();
							}
						}
					}
				}
				if (!detailed && ToLowerAsciiCopy(element.token) == "scratchpad" && element.text == "...")
					element.text.clear();
				if (detailed &&
					element.action == TAG_CITEM_HOLDINGPOINT &&
					element.text.empty())
				{
					element.text = "HP";
				}

				hasVisibleElement = hasVisibleElement || !element.text.empty();
				line.elements.push_back(std::move(element));
			}
			if (hasVisibleElement)
				result.lines.push_back(std::move(line));
		}
		return result;
	}

	std::string BuildBottomLine(
		CSMRRadar& radar,
		const CFlightPlan& flightPlan,
		const CRadarTargetPositionData& position,
		int transitionAltitude)
	{
		if (!flightPlan.IsValid())
			return "";

		const CFlightPlanData flightData = flightPlan.GetFlightPlanData();
		const CFlightPlanControllerAssignedData assignedData = flightPlan.GetControllerAssignedData();
		const std::string callsign = CopyText(flightPlan.GetCallsign());
		std::string result = callsign;
		const std::string callsignCode = callsign.substr(0, std::min<std::size_t>(3, callsign.size()));
		result += " (" + radar.LookupCallsignName(callsignCode) + ")";
		result += " (" + CopyText(flightPlan.GetPilotName()) + "): ";
		result += CopyText(flightData.GetAircraftFPType());
		result += " ";

		if (!flightData.IsReceived())
			return result;

		const std::string assignedSquawk = CopyText(assignedData.GetSquawk());
		const std::string reportedSquawk = position.IsValid() ? CopyText(position.GetSquawk()) : "----";
		if (!assignedSquawk.empty() && reportedSquawk.rfind(assignedSquawk, 0) != 0)
			result += assignedSquawk + ":" + reportedSquawk;
		else
			result += "I:" + reportedSquawk;

		result += " " + CopyText(flightData.GetOrigin()) + "==>" + CopyText(flightData.GetDestination());
		result += " (" + CopyText(flightData.GetAlternate()) + ") at ";
		int finalAltitude = assignedData.GetFinalAltitude();
		if (finalAltitude == 0)
			finalAltitude = flightData.GetFinalAltitude();
		if (finalAltitude > transitionAltitude)
			result += "FL" + std::to_string(finalAltitude / 100);
		else
			result += std::to_string(finalAltitude) + "ft";
		result += " Route: " + CopyText(flightData.GetRoute());
		return result;
	}

	std::string ResolveTagStatus(const Target& target)
	{
		if (target.role == TargetRole::Uncorrelated)
			return "default";
		if (target.airborne)
		{
			if (target.role == TargetRole::AirborneDeparture)
				return target.rimcas.onRunway ? "airdep_onrunway" : "airdep";
			return target.rimcas.onRunway ? "airarr_onrunway" : "airarr";
		}
		if (!target.flightPlanDataReceived)
			return "nofpl";
		if (target.role != TargetRole::Departure)
			return "default";
		switch (target.groundState)
		{
		case GroundStateCategory::Taxi: return "taxi";
		case GroundStateCategory::Lnup: return "lnup";
		case GroundStateCategory::Push: return "push";
		case GroundStateCategory::Stup: return "stup";
		case GroundStateCategory::Nsts: return "nsts";
		case GroundStateCategory::Depa: return "depa";
		default: return "default";
		}
	}

	std::string ResolveConfiguredTagStatus(
		const rapidjson::Value& labels,
		const std::string& type,
		const std::string& status)
	{
		if (status != "airdep_onrunway" && status != "airarr_onrunway")
			return status;
		if (!labels.IsObject() || !labels.HasMember(type.c_str()) || !labels[type.c_str()].IsObject())
			return status == "airdep_onrunway" ? "airdep" : "airarr";
		const rapidjson::Value& section = labels[type.c_str()];
		if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
			return status == "airdep_onrunway" ? "airdep" : "airarr";
		const rapidjson::Value& statuses = section["status_definitions"];
		if (statuses.HasMember(status.c_str()) && statuses[status.c_str()].IsObject())
			return status;
		return status == "airdep_onrunway" ? "airdep" : "airarr";
	}

	template <typename T>
	void HashSceneValue(std::uint64_t& seed, const T& value)
	{
		const std::uint64_t hashed = static_cast<std::uint64_t>(std::hash<T>{}(value));
		seed ^= hashed + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
	}

	std::uint64_t FingerprintController(const ControllerState& controller)
	{
		std::uint64_t result = 0xcbf29ce484222325ULL;
		HashSceneValue(result, controller.callsign);
		HashSceneValue(result, controller.positionId);
		HashSceneValue(result, controller.primaryFrequency);
		HashSceneValue(result, controller.mine);
		return result;
	}

	std::uint64_t FingerprintControllers(const std::vector<ControllerState>& controllers)
	{
		std::uint64_t xorValue = 0;
		std::uint64_t sumValue = 0;
		for (const ControllerState& controller : controllers)
		{
			const std::uint64_t item = FingerprintController(controller);
			xorValue ^= item;
			sumValue += item;
		}
		std::uint64_t result = 0xcbf29ce484222325ULL;
		HashSceneValue(result, controllers.size());
		HashSceneValue(result, xorValue);
		HashSceneValue(result, sumValue);
		return result;
	}

	std::uint64_t FingerprintTarget(const Target& target)
	{
		std::uint64_t result = 0xcbf29ce484222325ULL;
		HashSceneValue(result, target.normalizedCallsign);
		HashSceneValue(result, target.systemId);
		HashSceneValue(result, target.position.valid);
		HashSceneValue(result, target.position.latitude);
		HashSceneValue(result, target.position.longitude);
		HashSceneValue(result, target.reportedGroundSpeed);
		HashSceneValue(result, target.pressureAltitude);
		HashSceneValue(result, target.flightLevel);
		HashSceneValue(result, target.reportedHeadingDegrees);
		HashSceneValue(result, target.origin);
		HashSceneValue(result, target.destination);
		HashSceneValue(result, target.planType);
		HashSceneValue(result, target.aircraftType);
		HashSceneValue(result, target.assignedSquawk);
		HashSceneValue(result, target.reportedSquawk);
		HashSceneValue(result, target.groundStateText);
		HashSceneValue(result, target.hasFlightPlan);
		HashSceneValue(result, target.correlated);
		HashSceneValue(result, target.selected);
		HashSceneValue(result, target.iconVisible);
		HashSceneValue(result, target.tagVisible);
		HashSceneValue(result, static_cast<int>(target.groundState));
		HashSceneValue(result, static_cast<int>(target.role));
		HashSceneValue(result, target.tag.clearanceReceived);
		HashSceneValue(result, target.hasVacdmData);
		HashSceneValue(result, target.vacdmData.tobtState);
		HashSceneValue(result, target.vacdmData.tobtUtc);
		HashSceneValue(result, target.vacdmData.tsatUtc);
		HashSceneValue(result, target.vacdmData.ttotUtc);
		return result;
	}

	std::uint64_t FingerprintTargets(const std::vector<Target>& targets)
	{
		std::uint64_t xorValue = 0;
		std::uint64_t sumValue = 0;
		for (const Target& target : targets)
		{
			const std::uint64_t item = FingerprintTarget(target);
			xorValue ^= item;
			sumValue += item;
		}
		std::uint64_t result = 0xcbf29ce484222325ULL;
		HashSceneValue(result, targets.size());
		HashSceneValue(result, xorValue);
		HashSceneValue(result, sumValue);
		return result;
	}

	std::uint32_t InferRefreshReasonMask(
		const RadarScene* previous,
		const RadarScene& current) noexcept
	{
		using VsmrPerformance::FrameRefreshReason;
		using VsmrPerformance::RefreshReasonMask;
		if (previous == nullptr)
			return RefreshReasonMask(FrameRefreshReason::Initial);

		std::uint32_t result = 0;
		if (previous->airport.icao != current.airport.icao)
			result |= RefreshReasonMask(FrameRefreshReason::AirportUpdate);
		if (previous->profileName != current.profileName)
			result |= RefreshReasonMask(FrameRefreshReason::ProfileUpdate);
		if (previous->avisoGeneration != current.avisoGeneration)
			result |= RefreshReasonMask(FrameRefreshReason::AvisoDataChanged);
		if (previous->controllerFingerprint != current.controllerFingerprint)
			result |= RefreshReasonMask(FrameRefreshReason::ControllerUpdate);
		if (previous->targetFingerprint != current.targetFingerprint)
			result |= RefreshReasonMask(FrameRefreshReason::TargetOrFlightPlanUpdate);
		return result;
	}
}

const VsmrScene::Target* VsmrScene::RadarScene::FindTarget(const std::string& callsign) const noexcept
{
	const auto found = targetIndex.find(ToUpperAsciiCopy(callsign));
	if (found == targetIndex.end() || found->second >= targets.size())
		return nullptr;
	return &targets[found->second];
}

const VsmrScene::RadarScene* CSMRRadar::GetCurrentRadarScene() const noexcept
{
	return CurrentRadarScene.get();
}

std::shared_ptr<const VsmrScene::RadarScene> CSMRRadar::BuildRadarScene(
	bool lowVisibilityProcedures,
	double* outRimcasMilliseconds)
{
	using namespace VsmrScene;
	using Clock = std::chrono::steady_clock;
	const auto buildStart = Clock::now();
	const std::shared_ptr<const RadarScene> previousScene = CurrentRadarScene;
	if (outRimcasMilliseconds != nullptr)
		*outRimcasMilliseconds = 0.0;

	// ----- Preparing the scene buffer -----
	RadarSceneBuildBufferIndex = (RadarSceneBuildBufferIndex + 1) % RadarSceneBuffers.size();
	std::shared_ptr<RadarScene>& buildBuffer = RadarSceneBuffers[RadarSceneBuildBufferIndex];
	if (buildBuffer == nullptr || buildBuffer.use_count() != 1)
		buildBuffer = std::make_shared<RadarScene>();
	auto scene = buildBuffer;
	scene->airport = AirportState{};
	scene->targetPresentation = TargetPresentation{};
	scene->controllers.clear();
	scene->targets.clear();
	scene->targetIndex.clear();
	scene->avisoGeneration = 0;
	scene->controllerFingerprint = 0;
	scene->targetFingerprint = 0;
	scene->stats = BuildStats{};
	scene->frameId = ++RadarSceneFrameId;
	scene->captureTick = ::GetTickCount();
	auto measureSdkLookup = [&](auto&& callback)
	{
		const auto lookupStart = Clock::now();
		auto result = callback();
		scene->stats.sdkLookupMilliseconds += std::chrono::duration<double, std::milli>(
			Clock::now() - lookupStart).count();
		return result;
	};

	// ----- Capturing the airport and AVISO state -----
	CPlugIn* plugin = GetPlugIn();
	scene->airport.icao = getActiveAirport();
	scene->profileName = CurrentConfig != nullptr
		? CurrentConfig->getActiveProfileName()
		: std::string();
	scene->airport.lowVisibilityProcedures = lowVisibilityProcedures;
	scene->airport.transitionAltitude = plugin != nullptr
		? measureSdkLookup([&]() { return plugin->GetTransitionAltitude(); })
		: 0;
	CPosition airportPosition;
	if (TryGetActiveAirportPosition(airportPosition))
		scene->airport.referencePosition = CopyPosition(airportPosition);
	if (RimcasInstance != nullptr)
	{
		for (const auto& runway : RimcasInstance->GetRunwayStatuses())
			scene->airport.runwayStatuses[runway.first] = static_cast<int>(runway.second);
	}

	const auto avisoLoadStart = Clock::now();
	const std::string avisoPath = ResolveAvisoGeoJsonPathForAirport(scene->airport.icao);
	if (!avisoPath.empty())
		EnsureAvisoGeoJsonLoaded(avisoPath);
	scene->stats.avisoLoadMilliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - avisoLoadStart).count();

	if (plugin == nullptr)
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		scene->avisoGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
		scene->controllerFingerprint = FingerprintControllers(scene->controllers);
		scene->targetFingerprint = FingerprintTargets(scene->targets);
		scene->stats.refreshReasonMask = InferRefreshReasonMask(previousScene.get(), *scene);
		scene->stats.buildMilliseconds = std::chrono::duration<double, std::milli>(
			Clock::now() - buildStart).count();
		PerfLastSceneBuildMs = scene->stats.buildMilliseconds;
		CurrentRadarScene = scene;
		return CurrentRadarScene;
	}

	// ----- Capturing controllers -----
	const auto controllerCaptureStart = Clock::now();
	const CController myself = measureSdkLookup([&]() { return plugin->ControllerMyself(); });
	const std::string myCallsign = myself.IsValid() ? ToUpperAsciiCopy(CopyText(myself.GetCallsign())) : "";
	const std::string myPosition = myself.IsValid() ? ToUpperAsciiCopy(CopyText(myself.GetPositionId())) : "";
	auto captureController = [&](const CController& controller)
	{
		if (!controller.IsValid() || !controller.IsController())
			return;
		ControllerState captured;
		captured.callsign = ToUpperAsciiCopy(CopyText(controller.GetCallsign()));
		captured.positionId = ToUpperAsciiCopy(CopyText(controller.GetPositionId()));
		captured.primaryFrequency = controller.GetPrimaryFrequency();
		captured.mine = (!myCallsign.empty() && captured.callsign == myCallsign) ||
			(!myPosition.empty() && captured.positionId == myPosition);
		auto duplicate = std::find_if(scene->controllers.begin(), scene->controllers.end(), [&](const ControllerState& item)
		{
			return item.callsign == captured.callsign && item.positionId == captured.positionId;
		});
		if (duplicate == scene->controllers.end())
			scene->controllers.push_back(std::move(captured));
	};
	std::size_t controllerGuard = 0;
	CController controller = measureSdkLookup([&]() { return plugin->ControllerSelectFirst(); });
	for (; controller.IsValid() && controllerGuard < 4096; ++controllerGuard)
	{
		++scene->stats.sdkControllerEnumerations;
		captureController(controller);
		controller = measureSdkLookup([&]() { return plugin->ControllerSelectNext(controller); });
	}
	captureController(myself);
	{
		std::lock_guard<std::mutex> guard(AvisoGroupMutex);
		scene->avisoGeneration = AvisoGroupGeneration.load(std::memory_order_relaxed);
	}
	scene->stats.controllerCaptureMilliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - controllerCaptureStart).count();
	scene->controllerFingerprint = FingerprintControllers(scene->controllers);

	// ----- Loading target presentation settings -----
	const DisplayModeSettings displaySettings = GetActiveDisplayModeSettings();
	const CorrelationSettings correlationSettings = BuildCorrelationSettings();
	const std::string airportUpper = ToUpperAsciiCopy(scene->airport.icao);
	const bool proModeEnabled = displaySettings.requireAssignedSquawk;
	const bool towerModeEnabled = displaySettings.towerFilter;
	const rapidjson::Value* labels = nullptr;
	const rapidjson::Value* targetsConfig = nullptr;
	if (CurrentConfig != nullptr)
	{
		const rapidjson::Value& profile = CurrentConfig->getActiveProfile();
		if (profile.IsObject() && profile.HasMember("labels") && profile["labels"].IsObject())
			labels = &profile["labels"];
		if (profile.IsObject() && profile.HasMember("targets") && profile["targets"].IsObject())
			targetsConfig = &profile["targets"];
	}

	const std::string configuredIconStyle = GetActiveTargetIconStyle();
	IconStyle sceneIconStyle = IconStyle::Triangle;
	if (configuredIconStyle == "nova") sceneIconStyle = IconStyle::Nova;
	else if (configuredIconStyle == "realistic") sceneIconStyle = IconStyle::Realistic;
	else if (configuredIconStyle == "diamond") sceneIconStyle = IconStyle::Diamond;
	// NOVA's aircraft-shaped primary return is intrinsic to the style. The old
	// hidden toggle must not reduce it to only the secondary diamond/X symbol.
	const bool showPrimaryReturn = sceneIconStyle == IconStyle::Nova;
	scene->targetPresentation.icon = sceneIconStyle;
	scene->targetPresentation.showPrimaryReturn = showPrimaryReturn;
	auto configuredTrailPointCount = [&](const char* key, int fallback) -> int
	{
		if (targetsConfig == nullptr || !targetsConfig->HasMember(key) || !(*targetsConfig)[key].IsInt())
			return fallback;
		return std::clamp((*targetsConfig)[key].GetInt(), 0, 16);
	};
	scene->targetPresentation.trailEnabled = targetsConfig == nullptr ||
		!targetsConfig->HasMember("trail_enabled") ||
		!(*targetsConfig)["trail_enabled"].IsBool() ||
		(*targetsConfig)["trail_enabled"].GetBool();
	scene->targetPresentation.trailGroundPointCount = configuredTrailPointCount("trail_ground_points", 4);
	scene->targetPresentation.trailAirbornePointCount = configuredTrailPointCount("trail_airborne_points", 8);
	if (targetsConfig != nullptr && targetsConfig->HasMember("symbol_scale") && (*targetsConfig)["symbol_scale"].IsNumber())
		scene->targetPresentation.symbolScale = std::clamp((*targetsConfig)["symbol_scale"].GetDouble(), 0.5, 1.5);

	// ----- Capturing targets -----
	const auto targetCaptureStart = Clock::now();
	const CRadarTarget selectedTarget = measureSdkLookup([&]() { return plugin->RadarTargetSelectASEL(); });
	const std::string selectedCallsign = selectedTarget.IsValid() ? CopyText(selectedTarget.GetCallsign()) : "";

	double rimcasMilliseconds = 0.0;
	if (RimcasInstance != nullptr)
	{
		const auto rimcasBeginStart = Clock::now();
		RimcasInstance->OnRefreshBegin(lowVisibilityProcedures, scene->airport.transitionAltitude);
		rimcasMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - rimcasBeginStart).count();
	}

	scene->targets.reserve(CurrentRadarScene != nullptr ? CurrentRadarScene->targets.size() : 64);
	CRadarTarget radarTarget = measureSdkLookup([&]() { return plugin->RadarTargetSelectFirst(); });
	for (; radarTarget.IsValid();
		radarTarget = measureSdkLookup([&]() { return plugin->RadarTargetSelectNext(radarTarget); }))
	{
		++scene->stats.sdkTargetEnumerations;
		if (!radarTarget.IsValid())
			continue;
		const CRadarTargetPositionData position = radarTarget.GetPosition();
		if (!position.IsValid())
			continue;
		const std::string callsign = CopyText(radarTarget.GetCallsign());
		if (callsign.empty())
			continue;

		Target target;
		target.callsign = callsign;
		target.normalizedCallsign = ToUpperAsciiCopy(callsign);
		target.systemId = CopyText(radarTarget.GetSystemID());
		target.position = CopyPosition(position.GetPosition());
		target.reportedGroundSpeed = position.GetReportedGS();
		target.groundSpeed = radarTarget.GetGS();
		++scene->stats.sdkPreviousPositionLookups;
		const CRadarTargetPositionData previousPosition = measureSdkLookup(
			[&]() { return radarTarget.GetPreviousPosition(position); });
		if (previousPosition.IsValid())
		{
			target.previousPosition = CopyPosition(previousPosition.GetPosition());
			target.previousFlightLevel = previousPosition.GetFlightLevel();
		}
		const int effectiveGroundSpeed = (std::max)(target.reportedGroundSpeed, target.groundSpeed);
		if (scene->targetPresentation.trailEnabled && effectiveGroundSpeed > 5 && previousPosition.IsValid())
		{
			const int requestedPointCount = effectiveGroundSpeed > 50
				? scene->targetPresentation.trailAirbornePointCount
				: scene->targetPresentation.trailGroundPointCount;
			target.trailPositions.reserve(static_cast<std::size_t>(requestedPointCount));
			CRadarTargetPositionData trailPosition = previousPosition;
			for (int index = 0; index < requestedPointCount && trailPosition.IsValid(); ++index)
			{
				target.trailPositions.push_back(CopyPosition(trailPosition.GetPosition()));
				if (index + 1 >= requestedPointCount)
					break;
				++scene->stats.sdkPreviousPositionLookups;
				trailPosition = measureSdkLookup(
					[&]() { return radarTarget.GetPreviousPosition(trailPosition); });
			}
		}
		target.pressureAltitude = position.GetPressureAltitude();
		target.flightLevel = position.GetFlightLevel();
		target.reportedSquawk = CopyText(position.GetSquawk());
		target.transponderModeC = position.GetTransponderC();
		target.reportedHeadingDegrees = position.GetReportedHeading();
		target.trackHeadingDegrees = radarTarget.GetTrackHeading();
		target.headingTrueDegrees = static_cast<double>(position.GetReportedHeadingTrueNorth());
		if (!std::isfinite(target.headingTrueDegrees) || target.headingTrueDegrees < 0.0 || target.headingTrueDegrees >= 360.0)
			target.headingTrueDegrees = target.trackHeadingDegrees;
		target.headingProbe = CopyPosition(Haversine(position.GetPosition(), target.headingTrueDegrees, 50.0));
		target.selected = !selectedCallsign.empty() && selectedCallsign == callsign;
		// Retain the historical diagnostics counter name, but all valid targets
		// are now eligible; hidden profile filters no longer exist.
		++scene->stats.radarFilteredTargetCount;

		++scene->stats.sdkFlightPlanLookups;
		const CFlightPlan flightPlan = measureSdkLookup(
			[&]() { return plugin->FlightPlanSelect(callsign.c_str()); });
		target.hasFlightPlan = flightPlan.IsValid();
		if (flightPlan.IsValid())
		{
			const CFlightPlanData flightData = flightPlan.GetFlightPlanData();
			const CFlightPlanControllerAssignedData assignedData = flightPlan.GetControllerAssignedData();
			target.flightPlanDataReceived = flightData.IsReceived();
			target.origin = CopyText(flightData.GetOrigin());
			target.destination = CopyText(flightData.GetDestination());
			target.planType = CopyText(flightData.GetPlanType());
			target.aircraftType = CopyText(flightData.GetAircraftFPType());
			if (target.aircraftType.size() > 4)
				target.aircraftType.resize(4);
			target.wakeCategory = flightData.GetAircraftWtc();
			target.assignedSquawk = CopyText(assignedData.GetSquawk());
			target.groundStateText = CopyText(flightPlan.GetGroundState());
			target.tag.clearanceReceived = flightPlan.GetClearenceFlag();
		}

		target.correlated = IsCorrelatedWithSettings(flightPlan, radarTarget, correlationSettings);
		const bool hasAssignedSquawk = !target.assignedSquawk.empty();
		const bool hasReportedSquawk = !target.reportedSquawk.empty();
		const bool wrongSquawk = hasAssignedSquawk && hasReportedSquawk && target.assignedSquawk != target.reportedSquawk;
		if (proModeEnabled && !hasAssignedSquawk)
			target.correlated = false;
		const bool keepIconForSquawkMismatch = proModeEnabled && (wrongSquawk || !hasAssignedSquawk);
		const bool withinAirborneDisplayLimits = IsWithinAirborneDisplayLimits(
			target.reportedGroundSpeed,
			target.pressureAltitude,
			displaySettings);
		target.iconVisible = withinAirborneDisplayLimits &&
			(target.correlated || target.reportedGroundSpeed >= 1 || keepIconForSquawkMismatch);
		if (target.iconVisible)
			++scene->stats.iconTargetCount;

		target.towerModeGroundStateText = target.groundStateText;
		target.towerModeArrival = target.hasFlightPlan && !airportUpper.empty() &&
			_stricmp(target.destination.c_str(), airportUpper.c_str()) == 0;
		const bool needsCorrelatedFlightPlan = towerModeEnabled || RimcasInstance != nullptr;
		if (needsCorrelatedFlightPlan)
		{
			++scene->stats.sdkCorrelatedFlightPlanLookups;
			const CFlightPlan correlatedFlightPlan = measureSdkLookup(
				[&]() { return radarTarget.GetCorrelatedFlightPlan(); });
			target.hasCorrelatedFlightPlan = correlatedFlightPlan.IsValid();
			if (correlatedFlightPlan.IsValid())
			{
				target.towerModeGroundStateText = CopyText(correlatedFlightPlan.GetGroundState());
				const std::string correlatedDestination = CopyText(correlatedFlightPlan.GetFlightPlanData().GetDestination());
				target.towerModeArrival = !airportUpper.empty() &&
					_stricmp(correlatedDestination.c_str(), airportUpper.c_str()) == 0;
			}
		}

		// Safety processing is deliberately independent of display visibility.
		// A profile/display setting must never hide a target from RIMCAS.
		if (RimcasInstance != nullptr)
		{
			const auto rimcasTargetStart = Clock::now();
			RimcasInstance->OnRefresh(target, this);
			rimcasMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - rimcasTargetStart).count();
		}

		target.rimcas.onRunway = RimcasInstance != nullptr && RimcasInstance->isAcOnRunway(callsign);
		target.groundState = target.hasFlightPlan
			? classifyGroundStateForCallsign(callsign.c_str(), target.groundStateText.c_str(), target.reportedGroundSpeed, target.rimcas.onRunway)
			: GroundStateCategory::Unknown;
		target.departure = target.hasFlightPlan && target.correlated && !airportUpper.empty() &&
			_stricmp(target.origin.c_str(), airportUpper.c_str()) == 0;
		target.arrival = target.hasFlightPlan && target.correlated && !target.departure && !airportUpper.empty() &&
			_stricmp(target.destination.c_str(), airportUpper.c_str()) == 0;
		target.airborne = VsmrTargetRoleLogic::IsAirborneForTagRole(
			target.arrival,
			target.reportedGroundSpeed);
		if (!target.correlated && target.reportedGroundSpeed >= 3)
			target.role = TargetRole::Uncorrelated;
		else if (target.airborne)
			target.role = target.arrival ? TargetRole::AirborneArrival : TargetRole::AirborneDeparture;
		else
			target.role = target.arrival ? TargetRole::Arrival : TargetRole::Departure;

		++scene->stats.vacdmLookups;
		target.hasVacdmData = TryGetVacdmPilotDataForTarget(radarTarget, flightPlan, target.vacdmData);
		const VacdmPilotData* capturedVacdmData = target.hasVacdmData ? &target.vacdmData : nullptr;
		target.passesDisplayMode = ShouldDisplayTargetForDisplayMode(
			flightPlan,
			target.correlated,
			target.reportedGroundSpeed,
			target.pressureAltitude,
			target.rimcas.onRunway,
			displaySettings,
			capturedVacdmData);
		bool tagVisible = target.passesDisplayMode;
		if (proModeEnabled && (!hasAssignedSquawk || wrongSquawk))
			tagVisible = false;
		if (!target.correlated && target.reportedGroundSpeed < 3)
			tagVisible = false;
		if (towerModeEnabled && !target.towerModeArrival &&
			!shouldDisplayTagInTowerMode(target.towerModeGroundStateText.c_str(), target.reportedGroundSpeed, target.rimcas.onRunway))
		{
			tagVisible = false;
		}
		target.tagVisible = tagVisible;
		if (target.tagVisible)
			++scene->stats.tagTargetCount;

		target.bottomLine = BuildBottomLine(*this, flightPlan, position, scene->airport.transitionAltitude);
		const int* capturedPreviousFlightLevel = target.previousPosition.valid
			? &target.previousFlightLevel
			: nullptr;
		target.tag.tokens = GenerateTagData(
			radarTarget,
			flightPlan,
			target.selected,
			target.correlated,
			proModeEnabled,
			scene->airport.transitionAltitude,
			scene->airport.icao,
			callsign,
			capturedVacdmData,
			capturedPreviousFlightLevel);
		switch (target.role)
		{
		case TargetRole::Arrival:
		case TargetRole::AirborneArrival:
			target.tag.definitionType = "arrival";
			break;
		case TargetRole::Uncorrelated:
			target.tag.definitionType = "uncorrelated";
			break;
		default:
			target.tag.definitionType = "departure";
			break;
		}

		target.style.icon = sceneIconStyle;
		target.style.showPrimaryReturn = showPrimaryReturn;
		std::string aircraftKey = ToLowerAsciiCopy(target.aircraftType);
		auto fallbackAircraftKey = [](char wake) -> std::string
		{
			switch (std::toupper(static_cast<unsigned char>(wake)))
			{
			case 'L': return "c172";
			case 'H': return "b77w";
			case 'J': return "a388";
			default: return "a320";
			}
		};
		if (sceneIconStyle == IconStyle::Realistic && GetAircraftIcon(aircraftKey) == nullptr)
			aircraftKey = fallbackAircraftKey(target.wakeCategory);
		target.style.assetKey = aircraftKey;
		auto spec = AircraftSpecs.find(ToLowerAsciiCopy(target.aircraftType));
		if (spec == AircraftSpecs.end())
			spec = AircraftSpecs.find(aircraftKey);
		if (spec != AircraftSpecs.end())
		{
			target.style.lengthMeters = spec->second.length;
			target.style.wingspanMeters = spec->second.wingspan;
		}
		if (target.style.lengthMeters <= 0.0 || target.style.wingspanMeters <= 0.0)
		{
			switch (std::toupper(static_cast<unsigned char>(target.wakeCategory)))
			{
			case 'L': target.style.lengthMeters = 28.0; target.style.wingspanMeters = 28.0; break;
			case 'H': target.style.lengthMeters = 60.0; target.style.wingspanMeters = 60.0; break;
			case 'J': target.style.lengthMeters = 72.0; target.style.wingspanMeters = 80.0; break;
			default: target.style.lengthMeters = 40.0; target.style.wingspanMeters = 36.0; break;
			}
		}
		const auto primaryReturn = Patatoides.find(callsign);
		if (primaryReturn != Patatoides.end())
		{
			auto capturePolygon = [](const std::map<int, POINT2>& source, std::vector<GeoPoint>& destination)
			{
				destination.reserve(source.size());
				for (const auto& point : source)
					destination.push_back(GeoPoint{ point.second.x, point.second.y, true });
			};
			capturePolygon(primaryReturn->second.points, target.primaryReturnPolygon);
			if (scene->targetPresentation.trailEnabled && effectiveGroundSpeed > 5)
			{
				capturePolygon(primaryReturn->second.historyOnePoints, target.primaryReturnAfterglow[0]);
				capturePolygon(primaryReturn->second.historyTwoPoints, target.primaryReturnAfterglow[1]);
				capturePolygon(primaryReturn->second.historyThreePoints, target.primaryReturnAfterglow[2]);
			}
		}

		scene->targetIndex.emplace(target.normalizedCallsign, scene->targets.size());
		scene->targets.push_back(std::move(target));
	}
	scene->stats.targetCaptureMilliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - targetCaptureStart).count();

	// ----- Finalizing RIMCAS and tag presentation -----
	const auto finalizeStart = Clock::now();
	if (RimcasInstance != nullptr)
	{
		const auto rimcasEndStart = Clock::now();
		RimcasInstance->OnRefreshEnd(*scene, kRimcasStageTwoSpeedThresholdKt);
		rimcasMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - rimcasEndStart).count();
	}
	if (outRimcasMilliseconds != nullptr)
		*outRimcasMilliseconds = rimcasMilliseconds;

	const Gdiplus::Color whiteColor(static_cast<Gdiplus::ARGB>(Gdiplus::Color::White));
	static const std::vector<StructuredTagColorRule> emptyStructuredRules;
	const std::vector<StructuredTagColorRule>& structuredRules = displaySettings.structuredRulesEnabled
		? GetStructuredTagColorRules()
		: emptyStructuredRules;
	struct TagDefinitionColorRules
	{
		std::vector<TagColorRules::VacdmColorRuleDefinition> vacdm;
		std::vector<TagColorRules::RunwayColorRuleDefinition> runway;
	};
	std::unordered_map<std::string, TagDefinitionColorRules> tagDefinitionColorRuleCache;
	auto resolveTagDefinitionColorRules = [&](const std::string& type, const std::string& status, bool detailed) -> const TagDefinitionColorRules&
	{
		const std::string key = type + "|" + status + (detailed ? "|d" : "|n");
		auto found = tagDefinitionColorRuleCache.find(key);
		if (found == tagDefinitionColorRuleCache.end())
		{
			TagDefinitionColorRules rules;
			if (labels != nullptr)
			{
				const rapidjson::Value* definition = ResolveTagDefinition(*labels, type, status, detailed);
				if (definition != nullptr)
				{
					const std::vector<std::string> lines = TagColorRules::ConvertDefinitionValueToLineTexts(*definition);
					TagColorRules::CollectVacdmColorRulesFromLineTexts(lines, rules.vacdm);
					TagColorRules::CollectRunwayColorRulesFromLineTexts(lines, rules.runway);
				}
			}
			found = tagDefinitionColorRuleCache.emplace(key, std::move(rules)).first;
		}
		return found->second;
	};
	auto isColorObject = [](const rapidjson::Value& value) -> bool
	{
		return value.IsObject() && value.HasMember("r") && value["r"].IsInt() &&
			value.HasMember("g") && value["g"].IsInt() && value.HasMember("b") && value["b"].IsInt();
	};
	auto readColor = [&](const rapidjson::Value& object, const char* key, Gdiplus::Color& output) -> bool
	{
		if (CurrentConfig == nullptr || key == nullptr || !object.HasMember(key) || !isColorObject(object[key]))
			return false;
		output = CurrentConfig->getConfigColor(object[key]);
		return true;
	};
	auto groundColor = [&](const char* key, const Gdiplus::Color& fallback) -> Gdiplus::Color
	{
		if (targetsConfig == nullptr)
			return fallback;
		Gdiplus::Color resolved;
		auto section = [&](const char* sectionKey, const char* colorKey) -> bool
		{
			return targetsConfig->HasMember(sectionKey) && (*targetsConfig)[sectionKey].IsObject() &&
				readColor((*targetsConfig)[sectionKey], colorKey, resolved);
		};
		if ((_stricmp(key, "nofpl") == 0 || _stricmp(key, "no_fpl") == 0) && section("departure", "no_fpl")) return resolved;
		if ((_stricmp(key, "nsts") == 0 || _stricmp(key, "no_status") == 0) && section("departure", "no_status")) return resolved;
		if (_stricmp(key, "push") == 0 && section("departure", "push")) return resolved;
		if ((_stricmp(key, "stup") == 0 || _stricmp(key, "startup") == 0) && section("departure", "startup")) return resolved;
		if (_stricmp(key, "taxi") == 0 && section("departure", "taxi")) return resolved;
		if ((_stricmp(key, "lnup") == 0 || _stricmp(key, "lineup") == 0) && section("departure", "lineup")) return resolved;
		if ((_stricmp(key, "depa") == 0 || _stricmp(key, "departure") == 0) && section("departure", "departure")) return resolved;
		if (_stricmp(key, "departure_gate") == 0 && section("departure", "gate")) return resolved;
		if (_stricmp(key, "airborne_departure") == 0 && section("departure", "airborne")) return resolved;
		if (_stricmp(key, "arrival_gate") == 0 && section("arrival", "gate")) return resolved;
		if ((_stricmp(key, "arr") == 0 || _stricmp(key, "on_ground") == 0) && section("arrival", "on_ground")) return resolved;
		if (_stricmp(key, "airborne_arrival") == 0 && section("arrival", "airborne")) return resolved;
		if (_stricmp(key, "gate") == 0 && (section("departure", "gate") || section("arrival", "gate"))) return resolved;
		if (targetsConfig->HasMember("ground_icons") && (*targetsConfig)["ground_icons"].IsObject() &&
			readColor((*targetsConfig)["ground_icons"], key, resolved)) return resolved;
		return fallback;
	};
	auto evaluateTagColorRules = [&](const Target& target, bool detailed) -> TagColorRules::VacdmColorRuleOverrides
	{
		const TagDefinitionColorRules& definitionRules = resolveTagDefinitionColorRules(
			target.tag.definitionType,
			target.tag.status,
			detailed);
		const VacdmPilotData* pilotData = target.hasVacdmData ? &target.vacdmData : nullptr;
		TagColorRules::VacdmColorRuleOverrides overrides = TagColorRules::EvaluateVacdmColorRules(definitionRules.vacdm, pilotData);
		TagColorRules::MergeColorRuleOverrides(
			overrides,
			TagColorRules::EvaluateRunwayColorRules(definitionRules.runway, target.tag.tokens));

		TagColorRules::VacdmColorRuleOverrides structuredOverrides = TagColorRules::EvaluateStructuredTagColorRules(
			structuredRules,
			target.tag.definitionType,
			target.tag.status == "default" ? nullptr : target.tag.status.c_str(),
			detailed,
			target.tag.tokens,
			pilotData);
		if (detailed)
		{
			const TagColorRules::VacdmColorRuleOverrides normalStructuredOverrides = TagColorRules::EvaluateStructuredTagColorRules(
				structuredRules,
				target.tag.definitionType,
				target.tag.status == "default" ? nullptr : target.tag.status.c_str(),
				false,
				target.tag.tokens,
				pilotData);
			TagColorRules::MergeMissingColorRuleOverrides(structuredOverrides, normalStructuredOverrides);
		}
		TagColorRules::MergeColorRuleOverrides(overrides, structuredOverrides);
		return overrides;
	};
	auto resolveBaseTagPalette = [&](const Target& target) -> TagPalette
	{
		TagPalette palette;
		if (labels == nullptr || CurrentConfig == nullptr)
			return palette;

		std::string colorTypeKey = "departure";
		switch (target.role)
		{
		case TargetRole::Arrival:
			colorTypeKey = "arrival";
			break;
		case TargetRole::AirborneArrival:
			colorTypeKey = "airborne";
			break;
		case TargetRole::AirborneDeparture:
			colorTypeKey = "airborne";
			break;
		case TargetRole::Uncorrelated:
			colorTypeKey = "uncorrelated";
			break;
		default:
			break;
		}

		const rapidjson::Value* colorSection = nullptr;
		if (labels->HasMember(colorTypeKey.c_str()) && (*labels)[colorTypeKey.c_str()].IsObject())
			colorSection = &(*labels)[colorTypeKey.c_str()];
		auto colorWithLegacy = [&](const char* preferredKey, const char* legacyKey, const Gdiplus::Color& fallback) -> Gdiplus::Color
		{
			Gdiplus::Color resolved;
			if (colorSection != nullptr && readColor(*colorSection, preferredKey, resolved))
				return resolved;
			if (legacyKey != nullptr && colorSection != nullptr && readColor(*colorSection, legacyKey, resolved))
				return resolved;
			return fallback;
		};
		auto colorOrDefault = [&](const char* key, const Gdiplus::Color& fallback) -> Gdiplus::Color
		{
			Gdiplus::Color resolved;
			return colorSection != nullptr && readColor(*colorSection, key, resolved) ? resolved : fallback;
		};

		Gdiplus::Color background(255, 53, 126, 187);
		Gdiplus::Color backgroundOnRunway = background;
		Gdiplus::Color text = whiteColor;
		if (colorTypeKey == "departure")
		{
			background = colorWithLegacy("background_no_status_color", "gate_color", Gdiplus::Color(255, 53, 126, 187));
			backgroundOnRunway = colorWithLegacy("background_on_runway_color", "on_runway_color", background);
			text = colorWithLegacy("text_on_ground_color", "text_color", whiteColor);
		}
		else if (colorTypeKey == "arrival")
		{
			background = colorWithLegacy("background_on_ground_color", "background_color", Gdiplus::Color(255, 191, 87, 91));
			backgroundOnRunway = colorWithLegacy("background_on_runway_color", "background_color_on_runway", background);
			text = colorWithLegacy("text_on_ground_color", "text_color", whiteColor);
		}
		else if (colorTypeKey == "uncorrelated")
		{
			background = colorWithLegacy("background_on_ground_color", "background_color", Gdiplus::Color(255, 150, 22, 135));
			backgroundOnRunway = colorWithLegacy("background_on_runway_color", "background_color_on_runway", background);
			text = colorWithLegacy("text_on_ground_color", "text_color", whiteColor);
		}
		else
		{
			background = colorOrDefault("background_color", Gdiplus::Color(255, 53, 126, 187));
			backgroundOnRunway = colorOrDefault("background_color_on_runway", background);
			text = colorOrDefault("text_color", whiteColor);
		}

		const auto tagValue = [&](const char* key) -> const std::string&
		{
			static const std::string empty;
			const auto found = target.tag.tokens.find(key);
			return found != target.tag.tokens.end() ? found->second : empty;
		};
		if (colorTypeKey == "departure")
		{
			const std::string& assignedSid = !tagValue("asid").empty() ? tagValue("asid") : tagValue("sid");
			if (!assignedSid.empty() && CurrentConfig->isSidColorAvail(assignedSid, scene->airport.icao))
				background = CurrentConfig->getSidColor(assignedSid, scene->airport.icao);
			if (target.hasFlightPlan && !target.planType.empty() && target.planType[0] == 'I' && assignedSid.empty())
				background = colorWithLegacy("background_no_sid_color", "nosid_color", background);

			if (labels->HasMember("departure") && (*labels)["departure"].IsObject())
			{
				const rapidjson::Value& departureSection = (*labels)["departure"];
				const char* statusColorKey = "background_no_status_color";
				const char* legacyStatusColorKey = "nsts";
				switch (target.groundState)
				{
				case GroundStateCategory::Taxi: statusColorKey = "background_taxi_color"; legacyStatusColorKey = "taxi"; break;
				case GroundStateCategory::Lnup: statusColorKey = "background_lineup_color"; legacyStatusColorKey = "lnup"; break;
				case GroundStateCategory::Push: statusColorKey = "background_push_color"; legacyStatusColorKey = "push"; break;
				case GroundStateCategory::Stup: statusColorKey = "background_startup_color"; legacyStatusColorKey = "stup"; break;
				case GroundStateCategory::Depa: statusColorKey = "background_departure_color"; legacyStatusColorKey = "depa"; break;
				default: break;
				}
				Gdiplus::Color statusColor;
				if (readColor(departureSection, statusColorKey, statusColor))
				{
					background = statusColor;
				}
				else if (departureSection.HasMember("status_background_colors") &&
					departureSection["status_background_colors"].IsObject() &&
					readColor(departureSection["status_background_colors"], legacyStatusColorKey, statusColor))
				{
					background = statusColor;
				}
			}
		}
		if (tagValue("actype") == "NoFPL")
			background = colorWithLegacy("background_no_fpl_color", "nofpl_color", background);

		const bool isAirborneRole = target.role == TargetRole::AirborneDeparture || target.role == TargetRole::AirborneArrival;
		if (isAirborneRole && target.hasFlightPlan && target.correlated)
		{
			const bool departure = target.role == TargetRole::AirborneDeparture;
			const char* runwaySectionKey = departure ? "departure" : "arrival";
			if (labels->HasMember(runwaySectionKey) && (*labels)[runwaySectionKey].IsObject())
			{
				const rapidjson::Value& runwaySection = (*labels)[runwaySectionKey];
				readColor(runwaySection, "background_airborne_color", background);
				readColor(runwaySection, "text_airborne_color", text);
				if (!readColor(runwaySection, "background_on_runway_color", backgroundOnRunway) &&
					(!departure || !readColor(runwaySection, "on_runway_color", backgroundOnRunway)))
				{
					readColor(runwaySection, "background_color_on_runway", backgroundOnRunway);
				}
			}
			else if (labels->HasMember("airborne") && (*labels)["airborne"].IsObject())
			{
				const rapidjson::Value& airborneSection = (*labels)["airborne"];
				readColor(airborneSection, departure ? "departure_background_color" : "arrival_background_color", background);
				readColor(airborneSection, departure ? "departure_text_color" : "arrival_text_color", text);
				readColor(airborneSection,
					departure ? "departure_background_color_on_runway" : "arrival_background_color_on_runway",
					backgroundOnRunway);
			}
		}

		palette.background = CopyColor(background);
		palette.backgroundOnRunway = CopyColor(backgroundOnRunway);
		palette.text = CopyColor(text);
		return palette;
	};
	auto applyTagColorRules = [](TagPalette& palette, const TagColorRules::VacdmColorRuleOverrides& overrides)
	{
		auto channel = [](int value) -> std::uint8_t
		{
			return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
		};
		if (overrides.hasTagColor)
		{
			palette.background = VsmrScene::Color{
				channel(overrides.tagA), channel(overrides.tagR), channel(overrides.tagG), channel(overrides.tagB) };
			palette.backgroundOnRunway = palette.background;
		}
		if (overrides.hasTextColor)
		{
			palette.text = VsmrScene::Color{
				channel(overrides.textA), channel(overrides.textR), channel(overrides.textG), channel(overrides.textB) };
			palette.textRuleApplied = true;
		}
	};
	Gdiplus::Color squawkErrorColor(255, 255, 0, 0);
	if (labels != nullptr)
		readColor(*labels, "squawk_error_color", squawkErrorColor);
	const VsmrScene::Color capturedSquawkErrorColor = CopyColor(squawkErrorColor);
	auto applyTagElementColors = [&](Target& target, TagVariant& variant, const TagPalette& palette)
	{
		const auto sqError = target.tag.tokens.find("sqerror");
		const std::string* sqErrorText = sqError != target.tag.tokens.end() && !sqError->second.empty()
			? &sqError->second
			: nullptr;
		for (TagLine& line : variant.lines)
		{
			for (TagElement& element : line.elements)
			{
				element.effectiveColor = palette.text;
				const std::string& token = element.token;
				if (sqErrorText != nullptr && element.text == *sqErrorText)
				{
					element.effectiveColor = capturedSquawkErrorColor;
				}
				else if (!palette.textRuleApplied && target.hasVacdmData && (token == "tobt" || token == "tsat"))
				{
					int red = 255;
					int green = 255;
					int blue = 255;
					const bool resolved = token == "tobt"
						? TryResolveVacdmTobtTextColor(target.vacdmData, red, green, blue)
						: (token == "tsat" && TryResolveVacdmTsatTextColor(target.vacdmData, red, green, blue));
					if (resolved)
					{
						element.effectiveColor = VsmrScene::Color{
							255,
							static_cast<std::uint8_t>(std::clamp(red, 0, 255)),
							static_cast<std::uint8_t>(std::clamp(green, 0, 255)),
							static_cast<std::uint8_t>(std::clamp(blue, 0, 255)) };
					}
				}
				else if (element.clearanceToken)
				{
					element.effectiveColor = target.tag.clearanceReceived
						? VsmrScene::Color{ 255, 95, 225, 120 }
						: VsmrScene::Color{ 255, 235, 70, 70 };
				}
				if (element.hasCustomColor)
					element.effectiveColor = element.customColor;
			}
		}
	};

	for (Target& target : scene->targets)
	{
		target.rimcas.onRunway = RimcasInstance != nullptr && RimcasInstance->isAcOnRunway(target.callsign);
		target.rimcas.alertStage = RimcasInstance != nullptr ? static_cast<int>(RimcasInstance->getAlert(target.callsign)) : 0;
		target.rimcas.movementAlert = RimcasInstance != nullptr ? static_cast<int>(RimcasInstance->getMovementAlert(target.callsign)) : 0;
		if (RimcasInstance != nullptr)
			target.rimcas.severity = static_cast<int>(RimcasInstance->getAlertSeverity(static_cast<CRimcas::RimcasAlerts>(target.rimcas.movementAlert)));
		target.groundState = target.hasFlightPlan
			? classifyGroundStateForCallsign(target.callsign.c_str(), target.groundStateText.c_str(), target.reportedGroundSpeed, target.rimcas.onRunway)
			: GroundStateCategory::Unknown;
		target.tag.status = ResolveTagStatus(target);
		if (labels != nullptr)
			target.tag.status = ResolveConfiguredTagStatus(*labels, target.tag.definitionType, target.tag.status);
		if (labels != nullptr)
		{
			target.tag.normal = BuildTagVariant(*labels, target, false);
			target.tag.detailed = BuildTagVariant(*labels, target, true);
		}
		const TagColorRules::VacdmColorRuleOverrides normalTagColorOverrides = evaluateTagColorRules(target, false);
		const TagColorRules::VacdmColorRuleOverrides detailedTagColorOverrides = evaluateTagColorRules(target, true);
		target.tag.normalPalette = resolveBaseTagPalette(target);
		target.tag.detailedPalette = target.tag.normalPalette;
		applyTagColorRules(target.tag.normalPalette, normalTagColorOverrides);
		applyTagColorRules(target.tag.detailedPalette, detailedTagColorOverrides);
		applyTagElementColors(target, target.tag.normal, target.tag.normalPalette);
		applyTagElementColors(target, target.tag.detailed, target.tag.detailedPalette);
		Gdiplus::Color primaryReturnColor(255, 255, 242, 73);
		if (targetsConfig != nullptr)
			readColor(*targetsConfig, "target_color", primaryReturnColor);
		target.style.primaryReturnColor = CopyColor(primaryReturnColor);

		Gdiplus::Color color = whiteColor;
		if (!target.hasFlightPlan || (!target.airborne && !target.flightPlanDataReceived))
			color = groundColor("nofpl", groundColor("gate", Gdiplus::Color(255, 128, 128, 128)));
		else if (target.airborne)
			color = target.role == TargetRole::AirborneDeparture
				? groundColor("airborne_departure", groundColor("depa", Gdiplus::Color(255, 240, 240, 240)))
				: groundColor("airborne_arrival", groundColor("arr", Gdiplus::Color(255, 120, 190, 240)));
		else if (target.role == TargetRole::Departure)
		{
			switch (target.groundState)
			{
			case GroundStateCategory::Gate: color = groundColor("departure_gate", groundColor("gate", Gdiplus::Color(255, 165, 165, 165))); break;
			case GroundStateCategory::Push: color = groundColor("push", Gdiplus::Color(255, 253, 218, 13)); break;
			case GroundStateCategory::Stup: color = groundColor("stup", Gdiplus::Color(255, 253, 218, 13)); break;
			case GroundStateCategory::Taxi: color = groundColor("taxi", Gdiplus::Color(255, 240, 240, 240)); break;
			case GroundStateCategory::Lnup: color = groundColor("lnup", groundColor("taxi", Gdiplus::Color(255, 240, 240, 240))); break;
			case GroundStateCategory::Depa: color = groundColor("depa", groundColor("taxi", Gdiplus::Color(255, 240, 240, 240))); break;
			case GroundStateCategory::Nsts: color = groundColor("nsts", groundColor("departure_gate", groundColor("gate", Gdiplus::Color(255, 165, 165, 165)))); break;
			default: break;
			}
		}
		else
		{
			color = (target.groundState == GroundStateCategory::Gate || target.groundState == GroundStateCategory::Nsts)
				? groundColor("arrival_gate", groundColor("gate", Gdiplus::Color(255, 165, 165, 165)))
				: groundColor("arr", groundColor("arrival_gate", groundColor("gate", Gdiplus::Color(255, 165, 165, 165))));
		}

		if (normalTagColorOverrides.hasTargetColor)
		{
			color = Gdiplus::Color(
				static_cast<BYTE>(std::clamp(normalTagColorOverrides.targetA, 0, 255)),
				static_cast<BYTE>(std::clamp(normalTagColorOverrides.targetR, 0, 255)),
				static_cast<BYTE>(std::clamp(normalTagColorOverrides.targetG, 0, 255)),
				static_cast<BYTE>(std::clamp(normalTagColorOverrides.targetB, 0, 255)));
		}

		if (target.rimcas.movementAlert == static_cast<int>(CRimcas::EMERG))
		{
			color = Gdiplus::Color(255, 255, 0, 0);
			target.style.primaryReturnColor = CopyColor(color);
		}
		target.style.color = CopyColor(color);

		scene->stats.tagElementCount += [&]()
		{
			std::size_t count = 0;
			for (const TagLine& line : target.tag.normal.lines) count += line.elements.size();
			for (const TagLine& line : target.tag.detailed.lines) count += line.elements.size();
			return count;
		}();
	}

	// ----- Collecting scene diagnostics -----
	scene->stats.targetCount = scene->targets.size();
	scene->stats.controllerCount = scene->controllers.size();
	std::size_t estimatedBytes = sizeof(RadarScene) + scene->targets.capacity() * sizeof(Target) +
		scene->controllers.capacity() * sizeof(ControllerState);
	estimatedBytes += EstimateStringHeapBytes(scene->airport.icao) +
		EstimateStringHeapBytes(scene->profileName);
	for (const ControllerState& controllerState : scene->controllers)
		estimatedBytes += EstimateStringHeapBytes(controllerState.callsign) + EstimateStringHeapBytes(controllerState.positionId);
	for (const Target& target : scene->targets)
	{
		const std::string* targetStrings[] = {
			&target.callsign,
			&target.normalizedCallsign,
			&target.systemId,
			&target.bottomLine,
			&target.origin,
			&target.destination,
			&target.planType,
			&target.aircraftType,
			&target.assignedSquawk,
			&target.reportedSquawk,
			&target.groundStateText,
			&target.towerModeGroundStateText,
			&target.style.assetKey,
			&target.tag.definitionType,
			&target.tag.status,
			&target.vacdmData.callsign,
			&target.vacdmData.tobtState
		};
		for (const std::string* value : targetStrings)
			estimatedBytes += EstimateStringHeapBytes(*value);
		estimatedBytes +=
			(target.trailPositions.capacity() + target.primaryReturnPolygon.capacity()) * sizeof(GeoPoint);
		for (const std::vector<GeoPoint>& afterglow : target.primaryReturnAfterglow)
			estimatedBytes += afterglow.capacity() * sizeof(GeoPoint);
		for (const auto& token : target.tag.tokens)
			estimatedBytes += EstimateStringHeapBytes(token.first) + EstimateStringHeapBytes(token.second);
		estimatedBytes += EstimateTagVariantHeapBytes(target.tag.normal);
		estimatedBytes += EstimateTagVariantHeapBytes(target.tag.detailed);
	}
	scene->stats.lowerBoundOwnedBytes = estimatedBytes;
	scene->targetFingerprint = FingerprintTargets(scene->targets);
	scene->stats.refreshReasonMask = InferRefreshReasonMask(previousScene.get(), *scene);
	scene->stats.finalizeMilliseconds = std::chrono::duration<double, std::milli>(
		Clock::now() - finalizeStart).count();
	scene->stats.buildMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();
	PerfLastSceneBuildMs = scene->stats.buildMilliseconds;
	CurrentRadarScene = scene;
	return CurrentRadarScene;
}
