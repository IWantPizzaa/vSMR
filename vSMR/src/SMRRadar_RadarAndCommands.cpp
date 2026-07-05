#include "stdafx.h"
#include "SMRRadar.hpp"
#include <cctype>

void CSMRRadar::RefreshAirportActivity(void) {
	Logger::info(string(__FUNCSIG__));
	//
	// Getting the depatures and arrivals airports
	//

	Active_Arrivals.clear();
	CSectorElement airport;
	for (airport = GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
		airport.IsValid();
		airport = GetPlugIn()->SectorFileElementSelectNext(airport, SECTOR_ELEMENT_AIRPORT))
	{
		if (airport.IsElementActive(false)) {
			const char* airportName = airport.GetName();
			if (airportName == nullptr || airportName[0] == '\0')
				continue;
			string s = airportName;
			s = s.substr(0, 4);
			transform(s.begin(), s.end(), s.begin(), ::toupper);
			Active_Arrivals.push_back(s);
		}
	}
}

void CSMRRadar::OnRadarTargetPositionUpdate(CRadarTarget RadarTarget)
{
	Logger::info(string(__FUNCSIG__));
	if (!RadarTarget.IsValid() || !RadarTarget.GetPosition().IsValid())
		return;
	const char* callsignRaw = RadarTarget.GetCallsign();
	if (callsignRaw == nullptr || callsignRaw[0] == '\0')
	{
		static bool loggedMissingCallsign = false;
		if (!loggedMissingCallsign)
		{
			Logger::info("OnRadarTargetPositionUpdate: skipped target with missing callsign");
			loggedMissingCallsign = true;
		}
		return;
	}
	const std::string callsign = callsignRaw;

	CRadarTargetPositionData RtPos = RadarTarget.GetPosition();

	Patatoides[callsign].History_three_points = Patatoides[callsign].History_two_points;
	Patatoides[callsign].History_two_points = Patatoides[callsign].History_one_points;
	Patatoides[callsign].History_one_points = Patatoides[callsign].points;

	Patatoides[callsign].points.clear();

	CFlightPlan fp = GetPlugIn()->FlightPlanSelect(callsign.c_str());

	// All units in M
	float width = 34.0f;
	float cabin_width = 4.0f;
	float lenght = 38.0f;

	if (fp.IsValid()) {
		char wtc = fp.GetFlightPlanData().GetAircraftWtc();

		if (wtc == 'L') {
			width = 13.0f;
			cabin_width = 2.0f;
			lenght = 12.0f;
		}

		if (wtc == 'H') {
			width = 61.0f;
			cabin_width = 7.0f;
			lenght = 64.0f;
		}

		if (wtc == 'J') {
			width = 80.0f;
			cabin_width = 7.0f;
			lenght = 73.0f;
		}
	}


	width = width + float((rand() % 5) - 2);
	cabin_width = cabin_width + float((rand() % 3) - 1);
	lenght = lenght + float((rand() % 5) - 2);


	float trackHead = float(RadarTarget.GetPosition().GetReportedHeadingTrueNorth());
	float inverseTrackHead = float(fmod(trackHead + 180.0f, 360));
	float leftTrackHead = float(fmod(trackHead - 90.0f, 360));
	float rightTrackHead = float(fmod(trackHead + 90.0f, 360));

	float HalfLenght = lenght / 2.0f;
	float HalfCabWidth = cabin_width / 2.0f;
	float HalfSpanWidth = width / 2.0f;

	// Base shape is like a deformed cross


	CPosition topMiddle = Haversine(RtPos.GetPosition(), trackHead, HalfLenght);
	CPosition topLeft = Haversine(topMiddle, leftTrackHead, HalfCabWidth);
	CPosition topRight = Haversine(topMiddle, rightTrackHead, HalfCabWidth);

	CPosition bottomMiddle = Haversine(RtPos.GetPosition(), inverseTrackHead, HalfLenght);
	CPosition bottomLeft = Haversine(bottomMiddle, leftTrackHead, HalfCabWidth);
	CPosition bottomRight = Haversine(bottomMiddle, rightTrackHead, HalfCabWidth);

	CPosition middleTopLeft = Haversine(topLeft, float(fmod(inverseTrackHead + 25.0f, 360)), 0.8f*HalfLenght);
	CPosition middleTopRight = Haversine(topRight, float(fmod(inverseTrackHead - 25.0f, 360)), 0.8f*HalfLenght);
	CPosition middleBottomLeft = Haversine(bottomLeft, float(fmod(trackHead - 15.0f, 360)), 0.8f*HalfLenght);
	CPosition middleBottomRight = Haversine(bottomRight, float(fmod(trackHead + 15.0f, 360)), 0.8f*HalfLenght);

	CPosition rightTop = Haversine(middleBottomRight, rightTrackHead, 0.7f*HalfSpanWidth);
	CPosition rightBottom = Haversine(rightTop, inverseTrackHead, cabin_width);

	CPosition leftTop = Haversine(middleBottomLeft, leftTrackHead, 0.7f*HalfSpanWidth);
	CPosition leftBottom = Haversine(leftTop, inverseTrackHead, cabin_width);

	CPosition basePoints[12];
	basePoints[0] = topLeft;
	basePoints[1] = middleTopLeft;
	basePoints[2] = leftTop;
	basePoints[3] = leftBottom;
	basePoints[4] = middleBottomLeft;
	basePoints[5] = bottomLeft;
	basePoints[6] = bottomRight;
	basePoints[7] = middleBottomRight;
	basePoints[8] = rightBottom;
	basePoints[9] = rightTop;
	basePoints[10] = middleTopRight;
	basePoints[11] = topRight;

	// 12 points total, so 11 from 0
	// ------

	// Random points between points of base shape

	for (int i = 0; i < 12; i++){

		CPosition newPoint, lastPoint, endPoint, startPoint;

		startPoint = basePoints[i];
		if (i == 11) endPoint = basePoints[0];
		else endPoint = basePoints[i + 1];

		double dist, rndHeading;
		dist = startPoint.DistanceTo(endPoint);

		Patatoides[callsign].points[i * 7] = { startPoint.m_Latitude, startPoint.m_Longitude };
		lastPoint = startPoint;

		for (int k = 1; k < 7; k++){

			rndHeading = float(fmod(lastPoint.DirectionTo(endPoint) + (-25.0 + (rand() % 50 + 1)), 360));
			newPoint = Haversine(lastPoint, rndHeading, dist * 200);
			Patatoides[callsign].points[(i * 7) + k] = { newPoint.m_Latitude, newPoint.m_Longitude };
			lastPoint = newPoint;
		}
	}
}

string CSMRRadar::GetBottomLine(const char * Callsign) {
	Logger::info(string(__FUNCSIG__));
	auto safeCString = [](const char* text) -> const char*
	{
		return text != nullptr ? text : "";
	};

	CFlightPlan fp = GetPlugIn()->FlightPlanSelect(Callsign);
	string to_render = "";
	if (fp.IsValid()) {
		to_render += safeCString(fp.GetCallsign());

		string callsign_code = safeCString(fp.GetCallsign());
		callsign_code = callsign_code.substr(0, 3);
		to_render += " (" + Callsigns->getCallsign(callsign_code) + ")";

		to_render += " (";
		to_render += safeCString(fp.GetPilotName());
		to_render += "): ";
		to_render += safeCString(fp.GetFlightPlanData().GetAircraftFPType());
		to_render += " ";

		if (fp.GetFlightPlanData().IsReceived()) {
			const char * assr = safeCString(fp.GetControllerAssignedData().GetSquawk());
			CRadarTarget rt = GetPlugIn()->RadarTargetSelect(fp.GetCallsign());
			const char * ssr = "----";
			if (rt.IsValid() && rt.GetPosition().IsValid())
				ssr = safeCString(rt.GetPosition().GetSquawk());
			if (strlen(assr) != 0 && !startsWith(ssr, assr)) {
				to_render += assr;
				to_render += ":";
				to_render += ssr;
			}
			else {
				to_render += "I:";
				to_render += ssr;
			}

			to_render += " ";
			to_render += safeCString(fp.GetFlightPlanData().GetOrigin());
			to_render += "==>";
			to_render += safeCString(fp.GetFlightPlanData().GetDestination());
			to_render += " (";
			to_render += safeCString(fp.GetFlightPlanData().GetAlternate());
			to_render += ")";

			to_render += " at ";
			int rfl = fp.GetControllerAssignedData().GetFinalAltitude();
			string rfl_s;
			if (rfl == 0)
				rfl = fp.GetFlightPlanData().GetFinalAltitude();
			if (rfl > GetPlugIn()->GetTransitionAltitude())
				rfl_s = "FL" + std::to_string(rfl / 100);
			else
				rfl_s = std::to_string(rfl) + "ft";

			to_render += rfl_s;
			to_render += " Route: ";
			to_render += safeCString(fp.GetFlightPlanData().GetRoute());
		}
	}

	return to_render;
}

bool CSMRRadar::OnCompileCommand(const char * sCommandLine)
{
	Logger::info(string(__FUNCSIG__));
	if (sCommandLine == nullptr)
		return false;

	auto trimAsciiWhitespace = [](const std::string& text) -> std::string
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;

		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;
		return text.substr(start, end - start);
	};
	auto toLowerAscii = [](std::string value) -> std::string
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	};
	auto showAvisoPresetMessage = [&](const std::string& message)
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "AVISO Presets", message.c_str(), true, true, false, false, false);
	};

	const std::string command = trimAsciiWhitespace(sCommandLine);
	const std::string commandLower = toLowerAscii(command);
	const std::string avisoPrefix = ".smr aviso";
	if (commandLower == avisoPrefix || commandLower == avisoPrefix + " help")
	{
		showAvisoPresetMessage("Commands: .smr aviso list | save <name> | load <name> | update | reset | rename <name> | duplicate <name> | delete [name] | default [name|none] | link [on|off|toggle]");
		return true;
	}

	if (commandLower.rfind(avisoPrefix + " ", 0) == 0)
	{
		const std::string rest = trimAsciiWhitespace(command.substr(avisoPrefix.size()));
		const size_t verbEnd = rest.find_first_of(" \t");
		const std::string verb = toLowerAscii(verbEnd == std::string::npos ? rest : rest.substr(0, verbEnd));
		const std::string argument = verbEnd == std::string::npos ? "" : trimAsciiWhitespace(rest.substr(verbEnd + 1));

		if (verb == "list")
		{
			const std::vector<AvisoPreset> presets = GetAvisoPresets();
			if (presets.empty())
			{
				showAvisoPresetMessage("No AVISO presets saved.");
				return true;
			}

			std::string message = "AVISO presets:";
			const std::string activePreset = GetActiveAvisoPresetName();
			const std::string defaultPreset = GetDefaultAvisoPresetName();
			for (const AvisoPreset& preset : presets)
			{
				message += "\n";
				message += preset.name;
				if (!activePreset.empty() && preset.name == activePreset)
					message += " [active]";
				if (!defaultPreset.empty() && preset.name == defaultPreset)
					message += " [default]";
			}
			showAvisoPresetMessage(message);
			return true;
		}

		if (verb == "save")
		{
			std::string savedName;
			if (!argument.empty() && SaveAvisoPreset(argument, false, &savedName))
				showAvisoPresetMessage("Saved AVISO preset: " + savedName);
			else
				showAvisoPresetMessage("Usage: .smr aviso save <name>");
			return true;
		}

		if (verb == "load")
		{
			if (!argument.empty() && LoadAvisoPreset(argument))
				showAvisoPresetMessage("Loaded AVISO preset: " + GetActiveAvisoPresetName());
			else
				showAvisoPresetMessage("AVISO preset not found.");
			return true;
		}

		if (verb == "update")
		{
			const std::string activePreset = GetActiveAvisoPresetName();
			if (UpdateActiveAvisoPreset())
				showAvisoPresetMessage("Updated AVISO preset: " + activePreset);
			else
				showAvisoPresetMessage("No active AVISO preset to update.");
			return true;
		}

		if (verb == "reset")
		{
			if (ResetActiveAvisoPreset())
				showAvisoPresetMessage("Reset AVISO view to preset: " + GetActiveAvisoPresetName());
			else
				showAvisoPresetMessage("No active or default AVISO preset to reset.");
			return true;
		}

		if (verb == "rename")
		{
			const std::string activePreset = GetActiveAvisoPresetName();
			if (!activePreset.empty() && !argument.empty() && RenameAvisoPreset(activePreset, argument))
				showAvisoPresetMessage("Renamed AVISO preset: " + GetActiveAvisoPresetName());
			else
				showAvisoPresetMessage("Usage: .smr aviso rename <new name>");
			return true;
		}

		if (verb == "duplicate")
		{
			const std::string activePreset = GetActiveAvisoPresetName();
			std::string savedName;
			if (!activePreset.empty() && DuplicateAvisoPreset(activePreset, argument, &savedName))
				showAvisoPresetMessage("Duplicated AVISO preset: " + savedName);
			else
				showAvisoPresetMessage("No active AVISO preset to duplicate.");
			return true;
		}

		if (verb == "delete")
		{
			const std::string targetName = argument.empty() ? GetActiveAvisoPresetName() : argument;
			if (!targetName.empty() && DeleteAvisoPreset(targetName))
				showAvisoPresetMessage("Deleted AVISO preset: " + targetName);
			else
				showAvisoPresetMessage("AVISO preset not found.");
			return true;
		}

		if (verb == "default")
		{
			const std::string defaultArg = toLowerAscii(argument);
			if (defaultArg == "none" || defaultArg == "clear" || defaultArg == "off")
			{
				if (ClearDefaultAvisoPreset())
					showAvisoPresetMessage("Cleared default AVISO preset.");
				else
					showAvisoPresetMessage("Failed to clear default AVISO preset.");
				return true;
			}

			const std::string targetName = argument.empty() ? GetActiveAvisoPresetName() : argument;
			if (!targetName.empty() && SetDefaultAvisoPreset(targetName))
				showAvisoPresetMessage("Default AVISO preset: " + targetName);
			else
				showAvisoPresetMessage("AVISO preset not found.");
			return true;
		}

		if (verb == "link")
		{
			const std::string linkArg = toLowerAscii(argument);
			bool linked = !IsAvisoPresetLinkedMovementEnabled();
			if (linkArg == "on" || linkArg == "true" || linkArg == "1")
				linked = true;
			else if (linkArg == "off" || linkArg == "false" || linkArg == "0")
				linked = false;

			if (SetActiveAvisoPresetLinkedMovement(linked))
				showAvisoPresetMessage(std::string("AVISO linked movement ") + (linked ? "enabled." : "disabled."));
			else
				showAvisoPresetMessage("Failed to update AVISO linked movement.");
			return true;
		}

		showAvisoPresetMessage("Unknown AVISO preset command. Use .smr aviso help.");
		return true;
	}

	if (strcmp(sCommandLine, ".smr reload") == 0) {
		ReloadConfig();
		return true;
	}
	if (strcmp(sCommandLine, ".smr draw") == 0) {
		// Draw runways areas on radar screen
		drawRunways = !drawRunways;
		RequestRefresh();
		return true;
	}
	if (strcmp(sCommandLine, ".smr status") == 0) {
		// Print runway status
		string msg;
		for (const auto& [runway, status] : RimcasInstance->RunwayStatuses) {
			string rwyStatus;
			if (status == CRimcas::RunwayStatus::ARR) rwyStatus = "ARR";
			else if (status == CRimcas::RunwayStatus::DEP) rwyStatus = "DEP";
			else if (status == CRimcas::RunwayStatus::BOTH) rwyStatus = "BOTH";
			else if (status == CRimcas::RunwayStatus::CLSD) rwyStatus = "CLSD";
			msg += " Runway " + runway + ": " + rwyStatus + "\n";
		}
		GetPlugIn()->DisplayUserMessage("vSMR", "", msg.c_str(), true, true, false, false, false);
		return true;
	}


	return false;
}

void CSMRRadar::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	Logger::info(string(__FUNCSIG__));
	if (!FlightPlan.IsValid() || FlightPlan.GetCallsign() == nullptr || FlightPlan.GetCallsign()[0] == '\0')
		return;

	const string callsign = FlightPlan.GetCallsign();
	Patatoides.erase(callsign);
	TagsOffsets.erase(callsign);
	TagAngles.erase(callsign);
	TagLeaderLineLength.erase(callsign);
	tagAreas.erase(callsign);
	tagCollisionAreas.erase(callsign);
	TagDragOffsetFromCenter.erase(callsign);

	for (auto itr = DistanceTools.begin(); itr != DistanceTools.end(); )
	{
		if (itr->first == callsign || itr->second == callsign)
			itr = DistanceTools.erase(itr);
		else
			++itr;
	}
}
