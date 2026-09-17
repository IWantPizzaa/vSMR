#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
function Read-Source([string]$Path) {
    return [IO.File]::ReadAllText((Join-Path $RepositoryRoot $Path))
}
function Assert-Contains([string]$Text, [string]$Expected, [string]$Context) {
    if (-not $Text.Contains($Expected)) { throw "$Context must contain '$Expected'." }
}

$metadata = Read-Source "vSMR/src/plugin/PluginMetadata.hpp"
if ($metadata -notmatch 'VsmrPluginVersion\[\]\s*=\s*"v(\d+\.\d+\.\d+-beta\.\d+)"') {
    throw "Cannot read the beta release version from PluginMetadata.hpp."
}
$sourceVersion = $Matches[1]
if ($Version -and $Version -ne $sourceVersion) {
    throw "Requested version $Version does not match source version $sourceVersion."
}
$Version = $sourceVersion
$numericVersion = $Version.Replace('-beta.', '.').Replace('.', ',')
foreach ($path in @('vSMR/resources/vSMR.rc', 'vSMR/src/crash/handler/vSMRCrashHandler.rc')) {
    $resource = Read-Source $path
    foreach ($key in @('FILEVERSION', 'PRODUCTVERSION')) {
        Assert-Contains $resource "$key $numericVersion" $path
    }
    foreach ($key in @('FileVersion', 'ProductVersion')) {
        Assert-Contains $resource ('VALUE "' + $key + '", "' + $Version + '"') $path
    }
}
$loader = Read-Source 'vSMR/src/bootstrap/loader/LoaderResources.rc'
Assert-Contains $loader "PRODUCTVERSION $numericVersion" 'Loader product version'
Assert-Contains $loader ('VALUE "ProductVersion", "' + $Version + '"') 'Loader product version'
# Loader FileVersion is independently versioned; beta 6 does not change its ABI.
$packager = Read-Source 'vSMR/tools/create_release_package.ps1'
Assert-Contains $packager ('[string]$Version = "' + $Version + '"') 'Packager default'
$ci = Read-Source 'appveyor.yml'
Assert-Contains $ci "version: $Version.{build}" 'CI version'
Assert-Contains $ci "VSMR_RELEASE_VERSION: $Version" 'CI release version'
Assert-Contains $ci ('/^v?' + $Version.Replace('.', '[.]') + '$/') 'CI tag filter'
foreach ($suffix in @('.zip', '.update.json', '-symbols.zip', '-*-validation-only.zip', '-*-validation-only-symbols.zip')) {
    Assert-Contains $ci "artifacts\vSMR-$Version$suffix" 'CI artifact path'
}

$manifest = Read-Source 'docs/aviso-set-20260917.json' | ConvertFrom-Json
$policy = Read-Source 'vSMR/data/AVISO-UPDATE-POLICY.json' | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or $manifest.release -ne $Version -or
    $policy.schema_version -ne 1 -or $policy.release -ne $Version) {
    throw 'AVISO inventory and update policy must target the source release.'
}
$expected = @($manifest.files.PSObject.Properties)
$avisoRoot = Join-Path $RepositoryRoot 'vSMR/data/AVISO'
$actual = @(Get-ChildItem -LiteralPath $avisoRoot -Filter '*.geojson' -File)
if ($expected.Count -ne $manifest.file_count -or $actual.Count -ne $expected.Count -or $expected.Count -eq 0) {
    throw 'Bundled AVISO file count differs from the reviewed import manifest.'
}
foreach ($file in $expected) {
    if ($file.Name -notmatch '^[A-Z0-9]{4}\.geojson$' -or $file.Value -notmatch '^[0-9a-f]{64}$') {
        throw 'Invalid AVISO filename or SHA-256 in the import manifest.'
    }
    $path = Join-Path $avisoRoot $file.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Value) {
        throw "Bundled AVISO differs from the reviewed import: $($file.Name)."
    }
}
$deleted = @($policy.aviso.delete)
if ($policy.aviso.update -ne 'all' -or @($policy.aviso.replace).Count -ne 0 -or
    $policy.aviso.modified_files -ne 'protect_setting' -or
    @($deleted | Sort-Object -Unique).Count -ne $deleted.Count) {
    throw 'Unexpected AVISO update semantics or duplicate deletion entries.'
}
foreach ($name in $deleted) {
    if ($name -notmatch '^[A-Z0-9_]+\.geojson$' -or $name -in $actual.Name) {
        throw "Unsafe or still-bundled AVISO deletion entry: $name."
    }
}
foreach ($name in $manifest.removed_since_previous_import) {
    if ($name -notin $deleted) { throw "Removed airport missing from update policy: $name." }
}
Write-Host "Release inputs verified: $Version, $($actual.Count) exact AVISO maps, $($deleted.Count) obsolete maps scheduled for removal."
