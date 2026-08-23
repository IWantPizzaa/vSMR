#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DaySource,

    [string]$DataDirectory = ""
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
if ([string]::IsNullOrWhiteSpace($DataDirectory)) {
    $DataDirectory = Join-Path $PSScriptRoot "..\data\AVISO"
}

function Test-JsonProperty {
    param($Object, [string]$Name)

    return $null -ne $Object -and
        $null -ne $Object.PSObject -and
        $Object.PSObject.Properties.Name -contains $Name
}

function ConvertTo-CanonicalValue {
    param($Value)

    if ($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]) {
        return $Value
    }
    if ($Value -is [pscustomobject]) {
        $ordered = [ordered]@{}
        foreach ($property in @($Value.PSObject.Properties | Sort-Object Name)) {
            $ordered[$property.Name] = ConvertTo-CanonicalValue $property.Value
        }
        return [pscustomobject]$ordered
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $items = New-Object System.Collections.ArrayList
        foreach ($item in $Value) {
            [void]$items.Add((ConvertTo-CanonicalValue $item))
        }
        return ,$items.ToArray()
    }
    return $Value
}

function ConvertTo-ComparableStyle {
    param($Style)

    $paint = [ordered]@{}
    foreach ($property in @($Style.paint.PSObject.Properties | Sort-Object Name)) {
        if ($property.Name -in $script:ColorKeys -or $property.Name -eq "palette-overrides") {
            continue
        }
        $paint[$property.Name] = ConvertTo-CanonicalValue $property.Value
    }

    return [pscustomobject][ordered]@{
        name = [string]$Style.name
        layer = [string]$Style.layer
        object_type = [string]$Style.object_type
        feature_count = [int]$Style.feature_count
        paint = [pscustomobject]$paint
    }
}

function Set-JsonProperty {
    param($Object, [string]$Name, $Value)

    if (Test-JsonProperty $Object $Name) {
        $Object.$Name = $Value
    }
    else {
        $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value
    }
}

function Format-JsonTwoSpace {
    param([Parameter(Mandatory = $true)][string]$Json)

    $builder = New-Object System.Text.StringBuilder
    $indent = 0
    $inString = $false
    $escaped = $false
    for ($index = 0; $index -lt $Json.Length; $index++) {
        $character = $Json[$index]
        if ($inString) {
            [void]$builder.Append($character)
            if ($escaped) {
                $escaped = $false
            }
            elseif ($character -eq '\') {
                $escaped = $true
            }
            elseif ($character -eq '"') {
                $inString = $false
            }
            continue
        }

        if ($character -eq '"') {
            $inString = $true
            [void]$builder.Append($character)
            continue
        }
        if ([char]::IsWhiteSpace($character)) {
            continue
        }

        switch ($character) {
            { $_ -eq '{' -or $_ -eq '[' } {
                [void]$builder.Append($character)
                $matchingClose = if ($character -eq '{') { '}' } else { ']' }
                if ($index + 1 -lt $Json.Length -and $Json[$index + 1] -ne $matchingClose) {
                    $indent++
                    [void]$builder.Append("`n")
                    [void]$builder.Append(' ', $indent * 2)
                }
                break
            }
            { $_ -eq '}' -or $_ -eq ']' } {
                $matchingOpen = if ($character -eq '}') { '{' } else { '[' }
                if ($index -gt 0 -and $Json[$index - 1] -ne $matchingOpen) {
                    $indent--
                    [void]$builder.Append("`n")
                    [void]$builder.Append(' ', $indent * 2)
                }
                [void]$builder.Append($character)
                break
            }
            ',' {
                [void]$builder.Append(",`n")
                [void]$builder.Append(' ', $indent * 2)
                break
            }
            ':' {
                [void]$builder.Append(": ")
                break
            }
            default {
                [void]$builder.Append($character)
                break
            }
        }
    }
    return $builder.ToString() + "`n"
}

$resolvedDataDirectory = [System.IO.Path]::GetFullPath($DataDirectory)
if (-not (Test-Path -LiteralPath $resolvedDataDirectory -PathType Container)) {
    throw "AVISO data directory not found: $resolvedDataDirectory"
}

$resolvedDaySource = [System.IO.Path]::GetFullPath($DaySource)
if (-not (Test-Path -LiteralPath $resolvedDaySource)) {
    throw "Day AVISO source not found: $resolvedDaySource"
}

$temporaryDirectory = ""
try {
    if ([System.IO.Path]::GetExtension($resolvedDaySource) -ieq ".zip") {
        $temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("vsmr-aviso-day-" + [guid]::NewGuid().ToString("N"))
        [void](New-Item -ItemType Directory -Path $temporaryDirectory)
        Expand-Archive -LiteralPath $resolvedDaySource -DestinationPath $temporaryDirectory
        $dayDirectory = $temporaryDirectory
    }
    elseif (Test-Path -LiteralPath $resolvedDaySource -PathType Container) {
        $dayDirectory = $resolvedDaySource
    }
    else {
        throw "DaySource must be a zip archive or directory."
    }

    $dayFilesByName = @{}
    foreach ($file in @(Get-ChildItem -LiteralPath $dayDirectory -Filter "*.geojson" -File -Recurse)) {
        if ($dayFilesByName.ContainsKey($file.Name)) {
            throw "The day source contains duplicate '$($file.Name)' files."
        }
        $dayFilesByName[$file.Name] = $file.FullName
    }

    $currentFiles = @(
        Get-ChildItem -LiteralPath $resolvedDataDirectory -Filter "*.geojson" -File |
            Where-Object {
                $_.Name -match "^[A-Za-z0-9]{4}\.geojson$" -or
                $_.Name -eq "LFPG_Dyna.geojson"
            } |
            Sort-Object Name
    )
    if ($currentFiles.Count -eq 0) {
        throw "No canonical AVISO files were found in $resolvedDataDirectory."
    }

    $script:ColorKeys = @("fill", "stroke", "marker-color", "text-color", "text-halo-color")
    # The supplied LFML day export accidentally contains the night colors.
    # LFML uses the same shared surface palette as LFBO/LFLL, so keep the
    # correction explicit and style-based (a raw color-only substitution would
    # be ambiguous for #152841, which has different meanings by style).
    $knownDayPaletteCorrections = @{
        "LFML.geojson" = @{
            "structure.building"     = "#394446"
            "surface.hard_surface_2" = "#595E5B"
            "surface.hard_surface_3" = "#8A807F"
            "surface.hard_surface_4" = "#969393"
            "terrain.grass"          = "#00512F"
        }
    }
    $changedStyleCount = 0
    $changedColorCount = 0

    foreach ($currentFile in $currentFiles) {
        if (-not $dayFilesByName.ContainsKey($currentFile.Name)) {
            throw "The day source is missing '$($currentFile.Name)'."
        }

        $currentText = [System.IO.File]::ReadAllText($currentFile.FullName, [System.Text.Encoding]::UTF8)
        $wasPrettyPrinted = $currentFile.Name -eq "LFPG_Dyna.geojson" -or
            $currentText -match "^\s*\{\s*`r?`n"
        $current = $currentText | ConvertFrom-Json
        $dayText = [System.IO.File]::ReadAllText($dayFilesByName[$currentFile.Name], [System.Text.Encoding]::UTF8)
        $day = $dayText | ConvertFrom-Json
        if ([string]$current.type -ne "FeatureCollection" -or [string]$day.type -ne "FeatureCollection") {
            throw "$($currentFile.Name) is not a GeoJSON FeatureCollection in both sources."
        }

        $currentFeatures = ConvertTo-Json -InputObject (ConvertTo-CanonicalValue $current.features) -Depth 100 -Compress
        $dayFeatures = ConvertTo-Json -InputObject (ConvertTo-CanonicalValue $day.features) -Depth 100 -Compress
        if ($currentFeatures -cne $dayFeatures) {
            throw "$($currentFile.Name) differs in features or geometry; refusing a color-only merge."
        }

        $currentStyleIds = @($current.styles.PSObject.Properties.Name | Sort-Object)
        $dayStyleIds = @($day.styles.PSObject.Properties.Name | Sort-Object)
        if (($currentStyleIds -join "`n") -cne ($dayStyleIds -join "`n")) {
            throw "$($currentFile.Name) has different style catalogs; refusing a color-only merge."
        }

        foreach ($styleId in $currentStyleIds) {
            $currentStyle = $current.styles.$styleId
            $dayStyle = $day.styles.$styleId
            $currentComparable = ConvertTo-Json -InputObject (ConvertTo-ComparableStyle $currentStyle) -Depth 20 -Compress
            $dayComparable = ConvertTo-Json -InputObject (ConvertTo-ComparableStyle $dayStyle) -Depth 20 -Compress
            if ($currentComparable -cne $dayComparable) {
                throw "$($currentFile.Name) style '$styleId' differs outside its colors."
            }

            $dayColors = [ordered]@{}
            foreach ($colorKey in $script:ColorKeys) {
                $currentHasColor = Test-JsonProperty $currentStyle.paint $colorKey
                $dayHasColor = Test-JsonProperty $dayStyle.paint $colorKey
                if ($currentHasColor -ne $dayHasColor) {
                    throw "$($currentFile.Name) style '$styleId' has mismatched '$colorKey' fields."
                }
                if ($currentHasColor -and [string]$currentStyle.paint.$colorKey -cne [string]$dayStyle.paint.$colorKey) {
                    $dayColors[$colorKey] = [string]$dayStyle.paint.$colorKey
                    $changedColorCount++
                }
            }

            if ($knownDayPaletteCorrections.ContainsKey($currentFile.Name) -and
                $knownDayPaletteCorrections[$currentFile.Name].ContainsKey($styleId)) {
                $correctedColor = $knownDayPaletteCorrections[$currentFile.Name][$styleId]
                foreach ($colorKey in @("fill", "stroke")) {
                    if (-not (Test-JsonProperty $currentStyle.paint $colorKey)) {
                        throw "$($currentFile.Name) style '$styleId' is missing '$colorKey' required by its Day palette correction."
                    }
                    if (-not $dayColors.Contains($colorKey)) {
                        $changedColorCount++
                    }
                    $dayColors[$colorKey] = $correctedColor
                }
            }

            $palettes = [ordered]@{}
            if (Test-JsonProperty $currentStyle.paint "palette-overrides") {
                foreach ($property in @($currentStyle.paint."palette-overrides".PSObject.Properties | Sort-Object Name)) {
                    if ($property.Name -ne "day") {
                        $palettes[$property.Name] = ConvertTo-CanonicalValue $property.Value
                    }
                }
            }
            if ($dayColors.Count -gt 0) {
                $palettes["day"] = [pscustomobject]$dayColors
                $changedStyleCount++
            }

            if ($palettes.Count -gt 0) {
                Set-JsonProperty $currentStyle.paint "palette-overrides" ([pscustomobject]$palettes)
            }
            elseif (Test-JsonProperty $currentStyle.paint "palette-overrides") {
                $currentStyle.paint.PSObject.Properties.Remove("palette-overrides")
            }
        }

        if (-not (Test-JsonProperty $current "metadata") -or -not ($current.metadata -is [pscustomobject])) {
            throw "$($currentFile.Name) has no metadata object."
        }
        Set-JsonProperty $current.metadata "default_color_palette" "night"
        Set-JsonProperty $current.metadata "color_palettes" ([object[]]@("night", "day"))

        $compactJson = ConvertTo-Json -InputObject $current -Depth 100 -Compress
        if ($wasPrettyPrinted) {
            # Windows PowerShell escapes apostrophes even though JSON does not
            # require it. Preserve the human-readable dynamic source format.
            $compactJson = $compactJson.Replace('\u0027', "'")
        }
        $json = if ($wasPrettyPrinted) { Format-JsonTwoSpace $compactJson } else { $compactJson + "`n" }
        [System.IO.File]::WriteAllText($currentFile.FullName, $json, $Utf8NoBom)
    }

    Write-Host "Merged the day palette into $($currentFiles.Count) AVISO files: $changedStyleCount styles, $changedColorCount color fields."
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($temporaryDirectory) -and
        (Test-Path -LiteralPath $temporaryDirectory -PathType Container)) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
