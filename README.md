# vSMR 2.0 for EuroScope

vSMR is a configurable surface-movement radar plug-in for 32-bit EuroScope. It provides airport surface displays, aircraft tags and symbols, AVISO maps, RIMCAS alerts, native inset windows, VACDM data, and Hoppie CPDLC/PDC workflows.

Current version: **2.0.0-beta.5**

> Beta software should be validated in a safe environment before operational use. Keep a known-good backup and verify the active airport, profile, AVISO map, runway configuration, and alerts before controlling.

[Documentation](https://github.com/IWantPizzaa/vSMR/wiki) | [Releases](https://github.com/IWantPizzaa/vSMR/releases) | [Changelog](CHANGELOG.md) | [Report an issue](https://github.com/IWantPizzaa/vSMR/issues)

## Highlights

- Configurable surface radar with NOVA, aircraft-icon, and triangle targets
- Normal and detailed tags with status-specific layouts and structured color rules
- Night/Day AVISO maps with shared editing, groups, labels, and airport presets
- RIMCAS runway monitoring and configurable warning presentation
- AVISO, SRW 1, METAR, and Timer inset windows
- VACDM integration and Hoppie CPDLC/PDC support
- Optional vSID bridge data, tag tokens, rules, and Runtime Menu controls
- Airport-scoped layouts, display modes, profiles, and settings
- Transactional configuration, diagnostics, rollback, and signed-update support

See the [GitHub Wiki](https://github.com/IWantPizzaa/vSMR/wiki) for complete feature and configuration documentation.

## Requirements

- Windows with 32-bit EuroScope
- [Microsoft Visual C++ 2015-2022 Redistributable (x86)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
- [Microsoft Edge WebView2 Evergreen Runtime (x86)](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)
- The complete matching release package: `vSMR.dll` and `vSMR_Data\`

The optional vSID interface requires [vSID 0.15.0.2 or later](https://github.com/AlexisBalzano/vSID) and [EuroScope Plugin Bridge](https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge). Load both separately through EuroScope's plug-in settings; vSMR deliberately does not bundle or load the bridge DLL.

vSMR is a EuroScope plug-in, not a standalone application. WebView2 hosts the local Control Center; internet access is needed only for enabled online integrations and updates.

## Install or upgrade

1. Download the complete `vSMR-2.0.0-beta.5.zip` from [GitHub Releases](https://github.com/IWantPizzaa/vSMR/releases).
2. Close EuroScope.
3. Extract the archive to a temporary directory.
4. Run the packaged installer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR_Data\Tools\install_vsmr.ps1 `
  -DestinationDirectory "C:\path\to\EuroScope\Plugins"
```

5. Start EuroScope, open `Other Settings -> Plug-ins`, and load `vSMR.dll`.
6. Create or open a vSMR radar screen and verify the active airport and configuration.

Install beta.5 manually when upgrading from beta.3 or the unsigned beta.4 package. Use the complete package and do not preserve the older loader. Later compatible, signed releases can use the automatic updater.

The installer validates package hashes and creates a rollback backup before replacing files. Do not copy only the DLL or mix binaries and data from different versions.

Detailed procedures are maintained in the Wiki:

- [Installation and upgrades](https://github.com/IWantPizzaa/vSMR/wiki/Installation-and-Upgrades)
- [First-time setup](https://github.com/IWantPizzaa/vSMR/wiki/Getting-Started)
- [Backup and rollback](https://github.com/IWantPizzaa/vSMR/wiki/Backup-and-Rollback)
- [Automatic updates](https://github.com/IWantPizzaa/vSMR/wiki/Automatic-Updates)

## First run

1. Select the four-letter active-airport ICAO from the Runtime Menu.
2. Select a profile and display mode.
3. Open the Control Center with the Runtime Menu or `.smr`.
4. Verify the Profiles and AVISO paths in Settings.
5. Select the AVISO Dark, Light, or Real palette and review group visibility.
6. Configure RIMCAS runways and alert behavior.
7. Arrange the required insets and save an airport preset if needed.
8. Run `.smr diagnostics` and confirm the expected version and data sources.

Bundled operational data is a starting point and must be checked for the local airport and controlling position.

## Documentation

| Topic | Wiki page |
| --- | --- |
| Runtime Menu and Control Center | [Control Center](https://github.com/IWantPizzaa/vSMR/wiki/Control-Center) |
| Profiles, modes, tags, colors, and rules | [Profiles and Display](https://github.com/IWantPizzaa/vSMR/wiki/Profiles-and-Display) |
| AVISO maps, palettes, groups, and editing | [AVISO](https://github.com/IWantPizzaa/vSMR/wiki/AVISO) |
| RIMCAS alerts | [RIMCAS](https://github.com/IWantPizzaa/vSMR/wiki/RIMCAS) |
| Native inset windows | [Insets](https://github.com/IWantPizzaa/vSMR/wiki/Insets) |
| VACDM, CPDLC, PDC, and reminders | [Datalink](https://github.com/IWantPizzaa/vSMR/wiki/Datalink) |
| Commands, logs, and problem reports | [Troubleshooting](https://github.com/IWantPizzaa/vSMR/wiki/Troubleshooting-and-Diagnostics) |
| Source builds and release packaging | [Development](https://github.com/IWantPizzaa/vSMR/wiki/Development-and-Releases) |

## Useful commands

| Command | Purpose |
| --- | --- |
| `.smr` | Open the Control Center |
| `.smr diagnostics` | Write a bounded diagnostic report |
| `.smr rdf on` / `.smr rdf off` | Enable or disable native RDF display |

## Build and test

The supported release build is `Release | Win32`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\build_project.ps1
```

Run the regression suite independently with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tests\run_tests.ps1
```

Release packaging is fail-closed: publishable artifacts require a clean source commit, verified bundled-asset provenance, Authenticode-signed binaries, and the matching pinned update signer. See the [release documentation](https://github.com/IWantPizzaa/vSMR/wiki/Development-and-Releases).

## License

vSMR source code is licensed under the [GNU General Public License v3.0](LICENSE). Bundled dependencies and data assets retain their own terms; notices and provenance records are under `vSMR/data/Licenses/`.
