#requires -Version 5.1
<#
Read-only binary/SDK compatibility check. Does not load or execute EuroScope,
install anything, connect to VATSIM, or certify live-session behavior.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EuroScopeDirectory,
    [string]$RepositoryRoot = '',
    [string]$RuntimePath = '',
    [string]$SdkHeaderPath = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '../..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not $RuntimePath) { $RuntimePath = Join-Path $RepositoryRoot 'vSMR/bin/Release/Runtime/vSMR.Runtime.dll' }
$sdkDll = Join-Path $EuroScopeDirectory 'EuroScopePlugInDll.dll'
$hostExe = Join-Path $EuroScopeDirectory 'EuroScope.exe'
foreach ($path in @($RuntimePath, $sdkDll, $hostExe)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required binary not found: $path" }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio C++ tools are required for this inspection.' }
$dumpbin = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC/*/bin/Hostx64/x86/dumpbin.exe') |
    Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
if (-not $dumpbin) { throw 'dumpbin.exe was not found.' }
function Inspect-Binary([string]$Option, [string]$Path) {
    $output = @(& $dumpbin /nologo $Option $Path)
    if ($LASTEXITCODE -ne 0) { throw "Binary inspection failed: $Path" }
    return $output
}
foreach ($path in @($RuntimePath, $sdkDll, $hostExe)) {
    $headers = (Inspect-Binary '/headers' $path) -join "`n"
    if ($headers -notmatch '(?im)^\s*14C machine \(x86\)\s*$') { throw "Expected Win32/x86 binary: $path" }
}
$imports = @(Inspect-Binary '/imports:EuroScopePlugInDll.dll' $RuntimePath)
$required = @($imports | ForEach-Object {
    if ($_ -match '^\s+[0-9A-Fa-f]+\s+(\?\S+)\s*$') { $Matches[1] }
})
if (-not $required.Count -or ($imports -join "`n") -match '(?i)Ordinal\s+\d+') {
    throw 'Expected named EuroScope SDK imports; inspect the import table manually.'
}
$exports = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($line in (Inspect-Binary '/exports' $sdkDll)) {
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { [void]$exports.Add($Matches[1]) }
}
$missing = @($required | Where-Object { -not $exports.Contains($_) })
if ($missing.Count) { throw "Host SDK is missing required imports:`n$($missing -join "`n")" }

if (-not $SdkHeaderPath) {
    $candidate = Join-Path $EuroScopeDirectory 'EuroScopePlugIn.h'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $SdkHeaderPath = $candidate }
}
if ($SdkHeaderPath) {
    function Normalize-Sdk([string]$Path) {
        $text = Get-Content -LiteralPath $Path -Raw
        if ($text -notmatch 'COMPATIBILITY_CODE\s*=\s*16\s*;') { throw "Unexpected SDK compatibility code: $Path" }
        $text = [regex]::Replace($text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
        # 3.2.13 adds a menu constant, not a vtable entry or class member.
        $text = [regex]::Replace($text, 'const\s+int\s+TAG_ITEM_FUNCTION_SET_GROUND_STATUS_ADVANCED\s*=\s*46\s*;', '')
        return [regex]::Replace($text, '\s+', '')
    }
    if ((Normalize-Sdk $SdkHeaderPath) -cne (Normalize-Sdk (Join-Path $RepositoryRoot 'lib/include/EuroScopePlugIn.h'))) {
        throw 'SDK declarations changed beyond the reviewed additive constant/comments. Review the ABI before claiming compatibility.'
    }
    Write-Host 'SDK declarations verified: compatibility code 16 and unchanged class interfaces.'
} else {
    Write-Warning 'No SDK header supplied: class/vtable layout was not checked for this installation.'
}
$hostInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $hostExe).Path)
$sdkInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $sdkDll).Path)
Write-Host "EuroScope $($hostInfo.FileVersion), SDK DLL $($sdkInfo.FileVersion): all $($required.Count) runtime SDK imports resolve by name (x86)."
Write-Host "SDK DLL SHA-256: $((Get-FileHash -LiteralPath $sdkDll -Algorithm SHA256).Hash)"
Write-Host 'Binary inspection passed. Live load/render/interaction/unload and performance tests remain required.'
