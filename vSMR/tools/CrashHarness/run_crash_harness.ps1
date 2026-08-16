#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^(auto|v\d+)$")]
    [string]$Toolset = "auto",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$HandlerPath = "",
    [switch]$SkipBuild,
    [switch]$SkipProductionHandlerBuild,
    [switch]$IncludeWerIntegration,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$handlerProject = Join-Path $RepositoryRoot "vSMR\crash_handler\vSMRCrashHandler.vcxproj"
$harnessProject = Join-Path $PSScriptRoot "vSMRCrashHarness.vcxproj"
$harnessExe = Join-Path $PSScriptRoot "bin\$Configuration\vSMRCrashHarness.exe"
$noDbgHelpHandler = Join-Path $PSScriptRoot "bin\$Configuration\vSMRCrashHandlerNoDbgHelp.dll"
if ([string]::IsNullOrWhiteSpace($HandlerPath)) {
    $HandlerPath = Join-Path $RepositoryRoot "vSMR\crash_handler\bin\$Configuration\vSMRCrashHandler.dll"
}
$HandlerPath = [System.IO.Path]::GetFullPath($HandlerPath)

function Resolve-MsBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installation = @(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
            Select-Object -First 1
        if (-not [string]::IsNullOrWhiteSpace([string]$installation)) {
            $candidate = Join-Path ([string]$installation) "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return [pscustomobject]@{ Path = $candidate; Installation = [string]$installation }
            }
        }
    }
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) { throw "MSBuild was not found." }
    return [pscustomobject]@{ Path = [string]$command.Source; Installation = "" }
}

function Resolve-Toolset([string]$VisualStudioInstallation) {
    if ($Toolset -ne 'auto') { return $Toolset }
    if ([string]::IsNullOrWhiteSpace($VisualStudioInstallation)) {
        throw "Automatic toolset discovery needs a Visual Studio installation; pass -Toolset explicitly."
    }
    $vcRoot = Join-Path $VisualStudioInstallation "MSBuild\Microsoft\VC"
    $candidate = Get-ChildItem -Path (Join-Path $vcRoot "*\Platforms\Win32\PlatformToolsets\v*") -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^v\d+$' } |
        Sort-Object @{ Expression = { [int]$_.Name.Substring(1) }; Descending = $true } |
        Select-Object -First 1
    if ($null -eq $candidate) { throw "No installed Win32 MSVC toolset was found." }
    return [string]$candidate.Name
}

function Start-HarnessProcess([string[]]$Arguments) {
    function ConvertTo-NativeArgument([string]$Value) {
        if ($null -eq $Value) { return '""' }
        if ($Value.IndexOf([char]0) -ge 0) { throw "A process argument contains a NUL character." }
        if ($Value -notmatch '[\s"]') { return $Value }

        $quoted = New-Object System.Text.StringBuilder
        [void]$quoted.Append('"')
        $backslashes = 0
        foreach ($character in $Value.ToCharArray()) {
            if ($character -eq '\') {
                $backslashes++
                continue
            }
            if ($character -eq '"') {
                [void]$quoted.Append((('\' * (($backslashes * 2) + 1)) -join ''))
                [void]$quoted.Append('"')
                $backslashes = 0
                continue
            }
            if ($backslashes -gt 0) {
                [void]$quoted.Append((('\' * $backslashes) -join ''))
                $backslashes = 0
            }
            [void]$quoted.Append($character)
        }
        if ($backslashes -gt 0) {
            [void]$quoted.Append((('\' * ($backslashes * 2)) -join ''))
        }
        [void]$quoted.Append('"')
        return $quoted.ToString()
    }

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $harnessExe
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = (@($Arguments | ForEach-Object { ConvertTo-NativeArgument ([string]$_) }) -join ' ')
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Could not start the crash harness." }
    return $process
}

function Complete-HarnessProcess(
    [System.Diagnostics.Process]$Process,
    [int]$TimeoutMilliseconds,
    [bool]$RequireZeroExit) {
    if (-not $Process.WaitForExit($TimeoutMilliseconds)) {
        try { $Process.Kill() } catch { }
        throw "Crash harness timed out after $TimeoutMilliseconds ms."
    }
    $stdout = $Process.StandardOutput.ReadToEnd()
    $stderr = $Process.StandardError.ReadToEnd()
    if ($RequireZeroExit -and $Process.ExitCode -ne 0) {
        throw "Crash harness failed with exit code $($Process.ExitCode). stdout='$stdout' stderr='$stderr'"
    }
    return [pscustomobject]@{ ExitCode = $Process.ExitCode; Stdout = $stdout; Stderr = $stderr }
}

function Assert-ReportArtifacts(
    [string]$Directory,
    [int]$ExpectedTextCount,
    [int]$ExpectedDumpCount) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $textFiles = @(Get-ChildItem -LiteralPath $Directory -Filter '*.txt' -File -ErrorAction SilentlyContinue)
        $dumpFiles = @(Get-ChildItem -LiteralPath $Directory -Filter '*.dmp' -File -ErrorAction SilentlyContinue)
        if ($textFiles.Count -eq $ExpectedTextCount -and $dumpFiles.Count -eq $ExpectedDumpCount) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($textFiles.Count -ne $ExpectedTextCount -or $dumpFiles.Count -ne $ExpectedDumpCount) {
        throw "Expected $ExpectedTextCount text and $ExpectedDumpCount dump report(s) in '$Directory'; found $($textFiles.Count) text and $($dumpFiles.Count) dump files."
    }
    foreach ($file in @($textFiles) + @($dumpFiles)) {
        if ($file.Length -eq 0) { throw "Crash harness produced an empty artifact: $($file.FullName)" }
    }
    return [pscustomobject]@{ TextFiles = $textFiles; DumpFiles = $dumpFiles }
}

function Assert-ReportPairs([string]$Directory, [int]$ExpectedCount) {
    return (Assert-ReportArtifacts $Directory $ExpectedCount $ExpectedCount)
}

function Invoke-WerCrash([string]$Mode, [string]$ReportDirectory) {
    $process = Start-HarnessProcess @($Mode, $HandlerPath, $ReportDirectory)
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        $textCount = @(Get-ChildItem -LiteralPath $ReportDirectory -Filter '*.txt' -File -ErrorAction SilentlyContinue).Count
        $dumpCount = @(Get-ChildItem -LiteralPath $ReportDirectory -Filter '*.dmp' -File -ErrorAction SilentlyContinue).Count
        if ($textCount -ge 1 -and $dumpCount -ge 1) { break }
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $process.HasExited) {
        [void]$process.WaitForExit(5000)
    }
    if (-not $process.HasExited) {
        try { $process.Kill() } catch { }
    }
    [void](Assert-ReportPairs $ReportDirectory 1)
}

function ConvertTo-ExtendedPath([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($fullPath.StartsWith('\\?\', [System.StringComparison]::Ordinal)) { return $fullPath }
    if ($fullPath.StartsWith('\\', [System.StringComparison]::Ordinal)) {
        return '\\?\UNC\' + $fullPath.Substring(2)
    }
    return '\\?\' + $fullPath
}

function Add-CreateFileDenyRule([string]$Directory) {
    $acl = Get-Acl -LiteralPath $Directory
    $originalSddl = $acl.GetSecurityDescriptorSddlForm(
        [System.Security.AccessControl.AccessControlSections]::All)
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $sid,
        [System.Security.AccessControl.FileSystemRights]::CreateFiles,
        [System.Security.AccessControl.InheritanceFlags]::None,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Deny)
    [void]$acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $Directory -AclObject $acl
    return $originalSddl
}

function Restore-DirectoryAcl([string]$Directory, [string]$Sddl) {
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetSecurityDescriptorSddlForm($Sddl)
    Set-Acl -LiteralPath $Directory -AclObject $acl
}

if (-not $SkipBuild) {
    $msbuild = Resolve-MsBuild
    $resolvedToolset = Resolve-Toolset $msbuild.Installation
    $projects = if ($SkipProductionHandlerBuild) { @($harnessProject) } else { @($handlerProject, $harnessProject) }
    foreach ($project in $projects) {
        & $msbuild.Path $project /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:PlatformToolset=$resolvedToolset" /nodeReuse:false /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw "Crash-harness build failed for '$project' with exit code $LASTEXITCODE." }
    }
    & $msbuild.Path $handlerProject /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:PlatformToolset=$resolvedToolset" /p:VsmrCrashHarnessNoDbgHelp=true /nodeReuse:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "No-DbgHelp crash-handler build failed with exit code $LASTEXITCODE." }
}
if (-not (Test-Path -LiteralPath $HandlerPath -PathType Leaf)) { throw "Crash handler is missing: $HandlerPath" }
if (-not (Test-Path -LiteralPath $noDbgHelpHandler -PathType Leaf)) { throw "No-DbgHelp harness handler is missing: $noDbgHelpHandler" }
if (-not (Test-Path -LiteralPath $harnessExe -PathType Leaf)) { throw "Crash harness is missing: $harnessExe" }

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-crash-harness-" + [Guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
try {
    $rejectedDirectory = Join-Path $temporaryRoot "existing-read-only"
    $fallbackDirectory = Join-Path $temporaryRoot "writable-fallback"
    [System.IO.Directory]::CreateDirectory($rejectedDirectory) | Out-Null
    [System.IO.Directory]::CreateDirectory($fallbackDirectory) | Out-Null
    $originalRejectedAcl = Add-CreateFileDenyRule $rejectedDirectory
    try {
        $selection = Start-HarnessProcess @('--support-select', $rejectedDirectory, $fallbackDirectory)
        $selectionResult = Complete-HarnessProcess $selection 15000 $true
        if ($selectionResult.Stdout -notmatch 'selected=1') {
            throw "Writable-directory selection did not reject the existing non-writable directory."
        }
    }
    finally {
        Restore-DirectoryAcl $rejectedDirectory $originalRejectedAcl
    }

    $retentionDirectory = Join-Path $temporaryRoot "retention"
    [System.IO.Directory]::CreateDirectory($retentionDirectory) | Out-Null
    $retention = Start-HarnessProcess @('--support-retention', $retentionDirectory)
    $retentionResult = Complete-HarnessProcess $retention 15000 $true
    if ($retentionResult.Stdout -notmatch 'retention=count:10,size:2') {
        throw "Retention count, byte ceiling, orphan, or temporary-file checks did not complete."
    }

    $unicodeDirectoryName = "reports-" + [char]0x00E9 + "-" + [char]0x65E5 + [char]0x672C
    $unicodeDirectory = Join-Path $temporaryRoot $unicodeDirectoryName
    [System.IO.Directory]::CreateDirectory($unicodeDirectory) | Out-Null
    $direct = Start-HarnessProcess @('--direct', $HandlerPath, $unicodeDirectory)
    $directResult = Complete-HarnessProcess $direct 30000 $true
    if ($directResult.Stdout -notmatch 'claimed=0') { throw "Direct callback incorrectly claimed the synthetic vSMR crash." }
    [void](Assert-ReportPairs $unicodeDirectory 1)

    $longDirectory = Join-Path $unicodeDirectory "long-path"
    while ($longDirectory.Length -le 300) {
        $longDirectory = Join-Path $longDirectory ("segment-" + ('x' * 40))
    }
    $extendedLongDirectory = ConvertTo-ExtendedPath $longDirectory
    [System.IO.Directory]::CreateDirectory($extendedLongDirectory) | Out-Null
    $longPathDirect = Start-HarnessProcess @('--direct', $HandlerPath, $extendedLongDirectory)
    $longPathResult = Complete-HarnessProcess $longPathDirect 30000 $true
    if ($longPathResult.Stdout -notmatch 'claimed=0') {
        throw "The callback failed for the Unicode path longer than MAX_PATH."
    }
    [void](Assert-ReportPairs $extendedLongDirectory 1)

    $noDbgHelpDirectory = Join-Path $temporaryRoot "missing-dbghelp"
    [System.IO.Directory]::CreateDirectory($noDbgHelpDirectory) | Out-Null
    $noDbgHelp = Start-HarnessProcess @('--direct', $noDbgHelpHandler, $noDbgHelpDirectory)
    $noDbgHelpResult = Complete-HarnessProcess $noDbgHelp 30000 $true
    if ($noDbgHelpResult.Stdout -notmatch 'claimed=0') {
        throw "The no-DbgHelp callback failed."
    }
    $noDbgHelpArtifacts = Assert-ReportArtifacts $noDbgHelpDirectory 1 0
    $noDbgHelpText = Get-Content -LiteralPath $noDbgHelpArtifacts.TextFiles[0].FullName -Raw
    if ($noDbgHelpText -notmatch '(?m)^dump_status=failed\r?$') {
        throw "The text report did not survive the simulated missing-DbgHelp condition."
    }

    $sameProcessDirectory = Join-Path $temporaryRoot "same-process-concurrent"
    [System.IO.Directory]::CreateDirectory($sameProcessDirectory) | Out-Null
    $sameProcess = Start-HarnessProcess @('--direct-concurrent', $HandlerPath, $sameProcessDirectory)
    $sameProcessResult = Complete-HarnessProcess $sameProcess 30000 $true
    if ($sameProcessResult.Stdout -notmatch 'claimed=0 callbacks=8') {
        throw "Same-process concurrent callbacks failed."
    }
    [void](Assert-ReportPairs $sameProcessDirectory 1)

    $concurrentDirectory = Join-Path $temporaryRoot "concurrent"
    [System.IO.Directory]::CreateDirectory($concurrentDirectory) | Out-Null
    $concurrent = @(1..4 | ForEach-Object {
        Start-HarnessProcess @('--direct', $HandlerPath, $concurrentDirectory)
    })
    foreach ($process in $concurrent) { [void](Complete-HarnessProcess $process 30000 $true) }
    [void](Assert-ReportPairs $concurrentDirectory 4)

    if ($IncludeWerIntegration) {
        $registrySubKey = 'Software\Microsoft\Windows\Windows Error Reporting\RuntimeExceptionHelperModules'
        $registryBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::CurrentUser,
            [Microsoft.Win32.RegistryView]::Registry32)
        $registryKey = $registryBase.CreateSubKey($registrySubKey, $true)
        $existingNames = @($registryKey.GetValueNames())
        $hadExistingValue = @($existingNames | Where-Object {
            $_.Equals($HandlerPath, [System.StringComparison]::OrdinalIgnoreCase)
        }).Count -ne 0
        $existingValue = if ($hadExistingValue) {
            $registryKey.GetValue($HandlerPath, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        } else { $null }
        $existingKind = if ($hadExistingValue) { $registryKey.GetValueKind($HandlerPath) } else { $null }
        try {
            $registryKey.SetValue($HandlerPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord)

            $handledDirectory = Join-Path $temporaryRoot "wer-handled"
            [System.IO.Directory]::CreateDirectory($handledDirectory) | Out-Null
            $handled = Start-HarnessProcess @('--wer-handled', $HandlerPath, $handledDirectory)
            [void](Complete-HarnessProcess $handled 15000 $true)
            [void](Assert-ReportPairs $handledDirectory 0)

            $registrationDirectory = Join-Path $temporaryRoot "wer-register-cycle"
            [System.IO.Directory]::CreateDirectory($registrationDirectory) | Out-Null
            $registration = Start-HarnessProcess @('--wer-register-cycle', $HandlerPath, $registrationDirectory)
            [void](Complete-HarnessProcess $registration 15000 $true)
            [void](Assert-ReportPairs $registrationDirectory 0)

            $accessDirectory = Join-Path $temporaryRoot "wer-access"
            [System.IO.Directory]::CreateDirectory($accessDirectory) | Out-Null
            Invoke-WerCrash '--wer-access' $accessDirectory

            $stackDirectory = Join-Path $temporaryRoot "wer-stack"
            [System.IO.Directory]::CreateDirectory($stackDirectory) | Out-Null
            Invoke-WerCrash '--wer-stack' $stackDirectory
        }
        finally {
            if ($hadExistingValue) {
                $registryKey.SetValue($HandlerPath, $existingValue, $existingKind)
            }
            else {
                $registryKey.DeleteValue($HandlerPath, $false)
            }
            $registryKey.Dispose()
            $registryBase.Dispose()
        }
    }

    Write-Host "Crash harness passed writable-fallback, retention, no-DbgHelp, Unicode/long-path, concurrency, and DLL-reload checks."
    if ($IncludeWerIntegration) {
        Write-Host "Real WER register/unregister, handled-exception, access-violation, and stack-overflow checks passed."
    }
    else {
        Write-Host "Real WER crash checks were not requested; add -IncludeWerIntegration to run them."
    }
}
finally {
    if ($KeepArtifacts) {
        Write-Host "Crash-harness artifacts retained at: $temporaryRoot"
    }
    elseif (Test-Path -LiteralPath $temporaryRoot) {
        $resolvedTemporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot).TrimEnd('\', '/')
        $resolvedTemporaryParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/')
        if (-not $resolvedTemporaryRoot.StartsWith(
            $resolvedTemporaryParent + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove crash-harness artifacts outside the temporary directory: $resolvedTemporaryRoot"
        }
        [System.IO.Directory]::Delete((ConvertTo-ExtendedPath $resolvedTemporaryRoot), $true)
    }
}
