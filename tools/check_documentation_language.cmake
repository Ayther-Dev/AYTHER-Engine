# Verify that comments in the selected first-party public headers are written
# in English. This is intentionally a narrow migration guard, not a natural-
# language classifier: it rejects Spanish punctuation, accented characters,
# and a small set of unambiguous documentation words.

cmake_minimum_required(VERSION 3.21)

if(NOT AYTHER_INCLUDE_DIR OR NOT AYTHER_DOCUMENTED_HEADERS)
    message(FATAL_ERROR
        "Missing -DAYTHER_INCLUDE_DIR or -DAYTHER_DOCUMENTED_HEADERS")
endif()

set(_violations "")
foreach(_header IN LISTS AYTHER_DOCUMENTED_HEADERS)
    set(_path "${AYTHER_INCLUDE_DIR}/${_header}")
    if(NOT EXISTS "${_path}")
        list(APPEND _violations "${_header}: header does not exist")
        continue()
    endif()

    file(STRINGS "${_path}" _comment_lines
        REGEX "^[ \t]*(//|/\\*|\\*)")
    set(_line_number 0)
    foreach(_line IN LISTS _comment_lines)
        math(EXPR _line_number "${_line_number} + 1")
        if(_line MATCHES
           "[áéíóúñ¿¡ÁÉÍÓÚÑ]|(^|[ ,.;:/`])(capa|capas|devuelve|pantalla|sesión|archivo|sólo)([ ,.;:/`]|$)")
            list(APPEND _violations
                "${_header}: selected comment ${_line_number}: ${_line}")
        endif()
    endforeach()
endforeach()

if(_violations)
    foreach(_violation IN LISTS _violations)
        message("  [FAIL] ${_violation}")
    endforeach()
    message(FATAL_ERROR
        "Selected public-header comments must be written in English")
endif()

list(LENGTH AYTHER_DOCUMENTED_HEADERS _header_count)
message("  [ OK ] documentation language checked in ${_header_count} headers")
