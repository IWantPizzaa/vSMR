param(
    [ValidateRange(1, 10000)][int]$Samples = 40,
    [string]$OutputDirectory = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$workspaceDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $workspaceDirectory 'artifacts\aviso-benchmark' }
if (!$SkipBuild) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuildPath = & $vswherePath -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (!$msbuildPath) { throw 'Visual Studio C++ MSBuild was not found.' }
    & $msbuildPath (Join-Path $PSScriptRoot 'vSMR.AvisoBenchmark.vcxproj') /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Benchmark build failed with exit code $LASTEXITCODE" }
}
$executablePath = Join-Path $PSScriptRoot 'bin\Release\vSMR.AvisoBenchmark.exe'
& $executablePath (Join-Path $workspaceDirectory 'vSMR\data\AVISO') $OutputDirectory $Samples
if ($LASTEXITCODE -ne 0) { throw "Benchmark or image regression failed with exit code $LASTEXITCODE. Inspect measurements.csv and saved image pairs." }
