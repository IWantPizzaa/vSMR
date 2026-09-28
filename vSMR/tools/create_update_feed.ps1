#requires -Version 5.1
<#
Prepare creates an immutable payload tree from compiled release files. Commit
that tree on the update-feed branch, then Manifest verifies those exact Git
blobs before generating beta/version.json or stable/version.json. Neither phase
commits, pushes, or changes an installed copy of vSMR.
#>
[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Manifest')][string]$Phase = 'Prepare',
    [string]$RepositoryRoot = '',
    [string]$BuildOutputDirectory = '',
    [Parameter(Mandatory = $true)][string]$FeedDirectory,
    [ValidatePattern('^(|[0-9a-fA-F]{40})$')][string]$ContentCommit = '',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$MinimumLoaderVersion = '1.3.0',
    [ValidateSet('auto', 'stable', 'beta')][string]$Channel = 'auto',
    [switch]$ValidationOnly,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = [Text.UTF8Encoding]::new($false)
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '..\..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not $BuildOutputDirectory) { $BuildOutputDirectory = Join-Path $RepositoryRoot 'Release' }
$BuildOutputDirectory = [IO.Path]::GetFullPath($BuildOutputDirectory)
$FeedDirectory = [IO.Path]::GetFullPath($FeedDirectory)
$payloadRoot = Join-Path $FeedDirectory 'payload'
$recordPath = Join-Path $FeedDirectory 'prepared-feed.json'

function Assert-RegularPath([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not accepted in release paths: $($item.FullName)"
        }
        if ($item -is [IO.FileInfo]) { $item = $item.Directory } else { $item = $item.Parent }
    }
}
function Assert-ManagedPath([string]$Relative) {
    $allowed = '^(vSMR\.dll|vSMR_Data/(default\.json|airports_hp\.json|ICAO_Aircraft\.json|AVISO-UPDATE-POLICY\.json|AVISO/[A-Z0-9]{4}\.geojson|aircraft_icons/[A-Za-z0-9_-]+\.png|Audio/[A-Za-z0-9_.-]+\.wav|Runtime/[A-Za-z0-9_.-]+\.dll|CrashReporter/vSMRCrashHandler\.dll|Tools/[A-Za-z0-9_.-]+\.(exe|ps1|cs|patch)|Licenses/[A-Za-z0-9_.-]+\.(txt|md)|vSMR_webUI/(index\.html|styles\.css|data\.js|app-bundle\.js)))$'
    if ($Relative -notmatch $allowed -or
        $Relative -match '(?i)(^|/)(config\.json|vSMR_Profiles\.json|UserData|UpdateBaselines|\.update|version\.json)(/|$)' -or
        $Relative -match '(^|/)\.{1,2}(/|$)|[. ](/|$)|(?i)\.(asr|pdb|bak|tmp)$' -or
        $Relative -match '(?i)(^|/)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|/|$)') {
        throw "Not an application-managed payload path: $Relative"
    }
}
function Assert-Pe32([string]$Path) {
    Assert-RegularPath $Path
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 128 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "Not a compiled PE binary: $Path" }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 6) { throw "Invalid PE header: $Path" }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw "Expected a Win32 compiled binary: $Path" }
    } finally { $reader.Dispose() }
}
function Write-Json([string]$Path, $Value) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 100) + "`n"), $utf8)
}
function Assert-GitSuccess([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "Git failed while $Operation." }
}

if ($Phase -eq 'Prepare') {
    if ($ContentCommit) { throw 'ContentCommit belongs to Manifest phase, after payload is committed.' }
    if ($SkipBuild -and -not $ValidationOnly) { throw '-SkipBuild requires -ValidationOnly.' }
    if (Test-Path -LiteralPath $payloadRoot) { throw 'The payload directory already exists. Use a fresh feed staging directory.' }
    & (Join-Path $RepositoryRoot 'vSMR/tools/verify_release_inputs.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'vSMR/tools/build_config_defaults.ps1') -RepositoryRoot $RepositoryRoot -Check
    $pluginMetadata = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/src/plugin/PluginMetadata.hpp') -Raw
    if ($pluginMetadata -notmatch 'VsmrPluginVersion\[\]\s*=\s*"v([^"]+)"') { throw 'Cannot read runtime version.' }
    $version = $Matches[1]
    $loaderMetadata = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/src/bootstrap/loader/LoaderVersion.hpp') -Raw
    if ($loaderMetadata -notmatch 'Value\[\]\s*=\s*"([^"]+)"') { throw 'Cannot read loader version.' }
    $loaderVersion = $Matches[1]
    if ([version]$MinimumLoaderVersion -gt [version]$loaderVersion) { throw 'Minimum loader cannot exceed the bundled loader version.' }
    $runtimeApi = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/src/bootstrap/RuntimeApi.hpp') -Raw
    if ($runtimeApi -notmatch 'AbiVersion\s*=\s*(\d+)U?\s*;') { throw 'Cannot read runtime ABI.' }
    $runtimeAbi = [int]$Matches[1]
    if ($runtimeAbi -lt 1 -or $runtimeAbi -gt 65535) { throw 'Invalid runtime ABI.' }
    if (-not $ValidationOnly) {
        $dirty = @(& git -C $RepositoryRoot status --porcelain --untracked-files=normal)
        Assert-GitSuccess 'checking release source'
        if ($dirty.Count) { throw 'Publishable feed artifacts require clean committed source. Use -ValidationOnly for local tests.' }
        $provenance = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vSMR/data/Licenses/ASSET_PROVENANCE.md') -Raw
        if ($provenance -match '(?im)\|\s*verification required\s*\|\s*$') { throw 'Bundled asset provenance still requires verification.' }
    }
    if (-not $SkipBuild) {
        $defaultBuild = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'Release'))
        if (-not $BuildOutputDirectory.TrimEnd('\').Equals($defaultBuild.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
            throw 'BuildOutputDirectory must be the repository Release folder unless using -ValidationOnly -SkipBuild.'
        }
        & (Join-Path $RepositoryRoot 'vSMR/tools/build_project.ps1') -RepositoryRoot $RepositoryRoot
    }
    $managed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in @('vSMR.dll', 'vSMR_Data/Runtime/vSMR.Runtime.dll',
        'vSMR_Data/CrashReporter/vSMRCrashHandler.dll', 'vSMR_Data/Tools/vSMR.ApplyUpdate.exe')) {
        [void]$managed.Add($relative)
        Assert-Pe32 (Join-Path $BuildOutputDirectory $relative)
    }
    $runtimeInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $BuildOutputDirectory 'vSMR_Data/Runtime/vSMR.Runtime.dll'))
    if ($runtimeInfo.ProductVersion -ne $version) { throw 'Compiled runtime version differs from source version.' }
    $loaderInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $BuildOutputDirectory 'vSMR.dll'))
    if ($loaderInfo.FileVersion -ne ($loaderVersion + '.0')) { throw 'Compiled loader version differs from source version.' }
    # The source inventory, not arbitrary contents of an installation, defines
    # ownership. Legacy profiles are migration input only, never managed here.
    $sourceData = Join-Path $RepositoryRoot 'vSMR/data'
    foreach ($asset in @(Get-ChildItem -LiteralPath $sourceData -File -Recurse)) {
        Assert-RegularPath $asset.FullName
        $relative = $asset.FullName.Substring($sourceData.Length).TrimStart('\', '/').Replace('\', '/')
        if ($relative -match '(?i)(^|/)(config\.json|vSMR_Profiles\.json|UserData|UpdateBaselines|\.update|version\.json)(/|$)' -or
            $relative -match '^AVISO/(ALL_FR_.*|_LFXX)\.geojson$|^AVISO/version\.txt$') { continue }
        [void]$managed.Add('vSMR_Data/' + $relative)
    }
    foreach ($name in @('index.html', 'styles.css', 'data.js', 'app-bundle.js')) {
        [void]$managed.Add('vSMR_Data/vSMR_webUI/' + $name)
    }
    foreach ($name in @('vSMR.txt', 'RapidJSON.txt', 'Microsoft.WebView2-LICENSE.txt', 'Microsoft.WebView2-NOTICE.txt')) {
        [void]$managed.Add('vSMR_Data/Licenses/' + $name)
    }
    $files = [ordered]@{}
    # Validate the full input before creating a partially usable payload.
    foreach ($relative in @($managed | Sort-Object)) {
        Assert-ManagedPath $relative
        $source = Join-Path $BuildOutputDirectory $relative
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Compiled release asset is missing: $relative" }
        Assert-RegularPath $source
        $files.Add($relative, [ordered]@{
            sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
            size = [long](Get-Item -LiteralPath $source).Length
        })
    }
    [IO.Directory]::CreateDirectory($FeedDirectory) | Out-Null
    Assert-RegularPath $FeedDirectory
    foreach ($relative in $files.Keys) {
        $destination = Join-Path $payloadRoot $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $BuildOutputDirectory $relative) -Destination $destination
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $files[$relative].sha256) {
            throw "Release input changed while staging: $relative"
        }
    }
    Write-Json $recordPath ([ordered]@{ schema = 1; version = $version; minimum_loader_version = $MinimumLoaderVersion
        runtime_abi = $runtimeAbi; validation_only = [bool]$ValidationOnly; files = $files })
    if ($ValidationOnly) {
        Write-Host "Staged $($files.Count) compiled files for local validation only; no publishable manifest was created."
    } else {
        Write-Host "Staged $($files.Count) compiled files. Commit payload/ on update-feed, then run -Phase Manifest -ContentCommit <full commit>."
    }
    exit 0
}

if (-not $ContentCommit) { throw 'Manifest phase requires the full immutable payload ContentCommit.' }
$record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
if ($record.validation_only -and -not $ValidationOnly) { throw 'A validation-only payload cannot become a publishable manifest.' }
if ($record.schema -ne 1 -or $record.runtime_abi -lt 1 -or $record.runtime_abi -gt 65535) { throw 'Unsupported prepared payload schema or ABI.' }
if ($Channel -eq 'auto') { $Channel = if ($record.version -match '-beta\.') { 'beta' } else { 'stable' } }
if ($Channel -eq 'stable' -and $record.version -match '-') { throw 'Prereleases cannot be promoted to the stable channel.' }
$commit = @(& git -C $FeedDirectory rev-parse --verify ($ContentCommit + '^{commit}'))
Assert-GitSuccess 'resolving the payload commit'
if ($commit.Count -ne 1 -or $commit[0] -ne $ContentCommit) { throw 'The payload commit must resolve exactly.' }
$tree = @(& git -C $FeedDirectory ls-tree -r --full-tree $ContentCommit -- payload)
Assert-GitSuccess 'reading the immutable payload tree'
$blobs = @{}
foreach ($entry in $tree) {
    if ($entry -notmatch '^100644 blob ([0-9a-f]{40})\t(payload/.+)$') { throw "Unexpected payload Git entry: $entry" }
    $blobs.Add($Matches[2], $Matches[1])
}
$properties = @($record.files.PSObject.Properties)
if ($blobs.Count -ne $properties.Count -or $properties.Count -eq 0) { throw 'Committed payload inventory differs from prepared release.' }
foreach ($file in $properties) {
    Assert-ManagedPath $file.Name
    $path = Join-Path $payloadRoot $file.Name
    Assert-RegularPath $path
    if ($file.Value.sha256 -notmatch '^[0-9a-f]{64}$' -or
        [long]$file.Value.size -ne (Get-Item -LiteralPath $path).Length -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Value.sha256) {
        throw "Prepared payload was changed: $($file.Name)"
    }
    $blob = @(& git -C $FeedDirectory hash-object --no-filters -- $path)
    Assert-GitSuccess 'verifying committed payload bytes'
    if ($blob.Count -ne 1 -or $blobs['payload/' + $file.Name] -ne $blob[0]) {
        throw "Committed bytes differ from staged payload: $($file.Name). Disable Git text conversion for payload/."
    }
}
$manifest = [ordered]@{ schema = 1; version = $record.version; content_commit = $ContentCommit.ToLowerInvariant()
    minimum_loader_version = $record.minimum_loader_version; runtime_abi = $record.runtime_abi; files = $record.files }
$name = if ($ValidationOnly -or $record.validation_only) { 'version.validation-only.json' } else { 'version.json' }
$manifestPath = Join-Path (Join-Path $FeedDirectory $Channel) $name
Write-Json $manifestPath $manifest
Write-Host "Created $manifestPath. No files were committed or pushed."
