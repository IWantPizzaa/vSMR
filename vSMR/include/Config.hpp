#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <unordered_set>
#include <vector>
#include <cctype>
#include <Gdiplus.h>
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/filestream.h"
#include "rapidjson/stringbuffer.h"
#include "Constant.hpp"

using namespace std;
using namespace rapidjson;

class CConfig
{
public:
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

	const Value& getActiveProfile();
	bool isSidColorAvail(string sid, string airport);
	Gdiplus::Color getSidColor(string sid, string airport);
	const Value& getAirportMapIfAny(string airport);
	bool isAirportMapAvail(string airport);
	bool isCustomRunwayAvail(string airport, string name1, string name2);
	bool isCustomCursorUsed();

	Gdiplus::Color getConfigColor(const Value& config_path);
	COLORREF getConfigColorRef(const Value& config_path);

	vector<string> getAllProfiles();
	size_t getProfileCount() const;

	bool saveConfig();

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

	inline string getActiveProfileName() {
		string name;
		for (std::map<string, rapidjson::SizeType>::iterator it = profiles.begin(); it != profiles.end(); ++it)
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
	void reload();

protected:
	string config_path;
	string map_path;
	rapidjson::SizeType active_profile = 0;
	map<string, rapidjson::SizeType> profiles;
	map<int, vector<mapData>> maps;

	void loadConfig();
	void loadMap();
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
