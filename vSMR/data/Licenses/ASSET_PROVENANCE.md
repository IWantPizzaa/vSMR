# Bundled asset provenance register

This register prevents bundled assets from being mistaken for original vSMR
code or automatically covered by the project's GPL license. It is deliberately
conservative: an entry marked **verification required** must be resolved by the
release owner before a public production release.

`create_release_package.ps1` enforces this register: a publishable package is
refused while any entry remains unresolved. `-ForceNonPublishable` exists only
for local validation and must not be used to distribute those assets.
The release-status column accepts `Project license`, `License verified`,
`Permission documented`, `Public domain`, `Original asset`, or
`Verification required`. Missing, malformed, empty, or unknown entries also
stop publishable packaging.

| Asset group | Packaged path | Current provenance record | Release status |
| --- | --- | --- | --- |
| Aircraft silhouettes | `aircraft_icons/*.png` | Imported from the historical vSMR data set; individual creator and redistribution terms are not recorded in this repository | Verification required |
| Timer alarm | `Audio/Alarm.wav` | Restored from the historical ESTimers installation; creator and redistribution terms are not recorded in this repository | Verification required |
| CPDLC notification | `Audio/Ding.wav` | Added from the historical/project working data; creator and redistribution terms are not recorded in this repository | Verification required |
| AVISO airport geometry | `AVISO/<ICAO>.geojson` | Derived from project-maintained airport display data; source and contributor attribution are not recorded per airport | Verification required |
| Aircraft dimensions | `ICAO_Aircraft.json` | Normalized from the project aircraft database; upstream data source and terms are not recorded | Verification required |
| Control Center UI | `src/control_center/web/*` (packaged as `vSMR_webUI/*`) | Maintained as part of this repository | Project license |

For each unresolved group, record the source URL or contributor, retrieval or
creation date, applicable license/permission, and any required attribution.
Do not remove this notice merely because a file can be downloaded publicly.
