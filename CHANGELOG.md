# Changelog

## [2.0.1] - Unreleased

## [2.0.0] - 2026-10-18

- Removed the bundled-asset provenance register and its blocking release/feed checks. Retained third-party license notices, including VATSIM Radar's CC BY-NC 4.0 aircraft silhouettes and the maintainer-declared GPL v3 alarm. Clean-source, version, regression and configured signing checks remain enforced.

- Added the supplied Orly 2026 runway 06/24 works AVISO as the alternative `LFPO_Work.geojson`, retaining the standard `LFPO.geojson` unchanged. Both use operational ICAO LFPO; the works variant can be selected through the AVISO file picker and is included in release/update packaging.

- PDC logon now follows the connected DEL/GND/TWR position's airport when exactly one AVISO context is open (including CoFrance with its AVISO inset). A manual logon edit disables synchronization for the rest of the plugin session, including network reconnects. Password-only edits do not lock synchronization; active/connecting Hoppie sessions are never interrupted and no automatic connection is made.

- Fixed AVISO inset centers after ICAO changes and opening AVISO/CoFrance ASRs: uninitialized centers are no longer restored as valid; restored centers are checked once against the current airport geometry and viewport size. Linked views wait for the main view to reach the new airport, while valid saved pan/zoom and subsequent manual movement are preserved.

- Added Parked to Modes > Visible statuses, saved independently per mode and enabled by default for existing profiles. Only explicitly selected PARK/PARKED statuses use Parked; stationary departures with an empty status remain under No Status. Ground aircraft whose flight-plan data has not been received use No flight plan, even if EuroScope provides a valid placeholder. Other ground arrivals remain under Arrivals; airborne, on-runway and correlation filters remain in effect.

- Excluded the EGKK startup fallback from Recent Airports without preventing normal airport selection.

- RDF list detection, blinking TX and reset now work independently of Native RDF map visibility, including startup with `.smr rdf off`. Hiding the overlay no longer clears pending calls.

- Added an LFSB Real AVISO palette based on the supplied reference: dark blue-grey background, grey-violet runways/buildings, fine grass boundaries, green taxiway labels, muted yellow stands and orange/blue guidance. Runway/paved-surface outlines are disabled to avoid artificial polygon seams at the runway intersection and on the apron. Both runways retain the same neutral colour rather than the reference's red runway. All 235 existing geometries and Dark/Light styling are preserved.

- Restored LFBO's screenshot-reference Real palette: grass uses the uniform slate-grey background fill, with fine outlines to keep its 13 existing polygons identifiable instead of adding dark patches. Existing surface outlines, geometry, other Real colours and Dark/Light rendering are unchanged.

- Added an optional EuroScope list/tag item `RDF` (width 2) with a yellow/grey blinking `TX` for ground aircraft heard through native TrackAudio RDF. Calls remain pending after reception ends, including overlapping speakers, until the local `RDF reset` action acknowledges them. Reset never changes live RDF rings or flight-plan data; a later new transmission reactivates the item. Airborne/disconnected aircraft are cleaned up automatically. Requires TrackAudio running; list columns and mouse bindings remain user-configured.

- Fixed past-position markers keeping a fixed pixel size while aircraft icons changed with zoom. Dots, rings and ring strokes now follow the rendered icon dimensions (including minimum/maximum sizes, resolution scaling and missing-icon fallback) on main AVISO, CoFrance/AVISO insets and SRW. Trail sample counts, spacing and fading are unchanged; culling uses the scaled marker extent.

- Opening the HP tag editor now sets a ground aircraft to Taxi immediately, preserving scratchpad text and real speed assignments while clearing the shared lineup marker. Applies through the common tag action on main AVISO and insets; rejected EuroScope writes are reported.
- METAR detail now adapts automatically to available space and airport count; removed the manual Full/Compact/Mini selector and its saved override. All-open-airports or single-airport selection remains available in Settings and is saved with the ASR. Open AVISO/CoFrance airports are deduplicated and fetched together; responsive detail and title-bar paging keep every airport accessible without growing a snapped window.

- Added a single-line minimum-size METAR view showing only ICAO, wind (including VRB/gusts) and QNH. Weather windows now snap to edges/corners without enlarging; floating chrome, resize hitboxes and saved anchors keep the same content dimensions in AVISO and CoFrance views.

- Show `CONFIG - NOT LOADED` when vSID is online but reports an unloaded automatic configuration, and `CONFIG - MANUAL ONLY` for unmanaged airports, instead of leaving an unexplained configuration heading.

- Updated the vSID interface for generic `rules`, `areas` and `autoconfig` bridge snapshots. Configuration buttons follow live rules, send explicit airport-scoped assignments, support LFPB and LFOB's PGEAST rule, and keep LFPG taxi areas independent of Linked/Unlinked. Display Auto/manual status and offer Resume auto config for a manual override; retain compatibility with older companion builds and never change EuroScope's active airports/runways.

- Synchronized the four timer countdowns across AVISO and CoFrance v2 views. Starting/resetting a timer refreshes every visible timer window; countdowns survive closing or switching views, and expiration triggers one shared alarm instead of per-window alarms.

- Restored AVISO inset tag clicks through the same action dispatcher as the main AVISO. Resolve the painted, clipped tag cell for either mouse button instead of relying on the inset's parent hit rectangle; right-click panning now yields to tag actions, and releasing a background pan over a tag does not open a menu.

- Manual AVISO group visibility choices now remain in place while runway selections are unchanged. Automatic rules reapply when their source airport's ARR/DEP selection or the rule changes, including changes within the same runway direction; unrelated airports/profile saves do not cancel manual choices.

- Hardened automatic AVISO group visibility reads: use EuroScope's active sector for operational runway selections, normalize padded airport/runway identifiers, copy borrowed SDK strings immediately and combine duplicate runway observations. Restore the view's sector source after reading; log observed runways and group decisions only when they change.

- Made runway-driven AVISO group visibility configurable through `runway_group_visibility` in layered JSON configuration. Each group can monitor any airport's arrival/departure runways, match any/all runway ends, exclude opposing runways, invert visibility or disable automation. LFPG East/West behavior is now a bundled default, not hardcoded; user overrides survive updates.

- LFPG East/West arrow groups now follow EuroScope's live ARR/DEP runway selection at map load and on runway changes: 08/09 shows East, 26/27 shows West. No active direction or mixed directions hides both. Main AVISO and insets share the update without changing EuroScope runways or rewriting user configuration.

- Added a north-indicator toggle in Control Center > Settings > Display and drag-and-drop positioning in main AVISO and inset views. Positions are retained in the ASR; the main compass stays below insets and covered indicators do not intercept inset clicks.

- Added an automatic, compact north compass at the top right of rotated AVISO views (main view and insets, including CoFrance). It follows the actual map projection and hides when north is up.
- Fixed label font-size stepping by leaving native spinner values untouched during changes; removed the oversized font suggestion dropdown. Pixel sizing and existing profiles remain compatible.

- Removed the extra debounce delay on AVISO pan/zoom/resize rebuilds in the shared main/inset renderer. Pending requests still coalesce and obsolete work is cancelled; map quality, polygon-corner preservation and explicit/content-change delays are unchanged.

- Added opt-in zoom performance summaries to normal logging: per-window redraw peaks and frame gaps, wheel routing and message age, plus main/inset cache counters and background rebuild timings. Rendering and zoom behavior are unchanged.

### Control Center and source maintenance

- Added Select All to the profile-color, tag-definition, AVISO geometry and text lists; group contents also expose Select All for the currently filtered items.
- Added confirmed, section-scoped resets for selected colors, tag definitions, AVISO styles, icon/trail options, tag font/layout, color rules and alert options. Other sections, EuroScope runway assignments and manual runway closures are preserved; custom profiles fall back to the bundled Default profile.
- Harmonized inconsistent C++/resource, browser-test and bundled-source asset filenames. A shared distribution mapping preserves installed filenames and compatibility with existing loaders, custom sounds and configuration migration. Documented conventions in README.md.

Release metadata targets 2.0.0. GitHub release preparation remains draft-only
until the maintainer explicitly publishes it; third-party notices are shipped
in `vSMR/data/Licenses/ASSET_NOTICES.txt`.

### Added

- Added vSMR inset hosting on geo-referenced CoFrance views, with an expanded runtime rail, independent AVISO/SRW/METAR/timer windows and host-main-map rendering/preset isolation. Custom displays no longer depend on native TAG/list rendering phases; `.smr insets` restores the rail and reports host/refresh diagnostics.
- Added per-file update feeds with immutable Git content URLs, SHA-256 verification, staged installation and manifest-last recovery, plus a minimal external helper for locked loader replacements. Feed generation remains separate from publishing and preserves the legacy full-package bridge path.
- Added application-owned `default.json` and sparse user-owned `config.json`, with recursive object overrides, whole-array replacement and stable profile identities. Official AVISO customization is separated from replaceable geometry; unknown legacy edits are preserved conservatively.
- Added an LFLL Real AVISO palette based on supplied real-world screenshots, with a lighter slate-grey background, pale runways, fine outlines and cyan stand labels. Integrated supplied airport-detail geometry including 152 surface markings, 35 buildings, coloured guidance lines and nine independently grouped runway-distance labels. Dark/Light colours, existing stopbars/closures and runway settings are preserved; unrelated circuit annotations and ambiguous restriction classifications are excluded.
- Added an LFBO Real AVISO palette based on supplied real-world screenshots, with slate-grey surfaces, thin polygon outlines, lighter buildings and reference-coloured stand labels, plus a shared LFBO / LFLL profile with compact white targets and blue/mauve callsign/type tags. Existing geometry and Dark/Light palettes are preserved; polygon outlines are opt-in per palette.
- Added a Native RDF checkbox to Control Center Settings, synchronized with `.smr rdf on` / `.smr rdf off` and the saved EuroScope setting.
- Added synchronization of the Line Up ground status between vSMR clients using TAXI plus a reserved controller-assigned speed value. Real speed assignments are preserved; shared Line Up state is cleared when the aircraft becomes airborne or another ground status is selected. Changes to aircraft tracked by another controller are declined with an explanation.
- Added support for live LFPG taxi-area status from companion vSID bridge schema 1.4 (`lfpg_taxi`). Minimum Taxiing and Ground Crossing highlights use the published area state when available; older providers retain the last-completed-command fallback with an explanatory tooltip.

### Changed

- Capitalized the Diamond icon-style dropdown label while retaining the `diamond` configuration value. Diamond trail circles now use quarter-interval samples along the recent track, preserving the configured dot count and fade; other icon styles are unchanged.
- AVISO inset map labels adapt only their configured visibility distance to the ratio of the inset's drawable diagonal to the current host radar area's diagonal. Text and halos retain their normal size; automatic font shrinking was removed. The same visibility calculation applies on native vSMR and CoFrance, without fixed reference resolutions; changed visibility invalidates stale text rasters. Aircraft tags, controls and main-view label styling are unchanged.
- Restore the previous host cursor when leaving inset move/resize handles, including clicks back on CoFrance, without replacing a cursor already changed by the host.
- Repaired CoFrance resize-handle ownership outside inset frames, retained valid chrome drags outside the original bounds, and stabilized wheel/cursor routing with calibrated radar coordinates, active-view priority and single processing of dequeued mouse events.
- Made METAR QNH a dedicated, high-contrast pressure strip with larger digits and quieter units, including a pressure-first compact view. Refined the wind rose with a subtle shaded face, cardinal markers, scaled ticks and a tapered wind-from gradient; Day/Night, variable wind and variation arcs remain supported.
- Removed one-off updater-test packaging scripts and the superseded dated audit report; scratch directories and Python caches are now explicitly ignored. Maintained build, release, migration and regression tooling remains available.
- Simplified airport profile names to LFPG and LFMN, and replaced the separate LFBO/LFLL profiles with LFBO / LFLL using the LFBO configuration. Default and numbered Custom profiles are unchanged.
- Replaced the tag Label font size editor's 1-5 preset numbers with actual pixel sizes, common size suggestions and direct numeric entry. Existing profiles retain their selected preset and appearance; changes update its size for main-view, AVISO and SRW tags.
- Replaced the METAR compass arrow with a wind-strength-colored band fading inward from the reported wind-from bearing to the centre. Wind-variation arcs use the same wind-from orientation; calm and variable winds retain their existing non-directional display.
- Refreshed all 160 bundled AVISO GeoJSON maps from the converter's official GNG layouts and current settings, and updated the map hash inventory, provenance and validation fixtures.
- Restored LFPG's 89 East arrows and 97 West arrows in two independently selectable groups.
- Redesigned PDC and CPDLC message windows with aligned flight and clearance fields and side-by-side request/reply areas. Retained the Control Center's Tahoma typography, standard-height fields/buttons, compact striped title bar and square close button, with a charcoal/slate-grey palette and pale-blue Send button; CPDLC messages omit the clearance section.
- Removed the Auto mode button and status row from the vSID popup and reduced its height. This does not disable vSID's underlying automatic mode.
- Made vSID configuration buttons follow published live Paris rules, including changes made outside vSMR. Configuration selection remains manual in vSMR; automatic runway-based configuration detection is not included.
- Improved vSID command responsiveness with short-lived 50 ms completion checks and immediate configuration refreshes, without rescanning aircraft. Multi-command actions remain serialized and ambiguous deliveries are not retried.
- Expanded regression coverage for native command-window timers, repeated clicks, vSID live state and command completion, tag hover, inset-aware RDF, shared Line Up state, and bundled audio resources.

### Fixed

- Made layered-configuration tests stop on failed setup instead of dereferencing missing fixture data, and added file/Windows error details to updater test staging failures. Release-input validation now also accepts stable semantic versions.
- Fixed small AVISO rectangles and building corners turning into triangles when zoomed out with Rotate Screen enabled. Projection scale now includes both rotated axes, polygon corners are preserved, and line simplification uses final raster coordinates.
- Made the entire Timer inset follow the interface Day/Night setting, including idle, hover, running and expired cells, text and borders, without resetting countdowns.
- Fixed the PDC/CPDLC title bar intercepting close-button clicks; X now uses the Cancel path. Prevented decorative panels from painting over fields and filled multiline backgrounds completely. Fields retain Control Center input styling, with light-grey backgrounds for editable values and dark-grey backgrounds with muted text for read-only information.
- Separated LFPG's Linked/Unlinked controls from Minimum Taxiing/Ground Crossing. Linked/Unlinked uses the `opposing` rule; Minimum Taxiing resets areas before enabling NORTH and SOUTH, while Ground Crossing disables areas. Clicking an already-published link selection does not toggle it again.
- Preserved command-completion refresh notifications until EuroScope's regular callback acknowledges them, so the fast completion check cannot consume the notification needed to re-enable vSID buttons. Removed direct native-window repainting from command completion.
- Fixed aircraft tag hover requiring a click or drag by calibrating cursor coordinates against the actual radar view and requesting a refresh when hover begins.
- Made native RDF account for inset occlusion: an aircraft hidden beneath an inset produces a direction line toward it rather than a ring hidden under the inset.
- Added embedded timer-alarm and CPDLC-notification sound fallbacks when external WAV files are missing or cannot be played. Valid custom sound files remain preferred, and Windows mute and output settings remain respected.
- Deferred airport/runway refreshes until after EuroScope's runway-dialog callback returns, reading the committed selections while preserving each view's configured airport.
- Corrected holding-point catalogue entries for LFLL, LFML and LFPB.

## [2.0.0-beta.6] - 2026-09-18

### Added

- Added three-way updates for bundled AVISO maps and profiles using retained upstream defaults. Non-conflicting defaults update automatically, custom items and user edits are retained, and conflicts are recorded with incoming copies and complete rollback backups. Without a reliable baseline, edited files are preserved.
- Added optional unsigned GitHub updates with loader 1.2.0; existing loaders need a one-time manual upgrade. Repository-bound HTTPS downloads, SHA-256, package validation and rollback remain enforced. Explicitly signed/pinned installations retain signature enforcement.

- Restored LFPG's Minimum Taxiing and Ground Crossing buttons in the vSID CONFIG menu.

- Added explicit WL/EL/IPGW/IPOW vSID popup selections for LFPN, LFPV, LFPT and LFOB, with manual selection only. LFPG/LFPO retain Linked/Unlinked controls. Removed Auto runways and automatic runway-driven rule changes; the companion only publishes manually selected rules.

- Added a per-profile "Fit background to each text line" tag option for the main radar, AVISO insets, and SRW insets.

- Added a right-click airport history to the Runtime Menu ICAO field, listing the five most recently opened airports in the current radar-screen session for quick switching.

- Added regression coverage for JSON limits, updater URL and hashing checks, compiled tag definitions, and persistent text caches. CI now runs MSVC static analysis and a JSON fuzz smoke test under AddressSanitizer.
- Added an independent Night/Day interface theme in Settings for the Control Center, native Runtime Menu, and METAR display. It remains separate from the AVISO palette; Night retains the existing appearance, while Day uses a lighter slate-grey palette coordinated with the `#434A4F` AVISO background.
- Added validated Copy/Paste actions for Rules and AVISO geometry/text styles. AVISO paste and profile-color editing support the existing Ctrl/Shift multi-selection workflow.
- Added delayed, theme-aware interaction explanations for buttons and editable controls throughout the Control Center.
- Added an optional vSID 0.15.0.2 interface through EuroScope Plugin Bridge. A dedicated Runtime Menu panel replaces manual vSID command entry with validated buttons for airport modes, diagnostics, synchronization, and reloads. Live vSID SID, runway, and cleared-flight-level values are available as `vsid_sid`, `vsid_rwy`, and `vsid_cfl` tag tokens and as a dedicated Rules source.
- Replaced the retired vACDM HTTP integration with the bridge-enabled CDM plug-in. Its operational time fields are available to tags and the dedicated CDM Rules source as TOBT, TSAT, TTOT, CTOT, TSAC, ASRT, and ASAT.
- Added a `ready_startup` tag token that displays `RDY` in red until CDM publishes ASRT, then changes it to green.
- Made `ready_startup` invoke CDM's authoritative Ready Start-up toggle when clicked, and added a Ready aircraft requirement to display modes.
- Added the Ramp Agent interface through EuroScope Plugin Bridge. The `uk_stand` and `remark` tag tokens now show Ramp Agent's `rampagent/stand` and `rampagent/remark` values.

### Changed

- Replaced the bundled AVISO set with exactly 160 converter-supplied airport maps, byte-for-byte. Added LFRJ and removed 33 maps from the previous import. Dark/Light are available everywhere; Real is available at LFML, LFMN, LFPG, and LFPO. The final LFPG map has no East/West arrow groups.
- Applied the Settings resolution scale to AVISO geometry/text and aircraft icons/tags in the radar views, without resizing menus, Control Center, or inset controls.
- Aligned package defaults, binary product versions, and AppVeyor configuration for beta 6. Added release-input validation of versions, map hashes, and update-policy consistency.
- Reworked the EuroScope Plugin Bridge consumer to follow the bridge integration checklist: a single `esbridge.h` client shim attached from the timer, per-field resolution with type and schema checks, buffer resizing, stale-handle re-resolution, and one shared flight-plan scan per tick for vSID, Ramp Agent, and CDM. A missing plug-in or bridge now only disables the data it provides.
- `uk_stand` and `remark` no longer read flight strip annotations 3 and 4.
- Kept the declared CDM schema and manual Paris vSID controls when integrating the shared bridge client, with regression coverage for provider polling and Paris snapshots.

- Imported the installed Custom LFPG and Custom LFMN profiles, retaining bold callsign fields for LFPG. Slightly thickened bold tag text in the shared renderer and expanded its measured width to preserve spacing and hit areas.

- Placed vSID/CPDLC connection text before its colored indicator and removed close buttons from all Runtime Menu popups; clicking the same rail button again closes them.

- Split vSID and CPDLC into two separate Runtime Menu popups opened together by one button, each with its connection state in the title. Renamed Automatic mode to Auto mode, with explicit activated/deactivated text and matching green/red indicators when vSID reports its state.

- Harmonized the vSID / CPDLC panel with the other Runtime Menu popups: compact 220-pixel width, standard title, spacing and buttons, with connection indicators and grouped controls.

- Updated Default-profile tags with bold callsigns, Ready Startup on detailed no-status/startup tags, CTOT on detailed taxi/line-up tags, and the revised arrival layouts.

- Redesigned the combined vSID / CPDLC Runtime Menu with a compact layout and consistent state indicators. Automatic mode reads the optional `vsid/automode` bridge snapshot; older providers show Unknown. Included a companion vSID patch for publishing the actual airport states.
- Preserved native AVISO raster resolution at 2K and 4K by adapting off-screen cache margins to the existing memory budget, for both the main view and insets.

- Retained normal/detailed tag models between scene refreshes, updating text only when referenced inputs change. Removed indirect per-point target projection calls and replaced refresh-local RIMCAS maps with reusable records that preserve runway insertion order and countdown selection.
- Split profile normalization, tag formatting, Runtime Menu panels/actions, and AVISO/SRW rendering into focused helpers. Expanded regression coverage and added an isolated AddressSanitizer run of the native suite.
- Combined vSID and CPDLC/PDC controls into one Runtime Menu panel. Removed CDM Auto, its timer and message queue, bulk scans, timing controls, and saved settings.

- Made AVISO geometry, text, and groups shared across Dark, Light, and Real. Palette changes now affect colors only; older maps migrate using Light geometry when loaded or imported.

- Cached parsed tag definitions and font measurements across frames, with invalidation when settings change and bounded text-cache growth. Reused contiguous tag-token storage and drawing brushes, reduced hot-path copies and callback overhead, and gated detailed SDK timing behind verbose diagnostics.
- Applied consistent compiler warnings and binary hardening to local and CI builds, including Control Flow Guard and Spectre mitigations. Removed application-wide standard-library namespace pollution and marked security predicates `[[nodiscard]]`.
- Made RIMCAS runway-pair and ARR/DEP assignments follow the active airport's selected EuroScope runway ends automatically; manual closed-runway state remains independent.
- Reworked the Rules editor with a dedicated empty state, clearer condition columns, condition counts, and consistent shared controls.
- Refined the Rules editor into distinct identity, scope, condition, and color-override sections; expanded target symbol scaling to 0.25×–5.00× and made its fixed-size, theme-aware preview show a horizontal movement trail behind the aircraft.
- Renamed user-facing PDC reminder labels and messages to **CDM Reminder**.
- Reworked the Icons page around a dedicated preview and consistent settings cards, replaced the ambiguous Display navigation glyph, and moved every slider to one shared compact control style.
- Removed the legacy profile `.bak` fallback, restoration protocol, health state, UI action, and regression fixtures. Atomic writes, optimistic concurrency, Revert, and bundled-default recovery remain available.
- Made AVISO palette availability airport-specific: missing palettes are shown as disabled grey options and airport changes automatically select a valid fallback. Added geometry repairs and reported exclusions for misplaced source records during map conversion.

### Fixed

- Made active profile selection independent for each ASR. Opening, selecting, saving or closing one ASR no longer applies its profile to other screens; shared configuration reloads preserve each screen's selection.

- Fixed AVISO color edits and pasted colors changing other themes through inherited palette colors.

- Removed an ASR write from the radar close callback that could register changes after EuroScope had already asked whether to save. Active-profile persistence remains in the normal save callback.

- Made browser regression checks wait for real rendering and the page's completion result, preventing virtual-time timeouts from racing scroll-indicator updates.
- Preserved legacy colors and no-status tag definitions when profile migration replaces JSON fields, and made repeated normalization avoid rewriting unchanged definitions.
- Suppressed RDF indications for ground aircraft in SRW insets, using the scene's airborne classification.

- Fixed tags remaining detailed after the pointer leaves or a drag release is missed. Hover uses current tag bounds and the rendering window's cursor coordinates, with drag state isolated per radar view. Added detailed hover tags and their interactive fields to SRW and AVISO insets.

- Replaced the legacy RapidJSON snapshot with pinned upstream headers and applied bounded, iterative, UTF-8-validated parsing to every production JSON entry point. Excessive nesting, malformed encoding, and embedded NUL bytes now fail validation instead of overflowing the stack or silently parsing a prefix.
- Restricted updater downloads to exact approved hosts, required TLS 1.2 or newer and HTTPS port 443 in both HTTP clients, and enabled certificate revocation checking when discovering the updater signer.
- Made HTTP and hashing cleanup automatic, checked hash initialization failures, cleared stale hash results, and rejected missing rendering contexts before inset drawing.
- Prevented tag substitution from interpreting replacement values as further token names, and removed redundant CDM time formatting and per-target hover-text copies.
- Prevented another airport's selected runways from replacing the ASR/runtime airport and causing the active AVISO map to disappear.
- Prevented the Control Center from becoming stuck when rule settings were edited before a rule had been created. Rule fields and unavailable actions now remain disabled until a valid rule exists, and condition actions safely reject a missing draft.
- Aligned the Groups and Settings pages with the standard Control Center left-page offset.
- Applied the active interface theme to AVISO, SRW, and Timer inset title bars, and corrected AVISO inset tag text so its bounds and line layout remain vertically centered.
- Added automatic tag deconfliction to the AVISO inset and made its two-pass target rendering keep every aircraft symbol beneath every tag.
- Prevented the Tag Options behaviour controls from colliding at narrow widths and standardized the Control Center close glyph with native inset windows.
- Corrected the AVISO update policy to preserve bundled LFRJ and remove superseded maps, while retaining modified-map protection. LFPG labels retain one-pixel halos.

## [2.0.0-beta.5] - 2026-09-01

### Added

- Added a **No preset** action to the Runtime Menu. It clears the active inset preset and linked-view state, hides AVISO, SRW, Weather, and Timer insets, and returns to the main AVISO-only layout.
- Added per-display-mode maximum airborne altitude and ground-speed limits. Targets above either limit are omitted from the main AVISO and radar insets while RIMCAS safety processing remains active.
- Added an airport-specific Night/Day background color to every AVISO, exposed as the first color in the Geometry editor and rendered consistently in the main view, AVISO inset, and SRW.
- Added Copy and Paste actions to the Tag and Profile Color editors. Tag paste supports multi-selection and preserves normal/detailed layouts; color paste accepts 6- and 8-digit hex values including opacity.
- Added native regression tests for profile and AVISO validation, holding-point synchronization, tag tokens, RIMCAS runway monitoring, and radar geometry. AppVeyor now runs the suite and treats compiler warnings as errors.

### Changed

- Made automatic PDC reminders fail closed around airport and ground eligibility: queued messages are bound to one unambiguous active airport, require a fresh nearly stationary target within 5 NM of that airport, and are submitted at most once per callsign during the plug-in session. EuroScope command submission now waits for the command bar to consume the posted `.msg`; ambiguous results are not retried automatically.
- Reworked Settings into a compact two-column Display/updates and Data files layout, and removed the updater status badge and manual next-startup update-check action.
- Removed Control Center Undo/Redo and stopped normal profile and AVISO saves from automatically creating `.bak` files. Atomic replacement, failed-transaction rollback, Revert, and compatibility with existing profile backups remain available.
- Fixed loading vSMR again during the same EuroScope session after a safe unload had temporarily retained the runtime while radar screens and callbacks were still closing.
- Hardened runtime lifetime handling by publishing only fully constructed plug-ins, using monotonic clocks for long-running polling, and covering retained-runtime reload transitions with regression tests.
- Labeled profile `.bak` recovery as legacy data and now shows its modification date and age before restoration.
- Split plug-in commands and datalink protocol support, Control Center updater and performance processing, radar data types, and Web UI feature controllers into dedicated modules. Radar mutable state is now private, and regression tests enforce the Control Center script order and per-feature size boundary.
- Isolated the Control Center feature sources inside a generated private bundle, added headless browser coverage for initialization, host-state handling, event binding, and profile/AVISO saves, and extracted AVISO raster processing, plug-in runtime services, and bridge message routing from the remaining coordinators.
- Replaced the bundled profiles with the supplied five-profile configuration.
- Moved the previous bundled `Default` profile to `Custom LFPG` and restored `Default` from the 2.0.0-beta.2 profile set.
- Tightened square-corner tag borders while preserving the existing rounded-tag dimensions.
- Made runway and SID/custom rule matches use an operator selector plus an editable value list, including `in` and `not in` matching for both sources.
- Changed arrival tag classification so aircraft at 40 kt or below use the arrived presentation, including while still on the runway.
- Reworked Control Center persistence around one live model and a serialized latest-state save queue: every valid field is applied immediately, disk writes are lightly debounced, and acknowledgements advance revision tokens without repainting or replacing newer edits.
- Fixed clean preloaded Control Centers retaining a stale profiles revision after another radar window saved, which caused a false “profiles file changed in another vSMR window” warning on the next edit.
- Removed the CPDLC/PDC, RIMCAS debug, AVISO editor/reload, profile, config, and vSMR alias commands while retaining their supported Runtime and Control Center interfaces. Restored `.smr rdf on` and `.smr rdf off` for persistent native RDF control.
- Changed holding-point synchronization to the stable `VSMRHP/<value>` remarks marker and clean up duplicated markers produced by EuroScope's rewriting of the former `HP:<value>` format.
- Automatically removes the holding-point marker from flight-plan remarks when the correlated aircraft transitions to an airborne tag above 50 kt.
- Hardened the native RDF worker with an exception boundary and race-safe RAII ownership for WinHTTP and WebSocket handles, preventing worker failures from terminating EuroScope.
- Added strict size, depth, and item-count limits to profile and AVISO JSON loading, and made Windows installation paths Unicode-safe.
- Restricted the Control Center to its trusted local document and bounded both inbound WebView messages and the pending message queue.
- Replaced the Control Center's MultiByte resource picker with a Unicode-native Win32 dialog that preserves every path character without changing EuroScope's COM apartment.
- Made release-package rebuilds use the same warnings-as-errors compilation gate as CI.
- Made production packaging fail closed while bundled asset provenance remains unresolved; explicitly non-publishable local validation packages remain available.
- Removed obsolete profile-color and tag-editor mutation paths that bypassed the live transactional editor model.
- Aligned beta.5 version metadata, documentation, CI artifact names, and package policy; CI now publishes separate symbol archives and the beta.5 AVISO refresh protects locally edited maps by default.
- Reduced the installed Control Center payload to its generated bundle and runtime data, and removed development-only AVISO presets from the bundled profile configuration.
- Restricted release WebView resource discovery to installed plug-in roots, added a restrictive local-content policy, and bounded placement-file and bridge-input reads with their owning subsystem limits.

### Fixed

- Restored the canonical GPLv3 license text and made packaging reject merge-conflicted or mismatched license copies.
- Hardened updater transfers around timeout deadlines, redirects, response-size arithmetic, stream positioning, and process-launch errors; plug-in creation now remains exception-safe until EuroScope registration succeeds.
- Made publishable manual installs verify the pinned Authenticode signer on every executable component, and made full rollback validate the backed-up loader and its recorded metadata before restoration.
- Corrected radar graphics-context ownership, constructor cleanup, disconnected-aircraft cache cleanup, malformed optional-resource handling, and non-finite target geometry; reduced repeated RIMCAS parsing, copying, and lookups in refresh-sensitive paths.

## [2.0.0-beta.4] - 2026-08-24

### Added

- Synchronized AVISO, aircraft, trails, tags, RIMCAS overlays and hit-testing with EuroScope's native Rotate screen setting; native EuroScope panning and zooming now remain in the same coordinate system.
- Added configurable moving-aircraft position trails to the main radar, AVISO inset, and SRW. The Icons page controls trail visibility plus separate ground and airborne history lengths; NOVA uses compact vSMR history dots, Icon uses filled bubbles that grey and fade with age, and Triangle uses shrinking hollow circles.
- Added a persisted Night/Day AVISO color mode. Both the main view and AVISO inset select compact Day overrides from the same GeoJSON document, while existing base paint remains the Night palette and older/custom schema-2 files continue to work unchanged.
- Added release-controlled AVISO migrations with `none`, selected-airport, and all-airport modes, an official hash inventory for detecting local edits, complete rollback backups, and a default-on protection setting with a manual verified AVISO reload action.
- Added a synchronized `holdingpoint` tag token and EuroScope list item. Empty values omit their normal-tag row and use a clickable `HP` placeholder only in the detailed tag; either mouse button opens its selector, `None` clears the assignment, and assigned values are synchronized between controllers through flight-plan remarks without changing unrelated remarks.
- Added runway-aware holding-point selection to radar tags and EuroScope lists, with official airport/runway choices plus a leading `[...]` option for manual entry.

### Changed

- Moved CPDLC connection and PDC reminder controls from the Control Center into a compact native runtime-menu popup, while keeping the AVISO Day/Night selector with the AVISO editor.
- Split CPDLC credentials into an editable runtime login callsign and a password-only secure dialog, and replaced PDC timing fields with compact minute steppers.
- Made the CPDLC clearance-request notification sound unconditional and removed its obsolete setting and persisted state.
- UI changes across the Control Center.
- Restored the original NOVA target presentation: a configurable yellow irregular primary return, three cyan afterglow silhouettes for moving targets, white position-history dots, and a white Mode C diamond or non-Mode C cross. Icon and Triangle symbols follow radar zoom and their real-world dimensions; the centered symbol-scale slider applies a proportional 0.50–1.50 adjustment.
- Removed polygon outlines from AVISO rendering so edited area fills no longer retain an unrelated source stroke color; line features continue to render with their primary stroke color.
- Removed LFPG Dyna data selection and controller-ownership rendering; LFPG now uses only its standard airport GeoJSON.
- Replaced the bundled LFPG and LFMN AVISO documents and default profiles with the supplied data, removed AMSR, TMA, and VFR labels from every bundled airport map, and standardized gate/stand labels at zoom 9 and taxiway labels at zoom 7 outside LFPG and LFMN.
- Marked beta 4 as a mandatory all-airport AVISO replacement for the Night/Day migration, removed the obsolete `LFMM.geojson` and `LFPG_Dyna_fixed.geojson`, made the holding-point catalog package-owned, and added safe support for bundled variants such as `LFPG_Custom.geojson`; later releases must explicitly choose whether to update no maps, selected maps, or every map.
- Expanded the bundled holding-point catalogue from the French vACC vSID intersection data and added a validated, reproducible importer for future catalogue refreshes.
- Raised the bootstrap loader and beta 4 minimum-loader contract to `1.1.0`. Beta.3 installations require one complete manual beta.4 installation; release verification covers both a deterministic legacy fixture and the digest-pinned published beta.3 ZIP before beta.4 assets can be published.
- Made automatic configuration and datalink saves fully background operations and preloaded the hidden WebView2 Control Center after ASR initialization for a near-immediate first open.
- Accelerated routine Control Center autosaves with a shorter 300 ms debounce, retained validated in-memory owner configuration, compact AVISO writes without a redundant second parse, and compact authoritative profile/revision responses.
- Made AVISO and SRW inset content inherit EuroScope's live Sector / Inactive Sector Background color, added a thin black outer frame to both radar insets, and removed the obsolete per-profile SRW background override.
- Merged the supplied Day AVISO palette into the bundled airport data without duplicating geometry files. LFMN intentionally retains identical Day and Night colors.
- Restored the ESTimer alarm sound.
- Fixed AVISO `zoomLevel` visibility to use the same corner-to-corner viewport distance as the radar zoom level, and applied the rule consistently to labels, lines, and polygons at every airport.
- Made right-clicking an SRW tag background open EuroScope's Assume/Handoff list, and synchronized AVISO inset and SRW tag corners with the active profile's rounded-corners setting.
- Fixed RIMCAS alert-type changes being replaced by the previous live selection during autosave and restored EuroScope runway inheritance for profiles containing an empty runway list.
- Prevented moving main-view and inset AVISO caches from rescaling a near-native source by a single pixel, eliminating transient vertical and horizontal centre seams during panning.
- Corrected CPDLC clearance Next Frequency selection to use the lowest staffed departure position that issues the clearance: Delivery, then RMP, Ground, Tower, Approach/Departure, and Center fallback. LFPG north/south positions follow the departure runway complex when equivalent positions are online.
- Changed the fallback inactive-sector background used by AVISO and SRW insets to `#434A4F` until EuroScope's live inactive-sector color can be sampled.

## [2.0.0-beta.3] - 2026-08-21

### Added

- Added a normally fail-open, same-startup updater built around a stable `vSMR.dll` bootstrap and shadow-loaded `vSMR_Data\Runtime\vSMR.Runtime.dll`. Discovery, network, signature, compatibility, and pre-transaction failures leave the proven installed runtime available; an inconsistent durable install/rollback journal fails closed. It selects Stable or Beta GitHub releases, requires a pinned detached-CMS manifest signature, validates archive and internal package hashes, enforces Win32/runtime/loader compatibility, transactionally activates compatible runtime/data updates before creating the EuroScope plug-in, and rolls back to the previous runtime when initialization fails. Releases requiring a newer loader are reported for manual full-package installation.
- Added compact automatic-update status and preferences to General settings for checks, downloads, activation, channel selection, clearing previously skipped releases, manual full-package update notices, and next-startup check/retry requests. Update settings, durable recovery state, and status use the deterministic `%LOCALAPPDATA%\vSMR\Updater` journal and remain outside profile persistence; unwritable storage is reported instead of silently switching journals.
- Added bounded native performance diagnostics for render stages, scene capture, AVISO work, cache activity, worker queues, target processing, EuroScope lookups, GDI resources, bitmap memory, and refresh causes. The periodic `FramePerf` log remains available without adding a separate Settings page.
- Added out-of-process, WER-based crash reporting with direct/stack/worker association, fixed-size per-screen and per-thread breadcrumbs, recent logs, build/PDB identity, and an isolated crash harness. Reports are kept locally and are never uploaded automatically.
- Added the `remark` flight-strip annotation to the Control Center tag-token list.
- Added a persistent minimized Runtime Menu state: right-clicking the striped top handle now hides or restores all controls below it.
- Added LFPG dynamic frequency ownership from `LFPG_Dyna_fixed.geojson`: takeover chains determine non-RMP ownership, self-owned territory is visually distinct and label-free, unowned areas are hidden, and DEL points are intentionally ignored until polygons exist.
- Added separate west/east LFPG ground-layout groups containing the new brown, yellow-centerline, and green directional arrows.

### Changed

- Compacted the single Control Center Settings page so every data, display, and updater option fits in the fixed window without page scrolling.
- Redesigned the METAR inset around responsive wide, stacked, and compact layouts; added a larger wrapped raw report with highlighted wind, variation, visibility, weather, cloud, and QNH tokens; removed local controller time; and prioritized readable QNH, variation, and runway-component summaries at smaller window sizes.
- Simplified the Control Center Settings page to a single General view, removed the live Performance tab, and moved the compact automatic-update controls into General settings.
- Prioritized TSAT ahead of CTOT in the PDC window and clearance payload, pre-filled current vACDM TSAT/CTOT values when available, and rejected invalid optional UTC `HHMM` entries.
- Reduced AVISO raster churn by debouncing cache-backed view changes, cancelling superseded builds during geometry traversal, coalescing subpixel-equivalent requests, retrying transient failures, and retaining a same-source transformed raster through pan, zoom, preset, group, and ownership updates. AVISO data is now parsed once and prewarmed before the first rendered frame, raster overscan is smaller and hard allocation limits are enforced, while SRW reuses its bold font and measured typography metrics across frames.
- Introduced one immutable, per-radar `RadarScene` capture for each rendered frame and migrated the main radar, AVISO inset, SRW 1 inset, and native RDF overlay to its shared target, classification, icon/color, preformatted tag, finalized RIMCAS, dynamic-ownership, controller, and airport state. Viewports now repeat fewer EuroScope lookups, stay visually consistent, and report scene timing and size metrics through `FramePerf`.
- Unified Runtime Menu and Control Center navigation symbols: Settings uses shared slider controls, Groups uses layered views, Modes uses an eye, Insets uses a monitor, and the Control Center Display page uses an aircraft.
- Made the Control Center color swatch preview-only, removing the redundant system color picker because the complete color editor is already available beside it.
- Cleaned post-reorganization source metadata by removing an orphan dialog declaration, narrowing public header dependencies, and replacing stale generated file comments without changing runtime behavior.
- Reorganized the C++ and Control Center sources into a feature-oriented `vSMR/src/` tree with colocated headers and implementations and qualified project includes. This is a behavior-neutral source-layout change; the installed `vSMR_Data` structure and public/runtime interfaces are unchanged.
- Moved crash-report generation out of EuroScope's failing thread and into the packaged `vSMRCrashHandler.dll`. `%LOCALAPPDATA%\vSMR\CrashReports` is now the preferred private location, with probed plug-in-data and temporary-directory fallbacks, at most 10 report sets under a 256 MiB trimming budget, and text summaries flushed before dump creation.
- Made LFPG RMP activation service-wide: any connected reviewed RMP position activates all six RMP polygons, while their labels use the six area-specific GeoJSON `text-field`/`display_frequency` values and supplied coordinates instead of the connected controller's primary frequency. Removed the superseded static RMP frequency-label features.
- Reduced tag background padding in the main radar, AVISO inset, and SRW 1 inset so boxes fit their text more closely.
- Made `Release | Win32` the solution's sole configuration so an unspecified solution build cannot accidentally produce the much larger Debug DLL; the project-level Debug configuration remains available for explicit diagnostics.
- Removed the rounded corners from the Runtime Menu's outer frame while retaining rounded action buttons and icons.
- Extended release packages, symbols, metadata, CI validation, install/rollback helpers, and package verification for the stable loader, nested canonical runtime, signed external update manifest, durable transaction outcomes, verified backup runtimes, and explicit preserve-loader transactions.
- Bumped plug-in, Windows-resource, documentation, validation, and archive metadata to `2.0.0-beta.3`; the deliberately stable bootstrap-loader file version remains `1.0.0`.

### Fixed

- Prevented rotated AVISO views from disappearing during panning by retaining the valid raster instead of invalidating it from subpixel changes in an inferred rotation angle.
- Coalesced synchronized scratchpad/holding-point redraw notifications onto EuroScope's normal UI timer, preventing network update bursts from recursively flooding every open radar with immediate full-frame refreshes.
- Kept AVISO and SRW inset fills locked to EuroScope's inactive-sector background while the main view is zoomed into an active sector, and prevented the METAR center plate from masking the wind arrow.
- Synchronized AVISO Day/Night changes across every open radar screen sharing the Control Center configuration, and made stale main-view raster previews palette-aware so LFPG cannot continue displaying its previous Night bitmap during a slower Day rebuild.
- Made automatic PDC reminders session-only and fail-safe: every eligible aircraft now receives the complete monotonic delay, including aircraft with expired placeholder TOBTs; startup, stale/unavailable vACDM data, missing controller/airport state, TOBT removal, delay changes, and Stop now reset or re-arm the scheduler predictably.
- Prevented reminder floods and duplicates by defining zero cooldown as one reminder per eligibility period, spacing command-injection retries, stopping after an exhausted automatic retry batch, treating successful key-down submission as sent, and suppressing reminders while the PDC composer or transmission is active.
- Made Control Center performance exports prefer the documented `vSMR_Data\Diagnostics` directory, while retaining `%LOCALAPPDATA%` and temporary-directory fallbacks when the installation data folder is not writable.
- Replaced the Symbols page's generic square-like NOVA preview with a clean white filled primary-return silhouette without decorative center or trailing marks, retained `Show primary target` behavior, normalized the NOVA, Icon, and Triangle preview sizes, and shortened the aircraft-silhouette style label from `Icon (A320)` to `Icon`.
- Prevented handled first-chance exceptions from being mislabeled as fatal vSMR crashes, removed in-process DbgHelp/minidump work that could deadlock an unstable EuroScope process, and made existing-but-unwritable report directories fall through correctly.
- Made shared LFPG dynamic ownership boundaries deterministic: polygon fills render first, followed by self-owned outlines and then external-territory outlines, preventing cyan and yellow borders from clipping each other according to GeoJSON feature order.
- Prevented the LFPG AVISO map from flickering during normal network operation by caching its takeover rules and rebuilding its raster only when a resolved dynamic area's owner, self/other state, or displayed frequency actually changes; unrelated EuroScope controller updates no longer clear the map cache or rescan the full LFPG feature set.
- Replaced the METAR inset's heavy variable-wind arc with a translucent range band, highlighted endpoints, and a dedicated full-ring `VRB` treatment that leaves compass ticks readable.
- Refreshed the Control Center Groups list immediately after creating, duplicating, or deleting an AVISO group.
- Moved inset close buttons to the far-right title-bar edge while preserving resize-corner interaction.
- Reversed the METAR wind needle and variation arc to point toward airflow while retaining standard direction-from values and runway-component calculations.
- Added a 25-second `STAT RPA` grace period after an aircraft newly enters `DEPA`, preventing normal takeoff-clearance readback and initial movement delays from immediately raising a stationary-runway-protected-area alert.
- Preserved text, selection, and focus in EuroScope's command/message bar when an automatic CDM reminder injects and submits its private-message command.

## [2.0.0-beta.2] - 2026-08-14

### Added

- Added an independent four-minute countdown to the Timer inset and arranged the 1, 2, 3, and 4 minute timers in a compact 2×2 grid.

### Changed

- Retargeted the checked-in Debug and Release projects to MSVC `v145`; build automation can still request another installed compatible toolset explicitly.
- Updated the affected bundled AVISO label styles to use `#CCCCCC` text.
- Bumped plugin, Windows resource, documentation, validation, and archive metadata to `2.0.0-beta.2`.
- Pinned canonical profile, aircraft, and AVISO data to LF line endings so release validation is stable on Windows checkouts, and excluded AVISO aggregate/version source files from runtime packages.

### Fixed

- Replaced the browser-native Red, Green, Blue, and Opacity controls with explicit color-preview rails and matching thumbs, including a checkerboard transparency preview consistent with the Hue slider.
- Removed the unsupported Visual C++ AVISO project-item wildcard. Runtime AVISO assets remain copied by the build target, without the Visual Studio IDE instability/performance warning.
- Restored the Control Center color editor's full-spectrum hue rail and added a hue-colored, consistently positioned thumb instead of the browser-native monochrome slider.
- Applied AVISO feature-level paint overrides after shared catalog defaults, so saved per-label text color, font, size, halo, anchor, and zoom changes now appear immediately after the main radar and inset renderers reload. The same precedence now covers feature-level geometry color, opacity, and width overrides.
- Prevented a single AVISO edit from copying untouched paint or mixed visibility values into other labels/styles, rejected malformed hexadecimal colors instead of silently restoring the old value, and aligned both AVISO editors with the renderer's supported size and width ranges.
- Preserved intentional feature-level AVISO paint overrides during runtime-data normalization and now reports when a successful save cannot be reloaded by one or more radar renderers.

## [2.0.0-beta.1] - 2026-08-10

- Published the first vSMR 2.0 beta with the Control Center, native AVISO rendering and editing, common inset system, weather and timer insets, RIMCAS configuration, CPDLC/PDC workflows, transactional profile/AVISO persistence, and verified release packaging.

## [1.1.3] - 2026-06-20

### Added
- Added per-profile Tower Mode. Aircraft at `TAXI`, `DEPA`, `ARR`, or later states keep full tags, while targets with no status or an explicit `NSTS`, `PUSH`, or `STUP` status remain icon-only.
- Added checked `Pro mode` and `Tower mode` toggles to the top-bar Display menu, linked to the active profile settings.

### Fixed
- Arrival tags remain visible in Tower Mode even when the aircraft has no ground status.

### Changed
- Bumped plugin and Windows resource version metadata to `v1.1.3`.

## [1.1.2] - 2026-05-02

### Added
- Added `NOVA` icon style and icon-trail support.
- Added `Rounded Corners` tag option in the profile editor (`Icons & Tags -> Behavior`).
- Added persistent `labels.rounded_corners` profile key (global across tag types/statuses).

### Changed
- Renamed profile editor icon label from `Realistic` to `Icons`.
- Reworked icon-shape layout in the profile editor to two rows:
  - top: `Icons`, `NOVA`
  - bottom: `Arrow`, `Diamond`
- Moved icon-style selection out of the top `Target` menu; it is now managed in profile editor only.
- Updated About panel content and credits alignment with repository attribution.
- Bumped plugin/version metadata to `v1.1.2`.

### Fixed
- Fixed clipping issues in profile editor cards and controls at narrow widths.
- Fixed severe lag while resizing the profile editor window.
- Optimized icon rendering path, including realistic/icons mode hotspots.
- Fixed structured tag-color rule behavior so hover/detailed tags preserve normal rule colors when no detailed override exists.

### Repository
- Removed stale `tmp_alexis_upstream` gitlink/submodule entry.
- Removed `vSMR/Release_cli` build artifacts from repository tracking.

## [1.1.1]

### Changed
- Major profile JSON cleanup and normalization for `tags`, `icons`, and structured `rules`.
- Added migration path from older profile keys to the normalized layout.
- Simplified tag editor model around `Departure` and `Arrival` statuses.
- Aligned rules editor `Type` and `Status` options with tag classification.
- Improved arrival icon-state handling:
  - `Gate` remains separate
  - other on-ground arrival movement states use `On Ground`
- Unified profile list ordering (`Default` first, then alphabetical).
- Fixed profile editor selection sync when profile changes from radar menus.
