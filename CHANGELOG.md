# Changelog

## [2.0.0-beta.4] - 2026-08-24

### Added

- Synchronized AVISO, aircraft, trails, tags, RIMCAS overlays and hit-testing with EuroScope's native Rotate screen setting; native EuroScope panning and zooming now remain in the same coordinate system.
- Added configurable moving-aircraft position trails to the main radar, AVISO inset, and SRW. The Icons page controls trail visibility plus separate ground and airborne history lengths; NOVA uses compact vSMR history dots, Icon uses filled bubbles that grey and fade with age, and Triangle uses shrinking hollow circles.
- Added a persisted Night/Day AVISO color mode. Both the main view and AVISO inset select compact Day overrides from the same GeoJSON document, while existing base paint remains the Night palette and older/custom schema-2 files continue to work unchanged.
- Added release-controlled AVISO migrations with `none`, selected-airport, and all-airport modes, an official hash inventory for detecting local edits, complete rollback backups, and a default-on protection setting with a manual verified AVISO reload action.
- Added a synchronized `holdingpoint` tag token and EuroScope list item. Empty values omit their normal-tag row and use a clickable `HP` placeholder only in the detailed tag; either mouse button opens its selector, and assigned values remain visible while EuroScope synchronizes them through the scratchpad without changing unrelated scratchpad text.
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
- Added compact automatic-update status and preferences to General settings for checks, downloads, activation, channel selection, clearing previously skipped releases, manual full-package update notices, and next-startup check/retry requests. Update settings, durable recovery state, and status use the deterministic `%LOCALAPPDATA%\vSMR\Updater` journal and remain outside profile Save/Undo history; unwritable storage is reported instead of silently switching journals.
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
