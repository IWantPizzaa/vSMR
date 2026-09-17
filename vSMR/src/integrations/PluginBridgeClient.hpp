#pragma once

#include "integrations/PluginBridgeReads.hpp"

#include <esbridge.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace EuroScopePlugIn
{
	class CPlugIn;
}

namespace VsmrPluginBridge
{
	enum class AttachState : std::uint8_t
	{
		NotLoaded,    // EuroScopeBridge.dll is not in EuroScope's plug-in list (yet)
		Incompatible, // loaded, but it does not serve the ABI v1 members vSMR calls
		Attached
	};

	// Bridge access for one EuroScope timer tick. The API table is used only on the
	// EuroScope main thread (A8) and is never kept past the tick.
	struct Tick
	{
		const ESB_Api_v1* api = nullptr;
		// Normalized callsigns of EuroScope's live flight plans, minus callsigns that
		// disconnected since their last flight-plan update. Empty without a bridge.
		std::unordered_set<std::string> callsigns;
	};

	// EuroScope OnTimer only. Attaches lazily through ESB_Attach() instead of at
	// construction, because EuroScope may load the bridge after vSMR (A4).
	Tick BeginTick(EuroScopePlugIn::CPlugIn& plugin);

	// Any thread: the attach state seen by the latest BeginTick().
	AttachState GetAttachState() noexcept;

	// The shared esbridge.h wording, so a user running several bridge-aware
	// plug-ins reads one instruction (A7).
	const char* MissingBridgeMessage() noexcept;

	std::string NormalizeCallsign(const std::string& callsign);

	// EuroScope flight-plan callbacks. A disconnected callsign stays hidden from
	// every provider until its flight plan updates again, so a flight plan
	// EuroScope still lists cannot show values from the ended connection.
	void ForgetAircraft(const std::string& callsign);
	void ObserveAircraft(const std::string& callsign);

	// vSMR only polls: it registers no provider and holds no subscription, so the
	// bridge has nothing to release (A10, B2.10). This resets vSMR's own state.
	void Shutdown() noexcept;

	// EuroScope log entries for one provider's state changes and for fields rejected
	// at resolve time, each written once per change (B2.2, B2.3, B2.4).
	class ProviderDiagnostics
	{
	public:
		explicit ProviderDiagnostics(const char* displayName) noexcept :
			m_displayName(displayName)
		{
		}

		void Report(const ProviderBinding& provider);
		void Reset() noexcept;

	private:
		const char* m_displayName;
		ProviderState m_state = ProviderState::Absent;
		std::vector<ESB_Status> m_fieldStatuses;
	};
}
