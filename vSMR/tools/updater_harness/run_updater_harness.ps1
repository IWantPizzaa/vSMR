#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$MSBuildPath = "",
    [string]$ScratchDirectory = "",
    [ValidatePattern('^(auto|v\d{3})$')]
    [string]$Toolset = "auto",
    [switch]$SkipBuild,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$HarnessRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$WorkspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $HarnessRoot "..\..\.."))

if ([string]::IsNullOrWhiteSpace($MSBuildPath)) {
    $candidate = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $MSBuildPath = $candidate
    }
    else {
        $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
        if ($null -eq $command) { throw "MSBuild.exe was not found." }
        $MSBuildPath = $command.Source
    }
}
$MSBuildPath = [System.IO.Path]::GetFullPath($MSBuildPath)

if ([string]::IsNullOrWhiteSpace($ScratchDirectory)) {
    $ScratchDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-updater-harness-" + [Guid]::NewGuid().ToString("N"))
}
$ScratchDirectory = [System.IO.Path]::GetFullPath($ScratchDirectory)
if ([System.IO.Path]::GetFileName($ScratchDirectory) -notlike "vsmr-updater-harness-*") {
    throw "ScratchDirectory leaf must start with 'vsmr-updater-harness-'."
}
[System.IO.Directory]::CreateDirectory($ScratchDirectory) | Out-Null
$Succeeded = $false
$HarnessCertificates = @()

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function New-ZipEntryArchive([string]$Path, [string[]]$EntryNames) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Path) { [System.IO.File]::Delete($Path) }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew)
    $zip = New-Object System.IO.Compression.ZipArchive(
        $stream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($entryName in $EntryNames) {
            $entry = $zip.CreateEntry($entryName)
            $writer = New-Object System.IO.StreamWriter($entry.Open(), $Utf8NoBom)
            try { $writer.Write("offline updater harness") } finally { $writer.Dispose() }
        }
    }
    finally {
        $zip.Dispose()
        $stream.Dispose()
    }
}

function New-HarnessSigningCertificate([string]$Role) {
    $subject = "CN=vSMR Updater Harness $Role $([Guid]::NewGuid().ToString('N'))"
    $certificate = New-SelfSignedCertificate `
        -Subject $subject `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -Type CodeSigningCert `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddHours(2)
    if ($null -eq $certificate -or -not $certificate.HasPrivateKey) {
        throw "Could not create the ephemeral $Role CMS certificate."
    }
    $script:HarnessCertificates += $certificate
    return $certificate
}

function Get-CertificateDerSha256([System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate) {
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return (($sha256.ComputeHash($Certificate.RawData) | ForEach-Object {
            $_.ToString("x2")
        }) -join "")
    }
    finally {
        $sha256.Dispose()
    }
}

try {
    $solutionDirArgument = "/p:SolutionDir=$WorkspaceRoot\"
    $toolsetArguments = @()
    if ($Toolset -ne 'auto') { $toolsetArguments += "/p:PlatformToolset=$Toolset" }
    if (-not $SkipBuild) {
        & $MSBuildPath (Join-Path $WorkspaceRoot "vSMR\src\bootstrap\loader\vSMRLoader.vcxproj") `
            /t:Build /p:Configuration=Debug /p:Platform=Win32 /p:BuildProjectReferences=false `
            $solutionDirArgument $toolsetArguments /m:1 /nodeReuse:false /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "The Win32 loader build failed." }

        & $MSBuildPath (Join-Path $HarnessRoot "vSMRUpdaterHarness.vcxproj") `
            /t:Build /p:Configuration=Debug /p:Platform=Win32 `
            $solutionDirArgument $toolsetArguments /m:1 /nodeReuse:false /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "The updater harness build failed." }
    }

    $LoaderBinary = Join-Path $WorkspaceRoot "Debug\vSMR.dll"
    $PackageRoot = Join-Path $ScratchDirectory "base-package"
    $PackageData = Join-Path $PackageRoot "vSMR_Data"
    [System.IO.Directory]::CreateDirectory((Join-Path $PackageData "Runtime")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $PackageData "CrashReporter")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $PackageData "Tools")) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $PackageData "AVISO")) | Out-Null
    [System.IO.File]::Copy($LoaderBinary, (Join-Path $PackageRoot "vSMR.dll"), $true)
    $RuntimePath = Join-Path $PackageData "Runtime\vSMR.Runtime.dll"
    [System.IO.File]::Copy($LoaderBinary, $RuntimePath, $true)
    [System.IO.File]::AppendAllText($RuntimePath, "vsmr-offline-new-runtime", $Utf8NoBom)
    [System.IO.File]::Copy($LoaderBinary, (Join-Path $PackageData "CrashReporter\vSMRCrashHandler.dll"), $true)
    [System.IO.File]::Copy(
        (Join-Path $WorkspaceRoot "vSMR\data\Tools\install_vsmr.ps1"),
        (Join-Path $PackageData "Tools\install_vsmr.ps1"),
        $true)
    [System.IO.File]::Copy(
        (Join-Path $WorkspaceRoot "vSMR\data\Tools\restore_vsmr_backup.ps1"),
        (Join-Path $PackageData "Tools\restore_vsmr_backup.ps1"),
        $true)
    [System.IO.File]::Copy(
        (Join-Path $WorkspaceRoot "vSMR\data\airports_hp.json"),
        (Join-Path $PackageData "airports_hp.json"),
        $true)

    $PackageLoader = Join-Path $PackageRoot "vSMR.dll"
    $metadata = [ordered]@{
        schema_version = 1
        product = "vSMR"
        version = "2.0.0-beta.4"
        publishable = $true
        git_commit = "offline-updater-harness"
        loader = [ordered]@{
            relative_path = "vSMR.dll"
            version = "1.1.0"
            size = [int64](Get-Item -LiteralPath $PackageLoader).Length
            sha256 = (Get-FileHash -LiteralPath $PackageLoader -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        runtime = [ordered]@{
            relative_path = "vSMR_Data/Runtime/vSMR.Runtime.dll"
            version = "2.0.0-beta.4"
            abi = 1
            size = [int64](Get-Item -LiteralPath $RuntimePath).Length
            sha256 = (Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        automatic_update = [ordered]@{
            minimum_loader_version = "1.1.0"
            publishable = $true
        }
    }
    Write-Utf8NoBom (Join-Path $PackageData "RELEASE-METADATA.json") ((ConvertTo-Json $metadata -Depth 10) + "`n")

    $HarnessAvisoPath = Join-Path $PackageData "AVISO\TEST.geojson"
    Write-Utf8NoBom $HarnessAvisoPath '{"type":"FeatureCollection","features":[]}'
    $avisoPolicy = [ordered]@{
        schema_version = 1
        release = "2.0.0-beta.4"
        aviso = [ordered]@{
            update = "all"
            replace = @()
            delete = @()
            modified_files = "protect_setting"
        }
    }
    Write-Utf8NoBom (Join-Path $PackageData "AVISO-UPDATE-POLICY.json") ((ConvertTo-Json $avisoPolicy -Depth 5) + "`n")
    $avisoInventory = [ordered]@{
        schema_version = 1
        release = "2.0.0-beta.4"
        files = [ordered]@{
            'TEST.geojson' = (Get-FileHash -LiteralPath $HarnessAvisoPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    Write-Utf8NoBom (Join-Path $PackageData "AVISO-INVENTORY.json") ((ConvertTo-Json $avisoInventory -Depth 5) + "`n")

    $hashLines = foreach ($file in @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Sort-Object FullName)) {
        if ($file.FullName -eq (Join-Path $PackageData "SHA256SUMS.txt")) { continue }
        $relative = $file.FullName.Substring($PackageRoot.Length).TrimStart([char[]]"\/").Replace('\', '/')
        "{0}  {1}" -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    }
    Write-Utf8NoBom (Join-Path $PackageData "SHA256SUMS.txt") (($hashLines -join "`n") + "`n")

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $BaseArchive = Join-Path $ScratchDirectory "base.zip"
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $PackageRoot,
        $BaseArchive,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

    $TraversalArchive = Join-Path $ScratchDirectory "traversal.zip"
    New-ZipEntryArchive $TraversalArchive @("../escape.txt")
    $DuplicateArchive = Join-Path $ScratchDirectory "duplicate.zip"
    New-ZipEntryArchive $DuplicateArchive @("same.txt", "same.txt")
    $OversizedArchive = Join-Path $ScratchDirectory "oversized.zip"
    New-ZipEntryArchive $OversizedArchive @("large.bin")
    $oversizedBytes = [System.IO.File]::ReadAllBytes($OversizedArchive)
    $centralOffset = -1
    for ($index = 0; $index -le $oversizedBytes.Length - 4; $index++) {
        if ($oversizedBytes[$index] -eq 0x50 -and $oversizedBytes[$index + 1] -eq 0x4B -and
            $oversizedBytes[$index + 2] -eq 0x01 -and $oversizedBytes[$index + 3] -eq 0x02) {
            $centralOffset = $index
            break
        }
    }
    if ($centralOffset -lt 0) { throw "Could not create oversized ZIP fixture." }
    [BitConverter]::GetBytes([uint32]268435457).CopyTo($oversizedBytes, $centralOffset + 24)
    [System.IO.File]::WriteAllBytes($OversizedArchive, $oversizedBytes)

    # Exercise the exact native CryptVerifyDetachedMessageSignature + DER pin
    # path used by production manifests. Both private keys are ephemeral harness
    # state in CurrentUser and are removed (including keys) in the outer finally.
    Add-Type -AssemblyName System.Security
    $CmsSigningCertificate = New-HarnessSigningCertificate "signer"
    $CmsWrongCertificate = New-HarnessSigningCertificate "wrong-pin"
    $CmsContent = Join-Path $ScratchDirectory "cms-content.bin"
    $CmsSignature = Join-Path $ScratchDirectory "cms-signature.p7s"
    [System.IO.File]::WriteAllBytes(
        $CmsContent,
        [System.Text.Encoding]::UTF8.GetBytes("vSMR updater detached CMS harness content`n"))
    $CmsContentInfo = [System.Security.Cryptography.Pkcs.ContentInfo]::new(
        [System.IO.File]::ReadAllBytes($CmsContent))
    $SignedCms = [System.Security.Cryptography.Pkcs.SignedCms]::new($CmsContentInfo, $true)
    $CmsSigner = [System.Security.Cryptography.Pkcs.CmsSigner]::new($CmsSigningCertificate)
    $CmsSigner.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $SignedCms.ComputeSignature($CmsSigner, $true)
    [System.IO.File]::WriteAllBytes($CmsSignature, $SignedCms.Encode())
    $CmsSignerPin = Get-CertificateDerSha256 $CmsSigningCertificate
    $CmsWrongPin = Get-CertificateDerSha256 $CmsWrongCertificate

    $HarnessExe = Join-Path $HarnessRoot "bin\Debug\vSMRUpdaterHarness.exe"
    & $HarnessExe $WorkspaceRoot $ScratchDirectory $LoaderBinary $BaseArchive `
        $TraversalArchive $DuplicateArchive $OversizedArchive `
        $CmsContent $CmsSignature $CmsSignerPin $CmsWrongPin
    if ($LASTEXITCODE -ne 0) { throw "Offline updater harness failed. Artifacts: $ScratchDirectory" }
    $Succeeded = $true
}
finally {
    $CertificateCleanupErrors = @()
    foreach ($certificate in $HarnessCertificates) {
        try {
            $certificatePath = "Cert:\CurrentUser\My\$($certificate.Thumbprint)"
            if (Test-Path -Path $certificatePath) {
                Remove-Item -Path $certificatePath -DeleteKey -Force
            }
        }
        catch {
            $CertificateCleanupErrors += $_.Exception.Message
        }
        finally {
            $certificate.Dispose()
        }
    }
    if ($CertificateCleanupErrors.Count -ne 0) {
        $Succeeded = $false
    }
    if ($Succeeded -and -not $KeepArtifacts) {
        Remove-Item -LiteralPath $ScratchDirectory -Recurse -Force
    }
    elseif (Test-Path -LiteralPath $ScratchDirectory) {
        Write-Host "Updater harness artifacts: $ScratchDirectory"
    }
    if ($CertificateCleanupErrors.Count -ne 0) {
        throw ("Could not remove one or more ephemeral CMS certificates/private keys: " +
            ($CertificateCleanupErrors -join "; "))
    }
}
