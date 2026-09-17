# EuroScope Plugin Bridge data

vSMR reads optional data that other EuroScope plug-ins publish through [EuroScope Plugin Bridge](https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge). It is a consumer only: it registers no provider, holds no subscription, and never reads the relay's remote values. Every provider is optional. When the bridge or a plug-in is missing, the related tokens stay empty and everything else keeps working.

## Consumed fields

| Provider | Fields | Type | vSMR use |
| --- | --- | --- | --- |
| `vsid` 1.x | `sid`, `rwy`, `cfl` | aircraft STR, 32 bytes | `vsid_sid`, `vsid_rwy`, `vsid_cfl` tokens and the vSID Rules source |
| `vsid` 1.1 | `automode` | global STR, 4096 bytes | Runtime Menu Auto mode ([record format](vsid-automode.md)) |
| `vsid` companion 1.2/1.3 | `paris` | global STR, 54 bytes | Manual Paris Config selections ([record format](vsid-paris.md)); 1.3 enables the regional buttons |
| `rampagent` 1.x | `stand` | aircraft STR, 32 bytes | `uk_stand` token |
| `rampagent` 1.x | `remark` | aircraft STR, 128 bytes | `remark` token |
| `com.viffsys.cdm` 1.x | `tobt`, `tsat`, `ttot`, `ctot`, `tsac`, `asrt`, `asat` | aircraft I64, minutes since midnight UTC | CDM time tokens, CDM Rules, `ready_startup`, PDC TSAT/CTOT |
| `com.viffsys.cdm` 1.x | `deice`, `tobt_set_by`, `flow_restriction`, `ecfmp_restriction` | aircraft STR | CDM Rules source |
| `com.viffsys.cdm` 1.x | `manual_ctot` | aircraft BOOL | CDM Rules source |

`uk_stand` and `remark` come only from Ramp Agent; vSMR does not read flight strip annotations 3 and 4. Ramp Agent text is kept as validated UTF-8, which the tag renderer draws directly.

The CDM fields match `kCdmBridgeFields` in [the bridge-enabled CDM build](https://github.com/IWantPizzaa/CDM/blob/1df65062/CDMSingle.cpp). CDM strings have respective caps of 32, 16, 512 and 64 bytes. Builds without that provider leave CDM data unavailable.

Paris configuration remains manual. The merged client reads the companion's `vsid/paris` snapshot to display selections and retains the existing command buttons. It does not derive rules from EuroScope runways or edit those runways.

## Consumer procedure

`vSMR/src/integrations/PluginBridgeClient.cpp` is the only translation unit that defines `ESB_CLIENT_SHIM`. On every EuroScope timer tick vSMR:

1. Calls `ESB_Attach()`. It never attaches from a constructor and never loads the bridge itself.
2. Scans EuroScope's flight plans once for all providers.
3. For each provider, checks `provider_version()` (major 1 only), then resolves any unresolved field with its expected type. A missing provider, an undeclared field or a type mismatch disables only what it affects and is logged once.
4. Skips the provider when neither `provider_revision()` nor the callsign set changed.
5. Resolves each aircraft handle from its callsign and reads each field. It re-resolves once on `ESB_E_STALE_AIRCRAFT` and resizes once on `ESB_E_BUFFER_TOO_SMALL`. `ESB_E_UNSET` means no value; `ESB_E_NO_PROVIDER` drops the provider's resolved fields until it returns.

Rendering, the Runtime Menu and the datalink workflows read snapshots only, so no bridge call leaves the EuroScope main thread. `vSMR/tests/PluginBridgeTests.cpp` checks bridge reads and `PluginBridgePollingTests.cpp` exercises the production provider pollers against scripted data. The latter replaces only host attachment, diagnostics and command dispatch; live host behavior still requires verification inside EuroScope.

## Verify in EuroScope

- `.esb schema rampagent` and `.esb schema vsid` list the fields above.
- `.esb get rampagent/stand <callsign>` matches that aircraft's `uk_stand` token.
- Unload Ramp Agent, vSID, then the bridge itself: vSMR keeps running and only the related tokens clear.
