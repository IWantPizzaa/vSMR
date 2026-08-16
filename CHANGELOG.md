# Changelog

## [2.0.0-beta.2] - 2026-08-14

### Added

- Added out-of-process, WER-based crash reporting with direct/stack/worker association, fixed-size per-screen and per-thread breadcrumbs, recent logs, build/PDB identity, and an isolated crash harness. Reports are kept locally and are never uploaded automatically.
- Added an independent four-minute countdown to the Timer inset and arranged the 1, 2, 3, and 4 minute timers in a compact 2×2 grid.
- Added the `remark` flight-strip annotation to the Control Center tag-token list.
- Added a persistent minimized Runtime Menu state: right-clicking the striped top handle now hides or restores all controls below it.
- Added LFPG dynamic frequency ownership from `LFPG_Dyna_fixed.geojson`: takeover chains determine non-RMP ownership, self-owned territory is visually distinct and label-free, unowned areas are hidden, and DEL points are intentionally ignored until polygons exist.

### Changed

- Reorganized the C++ and Control Center sources into a feature-oriented `vSMR/src/` tree with colocated headers and implementations and qualified project includes. This is a behavior-neutral source-layout change; the installed `vSMR_Data` structure and public/runtime interfaces are unchanged.
- Moved crash-report generation out of EuroScope's failing thread and into the packaged `vSMRCrashHandler.dll`. `%LOCALAPPDATA%\vSMR\CrashReports` is now the preferred private location, with probed plug-in-data and temporary-directory fallbacks, at most 10 report sets under a 256 MiB trimming budget, and text summaries flushed before dump creation.
- Made LFPG RMP activation service-wide: any connected reviewed RMP position activates all six RMP polygons, while their labels use the six area-specific GeoJSON `text-field`/`display_frequency` values and supplied coordinates instead of the connected controller's primary frequency. Removed the superseded static RMP frequency-label features.
- Reduced tag background padding in the main radar, AVISO inset, and SRW 1 inset so boxes fit their text more closely.
- Made `Release | Win32` the solution's sole configuration so an unspecified solution build cannot accidentally produce the much larger Debug DLL; the project-level Debug configuration remains available for explicit diagnostics.
- Retargeted the checked-in Debug and Release projects to MSVC `v145`; build automation can still request another installed compatible toolset explicitly.
- Updated the affected bundled AVISO label styles to use `#CCCCCC` text.
- Removed the rounded corners from the Runtime Menu's outer frame while retaining rounded action buttons and icons.

### Fixed

- Prevented handled first-chance exceptions from being mislabeled as fatal vSMR crashes, removed in-process DbgHelp/minidump work that could deadlock an unstable EuroScope process, and made existing-but-unwritable report directories fall through correctly.
- Made shared LFPG dynamic ownership boundaries deterministic: polygon fills render first, followed by self-owned outlines and then external-territory outlines, preventing cyan and yellow borders from clipping each other according to GeoJSON feature order.
- Prevented the LFPG AVISO map from flickering during normal network operation by caching its takeover rules and rebuilding its raster only when a resolved dynamic area's owner, self/other state, or displayed frequency actually changes; unrelated EuroScope controller updates no longer clear the map cache or rescan the full LFPG feature set.
- Replaced the browser-native Red, Green, Blue, and Opacity controls with explicit color-preview rails and matching thumbs, including a checkerboard transparency preview consistent with the Hue slider.
- Removed the unsupported Visual C++ AVISO project-item wildcard. Runtime AVISO assets remain copied by the build target, without the Visual Studio IDE instability/performance warning.
- Restored the Control Center color editor's full-spectrum hue rail and added a hue-colored, consistently positioned thumb instead of the browser-native monochrome slider.
- Applied AVISO feature-level paint overrides after shared catalog defaults, so saved per-label text color, font, size, halo, anchor, and zoom changes now appear immediately after the main radar and inset renderers reload. The same precedence now covers feature-level geometry color, opacity, and width overrides.
- Prevented a single AVISO edit from copying untouched paint or mixed visibility values into other labels/styles, rejected malformed hexadecimal colors instead of silently restoring the old value, and aligned both AVISO editors with the renderer's supported size and width ranges.
- Preserved intentional feature-level AVISO paint overrides during runtime-data normalization and now reports when a successful save cannot be reloaded by one or more radar renderers.
- Preserved text, selection, and focus in EuroScope's command/message bar when an automatic CDM reminder injects and submits its private-message command.

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
