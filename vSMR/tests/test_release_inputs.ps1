#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = "")
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '..\..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$validator = Join-Path $RepositoryRoot 'vSMR/tools/verify_release_inputs.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('vsmr-release-input-tests-' + [guid]::NewGuid().ToString('N'))
$utf8 = New-Object Text.UTF8Encoding($false)
function Write-Fixture([string]$RelativePath, [string]$Text) {
    $path = Join-Path $fixtureRoot $RelativePath
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, $Text, $utf8)
}
function Assert-Rejected([string]$MessagePattern) {
    try { & $validator -RepositoryRoot $fixtureRoot | Out-Null }
    catch {
        if ($_.Exception.Message -like $MessagePattern) { return }
        throw
    }
    throw "Validator accepted invalid fixture; expected: $MessagePattern"
}
try {
    foreach ($relative in @('vSMR/src/plugin/PluginMetadata.hpp', 'vSMR/resources/vSMR.rc',
        'vSMR/src/crash/handler/vSMRCrashHandler.rc', 'vSMR/src/bootstrap/loader/LoaderResources.rc',
        'vSMR/tools/create_release_package.ps1', 'appveyor.yml')) {
        Write-Fixture $relative ([IO.File]::ReadAllText((Join-Path $RepositoryRoot $relative)))
    }
    $release = (Get-Content (Join-Path $RepositoryRoot 'vSMR/data/AVISO-UPDATE-POLICY.json') -Raw | ConvertFrom-Json).release
    Write-Fixture 'vSMR/data/AVISO/AAAA.geojson' '{}'
    $mapHash = (Get-FileHash (Join-Path $fixtureRoot 'vSMR/data/AVISO/AAAA.geojson') -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = @{ schema_version = 1; release = $release; file_count = 1;
        files = @{ 'AAAA.geojson' = $mapHash }; removed_since_previous_import = @('BBBB.geojson') }
    Write-Fixture 'vSMR/tests/fixtures/aviso-set-20260917.json' ($manifest | ConvertTo-Json -Depth 5)
    $policy = @{ schema_version = 1; release = $release; aviso = @{
        update = 'all'; replace = @(); delete = @('BBBB.geojson'); modified_files = 'protect_setting' } }
    $validPolicy = $policy | ConvertTo-Json -Depth 5
    Write-Fixture 'vSMR/data/AVISO-UPDATE-POLICY.json' $validPolicy
    & $validator -RepositoryRoot $fixtureRoot

    Write-Fixture 'vSMR/data/AVISO/AAAA.geojson' '{ }'
    Assert-Rejected 'Bundled AVISO differs*'
    Write-Fixture 'vSMR/data/AVISO/AAAA.geojson' '{}'
    $policy.aviso.delete = @('BBBB.geojson', 'AAAA.geojson')
    Write-Fixture 'vSMR/data/AVISO-UPDATE-POLICY.json' ($policy | ConvertTo-Json -Depth 5)
    Assert-Rejected 'Unsafe or still-bundled*'
    $policy.aviso.delete = @()
    Write-Fixture 'vSMR/data/AVISO-UPDATE-POLICY.json' ($policy | ConvertTo-Json -Depth 5)
    Assert-Rejected 'Removed airport missing*'
    Write-Fixture 'vSMR/data/AVISO-UPDATE-POLICY.json' $validPolicy
    $manifest.file_count = 2
    Write-Fixture 'vSMR/tests/fixtures/aviso-set-20260917.json' ($manifest | ConvertTo-Json -Depth 5)
    Assert-Rejected 'Bundled AVISO file count differs*'
    $manifest.file_count = 1
    Write-Fixture 'vSMR/tests/fixtures/aviso-set-20260917.json' ($manifest | ConvertTo-Json -Depth 5)
    Write-Fixture 'vSMR/src/bootstrap/loader/LoaderResources.rc' 'PRODUCTVERSION 0,0,0,0'
    Assert-Rejected 'Loader product version must contain*'
    Write-Host 'Release-input regression tests passed: valid fixture and five rejection cases.'
} finally {
    # Only remove the unique fixture directory created by this invocation.
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\vsmr-release-input-tests-'
    if (-not $resolvedFixture.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to clean a fixture outside the expected temporary directory.'
    }
    if (Test-Path -LiteralPath $resolvedFixture) { Remove-Item -LiteralPath $resolvedFixture -Recurse -Force }
}
