#include "platform/windows/PrecompiledHeader.hpp"
#include "config/RuntimeConfig.hpp"
#include "config/RuntimeConfig.Internal.hpp"
#include "config/LayeredConfig.hpp"
#include "shared/JsonDocument.hpp"
#include "updater/UpdaterVerification.hpp"

#include <filesystem>
#include <set>

using namespace VsmrRuntimeConfigInternal;
namespace layers = VsmrLayeredConfig;

namespace
{
	constexpr const char* ProfileIdKey = "_vsmr_profile_id";

	bool ReadObject(const std::string& path, rapidjson::Document& output, std::string& error,
		std::string* revision = nullptr)
	{
		std::string bytes;
		if (!ReadFileContents(path, bytes, &error) || !ValidateJsonInputLimits(bytes, &error)) return false;
		VsmrJson::ParseDocument(output, bytes);
		if (output.HasParseError() || !output.IsObject())
		{
			error = "Configuration must contain a valid JSON object: " + path;
			return false;
		}
		if (revision != nullptr) *revision = ContentRevision(bytes);
		return true;
	}

	bool ValidateLayer(const rapidjson::Value& root, bool defaults, std::string& error)
	{
		if (!root.IsObject() || !ValidateJsonDocumentLimits(root, &error))
		{
			if (error.empty()) error = "Configuration must be a JSON object.";
			return false;
		}
		const auto* schema = layers::Member(root, "schema_version");
		if ((defaults && schema == nullptr) || (schema != nullptr && (!schema->IsInt() || schema->GetInt() != 1)))
		{
			error = "Unsupported configuration schema_version; the file was not changed.";
			return false;
		}
		for (const char* key : { "profiles", "metadata", "hidden_profiles", "_migration" })
		{
			const auto* section = layers::Member(root, key);
			if (section != nullptr && !section->IsObject())
			{
				error = std::string("Configuration '") + key + "' must be an object.";
				return false;
			}
		}
		const auto* profiles = layers::Member(root, "profiles");
		if (defaults && (profiles == nullptr || profiles->ObjectEmpty()))
		{
			error = "default.json contains no profiles.";
			return false;
		}
		if (profiles != nullptr)
		{
			if (profiles->MemberCount() > kMaximumProfiles)
			{
				error = "Configuration exceeds the 256-profile limit.";
				return false;
			}
			for (auto item = profiles->MemberBegin(); item != profiles->MemberEnd(); ++item)
			{
				const std::string id(item->name.GetString(), item->name.GetStringLength());
				if (id.empty() || id.size() > 128 || !item->value.IsObject() ||
					!std::all_of(id.begin(), id.end(), [](unsigned char c) {
						return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
							(c >= '0' && c <= '9') || c == '-' || c == '_';
					}))
				{
					error = "Profiles must be objects keyed by a stable ASCII profile ID.";
					return false;
				}
			}
		}
		if (const auto* hidden = layers::Member(root, "hidden_profiles"))
			for (auto item = hidden->MemberBegin(); item != hidden->MemberEnd(); ++item)
				if (!item->value.IsBool())
				{
					error = "hidden_profiles entries must be booleans.";
					return false;
				}
		return true;
	}

	std::string FindProfileId(const rapidjson::Value* profiles, const std::string& name)
	{
		if (profiles == nullptr || !profiles->IsObject()) return {};
		for (auto item = profiles->MemberBegin(); item != profiles->MemberEnd(); ++item)
			if (EqualsNoCaseAscii(ReadStringMember(item->value, "name"), name)) return item->name.GetString();
		return {};
	}

	const rapidjson::Value* ProfileDefaults(const rapidjson::Value* profiles, const char* id)
	{
		if (profiles == nullptr) return nullptr;
		if (const auto* exact = layers::Member(*profiles, id)) return exact;
		// Custom profiles inherit newly added settings from the common Default
		// profile while retaining every explicitly supplied legacy/custom value.
		if (const auto* common = layers::Member(*profiles, "builtin-default")) return common;
		const std::string commonId = FindProfileId(profiles, "Default");
		return commonId.empty() ? nullptr : layers::Member(*profiles, commonId.c_str());
	}

	void RuntimeToKeyed(const rapidjson::Value& runtime, const rapidjson::Value* referenceProfiles,
		const std::vector<CConfig::ProfileSaveIdentity>& identities, rapidjson::Document& output)
	{
		output.SetObject();
		auto& allocator = output.GetAllocator();
		EnsureObjectMember(output, "profiles", allocator);
		std::set<std::string> used;
		if (!runtime.IsArray()) return;
		for (const auto& entry : runtime.GetArray())
		{
			if (IsMetadataEntry(entry))
			{
				layers::Put(output, "metadata", entry["_vsmr"], allocator);
				continue;
			}
			if (!IsProfileEntry(entry)) continue;
			const std::string name = ReadStringMember(entry, "name");
			std::string id = ReadStringMember(entry, ProfileIdKey);
			bool explicitlyNew = false;
			// Browser save acknowledgements intentionally do not echo profiles.
			// A newly cloned profile may therefore still carry its source's old
			// runtime ID until reload. Persisted identity wins over that stale ID.
			for (const auto& identity : identities)
				if (EqualsNoCaseAscii(identity.currentName, name))
				{
					explicitlyNew = identity.persistedName.empty();
					id = explicitlyNew ? std::string() : FindProfileId(referenceProfiles, identity.persistedName);
					break;
				}
			if (!id.empty() && used.count(id) != 0) id.clear();
			if (id.empty() && !explicitlyNew) id = FindProfileId(referenceProfiles, name);
			if (id.empty() || used.count(id) != 0)
			{
				const std::string stem = "user-" + ContentRevision(name);
				id = stem;
				unsigned int suffix = 1;
				while (used.count(id) != 0 || (referenceProfiles != nullptr && referenceProfiles->HasMember(id.c_str())))
					id = stem + "-" + std::to_string(suffix++);
			}
			used.insert(id);
			rapidjson::Value copy;
			copy.CopyFrom(entry, allocator);
			copy.RemoveMember(ProfileIdKey);
			layers::Put(output["profiles"], id.c_str(), copy, allocator);
		}
	}

	bool Compose(const rapidjson::Document& defaults, const rapidjson::Document& overrides,
		rapidjson::Document& effective, rapidjson::Document& runtime, std::string& error)
	{
		if (!ValidateLayer(defaults, true, error) || !ValidateLayer(overrides, false, error)) return false;
		effective.CopyFrom(defaults, effective.GetAllocator());
		layers::Merge(effective, overrides, effective.GetAllocator());
		if (!ValidateLayer(effective, true, error)) return false;
		const auto* officialProfiles = layers::Member(defaults, "profiles");
		for (auto profile = effective["profiles"].MemberBegin(); profile != effective["profiles"].MemberEnd(); ++profile)
		{
			if (officialProfiles->HasMember(profile->name)) continue;
			if (const auto* common = ProfileDefaults(officialProfiles, profile->name.GetString()))
			{
				rapidjson::Value inherited;
				inherited.CopyFrom(*common, effective.GetAllocator());
				layers::Merge(inherited, profile->value, effective.GetAllocator());
				if (!profile->value.HasMember("name"))
				{
					// Retain orphan edits if an upstream profile was retired. Do not
					// give it Default's name (which would invalidate all profiles).
					const std::string fallbackName = "Recovered " + std::string(profile->name.GetString());
					SetStringMember(inherited, "name", fallbackName, effective.GetAllocator());
				}
				profile->value.Swap(inherited);
			}
		}
		runtime.SetArray();
		auto& allocator = runtime.GetAllocator();
		const auto* hidden = layers::Member(effective, "hidden_profiles");
		for (auto profile = effective["profiles"].MemberBegin(); profile != effective["profiles"].MemberEnd(); ++profile)
		{
			const auto* disabled = hidden ? layers::Member(*hidden, profile->name.GetString()) : nullptr;
			if (disabled != nullptr && disabled->IsTrue()) continue;
			rapidjson::Value copy;
			copy.CopyFrom(profile->value, allocator);
			SetStringMember(copy, ProfileIdKey, profile->name.GetString(), allocator);
			runtime.PushBack(copy, allocator);
		}
		if (const auto* metadata = layers::Member(effective, "metadata"))
		{
			rapidjson::Value wrapper(rapidjson::kObjectType);
			layers::Put(wrapper, "_vsmr", *metadata, allocator);
			runtime.PushBack(wrapper, allocator);
		}
		bool migrated = false;
		return CConfig::validateAndMigrateProfilesDocument(runtime, error, migrated);
	}

	bool PersistObject(const std::string& destination, const rapidjson::Document& candidate, std::string& error,
		std::string* revision = nullptr)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		candidate.Accept(writer);
		const std::string bytes(buffer.GetString(), buffer.GetSize());
		if (!ValidateJsonInputLimits(bytes, &error)) return false;
		std::string temporary;
		if (!WriteTemporaryFile(destination, bytes, temporary))
		{
			error = "Unable to create a temporary configuration file.";
			return false;
		}
		std::string verified;
		const bool success = ReadFileContents(temporary, verified) && verified == bytes &&
			::MoveFileExW(std::filesystem::u8path(temporary).c_str(), std::filesystem::u8path(destination).c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		if (!success)
		{
			::DeleteFileW(std::filesystem::u8path(temporary).c_str());
			error = "Unable to atomically save config.json; the previous file was retained.";
		}
		else if (revision != nullptr) *revision = ContentRevision(bytes);
		return success;
	}

	bool ReadVerifiedLegacyBaseline(const std::filesystem::path& directory, rapidjson::Document& baseline)
	{
		std::string error;
		rapidjson::Document index;
		const auto root = directory / "UpdateBaselines";
		if (!ReadObject((root / "BASELINE-HASHES.json").u8string(), index, error)) return false;
		const auto* files = layers::Member(index, "files");
		const auto* expected = files ? layers::Member(*files, "vSMR_Profiles.json") : nullptr;
		const auto baselinePath = root / "vSMR_Profiles.json";
		std::string hash;
		if (expected == nullptr || !expected->IsString() ||
			!vsmr::updater::verification::Sha256File(baselinePath, hash) ||
			!EqualsNoCaseAscii(expected->GetString(), hash)) return false;
		std::string bytes;
		bool migrated = false;
		return ReadFileContents(baselinePath.u8string(), bytes) &&
			ParseValidatedArray(bytes, baseline) && CConfig::validateAndMigrateProfilesDocument(baseline, error, migrated);
	}
}

void CConfig::configureLayeredPaths(const std::string& requestedPath)
{
	const auto path = std::filesystem::u8path(requestedPath);
	const std::string filename = path.filename().u8string();
	const auto defaults = path.parent_path() / "default.json";
	// An arbitrary external JSON file remains an independent user-owned legacy
	// source. Only canonical filenames opt into the managed layer contract.
	if ((EqualsNoCaseAscii(filename, "config.json") || EqualsNoCaseAscii(filename, "default.json") ||
		EqualsNoCaseAscii(filename, "vSMR_Profiles.json")) && HasFile(defaults.u8string()))
	{
		layered_config = true;
		defaults_path = defaults.u8string();
		config_path = (path.parent_path() / "config.json").u8string();
		legacy_config_path = (path.parent_path() / "vSMR_Profiles.json").u8string();
	}
}

bool CConfig::loadLayeredConfig()
{
	std::lock_guard<std::mutex> lock(ConfigSaveMutex());
	config_healthy = false;
	config_revision = FileRevision(config_path);
	last_load_message.clear();
	std::string error;
	rapidjson::Document defaults;
	rapidjson::Document overrides;
	rapidjson::Document effective;
	rapidjson::Document runtime;
	std::string loadedDefaultsRevision;
	std::string loadedUserRevision = config_revision;
	if (!ReadObject(defaults_path, defaults, error, &loadedDefaultsRevision) || !ValidateLayer(defaults, true, error))
	{
		last_load_message = error;
		return false;
	}
	// Valid official defaults remain available to the explicit recovery/reset
	// flow even when the user JSON is malformed. Ordinary saves stay disabled.
	layered_defaults.CopyFrom(defaults, layered_defaults.GetAllocator());
	defaults_revision = loadedDefaultsRevision;
	const bool initialImport = !HasFile(config_path);
	bool needsInitialMarker = initialImport;
	std::string legacyImportRevision;
	if (!initialImport)
	{
		if (!ReadObject(config_path, overrides, error, &loadedUserRevision) || !ValidateLayer(overrides, false, error))
		{
			last_load_message = error + " Existing user configuration was preserved; saving is disabled.";
			config_revision = FileRevision(config_path);
			return false;
		}
		const auto* migration = layers::Member(overrides, "_migration");
		const auto* imported = migration ? layers::Member(*migration, "legacy_profiles") : nullptr;
		needsInitialMarker = imported == nullptr || !imported->IsTrue();
	}
	else
	{
		overrides.SetObject();
		if (HasFile(legacy_config_path))
		{
			std::string bytes;
			rapidjson::Document legacy;
			bool migrated = false;
			if (!ReadFileContents(legacy_config_path, bytes, &error) || !ParseValidatedArray(bytes, legacy, &error) ||
				!validateAndMigrateProfilesDocument(legacy, error, migrated))
			{
				last_load_message = "Legacy profile import failed: " + error + " The original file was not changed.";
				return false;
			}
			legacyImportRevision = ContentRevision(bytes);
			const auto* assetHashes = layers::Member(defaults, "asset_hashes");
			const auto* officialHash = assetHashes ? layers::Member(*assetHashes, "vSMR_Profiles.json") : nullptr;
			std::string installedHash;
			const bool cleanOfficialLegacy = officialHash != nullptr && officialHash->IsString() &&
				officialHash->GetStringLength() == 64 &&
				vsmr::updater::verification::Sha256File(std::filesystem::u8path(legacy_config_path), installedHash) &&
				EqualsNoCaseAscii(officialHash->GetString(), installedHash);
			if (!cleanOfficialLegacy)
			{
			rapidjson::Document oldBaseline;
			rapidjson::Document keyedBaseline;
			rapidjson::Document keyedLegacy;
			const bool hasBaseline = ReadVerifiedLegacyBaseline(std::filesystem::u8path(config_path).parent_path(), oldBaseline);
			const auto* reference = layers::Member(defaults, "profiles");
			if (hasBaseline)
			{
				RuntimeToKeyed(oldBaseline, reference, {}, keyedBaseline);
				reference = layers::Member(keyedBaseline, "profiles");
			}
			RuntimeToKeyed(legacy, reference, {}, keyedLegacy);
			if (hasBaseline) layers::Difference(&keyedBaseline, keyedLegacy, overrides, overrides.GetAllocator());
			else overrides.CopyFrom(keyedLegacy, overrides.GetAllocator());
			// Respect deletions of known built-ins. Without a baseline, preserve
			// visible legacy choices conservatively instead of guessing intent.
			auto& hidden = EnsureObjectMember(overrides, "hidden_profiles", overrides.GetAllocator());
			if (reference != nullptr)
				for (auto item = reference->MemberBegin(); item != reference->MemberEnd(); ++item)
					if (!keyedLegacy["profiles"].HasMember(item->name))
					{
						rapidjson::Value disabled(true);
						layers::Put(hidden, item->name.GetString(), disabled, overrides.GetAllocator());
					}
			if (hidden.ObjectEmpty()) overrides.RemoveMember("hidden_profiles");
			last_load_message = hasBaseline ? "Legacy profiles imported as sparse overrides; original file retained."
				: "Legacy profiles preserved conservatively without a verified baseline; original file retained.";
			}
			else last_load_message = "Official legacy profiles detected; current defaults are inherited without pinned overrides.";
		}
	}
	if (needsInitialMarker)
	{
		SetIntegerMember(overrides, "schema_version", 1, overrides.GetAllocator());
		auto& migration = EnsureObjectMember(overrides, "_migration", overrides.GetAllocator());
		rapidjson::Value imported(true);
		layers::Put(migration, "legacy_profiles", imported, overrides.GetAllocator());
	}
	if (!Compose(defaults, overrides, effective, runtime, error))
	{
		last_load_message = error + " Configuration was not changed.";
		return false;
	}
	if (needsInitialMarker &&
		(FileRevision(config_path) != loadedUserRevision || FileRevision(defaults_path) != loadedDefaultsRevision ||
			(!legacyImportRevision.empty() && FileRevision(legacy_config_path) != legacyImportRevision)))
	{
		last_load_message = "Configuration changed during migration; reload before continuing. No files were written.";
		return false;
	}
	if (needsInitialMarker && !PersistObject(config_path, overrides, error, &loadedUserRevision))
	{
		last_load_message = error;
		return false;
	}
	layered_defaults.CopyFrom(defaults, layered_defaults.GetAllocator());
	layered_overrides.CopyFrom(overrides, layered_overrides.GetAllocator());
	layered_effective.CopyFrom(effective, layered_effective.GetAllocator());
	const std::string active = getActiveProfileName();
	if (!replaceInMemoryConfig(runtime, active, error))
	{
		last_load_message = error;
		return false;
	}
	layered_runtime_snapshot.CopyFrom(document, layered_runtime_snapshot.GetAllocator());
	defaults_revision = loadedDefaultsRevision;
	config_revision = loadedUserRevision;
	config_healthy = true;
	return true;
}

bool CConfig::commitLayeredOverrides(rapidjson::Document& candidate, const std::string& expectedRevision,
	std::string& error, bool allowRecoveryReplacement)
{
	if (!config_healthy && !allowRecoveryReplacement)
	{
		error = "User configuration is invalid or unavailable. No settings were written.";
		return false;
	}
	if (FileRevision(config_path) != (expectedRevision.empty() ? config_revision : expectedRevision) ||
		FileRevision(defaults_path) != defaults_revision)
	{
		error = "Configuration or defaults changed in another window; reload before saving.";
		return false;
	}
	rapidjson::Document effective;
	rapidjson::Document runtime;
	std::string writtenRevision;
	if (!Compose(layered_defaults, candidate, effective, runtime, error) || !PersistObject(config_path, candidate, error, &writtenRevision)) return false;
	const std::string active = getActiveProfileName();
	layered_overrides.CopyFrom(candidate, layered_overrides.GetAllocator());
	layered_effective.CopyFrom(effective, layered_effective.GetAllocator());
	if (!replaceInMemoryConfig(runtime, active, error)) return false;
	layered_runtime_snapshot.CopyFrom(document, layered_runtime_snapshot.GetAllocator());
	config_revision = writtenRevision;
	config_healthy = true;
	return true;
}

bool CConfig::saveLayeredConfig(const std::vector<ProfileSaveIdentity>& identities,
	const std::string& expectedRevision, std::string& error, bool recovery,
	const rapidjson::Value* userSections, bool resetProfiles)
{
	error.clear();
	if (!document.IsArray()) { error = "Profiles state must be an array."; return false; }
	rapidjson::Document edited;
	rapidjson::Document previous;
	rapidjson::Document candidate;
	candidate.CopyFrom(layered_overrides, candidate.GetAllocator());
	if (!candidate.IsObject()) candidate.SetObject();
	const auto* defaults = layers::Member(layered_defaults, "profiles");
	RuntimeToKeyed(layered_runtime_snapshot, defaults, {}, previous);
	RuntimeToKeyed(document, resetProfiles ? defaults : layers::Member(previous, "profiles"),
		resetProfiles ? std::vector<ProfileSaveIdentity>{} : identities, edited);
	if (resetProfiles)
	{
		candidate.RemoveMember("profiles");
		candidate.RemoveMember("hidden_profiles");
		candidate.RemoveMember("metadata");
		previous.CopyFrom(layered_defaults, previous.GetAllocator());
	}
	auto& allocator = candidate.GetAllocator();
	// A profile deletion is explicit domain state, not JSON null, so null keeps
	// its ordinary override meaning everywhere in configuration.
	EnsureObjectMember(candidate, "profiles", allocator);
	EnsureObjectMember(candidate, "hidden_profiles", allocator);
	auto& profileOverrides = candidate["profiles"];
	auto& hidden = candidate["hidden_profiles"];
	const auto* beforeProfiles = layers::Member(previous, "profiles");
	if (beforeProfiles != nullptr)
		for (auto item = beforeProfiles->MemberBegin(); item != beforeProfiles->MemberEnd(); ++item)
			if (!edited["profiles"].HasMember(item->name))
			{
				profileOverrides.RemoveMember(item->name);
				if (defaults != nullptr && defaults->HasMember(item->name))
				{
					rapidjson::Value disabled(true);
					layers::Put(hidden, item->name.GetString(), disabled, allocator);
				}
				else hidden.RemoveMember(item->name);
			}
	for (auto item = edited["profiles"].MemberBegin(); item != edited["profiles"].MemberEnd(); ++item)
	{
		const char* id = item->name.GetString();
		hidden.RemoveMember(id);
		const auto* base = ProfileDefaults(defaults, id);
		const auto* before = beforeProfiles ? layers::Member(*beforeProfiles, id) : nullptr;
		rapidjson::Value patch;
		if (const auto* existing = layers::Member(profileOverrides, id)) patch.CopyFrom(*existing, allocator);
		else patch.SetObject();
		layers::ApplyEdits(base, before, item->value, patch, allocator);
		if (patch.IsObject() && patch.ObjectEmpty() && defaults != nullptr && defaults->HasMember(id)) profileOverrides.RemoveMember(id);
		else layers::Put(profileOverrides, id, patch, allocator);
	}
	const bool emptyProfiles = profileOverrides.ObjectEmpty();
	const bool emptyHidden = hidden.ObjectEmpty();
	if (emptyProfiles) candidate.RemoveMember("profiles");
	if (emptyHidden) candidate.RemoveMember("hidden_profiles");
	if (const auto* metadata = layers::Member(edited, "metadata"))
	{
		rapidjson::Value patch;
		if (const auto* saved = layers::Member(candidate, "metadata")) patch.CopyFrom(*saved, allocator);
		else patch.SetObject();
		layers::ApplyEdits(layers::Member(layered_defaults, "metadata"), layers::Member(previous, "metadata"), *metadata, patch, allocator);
		if (patch.IsObject() && patch.ObjectEmpty()) candidate.RemoveMember("metadata");
		else layers::Put(candidate, "metadata", patch, allocator);
	}
	if (userSections != nullptr)
	{
		if (!userSections->IsObject()) { error = "User configuration sections must be an object."; return false; }
		for (auto section = userSections->MemberBegin(); section != userSections->MemberEnd(); ++section)
		{
			const std::string key = section->name.GetString();
			if (key == "profiles" || key == "metadata" || key == "hidden_profiles" || key == "schema_version")
			{ error = "Profile-owned roots cannot be submitted as independent sections."; return false; }
			if (section->value.IsNull()) candidate.RemoveMember(section->name);
			else layers::Put(candidate, key.c_str(), section->value, allocator);
		}
	}
	return commitLayeredOverrides(candidate, expectedRevision, error, recovery);
}

const rapidjson::Value* CConfig::getUserConfigSection(const char* key) const
{
	return layered_config && key != nullptr ? layers::Member(layered_overrides, key) : nullptr;
}

const rapidjson::Value* CConfig::getEffectiveConfigSection(const char* key) const
{
	return layered_config && key != nullptr ? layers::Member(layered_effective, key) : nullptr;
}

const rapidjson::Value* CConfig::getDefaultConfigSection(const char* key) const
{
	return layered_config && key != nullptr ? layers::Member(layered_defaults, key) : nullptr;
}

bool CConfig::saveUserConfigSection(const char* key, const rapidjson::Value* value,
	const std::string& expectedRevision, std::string& error)
{
	std::lock_guard<std::mutex> lock(ConfigSaveMutex());
	if (!layered_config || key == nullptr || *key == '\0') { error = "Layered configuration is unavailable."; return false; }
	rapidjson::Document candidate;
	candidate.CopyFrom(layered_overrides, candidate.GetAllocator());
	if (value != nullptr) layers::Put(candidate, key, *value, candidate.GetAllocator());
	else candidate.RemoveMember(key);
	return commitLayeredOverrides(candidate, expectedRevision, error);
}

bool CConfig::resetProfileOverrides(const std::string& expectedRevision, std::string& error)
{
	std::lock_guard<std::mutex> lock(ConfigSaveMutex());
	if (!layered_config || !config_healthy) { error = "Layered configuration is unavailable; use explicit recovery."; return false; }
	rapidjson::Document candidate;
	candidate.CopyFrom(layered_overrides, candidate.GetAllocator());
	for (const char* key : { "profiles", "metadata", "hidden_profiles" }) candidate.RemoveMember(key);
	return commitLayeredOverrides(candidate, expectedRevision, error);
}

bool CConfig::resetUserOverride(const std::vector<std::string>& path,
	const std::string& expectedRevision, std::string& error)
{
	std::lock_guard<std::mutex> lock(ConfigSaveMutex());
	if (!layered_config || path.empty() || path.front() == "_migration" || path.front() == "schema_version")
	{ error = "The requested override cannot be reset."; return false; }
	rapidjson::Document candidate;
	candidate.CopyFrom(layered_overrides, candidate.GetAllocator());
	std::function<void(rapidjson::Value&, size_t)> remove = [&](rapidjson::Value& parent, size_t index) {
		if (!parent.IsObject()) return;
		rapidjson::Value* child = layers::Member(parent, path[index].c_str());
		if (child == nullptr) return;
		if (index + 1 == path.size()) parent.RemoveMember(path[index].c_str());
		else
		{
			remove(*child, index + 1);
			if (child->IsObject() && child->ObjectEmpty()) parent.RemoveMember(path[index].c_str());
		}
	};
	remove(candidate, 0);
	return commitLayeredOverrides(candidate, expectedRevision, error);
}

void CConfig::acknowledgeRuntimeNormalization()
{
	if (!layered_config || !document.IsArray()) return;
	// Normalize only the matching profile in the saved comparison snapshot. An
	// unrelated unsaved edit in another profile must never be acknowledged here.
	const auto& active = getActiveProfile();
	const std::string id = ReadStringMember(active, ProfileIdKey);
	if (id.empty() || !layered_runtime_snapshot.IsArray()) return;
	for (auto& profile : layered_runtime_snapshot.GetArray())
		if (ReadStringMember(profile, ProfileIdKey) == id)
		{
			profile.CopyFrom(active, layered_runtime_snapshot.GetAllocator());
			break;
		}
}

bool CConfig::getDefaultProfiles(rapidjson::Document& output, std::string& error) const
{
	if (!layered_config) { error = "Layered configuration is unavailable."; return false; }
	rapidjson::Document empty;
	empty.SetObject();
	rapidjson::Document effective;
	return Compose(layered_defaults, empty, effective, output, error);
}

bool CConfig::readRuntimeConfigForTransaction(rapidjson::Document& output, std::string& revision, std::string& error)
{
	std::string bytes;
	if (!layered_config)
	{
		if (!ReadFileContents(config_path, bytes, &error) || !ParseSizeBoundedArray(bytes, output, &error)) return false;
		revision = ContentRevision(bytes);
		return true;
	}
	if (FileRevision(defaults_path) != defaults_revision)
	{ error = "Defaults changed; reload configuration first."; return false; }
	rapidjson::Document overrides;
	rapidjson::Document effective;
	if (!ReadObject(config_path, overrides, error, &revision) || !Compose(layered_defaults, overrides, effective, output, error)) return false;
	return true;
}

bool CConfig::persistRuntimeConfigTransaction(const rapidjson::Document& output,
	const std::string& expectedRevision, std::string& error, std::string* writtenRevision)
{
	if (!layered_config)
	{
		const bool saved = PersistConfigDocument(config_path, output);
		if (saved && writtenRevision != nullptr) *writtenRevision = FileRevision(config_path);
		return saved;
	}
	const std::string savedRevision = config_revision;
	rapidjson::Document savedRuntime;
	rapidjson::Document savedSnapshot;
	rapidjson::Document savedOverrides;
	rapidjson::Document latestOverrides;
	rapidjson::Document latestEffective;
	rapidjson::Document latestRuntime;
	if (!ReadObject(config_path, latestOverrides, error) ||
		!Compose(layered_defaults, latestOverrides, latestEffective, latestRuntime, error) ||
		FileRevision(config_path) != expectedRevision) return false;
	savedRuntime.CopyFrom(document, savedRuntime.GetAllocator());
	savedSnapshot.CopyFrom(layered_runtime_snapshot, savedSnapshot.GetAllocator());
	savedOverrides.CopyFrom(layered_overrides, savedOverrides.GetAllocator());
	layered_overrides.CopyFrom(latestOverrides, layered_overrides.GetAllocator());
	layered_runtime_snapshot.CopyFrom(latestRuntime, layered_runtime_snapshot.GetAllocator());
	document.CopyFrom(output, document.GetAllocator());
	const bool saved = saveLayeredConfig({}, expectedRevision, error, false, nullptr, false);
	if (saved && writtenRevision != nullptr) *writtenRevision = config_revision;
	document.CopyFrom(savedRuntime, document.GetAllocator());
	layered_runtime_snapshot.CopyFrom(savedSnapshot, layered_runtime_snapshot.GetAllocator());
	config_revision = savedRevision;
	if (!saved) layered_overrides.CopyFrom(savedOverrides, layered_overrides.GetAllocator());
	return saved;
}

void CConfig::refreshLayeredSnapshotsAfterPresetTransaction(const rapidjson::Document& authoritative,
	const std::string& revision)
{
	if (!layered_config) return;
	std::string error;
	rapidjson::Document saved;
	std::string actualRevision;
	if (!ReadObject(config_path, saved, error, &actualRevision) || actualRevision != revision) return;
	layered_overrides.CopyFrom(saved, layered_overrides.GetAllocator());
	layered_effective.CopyFrom(layered_defaults, layered_effective.GetAllocator());
	layers::Merge(layered_effective, saved, layered_effective.GetAllocator());
	MergeLatestAvisoPresetRoots(layered_runtime_snapshot, authoritative, {});
	config_revision = revision;
}
