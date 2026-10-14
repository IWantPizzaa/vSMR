param(
    [ValidateRange(8, 10000)][int]$Frames = 32,
    [string]$OutputDirectory = '',
    [switch]$SkipBuild,
    [switch]$Quick,
    [switch]$Software,
    [switch]$VectorReference,
    [switch]$NativeAnalytic,
    [switch]$QualityOnly,
    [string]$Airports = '',
    [string]$Palettes = '',
    [double]$Zoom = 0
)
$ErrorActionPreference = 'Stop'
$airportNames = @($Airports.Split(',', [StringSplitOptions]::RemoveEmptyEntries))
$paletteNames = @($Palettes.Split(',', [StringSplitOptions]::RemoveEmptyEntries))
if ($airportNames.Where({ $_ -cnotmatch '^[A-Z]{4}$' }).Count -gt 0) { throw 'Airports must be comma-separated ICAO codes.' }
if ($paletteNames.Where({ $_ -cnotin @('dark', 'light', 'real') }).Count -gt 0) { throw 'Palettes must be dark, light or real.' }
if ($Zoom -lt 0 -or [double]::IsInfinity($Zoom) -or [double]::IsNaN($Zoom)) { throw 'Zoom must be a finite positive value.' }
$workspaceDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $workspaceDirectory 'artifacts\aviso-zoom-benchmark' }
if (!$SkipBuild) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuildPath = & $vswherePath -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (!$msbuildPath) { throw 'Visual Studio C++ MSBuild was not found.' }
    & $msbuildPath (Join-Path $PSScriptRoot 'vSMR.AvisoZoomBenchmark.vcxproj') /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Zoom benchmark build failed with exit code $LASTEXITCODE" }
}
$executablePath = Join-Path $PSScriptRoot 'bin\Release\vSMR.AvisoZoomBenchmark.exe'
$benchmarkArguments = @((Join-Path $workspaceDirectory 'vSMR\data\AVISO'), $OutputDirectory, $Frames)
if ($Quick) { $benchmarkArguments += '--quick' }
if ($Software) { $benchmarkArguments += '--software' }
if ($VectorReference) { $benchmarkArguments += '--vector-reference' }
if ($NativeAnalytic) { $benchmarkArguments += '--native-analytic' }
if ($QualityOnly) { $benchmarkArguments += '--quality-only' }
if ($airportNames.Count -gt 0) { $benchmarkArguments += '--airports=' + ($airportNames -join ',') }
if ($paletteNames.Count -gt 0) { $benchmarkArguments += '--palettes=' + ($paletteNames -join ',') }
if ($Zoom -gt 0) { $benchmarkArguments += '--zoom=' + $Zoom.ToString([Globalization.CultureInfo]::InvariantCulture) }
$sourcePaths = @(
    'vSMR/src/radar/RadarScreenTypes.hpp', 'vSMR/BuildHardening.props',
    'vSMR/vSMR.vcxproj', 'vSMR/tests/vSMR.Tests.vcxproj',
    'vSMR/src/aviso/AvisoRasterRenderer.cpp', 'vSMR/src/aviso/AvisoRasterBlitter.cpp',
    'vSMR/src/aviso/AvisoRasterSizing.hpp', 'vSMR/src/aviso/AvisoRenderScene.cpp',
    'vSMR/src/aviso/AvisoProjection.hpp', 'vSMR/src/aviso/AvisoVectorRenderer.cpp',
    'vSMR/src/aviso/AvisoVectorRenderer.hpp', 'vSMR/src/aviso/AvisoGlyphAnalyticRenderer.cpp',
    'vSMR/src/aviso/AvisoGlyphAnalyticRenderer.hpp', 'vSMR/src/aviso/AvisoViewportRenderer.cpp',
    'vSMR/src/aviso/shaders/AvisoGlyph.hlsl', 'vSMR/src/aviso/shaders/AvisoGlyphVertexShader.hpp',
    'vSMR/src/aviso/shaders/AvisoGlyphPixelShader.hpp',
    'vSMR/src/aviso/shaders/compile_aviso_glyph_shaders.ps1',
    'vSMR/src/aviso/AvisoViewportRenderer.hpp', 'vSMR/tests/aviso_benchmark/BenchmarkFixture.cpp',
    'vSMR/tests/aviso_zoom_benchmark/AvisoZoomBenchmark.cpp',
    'vSMR/tests/aviso_zoom_benchmark/vSMR.AvisoZoomBenchmark.vcxproj',
    'vSMR/tests/aviso_zoom_benchmark/run_zoom_benchmark.ps1'
)
$sourceHashes = foreach ($relativePath in $sourcePaths) {
    $digest = Get-FileHash -LiteralPath (Join-Path $workspaceDirectory $relativePath) -Algorithm SHA256
    [ordered]@{ path = $relativePath; sha256 = $digest.Hash }
}
$fixtureHashes = foreach ($airport in $(if ($airportNames.Count -gt 0) { $airportNames } else { @('LFPG', 'LFPO', 'LFML') })) {
    $relativePath = "vSMR/data/AVISO/$airport.geojson"
    $digest = Get-FileHash -LiteralPath (Join-Path $workspaceDirectory $relativePath) -Algorithm SHA256
    [ordered]@{ path = $relativePath; sha256 = $digest.Hash }
}
$runMetadata = [ordered]@{
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    base_commit = (& git -C $workspaceDirectory rev-parse HEAD)
    configuration = 'Release Win32 v145 /O2 /W4 /WX'
    frames = $Frames; quick = [bool]$Quick; quality_only = [bool]$QualityOnly
    airports = @($airportNames); palettes = @($paletteNames); zoom = $Zoom
    backend_requested = $(if ($Software) { 'WARP' } else { 'Hardware' })
    text_mode = $(if ($VectorReference -and !$NativeAnalytic) { 'Vector' } else { 'NativeAnalytic' })
    binary_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    sources = @($sourceHashes); fixtures = @($fixtureHashes)
}
& $executablePath @benchmarkArguments
$benchmarkExitCode = $LASTEXITCODE
$runMetadata.finished_utc = (Get-Date).ToUniversalTime().ToString('o')
$runMetadata.exit_code = $benchmarkExitCode
$runMetadata.processor = @(Get-CimInstance Win32_Processor | Select-Object Name)
$runMetadata.graphics = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
$runMetadata.operating_system = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber
$runMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run_metadata.json') -Encoding UTF8
if ($benchmarkExitCode -ne 0) { throw "Zoom benchmark or stability regression failed with exit code $benchmarkExitCode. Inspect zoom_measurements.csv and saved images." }
