# Changelog

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
