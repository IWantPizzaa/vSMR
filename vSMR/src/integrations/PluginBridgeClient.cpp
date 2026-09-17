#include "platform/windows/PrecompiledHeader.hpp"

// The only translation unit that instantiates the esbridge.h client shim (A2). It
// must come before any header that includes esbridge.h without the shim, or the
// include guard would silently drop ESB_Attach().
#define ESB_CLIENT_SHIM
#include <esbridge.h>

#include "integrations/PluginBridgeClient.hpp"

#include "EuroScopePlugIn.h"
#include "shared/TextUtils.hpp"
#include "shared/logging/Logger.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <utility>

namespace
{
	using namespace EuroScopePlugIn;
	using VsmrPluginBridge::AttachState;
	using VsmrPluginBridge::ProviderState;

	constexpr std::size_t MaximumFlightPlans = 4096U;
	// vSMR calls ABI v1 members up to provider_revision. A newer v1 bridge may be
	// larger, but never drops a released member (esbridge.h ABI rules).
	constexpr std::size_t MinimumApiSize =
		offsetof(ESB_Api_v1, provider_revision) + sizeof(decltype(ESB_Api_v1::provider_revision));

	std::atomic<AttachState> CurrentAttachState{ AttachState::NotLoaded };
	std::unordered_set<std::string> ForgottenCallsigns;
	bool ScanFailureLogged = false;

	bool ServesRequiredMembers(const ESB_Api_v1& api) noexcept
	{
		return api.abi_version == ESB_ABI_VERSION &&
			api.struct_size >= MinimumApiSize &&
			api.resolve != nullptr &&
			api.provider_version != nullptr &&
			api.get_global != nullptr &&
			api.aircraft != nullptr &&
			api.get_ac != nullptr &&
			api.provider_revision != nullptr;
	}

	void CollectCallsigns(CPlugIn& plugin, std::unordered_set<std::string>& callsigns)
	{
		std::unordered_set<std::string> listedForgotten;
		std::size_t flightPlanCount = 0U;
		CFlightPlan flightPlan = plugin.FlightPlanSelectFirst();
		for (; flightPlan.IsValid() && flightPlanCount < MaximumFlightPlans;
			flightPlan = plugin.FlightPlanSelectNext(flightPlan), ++flightPlanCount)
		{
			const char* rawCallsign = flightPlan.GetCallsign();
			std::string callsign = VsmrPluginBridge::NormalizeCallsign(
				rawCallsign != nullptr ? rawCallsign : "");
			if (callsign.empty())
				continue;
			if (ForgottenCallsigns.count(callsign) != 0U)
			{
				listedForgotten.insert(std::move(callsign));
				continue;
			}
			if (flightPlan.GetFPState() == FLIGHT_PLAN_STATE_TERMINATED ||
				flightPlan.GetSimulated())
			{
				continue;
			}
			callsigns.insert(std::move(callsign));
		}
		// A forgotten callsign EuroScope no longer lists cannot show stale values
		// any more; dropping it keeps the set bounded over a long session.
		if (!flightPlan.IsValid())
			ForgottenCallsigns = std::move(listedForgotten);
	}
}

VsmrPluginBridge::Tick VsmrPluginBridge::BeginTick(EuroScopePlugIn::CPlugIn& plugin)
{
	Tick tick;
	const ESB_Api_v1* api = ESB_Attach();
	if (api == nullptr || !ServesRequiredMembers(*api))
	{
		// ESB_Attach() returns NULL both without a bridge and when the bridge will
		// not serve ABI v1. GetModuleHandleA only asks the loader; it never loads
		// the bridge (A6).
		const bool loaded = api != nullptr || ::GetModuleHandleA(ESB_MODULE_NAME) != nullptr;
		CurrentAttachState.store(
			loaded ? AttachState::Incompatible : AttachState::NotLoaded,
			std::memory_order_relaxed);
		return tick;
	}
	CurrentAttachState.store(AttachState::Attached, std::memory_order_relaxed);
	tick.api = api;

	try
	{
		CollectCallsigns(plugin, tick.callsigns);
		ScanFailureLogged = false;
	}
	catch (const std::exception& exception)
	{
		tick.callsigns.clear();
		if (!ScanFailureLogged)
			Logger::info("Plug-in bridge flight-plan scan failed: " + std::string(exception.what()));
		ScanFailureLogged = true;
	}
	catch (...)
	{
		tick.callsigns.clear();
		if (!ScanFailureLogged)
			Logger::info("Plug-in bridge flight-plan scan failed: unknown exception");
		ScanFailureLogged = true;
	}
	return tick;
}

VsmrPluginBridge::AttachState VsmrPluginBridge::GetAttachState() noexcept
{
	return CurrentAttachState.load(std::memory_order_relaxed);
}

const char* VsmrPluginBridge::MissingBridgeMessage() noexcept
{
	return ESB_MISSING_MESSAGE;
}

std::string VsmrPluginBridge::NormalizeCallsign(const std::string& callsign)
{
	return ToUpperAsciiCopy(TrimAsciiWhitespaceCopy(callsign));
}

void VsmrPluginBridge::ForgetAircraft(const std::string& callsign)
{
	std::string normalized = NormalizeCallsign(callsign);
	if (!normalized.empty())
		ForgottenCallsigns.insert(std::move(normalized));
}

void VsmrPluginBridge::ObserveAircraft(const std::string& callsign)
{
	const std::string normalized = NormalizeCallsign(callsign);
	if (!normalized.empty())
		ForgottenCallsigns.erase(normalized);
}

void VsmrPluginBridge::Shutdown() noexcept
{
	ForgottenCallsigns.clear();
	ScanFailureLogged = false;
	CurrentAttachState.store(AttachState::NotLoaded, std::memory_order_relaxed);
	// A new plug-in instance attaches again on its first tick. The bridge pins its
	// own module, so the cached table could not dangle either way.
	esb_api = nullptr;
}

void VsmrPluginBridge::ProviderDiagnostics::Report(const ProviderBinding& provider)
{
	const ProviderState state = provider.State();
	const auto providerName = [&]
	{
		return std::string(m_displayName) + " bridge provider \"" + provider.ProviderId() + "\"";
	};
	if (state != m_state)
	{
		if (state == ProviderState::Ready)
		{
			Logger::info(providerName() + " connected with schema " +
				std::to_string(provider.SchemaMajor()) + "." +
				std::to_string(provider.SchemaMinor()));
		}
		else if (state == ProviderState::UnsupportedSchema)
			Logger::info(providerName() + " uses an unsupported schema major version; its data is ignored");
		else if (state == ProviderState::NoMatchingFields)
			Logger::info(providerName() + " declares none of the fields vSMR reads");
		else if (m_state == ProviderState::Ready)
			Logger::info(providerName() + " is no longer available");
		m_state = state;
	}
	if (state != ProviderState::Ready && state != ProviderState::NoMatchingFields)
		return;

	if (m_fieldStatuses.size() != provider.FieldCount())
		m_fieldStatuses.assign(provider.FieldCount(), ESB_OK);
	for (std::size_t index = 0U; index < provider.FieldCount(); ++index)
	{
		const ESB_Status status = provider.Field(index) != ESB_FIELD_NONE
			? ESB_OK
			: provider.ResolveStatus(index);
		if (status == m_fieldStatuses[index])
			continue;
		m_fieldStatuses[index] = status;

		const std::string fieldName = std::string(m_displayName) + " bridge field \"" +
			provider.ProviderId() + "/" + provider.Spec(index).name + "\"";
		if (status == ESB_E_TYPE_MISMATCH)
			Logger::info(fieldName + " changed type; vSMR does not read it");
		else if (status == ESB_E_NO_FIELD)
			Logger::info(fieldName + " is not declared by the loaded provider");
		else if (status != ESB_OK)
			Logger::info(fieldName + " could not be resolved (status " + std::to_string(status) + ")");
	}
}

void VsmrPluginBridge::ProviderDiagnostics::Reset() noexcept
{
	m_state = ProviderState::Absent;
	m_fieldStatuses.clear();
}
