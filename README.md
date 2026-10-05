# vSMR 2.0 for EuroScope

vSMR is a configurable surface-movement radar plug-in for 32-bit EuroScope. It provides airport surface displays, aircraft tags and symbols, AVISO maps, RIMCAS alerts, native inset windows, CDM data, and Hoppie CPDLC/PDC workflows.

Current source version: **2.0.0** (release preparation). This README describes the current source and bundled data; it does not announce publication of the final package. Public packaging remains blocked by the unresolved entries in the [provenance register](vSMR/data/Licenses/ASSET_PROVENANCE.txt).

> Verify the active airport, profile, AVISO map, runway configuration, and alerts before controlling. Development and validation-only packages are not official releases.

[Documentation](https://github.com/IWantPizzaa/vSMR/wiki) | [Releases](https://github.com/IWantPizzaa/vSMR/releases) | [Changelog](CHANGELOG.md) | [Report an issue](https://github.com/IWantPizzaa/vSMR/issues)

## Highlights

- Configurable surface radar with NOVA, aircraft-icon, Triangle, and Diamond targets
- Normal and detailed tags with status-specific layouts, structured color rules, and optional per-line backgrounds
- 160 bundled AVISO airport maps, with shared geometry/text editing and airport-specific palettes
- Resolution presets for AVISO rendering and aircraft icons/tags, without resizing menus or other UI
- Automatic RIMCAS runway assignment from EuroScope's active-airport runway selection
- AVISO, SRW 1, METAR, and Timer inset windows
- METAR detail adapts automatically to the window size and number of airports, down to ICAO, wind and QNH at minimum size. Settings let you choose all open AVISO airports, including CoFrance insets, or one airport at a time. Title-bar arrows switch airport/page without resizing the window; the airport grouping choice is saved with the ASR.
- Opening the HP tag editor immediately sets a ground aircraft to Taxi, even if the point selection is cancelled. Existing scratchpad text and real assigned speeds are preserved; EuroScope ownership restrictions still apply.
- Timer and CPDLC sounds use the files in `vSMR_Data/Audio`, with built-in fallbacks if those files are missing or cannot be played. If alerts remain silent, check the Windows output device and EuroScope's volume/mute setting in the Volume Mixer; vSMR does not override them.
- CDM bridge integration and Hoppie CPDLC/PDC support
- Optional vSID bridge data, tag tokens, rules, and Runtime Menu controls
- Optional Ramp Agent stand and stand remark tag values through the plug-in bridge
- Airport-scoped inset presets and independent active-profile selection for each ASR
- Atomic configuration saves, Revert, bundled-default recovery, diagnostics, and verified-update support

## 2.0.0 behavior and compatibility

- **Resolution:** Settings offers 1080p (100%), 2K (133%), and 4K (200%). These scale AVISO labels/lines and aircraft icons, trails, tags, and their hit areas in the main view and radar insets. They do not resize the Control Center, Runtime Menu, inset controls, weather/timer panels, or FPS display, and do not change geographic positions or radar zoom.
- **Interface theme:** Night/Day UI colors are independent of the AVISO palette and also affect Timer and METAR insets. Dark/Light palettes are available in the current bundled maps; Real is available for LFBO, LFLL, LFML, LFMN, LFPG, LFPO, and LFSB. Other maps may offer different palettes.
- **Target presentation:** Tag label font size is edited in pixels (6-72) instead of preset numbers 1-5. Diamond trails use more closely spaced samples while preserving the selected point count. METAR shows a prominent QNH strip and an inward-fading wind-from indicator. PDC/CPDLC composers distinguish editable light-grey fields from dark read-only fields; X uses the Cancel action.
- **ASR profiles:** Each radar screen keeps its own active profile. Profile definitions and the profiles-file source remain shared; save the ASR to retain its selection.
- **CDM:** The bridge-enabled CDM plug-in replaces the retired vACDM HTTP integration. The old CDM Auto/reminder workflow, timer, and message queue are no longer present. Manual Hoppie CPDLC/PDC workflows remain available.
- **Stand data:** `uk_stand` and `remark` come from Ramp Agent through EuroScope Plugin Bridge, not flight strip annotations 3 and 4. Missing providers leave their related values unavailable without disabling the rest of vSMR.
- **Configuration recovery:** The Control Center has Revert and Bundled defaults. The old Undo/Redo controls and legacy profile `.bak` restoration are no longer available. Keep your own configuration backups.

The current converter-supplied AVISO set contains **160 maps** in [`vSMR/data/AVISO/`](vSMR/data/AVISO/), replacing the previous 192-map set. Maps absent from the replacement set are no longer bundled. LFPG includes restored **East Arrows** and **West Arrows** groups. Those map groups are separate from the vSID configuration controls below.

## Requirements

- Windows with 32-bit EuroScope
- [Microsoft Visual C++ 2015-2022 Redistributable (x86)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
- [Microsoft Edge WebView2 Evergreen Runtime (x86)](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- The complete matching release package: `vSMR.dll` and `vSMR_Data\`

The optional vSID, Ramp Agent, and CDM interfaces require [EuroScope Plugin Bridge](https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge), plus a bridge-enabled [vSID](https://github.com/AlexisBalzano/vSID), [Ramp Agent](https://github.com/AlexisBalzano/EuroscopeRampAgent), or [CDM](https://github.com/IWantPizzaa/CDM) build. Stand and stand remark tag values come only from Ramp Agent through the bridge. Load them separately through EuroScope's plug-in settings; vSMR deliberately does not bundle or load their DLLs. The consumed fields are listed in [EuroScope Plugin Bridge data](https://github.com/IWantPizzaa/vSMR/wiki/Integrations).

[Paris configuration](https://github.com/IWantPizzaa/vSMR/wiki/Paris-vSID-Configuration): vSMR reads the generic `rules`, `areas` and `autoconfig` fields published by current bridge-enabled vSID builds. LFPG/LFPO/LFPB Linked/Unlinked sets `OPPOSING` explicitly; LFPN/LFPV/LFPT WL/EL/IPGW/IPOW selects the four regional rules atomically; LFOB uses CDG West/East to set its `PGEAST` rule. Buttons reflect the actual published values and remain unavailable when their required rules are absent. LFPG's taxi-area controls stay independent of Linked/Unlinked.

Runway-driven **Auto configuration belongs to vSID**: the popup reflects its Auto/manual/error status without changing EuroScope's active airports or runways. A manual rule selection can suspend vSID's automatic configuration for that airport; **Resume auto config** releases that airport's override when automation is enabled. This is separate from vSID's automatic SID assignment, whose former **Auto mode** button remains removed. Older companion builds retain their legacy controls.

For the LFPG configuration with NORTH and SOUTH areas, **Minimum Taxiing** sends `.vsid area LFPG OFF`, then `.vsid area LFPG NORTH`, then `.vsid area LFPG SOUTH`, waiting for each command to be consumed. Resetting first makes repeated clicks enable both areas instead of toggling them off. **Ground Crossing** sends `.vsid area LFPG OFF`. These change no rules. An ambiguous or failed submission stops the sequence without retrying a toggle; inspect vSID's area status before continuing. The generic `areas` snapshot (or legacy `lfpg_taxi` field) drives the taxi-row highlight; mixed/unknown states do not select either button. Older providers without live areas use the **last completed command sequence**, reset on reload/disconnect. NORTH/SOUTH are geographic areas, not alternate names for the modes.

With generic snapshots, Linked/Unlinked sends `.vsid rules ICAO OPPOSING=off` or `OPPOSING=on`. Repeated clicks are explicit assignments, not toggles. Legacy LFPG providers still use `.vsid rule LFPG opposing`: clicking the already published state does nothing; without published status, either button toggles and neither is highlighted. Changing link state never changes the taxi command, and area commands never change the link selection. No installed vSID DLL or configuration files are modified by these UI actions.

vSMR is a EuroScope plug-in, not a standalone application. WebView2 hosts the local Control Center; internet access is needed for online integrations, updates, and GitHub data imports.

## Install or upgrade

1. Choose a published release and download its complete `vSMR-<version>.zip` from [GitHub Releases](https://github.com/IWantPizzaa/vSMR/releases). A development version in this README does not imply its package has been published.
2. Close EuroScope and back up your existing installation, profiles, custom AVISO maps, and ASRs.
3. For a fresh installation, extract the complete matching package into an empty plug-in folder, keeping `vSMR.dll` beside `vSMR_Data\`. For an upgrade, extract it to a separate temporary folder and run its `vSMR_Data\Tools\install_vsmr.ps1 -DestinationDirectory "<existing plug-in folder>"`; this preserves user configuration and creates a rollback backup. Do not extract over edited legacy profiles or mix loader/runtime files from different packages.
4. Start EuroScope and load `vSMR.dll` through its plug-in settings. Load any optional bridge/provider plug-ins separately.

Detailed procedures are maintained in the Wiki:

- [Installation and upgrades](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating)
- [First-time setup and verification](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#install-or-upgrade)
- [Backup and rollback](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#backup-rollback-and-verification)
- [Automatic updates](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#updates-and-custom-data)

## First run

### Updater compatibility and custom data

The development updater uses a per-file `version.json` feed on the repository's `update-feed` branch: `stable/version.json`, or `beta/version.json` when Beta mode is enabled. File downloads use `raw.githubusercontent.com` URLs pinned to the manifest's immutable Git commit. Only missing or changed managed files are downloaded, with size and SHA-256 validation before installation. A push to `dev` or `main` does **not** publish an update feed.

Updates are staged separately from the working installation and applied before the runtime is loaded. A transaction journal and backups support recovery after a failed or interrupted installation; the local `vSMR_Data/version.json` is committed last. Loader DLL changes use a small external apply helper after EuroScope releases the installation. Do not delete `.update` staging/recovery data while an update is pending; complete-package installation and manual backup restoration refuse an unfinished transaction. SHA-256 checks integrity against the manifest; HTTPS and control of the fixed GitHub repository remain the trust boundary, not independent publisher authentication.

Configuration has two layers under `vSMR_Data`: `default.json` contains replaceable application defaults; `config.json` contains user overrides and is **never** included in the managed update manifest. Objects merge recursively, override arrays replace complete default arrays, and a supported explicit `null` is different from a missing key. Resetting a setting to its default removes that override. Profiles use stable internal IDs, independently of editable display names. New default keys become available without rewriting user settings; only structural schema changes need a runtime migration.

Existing profile files are imported conservatively, preserving customized profiles and using matching old defaults when available. Official AVISO geometry is replaceable; user styles/group properties are stored separately, keyed by stable object IDs. When a legacy map cannot be safely compared with a trusted baseline, a user-owned copy is preserved instead of guessing which geometry was customized. External profile/map paths remain user-owned. Keep backups and review migrated profiles/maps before operational use.

Older loaders still use the full ZIP and `.update.json` package mechanism; that remains available for the one-time bridge installation. Existing installations must receive loader **1.3.0**, runtime, `default.json`, and the apply helper together before the per-file feed is used. Runtime ABI remains 1. Never replace only a DLL for this transition.

### Initial configuration

1. Open an SMR radar screen and select the four-letter active-airport ICAO from the Runtime Menu. Right-click the ICAO field to access the five most recent airports for that screen's session.
2. Select a profile and display mode.
3. Open the Control Center with the Runtime Menu or `.smr`.
4. Verify the Profiles and AVISO paths in Settings.
5. Choose an available AVISO palette, review group visibility, and select a rendering resolution in Settings if needed.
6. Verify that RIMCAS reflects the active airport's selected EuroScope runways, then configure alert behavior and any closed-runway state.
7. Arrange the required insets and save an airport preset if needed.
8. Run `.smr diagnostics` and confirm the expected version and data sources.

Bundled operational data is a starting point and must be checked for the local airport and controlling position.

### Runway-driven AVISO group visibility

Add `runway_group_visibility` to your existing `vSMR_Data/config.json` root, alongside `profiles` or `aviso`; do not replace the rest of your file. Bundled rules live in replaceable `default.json`, so put customizations in `config.json` to preserve them across updates. Close the Control Center before editing externally.

For example, show LFPG's West Arrows based on **LFPO departure runways 06 or 07**:

```json
{
  "schema_version": 1,
  "runway_group_visibility": {
    "LFPG": {
      "ground-layout-west": {
        "enabled": true,
        "airport": "LFPO",
        "operation": "departure",
        "runways": ["06", "07"],
        "exclude_runways": [],
        "match": "any",
        "visible_when_matched": true,
        "visible_when_unmatched": false
      }
    }
  }
}
```

The outer airport (`LFPG`) identifies the AVISO being displayed. The next key is an **existing group ID**, not its displayed name; find IDs in the map's `vsmr_groups` array. `airport` identifies the airport to monitor, which must have runway data in EuroScope's active sector. The operational selection is read independently of the sector used for the view's geometry. It does not need to be the selected vSMR airport. Rules affect the main AVISO and its insets, including CoFrance.

| Field | Meaning |
| --- | --- |
| `enabled` | Defaults to `true`. Set to `false` to return this group to manual control. |
| `airport` | Required source airport ICAO. |
| `operation` | `either` (default), `arrival`, or `departure`, using EuroScope's actual runway selections. |
| `runways` | Required nonempty list. `26` matches 26/26L/26C/26R; `26L` matches only 26L. |
| `exclude_runways` | Optional list; any active matching runway blocks the rule. Uses the same operation filter. |
| `match` | `any` (default): at least one requested runway; `all`: every requested runway. Other active runways are allowed unless excluded. |
| `visible_when_matched` | Defaults to `true`. |
| `visible_when_unmatched` | Defaults to `false`, including when the source airport is unavailable or no runway is selected. |

Objects merge by airport and group ID; runway arrays replace inherited arrays. Use `"exclude_runways": []` to clear bundled exclusions when changing a rule. Unknown fields or invalid values cause that group's rule to be ignored and logged. Rules never create groups. You can manually show or hide a group even with its rule enabled: your choice remains until that rule's source airport changes its ARR/DEP runway selection, or the rule itself changes. A change within the same direction (for example 26L to 27R) also reapplies the rule; changes at unrelated airports and unrelated profile saves do not. Loading a different map or reopening the view applies the rules afresh. Disable a rule for permanent manual control. JSON edits and runway changes are picked up automatically while the radar refreshes; neither EuroScope runway selections nor saved user settings are rewritten.

LFPG defaults show East Arrows (`ground-layout-east`) for 08/09 and West Arrows (`ground-layout-west`) for 26/27, considering both arrivals and departures. No active direction or mixed east/west directions hides both. Each rule is independent: overriding West does not change East. To disable both defaults, merge this into your configuration:

```json
"runway_group_visibility": {
  "LFPG": {
    "ground-layout-east": { "enabled": false },
    "ground-layout-west": { "enabled": false }
  }
}
```

This only controls map-group visibility; it does not select vSID rules or configurations. Changed rule decisions are recorded as `AVISO runway visibility` diagnostics, including the source airport and observed ARR/DEP runways. Use `.smr diagnostics` after reproducing a problem to export the recent diagnostic messages, or `.smr log normal` to enable file logging before changing runways.

## Documentation

| Topic | Wiki page |
| --- | --- |
| Runtime Menu and Control Center | [Control Center](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center) |
| Profiles, modes, tags, colors, and rules | [Profiles and Display](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center-Display) |
| AVISO maps, palettes, groups, and editing | [AVISO](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center-AVISO) |
| RIMCAS alerts | [RIMCAS](https://github.com/IWantPizzaa/vSMR/wiki/RIMCAS) |
| Native inset windows | [Insets](https://github.com/IWantPizzaa/vSMR/wiki/Insets) |
| CDM bridge data and manual CPDLC/PDC | [Datalink](https://github.com/IWantPizzaa/vSMR/wiki/Datalink), [current bridge fields](https://github.com/IWantPizzaa/vSMR/wiki/Integrations) |
| Optional providers and bridge setup | [Integrations](https://github.com/IWantPizzaa/vSMR/wiki/Integrations) |
| Manual Paris vSID configuration | [Paris configuration](https://github.com/IWantPizzaa/vSMR/wiki/Paris-vSID-Configuration) |
| Commands, logs, and problem reports | [Troubleshooting](https://github.com/IWantPizzaa/vSMR/wiki/Commands-and-Troubleshooting) |
| Source builds and release packaging | [Development](https://github.com/IWantPizzaa/vSMR/wiki/Development-and-Releases) |

### RDF departure-list indication

With TrackAudio running and **Native RDF** enabled in Settings (or `.smr rdf on`), vSMR provides an optional EuroScope list item **RDF**. In the departure-list column setup, select the vSMR **RDF** item, set the width to **2**, and bind its left-click action to vSMR **RDF reset**. vSMR does not rewrite your EuroScope list layout or mouse bindings.

`TX` blinks yellow/grey for each ground aircraft heard transmitting, including simultaneous speakers. It stays pending after the call ends until clicked, so brief calls are not lost between refreshes. Reset acknowledges only the selected aircraft locally, without changing its flight plan or the live RDF ring. A new transmission activates it again; duplicate events or reception on another frequency during the same call do not undo a reset. Ground filtering follows vSMR's departure-tag speed threshold (reported ground speed at most 50 kt); an uncorrelated aircraft has no indication. Aircraft disconnect/takeoff and disabling Native RDF clear pending indications. A TrackAudio reconnect clears live rings but preserves known ground calls awaiting acknowledgement.

## Useful commands

| Command | Purpose |
| --- | --- |
| `.smr` | Open the Control Center |
| `.smr diagnostics` | Write a bounded diagnostic report |
| `.smr reload` | Reload runtime configuration/data for open radar screens |
| `.smr rdf on` / `.smr rdf off` | Enable or disable native RDF display (also in Control Center Settings) |
| `.smr log normal` / `.smr log verbose` / `.smr log off` | Set logging verbosity or disable logging |
| `.smr log status` | Show logging state and log location |

## Build and test

Build from the repository root with Visual Studio C++ tools, MFC support, and a Windows SDK. The build script defaults to MSVC toolset `v145` and `Release | Win32`; it rebuilds the solution and runs native and browser regressions. Microsoft Edge is required for the browser tests.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\build_project.ps1
```

After building the native test executable, run the regression suite independently with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tests\run_tests.ps1
```

Release-input checks enforce matching 2.0.0 versions, the reviewed hashes of all 160 maps, and an update policy that never deletes a bundled airport. LFPG retains the supplied map's 1,468 features plus 89 East arrows and 97 West arrows, independently controlled through the **East Arrows** and **West Arrows** groups. The hash manifest records this post-import restoration.

Publishable artifacts require a clean source commit and verified bundled-asset provenance. Signing is optional; configuring a signing certificate/pin or `-RequireSignature` enforces signed binaries and the matching detached update signature. The packager, binary product versions, and AppVeyor settings target 2.0.0; loader version remains 1.3.0 and runtime ABI remains 1. Five resource/dependency groups still need provenance verification, including the compiled bridge client shim; local validation packages are not distributable releases. See the [provenance register](vSMR/data/Licenses/ASSET_PROVENANCE.txt). Keep the changelog entry Unreleased and do not publish the tag or stable feed until those gates are resolved.

### Per-file update feed

`vSMR/tools/build_config_defaults.ps1` generates `default.json` from the bundled profile templates and official AVISO hashes. Run it after editing those source assets; the regression suite checks that the generated file is current. Do not edit the generated file or regenerate stable profile/feature IDs during unrelated changes.

The raw feed is separate from the legacy ZIP package. The following commands stage local artifacts only; publishing remains an explicit maintainer action:

```powershell
# Rebuild/test clean source and stage only application-owned compiled assets.
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\create_update_feed.ps1 `
  -Phase Prepare -FeedDirectory C:\release-staging\vsmr-payload

# Copy the exact staged payload/ tree and prepared-feed.json into a separate
# update-feed branch checkout at C:\release-worktrees\vsmr-update-feed.
# Commit only payload/ there, without stale files from a previous release.
# Use a .gitattributes entry "payload/** -text" to prevent byte conversion.
# Retain prepared-feed.json locally; it is not a published application file.

# Verify that the recorded commit contains exactly those payload bytes,
# then create beta/version.json (stable/version.json for a stable version).
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\create_update_feed.ps1 `
  -Phase Manifest -FeedDirectory C:\release-worktrees\vsmr-update-feed `
  -ContentCommit <full-40-character-payload-commit>
```

Commit/publish the payload before the channel manifest. The manifest contains `schema`, `version`, `content_commit`, `minimum_loader_version`, `runtime_abi`, and a `files` object mapping install-relative paths to `{ "sha256": "...", "size": 123 }`. No file URLs or user paths are supplied by the manifest. `config.json`, legacy editable profile files, custom maps, user data, symbols and local updater state are excluded. The generator checks real Win32 binary headers, exact hashes/sizes, and immutable Git blob contents. `-ValidationOnly -SkipBuild` stages an existing build for local tests; its manifest is deliberately named `version.validation-only.json` and cannot be promoted silently to the public feed.

### Generated files and cleanup

Project-owned C++/C#/resource files use PascalCase (`Owner.Feature.cpp` for split units); JavaScript/CSS use kebab-case; scripts, source JSON, sounds and cursors use descriptive snake_case. ICAO identifiers, third-party files, license notices and public binary names retain their established names. `vSMR/DistributionAssets.props` maps renamed source assets to stable installed filenames, preserving updater and migration compatibility. Update includes, project files, generators and tests together when renaming files; `test_file_naming.ps1` checks these conventions.

Build outputs (`Release/`, `Debug/`, `bin/`, `obj/`), `.vs/`, `.tmp/`, `artifacts/` and Python caches are not source files. Retain any release packages or private symbols you still need outside the checkout before removing generated outputs. Close Visual Studio and EuroScope first, and remove temporary Git worktrees with `git worktree remove` rather than deleting their directories directly. Keep `vSMR/data/`, generated web bundles and `default.json`: these are tracked application inputs validated by the build.

`vSMR/tools/build_project.ps1` rebuilds Release/Win32 and runs the native and browser regression suites. The installer, migration helpers and legacy updater recovery code remain necessary for existing installations; they are not disposable build artifacts.

### CoFrance insets

AVISO inset label visibility uses `r = min(1, inset drawable diagonal / host drawable diagonal)` and evaluates zoom visibility at `visible ground diagonal / r`. Text and halos retain their normal configured size; resizing does not shrink them. Existing GeoJSON visibility settings remain authoritative; labels without a zoom restriction remain unrestricted. The dimensions exclude title bars/chat and do not depend on raster overscan or a fixed screen resolution. Changes to visibility invalidate label caches. This applies equally to native vSMR and CoFrance insets; aircraft tags, UI controls and the main AVISO labels are unchanged.

vSMR also attaches to the geo-referenced `CoFrance radar display` type. CoFrance continues to own the main radar; vSMR draws its AVISO, SRW, METAR and timer insets plus their existing runtime controls. Other third-party and standard EuroScope views are not opted in.

Load vSMR together with CoFrance, then reopen a CoFrance ASR (an already-open screen must be recreated). The vSMR rail starts expanded on a new CoFrance view: select the airport, then open **Insets** and enable the desired windows. `.smr insets` expands and recenters the rail on all attached CoFrance views and reports received refresh phases and radar bounds; if no view is attached, it reports that explicitly. `.smr editor` opens the Control Center. Saving the ASR stores vSMR's inset state in its own plugin namespace. Keep a backup/test copy of the ASR and test disconnected from VATSIM first.

The adapter skips the main vSMR map, targets, tags, RDF, RIMCAS panels and FPS overlay. AVISO presets cannot recenter the CoFrance map, and linked main/inset movement is unavailable on this host. The shared vSMR profiles and maps remain available to the insets. Custom displays with `DisplayTypeNeedRadarContent:0` use EuroScope's before-TAG phase rather than depending on native TAG/list phases; hosts enabling native radar content use the after-lists phase. If the rail is missing, enable `.smr log normal`, reopen the ASR, run `.smr insets`, and export `.smr diagnostics`. This feature is integrated in `dev` for the upcoming release; integration does not publish a release or update feed.

## License

vSMR source code is licensed under the [GNU General Public License v3.0](LICENSE). Bundled dependencies and data assets retain their own terms; notices and provenance records are under `vSMR/data/Licenses/`.
