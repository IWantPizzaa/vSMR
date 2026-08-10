#requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory,
    [string]$BackupRoot = "",
    [switch]$ReplaceUserData
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Resolve-NonRootDirectoryPath([string]$Path, [string]$Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Description cannot be empty." }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
    if ($fullPath.TrimEnd('\', '/').Equals($pathRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description cannot be a filesystem root: $fullPath"
    }
    return $fullPath.TrimEnd('\', '/')
}

$PackageRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$PackageData = Join-Path $PackageRoot "vSMR_Data"
$PackageDll = Join-Path $PackageRoot "vSMR.dll"
$DestinationDirectory = Resolve-NonRootDirectoryPath $DestinationDirectory "DestinationDirectory"

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: $Path" }
}

function Assert-ChildPath([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if (-not $resolvedPath.StartsWith($resolvedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path '$resolvedPath' is outside '$resolvedParent'."
    }
}

function Test-PathEqualOrChild([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $resolvedPath.Equals($resolvedParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    [System.IO.Directory]::CreateDirectory($Destination) | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath $Source -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
    }
}

$destinationDll = Join-Path $DestinationDirectory "vSMR.dll"
$destinationData = Join-Path $DestinationDirectory "vSMR_Data"
if (Test-PathEqualOrChild $DestinationDirectory $PackageRoot) {
    throw "DestinationDirectory cannot be the extracted package or a directory below it."
}
if (Test-PathEqualOrChild $PackageRoot $destinationData) {
    throw "The extracted package cannot be inside the vSMR_Data tree being replaced."
}

Assert-File $PackageDll
Assert-File (Join-Path $PackageData "RELEASE-METADATA.json")
Assert-File (Join-Path $PackageData "SHA256SUMS.txt")
if (-not (Test-Path -LiteralPath $DestinationDirectory -PathType Container)) {
    throw "Destination plugin directory does not exist: $DestinationDirectory"
}
# Validate every packaged payload byte before copying it into EuroScope.
$manifestEntries = @{}
foreach ($line in @(Get-Content -LiteralPath (Join-Path $PackageData "SHA256SUMS.txt"))) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "The package SHA256SUMS.txt is invalid." }
    $relative = $Matches[2].Replace('/', '\')
    if ([System.IO.Path]::IsPathRooted($relative) -or @($relative.Split('\') | Where-Object { $_ -eq '..' }).Count -gt 0) {
        throw "Unsafe package manifest path: $relative"
    }
    if ($manifestEntries.ContainsKey($relative)) { throw "Duplicate package manifest path: $relative" }
    $manifestEntries[$relative] = $Matches[1]
}
foreach ($relative in @($manifestEntries.Keys)) {
    $path = Join-Path $PackageRoot $relative
    Assert-File $path
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $manifestEntries[$relative]) { throw "Package hash mismatch: $relative" }
}
$unlisted = @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Where-Object {
    $_.FullName -ne (Join-Path $PackageData "SHA256SUMS.txt") -and
    -not $manifestEntries.ContainsKey($_.FullName.Substring($PackageRoot.Length).TrimStart([char[]]"\/").Replace('/', '\'))
})
if ($unlisted.Count -gt 0) { throw "Package contains unlisted files: $($unlisted.FullName -join ', ')" }

$releaseMetadata = Get-Content -LiteralPath (Join-Path $PackageData "RELEASE-METADATA.json") -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace([string]$releaseMetadata.version)) { throw "Package release metadata has no version." }

if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
    $BackupRoot = Join-Path $DestinationDirectory "vSMR_Backups"
}
$BackupRoot = Resolve-NonRootDirectoryPath $BackupRoot "BackupRoot"
if (Test-PathEqualOrChild $BackupRoot $destinationData) {
    throw "BackupRoot cannot be inside the vSMR_Data directory being replaced."
}
if (Test-PathEqualOrChild $BackupRoot $PackageRoot) {
    throw "BackupRoot cannot be inside the extracted package."
}
$stamp = [DateTime]::UtcNow.ToString("yyyyMMdd_HHmmss")
$backupDirectory = Join-Path $BackupRoot ("vSMR-before-" + $releaseMetadata.version + "-" + $stamp + "-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
Assert-ChildPath $backupDirectory $BackupRoot
$hadDll = Test-Path -LiteralPath $destinationDll -PathType Leaf
$hadData = Test-Path -LiteralPath $destinationData -PathType Container

# Confirm before creating even a backup so -WhatIf is genuinely non-mutating.
if (-not $PSCmdlet.ShouldProcess($DestinationDirectory, "Install vSMR $($releaseMetadata.version); create rollback backup under $BackupRoot")) {
    Write-Host "Installation skipped; no files were changed."
    return
}

[System.IO.Directory]::CreateDirectory($BackupRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($backupDirectory) | Out-Null
if ($hadDll) { Copy-Item -LiteralPath $destinationDll -Destination $backupDirectory }
if ($hadData) { Copy-Item -LiteralPath $destinationData -Destination $backupDirectory -Recurse }

$profileSchemas = @()
if ($hadData -and (Test-Path -LiteralPath (Join-Path $destinationData "vSMR_Profiles.json") -PathType Leaf)) {
    try {
        $oldProfiles = Get-Content -LiteralPath (Join-Path $destinationData "vSMR_Profiles.json") -Raw | ConvertFrom-Json
        $profileSchemas = @($oldProfiles | Where-Object { $_.PSObject.Properties.Name -contains 'name' } |
            ForEach-Object { [int]$_.schema_version } | Sort-Object -Unique)
    }
    catch { $profileSchemas = @("unreadable") }
}
$backupMetadata = [ordered]@{
    schema_version = 1
    kind = "vSMR complete pre-install backup"
    created_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    destination = $DestinationDirectory
    installing_version = [string]$releaseMetadata.version
    had_dll = [bool]$hadDll
    had_data = [bool]$hadData
    profile_schema_versions = $profileSchemas
}
[System.IO.File]::WriteAllText(
    (Join-Path $backupDirectory "BACKUP-METADATA.json"),
    ((ConvertTo-Json $backupMetadata -Depth 10) + "`n"),
    $Utf8NoBom)

$operationId = [Guid]::NewGuid().ToString("N")
$stageRoot = Join-Path $DestinationDirectory (".vsmr-install-" + $operationId)
$oldDataSwap = Join-Path $DestinationDirectory (".vsmr-previous-data-" + $operationId)
$oldDllSwap = Join-Path $DestinationDirectory (".vsmr-previous-dll-" + $operationId)
Assert-ChildPath $stageRoot $DestinationDirectory
Assert-ChildPath $oldDataSwap $DestinationDirectory
Assert-ChildPath $oldDllSwap $DestinationDirectory
[System.IO.Directory]::CreateDirectory($stageRoot) | Out-Null
$stageData = Join-Path $stageRoot "vSMR_Data"
Copy-Item -LiteralPath $PackageData -Destination $stageData -Recurse
$stageDll = Join-Path $stageRoot "vSMR.dll"
Copy-Item -LiteralPath $PackageDll -Destination $stageDll

if ($hadData -and -not $ReplaceUserData) {
    $immutableNames = @('vSMR_webUI', 'Licenses', 'Tools', 'RELEASE-METADATA.json', 'SHA256SUMS.txt', 'INSTALLATION.json')
    foreach ($item in @(Get-ChildItem -LiteralPath $destinationData -Force)) {
        if ($immutableNames -contains $item.Name) { continue }
        $target = Join-Path $stageData $item.Name
        if ($item.PSIsContainer) {
            Copy-DirectoryContents $item.FullName $target
        }
        else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
}

$installationMetadata = [ordered]@{
    schema_version = 1
    product = "vSMR"
    installed_version = [string]$releaseMetadata.version
    installed_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    source_commit = [string]$releaseMetadata.git_commit
    user_data_preserved = -not [bool]$ReplaceUserData
    rollback_backup = $backupDirectory
}
[System.IO.File]::WriteAllText(
    (Join-Path $stageData "INSTALLATION.json"),
    ((ConvertTo-Json $installationMetadata -Depth 10) + "`n"),
    $Utf8NoBom)

$dataSwapped = $false
$dllSwapped = $false
try {
    if ($hadData) { Move-Item -LiteralPath $destinationData -Destination $oldDataSwap }
    Move-Item -LiteralPath $stageData -Destination $destinationData
    $dataSwapped = $true
    if ($hadDll) { Move-Item -LiteralPath $destinationDll -Destination $oldDllSwap }
    Move-Item -LiteralPath $stageDll -Destination $destinationDll
    $dllSwapped = $true
}
catch {
    if ($dllSwapped -and (Test-Path -LiteralPath $destinationDll)) { Remove-Item -LiteralPath $destinationDll -Force }
    if (Test-Path -LiteralPath $oldDllSwap) { Move-Item -LiteralPath $oldDllSwap -Destination $destinationDll }
    if ($dataSwapped -and (Test-Path -LiteralPath $destinationData)) { Remove-Item -LiteralPath $destinationData -Recurse -Force }
    if (Test-Path -LiteralPath $oldDataSwap) { Move-Item -LiteralPath $oldDataSwap -Destination $destinationData }
    throw
}
finally {
    if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
}

if (Test-Path -LiteralPath $oldDllSwap) { Remove-Item -LiteralPath $oldDllSwap -Force }
if (Test-Path -LiteralPath $oldDataSwap) { Remove-Item -LiteralPath $oldDataSwap -Recurse -Force }
Write-Host "Installed vSMR $($releaseMetadata.version)."
Write-Host "Rollback backup: $backupDirectory"
