# ASR profile selection and AVISO replacement

Each radar screen owns its active profile. For example, select Custom LFPG in the LFPG ASR and Default in the LFPO ASR; both selections remain active independently. The Runtime Menu and Control Center use the same screen-local selection.

The `ActiveProfile` ASR key saves and restores that screen's choice. Missing or deleted profiles fall back to Default, or the first available profile if Default is absent. The legacy `last_active_profile` configuration metadata no longer controls ASR selection. A previously saved incorrect choice must be changed and the ASR saved once.

Profile definitions and the chosen profiles-file source remain shared. Reloading definitions or choosing another profiles file preserves each ASR's profile by name where possible. Saving or closing another ASR does not overwrite a screen's selection.

## Imported AVISO set

The requested `AVISO.zip` was unavailable. The replacement uses the 192 `.geojson` files in the named converter's `GeoJSON` folder, imported on 2026-09-15. The 203 former maps absent from this set were removed; the update policy includes their deletion while retaining its existing modified-file protection setting.

[Source hashes and corrections](aviso-set-20260915.json) record every supplied file. LFLG contained one single-coordinate LineString, feature `LFLG-7eb871d1efb00b9af547`, which cannot form a line and prevented the complete airport map from loading. Only that feature was removed. The other 191 maps retain their supplied bytes.

The set includes LFPO Real colors and 89 East / 97 West features in LFPG's configuration groups. LFJD, LFJE and LFQI retain their supported legacy night/day palette names.

Native validation loads every bundled map, checks geometry and input limits, and exercises independent profile selection, configuration reloads, reopening saved choices and missing-profile fallback. Live EuroScope verification should open two ASRs, select different profiles, save them, and reopen them in the opposite order.
