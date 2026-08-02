# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added
- Added a built-in AVISO editor for GeoJSON AVISO files with live reload, object visibility, layer/name metadata, style fields, label text/font/position fields, simple line/label creation, duplication, deletion, and save/reload actions.
- Added airport-specific state and preset/default stores for AVISO, SRW 1, and SRW 2.
- Added SRW corner/split snapping, right-drag panning, and cursor-anchored wheel zoom.
- Added compact per-inset Reset actions to the Runtime Menu.
- Added CPDLC connection, polling, credentials, notification sound, and automatic PDC reminder controls to the Control Center Settings page.
- Added live CPDLC/CDM status reporting and manual CDM reminder scans for the active airport.

### Changed
- Renamed the Runtime Menu's `AVISO Insets` section to `Insets`.
- Unified floating inset title bars with the Runtime Menu's dark striped chrome.
- Removed the SRW `Z` range and `R` rotation menus; wheel zoom replaces the range menu.
- Made inset-preset writes authoritative across open radar screens and Control Center history without overwriting unrelated staged settings.
- Reworked the native PDC and received-message dialogs into compact, EuroScope-owned Cofrance-style windows while preserving editable PDC fields and message replies.
- Routed `.smr`, `.smr connect`, `.smr poll`, and `.smr cdm` through the shared native datalink service, with guarded asynchronous connection and polling operations.
- Consolidated automatic CPDLC saves and explicit PDC reminder Run, Stop, Update, and Check-now controls into Settings; removed the separate Datalink page, readiness card, feature switches, Advanced section, and Danger zone.
- Added the resolved EuroScope `.cdm` alias path to the Settings data-files card.

### Fixed
- Kept saved Hoppie credentials visibly masked after auto-save, normalized pasted codes, URL-encoded credentials, and replaced the misleading callsign-collision login error with the actual sanitized Hoppie or network failure.
- Stopping or rescheduling automatic PDC reminders now removes only queued automatic reminders while preserving manual reminder checks.

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
