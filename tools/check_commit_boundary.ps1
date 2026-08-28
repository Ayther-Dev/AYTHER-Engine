# ---------------------------------------------------------------------------
# check_commit_boundary.ps1 — un commit no toca `lab/` y código FOSS a la vez
# (#555 · recaudo F0.1 de ADR-004, especificación E1).
#
# POR QUÉ, Y POR QUÉ AHORA. Cuando se extraiga el repositorio público,
# `git filter-repo` elimina los PATHS de `lab/` — pero **no reescribe los
# mensajes de commit**. Y esos mensajes describen features cerradas con
# bastante detalle: workspaces, timeline, el horneado del pack, el modelo de
# datos del Lab. Cada commit mezclado que se agrega hoy es un mensaje más para
# revisar a mano antes del primer push público, y al 2026-08-24 ya había 171.
#
# Separarlos es gratis mientras se escriben y caro después: por eso esto corre
# desde ya y no el día de la migración. El recaudo se DEGRADA con cada commit
# nuevo, que es lo que lo distingue del resto del plan.
#
# QUÉ NO ES. No es el guardián de la frontera de código —ése es
# `check_repo_boundary.cmake`, y mira el árbol— sino el de la frontera del
# HISTORIAL. Son problemas distintos: se puede tener un árbol perfectamente
# separado y un historial imposible de publicar.
#
# LA REGLA. Si un cambio necesita las dos mitades, son dos commits: primero el
# del motor con su oráculo, después el del Lab que lo consume. Es además el
# orden en que conviene pensarlos.
#
# LA MARCA DE ESCAPE. Un rename o un movimiento masivo puede tener que cruzar
# de verdad —la mudanza de los oráculos del Lab (#568) es exactamente eso—. Se
# permite con `[cross-boundary]` en el mensaje. No es una puerta trasera: queda
# LISTADO en el reporte, así que la revisión de mensajes de E1.2 sabe dónde
# mirar en vez de leer 171 commits.
#
# POR QUE NO ESTA EN CTEST, al reves que su hermano del arbol. Porque depende
# de un RANGO de commits, y en un build local no hay un rango definido: contra
# `main` no dice nada util si ya se mergeo, y en un clon superficial de CI la
# base directamente no existe. Un test que no puede elegir bien su rango pasa en
# verde sobre lo que no miro. El rango correcto lo sabe el evento del PR, asi
# que ahi es donde corre.
#
# Uso:
#   pwsh tools/check_commit_boundary.ps1                    # commits vs. main
#   pwsh tools/check_commit_boundary.ps1 -Range A..B        # un rango explícito
#   pwsh tools/check_commit_boundary.ps1 -Base <sha> -Head <sha>
#   pwsh tools/check_commit_boundary.ps1 -Staged -MessageFile <f>   # el hook
# ---------------------------------------------------------------------------
param(
    [string]$Range,
    [string]$Base,
    [string]$Head = "HEAD",
    # El chequeo del índice, para el hook `commit-msg`: mira lo que está por
    # commitearse en vez de lo ya commiteado.
    [switch]$Staged,
    # El archivo con el mensaje que git le pasa al hook. Va con `-Staged`, y es
    # lo que permite que el hook honre `[cross-boundary]` igual que el CI: en
    # `pre-commit` el mensaje todavía no existe, y un hook que no puede leer la
    # marca obliga a `--no-verify` — que apaga TODOS los hooks, no éste.
    [string]$MessageFile
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path
Set-Location $repo

# El árbol cerrado, y el árbol que ADR-004 §F2 manda al repositorio público.
# `docs/`, la raíz y `.github/` quedan afuera a propósito: un commit del Lab que
# además registra su subdirectorio en el `CMakeLists.txt` de la raíz no es el
# problema que esto ataja, y prohibirlo convertiría la regla en una que se
# desactiva.
$cerrado = @('lab/')
$foss    = @('core/', 'engine/', 'runtime/', 'play/', 'tools/', 'tests/', 'sdk/')

function Lado($archivos) {
    $c = @($archivos | Where-Object { $a = $_; $cerrado | Where-Object { $a.StartsWith($_) } })
    $f = @($archivos | Where-Object { $a = $_; $foss    | Where-Object { $a.StartsWith($_) } })
    return @{ cerrado = $c; foss = $f }
}

# --------------------------------------------------------------------------
# Modo hook: el índice, antes de que el commit exista.
# --------------------------------------------------------------------------
if ($Staged) {
    $archivos = @(git diff --cached --name-only | Where-Object { $_ })
    $l = Lado $archivos
    if ($l.cerrado.Count -gt 0 -and $l.foss.Count -gt 0) {
        if ($MessageFile -and (Test-Path $MessageFile)) {
            $msg = Get-Content $MessageFile -Raw
            if ($msg -match '\[cross-boundary\]') {
                Write-Host "  [ ok ] cruce declarado con [cross-boundary]: $($l.cerrado.Count) archivo(s) de lab/ + $($l.foss.Count) de FOSS"
                Write-Host "         queda listado para la revision de mensajes de ADR-004 E1.2"
                exit 0
            }
        }
        Write-Host ""
        Write-Host "Este commit mezcla el Lab (cerrado) con codigo FOSS." -ForegroundColor Red
        Write-Host "  lab/  : $(($l.cerrado | Select-Object -First 4) -join ', ')"
        Write-Host "  FOSS  : $(($l.foss    | Select-Object -First 4) -join ', ')"
        Write-Host ""
        Write-Host "Son dos commits: primero el del motor (con su oraculo), despues el"
        Write-Host "del Lab que lo consume. Para partirlo:"
        Write-Host "    git reset                            # desarma el indice"
        Write-Host "    git add <los del motor>  ; git commit"
        Write-Host "    git add <los del Lab>    ; git commit"
        Write-Host ""
        Write-Host "Si el cambio TIENE que cruzar (un rename masivo, una mudanza de"
        Write-Host "directorios), pone [cross-boundary] en el mensaje: se permite aca y en"
        Write-Host "CI, y queda listado para la revision de mensajes de la migracion"
        Write-Host "(ADR-004 E1.2). El mensaje ya escrito se recupera de .git/COMMIT_EDITMSG."
        exit 1
    }
    exit 0
}

# --------------------------------------------------------------------------
# Modo rango: los commits de un PR.
# --------------------------------------------------------------------------
if (-not $Range) {
    if ($Base) {
        $Range = "$Base..$Head"
    } else {
        # Sin base explícita: contra `main` si existe, y si no contra su forma
        # remota. Se FALLA si no hay ninguna en vez de inventar un rango: un
        # chequeo que elige mal el rango pasa en verde sobre lo que no miró.
        $b = @('origin/main', 'main') | Where-Object {
            git rev-parse --verify --quiet "$_" 2>$null
            $LASTEXITCODE -eq 0
        } | Select-Object -First 1
        if (-not $b) {
            throw "no se pudo determinar la base (ni origin/main ni main). Pasa -Range o -Base."
        }
        $Range = "$b..$Head"
    }
}

$commits = @(git rev-list --no-merges $Range 2>$null)
if ($LASTEXITCODE -ne 0) {
    throw ("el rango '$Range' no se pudo recorrer. En CI suele ser fetch-depth: " +
           "sin la historia completa, git rev-list no ve la base.")
}

# NO-VACUIDAD. Un rango vacío y un rango que no se pudo resolver se ven igual
# desde afuera: los dos reportan «ninguna violación». Se dice cuál de los dos
# fue, y no se canta victoria sobre cero commits.
if ($commits.Count -eq 0) {
    Write-Host "  [ -- ] no hay commits propios en '$Range': nada que revisar"
    exit 0
}

$mezclados = @()
$cruces    = @()

foreach ($sha in $commits) {
    $archivos = @(git show --pretty=format: --name-only $sha | Where-Object { $_ })
    $l = Lado $archivos
    if ($l.cerrado.Count -eq 0 -or $l.foss.Count -eq 0) { continue }

    $msg   = (git show -s --format=%B $sha) -join "`n"
    $corto = git show -s --format="%h %s" $sha
    $item  = [pscustomobject]@{ sha = $corto; lab = $l.cerrado; foss = $l.foss }
    if ($msg -match '\[cross-boundary\]') { $cruces += $item } else { $mezclados += $item }
}

# Las excepciones se REPORTAN aunque todo pase: una marca de escape que no deja
# rastro visible es una marca que se vuelve costumbre.
if ($cruces.Count -gt 0) {
    Write-Host ""
    Write-Host "Cruces declarados con [cross-boundary] ($($cruces.Count)):" -ForegroundColor Yellow
    foreach ($c in $cruces) {
        Write-Host "  $($c.sha)"
        Write-Host "      lab/: $($c.lab.Count) archivo(s) - FOSS: $($c.foss.Count) archivo(s)"
    }
    Write-Host "  (quedan para la revision de mensajes de ADR-004 E1.2)"
}

if ($mezclados.Count -gt 0) {
    Write-Host ""
    Write-Host "$($mezclados.Count) commit(s) mezclan el Lab (cerrado) con codigo FOSS:" -ForegroundColor Red
    foreach ($m in $mezclados) {
        $masLab  = if ($m.lab.Count  -gt 4) { " (+$($m.lab.Count  - 4))" } else { "" }
        $masFoss = if ($m.foss.Count -gt 4) { " (+$($m.foss.Count - 4))" } else { "" }
        Write-Host ""
        Write-Host "  $($m.sha)"
        Write-Host "      lab/: $(($m.lab  | Select-Object -First 4) -join ', ')$masLab"
        Write-Host "      FOSS: $(($m.foss | Select-Object -First 4) -join ', ')$masFoss"
    }
    Write-Host ""
    Write-Host "POR QUE IMPORTA. git filter-repo borra los PATHS de lab/ al extraer el"
    Write-Host "repositorio publico, pero NO reescribe los mensajes de commit — y esos"
    Write-Host "mensajes describen features cerradas. Cada commit mezclado es uno mas para"
    Write-Host "revisar a mano antes del primer push publico (ADR-004 E1)."
    Write-Host ""
    Write-Host "COMO PARTIRLO. Son dos commits: primero el del motor con su oraculo,"
    Write-Host "despues el del Lab que lo consume."
    Write-Host "    git rebase -i <base>                 # marcar 'edit' el commit mezclado"
    Write-Host "    git reset HEAD^"
    Write-Host "    git add <los del motor>  ; git commit"
    Write-Host "    git add <los del Lab>    ; git commit"
    Write-Host "    git rebase --continue"
    Write-Host ""
    Write-Host "SI TIENE QUE CRUZAR de verdad (un rename masivo, una mudanza de"
    Write-Host "directorios): [cross-boundary] en el mensaje. Se permite y queda listado"
    Write-Host "arriba para la revision de la migracion."
    exit 1
}

Write-Host "  [ OK ] frontera del historial: $($commits.Count) commit(s) en '$Range', ninguno mezcla lab/ con FOSS"
