# Paris airport runway rules

The vSMR Runtime Menu provides **Linked**, **Unlinked**, and **Auto runways** for LFPG, LFPO, LFPN, LFPV, LFPT, and LFOB. These controls require the companion vSID build described below. The indicator reads actual vSID state through the bridge; older providers show the controls as unavailable.

With a compatible provider (schema 1.2 or later within major version 1), buttons are clickable even before the first airport runway-state report arrives. An unknown runway state does not prevent manual commands. The small companion/config message is omitted when no status is available.

**Auto runways** is separate from vSID's automatic SID assignment mode. It follows the active arrival and departure runways in EuroScope:

| LFPG flow | LFPO flow | Link state | Regional rule (PG perspective) |
| --- | --- | --- | --- |
| West (26/27) | West (24/25) | Linked | `wlpg` |
| East (08/09) | East (06/07) | Linked | `elpg` |
| West (26/27) | East (06/07) | Unlinked | `wipg` |
| East (08/09) | West (24/25) | Unlinked | `eipg` |

Only recognized runway ends count. Missing selections, simultaneous opposite flows, and Orly's 02/20 crosswind runway make automatic link state **Unknown**. Existing SID rules remain unchanged until there is an unambiguous configuration. PG and PO do not need to be active vSID airports; their runway selections are read directly from EuroScope's sector data.

Selecting **Linked** or **Unlinked** sets a manual override for the selected airport. It persists across runway changes and vSID's normal airport reloads until **Auto runways** is selected. The regional east/west component continues to follow PG. Runtime selections are not written back to JSON; the configuration supplies defaults after restarting EuroScope.

Equivalent commands, replacing `LFPN` with any supported airport:

```text
.vsid paris LFPN linked
.vsid paris LFPN unlinked
.vsid paris LFPN auto
```

The old LFPG menu had two pairs of buttons that all toggled the same `opposing` rule. The new menu uses one explicit link selection and a separate automatic runway control. Historical LFPG action IDs remain accepted as aliases.

## Airport configuration

Run this once against an existing airport configuration directory:

```powershell
python vSMR/tools/configure_vsid_paris.py 'C:\Users\mathi\AppData\Roaming\EuroScope\LFXX\Plugins\vSID\vSID AirportsConfig'
```

The migration adds `linked`, `unlinked`, and `paris_auto` boolean options to all six airports. Defaults are linked and automatic. Existing values are preserved. It ensures the four regional flags exist at PN/PV/PT/OB, and backs up every changed file under the sibling `Backups/paris-<timestamp>` directory before writing. All files are parsed before any airport is changed. Running the migration again is harmless.

When the adjacent `vSIDConfig.json` exists, the migration also checks its `airportConfigs` setting and points it at the selected directory, backing up the main file before changing it. For this installation the correct value is `vSID AirportsConfig/`; `vSID AirportConfigs/` names a different, nonexistent folder. Restart EuroScope after changing this path, or run `.vsid reload` followed by `.vsid reload ese` to reload the main configuration and then rebuild the active airport data.

The companion maps link state to the existing `opposing` SID rule at LFPG/LFPO. PN/PV/PT select exactly one of `wlpg`, `elpg`, `wipg`, `eipg`. The supplied LFOB procedures use `pgeast`; that compatibility flag is maintained from PG direction alongside the four regional flags. No SID route, runway, priority, equipment, or climb restriction is changed or invented. The three control flags are excluded from vSID's test for active SID rules so they cannot change procedure filtering on their own.

For a manual startup default, set `paris_auto` to false and choose exactly one of `linked` and `unlinked` as true. Prefer the menu or `.vsid paris` commands for runtime overrides; toggling individual derived flags with `.vsid rule` while automatic control is active will be superseded by the next runway update.

## Companion build

The companion targets [AlexisBalzano/vSID at fce87a0](https://github.com/AlexisBalzano/vSID/tree/fce87a08db147d25ff92b041456cc93cf83a32d1). From a clean checkout at that revision:

1. Apply [vsid-automode.patch](vsid-automode.patch).
2. Apply [vsid-paris.patch](vsid-paris.patch).
3. Copy [ParisIntegration.inc](vsid-paris/ParisIntegration.inc) and [ParisRunwayRules.hpp](../../vSMR/src/integrations/ParisRunwayRules.hpp) into the vSID repository root.
4. Configure with CMake for Win32 and build Release using MSVC with C++20 support.
5. Close EuroScope, back up the installed vSID DLL, and replace it with the resulting `vSID.dll`. Install the matching vSMR release as well, preserving existing configuration files.

The vSID source remains under its upstream GPL license. The companion is built separately from vSMR and still requires EuroScope Plugin Bridge. The patch reuses vSID's existing rule-change flight-plan reprocessing path, only when derived rule values change.

The added schema 1.2 global field `vsid/paris` contains at most six nine-byte records, for example `LFPN=WLA;`: airport, PG flow (`W`, `E`, `?`), link state (`L`, `U`, `?`), and mode (`A`, `M`). Missing or malformed data clears vSMR's displayed state; command consumption never establishes state.

## Validation

The native regression suite covers all four flow combinations, exclusive regional selection, manual overrides, returning to Auto, ambiguous runway data, stable updates, airport restrictions, and snapshot parsing. Configuration migration tests run with:

```powershell
python vSMR/tests/test_vsid_paris_config.py
```

After loading the rebuilt plugins in EuroScope, verify the four PG/PO combinations above, manual overrides, and returning to Auto. This live check also confirms sector selections and SID data in the controller's setup.

For RDF, `.smr rdf off` now immediately hides the overlay without waiting for network shutdown; `.smr rdf on` restores it, including an ongoing transmission. The TrackAudio receiver stays available while display is disabled. The change avoids joining a worker from the command callback while it may be blocked in a [synchronous WebSocket receive](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpwebsocketreceive). Verify off/on during an active transmission and while TrackAudio is idle.
