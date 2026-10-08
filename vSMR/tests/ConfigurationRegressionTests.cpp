#include <Windows.h>
#include <objidl.h>

#include "ConfigurationRegressionTests.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "aviso/AvisoPolygonOutline.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/ProfileNormalization.hpp"
#include "control_center/RuntimeResourceFiles.hpp"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <filesystem>
#include <fstream>
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
				lfmnProfileFound = lfmnProfileFound || name == "LFMN";
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
			if (!document.HasMember("metadata") || !document["metadata"].HasMember("geometry_source")) continue;
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
				bool east = false, west = false;
				Expect(document["vsmr_groups"].Size() == 2U, "LFPG has two independent arrow groups");
				for (const auto& group : document["vsmr_groups"].GetArray())
				{
					const std::string id = group["id"].GetString();
					east = east || id == "ground-layout-east";
					west = west || id == "ground-layout-west";
					Expect(group["visible"].GetBool(), "LFPG arrow groups are available and initially visible");
				}
				Expect(east && west, "LFPG exposes East Arrows and West Arrows separately");
				Expect(model.FeatureCount() == 1474U, "LFPG retains 1468 supplied features plus six grouped arrow features");
				int eastArrows = 0, westArrows = 0, ungroupedFeatures = 0;
				for (const auto& feature : document["features"].GetArray())
				{
					const auto& properties = feature["properties"];
					const auto& groups = properties["vsmr_group_ids"];
					if (groups.Empty()) { ++ungroupedFeatures; continue; }
					Expect(groups.Size() == 1U, "Each LFPG arrow belongs to only one direction group");
					Expect(properties.HasMember("geometry_role") &&
						std::string(properties["geometry_role"].GetString()) == "directional_arrows",
						"LFPG arrow groups do not hide unrelated airport geometry");
					const auto& geometry = feature["geometry"];
					const bool multiLine = std::string(geometry["type"].GetString()) == "MultiLineString";
					Expect(multiLine, "LFPG arrows use the updated grouped MultiLineString geometry");
					const int arrowCount = multiLine ? static_cast<int>(geometry["coordinates"].Size()) : 1;
					for (const auto& group : groups.GetArray())
					{
						const std::string id = group.GetString();
						Expect(id == "ground-layout-east" || id == "ground-layout-west", "LFPG arrow membership is valid");
						if (id == "ground-layout-east") eastArrows += arrowCount;
						if (id == "ground-layout-west") westArrows += arrowCount;
					}
				}
				Expect(eastArrows == 89 && westArrows == 97 && ungroupedFeatures == 1468,
					"LFPG preserves the original East/West arrow sets and ungrouped airport layout");
				bool grassPaletteFound = false;
				for (auto style = document["styles"].MemberBegin(); style != document["styles"].MemberEnd(); ++style)
				{
					const auto& paint = style->value["paint"];
					if (paint.HasMember("text-halo-width"))
						Expect(paint["text-halo-width"].GetDouble() == 1.0, "LFPG labels retain one-pixel halos");
					if (std::string(style->name.GetString()).find("polygon.grassurface.") == 0)
					{
						grassPaletteFound = true;
						const auto& overrides = paint["palette-overrides"];
						Expect(std::string(overrides["light"]["fill"].GetString()) == "#00512F" &&
							std::string(overrides["real"]["fill"].GetString()) == "#6A958B",
							"LFPG uses pack Light and preserved GeoJSON Real grass colors");
					}
				}
				Expect(grassPaletteFound, "LFPG includes sector-pack grass geometry");
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
	TestUnicodeResourcePaths(repositoryRoot);
	return Failures;
}
