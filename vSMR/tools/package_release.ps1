#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$BuildOutputDirectory = "",
    [string]$ArtifactsDirectory = "",
    [ValidatePattern("^\d+\.\d+\.\d+(?:-beta\.\d+)?$")]
    [string]$Version = "2.0.0-beta.2",
    [string]$Configuration = "Release",
    [string]$Platform = "Win32",
    [ValidatePattern("^(auto|v\d+)$")]
    [string]$Toolset = "auto",
    [string]$PdbPath = "",
    [string]$LoaderPdbPath = "",
    [string]$CrashHandlerPdbPath = "",
    [string]$CertificateThumbprint = "",
    [ValidatePattern("^(|[0-9a-fA-F]{64})$")]
    [string]$UpdateSignerCertSha256 = "",
    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$LoaderVersion = "1.0.0",
    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$MinimumLoaderVersion = "1.0.0",
    [ValidateRange(1, 65535)]
    [int]$RuntimeAbi = 1,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$RequireSignature,
    [switch]$AllowDirtySource,
    [switch]$ForceNonPublishable,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

if ([string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    $BuildOutputDirectory = Join-Path $RepositoryRoot "Release"
}
$BuildOutputDirectory = [System.IO.Path]::GetFullPath($BuildOutputDirectory)

if ([string]::IsNullOrWhiteSpace($ArtifactsDirectory)) {
    $ArtifactsDirectory = Join-Path $RepositoryRoot "artifacts"
}
$ArtifactsDirectory = [System.IO.Path]::GetFullPath($ArtifactsDirectory)

if ([string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $CertificateThumbprint = [string]$env:VSMR_SIGNING_CERT_THUMBPRINT
}
if ([string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)) {
    $UpdateSignerCertSha256 = [string]$env:VSMR_UPDATE_SIGNER_CERT_SHA256
}
if ($env:VSMR_REQUIRE_SIGNATURE -eq '1' -or $env:VSMR_REQUIRE_SIGNATURE -eq 'true') {
    $RequireSignature = $true
}

function Test-PathEqualOrChild {
    param([string]$Path, [string]$Parent)
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $resolvedPath.Equals($resolvedParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedPath.StartsWith($resolvedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required release file is missing: $Path"
    }
}

function Remove-SafeDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedParent
    )

    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $resolvedParent = [System.IO.Path]::GetFullPath($AllowedParent).TrimEnd('\', '/')
    $prefix = $resolvedParent + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolvedPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove staging path outside '$resolvedParent': $resolvedPath"
    }
    if (Test-Path -LiteralPath $resolvedPath) {
        Remove-Item -LiteralPath $resolvedPath -Recurse -Force
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function New-ZipArchive {
    param([string]$SourceDirectory, [string]$DestinationPath)

    if (Test-Path -LiteralPath $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }
    $items = @(Get-ChildItem -LiteralPath $SourceDirectory -Force)
    if ($items.Count -eq 0) {
        throw "Cannot create an archive from an empty directory: $SourceDirectory"
    }
    Compress-Archive -Path (Join-Path $SourceDirectory "*") -DestinationPath $DestinationPath -CompressionLevel Optimal
}

function Get-VisualStudioInstallationPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        return ""
    }

    $installation = @(
        & $vswhere -latest -products * -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } | Select-Object -First 1
    return [string]$installation
}

function Resolve-MsBuildPath {
    param([string]$VisualStudioInstallation)

    if (-not [string]::IsNullOrWhiteSpace($VisualStudioInstallation)) {
        $currentMsBuild = Join-Path $VisualStudioInstallation "MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path -LiteralPath $currentMsBuild -PathType Leaf) {
            return $currentMsBuild
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) {
        return [string]$command.Source
    }

    throw "MSBuild was not found. Install Visual Studio with the Desktop development with C++ and MFC components."
}

function Resolve-AutomaticToolset {
    param(
        [string]$VisualStudioInstallation,
        [string]$MsBuildPath,
        [string]$TargetPlatform
    )

    if ([string]::IsNullOrWhiteSpace($VisualStudioInstallation)) {
        $marker = "\MSBuild\"
        $markerIndex = $MsBuildPath.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase)
        if ($markerIndex -gt 0) {
            $VisualStudioInstallation = $MsBuildPath.Substring(0, $markerIndex)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($VisualStudioInstallation)) {
        $vcTargetsRoot = Join-Path $VisualStudioInstallation "MSBuild\Microsoft\VC"
        $toolsets = @(
            Get-ChildItem -Path (Join-Path $vcTargetsRoot "*\Platforms\$TargetPlatform\PlatformToolsets\v*") -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^v\d+$' } |
                Sort-Object @{ Expression = { [int]($_.Name.Substring(1)) }; Descending = $true }
        )
        if ($toolsets.Count -gt 0) {
            return [string]$toolsets[0].Name
        }
    }

    throw "No Visual C++ platform toolset was found for $TargetPlatform. Install the matching MSVC and MFC components or pass -Toolset explicitly."
}

function Resolve-CodeSigningCertificate {
    param([string]$Thumbprint)
    if ([string]::IsNullOrWhiteSpace($Thumbprint)) { return $null }
    $normalized = ($Thumbprint -replace '\s', '').ToUpperInvariant()
    foreach ($storePath in @('Cert:\CurrentUser\My', 'Cert:\LocalMachine\My')) {
        $certificate = Get-ChildItem -LiteralPath $storePath -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $normalized -and $_.HasPrivateKey } |
            Select-Object -First 1
        if ($null -ne $certificate) { return $certificate }
    }
    throw "The configured code-signing certificate with thumbprint '$normalized' and a private key was not found."
}

function Get-CertificateDerSha256 {
    param([Parameter(Mandatory = $true)]$Certificate)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($Certificate.RawData))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha256.Dispose() }
}

function Write-DetachedCmsSignature {
    param(
        [Parameter(Mandatory = $true)][string]$ContentPath,
        [Parameter(Mandatory = $true)][string]$SignaturePath,
        [Parameter(Mandatory = $true)]$Certificate
    )
    Add-Type -AssemblyName System.Security
    $contentBytes = [System.IO.File]::ReadAllBytes($ContentPath)
    $contentInfo = New-Object System.Security.Cryptography.Pkcs.ContentInfo -ArgumentList (,$contentBytes)
    $signedCms = New-Object System.Security.Cryptography.Pkcs.SignedCms -ArgumentList $contentInfo, $true
    $signer = New-Object System.Security.Cryptography.Pkcs.CmsSigner -ArgumentList (,$Certificate)
    $signer.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $signer.DigestAlgorithm = New-Object System.Security.Cryptography.Oid('2.16.840.1.101.3.4.2.1')
    $signedCms.ComputeSignature($signer, $false)
    [System.IO.File]::WriteAllBytes($SignaturePath, $signedCms.Encode())
}

$signingCertificate = Resolve-CodeSigningCertificate $CertificateThumbprint
if ($null -ne $signingCertificate) {
    $certificateDerSha256 = Get-CertificateDerSha256 $signingCertificate
    if ([string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)) {
        $UpdateSignerCertSha256 = $certificateDerSha256
    }
    elseif ($UpdateSignerCertSha256.ToLowerInvariant() -ne $certificateDerSha256) {
        throw "VSMR_UPDATE_SIGNER_CERT_SHA256 does not match the configured code-signing certificate."
    }
}
$UpdateSignerCertSha256 = $UpdateSignerCertSha256.ToLowerInvariant()
if ($RequireSignature -and ($null -eq $signingCertificate -or [string]::IsNullOrWhiteSpace($UpdateSignerCertSha256))) {
    throw "A publishable automatic-update release requires a code-signing certificate and pinned DER SHA-256."
}

$solutionPath = Join-Path $RepositoryRoot "vSMR.sln"
if (-not $SkipBuild) {
    Assert-File $solutionPath
    $visualStudioInstallation = Get-VisualStudioInstallationPath
    $msbuildPath = Resolve-MsBuildPath $visualStudioInstallation
    if ($Toolset -eq "auto") {
        $Toolset = Resolve-AutomaticToolset $visualStudioInstallation $msbuildPath $Platform
    }

    $outDir = $BuildOutputDirectory.TrimEnd([char[]]"\/") + "\"
    $buildProperties = @(
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/p:PlatformToolset=$Toolset",
        "/p:VsmrReleaseOutputDirectory=$outDir"
    )
    if (-not [string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)) {
        $buildProperties += "/p:VsmrUpdateSignerCertSha256=$UpdateSignerCertSha256"
    }

    Write-Host "Restoring vSMR with $Toolset..."
    & $msbuildPath $solutionPath /t:Restore @buildProperties /nodeReuse:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "NuGet restore failed with exit code $LASTEXITCODE."
    }

    Write-Host "Rebuilding $Configuration|$Platform..."
    & $msbuildPath $solutionPath /t:Rebuild /m @buildProperties /nodeReuse:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE."
    }

    if ([string]::IsNullOrWhiteSpace($PdbPath)) {
        $PdbPath = Join-Path $RepositoryRoot "vSMR\obj\$Configuration\Runtime\vSMR.Runtime.pdb"
    }
    if ([string]::IsNullOrWhiteSpace($LoaderPdbPath)) {
        $LoaderPdbPath = Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\obj\$Configuration\vSMR.pdb"
    }
    if ([string]::IsNullOrWhiteSpace($CrashHandlerPdbPath)) {
        $CrashHandlerPdbPath = Join-Path $RepositoryRoot "vSMR\src\crash\handler\obj\$Configuration\vSMRCrashHandler.pdb"
    }
}
else {
    if ($Toolset -eq "auto") {
        throw "-SkipBuild requires an explicit -Toolset so release provenance matches the prebuilt DLL."
    }
    if ([string]::IsNullOrWhiteSpace($PdbPath)) {
        throw "-SkipBuild requires -PdbPath pointing to the prebuilt runtime PDB."
    }
    if ([string]::IsNullOrWhiteSpace($LoaderPdbPath)) {
        throw "-SkipBuild requires -LoaderPdbPath pointing to the prebuilt loader PDB."
    }
    if ([string]::IsNullOrWhiteSpace($CrashHandlerPdbPath)) {
        throw "-SkipBuild requires -CrashHandlerPdbPath pointing to the PDB produced with the prebuilt WER crash handler."
    }
}
$PdbPath = [System.IO.Path]::GetFullPath($PdbPath)
Assert-File $PdbPath
$LoaderPdbPath = [System.IO.Path]::GetFullPath($LoaderPdbPath)
Assert-File $LoaderPdbPath
$CrashHandlerPdbPath = [System.IO.Path]::GetFullPath($CrashHandlerPdbPath)
Assert-File $CrashHandlerPdbPath

$validator = Join-Path $RepositoryRoot "vSMR\tools\validate_release.ps1"
Assert-File $validator
& $validator `
    -RepositoryRoot $RepositoryRoot `
    -ExpectedVersion $Version `
    -BuildOutputDirectory $BuildOutputDirectory `
    -PdbPath $PdbPath `
    -LoaderPdbPath $LoaderPdbPath `
    -CrashHandlerPdbPath $CrashHandlerPdbPath `
    -ExpectedLoaderVersion $LoaderVersion `
    -ExpectedRuntimeAbi $RuntimeAbi `
    -UpdateSignerCertSha256 $UpdateSignerCertSha256

$dllPath = Join-Path $BuildOutputDirectory "vSMR.dll"
$dataPath = Join-Path $BuildOutputDirectory "vSMR_Data"
Assert-File $dllPath
if (-not (Test-Path -LiteralPath $dataPath -PathType Container)) {
    throw "Required release directory is missing: $dataPath"
}

$stageParent = Join-Path $ArtifactsDirectory ("_vsmr-package-" + [Guid]::NewGuid().ToString("N"))
$packageStage = Join-Path $stageParent "package"
$symbolStage = Join-Path $stageParent "symbols"
if ((Test-PathEqualOrChild $stageParent $dataPath) -or
    (Test-PathEqualOrChild $dataPath $stageParent) -or
    (Test-PathEqualOrChild $ArtifactsDirectory $dataPath)) {
    throw "ArtifactsDirectory and its staging tree cannot overlap the vSMR_Data source."
}
[System.IO.Directory]::CreateDirectory($ArtifactsDirectory) | Out-Null
try {
    [System.IO.Directory]::CreateDirectory($packageStage) | Out-Null
    [System.IO.Directory]::CreateDirectory($symbolStage) | Out-Null

    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $packageStage "vSMR.dll")
    Copy-Item -LiteralPath $dataPath -Destination (Join-Path $packageStage "vSMR_Data") -Recurse
    $packagedDllPath = Join-Path $packageStage "vSMR.dll"
    $packagedRuntimePath = Join-Path $packageStage "vSMR_Data\Runtime\vSMR.Runtime.dll"
    $packagedCrashHandlerPath = Join-Path $packageStage "vSMR_Data\CrashReporter\vSMRCrashHandler.dll"
    Assert-File $packagedRuntimePath
    Assert-File $packagedCrashHandlerPath

if ($null -ne $signingCertificate) {
    $signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
    if ([string]::IsNullOrWhiteSpace($signTool)) {
        $kitsBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
        $signTool = Get-ChildItem -LiteralPath $kitsBin -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x86\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -ExpandProperty FullName -First 1
    }
    if ([string]::IsNullOrWhiteSpace($signTool)) {
        throw "A signing certificate was configured, but signtool.exe was not found."
    }
    $normalizedThumbprint = ($CertificateThumbprint -replace '\s', '')
    foreach ($binaryToSign in @($packagedDllPath, $packagedRuntimePath, $packagedCrashHandlerPath)) {
        & $signTool sign /sha1 $normalizedThumbprint /fd SHA256 /td SHA256 /tr $TimestampUrl $binaryToSign
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode signing failed for '$binaryToSign' with exit code $LASTEXITCODE."
        }
    }
}

$signature = Get-AuthenticodeSignature -LiteralPath $packagedDllPath
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $packagedRuntimePath
$crashHandlerSignature = Get-AuthenticodeSignature -LiteralPath $packagedCrashHandlerPath
if ($RequireSignature -and $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "A valid Authenticode signature is required, but vSMR.dll status is '$($signature.Status)'."
}
if ($RequireSignature -and $runtimeSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "A valid Authenticode signature is required, but vSMR.Runtime.dll status is '$($runtimeSignature.Status)'."
}
if ($RequireSignature -and $crashHandlerSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "A valid Authenticode signature is required, but vSMRCrashHandler.dll status is '$($crashHandlerSignature.Status)'."
}
if (-not [string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)) {
    $loaderAscii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($packagedDllPath))
    if (-not $loaderAscii.Contains($UpdateSignerCertSha256)) {
        throw "The packaged loader does not contain the configured VSMR_UPDATE_SIGNER_CERT_SHA256 pin. Rebuild with the same pin before packaging."
    }
}

$gitCommit = "unknown"
$sourceDirty = $true
$gitProvenanceVerified = $false
if (Get-Command git.exe -ErrorAction SilentlyContinue) {
    $gitCommitOutput = & git.exe -C $RepositoryRoot rev-parse HEAD 2>$null
    $commitResolved = $LASTEXITCODE -eq 0 -and
        ([string]$gitCommitOutput).Trim() -match '^[0-9a-fA-F]{40}$'
    if ($commitResolved) {
        $gitCommit = ([string]$gitCommitOutput).Trim()
    }
    $statusOutput = @(& git.exe -C $RepositoryRoot status --porcelain --untracked-files=all 2>$null)
    $statusResolved = $LASTEXITCODE -eq 0
    if ($statusResolved) {
        $sourceDirty = $statusOutput.Count -gt 0
    }
    $gitProvenanceVerified = $commitResolved -and $statusResolved
}
if ((-not $gitProvenanceVerified -or $sourceDirty) -and -not $AllowDirtySource) {
    throw "Refusing to create a publishable package because the Git commit and clean source state could not both be verified. Commit/stash all changes, or use -AllowDirtySource for a marked non-publishable local artifact."
}
$githubCi = $env:GITHUB_ACTIONS -eq 'true'
$trustedGithubRef = $githubCi -and
    $env:GITHUB_EVENT_NAME -ne 'pull_request' -and
    $env:GITHUB_REF -match '^refs/heads/(main|master|dev)$'
$appveyorCi = -not [string]::IsNullOrWhiteSpace([string]$env:APPVEYOR)
$appveyorPullRequest = $appveyorCi -and
    -not [string]::IsNullOrWhiteSpace([string]$env:APPVEYOR_PULL_REQUEST_NUMBER)
$forcedNonPublishable = $ForceNonPublishable -or
    $env:VSMR_FORCE_NONPUBLISHABLE -eq '1' -or
    ($githubCi -and -not $trustedGithubRef) -or
    $appveyorPullRequest
$publishable = $gitProvenanceVerified -and -not $sourceDirty -and -not $forcedNonPublishable
$updateChannel = if ($Version.Contains('-')) { 'beta' } else { 'stable' }
$packagedLoaderHash = (Get-FileHash -LiteralPath $packagedDllPath -Algorithm SHA256).Hash.ToLowerInvariant()
$packagedRuntimeHash = (Get-FileHash -LiteralPath $packagedRuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
$allBinarySignaturesValid =
    $signature.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and
    $runtimeSignature.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and
    $crashHandlerSignature.Status -eq [System.Management.Automation.SignatureStatus]::Valid
$updatePublishable = $publishable -and $allBinarySignaturesValid -and
    -not [string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)
if ($RequireSignature -and -not $updatePublishable) {
    throw "A signature-required release must be clean, publishable, Authenticode-signed, and pinned to its detached-manifest signer."
}

$metadata = [ordered]@{
    schema_version = 1
    product = "vSMR"
    version = $Version
    channel = $updateChannel
    git_commit = $gitCommit
    source_dirty = [bool]$sourceDirty
    publishable = [bool]$publishable
    built_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    configuration = $Configuration
    platform = $Platform
    toolset = $Toolset
    runtime = [ordered]@{
        version = $Version
        abi = $RuntimeAbi
        relative_path = "vSMR_Data/Runtime/vSMR.Runtime.dll"
        size = [int64](Get-Item -LiteralPath $packagedRuntimePath).Length
        sha256 = $packagedRuntimeHash
    }
    loader = [ordered]@{
        version = $LoaderVersion
        runtime_abi = $RuntimeAbi
        relative_path = "vSMR.dll"
        size = [int64](Get-Item -LiteralPath $packagedDllPath).Length
        sha256 = $packagedLoaderHash
    }
    automatic_update = [ordered]@{
        manifest_schema_version = 1
        minimum_loader_version = $MinimumLoaderVersion
        signer_cert_der_sha256 = $UpdateSignerCertSha256
        publishable = [bool]$updatePublishable
    }
    authenticode = [ordered]@{
        status = [string]$signature.Status
        subject = if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Subject } else { "" }
        thumbprint = if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Thumbprint } else { "" }
        runtime = [ordered]@{
            status = [string]$runtimeSignature.Status
            subject = if ($null -ne $runtimeSignature.SignerCertificate) { [string]$runtimeSignature.SignerCertificate.Subject } else { "" }
            thumbprint = if ($null -ne $runtimeSignature.SignerCertificate) { [string]$runtimeSignature.SignerCertificate.Thumbprint } else { "" }
        }
        crash_handler = [ordered]@{
            status = [string]$crashHandlerSignature.Status
            subject = if ($null -ne $crashHandlerSignature.SignerCertificate) { [string]$crashHandlerSignature.SignerCertificate.Subject } else { "" }
            thumbprint = if ($null -ne $crashHandlerSignature.SignerCertificate) { [string]$crashHandlerSignature.SignerCertificate.Thumbprint } else { "" }
        }
    }
    ci = [ordered]@{
        provider = if ($githubCi) { "GitHub Actions" } elseif ($appveyorCi) { "AppVeyor" } else { "local" }
        build_id = [string]$env:APPVEYOR_BUILD_ID
        build_number = [string]$env:APPVEYOR_BUILD_NUMBER
        job_id = [string]$env:APPVEYOR_JOB_ID
        pull_request = [string]$env:APPVEYOR_PULL_REQUEST_NUMBER
        event_name = [string]$env:GITHUB_EVENT_NAME
        ref = [string]$env:GITHUB_REF
        run_id = [string]$env:GITHUB_RUN_ID
    }
}

$metadataPath = Join-Path $packageStage "vSMR_Data\RELEASE-METADATA.json"
Write-Utf8NoBom $metadataPath ((ConvertTo-Json $metadata -Depth 10) + "`n")

$hashLines = New-Object System.Collections.Generic.List[string]
foreach ($file in @(Get-ChildItem -LiteralPath $packageStage -Recurse -File | Sort-Object FullName)) {
    $relativePath = $file.FullName.Substring($packageStage.Length).TrimStart([char[]]"\/").Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashLines.Add("$hash  $relativePath")
}
$payloadManifest = Join-Path $packageStage "vSMR_Data\SHA256SUMS.txt"
Write-Utf8NoBom $payloadManifest (($hashLines -join "`n") + "`n")

$archivePath = Join-Path $ArtifactsDirectory "vSMR-$Version.zip"
New-ZipArchive $packageStage $archivePath
$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Utf8NoBom "$archivePath.sha256" "$archiveHash  $([System.IO.Path]::GetFileName($archivePath))`n"

$updateManifest = [ordered]@{
    schema_version = 1
    product = "vSMR"
    version = $Version
    channel = $updateChannel
    publishable = [bool]$updatePublishable
    archive = [ordered]@{
        name = [System.IO.Path]::GetFileName($archivePath)
        size = [int64](Get-Item -LiteralPath $archivePath).Length
        sha256 = $archiveHash
    }
    loader = [ordered]@{
        name = "vSMR.dll"
        version = $LoaderVersion
        size = [int64](Get-Item -LiteralPath $packagedDllPath).Length
        sha256 = $packagedLoaderHash
    }
    minimum_loader_version = $MinimumLoaderVersion
    runtime_abi = $RuntimeAbi
    runtime_relative_path = "vSMR_Data/Runtime/vSMR.Runtime.dll"
}
$updateManifestPath = Join-Path $ArtifactsDirectory "vSMR-$Version.update.json"
$updateSignaturePath = "$updateManifestPath.p7s"
Write-Utf8NoBom $updateManifestPath ((ConvertTo-Json $updateManifest -Depth 10) + "`n")
if ($null -ne $signingCertificate -and $updatePublishable) {
    Write-DetachedCmsSignature `
        -ContentPath $updateManifestPath `
        -SignaturePath $updateSignaturePath `
        -Certificate $signingCertificate
}
elseif (Test-Path -LiteralPath $updateSignaturePath) {
    Remove-Item -LiteralPath $updateSignaturePath -Force
}
if ($RequireSignature -and -not (Test-Path -LiteralPath $updateSignaturePath -PathType Leaf)) {
    throw "The signed update manifest was not created."
}

Copy-Item -LiteralPath $LoaderPdbPath -Destination (Join-Path $symbolStage "vSMR.Loader.pdb")
Copy-Item -LiteralPath $packagedDllPath -Destination (Join-Path $symbolStage "vSMR.Loader.dll")
Copy-Item -LiteralPath $PdbPath -Destination (Join-Path $symbolStage "vSMR.Runtime.pdb")
Copy-Item -LiteralPath $packagedRuntimePath -Destination (Join-Path $symbolStage "vSMR.Runtime.dll")
Copy-Item -LiteralPath $CrashHandlerPdbPath -Destination (Join-Path $symbolStage "vSMRCrashHandler.pdb")
Copy-Item -LiteralPath $packagedCrashHandlerPath -Destination (Join-Path $symbolStage "vSMRCrashHandler.dll")
Write-Utf8NoBom (Join-Path $symbolStage "SYMBOLS-METADATA.json") ((ConvertTo-Json $metadata -Depth 10) + "`n")
$symbolArchivePath = Join-Path $ArtifactsDirectory "vSMR-$Version-symbols.zip"
New-ZipArchive $symbolStage $symbolArchivePath
}
finally {
    Remove-SafeDirectory $stageParent $ArtifactsDirectory
}

Write-Host "Created user package: $archivePath"
Write-Host "Created package checksum: $archivePath.sha256"
Write-Host "Created update manifest: $updateManifestPath"
if (Test-Path -LiteralPath $updateSignaturePath -PathType Leaf) {
    Write-Host "Created detached update signature: $updateSignaturePath"
} else {
    Write-Warning "No detached update signature was created; automatic updating will ignore this local artifact."
}
Write-Host "Created private symbols: $symbolArchivePath"
