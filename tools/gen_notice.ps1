# ---------------------------------------------------------------------------
# gen_notice.ps1 — the third-party NOTICE, generated and verifiable (#532).
#
# A hand-written NOTICE is a list that ages with every new dependency, and the
# day it ages it stops meeting the very obligation it exists to meet:
# permissive licences require the notice to travel WITH the binary, and an
# incomplete notice is as useless as none. Here it is derived from the two real
# sources:
#
#   · Cargo   — `cargo metadata` reports the `license` field of every crate in
#               the graph, with no network access;
#   · vcpkg   — every installed port leaves its `share/<port>/copyright`, which
#               is the text the port itself declared.
#
# With `-Check` it compares against the versioned file and fails if it went
# stale. The future release CI must invoke it to turn that comparison into an
# automatic guarantee.
#
# Usage:
#   pwsh tools/gen_notice.ps1 -BuildDir build/windows-native-vpx
#   pwsh tools/gen_notice.ps1 -BuildDir build/windows-native-vpx -Check
# ---------------------------------------------------------------------------
param(
    [switch]$Check,
    [string]$BuildDir = "build",
    [string]$Out = "NOTICE.md"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

# -- Rust --------------------------------------------------------------------
# The COMPLETE graph (transitive dependencies included): what is distributed is
# the linked binary, not the manifest.
# `-AsHashtable`: the full graph brings in crates whose features differ only in
# case (`USB` and `usb`), and the object converter treats them as the same
# property and aborts. With a hashtable there is no collision.
# `--filter-platform`: without it, crates for platforms this binary never links
# creep in (wasm, redox, the `unix` ones on Windows) and the notice fills up
# with dependencies that are not distributed. Declaring MORE breaks nothing,
# but a NOTICE nobody finishes reading does not comply either.
$triple = "x86_64-pc-windows-msvc"
$meta = cargo metadata --format-version 1 --filter-platform $triple |
        ConvertFrom-Json -AsHashtable -Depth 100
$propios = @($meta.workspace_members | ForEach-Object { ($_ -split '[ @]')[0] })
$crates = @()
foreach ($p in $meta.packages | Sort-Object { $_.name }) {
    if ($propios -contains $p.name) { continue }   # ours is not "third party"
    $crates += [pscustomobject]@{
        name    = $p.name
        version = $p.version
        license = if ($p.license) { $p.license } else { "(not declared)" }
    }
}

# -- vcpkg -------------------------------------------------------------------
$ports = @()
$share = Join-Path $repo "$BuildDir/vcpkg_installed/x64-windows/share"
$statusVersions = @{}
$statusPath = Join-Path $repo "$BuildDir/vcpkg_installed/vcpkg/status"
if (Test-Path $statusPath) {
    foreach ($block in ((Get-Content $statusPath -Raw) -split "(?:`r?`n){2,}")) {
        $package = [regex]::Match($block, '(?m)^Package:\s*(\S+)\s*$')
        $version = [regex]::Match($block, '(?m)^Version:\s*(.+?)\s*$')
        $arch = [regex]::Match($block, '(?m)^Architecture:\s*(\S+)\s*$')
        if ($package.Success -and $version.Success -and
            $arch.Success -and $arch.Groups[1].Value -eq 'x64-windows') {
            $statusVersions[$package.Groups[1].Value] =
                $version.Groups[1].Value
        }
    }
}
if (Test-Path $share) {
    foreach ($d in Get-ChildItem $share -Directory | Sort-Object Name) {
        $cp = Join-Path $d.FullName "copyright"
        if (-not (Test-Path $cp)) { continue }
        $texto = Get-Content $cp -Raw
        # The FIRST non-empty line is enough to identify the licence; the full
        # text ships in the vcpkg package and is not copied here (hundreds of
        # KB nobody reads and that go stale anyway).
        $primera = ($texto -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
        $version = if ($statusVersions.ContainsKey($d.Name)) {
            $statusVersions[$d.Name]
        } else { '(no version in status)' }
        $ports += [pscustomobject]@{
            name = $d.Name; version = $version; licencia = $primera
        }
    }
}

# -- The document ------------------------------------------------------------
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# NOTICE — third-party dependencies")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERATED — do not edit by hand.** ``pwsh tools/gen_notice.ps1``.")
[void]$sb.AppendLine("> Verify with ``pwsh tools/gen_notice.ps1 -BuildDir build/<native-preset> -Check``; the release CI must run it.")
[void]$sb.AppendLine("> An incomplete NOTICE is as useless as none — permissive licences")
[void]$sb.AppendLine("> require the notice to travel WITH the binary.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Rust crates (full graph, transitive dependencies included)")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| crate | version | licence |")
[void]$sb.AppendLine("|---|---|---|")
foreach ($c in $crates) {
    [void]$sb.AppendLine("| ``$($c.name)`` | $($c.version) | $($c.license) |")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## C/C++ libraries (vcpkg)")
[void]$sb.AppendLine()
if ($ports.Count) {
    [void]$sb.AppendLine("The full text of each licence ships in")
    [void]$sb.AppendLine("``vcpkg_installed/<triplet>/share/<port>/copyright``.")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("| port | version | licence (first line of the copyright) |")
    [void]$sb.AppendLine("|---|---|---|")
    foreach ($p in $ports) {
        $l = $p.licencia -replace '\|', '\|'
        [void]$sb.AppendLine("| ``$($p.name)`` | $($p.version) | $l |")
    }
} else {
    [void]$sb.AppendLine("_No ``vcpkg_installed``: this section was not derived._")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Sources vendored in the repository")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| component | revision | licence | why it is here |")
[void]$sb.AppendLine("|---|---|---|---|")
[void]$sb.AppendLine("| ``third_party/ymfm`` | ``81aec25ccbb98f4873a255f7551ac4dadac59b4a`` | BSD-3-Clause | FM synthesiser for the voice router (#327). It is ymfm and NOT the fork's Nuked OPN2, which is LGPL-2.1: the engine is a STATIC library and that would force dynamic distribution. |")
[void]$sb.AppendLine("| ``third_party/libvpx`` | tag ``v1.15.2`` | BSD-3-Clause + patent grant | decoder for the Cinematic subsystem (#263). It is libvpx and NOT FFmpeg for the same boundary reason: the FFmpeg core is LGPL-2.1+. |")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## What is NOT distributed")
[void]$sb.AppendLine()
[void]$sb.AppendLine("libretro cores (supplied by the user — BYOC), ROMs and BIOS images (BYOR),")
[void]$sb.AppendLine("and packs derived from commercial games. The guard")
[void]$sb.AppendLine("``sdk/tools/check_sdk_leak.cmake`` verifies this against the published artifact.")

$nuevo = $sb.ToString() -replace "`r`n", "`n"
$outPath = Join-Path $repo $Out

if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out does not exist — run the script without -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    # Without vcpkg_installed the C/C++ section was not derived: comparing it
    # would fail over something that was never measured.
    if (-not $ports.Count) {
        $viejo = ($viejo -split "## C/C\+\+ libraries")[0]
        $nuevo = ($nuevo -split "## C/C\+\+ libraries")[0]
        Write-Host "  (no vcpkg_installed: only the crates are verified)"
    }
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nThe NOTICE is out of date." -ForegroundColor Red
        Write-Host "Regenerate it with: pwsh tools/gen_notice.ps1"
        exit 1
    }
    Write-Host "  [ OK ] the NOTICE matches the declared dependencies"
    exit 0
}

[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  written: $Out  ($($crates.Count) crates, $($ports.Count) ports)"
