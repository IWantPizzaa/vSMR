#requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet("Check", "Write")]
    [string]$Mode = "Check",

    [string]$DataDirectory = ""
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$AvisoFileNamePattern = '^(?<airport>[A-Za-z0-9]{4})(?:_[A-Za-z0-9][A-Za-z0-9_-]{0,47})?\.geojson$'
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
            if (-not (Test-JsonProperty $entry "_vsmr") -or
                -not ($entry._vsmr -is [pscustomobject])) {
                throw "A nameless profile entry must contain _vsmr metadata."
            }
            if (-not (Test-JsonProperty $entry._vsmr "schema_version") -or
                [int]$entry._vsmr.schema_version -ne 1) {
                throw "_vsmr metadata must use supported schema_version 1."
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
            if ($schemaVersion -ne 2) {
                throw "Profile '$name' must use supported schema_version 2."
            }
            $normalized["schema_version"] = $schemaVersion
        }
        else {
            throw "Profile '$name' is missing schema_version 2."
        }

        foreach ($requiredObject in @("labels", "targets")) {
            if (-not (Test-JsonProperty $entry $requiredObject) -or
                -not ($entry.$requiredObject -is [pscustomobject])) {
                throw "Profile '$name' requires an object named '$requiredObject'."
            }
        }
        foreach ($knownObject in @("approach_insets", "filters", "font", "maps", "rimcas", "rules")) {
            if ((Test-JsonProperty $entry $knownObject) -and
                -not ($entry.$knownObject -is [pscustomobject])) {
                throw "Profile '$name' member '$knownObject' must be an object."
            }
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
            if ($property.Name -eq "rimcas" -and $property.Value -is [pscustomobject]) {
                $rimcas = ConvertTo-CanonicalValue $property.Value
                foreach ($obsoleteOption in @("enabled", "rimcas_label_only", "use_red_symbol_for_emergencies")) {
                    $rimcas.PSObject.Properties.Remove($obsoleteOption)
                }
                $normalized[$property.Name] = $rimcas
                continue
            }
            if ($property.Name -eq "approach_insets" -and $property.Value -is [pscustomobject]) {
                $approachInsets = ConvertTo-CanonicalValue $property.Value
                $approachInsets.PSObject.Properties.Remove("background_color")
                $normalized[$property.Name] = $approachInsets
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
    "marker-color", "marker-size",
    "text-anchor", "text-color", "text-font", "text-halo-color",
    "text-halo-width", "text-size", "zoomLevel", "zoom_level",
    "palette-overrides"
)

function New-NormalizedPaint {
    param($Source)

    $paint = [ordered]@{}
    foreach ($key in $PaintKeys) {
        if (Test-JsonProperty $Source $key) {
            $paint[$key] = if ($key -eq "palette-overrides") {
                ConvertTo-CanonicalValue $Source.$key
            }
            else {
                $Source.$key
            }
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

    # object_type is a vSMR semantic type, not the raw GeoJSON geometry type.
    # Some imported schema-2 files incorrectly use Polygon/Point here. The
    # raster renderer can still paint their geometry, but labels are skipped
    # and the Control Center requests the wrong color key for its swatches.
    $objectType = [string]$objectType
    switch ($objectType.ToUpperInvariant()) {
        { $_ -in @("POLYGON", "MULTIPOLYGON", "AREA") } { $objectType = "Area"; break }
        { $_ -in @("LINESTRING", "MULTILINESTRING", "LINE") } { $objectType = "Line"; break }
        "LABEL" { $objectType = "Label"; break }
        "POINT" {
            if ((Test-JsonProperty $paintSource "text-color") -or
                (Test-JsonProperty $paintSource "text-size") -or
                (Test-JsonProperty $paintSource "text-font")) {
                $objectType = "Label"
            }
            else {
                $objectType = "Point"
            }
            break
        }
    }

    return [pscustomobject][ordered]@{
        name = $name
        layer = $layer
        object_type = $objectType
        paint = New-NormalizedPaint $paintSource
    }
}

function Get-AvisoLabelZoomLevel {
    param(
        [string]$StyleId,
        [System.Collections.IEnumerable]$Features
    )

    $normalizedStyleId = $StyleId.ToLowerInvariant()

    # Keep the operational hierarchy stable across airports. Wide-area
    # annotations must remain available while viewing their associated
    # airspace; progressively denser surface detail appears closer in.
    if ($normalizedStyleId -match "(^|\.)(amsr|tma)(\.|$)") { return 0 }
    if ($normalizedStyleId -match "vfr[._-]?points?") { return 1 }
    if ($normalizedStyleId -match "circuit") { return 6 }
    if ($normalizedStyleId -match "active[._-]?configuration") { return 6 }
    if ($normalizedStyleId -match "frequenc|terminal") { return 7 }
    if ($normalizedStyleId -match "tora|engine[._-]?test") { return 9 }
    if ($normalizedStyleId -match "taxiway") { return 9 }
    if ($normalizedStyleId -match "gate|stand") { return 12 }

    $labelFeatures = @(
        $Features | Where-Object {
            $_.geometry.type -eq "Point" -and
            [string]$_.properties.style_id -eq $StyleId
        }
    )
    if ($labelFeatures.Count -le 1) {
        return 5
    }

    # Imported airport files often use one generic style for a mixture of
    # stands, taxiways and building names. Derive a conservative threshold
    # from the tenth-percentile nearest-neighbour spacing, adjusted for label
    # length. This reflects local crowding instead of the airport's total
    # label count or geographic extent (both are frequently misleading).
    $nearestDistancesMetres = New-Object System.Collections.ArrayList
    foreach ($feature in $labelFeatures) {
        $coordinates = $feature.geometry.coordinates
        $longitude = [double]$coordinates[0]
        $latitude = [double]$coordinates[1]
        $nearest = [double]::PositiveInfinity

        foreach ($other in $labelFeatures) {
            if ([object]::ReferenceEquals($feature, $other)) {
                continue
            }
            $otherCoordinates = $other.geometry.coordinates
            $otherLongitude = [double]$otherCoordinates[0]
            $otherLatitude = [double]$otherCoordinates[1]
            $averageLatitudeRadians = (($latitude + $otherLatitude) * 0.5) * [Math]::PI / 180.0
            $dx = ($longitude - $otherLongitude) * 111000.0 * [Math]::Cos($averageLatitudeRadians)
            $dy = ($latitude - $otherLatitude) * 111000.0
            $distance = [Math]::Sqrt(($dx * $dx) + ($dy * $dy))
            if ($distance -lt $nearest) {
                $nearest = $distance
            }
        }

        if (-not [double]::IsInfinity($nearest)) {
            [void]$nearestDistancesMetres.Add($nearest)
        }
    }

    if ($nearestDistancesMetres.Count -eq 0) {
        return 5
    }

    $sortedDistances = @($nearestDistancesMetres.ToArray() | Sort-Object)
    $percentileIndex = [Math]::Floor(($sortedDistances.Count - 1) * 0.10)
    $spacingMetres = [double]$sortedDistances[$percentileIndex]
    $averageCharacters = [double](
        $labelFeatures |
            ForEach-Object { ([string]$_.properties.'text-field').Length } |
            Measure-Object -Average
    ).Average
    $spacingPerCharacter = $spacingMetres / [Math]::Max($averageCharacters, 2.0)

    if ($spacingPerCharacter -le 6.0) { return 12 }
    if ($spacingPerCharacter -le 8.0) { return 11 }
    if ($spacingPerCharacter -le 11.0) { return 10 }
    if ($spacingPerCharacter -le 16.0) { return 9 }
    if ($spacingPerCharacter -le 24.0) { return 8 }
    if ($spacingPerCharacter -le 36.0) { return 7 }
    if ($spacingPerCharacter -le 55.0) { return 6 }
    if ($spacingPerCharacter -le 85.0) { return 5 }
    if ($spacingPerCharacter -le 130.0) { return 4 }
    if ($spacingPerCharacter -le 200.0) { return 3 }
    if ($spacingPerCharacter -le 350.0) { return 2 }
    if ($spacingPerCharacter -le 600.0) { return 1 }
    return 0
}

function ConvertTo-SafeIdPart {
    param([string]$Value)

    $part = $Value.ToLowerInvariant() -replace "[^a-z0-9]+", "."
    return $part.Trim(".")
}

function ConvertTo-AvisoDisplayName {
    param([string]$Value)

    $name = $Value -replace "^COLOR_", ""
    $name = $name -creplace "([a-z0-9])([A-Z])", '$1 $2'
    $name = $name -replace "[_-]+", " "
    $name = ($name -replace "\s+", " ").Trim()
    if ([string]::IsNullOrWhiteSpace($name)) {
        return "AVISO objects"
    }
    return $name
}

function New-DerivedAvisoStyleInfo {
    param($Properties, [string]$GeometryType)

    $colorName = Get-FirstStringProperty $Properties @("color_name")
    if ([string]::IsNullOrWhiteSpace($colorName)) {
        throw "A feature without style_id must provide color_name so a deterministic style can be derived."
    }

    $geometryRole = Get-FirstStringProperty $Properties @("geometry_role")
    if ([string]::IsNullOrWhiteSpace($geometryRole)) {
        $geometryRole = if ($GeometryType -eq "Point") {
            "point"
        }
        elseif ($GeometryType -like "*LineString") {
            "linework"
        }
        else {
            "filled_region"
        }
    }

    $kind = if ($GeometryType -eq "Point" -and $geometryRole -eq "text_label") {
        "label"
    }
    elseif ($GeometryType -eq "Point") {
        "point"
    }
    elseif ($GeometryType -like "*LineString") {
        "line"
    }
    else {
        "area"
    }

    $colorId = ConvertTo-SafeIdPart (ConvertTo-AvisoDisplayName $colorName)
    $roleId = ConvertTo-SafeIdPart $geometryRole
    if ([string]::IsNullOrWhiteSpace($colorId) -or [string]::IsNullOrWhiteSpace($roleId)) {
        throw "A deterministic AVISO style_id could not be derived from geometry_role '$geometryRole' and color_name '$colorName'."
    }

    $layer = if ($kind -eq "label") {
        "Labels"
    }
    elseif ($kind -eq "point") {
        "Reference points"
    }
    elseif ($geometryRole -eq "runway_centerline") {
        "Runways"
    }
    elseif ($kind -eq "line") {
        "Ground linework"
    }
    else {
        "Airfield surfaces"
    }

    $objectType = if ($kind -eq "label") {
        "Label"
    }
    elseif ($kind -eq "point") {
        "Point"
    }
    elseif ($kind -eq "line") {
        "Line"
    }
    else {
        "Area"
    }

    return New-LfpgStyleInfo `
        "$kind.$roleId.$colorId" `
        $layer `
        (ConvertTo-AvisoDisplayName $colorName) `
        $objectType `
        ""
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

function Test-ProhibitedAvisoLabel {
    param(
        [string]$GeometryType,
        [string]$ObjectType,
        [string]$GeometryRole,
        [string]$StyleId,
        [string]$Category,
        [string]$Layer,
        [string]$Name,
        $SourceProperties,
        $Style
    )

    $isLabel = $GeometryType -eq "Point" -and (
        $ObjectType -eq "Label" -or
        $GeometryRole -eq "text_label"
    )
    if (-not $isLabel) {
        return $false
    }

    $identity = @(
        $StyleId,
        $Category,
        $Layer,
        $Name,
        (Get-FirstStringProperty $SourceProperties @("label_class", "text-field", "text", "label")),
        (Get-FirstStringProperty $Style @("name", "layer"))
    ) -join " "
    return $identity -match '(?i)(^|[^A-Z0-9])(AMSR|TMA|VFR)([^A-Z0-9]|$)'
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
        if ($ids.Count -gt 0) {
            return $ids.ToArray()
        }
        return @()
    }
    return @()
}

function Get-AvisoAirportFromFileName {
    param([string]$Name)

    if ($Name -match $AvisoFileNamePattern) {
        return $matches['airport'].ToUpperInvariant()
    }
    throw "AVISO filename '$Name' must use ICAO.geojson or ICAO_Safe-Variant.geojson."
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
    $hasExplicitGroups = Test-JsonProperty $document "vsmr_groups"
    $styles = [ordered]@{}

    if ((Test-JsonProperty $document "metadata") -and
        (Test-JsonProperty $document.metadata "schema_version") -and
        [int]$document.metadata.schema_version -gt 2) {
        throw "$($File.Name) uses an unsupported future schema_version."
    }

    $backgroundNight = "#434A4F"
    $backgroundDay = "#434A4F"
    if ((Test-JsonProperty $document "metadata") -and
        (Test-JsonProperty $document.metadata "background_colors")) {
        $backgroundColors = $document.metadata.background_colors
        if (-not ($backgroundColors -is [pscustomobject])) {
            throw "$($File.Name) metadata.background_colors must be an object."
        }
        foreach ($palette in @("night", "day")) {
            if (-not (Test-JsonProperty $backgroundColors $palette) -or
                [string]$backgroundColors.$palette -notmatch '^#[0-9A-Fa-f]{6}$') {
                throw "$($File.Name) metadata.background_colors.$palette must be a #RRGGBB color."
            }
        }
        $backgroundNight = ([string]$backgroundColors.night).ToUpperInvariant()
        $backgroundDay = ([string]$backgroundColors.day).ToUpperInvariant()
    }

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
            $info = New-DerivedAvisoStyleInfo $sourceProperties $geometryType
            $styleId = $info.Id
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
		$sourceGeometryRole = Get-FirstStringProperty $sourceProperties @("geometry_role")

        # Canonicalize imported geometry names to the semantic types consumed
        # by both the renderer and the web editor. A Point carrying label text
        # paint/role is a Label, while marker-style reference points remain Point.
        if ($geometryType -eq "Point" -and (
            [string]$style.object_type -eq "Label" -or
            [string]$objectType -eq "Label" -or
            $sourceGeometryRole -in @("label", "text_label"))) {
            $objectType = "Label"
        }
        elseif ($geometryType -like "*LineString") {
            $objectType = "Line"
        }
        elseif ($geometryType -like "*Polygon") {
            $objectType = "Area"
        }

        $geometryRole = $sourceGeometryRole
        if ($geometryType -eq "Point" -and $objectType -eq "Label" -and
            ([string]::IsNullOrWhiteSpace($geometryRole) -or $geometryRole -in @("label", "point"))) {
            $geometryRole = "text_label"
        }
        elseif ([string]::IsNullOrWhiteSpace($geometryRole) -or $geometryRole -in @("aviso_region", "aviso_linework")) {
            $geometryRole = if ($geometryType -eq "Point") { "text_label" } elseif ($geometryType -like "*LineString") { "linework" } else { "filled_region" }
        }
        $sourceName = Get-FirstStringProperty $sourceProperties @("name")
        if (Test-ProhibitedAvisoLabel $geometryType $objectType $geometryRole $styleId $category $layer $sourceName $sourceProperties $style) {
            continue
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

        # The runtime schema is always a flat string array. Older normalizer
        # output used [ [] ] as an empty placeholder, which is not a valid
        # vsmr_group_ids value. Legacy documents retain a real [] so their
        # canonical shape remains explicit until they opt into a group catalog.
        $groupIds = @(Get-AvisoGroupIds $sourceProperties)
        if ($groupIds.Count -gt 0) {
            $properties["vsmr_group_ids"] = $groupIds
        }
        elseif (-not $hasExplicitGroups) {
            $properties["vsmr_group_ids"] = @()
        }

        foreach ($paintKey in $PaintKeys) {
            if (-not (Test-JsonProperty $sourceProperties $paintKey)) {
                continue
            }
            if ($airport -eq "LFMN" -and $paintKey -eq "palette-overrides") {
                continue
            }
            if (Test-JsonProperty $style.paint $paintKey) {
                # Catalog paint is the shared default; a differing feature value
                # is an intentional per-object override and must survive a
                # normalize pass. Redundant values are omitted canonically.
                $sourcePaintJson = ConvertTo-Json -InputObject $sourceProperties.$paintKey -Depth 10 -Compress
                $stylePaintJson = ConvertTo-Json -InputObject $style.paint.$paintKey -Depth 10 -Compress
                if ($sourcePaintJson -ceq $stylePaintJson) {
                    continue
                }
            }
            $properties[$paintKey] = if ($paintKey -eq "palette-overrides") {
                ConvertTo-CanonicalValue $sourceProperties.$paintKey
            }
            else {
                $sourceProperties.$paintKey
            }
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
        $normalizedPaint = New-NormalizedPaint $style.paint
        if ([string]$style.object_type -eq "Label") {
            $labelIdentity = "$styleId $($style.name)"
            $preserveSuppliedZoom = $airport -in @("LFPG", "LFMN") -and
                (Test-JsonProperty $normalizedPaint "zoomLevel")
            if ($preserveSuppliedZoom) {
                $labelZoomLevel = [int]$normalizedPaint.zoomLevel
            }
            elseif ($airport -notin @("LFPG", "LFMN") -and $labelIdentity -match '(?i)gate|stand') {
                $labelZoomLevel = 9
            }
            elseif ($airport -notin @("LFPG", "LFMN") -and $labelIdentity -match '(?i)taxiway') {
                $labelZoomLevel = 7
            }
            else {
                $labelZoomLevel = Get-AvisoLabelZoomLevel $styleId $normalizedFeatures
            }
            if (Test-JsonProperty $normalizedPaint "zoomLevel") {
                $normalizedPaint.zoomLevel = $labelZoomLevel
            }
            else {
                $normalizedPaint | Add-Member -MemberType NoteProperty -Name "zoomLevel" -Value $labelZoomLevel
            }
            # Re-run the canonical paint order so zoomLevel remains before the
            # optional palette-overrides object.
            $normalizedPaint = New-NormalizedPaint $normalizedPaint
        }
        if ($airport -eq "LFMN" -and (Test-JsonProperty $normalizedPaint "palette-overrides")) {
            # LFMN intentionally uses one palette in both modes. Removing the
            # override makes Day fall back exactly to the Night/base paint.
            $normalizedPaint.PSObject.Properties.Remove("palette-overrides")
        }
        $normalizedStyles[$styleId] = [pscustomobject][ordered]@{
            name = [string]$style.name
            layer = [string]$style.layer
            object_type = [string]$style.object_type
            paint = $normalizedPaint
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
        default_color_palette = "night"
        color_palettes = @("night", "day")
        background_colors = [pscustomobject][ordered]@{
            night = $backgroundNight
            day = $backgroundDay
        }
        feature_count = $normalizedFeatures.Count
        style_count = $normalizedStyles.Count
        layer_counts = [pscustomobject]$normalizedLayerCounts
        category_counts = [pscustomobject]$normalizedCategoryCounts
    }

    $documentName = Get-FirstStringProperty $document @("name")
    if ([string]::IsNullOrWhiteSpace($documentName)) {
        $documentName = "$airport AVISO"
    }
    $root = [ordered]@{
        type = "FeatureCollection"
        name = $documentName
        bbox = Get-JsonProperty $document "bbox"
        metadata = $metadata
        styles = [pscustomobject]$normalizedStyles
    }

    if (Test-JsonProperty $document "vsmr_groups") {
        if (-not ($document.vsmr_groups -is [System.Array])) {
            throw "$($File.Name) vsmr_groups must be an array."
        }
        $groups = @($document.vsmr_groups | Where-Object {
            $identity = ([string]$_.id) + " " + ([string]$_.name)
            $identity -notmatch '(?i)(^|[^A-Z0-9])(AMSR|TMA|VFR)([^A-Z0-9]|$)'
        })
        if ($groups.Count -gt 0) {
            $root["vsmr_groups"] = ConvertTo-CanonicalValue $groups
        }
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
$avisoFiles = @(
    Get-ChildItem -File -LiteralPath $avisoDirectory |
        Where-Object { $_.Name -match $AvisoFileNamePattern } |
        Sort-Object Name
)
if ($avisoFiles.Count -eq 0) {
    throw "No AVISO GeoJSON files were found."
}
foreach ($file in $avisoFiles) {
    Normalize-AvisoFile $file
}


Write-Host "Runtime data $($Mode.ToLowerInvariant()) completed successfully."
