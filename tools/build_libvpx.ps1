# ---------------------------------------------------------------------------
# build_libvpx.ps1 — construye libvpx (decoder VP9) y lo deja consumible
# desde el build del proyecto. Ver #263.
#
# POR QUÉ EXISTE ESTE SCRIPT Y NO UN `vcpkg install`: el port de vcpkg falla en
# esta máquina por una causa que no se aisló (probado y descartado: nasm —vcpkg
# lo baja solo—, el WindowsTargetPlatformVersion del vcxproj, y una variable de
# entorno que rompía vcvars). El resto de las dependencias nativas sí usa el
# manifiesto de vcpkg; libvpx se mantiene fuera de él para aislar ese fallo.
#
# POR QUÉ NO SE COMMITEA EL .lib: el repo no lleva binarios de terceros
# (third_party/cores/ está gitignoreado y el core se construye igual, ver
# README). Se mantiene ese precedente.
#
# SALIDA:  third_party/libvpx/include/vpx/*.h  +  third_party/libvpx/lib/vpxmd.lib
#          + VERSION/LICENSE/PATENTS/AUTHORS para empaquetado reproducible.
# El CMake del proyecto lo detecta ahí solo; sin eso, el video queda apagado y
# el resto compila igual.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    # Tag de libvpx. Fijo a propósito: un build reproducible no sigue a master.
    [string] $Tag = 'v1.15.2',
    # Rehacer desde cero (borra el árbol de build, no el clon).
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root      = Split-Path -Parent $PSScriptRoot
$srcDir    = Join-Path $root 'third_party\src\libvpx'
$buildDir  = Join-Path $root 'third_party\build-libvpx'
$outDir    = Join-Path $root 'third_party\libvpx'

function Need($what, $hint) { Write-Error "$what`n  -> $hint" }

# --- Dependencias ----------------------------------------------------------
# nasm: sin el SIMD, el decode de VP9 a 1080p en C puro no entra en el
# presupuesto de frame. No es opcional.
$nasm = (Get-Command nasm -ErrorAction SilentlyContinue).Source
if (-not $nasm) {
    $cand = 'C:\Users\{0}\AppData\Local\bin\NASM\nasm.exe' -f $env:USERNAME
    if (Test-Path $cand) { $nasm = $cand }
}
if (-not $nasm) { Need 'Falta nasm.' 'winget install --id NASM.NASM' }
$env:AS = $nasm -replace '\\','/'

# make: TIENE que ser uno que no convierta a ruta Windows el programa de la
# receta antes de pasárselo a bash. El de mingw-mstorsjo (que suele ganar el
# PATH) SÍ lo hace, y libvpx falla con
#   /bin/bash: C:Usersdavid...gen_msvs_def.sh: No such file or directory
# — las barras invertidas comidas. El de scoop `make` funciona. Este orden de
# búsqueda es deliberado; no ponerlo a resolver por PATH.
$makeCandidates = @(
    ('C:\Users\{0}\scoop\apps\make\current\bin\make.exe' -f $env:USERNAME),
    'C:\msys64\usr\bin\make.exe'
)
$make = $makeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $make) { Need 'Falta un make usable.' 'scoop install make' }

$bashCandidates = @(
    (Join-Path $env:ProgramW6432 'Git\bin\bash.exe'),
    (Join-Path $env:ProgramW6432 'Git\usr\bin\bash.exe'),
    (Join-Path $env:ProgramFiles 'Git\bin\bash.exe'),
    (Join-Path $env:ProgramFiles 'Git\usr\bin\bash.exe'),
    (Join-Path $env:USERPROFILE 'scoop\apps\git\current\bin\bash.exe')
)
$bash = $bashCandidates |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $bash) {
    $pathBash = (Get-Command bash -ErrorAction SilentlyContinue).Source
    # System32\bash.exe is the WSL launcher. The commands below intentionally
    # use Git-Bash paths such as C:/work, which WSL cannot resolve.
    if ($pathBash -and
        -not $pathBash.Equals(
            (Join-Path $env:SystemRoot 'System32\bash.exe'),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        $bash = $pathBash
    }
}
if (-not $bash) { Need 'Falta bash (el configure de libvpx es un script sh).' 'Instalar Git for Windows' }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Need 'Falta Visual Studio.' 'Se necesita VS 2022 (Build Tools alcanza)' }
$vsPath  = & $vswhere -latest -products * -property installationPath
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { Need "No hay MSBuild en $vsPath." 'Instalar el workload de C++ desktop' }

Write-Host "nasm    : $nasm"
Write-Host "make    : $make"
Write-Host "msbuild : $msbuild"

# --- Fuente ----------------------------------------------------------------
if (-not (Test-Path $srcDir)) {
    Write-Host "`nClonando libvpx $Tag ..."
    New-Item -ItemType Directory -Force (Split-Path -Parent $srcDir) | Out-Null
    git clone --depth 1 --branch $Tag https://chromium.googlesource.com/webm/libvpx $srcDir
    if ($LASTEXITCODE -ne 0) { Need 'Falló el clone de libvpx.' 'Revisar la red o el tag' }
}

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
New-Item -ItemType Directory -Force $buildDir | Out-Null

# --- Configure -------------------------------------------------------------
# Sólo el decoder de VP9. Sacar el encoder y VP8 lleva el .lib de ~29 MB a ~6.
# webm-io fuera: el demuxer IVF es nuestro (32 bytes de header + 12 por frame).
if (-not (Test-Path (Join-Path $buildDir 'config.mk'))) {
    Write-Host "`nConfigurando ..."
    $cfgArgs = @(
        '--target=x86_64-win64-vs17'
        '--disable-vp8'; '--disable-vp9-encoder'; '--enable-vp9-decoder'
        '--disable-examples'; '--disable-tools'; '--disable-docs'; '--disable-unit-tests'
        '--disable-webm-io'; '--enable-static'; '--disable-shared'
    ) -join ' '
    Push-Location $buildDir
    & $bash -lc "cd '$($buildDir -replace '\\','/')' && ../src/libvpx/configure $cfgArgs"
    Pop-Location
    if ($LASTEXITCODE -ne 0) { Need 'Falló el configure de libvpx.' 'Ver config.log en third_party/build-libvpx' }
}

# --- Generar los proyectos MSVC -------------------------------------------
Write-Host "`nGenerando la solución MSVC ..."
$buildDirSh = $buildDir -replace '\\','/'
$makeSh = $make -replace '\\','/'
& $bash -lc "cd '$buildDirSh' && '$makeSh' NO_LAUNCH_DEVENV=1"
if ($LASTEXITCODE -ne 0) {
    Need 'Falló la generación de vpx.vcxproj.' 'Revisar la salida de make; si el checkout se movió, usar -Clean'
}
if (-not (Test-Path (Join-Path $buildDir 'vpx.vcxproj'))) {
    Need 'No se generó vpx.vcxproj.' 'Suele ser el make equivocado — ver la nota de arriba'
}

# --- Compilar --------------------------------------------------------------
# WholeProgramOptimization=false NO ES OPCIONAL. El vcxproj que genera el
# configure pone /GL (LTCG) en Release, y eso emite objetos IL de MSVC en vez
# de COFF nativo. El proyecto linkea con lld-link, que no los lee:
#   lld-link: error: vpx_src_vpx_decoder.obj: is not a native COFF file
# Se descubrió linkeando de verdad; dumpbin sobre el .lib no lo delata.
Write-Host "`nCompilando (sin LTCG — lld-link no lee objetos /GL) ..."
$pathKeys = @([Environment]::GetEnvironmentVariables().Keys |
    ForEach-Object { $_.ToString() } |
    Where-Object { $_ -imatch '^path$' })
$pathPrefix = if ($pathKeys -ccontains 'PATH' -and
                  $pathKeys -ccontains 'Path') { 'set PATH=& ' } else { '' }
$msbuildCommand = $pathPrefix +
    '"' + $msbuild + '" "' + (Join-Path $buildDir 'vpx.vcxproj') +
    '" /t:Rebuild /p:Configuration=Release /p:Platform=x64' +
    ' /p:WholeProgramOptimization=false /m /v:minimal'
& $env:ComSpec /d /c $msbuildCommand
if ($LASTEXITCODE -ne 0) { Need 'Falló la compilación de libvpx.' 'Ver la salida de MSBuild' }

$lib = Join-Path $buildDir 'x64\Release\vpxmd.lib'
if (-not (Test-Path $lib)) { Need "No se produjo $lib." 'Revisar la salida de MSBuild' }

# --- Instalar --------------------------------------------------------------
# vpxmd = /MD, que es lo que CMake usa por defecto en Release. Si alguna vez el
# proyecto pasa a /MT hay que rehacer esto con --enable-static-msvcrt.
New-Item -ItemType Directory -Force (Join-Path $outDir 'include\vpx') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $outDir 'lib') | Out-Null
@('vpx_codec.h','vpx_decoder.h','vpx_image.h','vpx_integer.h',
  'vpx_frame_buffer.h','vp8.h','vp8dx.h') | ForEach-Object {
    Copy-Item (Join-Path $srcDir "vpx\$_") (Join-Path $outDir 'include\vpx') -Force
}
Copy-Item $lib (Join-Path $outDir 'lib') -Force
@('LICENSE','PATENTS','AUTHORS') | ForEach-Object {
    $notice = Join-Path $srcDir $_
    if (Test-Path $notice) { Copy-Item $notice $outDir -Force }
}
Set-Content -LiteralPath (Join-Path $outDir 'VERSION') -Value $Tag -Encoding utf8NoBOM

$mb = [math]::Round((Get-Item (Join-Path $outDir 'lib\vpxmd.lib')).Length / 1MB, 1)
Write-Host "`nListo: $outDir  (vpxmd.lib $mb MB)"
Write-Host 'Reconfigurar el build para que CMake lo detecte:'
Write-Host '  cmake -S . -B build'
