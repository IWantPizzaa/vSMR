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

	bool saveConfig();

	unordered_set<string> getInactiveAlert();
	bool setInactiveAlert(const unordered_set<string>& inactiveAlerts);

	inline int isItActiveProfile(string toTest) {
		auto it = profiles.find(toTest);
		if (it != profiles.end())
			return active_profile == it->second ? 1 : 0;

		auto equalsNoCase = [](const std::string& a, const std::string& b) -> bool
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				const unsigned char ac = static_cast<unsigned char>(a[i]);
				const unsigned char bc = static_cast<unsigned char>(b[i]);
				if (std::tolower(ac) != std::tolower(bc))
					return false;
			}
			return true;
		};

		for (const auto& profileEntry : profiles)
		{
			if (equalsNoCase(profileEntry.first, toTest))
				return active_profile == profileEntry.second ? 1 : 0;
		}

		return 0;
	};

	inline void setActiveProfile(string newProfile) {
		auto trimAsciiWhitespace = [](const std::string& text) -> std::string
		{
			size_t start = 0;
			while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
				++start;

			size_t end = text.size();
			while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
				--end;

			return text.substr(start, end - start);
		};
		auto equalsNoCase = [](const std::string& a, const std::string& b) -> bool
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				const unsigned char ac = static_cast<unsigned char>(a[i]);
				const unsigned char bc = static_cast<unsigned char>(b[i]);
				if (std::tolower(ac) != std::tolower(bc))
					return false;
			}
			return true;
		};

		const std::string trimmedProfile = trimAsciiWhitespace(newProfile);

		auto it = profiles.find(trimmedProfile);
		if (it != profiles.end())
		{
			active_profile = it->second;
			return;
		}

		for (const auto& profileEntry : profiles)
		{
			if (equalsNoCase(profileEntry.first, trimmedProfile))
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
};
