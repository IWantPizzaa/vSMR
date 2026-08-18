# vSMR 2.0 for EuroScope

vSMR is a 32-bit EuroScope plug-in that provides a configurable surface movement radar display for ground and low-level airport traffic. Version 2.0 adds a unified Control Center, AVISO editing, Windows-style inset windows, airport-scoped layouts, RIMCAS configuration, VACDM data, and Hoppie CPDLC/PDC workflows.

Current version: **2.0.0-beta.2**

This is a beta release for controlled operational testing. Keep a known-good backup, verify the display and alert configuration for the active airport before controlling, and do not mix the DLL or data files from different builds.

vSMR is a plug-in, not a standalone application. EuroScope must load `vSMR.dll` and create a vSMR radar screen.

## Highlights

- Configurable surface radar with native `NOVA`, `Icon`, and `Triangle` target rendering
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

The package includes a France-wide set of 396 airport-specific AVISO GeoJSON files, a normalized aircraft-dimension database, aircraft silhouettes, and five example profiles.

## Requirements

- Windows and 32-bit EuroScope
- The x86 Microsoft Visual C++ 2015-2022 Redistributable
- The x86 [Microsoft Edge WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- A complete matching release containing both `vSMR.dll` and `vSMR_Data\`

WebView2 hosts the local Control Center. The UI itself does not require a web server or an internet connection. Internet access is only required for enabled external features such as Hoppie CPDLC, VACDM, fallback METAR retrieval, or GitHub data imports.

## Installation

### Install a release package

1. Download the complete `vSMR-2.0.0-beta.2.zip` and its matching `.zip.sha256` file.
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
| `Settings` | General data/display/datalink settings and live performance diagnostics |

### Editing and saving

Profile, display, AVISO, alert, group, mode, CPDLC, and PDC reminder fields update the current in-memory draft as they are edited. There is no separate per-section Update, Apply, or Revert step. The single global `Save` action validates the complete draft, writes the affected data, and applies the saved configuration to the open vSMR radar screens. Closing and reopening the Control Center preserves pending field values; Reload asks before discarding unsaved, invalid, or unfinished edits.

Runtime actions such as changing the active profile or mode, toggling a group or inset, and loading an inset preset are immediate operational actions. Connect/Disconnect, Poll, Check now, and Run/Stop are likewise explicit live actions rather than configuration edits.

If a Profiles or AVISO file changes outside vSMR while edits are pending, the Control Center refuses to overwrite it and asks for a reload. A normal save rotates validated backups and rolls back the other document if a multi-file transaction cannot complete.

### Profiles and modes

Profiles control fonts, target symbols, target/tag colors, definitions, structured rules, filters, RIMCAS appearance, and SRW 1 styling. Display modes select status visibility and operational requirements without duplicating a complete profile.

The Symbols page previews NOVA, Icon, and Triangle at a consistent visual size. NOVA uses a clean white filled primary-return silhouette without decorative center or trailing marks; disabling `Show primary target` removes the filled return from both the preview and radar display.

The color editor combines saturation/brightness selection with RGB, opacity, and a full-spectrum hue slider. Every slider uses a color-preview rail and matching thumb: hue covers the complete 0–360° spectrum, each RGB rail previews that channel in the current color, and opacity fades from transparent to the selected RGB color over a checkerboard. The adjacent swatch is a preview only; all color changes stay in this editor instead of opening a second system color picker.

### Ground statuses and Line Up

The vSMR ground-status menu offers only the statuses applicable at the active airport: departures get `No Status`, `Startup`, `Push`, `Taxi`, `Line Up`, and `Departure`; arrivals get `No Status`, `Taxi In`, `Parked`, and `Arrival`. Open it by left-clicking a callsign or ground-status tag item; right-clicking the ground-status item opens the same menu.

[EuroScope's documented scratchpad ground states](https://www.euroscope.hu/wp/non-standard-extensions/) do not include `LNUP`. When `Line Up` is selected, vSMR publishes EuroScope's standard `TAXI` state for interoperability and keeps a session-local `LNUP` override shared by every open vSMR screen. vSMR then uses `LNUP` consistently for display-mode visibility, target and tag colors, tag definitions, structured rules, the main view, and AVISO. Selecting another status from the vSMR menu clears the override. It is also cleared when the flight plan disconnects or EuroScope reports a state other than `TAXI`; after a restart it safely falls back to `TAXI`.

Existing profiles are migrated additively: Line Up initially inherits each profile's Taxi visibility, target color, tag color, and tag definition, but can then be configured independently. A lined-up aircraft on runway must pass both the `Line Up` and `On runway` display-mode filters.

The bundled profile database uses schema 2. Older supported keys are normalized during loading and saving for compatibility.

### Tags and structured rules

Normal and detailed tag definitions can be customized for departure and arrival states. Supported data includes callsign and aircraft information, runway and stand fields, flight-plan values, ground status, scratchpad/remarks, VACDM times, event booking, and datalink clearance state. The Control Center token picker includes both `scratchpad` and the flight-strip annotation `remark` token. Tag backgrounds use compact text padding consistently in the main radar, AVISO inset, and SRW 1 inset.

Structured rules can match runway, custom, status, detail, and VACDM conditions, then override target, tag, or text colors. Rules are evaluated in profile order.

### Shared radar scene

Each radar screen builds one immutable `RadarScene` on EuroScope's UI thread for every rendered frame. The capture owns plain-value target coordinates, flight-plan and ground-state classification, effective colors and icon styles, preformatted normal and detailed tag elements, finalized RIMCAS state, LFPG dynamic-frequency ownership, and the connected-controller and active-airport state. The main radar, AVISO inset, SRW 1 inset, and native RDF overlay consume that same snapshot instead of repeating EuroScope target and flight-plan lookups independently.

Viewports still own projection, clipping, font measurement, tag placement, and interactive hit areas because those depend on their individual size, pan, and zoom. AVISO's background raster workers also remain separate and receive only their existing immutable map snapshots; they never access EuroScope objects or the live radar scene. The periodic `FramePerf` log remains available, while the Control Center Performance page exposes the same render pipeline through bounded rolling statistics and named cache/worker counters.

AVISO sources are parsed and validated once, then prewarmed after the airport-scoped ASR state is restored so the large LFPG source does not perform its cold load inside the first rendered scene. Cache-backed view changes use a short sliding debounce and cancellable geometry passes; the last same-source raster remains available as a transformed preview until the replacement is ready. Half-viewport overscan and hard raster-size limits reduce retained bitmap memory, and SRW caches its derived bold font and typography measurements instead of rebuilding them each frame.

### AVISO data

vSMR uses GeoJSON `FeatureCollection` files for airport maps. Schema-2 AVISO documents can contain metadata, reusable styles, object/layer information, labels, and group membership.

The default file for an active airport is `vSMR_Data\AVISO\<ICAO>.geojson`, for example `vSMR_Data\AVISO\LFPG.geojson`. LFPG additionally prefers `vSMR_Data\AVISO\LFPG_Dyna_fixed.geojson` when present so its reviewed dynamic-frequency features share the same AVISO document and renderer. An explicitly selected local or GitHub source remains authoritative. The earlier `LFPG_Dyna.geojson` preview and legacy `AVISO_<ICAO>.geojson` names remain compatibility fallbacks for manual installations that update only the DLL.

The Control Center can:

- load an AVISO or Profiles file from the computer
- download a supported file from `github.com` or `raw.githubusercontent.com`
- reload the active source
- edit AVISO geometry styles and visibility, label text and styles
- define groups and assign features to them, with create, duplicate, and delete actions reflected in the list immediately
- restore validated bundled defaults or `.bak` profile data

A reusable entry in the document's `styles` catalog supplies the default paint for every feature that references its `style_id`. Paint stored directly on a feature is a per-object override and takes precedence in the main radar and AVISO inset. Consequently, editing one selected label can override its text color, font, size, halo, or zoom without changing every other label in the shared style; `Current text group` and `All AVISO text` intentionally edit shared catalog styles. AVISO editor values use the renderer's supported ranges: text size 6–32, halo width 0–6, and line/outline width 0.25–8.

### LFPG dynamic frequency ownership

`LFPG_Dyna_fixed.geojson` extends the normal LFPG AVISO map with frequency ownership polygons and pre-positioned frequency labels. For each non-RMP `frequency_ownership_area`, vSMR walks its ordered `takeover_chain` and selects the first position ID that EuroScope currently reports as a connected controller. No online-controller list or label position is hard-coded.

- A polygon inherited by your own connected position is drawn in blue and its frequency label is hidden.
- A non-RMP polygon owned by another connected position retains its source service color and shows that controller's current EuroScope primary frequency at the GeoJSON label point.
- At a shared edge, external-territory outlines are painted after self-owned outlines so cyan and yellow boundaries remain continuous instead of clipping each other.
- A polygon with no connected owner in its chain is hidden.
- DEL frequency points are ignored until dedicated DEL polygons are available.
- LFPG RMP is resolved as one service: if any reviewed RMP position is connected, all six RMP polygons activate together. A local RMP controller owns all six and sees no RMP labels; another RMP controller activates all six area-specific labels.
- RMP labels always use each `frequency_point` feature's `text-field` or `display_frequency` at its supplied coordinates. A connected controller's primary frequency is never substituted, so the six displayed values remain BD `121.640`, F `121.580`, ACE `121.930`, FDX `131.605`, KL `121.680`, and J `121.880`.
- Controller connection, disconnection, position, or primary-frequency changes update the main display and AVISO inset together. Takeover rules are cached when the GeoJSON loads, and routine updates from controllers outside those LFPG chains do not invalidate the AVISO raster, preventing periodic map flicker while connected.

This dynamic behavior is enabled only for LFPG in beta.2. The ownership metadata and shared renderer path are generic, but another airport must be explicitly enabled after its data has been reviewed.

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

The Runtime Menu and Control Center use the same slider-control symbol for Settings, layered-view symbol for AVISO Groups, and eye symbol for display Modes. Insets use a monitor symbol, while the Control Center Display page uses an aircraft.

The Runtime Menu uses a square outer frame while retaining rounded action buttons. Its position, minimized state, and radar-window state are saved with the EuroScope ASR. Right-click the striped top handle to collapse the Runtime Menu to that handle only; right-click it again to restore the airport and action buttons. Inset presets are stored by airport, not by profile, so switching profiles does not remove or replace the current airport's presets.

### Window movement, snapping, and resizing

Resizable inset windows can be dragged by their title bar and resized from every edge or corner. Resize hit areas extend beyond a one-pixel border and use the matching horizontal, vertical, or diagonal cursor. The close button stays against the far-right title-bar edge while taking priority over the overlapping resize corner.

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

The METAR inset follows the active airport and displays a compact wind rose, wind variation, runway headwind/crosswind components, QNH, UTC, and local controller time. Numeric bearings retain the standard meteorological direction-from convention, while the rose needle and variation arc point toward the direction the air is moving. It uses EuroScope weather data when available and can request a bounded fallback METAR without blocking EuroScope's UI thread.

### Timer inset

The Timer contains independent `1M`, `2M`, `3M`, and `4M` countdowns in a compact grid, with `1M`/`2M` on the top row and `3M`/`4M` below:

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

RIMCAS uses configured runway geometry and target movement to produce runway and movement alerts. The Control Center `Alerts > RIMCAS runways` page controls vSMR's monitored arrival/departure runway pairs and closed-runway flags, plus Normal/LVP visibility, timers, thresholds, and colors. Every checkbox change enters the shared draft and is persisted and applied by the global `Save`; an explicitly empty runway list remains empty after reload.

These RIMCAS choices are separate from EuroScope's `Active Airports/Runways` dialog. EuroScope remains authoritative for sector airport/runway activity because its plug-in API exposes that state read-only. After that dialog is accepted with `OK`, vSMR now reloads the selected ARR/DEP activity immediately for conditional sector maps, weather components, the main view, and insets. If EuroScope has exactly one active airport and the current vSMR airport is no longer active, vSMR adopts that unambiguous airport; when multiple airports are active, choose the airport for each surface screen from its Runtime Menu.

`LNUP` authorizes runway entry and taxi movement for RIMCAS, so it suppresses `RWY INC` and `NO TAXI`. It does not authorize takeoff: `NO TKOF` remains active until the status changes to `DEPA`.

When an aircraft newly enters `DEPA`, `STAT RPA` has a 25-second grace period before a stationary target can trigger it. This allows time for the takeoff-clearance readback and initial movement; leaving and later re-entering `DEPA` starts a new grace period. Other RIMCAS alerts remain active during this interval.

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

Automatic CDM reminders preserve the contents, selection, and focus of EuroScope's command/message bar while vSMR submits the generated private message.

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
| `AVISO\<ICAO>.geojson` | Default airport-specific AVISO map data; legacy `AVISO_<ICAO>.geojson` files remain a compatibility fallback |
| `AVISO\LFPG_Dyna_fixed.geojson` | LFPG map plus dynamic controller ownership polygons and positioned, area-specific frequency labels; preferred for LFPG when present |
| `Profiles\*.json` | Collision-safe downloaded/imported profile variants |
| `ICAO_Aircraft.json` | Aircraft length and wingspan lookup |
| `aircraft_icons\*.png` | Aircraft-type silhouettes used by the `Icon` target style |
| `Audio\Alarm.wav` | Timer expiry sound |
| `Audio\Ding.wav` | CPDLC request sound |
| `vSMR_webUI\` | Local Control Center application and bundled recovery defaults |
| `Tools\` | Package install and rollback helpers |
| `Licenses\` | Project/dependency licenses and asset provenance |
| `Diagnostics\` | Redacted support reports and exported performance reports |
| `CrashReporter\` | Packaged Windows Error Reporting callback used to create crash reports outside EuroScope |
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

### Performance diagnostics

Open `Control Center > Settings > Performance` to inspect the active radar screen without enabling verbose logging. vSMR retains at most 2,048 raw frame samples in a fixed per-radar ring and computes statistics only when the page requests a snapshot. The selectable 30-second, 2-minute, and 10-minute windows are therefore also bounded by the samples actually retained; the page reports the real sample count and observed span.

The panel includes (counter totals are since the last reset; timing distributions follow the selected window):

- average, median, P95, and maximum vSMR frame-callback time, plus a bounded frame/main-AVISO trend
- scene capture and its AVISO-load, ownership, target-capture, and finalization slices; separate main AVISO, AVISO inset, SRW, RDF, inset-chrome, target, RIMCAS, tag, instrumented EuroScope lookup, and AVISO raster-rebuild timings
- refresh-cause counts plus the reason and measured stage context for frames at or above the 16 ms diagnostic spike threshold
- separate fresh/preview/miss outcomes for the main and inset AVISO caches; queued, coalesced, debounced, superseded, cancelled, applied, and discarded AVISO work; and aircraft source/scaled/rotated cache rates
- processed and main-view visible target counts, with the underlying capture/filter counts retained in exported samples
- the EuroScope process-wide GDI-object count, separately attributed vSMR bitmap-cache counts, and current/observed-peak estimated bitmap memory
- AVISO, network, and weather worker activity and pending/in-flight depth
- frames that used an older AVISO raster or had no raster while a newer update was pending

`Reset` starts a new collection generation for that radar screen. `Export report` writes a versioned JSON snapshot to `vSMR_Data\Diagnostics\` when that directory is writable, then uses `%LOCALAPPDATA%\vSMR\Diagnostics\` or a temporary-directory fallback. The successful export message shows the exact path used. The report contains the airport and profile names plus timing/resource counters and bounded numeric samples; it does not collect target/controller callsigns, typed messages, or credentials. A frame can record more than one refresh cause, while callbacks without an attributed cause remain `Unspecified`. The 16 ms spike threshold is a diagnostic marker rather than a claimed display-refresh deadline. The reported frame duration is vSMR's measured render-pipeline interval inside `REFRESH_PHASE_BEFORE_TAGS`, not the complete callback or EuroScope display-present interval; diagnostic stage rows can overlap and are not additive. The GDI count covers the whole EuroScope process, not only vSMR, and bitmap memory is an estimate of tracked cached pixel storage rather than total graphics-driver or process memory.

These measurements are the evidence gate for any later Direct2D/AVISO prototype. This release does not change the current GDI/GDI+ renderer.

### Logging

`vsmr.log` is stored beside `vSMR.dll` when logging is enabled. At approximately 4 MiB it rotates to `vsmr.log.1`.

- Use `normal` for routine troubleshooting.
- Use `verbose` only while reproducing a difficult issue.
- Turn logging off again after collecting the required information.

### Crash reports

vSMR registers the packaged `vSMR_Data\CrashReporter\vSMRCrashHandler.dll` with Windows Error Reporting (WER). When Windows identifies an unhandled fatal crash associated with vSMR, WER loads that callback in its reporting process. EuroScope's failing thread therefore performs no report-file I/O, stack walking, DbgHelp loading, or minidump writing. Handled first-chance exceptions are not reported as crashes, and normal EuroScope and Windows crash handling continues after the callback returns.

Reports are stored in `%LOCALAPPDATA%\vSMR\CrashReports\` by default. Startup creates the directory and proves that it is writable with a temporary file; if that fails, vSMR tries `vSMR_Data\CrashReports\` and then `%TEMP%\vSMR\CrashReports\`. Cleanup also runs only during normal startup: vSMR retains at most 10 report sets within a 256 MiB total budget, prefers newer sets that fit, and removes stale temporary dump files.

The callback creates and flushes the `.txt` summary before attempting its matching `.dmp`. The summary records whether the exception instruction was inside `vSMR.dll`, whether vSMR was instead found on the crashing stack, or whether the crashing thread was a registered vSMR worker. A stack association is explicitly reported as correlation, not proof that vSMR caused the crash. If no such association is found, vSMR creates no report merely because the plug-in was loaded.

Crash-safe diagnostics are prepared during normal execution in fixed-size memory: open radar screens and their airport/profile/inset state, the last EuroScope callbacks, vSMR worker roles, connection state, recent bounded log messages, and the exact DLL/Git/PDB/host-version identity. The external callback copies this block from the failed process and never calls live EuroScope, MFC, logger, or renderer objects. This also gives stack-overflow reports an independent healthy process and stack.

Crash reporting remains best effort. It may be unavailable when WER is disabled by policy, a debugger or another WER runtime module takes control first, the callback module is missing or blocked, the process is terminated without crash dispatch, or corruption prevents Windows from starting its reporter. The registration state and selected report directory are included in `.smr diagnostics`.

Keep the `.txt`, `.dmp`, exact `vSMR.dll`, `vSMRCrashHandler.dll`, and matching private PDBs together when diagnosing a crash. Minidumps and breadcrumbs can contain callsigns, local paths, typed text, credentials, or other process memory. Nothing is uploaded automatically. Review reports and share them only through a trusted private channel; do not attach a dump publicly without checking it first.

### Common problems

| Symptom | Check |
| --- | --- |
| `vSMR.dll` does not load | EuroScope is 32-bit and the x86 Visual C++ Redistributable is installed |
| Control Center is blank or unavailable | The x86 WebView2 Evergreen Runtime is installed and `vSMR_Data\vSMR_webUI\` is complete |
| Control Center opens off-screen | Close EuroScope and remove `%LOCALAPPDATA%\vSMR\control-center-window.json` |
| Profiles fail to load | Validate `vSMR_Data\vSMR_Profiles.json`; inspect its `.bak` and the diagnostics report |
| AVISO is missing | Confirm the active airport and active GeoJSON path on Settings |
| Aircraft silhouettes are missing | Confirm `ICAO_Aircraft.json`, `aircraft_icons\`, and the active `Icon` target style |
| VACDM values are empty | Confirm `_vsmr.vacdm.server_url`, network access, callsign matching, and backend data |
| CPDLC cannot connect | Confirm EuroScope connectivity, callsign/code, Hoppie availability, and the sanitized error/log output |
| PDC reminders are unavailable | Confirm the active airport, controller connection, and resolved `.cdm` alias path |
| An inset layout is wrong after changing airports | Load or reset the preset for that airport; presets are intentionally airport-scoped |
| Native RDF rings do not appear | TrackAudio is running and its WebSocket is available at `ws://127.0.0.1:49080/ws`; Audio for VATSIM standalone is not yet a native RDF source |
| The external RDF draws over a vSMR surface view | Rebuild the official RDF at the pinned revision with `vSMR_Data\Tools\RDF-vSMR-ground-view.patch`; `.RDF ASR DRAW 0` alone is not reliable across concurrent screens |
| No vSMR crash report is created | Check `crash_reporter_status` and `crash_report_directory` in `.smr diagnostics`; confirm `vSMR_Data\CrashReporter\vSMRCrashHandler.dll` is present and WER is not disabled by policy |

## Building from Source

The implementation is organized by feature under `vSMR\src\`, with headers colocated beside their implementations. The Repository Layout section below summarizes feature ownership and the stable runtime/package boundary.

### Toolchain

- Visual Studio or Microsoft C++ Build Tools with the `v145` toolset
- MSVC x86 tools and MFC for `v145`
- Windows 10 SDK
- C++17
- NuGet restore access for `Microsoft.Web.WebView2` `1.0.4078.44`
- The repository-provided EuroScope SDK header and import library under `lib\`

The solution exposes `Release | Win32` as its sole configuration, so a normal **Build Solution** or an MSBuild invocation without an explicit configuration produces the optimized DLL in `Release\vSMR.dll`. The project retains an explicit Debug configuration for targeted diagnostics, but it must be selected by building the project directly. Both configurations target `v145`. The packaging script detects the installed compatible `vNNN` toolset automatically; automated compatibility builds may override the project default explicitly.

The AVISO airport files are copied by the build target and intentionally are not expanded into hundreds of IDE project items. This avoids Visual C++'s unsupported project-item wildcard warning while retaining every canonical `data\AVISO\<ICAO>.geojson` file and the reviewed LFPG dynamic extension in `vSMR_Data`.

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
  -ArchivePath .\artifacts\vSMR-2.0.0-beta.2.zip
```

Add `-AllowNonPublishable` only when verifying a deliberately non-publishable local artifact.

### Crash-report harness

The isolated Win32 harness exercises the production directory-selection and retention code, the packaged WER callback ABI, Unicode and paths longer than `MAX_PATH`, missing-DbgHelp text survival, concurrent callbacks, and repeated DLL loading without starting or crashing EuroScope:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\crash_harness\run_crash_harness.ps1
```

Add `-IncludeWerIntegration` to also launch disposable harness subprocesses for a handled access violation, a real unhandled access violation, and stack overflow. The handled exception must create no report; the two unhandled failures must create report pairs. The runner temporarily adds only its exact per-user WER allowlist value and restores the previous value when it finishes.

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

`package_release.ps1` can sign both staged DLLs when `VSMR_SIGNING_CERT_THUMBPRINT` or `-CertificateThumbprint` identifies an installed code-signing certificate. `VSMR_REQUIRE_SIGNATURE=1` requires valid signatures on both `vSMR.dll` and `vSMRCrashHandler.dll`. Timestamping defaults to DigiCert and can be changed with `-TimestampUrl`.

Do not describe a package as signed unless package verification confirms the signature.

## Repository Layout

```text
vSMR.sln
vSMR\
  src\
    app\              MFC application object and EuroScope plug-in entry
    platform\windows\ Windows, GDI+, WinHTTP, PCH, and SDK integration
    shared\           Feature-neutral text and logging support
    config\           Runtime profile/configuration loading and persistence
    plugin\           Process-wide EuroScope plug-in coordinator
    aircraft\         Callsign lookup and ground-state domain support
    scene\            Immutable per-radar frame capture shared by viewports
    radar\            Main radar-screen lifecycle, rendering, and interaction
    tags\             Tag definitions, rendering, rules, and VACDM tag data
    insets\           AVISO, SRW 1, METAR, and Timer inset windows
    aviso\            AVISO document model, editor, presets, and integration
    profiles\         Profile editor and profile color paths
    control_center\   Native WebView2 host, bridge, and resource loading
      web\             Control Center HTML, CSS, and JavaScript source
    datalink\         CPDLC settings and datalink message dialogs
    weather\          Shared weather parsing and cache
    safety\           RIMCAS monitoring and alerts
    rdf\              Native TrackAudio RDF overlay
    crash\            WER registration, protocol, and breadcrumbs
      handler\         Packaged x86 out-of-process WER callback project
    diagnostics\      Bounded performance collection, statistics, and report serialization
  resources\          Windows resource scripts, cursors, and DLL exports
  data\               Runtime data copied to vSMR_Data in Release
  tools\              Data, release, and package-verification tools
    crash_harness\    Isolated crash-report harness and runner
```

Important implementation areas:

| Path | Responsibility |
| --- | --- |
| `vSMR/src/app/PluginEntry.cpp` | MFC application object and exported EuroScope initialization entry |
| `vSMR/src/plugin/Plugin.cpp` | Plug-in lifecycle, commands, CPDLC, VACDM, weather scheduling, and diagnostics |
| `vSMR/src/scene/` | Immutable per-frame target, tag, RIMCAS, ownership, controller, and airport state shared by radar viewports |
| `vSMR/src/radar/RadarScreen.cpp` and `RadarScreen.*.cpp` | Radar lifecycle, rendering, interaction, ASR state, commands, and Runtime Menu |
| `vSMR/src/insets/InsetWindow.cpp` | AVISO, SRW 1, METAR, and Timer insets; snapping and resizing |
| `vSMR/src/safety/Rimcas.cpp` | Runway monitoring and RIMCAS alert logic |
| `vSMR/src/config/RuntimeConfig.cpp` | Profile loading, migration, validation, and persistence |
| `vSMR/src/aviso/` | AVISO document validation, editing, presets, and radar integration |
| `vSMR/src/tags/` | Tag definitions, rendering, color rules, and VACDM tag helpers |
| `vSMR/src/control_center/` | Native WebView2 host, C++/JavaScript bridge, and Control Center resource management |
| `vSMR/src/control_center/web/` | Control Center HTML, CSS, and JavaScript source |
| `vSMR/src/crash/CrashReporter.cpp` and `vSMR/src/crash/handler/` | Normal-runtime WER registration/breadcrumbs and out-of-process report generation |
| `vSMR/src/diagnostics/` | Per-radar performance samples, aggregate statistics, cache/worker counters, and JSON reports |
| `vSMR/tools/crash_harness/` | Isolated deterministic and real-WER crash-report validation harness |
| `vSMR/tools/` | Runtime-data normalization, release packaging, and package verification |

The source location `vSMR\src\control_center\web\` is a development detail. Builds still install those files as `vSMR_Data\vSMR_webUI\`, and the crash handler is still installed as `vSMR_Data\CrashReporter\vSMRCrashHandler.dll`. The reorganization does not change the user package or runtime data contract.

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
