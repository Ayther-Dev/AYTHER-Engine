cmake_minimum_required(VERSION 3.21)

if(NOT AYTHER_ENGINE_PUBLIC_DIR OR NOT AYTHER_ENGINE_PUBLIC_HEADERS)
    message(FATAL_ERROR
        "Missing -DAYTHER_ENGINE_PUBLIC_DIR or -DAYTHER_ENGINE_PUBLIC_HEADERS")
endif()

file(GLOB_RECURSE _actual_headers
    RELATIVE "${AYTHER_ENGINE_PUBLIC_DIR}"
    "${AYTHER_ENGINE_PUBLIC_DIR}/*.h"
    "${AYTHER_ENGINE_PUBLIC_DIR}/*.hpp")
list(SORT _actual_headers)

set(_expected_headers ${AYTHER_ENGINE_PUBLIC_HEADERS})
list(SORT _expected_headers)

if(NOT _actual_headers STREQUAL _expected_headers)
    message(FATAL_ERROR
        "Runtime-facing public header inventory differs from the explicit "
        "contract. Expected: ${_expected_headers}; actual: ${_actual_headers}")
endif()

set(_violations "")
foreach(_header IN LISTS _actual_headers)
    set(_path "${AYTHER_ENGINE_PUBLIC_DIR}/${_header}")
    file(STRINGS "${_path}" _includes
        REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(_include IN LISTS _includes)
        if(_include MATCHES "(src|private|internal|detail)/")
            list(APPEND _violations "${_header}: ${_include}")
        endif()
    endforeach()
endforeach()

if(_violations)
    foreach(_violation IN LISTS _violations)
        message("  [FAIL] ${_violation}")
    endforeach()
    message(FATAL_ERROR
        "A Runtime-facing public header includes a forbidden private path")
endif()

list(LENGTH _actual_headers _header_count)
message("  [ OK ] Engine public contract checked in ${_header_count} headers")
