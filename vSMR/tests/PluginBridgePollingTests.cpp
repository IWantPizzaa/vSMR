#include "PluginBridgeTests.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/RampAgentBridgeClient.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	VsmrPluginBridge::AttachState HostAttachState = VsmrPluginBridge::AttachState::Attached;
	std::string SubmittedCommand;
	std::vector<std::string> SubmittedCommands;
	bool AcceptCommand = true;
	VsmrEuroScopeCommandLine::SubmissionStatus SubmissionResult = VsmrEuroScopeCommandLine::SubmissionStatus::Idle;
	struct PublishedField
	{
		const char* name;
		ESB_Type type;
		std::string text;
		std::int64_t scalar = 0;
		bool declared = true;
	};
	std::vector<PublishedField> Published;
	std::map<std::string, std::uint32_t> Providers;
	std::uint64_t Revision = 1U;
	int Reads = 0;

	ESB_Status __cdecl Version(const char* provider, std::uint32_t* major, std::uint32_t* minor)
	{
		const auto found = Providers.find(provider);
		if (found == Providers.end()) return ESB_E_NO_PROVIDER;
		*major = 1U;
		*minor = found->second;
		return ESB_OK;
	}

	ESB_Status __cdecl Resolve(const char* name, ESB_Type type, ESB_FieldId* field)
	{
		for (std::size_t i = 0; i < Published.size(); ++i)
		{
			const auto& value = Published[i];
			if (value.declared && std::strcmp(value.name, name) == 0)
			{
				if (value.type != type) return ESB_E_TYPE_MISMATCH;
				*field = static_cast<ESB_FieldId>(i + 1U);
				return ESB_OK;
			}
		}
		return ESB_E_NO_FIELD;
	}

	ESB_Status __cdecl Aircraft(const char* callsign, ESB_Aircraft* aircraft)
	{
		if (std::strcmp(callsign, "AFR123") != 0) return ESB_E_UNKNOWN_AIRCRAFT;
		*aircraft = 1U;
		return ESB_OK;
	}

	ESB_Status __cdecl Read(ESB_FieldId field, ESB_Value* out, void* buffer, std::uint32_t* bytes)
	{
		++Reads;
		if (field == 0U || field > Published.size()) return ESB_E_NO_FIELD;
		const auto& value = Published[field - 1U];
		if (!value.declared) return ESB_E_NO_FIELD;
		out->type = value.type;
		if (value.type == ESB_T_I64)
		{
			out->bytes = sizeof(std::int64_t);
			out->v.i64 = value.scalar;
			return ESB_OK;
		}
		if (value.type == ESB_T_BOOL)
		{
			out->bytes = sizeof(std::int32_t);
			out->v.b = value.scalar != 0;
			return ESB_OK;
		}
		if (value.text.empty()) return ESB_E_UNSET;
		out->bytes = static_cast<std::uint32_t>(value.text.size());
		const auto available = *bytes;
		*bytes = out->bytes;
		if (available < out->bytes) return ESB_E_BUFFER_TOO_SMALL;
		std::memcpy(buffer, value.text.data(), out->bytes);
		return ESB_OK;
	}

	ESB_Status __cdecl ReadAircraft(ESB_Aircraft, ESB_FieldId field, ESB_Value* out, void* buffer, std::uint32_t* bytes)
	{
		return Read(field, out, buffer, bytes);
	}

	std::uint64_t __cdecl ProviderRevision(const char*) { return Revision; }
}

// Host-only boundaries: production provider pollers and read helpers are linked
// unchanged. Tests never attach to EuroScope or send input to a real window.
std::atomic<bool> Logger::ENABLED{ false };
std::atomic<Logger::Mode> Logger::CURRENT_MODE{ Logger::Mode::Normal };
std::string Logger::DLL_PATH;
void VsmrCrashReporter::RecordLog(const char*) noexcept {}
VsmrPluginBridge::AttachState VsmrPluginBridge::GetAttachState() noexcept { return HostAttachState; }
const char* VsmrPluginBridge::MissingBridgeMessage() noexcept { return ESB_MISSING_MESSAGE; }
std::string VsmrPluginBridge::NormalizeCallsign(const std::string& callsign)
{
	return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
}
void VsmrPluginBridge::ProviderDiagnostics::Report(const ProviderBinding&) {}
void VsmrPluginBridge::ProviderDiagnostics::Reset() noexcept {}
bool VsmrEuroScopeCommandLine::Begin(Owner, const std::string& command, std::string*)
{
	if (!AcceptCommand) return false;
	SubmittedCommand = command;
	SubmittedCommands.push_back(command);
	SubmissionResult = SubmissionStatus::Pending;
	return true;
}
VsmrEuroScopeCommandLine::SubmissionStatus VsmrEuroScopeCommandLine::Poll(Owner)
{
	const auto result = SubmissionResult;
	if (result != SubmissionStatus::Pending) SubmissionResult = SubmissionStatus::Idle;
	return result;
}
bool VsmrEuroScopeCommandLine::IsBusy() noexcept { return SubmissionResult == SubmissionStatus::Pending; }
void VsmrEuroScopeCommandLine::Cancel(Owner) noexcept
{
	SubmittedCommand.clear();
	SubmissionResult = SubmissionStatus::Idle;
}

void RunPluginBridgePollingTests(std::vector<std::string>& failures)
{
	const auto check = [&](bool condition, const char* message) { if (!condition) failures.emplace_back(message); };
	VsmrVsid::Shutdown();
	VsmrCdm::Shutdown();
	VsmrRampAgent::Shutdown();
	Providers = { { "vsid", 3U }, { "rampagent", 0U }, { "com.viffsys.cdm", 0U } };
	Published = {
		{ "vsid/sid", ESB_T_STR, "BUB6B" },
		{ "vsid/rwy", ESB_T_STR, "26R" },
		{ "vsid/cfl", ESB_T_STR, "070" },
		{ "vsid/automode", ESB_T_STR, "LFPG=1;" },
		{ "vsid/paris", ESB_T_STR, "LFPG=?LM;LFPO=?UM;LFPN=WLM;LFPV=ELM;LFPT=WUM;LFOB=EUM;" },
		{ "rampagent/stand", ESB_T_STR, " K12 " },
		{ "rampagent/remark", ESB_T_STR, "Contact apron" },
		{ "com.viffsys.cdm/tsat", ESB_T_I64, {}, 742 },
		{ "com.viffsys.cdm/manual_ctot", ESB_T_BOOL, {}, 1 }
	};
	ESB_Api_v1 api{};
	api.resolve = Resolve;
	api.provider_version = Version;
	api.aircraft = Aircraft;
	api.get_global = Read;
	api.get_ac = ReadAircraft;
	api.provider_revision = ProviderRevision;
	VsmrPluginBridge::Tick tick{ &api, { "AFR123" } };
	const auto poll = [&] {
		(void)VsmrVsid::Poll(tick);
		(void)VsmrCdm::Poll(tick);
		(void)VsmrRampAgent::Poll(tick);
	};
	poll();
	VsmrVsid::AircraftData vsid;
	VsmrRampAgent::AircraftData ramp;
	VsmrCdm::AircraftData cdm;
	check(VsmrVsid::TryGetAircraftData(" afr123 ", vsid) && vsid.sid == "BUB6B" && vsid.runway == "26R",
		"production vSID polling publishes normalized aircraft snapshots");
	check(VsmrRampAgent::TryGetAircraftData("AFR123", ramp) && ramp.stand == "K12" && ramp.remark == "Contact apron",
		"production Ramp Agent polling publishes stand and remark");
	check(VsmrCdm::TryGetAircraftData("AFR123", cdm) && cdm.tsat == 742 && cdm.manualCtot == true,
		"production CDM polling reads its declared scalar fields despite absent optional fields");
	auto state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.parisCommandsAvailable && state.regionalCommandsAvailable && state.automaticMode == true && state.paris && state.paris->linked == true,
		"schema 1.3 preserves manual Paris controls and reads published LFPG state");
	state = VsmrVsid::GetInterfaceState("LFPO");
	check(state.paris && state.paris->linked == false,
		"LFPO has its own published link selection");
	for (const auto& item : std::map<std::string, std::string>{ { "LFPN", "wlpg" }, { "LFPV", "elpg" }, { "LFPT", "wipg" }, { "LFOB", "eipg" } })
	{
		state = VsmrVsid::GetInterfaceState(item.first);
		check(state.paris && VsmrParis::RegionalRule(*state.paris) == item.second,
			"each regional selection survives the shared bridge client merge");
	}
	const int before = Reads;
	poll();
	check(Reads == before, "unchanged revisions and callsigns skip provider reads");
	std::string error;
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::ParisWIPG, "LFPT", error) && SubmittedCommand == ".vsid paris LFPT wipg",
		"regional manual commands keep their companion command spelling");
	const auto completeCommand = [&] {
		SubmissionResult = VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed;
		poll();
	};
	completeCommand();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgGroundCrossing, "LFPG", error) && SubmittedCommand == ".vsid area LFPG OFF",
		"LFPG Ground Crossing uses the native area command without a new bridge schema");
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.paris && state.paris->linked == true && !state.lastSubmittedLfpgTaxiMode && state.commandLineBusy,
		"pending taxi command neither changes link state nor claims a completed taxi selection");
	completeCommand();
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.paris && state.paris->linked == true && state.lastSubmittedLfpgTaxiMode == VsmrVsid::LfpgTaxiMode::GroundCrossing,
		"completed Ground Crossing is recorded independently of Linked");
	AcceptCommand = false;
	check(!VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPG", error) &&
		VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode == VsmrVsid::LfpgTaxiMode::GroundCrossing,
		"rejecting the first area command preserves the previously submitted selection");
	AcceptCommand = true;
	check(!VsmrVsid::GetInterfaceState("LFPO").lastSubmittedLfpgTaxiMode,
		"LFPG's last area command is not displayed at other airports");
	const auto beforeLinked = SubmittedCommands.size();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgLinked, "LFPG", error) && SubmittedCommands.size() == beforeLinked,
		"clicking the published Linked selection does not toggle opposing");
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgUnlinked, "LFPG", error) && SubmittedCommand == ".vsid rule LFPG opposing",
		"changing link state uses only the opposing rule");
	completeCommand();
	Published[4].text = "LFPG=?UM;";
	++Revision;
	poll();
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.paris && state.paris->linked == false && state.lastSubmittedLfpgTaxiMode == VsmrVsid::LfpgTaxiMode::GroundCrossing,
		"published Unlinked does not change the taxi-row highlight");
	const auto beforeUnlinked = SubmittedCommands.size();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgUnlinked, "LFPG", error) && SubmittedCommands.size() == beforeUnlinked,
		"clicking the published Unlinked selection does not toggle opposing");
	for (int repeat = 0; repeat < 2; ++repeat)
	{
		const auto first = SubmittedCommands.size();
		check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPG", error) && SubmittedCommand == ".vsid area LFPG OFF",
			"each Minimum Taxiing request first resets areas to avoid inverting an existing selection");
		check(!VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgLinked, "LFPG", error) && SubmittedCommands.size() == first + 1U,
			"another command cannot interleave the pending area sequence");
		poll();
		check(SubmittedCommands.size() == first + 1U,
			"a pending command never advances or retries its sequence");
		completeCommand();
		check(SubmittedCommand == ".vsid area LFPG NORTH" && !VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode,
			"NORTH is enabled only after OFF is consumed, without prematurely highlighting Minimum Taxiing");
		completeCommand();
		check(SubmittedCommand == ".vsid area LFPG SOUTH" && VsmrVsid::GetInterfaceState("LFPG").commandLineBusy,
			"SOUTH follows NORTH and keeps the sequence busy until consumed");
		completeCommand();
		state = VsmrVsid::GetInterfaceState("LFPG");
		check(state.paris && state.paris->linked == false && state.lastSubmittedLfpgTaxiMode == VsmrVsid::LfpgTaxiMode::MinimumTaxiing && !state.commandLineBusy,
			"completed Minimum Taxiing preserves Unlinked and records only a local command selection");
	}
	Published[4].text = "LFPG=?LM;";
	++Revision;
	poll();
	check(VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode == VsmrVsid::LfpgTaxiMode::MinimumTaxiing,
		"returning to Linked preserves the Minimum Taxiing highlight");
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPG", error), "start ambiguous-delivery test");
	const auto beforeAmbiguous = SubmittedCommands.size();
	SubmissionResult = VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous;
	poll();
	poll();
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(!state.lastSubmittedLfpgTaxiMode && !state.commandLineBusy && SubmittedCommands.size() == beforeAmbiguous,
		"ambiguous area delivery aborts without retrying toggles or claiming a selection");
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPG", error), "start failed-followup test");
	AcceptCommand = false;
	completeCommand();
	AcceptCommand = true;
	check(!VsmrVsid::GetInterfaceState("LFPG").commandLineBusy && !VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode,
		"failed followup stops the sequence with no completed taxi selection");
	Published[4].text = "malformed";
	++Revision;
	poll();
	check(!VsmrVsid::GetInterfaceState("LFPG").paris,
		"malformed Paris snapshots clear the displayed selection");
	Providers["vsid"] = 1U;
	Published[4].declared = false;
	++Revision;
	poll();
	poll(); // The first stale field read invalidates bindings; the next resolves surviving fields.
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.providerReady && !state.parisCommandsAvailable && !state.regionalCommandsAvailable && !state.paris,
		"older vSID providers retain aircraft data without unsupported Paris controls");
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgGroundCrossing, "LFPG", error) && SubmittedCommand == ".vsid area LFPG OFF",
		"native LFPG area commands work even without companion Paris support");
	completeCommand();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgLinked, "LFPG", error) && SubmittedCommand == ".vsid rule LFPG opposing",
		"native LFPG link command works without companion Paris support and leaves selection unknown");
	completeCommand();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::ReloadConfiguration, "LFPG", error), "reload remains available");
	check(!VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode,
		"reload clears the remembered taxi command because config may change area defaults");
	completeCommand();
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgMinimumTaxiing, "LFPG", error), "start provider-loss test");
	const auto beforeUnload = SubmittedCommands.size();
	Providers.erase("vsid");
	SubmissionResult = VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed;
	poll();
	check(SubmittedCommands.size() == beforeUnload && !VsmrVsid::GetInterfaceState("LFPG").lastSubmittedLfpgTaxiMode &&
		!VsmrVsid::GetInterfaceState("LFPG").commandLineBusy,
		"provider loss cancels remaining area toggles before dispatch and clears local selection");
	check(!VsmrVsid::TryGetAircraftData("AFR123", vsid) && VsmrRampAgent::TryGetAircraftData("AFR123", ramp) && VsmrCdm::TryGetAircraftData("AFR123", cdm),
		"unloading vSID clears only its provider snapshot");
	Providers["vsid"] = 3U;
	Published[4].declared = true;
	Published[4].text = "LFPG=?UM;";
	poll();
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.regionalCommandsAvailable && state.paris && state.paris->linked == false,
		"reloading vSID restores controls and reads current manual state");
	tick.callsigns.clear();
	poll();
	check(!VsmrVsid::TryGetAircraftData("AFR123", vsid) && !VsmrRampAgent::TryGetAircraftData("AFR123", ramp) && !VsmrCdm::TryGetAircraftData("AFR123", cdm),
		"disconnects evict aircraft from every provider even with unchanged revisions");
	tick.api = nullptr;
	HostAttachState = VsmrPluginBridge::AttachState::NotLoaded;
	poll();
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(!state.bridgeLoaded && !state.providerReady && !state.paris && !state.parisCommandsAvailable,
		"a missing bridge clears the manual configuration snapshot and capabilities");
	VsmrVsid::Shutdown();
	VsmrCdm::Shutdown();
	VsmrRampAgent::Shutdown();
}
