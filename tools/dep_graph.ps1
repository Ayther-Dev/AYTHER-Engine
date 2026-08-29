# ---------------------------------------------------------------------------
# dep_graph.ps1 — derive the repository dependency graph from build metadata.
#
# The graph comes from the two build-system sources of truth:
#
#   · Rust  — `cargo metadata`, las dependencias por PATH entre crates propios;
#   · C++   — `cmake --graphviz`, el grafo REAL de targets del build configurado.
#
# `-Check` compares the derived graph with the versioned document so dependency
# drift cannot remain hidden in hand-maintained prose.
#
# Uso:
#   pwsh tools/dep_graph.ps1            # regenera el documento
#   pwsh tools/dep_graph.ps1 -Check     # falla si quedó desactualizado (CI)
# ---------------------------------------------------------------------------
param(
    [switch]$Check,
    [string]$BuildDir = "build",
    [string]$Out = "docs/DEPENDENCY_GRAPH.md"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

# -- Rust: dependencias por path entre crates del workspace ------------------
$meta = cargo metadata --no-deps --format-version 1 | ConvertFrom-Json
$rust = @()
foreach ($p in $meta.packages | Sort-Object name) {
    $propias = @($p.dependencies | Where-Object { $_.path } | ForEach-Object { $_.name } | Sort-Object)
    # `version` además de `path` (#556): sin ella, el día de la migración hay
    # que tocar cada manifiesto y re-verificar el build completo.
    $sin_version = @($p.dependencies | Where-Object { $_.path -and -not $_.req -or ($_.path -and $_.req -eq '*') } |
                     ForEach-Object { $_.name })
    $rust += [pscustomobject]@{
        name = $p.name; deps = $propias; sin_version = $sin_version
    }
}

# -- C++: el grafo real de targets, si hay un build configurado --------------
$cpp = @()
$cpp_ok = $false
# Keep only Engine repository targets. Product consumers live in separate
# repositories and must not reappear as local graph members.
$miembros = @("ayther_engine", "ayther_core", "ayther_cxx", "ayther_ymfm",
              "ayther_vpx", "Ayther::engine", "Ayther::core", "Ayther::cxx",
              "Ayther::ymfm", "Ayther::vpx")
if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "ayther-depgraph"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    cmake "--graphviz=$tmp/deps.dot" -S . -B $BuildDir | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path "$tmp/deps.dot")) {
        $dot = Get-Content "$tmp/deps.dot"
        foreach ($l in $dot) {
            if ($l -match '//\s*(\S+)\s*->\s*(\S+)\s*$') {
                $de = $Matches[1]; $a = $Matches[2]
                # Keep repository/package members only. Third-party and test edges
                # would obscure the installed Engine dependency boundary.
                if ($miembros -contains $de -and $miembros -contains $a -and $de -ne $a) {
                    $cpp += "$de -> $a"
                }
            }
        }
        $cpp = @($cpp | Sort-Object -Unique)
        $cpp_ok = $true
    } else {
        Write-Warning "CMake graph generation failed; the C++ section was not derived."
    }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# -- El documento ------------------------------------------------------------
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# Generated dependency graph")
[void]$sb.AppendLine()
[void]$sb.AppendLine("**Status:** generated from the current checkout")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERATED — do not edit by hand.** Run `pwsh tools/dep_graph.ps1`.")
[void]$sb.AppendLine("> `pwsh tools/dep_graph.ps1 -Check` verifies that this document still")
[void]$sb.AppendLine("> matches `Cargo.toml` and the configured CMake target graph.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Rust crates")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| Crate | Local path dependencies |")
[void]$sb.AppendLine("|---|---|")
foreach ($r in $rust) {
    $d = if ($r.deps.Count) { ($r.deps -join ", ") } else { "—" }
    [void]$sb.AppendLine("| ``$($r.name)`` | $d |")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## C++ targets")
[void]$sb.AppendLine()
if ($cpp_ok) {
    [void]$sb.AppendLine("Derived from ``cmake --graphviz`` over the configured build.")
    [void]$sb.AppendLine("Only Engine repository and installed ``Ayther::*`` package targets are included.")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine('```')
    foreach ($e in $cpp) { [void]$sb.AppendLine($e) }
    [void]$sb.AppendLine('```')
} else {
    [void]$sb.AppendLine("_No configured build was available, so this section was not derived._")
}
$nuevo = $sb.ToString() -replace "`r`n", "`n"

$outPath = Join-Path $repo $Out
if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out no existe — corré el script sin -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    # La sección de C++ sólo se compara cuando se pudo derivar: en un runner sin
    # build configurado, exigirla haría fallar por algo que no se midió.
    if (-not $cpp_ok) {
        $viejo  = ($viejo  -split "## C\+\+ targets")[0]
        $nuevo  = ($nuevo  -split "## C\+\+ targets")[0]
        Write-Host "  (no configured build: checking the Rust graph only)"
    }
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nThe documented graph does not match build metadata." -ForegroundColor Red
        Write-Host "Regenerate it with: pwsh tools/dep_graph.ps1"
        exit 1
    }
    Write-Host "  [ OK ] the documented graph matches build metadata"
    exit 0
}

New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  wrote: $Out"
