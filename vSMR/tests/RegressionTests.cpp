#include <Windows.h>
#include <objidl.h>

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "bootstrap/loader/RuntimeReleaseState.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/RuntimeConfig.Internal.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "control_center/WebMessageValidation.hpp"
#include "integrations/CdmBridgeData.hpp"
#include "integrations/VsidBridgeData.hpp"
#include "radar/RecentAirports.hpp"
#include "radar/RadarGeometry.hpp"
#include "safety/RimcasLogic.hpp"
#include "scene/TargetRoleLogic.hpp"
#include "shared/JsonInputLimits.hpp"
#include "shared/RapidJsonUtils.hpp"
#include "AvisoRasterPipelineTests.hpp"
#include "ConfigurationRegressionTests.hpp"
#include "SharedRenderingTests.hpp"
#include "AuditRegressionTests.hpp"
#include "PluginBridgeTests.hpp"
#include "TagColorRuleTests.hpp"
#include "UpdaterUrlPolicyTests.hpp"
#include "tags/TagDefinitionUtils.hpp"
#include "weather/WeatherStore.hpp"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	int FailureCount = 0;

	void Expect(bool condition, const std::string& name)
	{
		if (condition)
			return;

		++FailureCount;
		std::cerr << "FAILED: " << name << '\n';
	}

	std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

	void TestRapidJsonUtilities()
	{
		rapidjson::Document source;
		source.Parse<0>("{\"name\":\"source\",\"enabled\":true,\"items\":[1,\"two\"]}");
		const char embeddedNull[] = { 'a', '\0', 'b' };
		rapidjson::Value embeddedValue;
		embeddedValue.SetString(
			embeddedNull,
			static_cast<rapidjson::SizeType>(sizeof(embeddedNull)),
			source.GetAllocator());
		rapidjson::Value embeddedKey;
		embeddedKey.SetString("embedded", source.GetAllocator());
		source.AddMember(embeddedKey, embeddedValue, source.GetAllocator());
		rapidjson::Document destination;
		destination.SetObject();
		rapidjson::Value clone;
		VsmrRapidJson::CloneJsonValue(source, clone, destination.GetAllocator());
		Expect(
			clone.IsObject() && clone["name"].IsString() &&
			std::string(clone["name"].GetString()) == "source" &&
			clone["items"].IsArray() && clone["items"].Size() == 2U &&
			clone["embedded"].IsString() &&
			clone["embedded"].GetStringLength() == sizeof(embeddedNull),
			"shared RapidJSON utility deep-copies objects and arrays");

		VsmrRapidJson::SetStringMember(
			source,
			"name",
			"changed",
			source.GetAllocator());
		Expect(
			std::string(clone["name"].GetString()) == "source",
			"shared RapidJSON clone owns copied strings");

		char borrowedText[] = "borrowed";
		rapidjson::Document borrowedSource;
		borrowedSource.SetObject();
		rapidjson::Value borrowedKey;
		borrowedKey.SetString("value", borrowedSource.GetAllocator());
		rapidjson::Value borrowedValue;
		borrowedValue.SetString(
			borrowedText,
			static_cast<rapidjson::SizeType>(sizeof(borrowedText) - 1U));
		borrowedSource.AddMember(borrowedKey, borrowedValue, borrowedSource.GetAllocator());
		rapidjson::Value borrowedClone;
		VsmrRapidJson::CloneJsonValue(
			borrowedSource,
			borrowedClone,
			destination.GetAllocator());
		borrowedText[0] = 'X';
		Expect(
			std::string(borrowedClone["value"].GetString()) == "borrowed",
			"shared RapidJSON clone does not retain borrowed string storage");

		VsmrRapidJson::SetBoolMember(
			clone,
			"enabled",
			false,
			destination.GetAllocator());
		VsmrRapidJson::AddString(
			clone,
			"status",
			"ok",
			destination.GetAllocator());
		Expect(
			clone["enabled"].IsBool() && !clone["enabled"].GetBool() &&
			clone["status"].IsString() && std::string(clone["status"].GetString()) == "ok",
			"shared RapidJSON utility updates scalar members");
	}

	void TestJsonInputLimitBoundaries()
	{
		std::string error;
		VsmrJsonInputLimits::Limits limits;
		limits.maximumDepth = 2U;
		Expect(
			!VsmrJsonInputLimits::Validate("[[[]]]", limits, error),
			"shared JSON validator enforces nesting depth");

		limits = {};
		limits.maximumStringBytes = 3U;
		Expect(
			!VsmrJsonInputLimits::Validate(R"json({"x":"four"})json", limits, error),
			"shared JSON validator enforces string length");

		limits = {};
		limits.maximumValues = 2U;
		Expect(
			!VsmrJsonInputLimits::Validate("[1,2]", limits, error),
			"shared JSON validator enforces total value count");

		limits = {};
		limits.maximumContainerEntries = 2U;
		Expect(
			!VsmrJsonInputLimits::Validate("[1,2,3]", limits, error),
			"shared JSON validator enforces container size");

		limits = {};
		limits.maximumFeatures = 1U;
		Expect(
			!VsmrJsonInputLimits::Validate(
				R"json({"features":[{},{}]})json",
				limits,
				error),
			"shared JSON validator enforces AVISO feature count");

		limits = {};
		limits.maximumCoordinatePairs = 1U;
		Expect(
			!VsmrJsonInputLimits::Validate(
				R"json({"coordinates":[[1,2],[3,4]]})json",
				limits,
				error),
			"shared JSON validator enforces AVISO coordinate count");

		std::string oversizedConfig(CConfig::MaximumSerializedInputBytes + 1U, ' ');
		Expect(
			!CConfig::validateSerializedInputLimits(oversizedConfig, error),
			"configuration validation rejects oversized input before parsing");
		rapidjson::Document oversizedConfigDocument;
		std::string oversizedConfigParseError;
		Expect(
			!VsmrRuntimeConfigInternal::ParseValidatedArray(
				oversizedConfig,
				oversizedConfigDocument,
				&oversizedConfigParseError) &&
				oversizedConfigParseError.find("16 MB") != std::string::npos,
			"configuration parser bounds arbitrary serialized input");
		oversizedConfig.clear();
		oversizedConfig.shrink_to_fit();

		const std::filesystem::path boundedConfigPath =
			std::filesystem::temp_directory_path() /
			(L"vSMR_bounded_config_" +
				std::to_wstring(::GetCurrentProcessId()) + L"_" +
				std::to_wstring(::GetTickCount64()) + L".json");
		{
			std::ofstream output(boundedConfigPath, std::ios::binary | std::ios::trunc);
			output << "[]";
		}
		std::string boundedContents;
		std::string boundedError;
		rapidjson::Document boundedDocument;
		const bool boundedRead = VsmrRuntimeConfigInternal::ReadFileContents(
			boundedConfigPath.u8string(),
			boundedContents,
			&boundedError);
		Expect(
			boundedRead &&
				VsmrRuntimeConfigInternal::ParseSizeBoundedArray(
					boundedContents,
					boundedDocument,
					&boundedError),
			"configuration file pipeline parses bytes after the reader bounds their size");

		const std::filesystem::path oversizedConfigPath =
			boundedConfigPath.parent_path() /
			(L"vSMR_oversized_config_" +
				std::to_wstring(::GetCurrentProcessId()) + L"_" +
				std::to_wstring(::GetTickCount64()) + L".json");
		{
			std::ofstream output(oversizedConfigPath, std::ios::binary | std::ios::trunc);
			output.seekp(static_cast<std::streamoff>(CConfig::MaximumSerializedInputBytes));
			output.put(' ');
		}
		boundedContents = "stale";
		boundedError.clear();
		Expect(
			!VsmrRuntimeConfigInternal::ReadFileContents(
				oversizedConfigPath.u8string(),
				boundedContents,
				&boundedError) &&
				boundedContents.empty() &&
				boundedError.find("16 MB") != std::string::npos,
			"configuration reader rejects oversized files before allocation and parsing");
		std::error_code cleanupError;
		std::filesystem::remove(boundedConfigPath, cleanupError);
		cleanupError.clear();
		std::filesystem::remove(oversizedConfigPath, cleanupError);

		std::string oversizedAviso(AvisoDocumentModel::MaximumSerializedInputBytes + 1U, ' ');
		Expect(
			!AvisoDocumentModel::ValidateSerializedInputLimits(oversizedAviso, error),
			"AVISO validation rejects oversized input before parsing");
	}

	void TestGroundState()
	{
		Expect(classifyGroundState("ST-UP", 0, false) == GroundStateCategory::Stup, "ground state startup alias");
		Expect(classifyGroundState("P/B", 3, false) == GroundStateCategory::Push, "ground state push alias");
		Expect(classifyGroundState("LINE UP", 0, true) == GroundStateCategory::Lnup, "ground state lineup alias");
		Expect(classifyGroundState("", 0, false) == GroundStateCategory::Gate, "stationary empty state defaults to gate");
		Expect(classifyGroundState("", 2, false) == GroundStateCategory::Unknown, "moving empty state stays unknown");
		Expect(!shouldDisplayTagInTowerMode("STUP", 0, false, 0), "tower mode hides startup");
		Expect(shouldDisplayTagInTowerMode("TAXI", 8, false, 0), "tower mode shows taxi");
	}

	void TestSharedGroundState()
	{
		Expect(VsmrGroundStateSync::CanWriteSharedState(0) &&
			VsmrGroundStateSync::CanWriteSharedState(10) &&
			VsmrGroundStateSync::CanWriteSharedState(19),
			"shared states may use only unassigned speeds or their reserved band");
		for (int speed : { -1, 1, 9, 20, 180, 250 })
			Expect(!VsmrGroundStateSync::CanWriteSharedState(speed),
				"selecting Line Up must not overwrite a real speed assignment");
		Expect(
			VsmrGroundStateSync::AssignedSpeedForCategory(GroundStateCategory::Lnup) ==
				VsmrGroundStateSync::LineupAssignedSpeed,
			"line up maps to the reserved assigned speed");
		Expect(
			VsmrGroundStateSync::AssignedSpeedForCategory(GroundStateCategory::Taxi) == 0,
			"a state EuroScope synchronizes itself shares no assigned speed");
		Expect(
			VsmrGroundStateSync::CategoryForAssignedSpeed(VsmrGroundStateSync::LineupAssignedSpeed) ==
				GroundStateCategory::Lnup,
			"the reserved assigned speed reads back as line up");
		Expect(
			VsmrGroundStateSync::CategoryForAssignedSpeed(250) == GroundStateCategory::Unknown,
			"a real speed assignment carries no ground state");
		Expect(
			VsmrGroundStateSync::IsReservedAssignedSpeed(VsmrGroundStateSync::ReservedFirst) &&
				VsmrGroundStateSync::IsReservedAssignedSpeed(VsmrGroundStateSync::ReservedLast),
			"the reserved band covers both of its bounds");
		Expect(
			!VsmrGroundStateSync::IsReservedAssignedSpeed(0) &&
				!VsmrGroundStateSync::IsReservedAssignedSpeed(VsmrGroundStateSync::ReservedLast + 1),
			"speeds outside the reserved band stay real assignments");

		// Both can be set at once, and the shared value is the one a controller picked
		Expect(
			classifyGroundStateWithSharedState("TAXI", 8, false, VsmrGroundStateSync::LineupAssignedSpeed) ==
				GroundStateCategory::Lnup,
			"a shared line up wins over the taxi status");
		Expect(
			classifyGroundStateWithSharedState("TAXI", 8, false, 0) == GroundStateCategory::Taxi,
			"without a shared state the EuroScope status is used");
		Expect(
			classifyGroundStateWithSharedState("PUSH", 0, false, VsmrGroundStateSync::ReservedLast) ==
				GroundStateCategory::Push,
			"a reserved value this version does not know falls back to the EuroScope status");
		Expect(
			classifyGroundStateWithSharedState("TAXI", 8, false, 250) == GroundStateCategory::Taxi,
			"a real speed assignment leaves the EuroScope status alone");
		// A status a controller picks outside vSMR takes the aircraft off its
		// shared state, so both clients agree again on the default state
		Expect(
			classifyGroundState(VsmrGroundStateSync::LineupEuroScopeStatus, 0, false) ==
				VsmrGroundStateSync::CompanionCategoryForAssignedSpeed(
					VsmrGroundStateSync::LineupAssignedSpeed),
			"the status vSMR writes with line up is the one it expects to read back");
		Expect(
			classifyGroundStateWithSharedState("PUSH", 0, false, VsmrGroundStateSync::LineupAssignedSpeed) ==
				GroundStateCategory::Push,
			"a status changed outside vSMR wins over a shared state");
		Expect(
			classifyGroundStateWithSharedState("DEPA", 20, false, VsmrGroundStateSync::LineupAssignedSpeed) ==
				GroundStateCategory::Depa,
			"a departure status changed outside vSMR wins over a shared state");
		Expect(
			classifyGroundStateWithSharedState("", 0, false, VsmrGroundStateSync::LineupAssignedSpeed) ==
				GroundStateCategory::Gate,
			"a shared state without the status vSMR writes with it is not shown");
		Expect(
			shouldDisplayTagInTowerMode("TAXI", 0, false, VsmrGroundStateSync::LineupAssignedSpeed),
			"tower mode shows a shared line up");
		Expect(
			!shouldDisplayTagInTowerMode("PUSH", 0, false, VsmrGroundStateSync::LineupAssignedSpeed),
			"tower mode follows a status changed outside vSMR");
		Expect(
			!shouldDisplayTagInTowerMode("", 0, false, 0),
			"tower mode still hides an aircraft without any status");

		// The tag turns airborne above this speed, so a value nobody has cleared
		// yet is never shown as a ground state
		Expect(
			classifyGroundStateWithSharedState(
				"TAXI",
				VsmrTargetRoleLogic::DefaultAirborneThresholdKt,
				false,
				VsmrGroundStateSync::LineupAssignedSpeed) == GroundStateCategory::Lnup,
			"a shared ground state survives up to the airborne threshold");
		Expect(
			classifyGroundStateWithSharedState(
				"TAXI",
				VsmrTargetRoleLogic::DefaultAirborneThresholdKt + 1,
				false,
				VsmrGroundStateSync::LineupAssignedSpeed) == GroundStateCategory::Taxi,
			"an airborne target ignores a shared ground state");
		Expect(
			!shouldDisplayTagInTowerMode(
				"",
				VsmrTargetRoleLogic::DefaultAirborneThresholdKt + 1,
				false,
				VsmrGroundStateSync::LineupAssignedSpeed),
			"tower mode ignores a shared ground state once airborne");
	}

	void TestHoldingPointRemarks()
	{
		std::string normalized;
		std::string error;
		Expect(VsmrHoldingPoint::Normalize(" a3 ", normalized, &error) && normalized == "A3", "holding point normalization");
		Expect(!VsmrHoldingPoint::Normalize("TOO-LONG-9", normalized, &error), "holding point length limit");
		Expect(!VsmrHoldingPoint::Normalize("A 3", normalized, &error), "holding point character limit");

		const std::string replaced = VsmrHoldingPoint::Write(
			"RMK VSMRHP/T1 HP T2 HP:T3 OTHER",
			"A3");
		Expect(replaced == "RMK OTHER VSMRHP/A3", "holding point replaces duplicate current and legacy markers");
		Expect(VsmrHoldingPoint::Read(replaced) == "A3", "holding point reads synchronized marker");
		Expect(VsmrHoldingPoint::Write(replaced, "A3") == replaced, "holding point write is idempotent");
		Expect(VsmrHoldingPoint::Write(replaced, "") == "RMK OTHER", "holding point None removes the marker");
	}

	void TestTagTokens()
	{
		const std::vector<std::string> tokens = SplitDefinitionTokens(
			"callsign, clearance(WAIT,GO) | scratchpad");
		Expect(tokens.size() == 3, "tag token list keeps arguments together");
		Expect(tokens.size() >= 2 && tokens[1] == "clearance(WAIT,GO)", "tag clearance token remains intact");

		DefinitionTokenStyleData style = ParseDefinitionTokenStyle("b:callsign(12,34,56)");
		Expect(style.bold && style.hasCustomColor && style.token == "callsign", "tag style parses bold colored token");
		Expect(style.colorR == 12 && style.colorG == 34 && style.colorB == 56, "tag style parses RGB channels");

		std::string pending;
		std::string cleared;
		Expect(TryParseClearanceTokenDisplay("clearance(HOLD,CLEARED)", pending, cleared), "clearance display token parses");
		Expect(pending == "HOLD" && cleared == "CLEARED", "clearance display states are preserved");
		Expect(TryParseClearanceTokenDisplay("clearance()", pending, cleared) && pending.empty() && cleared.empty(), "empty clearance display hides both states");
	}

	void TestRecentAirports()
	{
		std::vector<std::string> history;
		for (const char* airport : { "LFPG", "LFPO", "LFMN", "LFML", "LFBO", "LFLL" })
			VsmrRadar::RememberAirport(history, airport);
		Expect(history == std::vector<std::string>({ "LFLL", "LFBO", "LFML", "LFMN", "LFPO" }),
			"Airport history keeps only the five most recently opened airports");
		VsmrRadar::RememberAirport(history, "lfmn");
		Expect(history == std::vector<std::string>({ "LFMN", "LFLL", "LFBO", "LFML", "LFPO" }),
			"Reopening an airport moves it to the front without duplicates");
		const auto before = history;
		for (const char* airport : { "", "LF", "LF PG", "LF.P" })
			VsmrRadar::RememberAirport(history, airport);
		Expect(history == before, "Invalid airport input cannot displace recent airports");
	}

	void TestVsidBridgeData()
	{
		using namespace VsmrParis;
		Expect(VsmrVsid::SupportsRegionalCommands(1U, 3U) &&
			!VsmrVsid::SupportsRegionalCommands(1U, 2U), "Regional commands require compatible providers");
		VsmrVsid::CommandAction removed{};
		Expect(!VsmrVsid::TryParseRuntimeActionId("runtime.vsid.paris-auto", removed),
			"Automatic runway control is no longer exposed");
		for (const auto airport : Airports)
		{
			std::map<std::string, bool> rules = {
				{ "paris_auto", true }, { "paris_manual_config", false },
				{ "pgeast", false }, { "opposing", false }, { "unrelated", true }
			};
			Expect(VsmrVsid::CanSubmitParisCommand(true, false, true, airport),
				"Manual controls do not require runway telemetry");
			if (IsRegional(airport))
			{
				for (std::size_t index = 0; index < RegionalRules.size(); ++index)
				{
					const auto rule = RegionalRules[index];
					const auto& action = VsmrVsid::RegionalActions[index];
					VsmrVsid::CommandAction parsed{};
					Expect(VsmrVsid::TryParseRuntimeActionId(action.objectId, parsed) &&
						VsmrVsid::BuildCommand(parsed, airport) == ".vsid paris " + std::string(airport) + " " + std::string(rule),
						"Regional popup builds the selected command");
					Expect(Select(rules, airport, rule), "Manual regional choice is accepted");
					const auto before = rules;
					const auto state = Resolve(rules, airport);
					Expect(RegionalRule(state) == rule && rules == before && rules["unrelated"] &&
						rules["pgeast"] == (rule.front() == 'e'),
						"Manual state is read without mutation, even with obsolete automatic flags enabled");
					for (const auto candidate : RegionalRules)
						Expect(rules[std::string(candidate)] == (candidate == rule), "Regional choices remain exclusive");
					Expect(Parse(Serialize(airport, state)).at(std::string(airport)) == state,
						"Published regional state roundtrips through the bridge");
					Select(rules, airport, rule);
					Expect(rules == before, "Repeating a manual selection does not reprocess unchanged rules");
				}
				const auto before = rules;
				Expect(!Select(rules, airport, "linked") && rules == before,
					"Regional link-only commands cannot infer a runway direction");
				rules["wlpg"] = true;
				rules["eipg"] = true;
				const auto ambiguous = rules;
				Expect(RegionalRule(Resolve(rules, airport)).empty() && rules == ambiguous,
					"Conflicting regional flags are reported unknown without correcting them automatically");
			}
			else
			{
				for (const auto linked : { true, false })
				{
					const auto choice = linked ? "linked" : "unlinked";
					Expect(Select(rules, airport, choice) && rules["linked"] == linked &&
						rules["unlinked"] != linked && rules["opposing"] != linked,
						"PG and PO manual choices update their SID rule explicitly");
					const auto before = rules;
					const auto state = Resolve(rules, airport);
					Expect(state.linked == linked && rules == before &&
						Parse(Serialize(airport, state)).at(std::string(airport)) == state,
						"Link state is published without automatic changes");
				}
				const auto before = rules;
				Expect(!Select(rules, airport, "wlpg") && rules == before, "Regional rules cannot change PG or PO");
			}
			const auto before = rules;
			Expect(!Select(rules, airport, "auto") && rules == before, "Removed automatic command has no effect");
		}
		Expect(IsControlRule("PARIS_AUTO") && IsControlRule("PARIS_MANUAL_CONFIG") &&
			IsControlRule("LINKED") && IsControlRule("UNLINKED") && !IsControlRule("WLPG"),
			"Obsolete mode metadata remains excluded from SID filtering for old configuration files");
		Expect(!VsmrVsid::CanSubmitParisCommand(false, false, true, "LFPG") &&
			!VsmrVsid::CanSubmitParisCommand(true, true, true, "LFPG") &&
			!VsmrVsid::CanSubmitParisCommand(true, false, false, "LFPG") &&
			!VsmrVsid::CanSubmitParisCommand(true, false, true, "LFLL"),
			"Manual controls require an available provider and supported airport");
		for (const auto invalid : { "LFPG=WLA", "LFPG=WXA;", "LFXX=WLA;", "LFPG=WLA;LFPG=EUM;", "LFPG=WLA;garbage" })
			Expect(Parse(invalid).empty(), "Paris bridge rejects malformed or duplicate state records");
		Expect(Parse(std::string(63, 'A')).empty(), "Paris bridge enforces its bounded snapshot size");
		const auto modes = VsmrVsid::ParseAutomaticModes("LFPG=1;LFPO=0;");
		Expect(modes.size() == 2 && modes.at("LFPG") && !modes.at("LFPO"),
			"vSID automatic mode preserves independent authoritative airport states");
		for (const auto invalid : { "LFPG=1", "LFPG=2;", "lfpg=1;", "LFPG=1;LFPG=0;", "LFPG=1;garbage" })
			Expect(VsmrVsid::ParseAutomaticModes(invalid).empty(), "Invalid vSID automatic snapshots become unknown");
		Expect(VsmrVsid::ParseAutomaticModes(std::string(4102, 'A')).empty(),
			"vSID automatic snapshots enforce a fixed size limit");
		Expect(
			VsmrVsid::NormalizeFieldValue("  LAM1X \t") == "LAM1X",
			"vSID bridge values trim protocol whitespace");
		Expect(
			VsmrVsid::NormalizeFieldValue(std::string(VsmrVsid::MaximumFieldBytes + 1U, 'A')).empty(),
			"vSID bridge values enforce the provider field limit");
		const char embeddedNull[] = { 'A', '5', '0', '\0', 'X' };
		Expect(
			VsmrVsid::NormalizeFieldValue(
				std::string_view(embeddedNull, sizeof(embeddedNull))).empty(),
			"vSID bridge values reject embedded nulls");

		VsmrVsid::AircraftData data;
		data.sid = "LAM1X";
		data.runway = "26R";
		data.clearedFlightLevel = "A50";
		Expect(
			VsmrVsid::HasPublishedAircraftData(data),
			"published vSID fields identify an active aircraft");
		Expect(
			!VsmrVsid::HasPublishedAircraftData(VsmrVsid::AircraftData()),
			"empty bridge records do not inflate the active aircraft count");
		std::map<std::string, std::string> tokens;
		VsmrVsid::AddTagTokens(tokens, &data);
		Expect(
			tokens["vsid_sid"] == "LAM1X" &&
			tokens["vsid_rwy"] == "26R" &&
			tokens["vsid_cfl"] == "A50",
			"vSID bridge data populates the three public tag tokens");
		VsmrVsid::AddTagTokens(tokens, nullptr);
		Expect(
			tokens["vsid_sid"].empty() &&
			tokens["vsid_rwy"].empty() &&
			tokens["vsid_cfl"].empty(),
			"missing vSID bridge data leaves stable empty tag tokens");

		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::AutomaticModeToggle,
				" lfpg ") == ".vsid auto LFPG",
			"vSID airport actions normalize a valid ICAO");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::AutomaticModeToggle,
				"LF PG").empty(),
			"vSID airport actions reject embedded whitespace");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgGroundCrossing,
				"LFPG&reload").empty(),
			"vSID airport actions reject command metacharacters");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::Synchronize,
				"") == ".vsid sync",
			"vSID global actions do not require an airport");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgMinimumTaxiing,
				"LFPG") == ".vsid area LFPG OFF" &&
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgGroundCrossing,
				"LFPG") == ".vsid area LFPG OFF" &&
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgGroundCrossing,
				"LFPO").empty(),
			"LFPG taxi commands use areas, never the opposing rule");
		Expect(
			VsmrVsid::BuildCommandSequence(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "lfpg") ==
				std::vector<std::string>{ ".vsid area LFPG OFF", ".vsid area LFPG NORTH", ".vsid area LFPG SOUTH" } &&
			VsmrVsid::BuildCommandSequence(VsmrVsid::CommandAction::LfpgGroundCrossing, "LFPG") ==
				std::vector<std::string>{ ".vsid area LFPG OFF" } &&
			VsmrVsid::BuildCommandSequence(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPO").empty(),
			"Minimum Taxiing starts from OFF before enabling both geographic areas; Ground Crossing disables areas");
		Expect(
			VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgMinimumTaxiing, VsmrVsid::LfpgTaxiMode::MinimumTaxiing) &&
			!VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgGroundCrossing, VsmrVsid::LfpgTaxiMode::MinimumTaxiing) &&
			VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgGroundCrossing, VsmrVsid::LfpgTaxiMode::GroundCrossing) &&
			!VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgMinimumTaxiing, VsmrVsid::LfpgTaxiMode::GroundCrossing) &&
			!VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgMinimumTaxiing, std::nullopt) &&
			!VsmrVsid::IsLfpgTaxiActionSelected(VsmrVsid::CommandAction::LfpgLinked, VsmrVsid::LfpgTaxiMode::MinimumTaxiing),
			"Taxi row highlights only its own last submitted command, never a link selection");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgLinked,
				"LFPG") == ".vsid rule LFPG opposing" &&
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgUnlinked,
				"LFPG") == ".vsid rule LFPG opposing" &&
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::LfpgLinked,
				"LFPO") == ".vsid paris LFPO linked",
			"LFPG link controls toggle opposing; LFPO keeps its existing companion commands");
		Expect(
			VsmrVsid::BuildCommand(
				VsmrVsid::CommandAction::ReloadConfiguration,
				"") == ".vsid reload",
			"vSID config reload builds only the documented fixed command");

		VsmrVsid::CommandAction action{};
		Expect(
			VsmrVsid::TryParseRuntimeActionId("runtime.vsid.lfpg-ground-crossing", action) &&
			action == VsmrVsid::CommandAction::LfpgGroundCrossing,
			"vSID Runtime Menu actions map to a fixed allowlist");
		Expect(
			VsmrVsid::TryParseRuntimeActionId("runtime.vsid.lfpg-unlinked", action) &&
			action == VsmrVsid::CommandAction::LfpgUnlinked,
			"vSID Runtime Menu maps the LFPG link-state actions");
		Expect(
			!VsmrVsid::TryParseRuntimeActionId("runtime.vsid.reload-ese", action) &&
			!VsmrVsid::TryParseRuntimeActionId("runtime.vsid.lvp", action) &&
			!VsmrVsid::TryParseRuntimeActionId("runtime.vsid.raw-command", action),
			"removed and unknown vSID actions are not exposed");
	}

	void TestCdmBridgeData()
	{
		Expect(
			VsmrCdm::FormatTimeToken(0) == "0000" &&
			VsmrCdm::FormatTimeToken(23 * 60 + 59) == "2359" &&
			VsmrCdm::FormatTimeToken(-1).empty() &&
			VsmrCdm::FormatTimeToken(24 * 60).empty(),
			"CDM bridge times use validated four-digit UTC text");
		Expect(
			VsmrCdm::NormalizeStringField("  REMOTE  ") == "REMOTE",
			"CDM bridge strings trim protocol whitespace");
		const char invalidText[] = { 'A', '\0', 'B' };
		Expect(
			VsmrCdm::NormalizeStringField(
				std::string_view(invalidText, sizeof(invalidText))).empty(),
			"CDM bridge strings reject embedded nulls");

		VsmrCdm::AircraftData data;
		data.tobt = 7 * 60 + 5;
		data.tsat = 7 * 60 + 10;
		data.ttot = 7 * 60 + 20;
		data.ctot = 7 * 60 + 25;
		data.tsac = 7 * 60 + 10;
		data.asrt = 7 * 60 + 2;
		data.asat = 7 * 60 + 11;
		data.deice = "REMOTE";
		data.tobtSetBy = "PILOT";
		data.flowRestriction = "ATFCM";
		data.ecfmpRestriction = "EDYY01";
		data.manualCtot = true;
		Expect(
			VsmrCdm::HasPublishedAircraftData(data),
			"published CDM fields identify an active bridge aircraft");
		Expect(
			VsmrCdm::IsReadyStartup(&data) && !VsmrCdm::IsReadyStartup(nullptr),
			"CDM Ready Start-up follows the published ASRT state");
		Expect(
			std::string(VsmrCdm::PluginName) == "CDM Plugin" &&
			VsmrCdm::ReadyStartupTagItemCode == 12 &&
			VsmrCdm::ToggleReadyStartupFunctionId == 106,
			"Ready Start-up dispatch matches CDM's registered tag function contract");

		std::map<std::string, std::string> tokens;
		VsmrCdm::AddTagTokens(tokens, &data);
		Expect(
			tokens["ready_startup"] == "RDY" &&
			tokens["tobt"] == "0705" &&
			tokens["tsat"] == "0710" &&
			tokens["ttot"] == "0720" &&
			tokens["ctot"] == "0725" &&
			tokens["tsac"] == "0710" &&
			tokens["asrt"] == "0702" &&
			tokens["asat"] == "0711" &&
			tokens.find("cdm_deice") == tokens.end() &&
			tokens.find("cdm_manual_ctot") == tokens.end(),
			"selected CDM bridge fields use concise public tag tokens");
		VsmrCdm::AddTagTokens(tokens, nullptr);
		Expect(
			tokens["ready_startup"] == "RDY" &&
			tokens["tobt"].empty() &&
			tokens.find("cdm_deice") == tokens.end(),
			"missing CDM bridge data keeps the red Ready Start-up indicator visible");
	}

	void TestRimcasRules()
	{
		const VsmrRimcasLogic::RunwayMonitoring departureOnly =
			VsmrRimcasLogic::ResolveSelectedRunwayMonitoring(false, true, false, false);
		Expect(!departureOnly.arrivals && departureOnly.departures,
			"RIMCAS derives departure monitoring from either selected runway end");
		const VsmrRimcasLogic::RunwayMonitoring mixedUse =
			VsmrRimcasLogic::ResolveSelectedRunwayMonitoring(true, false, false, true);
		Expect(mixedUse.arrivals && mixedUse.departures,
			"RIMCAS combines selected arrival and departure runway ends");
		const VsmrRimcasLogic::RunwayMonitoring inactive =
			VsmrRimcasLogic::ResolveSelectedRunwayMonitoring(false, false, false, false);
		Expect(!inactive.arrivals && !inactive.departures,
			"RIMCAS leaves an unselected runway pair inactive");
		Expect(VsmrRimcasLogic::IsRunwayOccupancyMonitored(true, false), "RIMCAS monitors arrival-only runway");
		Expect(VsmrRimcasLogic::IsRunwayOccupancyMonitored(false, true), "RIMCAS monitors departure-only runway");
		Expect(!VsmrRimcasLogic::IsRunwayOccupancyMonitored(false, false), "RIMCAS ignores disabled runway");
		Expect(VsmrRimcasLogic::HasApproachingConflict(1), "RIMCAS detects occupied approach conflict");
		Expect(!VsmrRimcasLogic::HasApproachingConflict(0), "RIMCAS accepts empty runway");
	}

	void TestTargetRoleThresholds()
	{
		Expect(
			!VsmrTargetRoleLogic::IsAirborneForTagRole(true, 40),
			"arrival at 40 kt uses the arrived tag");
		Expect(
			VsmrTargetRoleLogic::IsAirborneForTagRole(true, 41),
			"arrival above 40 kt keeps the airborne tag");
		Expect(
			!VsmrTargetRoleLogic::IsAirborneForTagRole(false, 50),
			"departure retains the 50 kt ground threshold");
		Expect(
			VsmrTargetRoleLogic::IsAirborneForTagRole(false, 51),
			"departure above 50 kt remains airborne");
	}

	void TestRuntimeReleaseLifecycle()
	{
		using VsmrLoaderLifecycle::RuntimeReleaseResult;

		void* module = nullptr;
		void* plugin = nullptr;
		std::function<bool()> shutdown;
		int unloadCalls = 0;
		auto unload = [&unloadCalls](void*)
		{
			++unloadCalls;
			return true;
		};
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				unload) == RuntimeReleaseResult::Released &&
			unloadCalls == 0,
			"empty loader state is already released");

		int moduleToken = 0;
		int pluginToken = 0;
		module = &moduleToken;
		plugin = &pluginToken;
		int shutdownCalls = 0;
		bool shutdownAllowed = false;
		shutdown = [&shutdownCalls, &shutdownAllowed]()
		{
			++shutdownCalls;
			return shutdownAllowed;
		};
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				unload) == RuntimeReleaseResult::RuntimeRetained &&
			module == &moduleToken &&
			plugin == &pluginToken &&
			shutdown &&
			shutdownCalls == 1 &&
			unloadCalls == 0,
			"active runtime remains published while shutdown is unsafe");

		shutdownAllowed = true;
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				unload) == RuntimeReleaseResult::Released &&
			module == nullptr &&
			plugin == nullptr &&
			!shutdown &&
			shutdownCalls == 2 &&
			unloadCalls == 1,
			"retained runtime releases successfully on a later retry");

		module = &moduleToken;
		plugin = &pluginToken;
		shutdown = []() -> bool
		{
			throw std::runtime_error("shutdown failure");
		};
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				unload) == RuntimeReleaseResult::RuntimeRetained &&
			module == &moduleToken &&
			plugin == &pluginToken,
			"shutdown exceptions retain the published runtime");

		shutdown = []() { return true; };
		auto rejectUnload = [](void*) { return false; };
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				rejectUnload) == RuntimeReleaseResult::ModuleUnloadFailed &&
			module == &moduleToken &&
			plugin == nullptr &&
			!shutdown,
			"failed module unmap retains only the inert module for another retry");
		Expect(
			VsmrLoaderLifecycle::TryReleasePublishedRuntime(
				module,
				shutdown,
				plugin,
				unload) == RuntimeReleaseResult::Released &&
			module == nullptr &&
			unloadCalls == 2,
			"inert module unload retries without invoking runtime shutdown again");
	}

	void TestGeometry()
	{
		Expect(SMRGeometry::ZoomLevelFromCrossDistance(2000.0) == 14, "zoom boundary 2000 m");
		Expect(SMRGeometry::ZoomLevelFromCrossDistance(2001.0) == 13, "zoom boundary above 2000 m");
		Expect(SMRGeometry::ZoomLevelFromCrossDistance(34001.0) == 0, "zoom outer boundary");

		EuroScopePlugIn::CPosition origin;
		origin.m_Latitude = 48.0;
		origin.m_Longitude = 2.0;
		const EuroScopePlugIn::CPosition projected = SMRGeometry::ProjectPosition(origin, 90.0, 1000.0);
		const double distance = SMRGeometry::DistanceMeters(origin, projected);
		Expect(std::abs(distance - 1000.0) < 10.0, "projected position preserves distance");
		Expect(projected.m_Longitude > origin.m_Longitude, "eastward projection increases longitude");
	}

	void TestWebMessageValidation()
	{
		using VsmrWebMessageValidation::HasValidInboundWebMessageShape;
		using VsmrWebMessageValidation::TryGetInboundWebMessageSelector;

		std::string selector;
		Expect(
			TryGetInboundWebMessageSelector(
				R"json({"version":1,"id":"42","type":"ui.ready","payload":{}})json",
				selector) && selector == "ui.ready",
			"WebView bridge accepts a valid protocol envelope");
		Expect(
			HasValidInboundWebMessageShape(
				R"json({"action":"state.request","payload":null})json"),
			"WebView bridge accepts the legacy action envelope");
		Expect(
			!HasValidInboundWebMessageShape(
				R"json({"version":2,"type":"ui.ready"})json"),
			"WebView bridge rejects unsupported protocol versions");
		Expect(
			!HasValidInboundWebMessageShape(
				R"json({"version":1,"type":"ui.ready","action":"state.request"})json"),
			"WebView bridge rejects ambiguous message selectors");
		Expect(
			!HasValidInboundWebMessageShape(
				R"json({"version":1,"type":"ui.ready","type":"state.request"})json"),
			"WebView bridge rejects duplicate protocol fields");
		Expect(
			!HasValidInboundWebMessageShape(R"json({"version":1,"payload":{}})json"),
			"WebView bridge requires a message selector");
		Expect(
			!HasValidInboundWebMessageShape(R"json(["ui.ready"])json"),
			"WebView bridge rejects non-object roots");

		const std::string longType(129U, 'x');
		Expect(
			!HasValidInboundWebMessageShape(
				std::string("{\"type\":\"") + longType + "\"}"),
			"WebView bridge bounds message-selector length");

		std::string deepMessage = R"json({"type":"ui.ready","payload":)json";
		deepMessage.append(65U, '[');
		deepMessage += '0';
		deepMessage.append(65U, ']');
		deepMessage += '}';
		Expect(
			!HasValidInboundWebMessageShape(deepMessage),
			"WebView bridge bounds JSON depth before queueing");

		const std::string oversizedPayloadString(64U * 1024U + 1U, 'x');
		Expect(
			!HasValidInboundWebMessageShape(
				std::string(R"json({"type":"state.save","payload":{"value":")json") +
				oversizedPayloadString + R"json("}})json"),
			"WebView bridge bounds payload strings before DOM parsing");

		std::string embeddedNull = R"json({"type":"ui.ready"})json";
		embeddedNull.push_back('\0');
		Expect(
			!HasValidInboundWebMessageShape(embeddedNull),
			"WebView bridge rejects embedded NUL bytes");
	}

	void TestControlCenterMessageProtocol()
	{
		using namespace VsmrControlCenterProtocol;
		Expect(
			ActionFromType("  STATE.SAVE ") == VsmrBridgeAction::StateSave,
			"Control Center protocol normalizes action names");
		Expect(
			ActionFromType("browse.aviso") == VsmrBridgeAction::ResourceComputerLoad,
			"Control Center protocol preserves compatibility aliases");
		Expect(
			ActionFromType("not.supported") == VsmrBridgeAction::Unknown,
			"Control Center protocol rejects unknown actions");

		rapidjson::Document valid;
		valid.Parse<0>(R"json({"version":1,"id":"request-1","type":"settings.update","payload":{"enabled":true}})json");
		DecodedEnvelope decoded;
		std::string error;
		Expect(
			DecodeEnvelope(valid, decoded, error) &&
				decoded.id == "request-1" &&
				decoded.action == VsmrBridgeAction::SettingsUpdate &&
				decoded.payload != nullptr && decoded.payload->IsObject(),
			"Control Center protocol decodes a typed payload envelope");

		rapidjson::Document unsupported;
		unsupported.Parse<0>(R"json({"version":2,"type":"ui.ready"})json");
		Expect(
			!DecodeEnvelope(unsupported, decoded, error) && !error.empty(),
			"Control Center protocol rejects unsupported versions");

		rapidjson::Document outgoing;
		MakeEnvelope(outgoing, "state.ack", "request-2");
		Expect(
			outgoing.IsObject() && outgoing["version"].GetInt() == Version &&
				std::string(outgoing["type"].GetString()) == "state.ack" &&
				std::string(outgoing["id"].GetString()) == "request-2",
			"Control Center protocol builds versioned response envelopes");
	}

	void TestControlCenterScriptLayout(const std::filesystem::path& repositoryRoot)
	{
		const std::filesystem::path webRoot =
			repositoryRoot / "vSMR" / "src" / "control_center" / "web";
		const std::array<const char*, 13> sources = {
			"app-model.js",
			"app-workflow.js",
			"app-runtime.js",
			"app-profile-colors.js",
			"app-profile-editor.js",
			"app-aviso-editor.js",
			"app-aviso-clipboard.js",
			"app-settings.js",
			"app-persistence.js",
			"app-interaction-help.js",
			"app-events.js",
			"app-actions.js",
			"app.js"
		};
		std::vector<std::string> styleSources;
		std::istringstream styleManifest(
			ReadTextFile(webRoot / "styles" / "sources.txt"));
		for (std::string sourceName; std::getline(styleManifest, sourceName);)
		{
			if (!sourceName.empty() && sourceName.back() == '\r')
				sourceName.pop_back();
			if (!sourceName.empty() && sourceName.front() != '#')
				styleSources.push_back(sourceName);
		}
		Expect(!styleSources.empty(), "Control Center style manifest is not empty");
		for (const char* retiredSource : {
			"visual-corrections.css", "theme-foundations.css", "shared-theme.css" })
		{
			Expect(
				!std::filesystem::exists(webRoot / "styles" / retiredSource),
				std::string("Control Center override layer stays retired: ") + retiredSource);
		}
		const std::string index = ReadTextFile(webRoot / "index.html");
		const std::string bundle = ReadTextFile(webRoot / "app-bundle.js");
		const std::string styleBundle = ReadTextFile(webRoot / "styles.css");
		Expect(
			index.find("<link href=\"styles.css\" rel=\"stylesheet\"/>") != std::string::npos,
			"Control Center index loads one generated stylesheet bundle");
		Expect(
			index.find("<script src=\"data.js\"></script>") != std::string::npos,
			"Control Center index loads its data snapshot");
		Expect(
			index.find("<script src=\"app-bundle.js\"></script>") != std::string::npos,
			"Control Center index loads one private application bundle");
		Expect(
			bundle.rfind("// Generated by vSMR/tools/build_control_center_bundle.ps1.", 0) == 0 &&
				bundle.find("(function () {") != std::string::npos &&
				bundle.rfind("})();") != std::string::npos,
			"Control Center bundle has one explicit private scope");

		std::size_t previousSourcePosition = 0;
		bool foundPreviousSource = false;
		for (const char* sourceName : sources)
		{
			const std::filesystem::path path = webRoot / sourceName;
			Expect(
				std::filesystem::is_regular_file(path),
				std::string("Control Center feature source exists: ") + sourceName);
			const std::string marker = std::string("// source: ") + sourceName;
			const std::size_t position = bundle.find(marker);
			Expect(
				position != std::string::npos,
				std::string("Control Center bundle includes: ") + sourceName);
			if (position != std::string::npos)
			{
				Expect(
					!foundPreviousSource || position > previousSourcePosition,
					std::string("Control Center bundle source order is stable at: ") + sourceName);
				previousSourcePosition = position;
				foundPreviousSource = true;
			}

			Expect(
				index.find(std::string("<script src=\"") + sourceName + "\"></script>") ==
					std::string::npos,
				std::string("Control Center feature source is not globally loaded: ") + sourceName);
			if (!std::filesystem::is_regular_file(path))
				continue;
			const std::string source = ReadTextFile(path);
			Expect(
				source.rfind("\"use strict\";", 0) == 0,
				std::string("Control Center feature source is strict: ") + sourceName);
			const std::size_t lineCount =
				static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + 1U;
			Expect(
				lineCount <= 1000U,
				std::string("Control Center feature source stays reviewable: ") + sourceName);
		}

		Expect(
			styleBundle.rfind(
				"/* Generated by vSMR/tools/build_control_center_styles.ps1.", 0) == 0,
			"Control Center stylesheet is generated from focused sources");
		previousSourcePosition = 0;
		foundPreviousSource = false;
		for (const std::string& sourceName : styleSources)
		{
			const std::filesystem::path path = webRoot / "styles" / sourceName;
			Expect(
				std::filesystem::is_regular_file(path),
				"Control Center style source exists: " + sourceName);
			const std::string marker = "/* source: " + sourceName + " */";
			const std::size_t position = styleBundle.find(marker);
			Expect(
				position != std::string::npos,
				"Control Center stylesheet includes: " + sourceName);
			if (position != std::string::npos)
			{
				Expect(
					!foundPreviousSource || position > previousSourcePosition,
					"Control Center style source order is stable at: " + sourceName);
				previousSourcePosition = position;
				foundPreviousSource = true;
			}

			if (!std::filesystem::is_regular_file(path))
				continue;
			const std::string source = ReadTextFile(path);
			const std::size_t lineCount =
				static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + 1U;
			Expect(
				lineCount <= 1000U,
				"Control Center style source stays reviewable: " + sourceName);
		}
	}

	void TestWeatherParsing()
	{
		VsmrWeather::Snapshot weather;
		Expect(
			VsmrWeather::ParseReport(
				"lfpg",
				"METAR LFPG 051200Z 22012G20KT 180V260 4000 -RA BKN012 OVC030 18/12 Q1009=",
				weather,
				1777982400),
			"METAR parser accepts a complete operational report");
		Expect(
			weather.icao == "LFPG" && weather.hasVisibility &&
			weather.visibilityMeters == 4000 && !weather.visibilityCavok,
			"METAR parser exposes structured visibility");
		Expect(
			weather.hasTemperature && weather.temperatureCelsius == 18 &&
			weather.hasDewPoint && weather.dewPointCelsius == 12,
			"METAR parser exposes temperature and dew point");
		Expect(
			weather.cloudSummary == "BKN012 OVC030",
			"METAR parser exposes a bounded compact cloud summary");

		Expect(
			VsmrWeather::ParseReport(
				"LFMN",
				"LFMN 051230Z VRB03KT CAVOK M02/M05 Q1021",
				weather,
				1777984200) &&
			weather.visibilityCavok && weather.visibilityMeters == 10000 &&
			weather.temperatureCelsius == -2 && weather.dewPointCelsius == -5,
			"METAR parser handles CAVOK and negative temperatures");
	}



	void TestSharedRenderingProjectIntegration(const std::filesystem::path& repositoryRoot)
	{
		struct SharedSourcePair
		{
			const char* sourcePath;
			const char* headerPath;
			const char* projectSourcePath;
			const char* projectHeaderPath;
			const char* headerInclude;
			const char* implementationMarker;
			const char* declarationMarker;
		};

		const std::array<SharedSourcePair, 3> sharedSources = { {
			{
				"src/aviso/AvisoRasterPipeline.cpp",
				"src/aviso/AvisoRasterPipeline.hpp",
				"src\\aviso\\AvisoRasterPipeline.cpp",
				"src\\aviso\\AvisoRasterPipeline.hpp",
				"#include \"aviso/AvisoRasterPipeline.hpp\"",
				"VsmrAviso::AvisoRasterPipeline::Queue",
				"class AvisoRasterPipeline final"
			},
			{
				"src/rendering/TagRenderer.cpp",
				"src/rendering/TagRenderer.hpp",
				"src\\rendering\\TagRenderer.cpp",
				"src\\rendering\\TagRenderer.hpp",
				"#include \"rendering/TagRenderer.hpp\"",
				"namespace VsmrTagRendering",
				"class FontContext final"
			},
			{
				"src/rendering/TargetSymbolRenderer.cpp",
				"src/rendering/TargetSymbolRenderer.hpp",
				"src\\rendering\\TargetSymbolRenderer.cpp",
				"src\\rendering\\TargetSymbolRenderer.hpp",
				"#include \"rendering/TargetSymbolRenderer.hpp\"",
				"namespace VsmrTargetRendering",
				"class Frame final"
			}
		} };

		const std::filesystem::path projectRoot = repositoryRoot / "vSMR";
		const std::string project = ReadTextFile(projectRoot / "vSMR.vcxproj");
		const std::string filters = ReadTextFile(projectRoot / "vSMR.vcxproj.filters");
		for (const SharedSourcePair& sharedSource : sharedSources)
		{
			const std::filesystem::path sourcePath = projectRoot / sharedSource.sourcePath;
			const std::filesystem::path headerPath = projectRoot / sharedSource.headerPath;
			const std::string componentName = headerPath.stem().string();
			const bool sourceExists = std::filesystem::is_regular_file(sourcePath);
			const bool headerExists = std::filesystem::is_regular_file(headerPath);
			Expect(sourceExists, componentName + " implementation source exists");
			Expect(headerExists, componentName + " declaration header exists");

			if (sourceExists)
			{
				const std::string source = ReadTextFile(sourcePath);
				Expect(
					source.find(sharedSource.headerInclude) != std::string::npos,
					componentName + " implementation includes its declaration header");
				Expect(
					source.find(sharedSource.implementationMarker) != std::string::npos,
					componentName + " implementation exposes its shared boundary");
			}
			if (headerExists)
			{
				const std::string header = ReadTextFile(headerPath);
				Expect(
					header.find(sharedSource.declarationMarker) != std::string::npos,
					componentName + " header declares its shared boundary");
			}

			const std::string compileEntry =
				std::string("<ClCompile Include=\"") + sharedSource.projectSourcePath + "\"";
			const std::string includeEntry =
				std::string("<ClInclude Include=\"") + sharedSource.projectHeaderPath + "\"";
			Expect(
				project.find(compileEntry) != std::string::npos,
				componentName + " implementation is compiled by the main project");
			Expect(
				project.find(includeEntry) != std::string::npos,
				componentName + " header is registered by the main project");
			Expect(
				filters.find(compileEntry + "><Filter>Source</Filter>") != std::string::npos,
				componentName + " implementation uses the Source project filter");
			Expect(
				filters.find(includeEntry + "><Filter>Headers</Filter>") != std::string::npos,
				componentName + " header uses the Headers project filter");
		}
	}


}

int wmain(int argc, wchar_t** argv)
{
	const std::filesystem::path repositoryRoot = argc > 1
		? std::filesystem::path(argv[1])
		: std::filesystem::current_path();

	TestGroundState();
	TestSharedGroundState();
	TestRapidJsonUtilities();
	TestJsonInputLimitBoundaries();
	TestHoldingPointRemarks();
	TestTagTokens();
	TestVsidBridgeData();
	TestRecentAirports();
	TestCdmBridgeData();
	TestRimcasRules();
	TestTargetRoleThresholds();
	TestWeatherParsing();
	TestRuntimeReleaseLifecycle();
	for (const std::string& failure : RunAvisoRasterPipelineTests())
		Expect(false, failure);
	for (const std::string& failure : RunSharedRenderingBehaviorTests())
		Expect(false, failure);
	for (const std::string& failure : RunTagColorRuleTests())
		Expect(false, failure);
	for (const std::string& failure : RunUpdaterUrlPolicyTests())
		Expect(false, failure);
	for (const auto& failure : RunAuditRegressionTests())
		Expect(false, failure);
	for (const std::string& failure : RunPluginBridgeTests())
		Expect(false, failure);
	TestGeometry();
	TestWebMessageValidation();
	TestControlCenterMessageProtocol();
	TestControlCenterScriptLayout(repositoryRoot);
	TestSharedRenderingProjectIntegration(repositoryRoot);
	for (const std::string& failure : RunConfigurationRegressionTests(repositoryRoot))
		Expect(false, failure);

	if (FailureCount != 0)
	{
		std::cerr << FailureCount << " regression test(s) failed\n";
		return 1;
	}

	std::cout << "All vSMR regression tests passed\n";
	return 0;
}
