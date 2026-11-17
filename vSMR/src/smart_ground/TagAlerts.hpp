#pragma once

#include <string>
#include <unordered_set>

namespace VsmrSmartGround
{
	inline constexpr const char* CollisionAlertSetting = "SG COLLISION";
	inline constexpr const char* OffTrackAlertSetting = "OFF TRACK";

	struct TagAlertState
	{
		bool collision = false;
		bool offTrack = false;
		bool critical = false;
		std::string conflictingCallsign;
		std::string reason;
	};

	struct TagAlerts
	{
		std::string text;
		std::string tooltip;
		bool warning = false;
	};

	inline std::string AlertCallsign(const std::string& value)
	{
		std::string result;
		for (unsigned char ch : value)
		{
			if (result.size() == 16) break;
			if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 'a' + 'A');
			if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')
				result.push_back(static_cast<char>(ch));
		}
		return result;
	}

	inline TagAlerts ResolveTagAlerts(
		const TagAlertState& state,
		const std::unordered_set<std::string>& inactive,
		bool enabled = true)
	{
		TagAlerts result;
		if (!enabled) return result;
		const bool showCollision = state.collision && inactive.count(CollisionAlertSetting) == 0;
		const bool showOffTrack = state.offTrack && inactive.count(OffTrackAlertSetting) == 0;
		if (showCollision)
		{
			result.text = "SG COLL";
			const std::string other = AlertCallsign(state.conflictingCallsign);
			if (!other.empty()) result.text += " " + other;
			result.tooltip = "Smart Ground: potential aircraft-envelope conflict";
			if (!other.empty()) result.tooltip += " with " + other;
			result.warning = state.critical;
		}
		if (showOffTrack)
		{
			if (!result.text.empty()) result.text += " | ";
			result.text += "OFF TRACK";
			if (!result.tooltip.empty()) result.tooltip += "; ";
			result.tooltip += "Smart Ground: confirmed deviation from the assigned taxi route";
		}
		// A shared engine reason can describe both conditions. Do not leak a
		// disabled condition back into the tooltip of the still-visible one.
		const bool reasonVisible = (!state.collision || showCollision) && (!state.offTrack || showOffTrack);
		if (!result.tooltip.empty() && !state.reason.empty() && reasonVisible)
		{
			result.tooltip += ". ";
			for (unsigned char ch : state.reason)
			{
				if (result.tooltip.size() >= 512) break;
				result.tooltip.push_back(ch < 32 || ch == 127 ? ' ' : static_cast<char>(ch));
			}
		}
		return result;
	}

	inline std::string AppendTagAlertText(const std::string& existing, const TagAlerts& smartGround)
	{
		if (smartGround.text.empty()) return existing;
		return existing.empty() ? smartGround.text : existing + " | " + smartGround.text;
	}
}
