#include <Windows.h>
#include <objidl.h>
#include "LayeredConfigTests.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/LayeredConfig.hpp"
#include "config/ProfileNormalization.hpp"
#include "updater/UpdaterVerification.hpp"

#include "rapidjson/writer.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
	using rapidjson::Document;
	using rapidjson::Value;
	namespace layers = VsmrLayeredConfig;
	std::vector<std::string> Failures;

	void Expect(bool condition, const std::string& name)
	{
		// Later assertions dereference values produced by earlier saves. Stop
		// this fixture on failure instead of turning a useful I/O error into an
		// access violation in Release builds where RapidJSON asserts are off.
		if (!condition) throw std::runtime_error(name);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;
		TemporaryDirectory()
		{
			static unsigned int sequence = 0;
			path = std::filesystem::temp_directory_path() /
				("vsmr-layered-tests-" + std::to_string(::GetCurrentProcessId()) + "-" +
					std::to_string(::GetTickCount64()) + "-" + std::to_string(++sequence));
			if (!std::filesystem::create_directory(path)) throw std::runtime_error("Cannot create owned temporary directory");
		}
		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void Write(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output.good()) throw std::runtime_error("Fixture write failed");
	}

	std::string Read(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream bytes;
		bytes << input.rdbuf();
		return bytes.str();
	}

	Document Parse(const std::string& text)
	{
		Document value;
		value.Parse(text.c_str());
		if (value.HasParseError()) throw std::runtime_error("Fixture JSON parse failed");
		return value;
	}

	std::string Serialize(const Value& value)
	{
		rapidjson::StringBuffer text;
		rapidjson::Writer<rapidjson::StringBuffer> writer(text);
		value.Accept(writer);
		return std::string(text.GetString(), text.GetSize());
	}

	std::string Defaults(int size = 10)
	{
		return std::string(R"({"schema_version":1,"profiles":{"builtin-default":{"schema_version":2,"name":"Default","labels":{},"targets":{"icon_style":"diamond","symbol_scale":1.0},"font":{"font_name":"Arial","sizes":{"one":)") +
			std::to_string(size) + R"(,"two":11}},"future_setting":42},"builtin-lfpg":{"schema_version":2,"name":"LFPG","labels":{},"targets":{"icon_style":"triangle"},"font":{"font_name":"Arial","sizes":{"one":10}}}},"metadata":{"schema_version":1,"last_active_profile":"Default"},"nullable_extension":{"value":"default"}})";
	}

	std::string Legacy(int size = 10)
	{
		return std::string(R"([{"schema_version":2,"name":"Default","labels":{},"targets":{"icon_style":"diamond","symbol_scale":1.0},"font":{"font_name":"Arial","sizes":{"one":)") +
			std::to_string(size) + R"(,"two":11}}},{"_vsmr":{"schema_version":1,"last_active_profile":"Default"}}])";
	}

	void TestMerge()
	{
		auto defaults = Parse(R"({"nested":{"a":1,"b":2},"array":[1,2],"zero":7,"flag":true,"text":"x","nullable":4})");
		auto overrides = Parse(R"({"nested":{"a":3},"array":[],"zero":0,"flag":false,"text":"","nullable":null})");
		layers::Merge(defaults, overrides, defaults.GetAllocator());
		Expect(defaults["nested"]["a"].GetInt() == 3 && defaults["nested"]["b"].GetInt() == 2, "objects merge recursively");
		Expect(defaults["array"].Empty() && defaults["zero"].GetInt() == 0 && !defaults["flag"].GetBool() &&
			std::string(defaults["text"].GetString()).empty() && defaults["nullable"].IsNull(), "arrays and explicit falsy/null values replace defaults");
		auto base = Parse(R"({"nested":{"a":3,"b":2}})");
		auto previous = Parse(R"({"nested":{"a":3,"b":2}})");
		auto edited = Parse(R"({"nested":{"a":3,"b":8}})");
		auto patch = Parse(R"({"nested":{"a":3}})");
		layers::ApplyEdits(&base, &previous, edited, patch, patch.GetAllocator());
		Expect(patch["nested"]["a"].GetInt() == 3 && patch["nested"]["b"].GetInt() == 8, "unchanged pinned overrides survive incidental equality to defaults");
	}

	void TestPersistence()
	{
		TemporaryDirectory temporary;
		const auto defaultsPath = temporary.path / "default.json";
		const auto userPath = temporary.path / "config.json";
		Write(defaultsPath, Defaults());
		CConfig config(defaultsPath.u8string(), "");
		Expect(config.isLayeredConfig() && config.isConfigHealthy() && config.getProfileCount() == 2, "fresh layered source loads real runtime profiles");
		Expect(config.getConfigPath() == userPath.u8string(), "actual save path is user config, not managed defaults");
		auto sparse = Parse(Read(userPath));
		Expect(!sparse.HasMember("profiles") && sparse["_migration"]["legacy_profiles"].IsTrue(), "fresh config is sparse with successful import marker");
		const std::string originalDefaults = Read(defaultsPath);
		config.getMutableActiveProfile()["font"]["sizes"]["one"].SetInt(14);
		Expect(config.saveConfig(), "single nested edit saves");
		sparse = Parse(Read(userPath));
		Expect(sparse["profiles"]["builtin-default"].MemberCount() == 1 &&
			sparse["profiles"]["builtin-default"]["font"]["sizes"].MemberCount() == 1, "one edit writes only one overridden leaf");
		Expect(Read(defaultsPath) == originalDefaults, "managed defaults never written by save");
		const std::string once = Read(userPath);
		Expect(config.saveConfig() && Read(userPath) == once, "no-op saves retain sparse content");
		std::string error;
		auto extension = Parse(R"({"value":null,"unknown":{"kept":true}})");
		Expect(config.saveUserConfigSection("nullable_extension", &extension, config.getConfigRevision(), error), "arbitrary user section saves under same transaction");
		Expect(config.getEffectiveConfigSection("nullable_extension")->operator[]("value").IsNull(), "explicit null remains distinguishable in effective config");
		config.getMutableActiveProfile()["name"].SetString("Renamed", config.document.GetAllocator());
		Expect(config.saveConfig(), "renaming a built-in succeeds");
		sparse = Parse(Read(userPath));
		Expect(sparse["profiles"].HasMember("builtin-default") && sparse["profiles"].MemberCount() == 1,
			"renaming retains stable profile ID");
		Write(defaultsPath, Defaults(12));
		Expect(!config.saveConfig(), "default revision change blocks stale save");
		Expect(config.reload(), "updated defaults reload");
		config.setActiveProfile("Renamed");
		Expect(config.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 14, "new default preserves explicit user override");
		Expect(config.resetUserOverride({ "profiles", "builtin-default", "font", "sizes", "one" }, config.getConfigRevision(), error), "reset removes specific override");
		config.setActiveProfile("Renamed");
		Expect(config.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 12, "reset inherits current default immediately");
		CConfig second(defaultsPath.u8string(), "");
		config.getMutableActiveProfile()["font"]["sizes"]["one"].SetInt(18);
		Expect(config.saveConfig(), "first writer succeeds");
		second.getMutableActiveProfile()["font"]["sizes"]["one"].SetInt(20);
		Expect(!second.saveConfig(), "stale independent writer rejected");
		Expect(config.resetProfileOverrides(config.getConfigRevision(), error), "profile reset succeeds");
		sparse = Parse(Read(userPath));
		Expect(!sparse.HasMember("profiles") && sparse.HasMember("nullable_extension") && sparse.HasMember("_migration"),
			"profile reset keeps unrelated user data and migration state");
		Document resetProfiles;
		Expect(config.getDefaultProfiles(resetProfiles, error), "reset source is current default.json");
		Expect(config.replaceInMemoryConfig(resetProfiles, "Default", error), "stage current defaults");
		config.getMutableActiveProfile()["font"]["sizes"]["one"].SetInt(16);
		Expect(config.saveConfig({}, {}, &error, false, nullptr, true), "edits made after staged reset persist");
		Expect(config.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 16, "staged reset does not discard subsequent edit");
		const std::string good = Read(userPath);
		Write(userPath, R"({"schema_version":999})");
		Expect(!config.reload() && !config.isConfigHealthy() && !config.saveConfig(), "future user schema is preserved read-only");
		Expect(Read(userPath) == R"({"schema_version":999})", "unsupported schema is never auto-overwritten");
		Write(userPath, good);
	}

	void TestCustomAndNormalization()
	{
		TemporaryDirectory temporary;
		const auto defaultsPath = temporary.path / "default.json";
		Write(defaultsPath, Defaults());
		CConfig config(defaultsPath.u8string(), "");
		Expect(config.isConfigHealthy(), "custom profile fixture could not initialize: " + config.getLastLoadMessage());
		Value copy;
		copy.CopyFrom(config.getActiveProfile(), config.document.GetAllocator());
		copy["name"].SetString("Custom user", config.document.GetAllocator());
		config.document.PushBack(copy, config.document.GetAllocator());
		Expect(config.saveConfig(), "duplicated profile gets independent ID");
		config.setActiveProfile("Custom user");
		const std::string customId = config.getActiveProfile()["_vsmr_profile_id"].GetString();
		Expect(customId != "builtin-default" && customId.rfind("user-", 0) == 0, "custom profile identity distinct from cloned built-in");
		auto defaults = Parse(Defaults());
		defaults["profiles"]["builtin-default"].AddMember("added_later", 99, defaults.GetAllocator());
		Write(defaultsPath, Serialize(defaults));
		Expect(config.reload(), "custom profile reloads after defaults update");
		config.setActiveProfile("Custom user");
		Expect(config.getActiveProfile()["added_later"].GetInt() == 99, "new custom-profile setting inherits common defaults");
		const std::string before = Read(temporary.path / "config.json");
		VsmrProfile::Normalize(config.getMutableActiveProfile(), config.document.GetAllocator());
		config.acknowledgeRuntimeNormalization();
		Expect(config.saveConfig() && Read(temporary.path / "config.json") == before, "runtime normalization does not flatten inherited values into user config");
		// Simulate the browser retaining a cloned source ID after a compact save
		// acknowledgement, then deleting the source before its next save.
		Document browser;
		browser.CopyFrom(config.document, browser.GetAllocator());
		for (auto profile = browser.Begin(); profile != browser.End();)
		{
			const auto* name = layers::Member(*profile, "name");
			if (name != nullptr && std::string(name->GetString()) == "Default") profile = browser.Erase(profile);
			else
			{
				if (name != nullptr && std::string(name->GetString()) == "Custom user")
					(*profile)["_vsmr_profile_id"].SetString("builtin-default", browser.GetAllocator());
				++profile;
			}
		}
		config.document.CopyFrom(browser, config.document.GetAllocator());
		Expect(config.saveConfig({ { "LFPG", "LFPG" }, { "Custom user", "Custom user" } }),
			"browser save resolves persisted name before stale cloned ID");
		config.setActiveProfile("Custom user");
		Expect(std::string(config.getActiveProfile()["_vsmr_profile_id"].GetString()) == customId && config.getProfileCount() == 2,
			"deleting source preserves custom identity and hides only original built-in");
	}

	void TestLegacyMigration()
	{
		TemporaryDirectory temporary;
		Write(temporary.path / "default.json", Defaults(12));
		Write(temporary.path / "vSMR_Profiles.json", Legacy(17));
		const std::string original = Read(temporary.path / "vSMR_Profiles.json");
		CConfig conservative((temporary.path / "vSMR_Profiles.json").u8string(), "");
		Expect(conservative.isLayeredConfig() && conservative.isConfigHealthy(), "canonical legacy source migrates");
		Expect(conservative.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 17, "unknown provenance retains legacy custom values");
		Expect(Read(temporary.path / "vSMR_Profiles.json") == original, "legacy source remains exact backup");
		Write(temporary.path / "external.json", Legacy(22));
		CConfig external((temporary.path / "external.json").u8string(), "");
		Expect(!external.isLayeredConfig() && external.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 22,
			"explicit noncanonical external profile file stays legacy and independent");

		TemporaryDirectory baselined;
		Write(baselined.path / "default.json", Defaults(12));
		Write(baselined.path / "vSMR_Profiles.json", Legacy(10));
		std::filesystem::create_directory(baselined.path / "UpdateBaselines");
		const auto baselineFile = baselined.path / "UpdateBaselines" / "vSMR_Profiles.json";
		Write(baselineFile, Legacy(10));
		std::string hash;
		Expect(vsmr::updater::verification::Sha256File(baselineFile, hash), "baseline fixture hashes");
		Write(baselined.path / "UpdateBaselines" / "BASELINE-HASHES.json",
			"{\"schema_version\":1,\"files\":{\"vSMR_Profiles.json\":\"" + hash + "\"}}");
		CConfig inherited((baselined.path / "vSMR_Profiles.json").u8string(), "");
		Expect(inherited.isConfigHealthy() && inherited.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 12,
			"verified old baseline migrates untouched values to inheritance of new defaults");

		TemporaryDirectory cleanPackage;
		const auto legacyFile = cleanPackage.path / "vSMR_Profiles.json";
		Write(legacyFile, Legacy(10));
		std::string officialHash;
		Expect(vsmr::updater::verification::Sha256File(legacyFile, officialHash), "official legacy fixture hashes");
		auto officialDefaults = Parse(Defaults(10));
		Value hashes(rapidjson::kObjectType);
		Value fingerprint(officialHash.c_str(), static_cast<rapidjson::SizeType>(officialHash.size()), officialDefaults.GetAllocator());
		layers::Put(hashes, "vSMR_Profiles.json", fingerprint, officialDefaults.GetAllocator());
		layers::Put(officialDefaults, "asset_hashes", hashes, officialDefaults.GetAllocator());
		Write(cleanPackage.path / "default.json", Serialize(officialDefaults));
		CConfig freshPackage(legacyFile.u8string(), "");
		Expect(freshPackage.isConfigHealthy() && freshPackage.getUserConfigSection("profiles") == nullptr,
			"fresh full package official legacy file does not pin shipped defaults");
		Expect(freshPackage.getProfileCount() == 2, "clean legacy fixture inherits all current official profiles");
		Expect(Read(legacyFile) == Legacy(10), "clean package legacy source remains unchanged");
	}

	void TestPresetAndSections()
	{
		TemporaryDirectory temporary;
		Write(temporary.path / "default.json", Defaults());
		CConfig owner((temporary.path / "default.json").u8string(), "");
		CConfig other((temporary.path / "default.json").u8string(), "");
		std::string error;
		auto sections = Parse(R"({"aviso":{"LFPG":{"properties":{"colour":12}}}})");
		owner.getMutableActiveProfile()["font"]["sizes"]["one"].SetInt(14);
		Expect(owner.saveConfig({}, {}, &error, false, &sections), "profile plus AVISO override commit in one save");
		Expect(owner.getUserConfigSection("aviso") != nullptr, "AVISO section retained");
		Expect(other.reload(), "other screen reloads config");
		const bool preset = owner.transactAvisoPresetStore("Default", "LFPG",
			[](Value& metadata, Document::AllocatorType& allocator) {
				Value marker(true);
				layers::Put(metadata, "test_preset_transaction", marker, allocator);
				return CConfig::AvisoPresetTransactionAction::Save;
			});
		Expect(preset, "shared inset preset transaction works on object config");
		Expect(other.getConfigRevision() == owner.getConfigRevision(), "preset transaction synchronizes current other windows revisions");
		Expect(owner.getUserConfigSection("aviso") != nullptr && owner.getActiveProfile()["font"]["sizes"]["one"].GetInt() == 14,
			"preset save preserves unrelated overrides");
		Expect(other.saveConfig(), "another current window can save after preset transaction");
	}
}

std::vector<std::string> RunLayeredConfigTests(const std::filesystem::path&)
{
	Failures.clear();
	try
	{
		TestMerge();
		TestPersistence();
		TestCustomAndNormalization();
		TestLegacyMigration();
		TestPresetAndSections();
	}
	catch (const std::exception& error) { Failures.push_back(std::string("Layered config exception: ") + error.what()); }
	return Failures;
}
