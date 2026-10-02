#include <Windows.h>
#include "AvisoSharedConfigTests.hpp"
#include "aviso/AvisoSharedConfig.hpp"
#include <fstream>

std::vector<std::string> RunAvisoSharedConfigTests(const std::filesystem::path&)
{
	std::vector<std::string> failures;
	const auto expect = [&](bool value, const std::string& message) {
		if (!value) failures.push_back("Shared AVISO config: " + message);
	};
	const auto directory = std::filesystem::temp_directory_path() /
		("vsmr-shared-aviso-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount64()));
	if (!std::filesystem::create_directory(directory)) return { "Shared AVISO test could not create owned fixture directory" };
	const auto write = [&](const char* name, const std::string& text) {
		std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
		output << text;
	};
	try
	{
		const auto absent = VsmrAvisoSharedConfig::Read(directory, true);
		expect(absent.document != nullptr && absent.document->IsObject() && absent.document->ObjectEmpty() && absent.error.empty(),
			"unconfigured legacy install returns empty read-only snapshot");
		expect(!std::filesystem::exists(directory / "config.json"), "read does not create user configuration");
		write("default.json", R"({"schema_version":1,"profiles":{"not-copied":{}},"aviso":{"LFPG":{"style":{"a":1,"b":2}}}})");
		write("config.json", R"({"schema_version":1,"aviso":{"LFPG":{"style":{"a":7,"nullable":null}}},"_migration":{"legacy_profiles":true,"legacy_aviso":true}})");
		write("external-profiles.json", R"([{"name":"Independent"}])");
		const auto external = VsmrAvisoSharedConfig::Read(directory, true);
		expect(external.document != nullptr && external.error.empty(), "independent profile source can read canonical airport styles");
		if (external.document != nullptr)
		{
			const auto& style = (*external.document)["aviso"]["LFPG"]["style"];
			expect(style["a"].GetInt() == 7 && style["b"].GetInt() == 2 && style["nullable"].IsNull(), "nested user styles override canonical defaults");
			expect(!external.document->HasMember("profiles") && external.document->HasMember("_migration"), "cache retains only map and migration roots");
		}
		const auto cached = VsmrAvisoSharedConfig::Read(directory);
		expect(cached.document == external.document && cached.revision == external.revision, "unchanged render reads reuse snapshot");
		write("default.json", R"({"schema_version":1,"runway_group_visibility":{"LFPG":{"arrows":{"airport":"LFPG","runways":["26","27"],"exclude_runways":["08"]},"other":{"enabled":false}}}})");
		write("config.json", R"({"schema_version":1,"runway_group_visibility":{"LFPG":{"arrows":{"airport":"LFPO","runways":["06"],"exclude_runways":[]}}}})");
		const auto ruleConfig = VsmrAvisoSharedConfig::Read(directory, true);
		expect(ruleConfig.document != nullptr && ruleConfig.error.empty(), "visibility rules load through canonical config for external profiles too");
		if (ruleConfig.document) {
			const auto& rules = (*ruleConfig.document)["runway_group_visibility"]["LFPG"];
			expect(std::string(rules["arrows"]["airport"].GetString()) == "LFPO" && rules["arrows"]["runways"].Size() == 1 &&
				rules["arrows"]["exclude_runways"].Empty() && rules.HasMember("other"), "user rules merge by group ID and replace runway arrays");
		}
		write("default.json", R"({"schema_version":1,"profiles":{"not-copied":{}},"aviso":{"LFPG":{"style":{"a":1,"b":2}}}})");
		write("config.json", R"({"schema_version":1,"aviso":{"LFPG":{"style":{"a":9}}}})");
		const auto updated = VsmrAvisoSharedConfig::Read(directory, true);
		expect(updated.document != nullptr && updated.revision != external.revision &&
			(*updated.document)["aviso"]["LFPG"]["style"]["a"].GetInt() == 9, "forced concurrency read observes changes immediately");
		expect(external.document != nullptr && (*external.document)["aviso"]["LFPG"]["style"]["a"].GetInt() == 7,
			"previous shared snapshots keep valid immutable lifetime");
		write("config.json", R"({"schema_version":999})");
		const auto future = VsmrAvisoSharedConfig::Read(directory, true);
		expect(future.document == nullptr && !future.error.empty(), "future schema fails closed instead of silently dropping customizations");
		write("config.json", "{ broken");
		const auto broken = VsmrAvisoSharedConfig::Read(directory, true);
		expect(broken.document == nullptr && !broken.error.empty(), "malformed existing user JSON produces an error");
		write("config.json", std::string(16U * 1024U * 1024U + 1U, ' '));
		const auto oversized = VsmrAvisoSharedConfig::Read(directory, true);
		expect(oversized.document == nullptr && !oversized.error.empty(), "16 MB limit is checked before parsing");
	}
	catch (const std::exception& exception) { failures.push_back(std::string("Shared AVISO test exception: ") + exception.what()); }
	std::error_code ignored;
	std::filesystem::remove_all(directory, ignored);
	return failures;
}
