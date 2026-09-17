# vSMR 2.0 for EuroScope

vSMR is a configurable surface-movement radar plug-in for 32-bit EuroScope. It provides airport surface displays, aircraft tags and symbols, AVISO maps, RIMCAS alerts, native inset windows, CDM data, and Hoppie CPDLC/PDC workflows.

Current development version: **2.0.0-beta.6** (`dev`). This README describes the current source and bundled data; published packages may differ.

> Beta software should be validated in a safe environment before operational use. Keep a known-good backup and verify the active airport, profile, AVISO map, runway configuration, and alerts before controlling.

[Documentation](https://github.com/IWantPizzaa/vSMR/wiki) | [Releases](https://github.com/IWantPizzaa/vSMR/releases) | [Changelog](CHANGELOG.md) | [Beta 6 guide](https://github.com/IWantPizzaa/vSMR/wiki/Beta-6-Release-Notes) | [Report an issue](https://github.com/IWantPizzaa/vSMR/issues)

## Highlights

- Configurable surface radar with NOVA, aircraft-icon, and triangle targets
- Normal and detailed tags with status-specific layouts, structured color rules, and optional per-line backgrounds
- 160 bundled AVISO airport maps, with shared geometry/text editing and airport-specific palettes
- Resolution presets for AVISO rendering and aircraft icons/tags, without resizing menus or other UI
- Automatic RIMCAS runway assignment from EuroScope's active-airport runway selection
- AVISO, SRW 1, METAR, and Timer inset windows
- CDM bridge integration and Hoppie CPDLC/PDC support
- Optional vSID bridge data, tag tokens, rules, and Runtime Menu controls
- Optional Ramp Agent stand and stand remark tag values through the plug-in bridge
- Airport-scoped inset presets and independent active-profile selection for each ASR
- Atomic configuration saves, Revert, bundled-default recovery, diagnostics, and verified-update support

## Beta 6 behavior and compatibility

- **Resolution:** Settings offers 1080p (100%), 2K (133%), and 4K (200%). These scale AVISO labels/lines and aircraft icons, trails, tags, and their hit areas in the main view and radar insets. They do not resize the Control Center, Runtime Menu, inset controls, weather/timer panels, or FPS display, and do not change geographic positions or radar zoom.
- **Interface theme:** Night/Day UI colors are independent of the AVISO palette. Dark/Light palettes are available in the current bundled maps; Real is available for LFML, LFMN, LFPG, and LFPO. Other maps may offer different palettes.
- **ASR profiles:** Each radar screen keeps its own active profile. Profile definitions and the profiles-file source remain shared; save the ASR to retain its selection.
- **CDM:** The bridge-enabled CDM plug-in replaces the retired vACDM HTTP integration. The old CDM Auto/reminder workflow, timer, and message queue are no longer present. Manual Hoppie CPDLC/PDC workflows remain available.
- **Stand data:** `uk_stand` and `remark` come from Ramp Agent through EuroScope Plugin Bridge, not flight strip annotations 3 and 4. Missing providers leave their related values unavailable without disabling the rest of vSMR.
- **Configuration recovery:** The Control Center has Revert and Bundled defaults. The old Undo/Redo controls and legacy profile `.bak` restoration are no longer available. Keep your own configuration backups.

The current converter-supplied AVISO set contains **160 maps** in [`vSMR/data/AVISO/`](vSMR/data/AVISO/), replacing the previous 192-map set. Maps absent from the replacement set are no longer bundled. Map groups depend on the supplied airport file: the current LFPG file does **not** include the former East/West arrow-control groups. Those map groups are separate from the vSID configuration controls below.

## Requirements

- Windows with 32-bit EuroScope
- [Microsoft Visual C++ 2015-2022 Redistributable (x86)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
- [Microsoft Edge WebView2 Evergreen Runtime (x86)](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- The complete matching release package: `vSMR.dll` and `vSMR_Data\`

The optional vSID, Ramp Agent, and CDM interfaces require [EuroScope Plugin Bridge](https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge), plus a bridge-enabled [vSID](https://github.com/AlexisBalzano/vSID), [Ramp Agent](https://github.com/AlexisBalzano/EuroscopeRampAgent), or [CDM](https://github.com/IWantPizzaa/CDM) build. Stand and stand remark tag values come only from Ramp Agent through the bridge. Load them separately through EuroScope's plug-in settings; vSMR deliberately does not bundle or load their DLLs. The consumed fields are listed in [EuroScope Plugin Bridge data](docs/integrations/plugin-bridge.md).

[Paris configuration](docs/integrations/vsid-paris.md) requires the companion vSID build and configuration migration. LFPG/LFPO offer manual Linked/Unlinked selections; LFPG also has Minimum Taxiing and Ground Crossing actions. LFPN, LFPV, LFPT, and LFOB offer manual WL/EL/IPGW/IPOW selections. **Auto runways has been removed:** EuroScope runway changes do not select these rules. vSID's separate **Auto mode** for automatic SID assignment remains available. This does not affect RIMCAS, which still follows EuroScope's selected runways automatically.

vSMR is a EuroScope plug-in, not a standalone application. WebView2 hosts the local Control Center; internet access is needed for online integrations, updates, and GitHub data imports.

## Install or upgrade

1. Choose a published release and download its complete `vSMR-<version>.zip` from [GitHub Releases](https://github.com/IWantPizzaa/vSMR/releases). A development version in this README does not imply its package has been published.
2. Close EuroScope and back up your existing installation, profiles, custom AVISO maps, and ASRs.
3. Extract the complete matching package into your EuroScope plug-in folder, keeping `vSMR.dll` beside `vSMR_Data\`. Do not mix loader/runtime files from different packages.
4. Start EuroScope and load `vSMR.dll` through its plug-in settings. Load any optional bridge/provider plug-ins separately.

Detailed procedures are maintained in the Wiki:

- [Installation and upgrades](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating)
- [First-time setup and verification](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#install-or-upgrade)
- [Backup and rollback](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#backup-rollback-and-verification)
- [Automatic updates](https://github.com/IWantPizzaa/vSMR/wiki/Installing-and-Updating#updates-and-custom-data)

## First run

1. Open an SMR radar screen and select the four-letter active-airport ICAO from the Runtime Menu. Right-click the ICAO field to access the five most recent airports for that screen's session.
2. Select a profile and display mode.
3. Open the Control Center with the Runtime Menu or `.smr`.
4. Verify the Profiles and AVISO paths in Settings.
5. Choose an available AVISO palette, review group visibility, and select a rendering resolution in Settings if needed.
6. Verify that RIMCAS reflects the active airport's selected EuroScope runways, then configure alert behavior and any closed-runway state.
7. Arrange the required insets and save an airport preset if needed.
8. Run `.smr diagnostics` and confirm the expected version and data sources.

Bundled operational data is a starting point and must be checked for the local airport and controlling position.

## Documentation

| Topic | Wiki page |
| --- | --- |
| Runtime Menu and Control Center | [Control Center](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center) |
| Profiles, modes, tags, colors, and rules | [Profiles and Display](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center-Display) |
| AVISO maps, palettes, groups, and editing | [AVISO](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center-AVISO) |
| RIMCAS alerts | [RIMCAS](https://github.com/IWantPizzaa/vSMR/wiki/RIMCAS) |
| Native inset windows | [Insets](https://github.com/IWantPizzaa/vSMR/wiki/Insets) |
| CDM bridge data and manual CPDLC/PDC | [Datalink](https://github.com/IWantPizzaa/vSMR/wiki/Datalink), [current bridge fields](docs/integrations/plugin-bridge.md) |
| Optional providers and bridge setup | [Integrations](https://github.com/IWantPizzaa/vSMR/wiki/Integrations) |
| Manual Paris vSID configuration | [Paris configuration](https://github.com/IWantPizzaa/vSMR/wiki/Paris-vSID-Configuration) |
| Commands, logs, and problem reports | [Troubleshooting](https://github.com/IWantPizzaa/vSMR/wiki/Commands-and-Troubleshooting) |
| Source builds and release packaging | [Development](https://github.com/IWantPizzaa/vSMR/wiki/Development-and-Releases) |

## Useful commands

| Command | Purpose |
| --- | --- |
| `.smr` | Open the Control Center |
| `.smr diagnostics` | Write a bounded diagnostic report |
| `.smr reload` | Reload runtime configuration/data for open radar screens |
| `.smr rdf on` / `.smr rdf off` | Enable or disable native RDF display |
| `.smr log normal` / `.smr log verbose` / `.smr log off` | Set logging verbosity or disable logging |
| `.smr log status` | Show logging state and log location |

## Build and test

Build from the repository root with Visual Studio C++ tools, MFC support, and a Windows SDK. The build script defaults to MSVC toolset `v145` and `Release | Win32`; it rebuilds the solution and runs native and browser regressions. Microsoft Edge is required for the browser tests.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\build_project.ps1
```

After building the native test executable, run the regression suite independently with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tests\run_tests.ps1
```

Release-input checks enforce matching beta 6 versions, the exact 160-map import, and an update policy that never deletes a bundled airport. LFPG regression expectations match the supplied map's 1,468 features and empty group list; the older East/West arrow groups are not part of this import.

Release packaging is fail-closed: publishable artifacts require a clean source commit, verified bundled-asset provenance, Authenticode-signed binaries, and the matching pinned update signer. The packager, binary product versions, and AppVeyor settings target beta 6. Five asset groups still need provenance verification; local validation packages are not distributable releases. See the [beta 6 release checklist](docs/beta-6-release.md), [release documentation](https://github.com/IWantPizzaa/vSMR/wiki/Development-and-Releases), and [provenance register](vSMR/data/Licenses/ASSET_PROVENANCE.md).

## License

vSMR source code is licensed under the [GNU General Public License v3.0](LICENSE). Bundled dependencies and data assets retain their own terms; notices and provenance records are under `vSMR/data/Licenses/`.
