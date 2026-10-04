#include "platform/windows/PrecompiledHeader.hpp"

#include "integrations/VsidBridgeClient.hpp"
#include "integrations/PluginBridgeClient.hpp"
#include "integrations/PluginBridgeReads.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include "shared/logging/Logger.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
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

	// vSID's provider declaration (vSIDPlugin.h): schema 1.0 publishes sid, rwy and
	// cfl as aircraft STR fields of at most 32 bytes. automode is the optional
	// schema 1.1 global described in the Wiki Integrations page. The
	// Legacy companion schemas 1.2-1.4 publish Paris-specific data. Upstream's
	// generic schema 1.1 instead declares rules/areas/autoconfig; resolve by field.
	constexpr char ProviderId[] = "vsid";
	constexpr std::uint32_t SupportedSchemaMajor = 1U;

	enum Field : std::size_t
	{
		SidField,
		RunwayField,
		ClearedFlightLevelField,
		AutomaticModeField,
		ParisStateField,
		LfpgTaxiField,
		RulesField,
		AreasField,
		AutoConfigurationField,
		FieldCount
	};

	constexpr std::array<FieldSpec, FieldCount> Fields = { {
		{ "sid", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "rwy", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "cfl", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumFieldBytes) },
		{ "automode", ESB_T_STR, static_cast<std::uint32_t>(VsmrVsid::MaximumAutomaticModeBytes) },
		{ "paris", ESB_T_STR, static_cast<std::uint32_t>(VsmrParis::Airports.size() * 9U) },
		{ "lfpg_taxi", ESB_T_STR, 1U },
		{ "rules", ESB_T_STR, 65536U },
		{ "areas", ESB_T_STR, 65536U },
		{ "autoconfig", ESB_T_STR, 65536U }
	} };

	constexpr std::uint64_t NoRevision = (std::numeric_limits<std::uint64_t>::max)();

	VsmrPluginBridge::ProviderBinding Provider(
		ProviderId,
		SupportedSchemaMajor,
		Fields.data(),
		Fields.size());
	VsmrPluginBridge::ProviderDiagnostics Diagnostics("vSID");

	// Snapshot shared with rendering, the Runtime Menu and command submission.
	std::mutex StateMutex;
	std::map<std::string, bool> AutomaticModes;
	std::map<std::string, VsmrParis::State> ParisStates;
	VsmrVsid::AirportRuleValues AirportRules;
	VsmrVsid::ConfigurationStatuses ConfigurationStates;
	bool GenericConfigurationAvailable = false;
	std::optional<VsmrVsid::LfpgTaxiMode> LastSubmittedLfpgTaxiMode;
	bool LiveLfpgTaxiAvailable = false;
	bool HasPublishedLfpgTaxiAreas = false;
	std::optional<VsmrVsid::LfpgTaxiMode> LiveLfpgTaxiMode;
	std::unordered_map<std::string, VsmrVsid::AircraftData> AircraftByCallsign;
	std::atomic<bool> ParisCommandsAvailable{ false };
	std::atomic<bool> RegionalCommandsAvailable{ false };
	std::atomic<bool> ProviderReady{ false };

	// Timer-only polling state.
	std::uint64_t LastProviderRevision = NoRevision;
	std::unordered_set<std::string> LastScannedCallsigns;
	bool InterfaceStateInitialized = false;
	AttachState LastAttachState = AttachState::NotLoaded;
	bool LastProviderReady = false;

	// Commands run on the EuroScope thread, one at a time. Never retry a toggle
	// after ambiguous delivery. Renderers only read the busy flag and last result.
	std::deque<std::string> PendingAreaCommands;
	std::optional<VsmrVsid::LfpgTaxiMode> PendingTaxiMode;
	std::atomic<bool> AreaSequenceActive{ false };
	void (*UiRefreshCallback)() = nullptr;
	// A native-window callback can request a repaint, but must not consume the
	// host's notification: EuroScope may defer/coalesce that request while its
	// clickable screen-object list is still the disabled frame's list.
	bool CommandRefreshPending = false;

	void ClearAreaSequence()
	{
		PendingAreaCommands.clear();
		PendingTaxiMode.reset();
		AreaSequenceActive.store(false, std::memory_order_relaxed);
	}

	bool DisconnectProvider()
	{
		VsmrEuroScopeCommandLine::Cancel(VsmrEuroScopeCommandLine::Owner::Vsid);
		ClearAreaSequence();
		Provider.Reset();
		ProviderReady.store(false, std::memory_order_relaxed);
		ParisCommandsAvailable.store(false, std::memory_order_relaxed);
		RegionalCommandsAvailable.store(false, std::memory_order_relaxed);
		LastProviderRevision = NoRevision;
		LastScannedCallsigns.clear();
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign.empty() && AutomaticModes.empty() && ParisStates.empty() && !LastSubmittedLfpgTaxiMode && !LiveLfpgTaxiAvailable && !GenericConfigurationAvailable && ConfigurationStates.empty())
			return false;
		AircraftByCallsign.clear();
		AutomaticModes.clear();
		ParisStates.clear();
		AirportRules.clear();
		ConfigurationStates.clear();
		GenericConfigurationAvailable = false;
		LastSubmittedLfpgTaxiMode.reset();
		LiveLfpgTaxiAvailable = false;
		HasPublishedLfpgTaxiAreas = false;
		LiveLfpgTaxiMode.reset();
		return true;
	}

	bool UpdateInterfaceState()
	{
		const AttachState attachState = VsmrPluginBridge::GetAttachState();
		const bool providerReady = ProviderReady.load(std::memory_order_relaxed);
		const bool changed = !InterfaceStateInitialized ||
			attachState != LastAttachState ||
			providerReady != LastProviderReady;
		InterfaceStateInitialized = true;
		LastAttachState = attachState;
		LastProviderReady = providerReady;
		return changed;
	}

	bool ReplaceSnapshot(
		std::unordered_map<std::string, VsmrVsid::AircraftData> aircraft,
		std::map<std::string, bool> automaticModes,
		std::map<std::string, VsmrParis::State> parisStates,
		bool liveTaxiAvailable, std::optional<VsmrVsid::LfpgTaxiMode> taxiMode, bool hasTaxiAreas,
		bool genericAvailable, VsmrVsid::AirportRuleValues rules, VsmrVsid::ConfigurationStatuses states)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		if (AircraftByCallsign == aircraft && AutomaticModes == automaticModes && ParisStates == parisStates &&
			LiveLfpgTaxiAvailable == liveTaxiAvailable && LiveLfpgTaxiMode == taxiMode && HasPublishedLfpgTaxiAreas == hasTaxiAreas &&
			GenericConfigurationAvailable == genericAvailable && AirportRules == rules && ConfigurationStates == states)
			return false;
		AircraftByCallsign = std::move(aircraft);
		AutomaticModes = std::move(automaticModes);
		ParisStates = std::move(parisStates);
		LiveLfpgTaxiAvailable = liveTaxiAvailable;
		LiveLfpgTaxiMode = taxiMode;
		HasPublishedLfpgTaxiAreas = hasTaxiAreas;
		GenericConfigurationAvailable = genericAvailable;
		AirportRules = std::move(rules);
		ConfigurationStates = std::move(states);
		return true;
	}
}

namespace VsmrVsid
{
namespace
{
bool PollState(const VsmrPluginBridge::Tick& tick, bool configurationOnly);

bool PollCommandProgress()
{
	const bool changed = PollState({ VsmrPluginBridge::AttachForUi(), {} }, true);
	CommandRefreshPending = CommandRefreshPending || changed;
	if (changed && UiRefreshCallback)
		UiRefreshCallback();
	return changed;
}

bool PollState(const VsmrPluginBridge::Tick& tick, bool configurationOnly)
{
	bool commandStateChanged = false;
	const auto submission = VsmrEuroScopeCommandLine::Poll(VsmrEuroScopeCommandLine::Owner::Vsid);
	switch (submission)
	{
	case VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed:
		Logger::info("vSID command consumed by EuroScope");
		commandStateChanged = true;
		break;
	case VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous:
		Logger::info("vSID command submission could not be confirmed");
		commandStateChanged = true;
		break;
	default:
		break;
	}
	// The short-lived UI timer waits for delivery without repeatedly scanning
	// aircraft or reading configuration while EuroScope is still processing.
	if (configurationOnly && submission == VsmrEuroScopeCommandLine::SubmissionStatus::Pending)
		return false;
	auto finish = [&](bool dataChanged)
	{
		const bool interfaceStateChanged = UpdateInterfaceState();
		return dataChanged || commandStateChanged || interfaceStateChanged;
	};

	try
	{
		if (tick.api == nullptr)
			return finish(DisconnectProvider());
		const ESB_Api_v1& api = *tick.api;

		// vSID is optional: an absent provider is a normal configuration (B2.2).
		const ProviderState state = Provider.Refresh(api);
		Diagnostics.Report(Provider);
		if (state != ProviderState::Ready)
			return finish(DisconnectProvider());
		ProviderReady.store(true, std::memory_order_relaxed);
		if (PendingTaxiMode)
		{
			if (submission == VsmrEuroScopeCommandLine::SubmissionStatus::Confirmed)
			{
				if (PendingAreaCommands.empty())
				{
					std::lock_guard<std::mutex> guard(StateMutex);
					LastSubmittedLfpgTaxiMode = PendingTaxiMode;
					ClearAreaSequence();
				}
				else
				{
					std::string error;
					if (VsmrEuroScopeCommandLine::Begin(VsmrEuroScopeCommandLine::Owner::Vsid,
						PendingAreaCommands.front(), &error, PollCommandProgress))
						PendingAreaCommands.pop_front();
					else
					{
						Logger::info("LFPG area sequence stopped: " + error);
						ClearAreaSequence();
					}
				}
			}
			else if (submission == VsmrEuroScopeCommandLine::SubmissionStatus::Ambiguous ||
				submission == VsmrEuroScopeCommandLine::SubmissionStatus::Idle)
			{
				Logger::info("LFPG area sequence stopped without retrying; verify areas in vSID.");
				ClearAreaSequence();
				commandStateChanged = true;
			}
		}
		const bool genericAvailable = Provider.Field(RulesField) != ESB_FIELD_NONE;
		const bool parisCommands = genericAvailable || SupportsParisCommands(Provider.SchemaMajor(), Provider.SchemaMinor());
		const bool regionalCommands = genericAvailable || SupportsRegionalCommands(Provider.SchemaMajor(), Provider.SchemaMinor());
		commandStateChanged = (ParisCommandsAvailable.exchange(parisCommands, std::memory_order_relaxed) != parisCommands) || commandStateChanged;
		commandStateChanged = (RegionalCommandsAvailable.exchange(regionalCommands, std::memory_order_relaxed) != regionalCommands) || commandStateChanged;

		// Coarse gate (B2.5): every vSID write or clear, global or per aircraft,
		// advances the provider revision.
		const std::uint64_t providerRevision = api.provider_revision(ProviderId);
		if (!configurationOnly && !commandStateChanged && providerRevision == LastProviderRevision &&
			tick.callsigns == LastScannedCallsigns)
		{
			return finish(false);
		}

		bool snapshotComplete = true;
		std::map<std::string, bool> automaticModes;
		if (Provider.Field(AutomaticModeField) != ESB_FIELD_NONE)
		{
			std::string snapshot;
			const ReadStatus automaticStatus = VsmrPluginBridge::ReadGlobalString(
				api,
				Provider.Field(AutomaticModeField),
				Fields[AutomaticModeField].expectedBytes,
				snapshot);
			if (automaticStatus == ReadStatus::ProviderLost)
				return finish(DisconnectProvider());
			if (automaticStatus == ReadStatus::Failed)
				snapshotComplete = false;
			// Unset or malformed snapshots stay Unknown, never an inferred Off state.
			if (automaticStatus == ReadStatus::Value)
				automaticModes = ParseAutomaticModes(snapshot);
		}

		std::map<std::string, VsmrParis::State> parisStates;
		if (Provider.Field(ParisStateField) != ESB_FIELD_NONE)
		{
			std::string snapshot;
			const ReadStatus parisStatus = VsmrPluginBridge::ReadGlobalString(
				api, Provider.Field(ParisStateField), Fields[ParisStateField].expectedBytes, snapshot);
			if (parisStatus == ReadStatus::ProviderLost)
				return finish(DisconnectProvider());
			if (parisStatus == ReadStatus::Failed)
				snapshotComplete = false;
			// Published runtime rules establish the selection regardless of Auto/manual.
			if (parisStatus == ReadStatus::Value)
				parisStates = VsmrParis::Parse(snapshot);
		}

		const bool liveTaxiAvailable = Provider.Field(AreasField) != ESB_FIELD_NONE || Provider.Field(LfpgTaxiField) != ESB_FIELD_NONE;
		std::optional<VsmrVsid::LfpgTaxiMode> taxiMode;
		if (Provider.Field(LfpgTaxiField) != ESB_FIELD_NONE)
		{
			std::string snapshot;
			const ReadStatus taxiStatus = VsmrPluginBridge::ReadGlobalString(
				api, Provider.Field(LfpgTaxiField), Fields[LfpgTaxiField].expectedBytes, snapshot);
			if (taxiStatus == ReadStatus::ProviderLost) return finish(DisconnectProvider());
			if (taxiStatus == ReadStatus::Failed) snapshotComplete = false;
			if (taxiStatus == ReadStatus::Value) taxiMode = ParseLfpgTaxiMode(snapshot);
		}

		AirportRuleValues ruleValues;
		ConfigurationStatuses configurationStates;
		bool hasTaxiAreas = false;
		for (const Field field : {RulesField, AreasField, AutoConfigurationField})
		{
			if (Provider.Field(field) == ESB_FIELD_NONE) continue;
			std::string snapshot;
			const auto result = VsmrPluginBridge::ReadGlobalString(api, Provider.Field(field), Fields[field].expectedBytes, snapshot);
			if (result == ReadStatus::ProviderLost) return finish(DisconnectProvider());
			if (result == ReadStatus::Failed) snapshotComplete = false;
			if (field == RulesField)
			{
				// Generic values are authoritative, even when unset/malformed.
				parisStates.clear();
				if (result == ReadStatus::Value)
				{
					ruleValues = ParseRuleValues(snapshot);
					for (const auto& [icao, rules] : ruleValues)
						if (VsmrParis::Supports(icao)) parisStates[icao] = ResolveConfiguration(icao, rules);
				}
			}
			else if (field == AreasField)
			{
				const auto areas = result == ReadStatus::Value ? ParseRuleValues(snapshot) : AirportRuleValues{};
				taxiMode = ResolveLfpgTaxi(areas);
				hasTaxiAreas = HasLfpgTaxiAreas(areas);
			}
			else if (result == ReadStatus::Value)
				configurationStates = ParseConfigurationStatuses(snapshot);
		}

		if (configurationOnly)
		{
			// Refresh button selections immediately, but leave aircraft snapshots
			// and the full-poll revision gate for the regular EuroScope timer.
			bool changed;
			{
				std::lock_guard<std::mutex> guard(StateMutex);
				changed = AutomaticModes != automaticModes || ParisStates != parisStates ||
					LiveLfpgTaxiAvailable != liveTaxiAvailable || LiveLfpgTaxiMode != taxiMode || HasPublishedLfpgTaxiAreas != hasTaxiAreas ||
					GenericConfigurationAvailable != genericAvailable || AirportRules != ruleValues || ConfigurationStates != configurationStates;
				AutomaticModes = std::move(automaticModes);
				ParisStates = std::move(parisStates);
				LiveLfpgTaxiAvailable = liveTaxiAvailable;
				LiveLfpgTaxiMode = taxiMode;
				HasPublishedLfpgTaxiAreas = hasTaxiAreas;
				GenericConfigurationAvailable = genericAvailable;
				AirportRules = std::move(ruleValues);
				ConfigurationStates = std::move(configurationStates);
			}
			return finish(changed);
		}

		std::unordered_map<std::string, AircraftData> next;
		for (const std::string& callsign : tick.callsigns)
		{
			ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
			// An aircraft the bridge has not seen cannot carry published values yet.
			if (VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft) != ReadStatus::Value)
				continue;

			AircraftData data;
			bool providerLost = false;
			const auto readField = [&](Field field, std::string& value)
			{
				// B2.3: a field that did not resolve with its expected type is never read.
				if (providerLost || Provider.Field(field) == ESB_FIELD_NONE)
					return;
				std::string raw;
				switch (VsmrPluginBridge::ReadAircraftString(
					api,
					callsign,
					aircraft,
					Provider.Field(field),
					Fields[field].expectedBytes,
					raw))
				{
				case ReadStatus::Value:
					value = NormalizeFieldValue(raw);
					break;
				case ReadStatus::ProviderLost:
					providerLost = true;
					break;
				case ReadStatus::Failed:
					snapshotComplete = false;
					break;
				default:
					// B2.8: unset means vSID holds no value for this aircraft.
					break;
				}
			};
			readField(SidField, data.sid);
			readField(RunwayField, data.runway);
			readField(ClearedFlightLevelField, data.clearedFlightLevel);
			if (providerLost)
				return finish(DisconnectProvider());
			// A bridge aircraft handle can exist without vSID publishing data for it.
			// Such handles are not connected vSID aircraft and must not inflate status.
			if (HasPublishedAircraftData(data))
				next.emplace(callsign, std::move(data));
		}

		// Retry incomplete reads even when the provider revision did not advance.
		LastProviderRevision = snapshotComplete ? providerRevision : NoRevision;
		LastScannedCallsigns = tick.callsigns;
		return finish(ReplaceSnapshot(std::move(next), std::move(automaticModes), std::move(parisStates), liveTaxiAvailable, taxiMode, hasTaxiAreas, genericAvailable, std::move(ruleValues), std::move(configurationStates)));
	}
	catch (const std::exception& exception)
	{
		Logger::info("vSID bridge poll failed: " + std::string(exception.what()));
	}
	catch (...)
	{
		Logger::info("vSID bridge poll failed: unknown exception");
	}
	return finish(DisconnectProvider());
}
}
}

bool VsmrVsid::Poll(const VsmrPluginBridge::Tick& tick)
{
	const bool pendingRefresh = std::exchange(CommandRefreshPending, false);
	const bool changed = PollState(tick, false);
	return changed || pendingRefresh;
}

void VsmrVsid::SetUiRefreshCallback(void (*callback)()) noexcept
{
	UiRefreshCallback = callback;
}

VsmrVsid::InterfaceState VsmrVsid::GetInterfaceState(const std::string& airport)
{
	InterfaceState state;
	const AttachState attachState = VsmrPluginBridge::GetAttachState();
	state.bridgeLoaded = attachState != AttachState::NotLoaded;
	state.bridgeCompatible = attachState == AttachState::Attached;
	state.providerReady = state.bridgeCompatible &&
		ProviderReady.load(std::memory_order_relaxed);
	state.parisCommandsAvailable = state.providerReady && ParisCommandsAvailable.load(std::memory_order_relaxed);
	state.regionalCommandsAvailable = state.providerReady && RegionalCommandsAvailable.load(std::memory_order_relaxed);
	state.commandLineBusy = VsmrEuroScopeCommandLine::IsBusy() || AreaSequenceActive.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		state.aircraftCount = AircraftByCallsign.size();
		const auto automatic = AutomaticModes.find(NormalizeAirport(airport));
		if (state.providerReady && automatic != AutomaticModes.end())
			state.automaticMode = automatic->second;
		state.genericConfigurationAvailable = state.providerReady && GenericConfigurationAvailable;
		const auto rules = AirportRules.find(NormalizeAirport(airport));
		if (state.genericConfigurationAvailable && rules != AirportRules.end()) state.rules = rules->second;
		const auto configuration = ConfigurationStates.find(NormalizeAirport(airport));
		if (state.providerReady && configuration != ConfigurationStates.end()) state.configuration = configuration->second;
		if (state.genericConfigurationAvailable)
		{
			const auto icao = NormalizeAirport(airport);
			state.parisCommandsAvailable = !BuildGenericRuleCommand(icao == "LFOB" ? CommandAction::BeauvaisEast :
				VsmrParis::IsRegional(icao) ? CommandAction::ParisWLPG : CommandAction::LfpgLinked, icao, state.rules).empty();
			state.regionalCommandsAvailable = state.parisCommandsAvailable;
		}
		const auto paris = ParisStates.find(NormalizeAirport(airport));
		if (state.providerReady && paris != ParisStates.end()) state.paris = paris->second;
		if (state.providerReady && NormalizeAirport(airport) == "LFPG")
		{
			state.lfpgTaxiCommandsAvailable = !state.genericConfigurationAvailable || HasPublishedLfpgTaxiAreas;
			state.lastSubmittedLfpgTaxiMode = LastSubmittedLfpgTaxiMode;
			state.liveLfpgTaxiAvailable = LiveLfpgTaxiAvailable;
			state.lfpgTaxiMode = LiveLfpgTaxiMode;
		}
	}
	return state;
}

bool VsmrVsid::SubmitCommand(
	CommandAction action,
	const std::string& activeAirport,
	std::string& error)
{
	error.clear();
	const InterfaceState state = GetInterfaceState(activeAirport);
	if (!state.bridgeLoaded)
	{
		error = VsmrPluginBridge::MissingBridgeMessage();
		return false;
	}
	if (!state.bridgeCompatible)
	{
		error = "The loaded EuroScope Plugin Bridge is incompatible.";
		return false;
	}
	if (!state.providerReady)
	{
		error = "A compatible vSID provider is not available through the bridge.";
		return false;
	}

	if (state.commandLineBusy)
	{
		error = "EuroScope is still processing another vSMR command.";
		return false;
	}
	if (IsLfpgTaxiAction(action) && !state.lfpgTaxiCommandsAvailable)
	{
		error = "LFPG has not published the NORTH and SOUTH areas required for taxi controls.";
		return false;
	}
	if (action == CommandAction::ResumeAutoConfiguration &&
		(!state.genericConfigurationAvailable || !CanResumeConfiguration(state.configuration)))
	{
		error = "This airport has no active automatic-configuration override to resume.";
		return false;
	}
	auto commands = BuildCommandSequence(action, activeAirport);
	if (state.genericConfigurationAvailable && IsParisAction(action) && !IsLfpgTaxiAction(action))
	{
		const auto command = BuildGenericRuleCommand(action, activeAirport, state.rules);
		if (command.empty())
		{
			error = "The airport has not published the rules required for this configuration.";
			return false;
		}
		commands = {command};
	}
	if (commands.empty())
	{
		error = "Select a valid four-character airport before using this vSID action.";
		return false;
	}
	const bool nativeLfpgAction = NormalizeAirport(activeAirport) == "LFPG" &&
		(IsLfpgTaxiAction(action) || action == CommandAction::LfpgLinked || action == CommandAction::LfpgUnlinked);
	if (IsParisAction(action) && !nativeLfpgAction && !state.parisCommandsAvailable)
	{
		error = "Paris runway controls require the companion vSID build and airport configuration.";
		return false;
	}
	if (IsRegionalAction(action) && !state.regionalCommandsAvailable)
	{
		error = "Regional configuration buttons require the companion vSID build with bridge schema 1.3.";
		return false;
	}
	// opposing is a toggle: clicking an already published selection is a no-op.
	if (!state.genericConfigurationAvailable && nativeLfpgAction && state.paris &&
		((action == CommandAction::LfpgLinked && state.paris->linked == true) ||
		 (action == CommandAction::LfpgUnlinked && state.paris->linked == false))) return true;
	if (!VsmrEuroScopeCommandLine::Begin(
		VsmrEuroScopeCommandLine::Owner::Vsid,
		commands.front(),
		&error, PollCommandProgress))
	{
		return false;
	}
	if (IsLfpgTaxiAction(action))
	{
		PendingAreaCommands.assign(commands.begin() + 1, commands.end());
		PendingTaxiMode = action == CommandAction::LfpgMinimumTaxiing
			? LfpgTaxiMode::MinimumTaxiing : LfpgTaxiMode::GroundCrossing;
		AreaSequenceActive.store(true, std::memory_order_relaxed);
	}
	if (IsLfpgTaxiAction(action) || action == CommandAction::ReloadConfiguration)
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		LastSubmittedLfpgTaxiMode.reset();
	}
	return true;
}

bool VsmrVsid::TryGetAircraftData(
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

void VsmrVsid::Shutdown() noexcept
{
	UiRefreshCallback = nullptr;
	CommandRefreshPending = false;
	ClearAreaSequence();
	VsmrEuroScopeCommandLine::Cancel(
		VsmrEuroScopeCommandLine::Owner::Vsid);
	{
		std::lock_guard<std::mutex> guard(StateMutex);
		AircraftByCallsign.clear();
		AutomaticModes.clear();
		ParisStates.clear();
		AirportRules.clear();
		ConfigurationStates.clear();
		GenericConfigurationAvailable = false;
		LastSubmittedLfpgTaxiMode.reset();
		LiveLfpgTaxiAvailable = false;
		HasPublishedLfpgTaxiAreas = false;
		LiveLfpgTaxiMode.reset();
	}
	Provider.Reset();
	Diagnostics.Reset();
	ProviderReady.store(false, std::memory_order_relaxed);
	ParisCommandsAvailable.store(false, std::memory_order_relaxed);
	RegionalCommandsAvailable.store(false, std::memory_order_relaxed);
	LastProviderRevision = NoRevision;
	LastScannedCallsigns.clear();
	InterfaceStateInitialized = false;
	LastAttachState = AttachState::NotLoaded;
	LastProviderReady = false;
}
