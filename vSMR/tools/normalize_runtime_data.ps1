#requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet("Check", "Write")]
    [string]$Mode = "Check",

    [string]$DataDirectory = ""
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
if ([string]::IsNullOrWhiteSpace($DataDirectory)) {
    $DataDirectory = Join-Path $PSScriptRoot "..\data"
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    return ($text | ConvertFrom-Json)
}

function Test-JsonProperty {
    param($Object, [string]$Name)

    return $null -ne $Object -and
        $null -ne $Object.PSObject -and
        $Object.PSObject.Properties.Name -contains $Name
}

function Get-JsonProperty {
    param($Object, [string]$Name, $Fallback = $null)

    if (Test-JsonProperty $Object $Name) {
        return $Object.$Name
    }
    return $Fallback
}

function ConvertTo-CanonicalValue {
    param($Value)

    if ($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]) {
        return $Value
    }

    if ($Value -is [System.Collections.IDictionary]) {
        $ordered = [ordered]@{}
        foreach ($key in @($Value.Keys | Sort-Object)) {
            $ordered[[string]$key] = ConvertTo-CanonicalValue $Value[$key]
        }
        return [pscustomobject]$ordered
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

function ConvertTo-PrettyJson {
    param($Value)

    $text = ConvertTo-Json -InputObject $Value -Depth 100
    return ($text -replace "`r`n", "`n") + "`n"
}

function ConvertTo-CompactJson {
    param($Value)

    return (ConvertTo-Json -InputObject $Value -Depth 100 -Compress) + "`n"
}

function Set-NormalizedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $existing = if (Test-Path -LiteralPath $Path) {
        [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    }
    else {
        ""
    }

    if ($existing -eq $Content) {
        Write-Host "$Description is canonical."
        return
    }

    if ($Mode -eq "Check") {
        throw "$Description is not canonical. Run this script with -Mode Write."
    }

    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
    Write-Host "Normalized $Description."
}

function Normalize-Profiles {
    param([string]$Path)

    $source = Read-JsonFile $Path
    if (-not ($source -is [System.Array])) {
        throw "vSMR_Profiles.json must contain a JSON array."
    }

    $profiles = New-Object System.Collections.ArrayList
    $metadataEntries = New-Object System.Collections.ArrayList
    $profileNames = @{}

    foreach ($entry in $source) {
        if (-not ($entry -is [pscustomobject])) {
            throw "Every vSMR_Profiles.json entry must be an object."
        }

        $name = [string](Get-JsonProperty $entry "name" "")
        if ([string]::IsNullOrWhiteSpace($name)) {
            if (-not (Test-JsonProperty $entry "_vsmr")) {
                throw "A nameless profile entry must contain _vsmr metadata."
            }
            [void]$metadataEntries.Add((ConvertTo-CanonicalValue $entry))
            continue
        }

        $name = $name.Trim()
        $nameKey = $name.ToUpperInvariant()
        if ($profileNames.ContainsKey($nameKey)) {
            throw "Duplicate profile name '$name'."
        }
        $profileNames[$nameKey] = $true

        $normalized = [ordered]@{
            name = $name
        }

        if (Test-JsonProperty $entry "schema_version") {
            $schemaVersion = [int]$entry.schema_version
            if ($schemaVersion -lt 2) {
                throw "Profile '$name' must use schema_version 2 or newer."
            }
            $normalized["schema_version"] = $schemaVersion
        }

        foreach ($property in @($entry.PSObject.Properties | Sort-Object Name)) {
            if ($property.Name -in @("name", "schema_version", "ui_layout")) {
                continue
            }
            if ($property.Name -eq "maps" -and
                $property.Value -is [pscustomobject] -and
                @($property.Value.PSObject.Properties).Count -eq 0) {
                continue
            }
            $normalized[$property.Name] = ConvertTo-CanonicalValue $property.Value
        }

        [void]$profiles.Add([pscustomobject]$normalized)
    }

    if ($profiles.Count -eq 0) {
        throw "vSMR_Profiles.json contains no usable profiles."
    }
    if ($metadataEntries.Count -gt 1) {
        throw "vSMR_Profiles.json contains more than one metadata entry."
    }

    foreach ($metadata in $metadataEntries) {
        [void]$profiles.Add($metadata)
    }

    $content = ConvertTo-CompactJson $profiles.ToArray()
    Set-NormalizedFile $Path $content "vSMR_Profiles.json ($($profiles.Count - $metadataEntries.Count) profiles)"
}

function Get-FirstNumberProperty {
    param($Object, [string[]]$Names)

    foreach ($name in $Names) {
        if (Test-JsonProperty $Object $name) {
            $value = $Object.$name
            if ($value -is [ValueType]) {
                return [double]$value
            }
        }
    }
    return $null
}

function Get-FirstStringProperty {
    param($Object, [string[]]$Names)

    foreach ($name in $Names) {
        if (Test-JsonProperty $Object $name) {
            $value = [string]$Object.$name
            if (-not [string]::IsNullOrWhiteSpace($value)) {
                return $value.Trim()
            }
        }
    }
    return ""
}

function Normalize-AircraftSpecs {
    param([string]$Path)

    $source = Read-JsonFile $Path
    $records = @{}
    $sourceCount = 0

    if ($source -is [System.Array]) {
        foreach ($entry in $source) {
            $sourceCount++
            $code = Get-FirstStringProperty $entry @("icao_code", "ICAO")
            $length = Get-FirstNumberProperty $entry @("length", "Length")
            $wingspan = Get-FirstNumberProperty $entry @("wingspan", "Wingspan")
            if ([string]::IsNullOrWhiteSpace($code) -or $null -eq $length -or $null -eq $wingspan) {
                continue
            }
            if ($length -le 0.0 -or $wingspan -le 0.0 -or
                [double]::IsNaN($length) -or [double]::IsInfinity($length) -or
                [double]::IsNaN($wingspan) -or [double]::IsInfinity($wingspan)) {
                continue
            }
            $records[$code.ToUpperInvariant()] = [pscustomobject][ordered]@{
                length = $length
                wingspan = $wingspan
            }
        }
    }
    elseif ($source -is [pscustomobject]) {
        foreach ($property in $source.PSObject.Properties) {
            $sourceCount++
            $length = Get-FirstNumberProperty $property.Value @("length", "Length")
            $wingspan = Get-FirstNumberProperty $property.Value @("wingspan", "Wingspan")
            if ($null -eq $length -or $null -eq $wingspan -or
                $length -le 0.0 -or $wingspan -le 0.0) {
                continue
            }
            $records[$property.Name.ToUpperInvariant()] = [pscustomobject][ordered]@{
                length = $length
                wingspan = $wingspan
            }
        }
    }
    else {
        throw "ICAO_Aircraft.json must contain an array or object."
    }

    if ($records.Count -eq 0) {
        throw "ICAO_Aircraft.json contains no usable dimensions."
    }

    $normalized = [ordered]@{}
    foreach ($code in @($records.Keys | Sort-Object)) {
        $normalized[$code] = $records[$code]
    }

    $content = ConvertTo-CompactJson ([pscustomobject]$normalized)
    Set-NormalizedFile $Path $content "ICAO_Aircraft.json ($($records.Count) usable of $sourceCount source records)"
}

function New-LfpgStyleInfo {
    param([string]$Id, [string]$Layer, [string]$Category, [string]$ObjectType, [string]$Prefix)

    return [pscustomobject][ordered]@{
        Id = $Id
        Layer = $Layer
        Category = $Category
        ObjectType = $ObjectType
        Prefix = $Prefix
    }
}

$LfpgRegionStyles = @{
    "COLOR_VoidLFPG" = New-LfpgStyleInfo "background.void" "Background" "Background" "Area" "Background area"
    "COLOR_GrassLFPG" = New-LfpgStyleInfo "terrain.grass" "Terrain" "Grass" "Area" "Grass area"
    "COLOR_BuildingsLFPG" = New-LfpgStyleInfo "structure.building" "Buildings and structures" "Buildings" "Area" "Building"
    "COLOR_TaxiwayContRegLFPG" = New-LfpgStyleInfo "surface.taxiway_containment" "Airfield surfaces" "Taxiway containment areas" "Area" "Taxiway containment area"
    "COLOR_MarkingsRWYLFPG" = New-LfpgStyleInfo "marking.runway" "Surface markings" "Runway markings" "Area" "Runway marking"
    "COLOR_TerminalOtherLFPG" = New-LfpgStyleInfo "structure.terminal_other" "Buildings and structures" "Other terminal structures" "Area" "Other terminal structure"
    "COLOR_JetwaysLFPG" = New-LfpgStyleInfo "structure.jetway" "Buildings and structures" "Jetways" "Area" "Jetway"
    "COLOR_RoadsLFPG" = New-LfpgStyleInfo "infrastructure.road" "Ground infrastructure" "Roads" "Area" "Road"
    "COLOR_TerminalSideLFPG" = New-LfpgStyleInfo "structure.terminal_side" "Buildings and structures" "Terminal sides" "Area" "Terminal side"
    "COLOR_TaxiwayRegionLFPG" = New-LfpgStyleInfo "surface.taxiway" "Airfield surfaces" "Taxiways" "Area" "Taxiway"
    "COLOR_StopPointsLFPG" = New-LfpgStyleInfo "marking.stop_point" "Surface markings" "Stop points" "Area" "Stop point"
    "COLOR_TerminalLFPG" = New-LfpgStyleInfo "structure.terminal" "Buildings and structures" "Terminals" "Area" "Terminal"
    "COLOR_ApronRegLFPG" = New-LfpgStyleInfo "surface.apron" "Airfield surfaces" "Aprons" "Area" "Apron"
    "COLOR_CAT1LFPG" = New-LfpgStyleInfo "marking.cat1" "Surface markings" "CAT I markings" "Area" "CAT I marking"
    "COLOR_DeIcePadRegLFPG" = New-LfpgStyleInfo "surface.deicing_pad" "Airfield surfaces" "De-icing pads" "Area" "De-icing pad"
    "COLOR_CAT3LFPG" = New-LfpgStyleInfo "marking.cat3" "Surface markings" "CAT III markings" "Area" "CAT III marking"
    "COLOR_NoEntryLFPG" = New-LfpgStyleInfo "marking.no_entry" "Surface markings" "No-entry markings" "Area" "No-entry marking"
    "COLOR_ClosedTaxiwaysLFPG" = New-LfpgStyleInfo "surface.closed_taxiway" "Airfield surfaces" "Closed taxiways" "Area" "Closed taxiway"
    "COLOR_PermanentLFPG" = New-LfpgStyleInfo "marking.permanent" "Surface markings" "Permanent markings" "Area" "Permanent marking"
    "COLOR_RunwayRegLFPG" = New-LfpgStyleInfo "surface.runway" "Airfield surfaces" "Runways" "Area" "Runway"
    "COLOR_RailsLFPG" = New-LfpgStyleInfo "infrastructure.railway" "Ground infrastructure" "Railways" "Area" "Railway"
    "COLOR_ApronT1RegLFPG" = New-LfpgStyleInfo "surface.apron_terminal_1" "Airfield surfaces" "Terminal 1 aprons" "Area" "Terminal 1 apron"
    "COLOR_TowerLFPG" = New-LfpgStyleInfo "structure.tower" "Buildings and structures" "Control towers" "Area" "Control tower"
    "COLOR_CenterlineRWYLFPG" = New-LfpgStyleInfo "line.runway_centerline" "Surface markings" "Runway centerlines" "Line" "Runway centerline"
    "COLOR_DeIceLFPG" = New-LfpgStyleInfo "line.deicing" "Surface markings" "De-icing guidance lines" "Line" "De-icing guidance line"
    "COLOR_OutlineTWYLFPG" = New-LfpgStyleInfo "line.taxiway_outline" "Surface markings" "Taxiway outlines" "Line" "Taxiway outline"
    "COLOR_StopBarLightsLFPG" = New-LfpgStyleInfo "line.stop_bar_lights" "Surface markings" "Stop-bar lights" "Line" "Stop-bar light line"
    "COLOR_TaxiArrowsBlueLFPG" = New-LfpgStyleInfo "line.taxiway_guidance.blue" "Surface markings" "Blue taxiway guidance lines" "Line" "Blue taxiway guidance line"
    "COLOR_TaxiArrowsLFPG" = New-LfpgStyleInfo "line.taxiway_guidance" "Surface markings" "Taxiway guidance lines" "Line" "Taxiway guidance line"
    "COLOR_TaxiArrowsOrangeLFPG" = New-LfpgStyleInfo "line.taxiway_guidance.orange" "Surface markings" "Orange taxiway guidance lines" "Line" "Orange taxiway guidance line"
    "COLOR_TaxiInDashLFPG" = New-LfpgStyleInfo "line.taxi_in.dashed" "Surface markings" "Dashed stand-entry lines" "Line" "Dashed stand-entry line"
    "COLOR_TaxiInLFPG" = New-LfpgStyleInfo "line.taxi_in" "Surface markings" "Stand-entry lines" "Line" "Stand-entry line"
    "COLOR_TaxiwayBlueLFPG" = New-LfpgStyleInfo "line.taxiway.blue" "Surface markings" "Blue taxiway markings" "Line" "Blue taxiway marking"
    "COLOR_TaxiwayLFPG" = New-LfpgStyleInfo "line.taxiway" "Surface markings" "Taxiway markings" "Line" "Taxiway marking"
    "COLOR_TaxiwayOrangeLFPG" = New-LfpgStyleInfo "line.taxiway.orange" "Surface markings" "Orange taxiway markings" "Line" "Orange taxiway marking"
}

$LfpgLabelStyles = @{
    "Engine Test Area" = New-LfpgStyleInfo "label.lfpg.engine.test.area" "Labels" "Engine-test-area labels" "Label" ""
    "Gates" = New-LfpgStyleInfo "label.lfpg.gates" "Labels" "Gate and stand labels" "Label" ""
    "Taxiways" = New-LfpgStyleInfo "label.lfpg.taxiways" "Labels" "Taxiway labels" "Label" ""
    "TORA" = New-LfpgStyleInfo "label.lfpg.tora" "Labels" "TORA labels" "Label" ""
    "Terminaux" = New-LfpgStyleInfo "label.lfpg.terminals" "Labels" "Terminal labels" "Label" ""
    "Frequences" = New-LfpgStyleInfo "label.lfpg.frequencies" "Labels" "Frequency labels" "Label" ""
}

$PaintKeys = @(
    "fill", "fill-opacity", "stroke", "stroke-opacity", "stroke-width",
    "text-anchor", "text-color", "text-font", "text-halo-color",
    "text-halo-width", "text-size", "zoomLevel", "zoom_level"
)

function New-NormalizedPaint {
    param($Source)

    $paint = [ordered]@{}
    foreach ($key in $PaintKeys) {
        if (Test-JsonProperty $Source $key) {
            $paint[$key] = $Source.$key
        }
    }
    return [pscustomobject]$paint
}

function New-NormalizedStyle {
    param($SourceStyle, $Info, $FallbackProperties)

    $paintSource = if ($null -ne $SourceStyle -and (Test-JsonProperty $SourceStyle "paint")) {
        $SourceStyle.paint
    }
    else {
        $FallbackProperties
    }

    $name = if ($null -ne $Info) {
        $Info.Category
    }
    else {
        Get-FirstStringProperty $SourceStyle @("name")
    }
    $layer = if ($null -ne $Info) {
        $Info.Layer
    }
    else {
        Get-FirstStringProperty $SourceStyle @("layer")
    }
    $objectType = if ($null -ne $Info) {
        $Info.ObjectType
    }
    else {
        Get-FirstStringProperty $SourceStyle @("object_type")
    }

    return [pscustomobject][ordered]@{
        name = $name
        layer = $layer
        object_type = $objectType
        paint = New-NormalizedPaint $paintSource
    }
}

function ConvertTo-SafeIdPart {
    param([string]$Value)

    $part = $Value.ToLowerInvariant() -replace "[^a-z0-9]+", "."
    return $part.Trim(".")
}

function Test-HiddenAvisoFeature {
    param($Properties)

    if (Test-JsonProperty $Properties "visible") {
        $value = $Properties.visible
        if ($value -is [bool] -and -not $value) {
            return $true
        }
        if ($value -is [string] -and $value.ToUpperInvariant() -in @("FALSE", "0", "NO", "OFF", "HIDDEN", "NONE")) {
            return $true
        }
    }
    if (Test-JsonProperty $Properties "visibility") {
        $value = [string]$Properties.visibility
        if ($value.ToUpperInvariant() -in @("FALSE", "0", "NO", "OFF", "HIDDEN", "NONE")) {
            return $true
        }
    }
    return $false
}

function Get-AvisoGroupIds {
    param($Properties)

    foreach ($key in @("vsmr_group_ids", "vsmr_groups", "group_ids", "group_id", "vsmr_group_id")) {
        if (-not (Test-JsonProperty $Properties $key)) {
            continue
        }
        $value = $Properties.$key
        $ids = New-Object System.Collections.ArrayList
        if ($value -is [System.Array]) {
            foreach ($item in $value) {
                if (-not [string]::IsNullOrWhiteSpace([string]$item) -and -not $ids.Contains([string]$item)) {
                    [void]$ids.Add([string]$item)
                }
            }
        }
        elseif (-not [string]::IsNullOrWhiteSpace([string]$value)) {
            [void]$ids.Add([string]$value)
        }
        return ,$ids.ToArray()
    }
    return ,@()
}

function Get-AvisoAirportFromFileName {
    param([string]$Name)

    if ($Name -match "^AVISO_([A-Za-z0-9]{4})\.geojson$") {
        return $matches[1].ToUpperInvariant()
    }
    throw "AVISO filename '$Name' must use AVISO_ICAO.geojson."
}

function Normalize-AvisoFile {
    param([System.IO.FileInfo]$File)

    $document = Read-JsonFile $File.FullName
    if (-not ($document -is [pscustomobject]) -or
        [string](Get-JsonProperty $document "type" "") -ne "FeatureCollection" -or
        -not (Test-JsonProperty $document "features") -or
        -not ($document.features -is [System.Array])) {
        throw "$($File.Name) must be a GeoJSON FeatureCollection."
    }

    $airport = Get-AvisoAirportFromFileName $File.Name
    $isLfpg = $airport -eq "LFPG"
    $styles = [ordered]@{}

    if ((Test-JsonProperty $document "styles") -and $document.styles -is [pscustomobject]) {
        foreach ($property in $document.styles.PSObject.Properties) {
            $styles[$property.Name] = New-NormalizedStyle $property.Value $null $null
        }
    }

    $normalizedFeatures = New-Object System.Collections.ArrayList
    $usedIds = @{}
    $styleCounts = @{}
    $nameCounters = @{}
    $layerCounts = @{}
    $categoryCounts = @{}

    foreach ($feature in $document.features) {
        if (-not ($feature -is [pscustomobject]) -or
            -not (Test-JsonProperty $feature "geometry") -or
            -not (Test-JsonProperty $feature "properties") -or
            -not ($feature.properties -is [pscustomobject])) {
            throw "$($File.Name) contains an invalid feature object."
        }

        $geometry = $feature.geometry
        $geometryType = Get-FirstStringProperty $geometry @("type")
        if ($geometryType -notin @("Point", "LineString", "MultiLineString", "Polygon", "MultiPolygon")) {
            throw "$($File.Name) contains unsupported geometry '$geometryType'."
        }
        if (-not (Test-JsonProperty $geometry "coordinates")) {
            throw "$($File.Name) contains a geometry without coordinates."
        }

        $sourceProperties = $feature.properties
        $info = $null
        $styleId = Get-FirstStringProperty $sourceProperties @("style_id")

        if ($isLfpg -and $geometryType -eq "Point" -and (Test-JsonProperty $sourceProperties "label_class")) {
            $labelClass = Get-FirstStringProperty $sourceProperties @("label_class")
            if (-not $LfpgLabelStyles.ContainsKey($labelClass)) {
                throw "No LFPG label mapping exists for '$labelClass'."
            }
            $info = $LfpgLabelStyles[$labelClass]
            $styleId = $info.Id
        }
        elseif ($isLfpg -and (Test-JsonProperty $sourceProperties "color_name")) {
            $colorName = [string]$sourceProperties.color_name
            if (-not $LfpgRegionStyles.ContainsKey($colorName)) {
                throw "No LFPG style mapping exists for '$colorName'."
            }
            $info = $LfpgRegionStyles[$colorName]
            $styleId = $info.Id
        }

        if ([string]::IsNullOrWhiteSpace($styleId)) {
            throw "$($File.Name) contains a feature without style_id."
        }

        $existingStyle = if ($styles.Contains($styleId)) { $styles[$styleId] } else { $null }
        if ($null -ne $info) {
            $styles[$styleId] = New-NormalizedStyle $existingStyle $info $sourceProperties
        }
        elseif ($null -eq $existingStyle) {
            throw "$($File.Name) references missing style '$styleId'."
        }

        $style = $styles[$styleId]
        $layer = if ($null -ne $info) { $info.Layer } else { Get-FirstStringProperty $sourceProperties @("layer") }
        if ([string]::IsNullOrWhiteSpace($layer)) {
            $layer = [string]$style.layer
        }
        $category = if ($null -ne $info) { $info.Category } else { Get-FirstStringProperty $sourceProperties @("category") }
        if ([string]::IsNullOrWhiteSpace($category)) {
            $category = [string]$style.name
        }
        $objectType = if ($null -ne $info) { $info.ObjectType } else { Get-FirstStringProperty $sourceProperties @("object_type", "type") }
        if ([string]::IsNullOrWhiteSpace($objectType)) {
            $objectType = if ($geometryType -eq "Point") { "Label" } elseif ($geometryType -like "*LineString") { "Line" } else { "Area" }
        }

        if (-not $nameCounters.ContainsKey($styleId)) {
            $nameCounters[$styleId] = 0
        }
        $nameCounters[$styleId]++

        $name = Get-FirstStringProperty $sourceProperties @("name")
        if ($null -ne $info -and -not [string]::IsNullOrWhiteSpace($info.Prefix)) {
            $name = "{0} {1:D3}" -f $info.Prefix, $nameCounters[$styleId]
        }
        elseif ([string]::IsNullOrWhiteSpace($name)) {
            $name = "{0} {1:D3}" -f $category, $nameCounters[$styleId]
        }

        $geometryRole = Get-FirstStringProperty $sourceProperties @("geometry_role")
        if ([string]::IsNullOrWhiteSpace($geometryRole) -or $geometryRole -in @("aviso_region", "aviso_linework")) {
            $geometryRole = if ($geometryType -eq "Point") { "text_label" } elseif ($geometryType -like "*LineString") { "linework" } else { "filled_region" }
        }

        $properties = [ordered]@{
            name = $name
            layer = $layer
            category = $category
            object_type = $objectType
            style_id = $styleId
            airport = $airport
            geometry_role = $geometryRole
        }

        if ($objectType -eq "Label" -or $geometryType -eq "Point") {
            $text = Get-FirstStringProperty $sourceProperties @("text-field", "text", "label", "title", "name")
            if ([string]::IsNullOrWhiteSpace($text)) {
                throw "$($File.Name) contains a label without text."
            }
            $properties["text-field"] = $text
        }

        if (Test-HiddenAvisoFeature $sourceProperties) {
            $properties["visible"] = $false
        }

        $groupIds = @(Get-AvisoGroupIds $sourceProperties)
        if ($groupIds.Count -gt 0) {
            $properties["vsmr_group_ids"] = $groupIds
        }

        foreach ($paintKey in $PaintKeys) {
            if (-not (Test-JsonProperty $sourceProperties $paintKey)) {
                continue
            }
            if (Test-JsonProperty $style.paint $paintKey) {
                continue
            }
            $properties[$paintKey] = $sourceProperties.$paintKey
        }

        $featureId = Get-FirstStringProperty $feature @("id")
        if ([string]::IsNullOrWhiteSpace($featureId)) {
            $sourceLine = Get-JsonProperty $sourceProperties "source_line_start" $nameCounters[$styleId]
            $featureId = "{0}.{1}.{2}" -f $airport.ToLowerInvariant(), (ConvertTo-SafeIdPart $styleId), $sourceLine
        }
        $baseId = $featureId
        $suffix = 2
        while ($usedIds.ContainsKey($featureId)) {
            $featureId = "$baseId.$suffix"
            $suffix++
        }
        $usedIds[$featureId] = $true

        if (-not $styleCounts.ContainsKey($styleId)) { $styleCounts[$styleId] = 0 }
        if (-not $layerCounts.ContainsKey($layer)) { $layerCounts[$layer] = 0 }
        if (-not $categoryCounts.ContainsKey($category)) { $categoryCounts[$category] = 0 }
        $styleCounts[$styleId]++
        $layerCounts[$layer]++
        $categoryCounts[$category]++

        [void]$normalizedFeatures.Add([pscustomobject][ordered]@{
            type = "Feature"
            id = $featureId
            properties = [pscustomobject]$properties
            geometry = $geometry
        })
    }

    $normalizedStyles = [ordered]@{}
    foreach ($styleId in @($styleCounts.Keys | Sort-Object)) {
        if (-not $styles.Contains($styleId)) {
            throw "$($File.Name) references missing style '$styleId'."
        }
        $style = $styles[$styleId]
        $normalizedStyles[$styleId] = [pscustomobject][ordered]@{
            name = [string]$style.name
            layer = [string]$style.layer
            object_type = [string]$style.object_type
            paint = New-NormalizedPaint $style.paint
            feature_count = $styleCounts[$styleId]
        }
    }

    $normalizedLayerCounts = [ordered]@{}
    foreach ($name in @($layerCounts.Keys | Sort-Object)) { $normalizedLayerCounts[$name] = $layerCounts[$name] }
    $normalizedCategoryCounts = [ordered]@{}
    foreach ($name in @($categoryCounts.Keys | Sort-Object)) { $normalizedCategoryCounts[$name] = $categoryCounts[$name] }

    $metadata = [pscustomobject][ordered]@{
        schema = "vSMR AVISO"
        schema_version = 2
        airport = $airport
        coordinate_reference_system = "WGS84"
        coordinate_order = "longitude, latitude"
        feature_count = $normalizedFeatures.Count
        style_count = $normalizedStyles.Count
        layer_counts = [pscustomobject]$normalizedLayerCounts
        category_counts = [pscustomobject]$normalizedCategoryCounts
    }

    $root = [ordered]@{
        type = "FeatureCollection"
        name = "$airport AVISO"
        bbox = Get-JsonProperty $document "bbox"
        metadata = $metadata
        styles = [pscustomobject]$normalizedStyles
    }

    if ((Test-JsonProperty $document "vsmr_groups") -and
        $document.vsmr_groups -is [System.Array] -and
        $document.vsmr_groups.Count -gt 0) {
        $root["vsmr_groups"] = ConvertTo-CanonicalValue $document.vsmr_groups
    }
    $root["features"] = $normalizedFeatures.ToArray()

    $content = ConvertTo-CompactJson ([pscustomobject]$root)
    Set-NormalizedFile $File.FullName $content "$($File.Name) ($($normalizedFeatures.Count) features, $($normalizedStyles.Count) styles)"
}

$resolvedDataDirectory = [System.IO.Path]::GetFullPath($DataDirectory)
if (-not (Test-Path -LiteralPath $resolvedDataDirectory -PathType Container)) {
    throw "Data directory not found: $resolvedDataDirectory"
}

Normalize-Profiles (Join-Path $resolvedDataDirectory "vSMR_Profiles.json")
Normalize-AircraftSpecs (Join-Path $resolvedDataDirectory "ICAO_Aircraft.json")

$avisoDirectory = Join-Path $resolvedDataDirectory "AVISO"
$avisoFiles = @(Get-ChildItem -File -LiteralPath $avisoDirectory | Where-Object Extension -eq ".geojson" | Sort-Object Name)
if ($avisoFiles.Count -eq 0) {
    throw "No AVISO GeoJSON files were found."
}
foreach ($file in $avisoFiles) {
    Normalize-AvisoFile $file
}

Write-Host "Runtime data $($Mode.ToLowerInvariant()) completed successfully."
