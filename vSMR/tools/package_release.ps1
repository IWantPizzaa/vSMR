#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$BuildOutputDirectory = "",
    [string]$ArtifactsDirectory = "",
    [ValidatePattern("^\d+\.\d+\.\d+-beta\.\d+$")]
    [string]$Version = "2.0.0-beta.2",
    [string]$Configuration = "Release",
    [string]$Platform = "Win32",
    [ValidatePattern("^(auto|v\d+)$")]
    [string]$Toolset = "auto",
    [string]$PdbPath = "",
    [string]$CrashHandlerPdbPath = "",
    [string]$CertificateThumbprint = "",
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
        $PdbPath = Join-Path $RepositoryRoot "vSMR\$Configuration\vSMR.pdb"
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
        throw "-SkipBuild requires -PdbPath pointing to the PDB produced with the prebuilt DLL."
    }
    if ([string]::IsNullOrWhiteSpace($CrashHandlerPdbPath)) {
        throw "-SkipBuild requires -CrashHandlerPdbPath pointing to the PDB produced with the prebuilt WER crash handler."
    }
}
$PdbPath = [System.IO.Path]::GetFullPath($PdbPath)
Assert-File $PdbPath
$CrashHandlerPdbPath = [System.IO.Path]::GetFullPath($CrashHandlerPdbPath)
Assert-File $CrashHandlerPdbPath

$validator = Join-Path $RepositoryRoot "vSMR\tools\validate_release.ps1"
Assert-File $validator
& $validator `
    -RepositoryRoot $RepositoryRoot `
    -ExpectedVersion $Version `
    -BuildOutputDirectory $BuildOutputDirectory `
    -PdbPath $PdbPath `
    -CrashHandlerPdbPath $CrashHandlerPdbPath

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
    $packagedCrashHandlerPath = Join-Path $packageStage "vSMR_Data\CrashReporter\vSMRCrashHandler.dll"
    Assert-File $packagedCrashHandlerPath

if ([string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $CertificateThumbprint = [string]$env:VSMR_SIGNING_CERT_THUMBPRINT
}
if ($env:VSMR_REQUIRE_SIGNATURE -eq '1' -or $env:VSMR_REQUIRE_SIGNATURE -eq 'true') {
    $RequireSignature = $true
}
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
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
    foreach ($binaryToSign in @($packagedDllPath, $packagedCrashHandlerPath)) {
        & $signTool sign /sha1 $normalizedThumbprint /fd SHA256 /td SHA256 /tr $TimestampUrl $binaryToSign
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode signing failed for '$binaryToSign' with exit code $LASTEXITCODE."
        }
    }
}

$signature = Get-AuthenticodeSignature -LiteralPath $packagedDllPath
$crashHandlerSignature = Get-AuthenticodeSignature -LiteralPath $packagedCrashHandlerPath
if ($RequireSignature -and $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "A valid Authenticode signature is required, but vSMR.dll status is '$($signature.Status)'."
}
if ($RequireSignature -and $crashHandlerSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "A valid Authenticode signature is required, but vSMRCrashHandler.dll status is '$($crashHandlerSignature.Status)'."
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

$metadata = [ordered]@{
    schema_version = 1
    product = "vSMR"
    version = $Version
    channel = "beta"
    git_commit = $gitCommit
    source_dirty = [bool]$sourceDirty
    publishable = [bool]$publishable
    built_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    configuration = $Configuration
    platform = $Platform
    toolset = $Toolset
    authenticode = [ordered]@{
        status = [string]$signature.Status
        subject = if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Subject } else { "" }
        thumbprint = if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Thumbprint } else { "" }
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

Copy-Item -LiteralPath $PdbPath -Destination (Join-Path $symbolStage "vSMR.pdb")
Copy-Item -LiteralPath $packagedDllPath -Destination (Join-Path $symbolStage "vSMR.dll")
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
Write-Host "Created private symbols: $symbolArchivePath"
