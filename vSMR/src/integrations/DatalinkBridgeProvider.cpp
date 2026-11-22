#include "platform/windows/PrecompiledHeader.hpp"

// No ESB_CLIENT_SHIM: PluginBridgeClient.cpp is the only translation unit that
// instantiates it. The api table arrives from its BeginTick().
#include <esbridge.h>

#include "integrations/DatalinkBridgeData.hpp"
#include "integrations/DatalinkBridgeProvider.hpp"
#include "integrations/PluginBridgeClient.hpp"

#include "shared/logging/Logger.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>

namespace
{
	constexpr const char* ProviderId = "vsmr";
	constexpr const char* DclField = "dcl";

	const ESB_FieldDecl ProvidedFields[] = {
		{
			DclField,
			ESB_T_STR,
			ESB_SCOPE_AIRCRAFT,
			// Not SYNC: the state belongs to the PINEDE station of this controller.
			// Not DENSE: only aircraft that asked for a PDC carry one.
			0U,
			1U,
			"PDC state: R request, S standby, T pilot message, C sent, W wilco; unset = none",
		},
	};

	// Members the provider calls, up to clear_ac. The proxy instance's bridge
	// facade is read-only and leaves the registration members null.
	constexpr std::size_t MinimumApiSize =
		offsetof(ESB_Api_v1, clear_ac) + sizeof(decltype(ESB_Api_v1::clear_ac));

	// Everything issued by the bridge the provider registered on, dropped as one
	// when that bridge goes away or a different one shows up.
	const ESB_Api_v1* RegisteredApi = nullptr;
	ESB_Provider* Provider = nullptr;
	ESB_FieldId Dcl = ESB_FIELD_NONE;

	// What vsmr/dcl currently holds, by normalized callsign
	std::map<std::string, char> Published;

	// Another live module owns "vsmr": said once, never retried
	bool ProviderConflict = false;

	void* SelfModule() noexcept
	{
		HMODULE self = nullptr;
		::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&SelfModule),
			&self);
		return self;
	}

	bool ServesProviderMembers(const ESB_Api_v1& api) noexcept
	{
		return api.abi_version == ESB_ABI_VERSION &&
			api.struct_size >= MinimumApiSize &&
			api.register_provider != nullptr &&
			api.unregister_provider != nullptr &&
			api.own_field != nullptr &&
			api.aircraft != nullptr &&
			api.set_ac != nullptr &&
			api.clear_ac != nullptr;
	}

	void Forget() noexcept
	{
		RegisteredApi = nullptr;
		Provider = nullptr;
		Dcl = ESB_FIELD_NONE;
		Published.clear();
	}

	bool Register(const ESB_Api_v1& api)
	{
		ESB_ProviderDecl decl = {};
		decl.struct_size = sizeof decl;
		decl.provider_id = ProviderId;
		decl.schema_major = 1U;
		decl.schema_minor = 0U;
		decl.display_name = "vSMR";
		decl.contact = "https://github.com/AlexisBalzano/vSMR";
		decl.fields = ProvidedFields;
		decl.field_count = static_cast<std::uint32_t>(std::size(ProvidedFields));
		decl.module = SelfModule();

		ESB_Provider* provider = nullptr;
		const ESB_Status status = api.register_provider(&decl, &provider);
		if (status != ESB_OK)
		{
			if (status == ESB_E_PROVIDER_TAKEN)
			{
				ProviderConflict = true;
				Logger::info("Another loaded plug-in owns the \"vsmr\" bridge provider; vsmr/dcl is not published");
			}
			return false;
		}

		// Only an owned handle carries write authority; resolve() would not.
		ESB_FieldId field = ESB_FIELD_NONE;
		if (api.own_field(provider, DclField, &field) != ESB_OK || field == ESB_FIELD_NONE)
		{
			api.unregister_provider(provider);
			return false;
		}

		RegisteredApi = &api;
		Provider = provider;
		Dcl = field;
		Published.clear();
		return true;
	}

	// False when the value could not be written, so the next tick tries again.
	bool Write(const ESB_Api_v1& api, const std::string& callsign, char code)
	{
		ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
		if (api.aircraft(callsign.c_str(), &aircraft) != ESB_OK)
			return false;
		if (code == '\0')
		{
			// UNSET, UNKNOWN or STALE all leave the field without a value
			api.clear_ac(Provider, aircraft, Dcl);
			return true;
		}
		const char text[2] = { code, '\0' };
		ESB_Value value = {};
		value.type = ESB_T_STR;
		value.bytes = 1U;
		value.v.ptr = text;
		return api.set_ac(Provider, aircraft, Dcl, &value) == ESB_OK;
	}
}

void VsmrDatalinkBridge::Publish(const ESB_Api_v1* api, const std::map<std::string, unsigned>& flags)
{
	if (api == nullptr || VsmrPluginBridge::IsProxyBridge() || !ServesProviderMembers(*api))
	{
		// No bridge in this process, or only the proxy facade: nothing to write to,
		// and a registration from an earlier bridge is not ours to use any more.
		Forget();
		return;
	}
	if (api != RegisteredApi)
		Forget();
	if (Provider == nullptr && (ProviderConflict || !Register(*api)))
		return;

	std::map<std::string, char> wanted;
	for (const auto& [rawCallsign, bits] : flags)
	{
		const char code = StateCode(bits);
		std::string callsign = VsmrPluginBridge::NormalizeCallsign(rawCallsign);
		if (code != '\0' && !callsign.empty())
			wanted[std::move(callsign)] = code;
	}

	for (auto it = Published.begin(); it != Published.end();)
	{
		if (wanted.count(it->first) != 0U)
		{
			++it;
			continue;
		}
		// An aircraft that left the network took its value with it; either way
		// there is nothing left to clear.
		Write(*api, it->first, '\0');
		it = Published.erase(it);
	}

	for (const auto& [callsign, code] : wanted)
	{
		const auto published = Published.find(callsign);
		if (published != Published.end() && published->second == code)
			continue;
		if (Write(*api, callsign, code))
			Published[callsign] = code;
		else if (published != Published.end())
			Published.erase(published);
	}
}

void VsmrDatalinkBridge::Shutdown() noexcept
{
	// The bridge pins its own module, so a table it handed out stays mapped; the
	// module test only covers a bridge that was never really there.
	if (Provider != nullptr && RegisteredApi != nullptr && ::GetModuleHandleA(ESB_MODULE_NAME) != nullptr)
		RegisteredApi->unregister_provider(Provider);
	Forget();
	ProviderConflict = false;
}
