# ---------------------------------------------------------------------------
# gen_api_reference.ps1 — la referencia de la API pública, generada (#553).
#
# POR QUÉ NO ES DOXYGEN (todavía). Doxygen produce una referencia mejor, pero
# exige una herramienta instalada en cada máquina que quiera regenerarla y en
# CI. Esto recorre la MISMA raíz que instala `cmake/AytherInstall.cmake` —todos
# los `.h` bajo `include/ayther/`— y no depende de nada: si un header entra o
# sale de la superficie, la referencia lo refleja sin mantener una lista
# paralela. El día que Doxygen esté en el CI, esto sigue sirviendo como índice.
#
# Lo que NO hace, dicho: no parsea C++ de verdad. Extrae la cabecera de cada
# header y sus declaraciones de primer nivel con sus comentarios. Alcanza para
# contestar «¿qué hay y para qué es?» y no para «¿cuál es la firma exacta de
# esta sobrecarga?» — para eso está el header, que viaja en el paquete.
#
# Uso:
#   pwsh tools/gen_api_reference.ps1           # regenera
#   pwsh tools/gen_api_reference.ps1 -Check    # CI: falla si quedó vieja
# ---------------------------------------------------------------------------
param(
    [switch]$Check,
    [string]$Out = "docs/PUBLIC_API_INDEX.md"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

# La instalación copia recursivamente todos los headers de esta raíz. Enumerar
# ese mismo árbol mantiene sincronizados paquete e índice sin parsear CMake.
$includeRoot = (Resolve-Path "include/ayther").Path
$headers = Get-ChildItem $includeRoot -Recurse -File -Filter "*.h" |
           ForEach-Object {
               [pscustomobject]@{
                   file = [System.IO.Path]::GetRelativePath(
                       $includeRoot, $_.FullName).Replace('\', '/')
                   nota = "Header público instalado."
               }
           } |
           Sort-Object file
if ($headers.Count -lt 2) { throw "la lista pública salió con $($headers.Count) headers" }

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# AYTHER Engine — índice de headers instalados")
[void]$sb.AppendLine()
[void]$sb.AppendLine("> **GENERADO — no editar a mano.** ``pwsh tools/gen_api_reference.ps1``.")
[void]$sb.AppendLine("> Sale de ``include/ayther/**/*.h``, la misma raíz que instala")
[void]$sb.AppendLine("> ``cmake/AytherInstall.cmake``: si un header entra")
[void]$sb.AppendLine("> o sale de la superficie, esta página lo refleja sin que nadie edite una")
[void]$sb.AppendLine("> lista paralela.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("La superficie instalada y su estabilidad se describen en")
[void]$sb.AppendLine("[``API_COMPATIBILITY.md``](API_COMPATIBILITY.md).")
[void]$sb.AppendLine("Aparecer en este índice no implica por sí solo una garantía de estabilidad.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Los $($headers.Count) headers")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| header | qué aporta |")
[void]$sb.AppendLine("|---|---|")
foreach ($h in $headers) {
    $anchor = "#" + ($h.file -replace '[^a-zA-Z0-9]', '-').ToLower()
    [void]$sb.AppendLine("| [``$($h.file)``]($anchor) | $($h.nota) |")
}
[void]$sb.AppendLine()

foreach ($h in $headers) {
    $ruta = Join-Path $repo "include/ayther/$($h.file)"
    if (-not (Test-Path $ruta)) { continue }
    $texto = Get-Content $ruta -Raw

    [void]$sb.AppendLine("---")
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("## $($h.file)")
    [void]$sb.AppendLine()

    # La cabecera del archivo: el bloque de comentarios de arriba de todo, que
    # es donde este repo explica POR QUÉ existe cada cosa. Se toma tal cual —
    # resumirla sería perder justamente lo que no está en otro lado.
    $cab = [regex]::Match($texto, '(?s)^(?:#\w+[^\n]*\n|/\* [-]+\n)?((?://[^\n]*\n| \*[^\n]*\n)+)')
    if ($cab.Success) {
        $lineas = $cab.Groups[1].Value -split "`n" | ForEach-Object {
            ($_ -replace '^\s*//\s?', '' -replace '^\s*\*\s?', '' -replace '^-+$', '').TrimEnd()
        }
        # Se corta en la primera línea vacía doble: el resto de la cabecera suele
        # ser detalle de implementación, y una referencia que copia todo no es
        # una referencia.
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

    # Declaraciones de primer nivel: tipos y funciones que el consumidor usa.
    $decls = [regex]::Matches($texto,
        '(?m)^(?:struct|class|enum(?:\s+class)?|typedef|inline|[A-Za-z_][A-Za-z0-9_:<>\*\s]*?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\(|\{|;)')
    $nombres = $decls | ForEach-Object { $_.Groups[1].Value } |
               Where-Object { $_ -notmatch '^(if|for|while|return|else|namespace)$' } |
               Select-Object -Unique | Sort-Object
    if ($nombres) {
        [void]$sb.AppendLine("**Declara:** " + (($nombres | ForEach-Object { "``$_``" }) -join ", "))
        [void]$sb.AppendLine()
    }
    [void]$sb.AppendLine("_El header instalado (``include/ayther/$($h.file)``) lleva la documentación completa de cada símbolo._")
    [void]$sb.AppendLine()
}

$nuevo = $sb.ToString() -replace "`r`n", "`n"
$outPath = Join-Path $repo $Out

if ($Check) {
    if (-not (Test-Path $outPath)) { throw "$Out no existe — corré el script sin -Check" }
    $viejo = (Get-Content $outPath -Raw) -replace "`r`n", "`n"
    if ($viejo.TrimEnd() -ne $nuevo.TrimEnd()) {
        Write-Host "`nLa referencia quedó desactualizada." -ForegroundColor Red
        Write-Host "Regeneralo con: pwsh tools/gen_api_reference.ps1"
        exit 1
    }
    Write-Host "  [ OK ] la referencia coincide con la superficie declarada"
    exit 0
}

[System.IO.Directory]::CreateDirectory(
    [System.IO.Path]::GetDirectoryName($outPath)) | Out-Null
[System.IO.File]::WriteAllText($outPath, $nuevo)
Write-Host "  escrito: $Out  ($($headers.Count) headers)"
