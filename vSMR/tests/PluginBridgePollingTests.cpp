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
	SubmittedCommand = command;
	return true;
}
VsmrEuroScopeCommandLine::SubmissionStatus VsmrEuroScopeCommandLine::Poll(Owner) { return SubmissionStatus::Idle; }
bool VsmrEuroScopeCommandLine::IsBusy() noexcept { return false; }
void VsmrEuroScopeCommandLine::Cancel(Owner) noexcept { SubmittedCommand.clear(); }

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
	check(VsmrVsid::SubmitCommand(VsmrVsid::CommandAction::LfpgGroundCrossing, "LFPG", error) && SubmittedCommand == ".vsid paris LFPG unlinked",
		"LFPG Ground Crossing remains available through the shared bridge client");
	state = VsmrVsid::GetInterfaceState("LFPG");
	check(state.paris && state.paris->linked == true,
		"command submission does not fabricate an authoritative Paris selection");
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
	Providers.erase("vsid");
	poll();
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
