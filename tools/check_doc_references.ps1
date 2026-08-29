# Verify local Markdown links and docs/... references embedded in source files.
param([string]$Repo = (Resolve-Path "$PSScriptRoot/..").Path)

$ErrorActionPreference = "Stop"
$repoPath = (Resolve-Path -LiteralPath $Repo).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Get-AnchorSet([string]$Path) {
    $anchors = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^#{1,6}\s+(.+?)\s*$') { continue }
        $anchor = $Matches[1].ToLowerInvariant()
        $anchor = $anchor -replace '<[^>]+>', ''
        $anchor = $anchor -replace '[^\p{L}\p{Nd}\s_-]', ''
        $anchor = $anchor.Trim() -replace '\s+', '-'
        [void]$anchors.Add($anchor)
    }
    return $anchors
}

function Test-Reference([string]$Source, [string]$Target, [string]$Display) {
    $decoded = [System.Uri]::UnescapeDataString($Target.Trim('<', '>'))
    if ($decoded -match '^(?i:https?://|mailto:)' -or $decoded.StartsWith('#')) { return }

    $parts = $decoded -split '#', 2
    $pathPart = $parts[0]
    $anchor = if ($parts.Count -eq 2) { $parts[1] } else { '' }
    if ([string]::IsNullOrWhiteSpace($pathPart)) { return }

    $base = Split-Path -Parent $Source
    $candidate = if ($pathPart -match '^[A-Za-z]:[/\\]') {
        $pathPart
    } elseif ($pathPart -like 'docs/*' -or $pathPart -like 'docs\*') {
        Join-Path $repoPath $pathPart
    } else {
        Join-Path $base $pathPart
    }

    $full = [System.IO.Path]::GetFullPath($candidate)
    if (-not $full.StartsWith($repoPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        $failures.Add("$Display escapes the repository: $Target")
        return
    }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        $failures.Add("$Display targets a missing file: $Target")
        return
    }

    if ($anchor -and [System.IO.Path]::GetExtension($full) -eq '.md') {
        $anchors = Get-AnchorSet $full
        if (-not $anchors.Contains($anchor)) {
            $failures.Add("$Display targets a missing anchor '#$anchor' in $Target")
        }
    }
}

$markdown = Get-ChildItem -LiteralPath (Join-Path $repoPath 'docs') -Recurse -File -Filter '*.md'
foreach ($file in $markdown) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        ++$lineNo
        foreach ($match in [regex]::Matches($line, '(?<!\!)\[[^\]]*\]\(([^)]+)\)')) {
            $target = ($match.Groups[1].Value -split '\s+["''][^"'']*["'']\s*$')[0]
            Test-Reference $file.FullName $target "$($file.FullName):$lineNo"
        }
    }
}

$sourceExtensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.rs', '.cmake', '.txt', '.ps1')
$sourceRoots = @('core', 'include', 'src', 'tests', 'tools', 'cmake')
foreach ($rootName in $sourceRoots) {
    $root = Join-Path $repoPath $rootName
    if (-not (Test-Path -LiteralPath $root)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
        if ($sourceExtensions -notcontains $file.Extension.ToLowerInvariant() -and
            $file.Name -ne 'CMakeLists.txt') { continue }
        $lineNo = 0
        foreach ($line in Get-Content -LiteralPath $file.FullName) {
            ++$lineNo
            foreach ($match in [regex]::Matches(
                $line, 'docs/[A-Za-z0-9_.\-/]+\.md(?:#[A-Za-z0-9_\-]+)?')) {
                Test-Reference $file.FullName $match.Value "$($file.FullName):$lineNo"
            }
        }
    }
}

if ($failures.Count) {
    $failures | Sort-Object -Unique | ForEach-Object { Write-Host "  [FAIL] $_" }
    throw "$($failures.Count) broken documentation reference(s)"
}

Write-Host "  [ OK ] local Markdown links and source documentation references resolve"
