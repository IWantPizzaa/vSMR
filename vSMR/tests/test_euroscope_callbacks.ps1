#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '../..' }
$count = 0
foreach ($component in @(
    @{ Directory = 'plugin'; Class = 'CSMRPlugin'; Header = 'Plugin.hpp' },
    @{ Directory = 'radar'; Class = 'CSMRRadar'; Header = 'RadarScreen.hpp' }
)) {
    $directory = Join-Path $RepositoryRoot ('vSMR/src/' + $component.Directory)
    $header = Get-Content -LiteralPath (Join-Path $directory $component.Header) -Raw
    $sources = (Get-ChildItem -LiteralPath $directory -Filter '*.cpp' | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Raw
    }) -join "`n"
    $declarations = [regex]::Matches($header, '(?m)^\s*(?:virtual\s+)?(?:void|bool|CRadarScreen\s*\*)\s*(On\w+)\s*\([^;{}]*\)\s*(?:override)?\s*;')
    foreach ($declaration in $declarations) {
        # Ignore vSMR's own dialog notification, not an SDK virtual callback.
        if ($declaration.Value -notmatch '\bvirtual\b|\boverride\b') { continue }
        $name = $component.Class + '::' + $declaration.Groups[1].Value
        $definition = [regex]::Match($sources, [regex]::Escape($name) + '\s*\([^{}]*\)\s*\{\s*(?<first>[^\r\n]+)')
        if (-not $definition.Success -or
            $definition.Groups['first'].Value.Trim() -cne 'AFX_MANAGE_STATE(AfxGetStaticModuleState());') {
            throw "SDK callback must enter vSMR's MFC module state before using resources/handle maps: $name"
        }
        ++$count
    }
}
if ($count -lt 25) { throw "Callback audit unexpectedly covered only $count entry points." }
$refresh = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/src/radar/RadarScreen.Refresh.cpp') -Raw
$refreshBody = [regex]::Match($refresh, 'void CSMRRadar::OnRefresh\([^)]*\)\s*\{(?<body>[\s\S]*)').Groups['body'].Value
$refreshBody = [regex]::Replace($refreshBody, '(?m)//[^\r\n]*', '')
if ($refreshBody -notmatch '^\s*AFX_MANAGE_STATE\(AfxGetStaticModuleState\(\)\);\s*if\s*\(Phase == REFRESH_PHASE_BACK_BITMAP \|\| hDC == nullptr\)\s*return;') {
    throw 'Background/null-DC refreshes must return before touching radar state or the host DC.'
}
Write-Host "EuroScope callback boundaries verified: $count entry points use vSMR's MFC module state."
