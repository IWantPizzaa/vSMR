#requires -Version 5.1
[CmdletBinding()]
param([string]$RepositoryRoot = "")

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Join-Path $PSScriptRoot '..\..' }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$runtimePath = Join-Path $RepositoryRoot 'vSMR\bin\Release\Runtime\vSMR.Runtime.dll'
if (-not ('VsmrAudioResourceTest' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VsmrAudioResourceTest {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, string type);
    [DllImport("kernel32.dll")]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
}
# Read resources only: never run DLL initialization or play audible test sounds.
$module = [VsmrAudioResourceTest]::LoadLibraryExW($runtimePath, [IntPtr]::Zero, 0x22)
if ($module -eq [IntPtr]::Zero) { throw "Cannot load runtime resources: $runtimePath" }
try {
    $ids = [IO.File]::ReadAllText((Join-Path $RepositoryRoot 'vSMR\src\platform\windows\ResourceIds.h'))
    foreach ($sound in @(
        @{ Id = 'IDR_TIMER_ALARM_WAVE'; File = 'Alarm.wav' },
        @{ Id = 'IDR_CPDLC_DING_WAVE'; File = 'Ding.wav' }
    )) {
        if ($ids -notmatch ('#define\s+' + $sound.Id + '\s+(\d+)')) { throw "Missing resource ID: $($sound.Id)" }
        $resource = [VsmrAudioResourceTest]::FindResourceW($module, [IntPtr][int]$Matches[1], 'WAVE')
        if ($resource -eq [IntPtr]::Zero) { throw "Missing built-in sound: $($sound.File)" }
        $size = [VsmrAudioResourceTest]::SizeofResource($module, $resource)
        $data = [VsmrAudioResourceTest]::LockResource([VsmrAudioResourceTest]::LoadResource($module, $resource))
        if ($size -lt 44 -or $data -eq [IntPtr]::Zero) { throw "Invalid WAVE resource: $($sound.File)" }
        $bytes = [byte[]]::new($size)
        [Runtime.InteropServices.Marshal]::Copy($data, $bytes, 0, $bytes.Length)
        if ([Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'RIFF' -or
            [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne 'WAVE') { throw 'Invalid WAVE signature.' }
        $hash = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-', '') }
        finally { $hash.Dispose() }
        $expected = (Get-FileHash -LiteralPath (Join-Path $RepositoryRoot ('vSMR\data\Audio\' + $sound.File)) -Algorithm SHA256).Hash
        if ($actual -ne $expected) { throw "Built-in $($sound.File) differs from the bundled sound." }
    }
} finally {
    [void][VsmrAudioResourceTest]::FreeLibrary($module)
}
Write-Host 'Runtime audio resources verified: timer and CPDLC fallbacks match bundled WAV files.'
