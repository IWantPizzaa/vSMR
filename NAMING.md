# Project file naming

Use the existing convention for each file type, and name files after their responsibility.

| Files | Convention | Examples |
| --- | --- | --- |
| C++, C#, resource scripts | PascalCase, acronyms treated as words | `CpdlcSettingsDialog.cpp`, `DatalinkDialog.hpp`, `PluginResources.rc` |
| Split C++ implementation units | Owner.Feature | `RadarScreen.AvisoRendering.cpp` |
| C++ headers | `.hpp`; Windows resource IDs retain `.h` | `ResourceIds.h` |
| JavaScript, CSS | lowercase kebab-case | `app-editor-actions.js`, `control-center-browser-tests.js` |
| PowerShell, Python | lowercase snake_case, action first | `build_project.ps1`, `test_release_inputs.ps1` |
| Source JSON, WAV, cursors, patches | descriptive lowercase snake_case | `profile_templates.json`, `timer_alarm.wav`, `move_tag.cur`, `rdf_smr_ground_view.patch` |
| AVISO maps and aircraft icons | existing lookup identifiers | `LFPG.geojson`, `a320.png` |
| MSBuild projects and public binaries | retain the vSMR product name | `vSMR.vcxproj`, `vSMR.Runtime.dll` |

Conventional entry points (`index.html`, `app.js`, `styles.css`), generated bundles,
repository metadata (`README.md`, `CHANGELOG.md`, `LICENSE`, `appveyor.yml`),
third-party files, license notices and historical changelog references retain their names.
Folders remain grouped by subsystem; this is a file-naming convention, not a directory migration.

## Installed-name compatibility

`vSMR/DistributionAssets.props` is the shared source-to-install mapping used by
MSBuild and the update-feed generator. Source assets have descriptive names;
installed names remain stable because existing loaders validate a closed set of paths.

| Source under `vSMR/data/` | Installed under `vSMR_Data/` |
| --- | --- |
| `aircraft_types.json` | `ICAO_Aircraft.json` |
| `holding_points.json` | `airports_hp.json` |
| `profile_templates.json` | `vSMR_Profiles.json` |
| `aviso_update_policy.json` | `AVISO-UPDATE-POLICY.json` |
| `Audio/timer_alarm.wav` | `Audio/Alarm.wav` |
| `Audio/cpdlc_notification.wav` | `Audio/Ding.wav` |

The legacy profile template is also copied as `vSMR_webUI/defaults/vSMR_Profiles.json`.
It remains excluded from the managed update feed. Migration fingerprint keys,
`default.json`, `config.json`, release manifest names, exported symbols and runtime
lookup paths are compatibility contracts, not cosmetic rename targets.
No user file is renamed by this source refactor.

After a rename, update includes, project/filter files, resource scripts, generators,
tests and packaging references together. Run `vSMR/tools/build_project.ps1` to
rebuild and run regressions, including the source/install mapping checks.
