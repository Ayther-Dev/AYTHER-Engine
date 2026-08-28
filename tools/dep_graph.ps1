# ---------------------------------------------------------------------------
# dep_graph.ps1 — el grafo de dependencias se DERIVA, no se dibuja a mano (#557).
#
# Migrar el monorepo es cortar por las dependencias reales, y hacerlo con el
# mapa equivocado es cortar por donde no es. El mapa estaba equivocado: el
# README decía que `ayther_play` linkea `ayther_core` cuando su Cargo.toml no lo
# listaba — un diagrama escrito a mano que envejeció sin que nadie se enterara.
#
# Acá el grafo sale de las dos fuentes de verdad:
#
#   · Rust  — `cargo metadata`, las dependencias por PATH entre crates propios;
#   · C++   — `cmake --graphviz`, el grafo REAL de targets del build configurado.
#
# Y se compara contra el documento versionado. Un diagrama que no se puede
# desactualizar en silencio es la única forma de que sirva el día de la
# migración, que es dentro de meses y con las decisiones ya tomadas.
#
# Uso:
#   pwsh tools/dep_graph.ps1            # regenera el documento
#   pwsh tools/dep_graph.ps1 -Check     # falla si quedó desactualizado (CI)
# ---------------------------------------------------------------------------
param(
    [switch]$Check,
    [string]$BuildDir = "build",
    [string]$Out = "docs/architecture/dependency-graph.md"
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
# `Ayther::*` son los targets del PAQUETE (ADR-004 E8.1): runtime y sdk consumen
# el motor por find_package, y graphviz no atraviesa el `$<LINK_ONLY:>` que une
# `Ayther::engine` con `ayther_engine`. Se dibujan como nodos propios: la arista
# `X -> Ayther::frontend` es exactamente la dependencia que existe.
$miembros = @("ayther_engine", "ayther_runtime", "ayther_lab", "ayther_core",
              "ayther_cxx", "ayther_ymfm", "ayther_play",
              "Ayther::engine", "Ayther::frontend", "Ayther::core", "Ayther::cxx",
              "ay_observe", "ay_conformance", "ay_test_core")
if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "ayther-depgraph"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    cmake "--graphviz=$tmp/deps.dot" -S . -B $BuildDir | Out-Null
    $dot = Get-Content "$tmp/deps.dot" -ErrorAction SilentlyContinue
    foreach ($l in $dot) {
        if ($l -match '//\s*(\S+)\s*->\s*(\S+)\s*$') {
            $de = $Matches[1]; $a = $Matches[2]
            # Sólo entre MIEMBROS del ecosistema: el grafo completo son cientos
            # de aristas de oráculos y de vcpkg, y lo que se decide con este
            # documento es dónde cortar el repo.
            # Las auto-aristas son ruido del graphviz (el exe y su target
            # homónimo), no una dependencia.
            if ($miembros -contains $de -and $miembros -contains $a -and $de -ne $a) {
                $cpp += "$de -> $a"
            }
        }
    }
    $cpp = @($cpp | Sort-Object -Unique)
    $cpp_ok = $true
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# -- El documento ------------------------------------------------------------
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# Grafo de dependencias del ecosistema")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERADO — no editar a mano.** `pwsh tools/dep_graph.ps1`.")
[void]$sb.AppendLine("> CI lo verifica (`-Check`): si este archivo no coincide con lo que")
[void]$sb.AppendLine("> declaran `Cargo.toml` y los `CMakeLists.txt`, el job falla. El mapa")
[void]$sb.AppendLine("> escrito a mano ya se desactualizó una vez, y de eso salió #557.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Crates de Rust")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| crate | depende de (crates propios) |")
[void]$sb.AppendLine("|---|---|")
foreach ($r in $rust) {
    $d = if ($r.deps.Count) { ($r.deps -join ", ") } else { "—" }
    [void]$sb.AppendLine("| ``$($r.name)`` | $d |")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Targets de C++")
[void]$sb.AppendLine()
if ($cpp_ok) {
    [void]$sb.AppendLine("Derivado de ``cmake --graphviz`` sobre el build configurado.")
    [void]$sb.AppendLine("``Ayther::*`` es el paquete que publica el motor (``find_package(Ayther)``):")
    [void]$sb.AppendLine("``Ayther::engine`` envuelve ``ayther_engine`` (el contrato, ``<ayther/...>``) y")
    [void]$sb.AppendLine("``Ayther::frontend`` agrega la superficie de primera parte — ADR-004 E8.")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine('```')
    foreach ($e in $cpp) { [void]$sb.AppendLine($e) }
    [void]$sb.AppendLine('```')
} else {
    [void]$sb.AppendLine("_Sin build configurado: esta sección no se derivó._")
}
$nuevo = $sb.ToString() -replace "`r`n", "`n"

$outPath = Join-Path $repo $Out
if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out no existe — corré el script sin -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    # La sección de C++ sólo se compara cuando se pudo derivar: en un runner sin
    # build configurado, exigirla haría fallar por algo que no se midió.
    if (-not $cpp_ok) {
        $viejo  = ($viejo  -split "## Targets de C\+\+")[0]
        $nuevo  = ($nuevo  -split "## Targets de C\+\+")[0]
        Write-Host "  (sin build: sólo se verifica el grafo de Rust)"
    }
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nEl grafo del documento NO coincide con lo declarado." -ForegroundColor Red
        Write-Host "Regeneralo con: pwsh tools/dep_graph.ps1"
        exit 1
    }
    Write-Host "  [ OK ] el grafo del documento coincide con lo declarado"
    exit 0
}

New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  escrito: $Out"
