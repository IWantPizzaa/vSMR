#include <Windows.h>
#include <objidl.h>

#include "ConfigurationRegressionTests.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "config/RuntimeConfig.hpp"
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
			if (std::string(airport) == "LFPG.geojson")
			{
				const rapidjson::Document& document = model.GetDocument();
				Expect(document.HasMember("vsmr_groups") && document["vsmr_groups"].IsArray() &&
					document["vsmr_groups"].Size() == 2U,
					"LFPG exposes the East and West arrow groups");
				bool hasEastArrowGroup = false;
				bool hasWestArrowGroup = false;
				if (document.HasMember("vsmr_groups") && document["vsmr_groups"].IsArray())
				{
					const rapidjson::Value& groups = document["vsmr_groups"];
					for (rapidjson::SizeType groupIndex = 0; groupIndex < groups.Size(); ++groupIndex)
					{
						if (!groups[groupIndex].IsObject() || !groups[groupIndex].HasMember("id") ||
							!groups[groupIndex]["id"].IsString())
						{
							continue;
						}
						const std::string groupId = groups[groupIndex]["id"].GetString();
						hasEastArrowGroup = hasEastArrowGroup || groupId == "ground-layout-east";
						hasWestArrowGroup = hasWestArrowGroup || groupId == "ground-layout-west";
					}
				}
				Expect(hasEastArrowGroup && hasWestArrowGroup,
					"LFPG arrow groups retain their stable East and West identifiers");
				int directionalArrowFeatures = 0;
				if (document.HasMember("features") && document["features"].IsArray())
				{
					const rapidjson::Value& features = document["features"];
					for (rapidjson::SizeType featureIndex = 0; featureIndex < features.Size(); ++featureIndex)
					{
						const rapidjson::Value& feature = features[featureIndex];
						if (!feature.IsObject() || !feature.HasMember("properties") ||
							!feature["properties"].IsObject())
						{
							continue;
						}
						const rapidjson::Value& properties = feature["properties"];
						if (properties.HasMember("geometry_role") &&
							properties["geometry_role"].IsString() &&
							std::string(properties["geometry_role"].GetString()) == "directional_arrows")
						{
							++directionalArrowFeatures;
						}
					}
				}
				Expect(directionalArrowFeatures == 6, "LFPG contains both directional-arrow groups");

				bool everyTextHaloIsOne = true;
				int textHaloStyleCount = 0;
				if (document.HasMember("styles") && document["styles"].IsObject())
				{
					const rapidjson::Value& styles = document["styles"];
					for (auto style = styles.MemberBegin(); style != styles.MemberEnd(); ++style)
					{
						if (!style->value.IsObject() || !style->value.HasMember("paint") ||
							!style->value["paint"].IsObject())
						{
							continue;
						}
						const rapidjson::Value& paint = style->value["paint"];
						if (!paint.HasMember("text-halo-width"))
							continue;
						++textHaloStyleCount;
						everyTextHaloIsOne = everyTextHaloIsOne &&
							paint["text-halo-width"].IsNumber() &&
							paint["text-halo-width"].GetDouble() == 1.0;
					}
				}
				Expect(textHaloStyleCount == 6 && everyTextHaloIsOne,
					"LFPG text styles use a one-pixel halo");
			}
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
