#include <Windows.h>
#include <objidl.h>

#include "aircraft/GroundState.hpp"
#include "aircraft/HoldingPoint.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "bootstrap/loader/RuntimeReleaseState.hpp"
#include "config/RuntimeConfig.hpp"
#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "control_center/WebMessageValidation.hpp"
#include "radar/RadarGeometry.hpp"
#include "safety/RimcasLogic.hpp"
#include "scene/TargetRoleLogic.hpp"
#include "tags/TagDefinitionUtils.hpp"

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

	void TestGroundState()
	{
		Expect(classifyGroundState("ST-UP", 0, false) == GroundStateCategory::Stup, "ground state startup alias");
		Expect(classifyGroundState("P/B", 3, false) == GroundStateCategory::Push, "ground state push alias");
		Expect(classifyGroundState("LINE UP", 0, true) == GroundStateCategory::Lnup, "ground state lineup alias");
		Expect(classifyGroundState("", 0, false) == GroundStateCategory::Gate, "stationary empty state defaults to gate");
		Expect(classifyGroundState("", 2, false) == GroundStateCategory::Unknown, "moving empty state stays unknown");
		Expect(!shouldDisplayTagInTowerMode("STUP", 0, false), "tower mode hides startup");
		Expect(shouldDisplayTagInTowerMode("TAXI", 8, false), "tower mode shows taxi");
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

	void TestRimcasRules()
	{
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
		const std::array<const char*, 11> sources = {
			"app-model.js",
			"app-workflow.js",
			"app-runtime.js",
			"app-profile-colors.js",
			"app-profile-editor.js",
			"app-aviso-editor.js",
			"app-settings.js",
			"app-persistence.js",
			"app-events.js",
			"app-actions.js",
			"app.js"
		};
		const std::array<const char*, 11> styleSources = {
			"base-theme.css",
			"shell-navigation.css",
			"aviso-editor.css",
			"editor-controls.css",
			"navigation-settings.css",
			"page-workspaces.css",
			"component-system.css",
			"visual-corrections.css",
			"shared-theme.css",
			"editor-workflows.css",
			"responsive-polish.css"
		};
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
		for (const char* sourceName : styleSources)
		{
			const std::filesystem::path path = webRoot / "styles" / sourceName;
			Expect(
				std::filesystem::is_regular_file(path),
				std::string("Control Center style source exists: ") + sourceName);
			const std::string marker = std::string("/* source: ") + sourceName + " */";
			const std::size_t position = styleBundle.find(marker);
			Expect(
				position != std::string::npos,
				std::string("Control Center stylesheet includes: ") + sourceName);
			if (position != std::string::npos)
			{
				Expect(
					!foundPreviousSource || position > previousSourcePosition,
					std::string("Control Center style source order is stable at: ") + sourceName);
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
				std::string("Control Center style source stays reviewable: ") + sourceName);
		}
	}

	void TestProfiles(const std::filesystem::path& repositoryRoot)
	{
		const std::filesystem::path profilePath = repositoryRoot / "vSMR" / "data" / "vSMR_Profiles.json";
		const std::string profileJson = ReadTextFile(profilePath);
		Expect(!profileJson.empty(), "default profiles file is readable");
		std::string profileInputError;
		Expect(
			CConfig::validateSerializedInputLimits(profileJson, profileInputError),
			"default profiles pass pre-DOM input limits");
		std::string deeplyNestedProfile(65U, '[');
		deeplyNestedProfile += '0';
		deeplyNestedProfile.append(65U, ']');
		Expect(
			!CConfig::validateSerializedInputLimits(
				deeplyNestedProfile,
				profileInputError),
			"profile imports reject excessive nesting before DOM parsing");

		rapidjson::Document profiles;
		profiles.Parse<0>(profileJson.c_str());
		Expect(!profiles.HasParseError(), "default profiles JSON parses");
		if (profiles.HasParseError())
			return;

		bool migrated = false;
		std::string error;
		Expect(CConfig::validateAndMigrateProfilesDocument(profiles, error, migrated), "default profiles validate");

		rapidjson::StringBuffer serialized;
		rapidjson::Writer<rapidjson::StringBuffer> writer(serialized);
		profiles.Accept(writer);
		rapidjson::Document roundTrip;
		roundTrip.Parse<0>(serialized.GetString());
		bool roundTripMigrated = false;
		std::string roundTripError;
		Expect(
			!roundTrip.HasParseError() && CConfig::validateAndMigrateProfilesDocument(roundTrip, roundTripError, roundTripMigrated),
			"profiles survive validation round trip");

		rapidjson::Document duplicates;
		duplicates.Parse<0>(R"json([{"name":"Alpha"},{"name":"alpha"}])json");
		bool duplicateMigrated = false;
		std::string duplicateError;
		Expect(!CConfig::validateAndMigrateProfilesDocument(duplicates, duplicateError, duplicateMigrated), "duplicate profile names fail closed");

		rapidjson::Document tooManyProfiles;
		tooManyProfiles.SetArray();
		for (std::size_t index = 0; index < 257U; ++index)
		{
			rapidjson::Value profile(rapidjson::kObjectType);
			const std::string name = "Profile " + std::to_string(index);
			rapidjson::Value profileName;
			profileName.SetString(
				name.c_str(),
				static_cast<rapidjson::SizeType>(name.size()),
				tooManyProfiles.GetAllocator());
			profile.AddMember("name", profileName, tooManyProfiles.GetAllocator());
			tooManyProfiles.PushBack(profile, tooManyProfiles.GetAllocator());
		}
		bool tooManyProfilesMigrated = false;
		std::string tooManyProfilesError;
		Expect(
			!CConfig::validateAndMigrateProfilesDocument(
				tooManyProfiles,
				tooManyProfilesError,
				tooManyProfilesMigrated),
			"profile-count limit fails closed");

		rapidjson::Document oversized;
		oversized.SetArray();
		rapidjson::Value oversizedProfile(rapidjson::kObjectType);
		rapidjson::Value oversizedName;
		oversizedName.SetString("Default", oversized.GetAllocator());
		oversizedProfile.AddMember("name", oversizedName, oversized.GetAllocator());
		const std::string oversizedText(64U * 1024U + 1U, 'x');
		rapidjson::Value oversizedValue;
		oversizedValue.SetString(
			oversizedText.c_str(),
			static_cast<rapidjson::SizeType>(oversizedText.size()),
			oversized.GetAllocator());
		oversizedProfile.AddMember("oversized", oversizedValue, oversized.GetAllocator());
		oversized.PushBack(oversizedProfile, oversized.GetAllocator());
		bool oversizedMigrated = false;
		std::string oversizedError;
		Expect(!CConfig::validateAndMigrateProfilesDocument(oversized, oversizedError, oversizedMigrated), "oversized profile strings fail closed");

		CConfig liveConfig(profilePath.u8string(), "");
		const std::string activeBefore = liveConfig.getActiveProfileName();
		const std::size_t countBefore = liveConfig.getProfileCount();
		rapidjson::Document invalidReplacement;
		invalidReplacement.Parse<0>(R"json([{"name":""}])json");
		std::string replacementError;
		Expect(!liveConfig.replaceInMemoryConfig(invalidReplacement, activeBefore, replacementError), "invalid profile replacement is rejected");
		Expect(liveConfig.getActiveProfileName() == activeBefore && liveConfig.getProfileCount() == countBefore, "failed profile replacement preserves live state");
	}

	void TestAviso(const std::filesystem::path& repositoryRoot)
	{
		const std::filesystem::path avisoRoot = repositoryRoot / "vSMR" / "data" / "AVISO";
		for (const char* airport : { "LFPG.geojson", "LFML.geojson", "LFMN.geojson", "LFBO.geojson" })
		{
			AvisoDocumentModel model;
			std::string error;
			const std::filesystem::path path = avisoRoot / airport;
			const std::string sourceJson = ReadTextFile(path);
			Expect(
				AvisoDocumentModel::ValidateSerializedInputLimits(sourceJson, error),
				std::string("AVISO passes pre-DOM input limits: ") + airport);
			Expect(model.LoadFromFile(path.u8string(), error), std::string("AVISO validates: ") + airport + (error.empty() ? "" : " (" + error + ")"));
			Expect(model.FeatureCount() > 0, std::string("AVISO has features: ") + airport);
		}
		std::string deeplyNestedAviso(65U, '[');
		deeplyNestedAviso += '0';
		deeplyNestedAviso.append(65U, ']');
		std::string avisoInputError;
		Expect(
			!AvisoDocumentModel::ValidateSerializedInputLimits(
				deeplyNestedAviso,
				avisoInputError),
			"AVISO imports reject excessive nesting before DOM parsing");

		AvisoDocumentModel invalid;
		invalid.MutableDocument().Parse<0>(
			R"json({"type":"FeatureCollection","features":[{"type":"Feature","id":"dup","geometry":{"type":"Point","coordinates":[2.0,48.0]},"properties":{}},{"type":"Feature","id":"dup","geometry":{"type":"Point","coordinates":[2.1,48.1]},"properties":{}}]})json");
		std::string validationError;
		Expect(!invalid.ValidateLoadedFeatureCollection(validationError), "duplicate AVISO feature IDs fail closed");

		AvisoDocumentModel oversized;
		oversized.ResetToEmpty();
		rapidjson::Document& oversizedDocument = oversized.MutableDocument();
		const std::string oversizedText(64U * 1024U + 1U, 'x');
		rapidjson::Value oversizedValue;
		oversizedValue.SetString(
			oversizedText.c_str(),
			static_cast<rapidjson::SizeType>(oversizedText.size()),
			oversizedDocument.GetAllocator());
		oversizedDocument.AddMember("oversized", oversizedValue, oversizedDocument.GetAllocator());
		std::string oversizedError;
		Expect(!oversized.ValidateLoadedFeatureCollection(oversizedError), "oversized AVISO strings fail closed");

		const std::filesystem::path featureLimitPath =
			std::filesystem::temp_directory_path() /
			(L"vSMR_feature_limit_" +
				std::to_wstring(::GetCurrentProcessId()) + L"_" +
				std::to_wstring(::GetTickCount64()) + L".geojson");
		std::string featureLimitJson =
			R"json({"type":"FeatureCollection","features":[)json";
		featureLimitJson.reserve(featureLimitJson.size() + 150010U);
		for (std::size_t index = 0; index < 50001U; ++index)
		{
			if (index != 0U)
				featureLimitJson.push_back(',');
			featureLimitJson += "{}";
		}
		featureLimitJson += "]}";
		{
			std::ofstream output(featureLimitPath, std::ios::binary | std::ios::trunc);
			output.write(
				featureLimitJson.data(),
				static_cast<std::streamsize>(featureLimitJson.size()));
		}
		std::string boundedSource;
		std::string featureLimitError;
		Expect(
			!AvisoDocumentModel::ReadBoundedSourceFile(
				featureLimitPath,
				boundedSource,
				featureLimitError) &&
				featureLimitError.find("feature") != std::string::npos,
			"AVISO feature limit is enforced before DOM construction");
		std::error_code cleanupError;
		std::filesystem::remove(featureLimitPath, cleanupError);
	}

	void TestUnicodeResourcePaths(const std::filesystem::path& repositoryRoot)
	{
		const std::filesystem::path temporaryRoot =
			std::filesystem::temp_directory_path();
		const std::filesystem::path testRoot =
			temporaryRoot /
			(L"vSMR_unicode_\u00E9_\u5F00\u53D1_" +
				std::to_wstring(::GetCurrentProcessId()) + L"_" +
				std::to_wstring(::GetTickCount64()));
		std::error_code safetyError;
		const bool safeTestRoot =
			std::filesystem::equivalent(
				testRoot.parent_path(),
				temporaryRoot,
				safetyError) &&
			!safetyError &&
			testRoot.filename().wstring().find(L"vSMR_unicode_") == 0U;
		Expect(safeTestRoot, "Unicode resource test path stays below the temporary folder");
		if (!safeTestRoot)
			return;

		std::error_code errorCode;
		std::filesystem::create_directories(testRoot, errorCode);
		Expect(!errorCode, "Unicode resource test directory is created");
		if (errorCode)
			return;

		const std::filesystem::path selectedFile =
			testRoot / L"profils_\u00E9_\u6D4B\u8BD5.json";
		std::filesystem::copy_file(
			repositoryRoot / "vSMR" / "data" / "vSMR_Profiles.json",
			selectedFile,
			std::filesystem::copy_options::overwrite_existing,
			errorCode);
		Expect(!errorCode, "Profiles fixture copies to a Unicode path");

		const std::wstring widePickerResult = selectedFile.wstring();
		const std::filesystem::path selectedFromWidePicker(widePickerResult);
		Expect(
			selectedFromWidePicker.u8string() == selectedFile.u8string(),
			"Wide file-picker path crosses the UTF-8 boundary without ACP loss");

		std::string normalizedPath;
		std::string error;
		Expect(
			VsmrResourceFiles::NormalizeExistingFilePath(
				selectedFromWidePicker.u8string(),
				normalizedPath,
				error),
			"Unicode selected-resource path normalizes");
		Expect(
			!normalizedPath.empty() &&
				std::filesystem::equivalent(
					std::filesystem::u8path(normalizedPath),
					selectedFile,
					errorCode),
			"Normalized resource path preserves Unicode");

		CConfig unicodeConfig(selectedFile.u8string(), "");
		Expect(
			unicodeConfig.isConfigHealthy() && unicodeConfig.getProfileCount() > 0U,
			"Profiles load from a Unicode installation path");
		Expect(
			unicodeConfig.saveConfig(
				{},
				unicodeConfig.getPersistedConfigRevision(),
				&error),
			"Profiles save atomically without a backup rotation");
		std::filesystem::path legacyProfilesBackup = selectedFile;
		legacyProfilesBackup += L".bak";
		Expect(
			!std::filesystem::exists(legacyProfilesBackup),
			"Profiles save does not create a .bak file");
		const std::string legacyProfilesContents = ReadTextFile(selectedFile);
		{
			std::ofstream backupOutput(legacyProfilesBackup, std::ios::binary | std::ios::trunc);
			backupOutput << legacyProfilesContents;
		}
		Expect(
			unicodeConfig.saveConfig(
				{},
				unicodeConfig.getPersistedConfigRevision(),
				&error) &&
				ReadTextFile(legacyProfilesBackup) == legacyProfilesContents,
			"Profiles save leaves an existing .bak file untouched");
		Expect(
			unicodeConfig.isBackupAvailable() &&
				unicodeConfig.getBackupModifiedUnixSeconds() > 0,
			"Validated legacy profiles backup exposes its modification date");

		const std::filesystem::path unicodeAvisoPath =
			testRoot / L"a\u00E9roport_\u6D4B\u8BD5.geojson";
		errorCode.clear();
		std::filesystem::copy_file(
			repositoryRoot / "vSMR" / "data" / "AVISO" / "LFMN.geojson",
			unicodeAvisoPath,
			std::filesystem::copy_options::overwrite_existing,
			errorCode);
		Expect(!errorCode, "AVISO fixture copies to a Unicode path");
		AvisoDocumentModel unicodeAviso;
		std::string avisoError;
		Expect(
			unicodeAviso.LoadFromFile(unicodeAvisoPath.u8string(), avisoError) &&
				unicodeAviso.FeatureCount() > 0U,
			"AVISO loads from a Unicode installation path");
		Expect(
			unicodeAviso.SaveAtomically(unicodeAvisoPath.u8string(), avisoError),
			"AVISO saves atomically without a backup rotation");
		std::filesystem::path legacyAvisoBackup = unicodeAvisoPath;
		legacyAvisoBackup += L".bak";
		Expect(
			!std::filesystem::exists(legacyAvisoBackup),
			"AVISO save does not create a .bak file");
		{
			std::ofstream backupOutput(legacyAvisoBackup, std::ios::binary | std::ios::trunc);
			backupOutput << "legacy AVISO backup";
		}
		Expect(
			unicodeAviso.SaveAtomically(unicodeAvisoPath.u8string(), avisoError) &&
				ReadTextFile(legacyAvisoBackup) == "legacy AVISO backup",
			"AVISO save leaves an existing .bak file untouched");

		std::string storedPath;
		error.clear();
		Expect(
			VsmrResourceFiles::StoreGithubDownload(
				VsmrResourceFiles::Kind::Profiles,
				testRoot.u8string(),
				"https://example.invalid/vSMR_Profiles.json",
				"LFPG",
				"[]",
				storedPath,
				error),
			"GitHub resource stores below a Unicode data path");
		Expect(
			!storedPath.empty() &&
				std::filesystem::is_regular_file(std::filesystem::u8path(storedPath)),
			"Stored resource path round-trips as UTF-8");

		errorCode.clear();
		std::filesystem::remove_all(testRoot, errorCode);
		Expect(!errorCode, "Unicode resource test directory is removed");
	}
}

int wmain(int argc, wchar_t** argv)
{
	const std::filesystem::path repositoryRoot = argc > 1
		? std::filesystem::path(argv[1])
		: std::filesystem::current_path();

	TestGroundState();
	TestHoldingPointRemarks();
	TestTagTokens();
	TestRimcasRules();
	TestTargetRoleThresholds();
	TestRuntimeReleaseLifecycle();
	TestGeometry();
	TestWebMessageValidation();
	TestControlCenterMessageProtocol();
	TestControlCenterScriptLayout(repositoryRoot);
	TestProfiles(repositoryRoot);
	TestAviso(repositoryRoot);
	TestUnicodeResourcePaths(repositoryRoot);

	if (FailureCount != 0)
	{
		std::cerr << FailureCount << " regression test(s) failed\n";
		return 1;
	}

	std::cout << "All vSMR regression tests passed\n";
	return 0;
}
