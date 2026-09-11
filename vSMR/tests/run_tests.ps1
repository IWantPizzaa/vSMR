#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"

function Invoke-BrowserEvaluation {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [System.Threading.CancellationToken]$Token,
        [string]$Expression
    )
    $command = @{ id = 1; method = "Runtime.evaluate"; params = @{
        expression = $Expression; returnByValue = $true
    } } | ConvertTo-Json -Compress -Depth 5
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($command)
    $Socket.SendAsync([ArraySegment[byte]]::new($bytes),
        [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $Token).GetAwaiter().GetResult() | Out-Null
    $buffer = [byte[]]::new(65536)
    do {
        $message = [System.IO.MemoryStream]::new()
        try {
            do {
                $received = $Socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $Token).GetAwaiter().GetResult()
                if ($received.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                    throw "Edge closed before the browser tests reported a result."
                }
                $message.Write($buffer, 0, $received.Count)
            } while (-not $received.EndOfMessage)
            $reply = [System.Text.Encoding]::UTF8.GetString($message.ToArray()) | ConvertFrom-Json
        } finally {
            $message.Dispose()
        }
    } while ($reply.id -ne 1)
    if ($reply.error -or $reply.result.exceptionDetails) {
        throw "Browser test result evaluation failed: $($reply | ConvertTo-Json -Compress -Depth 8)"
    }
    return $reply.result.result.value
}

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

$bundleScript = Join-Path $RepositoryRoot "vSMR\tools\build_control_center_bundle.ps1"
& $bundleScript -RepositoryRoot $RepositoryRoot -Check
if ($LASTEXITCODE -ne 0) {
    throw "The generated Control Center bundle is stale."
}

$styleScript = Join-Path $RepositoryRoot "vSMR\tools\build_control_center_styles.ps1"
& $styleScript -RepositoryRoot $RepositoryRoot -Check
if ($LASTEXITCODE -ne 0) {
    throw "The generated Control Center stylesheet bundle is stale."
}

$styleOwnershipScript = Join-Path $PSScriptRoot "verify_control_center_style_ownership.ps1"
& $styleOwnershipScript -RepositoryRoot $RepositoryRoot

& (Join-Path $PSScriptRoot "verify_json_entry_points.ps1") -RepositoryRoot $RepositoryRoot

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
$process = $null
$socket = $null
$browserCancellation = [System.Threading.CancellationTokenSource]::new(30000)
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
        "--user-data-dir=`"$profilePath`"",
        "--remote-debugging-port=0",
        $uri
    )
    $process = Start-Process -FilePath $edge -ArgumentList $arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath
    # Virtual-time dump-dom can exhaust timer deadlines before ResizeObserver
    # and requestAnimationFrame run. Let Edge render normally and poll the page's
    # explicit result over its loopback DevTools connection instead.
    $portFile = Join-Path $profilePath "DevToolsActivePort"
    while (-not (Test-Path -LiteralPath $portFile -PathType Leaf)) {
        $browserCancellation.Token.ThrowIfCancellationRequested()
        Start-Sleep -Milliseconds 50
    }
    $page = $null
    while ($null -eq $page) {
        $browserCancellation.Token.ThrowIfCancellationRequested()
        $port = [int]([System.IO.File]::ReadAllLines($portFile)[0])
        $pages = Invoke-RestMethod -Uri "http://127.0.0.1:$port/json/list" -TimeoutSec 5
        $page = $pages | Where-Object { $_.type -eq "page" -and $_.url -eq $uri } | Select-Object -First 1
        if ($null -eq $page) { Start-Sleep -Milliseconds 50 }
    }
    $socket = [System.Net.WebSockets.ClientWebSocket]::new()
    $socket.ConnectAsync([uri]$page.webSocketDebuggerUrl, $browserCancellation.Token).GetAwaiter().GetResult() | Out-Null
    do {
        $browserCancellation.Token.ThrowIfCancellationRequested()
        $status = Invoke-BrowserEvaluation $socket $browserCancellation.Token `
            "document.documentElement.dataset.vsmrBrowserTests || 'running'"
        if ($status -eq "running") { Start-Sleep -Milliseconds 50 }
    } while ($status -eq "running")
    if ($status -ne "passed") {
        $detail = Invoke-BrowserEvaluation $socket $browserCancellation.Token `
            "document.getElementById('vsmr-browser-test-result')?.textContent || 'No browser result marker'"
        throw "Control Center browser regression tests failed. $detail"
    }
    Write-Host "Control Center browser tests passed"
} finally {
    if ($null -ne $socket) {
        if ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
            try {
                $closeBytes = [System.Text.Encoding]::UTF8.GetBytes('{"id":2,"method":"Browser.close"}')
                $closeCancellation = [System.Threading.CancellationTokenSource]::new(1000)
                try {
                    $socket.SendAsync([ArraySegment[byte]]::new($closeBytes),
                        [System.Net.WebSockets.WebSocketMessageType]::Text, $true,
                        $closeCancellation.Token).GetAwaiter().GetResult() | Out-Null
                } finally { $closeCancellation.Dispose() }
            } catch { Write-Verbose "Edge was already closing: $_" }
        }
        $socket.Dispose()
    }
    if ($null -ne $process -and -not $process.HasExited -and -not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $browserCancellation.Dispose()
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $expectedParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
    if ([System.IO.Path]::GetDirectoryName($resolvedTestRoot) -eq $expectedParent -and
        [System.IO.Path]::GetFileName($resolvedTestRoot) -match '^vsmr-browser-tests-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
