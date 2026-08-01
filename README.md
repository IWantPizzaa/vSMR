# vSMR for EuroScope

vSMR is a Win32 EuroScope plugin that provides a surface movement radar display with configurable target symbols, profile-driven tags and colors, RIMCAS alerts, approach inset windows, VACDM integration, and Hoppie CPDLC support.

This repository is a maintained fork of:

- https://github.com/AlexisBalzano/vSMR
- https://github.com/pierr3/vSMR

## What's New In v1.1.3

- Added per-profile `Tower mode`
- Tower Mode keeps full tags for all arrivals and aircraft at `TAXI`, `DEPA`, `ARR`, or later states
- Aircraft with no status, `NSTS`, `PUSH`, or `STUP` remain icon-only in Tower Mode
- `Pro mode` and `Tower mode` are represented by profile display modes, selected
  from the Runtime Menu and configured on the Control Center `Modes` page

### Previously In v1.1.2

- Major rendering-path optimizations for symbol, tag, and Control Center responsiveness
- Added `NOVA` icon style and icon-trail rendering support
- Control Center icon layout refresh:
  - shape controls split into two rows (`Icons`, `NOVA` / `Arrow`, `Diamond`)
  - clipping fixes for tight-width layouts
- Icons & Tags editor improvements:
  - `Options` section renamed to `Tags`
  - new `Behavior` box with global `Auto Deconfliction` and `Rounded Corners`
  - `rounded_corners` persisted in `vSMR_Profiles.json`
- Tag rule fix: hover/detailed tags now preserve structured color overrides
- Top `Target` menu cleanup: icon style controls removed (handled in the Control Center)
- About panel cleanup and credits alignment with repository attribution

### Previously In v1.1.1

- Major profile JSON cleanup and normalization for `tags`, `icons`, and structured `rules`
- Automatic migration from older profile keys to the new layout
- Tags editor model simplified around `Departure` and `Arrival` statuses (airborne states redistributed)
- Rules editor `Type` and `Status` lists aligned with tag classification
- Arrival icon-state handling improved:
  - `Gate` stays separate
  - all other on-ground arrival movement states use `On Ground`
- Profile lists now use one consistent ordering across UI:
  - `Default` first (if present), then alphabetical
- Control Center selection sync fix when the active profile changes from runtime controls

## Overview

vSMR is not a standalone application. It is a EuroScope plugin DLL that:

- creates a custom SMR radar screen
- renders ground and airborne targets with multiple symbol styles
- builds tag text from profile-defined token layouts
- colors icons, tag backgrounds, and text from profile colors and rule logic
- monitors runway activity and movement conflicts through RIMCAS
- integrates VACDM timestamps and states into tags and color rules
- integrates Hoppie CPDLC for datalink clearance workflows
- ships with a fixed-size, EuroScope-owned modeless Control Center

Control Center maintainers should also read the
[implementation notes](docs/CONTROL_CENTER.md) and run the
[deterministic test checklist](docs/CONTROL_CENTER_TEST_CHECKLIST.md) before
release.

## Repository Layout

The main EuroScope plugin project lives in [`vSMR/`](vSMR/):

```text
vSMR/
  src/         C++ implementation files
  include/     Project headers and resource IDs
  resources/   RC script, cursors, audio, and linker definition
  data/        Default runtime JSON files to copy next to vSMR.dll
  tools/       Maintenance scripts
```

## Main Features

- Advanced SMR radar display for ground movement and low-level airborne traffic
- Four icon styles: `Arrow`, `Diamond`, `Icons`, and `NOVA`
- Realistic icon rendering from optional PNG silhouettes and aircraft dimensions
- Ground and approach trail dots
- Predicted track line
- Tag auto-deconfliction
- Tower Mode that hides tags below taxi status while keeping aircraft icons visible
- Per-profile fonts, colors, icon settings, alerts, and tag layouts
- Tag definitions by target type and status
- Detailed hover tag definitions with optional linkage to the normal definition
- Structured rule engine for icon, tag, and text recoloring
- RIMCAS alerts for runway and movement conflicts
- Two approach/surface inset windows (`SRW 1` and `SRW 2`)
- Compact Runtime Menu for profile, mode, group, inset, and preset operations
- Reduced top toolbar for active-airport and radar-session tools
- VACDM integration for `TOBT`, `TSAT`, `TTOT`, `ASAT`, `AOBT`, `ATOT`, `ASRT`, `AORT`, `CTOT`, and event booking
- Hoppie CPDLC login, polling, and datalink UI integration
- Fixed-size modeless Control Center with live runtime synchronization

## Requirements

- EuroScope 32-bit
- A Win32 build of `vSMR.dll`
- The x86 [Microsoft Edge WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- A valid `vSMR_Profiles.json` in `vSMR_Data\` next to the DLL, or in the DLL folder for older flat installs

Optional runtime data:

- `vSMR_Maps.json`
- `ICAO_Airlines.txt`
- `ICAO_Aircraft.json`
- `AVISO\AVISO_*.geojson`
- `aircraft_icons\*.png`

## Installation

1. Build or obtain `vSMR.dll` for `Release | Win32`.
2. Install the x86 WebView2 Evergreen Runtime if it is not already installed.
3. Copy `Release\vSMR.dll` and the complete `Release\vSMR_webUI\` build output into your EuroScope plugin folder (or use the equivalent files from a packaged release).
4. Create `vSMR_Data\` next to `vSMR.dll`.
5. Copy `vSMR_Profiles.json` into `vSMR_Data\`.
6. Optionally copy the other runtime files described below into `vSMR_Data\`.
7. In EuroScope, open `Other Settings -> Plug-ins` and add `vSMR.dll`.
8. Open the vSMR radar display from EuroScope.

Important:

- `vSMR_Profiles.json` is required. vSMR checks `vSMR_Data\` first, then the DLL folder for existing installs.
- Keep the complete six-file `vSMR_webUI\` package next to `vSMR.dll`: four application assets plus the two files under `defaults\`. The Control Center does not use an external web server.
- `vSMR_Maps.json` is optional and uses the same search order.
- The repository already ships example runtime files under [`vSMR/data/`](vSMR/data/).

Example deployment layout:

```text
EuroScope\Plugins\
  vSMR.dll
  vSMR_webUI\
    index.html
    styles.css
    app.js
    data.js
    defaults\
      vSMR_Profiles.json
      AVISO_LFPG.geojson
  vSMR_Data\
    vSMR_Profiles.json
    vSMR_Maps.json
    ICAO_Aircraft.json
    AVISO\
      AVISO_LFPG.geojson
    aircraft_icons\
      a320.png
      b738.png
      e190.png
```

## Runtime Files

### Required

| File                 | Location                                             | Purpose                                                                                                    |
| -------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `vSMR_Profiles.json` | `vSMR_Data\`, then same folder as `vSMR.dll` fallback | Main profile database: fonts, labels, rules, colors, alerts, icon settings, legacy UI metadata, and more |
| `vSMR_webUI\*`       | Directory next to `vSMR.dll`                         | Four local application assets plus full profile/LFPG AVISO defaults used by the modeless Control Center    |

### Optional

| File                     | Search location(s)                                                                                           | Purpose                                                                            |
| ------------------------ | ------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------- |
| `vSMR_Maps.json`         | `vSMR_Data\`, then same folder as `vSMR.dll` fallback                                                        | Map element visibility by zoom level and optional active runway/airport conditions |
| `AVISO_*.geojson`        | `vSMR_Data\AVISO\`, then `vSMR_Data\`, then `<dll folder>\AVISO\`, then DLL folder fallback                  | Optional AVISO map overlays by airport, for example `AVISO_LFPG.geojson`           |
| `ICAO_Airlines.txt`      | DLL folder, then `..\..\ICAO\ICAO_Airlines.txt`, then `..\..\..\ICAO\ICAO_Airlines.txt`                      | Airline/callsign lookup for bottom-line text and related displays                  |
| `ICAO_Aircraft.json`     | `vSMR_Data\`, then `%APPDATA%\EuroScope\LFXX\Plugins`, then DLL folder, then DLL parent folder fallback      | Aircraft length and wingspan data used by realistic icons                          |
| `aircraft_icons\*.png`   | `vSMR_Data\aircraft_icons\`, then `<dll folder>\aircraft_icons\` fallback                                    | Optional per-aircraft realistic icon silhouettes                                   |

### Profile Metadata

`vSMR_Profiles.json` can include one metadata object at the end of the profile array. It has no `name` field, so it is not shown as a profile:

```json
{
  "_vsmr": {
    "schema_version": 1,
    "last_active_profile": "Default",
    "vacdm": {
      "server_url": "https://your-server.example"
    }
  }
}
```

Notes:

- `last_active_profile` replaces `vSMR_LastActiveProfile.txt`.
- `vacdm.server_url` replaces `vacdm.txt`.
- vSMR appends `/api/v1/pilots` internally.
- VACDM polling is enabled only when `_vsmr.vacdm.server_url` is not empty.
- A trailing `/` is trimmed from the configured server URL.

## Commands

The plugin responds to the following EuroScope command-line commands:

| Command              | Scope            | Effect                                                                 |
| -------------------- | ---------------- | ---------------------------------------------------------------------- |
| `.smr`             | Plugin           | Opens the CPDLC settings dialog                                        |
| `.smr connect`     | Plugin           | Connects or disconnects Hoppie CPDLC                                   |
| `.smr poll`        | Plugin           | Manually polls CPDLC messages when connected                           |
| `.smr reload`      | Plugin and radar | Reloads `vSMR_Profiles.json` for open SMR radar screens              |
| `.smr log`         | Plugin           | Toggles logging off/on (when enabled this defaults to `normal` mode) |
| `.smr log normal`  | Plugin           | Enables concise logging                                                |
| `.smr log verbose` | Plugin           | Enables detailed logging                                               |
| `.smr log off`     | Plugin           | Disables logging                                                       |
| `.smr log status`  | Plugin           | Prints current logging status and mode                                 |
| `.smr profile`     | Plugin           | Opens the Control Center `Profiles` page on the first active SMR radar screen |
| `.smr editor`      | Plugin           | Opens the Control Center `Display` page                               |
| `.smr vsmr`        | Plugin           | Alias for `.smr editor`                                               |
| `.smr config`      | Plugin           | Opens the Control Center `Settings` page                              |
| `.smr draw`        | Radar screen     | Toggles runway-area drawing                                            |
| `.smr status`      | Radar screen     | Prints current runway status information from RIMCAS                   |

### Logging Modes

- `normal`: concise logs for routine troubleshooting; suppresses function-signature traces and editor-initialization step spam.
- `verbose`: detailed logs for deep debugging; still suppresses known hot-loop trace spam.
- The log file is `vsmr.log` in the same folder as `vSMR.dll`.
- `.smr log on`, `.smr log enable`, and `.smr log 1` are accepted aliases for `normal`.
- `.smr log disable` and `.smr log 0` are accepted aliases for `off`.

## Radar Screen Behavior

### Top toolbar

The reduced grey toolbar is reserved for active-airport and radar-session
controls. From left to right it shows:

- active airport selector
- `QDR`
- `Target`
- `Lighting`
- `/` distance tool
- the `FPS A/C/R/T/S` performance readout when space permits

Profile selection, display modes, AVISO groups, inset visibility, and inset
presets belong to the Runtime Menu. Persistent profile, tag, icon, AVISO, and
RIMCAS editing belongs to the Control Center. They are intentionally absent
from the top toolbar.

### QDR menu

The QDR menu retains the two radar reference tools:

- `QDR Fixed Reference`, measured from the active airport
- `QDR Select Reference`, measured from a point selected on the radar

### Target menu

The Target menu retains immediate radar-session controls:

- afterglow
- ground trail dots
- approach trail dots
- predicted track line (PTL)
- acquire and release correlation actions

Icon style, icon sizing, label size, and typeface are configured only in the
Control Center and do not appear here.

### Lighting menu

The Lighting menu contains runtime visual adjustments:

- day/night mode
- label, symbol, and afterglow brightness

These are session-level multipliers and are distinct from the persisted color
values edited on the Control Center `Display > Colors` page.

### Runtime Menu and AVISO inset

The draggable five-icon Runtime Menu owns:

- display-mode selection
- AVISO group visibility
- AVISO, SRW 1, and SRW 2 visibility
- AVISO inset preset load/save/update/rename/duplicate/reset/delete operations
- the `Set default` / `Clear default` preset toggle and linked movement
- active-profile selection
- opening the Control Center

The AVISO inset itself is limited to viewport interaction: its title/drag
surface, `F` detach control, resize handles/dividers, pan, and zoom. It has no
duplicate close, preset, reload, or editor buttons. Show/hide and preset actions
are performed from the Runtime Menu; AVISO editing and reload are performed in
the Control Center.

### RIMCAS

RIMCAS alert types, monitored runways, closed-runway state, and Normal/LVP
visibility are configured on the Control Center `Alerts` page rather than the
top toolbar.

Supported RIMCAS alert labels in code:

- `NO PUSH`
- `NO TAXI`
- `NO TKOF`
- `STAT RPA`
- `RWY INC`
- `RWY TYPE`
- `RWY CLSD`
- `HIGH SPD`
- `EMERG`

### Other interactions

- Tags can be dragged.
- Tag auto-deconfliction rotates and repositions tags to reduce overlap.
- The distance tool is attached to the `/` toolbar button.
- The plugin exposes a `Datalink clearance` tag item and a `Datalink menu` tag function to EuroScope.

## vSMR Control Center

The Control Center is a fixed 728 x 500 modeless window titled `vSMR`. It is
owned by EuroScope, remains inside the EuroScope client area, and restores its
position from `%LOCALAPPDATA%\vSMR\control-center-window.json`. Open it from
the Runtime Menu gear or with one of the commands above.

Its primary pages are:

- `Display`, with `Colors`, `Icons`, `Tags`, and `Rules` tabs
- `AVISO`, for geometry/text editing and data reload/import
- `Alerts`, for RIMCAS types, runways, timers, visibility, and appearance
- `Groups`, for AVISO group definition, membership, and ordering
- `Modes`, for display-mode definitions and activation
- `Profiles`, for profile filters, lifecycle, and activation
- `Settings`, for connected runtime settings and data sources

Local `Update` buttons stage editor changes. `Revert` discards the current
draft, while the global blue `Save` validates and persists staged profile and
AVISO documents. Runtime profile/mode/group/inset actions remain immediate and
are synchronized between the Control Center and Runtime Menu.

### Display editor

The Display page provides a hierarchical profile-color editor, icon style and
size controls, normal/detailed tag definitions with token insertion, global tag
behavior and typography, and structured target/tag/text color rules.

### AVISO, Alerts, and Groups

AVISO geometry, text, source loading, and reload are centralized in the
Control Center. Alert configuration likewise lives only on the Alerts page;
the old top-bar alert lists are not retained. Group definition and membership
are edited here, while visibility remains a quick Runtime Menu operation.

### Modes and Profiles

The Modes page creates and edits display modes, including requirements and
visible statuses. The Profiles page creates, duplicates, renames, deletes, and
activates profiles and edits altitude, speed, range, and night-alpha filters.

## Tag Definitions

Tag definitions are profile-backed and depend on:

- target type
- target status
- whether the tag is shown in normal or detailed mode

Control Center tag-editor target types:

- `departure`
- `arrival`

Compatibility notes:

- legacy `airborne` and `uncorrelated` sections are still read and migrated for backward compatibility
- airborne statuses are normalized into `departure`/`arrival` status definitions

Supported statuses by type:

- `departure`: `default`, `nofpl`, `push`, `stup`, `taxi`, `depa`, `airdep`, `airdep_onrunway`
- `arrival`: `default`, `nofpl`, `airarr`, `airarr_onrunway`

### Tag tokens

Supported tag-definition tokens in code:

- `callsign`
- `actype`
- `sctype`
- `sqerror`
- `deprwy`
- `seprwy`
- `arvrwy`
- `srvrwy`
- `gate`
- `sate`
- `flightlevel`
- `gs`
- `tobt`
- `tsat`
- `ttot`
- `asat`
- `aobt`
- `atot`
- `asrt`
- `aort`
- `ctot`
- `event_booking`
- `tendency`
- `wake`
- `ssr`
- `asid`
- `ssid`
- `origin`
- `dest`
- `groundstatus`
- `clearance`
- `systemid`
- `uk_stand`
- `remark`
- `scratchpad`

## Structured Rules

Structured rules are stored under `rules.items` in the active profile.
Older `labels.rules.items` layouts are still read/migrated for compatibility.

Each rule can define:

- one or more criteria
- optional tag-type filtering
- optional status filtering
- optional detail filtering
- icon color override
- tag color override
- text color override

### Rule sources and tokens

| Source     | Tokens                                                                                   |
| ---------- | ---------------------------------------------------------------------------------------- |
| `vacdm`  | `tobt`, `tsat`, `ttot`, `asat`, `aobt`, `atot`, `asrt`, `aort`, `ctot` |
| `runway` | `deprwy`, `seprwy`, `arvrwy`, `srvrwy`                                           |
| `custom` | `asid`, `ssid`, `deprwy`, `seprwy`, `arvrwy`, `srvrwy`                       |

### Rule conditions

The condition dropdown is dynamic:

- `runway`: `any`, `set`, `missing`
- `custom`: `any`, `set`, `missing`, `in: ...`, `not_in: ...`
- `tobt`: `any`, `set`, `missing`, `inactive`, `unconfirmed`, `confirmed`, `unconfirmed_delay`, `confirmed_delay`, `expired`
- `tsat`: `any`, `set`, `missing`, `inactive`, `future`, `valid`, `expired`, `future_ctot`, `valid_ctot`, `expired_ctot`
- other VACDM tokens: `any`, `set`, `missing`, `future`, `past`

Context filters:

- tag type (editor): `any`, `departure`, `arrival`
- detail: `any`, `normal`, `detailed`
- status (editor):
  - departure: `Any`, `No Status`, `No FPL`, `Push`, `Startup`, `Taxi`, `Departure`, `Airborne`, `On Runway`
  - arrival: `Any`, `On Ground`, `No FPL`, `Airborne`, `On Runway`

## VACDM Integration

VACDM pilot data is polled in the plugin and exposed to tag rendering and rule evaluation.

Behavior visible in code:

- polling is opt-in and starts only when `_vsmr.vacdm.server_url` is set in `vSMR_Profiles.json`
- URL used by polling is `<SERVER_URL>/api/v1/pilots`
- polling interval: 15 seconds
- polling waits for a stable EuroScope network connection before fetching
- callsign matching uses multiple normalized candidates
- `TOBT` falls back to flight plan `EOBT` when backend data is missing

VACDM values used by vSMR include:

- `TOBT`
- `TSAT`
- `TTOT`
- `ASAT`
- `AOBT`
- `ATOT`
- `ASRT`
- `AORT`
- `CTOT`
- event booking flag

## CPDLC / Hoppie Integration

CPDLC behavior is handled at plugin level.

Features:

- settings dialog opened by `.smr`
- Hoppie login/logout with `.smr connect`
- manual poll with `.smr poll`
- optional sound on clearance request
- datalink menu integration in tags

Saved EuroScope settings:

- `cpdlc_logon`
- `cpdlc_password`
- `cpdlc_sound`

The notification sound is compiled into the plugin resources from `Ding.wav`.

## Configuration Model

The main profile file is a JSON array. Each profile object currently uses top-level sections such as:

- `name`
- `font`
- `filters`
- `sid_text_colors`
- `labels`
- `rimcas`
- `targets`
- `approach_insets`
- `maps`
- `ui_layout`

### `font`

Controls:

- available font list
- font name
- weight
- size presets
- active label font size

### `filters`

Controls:

- altitude and speed visibility filters
- radar range
- night alpha
- pro mode behavior
- tower mode tag visibility (missing status, `NSTS`, `PUSH`, and `STUP` are icon-only)

### `labels`

Controls:

- tag auto-deconfliction
- leader line length
- gate/speed behavior
- squawk error color
- per-type tag definitions
- detailed definition linkage
- structured color rules

### `rimcas`

Controls:

- timers
- stage-two threshold
- warning/caution text and background colors
- inactive alert list

### `targets`

Controls:

- primary target display
- icon style
- ground icon behavior
- history colors
- base target color
- fixed pixel icon size
- fixed triangle scale
- small icon boost and resolution presets

### `approach_insets`

Controls:

- extended line length
- tick spacing
- line color
- runway color
- background color

### `ui_layout`

Legacy profiles may still contain `profile_editor_window`. The current
fixed-size Control Center does not use that profile field; its position is
stored in `%LOCALAPPDATA%\vSMR\control-center-window.json`.

## Maps

`vSMR_Maps.json` is a JSON array of map visibility entries. Each entry can define:

- `zoomLevel`
- `element`
- optional `active`

The optional `active` string is used to conditionally show sector elements based on airport and runway configuration. Example from the shipped file:

```text
LFPG:DEP:26R:ARR:26L
```

## Icons Aircraft Rendering

When `Icons` style is active (internally stored as `realistic`), vSMR can combine:

- aircraft dimensions from `vSMR_Data\ICAO_Aircraft.json`
- optional PNG silhouettes from `vSMR_Data\aircraft_icons\`
- WTC-based fallbacks when dimensions are missing

`ICAO_Aircraft.json` supports both:

- the native vSMR schema
- the alternate GNG-style schema

## Building From Source

### Toolchain

- Visual Studio 2022
- MSVC toolset `v143`
- C++17
- `Release | Win32`
- NuGet restore access to `Microsoft.Web.WebView2` version `1.0.4078.44`

### Important build notes

- EuroScope support is Win32 only here
- The project uses MFC
- The project links against `EuroScopePlugInDll.lib`
- The WebView2 loader is linked statically; do not deploy `WebView2Loader.dll`
- The x86 Evergreen Runtime remains required on the EuroScope computer
- The Control Center resources are local, so normal operation does not require internet access
- HTTP downloading is currently performed through WinHTTP (`HttpHelper.cpp`)
- `libcurl` headers/libs are still referenced in project settings for compatibility
- `winmm.lib` is linked for sound-related functionality

### Build command

```powershell
msbuild vSMR.sln /t:Restore /p:Configuration=Release /p:Platform=Win32
msbuild vSMR.sln /t:Build /p:Configuration=Release /p:Platform=Win32
```

Output:

```text
Release\vSMR.dll
Release\vSMR_webUI\
  index.html
  styles.css
  app.js
  data.js
  defaults\
    vSMR_Profiles.json
    AVISO_LFPG.geojson
```

### Offline dependencies

For an offline build, download `Microsoft.Web.WebView2.1.0.4078.44.nupkg` on a connected computer, place it in a local NuGet feed, and restore with `/p:RestoreSources=C:\path\to\offline-feed` before building.

For an offline EuroScope computer, download the **Evergreen Standalone Installer (x86)** on a connected computer and run it on the target machine. A silent installation can use:

```powershell
MicrosoftEdgeWebView2RuntimeInstallerX86.exe /silent /install
```

Deploy `vSMR.dll`, `vSMR_webUI\`, and `vSMR_Data\` together as shown above. The static loader removes the separate `WebView2Loader.dll` deployment dependency; it does not embed the Evergreen Runtime.

## Source Tree Guide

This is the quickest code map for new contributors:

| Path                                      | Responsibility                                                                         |
| ----------------------------------------- | -------------------------------------------------------------------------------------- |
| `vSMR/vSMR.cpp`                         | DLL entry point and EuroScope plugin export                                            |
| `vSMR/SMRPlugin.*`                      | Main plugin object, commands, CPDLC, VACDM polling, tag item registration              |
| `vSMR/SMRRadar.cpp`                     | Core radar screen lifecycle, rendering, menus, toolbar, target drawing                 |
| `vSMR/SMRRadar_RadarAndCommands.cpp`    | Radar-side command handling and some target geometry logic                             |
| `vSMR/SMRRadar_ScreenInteraction.cpp`   | Click handling, popup menus, dragging, tag interaction                                 |
| `vSMR/SMRRadar_FunctionCall.cpp`        | Popup function handlers and profile-backed menu actions                                |
| `vSMR/SMRRadar_TagDefinitions.cpp`      | Tag token handling, type/status normalization, structured rule parsing and persistence |
| `vSMR/SMRRadar_TagRendering.cpp`        | Tag drawing logic                                                                      |
| `vSMR/SMRRadar_TargetsAndFonts.cpp`     | Target display and font handling helpers                                               |
| `vSMR/SMRRadar_AircraftAndAsr.cpp`      | Aircraft dimensions, realistic icon data, ASR persistence                              |
| `vSMR/Config.*`                         | JSON config and map loading/saving                                                     |
| `vSMR/Rimcas.*`                         | RIMCAS alerting and runway monitoring                                                  |
| `vSMR/ProfileEditorDialog.*`            | Legacy native editor implementation retained for compatibility; current entry points use the Control Center |
| `vSMR/SMRRadar_ProfileEditorWindow.cpp` | Shared profile/mode editing APIs plus legacy editor lifecycle                           |
| `vSMR/src/VsmrControlCenter*.cpp`, `vSMR/include/VsmrControlCenter*.hpp` | Fixed-size WebView2 Control Center host, bridge, lifecycle, and runtime synchronization |
| `vSMR/src/SMRRadar_RuntimeMenu.cpp`     | Native five-icon Runtime Menu and inset-preset operations                              |
| `vSMR/InsetWindow.*`                    | Approach/surface inset windows                                                         |
| `vSMR/CallsignLookup.*`                 | Airline/callsign lookup from `ICAO_Airlines.txt`                                     |
| `vSMR/HttpHelper.*`                     | HTTP downloading helper (WinHTTP path used in current implementation)                  |
| `vSMR/DataLinkDialog.*`                 | CPDLC datalink dialog                                                                  |
| `vSMR/CPDLCSettingsDialog.*`            | CPDLC settings dialog                                                                  |

## Runtime Flow (Developer Quick Map)

This is the high-level execution flow when EuroScope loads and runs the plugin:

1. `SMRPlugin` is created, registers display/tag extensions, loads CPDLC settings, and initializes VACDM polling configuration.
2. EuroScope creates one `SMRRadar` instance per opened vSMR display window.
3. `SMRRadar` loads profiles/maps/resources, then drives drawing and interaction in `OnRefresh`.
4. `SMRPlugin::OnTimer` handles periodic tasks (CPDLC polling, VACDM fetch scheduling, status blinking, and cleanup).
5. Radar/profile state is persisted through profile JSON and ASR keys (`Airport`, `ActiveProfile`, SRW window geometry, trail options).
6. `.smr reload` reloads JSON config and reapplies profiles across currently opened radar windows.

## Troubleshooting

- If the plugin fails to load its profile data, validate `vSMR_Data\vSMR_Profiles.json` first.
- If the Control Center restores outside the visible EuroScope area, remove
  `%LOCALAPPDATA%\vSMR\control-center-window.json`; it will reopen centered and
  clamped inside EuroScope.
- If realistic icons do not appear, verify `targets.icon_style`, `vSMR_Data\ICAO_Aircraft.json`, and `vSMR_Data\aircraft_icons\`.
- If airline names are missing, verify `ICAO_Airlines.txt` in one of the supported search paths.
- If VACDM fields stay empty, verify that `_vsmr.vacdm.server_url` is set in `vSMR_Data\vSMR_Profiles.json`, then check callsign matching and backend data availability.
- If `.smr profile`, `.smr editor`, or `.smr config` does nothing, make sure at
  least one SMR radar screen is open and the x86 WebView2 Runtime is installed.

## Credits

Special thanks to Alexis.B, Baptiste.C, Steve.A and Yohannes.D.
