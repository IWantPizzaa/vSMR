#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '../..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not ('Vsmr.Updates.JsonThreeWayMerge' -as [type])) {
    Add-Type -Path (Join-Path $RepositoryRoot 'vSMR/data/Tools/JsonThreeWayMerge.cs')
}
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "Merge test failed: $Message" }
    ++$script:checks
}
function Merge([string]$Base, [string]$Local, [string]$Remote, [bool]$Profiles = $false) {
    return [Vsmr.Updates.JsonThreeWayMerge]::MergeJson($Base, $Local, $Remote, $Profiles)
}
$result = Merge '{"color":"old","size":1}' '{"color":"mine","size":1}' '{"color":"old","size":2}'
$json = $result.Json | ConvertFrom-Json
Check ($json.color -eq 'mine' -and $json.size -eq 2 -and $result.Conflicts.Length -eq 0) 'independent field changes'
$result = Merge '{"x":1,"y":1}' '{"x":2}' '{"x":3,"y":2}'
$json = $result.Json | ConvertFrom-Json
Check ($json.x -eq 2 -and -not ($json.PSObject.Properties.Name -contains 'y') -and $result.Conflicts.Length -eq 2) 'conflicts retain user values and deletions'
$result = Merge '{"x":null,"y":false}' '{"x":null,"y":true}' '{"x":[],"y":false}'
Check ($result.Json -eq '{"x":[],"y":true}') 'null, boolean and empty array remain distinct'
$result = Merge '{"X":1,"x":1}' '{"X":2,"x":1}' '{"X":1,"x":3}'
Check ($result.Json -eq '{"X":2,"x":3}') 'case-sensitive object keys'
$result = Merge '{"color":1,"coordinates":[2.12345678901234567,48.00000000000000001]}' '{"color":2,"coordinates":[2.12345678901234567,48.00000000000000001]}' '{"color":1,"coordinates":[3.12345678901234567,49.00000000000000001]}'
Check ($result.Json.Contains('[3.12345678901234567,49.00000000000000001]')) 'full incoming coordinate precision'
$result = Merge '{"features":[{"id":"a","geometry":{"type":"Point","coordinates":[1,2]}}]}' '{"features":[{"id":"a","geometry":{"type":"Point","coordinates":[3,4]}}]}' '{"features":[{"id":"a","geometry":{"type":"LineString","coordinates":[[1,2],[2,3]]}}]}'
Check (($result.Json | ConvertFrom-Json).features[0].geometry.type -eq 'Point' -and $result.Conflicts.Length -eq 1) 'geometry changes are atomic'
$result = Merge '[{"name":"Default","color":1,"size":1},{"_vsmr":{"schema_version":1}}]' '[{"name":"Default","color":2,"size":1},{"name":"Custom","color":4},{"_vsmr":{"schema_version":1}}]' '[{"name":"Default","color":1,"size":3},{"name":"New","color":5},{"_vsmr":{"schema_version":1}}]' $true
$profiles = $result.Json | ConvertFrom-Json
Check ($profiles.Count -eq 4 -and $profiles[0].color -eq 2 -and $profiles[0].size -eq 3 -and @($profiles | Where-Object { $_.name -eq 'Custom' }).Count -eq 1) 'profile names, additions and metadata identity'
$result = Merge '{"features":[{"id":"a","x":1},{"id":"b","x":1}]}' '{"features":[{"id":"b","x":2}]}' '{"features":[{"id":"b","x":1},{"id":"a","x":1},{"id":"c","x":3}]}'
$features = ($result.Json | ConvertFrom-Json).features
Check ($features.Count -eq 2 -and $features[0].id -eq 'b' -and $features[0].x -eq 2 -and $features[1].id -eq 'c') 'IDs, reordering, user deletion and upstream addition'
$result = Merge '{"features":[{"id":"a","x":1}]}' '{"features":[{"id":"a","x":2}]}' '{"features":[]}'
Check (($result.Json | ConvertFrom-Json).features.Count -eq 1 -and $result.Conflicts.Length -eq 1) 'upstream deletion retains edited feature'
$result = Merge '{"a":[1,2]}' '{"a":[1,3]}' '{"a":[2,2]}'
Check ($result.Json -eq '{"a":[1,3]}' -and $result.Conflicts.Length -eq 1) 'ordered arrays never merge by index'
$result = Merge '[{"name":"Default","schema_version":1,"x":1}]' '[{"name":"Default","schema_version":1,"x":2}]' '[{"name":"Default","schema_version":2,"x":{"new":1}}]' $true
Check (($result.Json | ConvertFrom-Json)[0].schema_version -eq 1 -and $result.Conflicts.Length -eq 1) 'schema changes preserve edited profile as a whole'
$result = Merge '{"features":[{"id":"a"}]}' '{"features":[{"id":"a","x":2}]}' '{"features":[{"id":"a"},{"id":"a"}]}'
Check ($result.Conflicts.Length -eq 1 -and ($result.Json | ConvertFrom-Json).features.Count -eq 1) 'ambiguous duplicate IDs retain user array'
foreach ($bad in @('{"x":1,"x":2}', '{"x":01}', '{"x":NaN}', '{"x":"\ud800"}', '{"x":1} trailing', ('{"x":' + ('[' * 65) + '0' + (']' * 65) + '}'))) {
    $rejected = $false
    try { $null = Merge '{}' '{}' $bad } catch { $rejected = $true }
    Check $rejected 'malformed or excessive-depth input rejected'
}
$result = Merge '{"styles":{"old":{}},"features":[{"id":"a","properties":{"style_id":"old"},"x":1}]}' '{"styles":{"old":{}},"features":[{"id":"a","properties":{"style_id":"old"},"x":2}]}' '{"styles":{"new":{}},"features":[]}'
Check (($result.Json | ConvertFrom-Json).styles.PSObject.Properties.Name -contains 'old') 'upstream style deletion cannot leave a retained feature with a dangling style'

# Exercise the real installer, checksum validation, baseline lifecycle and backups
# using isolated non-executable payloads, never an actual EuroScope installation.
$bundledMaps = @(Get-ChildItem (Join-Path $RepositoryRoot 'vSMR/data/AVISO') -Filter '*.geojson' -File)
foreach ($mapFile in $bundledMaps) {
    $source = [IO.File]::ReadAllText($mapFile.FullName).Trim()
    $result = Merge $source $source $source
    Check ($result.Json -ceq $source) ('exact token round-trip: ' + $mapFile.Name)
    $local = '{"__merge_test_local":true,' + $source.Substring(1)
    $remote = '{"__merge_test_remote":true,' + $source.Substring(1)
    $result = Merge $source $local $remote
    Check ($result.Json.Contains('"__merge_test_local":true') -and $result.Json.Contains('"__merge_test_remote":true') -and $result.Conflicts.Length -eq 0) ('independent changes in real map: ' + $mapFile.Name)
}
$source = [IO.File]::ReadAllText((Join-Path $RepositoryRoot 'vSMR/data/vSMR_Profiles.json')).Trim()
$result = Merge $source $source $source $true
Check ($result.Json -ceq $source) 'real bundled profiles round-trip without normalization'

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('vsmr-merge-tests-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $fixture 'package'
$packageData = Join-Path $package 'vSMR_Data'
$destination = Join-Path $fixture 'installed'
$installedData = Join-Path $destination 'vSMR_Data'
$encoding = New-Object Text.UTF8Encoding($false)
function Write-TestFile([string]$Path, [string]$Text) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}
function Update-TestPackage([string]$Map, [string]$Profiles, [bool]$Signed = $false) {
    Write-TestFile (Join-Path $packageData 'AVISO/AAAA.geojson') $Map
    Write-TestFile (Join-Path $packageData 'vSMR_Profiles.json') $Profiles
    $mapHash = (Get-FileHash (Join-Path $packageData 'AVISO/AAAA.geojson')).Hash.ToLowerInvariant()
    $runtime = Join-Path $packageData 'Runtime/vSMR.Runtime.dll'
    $loader = Join-Path $package 'vSMR.dll'
    $metadata = @{ schema_version = 1; product = 'vSMR'; version = '2.0.0-beta.6'; publishable = $true; git_commit = 'fixture';
        loader = @{ version = '1.2.0'; relative_path = 'vSMR.dll'; size = (Get-Item $loader).Length; sha256 = (Get-FileHash $loader).Hash.ToLowerInvariant() };
        runtime = @{ relative_path = 'vSMR_Data/Runtime/vSMR.Runtime.dll'; size = (Get-Item $runtime).Length; sha256 = (Get-FileHash $runtime).Hash.ToLowerInvariant() };
        automatic_update = @{ publishable = $true; signature_required = $Signed; minimum_loader_version = '1.2.0' } }
    Write-TestFile (Join-Path $packageData 'RELEASE-METADATA.json') ($metadata | ConvertTo-Json -Depth 8)
    Write-TestFile (Join-Path $packageData 'AVISO-INVENTORY.json') (@{ schema_version = 1; release = '2.0.0-beta.6'; files = @{ 'AAAA.geojson' = $mapHash } } | ConvertTo-Json -Depth 5)
    Write-TestFile (Join-Path $packageData 'AVISO-UPDATE-POLICY.json') (@{ schema_version = 1; release = '2.0.0-beta.6'; aviso = @{ update = 'all'; replace = @(); delete = @(); modified_files = 'protect_setting' } } | ConvertTo-Json -Depth 5)
    $sums = @(Get-ChildItem $package -File -Recurse | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | ForEach-Object {
        (Get-FileHash $_.FullName).Hash.ToLowerInvariant() + '  ' + $_.FullName.Substring($package.Length + 1).Replace('\','/')
    })
    Write-TestFile (Join-Path $packageData 'SHA256SUMS.txt') ($sums -join "`n")
}
function Install-TestPackage { & (Join-Path $packageData 'Tools/install_vsmr.ps1') -DestinationDirectory $destination }
try {
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $packageData 'Tools')) | Out-Null
    foreach ($name in @('install_vsmr.ps1', 'restore_vsmr_backup.ps1', 'merge_user_data.ps1', 'JsonThreeWayMerge.cs')) {
        Copy-Item (Join-Path $RepositoryRoot ('vSMR/data/Tools/' + $name)) (Join-Path $packageData ('Tools/' + $name))
    }
    foreach ($name in @('vSMR.dll', 'vSMR_Data/Runtime/vSMR.Runtime.dll', 'vSMR_Data/CrashReporter/vSMRCrashHandler.dll', 'vSMR_Data/airports_hp.json')) {
        Write-TestFile (Join-Path $package $name) '{}'
    }
    # Header-only test fixtures satisfy architecture checks but contain no executable code.
    $header = New-Object byte[] 96
    $header[0] = 0x4d; $header[1] = 0x5a; $header[60] = 64
    $header[64] = 0x50; $header[65] = 0x45; $header[68] = 0x4c; $header[69] = 1; $header[87] = 0x20
    foreach ($name in @('vSMR.dll', 'vSMR_Data/Runtime/vSMR.Runtime.dll', 'vSMR_Data/CrashReporter/vSMRCrashHandler.dll')) {
        [IO.File]::WriteAllBytes((Join-Path $package $name), $header)
    }
    Update-TestPackage '{"color":1,"size":1}' '[{"name":"Default","color":1,"size":1}]'
    & (Join-Path $packageData 'Tools/install_vsmr.ps1') -DestinationDirectory $destination -WhatIf
    Check (-not (Test-Path (Join-Path $destination 'vSMR_Backups'))) 'WhatIf has no installation side effects'
    Install-TestPackage
    Check (Test-Path (Join-Path $installedData 'UpdateBaselines/AVISO/AAAA.geojson')) 'fresh install seeds full baseline'
    Write-TestFile (Join-Path $installedData 'AVISO/AAAA.geojson') '{"color":2,"size":1}'
    Write-TestFile (Join-Path $installedData 'vSMR_Profiles.json') '[{"name":"Default","color":2,"size":1},{"name":"Custom","size":9}]'
    Write-TestFile (Join-Path $installedData 'AVISO/BBBB.geojson') '{"custom":true}'
    Update-TestPackage '{"color":1,"size":3}' '[{"name":"Default","color":1,"size":3}]'
    Install-TestPackage
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    $profiles = Get-Content (Join-Path $installedData 'vSMR_Profiles.json') -Raw | ConvertFrom-Json
    Check ($map.color -eq 2 -and $map.size -eq 3) 'installer merges AVISO defaults and user edits'
    Check ($profiles[0].color -eq 2 -and $profiles[0].size -eq 3 -and $profiles[1].name -eq 'Custom') 'installer merges profiles and retains custom profiles'
    Check (Test-Path (Join-Path $installedData 'AVISO/BBBB.geojson')) 'custom airport retained'
    Update-TestPackage '{"color":4,"size":5}' '[{"name":"Default","color":4,"size":5}]'
    Install-TestPackage
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    Check ($map.color -eq 2 -and $map.size -eq 5) 'second update uses upstream baseline, not previous merged result'
    $restoreBackup = (Get-Content (Join-Path $installedData 'INSTALLATION.json') -Raw | ConvertFrom-Json).rollback_backup
    $report = Get-Content (Join-Path $installedData 'DATA-UPDATE-REPORT.json') -Raw | ConvertFrom-Json
    Check (@($report.files | Where-Object { $_.status -eq 'merged_with_conflicts' }).Count -eq 2) 'conflicts reported for both maps and profiles'
    Check (Test-Path (Join-Path $installedData 'Data_Updates/2.0.0-beta.6/AVISO/AAAA.geojson')) 'incoming conflict copy saved'
    $backups = @(Get-ChildItem (Join-Path $destination 'vSMR_Backups') -Directory)
    Check ($backups.Count -eq 3) 'complete backup before each install'
    # A missing full baseline must preserve the file, even if upstream differs.
    Write-TestFile (Join-Path $installedData 'UpdateBaselines/AVISO/AAAA.geojson') 'corrupt baseline'
    Update-TestPackage '{"color":7,"size":7}' '[{"name":"Default","color":7,"size":7}]'
    Install-TestPackage
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    Check ($map.color -eq 2 -and $map.size -eq 5) 'corrupt baseline never overwrites local data'
    Remove-Item -LiteralPath (Join-Path $installedData 'AVISO/AAAA.geojson') -Force
    Install-TestPackage
    Check (-not (Test-Path (Join-Path $installedData 'AVISO/AAAA.geojson'))) 'user-deleted map is not resurrected'
    # Simulate an older installation with no saved defaults at all.
    Remove-Item -LiteralPath (Join-Path $installedData 'UpdateBaselines/BASELINE-HASHES.json') -Force
    Write-TestFile (Join-Path $installedData 'AVISO/AAAA.geojson') '{"color":42,"size":42}'
    Write-TestFile (Join-Path $installedData 'vSMR_Profiles.json') '[{"name":"Default","color":42,"size":42}]'
    Install-TestPackage
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    $profiles = Get-Content (Join-Path $installedData 'vSMR_Profiles.json') -Raw | ConvertFrom-Json
    Check ($map.color -eq 42 -and $profiles[0].color -eq 42) 'first upgrade preserves unknown historical edits in both file types'
    $report = Get-Content (Join-Path $installedData 'DATA-UPDATE-REPORT.json') -Raw | ConvertFrom-Json
    Check (@($report.files | Where-Object { $_.status -eq 'preserved_no_baseline' }).Count -eq 2) 'first-upgrade preservation is reported'
    # Explicit replacement remains available to the user; default merge is not forced over it.
    & (Join-Path $packageData 'Tools/install_vsmr.ps1') -DestinationDirectory $destination -ReplaceModifiedAviso
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    Check ($map.color -eq 7) 'explicit AVISO replacement still applies incoming defaults'
    # Signed mode remains fail-closed; unsigned does not imply bypassing it.
    Update-TestPackage '{"color":7,"size":7}' '[{"name":"Default"}]' $true
    $rejected = $false
    try { Install-TestPackage } catch { $rejected = $_.Exception.Message -like '*invalid signer certificate pin*' }
    Check $rejected 'explicit signing requirement cannot be bypassed'
    Update-TestPackage '{"color":7,"size":7}' '[{"name":"Default"}]'
    Write-TestFile (Join-Path $package 'vSMR.dll') 'tampered'
    $rejected = $false
    try { Install-TestPackage } catch { $rejected = $_.Exception.Message -like '*Package hash mismatch*' }
    Check $rejected 'unsigned package still rejects tampered bytes'
    & (Join-Path $packageData 'Tools/restore_vsmr_backup.ps1') -DestinationDirectory $destination -BackupDirectory $restoreBackup
    $map = Get-Content (Join-Path $installedData 'AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    $baseline = Get-Content (Join-Path $installedData 'UpdateBaselines/AVISO/AAAA.geojson') -Raw | ConvertFrom-Json
    Check ($map.color -eq 2 -and $map.size -eq 3 -and $baseline.color -eq 1 -and $baseline.size -eq 3) 'rollback restores edited data and its matching baseline'
    Write-Host "User-data merge and installer tests passed: $script:checks checks."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\vsmr-merge-tests-'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
