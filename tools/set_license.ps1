# ---------------------------------------------------------------------------
# set_license.ps1 — aplicar la licencia elegida en TODOS los lugares (#532).
#
# La decisión de qué licencia usar es de producto y es de David. Lo que no tiene
# por qué ser trabajo manual es APLICARLA: son cinco lugares que tienen que
# decir lo mismo, y cinco lugares editados a mano es una forma probada de que
# uno quede distinto — y con licencias, uno distinto es una contradicción legal,
# no una errata.
#
#   1. `LICENSE` en la raíz (el texto completo);
#   2. `license = "<SPDX>"` en el workspace de Cargo — sin él `cargo publish`
#      se niega, que es el bloqueo que hoy tenemos puesto a propósito;
#   3. la matriz de `docs/architecture/ecosystem.md` §1.1;
#   4. `lab/LICENSE` — propietario, para que un archivo suelto del Lab que se
#      escape del repo no viaje sin decir qué es;
#   5. el README del SDK.
#
# El TEXTO no se genera acá ni se guarda en el repo por adelantado: se toma del
# archivo que se le pasa (o del canónico de SPDX). Reproducir de memoria un
# texto legal es peor que no tenerlo — un carácter cambiado en una licencia es
# una licencia distinta.
#
# Uso:
#   pwsh tools/set_license.ps1 -Id MPL-2.0 -TextFile C:\ruta\MPL-2.0.txt
#   pwsh tools/set_license.ps1 -Id MIT -TextFile ... -Holder "David Lazarte"
#
# El texto canónico de cada licencia: https://spdx.org/licenses/<ID>.html
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory)][string]$Id,
    [Parameter(Mandatory)][string]$TextFile,
    [string]$Holder = "David Lazarte",
    [string]$Year   = "2026"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

if (-not (Test-Path $TextFile)) { throw "no existe el texto de la licencia: $TextFile" }
$texto = Get-Content $TextFile -Raw
if ($texto.Trim().Length -lt 200) {
    # Control de no vacuidad: un archivo vacío o truncado dejaría un LICENSE
    # que parece puesto y no dice nada, que es el peor de los dos estados.
    throw "el texto de la licencia parece truncado ($($texto.Length) caracteres)"
}
$texto = $texto -replace '\[yyyy\]|<year>|\{yyyy\}', $Year
$texto = $texto -replace '\[name of copyright owner\]|<copyright holders>|\{name of copyright owner\}', $Holder

# 1. LICENSE en la raíz.
[System.IO.File]::WriteAllText((Join-Path $repo "LICENSE"), $texto)
Write-Host "  LICENSE            → $Id"

# 2. El workspace de Cargo.
$cargo = Get-Content "Cargo.toml" -Raw
if ($cargo -match '(?m)^license\s*=') {
    $cargo = $cargo -replace '(?m)^license\s*=.*$', "license = `"$Id`""
} else {
    $cargo = $cargo -replace '(?m)^(repository = .*)$', "`$1`nlicense    = `"$Id`""
}
# Y sacar la nota que decía que faltaba: un comentario que describe un estado
# que ya cambió es una mentira con fecha.
$cargo = $cargo -replace '(?ms)^# #537: compartido por los crates publicables.*?correcto, no un olvido\.\r?\n', ''
[System.IO.File]::WriteAllText((Join-Path $repo "Cargo.toml"), $cargo)
Write-Host "  Cargo.toml         → license = `"$Id`""

# 3. La matriz de licencias.
$eco = Get-Content "docs/architecture/ecosystem.md" -Raw
$eco = $eco -replace '(?ms)\*\*Lo que falta y bloquea publicar\*\* \(#532\).*?nadie puede integrar legalmente el motor\.',
                     "**Licencia FOSS del motor: ``$Id``** (#532). El texto completo está en ``LICENSE``; el régimen ``FOSS`` de la tabla de arriba se refiere a esa licencia."
[System.IO.File]::WriteAllText((Join-Path $repo "docs/architecture/ecosystem.md"), $eco)
Write-Host "  ecosystem.md §1.1  → $Id"

# 4. El Lab, que NO es FOSS. Un archivo suelto que se escape del repo tiene que
#    decir qué es sin depender de que alguien mire la matriz.
$labLic = @"
AYTHER Lab — software propietario
Copyright (c) $Year $Holder. Todos los derechos reservados.

Este directorio (``lab/``) NO está cubierto por la licencia ``$Id`` del archivo
LICENSE de la raíz, que alcanza al motor y a las herramientas abiertas
(``core/`` ``engine/`` ``runtime/`` ``play/`` ``tools/`` ``sdk/`` ``tests/``).

Ninguna parte de este código puede copiarse, modificarse, redistribuirse ni
usarse para obras derivadas sin autorización escrita del titular.

La frontera está declarada en docs/architecture/ecosystem.md §1.1 y la verifica
sdk/tools/check_sdk_leak.cmake sobre cada artefacto publicado.
"@
[System.IO.File]::WriteAllText((Join-Path $repo "lab/LICENSE"), $labLic)
Write-Host "  lab/LICENSE        → propietario"

# 5. El README del SDK.
$sdk = Get-Content "sdk/README.md" -Raw
if ($sdk -notmatch '(?m)^## License') {
    $sdk += @"

## License

The engine and the open tooling (``core/`` ``engine/`` ``runtime/`` ``play/``
``tools/`` ``sdk/`` ``tests/``) are licensed under **$Id** — see
[``LICENSE``](../LICENSE). ``lab/`` is proprietary and is never part of a
published artifact; the boundary is enforced by ``sdk/tools/check_sdk_leak.cmake``.

Third-party notices: [``NOTICE.md``](../NOTICE.md), generated and verified in CI.
"@
    [System.IO.File]::WriteAllText((Join-Path $repo "sdk/README.md"), $sdk)
    Write-Host "  sdk/README.md      → sección License"
}

Write-Host "`nListo. Verificá con: ctest -R sdk_license" -ForegroundColor Green
