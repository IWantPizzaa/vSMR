#include "platform/windows/PrecompiledHeader.hpp"
#include "tags/TagTokenValues.hpp"
#include "radar/RadarScreen.hpp"
#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "tags/CdmTagHelpers.hpp"

#include "tags/TagDataFormatting.hpp"

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
			input.stand = CopyTagText(assigned.GetFlightStripAnnotation(3));
			input.remark = CopyTagText(assigned.GetFlightStripAnnotation(4));
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
				if (correlated) input.lineup = VsmrGroundState::IsLineupOverrideActive(input.callsign.c_str(),
					classifyGroundState(input.groundState, input.groundSpeed, false));
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
	const auto input = CaptureTagData(rt, fp, isAcCorrelated, isProMode, TransitionAltitude,
		stableCallsign, capturedPreviousFlightLevel);
	VsmrTags::FormatTagData(input, TagReplacingMap);
	VsmrVsid::AircraftData vsidData;
	const std::string vsidCallsign = !stableCallsign.empty() ? stableCallsign : (fp.IsValid() ? CopyTagText(fp.GetCallsign()) : "");
	const bool hasVsidData = VsmrVsid::TryGetAircraftData(vsidCallsign, vsidData);
	VsmrVsid::AddTagTokens(TagReplacingMap, hasVsidData ? &vsidData : nullptr);
	VsmrCdm::AddTagTokens(TagReplacingMap, capturedCdmData != nullptr ? &capturedCdmData->bridgeData : nullptr);
	if (Logger::is_verbose_mode()) Logger::info("GenerateTagData: callsign=" + TagReplacingMap.at("callsign") +
		" actype=" + TagReplacingMap.at("actype") + " gs=" + TagReplacingMap.at("gs"));
}
