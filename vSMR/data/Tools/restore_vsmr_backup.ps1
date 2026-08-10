#requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$DestinationDirectory,
    [Parameter(Mandatory = $true)][string]$BackupDirectory
)

$ErrorActionPreference = "Stop"
function Resolve-NonRootDirectoryPath([string]$Path, [string]$Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Description cannot be empty." }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
    if ($fullPath.TrimEnd('\', '/').Equals($pathRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description cannot be a filesystem root: $fullPath"
    }
    return $fullPath.TrimEnd('\', '/')
}

$DestinationDirectory = Resolve-NonRootDirectoryPath $DestinationDirectory "DestinationDirectory"
$BackupDirectory = Resolve-NonRootDirectoryPath $BackupDirectory "BackupDirectory"
function Test-PathEqualOrChild([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $resolvedPath.Equals($resolvedParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

$destinationDataPath = [System.IO.Path]::GetFullPath((Join-Path $DestinationDirectory "vSMR_Data")).TrimEnd('\', '/')
$backupDataPath = [System.IO.Path]::GetFullPath((Join-Path $BackupDirectory "vSMR_Data")).TrimEnd('\', '/')
if ((Test-PathEqualOrChild $BackupDirectory $destinationDataPath) -or
    (Test-PathEqualOrChild $destinationDataPath $backupDataPath)) {
    throw "Backup and active vSMR_Data trees cannot overlap: $BackupDirectory"
}
$metadataPath = Join-Path $BackupDirectory "BACKUP-METADATA.json"
if (-not (Test-Path -LiteralPath $DestinationDirectory -PathType Container)) { throw "Destination does not exist." }
if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) { throw "Backup metadata is missing." }
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
$requiredMetadataFields = @('schema_version', 'kind', 'destination', 'had_dll', 'had_data')
foreach ($field in $requiredMetadataFields) {
    if ($metadata.PSObject.Properties.Name -notcontains $field) {
        throw "Backup metadata is missing required field '$field'."
    }
}
if (($metadata.schema_version -isnot [int] -and $metadata.schema_version -isnot [long]) -or
    [int64]$metadata.schema_version -ne 1 -or
    $metadata.kind -isnot [string] -or [string]::IsNullOrWhiteSpace($metadata.kind) -or
    $metadata.destination -isnot [string] -or [string]::IsNullOrWhiteSpace($metadata.destination) -or
    $metadata.had_dll -isnot [bool] -or
    $metadata.had_data -isnot [bool]) {
    throw "Backup metadata contains invalid field types or values."
}
$supportedKinds = @('vSMR complete pre-install backup', 'vSMR pre-rollback safety backup')
if ($supportedKinds -notcontains $metadata.kind) {
    throw "This is not a supported vSMR backup."
}
$metadataDestination = Resolve-NonRootDirectoryPath $metadata.destination "Backup metadata destination"
if (-not $metadataDestination.Equals($DestinationDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "This backup belongs to '$metadataDestination', not '$DestinationDirectory'."
}
if ($metadata.had_dll -and -not (Test-Path -LiteralPath (Join-Path $BackupDirectory "vSMR.dll") -PathType Leaf)) {
    throw "Backup metadata expects vSMR.dll, but the file is missing."
}
if ($metadata.had_data -and -not (Test-Path -LiteralPath (Join-Path $BackupDirectory "vSMR_Data") -PathType Container)) {
    throw "Backup metadata expects vSMR_Data, but the directory is missing."
}
if (-not $PSCmdlet.ShouldProcess($DestinationDirectory, "Restore vSMR backup $BackupDirectory")) { return }

$safetyRoot = Join-Path (Split-Path -Parent $BackupDirectory) ("vSMR-before-rollback-" + [DateTime]::UtcNow.ToString("yyyyMMdd_HHmmss") + "-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
[System.IO.Directory]::CreateDirectory($safetyRoot) | Out-Null
$destinationDll = Join-Path $DestinationDirectory "vSMR.dll"
$destinationData = Join-Path $DestinationDirectory "vSMR_Data"
$currentDll = Test-Path -LiteralPath $destinationDll -PathType Leaf
$currentData = Test-Path -LiteralPath $destinationData -PathType Container
if ($currentDll) { Copy-Item -LiteralPath $destinationDll -Destination $safetyRoot }
if ($currentData) { Copy-Item -LiteralPath $destinationData -Destination $safetyRoot -Recurse }
$safetyMetadata = [ordered]@{
    schema_version = 1
    kind = "vSMR pre-rollback safety backup"
    created_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    destination = $DestinationDirectory
    source_backup = $BackupDirectory
    had_dll = [bool]$currentDll
    had_data = [bool]$currentData
}
[System.IO.File]::WriteAllText(
    (Join-Path $safetyRoot "BACKUP-METADATA.json"),
    ((ConvertTo-Json $safetyMetadata -Depth 5) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$operationId = [Guid]::NewGuid().ToString("N")
$oldDll = Join-Path $DestinationDirectory (".vsmr-rollback-dll-" + $operationId)
$oldData = Join-Path $DestinationDirectory (".vsmr-rollback-data-" + $operationId)
$dllMoved = $false
$dataMoved = $false
$newDllInstallAttempted = $false
$newDataInstallAttempted = $false
try {
    if ($currentDll) { Move-Item -LiteralPath $destinationDll -Destination $oldDll; $dllMoved = $true }
    if ($currentData) { Move-Item -LiteralPath $destinationData -Destination $oldData; $dataMoved = $true }
    if ($metadata.had_dll) {
		$newDllInstallAttempted = $true
        Copy-Item -LiteralPath (Join-Path $BackupDirectory "vSMR.dll") -Destination $destinationDll
    }
    if ($metadata.had_data) {
		$newDataInstallAttempted = $true
        Copy-Item -LiteralPath (Join-Path $BackupDirectory "vSMR_Data") -Destination $destinationData -Recurse
    }
}
catch {
	if ($newDllInstallAttempted -and (Test-Path -LiteralPath $destinationDll)) { Remove-Item -LiteralPath $destinationDll -Force }
	if ($newDataInstallAttempted -and (Test-Path -LiteralPath $destinationData)) { Remove-Item -LiteralPath $destinationData -Recurse -Force }
    if ($dllMoved -and (Test-Path -LiteralPath $oldDll)) { Move-Item -LiteralPath $oldDll -Destination $destinationDll }
    if ($dataMoved -and (Test-Path -LiteralPath $oldData)) { Move-Item -LiteralPath $oldData -Destination $destinationData }
    throw
}
if (Test-Path -LiteralPath $oldDll) { Remove-Item -LiteralPath $oldDll -Force }
if (Test-Path -LiteralPath $oldData) { Remove-Item -LiteralPath $oldData -Recurse -Force }
Write-Host "Restored vSMR backup: $BackupDirectory"
Write-Host "Pre-rollback safety backup: $safetyRoot"
