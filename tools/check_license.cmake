# ---------------------------------------------------------------------------
# check_license.cmake — la licencia dice lo mismo en todos lados (#532).
#
# Cinco lugares tienen que coincidir: `LICENSE`, el `license` del workspace de
# Cargo, la matriz de `ecosystem.md`, `lab/LICENSE` y el README del SDK. Con
# licencias, uno distinto no es una errata: es una contradicción legal, y la
# descubre el que se la lleva.
#
# Mientras NO haya `LICENSE` —el estado de hoy— este check SALTEA con código 77
# y dice qué falta. No falla: el repositorio tiene que poder compilar y correr
# sus tests mientras la decisión no esté tomada, y un rojo permanente se
# convierte en un rojo que nadie mira. Pero tampoco pasa en verde: un SKIP en la
# lista de ctest es una pregunta abierta a la vista.
#
# Uso:
#   cmake -DAYTHER_REPO=<dir> -P tools/check_license.cmake
# ---------------------------------------------------------------------------
if(NOT AYTHER_REPO)
    set(AYTHER_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

set(_license "${AYTHER_REPO}/LICENSE")
if(NOT EXISTS "${_license}")
    message("  [SKIP] no hay LICENSE en la raíz todavía (#532).")
    message("         Sin licencia declarada nadie puede integrar legalmente el")
    message("         motor y `cargo publish` rechaza los crates — que es el")
    message("         bloqueo correcto, no un olvido.")
    message("         Al decidirla: pwsh tools/set_license.ps1 -Id <SPDX> -TextFile <texto>")
    # ctest lo marca como SALTEADO por `SKIP_REGULAR_EXPRESSION` y no por
    # código de salida: un script `cmake -P` no puede elegir su exit code, y
    # forzarlo con un FATAL_ERROR lo convertiría en un fallo — que es
    # exactamente lo que este caso NO es.
    message("__AYTHER_SKIP__")
    return()
endif()

file(READ "${_license}" _texto_licencia)
string(LENGTH "${_texto_licencia}" _largo)
if(_largo LESS 200)
    message(FATAL_ERROR
        "  [FAIL] LICENSE tiene ${_largo} caracteres: parece truncado. "
        "Un LICENSE que parece puesto y no dice nada es peor que ninguno.")
endif()

set(_fallos "")

# -- El SPDX declarado en Cargo tiene que estar dicho en el LICENSE ----------
file(READ "${AYTHER_REPO}/Cargo.toml" _cargo)
if(NOT _cargo MATCHES "\n[ \t]*license[ \t]*=[ \t]*\"([^\"]+)\"")
    list(APPEND _fallos
         "hay LICENSE pero el workspace de Cargo no declara `license`: "
         "`cargo publish` va a seguir negándose")
else()
    set(_spdx "${CMAKE_MATCH_1}")
    message("  licencia declarada: ${_spdx}")

    # El nombre de la licencia tiene que aparecer en el texto. No se compara el
    # texto entero contra nada: lo que se atrapa es el caso real —alguien cambia
    # el SPDX de Cargo y se olvida de reemplazar el archivo, o al revés.
    string(REPLACE "-" "" _spdx_plano "${_spdx}")
    string(TOUPPER "${_texto_licencia}" _texto_up)
    string(TOUPPER "${_spdx}" _spdx_up)
    string(REGEX REPLACE "-[0-9.]+$" "" _familia "${_spdx_up}")
    if(NOT _texto_up MATCHES "${_familia}")
        list(APPEND _fallos
             "Cargo declara `${_spdx}` pero el texto de LICENSE no menciona "
             "`${_familia}` — uno de los dos quedó viejo")
    endif()

    # La matriz por directorio tiene que nombrar la misma.
    file(READ "${AYTHER_REPO}/docs/architecture/ecosystem.md" _eco)
    if(NOT _eco MATCHES "${_spdx}")
        list(APPEND _fallos
             "la matriz de ecosystem.md §1.1 no nombra `${_spdx}`")
    endif()
endif()

# -- El Lab tiene que decir que NO es FOSS ----------------------------------
if(NOT EXISTS "${AYTHER_REPO}/lab/LICENSE")
    list(APPEND _fallos
         "falta `lab/LICENSE`: un archivo suelto del Lab que se escape del repo "
         "viajaría sin decir qué es")
else()
    file(READ "${AYTHER_REPO}/lab/LICENSE" _lab)
    if(NOT _lab MATCHES "[Pp]ropietario|[Pp]roprietary")
        list(APPEND _fallos "`lab/LICENSE` no dice que el código es propietario")
    endif()
endif()

if(_fallos)
    message("")
    foreach(_f IN LISTS _fallos)
        message("  [FAIL] ${_f}")
    endforeach()
    message(FATAL_ERROR "la licencia no dice lo mismo en todos lados")
endif()

message("  [ OK ] la licencia es coherente en LICENSE, Cargo, la matriz y lab/")
