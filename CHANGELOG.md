# Changelog

## [2.0.0-beta.6] - 2026-09-05

### Added

- Added an independent Night/Day interface theme in Settings for the Control Center, native Runtime Menu, and METAR display. It remains separate from the AVISO palette; Night retains the existing appearance, while Day uses a lighter slate-grey palette coordinated with the `#434A4F` AVISO background.
- Added validated Copy/Paste actions for Rules and AVISO geometry/text styles. AVISO paste and profile-color editing support the existing Ctrl/Shift multi-selection workflow.
- Added delayed, theme-aware interaction explanations for buttons and editable controls throughout the Control Center.
- Added an optional vSID 0.15.0.2 interface through EuroScope Plugin Bridge. A dedicated Runtime Menu panel replaces manual vSID command entry with validated buttons for airport modes, diagnostics, synchronization, and reloads. Live vSID SID, runway, and cleared-flight-level values are available as `vsid_sid`, `vsid_rwy`, and `vsid_cfl` tag tokens and as a dedicated Rules source.
- Replaced the retired vACDM HTTP integration with the bridge-enabled CDM plug-in. Its operational time fields are available to tags and the dedicated CDM Rules source as TOBT, TSAT, TTOT, CTOT, TSAC, ASRT, and ASAT.
- Added a `ready_startup` tag token that displays `RDY` in red until CDM publishes ASRT, then changes it to green.
- Made `ready_startup` invoke CDM's authoritative Ready Start-up toggle when clicked, and added a Ready aircraft requirement to display modes.

### Changed

- Reworked the Rules editor with a dedicated empty state, clearer condition columns, condition counts, and consistent shared controls.
- Refined the Rules editor into distinct identity, scope, condition, and color-override sections; expanded target symbol scaling to 0.25×–5.00× and made its fixed-size, theme-aware preview show a horizontal movement trail behind the aircraft.
- Renamed user-facing PDC reminder labels and messages to **CDM Reminder**.
- Reworked the Icons page around a dedicated preview and consistent settings cards, replaced the ambiguous Display navigation glyph, and moved every slider to one shared compact control style.
- Removed the legacy profile `.bak` fallback, restoration protocol, health state, UI action, and regression fixtures. Atomic writes, optimistic concurrency, Revert, and bundled-default recovery remain available.
- Replaced the AVISO Night/Day selector with Dark, Light, and Real palettes. Dark retains the former Night colors, Light uses the LFPG Custom Day visual language, and Real preserves the former Day palettes for LFPG, LFMN, LFPO, and LFML.
- Made AVISO palette availability airport-specific: missing palettes are shown as disabled grey options and airport changes automatically select a valid fallback. LFPG now embeds its complete Custom geometry, text, groups, and colors for Dark/Light alongside the existing Real map in the single canonical `LFPG.geojson` asset.

### Fixed

- Prevented the Control Center from becoming stuck when rule settings were edited before a rule had been created. Rule fields and unavailable actions now remain disabled until a valid rule exists, and condition actions safely reject a missing draft.
- Aligned the Groups and Settings pages with the standard Control Center left-page offset.
- Applied the active interface theme to AVISO, SRW, and Timer inset title bars, and corrected AVISO inset tag text so its bounds and line layout remain vertically centered.
- Added automatic tag deconfliction to the AVISO inset and made its two-pass target rendering keep every aircraft symbol beneath every tag.
- Prevented the Tag Options behaviour controls from colliding at narrow widths and standardized the Control Center close glyph with native inset windows.
- Restored LFPG's East/West directional-arrow groups from `LFPG_Custom` and standardized all LFPG text halo widths at one pixel.

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
