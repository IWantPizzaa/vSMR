#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagTokenValues.hpp"
#include "radar/RadarScreen.hpp"
#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "tags/CdmTagHelpers.hpp"

#include "tags/TagDataFormatting.hpp"

#include <utility>

namespace
{
	std::string CopyTagText(const char* value) { return value != nullptr ? value : ""; }

	VsmrTags::TagDataInput CaptureTagData(const CRadarTarget& rt, const CFlightPlan& fp,
		bool correlated, bool proMode, int transitionAltitude, const std::string& stableCallsign,
		const int* previousFlightLevel)
	{
		VsmrTags::TagDataInput input;
		input.correlated = correlated;
		input.proMode = proMode;
		input.transitionAltitude = transitionAltitude;
		input.callsign = stableCallsign;
		if (rt.IsValid())
		{
			if (input.callsign.empty()) input.callsign = CopyTagText(rt.GetCallsign());
			const auto position = rt.GetPosition();
			input.hasRadarTarget = position.IsValid();
			if (input.hasRadarTarget)
			{
				input.groundSpeed = position.GetReportedGS();
				input.primary = !position.GetTransponderC();
				input.flightLevel = position.GetFlightLevel();
				input.pressureAltitude = position.GetPressureAltitude();
				input.squawk = CopyTagText(position.GetSquawk());
				if (previousFlightLevel) input.altitudeDelta = input.flightLevel - *previousFlightLevel;
			}
		}
		input.hasFlightPlan = fp.IsValid();
		if (input.hasFlightPlan)
		{
			const auto data = fp.GetFlightPlanData();
			const auto assigned = fp.GetControllerAssignedData();
			if (input.callsign.empty()) input.callsign = CopyTagText(fp.GetCallsign());
			input.receivedFlightPlan = data.IsReceived();
			input.scratchpad = VsmrHoldingPoint::WithoutHoldingPoint(CopyTagText(assigned.GetScratchPadString()));
			input.assignedSquawk = CopyTagText(assigned.GetSquawk());
			input.clearance = fp.GetClearenceFlag();
			if (input.receivedFlightPlan)
			{
				input.assignedCommunication = assigned.GetCommunicationType();
				input.filedCommunication = data.GetCommunicationType();
				input.flightPlanState = fp.GetState();
				input.aircraftType = CopyTagText(data.GetAircraftFPType());
				input.departureRunway = CopyTagText(data.GetDepartureRwy());
				input.arrivalRunway = CopyTagText(data.GetArrivalRwy());
				input.wake = data.GetAircraftWtc();
				input.sid = CopyTagText(data.GetSidName());
				input.origin = CopyTagText(data.GetOrigin());
				input.destination = CopyTagText(data.GetDestination());
				input.groundState = CopyTagText(fp.GetGroundState());
				// A shared ground state wins over the EuroScope status here as well.
				if (correlated) input.lineup = classifyGroundStateWithSharedState(
					input.groundState.c_str(), input.groundSpeed, false, assigned.GetAssignedSpeed()) == GroundStateCategory::Lnup;
			}
		}
		const std::string holdingCallsign = !stableCallsign.empty() ? stableCallsign : (input.hasFlightPlan ? CopyTagText(fp.GetCallsign()) : "");
		input.holdingPoint = VsmrHoldingPoint::Resolve(holdingCallsign,
			input.hasFlightPlan ? CopyTagText(fp.GetFlightPlanData().GetRemarks()) : "");
		return input;
	}
}

void CSMRRadar::GenerateTagData(VsmrTags::TokenValues& TagReplacingMap, const CRadarTarget& rt, const CFlightPlan& fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, const std::string& ActiveAirport, const std::string& stableCallsign, const CdmPilotData* capturedCdmData, const int* capturedPreviousFlightLevel)
{
	(void)isASEL;
	(void)ActiveAirport;
	VsmrTags::TagDataInput input = CaptureTagData(rt, fp, isAcCorrelated, isProMode, TransitionAltitude,
		stableCallsign, capturedPreviousFlightLevel);
	const std::string bridgeCallsign = !stableCallsign.empty() ? stableCallsign : (fp.IsValid() ? CopyTagText(fp.GetCallsign()) : "");
	// The stand and its remark come only from Ramp Agent through the plug-in bridge.
	VsmrRampAgent::AircraftData rampAgentData;
	if (VsmrRampAgent::TryGetAircraftData(bridgeCallsign, rampAgentData))
	{
		input.stand = std::move(rampAgentData.stand);
		input.remark = std::move(rampAgentData.remark);
	}
	VsmrTags::FormatTagData(input, TagReplacingMap);
	VsmrVsid::AircraftData vsidData;
	const bool hasVsidData = VsmrVsid::TryGetAircraftData(bridgeCallsign, vsidData);
	VsmrVsid::AddTagTokens(TagReplacingMap, hasVsidData ? &vsidData : nullptr);
	VsmrCdm::AddTagTokens(TagReplacingMap, capturedCdmData != nullptr ? &capturedCdmData->bridgeData : nullptr);
	if (Logger::is_verbose_mode()) Logger::info("GenerateTagData: callsign=" + TagReplacingMap.at("callsign") +
		" actype=" + TagReplacingMap.at("actype") + " gs=" + TagReplacingMap.at("gs"));
}
