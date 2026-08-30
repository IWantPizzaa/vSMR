#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^v\d+$")]
    [string]$Toolset = "v145"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$solutionPath = Join-Path $RepositoryRoot "vSMR.sln"
if (-not (Test-Path -LiteralPath $solutionPath -PathType Leaf)) {
    throw "vSMR.sln was not found at '$solutionPath'."
}

$bundleScript = Join-Path $PSScriptRoot "build_control_center_bundle.ps1"
Write-Host "Checking the generated Control Center bundle..."
& $bundleScript -RepositoryRoot $RepositoryRoot -Check
if ($LASTEXITCODE -ne 0) {
    throw "The Control Center bundle is stale. Run '$bundleScript' and commit the result."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$msbuildPath = ""
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $installationPath = @(
        & $vswhere -latest -products * `
            -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ATLMFC `
            -property installationPath
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } | Select-Object -First 1
    if ($installationPath) {
        $candidate = Join-Path ([string]$installationPath) "MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $msbuildPath = $candidate
        }
    }
}
if ([string]::IsNullOrWhiteSpace($msbuildPath)) {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $msbuildPath = [string]$command.Source }
}
if ([string]::IsNullOrWhiteSpace($msbuildPath)) {
    throw "MSBuild was not found. Install Visual Studio with C++ and MFC support."
}

$buildProperties = @(
    "/p:Configuration=Release",
    "/p:Platform=Win32",
    "/p:PlatformToolset=$Toolset",
    "/nodeReuse:false",
    "/nologo",
    "/verbosity:minimal"
)

Write-Host "Restoring vSMR dependencies..."
& $msbuildPath $solutionPath "/t:Restore" @buildProperties
if ($LASTEXITCODE -ne 0) {
    throw "Dependency restore failed with exit code $LASTEXITCODE."
}

$rebuildArguments = @(
    $solutionPath,
    "/m",
    "/t:Rebuild",
    $buildProperties
)

Write-Host "Rebuilding vSMR in Release|Win32 with $Toolset..."
& $msbuildPath @rebuildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Release rebuild failed with exit code $LASTEXITCODE."
}

Write-Host "Release rebuild completed: $(Join-Path $RepositoryRoot 'Release\vSMR.dll')"

$testScript = Join-Path $RepositoryRoot "vSMR\tests\run_tests.ps1"
Write-Host "Running native and browser regression tests..."
& $testScript -RepositoryRoot $RepositoryRoot
if ($LASTEXITCODE -ne 0) {
    throw "Regression tests failed with exit code $LASTEXITCODE."
}
