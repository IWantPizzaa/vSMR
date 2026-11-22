#pragma once

#include <map>
#include <string>

struct ESB_Api_v1;

// vSMR's only bridge provider: "vsmr", publishing the PINEDE PDC state of each
// aircraft as vsmr/dcl (see DatalinkBridgeData.hpp for the values). Every other
// bridge client in vSMR only reads.
namespace VsmrDatalinkBridge
{
	// EuroScope OnTimer only, with the api of this tick's VsmrPluginBridge::BeginTick
	// (nullptr without a bridge). flags maps callsigns to the Flag* bits; an aircraft
	// missing from it has its value cleared. Writes only what changed since the last
	// call. Does nothing on the proxy instance's read-only bridge facade.
	void Publish(const ESB_Api_v1* api, const std::map<std::string, unsigned>& flags);

	// Drops every published value with the provider registration.
	void Shutdown() noexcept;
}
