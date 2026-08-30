# ---------------------------------------------------------------------------
# check_repo_boundary.cmake — las fronteras de ADR-004, verificadas (#566, E8).
#
# QUÉ PROTEGE. ADR-004 (rev. 2026-08-26) manda un repositorio por componente:
# `core/`+`src/`+`include/` (el motor), `runtime/`, `sdk/`, `play/`, `hub/` y `lab/`,
# cada uno con sus tests, tools y docs adentro. El corte se va a hacer con
# `git filter-repo`, que es una operación de RUTAS: se lleva directorios enteros
# y no entiende de referencias.
#
# El defecto que existe para atrapar es el que ya nos pasó dos veces: un oráculo
# en `tests/` que compilaba `lab/src/app/pack_bake.cpp` (E7), y el renderer del
# motor cargando sus shaders desde `runtime/src/vulkan_backend/shaders/` (E8).
# Adentro del monorepo eso compila perfecto y nadie se entera. El día del corte
# hay exactamente dos salidas, las dos malas: deja de compilar, o hay que
# llevarse código ajeno al otro repo para que compile. Por eso esto corre en
# ctest todos los días y no como checklist del día de la migración — una
# frontera que se verifica una sola vez ya está rota antes de esa vez.
#
# ES UNA ALLOWLIST AL REVÉS: no enumera lo prohibido (una lista negra falla por
# omisión, que es exactamente cómo se cuela el caso nuevo), sino que declara que
# cada lado **no menciona el árbol del otro en absoluto**, y lista aparte las
# excepciones legítimas, cada una con su motivo. Si mañana aparece una
# referencia nueva, falla; agregarla acá exige escribir por qué.
#
# LA DIRECCIÓN IMPORTA. Un consumidor puede alcanzar la superficie PÚBLICA de su
# proveedor (`include/`, el target `ayther_engine`, el crate por `path` +
# `version`): eso es lo que el paquete publicado va a darle después. Lo que no
# puede pasar es (a) que un proveedor conozca a sus consumidores —el motor no
# sabe que existen runtime, sdk, play ni hub— ni (b) que un consumidor entre al
# árbol privado del proveedor (`src/`, `core/src`) o al de un hermano.
#
# Uso:
#   cmake -DAYTHER_ROOT=<repo> -P tools/check_repo_boundary.cmake
# ---------------------------------------------------------------------------
# `IN_LIST` necesita la politica CMP0057 en NEW, y en modo script (`cmake -P`)
# las politicas arrancan en su valor por defecto: sin esto, CMake dice
# «Unknown arguments specified» y el guardian falla por sintaxis en vez de
# revisar nada. Local pasaba (CMake 4.x) y en el runner de CI no — el peor modo
# de fallar para un guardian, porque el verde de la maquina del que lo escribio
# no dice nada del de CI.
cmake_minimum_required(VERSION 3.21)
if(NOT AYTHER_ROOT)
    message(FATAL_ERROR "falta -DAYTHER_ROOT=<raíz del repositorio>")
endif()

# ---------------------------------------------------------------------------
# Las fronteras. Cada una: qué directorios se revisan y qué NO pueden mencionar.
# Se buscan las formas en que un archivo puede alcanzar otro árbol: incluirlo
# (`../x/`), compilarlo (`x/src`), o construirlo (el target del otro lado).
# Los patrones son deliberadamente de RUTA/TARGET y no de palabra: «play/»
# solo matchearía «display/», y «hub/» a «github/».
# ---------------------------------------------------------------------------
set(_n_fronteras 4)

# 1. Nada del lado FOSS alcanza el árbol cerrado del Lab (E7). `lab/` → FOSS es
#    legítimo y esperado: el Lab consume el motor.
set(_f1_nombre "FOSS → lab/ (E7)")
set(_f1_dirs   core src include runtime tools tests sdk)
set(_f1_pat    "lab/src" "lab/include" "[.][.]/lab/" "ayther_lab")

# 2. El motor no conoce a sus consumidores (E8). `tools/` y `tests/` de la raíz
#    son del motor: si algo de ahí necesita runtime o sdk, es de runtime o sdk.
set(_f2_nombre "motor → runtime/ sdk/ play/ hub/ (E8)")
set(_f2_dirs   core src include tools tests)
set(_f2_pat    "runtime/src" "[.][.]/runtime/" "ayther_runtime"
               "sdk/tools" "sdk/tests" "sdk/examples" "sdk/fixtures" "[.][.]/sdk/"
               "play/src" "[.][.]/play/"
               "hub/api" "[.][.]/hub/")

# 3. El runtime consume el motor por su superficie pública, no por su árbol
#    privado; y no conoce a sus hermanos.
set(_f3_nombre "runtime → ../src core/src sdk/ play/ hub/ (E8)")
set(_f3_dirs   runtime)
set(_f3_pat    "[.][.]/src/" "core/src" "[.][.]/core/"
               "sdk/tools" "sdk/tests" "sdk/examples" "[.][.]/sdk/"
               "play/src" "[.][.]/play/" "hub/api" "[.][.]/hub/")

# 4. El SDK, igual: `include/` y el crate por `path`+`version` sí (es lo que el
#    paquete publicado le va a dar); `../src/` y `core/src`, no.
set(_f4_nombre "sdk → ../src core/src runtime/ play/ hub/ (E8)")
set(_f4_dirs   sdk)
set(_f4_pat    "[.][.]/src/" "core/src"
               "runtime/src" "[.][.]/runtime/" "ayther_runtime"
               "play/src" "[.][.]/play/" "hub/api" "[.][.]/hub/")

# Excepciones, con motivo. Rutas relativas a la raíz. Una excepción sin motivo
# escrito es una que nadie revisó.
set(_permitido
    # Los guardianes NOMBRAN lo que prohíben: es su trabajo, y uno que se
    # denuncia a sí mismo enseña a desactivarlo.
    tools/check_repo_boundary.cmake
    tools/check_commit_boundary.ps1
    sdk/tools/check_sdk_leak.cmake
    # El grafo de dependencias dibuja TODOS los targets del monorepo: nombrarlos
    # es exactamente lo que hace. No compila ni enlaza. Se copia por repo (§3).
    tools/dep_graph.ps1
    # El NOTICE y la sección de licencia (ecosystem.md §1.1, sdk/README.md)
    # nombran al guardián del artefacto del SDK en su texto: es una oración,
    # no una dependencia. Ambos scripts se copian por repo (§3).
    tools/gen_notice.ps1
    tools/set_license.ps1
)

set(_hallazgos "")
set(_revisados 0)

foreach(_i RANGE 1 ${_n_fronteras})
    set(_nombre "${_f${_i}_nombre}")
    foreach(_dir IN LISTS _f${_i}_dirs)
        if(NOT IS_DIRECTORY "${AYTHER_ROOT}/${_dir}")
            continue()
        endif()
        file(GLOB_RECURSE _archivos
             "${AYTHER_ROOT}/${_dir}/*.cmake"
             "${AYTHER_ROOT}/${_dir}/*.txt"
             "${AYTHER_ROOT}/${_dir}/*.c"
             "${AYTHER_ROOT}/${_dir}/*.h"
             "${AYTHER_ROOT}/${_dir}/*.cpp"
             "${AYTHER_ROOT}/${_dir}/*.hpp"
             "${AYTHER_ROOT}/${_dir}/*.rs"
             "${AYTHER_ROOT}/${_dir}/*.ps1")
        foreach(_f IN LISTS _archivos)
            file(RELATIVE_PATH _rel "${AYTHER_ROOT}" "${_f}")
            if(_rel IN_LIST _permitido)
                continue()
            endif()
            # Los `target/` de Cargo y los build dirs no son fuentes.
            if(_rel MATCHES "(^|/)(target|build[^/]*)/")
                continue()
            endif()
            math(EXPR _revisados "${_revisados} + 1")
            # Partir el archivo en lineas sin que la sintaxis de listas de CMake
            # interprete el codigo. Ademas del `;` separador, corchetes no
            # balanceados impiden que CMake corte una lista: un indice `a[i]`
            # puede fusionar miles de lineas y convertir un comentario posterior
            # en un falso hallazgo. Las barras pueden escapar separadores. Se
            # protegen los cuatro caracteres y se restauran solo al informar.
            file(READ "${_f}" _texto)
            string(ASCII 28 _prot_backslash)
            string(ASCII 29 _prot_lbracket)
            string(ASCII 30 _prot_rbracket)
            string(ASCII 31 _prot_semicolon)
            string(REPLACE "\\" "${_prot_backslash}" _texto "${_texto}")
            string(REPLACE "[" "${_prot_lbracket}" _texto "${_texto}")
            string(REPLACE "]" "${_prot_rbracket}" _texto "${_texto}")
            string(REPLACE ";" "${_prot_semicolon}" _texto "${_texto}")
            string(REGEX REPLACE "\r" "" _texto "${_texto}")
            string(REPLACE "\n" ";" _texto "${_texto}")
            foreach(_l IN LISTS _texto)
                # Se matchea y se guarda con los caracteres de lista protegidos.
                string(STRIP "${_l}" _l)
                # Los comentarios no compilan. Nombrar otro componente en una
                # explicacion no es depender de el, y un guardian que no distingue
                # eso obliga a escribir codigo sin comentarios para que pase.
                if(_l MATCHES "^(#|//|[*]|/[*])")
                    continue()
                endif()
                foreach(_p IN LISTS _f${_i}_pat)
                    if(_l MATCHES "${_p}")
                        list(APPEND _hallazgos "[${_nombre}] ${_rel}: ${_l}")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endforeach()

# NO-VACUIDAD. Un guardián que no leyó nada pasa siempre, y un glob que dejó de
# encontrar archivos (por un rename de directorio, por correr desde otro cwd)
# se ve idéntico a un árbol limpio. El umbral es holgado a propósito: sólo
# detecta el caso «no leí nada», no fija un tamaño del repo.
if(_revisados LESS 100)
    message(FATAL_ERROR
        "el guardián de frontera revisó sólo ${_revisados} archivos: el glob no "
        "encontró el árbol (¿AYTHER_ROOT mal? ¿se renombró un directorio?). "
        "Se falla en vez de reportar verde sobre la nada.")
endif()

if(_hallazgos)
    list(REMOVE_DUPLICATES _hallazgos)
    list(LENGTH _hallazgos _n)
    message("")
    message("Referencias que cruzan una frontera de repositorio (${_n}):")
    foreach(_h IN LISTS _hallazgos)
        string(REPLACE "${_prot_backslash}" "\\" _h "${_h}")
        string(REPLACE "${_prot_lbracket}" "[" _h "${_h}")
        string(REPLACE "${_prot_rbracket}" "]" _h "${_h}")
        string(REPLACE "${_prot_semicolon}" ";" _h "${_h}")
        message("  ${_h}")
    endforeach()
    message("")
    message("ADR-004 manda un repositorio por componente, cada uno con sus tests,")
    message("tools y docs. Cada línea de arriba es algo que deja de compilar el día")
    message("del corte. Las salidas, en orden de preferencia:")
    message("  1. mudar el consumidor al componente que de verdad prueba o sirve")
    message("     (`<componente>/tests/`, `<componente>/tools/`): un test que compila")
    message("     fuentes del runtime es del runtime;")
    message("  2. tomar lo que hace falta de la superficie PÚBLICA del proveedor")
    message("     (`include/`, el target, el crate) — y si no está ahí, es que")
    message("     falta en ADR-003, no que haya que incluirlo por ruta;")
    message("  3. agregarlo a `_permitido` en este archivo, CON el motivo.")
    message(FATAL_ERROR "frontera de repositorios violada")
endif()

message("  [ OK ] fronteras ADR-004: ${_revisados} archivos revisados en ${_n_fronteras} fronteras, ninguna cruzada")
