#pragma once
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <cctype>
#include <Gdiplus.h>
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/filestream.h"
#include "rapidjson/stringbuffer.h"
#include "radar/RadarUiSupport.hpp"

using namespace std;
using namespace rapidjson;

class CConfig
{
public:
	inline static constexpr size_t MaximumSerializedInputBytes =
		16U * 1024U * 1024U;

	enum class AvisoPresetTransactionAction
	{
		Abort,
		NoChange,
		Save
	};

	using AvisoPresetTransaction = std::function<AvisoPresetTransactionAction(
		rapidjson::Value& sharedMetadata,
		rapidjson::Document::AllocatorType& allocator)>;

	struct ProfileSaveIdentity
	{
		string currentName;
		string persistedName;
	};

	struct mapData
	{
		string element;
		string active; //ACTIVE:RWY:ARR:08R:DEP:08L

		bool operator==(const mapData& other) const noexcept {
			return element == other.element && active == other.active;
		}
		bool operator!=(const mapData& other) const noexcept {
			return !(*this == other);
		}
	};

	CConfig(string configPath, string mapsPath);
	virtual ~CConfig();

	const Value& getActiveProfile() const;
	Value& getMutableActiveProfile();
	bool isSidColorAvail(string sid, string airport);
	Gdiplus::Color getSidColor(string sid, string airport);
	const Value& getAirportMapIfAny(string airport);
	bool isAirportMapAvail(string airport);
	bool isCustomRunwayAvail(string airport, string name1, string name2);
	bool isCustomCursorUsed();

	Gdiplus::Color getConfigColor(const Value& config_path);
	COLORREF getConfigColorRef(const Value& config_path);

	vector<string> getAllProfiles() const;
	size_t getProfileCount() const;

	bool saveConfig(
		const vector<ProfileSaveIdentity>& profileIdentities = {},
		const string& expectedRevision = {},
		string* error = nullptr,
		bool allowRecoveryReplacement = false);
	string getConfigRevision() const;
	string getPersistedConfigRevision() const;
	string getLastLoadMessage() const;
	bool isConfigHealthy() const;
	bool isUsingBackup() const;
	bool isBackupAvailable() const;
	std::int64_t getBackupModifiedUnixSeconds() const;
	bool restoreBackup(string& error);
	static bool validateAndMigrateProfilesDocument(
		rapidjson::Document& profilesDocument,
		string& error,
		bool& migrated);
	static bool validateSerializedInputLimits(
		const string& serializedJson,
		string& error);
	const Value* getSharedAvisoPresetContainer() const;
	bool transactAvisoPresetStore(
		const string& preferredProfileName,
		const string& activeAirport,
		const AvisoPresetTransaction& transaction);
	bool assignUnscopedAvisoPresetsToAirport(
		const string& preferredProfileName,
		const string& airport,
		size_t& assignedPresetCount,
		string& error);
	bool sharesConfigFileWith(const CConfig& other) const;

	unordered_set<string> getInactiveAlert();
	bool setInactiveAlert(const unordered_set<string>& inactiveAlerts);
	string getLastActiveProfileName() const;
	bool setLastActiveProfileName(const string& profileName);
	string getVacdmServerUrl() const;
	bool setVacdmServerUrl(const string& serverUrl);

	inline int isItActiveProfile(string toTest) {
		auto it = profiles.find(toTest);
		if (it != profiles.end())
			return active_profile == it->second ? 1 : 0;

		for (const auto& profileEntry : profiles)
		{
			if (profileNamesEqualNoCase(profileEntry.first, toTest))
				return active_profile == profileEntry.second ? 1 : 0;
		}

		return 0;
	};

	inline void setActiveProfile(string newProfile) {
		const std::string trimmedProfile = trimProfileName(newProfile);

		auto it = profiles.find(trimmedProfile);
		if (it != profiles.end())
		{
			active_profile = it->second;
			return;
		}

		for (const auto& profileEntry : profiles)
		{
			if (profileNamesEqualNoCase(profileEntry.first, trimmedProfile))
			{
				active_profile = profileEntry.second;
				return;
			}
		}

		if (!profiles.empty())
			active_profile = profiles.begin()->second;
	};

	inline string getActiveProfileName() const {
		string name;
		for (std::map<string, rapidjson::SizeType>::const_iterator it = profiles.begin(); it != profiles.end(); ++it)
		{
			if (it->second == active_profile) {
				name = it->first;
				break;
			}
		}
		return name;
	};

	vector<mapData> getMapElementsForZoomLevel(int zoomLevel);

	Document document;
	Document mapDocument;
	bool reload();
	bool replaceInMemoryConfig(
		const Value& replacementDocument,
		const string& requestedActiveProfile,
		string& error);

protected:
	static bool validateAndMigratePrevalidatedProfilesDocument(
		rapidjson::Document& profilesDocument,
		string& error,
		bool& migrated);
	string config_path;
	string map_path;
	rapidjson::SizeType active_profile = 0;
	map<string, rapidjson::SizeType> profiles;
	map<int, vector<mapData>> maps;
	// Mutable callers receive this instance-owned fail-closed object when no
	// profile is available, never a shared/global sentinel.
	rapidjson::Value invalid_profile;
	string config_revision;
	string last_load_message;
	bool config_healthy = false;
	bool using_backup = false;

	bool loadConfig();
	bool loadMap();
	const Value* findSidDefinition(const string& sid, const string& airport);
	const Value* findMetadata() const;
	Value& ensureMetadata();

	static string trimProfileName(const string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
			++start;

		size_t end = text.size();
		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
			--end;

		return text.substr(start, end - start);
	}

	static bool profileNamesEqualNoCase(const string& left, const string& right)
	{
		if (left.size() != right.size())
			return false;
		for (size_t i = 0; i < left.size(); ++i)
		{
			const unsigned char leftChar = static_cast<unsigned char>(left[i]);
			const unsigned char rightChar = static_cast<unsigned char>(right[i]);
			if (std::tolower(leftChar) != std::tolower(rightChar))
				return false;
		}
		return true;
	}
};
