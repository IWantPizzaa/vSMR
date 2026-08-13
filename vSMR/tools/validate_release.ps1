#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^\d+\.\d+\.\d+-beta\.\d+$")]
    [string]$ExpectedVersion = "2.0.0-beta.2",
    [string]$BuildOutputDirectory = "",
    [string]$PdbPath = ""
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

foreach ($relativePath in @(
    "vSMR\include\SMRPlugin.hpp",
    "vSMR\resources\vSMR.rc",
    "vSMR\vSMR.vcxproj",
    "vSMR\data\vSMR_Profiles.json",
    "vSMR\data\ICAO_Aircraft.json",
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
$ciText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "appveyor.yml"))
$readmeText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "README.md"))
$changelogText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "CHANGELOG.md"))
$packageScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\package_release.ps1"))
Assert-True ($headerText -match "MY_PLUGIN_VERSION\s+`"v$escapedVersion`"") "Plugin version macro is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedVersion`"") "Windows FileVersion is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Windows ProductVersion is inconsistent."
Assert-True ($resourceText -match "(?s)#ifdef\s+_DEBUG\s+FILEFLAGS\s+0x3L\s+#else\s+FILEFLAGS\s+0x2L") "Windows beta resource is missing the prerelease flag."
Assert-True ($ciText -match "(?m)^version:\s+$escapedVersion\.\{build\}\s*$") "AppVeyor version is inconsistent."
Assert-True ($readmeText.Contains($ExpectedVersion)) "README does not identify the beta version."
Assert-True ($changelogText -match "\[$escapedVersion\]") "CHANGELOG has no beta release section."
Assert-True ($packageScriptText -match '_vsmr-package-.+NewGuid') "Release packaging must use a private GUID staging directory."
Assert-True (-not ($packageScriptText -match 'Join-Path\s+\$ArtifactsDirectory\s+"_staging"')) "Release packaging must not delete a caller-owned fixed _staging directory."

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

if (-not [string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    $BuildOutputDirectory = [System.IO.Path]::GetFullPath($BuildOutputDirectory)
    Assert-File (Join-Path $BuildOutputDirectory "vSMR.dll")
    Assert-True (Test-Path -LiteralPath (Join-Path $BuildOutputDirectory "vSMR_Data") -PathType Container) "Built vSMR_Data is missing."
    $releaseRootNames = @(Get-ChildItem -LiteralPath $BuildOutputDirectory -Force | ForEach-Object Name | Sort-Object)
    Assert-True (($releaseRootNames -join '|') -eq 'vSMR.dll|vSMR_Data') "Release root must contain only vSMR.dll and vSMR_Data; found $($releaseRootNames -join ', ')."

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
}

Write-Host "vSMR $ExpectedVersion release validation passed."
