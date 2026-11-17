#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/CdmBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	using VsmrPluginBridge::AttachState;
	using VsmrPluginBridge::FieldSpec;
	using VsmrPluginBridge::ProviderState;
	using VsmrPluginBridge::ReadStatus;

	// CDM schema 1.0, declared by kCdmBridgeFields in CDMSingle.cpp
	// (IWantPizzaa/CDM commit 1df65062). Schema 1.2 adds the optional global airport panel,
	// schema 1.3 the optional "released" flag the DEL's Ready Start-up toggles.
	constexpr char ProviderId[] = "com.viffsys.cdm";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
	{
		Tobt,
		Tsat,
		Ttot,
		Ctot,
		Tsac,
		Asrt,
		Asat,
		Released,
		Deice,
		TobtSetBy,
		FlowRestriction,
		EcfmpRestriction,
		ManualCtot,
		AirportPanelField,
		FieldCount
	};

	// Times are I64 minutes since midnight UTC, the unit
	// VsmrCdm::FormatTimeToken renders.
	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "tobt", ESB_T_I64, 0U },
		{ "tsat", ESB_T_I64, 0U },
		{ "ttot", ESB_T_I64, 0U },
		{ "ctot", ESB_T_I64, 0U },
		{ "tsac", ESB_T_I64, 0U },
		{ "asrt", ESB_T_I64, 0U },
		{ "asat", ESB_T_I64, 0U },
		{ "released", ESB_T_I64, 0U },
		{ "deice", ESB_T_STR, 32U },
		{ "tobt_set_by", ESB_T_STR, 16U },
		{ "flow_restriction", ESB_T_STR, 512U },
		{ "ecfmp_restriction", ESB_T_STR, 64U },
		{ "manual_ctot", ESB_T_BOOL, 0U },
		{ "airport_panel", ESB_T_STR, static_cast<std::uint32_t>(VsmrCdm::MaximumPanelBytes) }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("CDM");

	// Snapshot shared with rendering and the datalink workflows.
	std::mutex StateMutex;
	std::unordered_map<std::string, VsmrCdm::AircraftData> AircraftByCallsign;
	std::atomic<bool> ProviderReady{ false };
	std::optional<VsmrCdm::AirportPanel> PanelSnapshot;
	struct MasterChange { bool master; ULONGLONG started; };
	std::map<std::string, MasterChange> PendingMasters;
	void (*UiRefreshCallback)() = nullptr;
	bool CommandRefreshPending = false;
	VsmrPluginBridge::AttachState LastAttach = VsmrPluginBridge::AttachState::NotLoaded;
	bool LastReady = false;

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;

	bool DisconnectProvider()
	{
		VsmrEuroScopeCommandLine::Cancel(VsmrEuroScopeCommandLine::Owner::Cdm);
		Provider.Reset();
		ProviderReady.store(false, std::memory_order_relaxed);
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty() && !PanelSnapshot && PendingMasters.empty())
			return false;
		AircraftByCallsign.clear();
		PanelSnapshot.reset();
		PendingMasters.clear();
		return true;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrCdm::AircraftData> next)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == next)
			return false;
		AircraftByCallsign = std::move(next);
		return true;
	}
}

namespace VsmrCdm
{
namespace
{
bool PollState(const VsmrPluginBridge::Tick& tick, bool interfaceOnly)
{
	try
	{
		if (tick.api == nullptr)
			return DisconnectProvider();
		const ESB_Api_v1& api = *tick.api;

		// CDM is optional: an absent provider is a normal configuration (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return DisconnectProvider();
		ProviderReady.store(true, std::memory_order_relaxed);

		// Coarse gate (B2.5): every CDM write or clear advances the provider revision.
		const std::uint64_t providerRevision = api.provider_revision(ProviderId);
		if (!Provider.SourceChanged() && providerRevision == LastProviderRevision &&
			(interfaceOnly || tick.callsigns == LastScannedCallsigns))
		{
			return false;
		}

		bool snapshotComplete = true;
		bool panelChanged = false;
		std::optional<AirportPanel> nextPanel;
		if (Provider.Field(AirportPanelField) != ESB_FIELD_NONE)
		{
			std::string raw;
			const auto status = VsmrPluginBridge::ReadGlobalString(api, Provider.Field(AirportPanelField),
				static_cast<std::uint32_t>(MaximumPanelBytes), raw);
			if (status == ReadStatus::ProviderLost) return DisconnectProvider();
			if (status == ReadStatus::Value) nextPanel = ParseAirportPanel(raw);
			if (status == ReadStatus::Failed) snapshotComplete = false;
		}
		{
			std::lock_guard<std::mutex> guard(StateMutex);
			panelChanged = PanelSnapshot != nextPanel;
			PanelSnapshot = std::move(nextPanel);
		}
		if (interfaceOnly) return panelChanged;
		std::unordered_map<std::string, AircraftData> next;
		for (const std::string& callsign : tick.callsigns)
		{
			ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
			// An aircraft the bridge has not seen cannot carry published values yet.
			if (VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft) != ReadStatus::Value)
				continue;

			AircraftData data;
			bool providerLost = false;
			// B2.3: a field that did not resolve with its expected type is never read.
			const auto readable = [&](Field field)
			{
				return !providerLost && Provider.Field(field) != ESB_FIELD_NONE;
			};
			// B2.8: unset leaves the value empty; only failures force a rescan.
			const auto accept = [&](ReadStatus status)
			{
				if (status == ReadStatus::ProviderLost)
					providerLost = true;
				else if (status == ReadStatus::Failed)
					snapshotComplete = false;
				return status == ReadStatus::Value;
			};
			const auto readTime = [&](Field field, std::optional<std::int64_t>& value)
			{
				std::int64_t minutes = 0;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftInteger(
					api, callsign, aircraft, Provider.Field(field), minutes)))
				{
					value = minutes;
				}
			};
			const auto readText = [&](Field field, std::string& value)
			{
				std::string raw;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftString(
					api, callsign, aircraft, Provider.Field(field),
					Fields[field].expectedBytes, raw)))
				{
					value = NormalizeStringField(raw);
				}
			};
			const auto readFlag = [&](Field field, std::optional<bool>& value)
			{
				bool flag = false;
				if (readable(field) && accept(VsmrPluginBridge::ReadAircraftBoolean(
					api, callsign, aircraft, Provider.Field(field), flag)))
				{
					value = flag;
				}
			};
			readTime(Tobt, data.tobt);
			readTime(Tsat, data.tsat);
			readTime(Ttot, data.ttot);
			readTime(Ctot, data.ctot);
			readTime(Tsac, data.tsac);
			readTime(Asrt, data.asrt);
			readTime(Asat, data.asat);
			data.releasedDeclared = Provider.Field(Released) != ESB_FIELD_NONE;
			readTime(Released, data.released);
			readText(Deice, data.deice);
			readText(TobtSetBy, data.tobtSetBy);
			readText(FlowRestriction, data.flowRestriction);
			readText(EcfmpRestriction, data.ecfmpRestriction);
			readFlag(ManualCtot, data.manualCtot);
			if (providerLost)
				return DisconnectProvider();
			if (HasPublishedAircraftData(data))
				next.emplace(callsign, std::move(data));
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete ? providerRevision : NoRevision;
		LastScannedCallsigns = tick.callsigns;
		return ReplaceSnapshot(std::move(next)) || panelChanged;
	}
	catch (const std::exception& exception)
	{
		Logger::info("CDM bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("CDM bridge poll failed: unknown exception");
	}
	return DisconnectProvider();
}

bool PollCommandProgress()
{
	const auto result = VsmrEuroScopeCommandLine::Poll(VsmrEuroScopeCommandLine::Owner::Cdm);
	if (result == VsmrEuroScopeCommandLine::SubmissionStatus::Idle) return false;
	bool changed = result == VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed ||
		result == VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous;
	if (result != VsmrEuroScopeCommandLine::SubmissionStatus::Pending)
		changed = PollState({ VsmrPluginBridge::AttachForUi(), {} }, true) || changed;
	CommandRefreshPending = CommandRefreshPending || changed;
	if (changed && UiRefreshCallback) UiRefreshCallback();
	return changed;
}
} // namespace
} // namespace VsmrCdm

bool VsmrCdm::Poll(const VsmrPluginBridge::Tick& tick)
{
	bool changed = PollCommandProgress();
	changed = PollState(tick, false) || changed || CommandRefreshPending;
	CommandRefreshPending = false;
	const auto attach = VsmrPluginBridge::GetAttachState();
	const bool ready = ProviderReady.load(std::memory_order_relaxed);
	changed = changed || attach != LastAttach || ready != LastReady;
	LastAttach = attach; LastReady = ready;
	std::lock_guard<std::mutex> guard(StateMutex);
	for (auto entry = PendingMasters.begin(); entry != PendingMasters.end();)
	{
		const auto found = PanelSnapshot ? std::find_if(PanelSnapshot->airports.begin(), PanelSnapshot->airports.end(),
			[&](const AirportStatus& airport) { return airport.icao == entry->first; }) : std::vector<AirportStatus>::iterator{};
		if (::GetTickCount64() - entry->second.started >= 15000 ||
			(PanelSnapshot && found != PanelSnapshot->airports.end() && found->local == entry->second.master))
		{
			entry = PendingMasters.erase(entry); changed = true;
		}
		else ++entry;
	}
	return changed;
}

void VsmrCdm::SetUiRefreshCallback(void (*callback)()) noexcept { UiRefreshCallback = callback; }

bool VsmrCdm::SubmitCommand(CommandAction action, const std::string& airport, std::string& error)
{
	error.clear();
	const auto state = GetInterfaceState();
	if (!state.bridgeLoaded) error = VsmrPluginBridge::MissingBridgeMessage();
	else if (!state.bridgeCompatible || !state.providerReady) error = "A compatible CDM provider is not available through the bridge.";
	else if (!state.panel) error = "Update CDM to a build that publishes its airport panel (bridge schema 1.2).";
	else if (state.commandLineBusy) error = "EuroScope is still processing another vSMR command.";
	else if (state.proxy && (action == CommandAction::NativePanel || action == CommandAction::Atfcm || action == CommandAction::Capacity))
		error = "Open CDM floating panels in the primary EuroScope instance.";
	if (!error.empty()) return false;
	if (RequiresAirport(action))
	{
		const auto found = std::find_if(state.panel->airports.begin(), state.panel->airports.end(),
			[&](const AirportStatus& value) { return value.icao == airport; });
		if (found == state.panel->airports.end()) { error = "Select a configured CDM airport."; return false; }
		if (found->pending) { error = "CDM is still confirming this airport's Master change."; return false; }
		if ((action == CommandAction::Master && found->local) || (action == CommandAction::Slave && !found->local)) return true;
	}
	const auto command = BuildCommand(action, airport);
	if (command.empty()) { error = "Invalid CDM action or airport."; return false; }
	const bool accepted = state.proxy
		? VsmrPluginBridge::SubmitProxyCdm(static_cast<int>(action), airport, error)
		: VsmrEuroScopeCommandLine::Begin(VsmrEuroScopeCommandLine::Owner::Cdm, command, &error, PollCommandProgress);
	if (accepted && (action == CommandAction::Master || action == CommandAction::Slave))
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		PendingMasters[airport] = { action == CommandAction::Master, ::GetTickCount64() };
	}
	return accepted;
}

bool VsmrCdm::TryGetAircraftData(
	const std::string& callsign,
	AircraftData& outData)
{
	const std::string normalizedCallsign = VsmrPluginBridge::NormalizeCallsign(callsign);
	if (normalizedCallsign.empty())
		return false;
	std::lock_guard<std::mutex> guard(StateMutex);
	const auto found = AircraftByCallsign.find(normalizedCallsign);
	if (found == AircraftByCallsign.end())
		return false;
	outData = found->second;
	return true;
}

VsmrCdm::InterfaceState VsmrCdm::GetInterfaceState()
{
	InterfaceState state;
	const AttachState attachState = VsmrPluginBridge::GetAttachState();
	state.bridgeLoaded = attachState != AttachState::NotLoaded;
	state.bridgeCompatible = attachState == AttachState::Attached;
	state.providerReady = state.bridgeCompatible &&
		ProviderReady.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		state.aircraftCount = AircraftByCallsign.size();
		state.panel = PanelSnapshot;
		if (state.panel) for (auto& airport : state.panel->airports)
			airport.pending = airport.pending || PendingMasters.count(airport.icao) != 0U;
	}
	state.commandLineBusy = VsmrEuroScopeCommandLine::IsBusy() || VsmrPluginBridge::ProxyCommandPending();
	state.proxy = VsmrPluginBridge::IsProxyBridge();
	return state;
}

void VsmrCdm::Shutdown() noexcept
{
	UiRefreshCallback = nullptr;
	CommandRefreshPending = false;
	LastAttach = VsmrPluginBridge::AttachState::NotLoaded; LastReady = false;
	VsmrEuroScopeCommandLine::Cancel(VsmrEuroScopeCommandLine::Owner::Cdm);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
		PanelSnapshot.reset();
		PendingMasters.clear();
	}
	Provider.Reset();
	Diagnostics.Reset();
	ProviderReady.store(false, std::memory_order_relaxed);
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
}
