#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path $RepositoryRoot 'vSMR\src'
$approved = @('shared\JsonDocument.hpp', 'shared\JsonInputLimits.hpp', 'control_center\WebMessageValidation.cpp')
foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
    if ($file.Extension -notin @('.cpp', '.hpp')) { continue }
    $relative = $file.FullName.Substring($sourceRoot.TrimEnd('\').Length + 1)
    if ($relative -in $approved) { continue }
    if ([System.IO.File]::ReadAllText($file.FullName) -match '\.\s*Parse(?:Stream|Insitu)?\s*[<(]') {
        throw "JSON parsing must use shared/JsonDocument.hpp: $relative"
    }
}
Write-Host 'All production JSON entry points use bounded validation'
