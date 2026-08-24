#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$SourceDirectory = [System.IO.Path]::GetFullPath($SourceDirectory)
if (-not (Test-Path -LiteralPath $SourceDirectory -PathType Container)) {
    throw "vSID configuration directory not found: $SourceDirectory"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $PSScriptRoot "..\data\airports_hp.json"
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)

# vSID source: https://github.com/rmaure06/vsid-configurations
# JavaScriptSerializer accepts valid JSON property names that PowerShell 5.1's
# ConvertFrom-Json rejects, including the empty SID key present in lfkf.json.
Add-Type -AssemblyName System.Web.Extensions
$serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
$serializer.MaxJsonLength = [int]::MaxValue
$catalog = [ordered]@{}
$warnings = New-Object System.Collections.Generic.List[string]
$runwayCount = 0
$pointCount = 0

$sourceFiles = @(
    Get-ChildItem -LiteralPath $SourceDirectory -Filter "*.json" -File |
        Sort-Object Name
)
if ($sourceFiles.Count -eq 0) {
    throw "No vSID JSON files were found in $SourceDirectory."
}

foreach ($file in $sourceFiles) {
    $document = $serializer.DeserializeObject(
        [System.IO.File]::ReadAllText($file.FullName))
    if ($document -isnot [System.Collections.IDictionary] -or $document.Count -ne 1) {
        throw "$($file.Name) must contain exactly one airport object."
    }

    $airport = [string]@($document.Keys)[0]
    $airport = $airport.Trim().ToUpperInvariant()
    if ($airport -notmatch '^[A-Z0-9]{4}$' -or
        $airport -ne [System.IO.Path]::GetFileNameWithoutExtension($file.Name).ToUpperInvariant()) {
        throw "$($file.Name) has invalid or mismatched airport key '$airport'."
    }

    $configuration = $document[$airport]
    if ($configuration -isnot [System.Collections.IDictionary] -or
        -not $configuration.ContainsKey('intersections') -or
        $configuration['intersections'] -isnot [System.Collections.IDictionary]) {
        throw "$($file.Name) has no valid intersections object."
    }

    $runways = [ordered]@{}
    $intersections = $configuration['intersections']
    foreach ($sourceRunway in @($intersections.Keys | Sort-Object)) {
        $runway = ([string]$sourceRunway).Trim().ToUpperInvariant()
        if ($runway -notmatch '^[0-9]{2}[LRC]?$') {
            throw "$($file.Name) contains invalid runway '$sourceRunway'."
        }
        $rawPoints = $intersections[$sourceRunway]
        if ($rawPoints -isnot [string]) {
            throw "$($file.Name) runway $runway must contain a comma-separated string."
        }

        $points = New-Object System.Collections.Generic.List[string]
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
        foreach ($rawPoint in @($rawPoints -split ',')) {
            $point = ([string]$rawPoint).Trim().ToUpperInvariant()
            if ([string]::IsNullOrWhiteSpace($point)) {
                continue
            }
            if ($point -notmatch '^[A-Z0-9/-]{1,8}$') {
                throw "$($file.Name) runway $runway contains invalid holding point '$point'."
            }
            if ($seen.Add($point)) {
                $points.Add($point)
            }
            else {
                $warnings.Add("$airport/${runway}: ignored duplicate holding point $point")
            }
        }
        if ($points.Count -eq 0) {
            throw "$($file.Name) runway $runway contains no holding points."
        }

        $runways[$runway] = [string[]]$points.ToArray()
        $runwayCount++
        $pointCount += $points.Count
    }
    if ($runways.Count -eq 0) {
        throw "$($file.Name) contains no runway holding-point mappings."
    }
    $catalog[$airport] = $runways
}

$json = New-Object System.Text.StringBuilder
[void]$json.AppendLine('{')
$airportNames = @($catalog.Keys)
for ($airportIndex = 0; $airportIndex -lt $airportNames.Count; $airportIndex++) {
    $airport = [string]$airportNames[$airportIndex]
    [void]$json.AppendLine("  `"$airport`": {")
    $runwayNames = @($catalog[$airport].Keys)
    for ($runwayIndex = 0; $runwayIndex -lt $runwayNames.Count; $runwayIndex++) {
        $runway = [string]$runwayNames[$runwayIndex]
        $quotedPoints = @($catalog[$airport][$runway] | ForEach-Object { "`"$_`"" })
        $suffix = if ($runwayIndex -lt $runwayNames.Count - 1) { ',' } else { '' }
        [void]$json.AppendLine("    `"$runway`": [$($quotedPoints -join ', ')]$suffix")
    }
    $suffix = if ($airportIndex -lt $airportNames.Count - 1) { ',' } else { '' }
    [void]$json.AppendLine("  }$suffix")
}
[void]$json.Append('}')
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutputPath)) | Out-Null
[System.IO.File]::WriteAllText(
    $OutputPath,
    $json.ToString() + [Environment]::NewLine,
    (New-Object System.Text.UTF8Encoding($false)))

foreach ($warning in $warnings) {
    Write-Warning $warning
}
Write-Host "Imported $($catalog.Count) airports, $runwayCount runways, and $pointCount holding points into $OutputPath."
