#include <Windows.h>
#include <objidl.h>

#include "ConfigurationRegressionTests.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoBundledGroups.hpp"
#include "aviso/AvisoPolygonOutline.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/ProfileNormalization.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "datalink/PdcTsatPrefill.hpp"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace
{
	std::vector<std::string> Failures;

	void Expect(bool condition, const std::string& name)
	{
		if (!condition)
			Failures.push_back(name);
	}

	std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

	void TestProfiles(const std::filesystem::path& repositoryRoot)
	{
		const auto matchesLfpgDetailedRows = [](const rapidjson::Value& profile)
		{
			if (!profile.IsObject() || !profile.HasMember("labels") ||
				!profile["labels"].IsObject() || !profile["labels"].HasMember("departure"))
				return false;
			const auto& departure = profile["labels"]["departure"];
			const auto matchesRows = [](const rapidjson::Value& rows)
			{
				const auto isToken = [](const rapidjson::Value& value, const char* expected)
				{
					return value.IsString() && std::string(value.GetString()) == expected;
				};
				return rows.IsArray() && rows.Size() == 3U &&
					rows[0].IsArray() && rows[0].Size() == 1U && isToken(rows[0][0], "b:callsign") &&
					rows[1].IsArray() && rows[1].Size() == 2U && isToken(rows[1][0], "actype") && isToken(rows[1][1], "deprwy") &&
					rows[2].IsArray() && rows[2].Size() == 2U && isToken(rows[2][0], "ready_startup") && isToken(rows[2][1], "scratchpad");
			};
			return departure.IsObject() && departure.HasMember("definition_detailed") &&
				departure.HasMember("status_definitions") &&
				departure["status_definitions"].IsObject() &&
				departure["status_definitions"].HasMember("stup") &&
				departure["status_definitions"]["stup"].IsObject() &&
				departure["status_definitions"]["stup"].HasMember("definition_detailed") &&
				matchesRows(departure["definition_detailed"]) &&
				matchesRows(departure["status_definitions"]["stup"]["definition_detailed"]);
		};
		const auto matchesLfmnArrivalStand = [](const rapidjson::Value& profile)
		{
			if (!profile.IsObject() || !profile.HasMember("labels") || !profile["labels"].IsObject() ||
				!profile["labels"].HasMember("arrival") || !profile["labels"]["arrival"].IsObject())
				return false;
			const auto& arrival = profile["labels"]["arrival"];
			for (const char* key : { "definition", "definition_detailed" })
			{
				if (!arrival.HasMember(key) || !arrival[key].IsArray()) return false;
				bool found = false;
				for (const auto& row : arrival[key].GetArray())
					if (row.IsArray()) for (const auto& token : row.GetArray())
						found = found || (token.IsString() && std::string(token.GetString()) == "uk_stand");
				if (!found) return false;
			}
			return true;
		};
		const std::filesystem::path profilePath = repositoryRoot / "vSMR" / "data" / "profile_templates.json";
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

		bool lfboProfileFound = false;
		bool lfpgProfileFound = false;
		bool lfmnProfileFound = false;
		if (profiles.IsArray()) for (const auto& original : profiles.GetArray())
		{
			if (original.HasMember("name") && std::string(original["name"].GetString()) == "LFBO / LFLL")
			{
				lfboProfileFound = true;
				Expect(std::string(original["targets"]["icon_style"].GetString()) == "diamond" &&
					original["targets"]["symbol_scale"].GetDouble() == 0.6 &&
					!original["targets"]["trail_enabled"].GetBool(), "LFBO uses compact, trail-free white targets");
				Expect(original["rules"]["items"].Empty(), "LFBO does not inherit LFPG CDM text-colour overrides");
				const auto& labels = original["labels"];
				Expect(labels["departure"]["text_on_ground_color"]["b"].GetInt() == 207 &&
					labels["arrival"]["text_on_ground_color"]["r"].GetInt() == 205,
					"LFBO has blue departure and mauve arrival labels");
				for (const char* family : { "departure", "arrival" })
				{
					const auto& lines = labels[family]["definition"];
					const bool twoLines = lines.IsArray() && lines.Size() == 2U &&
						lines[0].IsArray() && lines[0].Size() == 1U && lines[0][0].IsString() &&
						lines[1].IsArray() && lines[1].Size() == 1U && lines[1][0].IsString();
					Expect(twoLines, "LFBO ground tags contain two valid token rows");
					if (twoLines) Expect(std::string(lines[0][0].GetString()) == "callsign" &&
						std::string(lines[1][0].GetString()) == "actype", "LFBO ground tags show callsign above aircraft type");
				}
			}
			if (original.HasMember("name"))
			{
				const std::string name = original["name"].GetString();
				Expect(name != "LFLL" && name.find("Custom LF") != 0,
					"No standalone LFLL or Custom-prefixed airport profiles remain");
				lfpgProfileFound = lfpgProfileFound || name == "LFPG";
				if (name == "LFPG")
					Expect(matchesLfpgDetailedRows(original), "LFPG template shows Ready Startup in Default and Startup detailed tags");
				lfmnProfileFound = lfmnProfileFound || name == "LFMN";
				if (name == "LFMN")
					Expect(matchesLfmnArrivalStand(original), "LFMN arrival tags display the Ramp Agent stand");
			}
			rapidjson::Document normalized;
			normalized.CopyFrom(original, normalized.GetAllocator());
			VsmrProfile::Normalize(normalized, normalized.GetAllocator());
			rapidjson::Document snapshot;
			snapshot.CopyFrom(normalized, snapshot.GetAllocator());
			Expect(!VsmrProfile::Normalize(normalized, normalized.GetAllocator()) && normalized == snapshot,
				"bundled profile normalization is idempotent");
		}
		Expect(lfboProfileFound, "LFBO / LFLL retains the LFBO profile configuration");
		Expect(lfpgProfileFound && lfmnProfileFound, "Airport profiles use LFPG and LFMN names");
		const std::string managedProfileJson = ReadTextFile(repositoryRoot / "vSMR" / "data" / "default.json");
		rapidjson::Document managedProfiles;
		managedProfiles.Parse<0>(managedProfileJson.c_str());
		Expect(!managedProfiles.HasParseError() && managedProfiles.IsObject() &&
			managedProfiles.HasMember("profiles") && managedProfiles["profiles"].IsObject() &&
			managedProfiles["profiles"].HasMember("builtin-lfpg") &&
			matchesLfpgDetailedRows(managedProfiles["profiles"]["builtin-lfpg"]),
			"Managed LFPG profile matches the Ready Startup template");
		Expect(!managedProfiles.HasParseError() && managedProfiles.IsObject() &&
			managedProfiles.HasMember("profiles") && managedProfiles["profiles"].IsObject() &&
			managedProfiles["profiles"].HasMember("builtin-lfmn") &&
			matchesLfmnArrivalStand(managedProfiles["profiles"]["builtin-lfmn"]),
			"Managed LFMN profile displays the arrival stand");
		const rapidjson::Value* pdc = !managedProfiles.HasParseError() && managedProfiles.IsObject() &&
			managedProfiles.HasMember("pdc") ? &managedProfiles["pdc"] : nullptr;
		const bool sixAirports = pdc != nullptr && pdc->IsObject() &&
			pdc->HasMember("tsat_prefill_airports") &&
			(*pdc)["tsat_prefill_airports"].IsArray() &&
			(*pdc)["tsat_prefill_airports"].Size() == 6U;
		Expect(sixAirports, "Managed PDC defaults list six TSAT prefill airports");
		for (const char* airport : { "LFPG", "LFPO", "LFML", "LFMN", "LFBO", "LFLL" })
			Expect(VsmrPdcTsatPrefill::Allowed(pdc, airport),
				std::string("TSAT prefill is enabled for ") + airport);
		Expect(VsmrPdcTsatPrefill::Allowed(pdc, "lfpg") &&
			!VsmrPdcTsatPrefill::Allowed(pdc, "EGKK") &&
			!VsmrPdcTsatPrefill::Allowed(nullptr, "LFPG"),
			"PDC TSAT prefill follows configured departure airports only");

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

		// Exercise the legacy array adapter using an owned fixture. The shipped
		// source now has a managed default.json sibling, whose real startup path
		// intentionally creates sparse user config.json during first migration.
		const auto replacementTestRoot = std::filesystem::temp_directory_path() /
			("vsmr-profile-replacement-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
		const bool fixtureCreated = std::filesystem::create_directory(replacementTestRoot);
		Expect(fixtureCreated, "profile replacement fixture directory created");
		if (!fixtureCreated) return;
		const auto replacementFixture = replacementTestRoot / "profiles.json";
		{
			std::ofstream output(replacementFixture, std::ios::binary);
			output << profileJson;
		}
		CConfig liveConfig(replacementFixture.u8string(), "");
		const std::string activeBefore = liveConfig.getActiveProfileName();
		const std::size_t countBefore = liveConfig.getProfileCount();
		rapidjson::Document invalidReplacement;
		invalidReplacement.Parse<0>(R"json([{"name":""}])json");
		std::string replacementError;
		Expect(!liveConfig.replaceInMemoryConfig(invalidReplacement, activeBefore, replacementError), "invalid profile replacement is rejected");
		Expect(liveConfig.getActiveProfileName() == activeBefore && liveConfig.getProfileCount() == countBefore, "failed profile replacement preserves live state");
		std::error_code cleanupError;
		std::filesystem::remove_all(replacementTestRoot, cleanupError);
	}

	void TestIndependentProfileSelections()
	{
		const auto testRoot = std::filesystem::temp_directory_path() /
			("vsmr-asr-profiles-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
		std::filesystem::create_directories(testRoot);
		const auto path = testRoot / "profiles.json";
		{
			std::ofstream output(path);
			output << R"json([{"name":"Custom LFPG"},{"name":"Default"},{"name":"Local"},{"_vsmr":{"schema_version":1,"last_active_profile":"Custom LFPG"}}])json";
		}
		{
			CConfig pg(path.u8string(), "");
			CConfig po(path.u8string(), "");
			const auto before = ReadTextFile(path);
			pg.setActiveProfile("Custom LFPG");
			po.setActiveProfile("Default");
			Expect(pg.getActiveProfileName() == "Custom LFPG" && po.getActiveProfileName() == "Default",
				"Two ASRs sharing one profiles file keep different selections");
			Expect(ReadTextFile(path) == before, "Selecting profiles does not write shared configuration metadata");
			const std::string pgAsrSelection = pg.getActiveProfileName();
			const std::string poAsrSelection = po.getActiveProfileName();
			po.setActiveProfile("Local");
			Expect(pg.getActiveProfileName() == pgAsrSelection, "A second ASR cannot change the first selection");
			Expect(po.saveConfig() && pg.reload(), "Shared definition edits can be saved and reloaded");
			Expect(pg.getActiveProfileName() == pgAsrSelection && po.getActiveProfileName() == "Local",
				"Reloading shared definitions preserves each ASR selection");
			CConfig reopenedPg(path.u8string(), "");
			CConfig reopenedPo(path.u8string(), "");
			reopenedPo.setActiveProfile(poAsrSelection);
			reopenedPg.setActiveProfile(pgAsrSelection);
			Expect(reopenedPg.getActiveProfileName() == pgAsrSelection && reopenedPo.getActiveProfileName() == poAsrSelection,
				"ASRs reopen independently of load order and legacy last-active metadata");
			for (const char* missing : { "", "Deleted profile" })
			{
				reopenedPo.setActiveProfile(missing);
				Expect(reopenedPo.getActiveProfileName() == "Default", "Missing ASR profiles fall back to Default before Custom LFPG");
			}
			rapidjson::Document replacement;
			replacement.Parse<0>(R"json([{"name":"Default"},{"name":"Custom LFPG"}])json");
			std::string error;
			Expect(pg.replaceInMemoryConfig(replacement, pg.getActiveProfileName(), error) &&
				po.replaceInMemoryConfig(replacement, po.getActiveProfileName(), error),
				"A shared source replacement accepts each screen's requested profile");
			Expect(pg.getActiveProfileName() == "Custom LFPG" && po.getActiveProfileName() == "Default",
				"Source replacement preserves existing selections and falls back only for removed profiles");
		}
		std::filesystem::remove_all(testRoot);
	}

	void TestAviso(const std::filesystem::path& repositoryRoot)
	{
		rapidjson::Document backgroundMetadata;
		backgroundMetadata.Parse(R"({"background_opacities":{"dark":0.25,"real":0}})");
		Expect(AvisoDocumentModel::BackgroundOpacity(&backgroundMetadata, "dark") == 0.25 &&
			AvisoDocumentModel::BackgroundOpacity(&backgroundMetadata, "real") == 0 &&
			AvisoDocumentModel::BackgroundOpacity(&backgroundMetadata, "light") == 1,
			"Background opacity is palette-specific, including zero and opaque legacy defaults");
		for (const char* invalid : { R"({"dark":-0.1})", R"({"real":1.1})", R"({"light":"0.5"})", R"({"unknown":0.5})" })
		{
			AvisoDocumentModel invalidBackground;
			const std::string json = std::string(R"({"type":"FeatureCollection","features":[],"metadata":{"background_opacities":)") + invalid + "}}";
			invalidBackground.MutableDocument().Parse(json.c_str());
			std::string error;
			Expect(!invalidBackground.ValidateLoadedFeatureCollection(error), "Malformed background opacity is rejected before rendering");
		}
		rapidjson::Document outlineStyle;
		outlineStyle.Parse<0>(R"json({"paint":{"fill":"#434C51","palette-overrides":{"real":{"polygon-outline":true,"stroke":"#727C82"}}},"feature":{"polygon-outline":false},"invalid":{"polygon-outline":"true"}})json");
		const auto* outlinePaint = &outlineStyle["paint"];
		const auto* real = &(*outlinePaint)["palette-overrides"]["real"];
		Expect(!VsmrAviso::ResolvePolygonOutline(nullptr, nullptr, nullptr, nullptr) &&
			!VsmrAviso::ResolvePolygonOutline(outlinePaint, nullptr, nullptr, nullptr),
			"Existing polygon styles and LFBO Dark/Light remain fill-only");
		Expect(VsmrAviso::ResolvePolygonOutline(outlinePaint, nullptr, real, nullptr),
			"Real can opt into polygon boundaries independently");
		Expect(!VsmrAviso::ResolvePolygonOutline(outlinePaint, &outlineStyle["feature"], real, nullptr) &&
			VsmrAviso::ResolvePolygonOutline(outlinePaint, &outlineStyle["feature"], real, real),
			"Feature base and palette outline overrides follow colour precedence");
		Expect(!VsmrAviso::ResolvePolygonOutline(&outlineStyle["invalid"], nullptr, nullptr, nullptr),
			"Non-boolean polygon outline flags do not enable outlines");
		const std::filesystem::path avisoRoot = repositoryRoot / "vSMR" / "data" / "AVISO";
		for (const auto& entry : std::filesystem::directory_iterator(avisoRoot))
		{
			if (entry.path().extension() != ".geojson") continue;
			const std::string airport = entry.path().stem().string();
			AvisoDocumentModel model;
			std::string error;
			const std::string sourceJson = ReadTextFile(entry.path());
			Expect(AvisoDocumentModel::ValidateSerializedInputLimits(sourceJson, error),
				"AVISO passes pre-DOM input limits: " + airport);
			const bool loaded = model.LoadFromFile(entry.path().u8string(), error);
			Expect(loaded, "AVISO validates: " + airport + " " + error);
			if (!loaded) continue;
			Expect(model.FeatureCount() > 0, "AVISO has features: " + airport);
			const auto& document = model.GetDocument();
			if (!document.HasMember("metadata") ||
				(airport != "LFPG" && !document["metadata"].HasMember("geometry_source"))) continue;
			const auto& metadata = document["metadata"];
			const bool hasReal = airport == "LFPG" || airport == "LFPO" || airport == "LFPO_Work" || airport == "LFML" || airport == "LFMN" || airport == "LFBO" || airport == "LFLL" || airport == "LFSB";
			const auto& palettes = metadata["color_palettes"];
			if (airport == "LFPO_Work")
			{
				Expect(model.FeatureCount() == 576U && std::string(metadata["airport"].GetString()) == "LFPO",
					"Orly works variant preserves supplied geometry and operational ICAO LFPO");
				Expect(std::filesystem::exists(avisoRoot / "LFPO.geojson"),
					"Orly works variant does not replace the standard LFPO AVISO");
			}
			Expect(palettes.Size() == (hasReal ? 3U : 2U) &&
				std::string(palettes[rapidjson::SizeType(0)].GetString()) == "dark" &&
				std::string(palettes[1].GetString()) == "light" &&
				(!hasReal || std::string(palettes[2].GetString()) == "real"),
				"Imported AVISO preserves supplied Dark/Light palettes and airport-specific Real colors: " + airport);
			Expect(metadata["background_colors"].HasMember("real") == hasReal,
				"Background palettes agree with available palettes: " + airport);
			for (const auto& feature : document["features"].GetArray())
				Expect(!feature["properties"].HasMember("color_palettes"),
					"Every palette uses the same sector-pack geometry: " + airport);
			if (airport == "LFSB")
			{
				const auto& styles = document["styles"];
				Expect(model.FeatureCount() == 235U && styles.MemberCount() == 13U,
					"LFSB Real retains all existing geometry and shared style IDs");
				Expect(std::string(metadata["background_colors"]["real"].GetString()) == "#252B37",
					"LFSB Real uses the reference dark blue-grey background");
				for (auto style = styles.MemberBegin(); style != styles.MemberEnd(); ++style)
				{
					const auto& paint = style->value["paint"];
					Expect(paint.HasMember("palette-overrides") && paint["palette-overrides"].HasMember("real"),
						"Every LFSB style explicitly defines its Real palette");
					Expect(!VsmrAviso::ResolvePolygonOutline(&paint, nullptr, nullptr, nullptr),
						"LFSB Dark remains fill-only instead of inheriting Real outlines");
				}
				for (const char* styleId : { "polygon.runwayconcrete.555555", "polygon.hardsurface2.595e5b",
					"polygon.hardsurface3.8a807f", "polygon.hardsurface4.969393" })
				{
					const auto& paint = styles[styleId]["paint"];
					Expect(!VsmrAviso::ResolvePolygonOutline(&paint, nullptr, &paint["palette-overrides"]["real"], nullptr),
						"LFSB Real runway/apron polygon joins must not become artificial contour lines");
				}
				const auto& runway = styles["polygon.runwayconcrete.555555"]["paint"];
				Expect(std::string(runway["palette-overrides"]["real"]["fill"].GetString()) == "#62687C" &&
					std::string(runway["fill"].GetString()) == "#111318" &&
					std::string(runway["palette-overrides"]["light"]["fill"].GetString()) == "#555555",
					"LFSB both Real runways use grey-violet, never the red closed-runway reference colour; Dark/Light remain unchanged");
				unsigned runwayParts = 0;
				for (const auto& feature : document["features"].GetArray())
				{
					const auto& properties = feature["properties"];
					if (std::string(properties["style_id"].GetString()) != "polygon.runwayconcrete.555555") continue;
					++runwayParts;
					Expect(!properties.HasMember("fill") && !properties.HasMember("palette-overrides"),
						"All LFSB runway geometry inherits the same Real fill without red feature overrides");
				}
				Expect(runwayParts == 3U, "LFSB preserves the existing three runway polygon parts");
				Expect(std::string(styles["label.taxiways"]["paint"]["palette-overrides"]["real"]["text-color"].GetString()) == "#7EA18B" &&
					std::string(styles["label.gates"]["paint"]["palette-overrides"]["real"]["text-color"].GetString()) == "#B7AE6A",
					"LFSB Real distinguishes green taxiway labels from subdued yellow stands");
			}
			if (airport == "LFLL")
			{
				Expect(model.FeatureCount() == 420U && document["styles"].MemberCount() == 21U,
					"LFLL retains the detailed GNG map with consolidated multiline guidance features");
				Expect(std::string(metadata["background_colors"]["real"].GetString()) == "#50595F",
					"LFLL Real uses the lighter reference slate background");
				const auto& styles = document["styles"];
				for (auto style = styles.MemberBegin(); style != styles.MemberEnd(); ++style)
				{
					const auto& paint = style->value["paint"];
					Expect(paint.HasMember("palette-overrides") && paint["palette-overrides"].HasMember("real"),
						"Every LFLL style explicitly defines its Real colours");
					Expect(!VsmrAviso::ResolvePolygonOutline(&paint, nullptr, nullptr, nullptr),
						"LFLL Dark remains fill-only");
					unsigned actualCount = 0;
					for (const auto& feature : document["features"].GetArray())
						if (std::string(feature["properties"]["style_id"].GetString()) == style->name.GetString()) ++actualCount;
					Expect(style->value["feature_count"].GetUint() == actualCount, "LFLL detail style counts match the imported geometry");
				}
				Expect(styles["polygon.surfacemarking"]["feature_count"].GetUint() == 152U &&
					styles["polygon.building.394446"]["feature_count"].GetUint() == 35U &&
					styles["line.standentry"]["feature_count"].GetUint() == 1U,
					"LFLL includes supplied runway markings, detailed buildings and stand entry lines");
				Expect(styles["polygon.stopbar.7e0000"]["feature_count"].GetUint() == 17U &&
					styles["polygon.closurearea.ff0000"]["feature_count"].GetUint() == 4U &&
					!styles.HasMember("restriction.prohibited_area"),
					"LFLL retains the original stopbar/closure classification instead of importing ambiguous restrictions");
				Expect(!styles.HasMember("label.circuit.rwy17") && !styles.HasMember("label.circuit.rwy35"),
					"LFLL surface map omits overlapping traffic-circuit annotations");
				unsigned distanceLabels = 0;
				for (const auto& feature : document["features"].GetArray())
				{
					const auto& properties = feature["properties"];
					const std::string styleId = properties["style_id"].GetString();
					if (styleId.find("label.tora.") != 0) continue;
					++distanceLabels;
					const auto& groups = properties["vsmr_group_ids"];
					Expect(groups.Size() == 1U && std::string(groups[0].GetString()) == "lfll-distance-labels",
						"LFLL distance labels can be toggled without hiding other airport geometry");
				}
				Expect(distanceLabels == 9U && document["vsmr_groups"].Size() == 1U &&
					document["vsmr_groups"][0]["visible"].GetBool(), "LFLL distance-label group is available and initially visible");
				Expect(document["bbox"][0].GetDouble() > 5.0 && document["bbox"][2].GetDouble() < 5.2 &&
					document["bbox"][1].GetDouble() > 45.6 && document["bbox"][3].GetDouble() < 45.8,
					"LFLL bounds describe the actual airport instead of the stale supplied bbox");
				const auto& gates = styles["label.gates"]["paint"];
				Expect(std::string(gates["text-color"].GetString()) == "#CCCCCC" &&
					std::string(gates["palette-overrides"]["real"]["text-color"].GetString()) == "#64CDD0",
					"LFLL stand labels become cyan only in Real");
				const auto& runway = styles["polygon.runwayconcrete.555555"]["paint"];
				const auto& runwayReal = runway["palette-overrides"]["real"];
				Expect(std::string(runwayReal["fill"].GetString()) == "#818C92" &&
					VsmrAviso::ResolvePolygonOutline(&runway, nullptr, &runwayReal, nullptr),
					"LFLL Real uses light runway surfaces with fine outlines");
				Expect(std::string(styles["polygon.closurearea.ff0000"]["paint"]["palette-overrides"]["real"]["fill"].GetString()) == "#FF0000",
					"LFLL Real preserves visible closure warnings");
			}
			if (airport == "LFBO")
			{
				Expect(model.FeatureCount() == 471U && document["styles"].MemberCount() == 17U,
					"LFBO retains its 471 features with three reference-coloured gate styles");
				Expect(std::string(metadata["background_colors"]["real"].GetString()) == "#434C51",
					"LFBO Real uses the reference slate background");
				const auto& styles = document["styles"];
				const auto& grass = styles["polygon.grassurface.00512f"]["paint"];
				const std::string realGrass = grass["palette-overrides"]["real"]["fill"].GetString();
				const auto& realGrassPaint = grass["palette-overrides"]["real"];
				Expect(realGrass == "#434C51" && realGrass == metadata["background_colors"]["real"].GetString(),
					"LFBO Real restores the screenshot-reference uniform slate fill instead of dark grass patches");
				Expect(VsmrAviso::ResolvePolygonOutline(&grass, nullptr, &realGrassPaint, nullptr) &&
					std::string(realGrassPaint["stroke"].GetString()) == "#5B656B" && grass["stroke-width"].GetDouble() == 0.75,
					"LFBO Real grass stays identifiable through fine outlines matching the paved surfaces");
				Expect(!VsmrAviso::ResolvePolygonOutline(&grass, nullptr, nullptr, nullptr) &&
					!VsmrAviso::ResolvePolygonOutline(&grass, nullptr, &grass["palette-overrides"]["light"], nullptr),
					"LFBO grass outlines are enabled only in Real");
				Expect(std::string(grass["fill"].GetString()) == "#010D19" &&
					std::string(grass["palette-overrides"]["light"]["fill"].GetString()) == "#00512F",
					"LFBO grass visibility fix leaves Dark and Light styling unchanged");
				for (auto style = styles.MemberBegin(); style != styles.MemberEnd(); ++style)
				{
					const auto& paint = style->value["paint"];
					Expect(paint.HasMember("palette-overrides") && paint["palette-overrides"].HasMember("real"),
						"Every LFBO style explicitly defines its Real palette");
					unsigned count = 0;
					for (const auto& feature : document["features"].GetArray())
						if (std::string(feature["properties"]["style_id"].GetString()) == style->name.GetString()) ++count;
					Expect(style->value["feature_count"].GetUint() == count, "LFBO style feature counts are accurate");
				}
				const char* gateStyles[] = { "label.gates", "label.gates.north", "label.gates.south", "label.gates.runways" };
				const char* gateColors[] = { "#A2A5A7", "#4BC5CA", "#C8A442", "#C99185" };
				for (int i = 0; i < 4; ++i)
				{
					const auto& paint = styles[gateStyles[i]]["paint"];
					Expect(std::string(paint["text-color"].GetString()) == "#CCCCCC",
						"LFBO directional gate styles preserve Dark and inherited Light text");
					Expect(std::string(paint["palette-overrides"]["real"]["text-color"].GetString()) == gateColors[i],
						"LFBO Real gate colours follow the supplied reference legend");
				}
			}
			if (airport == "LFPG")
			{
				Expect(model.FeatureCount() == 3278U, "LFPG keeps 34 A380 restriction surfaces and 48 restored holding-point labels");
				Expect(document["metadata"]["color_palettes"].Size() == 3U, "Unified LFPG exposes Dark, Light and Real palettes");
				unsigned originalCount = 0, realCount = 0, sharedCount = 0, restoredLabels = 0;
				for (const auto& feature : document["features"].GetArray())
				{
					const auto& variantPalettes = feature["properties"]["geometry_palettes"];
					if (variantPalettes.Size() == 2U) ++originalCount;
					else if (variantPalettes.Size() == 1U) ++realCount;
					else ++sharedCount;
					for (const auto& group : feature["properties"]["vsmr_group_ids"].GetArray())
						if (std::string(group.GetString()) == "labels-holding-points") ++restoredLabels;
				}
				Expect(originalCount == 1505U && realCount == 1557U && sharedCount == 216U && restoredLabels == 48U,
					"Dark/Light and Real retain their geometry and share all restored holding-point labels");
				unsigned a380Count = 0, b748Count = 0, b744Count = 0;
				for (const auto& feature : document["features"].GetArray())
					for (const auto& group : feature["properties"]["vsmr_group_ids"].GetArray())
					{
						const std::string id = group.GetString();
						if (id == "wb-a380") ++a380Count;
						else if (id == "wb-b748") ++b748Count;
						else if (id == "wb-b744") ++b744Count;
					}
				Expect(a380Count == 34U && b748Count == 37U && b744Count == 18U,
					"LFPG aircraft restriction contours remain complete and separate");
				for (const auto& group : document["vsmr_groups"].GetArray())
				{
					const std::string id = group["id"].GetString();
					if (id == "wb-a380" || id == "wb-b748" || id == "wb-b744")
						Expect(group["visible"].GetBool() == (id == "wb-a380"),
							"Only the A380 restriction profile is active by default");
				}
				for (const char* label : { "W2", "W3", "W4", "W5", "T9", "T10" })
				{
				bool found = false;
				for (const auto& feature : document["features"].GetArray())
					if (feature["properties"].HasMember("text-field") &&
						std::string(feature["properties"]["text-field"].GetString()) == label)
						found = true;
				Expect(found, std::string("LFPG keeps holding-point text ") + label);
				}
				const auto insidePolygon = [](const rapidjson::Value& rings, double longitude, double latitude)
				{
					bool inside = false;
					for (const auto& ring : rings.GetArray())
						for (rapidjson::SizeType i = 0, j = ring.Size() - 1; i < ring.Size(); j = i++)
						{
							const double ax = ring[i][0].GetDouble(), ay = ring[i][1].GetDouble();
							const double bx = ring[j][0].GetDouble(), by = ring[j][1].GetDouble();
							if ((ay > latitude) != (by > latitude) &&
								longitude < (bx - ax) * (latitude - ay) / (by - ay) + ax) inside = !inside;
						}
					return inside;
				};
				const auto a380RestrictedAt = [&](double longitude, double latitude)
				{
					for (const auto& feature : document["features"].GetArray())
					{
						bool restricted = false;
						for (const auto& entry : feature["properties"]["vsmr_group_ids"].GetArray())
							restricted = restricted || std::string(entry.GetString()) == "wb-a380";
						if (!restricted) continue;
						const auto& geometry = feature["geometry"];
						const auto& coordinates = geometry["coordinates"];
						if (std::string(geometry["type"].GetString()) == "Polygon")
						{
							if (insidePolygon(coordinates, longitude, latitude)) return true;
						}
						else for (const auto& polygon : coordinates.GetArray())
							if (insidePolygon(polygon, longitude, latitude)) return true;
					}
					return false;
				};
				Expect(a380RestrictedAt(2.550980000, 49.024503063), "A380 restriction covers the prohibited Z5 corridor");
				Expect(a380RestrictedAt(2.543629689, 49.013969378), "A3 remains prohibited beneath the photograph's opaque labels");
				Expect(a380RestrictedAt(2.533436667, 48.994139009), "A380-only C/U restriction remains visible");
				Expect(!a380RestrictedAt(2.537455272, 49.023689028), "Approved Z4 is not prohibited by an older photograph");
				Expect(!a380RestrictedAt(2.552884313, 49.006929024), "FN2 does not acquire a blanket A380 prohibition from the adjacent E wedge");
				Expect(!a380RestrictedAt(2.553070000, 49.004260360) &&
					!a380RestrictedAt(2.554653333, 49.004260360), "Approved N and F axes remain clear of adjacent A380 fills");
				Expect(std::string(document["metadata"]["geometry_mode"].GetString()) == "palette_specific",
					"Loading and normalizing the editor preserves explicit geometry variants");
				bool east = false, west = false, gates = false;
				for (const auto& group : document["vsmr_groups"].GetArray())
				{
					const std::string id = group["id"].GetString();
					east = east || id == "ground-layout-east";
					west = west || id == "ground-layout-west";
					gates = gates || id == "display-gates";
					Expect(id.rfind("real-", 0) != 0, "LFPG group identifiers are common across palettes");
					Expect(id != "dgv-labels-north" && id != "dgv-labels-south", "Rapid exit groups have been removed");
					for (const char* field : { "name", "folder" })
						for (const unsigned char c : std::string(group[field].GetString()))
							Expect(c < 128U, "LFPG group names and folders use plain English UTF-8 text");
					for (const char* paletteName : { "dark", "light", "real" })
					{
						if (group.HasMember("geometry_palettes") && std::string(paletteName) != "real") continue;
						unsigned affected = 0;
						for (const auto& feature : document["features"].GetArray())
						{
							const auto& props = feature["properties"];
							bool paletteMatches = false, groupMatches = false;
							for (const auto& paletteEntry : props["geometry_palettes"].GetArray())
								paletteMatches = paletteMatches || std::string(paletteEntry.GetString()) == paletteName;
							for (const auto& groupEntry : props["vsmr_group_ids"].GetArray())
								groupMatches = groupMatches || std::string(groupEntry.GetString()) == id;
							if (paletteMatches && groupMatches) ++affected;
						}
						Expect(affected > 0U, "Each LFPG group toggles actual geometry in every palette: " + id + "/" + paletteName);
					}

				}
				Expect(east && west && gates, "Unified LFPG preserves arrow and stand visibility controls");
				const auto& grass = document["styles"]["polygon.grassurface.00512f"]["paint"];
				Expect(std::string(grass["fill"].GetString()) == "#010D19" &&
					std::string(grass["palette-overrides"]["light"]["fill"].GetString()) == "#00512F" &&
					std::string(document["styles"]["real-polygon.grassurface.00512f"]["paint"]["palette-overrides"]["real"]["fill"].GetString()) == "#90D8D0",
					"Unified LFPG preserves the three source grass colours");
				for (const auto& feature : document["features"].GetArray())
					Expect(document["styles"].HasMember(feature["properties"]["style_id"].GetString()),
						"Every unified LFPG feature resolves its shared style");
			}
		}
		Expect(!std::filesystem::exists(avisoRoot / "LFPG_Custom.geojson"),
			"LFPG no longer depends on a separate Custom package asset");
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
		const auto migrationPath = std::filesystem::temp_directory_path() /
			("vsmr-shared-aviso-" + std::to_string(GetCurrentProcessId()) + ".geojson");
		{
			std::ofstream source(migrationPath, std::ios::binary);
			source << R"json({"type":"FeatureCollection","styles":{"old":{"color_palettes":["dark"]},"shared":{"paint":{"fill":"#112233","palette-overrides":{"light":{"fill":"#445566"},"real":{"fill":"#778899"}}}}},"vsmr_groups":[{"id":"g","color_palettes":["light"]}],"features":[{"type":"Feature","properties":{"color_palettes":["dark"]},"geometry":{"type":"Point","coordinates":[1,40]}},{"type":"Feature","properties":{"style_id":"shared","text":"Light label","color_palettes":["day"]},"geometry":{"type":"Point","coordinates":[2.12345678901234567,48.00000000000000001]}},{"type":"Feature","properties":{"color_palettes":["real"]},"geometry":{"type":"Point","coordinates":[3,49]}}]})json";
		}
		AvisoDocumentModel migrated;
		std::string migrationError;
		const bool migrationLoaded = migrated.LoadFromFile(migrationPath.u8string(), migrationError);
		Expect(migrationLoaded && migrated.FeatureCount() == 1, "Legacy maps retain only Light geometry and text");
		if (migrationLoaded && migrated.FeatureCount() == 1)
		{
			const auto& document = migrated.GetDocument();
			Expect(!document["features"][0]["properties"].HasMember("color_palettes") &&
				!document["styles"].HasMember("old") &&
				!document["vsmr_groups"][0].HasMember("color_palettes"),
				"Migration removes geometry palette scopes and obsolete styles");
			Expect(migrated.SaveAtomically(migrationPath.u8string(), migrationError), "Migrated AVISO saves");
			const auto saved = ReadTextFile(migrationPath);
			Expect(saved.find("[2.12345678901234567,48.00000000000000001]") != std::string::npos,
				"Light geometry retains original coordinate precision after removing earlier features");
			Expect(saved.find("#778899") != std::string::npos && saved.find("Light label") != std::string::npos,
				"Migration retains Real colors and shared label text");
		}
		std::filesystem::remove(migrationPath);
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

	void TestLfpgBundledOverlay()
	{
		rapidjson::Document remote, bundled;
		remote.Parse(R"({"features":[{"id":"upstream","properties":{"name":"New AIRAC geometry"}}],"styles":{},"vsmr_groups":[]})");
		bundled.Parse(R"({"vsmr_groups":[{"id":"lvp-critical-east"},{"id":"lvp-critical-west"},{"id":"lvp-loc-markers"},{"id":"lvp-hp-cat1-cat3"}],"styles":{"lvp":{"paint":{"stroke":"#ff0000"}}},"features":[{"id":"upstream","properties":{"style_id":"lvp","vsmr_group_ids":["lvp-critical-east"]},"geometry":{"type":"Point","coordinates":[1,2]}},{"id":"west","properties":{"style_id":"lvp","vsmr_group_ids":["lvp-critical-west"]},"geometry":{"type":"Point","coordinates":[2,3]}},{"id":"loc","properties":{"style_id":"lvp","vsmr_group_ids":["lvp-loc-markers"]},"geometry":{"type":"Point","coordinates":[3,4]}},{"id":"hp","properties":{"style_id":"lvp","vsmr_group_ids":["lvp-hp-cat1-cat3"]},"geometry":{"type":"Point","coordinates":[4,5]}}]})");
		std::string error;
		Expect(VsmrAvisoBundledGroups::MergeLfpgLvp(remote, bundled, error) &&
			remote["features"].Size() == 4U && remote["vsmr_groups"].Size() == 4U &&
			remote["styles"].MemberCount() == 1U &&
			std::string(remote["features"][0]["properties"]["name"].GetString()) == "New AIRAC geometry",
			"bundled LVP geometry supplements the current AIRAC without replacing matching upstream IDs");
		Expect(VsmrAvisoBundledGroups::MergeLfpgLvp(remote, bundled, error) &&
			remote["features"].Size() == 4U && remote["vsmr_groups"].Size() == 4U,
			"loading LFPG LVP overlay repeatedly does not duplicate its features or groups");
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
			repositoryRoot / "vSMR" / "data" / "profile_templates.json",
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
			"Profiles save atomically");

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
			"AVISO saves atomically");

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

std::vector<std::string> RunConfigurationRegressionTests(
	const std::filesystem::path& repositoryRoot)
{
	Failures.clear();
	TestProfiles(repositoryRoot);
	TestIndependentProfileSelections();
	TestAviso(repositoryRoot);
	TestLfpgBundledOverlay();
	TestUnicodeResourcePaths(repositoryRoot);
	return Failures;
}
