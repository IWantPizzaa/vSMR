#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '', [string]$BuildOutputDirectory = '')
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '../..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not $BuildOutputDirectory) { $BuildOutputDirectory = Join-Path $RepositoryRoot 'Release' }
$distributionData = Join-Path ([IO.Path]::GetFullPath($BuildOutputDirectory)) 'vSMR_Data'
[xml]$mapping = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/DistributionAssets.props') -Raw
$sources = @{}
$destinations = @{}
foreach ($entry in $mapping.SelectNodes("//*[local-name()='VsmrRenamedData']")) {
    $source = [string]$entry.SourceName
    $installed = [string]$entry.InstallName
    if ($sources.ContainsKey($source) -or $destinations.ContainsKey($installed)) { throw 'Duplicate distribution alias.' }
    if ($source -notmatch '^(Audio/)?[a-z][a-z0-9_]*\.(json|wav)$' -or
        $installed -notmatch '^(Audio/)?[A-Za-z0-9_.-]+\.(json|wav)$' -or $installed.Contains('..')) {
        throw "Invalid distribution alias: $source -> $installed"
    }
    $sources[$source] = $true
    $destinations[$installed] = $true
    if ($entry.Include -cne ('$(MSBuildThisFileDirectory)data\' + $source.Replace('/', '\'))) {
        throw "MSBuild source and feed mapping disagree for $source"
    }
    $sourcePath = Join-Path $RepositoryRoot ('vSMR/data/' + $source)
    $installedPath = Join-Path $distributionData $installed
    if ((Get-FileHash -LiteralPath $sourcePath).Hash -ne (Get-FileHash -LiteralPath $installedPath).Hash) {
        throw "Installed alias differs from its source: $installed"
    }
    if (Test-Path -LiteralPath (Join-Path $distributionData $source)) {
        throw "Source-only name leaked into the distribution: $source"
    }
}
if ($sources.Count -ne 5) { throw 'Expected five compatibility aliases.' }
if ((Get-FileHash -LiteralPath (Join-Path $RepositoryRoot 'vSMR/data/profile_templates.json')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $distributionData 'vSMR_webUI/defaults/vSMR_Profiles.json')).Hash) {
    throw 'The Control Center legacy defaults alias is stale.'
}
Write-Host 'Distribution assets verified: renamed sources retain byte-identical installed aliases.'
