#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

$bundleScript = Join-Path $RepositoryRoot "vSMR\tools\build_control_center_bundle.ps1"
& $bundleScript -RepositoryRoot $RepositoryRoot -Check
if ($LASTEXITCODE -ne 0) {
    throw "The generated Control Center bundle is stale."
}

$nativeTests = Join-Path $RepositoryRoot "vSMR\tests\bin\Release\vSMR.Tests.exe"
if (-not (Test-Path -LiteralPath $nativeTests -PathType Leaf)) {
    throw "Native regression test executable was not found: $nativeTests"
}
& $nativeTests $RepositoryRoot
if ($LASTEXITCODE -ne 0) {
    throw "Native regression tests failed with exit code $LASTEXITCODE."
}

$edgeCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft\Edge\Application\msedge.exe"),
    (Join-Path $env:ProgramFiles "Microsoft\Edge\Application\msedge.exe")
)
$edge = $edgeCandidates | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_ -PathType Leaf)
} | Select-Object -First 1
if (-not $edge) {
    throw "Microsoft Edge is required for the Control Center browser regression tests."
}

$webRoot = Join-Path $RepositoryRoot "vSMR\src\control_center\web"
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-browser-tests-" + [guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($testRoot) | Out-Null
try {
    foreach ($asset in @("index.html", "styles.css", "data.js", "app-bundle.js")) {
        Copy-Item -LiteralPath (Join-Path $webRoot $asset) -Destination $testRoot
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "ControlCenterBrowserTests.js") -Destination $testRoot

    $indexPath = Join-Path $testRoot "index.html"
    $index = [System.IO.File]::ReadAllText($indexPath)
    $testScript = '<script src="ControlCenterBrowserTests.js"></script>'
    $index = $index.Replace("</body>", "$testScript`n</body>")
    [System.IO.File]::WriteAllText($indexPath, $index, [System.Text.UTF8Encoding]::new($false))

    $outputPath = Join-Path $testRoot "browser-output.html"
    $errorPath = Join-Path $testRoot "browser-error.txt"
    $profilePath = Join-Path $testRoot "edge-profile"
    $uri = ([System.Uri]$indexPath).AbsoluteUri
    $arguments = @(
        "--headless=new",
        "--disable-gpu",
        "--no-first-run",
        "--disable-extensions",
        "--user-data-dir=$profilePath",
        "--virtual-time-budget=6000",
        "--dump-dom",
        $uri
    )
    $process = Start-Process -FilePath $edge -ArgumentList $arguments -Wait -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath
    if ($process.ExitCode -ne 0) {
        throw "Control Center browser tests could not start (Edge exit code $($process.ExitCode))."
    }
    $output = [System.IO.File]::ReadAllText($outputPath)
    if ($output -notmatch 'data-vsmr-browser-tests="passed"') {
        $detail = if ($output -match '(?s)<pre id="vsmr-browser-test-result"[^>]*>(.*?)</pre>') {
            [System.Net.WebUtility]::HtmlDecode($Matches[1])
        } else {
            $browserError = [System.IO.File]::ReadAllText($errorPath)
            $tailLength = [Math]::Min(1200, $output.Length)
            $outputTail = if ($tailLength -gt 0) {
                $output.Substring($output.Length - $tailLength)
            } else {
                "(empty DOM output)"
            }
            "No browser result marker was produced. Edge: $browserError DOM tail: $outputTail"
        }
        throw "Control Center browser regression tests failed. $detail"
    }
    Write-Host "Control Center browser tests passed"
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
