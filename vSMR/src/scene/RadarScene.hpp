#pragma once

#include "aircraft/GroundState.hpp"
#include "aviso/FrequencyOwnership.hpp"
#include "tags/TagDataTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace VsmrScene
{
	struct GeoPoint
	{
		double latitude = 0.0;
		double longitude = 0.0;
		bool valid = false;
	};

	struct Color
	{
		std::uint8_t alpha = 255;
		std::uint8_t red = 255;
		std::uint8_t green = 255;
		std::uint8_t blue = 255;
	};

	enum class TargetRole
	{
		Departure,
		Arrival,
		AirborneDeparture,
		AirborneArrival,
		Uncorrelated
	};

	enum class IconStyle
	{
		Nova,
		Realistic,
		Triangle,
		Diamond
	};

	struct TagElement
	{
		std::string token;
		std::string text;
		int action = 0;
		bool bold = false;
		bool hasCustomColor = false;
		Color customColor;
		Color effectiveColor;
		bool clearanceToken = false;
	};

	struct TagLine
	{
		std::vector<TagElement> elements;
	};

	struct TagVariant
	{
		std::vector<TagLine> lines;
	};

	struct TagPalette
	{
		Color background{ 255, 53, 126, 187 };
		Color backgroundOnRunway{ 255, 53, 126, 187 };
		Color text{ 255, 255, 255, 255 };
		bool textRuleApplied = false;
	};

	struct TagContent
	{
		std::map<std::string, std::string> tokens;
		TagVariant normal;
		TagVariant detailed;
		TagPalette normalPalette;
		TagPalette detailedPalette;
		std::string definitionType;
		std::string status;
		bool clearanceReceived = false;
	};

	struct RimcasState
	{
		bool onRunway = false;
		int alertStage = 0;
		int movementAlert = 0;
		int severity = 0;
	};

	struct TargetStyle
	{
		IconStyle icon = IconStyle::Nova;
		Color color;
		Color primaryReturnColor{ 255, 255, 242, 73 };
		std::string assetKey;
		double lengthMeters = 0.0;
		double wingspanMeters = 0.0;
		bool showPrimaryReturn = false;
	};

	struct TargetPresentation
	{
		IconStyle icon = IconStyle::Nova;
		bool showPrimaryReturn = false;
		bool smallIconBoostEnabled = false;
		bool fixedPixelSize = false;
		double smallIconBoostFactor = 1.0;
		double resolutionScale = 1.0;
		double fixedTriangleScale = 1.0;
	};

	struct Target
	{
		std::string callsign;
		std::string normalizedCallsign;
		std::string systemId;
		std::string bottomLine;
		std::string origin;
		std::string destination;
		std::string planType;
		std::string aircraftType;
		std::string assignedSquawk;
		std::string reportedSquawk;
		std::string groundStateText;
		std::string towerModeGroundStateText;

		GeoPoint position;
		GeoPoint previousPosition;
		GeoPoint headingProbe;
		std::vector<GeoPoint> primaryReturnPolygon;

		int reportedGroundSpeed = 0;
		int groundSpeed = 0;
		int pressureAltitude = 0;
		int flightLevel = 0;
		int previousFlightLevel = 0;
		int reportedHeadingDegrees = 0;
		double headingTrueDegrees = 0.0;
		double trackHeadingDegrees = 0.0;
		char wakeCategory = '\0';

		bool transponderModeC = false;
		bool hasFlightPlan = false;
		bool hasCorrelatedFlightPlan = false;
		bool flightPlanDataReceived = false;
		bool correlated = false;
		bool selected = false;
		bool departure = false;
		bool arrival = false;
		bool towerModeArrival = false;
		bool airborne = false;
		bool passesRadarFilter = false;
		bool passesDisplayMode = false;
		bool iconVisible = false;
		bool tagVisible = false;

		GroundStateCategory groundState = GroundStateCategory::Unknown;
		TargetRole role = TargetRole::Uncorrelated;
		TargetStyle style;
		TagContent tag;
		RimcasState rimcas;
		bool hasVacdmData = false;
		VacdmPilotData vacdmData;
	};

	struct ControllerState
	{
		std::string callsign;
		std::string positionId;
		double primaryFrequency = 0.0;
		bool mine = false;
	};

	struct AirportState
	{
		std::string icao;
		GeoPoint referencePosition;
		bool lowVisibilityProcedures = false;
		int transitionAltitude = 0;
		std::map<std::string, int> runwayStatuses;
	};

	struct BuildStats
	{
		std::size_t targetCount = 0;
		std::size_t tagElementCount = 0;
		std::size_t controllerCount = 0;
		std::size_t sdkTargetEnumerations = 0;
		std::size_t sdkControllerEnumerations = 0;
		std::size_t sdkFlightPlanLookups = 0;
		std::size_t sdkCorrelatedFlightPlanLookups = 0;
		std::size_t sdkPreviousPositionLookups = 0;
		std::size_t vacdmLookups = 0;
		std::size_t radarFilteredTargetCount = 0;
		std::size_t iconTargetCount = 0;
		std::size_t tagTargetCount = 0;
		std::size_t lowerBoundOwnedBytes = 0;
		double sdkLookupMilliseconds = 0.0;
		double buildMilliseconds = 0.0;
	};

	struct RadarScene
	{
		std::uint64_t frameId = 0;
		unsigned long captureTick = 0;
		AirportState airport;
		TargetPresentation targetPresentation;
		std::vector<ControllerState> controllers;
		std::vector<Target> targets;
		std::unordered_map<std::string, std::size_t> targetIndex;
		std::shared_ptr<const VsmrAviso::FrequencyOwnershipSnapshot> frequencyOwnership;
		std::uint64_t avisoGeneration = 0;
		BuildStats stats;

		const Target* FindTarget(const std::string& callsign) const noexcept;
	};
}
