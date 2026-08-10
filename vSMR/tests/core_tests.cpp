#include "stdafx.h"
#include "AvisoDocumentModel.hpp"
#include "Config.hpp"
#include "HttpHelper.hpp"
#include "WeatherData.hpp"
#include "SMRGeometry.hpp"

#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
	int failures = 0;

	void Check(bool condition, const char* description)
	{
		if (condition)
			return;
		std::cerr << "FAIL: " << description << '\n';
		++failures;
	}

	std::time_t UtcTime(int year, int month, int day, int hour, int minute)
	{
		std::tm value = {};
		value.tm_year = year - 1900;
		value.tm_mon = month - 1;
		value.tm_mday = day;
		value.tm_hour = hour;
		value.tm_min = minute;
		return ::_mkgmtime(&value);
	}

	void TestWeatherParsing()
	{
		using namespace VsmrWeather;
		Check(NormalizeIcao(" lfpg \t") == "LFPG", "ICAO values are trimmed and uppercased");
		Check(NormalizeIcao("LF-P").empty(), "non-alphanumeric ICAO is rejected");
		Check(NormalizeIcao("LFP").empty(), "non-four-character ICAO is rejected");

		const std::time_t receipt = UtcTime(2026, 8, 10, 12, 45);
		Snapshot parsed;
		Check(ParseReport("lfpg", "METAR LFPG 101230Z 27015G25KT 220V310 Q1013=", parsed, receipt),
			"standard METAR parses");
		Check(parsed.icao == "LFPG", "parsed station is normalized");
		Check(parsed.hasWind && !parsed.windVariable && !parsed.windCalm,
			"fixed non-calm wind flags parse");
		Check(parsed.windDirectionDegrees == 270 && parsed.windSpeedKnots == 15,
			"wind direction and speed parse");
		Check(parsed.hasWindGust && parsed.windGustKnots == 25, "gust parses");
		Check(parsed.hasWindVariation && parsed.windVariationFromDegrees == 220 &&
			parsed.windVariationToDegrees == 310, "wind variation parses");
		Check(parsed.hasQnh && parsed.qnhHpa == 1013, "QNH parses");
		Check(parsed.observationUtc == UtcTime(2026, 8, 10, 12, 30),
			"observation time resolves against receipt month");

		Snapshot metric;
		Check(ParseReport("LFPO", "SPECI LFPO 101240Z VRB05G10MPS A2992", metric, receipt),
			"MPS and altimeter report parses");
		Check(metric.windVariable && metric.windSpeedKnots == 10 && metric.windGustKnots == 19,
			"MPS wind converts to rounded knots");
		Check(metric.hasQnh && metric.qnhHpa == 1013, "altimeter converts to hPa");

		Snapshot calm;
		Check(ParseReport("LFBO", "LFBO 101200Z 00000KT Q1000", calm, receipt) && calm.windCalm,
			"calm wind remains valid");
		Snapshot invalid;
		Check(!ParseReport("LFPG", "LFPG 101200Z NIL", invalid, receipt), "NIL report is rejected");
		Check(!ParseReport("bad", "27010KT Q1013", invalid, receipt), "invalid station is rejected");
	}

	void TestAvisoSchemaValidation()
	{
		auto validate = [](const char* json, std::string* errorText = nullptr)
		{
			AvisoDocumentModel model;
			model.MutableDocument().Parse<0>(json);
			model.MarkIndexesDirty();
			const AvisoValidationResult result = model.ValidateAndRecalculate();
			if (errorText != nullptr)
				*errorText = result.errorText;
			return result.ok;
		};

		Check(validate(R"({"type":"FeatureCollection","metadata":{"schema_version":2},"features":[]})"),
			"current AVISO schema is accepted");
		Check(!validate(R"({"type":"NotAFeatureCollection","features":[]})"),
			"wrong AVISO root type is rejected");
		Check(!validate(R"({"type":"FeatureCollection","metadata":[],"features":[]})"),
			"non-object AVISO metadata is rejected");
		Check(!validate(R"({"type":"FeatureCollection","metadata":{"schema_version":3},"features":[]})"),
			"future AVISO schema is rejected");
		Check(!validate(R"({"type":"FeatureCollection","metadata":{"schema_version":"2"},"features":[]})"),
			"non-integer AVISO schema is rejected");
	}

	void TestWeatherCacheOrdering()
	{
		using namespace VsmrWeather;
		Clear();
		const std::time_t firstReceipt = UtcTime(2026, 8, 10, 12, 31);
		Check(Update("LFPG", "LFPG 101230Z 27010KT Q1013", firstReceipt, false),
			"EuroScope snapshot is cached");
		Check(!Update("LFPG", "LFPG 101230Z 18020KT Q1000", firstReceipt + 10, true),
			"fallback cannot replace EuroScope data for the same observation");
		Check(!Update("LFPG", "LFPG 101130Z 18020KT Q1000", firstReceipt + 20, false),
			"older observation cannot replace newer data");
		Check(Update("LFPG", "LFPG 101330Z 18020KT Q1000", firstReceipt + 3600, true),
			"newer fallback observation is accepted");

		Snapshot cached;
		Check(TryGet("lfpg", cached), "cache lookup normalizes ICAO");
		Check(cached.windDirectionDegrees == 180 && cached.fromFallback,
			"cache returns the accepted newer snapshot");
		Erase("LFPG");
		Check(!TryGet("LFPG", cached), "cache erase removes station");
		Clear();
	}

	void TestGeometry()
	{
		EuroScopePlugIn::CPosition origin;
		origin.m_Latitude = 49.0097;
		origin.m_Longitude = 2.5479;

		const auto north = SMRGeometry::ProjectPosition(origin, 0.0, 1000.0);
		const auto east = SMRGeometry::ProjectPosition(origin, 90.0, 1000.0);
		Check(north.m_Latitude > origin.m_Latitude, "north projection increases latitude");
		Check(std::fabs(north.m_Longitude - origin.m_Longitude) < 0.0001,
			"north projection preserves longitude");
		Check(east.m_Longitude > origin.m_Longitude, "east projection increases longitude");
		Check(std::fabs(east.m_Latitude - origin.m_Latitude) < 0.0001,
			"east projection approximately preserves latitude");
		Check(std::fabs(SMRGeometry::DistanceMeters(origin, north) - 1000.0) < 2.0,
			"projection and distance agree at one kilometre");
		Check(SMRGeometry::DistanceMeters(origin, origin) == 0.0, "identical points have zero distance");

		Check(SMRGeometry::ZoomLevelFromCrossDistance(2000.0) == 14, "zoom threshold is inclusive");
		Check(SMRGeometry::ZoomLevelFromCrossDistance(2001.0) == 13, "zoom advances after threshold");
		Check(SMRGeometry::ZoomLevelFromCrossDistance(34000.0) == 1, "last zoom threshold is inclusive");
		Check(SMRGeometry::ZoomLevelFromCrossDistance(34001.0) == 0, "distance beyond range returns base zoom");
		Check(SMRGeometry::SectorElementCategoryFromName("RUNWAY") == EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY,
			"known sector category maps to SDK constant");
		Check(SMRGeometry::SectorElementCategoryFromName("runway") == -1,
			"unknown/case-mismatched sector category is rejected");
	}

	void TestHttpsUrlValidation()
	{
		std::string host;
		Check(HttpHelper::IsValidHttpsUrl("https://Example.COM/api/v1?q=ok", &host) && host == "example.com",
			"HTTPS URL validation extracts a normalized host");
		Check(HttpHelper::IsValidHttpsUrl("https://raw.githubusercontent.com/org/repo/dev/file.json"),
			"valid GitHub raw URL is accepted");
		Check(!HttpHelper::IsValidHttpsUrl("http://example.com/file"), "plain HTTP URL is rejected");
		Check(!HttpHelper::IsValidHttpsUrl("https://user:secret@example.com/file"), "URL userinfo is rejected");
		Check(!HttpHelper::IsValidHttpsUrl("https://example.com/file#fragment"), "URL fragment is rejected");
		Check(!HttpHelper::IsValidHttpsUrl(" https://example.com/file"), "leading whitespace is rejected");
		Check(!HttpHelper::IsValidHttpsUrl("https://example.com/a b"), "embedded whitespace is rejected");
		Check(!HttpHelper::IsValidHttpsUrl("https:///local/file"), "hostless local-looking URL is rejected");
		Check(!HttpHelper::IsValidHttpsUrl("file:///C:/vSMR/data.json"), "local file URL is rejected");
		Check(HttpHelper::IsHttpsUrlForHost(
			"https://RAW.GITHUBUSERCONTENT.COM/org/repo/dev/file.json",
			"raw.githubusercontent.com"), "GitHub host comparison is case-insensitive");
		Check(!HttpHelper::IsHttpsUrlForHost(
			"https://raw.githubusercontent.com.evil.test/org/file.json",
			"raw.githubusercontent.com"), "GitHub lookalike suffix is rejected");
		Check(!HttpHelper::IsHttpsUrlForHost(
			"https://github.com/org/repo/raw/dev/file.json",
			"raw.githubusercontent.com"), "non-raw GitHub host is rejected by exact matcher");
	}

	std::string ReadTextFile(const std::string& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream contents;
		contents << input.rdbuf();
		return input.good() || input.eof() ? contents.str() : std::string();
	}

	bool WriteTextFile(const std::string& path, const std::string& contents)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		output.close();
		return !output.fail();
	}

	void TestProfileFixture(
		const std::string& fixtureDirectory,
		const char* fileName,
		bool expectedValid,
		bool expectedMigrated)
	{
		const std::string path = fixtureDirectory + "\\" + fileName;
		const std::string json = ReadTextFile(path);
		Check(!json.empty(), (std::string("profile fixture can be read: ") + fileName).c_str());
		if (json.empty())
			return;

		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		Check(!document.HasParseError(), (std::string("profile fixture is JSON: ") + fileName).c_str());
		if (document.HasParseError())
			return;

		std::string error;
		bool migrated = false;
		const bool valid = CConfig::validateAndMigrateProfilesDocument(document, error, migrated);
		Check(valid == expectedValid, (std::string("profile validation result: ") + fileName).c_str());
		if (valid)
		{
			Check(migrated == expectedMigrated,
				(std::string("profile migration result: ") + fileName).c_str());
			Check(document.IsArray() && document.Size() >= 2,
				(std::string("validated profile document remains complete: ") + fileName).c_str());
			if (std::string(fileName) == "profiles-v1-migrate.json")
			{
				const rapidjson::Value& profile = document[static_cast<rapidjson::SizeType>(0)];
				Check(profile["name"].IsString() && std::string(profile["name"].GetString()) == "Legacy",
					"v1 migration trims profile names");
				Check(profile["schema_version"].IsInt() && profile["schema_version"].GetInt() == 2,
					"v1 migration advances schema version");
				Check(profile.HasMember("labels") && profile["labels"].IsObject() &&
					profile.HasMember("targets") && profile["targets"].IsObject(),
					"v1 migration adds required core objects");
				Check(profile.HasMember("custom_extension") && profile["custom_extension"].IsObject() &&
					profile["custom_extension"].HasMember("must_survive") &&
					profile["custom_extension"]["must_survive"].IsBool() &&
					profile["custom_extension"]["must_survive"].GetBool(),
					"v1 migration preserves unknown extension data");
				Check(document[static_cast<rapidjson::SizeType>(1)].IsObject() &&
					document[static_cast<rapidjson::SizeType>(1)].HasMember("_vsmr") &&
					document[static_cast<rapidjson::SizeType>(1)]["_vsmr"].IsObject(),
					"v1 migration adds metadata");
			}
		}
		else
		{
			Check(!error.empty(), (std::string("invalid profile has diagnostic: ") + fileName).c_str());
		}
	}

	void TestProfileSchemas(const std::string& fixtureDirectory)
	{
		TestProfileFixture(fixtureDirectory, "profiles-v2-valid.json", true, false);
		TestProfileFixture(fixtureDirectory, "profiles-v1-migrate.json", true, true);
		TestProfileFixture(fixtureDirectory, "profiles-future-invalid.json", false, false);
		TestProfileFixture(fixtureDirectory, "profiles-wrong-type-invalid.json", false, false);
		TestProfileFixture(fixtureDirectory, "profiles-duplicate-invalid.json", false, false);

		const std::string malformed = ReadTextFile(fixtureDirectory + "\\profiles-malformed.json");
		rapidjson::Document malformedDocument;
		malformedDocument.Parse<0>(malformed.c_str());
		Check(!malformed.empty() && malformedDocument.HasParseError(),
			"malformed profile fixture is rejected before migration");
	}

	void TestConfigPersistence(const std::string& fixtureDirectory)
	{
		char tempRootBuffer[MAX_PATH] = {};
		const DWORD tempLength = ::GetTempPathA(MAX_PATH, tempRootBuffer);
		Check(tempLength > 0 && tempLength < MAX_PATH, "temporary directory is available");
		if (tempLength == 0 || tempLength >= MAX_PATH)
			return;

		std::ostringstream uniqueName;
		uniqueName << tempRootBuffer << "vsmr-core-tests-" << ::GetCurrentProcessId() << '-' << ::GetTickCount();
		const std::string testDirectory = uniqueName.str();
		Check(::CreateDirectoryA(testDirectory.c_str(), nullptr) != FALSE, "config test directory is created");
		const std::string configPath = testDirectory + "\\vSMR_Profiles.json";
		const std::string backupPath = configPath + ".bak";
		const std::string fixture = ReadTextFile(fixtureDirectory + "\\profiles-v2-valid.json");
		Check(WriteTextFile(configPath, fixture), "config fixture is staged");

		{
			CConfig first(configPath, "");
			CConfig second(configPath, "");
			Check(first.isConfigHealthy() && second.isConfigHealthy(), "two config readers load the same valid file");
			const std::string sharedRevision = first.getConfigRevision();
			Check(!sharedRevision.empty() && sharedRevision == second.getConfigRevision(),
				"two config readers start from one revision");

			first.setVacdmServerUrl("https://first.invalid");
			std::string firstError;
			Check(first.saveConfig({}, sharedRevision, &firstError), "first writer saves transactionally");
			Check(firstError.empty(), "successful save has no error");
			Check(first.isBackupAvailable(), "successful save creates a validated backup");

			second.setVacdmServerUrl("https://stale.invalid");
			std::string staleError;
			Check(!second.saveConfig({}, sharedRevision, &staleError), "stale second writer is rejected");
			Check(staleError.find("changed") != std::string::npos,
				"stale-writer rejection explains the revision conflict");
			const std::string afterConflict = ReadTextFile(configPath);
			Check(afterConflict.find("https://first.invalid") != std::string::npos &&
				afterConflict.find("https://stale.invalid") == std::string::npos,
				"stale writer cannot overwrite the first writer");

			Check(WriteTextFile(configPath, "{malformed"), "primary config can be damaged for restore test");
			std::string restoreError;
			Check(first.restoreBackup(restoreError), "validated backup restores a malformed primary");
			Check(restoreError.empty() && first.isConfigHealthy() && !first.isUsingBackup(),
				"restored config becomes the healthy primary");

			::DeleteFileA(backupPath.c_str());
			restoreError.clear();
			Check(!first.restoreBackup(restoreError) && restoreError.find("No readable") != std::string::npos,
				"missing backup is rejected with a diagnostic");
			Check(WriteTextFile(backupPath, "not-json"), "malformed backup fixture is staged");
			Check(!first.isBackupAvailable(), "malformed backup is not advertised as usable");
			restoreError.clear();
			Check(!first.restoreBackup(restoreError) && !restoreError.empty(),
				"malformed backup is rejected with a diagnostic");

			::DeleteFileA(configPath.c_str());
			std::string missingError;
			Check(!first.saveConfig({}, first.getConfigRevision(), &missingError),
				"missing primary cannot be silently overwritten by a stale in-memory copy");
			Check(!missingError.empty(), "missing-primary save failure has a diagnostic");
		}

		::DeleteFileA(configPath.c_str());
		::DeleteFileA(backupPath.c_str());
		{
			CConfig missing(configPath, "");
			Check(!missing.isConfigHealthy() && missing.getProfileCount() == 0,
				"missing primary without backup fails closed");
			Check(missing.getActiveProfile().IsObject(),
				"missing config exposes an instance-owned harmless profile sentinel");
			Check(missing.getLastLoadMessage().find("missing") != std::string::npos,
				"missing primary has a recovery diagnostic");
		}

		Check(WriteTextFile(configPath, "{malformed"), "malformed primary fixture is staged");
		{
			CConfig malformed(configPath, "");
			Check(!malformed.isConfigHealthy() && malformed.getProfileCount() == 0,
				"malformed primary without backup fails closed");
			Check(!malformed.getLastLoadMessage().empty(),
				"malformed primary has a recovery diagnostic");
		}

		Check(WriteTextFile(backupPath, fixture), "valid recovery backup fixture is staged");
		{
			CConfig replacement(configPath, "");
			Check(replacement.isUsingBackup() && !replacement.isConfigHealthy(),
				"validated backup is active before confirmed recovery replacement");
			const std::string validatedBackupBefore = ReadTextFile(backupPath);
			std::string replacementError;
			Check(replacement.saveConfig(
				{},
				replacement.getConfigRevision(),
				&replacementError,
				true),
				"confirmed recovery can replace a malformed primary");
			Check(replacementError.empty() && replacement.isConfigHealthy(),
				"confirmed recovery replacement becomes healthy");
			Check(ReadTextFile(backupPath) == validatedBackupBefore,
				"recovery replacement never overwrites the validated backup with the malformed primary");
		}

		Check(WriteTextFile(configPath, "{malformed"),
			"primary is damaged again for explicit backup promotion");
		{
			CConfig recovered(configPath, "");
			Check(recovered.isUsingBackup() && !recovered.isConfigHealthy() && recovered.getProfileCount() == 1,
				"malformed primary loads only a validated backup in memory");
			Check(recovered.getLastLoadMessage().find(".bak") != std::string::npos,
				"backup recovery status is explicit");
			std::string recoveryError;
			Check(recovered.restoreBackup(recoveryError) && recovered.isConfigHealthy() &&
				!recovered.isUsingBackup(), "explicit restore promotes validated backup to primary");
		}

		::DeleteFileA(configPath.c_str());
		::DeleteFileA(backupPath.c_str());
		Check(::RemoveDirectoryA(testDirectory.c_str()) != FALSE, "config test directory is removed");
	}

	void TestLegacyPresetAirportAssignment()
	{
		char tempRootBuffer[MAX_PATH] = {};
		const DWORD tempLength = ::GetTempPathA(MAX_PATH, tempRootBuffer);
		Check(tempLength > 0 && tempLength < MAX_PATH,
			"legacy-preset test temporary directory is available");
		if (tempLength == 0 || tempLength >= MAX_PATH)
			return;

		std::ostringstream uniqueName;
		uniqueName << tempRootBuffer << "vsmr-preset-tests-" <<
			::GetCurrentProcessId() << '-' << ::GetTickCount();
		const std::string testDirectory = uniqueName.str();
		Check(::CreateDirectoryA(testDirectory.c_str(), nullptr) != FALSE,
			"legacy-preset test directory is created");
		const std::string configPath = testDirectory + "\\vSMR_Profiles.json";
		const std::string legacyJson =
			"[{\"name\":\"Default\",\"schema_version\":2,\"labels\":{},\"targets\":{},"
			"\"aviso_presets\":{\"items\":[{\"name\":\"Profile legacy\"}],"
			"\"default\":\"Profile legacy\"}},"
			"{\"_vsmr\":{\"schema_version\":1,\"aviso_presets\":{"
			"\"items\":[{\"name\":\"Global legacy\"}],"
			"\"default\":\"Global legacy\"}}}]";
		Check(WriteTextFile(configPath, legacyJson),
			"unscoped legacy-preset fixture is staged");

		{
			CConfig assigning(configPath, "");
			CConfig stale(configPath, "");
			size_t assignedCount = 0;
			std::string assignmentError;
			Check(assigning.assignUnscopedAvisoPresetsToAirport(
				"Default", "lfpg", assignedCount, assignmentError),
				"unscoped inset presets can be explicitly assigned to an airport");
			Check(assignmentError.empty() && assignedCount == 2,
				"explicit inset-preset assignment reports every migrated preset");

			rapidjson::Document assigned;
			assigned.Parse<0>(ReadTextFile(configPath).c_str());
			const rapidjson::Value& metadata = assigned[static_cast<rapidjson::SizeType>(1)]["_vsmr"];
			const rapidjson::Value& presetRoot = metadata["aviso_presets"];
			Check(!presetRoot.HasMember("items") && !presetRoot.HasMember("default") &&
				presetRoot.HasMember("airports") &&
				presetRoot["airports"].HasMember("LFPG") &&
				presetRoot["airports"]["LFPG"]["items"].IsArray() &&
				presetRoot["airports"]["LFPG"]["items"].Size() == 2,
				"assigned presets persist only in the canonical airport store");
			Check(!assigned[static_cast<rapidjson::SizeType>(0)].HasMember("aviso_presets"),
				"legacy profile-local preset store is removed after assignment");

			// A stale native writer must merge the newly canonical preset root from
			// disk instead of resurrecting its old unscoped copy.
			stale.setVacdmServerUrl("https://after-assignment.invalid");
			Check(stale.saveConfig(),
				"ordinary stale in-memory save rebases the canonical preset root");
			rapidjson::Document afterStaleSave;
			afterStaleSave.Parse<0>(ReadTextFile(configPath).c_str());
			const rapidjson::Value& savedRoot =
				afterStaleSave[static_cast<rapidjson::SizeType>(1)]["_vsmr"]["aviso_presets"];
			Check(!savedRoot.HasMember("items") &&
				savedRoot["airports"]["LFPG"]["items"].Size() == 2 &&
				!afterStaleSave[static_cast<rapidjson::SizeType>(0)].HasMember("aviso_presets"),
				"later saves cannot resurrect unscoped inset presets");

			assignedCount = 0;
			assignmentError.clear();
			Check(!assigning.assignUnscopedAvisoPresetsToAirport(
				"Default", "LFPG", assignedCount, assignmentError) &&
				!assignmentError.empty(),
				"assignment is one-time and rejects an already-migrated store");
		}

		::DeleteFileA((configPath + ".bak").c_str());
		::DeleteFileA(configPath.c_str());
		Check(::RemoveDirectoryA(testDirectory.c_str()) != FALSE,
			"legacy-preset test directory is removed");
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "Usage: vSMR.CoreTests.exe <profile-fixture-directory>\n";
		return 2;
	}
	TestWeatherParsing();
	TestAvisoSchemaValidation();
	TestWeatherCacheOrdering();
	TestGeometry();
	TestHttpsUrlValidation();
	TestProfileSchemas(argv[1]);
	TestConfigPersistence(argv[1]);
	TestLegacyPresetAirportAssignment();
	if (failures != 0)
	{
		std::cerr << failures << " core test(s) failed.\n";
		return 1;
	}
	std::cout << "All vSMR core tests passed.\n";
	return 0;
}
