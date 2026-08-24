#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^\d+\.\d+\.\d+(?:-beta\.\d+)?$")]
    [string]$ExpectedVersion = "2.0.0-beta.4",
    [string]$BuildOutputDirectory = "",
    [string]$PdbPath = "",
    [string]$LoaderPdbPath = "",
    [string]$CrashHandlerPdbPath = "",
    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$ExpectedLoaderVersion = "1.1.0",
    [ValidateRange(1, 65535)]
    [int]$ExpectedRuntimeAbi = 1,
    [ValidatePattern("^(|[0-9a-fA-F]{64})$")]
    [string]$UpdateSignerCertSha256 = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$dataDirectory = Join-Path $RepositoryRoot "vSMR\data"
$normalizer = Join-Path $RepositoryRoot "vSMR\tools\normalize_runtime_data.ps1"
$AvisoFileNamePattern = '^(?<airport>[A-Za-z0-9]{4})(?:_[A-Za-z0-9][A-Za-z0-9_-]{0,47})?\.geojson$'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-File {
    param([string]$Path)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "Required file is missing: $Path"
}

function Get-PeMachine {
    param([string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        Assert-True ($reader.ReadUInt16() -eq 0x5A4D) "$Path has no DOS/PE header."
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        Assert-True ($peOffset -ge 0 -and $peOffset -le ($stream.Length - 6)) "$Path has an invalid PE offset."
        $stream.Position = $peOffset
        Assert-True ($reader.ReadUInt32() -eq 0x00004550) "$Path has an invalid PE signature."
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

foreach ($relativePath in @(
    "vSMR\src\plugin\Plugin.hpp",
    "vSMR\src\bootstrap\RuntimeApi.hpp",
    "vSMR\src\bootstrap\loader\BootstrapLoader.cpp",
    "vSMR\src\bootstrap\loader\LoaderResources.rc",
    "vSMR\src\bootstrap\loader\LoaderVersion.hpp",
    "vSMR\src\bootstrap\loader\vSMRLoader.vcxproj",
    "vSMR\src\updater\UpdaterCore.cpp",
    "vSMR\src\updater\UpdaterCore.hpp",
    "vSMR\src\platform\windows\PrecompiledHeader.cpp",
    "vSMR\src\platform\windows\PrecompiledHeader.hpp",
    "vSMR\src\platform\windows\ResourceIds.h",
    "vSMR\src\platform\windows\WindowsTargetVersion.hpp",
    "vSMR\src\crash\CrashReportProtocol.hpp",
    "vSMR\src\crash\CrashReportSupport.hpp",
    "vSMR\src\crash\CrashReporter.hpp",
    "vSMR\src\crash\CrashRuntime.hpp",
    "vSMR\resources\vSMR.def",
    "vSMR\resources\vSMR.rc",
    "vSMR\vSMR.vcxproj",
    "vSMR\src\crash\handler\CrashHandler.cpp",
    "vSMR\src\crash\handler\vSMRCrashHandler.def",
    "vSMR\src\crash\handler\vSMRCrashHandler.rc",
    "vSMR\src\crash\handler\vSMRCrashHandler.vcxproj",
    "vSMR\src\crash\handler\vSMRCrashHandler.vcxproj.filters",
    "vSMR\tools\crash_harness\CrashHarness.cpp",
    "vSMR\tools\crash_harness\run_crash_harness.ps1",
    "vSMR\tools\crash_harness\vSMRCrashHarness.vcxproj",
    "vSMR\tools\crash_harness\vSMRCrashHarness.vcxproj.filters",
    "vSMR\tools\create_github_release.ps1",
    "vSMR\tools\import_vsid_holding_points.ps1",
    "vSMR.sln",
    "vSMR\data\AVISO-UPDATE-POLICY.json",
    "vSMR\data\vSMR_Profiles.json",
    "vSMR\data\ICAO_Aircraft.json",
    "vSMR\data\airports_hp.json",
    "vSMR\data\Licenses\DEPENDENCIES.md",
    "vSMR\data\Licenses\ASSET_PROVENANCE.md",
    "vSMR\src\control_center\web\index.html",
    "vSMR\src\control_center\web\styles.css",
    "vSMR\src\control_center\web\app.js",
    "vSMR\src\control_center\web\data.js",
    "appveyor.yml",
    "README.md",
    "CHANGELOG.md"
)) {
    Assert-File (Join-Path $RepositoryRoot $relativePath)
}

$escapedVersion = [Regex]::Escape($ExpectedVersion)
$headerText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\plugin\Plugin.hpp"))
$resourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\resources\vSMR.rc"))
$runtimeDefinitionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\resources\vSMR.def"))
$runtimeApiText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\bootstrap\RuntimeApi.hpp"))
$loaderSourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\BootstrapLoader.cpp"))
$controlCenterBridgeText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\control_center\ControlCenterBridge.cpp"))
$loaderResourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\LoaderResources.rc"))
$loaderVersionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\LoaderVersion.hpp"))
$crashHandlerResourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\crash\handler\vSMRCrashHandler.rc"))
$crashHandlerDefinitionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\crash\handler\vSMRCrashHandler.def"))
$ciText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "appveyor.yml"))
$readmeText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "README.md"))
$changelogText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "CHANGELOG.md"))
$packageScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\package_release.ps1"))
$verifyPackageScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\verify_release_package.ps1"))
$releaseDriverText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\create_github_release.ps1"))
$solutionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR.sln"))
Assert-True ($headerText -match "MY_PLUGIN_VERSION\s+`"v$escapedVersion`"") "Plugin version macro is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedVersion`"") "Windows FileVersion is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Windows ProductVersion is inconsistent."
$escapedLoaderVersion = [Regex]::Escape($ExpectedLoaderVersion)
Assert-True ($loaderVersionText -match "Value\[\]\s*=\s*`"$escapedLoaderVersion`"") "Loader implementation version is inconsistent."
Assert-True ($loaderResourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedLoaderVersion\.0`"") "Loader FileVersion is inconsistent."
Assert-True ($loaderResourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Loader ProductVersion is inconsistent."
Assert-True ($runtimeApiText -match "AbiVersion\s*=\s*$ExpectedRuntimeAbi" ) "Runtime ABI constant is inconsistent."
$expectedRuntimeDefinitionExports = @('VsmrRuntimeCreate', 'VsmrRuntimeGetAbiVersion', 'VsmrRuntimeShutdown')
$expectedLoaderExports = @(
    '?EuroScopePlugInExit@@YAXXZ',
    '?EuroScopePlugInInit@@YAXPAPAVCPlugIn@EuroScopePlugIn@@@Z'
)
$runtimeDefinitionExports = @($runtimeDefinitionText -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and $_ -ne 'EXPORTS' -and $_ -ne 'LIBRARY' -and -not $_.StartsWith(';')
})
$actualRuntimeDefinitionExportText = (($runtimeDefinitionExports | Sort-Object) -join '|')
$expectedRuntimeDefinitionExportText = (($expectedRuntimeDefinitionExports | Sort-Object) -join '|')
Assert-True ($actualRuntimeDefinitionExportText -ceq $expectedRuntimeDefinitionExportText) "Runtime definition must expose only the exact three-name loader ABI."
Assert-True ($loaderSourceText -match '__declspec\s*\(\s*dllexport\s*\)\s+EuroScopePlugInInit') "Loader does not explicitly export EuroScopePlugInInit."
Assert-True ($loaderSourceText -match '__declspec\s*\(\s*dllexport\s*\)\s+EuroScopePlugInExit') "Loader does not explicitly export EuroScopePlugInExit."
Assert-True ($loaderSourceText -match 'kRuntimeShadowMutexWaitMs\s*=\s*\d+U' -and
    $loaderSourceText -match 'WAIT_ABANDONED') "Runtime-shadow synchronization is not bounded or crash-tolerant."
Assert-True ($loaderSourceText -match '(?s)shadowCacheGuard\.Acquire\s*\(.*?CreateVerifiedShadowCopy\s*\(.*?LoadLibraryExW\s*\(.*?shadowCacheGuard\.Release\s*\(') "Runtime-shadow synchronization does not cover copy, pruning, and LoadLibrary."
Assert-True ($loaderSourceText -match '(?s)FileHandleGuard\s+shadowLease\s*\(\s*::CreateFileW\s*\(.*?GENERIC_READ\s*,\s*FILE_SHARE_READ\s*,.*?ResolveHandlePath\s*\(\s*shadowLease\.Get\(\).*?HashFileSha256\s*\(\s*resolvedShadowPath.*?LoadLibraryExW\s*\(\s*resolvedShadowPath\.c_str\(\).*?LOAD_LIBRARY_SEARCH_DEFAULT_DIRS\s*\).*?shadowLease\.Reset\s*\(\s*\)') "The finalized runtime shadow is not deny-write/delete leased, handle-resolved, rehashed, and safely loaded."
Assert-True ($loaderSourceText -notmatch 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR') "The runtime loader must not resolve dependencies beside a Temp-fallback shadow DLL."
Assert-True ($loaderSourceText -match '(?s)RuntimeShadowDirectories\s*\(.*?CSIDL_LOCAL_APPDATA.*?GetEnvironmentVariableW\s*\(.*?LOCALAPPDATA.*?GetTempPathW\s*\(' -and
    $loaderSourceText -match '(?s)for\s*\([^)]*directory\s*:\s*directories\).*?CreateVerifiedShadowCopyInDirectory\s*\(') "Runtime-shadow storage does not fall back from unwritable LocalAppData to Temp."
$updaterDirectorySource = [Regex]::Match(
    $controlCenterBridgeText,
    '(?s)std::filesystem::path\s+UpdaterDirectory\s*\(\s*\).*?(?=std::string\s+InstalledVersion)').Value
Assert-True (-not [string]::IsNullOrWhiteSpace($updaterDirectorySource) -and
    $updaterDirectorySource -match 'FOLDERID_LocalAppData' -and
    $updaterDirectorySource -match 'EnvironmentDirectory\s*\(\s*L"LOCALAPPDATA"' -and
    $updaterDirectorySource -notmatch 'TemporaryDirectory|GetTempPath') "Control Center must keep updater config/state/action on the deterministic LocalAppData journal."
Assert-True ($resourceText -match '(?m)^#include\s+"platform/windows/ResourceIds\.h"\s*$') "The resource compiler does not use the relocated resource-ID header."
Assert-True ($resourceText -match '(?m)^#include\s+"platform/windows/WindowsTargetVersion\.hpp"\s*$') "The resource compiler does not use the relocated Windows target header."
Assert-True ($crashHandlerResourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedVersion`"") "Crash-handler FileVersion is inconsistent."
Assert-True ($crashHandlerResourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Crash-handler ProductVersion is inconsistent."
$expectedCrashHandlerExports = @(
    'OutOfProcessExceptionEventCallback',
    'OutOfProcessExceptionEventSignatureCallback',
    'OutOfProcessExceptionEventDebuggerLaunchCallback'
)
$crashHandlerExportLines = @($crashHandlerDefinitionText -split "`r?`n" | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and $_.Trim() -ne 'EXPORTS' -and -not $_.TrimStart().StartsWith(';')
})
Assert-True ($crashHandlerExportLines.Count -eq $expectedCrashHandlerExports.Count) "Crash handler must expose only the three WER callbacks."
foreach ($exportName in $expectedCrashHandlerExports) {
    Assert-True ($crashHandlerDefinitionText -match "(?m)^\s*$exportName\s*$") "Crash handler does not declare the exact public export $exportName."
}
Assert-True ($resourceText -match "(?s)#ifdef\s+_DEBUG\s+FILEFLAGS\s+0x3L\s+#else\s+FILEFLAGS\s+0x2L") "Windows beta resource is missing the prerelease flag."
Assert-True ($ciText -match "(?m)^version:\s+$escapedVersion\.\{build\}\s*$") "AppVeyor version is inconsistent."
Assert-True ($readmeText.Contains($ExpectedVersion)) "README does not identify the beta version."
Assert-True ($changelogText -match "\[$escapedVersion\]") "CHANGELOG has no beta release section."
Assert-True ($packageScriptText -match '_vsmr-package-.+NewGuid') "Release packaging must use a private GUID staging directory."
Assert-True (-not ($packageScriptText -match 'Join-Path\s+\$ArtifactsDirectory\s+"_staging"')) "Release packaging must not delete a caller-owned fixed _staging directory."
Assert-True ($packageScriptText -match 'SignedCms' -and
    $packageScriptText -match 'Write-DetachedCmsSignature' -and
    $packageScriptText -match '\.p7s') "Release packaging does not create a detached CMS update signature."
Assert-True ($packageScriptText -match 'minimum_loader_version' -and
    $packageScriptText -match 'vSMR_Data/Runtime/vSMR\.Runtime\.dll') "Release packaging does not emit the loader/runtime update contract."
Assert-True ($packageScriptText -match 'AVISO-UPDATE-POLICY\.json' -and
    $packageScriptText -match 'AVISO-INVENTORY\.json') "Release packaging does not validate the AVISO policy and generate its inventory."
Assert-True (-not ($packageScriptText -match 'Write-Utf8NoBom\s+"?\$archivePath\.sha256')) "Release packaging must not create a redundant external .zip.sha256 asset."
Assert-True (-not ($verifyPackageScriptText -match '\[string\]\$ChecksumPath')) "Release verification still depends on the obsolete external .zip.sha256 asset."
Assert-True ($releaseDriverText -match 'VSMR_SIGNING_CERT_THUMBPRINT' -and
    $releaseDriverText -match 'VSMR_UPDATE_SIGNER_CERT_SHA256' -and
    $releaseDriverText -match '-RequireSignature' -and
    $releaseDriverText -match '-RequirePublishedBeta3Migration' -and
    $releaseDriverText -notmatch '(?m)^\s*-ForceNonPublishable(?:\s|$)') "The GitHub release driver is not fail-closed for signatures and the published beta.3 migration fixture."
$installScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\data\Tools\install_vsmr.ps1"))
$restoreScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\data\Tools\restore_vsmr_backup.ps1"))
Assert-True ($installScriptText -match '\[switch\]\$PreserveLoader' -and
    $installScriptText -match 'validation_scope' -and
    $installScriptText -match 'installed_loader_sha256' -and
    $installScriptText -match 'manual_loader_update_required') "Installer lacks minimum-loader enforcement, preserve-loader mode, or scoped live-install provenance."
Assert-True ($installScriptText -match '\[switch\]\$ReloadAviso' -and
    $installScriptText -match '\[switch\]\$ReplaceModifiedAviso' -and
    $installScriptText -match 'AVISO-UPDATE-REPORT\.json') "Installer lacks AVISO reload, modified-file protection, or migration reporting."
Assert-True ($restoreScriptText -match '\[switch\]\$PreserveLoader') "Rollback helper lacks preserve-loader mode."
Assert-True ($solutionText -match '(?m)^\s*Release\|Win32\s*=\s*Release\|Win32\s*$') "The solution does not expose Release|Win32."
Assert-True (-not ($solutionText -match '(?m)^\s*Debug\|Win32\s*=\s*Debug\|Win32\s*$')) "The solution must default to its sole Release|Win32 configuration."
Assert-True ($solutionText -match 'vSMRCrashHandler.+vSMR\\src\\crash\\handler\\vSMRCrashHandler\.vcxproj') "The WER crash-handler project is missing from the solution."
Assert-True ($solutionText -match '"vSMR\.Runtime",\s+"vSMR\\vSMR\.vcxproj"') "The runtime project is missing or ambiguously named in the solution."
Assert-True ($solutionText -match '"vSMR",\s+"vSMR\\src\\bootstrap\\loader\\vSMRLoader\.vcxproj"') "The stable loader project is missing from the solution."

$legacyThreads = @(
    Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot "vSMR\src") -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.hpp', '.h') } |
        Select-String -Pattern '_beginthread|_beginthreadex'
)
Assert-True ($legacyThreads.Count -eq 0) "Detached CRT thread creation remains in the source: $($legacyThreads.Path -join ', ')."

[xml]$projectXml = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\vSMR.vcxproj"))
$namespace = New-Object System.Xml.XmlNamespaceManager($projectXml.NameTable)
$namespace.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")
$releaseDefinitions = @($projectXml.SelectNodes("//msb:ItemDefinitionGroup", $namespace) |
    Where-Object { [string]$_.Condition -like '*Release|Win32*' })
Assert-True ($releaseDefinitions.Count -eq 1) "Release|Win32 compiler settings were not found uniquely."
$releaseCompile = $releaseDefinitions[0].SelectSingleNode("msb:ClCompile", $namespace)
$releaseLink = $releaseDefinitions[0].SelectSingleNode("msb:Link", $namespace)
$releaseResourceCompile = $releaseDefinitions[0].SelectSingleNode("msb:ResourceCompile", $namespace)
Assert-True ($releaseCompile.WarningLevel -eq 'Level4') "Release must compile at warning level 4."
Assert-True ($releaseCompile.ExternalWarningLevel -eq 'TurnOffAllWarnings') "Vendored headers are not isolated at external warning level 0."
Assert-True (-not ([string]$releaseCompile.AdditionalIncludeDirectories -match '(?i)lib[\\/]include')) "Vendored headers must not be compiled as first-party includes."
Assert-True ([string]$releaseCompile.AdditionalIncludeDirectories -eq '$(ProjectDir)src;%(AdditionalIncludeDirectories)') "Release must use src as its sole first-party include root."
Assert-True ([string]$releaseCompile.PrecompiledHeaderFile -eq 'platform/windows/PrecompiledHeader.hpp') "Release does not use the canonical feature-qualified PCH token."
$mainPchFiles = @($projectXml.SelectNodes("//msb:ItemDefinitionGroup/msb:ClCompile/msb:PrecompiledHeaderFile", $namespace))
Assert-True ($mainPchFiles.Count -eq 2 -and
    @($mainPchFiles | Where-Object { [string]$_.InnerText -ne 'platform/windows/PrecompiledHeader.hpp' }).Count -eq 0) "Debug and Release must use the same feature-qualified PCH token."
Assert-True ($releaseCompile.SDLCheck -eq 'true') "Release SDL checks are disabled."
Assert-True ($releaseCompile.BufferSecurityCheck -eq 'true') "Release /GS buffer security checks are disabled."
Assert-True ([string]$releaseResourceCompile.AdditionalIncludeDirectories -eq '$(ProjectDir)src;$(IntDir);%(AdditionalIncludeDirectories)') "Resource compilation must use src as its first-party include root."
Assert-True ($releaseLink.GenerateDebugInformation -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace([string]$releaseLink.ProgramDatabaseFile)) "Release private PDB generation is disabled."
$releaseExternalPaths = @($projectXml.SelectNodes("//msb:PropertyGroup/msb:ExternalIncludePath", $namespace) |
    Where-Object { [string]$_.ParentNode.Condition -like '*Release|Win32*' })
Assert-True ($releaseExternalPaths.Count -eq 1 -and
    [string]$releaseExternalPaths[0].InnerText -match '(?i)lib[\\/]include') "The vendored include directory is not configured as external for Release|Win32."
$pchCreator = $projectXml.SelectSingleNode("//msb:ClCompile[@Include='src\platform\windows\PrecompiledHeader.cpp']", $namespace)
$pchCreatorModes = if ($null -eq $pchCreator) { @() } else {
    @($pchCreator.SelectNodes("msb:PrecompiledHeader", $namespace) | ForEach-Object { [string]$_.InnerText })
}
Assert-True ($null -ne $pchCreator -and $pchCreatorModes.Count -eq 2 -and
    @($pchCreatorModes | Where-Object { $_ -ne 'Create' }).Count -eq 0) "The canonical PCH source must create the PCH in Debug and Release."
$crashHandlerProjectReference = $projectXml.SelectSingleNode("//msb:ProjectReference[contains(@Include, 'vSMRCrashHandler.vcxproj')]", $namespace)
Assert-True ($null -ne $crashHandlerProjectReference) "vSMR does not build the WER crash-handler dependency."
Assert-True ([string]$projectXml.Project.PropertyGroup[1].TargetName -eq 'vSMR.Runtime' -or
    @($projectXml.SelectNodes("//msb:PropertyGroup/msb:TargetName", $namespace) | Where-Object { [string]$_.InnerText -eq 'vSMR.Runtime' }).Count -eq 2) "Runtime output is not named vSMR.Runtime.dll."
Assert-True (@($projectXml.SelectNodes("//msb:PropertyGroup/msb:OutDir", $namespace) | Where-Object {
    [string]$_.InnerText -eq '$(ProjectDir)bin\$(Configuration)\Runtime\'
}).Count -eq 2) "Runtime private build output is not isolated below bin/<Configuration>/Runtime."
Assert-True ($projectXml.OuterXml -match 'vSMR_Data\\Runtime\\vSMR\.Runtime\.dll') "Runtime project does not copy its canonical DLL under vSMR_Data/Runtime."

[xml]$loaderProjectXml = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\vSMRLoader.vcxproj"))
$loaderNamespace = New-Object System.Xml.XmlNamespaceManager($loaderProjectXml.NameTable)
$loaderNamespace.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")
$loaderReleaseDefinitions = @($loaderProjectXml.SelectNodes("//msb:ItemDefinitionGroup", $loaderNamespace) |
    Where-Object { [string]$_.Condition -like '*Release|Win32*' })
Assert-True ($loaderReleaseDefinitions.Count -eq 1) "Loader Release|Win32 settings were not found uniquely."
$loaderReleaseCompile = $loaderReleaseDefinitions[0].SelectSingleNode("msb:ClCompile", $loaderNamespace)
$loaderReleaseLink = $loaderReleaseDefinitions[0].SelectSingleNode("msb:Link", $loaderNamespace)
Assert-True ([string]$loaderReleaseCompile.RuntimeLibrary -eq 'MultiThreaded') "Stable loader must use the static Release CRT."
Assert-True ([string]$loaderReleaseCompile.WarningLevel -eq 'Level4' -and
    [string]$loaderReleaseCompile.SDLCheck -eq 'true' -and
    [string]$loaderReleaseCompile.BufferSecurityCheck -eq 'true') "Loader compiler hardening is incomplete."
Assert-True ([string]$loaderReleaseCompile.PreprocessorDefinitions -match 'VSMR_UPDATE_SIGNER_CERT_SHA256') "Loader does not compile the updater signer-certificate pin."
Assert-True ([string]$loaderReleaseLink.GenerateDebugInformation -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace([string]$loaderReleaseLink.ProgramDatabaseFile)) "Loader private PDB generation is disabled."
Assert-True (@($loaderProjectXml.SelectNodes("//msb:PropertyGroup/msb:TargetName", $loaderNamespace) | Where-Object {
    [string]$_.InnerText -eq 'vSMR'
}).Count -eq 2) "Loader output is not the stable root vSMR.dll."
Assert-True ($loaderProjectXml.OuterXml -match 'UpdaterCore\.cpp') "Stable loader does not include the updater core."

[xml]$crashHandlerProjectXml = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\src\crash\handler\vSMRCrashHandler.vcxproj"))
$crashNamespace = New-Object System.Xml.XmlNamespaceManager($crashHandlerProjectXml.NameTable)
$crashNamespace.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")
$crashReleaseConfiguration = @($crashHandlerProjectXml.SelectNodes("//msb:PropertyGroup", $crashNamespace) |
    Where-Object { [string]$_.Condition -like '*Release|Win32*' -and [string]$_.Label -eq 'Configuration' })
$crashReleaseDefinitions = @($crashHandlerProjectXml.SelectNodes("//msb:ItemDefinitionGroup", $crashNamespace) |
    Where-Object { [string]$_.Condition -like '*Release|Win32*' })
Assert-True ($crashReleaseConfiguration.Count -eq 1 -and
    [string]$crashReleaseConfiguration[0].ConfigurationType -eq 'DynamicLibrary') "The crash handler is not a Release|Win32 DLL."
Assert-True ($crashReleaseDefinitions.Count -eq 1) "Crash-handler Release|Win32 settings were not found uniquely."
$crashReleaseProperties = @($crashHandlerProjectXml.SelectNodes("//msb:PropertyGroup", $crashNamespace) |
    Where-Object { [string]$_.Condition -like '*Release|Win32*' -and [string]$_.Label -ne 'Configuration' })
Assert-True ($crashReleaseProperties.Count -eq 1 -and
    [string]$crashReleaseProperties[0].OutDir -match '\$\(ProjectDir\)bin\\\$\(Configuration\)') "Crash-handler build output is not isolated from the release root."
$crashReleaseCompile = $crashReleaseDefinitions[0].SelectSingleNode("msb:ClCompile", $crashNamespace)
$crashReleaseLink = $crashReleaseDefinitions[0].SelectSingleNode("msb:Link", $crashNamespace)
Assert-True ([string]$crashReleaseCompile.RuntimeLibrary -eq 'MultiThreaded') "The out-of-process crash handler must use the static Release CRT."
Assert-True ([string]$crashReleaseCompile.WarningLevel -eq 'Level4') "The crash handler must compile at warning level 4."
Assert-True ([string]$crashReleaseCompile.SDLCheck -eq 'true' -and
    [string]$crashReleaseCompile.BufferSecurityCheck -eq 'true') "Crash-handler compiler hardening is incomplete."
Assert-True ([string]$crashReleaseCompile.PrecompiledHeader -eq 'NotUsing') "The crash handler must remain independent of vSMR's MFC precompiled header."
Assert-True ([string]$crashReleaseLink.GenerateDebugInformation -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace([string]$crashReleaseLink.ProgramDatabaseFile)) "Crash-handler private PDB generation is disabled."

& $normalizer -Mode Check -DataDirectory $dataDirectory

$profiles = Get-Content -LiteralPath (Join-Path $dataDirectory "vSMR_Profiles.json") -Raw | ConvertFrom-Json
Assert-True ($profiles -is [System.Array]) "Bundled profiles must be an array."
$profileEntries = @($profiles | Where-Object { $_.PSObject.Properties.Name -contains 'name' })
$metadataEntries = @($profiles | Where-Object { $_.PSObject.Properties.Name -contains '_vsmr' })
Assert-True ($profileEntries.Count -gt 0) "Bundled profiles contain no named profile."
Assert-True ($metadataEntries.Count -eq 1) "Bundled profiles must contain exactly one metadata entry."
$profileNames = @{}
foreach ($profile in $profileEntries) {
    Assert-True ([int]$profile.schema_version -eq 2) "Profile '$($profile.name)' is not schema 2."
    Assert-True ($profile.labels -is [pscustomobject]) "Profile '$($profile.name)' has no labels object."
    Assert-True ($profile.targets -is [pscustomobject]) "Profile '$($profile.name)' has no targets object."
    Assert-True ($profile.targets.departure.lineup -is [pscustomobject]) "Profile '$($profile.name)' has no Line Up target color."
    Assert-True ($profile.labels.departure.background_lineup_color -is [pscustomobject]) "Profile '$($profile.name)' has no Line Up tag color."
    Assert-True ($profile.labels.departure.status_definitions.lnup -is [pscustomobject]) "Profile '$($profile.name)' has no LNUP tag definition."
    foreach ($mode in @($profile.filters.display_modes.items)) {
        Assert-True ($mode.statuses.lineup -is [bool]) "Profile '$($profile.name)' display mode '$($mode.name)' has no Line Up visibility flag."
    }
    $key = ([string]$profile.name).Trim().ToUpperInvariant()
    Assert-True (-not $profileNames.ContainsKey($key)) "Duplicate profile name '$($profile.name)'."
    $profileNames[$key] = $true
}
Assert-True ([int]$metadataEntries[0]._vsmr.schema_version -eq 1) "Profile metadata is not schema 1."
$lastActive = ([string]$metadataEntries[0]._vsmr.last_active_profile).Trim().ToUpperInvariant()
Assert-True ([string]::IsNullOrWhiteSpace($lastActive) -or $profileNames.ContainsKey($lastActive)) "last_active_profile does not exist."

$aircraft = Get-Content -LiteralPath (Join-Path $dataDirectory "ICAO_Aircraft.json") -Raw | ConvertFrom-Json
$aircraftRecords = @($aircraft.PSObject.Properties)
Assert-True ($aircraftRecords.Count -ge 500) "Aircraft database is unexpectedly small."
foreach ($record in $aircraftRecords) {
    Assert-True ([double]$record.Value.length -gt 0.0 -and [double]$record.Value.wingspan -gt 0.0) "Invalid aircraft dimensions for $($record.Name)."
}

$holdingPointCatalog = Get-Content -LiteralPath (Join-Path $dataDirectory "airports_hp.json") -Raw | ConvertFrom-Json
$holdingPointAirports = @($holdingPointCatalog.PSObject.Properties)
Assert-True ($holdingPointAirports.Count -gt 0) "Holding-point catalog contains no airports."
$holdingPointRunwayCount = 0
$holdingPointValueCount = 0
foreach ($airport in $holdingPointAirports) {
    Assert-True ($airport.Name -match '^[A-Z0-9]{4}$') "Invalid holding-point airport '$($airport.Name)'."
    $runways = @($airport.Value.PSObject.Properties)
    Assert-True ($runways.Count -gt 0) "Holding-point airport '$($airport.Name)' contains no runways."
    $holdingPointRunwayCount += $runways.Count
    foreach ($runway in $runways) {
        Assert-True ($runway.Name -match '^\d{2}[LCR]?$') "Invalid runway '$($runway.Name)' for $($airport.Name)."
        $points = @($runway.Value)
        Assert-True ($points.Count -gt 0) "Runway $($airport.Name)/$($runway.Name) contains no holding points."
        $holdingPointValueCount += $points.Count
        $uniquePoints = @{}
        foreach ($pointValue in $points) {
            $point = ([string]$pointValue).Trim().ToUpperInvariant()
            Assert-True ($point -match '^[A-Z0-9/-]{1,8}$') "Invalid holding point '$point' for $($airport.Name)/$($runway.Name)."
            Assert-True (-not $uniquePoints.ContainsKey($point)) "Duplicate holding point '$point' for $($airport.Name)/$($runway.Name)."
            $uniquePoints[$point] = $true
        }
    }
}
if ($ExpectedVersion -eq '2.0.0-beta.4') {
    Assert-True ($holdingPointAirports.Count -eq 73 -and
        $holdingPointRunwayCount -eq 189 -and
        $holdingPointValueCount -eq 455) "Beta 4 must contain the complete imported vSID holding-point catalogue (73 airports, 189 runways, 455 points)."
}

$avisoFiles = @(
    # The build deliberately excludes the aggregate ALL_FR_* and _LFXX
    # sources. It packages canonical airport files and controlled variants
    # such as LFPG_Custom.geojson.
    Get-ChildItem -LiteralPath (Join-Path $dataDirectory "AVISO") -Filter "*.geojson" -File |
        Where-Object { $_.Name -match $AvisoFileNamePattern }
)
Assert-True ($avisoFiles.Count -gt 0) "No bundled AVISO files were found."
$avisoFileNames = @($avisoFiles | ForEach-Object { $_.Name })
$avisoPolicyPath = Join-Path $dataDirectory "AVISO-UPDATE-POLICY.json"
$avisoPolicy = Get-Content -LiteralPath $avisoPolicyPath -Raw | ConvertFrom-Json
Assert-True ([int]$avisoPolicy.schema_version -eq 1) "AVISO update policy is not schema 1."
Assert-True ([string]$avisoPolicy.release -eq $ExpectedVersion) "AVISO update policy does not target $ExpectedVersion."
Assert-True ($avisoPolicy.aviso -is [pscustomobject]) "AVISO update policy has no aviso object."
$avisoPolicyPropertyNames = @($avisoPolicy.aviso.PSObject.Properties | ForEach-Object { $_.Name })
Assert-True ($avisoPolicyPropertyNames -contains 'replace' -and $avisoPolicy.aviso.replace -is [System.Array]) "AVISO update policy replace must be an explicit JSON array."
Assert-True ($avisoPolicyPropertyNames -contains 'delete' -and $avisoPolicy.aviso.delete -is [System.Array]) "AVISO update policy delete must be an explicit JSON array."
$avisoUpdateMode = [string]$avisoPolicy.aviso.update
$avisoModifiedFilePolicy = [string]$avisoPolicy.aviso.modified_files
Assert-True ($avisoUpdateMode -in @('none', 'selected', 'all')) "AVISO update policy has unsupported update mode '$avisoUpdateMode'."
Assert-True ($avisoModifiedFilePolicy -in @('preserve', 'protect_setting', 'replace')) "AVISO update policy has unsupported modified-files mode '$avisoModifiedFilePolicy'."
$avisoPolicyReplace = @($avisoPolicy.aviso.replace | ForEach-Object { [string]$_ })
$avisoPolicyDelete = @($avisoPolicy.aviso.delete | ForEach-Object { [string]$_ })
foreach ($name in @($avisoPolicyReplace) + @($avisoPolicyDelete)) {
    Assert-True ($name -match $AvisoFileNamePattern) "AVISO update policy contains unsafe or noncanonical filename '$name'."
}
$normalizedAvisoPolicyReplace = @($avisoPolicyReplace | ForEach-Object { $_.ToUpperInvariant() })
$normalizedAvisoPolicyDelete = @($avisoPolicyDelete | ForEach-Object { $_.ToUpperInvariant() })
Assert-True (@($normalizedAvisoPolicyReplace | Sort-Object -Unique).Count -eq $normalizedAvisoPolicyReplace.Count) "AVISO update policy contains duplicate replacement filenames."
Assert-True (@($normalizedAvisoPolicyDelete | Sort-Object -Unique).Count -eq $normalizedAvisoPolicyDelete.Count) "AVISO update policy contains duplicate deletion filenames."
$avisoPolicyOverlap = @($normalizedAvisoPolicyReplace | Where-Object { $normalizedAvisoPolicyDelete -contains $_ })
Assert-True ($avisoPolicyOverlap.Count -eq 0) "AVISO update policy both replaces and deletes: $($avisoPolicyOverlap -join ', ')."
if ($avisoUpdateMode -eq 'selected') {
    Assert-True ($avisoPolicyReplace.Count -gt 0) "Selected AVISO updates require at least one replacement filename."
    foreach ($name in $avisoPolicyReplace) {
        Assert-True ($avisoFileNames -contains $name) "Selected AVISO replacement is not bundled: $name."
    }
}
else {
    Assert-True ($avisoPolicyReplace.Count -eq 0) "AVISO update mode '$avisoUpdateMode' must leave replace empty."
}
foreach ($name in $avisoPolicyDelete) {
    Assert-True ($avisoFileNames -notcontains $name) "Deleted AVISO '$name' is still bundled."
}
if ($ExpectedVersion -eq '2.0.0-beta.4') {
    Assert-True ($avisoUpdateMode -eq 'all' -and $avisoModifiedFilePolicy -eq 'replace') "Beta 4 must replace every AVISO, including locally modified files, for the Night/Day schema migration."
    Assert-True ($avisoFileNames -contains 'LFPG_Custom.geojson') "Beta 4 must bundle the controlled LFPG_Custom.geojson variant."
    $requiredBeta4Deletes = @('LFMM.geojson', 'LFPG_Dyna_fixed.geojson')
    Assert-True ($avisoPolicyDelete.Count -eq $requiredBeta4Deletes.Count -and
        @($requiredBeta4Deletes | Where-Object { $avisoPolicyDelete -notcontains $_ }).Count -eq 0) "Beta 4 must remove LFMM.geojson and LFPG_Dyna_fixed.geojson and no other AVISO file."
}
$avisoPaletteStyleCount = 0
$avisoPaletteColorCount = 0
$avisoPaletteColorKeys = @('fill', 'stroke', 'marker-color', 'text-color', 'text-halo-color')
function Assert-AvisoPaletteData {
    param($Document, [string]$Name)

    Assert-True ([string]$Document.metadata.default_color_palette -eq 'night') "$Name does not declare Night as its base AVISO palette."
    $palettes = @($Document.metadata.color_palettes)
    Assert-True ($palettes.Count -eq 2 -and [string]$palettes[0] -eq 'night' -and [string]$palettes[1] -eq 'day') "$Name must declare Night and Day AVISO palettes."

    foreach ($styleProperty in @($Document.styles.PSObject.Properties)) {
        $paint = $styleProperty.Value.paint
        if ([string]$styleProperty.Value.object_type -eq 'Label') {
            Assert-True ($null -ne $paint -and
                $paint.PSObject.Properties.Name -contains 'zoomLevel') "$Name label style '$($styleProperty.Name)' has no zoom visibility threshold."
            $zoomLevel = $paint.zoomLevel
            Assert-True (($zoomLevel -is [int]) -or ($zoomLevel -is [long])) "$Name label style '$($styleProperty.Name)' has a non-integer zoom visibility threshold."
            Assert-True ([int]$zoomLevel -ge 0 -and [int]$zoomLevel -le 14) "$Name label style '$($styleProperty.Name)' has an out-of-range zoom visibility threshold."
        }
        if ($null -eq $paint -or
            -not ($paint.PSObject.Properties.Name -contains 'palette-overrides')) {
            continue
        }
        Assert-True ($Name -ne 'LFMN.geojson') "LFMN must render identically in Day and Night modes and cannot contain palette overrides."
        $overrides = $paint.'palette-overrides'
        Assert-True ($overrides -is [pscustomobject]) "$Name style '$($styleProperty.Name)' has invalid palette-overrides."
        foreach ($paletteProperty in @($overrides.PSObject.Properties)) {
            Assert-True ($paletteProperty.Name -eq 'day') "$Name style '$($styleProperty.Name)' has an unsupported '$($paletteProperty.Name)' palette override."
            Assert-True ($paletteProperty.Value -is [pscustomobject]) "$Name style '$($styleProperty.Name)' has an invalid Day palette."
            $script:avisoPaletteStyleCount++
            foreach ($colorProperty in @($paletteProperty.Value.PSObject.Properties)) {
                Assert-True ($colorProperty.Name -in $script:avisoPaletteColorKeys) "$Name style '$($styleProperty.Name)' has unsupported Day property '$($colorProperty.Name)'."
                Assert-True ([string]$colorProperty.Value -match '^#[0-9A-Fa-f]{6}$') "$Name style '$($styleProperty.Name)' has invalid Day color '$($colorProperty.Value)'."
                $script:avisoPaletteColorCount++
            }
        }
    }
}
foreach ($file in $avisoFiles) {
    $document = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Assert-True ($file.Name -match $AvisoFileNamePattern) "$($file.Name) is not a safe AVISO filename."
    $expectedAirport = $Matches['airport'].ToUpperInvariant()
    Assert-True ($document.type -eq 'FeatureCollection') "$($file.Name) is not a FeatureCollection."
    Assert-True ([int]$document.metadata.schema_version -eq 2) "$($file.Name) is not AVISO schema 2."
    Assert-True ([string]$document.metadata.airport -eq $expectedAirport) "$($file.Name) metadata airport does not match its filename."
    Assert-True ([int]$document.metadata.feature_count -eq @($document.features).Count) "$($file.Name) feature count is stale."
    Assert-True ([int]$document.metadata.style_count -eq @($document.styles.PSObject.Properties).Count) "$($file.Name) style count is stale."
    $ids = @($document.features | ForEach-Object { [string]$_.id })
    Assert-True (@($ids | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) "$($file.Name) contains an empty feature id."
    Assert-True (@($ids | Sort-Object -Unique).Count -eq $ids.Count) "$($file.Name) contains duplicate feature ids."
    foreach ($styleProperty in @($document.styles.PSObject.Properties)) {
        Assert-True ([string]$styleProperty.Value.object_type -in @('Area', 'Line', 'Label', 'Point')) "$($file.Name) style '$($styleProperty.Name)' uses non-semantic object_type '$($styleProperty.Value.object_type)'."
    }
    foreach ($feature in @($document.features)) {
        $properties = $feature.properties
        Assert-True ($expectedAirport -ne 'LFMN' -or
            -not ($properties.PSObject.Properties.Name -contains 'palette-overrides')) "LFMN must render identically in Day and Night modes and cannot contain feature palette overrides."
        if ($properties.PSObject.Properties.Name -contains 'vsmr_group_ids') {
            Assert-True ($properties.vsmr_group_ids -is [System.Array]) "$($file.Name) feature '$($feature.id)' vsmr_group_ids must be an array."
            $featureGroupIds = @($properties.vsmr_group_ids)
            foreach ($groupId in $featureGroupIds) {
                Assert-True ($groupId -is [string] -and -not [string]::IsNullOrWhiteSpace([string]$groupId)) "$($file.Name) feature '$($feature.id)' vsmr_group_ids must contain only non-empty strings."
            }
            Assert-True (@($featureGroupIds | Sort-Object -Unique).Count -eq $featureGroupIds.Count) "$($file.Name) feature '$($feature.id)' contains duplicate vsmr_group_ids."
        }
        $geometryType = [string]$feature.geometry.type
        $objectType = [string]$properties.object_type
        if ($geometryType -in @('Polygon', 'MultiPolygon')) {
            Assert-True ($objectType -eq 'Area') "$($file.Name) feature '$($feature.id)' must use object_type Area."
        }
        elseif ($geometryType -in @('LineString', 'MultiLineString')) {
            Assert-True ($objectType -eq 'Line') "$($file.Name) feature '$($feature.id)' must use object_type Line."
        }
        elseif ($geometryType -eq 'Point' -and $objectType -eq 'Label') {
            Assert-True ($objectType -eq 'Label') "$($file.Name) text feature '$($feature.id)' must use object_type Label."
            Assert-True ([string]$properties.geometry_role -eq 'text_label') "$($file.Name) text feature '$($feature.id)' must use geometry_role text_label."
        }
        $isLabel = [string]$feature.geometry.type -eq 'Point' -and (
            [string]$properties.object_type -eq 'Label' -or
            [string]$properties.geometry_role -eq 'text_label'
        )
        if ($isLabel) {
            $styleId = [string]$properties.style_id
            $styleDefinition = $document.styles.PSObject.Properties[$styleId].Value
            $identity = @(
                [string]$feature.id,
                $styleId,
                [string]$properties.category,
                [string]$properties.layer,
                [string]$properties.name,
                [string]$properties.label_class,
                [string]$properties.'text-field',
                [string]$properties.text,
                [string]$styleDefinition.name
            ) -join ' '
            Assert-True ($identity -notmatch '(?i)(^|[^A-Z0-9])(AMSR|TMA|VFR)([^A-Z0-9]|$)') "$($file.Name) contains prohibited label '$($feature.id)'."
        }
    }
    foreach ($styleProperty in @($document.styles.PSObject.Properties)) {
        if ([string]$styleProperty.Value.object_type -ne 'Label' -or
            $expectedAirport -in @('LFPG', 'LFMN')) {
            continue
        }
        $identity = "$($styleProperty.Name) $($styleProperty.Value.name)"
        if ($identity -match '(?i)gate|stand') {
            Assert-True ([int]$styleProperty.Value.paint.zoomLevel -eq 9) "$($file.Name) gate/stand labels must use zoom level 9."
        }
        elseif ($identity -match '(?i)taxiway') {
            Assert-True ([int]$styleProperty.Value.paint.zoomLevel -eq 7) "$($file.Name) taxiway labels must use zoom level 7."
        }
    }
    foreach ($group in @($document.vsmr_groups)) {
        $identity = ([string]$group.id) + ' ' + ([string]$group.name)
        Assert-True ($identity -notmatch '(?i)(^|[^A-Z0-9])(AMSR|TMA|VFR)([^A-Z0-9]|$)') "$($file.Name) contains obsolete AVISO group '$($group.id)'."
    }
    Assert-AvisoPaletteData $document $file.Name
}

$obsoleteDynaAvisoFiles = @(Get-ChildItem -LiteralPath (Join-Path $dataDirectory "AVISO") -Filter "*_Dyna*.geojson" -File)
Assert-True ($obsoleteDynaAvisoFiles.Count -eq 0) "Bundled AVISO data still contains LFPG Dyna files: $($obsoleteDynaAvisoFiles.Name -join ', ')."
Assert-True ($avisoPaletteStyleCount -gt 0 -and $avisoPaletteColorCount -gt 0) "Bundled AVISO data contains no Day palette overrides."

if (-not [string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    $BuildOutputDirectory = [System.IO.Path]::GetFullPath($BuildOutputDirectory)
    Assert-File (Join-Path $BuildOutputDirectory "vSMR.dll")
    Assert-True (Test-Path -LiteralPath (Join-Path $BuildOutputDirectory "vSMR_Data") -PathType Container) "Built vSMR_Data is missing."
    Assert-File (Join-Path $BuildOutputDirectory "vSMR_Data\AVISO-UPDATE-POLICY.json")
    Assert-File (Join-Path $BuildOutputDirectory "vSMR_Data\airports_hp.json")
    $releaseRootNames = @(Get-ChildItem -LiteralPath $BuildOutputDirectory -Force | ForEach-Object Name | Sort-Object)
    Assert-True (($releaseRootNames -join '|') -eq 'vSMR.dll|vSMR_Data') "Release root must contain only vSMR.dll and vSMR_Data; found $($releaseRootNames -join ', ')."
    $builtCrashHandler = Join-Path $BuildOutputDirectory "vSMR_Data\CrashReporter\vSMRCrashHandler.dll"
    $builtRuntime = Join-Path $BuildOutputDirectory "vSMR_Data\Runtime\vSMR.Runtime.dll"
    $builtLoader = Join-Path $BuildOutputDirectory "vSMR.dll"
    Assert-File $builtRuntime
    Assert-File $builtCrashHandler
    Assert-True ((Get-PeMachine $builtLoader) -eq 0x014C) "Built vSMR.dll is not Win32/x86."
    Assert-True ((Get-PeMachine $builtRuntime) -eq 0x014C) "Built vSMR.Runtime.dll is not Win32/x86."
    Assert-True ((Get-PeMachine $builtCrashHandler) -eq 0x014C) "Built vSMRCrashHandler.dll is not Win32/x86."
    $loaderVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($builtLoader)
    Assert-True (($loaderVersion.FileVersion -eq $ExpectedLoaderVersion -or $loaderVersion.FileVersion -eq "$ExpectedLoaderVersion.0") -and
        $loaderVersion.ProductVersion -eq $ExpectedVersion) "Built loader implementation/product versions are inconsistent."
    $runtimeVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($builtRuntime)
    Assert-True ($runtimeVersion.FileVersion -eq $ExpectedVersion -and
        $runtimeVersion.ProductVersion -eq $ExpectedVersion) "Built runtime version does not match $ExpectedVersion."
    $crashHandlerVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($builtCrashHandler)
    Assert-True ($crashHandlerVersion.FileVersion -eq $ExpectedVersion -and
        $crashHandlerVersion.ProductVersion -eq $ExpectedVersion) "Built crash-handler version does not match $ExpectedVersion."
    $builtCrashHandlerExports = @(Get-PeExportNames $builtCrashHandler | Sort-Object)
    $expectedBuiltExports = @($expectedCrashHandlerExports | Sort-Object)
    Assert-True (($builtCrashHandlerExports -join '|') -ceq ($expectedBuiltExports -join '|')) "Built crash-handler export table is not the exact three-name WER ABI."
    $builtRuntimeExports = @(Get-PeExportNames $builtRuntime | Sort-Object)
    Assert-True (($builtRuntimeExports -join '|') -ceq (($expectedRuntimeDefinitionExports | Sort-Object) -join '|')) "Built runtime export table is not the exact three-name loader ABI."
    $builtLoaderExports = @(Get-PeExportNames $builtLoader | Sort-Object)
    Assert-True (($builtLoaderExports -join '|') -ceq (($expectedLoaderExports | Sort-Object) -join '|')) "Built loader export table is not the exact decorated x86 EuroScope SDK ABI."
    if (-not [string]::IsNullOrWhiteSpace($UpdateSignerCertSha256)) {
        $loaderAscii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($builtLoader))
        Assert-True ($loaderAscii.Contains($UpdateSignerCertSha256.ToLowerInvariant())) "Built loader does not contain the configured updater signer certificate pin."
    }

    foreach ($license in @(
        "vSMR_Data\Licenses\vSMR.txt",
        "vSMR_Data\Licenses\RapidJSON.txt",
        "vSMR_Data\Licenses\Microsoft.WebView2-LICENSE.txt",
        "vSMR_Data\Licenses\Microsoft.WebView2-NOTICE.txt",
        "vSMR_Data\Licenses\DEPENDENCIES.md",
        "vSMR_Data\Licenses\ASSET_PROVENANCE.md"
    )) {
        Assert-File (Join-Path $BuildOutputDirectory $license)
    }

    if ([string]::IsNullOrWhiteSpace($PdbPath)) {
        $PdbPath = Join-Path $RepositoryRoot "vSMR\obj\Release\Runtime\vSMR.Runtime.pdb"
    }
    Assert-File ([System.IO.Path]::GetFullPath($PdbPath))
    if ([string]::IsNullOrWhiteSpace($LoaderPdbPath)) {
        $LoaderPdbPath = Join-Path $RepositoryRoot "vSMR\src\bootstrap\loader\obj\Release\vSMR.pdb"
    }
    Assert-File ([System.IO.Path]::GetFullPath($LoaderPdbPath))
    if ([string]::IsNullOrWhiteSpace($CrashHandlerPdbPath)) {
        $CrashHandlerPdbPath = Join-Path $RepositoryRoot "vSMR\src\crash\handler\obj\Release\vSMRCrashHandler.pdb"
    }
    Assert-File ([System.IO.Path]::GetFullPath($CrashHandlerPdbPath))
}

Write-Host "vSMR $ExpectedVersion release validation passed."
