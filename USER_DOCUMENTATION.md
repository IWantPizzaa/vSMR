# vSMR User Documentation (Customized Build)

This document describes the user-facing modifications that were implemented in this customized vSMR build.

## Overview

The plugin now uses a profile-driven customization workflow. You can edit behavior and visuals from the UI, and settings are persisted in `vSMR_Profiles.json`.

Main additions:

- A full **Profile Editor** UI for tag definitions and status-based styling.
- A modern **Profile Color Picker** (wheel + value + opacity + RGB/A + HEX).
- Status-based definitions/colors for **Departure**, **Arrival**, and **Airborne**.
- Separate **Arrival/Departure** styling for gate, taxi, airborne, and runway contexts.
- Selectable icon style (**Realistic** / **Triangle**) and advanced icon scaling options.
- Tag font management from UI + profile JSON.
- New token capabilities:
  - `csid` (colored SID text logic from profile JSON)
  - custom token text color syntax: `token(r,g,b)` (example: `callsign(255,0,255)`)
  - customizable clearance token display text

## Top Bar Menu Locations

### Display Menu

- `Profiles`: switch active profile.
- `Icon Style`: icon style and size behavior.
- `Profile Editor`: unified editor for type/status colors and definitions.

### Colours Menu

- `Profile Colors`: open full profile color list and color picker.
- `Colour Settings`: day/night mode options.
- `Brightness`: label/symbol/afterglow brightness controls.

### Target Menu

- `Label Font Size`
- `Tag Font`
- trail and predicted track settings

## Profiles

Default profile set includes:

- `Default`
- `Default Pro`
- `Custom 1`
- `Custom 2`
- `Custom 3`

All profile-specific settings are stored in:

- local project copy: `vSMR/vSMR_Profiles.json`
- runtime EuroScope copy: `%AppData%\\EuroScope\\...\\Plugins\\vSMR_Profiles.json`

When editing JSON manually, keep valid JSON syntax and restart/reload plugin.

## Profile Editor (Unified Editor)

The Profile Editor combines:

- target icon color selection
- label background color selection
- definition editing
- definition detailed editing
- status-specific editing
- live preview

### Type selector

- `Departure`
- `Arrival`
- `Airborne`

### Status selector by type

Departure statuses:

- `Default`
- `No FPL`
- `NSTS`
- `PUSH`
- `STUP`
- `TAXI`
- `DEPA`

Arrival statuses:

- `Default`
- `No FPL`
- `ARR`
- `TAXI`

Airborne statuses:

- `Default`
- `Airborne Departure`
- `Airborne Arrival`
- `Airborne Departure On Runway`
- `Airborne Arrival On Runway`

### Definition editor behavior

- Editable lines are `L1` to `L4`.
- `Insert Token...` inserts a token at current line.
- `Insert Bold...` inserts token with bold style.
- Definitions are saved to JSON.
- If all lines are emptied, `callsign` is automatically kept on `L1` (safety fallback).
- Scratchpad behavior is token-based (`scratchpad`) and no longer forces blank lines.

## Available Definition Tokens

Supported tokens:

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
- `csid`
- `ssid`
- `origin`
- `dest`
- `groundstatus`
- `clearance`
- `systemid`
- `uk_stand`
- `remark`
- `scratchpad`

Notes:

- `tobt` and `tsat` use VACDM-style state coloring in live tag rendering.

## Token Styling Syntax

### Bold token

Any of these are valid:

- `*callsign`
- `b:callsign`
- `bold:callsign`

### Custom token text color (new)

Syntax:

- `token(r,g,b)`

Examples:

- `callsign(255,0,255)`
- `asid(255,180,40)`
- `b:csid(0,255,200)`

Rules:

- `r`, `g`, `b` must be integers in `0..255`.
- Works in live tags and Profile Editor preview.
- Applies to that rendered token text.
- Custom token color has priority over default token text color.

### VACDM programmable color rules (new)

You can add VACDM state-based color directives directly inside definition lines.

Supported VACDM rule tokens:

- `tobt`
- `tsat`
- `ttot`
- `asat`
- `aobt`
- `atot`
- `asrt`
- `aort`
- `ctot`

Display-only VACDM token (no state rule syntax):

- `event_booking`

Rule syntax:

- `token(state_<state_name>=[scope,color_key="r,g,b",...])`

Scopes:

- `target` -> target symbol color
- `tag` -> tag background color
- `text` -> default tag text color

Color keys:

- `color_target="r,g,b"`
- `color_tag="r,g,b"`
- `color_text="r,g,b"`
- `color="r,g,b"` (shared color for requested scopes)

Notes:

- Rule tokens are control directives and are not printed as text in the tag.
- Rules are evaluated live (per aircraft state).
- If multiple rules match, later matching rules override earlier ones.

Examples:

```text
tobt(state_unconfirmed=[text,color_text="127,252,73"])
tobt(state_confirmed=[text,color_text="0,181,27"])
tobt(state_unconfirmed_delay=[text,color_text="255,255,191"])
tobt(state_confirmed_delay=[text,color_text="255,255,0"])
tobt(state_expired=[text,color_text="255,153,0"])
```

```text
tsat(state_future=[text,color_text="127,252,73"])
tsat(state_valid=[text,color_text="0,181,27"])
tsat(state_expired=[text,color_text="255,153,0"])
tsat(state_future_ctot=[text,color_text="102,204,255"])
tsat(state_valid_ctot=[text,color_text="0,102,255"])
tsat(state_expired_ctot=[text,color_text="255,80,80"])
```

```text
ttot(state_future=[target,color_target="0,150,255"])
ttot(state_past=[target,color_target="255,110,0"])
asat(state_set=[text,color_text="220,220,220"])
aobt(state_set=[text,color_text="255,210,0"])
atot(state_set=[text,color_text="255,160,0"])
asrt(state_set=[tag,color_tag="70,120,70"])
aort(state_set=[tag,color_tag="70,90,140"])
ctot(state_set=[target,tag,color="90,160,255"])
asat(state_missing=[tag,color_tag="80,80,80"])
```

```text
tobt(state_confirmed=[target,tag,text,color="0,181,27"])
```

Common state names:

- Generic: `any`, `set`, `missing`, `future`, `past`
- TOBT: `unconfirmed`, `confirmed`, `unconfirmed_delay`, `confirmed_delay`, `expired`, `inactive`
- TSAT: `future`, `valid`, `expired`, `future_ctot`, `valid_ctot`, `expired_ctot`

Alias examples also accepted:

- `state_green` -> TOBT confirmed
- `state_lightgreen` -> TOBT unconfirmed
- `state_yellow` -> TOBT confirmed_delay
- `state_blue` -> TSAT valid_ctot
- `state_lightblue` -> TSAT future_ctot
- `state_red` -> TSAT expired_ctot

### Runway-based programmable color rules (new)

You can conditionally recolor a tag based on a specific runway value shown by runway tokens.

Supported runway rule tokens:

- `deprwy`
- `seprwy`
- `arvrwy`
- `srvrwy`

Syntax:

- `runway_token(runway_<value>=[tag|text|target,color_tag="r,g,b",...])`

Condition aliases accepted:

- `runway_26R`
- `rwy_26R`
- `value_26R`
- direct value: `26R`

Examples:

```text
deprwy(runway_26R=[tag,color_tag="65,95,180"])
deprwy(runway_26L=[tag,color_tag="180,95,65"])
arvrwy(rwy_08L=[tag,text,color="60,120,60"])
srvrwy(value_27R=[target,color_target="255,170,0"])
```

Special conditions:

- `set` / `any`: runway value is available
- `missing`: no runway value available

### Clearance token custom display

Default clearance token:

- `clearance` (or `cleared`) shows default unchecked/checked symbols.

Custom display syntax:

- `clearance(not_cleared_text,cleared_text)`

Examples:

- `clearance(C,C)` -> always `C`
- `clearance([], [x])` -> classic checkbox style
- `clearance(, [x])` -> hidden when not cleared, shown when cleared
- `clearance([], )` -> shown when not cleared, hidden when cleared
- `clearance()` -> hidden in both states

Note:

- `clearance(...)` syntax is reserved for clearance behavior and is not interpreted as RGB token color syntax.

## Color Editing UI (Profile Color Picker)

Features:

- color wheel (hue/saturation)
- vertical value slider
- vertical opacity slider with checker background
- live color preview box
- editable `RGB r,g,b` text field
- editable opacity (`A`) text field
- editable `HEX` field:
  - `#RRGGBB`
  - `#RRGGBBAA`
- `Change Color...` button inside picker to jump directly to another profile color path without closing

All edits are saved to `vSMR_Profiles.json`.

## Status-Based Color System

### Target icon colors

Per-profile path:

- `targets.ground_icons`

Important keys:

- departure/ground statuses: `push`, `stup`, `taxi`, `nsts`, `depa`, `departure_gate`
- arrival/ground statuses: `arr`, `arrival_taxi`, `arrival_gate`
- no flight plan: `nofpl`
- airborne: `airborne_departure`, `airborne_arrival`

### Label background colors

Per-profile paths:

- departure: `labels.departure`
- arrival: `labels.arrival`
- airborne: `labels.airborne`

Airborne dedicated keys:

- `departure_background_color`
- `arrival_background_color`
- `departure_background_color_on_runway`
- `arrival_background_color_on_runway`
- `departure_text_color`
- `arrival_text_color`

## Status-Specific Definitions in JSON

Definitions can be overridden by status using:

- `labels.<type>.status_definitions.<status>.definition`
- `labels.<type>.status_definitions.<status>.definitionDetailled`

Now supported for:

- departure statuses
- arrival statuses
- airborne statuses (`airdep`, `airarr`, `airdep_onrunway`, `airarr_onrunway`)

## SID Coloring (`csid`)

`csid` token supports profile-level colored SID text groups.

Profile JSON path:

- `sid_text_colors` (array, per profile)

Group schema:

```json
{
  "sids": ["ATREX", "NURMO"],
  "runways": ["26R", "26L", "08R", "08L"],
  "color": { "r": 255, "g": 180, "b": 40 }
}
```

Matching behavior:

- SID match is prefix-aware:
  - `ATREX` matches `ATREX6G`, `ATREX6A`, etc.
- runway matching accepts normalized values (e.g. with/without `RWY` prefix).
- runway wildcards are supported with values like `*`, `ALL`, `ANY`.

## Icon Style and Size Behavior

Display -> Icon Style contains:

- `Icons`
  - `Arrow`
  - `Diamond`
  - `Realistic`
- `Size`
  - `Fixed Pixel` (toggle)
  - `Fixed Size` (`0.10x` to `2.00x`, includes `0.10x` and `0.25x`)
  - `Boost`
    - `Increment`
    - `Small Icon Boost` (toggle)
    - `Resolution`

### Arrow vs Diamond vs Realistic

- You can switch styles at runtime.
- Styling still follows active profile colors.

### Boost small icons

- Optional zoom-based size boost for small icons.
- Works with realistic, triangle, and diamond icon styles.
- Boost increment is configurable (`0.75x` to `3.00x` options in menu).
- Resolution preset affects activation behavior:
  - `1080p`
  - `2K`
  - `4K`

### Fixed pixel icon size

- Optional mode to keep icon size in pixels instead of world meters.
- `Fixed Size` controls symbol size for arrow and diamond styles in both modes (fixed-pixel ON or OFF).

## Font System

### UI font controls

- `Target -> Tag Font`
- `Target -> Label Font Size`

### JSON-driven available fonts

Per profile:

- `font.font_name`
- `font.available_fonts` (editable list)
- `font.sizes`

Default included examples:

- `EuroScope`
- `Consolas`
- `Lucida Console`
- `Courier New`
- `Segoe UI`
- `Tahoma`
- `Arial`
- `ods`
- `Deesse Medium`

## Tag Collision/Deconfliction

Deconfliction logic was adjusted so collision handling uses the default-definition tag footprint rather than expanding due to detailed view. This makes tag dragging and placement more stable in dense traffic.

## Other Implemented Behavior Changes

- Removed intrusive debug runway status message spam.
- Removed Xpdr standby error popup behavior from user workflow.
- Profile editor UI was streamlined and compacted.
- Arrival/departure gate state handling separated where needed.
- No FPL state has dedicated color handling (not tied to departure gate color).

## JSON Editing Best Practices

- Keep valid JSON syntax (quotes, commas, braces).
- Use UTF-8 text.
- Backup before major edits.
- Restart/reload plugin after manual edits.

## Quick Example: Colored and Styled Definition

Example line:

```text
b:callsign(255,255,255) csid(255,180,40) clearance([], [x])
```

Meaning:

- bold white callsign
- SID text in amber
- clearance checkbox with custom symbols
