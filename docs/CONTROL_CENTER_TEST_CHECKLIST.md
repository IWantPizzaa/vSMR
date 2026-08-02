# Control Center deterministic test checklist

Use this checklist in a disposable EuroScope setup. Do not run persistence or
malformed-file tests against a controller's only production configuration.

Record the build identifier, Windows version, EuroScope version, WebView2
Runtime version, airport, profile path, AVISO path, and tester at the top of
the test record.

## Test fixture

1. Build and deploy `Release | Win32`, including all four application assets
   and both files under `vSMR_webUI\defaults`, using the layout in the
   [README](../README.md#installation).
2. Put working copies of `vSMR_Profiles.json` and the active airport's AVISO
   GeoJSON in the disposable deployment's normal `vSMR_Data` paths, and keep
   pristine copies in a separate temporary directory.
3. Add one sentinel unknown object to the profiles array, one unknown member to
   a profile, one unknown top-level AVISO member, one unknown feature property,
   and recognizable high-precision coordinates.
4. Keep hashes and byte copies of both input files.
5. Open one vSMR radar display at LFPG with traffic or replay targets suitable
   for tag, filter, and RIMCAS observations.
6. Save the ASR once so runtime-rail, inset, airport, and FPS persistence can
   be checked.

For each row, mark Pass, Fail, Blocked, or N/A and attach a screenshot/log/file
diff where the expected result is not immediately visible.

## Host and visual shell

| ID | Action | Expected result |
| --- | --- | --- |
| H-01 | Start with `vSMR_webUI` absent, then open Control Center. | A readable native fallback identifies the missing resources; EuroScope remains responsive. |
| H-02 | Restore the four application assets and two reset defaults, then open Control Center. | The modeless window opens at exactly 728 x 500 inside EuroScope without stealing normal radar operation, and its title reads `vSMR`. |
| H-03 | Drag by the HTML title bar against every EuroScope client edge, move/resize EuroScope, close, and reopen. Attempt to resize the Control Center or use Windows Snap. | Drag is native, the window is re-clamped inside EuroScope, its fixed size cannot change, and its position restores inside the EuroScope client area. |
| H-04 | Exercise Display, AVISO Geometry/Text, Alerts tabs, Groups, Modes, Profiles, Settings, dialogs, long names, empty lists, mixed selections, hover, selected, disabled, and scroll states. | No clipped text, doubled borders, broken SVGs, raw browser controls, unexplained rectangles, or unnecessary horizontal scrollbars. |
| H-05 | Right-click, press browser DevTools/zoom shortcuts, drag a file onto the window, and try a link/new window. | Context menu, DevTools/accelerators, zoom, external drop, permissions, and off-origin navigation stay blocked. |
| H-06 | Remove/disable the x86 WebView2 Runtime and open Control Center. | The host displays the Runtime requirement and does not crash EuroScope. |
| H-07 | Close during WebView startup; then unload/reload the plugin while Control Center is open and while a GitHub request is active. Reopen Control Center after reload. | Windows, handlers, workers, controller, registered host class, and COM resources close without a hang, use-after-free, stale window procedure, or crash. |
| H-08 | At 728 x 500, capture each page/tab/dialog/state beside the supplied prototype. | Dimensions, spacing, typography, colors, borders, icons, hierarchy, hover/selected/disabled states, and scrollbars have no unexplained visual divergence. |
| H-09 | In the EuroScope setup that initializes its callback thread as MTA, open through the runtime icon and through `.smr editor`. | The full Control Center loads through the dedicated STA host; no COM-apartment fallback appears, and both routes remain interactive. |
| H-10 | Open Control Center from two radar screens, move and use both, then close them in each order. | Each fixed-size EuroScope-owned window keeps its own bridge/page state; the shared STA host class remains valid until the final window closes. |

## Runtime rail

| ID | Action | Expected result |
| --- | --- | --- |
| RT-01 | Inspect the normal radar display before opening Control Center. | The Runtime Menu has its grip, a centered current-airport text row as its first content entry, then exactly five icon buttons with Mode, Groups, Insets, Profile, and Control Center tooltips. |
| RT-02 | Drag the grip to every radar edge, save, and reopen the ASR. | The complete rail, including its airport row, stays inside the radar area and restores from `RuntimeMenuX`/`RuntimeMenuY`. |
| RT-03 | Place the rail near the right edge and open every popup. | Popups flip left and remain on-screen. Long lists page without clipping. |
| RT-04 | Open Mode and select a different row. | Row contains only indicator/name; renderer immediately uses that mode. |
| RT-05 | Open Groups and toggle a group containing an exclusively grouped label, line, and area. | Row contains only eye/name; those items disappear/reappear in main and AVISO inset rendering without a file reload. |
| RT-06 | Toggle AVISO Inset, SRW 1, SRW 2, and Weather independently. | Each actual native window changes immediately and the other three retain their state. |
| RT-07 | Select an inset preset for the current airport, then switch repeatedly between profiles from the rail. | Row contains only indicator/name; colors, tags, icons, modes, rules, and alerts follow the selected profile, while the airport's preset list, default, active preset, linked state, and live inset layout remain unchanged. |
| RT-08 | Click Control Center and then close its title button. | Modeless Control Center opens and hides; the runtime rail remains usable. |
| RT-09 | Click outside an open popup. | Popup closes and no stale hit regions remain. |
| RT-10 | Compare the rail and each native popup with the supplied prototype. | The grip stripes stay clipped inside the rail, neither rail nor popup has an offset shadow, icon family and compact styling match, and no permanent button text, emoji substitute, description, internal ID, count, or redundant “Active”/On/Off label appears. |
| RT-11 | Open Insets and inspect every row/action before and after selecting the current default preset. | Visibility and a compact per-window Reset action for AVISO/SRW 1/SRW 2/Weather plus all preset operations are present only here; the action reads `Set default` for a non-default preset and changes to `Clear default` for the current default. |
| RT-12 | Change the position, applicable pan/zoom, and snap mode of each inset, then use its compact Reset action. | Only that inset returns to its floating default view; its visibility and the other three inset states do not change. |

## Runtime airport and FPS overlay

| ID | Action | Expected result |
| --- | --- | --- |
| RC-01 | Inspect the full radar top edge at narrow and wide widths, then click where the old menu entries used to be. | There is no grey band, top menu, QDR, Target, Lighting, `/`, legacy text, or stale menu hit region. Normal radar interaction continues in the reclaimed area. |
| RC-02 | Inspect the Runtime Menu before using any icon. | The current-airport ICAO row is the first content entry immediately below the grip and above Mode; the airport is not duplicated elsewhere. |
| RC-03 | Click the airport row, submit a different configured ICAO with surrounding spaces and lowercase letters, then submit an empty value. Save/reopen the ASR and finally restore the fixture airport. | The non-empty value is trimmed, uppercased, applied live, synchronized with airport-dependent rendering, and restored from the `Airport` ASR key. Empty input changes nothing. |
| RC-04 | Enable Show FPS and watch the top-right corner for at least two sample intervals. | A compact updating `FPS <integer>` readout appears without a background toolbar. It contains no A, C, R, T, S, component timings, or other menu text. |
| RC-05 | Disable Show FPS, Apply, save/reopen the ASR, then enable it and repeat. | The readout disappears and reappears immediately; each state restores from the `ShowFps` ASR key and agrees with the Control Center checkbox. |
| RC-06 | Enable Show FPS while AVISO is snapped top-right and then to each full edge; move the Runtime Menu near and then directly over the counter. | FPS stays at the top-right of the unobstructed main area or immediately below a top-right corner inset, never covers an inset `X`, and does not restore a reserved toolbar lane. The interactive Runtime Menu paints above the decorative counter when deliberately overlapped. |
| RC-07 | Stage an unsaved AVISO change in Control Center, then change the airport from the Runtime Menu. | Control Center reports that its airport context changed and disables Save, Undo, and Redo until Reload is confirmed. Neither airport's AVISO file is modified by the switch. |

## AVISO inset viewport controls

| ID | Action | Expected result |
| --- | --- | --- |
| AI-01 | Inspect AVISO, both SRWs, and Weather in floating, edge-snapped, and corner-snapped layouts. | Every inset uses the striped title bar with a coherent right-aligned `X`. AVISO and Weather have no `F`, `P`, `R`, Editor, Reload, or Presets action; a floating SRW retains only its altitude-filter `F` beside `X`. The title bar is not treated as content. |
| AI-02 | Drag each title toward every radar edge and corner, pause before release, release, then drag the snapped title again. | A clear preview appears before release and exactly matches the resulting half-screen or quadrant frame. Every edge/corner target works, and drag-to-unsnap keeps the grabbed title point under the cursor without a jump. |
| AI-03 | Hover and drag every floating edge and corner; resize each inward snapped divider; click/release each border without moving, then move onto empty radar space. | Horizontal, vertical, and both diagonal stock resize cursors appear only inside the exact forgiving drag bands. Every point showing a resize cursor starts that resize, anchored dividers stay docked, a click without a drag never nudges or unsnaps the inset, and the resize cursor clears after release when the pointer leaves the frame. |
| AI-04 | Edit AVISO geometry/text in Control Center, Save, then use Control Center Reload. | Main and inset rendering update from the Control Center workflow; the inset exposes neither Editor nor Reload AVISO. |
| AI-05 | Save all four edge and four corner layouts in presets and the ASR; reopen and load each one. | Every layout re-anchors to the radar bounds, and preview, rendering, click, resize, and restored frame bounds remain identical. |
| AI-06 | For each SRW, right-drag to pan, wheel over several cursor positions to zoom, drag the title into each corner, drag back to float, and inspect the toolbar. | Pan and cursor-anchored zoom match AVISO behavior, corner/split snapping works, and the floating SRW toolbar has altitude-filter `F` and close `X` only—no `Z` or `R`. |
| AI-07 | Snap an inset to each full edge while watching the main AVISO, then continuously resize its divider through short and long drags. Test two opposing edge insets. | The main AVISO immediately uses only the complementary area, remains geographically aligned with EuroScope targets, and follows divider movement without twitching or rendering below a dock. The inset cache keeps constant geographic scale and aspect ratio throughout the live resize with no shrink/snap-back transition. If no area remains, the main AVISO is suppressed. |
| AI-08 | Overlap two insets, operate the front window's title, border, content, and `X`, then switch EuroScope views or focus while dragging. | Input never falls through to the covered inset. Close hides only the front inset, and capture, cursors, and pan state are clean after interruption. |
| AI-09 | Open Weather with live fixed, variable, calm, gusting, stale, and unavailable METARs; include an airport not already subscribed in EuroScope, then hover its content and use the wheel. | EuroScope data or the background VATSIM fallback updates wind/QNH and true-bearing runway components for the active airport; `VRB`/`CALM`/stale/waiting states are explicit, no raw METAR is shown, and wheel input does not zoom Weather or the covered main radar. |

## Inset presets

| ID | Action | Expected result |
| --- | --- | --- |
| IP-01 | Arrange main AVISO view, AVISO inset, both SRWs, Weather, filters, scales, visibility, pan, and snap layout; save a uniquely named preset from the Runtime Menu. | Preset is created and becomes active. |
| IP-02 | Change all those values, then load the preset from the Runtime Menu. | Main/secondary AVISO, both SRWs, and Weather restore their captured state. |
| IP-03 | Use Runtime Menu Update on the active preset, alter the layout, then use Reset. | Reset returns to the updated snapshot. |
| IP-04 | Rename, duplicate, set the duplicate as default, confirm the action changes to `Clear default`, clear it, toggle linked movement, and delete the duplicate. | Every operation updates the Runtime Menu and global airport preset data; default state clears deterministically, and invalid/duplicate names report failure without corruption. |
| IP-05 | Set a default again, save/reopen the ASR and configuration. | The airport default preset and linked movement apply as configured regardless of the active profile. |
| IP-06 | With preset A configured as default, load non-default preset B, delete B, then inspect and click the default action. | The Runtime Menu no longer treats deleted B as active; `Clear default` remains enabled and removes default A. |
| IP-07 | Create distinct LFPG and LFPO presets (including both SRWs and Weather), switch airports in both directions, and reopen the ASR. | Each airport lists and restores only its own presets/default/four-window state; Weather follows the selected ICAO, an unused airport is reset and hidden, and no LFPG name or geometry appears under LFPO. |
| IP-08 | Clear an airport default, make and save an unrelated Control Center edit, then reload. | The default remains cleared; the first preset may be selected for use but is not silently persisted as the default. |
| IP-09 | Leave an unrelated Control Center profile edit unsaved, capture/update/delete an airport preset natively, switch profile, use Undo/Redo on the editor change, then Save. | Editor history changes only the staged profile edit; the authoritative airport preset store, active preset, and live SRW geometry are not rolled back, hidden, or overwritten. |
| IP-10 | Open two radar screens on different airports and profiles, change presets from each, then save a non-preset profile setting from the older screen. | Both airports' latest shared preset stores remain present; a stale screen or profile document cannot erase or resurrect another airport's presets. |
| IP-11 | Arrange Weather, then load a preset created before Weather support. | The legacy preset restores only the fields it contains; the current Weather visibility and placement remain unchanged. |
| IP-11 | Start with a legacy configuration containing distinct per-profile preset stores for the same airport, including a duplicate name, then load and save it. | Migration creates one profile-independent airport store, retains every distinct layout with deterministic unique names, preserves a valid default, and removes the migrated profile ownership without repeated imports on later reloads. |
| IP-12 | Create and activate an airport preset, then create, duplicate, rename, and delete profiles while alternating the active profile. | Profile lifecycle operations never clone, rename, remove, or deactivate the airport preset; every surviving profile sees the same list and default. |

## Global commands and history

| ID | Action | Expected result |
| --- | --- | --- |
| GH-01 | Open clean state. | Save is disabled; Undo and Redo are disabled. |
| GH-02 | Change a field, press its local Update, then navigate elsewhere. | Save becomes blue/enabled; change remains staged. |
| GH-03 | Undo and Redo from another page. | Data changes in each direction while the current page and selection remain stable; availability buttons update. |
| GH-04 | Make at least 13 committed edits and undo repeatedly. | History remains stable and is bounded to the last 12 snapshots. |
| GH-05 | Click Reload while dirty, first cancel and then confirm. | Cancel retains staged state; confirm replaces it with disk-authoritative state. |
| GH-06 | Click Save and wait for native replies. | Save stays pending/disabled until success, then authoritative state reloads and Save becomes disabled. |
| GH-07 | Make an invalid staged document and Save. | Correlated error is visible, pending state clears, and the destination remains valid. |

## Display: colors and icons

| ID | Action | Expected result |
| --- | --- | --- |
| DI-01 | Search/select a color, change hue/R/G/B/opacity and Update. | Working profile changes, Revert restores the draft, and disk is unchanged until global Save. |
| DI-02 | Save the color, observe matching live radar element, then Reload. | Renderer and reopened editor use the saved RGBA value. |
| DI-03 | Exercise each icon style, primary-target visibility, fixed-pixel size, and small-icon boost. | Preview follows the selection; after Save the actual target renderer follows it. |
| DI-04 | Change Resolution in Settings, Apply, Save, and inspect small icons. | Resolution is stored in `targets.small_icon_boost_resolution_preset` and applied live. |

## Display: tags

| ID | Action | Expected result |
| --- | --- | --- |
| DT-01 | Select several tag definitions/statuses. | List rows contain readable names only; no duplicated codes/counts/color squares. |
| DT-02 | Edit normal/detailed lines, inheritance, and insert a token; Revert, then Update. | Revert discards the draft; Update stages exact token arrays; Save changes rendered tags. |
| DT-03 | Change rounded corners, automatic deconfliction, speed-based gate, departure/arrival coloring, leader length, label size, typeface, and a supported font weight. | Global options change matching renderer behavior after Save/reload. |
| DT-04 | Select each offered font weight. | The offered Regular, Bold, and Italic weights are visually distinct and persist after Save/reload. |

## Display: rules

| ID | Action | Expected result |
| --- | --- | --- |
| DR-01 | Create, duplicate, rename, and delete a rule. | List shows names only; actions are undoable and remain staged until Save. |
| DR-02 | Add multiple criteria and target/tag/text colors. | Saved rule evaluates all criteria and affects only enabled output channels. |
| DR-03 | Select All statuses, one status, and a subset of at least two statuses. | Dropdown mixed/all state is clear; native `statuses[]` matching applies the rule to exactly the chosen set. |
| DR-04 | Load a legacy rule with singular `status`. | The correct checkbox selection appears and a no-op Save preserves compatible behavior. |

## AVISO loading

| ID | Action | Expected result |
| --- | --- | --- |
| AL-01 | Open From computer, cancel the native picker, then reopen it and select valid GeoJSON. | Cancel changes nothing. The selected document is parsed/staged, source caption changes, and the configured destination is untouched before Save. |
| AL-02 | Load a normal `github.com/.../blob/...` URL and a `raw.githubusercontent.com` URL. | Download is asynchronous, each parses/stages successfully, and EuroScope drawing remains responsive. |
| AL-03 | Try a repository page, non-GitHub host, malformed JSON, and empty response. | Each is rejected with a visible correlated error; prior state remains active. |
| AL-04 | Start a GitHub load and inspect/operate the URL controls before it completes. | Input and Load stay disabled, the browser does not post a second request, and there is at most one active worker. A bridge-level test may inject a second canonical request to verify the correlated native rejection. |

## AVISO geometry

| ID | Action | Expected result |
| --- | --- | --- |
| AG-01 | Search, Ctrl-click additive select, Shift-select a range, and select all filtered. | Selection and anchor behavior are deterministic and survive rendering. |
| AG-02 | Select only areas and edit fill/stroke/visibility. | Only area-relevant controls show; Update touches explicitly modified values. |
| AG-03 | Select only lines and edit stroke/visibility. | Fill controls are absent; line renderer changes after Save/reload. |
| AG-04 | Select mixed lines and areas with different values. | Mixed states are explicit, incompatible fill controls hide, and untouched values remain byte/semantically unchanged. |
| AG-05 | Revert, Update, Save, and Reload. | Revert restores the current staged state; Update stages; Save validates/reloads the main and inset renderer. |

## AVISO text

| ID | Action | Expected result |
| --- | --- | --- |
| AT-01 | Filter by one style, search, Ctrl-click, Shift-select, and select all filtered. | One style selector is shown and multi-selection is deterministic. |
| AT-02 | Select one label and edit text, visibility, font, size, text/halo colors, and halo width. | Update affects the selected label/scope and renders after Save/reload. |
| AT-03 | Select multiple labels with mixed values. | Mixed state is clear and text-content input is disabled. |
| AT-04 | Apply settings to selection, current text group, and all AVISO text in turn. | Only the chosen scope changes; one Update action is recorded each time. |
| AT-05 | Change Zoom visibility at several levels and vary the main and inset radar range. | Labels obey the configured `zoomLevel` range threshold in both renderer paths. |
| AT-06 | Save a document containing a text anchor and unknown text property. | Both remain present even though the UI does not expose them. |

## Groups

| ID | Action | Expected result |
| --- | --- | --- |
| GR-01 | Create, rename, duplicate, and delete groups. | Stable internal IDs remain hidden; visible names/actions update without collisions. |
| GR-02 | With group search clear, drag groups before and after one another, Save, and reopen. | Page order, runtime popup order, and saved `vsmr_groups` array order agree. Searching disables reordering. |
| GR-03 | Open Add/remove and switch Text, Lines, Areas; search, multi-select, Select shown, Clear shown, Apply content. | Mixed membership is stored on the selected features and member list uses compact type indicators. |
| GR-04 | Remove one member and Clear a group. | Only requested memberships are removed; unrelated properties and other group memberships remain. |
| GR-05 | Put one item in two groups; toggle those groups individually from the runtime popup. | Main and inset update immediately. The item remains visible while either known group is visible and hides only when both are hidden; no stale raster survives cache invalidation. |
| GR-06 | Save/reload/restart with existing non-slug group IDs. | IDs and memberships remain stable; no automatic case or punctuation rewrite occurs. |
| GR-07 | Import a fixture using each supported membership alias plus a membership whose group has no top-level definition, then Save and reload. | Canonical-array precedence is deterministic, and the unknown legacy membership appears as a controllable runtime group after the staged document becomes the native-loaded AVISO. |

## Alerts/RIMCAS

| ID | Action | Expected result |
| --- | --- | --- |
| AR-01 | Toggle global RIMCAS, label-only, red emergency symbol, each of the nine alert types, All, and None; Update. | Actual RIMCAS flags and inactive-alert set change live, then survive Save/reload. |
| AR-02 | Toggle ARR, DEP, Closed, Normal/LVP, All ARR, All DEP, and Open all. | `CRimcas` live runway maps and LVP mode match every row. |
| AR-03 | Add a valid runway pair, reject an invalid/duplicate pair, and remove a row. | Validation is visible; live table and native maps contain exactly the accepted rows. |
| AR-04 | Edit all five normal timers, all five LVP timers, and stage-two speed threshold. | Valid non-negative integers reach native countdown/threshold logic. |
| AR-05 | Edit six appearance colors. | Stage-one/stage-two aircraft and caution/warning label colors update in live rendering. |
| AR-06 | Save, close EuroScope, reopen the ASR/profile, and inspect runway/LVP state. | Profile runway and visibility choices restore; an explicit runway array remains authoritative over sector-derived legacy defaults. |

## Modes

| ID | Action | Expected result |
| --- | --- | --- |
| MO-01 | Create, duplicate, rename, Update, and delete a mode. | List and active reference remain consistent; at least one mode is retained. |
| MO-02 | Add/remove blocked squawk chips; reject a code containing 8 or 9; edit every requirement. | Only unique four-digit octal codes are accepted; assigned/pilot squawk, clearance, TSAT, TOBT, tower-filter, and structured-rule behavior follow the saved mode. |
| MO-03 | Select visible statuses individually and use All/None. | Target visibility matches the exact set and both helper controls update every status deterministically. |
| MO-04 | Activate the mode from page and runtime rail. | Change is immediate and both surfaces show the same selection. |
| MO-05 | Toggle `accept_pilot_squawk`, `tower_filter`, and `structured_rules`; Save and reopen. | The three visible editors round-trip their native fields and affect the selected mode without dropping unrelated mode data. |

## Profiles

| ID | Action | Expected result |
| --- | --- | --- |
| PR-01 | Create, duplicate, rename, Update, and delete profiles. | Names remain unique, at least one profile remains, and metadata active-name reference stays valid. |
| PR-02 | Edit altitude, speed, range, and night-alpha filters. | Main/inset radar filtering and night overlay use saved values. |
| PR-03 | Confirm Advanced starts collapsed, expand it, and inspect Schema. | Schema stays out of the primary form; its value is read-only and remains unchanged. |
| PR-04 | Activate from page, rail selector, and runtime rail. | Live renderer and every selector agree; last active profile persists. |
| PR-05 | Make the profile destination unwritable and activate a different profile from the Control Center. | The live selection may already be applied, but a correlated error is shown, no authoritative success is sent, and the on-disk active-profile value remains unchanged. |

## Settings

| ID | Action | Expected result |
| --- | --- | --- |
| SE-01 | Inspect native-host Settings at 728 x 500. | Data files and Display form the left stack; CPDLC connection and PDC reminders form the right stack. There is no Datalink rail page, readiness card, Features card, Advanced section, or Danger zone. |
| SE-02 | Use Computer and GitHub for Profiles and AVISO. | Valid data stages; source text reports the source; native destination paths do not silently change. |
| SE-03 | Inspect Data files, change Resolution, then click Apply display. | Profile, AVISO, and EuroScope alias paths are read-only in the native host; the resolution change applies and marks staged profile state dirty. |
| SE-04 | Toggle Show FPS and click Apply display in both directions. | The checkbox remains enabled in the native host, changes the FPS-only overlay live, and immediately persists `ShowFps` in the ASR. Undo/Redo keeps browser and native visibility synchronized. |

## CPDLC and PDC in Settings

| ID | Action | Expected result |
| --- | --- | --- |
| DL-01 | Open `.smr` with an SMR screen active. | The fixed Control Center opens directly on Settings; no legacy settings dialog or separate Datalink page appears. |
| DL-02 | Change callsign, optionally replace the masked Hoppie code, toggle sound, and set reminder delay/cooldown to boundary values 0 and 1440. Wait for auto-save, reopen, and switch profiles. | Native EuroScope settings persist without Apply/Revert and independently of profiles; the secret is never returned to JavaScript or written into profile/history payloads. |
| DL-03 | Enter delay/cooldown below 0 or above 1440 and leave the field. | The browser clamps the value to 0–1440 and auto-saves it while reminders are stopped; native bridge requests outside that range remain rejected. |
| DL-04 | Connect, click Connect repeatedly, disconnect while connecting, then reconnect. | Only one login worker runs; a cancelled stale worker cannot reconnect; status updates from native state. |
| DL-05 | Poll manually while a poll is active and leave Settings open through timer polling. | Polls do not overlap, the UI stays responsive, and the busy state clears after completion. |
| DL-06 | Remove the `.cdm` alias, restore it, and use `Check now` for an active airport. | The Data-files alias path updates live; the check is disabled with a clear missing-template hint until a valid alias is restored, then reports checked/queued/suppressed counts. |
| DL-07 | Open Confirm and Message from an aircraft datalink menu, then drag each title bar against every EuroScope edge. | Each opens as one fixed frameless striped Cofrance window with no Windows caption, taskbar entry, light edit bevel, or light scrollbar. It remains inside EuroScope. PDC flight-plan fields are read-only; CTOT, TSAT, frequency, and additional text are editable. Message mode keeps the received text and reply workflow. |
| DL-08 | Paste a Hoppie code with surrounding whitespace, leave the field so it auto-saves, and reconnect; also try an invalid code. | The saved field remains visibly masked, surrounding whitespace is discarded, a valid code connects, and an invalid/network failure reports the sanitized Hoppie or transport result instead of a callsign-collision guess. |
| DL-09 | Run PDC reminders, edit delay/cooldown while running, click Update, then Stop while an automatic reminder is queued. | Run changes to Stop; edits reveal Update without stopping the service; Update restarts automatic tracking with the new schedule; Stop cancels queued automatic reminders while preserving explicit `Check now` work. |

## Persistence, compatibility, and failure recovery

| ID | Action | Expected result |
| --- | --- | --- |
| PC-01 | Save profile-only changes. | Destination parses; old destination is in `.bak`; no temporary file remains; live renderer reloads. |
| PC-02 | Save AVISO-only changes. | Destination is a valid FeatureCollection; the previous file is in `.bak`; no partial/truncated temporary file remains; renderer reloads. |
| PC-03 | Load a profile fixture with non-profile top-level sentinels, make a profile edit, Undo/Redo it, then compare sentinels and unknown fields before/after no-op and targeted Saves. | Imported top-level entries, unknown profile members, AVISO top-level/feature properties, anchors, style IDs, feature IDs, and group IDs remain. |
| PC-04 | Compare original high-precision coordinates after non-geometry AVISO edits. | Serialized coordinate text is unchanged for matching untouched features. |
| PC-05 | Introduce duplicate profile names, duplicate feature IDs, invalid geometry, or a missing style reference. | Save fails visibly and does not replace the corresponding destination. |
| PC-06 | Replace profile JSON with malformed text and Reload. | A correlated Control Center error appears and the previously loaded profile remains active. |
| PC-07 | Replace map JSON with malformed entries and Reload. | A correlated Control Center error appears and the previously loaded map index remains active. |
| PC-08 | Replace AVISO JSON with malformed text and Reload. | A correlated Control Center error appears; prior main/inset AVISO snapshots remain rendered and no empty document is staged or saved. |
| PC-09 | Make the AVISO destination unwritable and Save changes to both documents. | `state.error` clears pending UI; profile destination is untouched and neither file is partially overwritten. |
| PC-10 | With an existing AVISO destination, allow its write but force the subsequent profile replacement to fail. | Error is visible; the in-memory profile is restored and AVISO is rolled back from `.bak`. Review both files because two-file Save is not a transactional filesystem operation. |

## Completion record

The Control Center is ready for release only when:

- every non-N/A row passes;
- each N/A row corresponds to the current limitations table in
  [CONTROL_CENTER.md](CONTROL_CENTER.md);
- `Release | Win32` builds from a clean restore;
- `Release\vSMR_webUI` contains the four application assets plus
  `defaults\vSMR_Profiles.json` and `defaults\AVISO_LFPG.geojson`;
- no malformed-file or unload test crashes EuroScope; and
- profile/AVISO diffs show no unexplained data loss.
