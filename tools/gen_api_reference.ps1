# ---------------------------------------------------------------------------
# gen_api_reference.ps1 — generated public API reference.
#
# WHY IT IS NOT DOXYGEN (yet). Doxygen produces a better reference, but it
# demands a tool installed on every machine that wants to regenerate it and in
# CI. This reads the SAME manifest that drives CMake's installed allowlist. If
# a header enters or leaves the package, the reference changes with it instead
# of maintaining a parallel inventory.
#
# What it does NOT do, stated plainly: it does not really parse C++. It
# extracts each header's file banner and its top-level declarations with their
# comments. That is enough to answer "what is there and what is it for?" and
# not "what is the exact signature of this overload?" — the header answers
# that, and it ships in the package.
#
# Usage:
#   pwsh tools/gen_api_reference.ps1           # regenerate
#   pwsh tools/gen_api_reference.ps1 -Check    # CI: fails if it went stale
# ---------------------------------------------------------------------------
param(
    [switch]$Check,
    [string]$Out = "docs/PUBLIC_API_INDEX.md"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

$includeRoot = (Resolve-Path "include/ayther").Path
$manifest = Join-Path $repo "cmake/AytherPublicHeaders.txt"
$installed = Get-Content $manifest |
             ForEach-Object { $_.Trim() } |
             Where-Object { $_ -and -not $_.StartsWith('#') }
if (($installed | Select-Object -Unique).Count -ne $installed.Count) {
    throw "cmake/AytherPublicHeaders.txt contains duplicate paths"
}
$headers = $installed | ForEach-Object {
    if ([System.IO.Path]::GetExtension($_) -notin ".h", ".hpp") {
        throw "installed public entry is not a header: $_"
    }
    $path = Join-Path $includeRoot $_
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "installed public header does not exist: $_"
    }
    [pscustomobject]@{
        file = $_.Replace('\', '/')
        nota = "Installed public header."
    }
} | Sort-Object file
if ($headers.Count -lt 2) { throw "the public list came out with $($headers.Count) headers" }

function Get-HeaderAnchor([string]$File) {
    # Match GitHub/JetBrains heading slugs: preserve '_' and '-', remove path
    # separators and punctuation, and turn whitespace into '-'.
    $slug = $File.ToLowerInvariant() -replace '[^a-z0-9 _-]', ''
    return (($slug -replace '\s+', '-').Trim('-'))
}

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# AYTHER Engine — index of installed headers")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERATED — do not edit by hand.** ``pwsh tools/gen_api_reference.ps1``.")
[void]$sb.AppendLine("> Derived from ``cmake/AytherPublicHeaders.txt``, the manifest that")
[void]$sb.AppendLine("> drives CMake's installed allowlist: if a header enters")
[void]$sb.AppendLine("> or leaves the surface, this page reflects it without anyone editing a")
[void]$sb.AppendLine("> parallel list.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("The installed surface and its stability are described in")
[void]$sb.AppendLine("[``API_COMPATIBILITY.md``](API_COMPATIBILITY.md).")
[void]$sb.AppendLine("Appearing in this index does not by itself imply a stability guarantee.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## The $($headers.Count) headers")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| header | what it provides |")
[void]$sb.AppendLine("|---|---|")
foreach ($h in $headers) {
    $anchor = Get-HeaderAnchor $h.file
    [void]$sb.AppendLine("| [``$($h.file)``](#$anchor) | $($h.nota) |")
}
[void]$sb.AppendLine()

foreach ($h in $headers) {
    $ruta = Join-Path $repo "include/ayther/$($h.file)"
    if (-not (Test-Path $ruta)) { continue }
    $texto = Get-Content $ruta -Raw

    [void]$sb.AppendLine("---")
    [void]$sb.AppendLine()
    # Explicit IDs avoid renderer-specific heading slug rules for '_', '/' and
    # '.'. The table above and IDE inspections now resolve the same stable ID.
    $anchor = Get-HeaderAnchor $h.file
    [void]$sb.AppendLine("<a id=`"$anchor`"></a>")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("## $($h.file)")
    [void]$sb.AppendLine()

    # The file banner: the comment block at the very top, which is where this
    # repo explains WHY each thing exists. It is taken verbatim — summarising
    # it would lose precisely what is not written down anywhere else.
    $cab = [regex]::Match($texto, '(?s)^(?:#\w+[^\n]*\n|/\* [-]+\n)?((?://[^\n]*\n| \*[^\n]*\n)+)')
    if ($cab.Success) {
        $lineas = $cab.Groups[1].Value -split "`n" | ForEach-Object {
            ($_ -replace '^\s*//\s?', '' -replace '^\s*\*\s?', '' -replace '^-+$', '').TrimEnd()
        }
        # It stops at the first double blank line: the rest of the banner is
        # usually implementation detail, and a reference that copies everything
        # is not a reference.
        $acum = @()
        $vacias = 0
        foreach ($l in $lineas) {
            if ([string]::IsNullOrWhiteSpace($l)) { $vacias++ } else { $vacias = 0 }
            if ($vacias -ge 2 -and $acum.Count -gt 3) { break }
            $acum += $l
        }
        [void]$sb.AppendLine(($acum -join "`n").Trim())
        [void]$sb.AppendLine()
    }

    # Top-level declarations: the types and functions the consumer uses.
    $decls = [regex]::Matches($texto,
        '(?m)^(?:struct|class|enum(?:[ \t]+class)?|typedef|inline|[A-Za-z_][A-Za-z0-9_:<>\* \t]*?)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(\(|\{|;)')
    $nombres = $decls | ForEach-Object { $_.Groups[1].Value } |
               Where-Object { $_ -notmatch '^(if|for|while|return|else|namespace)$' } |
               Select-Object -Unique | Sort-Object
    if ($nombres) {
        [void]$sb.AppendLine("**Declares:** " + (($nombres | ForEach-Object { "``$_``" }) -join ", "))
        [void]$sb.AppendLine()
    }
    [void]$sb.AppendLine("_The installed header (``include/ayther/$($h.file)``) carries the full documentation of every symbol._")
    [void]$sb.AppendLine()
}

$nuevo = (($sb.ToString() -replace "`r`n", "`n").TrimEnd()) + "`n"
$outPath = Join-Path $repo $Out

if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out does not exist — run the script without -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nThe reference is out of date." -ForegroundColor Red
        Write-Host "Regenerate it with: pwsh tools/gen_api_reference.ps1"
        exit 1
    }
    Write-Host "  [ OK ] the reference matches the declared surface"
    exit 0
}

[System.IO.Directory]::CreateDirectory(
    [System.IO.Path]::GetDirectoryName($outPath)) | Out-Null
[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  written: $Out  ($($headers.Count) headers)"
