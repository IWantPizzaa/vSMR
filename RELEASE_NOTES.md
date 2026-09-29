# vSMR 2.0.0

Release preparation only: this version has not been published. Outstanding
resource/dependency permissions are tracked in
[ASSET_PROVENANCE.md](vSMR/data/Licenses/ASSET_PROVENANCE.md). The final release
date will be recorded in the changelog when publication is unblocked.

## Highlights

- A configurable Control Center for profiles, display modes, conditional colors,
  aircraft symbols, tags, AVISO geometry/text and alerts.
- 160 bundled airport AVISO maps, including Real palettes for LFBO and LFLL and
  the shared LFBO / LFLL profile. LFPG East and West arrows have separate groups.
- Independent active profiles per radar screen, airport-specific inset presets,
  and AVISO, SRW, METAR and Timer tools. Insets can also be hosted on CoFrance v2.
- Clearer METAR QNH and a refined wind rose; Day/Night support for Timer.
- Tag font sizes entered in pixels, compact Diamond trails and readable AVISO
  inset labels whose visibility adapts to the available drawing area.
- Redesigned manual PDC/CPDLC composers, with distinct editable/read-only fields
  and a working title-bar close action.
- Optional vSID, CDM and Ramp Agent integration through EuroScope Plugin Bridge;
  independent LFPG link/taxi controls and live taxi-area state when supplied.
- Per-file update feeds with SHA-256 checks, immutable content references,
  staged application and interruption recovery. Signing is optional.

## Important behavior changes

- The former vACDM HTTP integration and automatic PDC/CDM reminders are removed.
  Manual Hoppie messaging remains available.
- vSID configuration selection remains manual. There is no runway-driven
  automatic configuration selector and no Auto mode button in the vSMR popup.
- Ramp Agent provides `uk_stand` and `remark`; they no longer use flight-strip
  annotations 3 and 4.
- Rendering-resolution presets affect AVISO and aircraft presentation, not menu
  dimensions. Map rotation preserves small polygon corners when zooming out.
- Tag hover, inset-obscured native RDF, runway-dialog refresh timing and sound
  playback fallbacks have been corrected.

## Release components

Product version: **2.0.0**. Loader: **1.3.0**. Runtime ABI: **1**.

The final release must include the complete Win32 package and its matching ZIP
update manifest. Keep the matching symbols archive with the release build for
crash diagnosis. Publish the per-file stable feed separately, from the same
release source. A GitHub release or branch push does not publish that feed.

See [CHANGELOG.md](CHANGELOG.md) for the detailed changes and historical betas.
