# Changelog

## [2.0.0-beta.1] - 2026-08-10

This beta is a major runtime, configuration, and distribution update. It is
intended for controlled operational testing rather than production promotion.

### Added

- Added a modeless, EuroScope-owned Control Center for Display, AVISO, Alerts, Groups, Modes, Profiles, and Settings. Its WebView2 interface is loaded entirely from the packaged `vSMR_Data\vSMR_webUI` assets.
- Added the compact Runtime Menu for changing the active airport, profile, display mode, AVISO groups, inset visibility, and airport-specific layout presets without leaving the radar screen.
- Added native AVISO GeoJSON rendering to the main radar view, including styled geometry, text labels, group visibility, screen rotation, asynchronous raster generation, and cached interaction previews.
- Added an independent AVISO inset that renders the airport, targets, symbols, tags, and configured colors with its own pan and cursor-anchored zoom.
- Added a common inset window system for AVISO, SRW 1, Weather, and Timer. Resizable insets support every edge and corner, practical hit areas, matching resize cursors, drag capture, Windows-style snap previews, edge splits, and size-preserving corner docking.
- Added airport-scoped inset working state, named presets, default presets, linked main/inset movement, and per-inset Reset actions. Presets remain available when the active profile changes.
- Added a native Weather inset with the latest EuroScope METAR, bounded VATSIM fallback retrieval, wind rose, active-runway wind components, QNH, UTC, and local time.
- Added a striped Timer inset with independent 1, 2, and 3 minute countdowns. Left-click starts a timer, right-click resets it, and `vSMR_Data\Audio\Alarm.wav` plays once at expiry.
- Added a built-in native AVISO editor for source selection, live reload, feature filtering, editable geometry coordinates, point-label positioning, deletion, visibility, group membership, style metadata, text, and fonts.
- Added configurable display modes with target-status visibility and operational requirements, including the v1.1 Pro and Tower behaviors.
- Added Hoppie CPDLC connection and polling, compact PDC/Message windows, datalink tag actions, and automatic CDM/PDC reminder scheduling with Run, Stop, cooldown, and manual check controls.
- Added `.smr connect`, `.smr poll`, `.smr cdm`, `.smr cdm auto`, `.smr cdm cooldown`, and `.smr diagnostics` workflows alongside the existing radar commands.
- Added a redacted diagnostics report and bounded log rotation for beta support.
- Added a native TrackAudio RDF overlay for the main surface radar, AVISO inset, and SRW 1 inset. Each view uses its own projection, with white 20-pixel rings for normal transmissions and red rings for concurrent transmissions.
- Added persistent `.smr rdf`, `.smr rdf status`, `.smr rdf on`, and `.smr rdf off` controls. Native RDF is enabled by default and connects to `ws://127.0.0.1:49080/ws`.
- Added `vSMR_Data\Tools\RDF-vSMR-ground-view.patch` for official RDF commit `a4bd0ae5272088286acee1c2495ed3e4a2e627c6`. The GPL-3.0 compatibility patch prevents the external RDF from attaching to vSMR's `SMR radar display`, while leaving approach displays unchanged.
- Added a native Line Up (`LNUP`) ground status. The vSMR ground-status menu publishes EuroScope-compatible `TAXI` while maintaining a shared session-local LNUP override, and exposes independent display-mode visibility, target color, tag color, tag definition, and structured-rule matching in both editors.

### Changed

- Retargeted the checked-in Debug and Release projects to MSVC `v145`; build automation can still request another installed compatible toolset explicitly.
- Replaced the previous editor and toolbar workflows with the Control Center, Runtime Menu, and optional compact FPS readout. The Control Center keeps its current page while open and restores its native window position across sessions.
- Removed local `Update`, `Apply`, and `Revert` controls from Control Center editors. Profile, display, AVISO, alert, group, mode, CPDLC, and PDC reminder field changes now enter the shared draft automatically; the single global `Save` action validates, persists, and applies that draft.
- Harmonized Control Center pages, Runtime Menu controls, inset title bars, and datalink dialogs around the same Cofrance/vSMR colors, spacing, typography, cards, buttons, and striped chrome.
- Consolidated CPDLC connection and PDC reminder settings into Settings and removed the separate Datalink page, readiness/status cards, feature switches, Advanced section, and Danger zone. The resolved EuroScope `.cdm` alias path is shown with the other data files.
- Reworked the native PDC and received-message dialogs as compact frameless vSMR popups, with `ADEP`, `ADES`, and `RWY` field names and movement constrained to the EuroScope client area.
- Migrated profiles to the validated schema-2 configuration model while retaining transactional migration from compatible v1 files and legacy read paths where safe.
- Made inset state and preset/default storage airport-specific instead of profile-specific, authoritative across open radar screens, and resistant to stale Control Center snapshots.
- Edge-snapped insets now reserve their exact area from the main AVISO view; corner-snapped windows keep their floating size. All insets use a coherent `X` close action, and obsolete thick snap borders were removed.
- Unified AVISO and SRW interaction around right-drag panning and cursor-anchored wheel zoom. Removed the SRW `Z` range and `R` rotation menus; its floating `F` altitude control is hidden when docked.
- Removed the Approach Path/SRW 2 inset and its rendering, persistence, preset, and runtime logic while retaining later inset IDs for compatibility.
- Moved the Timer alarm and CPDLC notification sounds to `vSMR_Data\Audio` so both are replaceable runtime assets rather than DLL resources.
- Canonicalized the five bundled profiles, all six AVISO airport files, and the 873 usable aircraft-dimension records for direct use by vSMR 2.0.
- Loading a local Profiles or AVISO file now activates that exact file. GitHub imports are validated and stored as collision-safe files under `vSMR_Data\Profiles` or `vSMR_Data\AVISO` instead of overwriting bundled airport/profile data.
- Reduced radar and editor work through cached fonts, symbols and runway headings, immutable AVISO snapshots, indexed runway occupancy, shared target metadata, and deduplicated Control Center and group refreshes.
- Reorganized deployment so a release root contains only `vSMR.dll` and `vSMR_Data\`; the Control Center UI, audio, icons, AVISO, licenses, tools, and data all live below `vSMR_Data`.
- Applied effective LNUP state consistently to the main radar and AVISO target/tag renderers, Tower/display-mode filtering, status tokens, and structured color rules. Existing profiles inherit their initial LNUP presentation from Taxi, and all bundled profiles now contain explicit Line Up settings.
- Updated RIMCAS so LNUP authorizes runway entry and taxi movement without authorizing takeoff: `RWY INC` and `NO TAXI` are suppressed, while `NO TKOF` still requires `DEPA`.

### Fixed

- Removed the unsupported Visual C++ AVISO project-item wildcard. Runtime AVISO assets remain copied by the build target, without the Visual Studio IDE instability/performance warning.
- Restored the Control Center color editor's full-spectrum hue rail and added a hue-colored, consistently positioned thumb instead of the browser-native monochrome slider.
- Applied AVISO feature-level paint overrides after shared catalog defaults, so saved per-label text color, font, size, halo, anchor, and zoom changes now appear immediately after the main radar and inset renderers reload. The same precedence now covers feature-level geometry color, opacity, and width overrides.
- Prevented a single AVISO edit from copying untouched paint or mixed visibility values into other labels/styles, rejected malformed hexadecimal colors instead of silently restoring the old value, and aligned both AVISO editors with the renderer's supported size and width ranges.
- Preserved intentional feature-level AVISO paint overrides during runtime-data normalization and now reports when a successful save cannot be reloaded by one or more radar renderers.
- Prevented the main AVISO display from briefly disappearing during zoom and prevented the AVISO inset from transiently stretching, shrinking, or changing aspect ratio while a new raster is rendered.
- Stabilized split-panel resize rendering so the left radar view remains geographically anchored and the right AVISO inset does not twitch or rescale during drag.
- Aligned visible resize cursors with the actual edge/corner hitboxes and reliably cleared resize state and cursor ownership when a drag is released or leaves the window.
- Restored drag, resize, wheel, and general EuroScope interaction after inset mouse capture, including safe release during interrupted operations.
- Reapplied saved inset geometry after EuroScope radar bounds settle, fixing incorrectly placed startup presets while preserving the Runtime Menu position.
- Preserved intentionally empty AVISO group lists, the chosen AVISO text style, and unrelated staged edits when saving or synchronizing runtime state.
- Replaced placeholder symbol drawings in the Control Center with the native NOVA and triangle geometry and the bundled A320 icon render.
- Prevented local or downloaded AVISO/profile imports from replacing canonical files and kept Settings, Reload, ASR state, and all open radar screens on the selected active source.
- Kept saved Hoppie credentials masked after automatic persistence, normalized pasted codes, URL-encoded connection values, and surfaced the sanitized Hoppie/network failure instead of a misleading callsign-collision message.
- Stopping or rescheduling automatic reminders now removes only queued automatic reminders and preserves manual checks.
- Removed duplicate Windows chrome and system edit styling from PDC/Message windows and corrected their close and drag hit areas.
- Fixed shutdown hangs, guarded EuroScope callbacks and background work against exceptions, and prevented stale asynchronous AVISO or network results from replacing newer state.
- Corrected SRW drawing-state restoration, RIMCAS runway cache behavior, and several avoidable per-frame and once-per-second scans.
- Fixed ARR/DEP and Closed checkbox changes on `Alerts > RIMCAS runways` so a `change` event is always staged before the global Save, even when WebView does not emit a preceding `input` event.
- Preserved an explicitly empty RIMCAS runway list instead of repopulating it from the current runtime runway inventory after reload, while preventing a temporary geometry-cache reset during an airport change from publishing a false empty runtime list.
- Added EuroScope's airport/runway-activity callback and explicit per-screen sector selection so accepting manual ARR/DEP changes in the `Active Airports/Runways` dialog immediately refreshes conditional maps, weather data, and vSMR insets. A single unambiguous active airport is adopted automatically; multi-airport configurations keep each screen's explicit vSMR airport.
- Corrected legacy-profile runway discovery so an ARR-only runway is no longer also enabled for DEP; explicit RIMCAS runway configuration remains independent and authoritative.

### Compatibility

- Documented why the official RDF's `.RDF ASR DRAW 0` setting is not a reliable concurrent-screen exclusion at the pinned revision: each screen refresh loads its ASR values into the plug-in's shared drawing state. The supplied creation-time display exclusion is required when external RDF and native vSMR RDF are used together.
- EuroScope does not provide a native LNUP state. vSMR therefore publishes TAXI to EuroScope and other plug-ins, preserves the existing scratchpad content, and keeps LNUP only for the current vSMR session; restarting safely degrades it to TAXI.

### Security and reliability

- Protected persisted Hoppie codes with Windows DPAPI for the current user and migrated legacy plaintext values without retaining a plaintext fallback.
- Restricted network configuration to HTTPS, bounded response sizes and timeouts, redacted credentials and message payloads from diagnostics, and kept CPDLC, VACDM, weather, and download work away from the EuroScope UI thread.
- Added schema and semantic validation, atomic replacement, backup rotation, recovery prompts, revision checks, and transactional saves for profiles and AVISO documents.
- Hardened ASR parsing and inset geometry restoration against malformed, incomplete, stale, or out-of-range values.

### Packaging and repository

- Unified plugin, Windows resource, documentation, validation, and archive versioning as `2.0.0-beta.1`.
- Added repeatable Release/Win32 packaging with Visual Studio/toolset discovery, NuGet restore, a clean rebuild, runtime-data validation, clean staging, exact manifests, SHA-256 checks, Git provenance, package verification, and a separate private-symbol archive.
- Added package-validated install and rollback helpers with complete timestamped backups, preservation of user/imported data by default, and explicit opt-in replacement of user data.
- Added optional Authenticode signing and a signature-required public-release gate.
- Enabled warning level 4, SDL and buffer-security checks, DEP/ASLR, private PDB generation, and a warning-clean build while treating the vendored EuroScope SDK as an external header dependency.
- Included the vSMR, RapidJSON, and Microsoft WebView2 license material, dependency manifest, payload checksums, release metadata, and asset-provenance register in every package.
- Removed the redundant bundled `vSMR_Maps.json` while retaining its optional compatibility loader for airports without AVISO data; added deterministic validation for profiles, aircraft dimensions, and bundled AVISO files.
- Removed unused bundled Asio/libcurl code, the obsolete custom resize cursor, superseded project notes, the repository test project and fixtures, and repository-local GitHub automation. Release validation remains available through the maintained PowerShell tools and AppVeyor configuration.

### Known beta limitations

- vSMR and EuroScope are Win32 applications; the x86 Visual C++ runtime, x86 MFC runtime, and x86 WebView2 Evergreen Runtime are required.
- Hoppie, VACDM, VATSIM weather fallback, and GitHub imports depend on independent external services and can fail without stopping the radar display.
- Weather runway components are advisory; this beta does not automatically select or modify EuroScope's active runways.
- The bundled airport and aircraft data must be checked against current local operational requirements before controlling.
- Some aircraft icon, audio, AVISO, and aircraft-dimension provenance entries still require verification; see `vSMR_Data\Licenses\ASSET_PROVENANCE.md` in the package.
- Authenticode support does not mean a particular beta archive is signed. Treat it as signed only when package verification confirms a valid signature.
- Native vSMR RDF currently reads TrackAudio's loopback WebSocket only. Audio for VATSIM standalone hidden-window transmissions are not consumed by the native overlay.

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
