#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,
    [ValidatePattern("^\d+\.\d+\.\d+(?:-beta\.\d+)?$")]
    [string]$ExpectedVersion = "2.0.0-beta.4",
    [string]$ChecksumPath = "",
    [string]$UpdateManifestPath = "",
    [string]$UpdateSignaturePath = "",
    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$ExpectedLoaderVersion = "1.0.0",
    [ValidateRange(1, 65535)]
    [int]$ExpectedRuntimeAbi = 1,
    [switch]$RequireSignature,
    [switch]$AllowNonPublishable
)

$ErrorActionPreference = "Stop"
if ($env:VSMR_ALLOW_NONPUBLISHABLE -eq '1') { $AllowNonPublishable = $true }
$ArchivePath = [System.IO.Path]::GetFullPath($ArchivePath)
if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
    throw "Release archive not found: $ArchivePath"
}
if ([System.IO.Path]::GetFileName($ArchivePath) -ne "vSMR-$ExpectedVersion.zip") {
    throw "Release archive must be named vSMR-$ExpectedVersion.zip."
}
if ([string]::IsNullOrWhiteSpace($ChecksumPath)) {
    $ChecksumPath = "$ArchivePath.sha256"
}
$ChecksumPath = [System.IO.Path]::GetFullPath($ChecksumPath)
if (-not (Test-Path -LiteralPath $ChecksumPath -PathType Leaf)) {
    throw "Archive checksum file not found: $ChecksumPath"
}
$checksumLine = ([System.IO.File]::ReadAllText($ChecksumPath)).Trim()
if ($checksumLine -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
    throw "Invalid outer SHA-256 checksum format: $ChecksumPath"
}
if ($Matches[2] -ne [System.IO.Path]::GetFileName($ArchivePath)) {
    throw "Outer checksum names '$($Matches[2])' instead of '$([System.IO.Path]::GetFileName($ArchivePath))'."
}
$archiveHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
if ($archiveHash -ne $Matches[1]) {
    throw "Outer SHA-256 checksum does not match the release archive."
}
if ($env:VSMR_REQUIRE_SIGNATURE -eq '1' -or $env:VSMR_REQUIRE_SIGNATURE -eq 'true') {
    $RequireSignature = $true
}
if ([string]::IsNullOrWhiteSpace($UpdateManifestPath)) {
    $UpdateManifestPath = Join-Path (Split-Path -Parent $ArchivePath) "vSMR-$ExpectedVersion.update.json"
}
$UpdateManifestPath = [System.IO.Path]::GetFullPath($UpdateManifestPath)
if (-not (Test-Path -LiteralPath $UpdateManifestPath -PathType Leaf)) {
    throw "Update manifest not found: $UpdateManifestPath"
}
if ([string]::IsNullOrWhiteSpace($UpdateSignaturePath)) {
    $UpdateSignaturePath = "$UpdateManifestPath.p7s"
}
$UpdateSignaturePath = [System.IO.Path]::GetFullPath($UpdateSignaturePath)
if ($RequireSignature -and -not (Test-Path -LiteralPath $UpdateSignaturePath -PathType Leaf)) {
    throw "Detached update signature not found: $UpdateSignaturePath"
}

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required package file is missing: $Path"
    }
}

function Get-PeMachine {
    param([string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "$Path has no DOS/PE header."
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) {
            throw "$Path has an invalid PE offset."
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "$Path has an invalid PE signature."
        }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-PeExportNames {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
        throw "$Path has no DOS/PE header."
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 24 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "$Path has an invalid PE signature."
    }
    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $optionalOffset = $peOffset + 24
    if ($optionalOffset + $optionalSize -gt $bytes.Length) { throw "$Path has a truncated optional header." }
    $magic = [BitConverter]::ToUInt16($bytes, $optionalOffset)
    $dataDirectoryOffset = if ($magic -eq 0x010B) { $optionalOffset + 96 } elseif ($magic -eq 0x020B) { $optionalOffset + 112 } else { throw "$Path has an unsupported PE optional header." }
    if ($dataDirectoryOffset + 8 -gt $optionalOffset + $optionalSize) { throw "$Path has no export data directory." }
    $exportRva = [BitConverter]::ToUInt32($bytes, $dataDirectoryOffset)
    if ($exportRva -eq 0) { return @() }
    $sizeOfHeaders = [BitConverter]::ToUInt32($bytes, $optionalOffset + 60)
    $sections = @()
    $sectionOffset = $optionalOffset + $optionalSize
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $offset = $sectionOffset + (40 * $index)
        if ($offset + 40 -gt $bytes.Length) { throw "$Path has a truncated section table." }
        $sections += [pscustomobject]@{
            VirtualSize = [BitConverter]::ToUInt32($bytes, $offset + 8)
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $offset + 12)
            RawSize = [BitConverter]::ToUInt32($bytes, $offset + 16)
            RawOffset = [BitConverter]::ToUInt32($bytes, $offset + 20)
        }
    }
    $rvaToOffset = {
        param([uint32]$Rva)
        if ($Rva -lt $sizeOfHeaders) { return [int]$Rva }
        foreach ($section in $sections) {
            $span = [Math]::Max([uint64]$section.VirtualSize, [uint64]$section.RawSize)
            if ([uint64]$Rva -ge [uint64]$section.VirtualAddress -and
                [uint64]$Rva -lt ([uint64]$section.VirtualAddress + $span)) {
                return [int]([uint64]$section.RawOffset + ([uint64]$Rva - [uint64]$section.VirtualAddress))
            }
        }
        throw ("RVA 0x{0:X8} is not backed by a PE section in $Path." -f $Rva)
    }
    $exportOffset = & $rvaToOffset $exportRva
    if ($exportOffset + 40 -gt $bytes.Length) { throw "$Path has a truncated export directory." }
    $nameCount = [BitConverter]::ToUInt32($bytes, $exportOffset + 24)
    if ($nameCount -gt 4096) { throw "$Path has an unreasonable export-name count." }
    $nameTableRva = [BitConverter]::ToUInt32($bytes, $exportOffset + 32)
    $nameTableOffset = & $rvaToOffset $nameTableRva
    $names = @()
    for ($index = 0; $index -lt $nameCount; $index++) {
        $entryOffset = $nameTableOffset + (4 * $index)
        if ($entryOffset + 4 -gt $bytes.Length) { throw "$Path has a truncated export-name table." }
        $nameRva = [BitConverter]::ToUInt32($bytes, $entryOffset)
        $nameOffset = & $rvaToOffset $nameRva
        $end = $nameOffset
        while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
        if ($end -eq $bytes.Length) { throw "$Path has an unterminated export name." }
        $names += [System.Text.Encoding]::ASCII.GetString($bytes, $nameOffset, $end - $nameOffset)
    }
    return $names
}

function Get-CertificateDerSha256 {
    param([Parameter(Mandatory = $true)]$Certificate)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($Certificate.RawData))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha256.Dispose() }
}

function Test-VersionEquals {
    param([string]$Actual, [string]$Expected)
    if ([string]::IsNullOrWhiteSpace($Actual)) { return $false }
    return $Actual -eq $Expected -or $Actual -eq "$Expected.0"
}

function Write-TestReleaseMetadata {
    param(
        [Parameter(Mandatory = $true)][string]$DataDirectory,
        [Parameter(Mandatory = $true)][string]$RuntimePath,
        [string]$Version = '1.9.9'
    )
    $metadata = [ordered]@{
        schema_version = 1
        product = 'vSMR'
        version = $Version
        runtime = [ordered]@{
            relative_path = 'vSMR_Data/Runtime/vSMR.Runtime.dll'
            size = [int64](Get-Item -LiteralPath $RuntimePath).Length
            sha256 = (Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $DataDirectory 'RELEASE-METADATA.json'),
        ((ConvertTo-Json $metadata -Depth 5) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
}

function Read-AndVerifyDetachedCms {
    param(
        [Parameter(Mandatory = $true)][string]$ContentPath,
        [Parameter(Mandatory = $true)][string]$SignaturePath,
        [switch]$ValidateCertificate
    )
    Add-Type -AssemblyName System.Security
    $content = [System.IO.File]::ReadAllBytes($ContentPath)
    $contentInfo = New-Object System.Security.Cryptography.Pkcs.ContentInfo -ArgumentList (,$content)
    $signedCms = New-Object System.Security.Cryptography.Pkcs.SignedCms -ArgumentList $contentInfo, $true
    $signedCms.Decode([System.IO.File]::ReadAllBytes($SignaturePath))
    $signedCms.CheckSignature(-not [bool]$ValidateCertificate)
    if ($signedCms.SignerInfos.Count -ne 1 -or $null -eq $signedCms.SignerInfos[0].Certificate) {
        throw "Detached update signature must contain exactly one signer certificate."
    }
    return $signedCms.SignerInfos[0].Certificate
}

$updateManifestBytes = [System.IO.File]::ReadAllBytes($UpdateManifestPath)
if ($updateManifestBytes.Length -eq 0 -or
    ($updateManifestBytes.Length -ge 3 -and $updateManifestBytes[0] -eq 0xEF -and
        $updateManifestBytes[1] -eq 0xBB -and $updateManifestBytes[2] -eq 0xBF)) {
    throw "Update manifest must be non-empty UTF-8 without a BOM."
}
$updateManifest = [System.Text.Encoding]::UTF8.GetString($updateManifestBytes) | ConvertFrom-Json
$expectedChannel = if ($ExpectedVersion.Contains('-')) { 'beta' } else { 'stable' }
if ([int]$updateManifest.schema_version -ne 1 -or
    $updateManifest.product -ne 'vSMR' -or
    $updateManifest.version -ne $ExpectedVersion -or
    $updateManifest.channel -ne $expectedChannel -or
    $updateManifest.publishable -isnot [bool] -or
    $updateManifest.archive.name -ne [System.IO.Path]::GetFileName($ArchivePath) -or
    [int64]$updateManifest.archive.size -ne [int64](Get-Item -LiteralPath $ArchivePath).Length -or
    [string]$updateManifest.archive.sha256 -ne $archiveHash.ToLowerInvariant() -or
    $updateManifest.loader.name -ne 'vSMR.dll' -or
    $updateManifest.loader.version -ne $ExpectedLoaderVersion -or
    [int]$updateManifest.runtime_abi -ne $ExpectedRuntimeAbi -or
    $updateManifest.runtime_relative_path -ne 'vSMR_Data/Runtime/vSMR.Runtime.dll' -or
    [string]::IsNullOrWhiteSpace([string]$updateManifest.minimum_loader_version)) {
    throw "The external update manifest is incomplete or inconsistent with the release archive."
}
$minimumLoaderVersion = $null
$packagedLoaderVersion = $null
if (-not [Version]::TryParse([string]$updateManifest.minimum_loader_version, [ref]$minimumLoaderVersion) -or
    -not [Version]::TryParse([string]$updateManifest.loader.version, [ref]$packagedLoaderVersion) -or
    $packagedLoaderVersion -lt $minimumLoaderVersion) {
    throw "Update manifest minimum_loader_version is invalid or newer than its packaged loader."
}
if ($RequireSignature -and -not [bool]$updateManifest.publishable) {
    throw "A signature-required update manifest must explicitly set publishable=true."
}
if ([bool]$updateManifest.publishable) {
    $RequireSignature = $true
    if (-not (Test-Path -LiteralPath $UpdateSignaturePath -PathType Leaf)) {
        throw "A publishable update manifest requires its detached CMS signature."
    }
}
$updateSigner = $null
if (Test-Path -LiteralPath $UpdateSignaturePath -PathType Leaf) {
    if (-not [bool]$updateManifest.publishable) {
        throw "A non-publishable update manifest must never carry a detached production signature."
    }
    $updateSigner = Read-AndVerifyDetachedCms `
        -ContentPath $UpdateManifestPath `
        -SignaturePath $UpdateSignaturePath `
        -ValidateCertificate:$RequireSignature
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($name)) {
            continue
        }
        if ($name.StartsWith('/') -or [System.IO.Path]::IsPathRooted($name) -or
            @($name.Split('/') | Where-Object { $_ -eq '..' }).Count -gt 0) {
            throw "Unsafe archive entry: $name"
        }
    }
}
finally {
    $zip.Dispose()
}

$extractRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-release-verify-" + [Guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($extractRoot) | Out-Null
try {
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $extractRoot

    $rootEntries = @(Get-ChildItem -LiteralPath $extractRoot -Force | Sort-Object Name)
    $rootNames = @($rootEntries | ForEach-Object Name)
    if (($rootNames -join '|') -ne 'vSMR.dll|vSMR_Data') {
        throw "Package root must contain only vSMR.dll and vSMR_Data; found: $($rootNames -join ', ')"
    }
    if (-not ($rootEntries | Where-Object { $_.Name -eq 'vSMR_Data' -and $_.PSIsContainer })) {
        throw "vSMR_Data is not a directory."
    }

    $dllPath = Join-Path $extractRoot "vSMR.dll"
    $dataPath = Join-Path $extractRoot "vSMR_Data"
    $runtimePath = Join-Path $dataPath "Runtime\vSMR.Runtime.dll"
    $crashHandlerPath = Join-Path $dataPath "CrashReporter\vSMRCrashHandler.dll"
    Assert-File $dllPath

    $requiredRelativeFiles = @(
        'vSMR_Profiles.json',
        'ICAO_Aircraft.json',
        'Audio\Alarm.wav',
        'Audio\Ding.wav',
        'vSMR_webUI\index.html',
        'vSMR_webUI\styles.css',
        'vSMR_webUI\app.js',
        'vSMR_webUI\data.js',
        'vSMR_webUI\defaults\vSMR_Profiles.json',
        'AVISO\LFPG.geojson',
        'Runtime\vSMR.Runtime.dll',
        'CrashReporter\vSMRCrashHandler.dll',
        'Licenses\vSMR.txt',
        'Licenses\RapidJSON.txt',
        'Licenses\Microsoft.WebView2-LICENSE.txt',
        'Licenses\Microsoft.WebView2-NOTICE.txt',
        'Licenses\DEPENDENCIES.md',
        'Licenses\ASSET_PROVENANCE.md',
        'Tools\install_vsmr.ps1',
        'Tools\restore_vsmr_backup.ps1',
        'RELEASE-METADATA.json',
        'SHA256SUMS.txt'
    )
    foreach ($relativePath in $requiredRelativeFiles) {
        Assert-File (Join-Path $dataPath $relativePath)
    }

    $packagedAvisoFiles = @(
        Get-ChildItem -LiteralPath (Join-Path $dataPath 'AVISO') -Filter '*.geojson' -File
    )
    if ($packagedAvisoFiles.Count -eq 0) {
        throw 'The package does not contain any airport AVISO defaults.'
    }
    $nonCanonicalAvisoFiles = @(
        $packagedAvisoFiles | Where-Object {
            $_.Name -notmatch '^[A-Za-z0-9]{4}\.geojson$'
        }
    )
    if ($nonCanonicalAvisoFiles.Count -ne 0) {
        throw "The package contains noncanonical AVISO defaults: $($nonCanonicalAvisoFiles.Name -join ', ')."
    }

    $forbidden = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File | Where-Object {
        $_.Extension -in @('.pdb', '.lib', '.exp', '.obj', '.pch', '.ilk', '.log', '.bak', '.tmp', '.dmp')
    })
    if ($forbidden.Count -gt 0) {
        throw "Development or user files leaked into the package: $($forbidden.FullName -join ', ')"
    }

    $packagedDllLocations = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter '*.dll' |
        ForEach-Object { $_.FullName.Substring($extractRoot.Length).TrimStart([char[]]"\/").Replace('\', '/') } |
        Sort-Object)
    if (($packagedDllLocations -join '|') -ne 'vSMR.dll|vSMR_Data/CrashReporter/vSMRCrashHandler.dll|vSMR_Data/Runtime/vSMR.Runtime.dll') {
        throw "Package DLL allowlist mismatch: $($packagedDllLocations -join ', ')"
    }

    foreach ($binary in @($dllPath, $runtimePath, $crashHandlerPath)) {
        $machine = Get-PeMachine $binary
        if ($machine -ne 0x014C) {
            throw ("$([System.IO.Path]::GetFileName($binary)) is not Win32/x86 (PE machine 0x{0:X4})." -f $machine)
        }
    }

    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($dllPath)
    if (-not (Test-VersionEquals $versionInfo.FileVersion $ExpectedLoaderVersion) -or
        $versionInfo.ProductVersion -ne $ExpectedVersion) {
        throw "Loader version mismatch. FileVersion='$($versionInfo.FileVersion)' (expected '$ExpectedLoaderVersion'), ProductVersion='$($versionInfo.ProductVersion)' (expected '$ExpectedVersion')."
    }
    $runtimeVersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($runtimePath)
    if ($runtimeVersionInfo.FileVersion -ne $ExpectedVersion -or $runtimeVersionInfo.ProductVersion -ne $ExpectedVersion) {
        throw "Runtime version mismatch. FileVersion='$($runtimeVersionInfo.FileVersion)', ProductVersion='$($runtimeVersionInfo.ProductVersion)', expected '$ExpectedVersion'."
    }
    $crashHandlerVersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($crashHandlerPath)
    if ($crashHandlerVersionInfo.FileVersion -ne $ExpectedVersion -or $crashHandlerVersionInfo.ProductVersion -ne $ExpectedVersion) {
        throw "Crash-handler version mismatch. FileVersion='$($crashHandlerVersionInfo.FileVersion)', ProductVersion='$($crashHandlerVersionInfo.ProductVersion)', expected '$ExpectedVersion'."
    }
    $expectedCrashHandlerExports = @(
        'OutOfProcessExceptionEventCallback',
        'OutOfProcessExceptionEventDebuggerLaunchCallback',
        'OutOfProcessExceptionEventSignatureCallback'
    ) | Sort-Object
    $actualCrashHandlerExports = @(Get-PeExportNames $crashHandlerPath | Sort-Object)
    if (($actualCrashHandlerExports -join '|') -cne ($expectedCrashHandlerExports -join '|')) {
        throw "Packaged crash handler does not expose the exact three-name WER ABI."
    }
    $expectedRuntimeExports = @('VsmrRuntimeCreate', 'VsmrRuntimeGetAbiVersion', 'VsmrRuntimeShutdown') | Sort-Object
    $actualRuntimeExports = @(Get-PeExportNames $runtimePath | Sort-Object)
    if (($actualRuntimeExports -join '|') -cne ($expectedRuntimeExports -join '|')) {
        throw "Packaged runtime does not expose the exact three-name loader ABI."
    }
    $expectedLoaderExports = @(
        '?EuroScopePlugInExit@@YAXXZ',
        '?EuroScopePlugInInit@@YAXPAPAVCPlugIn@EuroScopePlugIn@@@Z'
    )
    $loaderExports = @(Get-PeExportNames $dllPath | Sort-Object)
    if (($loaderExports -join '|') -cne (($expectedLoaderExports | Sort-Object) -join '|')) {
        throw "Packaged loader must expose only the exact decorated x86 EuroScope SDK entry points."
    }
    if ($RequireSignature) {
        foreach ($binary in @($dllPath, $runtimePath, $crashHandlerPath)) {
            $signature = Get-AuthenticodeSignature -LiteralPath $binary
            if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
                throw "A valid Authenticode signature is required, but packaged $([System.IO.Path]::GetFileName($binary)) status is '$($signature.Status)'."
            }
        }
    }

    $packagedLoaderHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedRuntimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([string]$updateManifest.loader.sha256 -ne $packagedLoaderHash -or
        [int64]$updateManifest.loader.size -ne [int64](Get-Item -LiteralPath $dllPath).Length) {
        throw "Update manifest loader hash or size does not match the packaged loader."
    }

    $metadataPath = Join-Path $dataPath "RELEASE-METADATA.json"
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ($metadata.product -ne 'vSMR' -or $metadata.version -ne $ExpectedVersion -or
        $metadata.channel -ne $expectedChannel -or [string]::IsNullOrWhiteSpace([string]$metadata.git_commit) -or
        $metadata.runtime.version -ne $ExpectedVersion -or
        [int]$metadata.runtime.abi -ne $ExpectedRuntimeAbi -or
        $metadata.runtime.relative_path -ne 'vSMR_Data/Runtime/vSMR.Runtime.dll' -or
        [int64]$metadata.runtime.size -ne [int64](Get-Item -LiteralPath $runtimePath).Length -or
        [string]$metadata.runtime.sha256 -ne $packagedRuntimeHash -or
        $metadata.loader.version -ne $ExpectedLoaderVersion -or
        [int]$metadata.loader.runtime_abi -ne $ExpectedRuntimeAbi -or
        $metadata.loader.relative_path -ne 'vSMR.dll' -or
        [int64]$metadata.loader.size -ne [int64](Get-Item -LiteralPath $dllPath).Length -or
        [string]$metadata.loader.sha256 -ne $packagedLoaderHash -or
        $metadata.automatic_update.minimum_loader_version -ne $updateManifest.minimum_loader_version -or
        $metadata.automatic_update.publishable -isnot [bool] -or
        [bool]$metadata.automatic_update.publishable -ne [bool]$updateManifest.publishable -or
        $null -eq $metadata.authenticode.runtime -or
        $null -eq $metadata.authenticode.crash_handler) {
        throw "RELEASE-METADATA.json is incomplete or inconsistent."
    }
    if ($RequireSignature -and (-not [bool]$metadata.publishable -or
        -not [bool]$metadata.automatic_update.publishable)) {
        throw "A remotely publishable archive must mark both source and automatic-update provenance as publishable."
    }
    $configuredSignerPin = ([string]$metadata.automatic_update.signer_cert_der_sha256).ToLowerInvariant()
    if ($null -ne $updateSigner) {
        $cmsSignerPin = Get-CertificateDerSha256 $updateSigner
        if ([string]::IsNullOrWhiteSpace($configuredSignerPin) -or $cmsSignerPin -ne $configuredSignerPin) {
            throw "Detached update manifest signer does not match RELEASE-METADATA.json's certificate pin."
        }
        $loaderSignature = Get-AuthenticodeSignature -LiteralPath $dllPath
        if ($loaderSignature.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and
            $null -ne $loaderSignature.SignerCertificate -and
            (Get-CertificateDerSha256 $loaderSignature.SignerCertificate) -ne $cmsSignerPin) {
            throw "Detached update manifest signer does not match the loader's Authenticode signer."
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($configuredSignerPin) -and $RequireSignature) {
        throw "Release metadata declares an updater signer pin but the detached signature is missing."
    }
    $commitIsVerified = [string]$metadata.git_commit -match '^[0-9a-fA-F]{40}$'
    $sourceIsClean = $metadata.source_dirty -is [bool] -and -not [bool]$metadata.source_dirty
    $markedPublishable = $metadata.publishable -is [bool] -and [bool]$metadata.publishable
    if (-not $AllowNonPublishable -and
        (-not $commitIsVerified -or -not $sourceIsClean -or -not $markedPublishable)) {
        throw "The archive is not publishable: it must identify a verified 40-character Git commit and a clean source tree."
    }

    $manifestPath = Join-Path $dataPath "SHA256SUMS.txt"
    $manifestEntries = @{}
    foreach ($line in @(Get-Content -LiteralPath $manifestPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
            throw "Invalid SHA256SUMS.txt line: $line"
        }
        $relativePath = $Matches[2].Replace('\', '/')
        if ($manifestEntries.ContainsKey($relativePath)) {
            throw "Duplicate SHA256 manifest entry: $relativePath"
        }
        $manifestEntries[$relativePath] = $Matches[1]
    }
    if (-not $manifestEntries.ContainsKey('vSMR_Data/CrashReporter/vSMRCrashHandler.dll')) {
        throw "SHA256SUMS.txt does not protect the WER crash handler."
    }
    if (-not $manifestEntries.ContainsKey('vSMR_Data/Runtime/vSMR.Runtime.dll') -or
        -not $manifestEntries.ContainsKey('vSMR.dll')) {
        throw "SHA256SUMS.txt does not protect the loader and canonical runtime."
    }

    $payloadFiles = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File | Where-Object {
        $_.FullName -ne $manifestPath
    })
    foreach ($file in $payloadFiles) {
        $relativePath = $file.FullName.Substring($extractRoot.Length).TrimStart([char[]]"\/").Replace('\', '/')
        if (-not $manifestEntries.ContainsKey($relativePath)) {
            throw "File is missing from SHA256SUMS.txt: $relativePath"
        }
        $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $manifestEntries[$relativePath]) {
            throw "SHA-256 mismatch for $relativePath."
        }
        $manifestEntries.Remove($relativePath)
    }
    if ($manifestEntries.Count -ne 0) {
        throw "SHA256SUMS.txt references missing files: $($manifestEntries.Keys -join ', ')"
    }

    # Exercise the shipped upgrade/rollback path against an existing user-data
    # tree. The installer must replace immutable assets while preserving data by
    # default, and the recorded backup must restore the exact prior tree.
    $installTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-install-verify-" + [Guid]::NewGuid().ToString("N"))
    [System.IO.Directory]::CreateDirectory((Join-Path $installTarget "vSMR_Data\AVISO")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $installTarget "vSMR_Data\vSMR_webUI")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $installTarget "vSMR_Data\CrashReporter")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $installTarget "vSMR_Data\Runtime")) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $installTarget "vSMR.dll"), "old-dll")
    $installedCrashHandlerPath = Join-Path $installTarget "vSMR_Data\CrashReporter\vSMRCrashHandler.dll"
    [System.IO.File]::WriteAllText($installedCrashHandlerPath, "old-crash-handler")
    Copy-Item -LiteralPath $crashHandlerPath -Destination (Join-Path $installTarget "vSMR_Data\Runtime\vSMR.Runtime.dll")
    Write-TestReleaseMetadata `
        -DataDirectory (Join-Path $installTarget "vSMR_Data") `
        -RuntimePath (Join-Path $installTarget "vSMR_Data\Runtime\vSMR.Runtime.dll")
    $packagedCrashHandlerHash = (Get-FileHash -LiteralPath $crashHandlerPath -Algorithm SHA256).Hash
    [System.IO.File]::WriteAllText(
        (Join-Path $installTarget "vSMR_Data\vSMR_Profiles.json"),
        '[{"name":"User","schema_version":2,"labels":{},"targets":{}}]')
    [System.IO.File]::WriteAllText((Join-Path $installTarget "vSMR_Data\AVISO\TEST.geojson"), "user-aviso")
    [System.IO.File]::WriteAllText((Join-Path $installTarget "vSMR_Data\user-import.bin"), "user-import")
    [System.IO.File]::WriteAllText((Join-Path $installTarget "vSMR_Data\vSMR_webUI\index.html"), "stale-ui")
    try {
        $filesystemRoot = [System.IO.Path]::GetPathRoot($installTarget)
        $rootRejected = $false
        try {
            & (Join-Path $dataPath "Tools\install_vsmr.ps1") -DestinationDirectory $filesystemRoot -WhatIf
        }
        catch { $rootRejected = $_.Exception.Message -match 'filesystem root' }
        if (-not $rootRejected) { throw "Install helper accepted a filesystem root destination." }
        $rootRejected = $false
        try {
            & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
                -DestinationDirectory $filesystemRoot `
                -BackupDirectory $filesystemRoot `
                -WhatIf
        }
        catch { $rootRejected = $_.Exception.Message -match 'filesystem root' }
        if (-not $rootRejected) { throw "Rollback helper accepted a filesystem root path." }

        $nestedPackageDestination = Join-Path $dataPath "invalid-install-destination"
        [System.IO.Directory]::CreateDirectory($nestedPackageDestination) | Out-Null
        $packageOverlapRejected = $false
        try {
            & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
                -DestinationDirectory $nestedPackageDestination `
                -WhatIf
        }
        catch { $packageOverlapRejected = $_.Exception.Message -match 'extracted package' }
        finally { Remove-Item -LiteralPath $nestedPackageDestination -Recurse -Force }
        if (-not $packageOverlapRejected) { throw "Install helper accepted a destination below its extracted package." }

        $packageBackupRejected = $false
        try {
            & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
                -DestinationDirectory $installTarget `
                -BackupRoot (Join-Path $dataPath "invalid-package-backups") `
                -WhatIf
        }
        catch { $packageBackupRejected = $_.Exception.Message -match 'inside the extracted package' }
        if (-not $packageBackupRejected) { throw "Install helper accepted a BackupRoot inside its extracted package." }

        $dataBackupRejected = $false
        try {
            & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
                -DestinationDirectory $installTarget `
                -BackupRoot (Join-Path $installTarget "vSMR_Data") `
                -WhatIf
        }
        catch { $dataBackupRejected = $_.Exception.Message -match 'BackupRoot cannot be inside' }
        if (-not $dataBackupRejected) { throw "Install helper accepted vSMR_Data itself as BackupRoot." }
        $dataBackupRejected = $false
        try {
            & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
                -DestinationDirectory $installTarget `
                -BackupRoot (Join-Path $installTarget "vSMR_Data\nested-backups") `
                -WhatIf
        }
        catch { $dataBackupRejected = $_.Exception.Message -match 'BackupRoot cannot be inside' }
        if (-not $dataBackupRejected) { throw "Install helper accepted a BackupRoot below vSMR_Data." }

		$restoreOverlapMetadata = [ordered]@{
			schema_version = 1
			kind = 'vSMR complete pre-install backup'
			destination = $installTarget
			had_dll = $false
			had_data = $false
		}
		$restoreOverlapJson = (ConvertTo-Json $restoreOverlapMetadata -Depth 4) + "`n"
		$activeDataRoot = Join-Path $installTarget 'vSMR_Data'
		$activeDataMetadata = Join-Path $activeDataRoot 'BACKUP-METADATA.json'
		[System.IO.File]::WriteAllText($activeDataMetadata, $restoreOverlapJson, (New-Object System.Text.UTF8Encoding($false)))
		$restoreOverlapRejected = $false
		try {
			& (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
				-DestinationDirectory $installTarget `
				-BackupDirectory $activeDataRoot `
				-WhatIf
		}
		catch { $restoreOverlapRejected = $_.Exception.Message -match 'cannot overlap' }
		finally { Remove-Item -LiteralPath $activeDataMetadata -Force }
		if (-not $restoreOverlapRejected) { throw "Rollback helper accepted vSMR_Data itself as BackupDirectory." }

		$nestedRestoreBackup = Join-Path $activeDataRoot 'nested-rollback'
		[System.IO.Directory]::CreateDirectory($nestedRestoreBackup) | Out-Null
		[System.IO.File]::WriteAllText(
			(Join-Path $nestedRestoreBackup 'BACKUP-METADATA.json'),
			$restoreOverlapJson,
			(New-Object System.Text.UTF8Encoding($false)))
		$restoreOverlapRejected = $false
		try {
			& (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
				-DestinationDirectory $installTarget `
				-BackupDirectory $nestedRestoreBackup `
				-WhatIf
		}
		catch { $restoreOverlapRejected = $_.Exception.Message -match 'cannot overlap' }
		finally { Remove-Item -LiteralPath $nestedRestoreBackup -Recurse -Force }
		if (-not $restoreOverlapRejected) { throw "Rollback helper accepted a BackupDirectory below vSMR_Data." }

        $reverseOverlapBackup = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-reverse-overlap-" + [Guid]::NewGuid().ToString("N"))
        $reverseOverlapDestination = Join-Path $reverseOverlapBackup "vSMR_Data\nested-destination"
        [System.IO.Directory]::CreateDirectory($reverseOverlapDestination) | Out-Null
        $reverseOverlapRejected = $false
        try {
            & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
                -DestinationDirectory $reverseOverlapDestination `
                -BackupDirectory $reverseOverlapBackup `
                -WhatIf
        }
        catch { $reverseOverlapRejected = $_.Exception.Message -match 'cannot overlap' }
        finally { Remove-Item -LiteralPath $reverseOverlapBackup -Recurse -Force }
        if (-not $reverseOverlapRejected) { throw "Rollback helper accepted an active vSMR_Data tree below its backup payload." }

        & (Join-Path $dataPath "Tools\install_vsmr.ps1") -DestinationDirectory $installTarget -WhatIf
        if ((Test-Path -LiteralPath (Join-Path $installTarget "vSMR_Backups")) -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR.dll") -Raw) -ne 'old-dll' -or
            (Get-Content -LiteralPath $installedCrashHandlerPath -Raw) -ne 'old-crash-handler') {
            throw "Install helper mutated the destination during -WhatIf."
        }

        & (Join-Path $dataPath "Tools\install_vsmr.ps1") -DestinationDirectory $installTarget -Confirm:$false
        $installation = Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\INSTALLATION.json") -Raw | ConvertFrom-Json
        if (-not $installation.user_data_preserved -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\AVISO\TEST.geojson") -Raw) -ne 'user-aviso' -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\user-import.bin") -Raw) -ne 'user-import' -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\vSMR_Profiles.json") -Raw) -notmatch '"User"' -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\vSMR_webUI\index.html") -Raw) -eq 'stale-ui' -or
            (Get-FileHash -LiteralPath $installedCrashHandlerPath -Algorithm SHA256).Hash -ne $packagedCrashHandlerHash) {
            throw "Install helper did not preserve user data and replace immutable assets correctly."
        }
        $rollbackBackup = [string]$installation.rollback_backup
        Assert-File (Join-Path $rollbackBackup "BACKUP-METADATA.json")
        $transactionOutcome = Get-Content -LiteralPath (Join-Path $rollbackBackup "TRANSACTION-OUTCOME.json") -Raw | ConvertFrom-Json
        if ($transactionOutcome.status -ne 'committed' -or
            $transactionOutcome.transaction_id -ne $installation.transaction_id) {
            throw "Install helper did not record a durable committed transaction outcome."
        }
        if ((Get-Content -LiteralPath (Join-Path $rollbackBackup "vSMR_Data\CrashReporter\vSMRCrashHandler.dll") -Raw) -ne 'old-crash-handler') {
            throw "Install helper did not back up the previous crash handler."
        }

		$originalInstalledDllHash = (Get-FileHash -LiteralPath (Join-Path $installTarget "vSMR.dll") -Algorithm SHA256).Hash
		foreach ($invalidCase in @('missing', 'wrong-type')) {
			$invalidMetadataBackup = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-invalid-backup-" + [Guid]::NewGuid().ToString("N"))
			[System.IO.Directory]::CreateDirectory($invalidMetadataBackup) | Out-Null
			try {
				$invalidMetadata = [ordered]@{
					schema_version = 1
					kind = 'vSMR complete pre-install backup'
					destination = $installTarget
					had_dll = if ($invalidCase -eq 'wrong-type') { 'true' } else { $true }
					had_data = $true
				}
				if ($invalidCase -eq 'missing') { $invalidMetadata.Remove('had_dll') }
				[System.IO.File]::WriteAllText(
					(Join-Path $invalidMetadataBackup 'BACKUP-METADATA.json'),
					((ConvertTo-Json $invalidMetadata -Depth 4) + "`n"),
					(New-Object System.Text.UTF8Encoding($false)))
				$invalidMetadataRejected = $false
				try {
					& (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
						-DestinationDirectory $installTarget `
						-BackupDirectory $invalidMetadataBackup `
						-Confirm:$false
				}
				catch { $invalidMetadataRejected = $_.Exception.Message -match 'metadata' }
				if (-not $invalidMetadataRejected) { throw "Rollback helper accepted $invalidCase backup metadata." }
				if ((Get-FileHash -LiteralPath (Join-Path $installTarget "vSMR.dll") -Algorithm SHA256).Hash -ne $originalInstalledDllHash) {
					throw "Rollback helper mutated the installation after rejecting $invalidCase backup metadata."
				}
			}
			finally { Remove-Item -LiteralPath $invalidMetadataBackup -Recurse -Force }
		}

        $wrongTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-wrong-restore-" + [Guid]::NewGuid().ToString("N"))
        [System.IO.Directory]::CreateDirectory($wrongTarget) | Out-Null
        $wrongDestinationRejected = $false
        try {
            & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
                -DestinationDirectory $wrongTarget `
                -BackupDirectory $rollbackBackup `
                -Confirm:$false
        }
        catch { $wrongDestinationRejected = $_.Exception.Message -match 'backup belongs to' }
        finally { Remove-Item -LiteralPath $wrongTarget -Recurse -Force }
        if (-not $wrongDestinationRejected) { throw "Rollback helper accepted a backup for another destination." }
        & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
            -DestinationDirectory $installTarget `
            -BackupDirectory $rollbackBackup `
            -Confirm:$false
        if ((Get-Content -LiteralPath (Join-Path $installTarget "vSMR.dll") -Raw) -ne 'old-dll' -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\vSMR_webUI\index.html") -Raw) -ne 'stale-ui' -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\user-import.bin") -Raw) -ne 'user-import' -or
            (Get-Content -LiteralPath $installedCrashHandlerPath -Raw) -ne 'old-crash-handler') {
            throw "Rollback helper did not restore the pre-install tree."
        }
        $safetyBackup = Get-ChildItem -LiteralPath (Split-Path -Parent $rollbackBackup) -Directory |
            Where-Object { $_.Name -like 'vSMR-before-rollback-*' } |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        if ($null -eq $safetyBackup) { throw "Rollback helper did not create a safety backup." }
        $rollbackOutcome = Get-Content -LiteralPath (Join-Path $safetyBackup.FullName "TRANSACTION-OUTCOME.json") -Raw | ConvertFrom-Json
        if ($rollbackOutcome.status -ne 'committed' -or [bool]$rollbackOutcome.loader_preserved) {
            throw "Rollback helper did not record a durable committed full-restore outcome."
        }
        & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
            -DestinationDirectory $installTarget `
            -BackupDirectory $safetyBackup.FullName `
            -Confirm:$false
        if ((Get-Content -LiteralPath (Join-Path $installTarget "vSMR.dll") -Raw) -eq 'old-dll' -or
            (Get-FileHash -LiteralPath $installedCrashHandlerPath -Algorithm SHA256).Hash -ne $packagedCrashHandlerHash) {
            throw "Rollback safety backup could not restore the pre-rollback installation."
        }
        & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
            -DestinationDirectory $installTarget `
            -ReplaceUserData `
            -Confirm:$false
        if ((Test-Path -LiteralPath (Join-Path $installTarget "vSMR_Data\user-import.bin")) -or
            (Test-Path -LiteralPath (Join-Path $installTarget "vSMR_Data\AVISO\TEST.geojson")) -or
            (Get-Content -LiteralPath (Join-Path $installTarget "vSMR_Data\vSMR_Profiles.json") -Raw) -match '"User"' -or
            (Get-FileHash -LiteralPath $installedCrashHandlerPath -Algorithm SHA256).Hash -ne $packagedCrashHandlerHash) {
            throw "Install helper ignored explicit -ReplaceUserData."
        }
    }
    finally {
        if (Test-Path -LiteralPath $installTarget) {
            Remove-Item -LiteralPath $installTarget -Recurse -Force
        }
    }

    # Exercise the same-launch updater path separately. It must validate the
    # complete package, replace immutable runtime/data, and never mutate the
    # already-loaded root loader during either install or rollback.
    $runtimeInstallTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-runtime-install-verify-" + [Guid]::NewGuid().ToString("N"))
    [System.IO.Directory]::CreateDirectory((Join-Path $runtimeInstallTarget "vSMR_Data\Runtime")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $runtimeInstallTarget "vSMR_Data\AVISO")) | Out-Null
    $runtimeTargetLoader = Join-Path $runtimeInstallTarget "vSMR.dll"
    $runtimeTargetRuntime = Join-Path $runtimeInstallTarget "vSMR_Data\Runtime\vSMR.Runtime.dll"
    [System.IO.File]::WriteAllText($runtimeTargetLoader, "preserved-loader")
    Copy-Item -LiteralPath $crashHandlerPath -Destination $runtimeTargetRuntime
    $previousRuntimeHash = (Get-FileHash -LiteralPath $runtimeTargetRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-TestReleaseMetadata `
        -DataDirectory (Join-Path $runtimeInstallTarget "vSMR_Data") `
        -RuntimePath $runtimeTargetRuntime
    [System.IO.File]::WriteAllText((Join-Path $runtimeInstallTarget "vSMR_Data\AVISO\USER.geojson"), "user-aviso")
    $preservedLoaderHash = (Get-FileHash -LiteralPath $runtimeTargetLoader -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedRuntimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant()
    try {
        & (Join-Path $dataPath "Tools\install_vsmr.ps1") `
            -DestinationDirectory $runtimeInstallTarget `
            -PreserveLoader `
            -Confirm:$false
        $runtimeInstallation = Get-Content -LiteralPath (Join-Path $runtimeInstallTarget "vSMR_Data\INSTALLATION.json") -Raw | ConvertFrom-Json
        if ((Get-FileHash -LiteralPath $runtimeTargetLoader -Algorithm SHA256).Hash.ToLowerInvariant() -ne $preservedLoaderHash -or
            (Get-FileHash -LiteralPath $runtimeTargetRuntime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $packagedRuntimeHash -or
            (Get-Content -LiteralPath (Join-Path $runtimeInstallTarget "vSMR_Data\AVISO\USER.geojson") -Raw) -ne 'user-aviso' -or
            -not [bool]$runtimeInstallation.loader_preserved -or
            [string]$runtimeInstallation.validation_scope -ne 'runtime_and_data' -or
            [string]$runtimeInstallation.package_loader_sha256 -ne $packagedLoaderHash -or
            [string]$runtimeInstallation.installed_loader_sha256 -ne $preservedLoaderHash -or
            [bool]$runtimeInstallation.loader_matches_package) {
            throw "Runtime-update install did not preserve the loader, replace the runtime, preserve user data, and record scoped validation correctly."
        }
        $runtimeRollbackBackup = [string]$runtimeInstallation.rollback_backup
        $runtimeTransactionOutcome = Get-Content -LiteralPath (Join-Path $runtimeRollbackBackup "TRANSACTION-OUTCOME.json") -Raw | ConvertFrom-Json
        if ($runtimeTransactionOutcome.status -ne 'committed' -or
            $runtimeTransactionOutcome.transaction_id -ne $runtimeInstallation.transaction_id -or
            -not [bool]$runtimeTransactionOutcome.loader_preserved) {
            throw "Runtime-update install did not record a durable committed preserve-loader outcome."
        }
        if ((Get-Content -LiteralPath (Join-Path $runtimeRollbackBackup "vSMR.dll") -Raw) -ne 'preserved-loader' -or
            (Get-FileHash -LiteralPath (Join-Path $runtimeRollbackBackup "vSMR_Data\Runtime\vSMR.Runtime.dll") -Algorithm SHA256).Hash.ToLowerInvariant() -ne $previousRuntimeHash) {
            throw "Runtime-update install did not create a complete rollback backup."
        }

        $tamperedRuntimeBackup = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-tampered-rollback-" + [Guid]::NewGuid().ToString("N"))
        Copy-Item -LiteralPath $runtimeRollbackBackup -Destination $tamperedRuntimeBackup -Recurse
        try {
            [System.IO.File]::AppendAllText(
                (Join-Path $tamperedRuntimeBackup "vSMR_Data\Runtime\vSMR.Runtime.dll"),
                "tampered")
            $tamperedRejected = $false
            try {
                & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
                    -DestinationDirectory $runtimeInstallTarget `
                    -BackupDirectory $tamperedRuntimeBackup `
                    -RuntimeUpdate `
                    -Confirm:$false
            }
            catch { $tamperedRejected = $_.Exception.Message -match 'canonical runtime' }
            if (-not $tamperedRejected -or
                (Get-FileHash -LiteralPath $runtimeTargetLoader -Algorithm SHA256).Hash.ToLowerInvariant() -ne $preservedLoaderHash -or
                (Get-FileHash -LiteralPath $runtimeTargetRuntime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $packagedRuntimeHash) {
                throw "Runtime-update rollback did not reject a tampered backup before mutating the active installation."
            }
        }
        finally {
            if (Test-Path -LiteralPath $tamperedRuntimeBackup) {
                Remove-Item -LiteralPath $tamperedRuntimeBackup -Recurse -Force
            }
        }

        & (Join-Path $dataPath "Tools\restore_vsmr_backup.ps1") `
            -DestinationDirectory $runtimeInstallTarget `
            -BackupDirectory $runtimeRollbackBackup `
            -RuntimeUpdate `
            -Confirm:$false
        if ((Get-FileHash -LiteralPath $runtimeTargetLoader -Algorithm SHA256).Hash.ToLowerInvariant() -ne $preservedLoaderHash -or
            (Get-FileHash -LiteralPath $runtimeTargetRuntime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $previousRuntimeHash -or
            (Get-Content -LiteralPath (Join-Path $runtimeInstallTarget "vSMR_Data\AVISO\USER.geojson") -Raw) -ne 'user-aviso') {
            throw "Runtime-update rollback did not preserve the loader and restore the previous runtime/data tree."
        }
        $runtimeSafetyBackup = Get-ChildItem -LiteralPath (Split-Path -Parent $runtimeRollbackBackup) -Directory |
            Where-Object { $_.Name -like 'vSMR-before-rollback-*' } |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        if ($null -eq $runtimeSafetyBackup) {
            throw "Runtime-update rollback did not create a safety backup."
        }
        $runtimeRollbackOutcome = Get-Content -LiteralPath (Join-Path $runtimeSafetyBackup.FullName "TRANSACTION-OUTCOME.json") -Raw | ConvertFrom-Json
        if ($runtimeRollbackOutcome.status -ne 'committed' -or -not [bool]$runtimeRollbackOutcome.loader_preserved) {
            throw "Runtime-update rollback did not record a durable committed preserve-loader outcome."
        }
    }
    finally {
        if (Test-Path -LiteralPath $runtimeInstallTarget) {
            Remove-Item -LiteralPath $runtimeInstallTarget -Recurse -Force
        }
    }

    Write-Host "Verified vSMR $ExpectedVersion Win32 release package ($($payloadFiles.Count) hashed files)."
}
finally {
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
}
