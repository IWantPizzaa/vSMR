#include <Windows.h>
#include <objidl.h>

#include "ConfigurationRegressionTests.hpp"
#include "aviso/AvisoDocumentModel.hpp"
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

		if (profiles.IsArray()) for (const auto& original : profiles.GetArray())
		{
			rapidjson::Document normalized;
			normalized.CopyFrom(original, normalized.GetAllocator());
			VsmrProfile::Normalize(normalized, normalized.GetAllocator());
			rapidjson::Document snapshot;
			snapshot.CopyFrom(normalized, snapshot.GetAllocator());
			Expect(!VsmrProfile::Normalize(normalized, normalized.GetAllocator()) && normalized == snapshot,
				"bundled profile normalization is idempotent");
		}

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
			const bool hasReal = airport == "LFPG" || airport == "LFML" || airport == "LFMN";
			const auto& palettes = metadata["color_palettes"];
			Expect(palettes.Size() == (hasReal ? 3U : 2U) &&
				std::string(palettes[rapidjson::SizeType(0)].GetString()) == "dark" &&
				std::string(palettes[1].GetString()) == "light" &&
				(!hasReal || std::string(palettes[2].GetString()) == "real"),
				"Generated AVISO offers Real only for LFPG, LFML and LFMN: " + airport);
			Expect(metadata["background_colors"].HasMember("real") == hasReal,
				"Background palettes agree with available palettes: " + airport);
			for (const auto& feature : document["features"].GetArray())
				Expect(!feature["properties"].HasMember("color_palettes"),
					"Every palette uses the same sector-pack geometry: " + airport);
			if (airport == "LFPG")
			{
				bool east = false, west = false;
				for (const auto& group : document["vsmr_groups"].GetArray())
				{
					const std::string id = group["id"].GetString();
					east = east || id == "ground-layout-east";
					west = west || id == "ground-layout-west";
					Expect(id != "runway-details", "LFPG runway details are not a toggleable group");
				}
				Expect(east && west, "LFPG includes independent East and West arrow controls");
				int eastArrows = 0, westArrows = 0;
				for (const auto& feature : document["features"].GetArray())
				{
					const auto& properties = feature["properties"];
					for (const auto& group : properties["vsmr_group_ids"].GetArray())
					{
						const std::string id = group.GetString();
						Expect(id != "runway-details", "LFPG runway details retain visible geometry without group references");
						if (id == "ground-layout-east") ++eastArrows;
						if (id == "ground-layout-west") ++westArrows;
					}
				}
				Expect(eastArrows == 3 && westArrows == 3, "LFPG preserves three original arrow colors for each direction");
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
	TestAviso(repositoryRoot);
	TestUnicodeResourcePaths(repositoryRoot);
	return Failures;
}
