# vSMR for EuroScope

vSMR is a Win32 EuroScope plugin that provides a surface movement radar display with configurable target symbols, profile-driven tags and colors, RIMCAS alerts, approach inset windows, VACDM integration, and Hoppie CPDLC support.

This repository is a maintained fork of:

- https://github.com/AlexisBalzano/vSMR
- https://github.com/pierr3/vSMR

The plugin name reported to EuroScope is `vSMR`, version `v1.1.0`.

## Overview

vSMR is not a standalone application. It is a EuroScope plugin DLL that:

- creates a custom SMR radar screen
- renders ground and airborne targets with multiple symbol styles
- builds tag text from profile-defined token layouts
- colors icons, tag backgrounds, and text from profile colors and rule logic
- monitors runway activity and movement conflicts through RIMCAS
- integrates VACDM timestamps and states into tags and color rules
- integrates Hoppie CPDLC for datalink clearance workflows
- ships with a detachable live profile editor

## Main Features

- Advanced SMR radar display for ground movement and low-level airborne traffic
- Three icon styles: `Arrow`, `Diamond`, and `Realistic`
- Realistic icon rendering from optional PNG silhouettes and aircraft dimensions
- Ground and approach trail dots
- Predicted track line
- Tag auto-deconfliction
- Per-profile fonts, colors, icon settings, alerts, and tag layouts
- Tag definitions by target type and status
- Detailed hover tag definitions with optional linkage to the normal definition
- Structured rule engine for icon, tag, and text recoloring
- RIMCAS alerts for runway and movement conflicts
- Two approach/surface inset windows (`SRW 1` and `SRW 2`)
- Top toolbar menus for display, targets, colors, alerts, and distance tools
- VACDM integration for `TOBT`, `TSAT`, `TTOT`, `ASAT`, `AOBT`, `ATOT`, `ASRT`, `AORT`, `CTOT`, and event booking
- Hoppie CPDLC login, polling, and datalink UI integration
- Detached modeless profile editor with live updates

## Requirements

- EuroScope 32-bit
- A Win32 build of `vSMR.dll`
- A valid `vSMR_Profiles.json` next to the DLL

Optional runtime data:

- `vSMR_Maps.json`
- `ICAO_Airlines.txt`
- `ICAO_Aircraft.json`
- `aircraft_icons\*.png`
- `vacdm.txt`

## Installation

1. Build or obtain `vSMR.dll` for `Release | Win32`.
2. Copy `vSMR.dll` into your EuroScope plugin folder.
3. Copy `vSMR_Profiles.json` into the same folder as `vSMR.dll`.
4. Optionally copy the other runtime files described below.
5. In EuroScope, open `Other Settings -> Plug-ins` and add `vSMR.dll`.
6. Open the vSMR radar display from EuroScope.

Important:

- `vSMR_Profiles.json` is required. The code expects it to exist and parses it on startup.
- `vSMR_Maps.json` is optional.
- The repository already ships example runtime files under [`vSMR/`](vSMR/).

Example deployment layout:

```text
EuroScope\Plugins\
  vSMR.dll
  vSMR_Profiles.json
  vSMR_Maps.json
  ICAO_Aircraft.json
  vacdm.txt
  aircraft_icons\
    a320.png
    b738.png
    e190.png
```

## Runtime Files

### Required

| File                   | Location                    | Purpose                                                                                                    |
| ---------------------- | --------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `vSMR_Profiles.json` | Same folder as `vSMR.dll` | Main profile database: fonts, labels, rules, colors, alerts, icon settings, editor window layout, and more |

### Optional

| File                     | Search location(s)                                                                          | Purpose                                                                            |
| ------------------------ | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `vSMR_Maps.json`       | Same folder as `vSMR.dll`                                                                 | Map element visibility by zoom level and optional active runway/airport conditions |
| `vacdm.txt`            | Same folder as `vSMR.dll`                                                                 | Overrides the VACDM base server URL                                                |
| `ICAO_Airlines.txt`    | DLL folder, then `..\..\ICAO\ICAO_Airlines.txt`, then `..\..\..\ICAO\ICAO_Airlines.txt` | Airline/callsign lookup for bottom-line text and related displays                  |
| `ICAO_Aircraft.json`   | `%APPDATA%\EuroScope\LFXX\Plugins`, then DLL folder, then DLL parent folder               | Aircraft length and wingspan data used by realistic icons                          |
| `aircraft_icons\*.png` | `<dll folder>\aircraft_icons\`                                                            | Optional per-aircraft realistic icon silhouettes                                   |

### `vacdm.txt` format

If present, the file is parsed as simple `key=value` text. The supported key is:

```text
SERVER_URL=https://your-server.example
```

vSMR appends `/api/v1/pilots` internally.

If `vacdm.txt` is missing, the default endpoint is:

```text
https://app.vacdm.net/api/v1/pilots
```

## Commands

The plugin responds to the following EuroScope command-line commands:

| Command          | Scope            | Effect                                                                 |
| ---------------- | ---------------- | ---------------------------------------------------------------------- |
| `.smr`         | Plugin           | Opens the CPDLC settings dialog                                        |
| `.smr connect` | Plugin           | Connects or disconnects Hoppie CPDLC                                   |
| `.smr poll`    | Plugin           | Manually polls CPDLC messages when connected                           |
| `.smr reload`  | Plugin and radar | Reloads `vSMR_Profiles.json` for open SMR radar screens              |
| `.smr log`     | Plugin           | Toggles plugin logging                                                 |
| `.smr profile` | Plugin           | Opens the detached profile editor on the first active SMR radar screen |
| `.smr editor`  | Plugin           | Alias for `.smr profile`                                             |
| `.smr config`  | Plugin           | Alias for `.smr profile`                                             |
| `.smr draw`    | Radar screen     | Toggles runway-area drawing                                            |
| `.smr status`  | Radar screen     | Prints current runway status information from RIMCAS                   |

## Radar Screen Behavior

### Top toolbar

The radar screen draws a top toolbar with:

- active airport selector
- `Display`
- `Target`
- `Colours`
- `Alerts`
- `/` distance tool

### Display menu

The display menu includes:

- active airport selection
- profile selection
- `SRW 1` and `SRW 2` inset window toggles

### Target menu

The target menu includes profile-backed controls such as:

- icon style
- fixed pixel icon sizing
- fixed size scale
- small icon boost
- boost factor
- resolution preset
- trail settings
- predicted track line

### Colours menu

The colors menu includes:

- day/night mode
- label font size
- tag font selection

### Alerts menu

The alerts menu includes:

- active alert toggles
- monitored runway controls
- runway state related settings

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

## Profile Editor

The profile editor is a detached, modeless window. It updates the active radar screen live and persists its own window rectangle in `ui_layout.profile_editor_window` inside `vSMR_Profiles.json`.

Current pages:

- `Colors`
- `Icons & Tags`
- `Rules`
- `Profiles`

### Colors

The Colors page provides:

- a hierarchical color tree on the left
- a live preview
- color wheel editing
- value and opacity sliders
- RGBA and HEX editing
- immediate apply/reset against the active profile

### Icons & Tags

The Icons & Tags page combines icon configuration and tag definition editing.

Icon controls include:

- shape selection: `Arrow`, `Diamond`, `Realistic`
- fixed pixel mode
- fixed size scale
- small icon boost
- boost factor
- display resolution presets: `1080p`, `2K`, `4K`

Tag controls include:

- tag type selection
- status selection
- token insertion dropdown
- a multiline normal definition editor
- `Custom Hover Details` toggle
- a multiline detailed definition editor

### Rules

The Rules page edits the structured rule engine used to recolor:

- target icons
- tag backgrounds
- tag text

A rule can contain multiple criteria. The UI supports:

- adding and removing rules
- adding extra parameters to a rule
- selecting rule source, token, and condition
- limiting by tag type, status, and detail level
- editing icon/tag/text output colors

### Profiles

The Profiles page provides:

- profile list
- add
- duplicate
- rename
- delete
- an About panel with project links

## Tag Definitions

Tag definitions are profile-backed and depend on:

- target type
- target status
- whether the tag is shown in normal or detailed mode

Supported target types:

- `departure`
- `arrival`
- `airborne`
- `uncorrelated`

Supported statuses by type:

- `departure`: `default`, `nofpl`, `nsts`, `push`, `stup`, `taxi`, `depa`
- `arrival`: `default`, `nofpl`, `arr`, `taxi`
- `airborne`: `default`, `airdep`, `airarr`, `airdep_onrunway`, `airarr_onrunway`
- `uncorrelated`: `default`

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

Structured rules are stored under `labels.rules.items` in the active profile.

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

- tag type: `any`, `departure`, `arrival`, `airborne`, `uncorrelated`
- detail: `any`, `normal`, `detailed`
- status: `any` or one of the normalized tag statuses

## VACDM Integration

VACDM pilot data is polled in the plugin and exposed to tag rendering and rule evaluation.

Behavior visible in code:

- default pilots URL: `https://app.vacdm.net/api/v1/pilots`
- optional override through `vacdm.txt`
- polling interval: 15 seconds
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

Currently used for at least:

- `profile_editor_window`

## Maps

`vSMR_Maps.json` is a JSON array of map visibility entries. Each entry can define:

- `zoomLevel`
- `element`
- optional `active`

The optional `active` string is used to conditionally show sector elements based on airport and runway configuration. Example from the shipped file:

```text
LFPG:DEP:26R:ARR:26L
```

## Realistic Aircraft Icons

When `Realistic` icon style is active, vSMR can combine:

- aircraft dimensions from `ICAO_Aircraft.json`
- optional PNG silhouettes from `aircraft_icons\`
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

### Important build notes

- EuroScope support is Win32 only here
- The project uses MFC
- The project links against `EuroScopePlugInDll.lib`
- Networking uses `libcurl` with WinHTTP fallback
- `winmm.lib` is linked for sound-related functionality

### Build command

```powershell
msbuild vSMR.sln /t:Build /p:Configuration=Release /p:Platform=Win32
```

Output:

```text
Release\vSMR.dll
```

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
| `vSMR/ProfileEditorDialog.*`            | Detached profile editor UI                                                             |
| `vSMR/SMRRadar_ProfileEditorWindow.cpp` | Profile editor window creation, persistence, and lifecycle                             |
| `vSMR/InsetWindow.*`                    | Approach/surface inset windows                                                         |
| `vSMR/CallsignLookup.*`                 | Airline/callsign lookup from `ICAO_Airlines.txt`                                     |
| `vSMR/HttpHelper.*`                     | HTTP downloading via libcurl with WinHTTP fallback                                     |
| `vSMR/DataLinkDialog.*`                 | CPDLC datalink dialog                                                                  |
| `vSMR/CPDLCSettingsDialog.*`            | CPDLC settings dialog                                                                  |

## Troubleshooting

- If the plugin fails to load its profile data, validate `vSMR_Profiles.json` first.
- If the profile editor opens off-screen, remove or correct `ui_layout.profile_editor_window`.
- If realistic icons do not appear, verify `targets.icon_style`, `ICAO_Aircraft.json`, and `aircraft_icons\`.
- If airline names are missing, verify `ICAO_Airlines.txt` in one of the supported search paths.
- If VACDM fields stay empty, verify the server URL, callsign matching, and backend data availability.
- If `.smr profile` does nothing, make sure at least one SMR radar screen is open.

## License

GPL v3. See [LICENSE](LICENSE).

## Credits

Special thanks to Baptiste.C and Steve.A.
