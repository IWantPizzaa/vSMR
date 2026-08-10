#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [ValidatePattern("^\d+\.\d+\.\d+-beta\.\d+$")]
    [string]$ExpectedVersion = "2.0.0-beta.1",
    [string]$BuildOutputDirectory = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$dataDirectory = Join-Path $RepositoryRoot "vSMR\data"
$normalizer = Join-Path $RepositoryRoot "vSMR\tools\normalize_runtime_data.ps1"
$fixtureDirectory = Join-Path $RepositoryRoot "vSMR\tests\fixtures"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-File {
    param([string]$Path)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "Required file is missing: $Path"
}

function Assert-ExpectedFailure {
    param([scriptblock]$Action, [string]$Description)
    $failed = $false
    try {
        & $Action
    }
    catch {
        $failed = $true
        Write-Host "Expected rejection: $Description"
    }
    if (-not $failed) {
        throw "Expected failure did not occur: $Description"
    }
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
    ".github\workflows\release-validation.yml",
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
$githubWorkflowText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot ".github\workflows\release-validation.yml"))
$readmeText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "README.md"))
$changelogText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "CHANGELOG.md"))
$packageScriptText = [System.IO.File]::ReadAllText((Join-Path $RepositoryRoot "vSMR\tools\package_release.ps1"))
Assert-True ($headerText -match "MY_PLUGIN_VERSION\s+`"v$escapedVersion`"") "Plugin version macro is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"FileVersion`",\s+`"$escapedVersion`"") "Windows FileVersion is inconsistent."
Assert-True ($resourceText -match "VALUE\s+`"ProductVersion`",\s+`"$escapedVersion`"") "Windows ProductVersion is inconsistent."
Assert-True ($resourceText -match "(?s)#ifdef\s+_DEBUG\s+FILEFLAGS\s+0x3L\s+#else\s+FILEFLAGS\s+0x2L") "Windows beta resource is missing the prerelease flag."
Assert-True ($ciText -match "(?m)^version:\s+$escapedVersion\.\{build\}\s*$") "AppVeyor version is inconsistent."
Assert-True (-not ($githubWorkflowText -match '(?m)^\s*uses:\s*[^#\r\n]+@(?![0-9a-fA-F]{40}(?:\s|#|$))')) "GitHub Actions dependencies must be pinned to full commit SHAs."
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
Assert-True ($releaseCompile.SDLCheck -eq 'true') "Release SDL checks are disabled."
Assert-True ($releaseCompile.BufferSecurityCheck -eq 'true') "Release /GS buffer security checks are disabled."
Assert-True ($releaseLink.GenerateDebugInformation -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace([string]$releaseLink.ProgramDatabaseFile)) "Release private PDB generation is disabled."

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

$avisoFiles = @(Get-ChildItem -LiteralPath (Join-Path $dataDirectory "AVISO") -Filter "AVISO_*.geojson" -File)
Assert-True ($avisoFiles.Count -gt 0) "No bundled AVISO files were found."
foreach ($file in $avisoFiles) {
    $document = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Assert-True ($document.type -eq 'FeatureCollection') "$($file.Name) is not a FeatureCollection."
    Assert-True ([int]$document.metadata.schema_version -eq 2) "$($file.Name) is not AVISO schema 2."
    Assert-True ([int]$document.metadata.feature_count -eq @($document.features).Count) "$($file.Name) feature count is stale."
    $ids = @($document.features | ForEach-Object { [string]$_.id })
    Assert-True (@($ids | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) "$($file.Name) contains an empty feature id."
    Assert-True (@($ids | Sort-Object -Unique).Count -eq $ids.Count) "$($file.Name) contains duplicate feature ids."
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-data-tests-" + [Guid]::NewGuid().ToString("N"))
try {
    $fixtureData = Join-Path $temporaryRoot "data"
    $fixtureAviso = Join-Path $fixtureData "AVISO"
    [System.IO.Directory]::CreateDirectory($fixtureAviso) | Out-Null
    Copy-Item -LiteralPath (Join-Path $dataDirectory "vSMR_Profiles.json") -Destination $fixtureData
    Copy-Item -LiteralPath (Join-Path $dataDirectory "ICAO_Aircraft.json") -Destination $fixtureData
    $smallestAviso = $avisoFiles | Sort-Object Length | Select-Object -First 1
    $fixtureAvisoPath = Join-Path $fixtureAviso $smallestAviso.Name
    Copy-Item -LiteralPath $smallestAviso.FullName -Destination $fixtureAvisoPath

    $emptyGroupsDocument = Get-Content -LiteralPath $fixtureAvisoPath -Raw | ConvertFrom-Json
    $emptyGroupsDocument | Add-Member -MemberType NoteProperty -Name vsmr_groups -Value @() -Force
    $fixtureJson = (ConvertTo-Json $emptyGroupsDocument -Depth 100 -Compress) + "`n"
    [System.IO.File]::WriteAllText($fixtureAvisoPath, $fixtureJson, (New-Object System.Text.UTF8Encoding($false)))
    & $normalizer -Mode Write -DataDirectory $fixtureData
    $normalizedFixture = Get-Content -LiteralPath $fixtureAvisoPath -Raw | ConvertFrom-Json
    Assert-True ($normalizedFixture.PSObject.Properties.Name -contains 'vsmr_groups') "Normalizer removed an explicit empty vsmr_groups array."
    Assert-True (@($normalizedFixture.vsmr_groups).Count -eq 0) "Normalizer changed an explicit empty vsmr_groups array."

    $futureProfiles = Get-Content -LiteralPath (Join-Path $fixtureData "vSMR_Profiles.json") -Raw | ConvertFrom-Json
    ($futureProfiles | Where-Object { $_.PSObject.Properties.Name -contains 'name' } | Select-Object -First 1).schema_version = 99
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureData "vSMR_Profiles.json"),
        ((ConvertTo-Json $futureProfiles -Depth 100 -Compress) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Assert-ExpectedFailure { & $normalizer -Mode Check -DataDirectory $fixtureData } "future profile schema"

    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureData "vSMR_Profiles.json"),
        "{malformed",
        (New-Object System.Text.UTF8Encoding($false)))
    Assert-ExpectedFailure { & $normalizer -Mode Check -DataDirectory $fixtureData } "malformed profile JSON"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
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

    $pdbPath = Join-Path $RepositoryRoot "vSMR\Release\vSMR.pdb"
    Assert-File $pdbPath
    $coreTests = Join-Path $RepositoryRoot "vSMR\tests\bin\Release\vSMR.CoreTests.exe"
    Assert-File $coreTests
    & $coreTests $fixtureDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Native core tests failed with exit code $LASTEXITCODE."
    }
}

Write-Host "vSMR $ExpectedVersion release tests passed."
