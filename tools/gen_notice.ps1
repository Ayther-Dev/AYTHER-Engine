# ---------------------------------------------------------------------------
# gen_notice.ps1 — el NOTICE de terceros, generado y verificable (#532).
#
# Un NOTICE escrito a mano es una lista que envejece con cada dependencia nueva,
# y el día que envejece deja de cumplir la obligación que existe para cumplir:
# las licencias permisivas piden que el aviso viaje CON el binario, y un aviso
# incompleto es tan inútil como ninguno. Acá sale de las dos fuentes reales:
#
#   · Cargo   — `cargo metadata` trae el campo `license` de cada crate del
#               grafo, sin red;
#   · vcpkg   — cada port instalado deja su `share/<port>/copyright`, que es el
#               texto que el port declaró.
#
# Con `-Check` compara contra el archivo versionado y falla si quedó
# desactualizado. La futura CI de release debe invocarlo para convertir esa
# comprobación en una garantía automática.
#
# Uso:
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
# El grafo COMPLETO (con dependencias transitivas): lo que se distribuye es el
# binario enlazado, no el manifiesto.
# `-AsHashtable`: el grafo completo trae crates con features que difieren sólo
# en mayúsculas (`USB` y `usb`), y el conversor a objetos las toma como la misma
# propiedad y aborta. Con hashtable no hay colisión.
# `--filter-platform`: sin eso entran los crates de plataformas que este binario
# nunca enlaza (wasm, redox, los `unix` en Windows) y el aviso se llena de
# dependencias que no se distribuyen. Declarar de MÁS no incumple nada, pero un
# NOTICE que nadie termina de leer tampoco cumple.
$triple = "x86_64-pc-windows-msvc"
$meta = cargo metadata --format-version 1 --filter-platform $triple |
        ConvertFrom-Json -AsHashtable -Depth 100
$propios = @($meta.workspace_members | ForEach-Object { ($_ -split '[ @]')[0] })
$crates = @()
foreach ($p in $meta.packages | Sort-Object { $_.name }) {
    if ($propios -contains $p.name) { continue }   # lo nuestro no es «tercero»
    $crates += [pscustomobject]@{
        name    = $p.name
        version = $p.version
        license = if ($p.license) { $p.license } else { "(no declarada)" }
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
        # La PRIMERA línea con contenido alcanza para identificar la licencia;
        # el texto completo viaja en el paquete de vcpkg y no se copia acá (son
        # cientos de KB que nadie lee y que se desactualizan igual).
        $primera = ($texto -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
        $version = if ($statusVersions.ContainsKey($d.Name)) {
            $statusVersions[$d.Name]
        } else { '(sin versión en status)' }
        $ports += [pscustomobject]@{
            name = $d.Name; version = $version; licencia = $primera
        }
    }
}

# -- El documento ------------------------------------------------------------
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# NOTICE — dependencias de terceros")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERADO — no editar a mano.** ``pwsh tools/gen_notice.ps1``.")
[void]$sb.AppendLine("> Verificar con ``pwsh tools/gen_notice.ps1 -BuildDir build/<native-preset> -Check``; la CI de release debe ejecutarlo.")
[void]$sb.AppendLine("> Un NOTICE incompleto es tan inútil como ninguno — las licencias")
[void]$sb.AppendLine("> permisivas piden que el aviso viaje CON el binario.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Crates de Rust (grafo completo, incluidas las transitivas)")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| crate | versión | licencia |")
[void]$sb.AppendLine("|---|---|---|")
foreach ($c in $crates) {
    [void]$sb.AppendLine("| ``$($c.name)`` | $($c.version) | $($c.license) |")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Bibliotecas de C/C++ (vcpkg)")
[void]$sb.AppendLine()
if ($ports.Count) {
    [void]$sb.AppendLine("El texto completo de cada licencia viaja en")
    [void]$sb.AppendLine("``vcpkg_installed/<triplet>/share/<port>/copyright``.")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("| port | versión | licencia (primera línea del copyright) |")
    [void]$sb.AppendLine("|---|---|---|")
    foreach ($p in $ports) {
        $l = $p.licencia -replace '\|', '\|'
        [void]$sb.AppendLine("| ``$($p.name)`` | $($p.version) | $l |")
    }
} else {
    [void]$sb.AppendLine("_Sin ``vcpkg_installed``: esta sección no se derivó._")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Fuentes vendorizadas en el repositorio")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| componente | revisión | licencia | por qué está |")
[void]$sb.AppendLine("|---|---|---|---|")
[void]$sb.AppendLine("| ``third_party/ymfm`` | ``81aec25ccbb98f4873a255f7551ac4dadac59b4a`` | BSD-3-Clause | sintetizador FM del router de voces (#327). Es ymfm y NO el Nuked OPN2 del fork, que es LGPL-2.1: el motor es una lib ESTÁTICA y eso obligaría a distribución dinámica. |")
[void]$sb.AppendLine("| ``third_party/libvpx`` | tag ``v1.15.2`` | BSD-3-Clause + patent grant | decodificador de la Cinemática (#263). Es libvpx y NO FFmpeg por la misma frontera: el núcleo de FFmpeg es LGPL-2.1+. |")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Lo que NO se distribuye")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Cores de libretro (los aporta el usuario — BYOC), ROMs y BIOS (BYOR),")
[void]$sb.AppendLine("y packs derivados de juegos comerciales. El guardián")
[void]$sb.AppendLine("``sdk/tools/check_sdk_leak.cmake`` lo verifica sobre el artefacto publicado.")

$nuevo = $sb.ToString() -replace "`r`n", "`n"
$outPath = Join-Path $repo $Out

if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out no existe — corré el script sin -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    # Sin vcpkg_installed la sección de C/C++ no se derivó: compararla haría
    # fallar por algo que no se midió.
    if (-not $ports.Count) {
        $viejo = ($viejo -split "## Bibliotecas de C/C\+\+")[0]
        $nuevo = ($nuevo -split "## Bibliotecas de C/C\+\+")[0]
        Write-Host "  (sin vcpkg_installed: sólo se verifican los crates)"
    }
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nEl NOTICE quedó desactualizado." -ForegroundColor Red
        Write-Host "Regeneralo con: pwsh tools/gen_notice.ps1"
        exit 1
    }
    Write-Host "  [ OK ] el NOTICE coincide con las dependencias declaradas"
    exit 0
}

[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  escrito: $Out  ($($crates.Count) crates, $($ports.Count) ports)"
