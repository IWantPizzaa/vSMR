# Custom vSMR for EuroScope

vSMR is a EuroScope plugin that provides an advanced surface movement radar (SMR) display, realistic tags, and an integrated profile editor.

This README is both a user guide and a quick developer reference.

This project was initially forked from https://github.com/AlexisBalzano/vSMR, which was itself forked from https://github.com/pierr3/vSMR.

## Features

- Advanced SMR display with custom target icons and history trails.
- Realistic tag system with configurable layouts and hover details.
- RIMCAS simulation and alerting.
- Approach inset windows.
- VACDM integration (TOBT/TSAT/TTOT/ASAT/AOBT/ATOT/ASRT/AORT/CTOT + Event Booking) with state-aware colors.
- CPDLC/Hoppie datalink clearance support.
- Detachable, live-updating Profile Editor (Colors, Icons, Rules, Tags).

## Requirements

- EuroScope (32-bit).
- vSMR `vSMR.dll` built for Win32.
- Optional: `ICAO_Airlines.txt` (see **Airline Lookup**).

## Installation (Users)

1. Build or download `vSMR.dll` and place it in your EuroScope plugin folder.
2. In EuroScope: `Other Settings -> Plugins` and add `vSMR.dll`.
3. Start EuroScope and open the vSMR radar display.
4. The plugin will generate `vSMR_Profiles.json` and `vSMR_Maps.json` in the same folder as the DLL on first run.

### Airline Lookup

Place `ICAO_Airlines.txt` next to `vSMR.dll`. The plugin will also search:

- `..\..\ICAO\ICAO_Airlines.txt`
- `..\..\..\ICAO\ICAO_Airlines.txt`

## Usage

### Commands

Open the EuroScope command line and use:

- `.smr reload`   reloads `vSMR_Profiles.json` in all open SMR screens.
- `.smr draw`   toggle runway drawing.
- `.smr status`   prints runway status from RIMCAS.
- `.smr log`   toggles vSMR logging.
- `.smr connect`   connect/disconnect to Hoppie CPDLC.
- `.smr poll`   manual CPDLC message poll.
- `.smr`   opens the CPDLC settings dialog.

### Profile Editor

Open the Profile Editor from the vSMR UI. It is modeless and updates live.

#### Colors Tab

- Left: hierarchical color list (expand/collapse).
- Right: live preview, color wheel, value + opacity sliders, RGBA/HEX inputs.
- Apply/Reset updates the active profile immediately.

#### Icons Tab

- Icon style: Arrow, Diamond, Realistic.
- Fixed pixel mode and size scale.
- Small icon boost and resolution presets.

#### Rules Tab (Structured Rules Engine)

Rules let you color targets, tag backgrounds, and text based on token states.

- Choose **Source** (e.g., `vacdm`) and **Token** (e.g., `tsat`).
- The **Condition** list adapts to the token.
- Apply colors to **Icons**, **Tag**, and **Text** using the rule color wheel.
- Rules apply live and are saved in the profile.

#### Tags Tab

- Define L1-L4 tag lines for each type/status.
- Insert tokens from the dropdown.
- Toggle **Custom Hover Details** to define separate L1-L4 for hover.
- Live preview updates immediately.

### VACDM Tokens

VACDM data is shown through tokens in tags and rules.

Supported tokens:

- `tobt`, `tsat`, `ttot`, `asat`, `aobt`, `atot`, `asrt`, `aort`, `ctot`, `event_booking`

Color rules can be set per token state (for example valid/expired/scheduled). The exact condition list is provided by the Rules tab after selecting a token.

## Files and Configuration

- `vSMR_Profiles.json`
  - All profile settings (colors, rules, tags, fonts, icon settings, alerts, editor layout).
- `vSMR_Maps.json`
  - Map/sector display configuration.

Both files live in the same folder as `vSMR.dll`.

## Troubleshooting

- **Tags or colors not updating**: run `.smr reload` and verify the profile is active.
- **VACDM tokens empty**: ensure VACDM is active for the event and the callsign matches the server data.
- **Missing airline names**: add `ICAO_Airlines.txt` to the plugin folder.
- **Profile Editor window not visible**: check Windows off-screen placement or delete `ui_layout.profile_editor_window` in `vSMR_Profiles.json`.

## Building (Developers)

### Visual Studio

- Open `vSMR.sln`.
- Build `Release | Win32`.
- Output: `Release\vSMR.dll`.

### Notes

- EuroScope is 32-bit; use Win32 builds only.
- The plugin uses MFC (`_AFXDLL`) and static curl.

## License

GPL v3. See `LICENSE` for details.

## Credits

Special thanks to Baptise.C and Steve.A.
