#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern("^\d+\.\d+\.\d+(?:-beta\.\d+)?$")]
    [string]$Version,
    [string]$RepositoryRoot = "",
    [string]$ArtifactsDirectory = "",
    [ValidatePattern("^(auto|v\d+)$")]
    [string]$Toolset = "auto",
    [string]$CertificateThumbprint = "",
    [ValidatePattern("^(|[0-9a-fA-F]{64})$")]
    [string]$UpdateSignerCertSha256 = "",
    [string]$PublishedBeta3ArchivePath = ""
)

$ErrorActionPreference = "Stop"
$LoaderVersion = "1.1.0"
$RuntimeAbi = 1

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
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
if ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -or
    $UpdateSignerCertSha256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw "A publishable release requires VSMR_SIGNING_CERT_THUMBPRINT and the matching 64-character VSMR_UPDATE_SIGNER_CERT_SHA256. Use package_release.ps1 -ForceNonPublishable for local validation instead."
}
$UpdateSignerCertSha256 = $UpdateSignerCertSha256.ToLowerInvariant()
if ($Version -eq '2.0.0-beta.4' -and [string]::IsNullOrWhiteSpace($PublishedBeta3ArchivePath)) {
    throw "Beta.4 release gating requires -PublishedBeta3ArchivePath pointing to the published vSMR-2.0.0-beta.3.zip."
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $git) { throw "git.exe is required to establish release provenance." }
$head = ([string](& $git.Source -C $RepositoryRoot rev-parse HEAD)).Trim()
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40}$') {
    throw "The release commit could not be resolved."
}
$branch = ([string](& $git.Source -C $RepositoryRoot branch --show-current)).Trim()
if ($LASTEXITCODE -ne 0 -or $branch -notin @('main', 'master')) {
    throw "Publishable releases must be created from main or master, not '$branch'."
}
$status = @(& $git.Source -C $RepositoryRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) {
    throw "The working tree is not clean. Commit or stash every change before creating release assets."
}
$pointingTags = @(& $git.Source -C $RepositoryRoot tag --points-at HEAD)
if ($LASTEXITCODE -ne 0) { throw "Tags at the release commit could not be read." }
$releaseTags = @($Version, "v$Version")
$matchingTags = @($pointingTags | Where-Object { $_ -in $releaseTags })
if ($matchingTags.Count -ne 1) {
    throw "Exactly one tag named '$Version' or 'v$Version' must point at release commit $head."
}
$tagCommit = ([string](& $git.Source -C $RepositoryRoot rev-parse "$($matchingTags[0])^{commit}")).Trim()
if ($LASTEXITCODE -ne 0 -or $tagCommit -ne $head) {
    throw "Release tag $($matchingTags[0]) does not resolve exactly to HEAD."
}

& (Join-Path $PSScriptRoot "normalize_runtime_data.ps1") `
    -Mode Check `
    -DataDirectory (Join-Path $RepositoryRoot "vSMR\data")
& (Join-Path $PSScriptRoot "validate_release.ps1") `
    -RepositoryRoot $RepositoryRoot `
    -ExpectedVersion $Version `
    -ExpectedLoaderVersion $LoaderVersion `
    -ExpectedRuntimeAbi $RuntimeAbi `
    -UpdateSignerCertSha256 $UpdateSignerCertSha256
& (Join-Path $PSScriptRoot "crash_harness\run_crash_harness.ps1") `
    -RepositoryRoot $RepositoryRoot `
    -Configuration Release `
    -Toolset $Toolset `
    -IncludeWerIntegration
& (Join-Path $PSScriptRoot "updater_harness\run_updater_harness.ps1") `
    -Toolset $Toolset

& (Join-Path $PSScriptRoot "package_release.ps1") `
    -RepositoryRoot $RepositoryRoot `
    -ArtifactsDirectory $ArtifactsDirectory `
    -Version $Version `
    -Configuration Release `
    -Platform Win32 `
    -Toolset $Toolset `
    -CertificateThumbprint $CertificateThumbprint `
    -UpdateSignerCertSha256 $UpdateSignerCertSha256 `
    -LoaderVersion $LoaderVersion `
    -MinimumLoaderVersion $LoaderVersion `
    -RuntimeAbi $RuntimeAbi `
    -RequireSignature

$archivePath = Join-Path $ArtifactsDirectory "vSMR-$Version.zip"
$manifestPath = Join-Path $ArtifactsDirectory "vSMR-$Version.update.json"
$signaturePath = "$manifestPath.p7s"
& (Join-Path $PSScriptRoot "verify_release_package.ps1") `
    -ArchivePath $archivePath `
    -UpdateManifestPath $manifestPath `
    -UpdateSignaturePath $signaturePath `
    -ExpectedVersion $Version `
    -ExpectedLoaderVersion $LoaderVersion `
    -ExpectedRuntimeAbi $RuntimeAbi `
    -RequireSignature `
    -PublishedBeta3ArchivePath $PublishedBeta3ArchivePath `
    -RequirePublishedBeta3Migration:($Version -eq '2.0.0-beta.4')

$publicAssets = @($archivePath, $manifestPath, $signaturePath)
foreach ($asset in $publicAssets) {
    if (-not (Test-Path -LiteralPath $asset -PathType Leaf) -or
        (Get-Item -LiteralPath $asset).Length -le 0) {
        throw "Required GitHub release asset is missing or empty: $asset"
    }
}
if (Test-Path -LiteralPath "$archivePath.sha256" -PathType Leaf) {
    throw "Obsolete external .zip.sha256 asset exists; remove it and rerun the release driver."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.publishable -isnot [bool] -or -not [bool]$manifest.publishable -or
    [string]$manifest.minimum_loader_version -ne $LoaderVersion -or
    [string]$manifest.loader.version -ne $LoaderVersion) {
    throw "The verified update manifest is not publishable with loader $LoaderVersion."
}

Write-Host ""
Write-Host "Verified GitHub release assets (upload exactly these three):" -ForegroundColor Green
foreach ($asset in $publicAssets) { Write-Host "  $asset" }
Write-Host "Keep private: $(Join-Path $ArtifactsDirectory "vSMR-$Version-symbols.zip")"
Write-Host "Create GitHub beta releases with the prerelease flag; do not upload validation-only or checksum sidecars."
