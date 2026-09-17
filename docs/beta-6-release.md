# Beta 6 release checklist

Target: `2.0.0-beta.6`. This is a prepared development candidate, not a published release.
The loader remains `1.1.0` / runtime ABI `1`; its product version is beta 6.

## Automated validation (2026-09-17)

- [x] Full Release/Win32 rebuild with local MSVC v145.
- [x] Native regression suite, including every bundled AVISO and current LFPG expectations.
- [x] Control Center browser regressions, generated bundles, CSS ownership, and JSON-entry-point audits.
- [x] Native regression suite under AddressSanitizer.
- [x] JSON sanitizer smoke test: 10,012 inputs.
- [x] Paris vSID configuration migration: seven Python tests.
- [x] Release-input validation: aligned source/resource/packager/CI versions; 160 exact map hashes; 237 obsolete maps in the update policy, with no bundled-map overlap.
- [x] Release-input negative tests reject altered maps, count mismatches, bundled-map deletion, missing deletion entries, and stale loader product metadata.
- [x] Production packaging still rejects the five unresolved asset-provenance groups.
- [x] Local validation ZIP and private symbols created; archive metadata is explicitly non-publishable and all 160 archived map hashes match the import manifest.

The [current map manifest](aviso-set-20260917.json) records the final import.
No converter-supplied map was edited for release preparation. LFPG has 1,468
features and no optional groups. LFRJ is bundled and is not scheduled for deletion.
The updater retains `modified_files: protect_setting`; it must respect users'
configured protection of modified maps.

## Required before publication

- [ ] Resolve the five groups in the [asset provenance register](../vSMR/data/Licenses/ASSET_PROVENANCE.md): aircraft icons, timer sound, CPDLC sound, AVISO geometry, and aircraft dimensions. Supply attribution and redistribution evidence; do not simply mark them verified.
- [ ] Provision a code-signing certificate and configure `VSMR_SIGNING_CERT_THUMBPRINT` plus its matching `VSMR_UPDATE_SIGNER_CERT_SHA256`. No signing material was available during local preparation. Never commit private keys or passwords.
- [ ] In EuroScope, test a complete matching installation: loader/runtime startup, `.smr diagnostics`, two independent ASRs, AVISO palettes, and resolution changes at 1080p/2K/4K. Verify that AVISO rendering and aircraft icons/tags scale while menus and inset controls keep their size.
- [ ] Exercise optional bridge providers and manual Paris controls with the matching companion builds.
- [ ] Test upgrade and rollback on a backed-up installation, including obsolete maps, bundled LFRJ, and protection of a locally modified map. Local automated checks do not replace this live test.
- [ ] Review and commit the preparation changes. Build from a clean checkout; leave unrelated local editor settings out of the release commit.
- [ ] Merge the reviewed candidate into the protected release branch (`main` or `master`) and pass AppVeyor's v143 build/static-analysis/test gates. A local v145 build does not certify that CI run.
- [ ] Set the actual release date in CHANGELOG, refresh the Wiki's release-readiness status, and tag the final protected-branch commit only after approval.
- [ ] Build the production package with the signing environment configured and without validation bypass switches. Verify binary signatures, detached update-manifest signature, metadata, checksums, and matching symbols before publishing.

## Local commands

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\build_project.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tests\run_tests.ps1
python -B .\vSMR\tests\test_vsid_paris_config.py
```

Production packaging defaults to beta 6 and fails closed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\vSMR\tools\create_release_package.ps1 -Version 2.0.0-beta.6
```

`-ForceNonPublishable` / `-AllowDirtySource` are only for local validation.
Any resulting validation archive has `publishable: false`, is not an update
release, and must not be distributed while asset provenance remains unresolved.
No release tag or public release was created during preparation.
