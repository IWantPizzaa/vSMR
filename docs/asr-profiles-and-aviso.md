# ASR profile selection and AVISO replacement

Each radar screen owns its active profile. For example, select Custom LFPG in the LFPG ASR and Default in the LFPO ASR; both selections remain active independently. The Runtime Menu and Control Center use the same screen-local selection.

The `ActiveProfile` ASR key saves and restores that screen's choice. Missing or deleted profiles fall back to Default, or the first available profile if Default is absent. The legacy `last_active_profile` configuration metadata no longer controls ASR selection. A previously saved incorrect choice must be changed and the ASR saved once.

Profile definitions and the chosen profiles-file source remain shared. Reloading definitions or choosing another profiles file preserves each ASR's profile by name where possible. Saving or closing another ASR does not overwrite a screen's selection.

## Imported AVISO set

Beta 6 uses exactly the 160 `.geojson` files supplied in the vSMR AVISO Converter's `GeoJSON` folder on 2026-09-17. Compared with the previous import, LFRJ was added and 33 maps were removed. The update policy covers all 237 obsolete maps across these imports, excludes every currently bundled map, and retains its existing modified-file protection setting.

[Current source hashes](aviso-set-20260917.json) record every supplied file; all 160 maps retain their supplied bytes without corrections. The [2026-09-15 manifest](aviso-set-20260915.json) is historical and does not describe the release set. `verify_release_inputs.ps1` checks the current hashes, file count, release versions, and update policy during tests and packaging.

All maps provide Dark and Light palettes. Real is available at LFML, LFMN, LFPG, and LFPO. The supplied LFPG map has 1,468 features and no optional groups; it does not contain the earlier East/West arrow controls. Native assertions validate this final data set rather than restoring removed geometry.

Native validation loads every bundled map, checks geometry and input limits, and exercises independent profile selection, configuration reloads, reopening saved choices and missing-profile fallback. Live EuroScope verification should open two ASRs, select different profiles, save them, and reopen them in the opposite order.
