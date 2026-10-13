#requires -Version 5.1

param(
    [string]$FxcPath,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
if (-not $FxcPath) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $sdk = Get-ChildItem -LiteralPath $sdkRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.' } |
        Sort-Object { [version]$_.Name } -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x86\fxc.exe') } |
        Select-Object -First 1
    if (-not $sdk) { throw 'Install a Windows SDK containing fxc.exe, or pass -FxcPath.' }
    $FxcPath = Join-Path $sdk.FullName 'x86\fxc.exe'
}

$source = Join-Path $PSScriptRoot 'AvisoGlyph.hlsl'
foreach ($stage in @(
    @{ Entry = 'VS'; Profile = 'vs_4_0'; Variable = 'AvisoGlyphVertexBytecode'; File = 'AvisoGlyphVertexShader.hpp' },
    @{ Entry = 'PS'; Profile = 'ps_4_0'; Variable = 'AvisoGlyphPixelBytecode'; File = 'AvisoGlyphPixelShader.hpp' }
)) {
    $destination = Join-Path $PSScriptRoot $stage.File
    $temporary = [System.IO.Path]::GetTempFileName()
    try {
        & $FxcPath /nologo /O3 /T $stage.Profile /E $stage.Entry /Qstrip_reflect /Qstrip_debug /Qstrip_priv /Vn $stage.Variable /Fh $temporary $source
        if ($LASTEXITCODE -ne 0) { throw "$($stage.Entry) shader compilation failed." }
        $header = [System.IO.File]::ReadAllText($temporary)
        $offset = $header.IndexOf("const BYTE $($stage.Variable)")
        if ($offset -lt 0) { throw 'Shader compiler did not produce the expected bytecode array.' }
        $header = "// Generated from AvisoGlyph.hlsl by compile_aviso_glyph_shaders.ps1. Do not edit.`r`n#pragma once`r`n`r`n" +
            $header.Substring($offset).Replace("`r`n", "`n").Replace("`n", "`r`n")
        $header = [System.Text.RegularExpressions.Regex]::Replace($header, '[\t ]+(?=\r?$)', '',
            [System.Text.RegularExpressions.RegexOptions]::Multiline)
        if ($Check) {
            if (-not (Test-Path -LiteralPath $destination) -or [System.IO.File]::ReadAllText($destination) -cne $header) {
                throw "$($stage.File) differs from the compiled HLSL. Regenerate the shader headers."
            }
        } else {
            [System.IO.File]::WriteAllText($destination, $header, [System.Text.UTF8Encoding]::new($false))
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force
    }
}
