#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '', [switch]$Check)
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '..\..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$source = Join-Path $RepositoryRoot 'vSMR/data/profile_templates.json'
$destination = Join-Path $RepositoryRoot 'vSMR/data/default.json'
$profiles = [ordered]@{}
$metadata = [pscustomobject]@{}
$knownIds = @{
    'Default' = 'builtin-default'; 'LFPG' = 'builtin-lfpg'; 'LFMN' = 'builtin-lfmn'
    'LFBO / LFLL' = 'builtin-lfbo-lfll'; 'Custom 1' = 'builtin-custom-1'; 'Custom 2' = 'builtin-custom-2'
}
$legacyProfiles = Get-Content -LiteralPath $source -Raw | ConvertFrom-Json
foreach ($profile in $legacyProfiles) {
    if ($profile.PSObject.Properties['_vsmr']) { $metadata = $profile._vsmr; continue }
    if (-not $profile.name) { throw 'Bundled profile is missing its display name.' }
    $id = $knownIds[[string]$profile.name]
    if (-not $id) {
        $slug = ([string]$profile.name).ToLowerInvariant() -replace '[^a-z0-9]+', '-'
        $slug = $slug.Trim('-')
        if (-not $slug) { throw "Cannot derive a stable ID for '$($profile.name)'." }
        $id = 'builtin-' + $slug
    }
    if ($profiles.Contains($id)) { throw "Duplicate stable profile ID '$id'." }
    # IDs remain separate from editable names and are not embedded in defaults.
    $profile.PSObject.Properties.Remove('_vsmr_profile_id')
    $profiles.Add($id, $profile)
}
if (-not $profiles.Contains('builtin-default')) { throw 'The Default profile is required.' }
$assetHashes = [ordered]@{}
# Migration fingerprint only: the editable legacy profile file is never an
# update target. A pristine bridge install can adopt defaults without pinning
# every old value as an apparent user change.
$assetHashes.Add('vSMR_Profiles.json', (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant())
foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'vSMR/data/AVISO') -Filter '*.geojson' -File | Sort-Object Name)) {
    if ($file.Name -notmatch '^[A-Z0-9]{4}(?:_[A-Za-z0-9][A-Za-z0-9_-]{0,47})?\.geojson$') { continue }
    $assetHashes.Add('AVISO/' + $file.Name, (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant())
}
$runwayVisibility = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/data/runway_group_visibility.json') -Raw | ConvertFrom-Json
$document = [ordered]@{ schema_version = 1; profiles = $profiles; metadata = $metadata; asset_hashes = $assetHashes; runway_group_visibility = $runwayVisibility }
$serialized = ($document | ConvertTo-Json -Depth 100 -Compress) + "`n"
if ($Check) {
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or
        [IO.File]::ReadAllText($destination) -cne $serialized) {
        throw 'default.json is stale. Run vSMR/tools/build_config_defaults.ps1.'
    }
} else {
    [IO.File]::WriteAllText($destination, $serialized, [Text.UTF8Encoding]::new($false))
}
Write-Host "Layered defaults verified: $($profiles.Count) stable profile IDs."
