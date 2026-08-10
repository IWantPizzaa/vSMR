# vSMR 2.0 for EuroScope

vSMR is a 32-bit EuroScope plug-in that provides a configurable surface movement radar display for ground and low-level airport traffic. Version 2.0 adds a unified Control Center, AVISO editing, Windows-style inset windows, airport-scoped layouts, RIMCAS configuration, VACDM data, and Hoppie CPDLC/PDC workflows.

Current version: **2.0.0-beta.1**

This is a beta release for controlled operational testing. Keep a known-good backup, verify the display and alert configuration for the active airport before controlling, and do not mix the DLL or data files from different builds.

vSMR is a plug-in, not a standalone application. EuroScope must load `vSMR.dll` and create a vSMR radar screen.

## Highlights

- Configurable surface radar with native `NOVA`, `Icon (A320)`, and `Triangle` target rendering
- Normal and detailed tags with status-specific layouts, drag positioning, and optional auto-deconfliction
- Structured rules for target, tag, and text colors
- Schema-2 profiles with reusable display modes
- A native Line Up (`LNUP`) ground status with independent visibility, target color, tag color, tag definition, and rule matching
- GeoJSON AVISO maps, groups, styles, labels, and a native geometry editor
- Configurable RIMCAS runway and movement alerts
- Native AVISO, SRW 1, METAR, and Timer insets
- Native TrackAudio RDF rings on the main surface view and the AVISO and SRW 1 insets
- Windows-style inset movement, resizing, edge/corner snapping, and live snap previews
- Airport-specific inset state and presets that remain available when the active profile changes
- VACDM time and state integration
- Hoppie CPDLC connection, message handling, PDC composition, and automatic PDC reminders
- Transactional configuration writes, backups, diagnostics, and reproducible release packaging

The package includes ready-to-use AVISO data for LFBO, LFLL, LFML, LFMN, LFPG, and LFPO, a normalized aircraft-dimension database, aircraft silhouettes, and five example profiles.

## Requirements

- Windows and 32-bit EuroScope
- The x86 Microsoft Visual C++ 2015-2022 Redistributable
- The x86 [Microsoft Edge WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- A complete matching release containing both `vSMR.dll` and `vSMR_Data\`

WebView2 hosts the local Control Center. The UI itself does not require a web server or an internet connection. Internet access is only required for enabled external features such as Hoppie CPDLC, VACDM, fallback METAR retrieval, or GitHub data imports.

## Installation

### Install a release package

1. Download the complete `vSMR-2.0.0-beta.1.zip` and its matching `.zip.sha256` file.
2. Verify the ZIP checksum.
3. Close EuroScope.
4. Extract the package to a temporary directory.
5. Install the x86 Visual C++ Redistributable and x86 WebView2 Evergreen Runtime if required.
6. Install the matched DLL and data directory into the EuroScope plug-in directory.
7. In EuroScope, open `Other Settings -> Plug-ins` and load `vSMR.dll`.
8. Open a vSMR radar screen, select the active airport, and review the active profile, AVISO, groups, RIMCAS runways, and inset layout.

The recommended installer is included in every package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR_Data\Tools\install_vsmr.ps1 `
  -DestinationDirectory "C:\path\to\EuroScope\Plugins"
```

The installer validates the package manifest before copying files and creates a complete timestamped rollback backup. On an upgrade, it replaces the DLL and immutable package resources while preserving existing user data by default, including profiles, AVISO files, imported resources, aircraft data and icons, audio, and unknown files under `vSMR_Data`.

Use `-ReplaceUserData` only when deliberately resetting user-managed data to the bundled defaults.

For a manual clean installation, place the two package entries together:

```text
EuroScope\Plugins\
  vSMR.dll
  vSMR_Data\
    vSMR_Profiles.json
    ICAO_Aircraft.json
    AVISO\
    aircraft_icons\
    Audio\
    Licenses\
    Tools\
    vSMR_webUI\
```

Do not copy only the DLL. The Control Center, default data, audio, licenses, and package metadata live under `vSMR_Data`.

### Upgrade from vSMR 1.x

1. Close EuroScope.
2. Back up the existing `vSMR.dll`, `vSMR_Data\`, and any legacy flat JSON/GeoJSON files.
3. Extract the full 2.0 package to a temporary directory.
4. Run the packaged installer against the existing plug-in directory.
5. Start EuroScope and verify the active airport and configuration before use.

Legacy profile data is migrated to schema 2 when required. The previous valid profile file is retained as `vSMR_Profiles.json.bak` when a migrated document is committed. Invalid primary data is not silently overwritten.

### Roll back

The installer writes the backup location to `vSMR_Data\INSTALLATION.json`. Close EuroScope, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR_Data\Tools\restore_vsmr_backup.ps1 `
  -DestinationDirectory "C:\path\to\EuroScope\Plugins" `
  -BackupDirectory "C:\path\to\the\vSMR-backup"
```

The rollback helper validates that the backup belongs to the selected installation and creates a safety backup of the current installation before restoring it.

## First Run

1. Create or open a vSMR radar screen in EuroScope.
2. Open the Runtime Menu and set the four-letter active airport ICAO.
3. Choose a profile and display mode.
4. Open the Control Center from the Runtime Menu or enter `.smr`.
5. On `Settings`, verify the active Profiles and AVISO paths.
6. On `Alerts`, verify monitored arrival/departure runways and RIMCAS behavior.
7. Configure the required inset windows and save an airport preset if desired.
8. Enter `.smr diagnostics` and confirm that the report identifies the expected version and data sources.

Operational data included with the project is a starting point, not a substitute for local validation.

## Control Center

The modeless vSMR Control Center is owned by EuroScope and stays synchronized with the radar screen. It keeps the current page while its WebView remains open and restores its native window position across sessions.

The page rail contains:

| Page | Purpose |
| --- | --- |
| `Display` | Profile colors, target symbols, tags, typography, and structured rules |
| `AVISO` | Geometry styling and visibility, label editing, style selection, reload, and source loading |
| `Alerts` | RIMCAS runways, alert types, timers, visibility, and colors |
| `Groups` | AVISO group creation, ordering, membership, and default visibility |
| `Modes` | Display-mode filters and requirements |
| `Profiles` | Profile creation, duplication, naming, deletion, activation, and filters |
| `Settings` | Data sources, display settings, CPDLC connection, and PDC reminder controls |

### Editing and saving

Profile, display, AVISO, alert, group, mode, CPDLC, and PDC reminder fields update the current in-memory draft as they are edited. There is no separate per-section Update, Apply, or Revert step. The single global `Save` action validates the complete draft, writes the affected data, and applies the saved configuration to the open vSMR radar screens. Closing and reopening the Control Center preserves pending field values; Reload asks before discarding unsaved, invalid, or unfinished edits.

Runtime actions such as changing the active profile or mode, toggling a group or inset, and loading an inset preset are immediate operational actions. Connect/Disconnect, Poll, Check now, and Run/Stop are likewise explicit live actions rather than configuration edits.

If a Profiles or AVISO file changes outside vSMR while edits are pending, the Control Center refuses to overwrite it and asks for a reload. A normal save rotates validated backups and rolls back the other document if a multi-file transaction cannot complete.

### Profiles and modes

Profiles control fonts, target symbols, target/tag colors, definitions, structured rules, filters, RIMCAS appearance, and SRW 1 styling. Display modes select status visibility and operational requirements without duplicating a complete profile.

### Ground statuses and Line Up

The vSMR ground-status menu offers only the statuses applicable at the active airport: departures get `No Status`, `Startup`, `Push`, `Taxi`, `Line Up`, and `Departure`; arrivals get `No Status`, `Taxi In`, `Parked`, and `Arrival`. Open it by left-clicking a callsign or ground-status tag item; right-clicking the ground-status item opens the same menu.

[EuroScope's documented scratchpad ground states](https://www.euroscope.hu/wp/non-standard-extensions/) do not include `LNUP`. When `Line Up` is selected, vSMR publishes EuroScope's standard `TAXI` state for interoperability and keeps a session-local `LNUP` override shared by every open vSMR screen. vSMR then uses `LNUP` consistently for display-mode visibility, target and tag colors, tag definitions, structured rules, the main view, and AVISO. Selecting another status from the vSMR menu clears the override. It is also cleared when the flight plan disconnects or EuroScope reports a state other than `TAXI`; after a restart it safely falls back to `TAXI`.

Existing profiles are migrated additively: Line Up initially inherits each profile's Taxi visibility, target color, tag color, and tag definition, but can then be configured independently. A lined-up aircraft on runway must pass both the `Line Up` and `On runway` display-mode filters.

The bundled profile database uses schema 2. Older supported keys are normalized during loading and saving for compatibility.

### Tags and structured rules

Normal and detailed tag definitions can be customized for departure and arrival states. Supported data includes callsign and aircraft information, runway and stand fields, flight-plan values, ground status, scratchpad/remarks, VACDM times, event booking, and datalink clearance state.

Structured rules can match runway, custom, status, detail, and VACDM conditions, then override target, tag, or text colors. Rules are evaluated in profile order.

### AVISO data

vSMR uses GeoJSON `FeatureCollection` files for airport maps. Schema-2 AVISO documents can contain metadata, reusable styles, object/layer information, labels, and group membership.

The Control Center can:

- load an AVISO or Profiles file from the computer
- download a supported file from `github.com` or `raw.githubusercontent.com`
- reload the active source
- edit AVISO geometry styles and visibility, label text and styles
- define groups and assign features to them
- restore validated bundled defaults or `.bak` profile data

A computer file is activated in place; it is not copied over the bundled airport file. A GitHub resource is validated and downloaded to a collision-safe file under `vSMR_Data\AVISO\` or `vSMR_Data\Profiles\`, then activated from that location. The Settings page always shows the actual active path.

## Runtime Menu and Insets

The compact Runtime Menu provides quick access to:

- active airport
- active profile and display mode
- AVISO group visibility
- AVISO, SRW 1, METAR, and Timer inset visibility
- inset reset actions
- airport-specific inset presets and defaults
- the Control Center

The menu position and radar-window state are saved with the EuroScope ASR. Inset presets are stored by airport, not by profile, so switching profiles does not remove or replace the current airport's presets.

### Window movement, snapping, and resizing

Resizable inset windows can be dragged by their title bar and resized from every edge or corner. Resize hit areas extend beyond a one-pixel border and use the matching horizontal, vertical, or diagonal cursor.

When a window approaches a valid edge or corner, vSMR draws a preview of the exact target position and size before the mouse button is released.

- Edge snapping an AVISO, SRW 1, or METAR inset creates a left, right, top, or bottom split and adjusts the main AVISO render area to the remaining space.
- Corner snapping anchors the inset without expanding it to one quarter of the screen.
- Floating and snapped windows remain resizable through the valid exposed edges and corners.
- Moving or resizing an inset is persisted with the airport's ASR state.

### AVISO inset

The AVISO inset is a second viewport of the active airport map. It supports independent pan and cursor-anchored wheel zoom while sharing the loaded AVISO document and group visibility with the main view.

### SRW 1

SRW 1 is the secondary surface radar inset. It supports panning, cursor-anchored wheel zoom, snapping, resizing, and a floating altitude filter. The former SRW 2/Approach Path inset is no longer part of vSMR 2.0.

### METAR inset

The METAR inset follows the active airport and displays a compact wind rose, wind variation, runway headwind/crosswind components, QNH, UTC, and local controller time. It uses EuroScope weather data when available and can request a bounded fallback METAR without blocking EuroScope's UI thread.

### Timer inset

The Timer contains independent `1M`, `2M`, and `3M` countdowns:

- left-click a duration to start it
- right-click the same duration to reset it
- `vSMR_Data\Audio\Alarm.wav` plays once when a countdown reaches zero

Countdowns are session state. Presets store only Timer visibility and placement.

## Native RDF and Official RDF Compatibility

vSMR has its own radio-direction-finding overlay for surface views. It draws each transmission with the projection of the view that contains it, so the main radar, AVISO inset, and SRW 1 inset remain geographically aligned when they have different pan, zoom, size, or aspect ratio.

The native RDF client:

- is enabled by default and remembers its state in the EuroScope plug-in settings
- connects to TrackAudio at the fixed local endpoint `ws://127.0.0.1:49080/ws`
- draws a white 20-pixel ring for a normal transmission
- draws rings in red while more than one station is transmitting

Use `.smr rdf` or `.smr rdf status` to show its state, `.smr rdf on` to enable it, and `.smr rdf off` to disable it.

The native implementation currently consumes TrackAudio only. It does not receive the hidden-window RDF feed from the Audio for VATSIM standalone client. When only that standalone client is running, vSMR's native rings have no transmission source.

### Keep the official RDF on approach views only

The external [RDF Plugin for EuroScope](https://github.com/KingfuChan/RDF) can remain loaded for approach radar displays, but it must not create a radar-screen instance for vSMR's `SMR radar display`. Otherwise, the external plug-in renders once with the parent EuroScope screen projection and cannot correctly follow the independently transformed AVISO and SRW 1 insets.

Apply [`vSMR/data/Tools/RDF-vSMR-ground-view.patch`](vSMR/data/Tools/RDF-vSMR-ground-view.patch) to official RDF commit `a4bd0ae5272088286acee1c2495ed3e4a2e627c6`, then rebuild that plug-in using its upstream development instructions. The same patch is distributed in a release as `vSMR_Data\Tools\RDF-vSMR-ground-view.patch`.

```powershell
git clone https://github.com/KingfuChan/RDF.git
Set-Location RDF
git checkout a4bd0ae5272088286acee1c2495ed3e4a2e627c6
git apply C:\path\to\vSMR_Data\Tools\RDF-vSMR-ground-view.patch
```

The patch changes only `CRDFPlugin::OnRadarScreenCreated`: it returns `nullptr` for the exact display name `SMR radar display`, while the official RDF continues to create screens and draw on every other display.

Do not use `.RDF ASR DRAW 0` as the sole exclusion when ground and approach displays are open together. At the pinned upstream revision, RDF loads per-ASR values into one shared drawing-settings object during refresh. Refreshing a different screen can therefore replace the setting currently in effect. Refusing the vSMR screen at creation time removes that ambiguity.

The compatibility patch is a modification of the GPL-3.0-licensed official RDF project. Keep its upstream copyright notices and GPL license with any redistributed patched source or binary. vSMR does not bundle the external RDF source or DLL.

## RIMCAS

RIMCAS uses configured runway geometry and target movement to produce runway and movement alerts. The Control Center `Alerts` page controls monitored arrival/departure runways, closed runways, Normal/LVP visibility, timers, thresholds, and colors.

`LNUP` authorizes runway entry and taxi movement for RIMCAS, so it suppresses `RWY INC` and `NO TAXI`. It does not authorize takeoff: `NO TKOF` remains active until the status changes to `DEPA`.

Current alert labels include:

- `NO PUSH`
- `NO TAXI`
- `NO TKOF`
- `STAT RPA`
- `RWY INC`
- `RWY TYPE`
- `RWY CLSD`
- `HIGH SPD`
- `EMERG`

Always verify runway geometry, monitored directions, and alert timing for the active airport before operational use.

## VACDM

VACDM values are available to tags and structured rules:

- TOBT, TSAT, TTOT
- ASAT, AOBT, ATOT
- ASRT, AORT, CTOT
- event-booking state

Configure the backend URL in the metadata object at the end of `vSMR_Profiles.json`:

```json
{
  "_vsmr": {
    "schema_version": 1,
    "vacdm": {
      "server_url": "https://your-vacdm-server.example"
    }
  }
}
```

vSMR trims a trailing slash and requests `<server_url>/api/v1/pilots`. Polling is disabled when the URL is empty. TOBT falls back to the flight-plan EOBT when the backend has no value.

## CPDLC and PDC

The `Settings` page contains the CPDLC connection and automatic PDC reminder controls. It supports:

- a Hoppie logon callsign and protected Hoppie code
- connect/disconnect and manual polling
- optional request notification sound
- manual PDC eligibility checks for the active airport
- automatic PDC reminders with a delay and resend cooldown
- Run and Stop controls for reminder automation
- compact vSMR-styled PDC and received-message windows
- a `Datalink clearance` tag item and `Datalink menu` tag function

The PDC window uses operational field names including `ADEP`, `ADES`, and `RWY`.

CPDLC connection and reminder settings use a separate protected EuroScope settings store rather than profile JSON or AVISO. Their field changes remain pending until the same global `Save` action is pressed. The explicit Connect and PDC Run/Stop actions can first persist the settings required for that live operation; there is no separate Update action.

The Hoppie code is encrypted with Windows DPAPI for the current Windows user before it is saved. It is excluded from profile JSON, editor history, diagnostics, and logs. Existing plaintext credentials are migrated when possible; vSMR does not deliberately fall back to saving plaintext.

The CPDLC notification is `vSMR_Data\Audio\Ding.wav`. A missing audio file does not disable datalink operation.

CPDLC and PDC require a valid local `.cdm` alias template. The resolved alias path is shown under `Settings -> Data files`.

## EuroScope Commands

Enter commands in lowercase as shown.

| Command | Effect |
| --- | --- |
| `.smr` | Opens the Control Center Settings page; opens the legacy CPDLC dialog only when no vSMR radar screen exists |
| `.smr editor` | Opens the Control Center |
| `.smr vsmr` | Alias for `.smr editor` |
| `.smr config` | Opens the Control Center Settings page |
| `.smr profile` | Opens the Control Center Profiles page |
| `.smr reload` | Reloads vSMR runtime data on all open vSMR screens |
| `.smr aviso reload` | Reloads the active AVISO GeoJSON |
| `.smr aviso editor` | Opens the AVISO editor |
| `.smr rdf` | Shows whether the native TrackAudio RDF overlay is enabled |
| `.smr rdf status` | Shows whether the native TrackAudio RDF overlay is enabled |
| `.smr rdf on` | Enables the native TrackAudio RDF overlay and saves the setting |
| `.smr rdf off` | Disables the native TrackAudio RDF overlay and saves the setting |
| `.smr connect` | Connects or disconnects Hoppie CPDLC |
| `.smr poll` | Polls Hoppie messages immediately |
| `.smr cdm` | Runs a manual PDC reminder scan for the active airport |
| `.smr cdm auto status` | Shows automatic reminder state and delay |
| `.smr cdm auto on` | Starts automatic reminders with the saved delay |
| `.smr cdm auto off` | Stops automatic reminders |
| `.smr cdm auto <minutes>` | Sets the delay and enables automatic reminders |
| `.smr cdm cooldown status` | Shows the reminder resend cooldown |
| `.smr cdm cooldown <minutes>` | Sets the reminder resend cooldown |
| `.smr draw` | Toggles RIMCAS runway-area drawing |
| `.smr status` | Prints current RIMCAS runway states |
| `.smr log` | Toggles normal logging |
| `.smr log normal` | Enables concise logging |
| `.smr log verbose` | Enables detailed logging |
| `.smr log off` | Disables logging |
| `.smr log status` | Reports logging state and path |
| `.smr diagnostics` | Writes a redacted diagnostics report |
| `.smr diag` | Alias for `.smr diagnostics` |

## Runtime Data

The normal runtime root is `vSMR_Data` beside `vSMR.dll`.

| Path | Role |
| --- | --- |
| `vSMR_Profiles.json` | Profiles, display modes, tag definitions, rules, colors, filters, presets, and metadata |
| `AVISO\AVISO_<ICAO>.geojson` | Airport-specific AVISO map data |
| `Profiles\*.json` | Collision-safe downloaded/imported profile variants |
| `ICAO_Aircraft.json` | Aircraft length and wingspan lookup |
| `aircraft_icons\*.png` | Silhouettes used by the `Icon (A320)` target style |
| `Audio\Alarm.wav` | Timer expiry sound |
| `Audio\Ding.wav` | CPDLC request sound |
| `vSMR_webUI\` | Local Control Center application and bundled recovery defaults |
| `Tools\` | Package install and rollback helpers |
| `Licenses\` | Project/dependency licenses and asset provenance |
| `Diagnostics\` | Redacted reports created by `.smr diagnostics` |
| `RELEASE-METADATA.json` | Version, source, build, signing, and publishability information |
| `SHA256SUMS.txt` | Exact package payload manifest |

For compatibility, some loaders also inspect legacy flat locations beside the DLL. New installations should use the structured `vSMR_Data` layout.

`vSMR_Maps.json` is no longer bundled because the supported airport maps are GeoJSON AVISO documents. A legacy maps file can still be loaded as a fallback for an airport without AVISO data.

## Diagnostics and Troubleshooting

### Create a support report

Enter:

```text
.smr diagnostics
```

The report is written to `vSMR_Data\Diagnostics\` or, if that directory is not writable, `%TEMP%\vSMR_Diagnostics\`. It includes version, data-source, runtime, and recent-log information while redacting known credentials and sensitive endpoint values.

Review every report before sharing it. Operational callsigns or local paths can still be present. Never share a Hoppie code or raw CPDLC message.

### Logging

`vsmr.log` is stored beside `vSMR.dll` when logging is enabled. At approximately 4 MiB it rotates to `vsmr.log.1`.

- Use `normal` for routine troubleshooting.
- Use `verbose` only while reproducing a difficult issue.
- Turn logging off again after collecting the required information.

### Common problems

| Symptom | Check |
| --- | --- |
| `vSMR.dll` does not load | EuroScope is 32-bit and the x86 Visual C++ Redistributable is installed |
| Control Center is blank or unavailable | The x86 WebView2 Evergreen Runtime is installed and `vSMR_Data\vSMR_webUI\` is complete |
| Control Center opens off-screen | Close EuroScope and remove `%LOCALAPPDATA%\vSMR\control-center-window.json` |
| Profiles fail to load | Validate `vSMR_Data\vSMR_Profiles.json`; inspect its `.bak` and the diagnostics report |
| AVISO is missing | Confirm the active airport and active GeoJSON path on Settings |
| Aircraft silhouettes are missing | Confirm `ICAO_Aircraft.json`, `aircraft_icons\`, and the active `Icon (A320)` target style |
| VACDM values are empty | Confirm `_vsmr.vacdm.server_url`, network access, callsign matching, and backend data |
| CPDLC cannot connect | Confirm EuroScope connectivity, callsign/code, Hoppie availability, and the sanitized error/log output |
| PDC reminders are unavailable | Confirm the active airport, controller connection, and resolved `.cdm` alias path |
| An inset layout is wrong after changing airports | Load or reset the preset for that airport; presets are intentionally airport-scoped |
| Native RDF rings do not appear | TrackAudio is running and its WebSocket is available at `ws://127.0.0.1:49080/ws`; Audio for VATSIM standalone is not yet a native RDF source |
| The external RDF draws over a vSMR surface view | Rebuild the official RDF at the pinned revision with `vSMR_Data\Tools\RDF-vSMR-ground-view.patch`; `.RDF ASR DRAW 0` alone is not reliable across concurrent screens |

## Building from Source

### Toolchain

- Visual Studio 2022 with Desktop development with C++
- MSVC x86 tools and MFC
- Windows 10 SDK
- C++17
- NuGet restore access for `Microsoft.Web.WebView2` `1.0.4078.44`
- The repository-provided EuroScope SDK header and import library under `lib\`

The checked-in project targets `v143`; the packaging script can select a newer installed `vNNN` toolset when it is compatible. Releases are built as `Release | Win32`.

### Validate source data

Check that bundled profiles, aircraft data, and every AVISO file are canonical:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\normalize_runtime_data.ps1 -Mode Check
```

Maintainers can use `-Mode Write` to normalize imported data before reviewing the resulting diff.

### Build and package

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\package_release.ps1
```

The script:

1. locates Visual Studio and MSBuild
2. selects an installed Win32 toolset with MFC
3. restores NuGet packages
4. rebuilds `Release | Win32`
5. validates versioning, compiler hardening, runtime data, licenses, and release layout
6. stages a clean payload
7. writes release metadata and SHA-256 manifests
8. creates the user ZIP and a separate private-symbol archive

A publishable package requires a clean Git working tree and a verifiable commit. For a local dirty-tree check only:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\package_release.ps1 `
  -AllowDirtySource -ForceNonPublishable
```

That artifact is explicitly marked `publishable: false`. Verify a normal package with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\verify_release_package.ps1 `
  -ArchivePath .\artifacts\vSMR-2.0.0-beta.1.zip
```

Add `-AllowNonPublishable` only when verifying a deliberately non-publishable local artifact.

The user archive contains only:

```text
vSMR.dll
vSMR_Data\
```

PDB files never enter the user package.

### Offline builds and deployment

For an offline build, download `Microsoft.Web.WebView2.1.0.4078.44.nupkg` on a connected machine, place it in a local NuGet feed, and restore against that feed before building.

For an offline EuroScope machine, install the x86 WebView2 Evergreen Standalone Runtime in advance. The WebView2 loader is linked into `vSMR.dll`, but the Evergreen Runtime itself is not bundled.

### Signing

`package_release.ps1` can sign the staged DLL when `VSMR_SIGNING_CERT_THUMBPRINT` or `-CertificateThumbprint` identifies an installed code-signing certificate. `VSMR_REQUIRE_SIGNATURE=1` enables the signature-required release gate. Timestamping defaults to DigiCert and can be changed with `-TimestampUrl`.

Do not describe a package as signed unless package verification confirms the signature.

## Repository Layout

```text
vSMR.sln
vSMR\
  include\       C++ headers
  src\           Plug-in, radar, inset, editor, and integration code
  resources\     Windows resources, cursors, and DLL exports
  data\          Runtime data copied to vSMR_Data in Release
  vSMR_webUI\    Control Center HTML, CSS, and JavaScript source
  tools\         Data, validation, packaging, and package-verification scripts
```

Important implementation areas:

| Path | Responsibility |
| --- | --- |
| `vSMR/src/SMRPlugin.cpp` | Plug-in lifecycle, commands, CPDLC, VACDM, weather scheduling, diagnostics |
| `vSMR/src/SMRRadar*.cpp` | Radar lifecycle, rendering, interaction, ASR state, tags, profiles, and Runtime Menu |
| `vSMR/src/InsetWindow.cpp` | AVISO, SRW 1, METAR, and Timer insets; snapping and resizing |
| `vSMR/src/Rimcas.cpp` | Runway monitoring and RIMCAS alert logic |
| `vSMR/src/Config.cpp` | Profile loading, migration, validation, and persistence |
| `vSMR/src/AvisoDocumentModel.cpp` | AVISO document validation and editing model |
| `vSMR/src/VsmrControlCenter*.cpp` | Native WebView2 host and C++/JavaScript bridge |
| `vSMR/vSMR_webUI/` | Control Center user interface |
| `vSMR/tools/` | Release and runtime-data tooling |

## Beta Notes

- The plug-in and runtime dependency path are x86 only.
- Hoppie, VACDM, VATSIM weather fallback, and GitHub imports are external services and can fail independently of radar rendering.
- Bundled airport data must be checked against current local procedures before use.
- The asset provenance register contains media/data groups that require resolution before a production release. See `vSMR/data/Licenses/ASSET_PROVENANCE.md`.
- Preserve the exact DLL, package checksum, and matching private symbols when investigating a crash.

## License and Credits

vSMR is distributed under the GNU General Public License version 3. See [`LICENSE`](LICENSE). Packaged third-party notices and the asset provenance register are under `vSMR_Data\Licenses\`.

This project continues work from:

- <https://github.com/AlexisBalzano/vSMR>
- <https://github.com/pierr3/vSMR>

Native RDF interoperability was developed against the GPL-3.0 [RDF Plugin for EuroScope](https://github.com/KingfuChan/RDF). The optional source patch in `vSMR_Data\Tools\` remains subject to that project's GPL-3.0 terms.

Special thanks to Alexis B., Baptiste C., Steve A., and Yohannes D.
