#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '../..' }
$files = @(& git -C $RepositoryRoot ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate project files.' }
$checked = 0
foreach ($file in $files) {
    # Third-party files/notices and identifier-based map/icon names are contracts.
    if ($file -match '^(lib/|vSMR/data/Licenses/|vSMR/data/AVISO/|vSMR/data/aircraft_icons/)' -or
        $file -notmatch '\.(cpp|hpp|cs|rc|ps1|py|js|css|cur|json|wav|patch)$') { continue }
    $name = Split-Path $file -Leaf
    $pattern = if ($name -match '\.(cpp|hpp|cs|rc)$') {
        '^[A-Z][A-Za-z0-9]*(\.[A-Z][A-Za-z0-9]*)*\.[a-z]+$'
    } elseif ($name -match '\.(js|css)$') {
        '^[a-z][a-z0-9]*(-[a-z0-9]+)*\.[a-z]+$'
    } else {
        '^[a-z][a-z0-9]*(_[a-z0-9]+)*\.[a-z0-9]+$'
    }
    if ($name -cnotmatch $pattern) { throw "File naming does not match README.md: $file" }
    ++$checked
}
Write-Host "File naming verified: $checked project-owned source and asset files."
