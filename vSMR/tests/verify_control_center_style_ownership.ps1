#requires -Version 5.1

[CmdletBinding()]
param([string]$RepositoryRoot = "")

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$styleRoot = Join-Path $RepositoryRoot "vSMR\src\control_center\web\styles"
$manifestPath = Join-Path $styleRoot "sources.txt"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing style source manifest: $manifestPath"
}
$sourceNames = @(
    [IO.File]::ReadAllLines($manifestPath) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') }
)
if ($sourceNames.Count -eq 0 -or ($sourceNames | Select-Object -Unique).Count -ne $sourceNames.Count) {
    throw "Style source manifest must contain a non-empty, unique source list."
}
$unlistedSources = @(Get-ChildItem -LiteralPath $styleRoot -Filter '*.css' -File |
    Where-Object { $_.Name -notin $sourceNames })
if ($unlistedSources.Count -gt 0) {
    throw "Unlisted stylesheet source(s): $(($unlistedSources.Name | Sort-Object) -join ', ')"
}

function Split-SelectorList([string]$selectorList) {
    $parts = [Collections.Generic.List[string]]::new()
    $start = 0
    $depth = 0
    for ($index = 0; $index -lt $selectorList.Length; ++$index) {
        $character = $selectorList[$index]
        if ($character -eq '(' -or $character -eq '[') { ++$depth }
        elseif ($character -eq ')' -or $character -eq ']') { --$depth }
        elseif ($character -eq ',' -and $depth -eq 0) {
            $parts.Add($selectorList.Substring($start, $index - $start).Trim())
            $start = $index + 1
        }
    }
    $parts.Add($selectorList.Substring($start).Trim())
    return $parts
}

$rules = [Collections.Generic.List[object]]::new()
$forbiddenPatterns = [Collections.Generic.List[string]]::new()
$duplicateProperties = [Collections.Generic.List[string]]::new()
foreach ($sourceName in $sourceNames) {
    $path = Join-Path $styleRoot $sourceName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing style source: $path" }
    $text = [IO.File]::ReadAllText($path)
    if ($text -match '(?i)@scope\s*\(\s*body\s*\)' -or
        $text -match '(?i):is\s*\(\s*\*\s*\)') {
        $forbiddenPatterns.Add($sourceName)
    }
    $text = $text -replace '(?s)/\*.*?\*/', ''
    foreach ($match in [regex]::Matches(
        $text,
        '(?s)(?<selector>[^{}]+?)\s*\{(?<body>[^{}]*)\}')) {
        $selector = ($match.Groups['selector'].Value -replace '\s+', ' ').Trim()
        if (-not $selector -or $selector.StartsWith('@')) { continue }
        $properties = foreach ($part in ($match.Groups['body'].Value -split ';')) {
            $separator = $part.IndexOf(':')
            if ($separator -gt 0) {
                $part.Substring(0, $separator).Trim().ToLowerInvariant()
            }
        }
        foreach ($property in ($properties | Group-Object | Where-Object Count -gt 1)) {
            $duplicateProperties.Add(
                "${sourceName}: ${selector}: $($property.Name)")
        }
    }
    $stack = [Collections.Generic.List[object]]::new()
    $buffer = [Text.StringBuilder]::new()
    foreach ($character in $text.ToCharArray()) {
        if ($character -eq '{') {
            $token = $buffer.ToString().Trim()
            $buffer.Clear() | Out-Null
            if ($token.StartsWith('@')) {
                $stack.Add([pscustomobject]@{ AtRule = $true; Header = $token })
            } elseif ($token) {
                $context = (($stack | Where-Object AtRule | ForEach-Object Header) -join ' > ')
                foreach ($selector in Split-SelectorList $token) {
                    if ($selector) {
                        $rules.Add([pscustomobject]@{
                            Source = $sourceName
                            Selector = ($selector -replace '\s+', ' ')
                            Context = $context
                        })
                    }
                }
                $stack.Add([pscustomobject]@{ AtRule = $false; Header = '' })
            } else {
                $stack.Add([pscustomobject]@{ AtRule = $false; Header = '' })
            }
        } elseif ($character -eq '}') {
            $buffer.Clear() | Out-Null
            if ($stack.Count) { $stack.RemoveAt($stack.Count - 1) }
        } else {
            [void]$buffer.Append($character)
        }
    }
}

if ($forbiddenPatterns.Count -gt 0) {
    throw "Broad scope or selector aliases are not valid ownership boundaries: $($forbiddenPatterns -join ', ')"
}
if ($duplicateProperties.Count -gt 0) {
    throw "Duplicate properties inside stylesheet rule(s):`n$($duplicateProperties -join "`n")"
}

$sameFileDuplicates = $rules | Group-Object Source, Context, Selector |
    Where-Object Count -gt 1
if ($sameFileDuplicates) {
    $detail = $sameFileDuplicates | ForEach-Object {
        $rule = $_.Group[0]
        $context = if ($rule.Context) { $rule.Context } else { '(base)' }
        "$($rule.Source): ${context}: $($rule.Selector)"
    }
    throw "$($sameFileDuplicates.Count) repeated selector/context group(s):`n$($detail -join "`n")"
}

# Responsive variants intentionally repeat selectors. All other at-rule contexts,
# including @supports and @scope, retain the same component ownership boundary.
$baseRules = $rules | Where-Object { $_.Context -notmatch '@(media|container)\b' }
$duplicates = $baseRules | Group-Object Selector |
    Where-Object { ($_.Group.Source | Select-Object -Unique).Count -gt 1 }
if ($duplicates) {
    $detail = $duplicates | ForEach-Object {
        "$($_.Name): $(($_.Group.Source | Select-Object -Unique) -join ', ')"
    }
    throw "$($duplicates.Count) cross-file stylesheet ownership conflict(s):`n$($detail -join "`n")"
}

$nonResponsive = $rules | Where-Object {
    $_.Source -eq 'responsive-polish.css' -and $_.Context -notmatch '@(media|container)'
}
if ($nonResponsive) { throw "responsive-polish.css contains non-responsive rules." }

Write-Host (
    "Control Center stylesheet ownership audit passed: " +
    "$($baseRules.Count) base rules, 0 repeated selector/context groups, " +
    "0 duplicate rule properties, 0 cross-file conflicts, " +
    "0 non-responsive rules, and 0 unlisted sources.")
