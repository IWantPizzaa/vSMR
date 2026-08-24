# Publishing vSMR updates

Every release package contains a publisher-owned AVISO policy and a generated inventory. Together they let a release update exactly the maps it intends to change while keeping local edits safe by default.

## Choose the AVISO policy

Before packaging a release, edit `vSMR\data\AVISO-UPDATE-POLICY.json` and set its `release` to the exact release version:

```json
{
  "schema_version": 1,
  "release": "2.0.0-beta.4",
  "aviso": {
    "update": "all",
    "replace": [],
    "delete": ["LFMM.geojson", "LFPG_Dyna_fixed.geojson"],
    "modified_files": "replace"
  }
}
```

`aviso.update` controls which bundled maps are copied from the release:

| Value | Behavior | `replace` |
| --- | --- | --- |
| `none` | Do not change any installed AVISO map. This is the normal default for code-only releases. | Must be empty. |
| `selected` | Update only the named bundled maps. | List canonical names such as `LFPG.geojson`. |
| `all` | Update every AVISO map bundled in the release. | Must be empty. |

`aviso.delete` lists obsolete canonical map files to remove. A deleted name must not still exist in `vSMR\data\AVISO`. Unknown custom airport filenames are not deleted.

Both `replace` and `delete` are mandatory JSON arrays, including when they are empty. Filenames are compared case-insensitively, and one name cannot be listed twice or appear in both arrays.

`aviso.modified_files` controls what happens when an installed map differs from the official hash recorded by the previously installed release:

| Value | Behavior |
| --- | --- |
| `preserve` | Always keep a locally modified file. The new official copy is placed in `vSMR_Data\AVISO_Updates\<version>\`. |
| `protect_setting` | Respect the user's default-on `Protect locally modified AVISOs` updater option. If protection is off, replace the modified file. |
| `replace` | Mandatory migration: replace even locally modified files. Use only when older data is incompatible. |

The protection toggle intentionally cannot override `replace`. A mandatory migration is still recoverable because the installer creates a complete pre-update backup before swapping any files.

### Safe recipes

Most releases should use no AVISO changes:

```json
"aviso": {
  "update": "none",
  "replace": [],
  "delete": [],
  "modified_files": "protect_setting"
}
```

To publish changes only to LFPG and LFML while protecting local edits:

```json
"aviso": {
  "update": "selected",
  "replace": ["LFPG.geojson", "LFML.geojson"],
  "delete": [],
  "modified_files": "protect_setting"
}
```

Beta 4 uses `all` plus `replace` because every bundled GeoJSON must acquire compatible Night/Day palette data. It also deletes `LFMM.geojson` and `LFPG_Dyna_fixed.geojson`. Do not copy that policy into an ordinary release.

## Mandatory beta.3 migration

Beta.4 raises both the packaged and minimum bootstrap-loader version from `1.0.0` to `1.1.0`. A beta.3 installation must therefore be upgraded once with the complete beta.4 ZIP while EuroScope is closed. The updater and transactional installer deliberately refuse to preserve loader `1.0.0`; do not use `-PreserveLoader` for this migration. This boundary also prevents beta.4 from inheriting the beta.3 loader's obsolete release-verification behavior.

## Inventory and install results

`package_release.ps1` validates the policy and generates `vSMR_Data\AVISO-INVENTORY.json` inside the staged package. The inventory contains the SHA-256 of every official bundled `.geojson`; do not create or edit it by hand.

After installation, `vSMR_Data\AVISO-UPDATE-REPORT.json` lists maps that were added, updated, preserved, or deleted. When a modified map is protected, its incoming official copy is retained under `vSMR_Data\AVISO_Updates\<version>\` for review. The installer writes an effective inventory: it adopts the new official hash only for a map actually installed, keeps the previous official hash for a preserved or unselected map, and omits unknown custom files. Future releases can therefore continue to recognize a protected active map as locally modified.

## Manual AVISO reload

The Control Center's automatic-update settings include `Reload AVISOs` and `Protect locally modified AVISOs`.

Reload queues a next-startup action. The stable loader downloads the exact installed version from GitHub Releases, validates the signed update manifest and complete package, then asks the transactional installer to reapply all bundled maps. It does not download mutable files from the repository's `main` branch and does not require a newer vSMR version.

With protection enabled, modified maps remain active and official copies go to `AVISO_Updates`. With protection disabled, all bundled maps return to the installed release's official versions. Complete installation backup and rollback behavior is unchanged.

## Release checklist

1. Update every version declaration and the changelog.
2. Choose an AVISO policy deliberately; do not leave the previous release's `release` value or migration mode in place.
3. Remove every filename in `aviso.delete` from the bundled `vSMR\data\AVISO` directory.
4. Normalize and validate runtime data.
5. Commit all intended changes and confirm the worktree is clean for a publishable build.
6. Create the exact version tag at the release commit.
7. Run `vSMR\tools\create_github_release.ps1 -Version <version>`. For beta.4, also pass `-PublishedBeta3ArchivePath <path-to-vSMR-2.0.0-beta.3.zip>`. The driver pins that published archive to SHA-256 `348dc981253badabd4a697ff2acfe2536be6af6beb31fca20c694fdaa48c6905` and internal source commit `385b8ee4742eb6cccfbf9b96d17932abba204638`; it fails unless the tag, clean commit, trusted certificate, embedded pin, binary signatures, detached manifest signature, real and synthetic migration fixtures, and final three-asset contract all verify.
8. Upload only the ZIP, update manifest, and detached manifest signature printed by the release driver. Use only assets from the same run; the private symbols ZIP is not a public release asset.

Validation rejects unsafe paths, duplicate or contradictory policy names, selected maps missing from the package, deleted maps still bundled, an inventory that does not cover the package exactly, and stale inventory hashes.
