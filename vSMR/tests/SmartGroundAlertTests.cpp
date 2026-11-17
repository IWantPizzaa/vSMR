#include <Windows.h>
#include <objidl.h>

#include "SmartGroundAlertTests.hpp"
#include "smart_ground/TagAlerts.hpp"
#include "config/RuntimeConfig.hpp"

#include <fstream>
#include <unordered_set>

std::vector<std::string> RunSmartGroundAlertTests(const std::filesystem::path& repositoryRoot)
{
	std::vector<std::string> failures;
	const auto expect = [&](bool condition, const char* message) { if (!condition) failures.push_back(message); };
	using namespace VsmrSmartGround;
	TagAlertState state;
	expect(ResolveTagAlerts(state, {}).text.empty(), "Smart Ground has no tag band without an active alert");
	state = { true, true, true, "afr123", "Aircraft envelopes may conflict." };
	const auto sessionDisabled = ResolveTagAlerts(state, {}, false);
	expect(sessionDisabled.text.empty() && sessionDisabled.tooltip.empty() && !sessionDisabled.warning,
		"Disabled Smart Ground hides even previously cached collision and Off Track alerts");
	expect(AppendTagAlertText("RWY INCURSION", sessionDisabled) == "RWY INCURSION",
		"Disabling Smart Ground preserves independent RIMCAS alerts");
	const auto both = ResolveTagAlerts(state, {});
	expect(both.text == "SG COLL AFR123 | OFF TRACK" && both.warning, "Smart Ground displays both independently active alerts and the opposing callsign");
	expect(AppendTagAlertText("NO TAXI CLR", both) == "NO TAXI CLR | SG COLL AFR123 | OFF TRACK", "Smart Ground preserves the existing RIMCAS alert band");
	const auto collisionDisabled = ResolveTagAlerts(state, { CollisionAlertSetting });
	expect(collisionDisabled.text == "OFF TRACK" && !collisionDisabled.warning, "Disabling collision leaves Off Track visible without disabled collision severity");
	expect(collisionDisabled.tooltip.find("Aircraft envelopes") == std::string::npos, "Disabled collision diagnostics do not leak through the Off Track tooltip");
	const auto trackDisabled = ResolveTagAlerts(state, { OffTrackAlertSetting });
	expect(trackDisabled.text == "SG COLL AFR123" && trackDisabled.warning, "Disabling Off Track leaves the collision warning visible");
	const auto disabled = ResolveTagAlerts(state, { CollisionAlertSetting, OffTrackAlertSetting });
	expect(disabled.text.empty() && disabled.tooltip.empty() && !disabled.warning, "Disabling both Smart Ground alerts restores the base tag");
	expect(AppendTagAlertText("RWY INCURSION", disabled) == "RWY INCURSION", "Disabled Smart Ground alerts cannot hide a RIMCAS runway alert");
	expect(ResolveTagAlerts(state, { "NO TAXI", "FUTURE ALERT" }).text == both.text, "Existing or unknown alert switches cannot disable Smart Ground implicitly");
	state.critical = false;
	expect(!ResolveTagAlerts(state, {}).warning, "A potential low-confidence conflict retains caution severity");
	state.conflictingCallsign = "a\nfr123<>";
	expect(ResolveTagAlerts(state, {}).text.find("AFR123") != std::string::npos && ResolveTagAlerts(state, {}).text.find('\n') == std::string::npos,
		"Opposing callsigns cannot add multiline or markup-like tag text");

	// Exercise the existing persisted inactive_alerts contract rather than
	// introducing a second Smart Ground configuration authority.
	const auto fixtureRoot = std::filesystem::temp_directory_path() /
		("vsmr-smart-ground-alerts-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	std::error_code error;
	std::filesystem::create_directories(fixtureRoot, error);
	if (error) { failures.push_back("Smart Ground alert profile fixture could not be created"); return failures; }
	const auto fixture = fixtureRoot / "profiles.json";
	std::filesystem::copy_file(repositoryRoot / "vSMR/data/profile_templates.json", fixture, std::filesystem::copy_options::overwrite_existing, error);
	if (error) { failures.push_back("Smart Ground alert profile fixture could not be populated"); std::filesystem::remove_all(fixtureRoot, error); return failures; }
	{
		CConfig config(fixture.u8string(), "");
		config.setActiveProfile("Default");
		expect(config.getInactiveAlert().count(CollisionAlertSetting) == 0 && config.getInactiveAlert().count(OffTrackAlertSetting) == 0,
			"Smart Ground alert switches default to enabled in existing default profiles");
		expect(config.setInactiveAlert({ CollisionAlertSetting, "NO TAXI", "FUTURE ALERT" }), "Collision alert can be disabled through the existing profile API");
		expect(config.saveConfig(), "Smart Ground alert switches save through the existing profile persistence");
	}
	{
		CConfig config(fixture.u8string(), "");
		config.setActiveProfile("Default");
		const auto inactive = config.getInactiveAlert();
		expect(inactive.count(CollisionAlertSetting) == 1 && inactive.count(OffTrackAlertSetting) == 0 && inactive.count("NO TAXI") == 1 && inactive.count("FUTURE ALERT") == 1,
			"Reload preserves independent collision, existing RIMCAS and unlisted alert settings");
		expect(config.setInactiveAlert({ OffTrackAlertSetting, "NO TAXI" }) && config.saveConfig(), "Off Track can be disabled while collision is re-enabled");
	}
	{
		CConfig config(fixture.u8string(), "");
		config.setActiveProfile("Default");
		const auto inactive = config.getInactiveAlert();
		expect(inactive.count(CollisionAlertSetting) == 0 && inactive.count(OffTrackAlertSetting) == 1 && inactive.count("NO TAXI") == 1,
			"The two Smart Ground alert switches remain independent after another reload");
	}
	std::filesystem::remove_all(fixtureRoot, error);
	return failures;
}
