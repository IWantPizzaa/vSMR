# Bundled asset provenance register

This register prevents bundled assets from being mistaken for original vSMR
code or automatically covered by the project's GPL license. It is deliberately
conservative: an entry marked **verification required** must be resolved by the
release owner before a public production release.

| Asset group | Packaged path | Current provenance record | Release status |
| --- | --- | --- | --- |
| Aircraft silhouettes | `aircraft_icons/*.png` | Imported from the historical vSMR data set; individual creator and redistribution terms are not recorded in this repository | Verification required |
| Timer alarm | `Audio/Alarm.wav` | Added from the historical/project working data; creator and redistribution terms are not recorded in this repository | Verification required |
| CPDLC notification | `Audio/Ding.wav` | Added from the historical/project working data; creator and redistribution terms are not recorded in this repository | Verification required |
| AVISO airport geometry | `AVISO/<ICAO>.geojson`, `AVISO/LFPG_Dyna_fixed.geojson` | Derived from project-maintained airport display and dynamic ownership data; source and contributor attribution are not recorded per airport | Verification required |
| Aircraft dimensions | `ICAO_Aircraft.json` | Normalized from the project aircraft database; upstream data source and terms are not recorded | Verification required |
| Control Center UI | `src/control_center/web/*` (packaged as `vSMR_webUI/*`) | Maintained as part of this repository | Project license |

For each unresolved group, record the source URL or contributor, retrieval or
creation date, applicable license/permission, and any required attribution.
Do not remove this notice merely because a file can be downloaded publicly.
