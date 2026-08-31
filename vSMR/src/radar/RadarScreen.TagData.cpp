#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "tags/VacdmTagHelpers.hpp"

map<string, string> CSMRRadar::GenerateTagData(CRadarTarget rt, CFlightPlan fp, bool isASEL, bool isAcCorrelated, bool isProMode, int TransitionAltitude, string ActiveAirport, const std::string& stableCallsign, const VacdmPilotData* capturedVacdmData, const int* capturedPreviousFlightLevel)
{
	(void)isASEL;
	(void)ActiveAirport;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	auto verboseStep = [&](const std::string& step)
	{
		if (!Logger::is_verbose_mode())
			return;

		Logger::info("GenerateTagData: " + step);
	};
	verboseStep("begin stable_callsign=" + (stableCallsign.empty() ? std::string("<empty>") : stableCallsign));
	// ----
	// Tag items available
	// callsign: Callsign with freq state and comm *
	// actype: Aircraft type *
	// sctype: Aircraft type that changes for squawk error *
	// sqerror: Squawk error if there is one, or empty *
	// deprwy: Departure runway *
	// seprwy: Departure runway that changes to speed if speed > 25kts *
	// arvrwy: Arrival runway *
	// srvrwy: Speed that changes to arrival runway if speed < 25kts *
	// gate: Gate from scratchpad *
	// sate: Gate, from speed or scratchpad that changes to speed if speed > 25kts *
	// flightlevel: Flightlevel/Pressure altitude of the ac *
	// gs: Ground speed of the ac *
	// tobt: VACDM TOBT (HHMM)
	// tsat: VACDM TSAT (HHMM)
	// ttot: VACDM TTOT (HHMM)
	// asat: VACDM ASAT (HHMM)
	// aobt: VACDM AOBT (HHMM)
	// atot: VACDM ATOT (HHMM)
	// asrt: VACDM ASRT (HHMM)
	// aort: VACDM AORT (HHMM)
	// ctot: VACDM CTOT (HHMM)
	// event_booking: VACDM event booking flag ("B")
	// tendency: Climbing or descending symbol *
	// wake: Wake turbulance cat *
	// groundstatus: Current status *
	// ssr: the current squawk of the ac
	// asid: the assigned SID
	// ssid: a short version of the SID
	// origin: origin aerodrome
	// dest: destination aerodrome
	// clearance: departure/startup clearance flag ([ ] / [x]), clickable toggle
	// holdingpoint: synchronized holding point from the flight plan remarks
	// ----

	auto safeCString = [](const char* text) -> const char*
	{
		return text != nullptr ? text : "";
	};
	auto safeString = [&](const char* text) -> std::string
	{
		return text != nullptr ? std::string(text) : std::string();
	};
	const bool radarTargetValid = rt.IsValid();
	CRadarTargetPositionData rtPos;
	if (radarTargetValid)
		rtPos = rt.GetPosition();
	const bool hasRadarTarget = radarTargetValid && rtPos.IsValid();

	const bool hasFlightPlan = fp.IsValid();
	const bool hasReceivedFlightPlanData = hasFlightPlan && fp.GetFlightPlanData().IsReceived();
	std::string rawScratchpad;
	if (hasFlightPlan)
		rawScratchpad = safeString(fp.GetControllerAssignedData().GetScratchPadString());
	// Hide markers left by older vSMR builds while otherwise preserving the
	// controller scratchpad verbatim. New HP values are stored in FP remarks.
	const std::string userScratchpad = VsmrHoldingPoint::WithoutHoldingPoint(rawScratchpad);
	const int reportedGs = hasRadarTarget ? rtPos.GetReportedGS() : 0;
	bool IsPrimary = hasRadarTarget ? !rtPos.GetTransponderC() : true;
	bool isAirborne = reportedGs > 50;
	verboseStep(
		"snapshot has_rt=" + std::string(hasRadarTarget ? "1" : "0") +
		" has_fp=" + std::string(hasFlightPlan ? "1" : "0") +
		" fp_received=" + std::string(hasReceivedFlightPlanData ? "1" : "0") +
		" reported_gs=" + std::to_string(reportedGs) +
		" corr=" + std::string(isAcCorrelated ? "1" : "0"));

	// ----- Callsign -------
	string callsign = stableCallsign;
	if (callsign.empty())
		callsign = safeString(radarTargetValid ? rt.GetCallsign() : nullptr);
	if (callsign.empty())
		callsign = safeString(hasFlightPlan ? fp.GetCallsign() : nullptr);
	if (hasReceivedFlightPlanData) {
		if (fp.GetControllerAssignedData().GetCommunicationType() == 't' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'T' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'r' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'R' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'v' ||
			fp.GetControllerAssignedData().GetCommunicationType() == 'V')
		{
			if (fp.GetControllerAssignedData().GetCommunicationType() != 'v' &&
				fp.GetControllerAssignedData().GetCommunicationType() != 'V') {
				callsign.append("/");
				callsign += fp.GetControllerAssignedData().GetCommunicationType();
			}
		}
		else if (fp.GetFlightPlanData().GetCommunicationType() == 't' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'r' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'T' ||
			fp.GetFlightPlanData().GetCommunicationType() == 'R')
		{
			callsign.append("/");
			callsign += fp.GetFlightPlanData().GetCommunicationType();
		}

		switch (fp.GetState()) {

		case FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED:
			callsign = ">>" + callsign;
			break;

		case FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED:
			callsign = callsign + ">>";
			break;

		case FLIGHT_PLAN_STATE_ASSUMED:
			callsign = "[" + callsign + "]";
			break;

		}
	}

	// ----- Squawk error -------
	string sqerror = "";
	const char* assr = hasFlightPlan ? safeCString(fp.GetControllerAssignedData().GetSquawk()) : "";
	const char* ssr = hasRadarTarget ? safeCString(rtPos.GetSquawk()) : "";
	bool has_squawk_error = false;
	if (strlen(assr) != 0 && strlen(ssr) != 0 && !VsmrRadarUiSupport::startsWith(ssr, assr)) {
		has_squawk_error = true;
		sqerror = "A";
		sqerror.append(assr);
	}

	verboseStep("callsign token prepared value=" + (callsign.empty() ? std::string("<empty>") : callsign));

	// ----- Aircraft type -------

	string actype = "NoFPL";
	if (hasReceivedFlightPlanData)
		actype = safeString(fp.GetFlightPlanData().GetAircraftFPType());
	if (actype.size() > 4 && actype != "NoFPL")
		actype = actype.substr(0, 4);

	// ----- Aircraft type that changes to squawk error -------
	string sctype = actype;
	if (has_squawk_error)
		sctype = sqerror;

	// ----- Groundspeed -------
	string speed = std::to_string(reportedGs);

	// ----- Departure runway -------
	string deprwy = hasReceivedFlightPlanData ? safeString(fp.GetFlightPlanData().GetDepartureRwy()) : "";
	if (deprwy.length() == 0)
		deprwy = "RWY";

	// ----- Departure runway that changes for overspeed -------
	string seprwy = deprwy;
	if (hasRadarTarget && reportedGs > 25)
		seprwy = std::to_string(reportedGs);

	// ----- Arrival runway -------
	string arvrwy = hasReceivedFlightPlanData ? safeString(fp.GetFlightPlanData().GetArrivalRwy()) : "";
	if (arvrwy.length() == 0)
		arvrwy = "RWY";

	// ----- Speed that changes to arrival runway -----
	string srvrwy = speed;
	if (hasRadarTarget && reportedGs < 25)
		srvrwy = arvrwy;

	// ----- Gate -------
	string gate;
	if (hasFlightPlan)
		gate = userScratchpad;

	VsmrRadarUiSupport::replaceAll(gate, "STAND=", "");
	if (gate.size() > 4)
		gate = gate.substr(0, 4);

	if (gate.size() == 0 || gate == "0" || !isAcCorrelated)
		gate = "NoGate";

	// ----- Gate that changes to speed -------
	string sate = gate;
	if (hasRadarTarget && reportedGs > 25)
		sate = speed;

	// ----- Flightlevel -------
	int fl = hasRadarTarget ? rtPos.GetFlightLevel() : 0;
	int padding = 5;
	string pfls = "";
	if (fl <= TransitionAltitude) {
		fl = hasRadarTarget ? rtPos.GetPressureAltitude() : 0;
		pfls = "A";
		padding = 4;
	}
	string flightlevel = (pfls + VsmrRadarUiSupport::padWithZeros(padding, fl)).substr(0, 3);

	// ----- Tendency -------
	string tendency = "-";
	int delta_fl = 0;
	if (hasRadarTarget && capturedPreviousFlightLevel != nullptr)
		delta_fl = rtPos.GetFlightLevel() - *capturedPreviousFlightLevel;
	if (abs(delta_fl) >= 50) {
		if (delta_fl < 0) {
			tendency = "|";
		}
		else {
			tendency = "^";
		}
	}

	// ----- Wake cat -------
	string wake = "?";
	if (hasReceivedFlightPlanData && isAcCorrelated) {
		wake = "";
		wake += fp.GetFlightPlanData().GetAircraftWtc();
	}

	// ----- SSR -------
	string tssr = hasRadarTarget ? safeCString(rtPos.GetSquawk()) : "";

	// ----- SID -------
	string dep = "SID";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		dep = safeString(fp.GetFlightPlanData().GetSidName());
	}

	// ----- Short SID -------
	string ssid = dep;
	if (hasFlightPlan && ssid.size() > 5 && isAcCorrelated)
	{
		ssid = dep.substr(0, 3);
		ssid += dep.substr(dep.size() - 2, dep.size());
	}

	// ------- Origin aerodrome -------
	string origin = "????";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		origin = safeString(fp.GetFlightPlanData().GetOrigin());
	}

	// ------- Destination aerodrome -------
	string dest = "????";
	if (hasReceivedFlightPlanData && isAcCorrelated)
	{
		dest = safeString(fp.GetFlightPlanData().GetDestination());
	}

	// ----- GSTAT -------
	string gstat = "STS";
	if (hasReceivedFlightPlanData && isAcCorrelated) {
		const char* groundState = safeCString(fp.GetGroundState());
		std::string stateCallsign = stableCallsign;
		if (stateCallsign.empty())
			stateCallsign = safeString(radarTargetValid ? rt.GetCallsign() : nullptr);
		if (stateCallsign.empty())
			stateCallsign = safeString(fp.GetCallsign());
		const GroundStateCategory observedState = classifyGroundState(groundState, reportedGs, false);
		if (VsmrGroundState::IsLineupOverrideActive(stateCallsign.c_str(), observedState))
			gstat = "LNUP";
		else if (strlen(groundState) != 0)
			gstat = groundState;
	}

	// ----- Clearance flag -------
	string clearance = "";
	if (hasFlightPlan && isAcCorrelated)
		clearance = fp.GetClearenceFlag() ? "[x]" : "[ ]";

	// ----- UK Controller Plugin / Assigned Stand -------
	string uk_stand;
	if (hasFlightPlan)
		uk_stand = safeString(fp.GetControllerAssignedData().GetFlightStripAnnotation(3));
	if (uk_stand.length() == 0)
		uk_stand = "";

	// ----- Ramp Agent Remark -------
	string remark;
	if (hasFlightPlan)
		remark = safeString(fp.GetControllerAssignedData().GetFlightStripAnnotation(4));
	if (remark.length() == 0)
		remark = "";

	// ----- Scratchpad -------
	string scratchpad;
	if (hasFlightPlan)
		scratchpad = userScratchpad;
	if (scratchpad.length() == 0)
		scratchpad = "...";

	// ----- Holding point (synchronized through flight plan remarks) -------
	const std::string holdingPointCallsign = !stableCallsign.empty()
		? stableCallsign
		: (hasFlightPlan ? safeString(fp.GetCallsign()) : "");
	const std::string flightPlanRemarks = hasFlightPlan
		? safeString(fp.GetFlightPlanData().GetRemarks())
		: std::string();
	string holdingpoint = VsmrHoldingPoint::Resolve(holdingPointCallsign, flightPlanRemarks);

	// ----- VACDM fields -------
	string tobt = "";
	string tsat = "";
	string ttot = "";
	string asat = "";
	string aobt = "";
	string atot = "";
	string asrt = "";
	string aort = "";
	string ctot = "";
	string eventBooking = "";
	if (capturedVacdmData != nullptr)
	{
		const VacdmPilotData& vacdmPilot = *capturedVacdmData;
		if (vacdmPilot.hasTobt)
			tobt = FormatVacdmTimeToken(vacdmPilot.tobtUtc);
		if (vacdmPilot.hasTsat)
			tsat = FormatVacdmTimeToken(vacdmPilot.tsatUtc);
		if (vacdmPilot.hasTtot)
			ttot = FormatVacdmTimeToken(vacdmPilot.ttotUtc);
		if (vacdmPilot.hasAsat)
			asat = FormatVacdmTimeToken(vacdmPilot.asatUtc);
		if (vacdmPilot.hasAobt)
			aobt = FormatVacdmTimeToken(vacdmPilot.aobtUtc);
		if (vacdmPilot.hasAtot)
			atot = FormatVacdmTimeToken(vacdmPilot.atotUtc);
		if (vacdmPilot.hasAsrt)
			asrt = FormatVacdmTimeToken(vacdmPilot.asrtUtc);
		if (vacdmPilot.hasAort)
			aort = FormatVacdmTimeToken(vacdmPilot.aortUtc);
		if (vacdmPilot.hasCtot)
			ctot = FormatVacdmTimeToken(vacdmPilot.ctotUtc);
		eventBooking = vacdmPilot.hasBooking ? "B" : "";
	}

	// VACDM fallback: when backend has no entry, use FPL EOBT as TOBT baseline (matches VACDM plugin bootstrap behavior).
	if (tobt.empty() && hasReceivedFlightPlanData && isAcCorrelated)
		tobt = NormalizeHhmmToken(safeCString(fp.GetFlightPlanData().GetEstimatedDepartureTime()));


	// ----- Generating the replacing map -----
	map<string, string> TagReplacingMap;

	// System ID for uncorrelated
	TagReplacingMap["systemid"] = "T:";
	string tpss = callsign;
	if (tpss.empty())
		tpss = "000000";
	if (tpss.size() > 1)
		TagReplacingMap["systemid"].append(tpss.substr(1, min<size_t>(6, tpss.size() - 1)));
	else if (!tpss.empty())
		TagReplacingMap["systemid"].append(tpss.substr(0, min<size_t>(6, tpss.size())));
	else
		TagReplacingMap["systemid"].append("000000");

	// Display modes with the squawk rule enabled use SSR-centric fallback data.
	if (isProMode)
	{

		if (isAirborne && !isAcCorrelated)
		{
			callsign = tssr;
		}

		if (!isAcCorrelated)
		{
			actype = "NoFPL";
		}

		// Is a primary target

		if (isAirborne && !isAcCorrelated && IsPrimary)
		{
			flightlevel = "NoALT";
			tendency = "?";
			speed = std::to_string(reportedGs);
		}

		if (isAirborne && !isAcCorrelated && IsPrimary)
		{
			callsign = TagReplacingMap["systemid"];
		}
	}

	TagReplacingMap["callsign"] = callsign;
	TagReplacingMap["actype"] = actype;
	TagReplacingMap["sctype"] = sctype;
	TagReplacingMap["sqerror"] = sqerror;
	TagReplacingMap["deprwy"] = deprwy;
	TagReplacingMap["seprwy"] = seprwy;
	TagReplacingMap["arvrwy"] = arvrwy;
	TagReplacingMap["srvrwy"] = srvrwy;
	TagReplacingMap["gate"] = gate;
	TagReplacingMap["sate"] = sate;
	TagReplacingMap["flightlevel"] = flightlevel;
	TagReplacingMap["gs"] = speed;
	TagReplacingMap["tobt"] = tobt;
	TagReplacingMap["tsat"] = tsat;
	TagReplacingMap["ttot"] = ttot;
	TagReplacingMap["asat"] = asat;
	TagReplacingMap["aobt"] = aobt;
	TagReplacingMap["atot"] = atot;
	TagReplacingMap["asrt"] = asrt;
	TagReplacingMap["aort"] = aort;
	TagReplacingMap["ctot"] = ctot;
	TagReplacingMap["event_booking"] = eventBooking;
	TagReplacingMap["tendency"] = tendency;
	TagReplacingMap["wake"] = wake;
	TagReplacingMap["ssr"] = tssr;
	TagReplacingMap["asid"] = dep;
	TagReplacingMap["ssid"] = ssid;
	TagReplacingMap["origin"] = origin;
	TagReplacingMap["dest"] = dest;
	TagReplacingMap["groundstatus"] = gstat;
	TagReplacingMap["clearance"] = clearance;
	TagReplacingMap["uk_stand"] = uk_stand;
	TagReplacingMap["remark"] = remark;
	TagReplacingMap["scratchpad"] = scratchpad;
	TagReplacingMap["holdingpoint"] = holdingpoint;
	verboseStep(
		"done callsign=" + TagReplacingMap["callsign"] +
		" actype=" + TagReplacingMap["actype"] +
		" gs=" + TagReplacingMap["gs"] +
		" sid=" + TagReplacingMap["asid"] +
		" corr=" + std::string(isAcCorrelated ? "1" : "0"));

	return TagReplacingMap;
}
