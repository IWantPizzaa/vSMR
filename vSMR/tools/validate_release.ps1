#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^\d+\.\d+\.\d+-beta\.\d+$")]
    [string]$ExpectedVersion = "2.0.0-beta.2",
    [string]$BuildOutputDirectory = "",
    [string]$PdbPath = "",
    [string]$CrashHandlerPdbPath = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$dataDirectory = Join-Path $RepositoryRoot "vSMR\data"
$normalizer = Join-Path $RepositoryRoot "vSMR\tools\normalize_runtime_data.ps1"

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
    "vSMR\include\SMRPlugin.hpp",
    "vSMR\include\CrashReportProtocol.hpp",
    "vSMR\include\CrashReportSupport.hpp",
    "vSMR\include\CrashReporter.hpp",
    "vSMR\include\CrashRuntime.hpp",
    "vSMR\resources\vSMR.rc",
    "vSMR\vSMR.vcxproj",
    "vSMR\crash_handler\CrashHandler.cpp",
    "vSMR\crash_handler\vSMRCrashHandler.def",
    "vSMR\crash_handler\vSMRCrashHandler.rc",
    "vSMR\crash_handler\vSMRCrashHandler.vcxproj",
    "vSMR\crash_handler\vSMRCrashHandler.vcxproj.filters",
    "vSMR\tools\CrashHarness\CrashHarness.cpp",
    "vSMR\tools\CrashHarness\run_crash_harness.ps1",
    "vSMR\tools\CrashHarness\vSMRCrashHarness.vcxproj",
    "vSMR\tools\CrashHarness\vSMRCrashHarness.vcxproj.filters",
    "vSMR.sln",
    "vSMR\data\vSMR_Profiles.json",
    "vSMR\data\ICAO_Aircraft.json",
    "vSMR\data\AVISO\LFPG_Dyna_fixed.geojson",
    "vSMR\data\Licenses\DEPENDENCIES.md",
    "vSMR\data\Licenses\ASSET_PROVENANCE.md",
    "vSMR\vSMR_webUI\index.html",
    "vSMR\vSMR_webUI\styles.css",
    "vSMR\vSMR_webUI\app.js",
    "vSMR\vSMR_webUI\data.js",
    "appveyor.yml",
    "README.md",
    "CHANGELOG.md"
)) {
    Assert-File (Join-Path $RepositoryRoot $relativePath)
}

$escapedVersion = [Regex]::Escape($ExpectedVersion)
$headerText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\include\SMRPlugin.hpp"))
$resourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\resources\vSMR.rc"))
$crashHandlerResourceText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\crash_handler\vSMRCrashHandler.rc"))
$crashHandlerDefinitionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\crash_handler\vSMRCrashHandler.def"))
$ciText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "appveyor.yml"))
$readmeText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "README.md"))
$changelogText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "CHANGELOG.md"))
$packageScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\package_release.ps1"))
$solutionText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR.sln"))
Assert-True ($headerText -match "MY_PLUGIN_VERSION\s+`"v$escapedVersion`"") "Plugin version macro is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedVersion`"") "Windows FileVersion is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Windows ProductVersion is inconsistent."
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
Assert-True ($solutionText -match '(?m)^\s*Release\|Win32\s*=\s*Release\|Win32\s*$') "The solution does not expose Release|Win32."
Assert-True (-not ($solutionText -match '(?m)^\s*Debug\|Win32\s*=\s*Debug\|Win32\s*$')) "The solution must default to its sole Release|Win32 configuration."
Assert-True ($solutionText -match 'vSMRCrashHandler.+vSMR\\crash_handler\\vSMRCrashHandler\.vcxproj') "The WER crash-handler project is missing from the solution."

$legacyThreads = @(
    Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot "vSMR\src"), (Join-Path $RepositoryRoot "vSMR\include") -Recurse -File |
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
Assert-True ($releaseCompile.WarningLevel -eq 'Level4') "Release must compile at warning level 4."
Assert-True ($releaseCompile.ExternalWarningLevel -eq 'TurnOffAllWarnings') "Vendored headers are not isolated at external warning level 0."
Assert-True (-not ([string]$releaseCompile.AdditionalIncludeDirectories -match '(?i)lib[\\/]include')) "Vendored headers must not be compiled as first-party includes."
Assert-True ($releaseCompile.SDLCheck -eq 'true') "Release SDL checks are disabled."
Assert-True ($releaseCompile.BufferSecurityCheck -eq 'true') "Release /GS buffer security checks are disabled."
Assert-True ($releaseLink.GenerateDebugInformation -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace([string]$releaseLink.ProgramDatabaseFile)) "Release private PDB generation is disabled."
$releaseExternalPaths = @($projectXml.SelectNodes("//msb:PropertyGroup/msb:ExternalIncludePath", $namespace) |
    Where-Object { [string]$_.ParentNode.Condition -like '*Release|Win32*' })
Assert-True ($releaseExternalPaths.Count -eq 1 -and
    [string]$releaseExternalPaths[0].InnerText -match '(?i)lib[\\/]include') "The vendored include directory is not configured as external for Release|Win32."
$crashHandlerProjectReference = $projectXml.SelectSingleNode("//msb:ProjectReference[contains(@Include, 'vSMRCrashHandler.vcxproj')]", $namespace)
Assert-True ($null -ne $crashHandlerProjectReference) "vSMR does not build the WER crash-handler dependency."

[xml]$crashHandlerProjectXml = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\crash_handler\vSMRCrashHandler.vcxproj"))
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

$avisoFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $dataDirectory "AVISO") -Filter "*.geojson" -File |
        Where-Object { $_.Name -match '^[A-Za-z0-9]{4}\.geojson$' }
)
Assert-True ($avisoFiles.Count -gt 0) "No bundled AVISO files were found."
foreach ($file in $avisoFiles) {
    $document = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Assert-True ($document.type -eq 'FeatureCollection') "$($file.Name) is not a FeatureCollection."
    Assert-True ([int]$document.metadata.schema_version -eq 2) "$($file.Name) is not AVISO schema 2."
    Assert-True ([string]$document.metadata.airport -eq $file.BaseName.ToUpperInvariant()) "$($file.Name) metadata airport does not match its filename."
    Assert-True ([int]$document.metadata.feature_count -eq @($document.features).Count) "$($file.Name) feature count is stale."
    $ids = @($document.features | ForEach-Object { [string]$_.id })
    Assert-True (@($ids | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) "$($file.Name) contains an empty feature id."
    Assert-True (@($ids | Sort-Object -Unique).Count -eq $ids.Count) "$($file.Name) contains duplicate feature ids."
}

$dynamicAvisoPath = Join-Path $dataDirectory "AVISO\LFPG_Dyna_fixed.geojson"
$dynamicAviso = Get-Content -LiteralPath $dynamicAvisoPath -Raw | ConvertFrom-Json
Assert-True ($dynamicAviso.type -eq 'FeatureCollection') "LFPG_Dyna_fixed.geojson is not a FeatureCollection."
Assert-True ([int]$dynamicAviso.metadata.schema_version -eq 2 -and
    [string]$dynamicAviso.metadata.airport -eq 'LFPG') "LFPG_Dyna_fixed.geojson metadata is not LFPG schema 2."
$dynamicFeatures = @($dynamicAviso.features)
Assert-True ([int]$dynamicAviso.metadata.feature_count -eq $dynamicFeatures.Count) "LFPG_Dyna_fixed.geojson feature count is stale."
$dynamicIds = @($dynamicFeatures | ForEach-Object { [string]$_.id })
Assert-True (@($dynamicIds | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) "LFPG_Dyna_fixed.geojson contains an empty feature id."
Assert-True (@($dynamicIds | Sort-Object -Unique).Count -eq $dynamicIds.Count) "LFPG_Dyna_fixed.geojson contains duplicate feature ids."
$dynamicAreas = @($dynamicFeatures | Where-Object {
    [string]$_.properties.geometry_role -eq 'frequency_ownership_area'
})
$dynamicLabels = @($dynamicFeatures | Where-Object {
    [string]$_.properties.feature_type -eq 'frequency_point'
})
Assert-True ($dynamicAreas.Count -eq 16) "LFPG_Dyna_fixed.geojson must contain 16 frequency ownership areas."
Assert-True ($dynamicLabels.Count -eq 18) "LFPG_Dyna_fixed.geojson must contain 18 dynamic frequency labels."
Assert-True (@($dynamicAreas | Where-Object { [string]$_.properties.service -eq 'DEL' }).Count -eq 0) "LFPG_Dyna_fixed.geojson must not define DEL polygons yet."
Assert-True (@($dynamicFeatures | Where-Object {
    [string]$_.properties.style_id -eq 'label.lfpg.frequencies'
}).Count -eq 0) "LFPG_Dyna_fixed.geojson still contains the superseded static RMP frequency labels."
foreach ($area in $dynamicAreas) {
    $service = [string]$area.properties.service
    $ownerKey = [string]$area.properties.owner_key
    $chain = @($area.properties.takeover_chain)
    Assert-True ($service -in @('RMP', 'GND', 'TWR')) "LFPG_Dyna_fixed.geojson contains unsupported polygon service '$service'."
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$area.properties.frequency)) "LFPG_Dyna_fixed.geojson contains an area without a frequency."
    Assert-True (-not [string]::IsNullOrWhiteSpace($ownerKey) -and $chain.Count -gt 0) "LFPG_Dyna_fixed.geojson contains an area without an ownership chain."
    Assert-True ([string]$chain[0] -eq $ownerKey) "LFPG_Dyna_fixed.geojson area '$($area.id)' does not start its takeover chain with owner_key."
    $sourceId = [string]$area.properties.dynamic_source_id
    Assert-True (-not [string]::IsNullOrWhiteSpace($sourceId)) "LFPG_Dyna_fixed.geojson area '$($area.id)' has no dynamic source id."
    Assert-True (@($dynamicLabels | Where-Object {
        [string]$_.properties.parent_feature_id -eq $sourceId
    }).Count -gt 0) "LFPG_Dyna_fixed.geojson area '$($area.id)' has no positioned frequency label."
}
foreach ($label in $dynamicLabels) {
    $service = [string]$label.properties.service
    $displayFrequency = [string]$label.properties.'text-field'
    if ([string]::IsNullOrWhiteSpace($displayFrequency)) {
        $displayFrequency = [string]$label.properties.display_frequency
    }
    Assert-True ($service -in @('DEL', 'RMP', 'GND', 'TWR')) "LFPG_Dyna_fixed.geojson contains unsupported label service '$service'."
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$label.properties.frequency)) "LFPG_Dyna_fixed.geojson contains a dynamic label without a frequency."
    Assert-True (-not [string]::IsNullOrWhiteSpace($displayFrequency)) "LFPG_Dyna_fixed.geojson contains a dynamic label without text-field or display_frequency."
    if ($service -ne 'DEL') {
        $chain = @($label.properties.takeover_chain)
        Assert-True ($chain.Count -gt 0) "LFPG_Dyna_fixed.geojson contains a non-DEL label without a takeover chain."
    }
}
$rmpLabels = @($dynamicLabels | Where-Object { [string]$_.properties.service -eq 'RMP' })
$expectedRmpFrequencies = @('121.580', '121.640', '121.680', '121.880', '121.930', '131.605')
$actualRmpFrequencies = @($rmpLabels | ForEach-Object {
    $value = [string]$_.properties.'text-field'
    if ([string]::IsNullOrWhiteSpace($value)) { $value = [string]$_.properties.display_frequency }
    $value
} | Sort-Object)
Assert-True ($rmpLabels.Count -eq 6 -and
    ($actualRmpFrequencies -join '|') -ceq ($expectedRmpFrequencies -join '|')) "LFPG_Dyna_fixed.geojson RMP frequency points do not match the six reviewed area frequencies."
$dynamicStyleIds = @($dynamicAviso.styles.PSObject.Properties.Name)
foreach ($feature in $dynamicFeatures) {
    $styleId = [string]$feature.properties.style_id
    Assert-True (-not [string]::IsNullOrWhiteSpace($styleId) -and
        $dynamicStyleIds -contains $styleId) "LFPG_Dyna_fixed.geojson feature '$($feature.id)' references missing style '$styleId'."
}
$baseLfpg = Get-Content -LiteralPath (Join-Path $dataDirectory "AVISO\LFPG.geojson") -Raw | ConvertFrom-Json
$supersededFrequencyLabels = @($baseLfpg.features | Where-Object {
    [string]$_.properties.style_id -eq 'label.lfpg.frequencies'
})
Assert-True ($supersededFrequencyLabels.Count -eq 6) "LFPG.geojson no longer contains the six reference RMP label positions."
foreach ($rmpLabel in $rmpLabels) {
    $frequency = [string]$rmpLabel.properties.'text-field'
    if ([string]::IsNullOrWhiteSpace($frequency)) { $frequency = [string]$rmpLabel.properties.display_frequency }
    $reference = @($supersededFrequencyLabels | Where-Object {
        [string]$_.properties.'text-field' -eq $frequency
    })
    Assert-True ($reference.Count -eq 1) "LFPG_Dyna_fixed.geojson RMP point '$frequency' has no unique reference position."
    Assert-True ([double]$rmpLabel.geometry.coordinates[0] -eq [double]$reference[0].geometry.coordinates[0] -and
        [double]$rmpLabel.geometry.coordinates[1] -eq [double]$reference[0].geometry.coordinates[1]) "LFPG_Dyna_fixed.geojson RMP point '$frequency' is not at its reviewed source coordinate."
}
$baseLfpgIds = @($baseLfpg.features | Where-Object {
    [string]$_.properties.style_id -ne 'label.lfpg.frequencies'
} | ForEach-Object { [string]$_.id } | Sort-Object)
$dynamicExtensionIds = @($dynamicAreas.id) + @($dynamicLabels.id)
$dynamicBaseIds = @($dynamicFeatures | Where-Object {
    $dynamicExtensionIds -notcontains ([string]$_.id)
} | ForEach-Object { [string]$_.id } | Sort-Object)
Assert-True ($dynamicBaseIds.Count -eq $baseLfpgIds.Count) "LFPG_Dyna_fixed.geojson does not contain the expected base LFPG feature set."
for ($index = 0; $index -lt $baseLfpgIds.Count; $index++) {
    Assert-True ($dynamicBaseIds[$index] -ceq $baseLfpgIds[$index]) "LFPG_Dyna_fixed.geojson base LFPG feature identities differ."
}

if (-not [string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    $BuildOutputDirectory = [System.IO.Path]::GetFullPath($BuildOutputDirectory)
    Assert-File (Join-Path $BuildOutputDirectory "vSMR.dll")
    Assert-True (Test-Path -LiteralPath (Join-Path $BuildOutputDirectory "vSMR_Data") -PathType Container) "Built vSMR_Data is missing."
    $releaseRootNames = @(Get-ChildItem -LiteralPath $BuildOutputDirectory -Force | ForEach-Object Name | Sort-Object)
    Assert-True (($releaseRootNames -join '|') -eq 'vSMR.dll|vSMR_Data') "Release root must contain only vSMR.dll and vSMR_Data; found $($releaseRootNames -join ', ')."
    $builtCrashHandler = Join-Path $BuildOutputDirectory "vSMR_Data\CrashReporter\vSMRCrashHandler.dll"
    Assert-File $builtCrashHandler
    Assert-True ((Get-PeMachine (Join-Path $BuildOutputDirectory "vSMR.dll")) -eq 0x014C) "Built vSMR.dll is not Win32/x86."
    Assert-True ((Get-PeMachine $builtCrashHandler) -eq 0x014C) "Built vSMRCrashHandler.dll is not Win32/x86."
    $crashHandlerVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($builtCrashHandler)
    Assert-True ($crashHandlerVersion.FileVersion -eq $ExpectedVersion -and
        $crashHandlerVersion.ProductVersion -eq $ExpectedVersion) "Built crash-handler version does not match $ExpectedVersion."
    $builtCrashHandlerExports = @(Get-PeExportNames $builtCrashHandler | Sort-Object)
    $expectedBuiltExports = @($expectedCrashHandlerExports | Sort-Object)
    Assert-True (($builtCrashHandlerExports -join '|') -ceq ($expectedBuiltExports -join '|')) "Built crash-handler export table is not the exact three-name WER ABI."

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
        $PdbPath = Join-Path $RepositoryRoot "vSMR\Release\vSMR.pdb"
    }
    Assert-File ([System.IO.Path]::GetFullPath($PdbPath))
    if ([string]::IsNullOrWhiteSpace($CrashHandlerPdbPath)) {
        $CrashHandlerPdbPath = Join-Path $RepositoryRoot "vSMR\crash_handler\obj\Release\vSMRCrashHandler.pdb"
    }
    Assert-File ([System.IO.Path]::GetFullPath($CrashHandlerPdbPath))
}

Write-Host "vSMR $ExpectedVersion release validation passed."
