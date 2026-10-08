#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '..\..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$generator = Join-Path $RepositoryRoot 'vSMR/tools/create_update_feed.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('vsmr-feed-tests-' + [guid]::NewGuid().ToString('N'))
$utf8 = [Text.UTF8Encoding]::new($false)
function Write-Fixture([string]$Relative, [string]$Value) {
    $path = Join-Path $fixture $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, $Value, $utf8)
}
function Save-Record { Write-Fixture 'prepared-feed.json' ($script:record | ConvertTo-Json -Depth 15) }
function Generate { & $generator -Phase Manifest -FeedDirectory $fixture -ContentCommit $script:commit -ValidationOnly }
function Assert-Rejected([string]$Pattern) {
    try { Generate | Out-Null } catch {
        if ($_.Exception.Message -like $Pattern) { return }
        throw
    }
    throw "Feed generator accepted invalid fixture: $Pattern"
}
try {
    Write-Fixture 'payload/vSMR_Data/default.json' '{"schema_version":1,"profiles":{"builtin-default":{"name":"Default"}}}'
    Write-Fixture '.gitattributes' 'payload/** -text'
    & git -C $fixture init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize feed fixture repository.' }
    & git -C $fixture add -- payload .gitattributes
    & git -C $fixture -c user.name=FeedTests -c user.email=feed-tests@example.invalid -c commit.gpgsign=false commit --quiet -m 'Fixture payload'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot commit feed fixture repository.' }
    $script:commit = [string](& git -C $fixture rev-parse HEAD)
    $path = Join-Path $fixture 'payload/vSMR_Data/default.json'
    $original = [IO.File]::ReadAllText($path)
    $entry = [ordered]@{ sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant(); size = [long](Get-Item $path).Length }
    $script:record = [ordered]@{ schema = 1; version = '2.0.0-beta.6'; minimum_loader_version = '1.3.0'
        runtime_abi = 1; validation_only = $true; files = [ordered]@{ 'vSMR_Data/default.json' = $entry } }
    Save-Record
    Generate
    Write-Fixture 'payload/vSMR_Data/AVISO/LFPO_Work.geojson' '{"type":"FeatureCollection","features":[]}'
    & git -C $fixture add -- payload
    & git -C $fixture -c user.name=FeedTests -c user.email=feed-tests@example.invalid -c commit.gpgsign=false commit --quiet -m 'Add works AVISO fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot commit works AVISO fixture.' }
    $script:commit = [string](& git -C $fixture rev-parse HEAD)
    $worksPath = Join-Path $fixture 'payload/vSMR_Data/AVISO/LFPO_Work.geojson'
    $script:record.files['vSMR_Data/AVISO/LFPO_Work.geojson'] = [ordered]@{
        sha256 = (Get-FileHash -LiteralPath $worksPath).Hash.ToLowerInvariant()
        size = [long](Get-Item -LiteralPath $worksPath).Length
    }
    Save-Record
    Generate
    $script:record.files.Remove('vSMR_Data/AVISO/LFPO_Work.geojson')
    Remove-Item -LiteralPath $worksPath
    & git -C $fixture add -- payload
    & git -C $fixture -c user.name=FeedTests -c user.email=feed-tests@example.invalid -c commit.gpgsign=false commit --quiet -m 'Restore original payload fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot restore original feed fixture.' }
    $script:commit = [string](& git -C $fixture rev-parse HEAD)
    Save-Record
    Generate
    $resultPath = Join-Path $fixture 'beta/version.validation-only.json'
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($result.content_commit -ne $script:commit -or $result.files.'vSMR_Data/default.json'.sha256 -ne $entry.sha256 -or
        $result.schema -ne 1 -or $result.runtime_abi -ne 1 -or (Test-Path (Join-Path $fixture 'beta/version.json'))) {
        throw 'Incorrect feed contract or validation-only output name.'
    }
    Write-Fixture 'payload/vSMR_Data/default.json' ($original + ' ')
    Assert-Rejected 'Prepared payload was changed*'
    Write-Fixture 'payload/vSMR_Data/default.json' $original
    foreach ($unsafe in @('vSMR_Data/config.json', 'vSMR_Data/CONFIG.JSON',
        'vSMR_Data/UserData/custom.geojson', 'vSMR_Data/../outside.dll',
        'vSMR_Data/vSMR_Profiles.json', 'vSMR_Data/version.json', 'C:/outside.dll')) {
        $script:record.files = [ordered]@{ $unsafe = $entry }
        Save-Record
        Assert-Rejected 'Not an application-managed payload path*'
    }
    $script:record.files = [ordered]@{ 'vSMR_Data/default.json' = $entry }
    Save-Record
    # Even re-hashed local edits must not be published under an earlier commit.
    Write-Fixture 'payload/vSMR_Data/default.json' ($original + ' ')
    $entry.sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    $entry.size = [long](Get-Item $path).Length
    Save-Record
    Assert-Rejected 'Committed bytes differ*'
    try {
        & $generator -Phase Manifest -FeedDirectory $fixture -ContentCommit $script:commit -Channel stable -ValidationOnly
        throw 'Accepted a beta version on the stable channel.'
    } catch { if ($_.Exception.Message -notlike 'Prereleases cannot be promoted*') { throw } }
    try {
        & $generator -Phase Manifest -FeedDirectory $fixture -ContentCommit $script:commit
        throw 'Promoted a validation payload to a publishable manifest.'
    } catch { if ($_.Exception.Message -notlike 'A validation-only payload cannot*') { throw } }
    # Stable product versions must select the stable channel automatically,
    # while keeping local validation artifacts impossible to publish by name.
    Write-Fixture 'payload/vSMR_Data/default.json' $original
    $entry.sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    $entry.size = [long](Get-Item $path).Length
    $script:record.version = '2.0.0'
    Save-Record
    Generate
    $stablePath = Join-Path $fixture 'stable/version.validation-only.json'
    $stable = Get-Content -LiteralPath $stablePath -Raw | ConvertFrom-Json
    if ($stable.version -ne '2.0.0' -or $stable.content_commit -ne $script:commit -or
        $stable.files.'vSMR_Data/default.json'.sha256 -ne $entry.sha256 -or
        (Test-Path (Join-Path $fixture 'stable/version.json'))) {
        throw 'Incorrect stable-channel contract or validation-only isolation.'
    }
    Write-Fixture 'invalid-build/vSMR.dll' 'This is not a compiled DLL.'
    try {
        & $generator -Phase Prepare -RepositoryRoot $RepositoryRoot -BuildOutputDirectory (Join-Path $fixture 'invalid-build') `
            -FeedDirectory (Join-Path $fixture 'invalid-feed') -ValidationOnly -SkipBuild
        throw 'Accepted an uncompiled payload.'
    } catch { if ($_.Exception.Message -notlike 'Not a compiled PE binary*') { throw } }
    if (Test-Path -LiteralPath (Join-Path $fixture 'invalid-feed/payload')) {
        throw 'Invalid binaries left a publishable-looking payload tree.'
    }
    Write-Host 'Raw update feed tests passed: immutable Git bytes, hashes, ownership, channels and validation-only isolation.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\vsmr-feed-tests-'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe feed fixture cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
