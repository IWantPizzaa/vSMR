# vSMR Control Center

This document describes the native Control Center implementation in this
repository. It is intentionally narrower than the user-facing
[README](../README.md): use the README for installation, deployment layout,
build commands, offline restore instructions, and general vSMR configuration.
Use this document for ownership, bridge, lifecycle, persistence, and current
capability boundaries.

## Architecture

| Layer | Main files | Responsibility |
| --- | --- | --- |
| Native runtime UI | `vSMR/src/SMRRadar_RuntimeMenu.cpp` | Draws the editable current-airport row, focusless radar-screen icon rail, and compact popups with EuroScope screen objects. |
| Radar operational UI | `vSMR/src/SMRRadar.cpp`, `vSMR/src/SMRRadar_ScreenInteraction.cpp` | Draws the optional FPS-only overlay and handles current radar-screen interactions without a top menu. |
| Inset window UI | `vSMR/src/InsetWindow.cpp` | Draws AVISO and SRW chrome and interaction: title/drag, live snap previews, edge/corner resize, close, pan, and zoom. |
| Runtime/render state | `vSMR/src/SMRRadar.cpp`, `vSMR/src/InsetWindow.cpp` | Applies profiles, modes, inset state, RIMCAS state, and AVISO renderer state. |
| Modeless host | `vSMR/src/SMRRadar_ControlCenter.cpp`, `vSMR/src/VsmrControlCenterDialog.cpp` | Owns the modeless MFC window, WebView2 lifetime, local-resource mapping, placement, file selection, and asynchronous GitHub loading. |
| Typed dispatch | `vSMR/include/VsmrControlCenterBridge.hpp`, `vSMR/src/VsmrControlCenterBridge.cpp` | Decodes one versioned envelope, validates payloads, dispatches to radar/config APIs, and emits replies. |
| Browser application | `vSMR_webUI/index.html`, `styles.css`, `app.js`, `data.js` | Reproduces the supplied interface, stages edits, maintains undo/redo history, and renders authoritative native state. |
| Persistence | `vSMR/src/Config.cpp`, `vSMR/src/AvisoDocumentModel.cpp`, `vSMR/src/SMRRadar_AircraftAndAsr.cpp` | Validates profile/AVISO documents and persists radar-screen preferences in the ASR. |

The normal data flow is:

```text
EuroScope radar callbacks
  ├─ native runtime rail ────────────────> live radar/config APIs
  └─ modeless Control Center
       └─ local WebView2 application
            ├─ versioned JSON envelope ──> central C++ bridge
            └─ authoritative replies <───┘
                                             ├─ profile/mode/RIMCAS APIs
                                             ├─ inset/preset APIs
                                             ├─ AVISO renderer state
                                             └─ validated persistence
```

The browser state is not the persistence authority. The native host sends
profile state and AVISO state after the `ui.ready` handshake and after
operations that change native state. Local editor `Update` buttons commit a
draft into staged browser state; the blue global `Save` button writes that
staged state.

## Surface ownership

Runtime and editing controls have one owner each. This prevents the same action
from appearing in the AVISO inset, Runtime Menu, and Control Center at the same
time.

| Surface | Owned controls | Intentionally absent |
| --- | --- | --- |
| Runtime Menu | Current-airport editing, mode/profile selection, AVISO group visibility, AVISO/SRW visibility and reset, full airport-specific inset-preset management, and opening the Control Center | Persistent AVISO, profile, tag, icon, mode-definition, and alert editors |
| AVISO/SRW insets | Striped title/drag bar, `X` close control, edge/corner resizing, snapping, pan, and zoom; floating SRWs also expose the `F` altitude filter | Preset, reload, and editor buttons |
| Control Center | AVISO geometry/text editing and reload/import; profile, mode, alert, group, tag, icon, color, rule, and settings editing, including FPS visibility | Radar cursor tools and duplicated inset chrome |
| FPS overlay | Optional `FPS <value>` text placed in an unobstructed corner of the remaining main AVISO area | Component timings, controls, background toolbar, and hit regions |

The old grey top menu and its QDR, Target, Lighting, and distance actions are
removed completely. It leaves no background band or legacy hit regions. FPS
is an independent optional overlay and displays no A/C/R/T/S component values.

## Runtime rail

The runtime rail is a native GDI/GDI+ overlay, not the browser preview rail. It
uses EuroScope hit regions (`RUNTIME_MENU_RAIL` and
`RUNTIME_MENU_POPUP`). Immediately below its 10 px draggable grip, its first
content row is the centered current-airport ICAO text. Clicking that row opens
EuroScope's text editor; accepted input is trimmed, uppercased, applied live,
and saved under the ASR `Airport` key. Empty input is ignored.

Five 40 px icon buttons follow the airport row:

- Mode
- Groups
- Insets
- Profile
- Control Center

The grip is draggable without moving focus away from EuroScope.
`RuntimeMenuX` and `RuntimeMenuY` are saved in the ASR. Popups open beside the
rail, flip to its left near the right screen edge, and page long mode, group,
profile, or preset lists.

Mode and profile choices call the existing live activation APIs. Inset choices
independently toggle or reset AVISO, SRW 1, and SRW 2. Presets capture and restore the
main AVISO view, AVISO secondary window, and both SRWs, including visibility,
placement, pan, scale/range, filter, compatible legacy rotation, snap layout,
and linked movement. Preset lists and defaults are scoped by profile and active
airport. Airport-qualified ASR state restores only that airport's three windows;
an unseen airport without a default starts reset and hidden. Rename
uses EuroScope's popup editor. The default action reads `Set default` when an
active non-default preset can be promoted. It reads `Clear default` when the
active preset is already the default, or when no preset is active but a
configured default remains; the other rail interactions remain focusless.

AVISO and SRW top, top-left, and top-right snap layouts use `GetRadarArea().top`
directly. Rendering and interaction share those bounds, so no obsolete toolbar
clearance remains in snapping, dragging, resizing, or preset restoration.
Floating layouts retain only the small clearance required to keep their own
title/drag surface reachable.

All three inset windows use the same striped title bar and a right-aligned `X`
close action. The floating SRW windows additionally retain `F` as their altitude
filter; `F` is not a detach control. Dragging any inset title bar near a radar
edge or corner shows the exact prospective half-screen or quadrant footprint
before release. Dragging a snapped title bar restores a floating window while
keeping the grabbed title point under the pointer. Each outer edge and corner
has a forgiving resize hit band with the matching Windows resize cursor; an
anchored split or quadrant keeps its dock while its inward divider is resized,
and dragging an outer anchored edge converts it to a freely resizable floating
window. Cursor selection and EuroScope dragging use the same canonical hit
rectangles, and cursor state is recalculated from the live pointer position so
it cannot remain stuck after leaving a released border.

An edge-snapped inset reserves its occupied strip from the main AVISO viewport.
The main renderer derives and rasterizes only the geographic bounds visible in
the complementary area while preserving EuroScope's screen projection, so
targets and geometry stay aligned without divider-induced projection jitter.
Inset caches likewise compare geographic pixel scale rather than raw window
dimensions, preventing temporary shrink or aspect distortion during live
resize. Corner snaps remain overlays and do not
shrink the main viewport. The snapped title bar is excluded from inset map
rendering, pan, and wheel input.

AVISO items may belong to more than one group. Visibility uses union semantics:
an item is rendered when at least one known assigned group is visible.
Ungrouped items and references to an unknown/legacy group remain visible.
The top-level `vsmr_groups` array contains ordered group metadata only;
membership remains on each feature (canonically in
`properties.vsmr_group_ids`). Group creation, ordering, and membership edits
are staged until global Save, while their effect on the renderer is previewed
immediately through immutable native snapshots.

For compatibility, group definitions accept `id` or `group_id`. Feature
membership precedence is `vsmr_group_ids`, `vsmr_groups`, `group_ids`, then
the scalar `group_id` or `vsmr_group_id`. IDs are compared exactly rather than
slug-normalized. A legacy membership without a top-level definition in the
native-loaded AVISO document produces a runtime group so it remains
controllable. An imported document is staged first; Save and reload it before
expecting synthesized runtime groups.

AVISO text accepts `zoomLevel` and legacy `zoom_level`. Level 0 or an absent
key is unlimited; levels 1 through 14 use maximum half-view ranges of 34, 28,
22, 18, 14, 12, 9.5, 8, 6, 5, 4, 3, 2.5, and 2 km respectively.

## Modeless WebView2 host

The Control Center is created lazily per radar screen and owned by
`CSMRRadar`. Closing hides the modeless window; plugin/radar shutdown destroys
it and releases WebView2. Its borderless 728 x 500 px shell is fixed-size,
owned by the EuroScope window, and clamped to the EuroScope client area while
it is restored or dragged and after the EuroScope window moves or resizes.

Host-local state is stored under `%LOCALAPPDATA%\vSMR`:

| Path | Purpose |
| --- | --- |
| `WebView2\` | WebView2 user-data directory |
| `control-center-window.json` | Atomically replaced window position |

The host searches for `vSMR_webUI` next to the DLL, below the vSMR data path,
and below the current working directory. It maps the chosen directory to
`https://app.vsmr/` and loads:

- `index.html`
- `styles.css`
- `data.js`
- `app.js`

`data.js` is a compact offline/browser-preview seed. In the native host it is
replaced by authoritative profile and AVISO state. The source tree keeps the
four runtime assets split; generated standalone prototypes are not part of the
runtime package or repository.

The native Reset action reads two complete packaged defaults:

- `defaults\vSMR_Profiles.json`
- `defaults\AVISO_LFPG.geojson`

It validates both files and stages both documents; it does not use the compact
`data.js` preview seed. The Release build therefore copies four application
assets plus these two defaults to `Release\vSMR_webUI\`. The AppVeyor package
copies that complete directory beside `vSMR.dll`.

After both default files pass the host's initial checks, Reset emits one
correlated `resource.loaded` reply for profiles and one for AVISO. The browser
marks the resulting combined staged state dirty; disk writes still require
global Save.

## Bridge protocol

### Envelope

The canonical protocol version is 1:

```json
{
  "version": 1,
  "id": "ui-l5p7-3",
  "type": "runtime.profile.change",
  "payload": {
    "profile": "Custom LFPG"
  }
}
```

`id` is generated by the sender and is echoed by correlated replies. `type` is
the action discriminator. `payload` is action-specific. The native decoder
also accepts a small set of legacy action names, but new code should use the
canonical names below. Messages larger than 32 MiB, malformed JSON, unknown
versions, missing types, and invalid payloads are rejected.

All WebView messages enter through `VsmrControlCenterBridge`; individual
editor and renderer classes do not parse raw message strings.

### Browser-to-native actions

| Type | Native result |
| --- | --- |
| `ui.ready` | Sends the initial authoritative profiles/settings/runtime state and the split AVISO document. |
| `window.close` | Hides the modeless host. |
| `window.drag.start` | Starts a native non-client drag from the HTML title bar. |
| `state.save` | Validates the staged airport binding, then writes profiles and, when present, AVISO; reloads affected live systems. |
| `state.reload` | Reloads profile/map and pre-validated AVISO data from disk; a failure returns a correlated error and the failed document/overlay retains its prior live version. |
| `state.reset` | Requests the host to validate and stage the packaged profiles and LFPG AVISO defaults after the browser confirmation. Nothing is written until global Save. |
| `state.undo`, `state.redo` | Validates the supplied history snapshot, applies its profile/runtime state plus staged AVISO group metadata/membership in memory, and returns staged authoritative state. |
| `runtime.profile.change` | Activates the selected profile live, persists the active selection, then pushes authoritative state. A persistence failure returns a correlated error; the already-applied live selection is not rolled back. |
| `runtime.mode.change` | Activates the selected display mode, then pushes authoritative state. |
| `aviso.group.visibility` | Changes one runtime AVISO group visibility. |
| `aviso.groups.visibility` | Changes multiple runtime AVISO group visibilities. |
| `aviso.groups.update` | Updates native group metadata/order and, when the payload includes staged AVISO, refreshes renderer membership from that document. Persistence still occurs through global Save. |
| `aviso.inset.toggle`, `display.srw.toggle` | Toggles AVISO, SRW 1, or SRW 2 live. |
| `aviso.inset.preset.load` | Loads an existing native inset preset. |
| `aviso.inset.preset.capture` | Captures the current native layout as a preset. |
| `aviso.inset.preset.update` | Re-captures the active preset. |
| `aviso.inset.preset.rename` | Renames the selected preset. |
| `aviso.inset.preset.duplicate` | Duplicates the selected preset. |
| `aviso.inset.preset.default` | Sets the active profile-and-airport default preset; the native Runtime Menu uses the same action to clear an existing default when appropriate. |
| `aviso.inset.preset.reset` | Reloads the active/default preset. |
| `aviso.inset.preset.delete` | Deletes a preset. |
| `aviso.inset.preset.linked` | Changes linked movement. |
| `alerts.update` | Applies active-profile RIMCAS fields and runway state live; changes remain dirty until global Save. |
| `settings.update` | Applies resolution and RIMCAS live in staged profile state, and applies FPS visibility live while immediately persisting it under the ASR `ShowFps` key. |
| `resource.computer.load` | Opens the native JSON/GeoJSON file picker. Compatibility aliases are accepted. |
| `resource.github.load` | Downloads a GitHub file asynchronously after URL validation. |

Most profile, display, rule, tag, AVISO, mode, and group editor changes do not
need a separate native action: their `Update` button records a staged snapshot,
and `state.save` sends the complete profiles/AVISO documents.

Inset preset create/update/rename/duplicate/set-or-clear-default/delete
operations use the existing native preset API, which persists the active
profile-and-airport store immediately. They are runtime-management operations
rather than ordinary staged editor fields.
Profile and display-mode activation are also immediate runtime operations and
persist their active selection through the existing profile/ASR paths.

### Native-to-browser messages

| Type | Payload and UI behavior |
| --- | --- |
| `state.authoritative` | `profiles`, `settings`, `runtime`, `activeProfile`, `airport`, and `reason`, with inline `aviso` for staged undo/redo and group-update replies. Replaces clean state; runtime-only pushes preserve dirty editor documents. |
| `state.aviso` | The authoritative GeoJSON FeatureCollection. Sent separately to avoid duplicating the large document in every state envelope. |
| `state.saved` | Correlated Save success. The UI clears Save pending state and marks the current snapshot clean. |
| `state.ack` | Correlated success acknowledgement with `action` and optional `message`. |
| `state.error` | Correlated failure with a human-readable `message`; pending Save, Reload, or resource state is cleared by the UI. |
| `resource.loaded` | `resource`, `source`, and parsed `data`; the UI stages the imported profiles or AVISO document. |

Typical Save sequence:

```text
state.save
  -> state.saved
  -> state.authoritative (reason = "save")
  -> state.aviso
```

Typical validation failure:

```text
request with id = "ui-..."
  -> state.error with the same id
```

## State and history semantics

- Browser history is bounded to 12 committed editor snapshots.
- Undo/redo preserve the current page and selection state.
- Local form input is not a history entry until the relevant `Update` or other
  editor operation records it.
- Save is disabled when the current snapshot matches the last saved snapshot.
- Reload asks before discarding dirty state.
- Native runtime pushes update runtime fields while retaining staged profile
  and AVISO edits.
- Staged AVISO state remains bound to the airport from which it was loaded. If
  the Runtime Menu changes airports while editors are dirty, Save, Undo, and
  Redo stay disabled until Reload confirms discarding the stale staged state.
- Resource loading only stages parsed content. It does not change the configured
  destination path or write a file until Save.

## Threading, lifecycle, and security

- The native dialog and bridge remain on EuroScope's callback/window thread,
  which may already belong to an MTA COM apartment. A child host window, the
  WebView environment/controller, and every WebView call/callback are owned by
  a dedicated STA thread with its own message pump.
- Web messages are copied and posted to the native dialog before bridge
  dispatch. JSON responses, resize/page requests, and parent-move notifications
  are posted back to the STA host, so live radar mutations stay on EuroScope's
  thread and WebView COM interfaces never cross apartments.
- GitHub download uses a worker `std::thread`; completion is marshalled to the
  modeless window with a private `WM_APP` message. Only one GitHub download is
  allowed at a time.
- Weak lifetime tokens prevent late WebView and download completions from
  dereferencing a destroyed dialog. Event handlers are removed, the controller
  is closed, the STA message pump is stopped and joined, and COM is balanced on
  the same STA thread during destruction.
- AVISO group visibility is published as immutable snapshots. Rendering reads
  a snapshot without retaining the mutation mutex. Staged membership changes
  similarly publish replacement feature/label snapshots after validating
  membership values and unique staged feature IDs. Exact IDs are used when
  present and index matching is limited to ID-less features; unmatched loaded
  features retain their prior membership until global Save.
- Browser navigation is limited to `about:blank` and `https://app.vsmr/`.
  New windows and permission requests are blocked.
- Context menus, DevTools, browser accelerator keys, status bar, zoom controls,
  host objects, and external drop are disabled.
- Virtual-host resource access uses `DENY_CORS`.
- GitHub loading accepts only `github.com`, `www.github.com`, and
  `raw.githubusercontent.com` file URLs. Normal operation performs no network
  request.

## Persistence and recovery

### Profiles

`CConfig::saveConfig()`:

1. Serializes and parses the in-memory profile array.
2. Creates a unique same-directory temporary file.
3. Writes with write-through, flushes the file handle, reads it back, compares
   it byte-for-byte, and parses it again.
4. Copies the previous destination through a temporary file to
   `vSMR_Profiles.json.bak`.
5. Replaces the destination with `MoveFileEx(...,
   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

Malformed reloads are parsed into staging documents first. The current live
profile or map document remains active when a replacement is missing or
invalid. AVISO is likewise validated through `AvisoDocumentModel` before its
renderer snapshots are cleared. `state.reload` reports any of these failures
to the Control Center instead of sending a success acknowledgement.

The browser carries non-profile top-level array entries through initial load,
Computer/GitHub import, authoritative updates, history, and serialization.
The bridge persists incoming unknown entries in their submitted order and
retains the original document's entries as a defensive compatibility fallback
when the incoming array contains none. `_vsmr` metadata is emitted last.
Profile objects originate as clones of the source document, so fields not
represented by a visible editor remain present. Top-level placement may change
because the current browser emits profiles first, followed by retained
non-profile entries and metadata.

### Radar-screen preferences

The current airport, Runtime Menu position, inset state/geometry, active
profile, and optional FPS visibility are radar-screen preferences stored in the
ASR. `Settings > Display > Show FPS` updates the live `ShowFps` value and writes
the `ShowFps` ASR key as `1` or `0`; a missing key defaults to enabled for
compatibility. The overlay renders only `FPS <value>` at the top-right of the
unobstructed main area, moving below a corner inset when necessary.
Detailed component timings may still be collected for logs, but are never
included in the radar overlay.

### AVISO

`AvisoDocumentModel::SaveAtomically()` validates the FeatureCollection,
features, persisted IDs, geometry, and style references; recalculates supported
metadata; writes a unique sibling temporary file with write-through; flushes,
reads back, byte-compares, and parses it; atomically replaces an existing
`.bak`; and replaces the destination with write-through `MoveFileEx`.

For matching existing features, the bridge merges editor properties while
retaining native geometry. The document model also patches unchanged
coordinates from their original serialized text. Unsupported feature
properties, text anchors, style IDs, feature IDs, and unknown top-level fields
remain in the staged document.

Profile and AVISO writes are atomic per file, but are not one filesystem
transaction. Save writes AVISO first and profiles second. An AVISO failure
leaves the profile destination untouched. If the profile write then fails, the
bridge restores the in-memory profile document and attempts to restore the
pre-existing AVISO destination from its `.bak`. The defensive
no-prior-destination branch removes the newly written AVISO; it is relevant
only to a file-system race because a normal Save first requires the current
AVISO file to load successfully. A rollback failure is reported explicitly;
two independent files cannot be made fully transactional by the replace
operations alone.

## Build and runtime dependencies

The authoritative commands and offline instructions are in
[Building From Source](../README.md#building-from-source).

| Dependency | Current integration |
| --- | --- |
| Visual Studio 2022, MFC, Win32, C++17 | `Release | Win32`, normally toolset `v143` |
| `Microsoft.Web.WebView2` | NuGet `PackageReference`, version `1.0.4078.44` |
| WebView2 loader | Statically linked; do not deploy `WebView2Loader.dll` |
| WebView2 runtime | x86 Evergreen Runtime required on the EuroScope machine |
| UI assets | Four application files and two full reset defaults copied beside the DLL under `vSMR_webUI\` |

The host shows an in-window diagnostic instead of crashing EuroScope when the
runtime or packaged resources are unavailable.

## Native features added or changed

- Added the native draggable runtime rail with its current-airport row, five
  icon buttons, and compact edge-aware popups.
- Added runtime profile/mode switching, group visibility, three independent
  inset toggles, and full native inset-preset management, including a
  Set/Clear-default toggle.
- Reduced AVISO inset chrome to viewport-only controls and moved visibility and
  presets to the Runtime Menu, with editing/reload owned by the Control Center.
- Removed the old grey top menu and its QDR, Target, Lighting, and distance
  actions; moved current-airport editing to the first Runtime Menu content row.
- Added the persisted optional FPS-only overlay and reclaimed the full radar
  top edge for AVISO top and corner snapping.
- Added modeless WebView2 hosting, local virtual-host mapping, placement
  persistence, and runtime/resource fallback diagnostics.
- Added the centralized versioned bridge and authoritative state/error flows.
- Added asynchronous, allow-listed GitHub JSON/GeoJSON loading.
- Added validated packaged profile/LFPG AVISO restoration through the collapsed
  Danger zone.
- Added Control Center routing from existing profile/AVISO commands and menus.
- Added AVISO group membership/visibility to the main and inset renderer paths,
  with cache invalidation and snapshot-based render-thread handoff.
- Added AVISO text `zoomLevel` filtering to the main and inset renderer paths.
- Added native `statuses[]` support for structured rules while retaining the
  legacy singular `status` field.
- Added visible mode editors for pilot-squawk acceptance, tower filtering,
  structured rules, octal blocked-squawk chips, and status All/None helpers.
- Added profile restoration of configured RIMCAS runway rows and Normal/LVP
  visibility on profile/session load.
- Hardened profile/map/AVISO reload so invalid replacements do not erase live
  state and return a correlated Control Center error.
- Hardened profile Save with validation, read-back, backup, and atomic replace.
- Hardened AVISO Save with validation, coordinate preservation, read-back,
  backup, atomic replace, and best-effort cross-file rollback.
- Connected global Save to profile and AVISO validation/reload.
- Added native PackageReference restore, static WebView2 loader integration,
  asset-copy targets, and packaged CI output.

## Intentionally disabled or not connected

The native host disables controls when no safe native behavior exists. Disabled
controls have a tooltip explaining the reason.

| Control/capability | Native-host state | Technical reason or supported path |
| --- | --- | --- |
| Auto-reload changed local files | Disabled | No filesystem watcher service is implemented. Use Reload. |
| Bridge selector | Disabled | The embedded host always uses native WebView2. |
| Update interval | Disabled | Native synchronization is event-driven, not polling-based. |
| Runtime sync toggle | Disabled | Runtime synchronization is always enabled. |
| VACDM feature toggle | Disabled | Configure the VACDM server through existing `_vsmr.vacdm.server_url` data. |
| CPDLC feature toggle | Disabled | CPDLC remains owned by the existing vSMR/EuroScope settings flow. |
| Inset-windows feature toggle | Disabled | AVISO/SRW visibility is controlled directly by the runtime rail. |
| Profile and AVISO source text fields | Read-only | They report the native paths/source; import uses Computer/GitHub and Save writes the configured native destinations. |
| Maps source/editor | Not present | Maps were explicitly removed from this Control Center design; bridge capability reports `maps: false`. |
| Deletion confirmation | Browser-session setting | It controls web confirmation prompts but is not persisted by the native configuration model. |

Run the deterministic checklist in
[CONTROL_CENTER_TEST_CHECKLIST.md](CONTROL_CENTER_TEST_CHECKLIST.md) before
removing any item from this table.
