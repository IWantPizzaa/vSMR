#requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$DestinationDirectory,
    [Parameter(Mandatory = $true)][string]$BackupDirectory,
    [switch]$PreserveLoader,
    [switch]$RuntimeUpdate
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
$preserveTopLevelLoader = [bool]($PreserveLoader -or $RuntimeUpdate)
function Write-RollbackOutcome([string]$Path, [string]$Status, [string]$TransactionId, [string]$SourceBackup, [bool]$LoaderPreserved) {
    $outcome = [ordered]@{
        schema_version = 1
        kind = "vSMR rollback transaction outcome"
        transaction_id = $TransactionId
        status = $Status
        recorded_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        destination = $DestinationDirectory
        source_backup = $SourceBackup
        loader_preserved = $LoaderPreserved
    }
    [System.IO.File]::WriteAllText(
        $Path,
        ((ConvertTo-Json $outcome -Depth 5) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
}
function Test-PathEqualOrChild([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $resolvedPath.Equals($resolvedParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Backup runtime has no DOS/PE header." }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) { throw "Backup runtime has an invalid PE offset." }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Backup runtime has an invalid PE signature." }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
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
if ($metadata.had_data) {
    $backupData = Join-Path $BackupDirectory "vSMR_Data"
    $backupReleaseMetadataPath = Join-Path $backupData "RELEASE-METADATA.json"
    $backupRuntimePath = Join-Path $backupData "Runtime\vSMR.Runtime.dll"
    $hasBackupReleaseMetadata = Test-Path -LiteralPath $backupReleaseMetadataPath -PathType Leaf
    $hasBackupRuntime = Test-Path -LiteralPath $backupRuntimePath -PathType Leaf
    if ($preserveTopLevelLoader -and (-not $hasBackupReleaseMetadata -or -not $hasBackupRuntime)) {
        throw "Backup does not contain a verifiable canonical vSMR runtime."
    }
    if ($hasBackupReleaseMetadata -ne $hasBackupRuntime) {
        throw "Backup contains an incomplete canonical runtime/metadata pair."
    }
    if ($hasBackupRuntime) {
        $backupReleaseMetadata = Get-Content -LiteralPath $backupReleaseMetadataPath -Raw | ConvertFrom-Json
        $expectedRuntimeHash = ([string]$backupReleaseMetadata.runtime.sha256).ToLowerInvariant()
        $expectedRuntimeSize = [int64]$backupReleaseMetadata.runtime.size
        if ($backupReleaseMetadata.product -ne 'vSMR' -or
            [string]$backupReleaseMetadata.runtime.relative_path -ne 'vSMR_Data/Runtime/vSMR.Runtime.dll' -or
            $expectedRuntimeSize -le 0 -or
            $expectedRuntimeHash -notmatch '^[0-9a-f]{64}$' -or
            [int64](Get-Item -LiteralPath $backupRuntimePath).Length -ne $expectedRuntimeSize -or
            (Get-FileHash -LiteralPath $backupRuntimePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedRuntimeHash -or
            (Get-PeMachine $backupRuntimePath) -ne 0x014C) {
            throw "Backup canonical runtime failed metadata, size, SHA-256, or Win32 validation."
        }
    }
}
$destinationDll = Join-Path $DestinationDirectory "vSMR.dll"
$destinationData = Join-Path $DestinationDirectory "vSMR_Data"
$currentDll = Test-Path -LiteralPath $destinationDll -PathType Leaf
$currentData = Test-Path -LiteralPath $destinationData -PathType Container
if ($preserveTopLevelLoader -and -not $currentDll) {
    throw "Runtime-update rollback requires an existing top-level vSMR.dll loader."
}
$restoreMode = if ($preserveTopLevelLoader) { " while preserving the loaded top-level vSMR.dll loader" } else { "" }
if (-not $PSCmdlet.ShouldProcess($DestinationDirectory, "Restore vSMR backup $BackupDirectory$restoreMode")) { return }

$safetyRoot = Join-Path (Split-Path -Parent $BackupDirectory) ("vSMR-before-rollback-" + [DateTime]::UtcNow.ToString("yyyyMMdd_HHmmss") + "-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
[System.IO.Directory]::CreateDirectory($safetyRoot) | Out-Null
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
    loader_preserved = $preserveTopLevelLoader
}
[System.IO.File]::WriteAllText(
    (Join-Path $safetyRoot "BACKUP-METADATA.json"),
    ((ConvertTo-Json $safetyMetadata -Depth 5) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$operationId = [Guid]::NewGuid().ToString("N")
$rollbackOutcomePath = Join-Path $safetyRoot "TRANSACTION-OUTCOME.json"
Write-RollbackOutcome $rollbackOutcomePath "prepared" $operationId $BackupDirectory $preserveTopLevelLoader
$oldDll = Join-Path $DestinationDirectory (".vsmr-rollback-dll-" + $operationId)
$oldData = Join-Path $DestinationDirectory (".vsmr-rollback-data-" + $operationId)
$dllMoved = $false
$dataMoved = $false
$newDllInstallAttempted = $false
$newDataInstallAttempted = $false
$rollbackCommitted = $false
try {
    if (-not $preserveTopLevelLoader -and $currentDll) { Move-Item -LiteralPath $destinationDll -Destination $oldDll; $dllMoved = $true }
    if ($currentData) { Move-Item -LiteralPath $destinationData -Destination $oldData; $dataMoved = $true }
    if (-not $preserveTopLevelLoader -and $metadata.had_dll) {
		$newDllInstallAttempted = $true
        Copy-Item -LiteralPath (Join-Path $BackupDirectory "vSMR.dll") -Destination $destinationDll
    }
    if ($metadata.had_data) {
		$newDataInstallAttempted = $true
        Copy-Item -LiteralPath (Join-Path $BackupDirectory "vSMR_Data") -Destination $destinationData -Recurse
    }
    Write-RollbackOutcome $rollbackOutcomePath "committed" $operationId $BackupDirectory $preserveTopLevelLoader
    $rollbackCommitted = $true
}
catch {
	$restoreError = $_
    try {
		if ($newDllInstallAttempted -and (Test-Path -LiteralPath $destinationDll)) { Remove-Item -LiteralPath $destinationDll -Force }
		if ($newDataInstallAttempted -and (Test-Path -LiteralPath $destinationData)) { Remove-Item -LiteralPath $destinationData -Recurse -Force }
        if ($dllMoved -and (Test-Path -LiteralPath $oldDll)) { Move-Item -LiteralPath $oldDll -Destination $destinationDll }
        if ($dataMoved -and (Test-Path -LiteralPath $oldData)) { Move-Item -LiteralPath $oldData -Destination $destinationData }
        Write-RollbackOutcome $rollbackOutcomePath "rolled_back" $operationId $BackupDirectory $preserveTopLevelLoader
    }
    catch {
        Write-Warning "Rollback recovery or transaction-journal update was incomplete: $($_.Exception.Message)"
    }
    throw $restoreError
}
if (-not $rollbackCommitted) {
    throw "The rollback transaction did not reach its durable committed outcome."
}
foreach ($obsoleteSwap in @($oldDll, $oldData)) {
    try {
        if (Test-Path -LiteralPath $obsoleteSwap) {
            Remove-Item -LiteralPath $obsoleteSwap -Recurse -Force
        }
    }
    catch {
        Write-Warning "The rollback succeeded, but obsolete pre-swap files could not be removed from '$obsoleteSwap': $($_.Exception.Message)"
    }
}
Write-Host "Restored vSMR backup: $BackupDirectory"
if ($preserveTopLevelLoader) { Write-Host "Preserved the loaded top-level vSMR.dll loader." }
Write-Host "Pre-rollback safety backup: $safetyRoot"
