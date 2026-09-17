# Paris airport manual configuration

The vSMR vSID popup provides **Linked** and **Unlinked** for LFPG and LFPO. LFPN, LFPV, LFPT and LFOB provide **WL**, **EL**, **IPGW** and **IPOW**, from LFPG's point of view.

LFPG also provides **Minimum Taxiing** and **Ground Crossing** buttons in CONFIG. They use the existing LFPG actions: Minimum Taxiing selects linked rules, and Ground Crossing selects unlinked rules.

All selections are manual. There is no Auto runways button, automatic runway comparison, or timer-driven rule assignment. EuroScope runway edits do not change these custom rules. This integration does not read, modify, or lock EuroScope's runway selections.

vSID's separate **Auto mode** control still governs automatic SID assignment using the manually selected rules.

| Regional selection | vSID rule | Meaning from LFPG's point of view |
| --- | --- | --- |
| WL | `wlpg` | West Lie |
| EL | `elpg` | East Lie |
| IPGW | `wipg` | West Inverse |
| IPOW | `eipg` | East Inverse |

A selection remains active until changed manually. It survives vSID's normal airport reloads; restarting EuroScope restores defaults from the airport JSON configuration. The CONFIG section highlights the active choice from the actual custom rules published by vSID. It has no separate Selected status line. Missing or conflicting regional flags leave all choices unhighlighted and are left unchanged.

Equivalent commands:

```text
.vsid paris LFPG linked
.vsid paris LFPO unlinked
.vsid paris LFPN wlpg
.vsid paris LFPV elpg
.vsid paris LFPT wipg
.vsid paris LFOB eipg
```

Link commands are limited to LFPG/LFPO; the four regional commands apply to LFPN/LFPV/LFPT/LFOB. The former `.vsid paris ICAO auto` command is rejected without changing rules. Historical LFPG action IDs remain aliases for manual link selections.

## Airport configuration

Run the migration against the airport configuration directory:

```powershell
python vSMR/tools/configure_vsid_paris.py 'C:\Users\mathi\AppData\Roaming\EuroScope\LFXX\Plugins\vSID\vSID AirportsConfig'
```

It removes obsolete `paris_auto` and `paris_manual_config` metadata, preserves existing procedure flags, and ensures the manual link and regional options exist. Every modified file is backed up under the sibling `Backups/paris-<timestamp>` folder. All files are parsed before any changes are made; repeated runs are harmless.

When the adjacent `vSIDConfig.json` exists, the migration also checks its `airportConfigs` path and backs up the file before correcting it to the selected directory. For this installation the folder is `vSID AirportsConfig/`.

LFPG/LFPO selections update `linked`, `unlinked`, and the existing `opposing` procedure flag. Regional selections enable exactly one of the four configuration flags. LFOB's existing `pgeast` flag follows the chosen east/west component; its existing procedures therefore remain identical for linked/inverse choices within the same direction. SID routes, runway data, priorities, equipment and climb restrictions are preserved.

For startup defaults, set `opposing` at LFPG/LFPO and keep `linked`/`unlinked` consistent with it. At regional airports, enable exactly one of the four configuration flags (and keep LFOB's `pgeast` consistent). Obsolete automatic metadata in older, unmigrated files is ignored and excluded from SID filtering.

## Companion build

The companion targets [AlexisBalzano/vSID at fce87a0](https://github.com/AlexisBalzano/vSID/tree/fce87a08db147d25ff92b041456cc93cf83a32d1). From a clean checkout at that revision:

1. Apply [vsid-automode.patch](vsid-automode.patch).
2. Apply [vsid-paris.patch](vsid-paris.patch).
3. Copy [ParisIntegration.inc](vsid-paris/ParisIntegration.inc) and [ParisRunwayRules.hpp](../../vSMR/src/integrations/ParisRunwayRules.hpp) into the vSID repository root.
4. Configure with CMake for Win32 and build Release using MSVC with C++20 support.
5. Close EuroScope, back up the installed vSID DLL, and replace it with the resulting `vSID.dll`. Install the matching vSMR release as well, preserving existing configuration files.

The vSID source remains under its upstream GPL license. The companion is built separately from vSMR and still requires EuroScope Plugin Bridge. The patch reuses vSID's existing flight-plan reprocessing path only after a manual selection changes custom rules. Timer callbacks publish state without changing rules.

The schema 1.3 companion retains the schema 1.2 global field `vsid/paris`: at most six nine-byte records, for example `LFPN=WLA;`. Each record contains the airport, PG configuration component (`W`, `E`, `?`), link state (`L`, `U`, `?`), and mode (`A`, `M`). The direction reports the manually selected regional rule; the companion always publishes mode M. Missing or malformed data clears vSMR's displayed state; command consumption never establishes state.

## Validation

Release builds and native regressions cover explicit selections, mutually exclusive regional flags, LFOB compatibility, read-only status resolution, unchanged selections, unsupported commands, legacy metadata and bridge snapshot parsing. Migration tests run with:

```powershell
python -B vSMR/tests/test_vsid_paris_config.py
```

After restarting EuroScope with the updated plugins, select each manual option, edit EuroScope runway selections, and confirm the selected custom rule remains unchanged. Confirm Auto runways is absent and vSID's separate Auto mode still works.

For RDF, `.smr rdf off` now immediately hides the overlay without waiting for network shutdown; `.smr rdf on` restores it, including an ongoing transmission. The TrackAudio receiver stays available while display is disabled. The change avoids joining a worker from the command callback while it may be blocked in a [synchronous WebSocket receive](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpwebsocketreceive). Verify off/on during an active transmission and while TrackAudio is idle.

If the separate RDFPlugin is also loaded, its independent overlay can remain visible after `.smr rdf off`. Apply [RDF-vSMR-ground-view.patch](../../vSMR/data/Tools/RDF-vSMR-ground-view.patch) when building RDFPlugin to exclude `SMR radar display`, leaving vSMR responsible for RDF there. Other radar displays and the separate plugin's audio bridge retain their existing behavior. See [RDF companion build instructions](rdf-smr.md).
